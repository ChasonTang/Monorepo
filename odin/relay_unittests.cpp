// odin/relay_unittests.cpp
//
// Unit tests T1-T16 from §6 of odin/docs/rfc_014_relay_v2_transport.md.
//
// T1-T7 and T16 drive the relay against a test-local fake transport (no fd, no
// loop), injecting readiness by calling the exported odin_relay_ready
// directly. T8-T15 are integration tests over two real odin_fd_transport
// endpoints plus a live odin_event_loop, reusing the RFC-011 fork + waitpid 2 s
// deadline harness (replicated below as RelayRunDeadline) plus a per-row
// watchdog timer.

#include "odin/relay.h"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "odin/event_loop.h"
#include "odin/event_loop_internal_test.h"
#include "odin/transport.h"
#include "odin/transport_fd.h"
#if defined(ODIN_TRANSPORT_FD_TESTING)
#include "odin/transport_fd_internal_test.h"
#endif

#include "gtest/gtest.h"

// NOLINTBEGIN(misc-const-correctness, misc-use-internal-linkage)

namespace {

// The relay's fixed per-direction buffer capacity (§3.2.2 CAP). Mirrored here
// for T13's saturation gate; the relay does not export it.
constexpr size_t kCap = 65536;

// Replicated fork + waitpid 2 s deadline fixture (RFC-010 §6). The child runs
// the loop and all assertions, then _exit(HasFailure() ? 1 : 0); the parent
// fails the row unless the child exits 0 within the deadline.
class RelayRunDeadline {
public:
  template <typename Fn> static void Run(Fn fn) {
    const pid_t pid = fork();
    ASSERT_NE(pid, -1) << std::strerror(errno);
    if (pid == 0) {
      fn();
      _exit(::testing::Test::HasFailure() ? 1 : 0);
    }

    int wstatus = 0;
    bool exited = false;
    for (int i = 0; i < 200; ++i) {
      const pid_t got = waitpid(pid, &wstatus, WNOHANG);
      if (got == pid) {
        exited = true;
        break;
      }
      if (got == -1 && errno != EINTR) {
        break;
      }
      usleep(10000);
    }
    if (!exited) {
      kill(pid, SIGKILL);
      waitpid(pid, &wstatus, 0);
      FAIL() << "RelayRunDeadline exceeded 2 seconds";
    }
    ASSERT_TRUE(WIFEXITED(wstatus));
    EXPECT_EQ(WEXITSTATUS(wstatus), 0);
  }
};

struct DoneState {
  int calls = 0;
  odin_relay_status_t status = ODIN_RELAY_OK;
  int err = 0;
  bool timed_out = false;
  bool destroy_in_cb = false;
  odin_event_loop_t *loop = nullptr;
};

void OnDone(odin_relay_t *relay, odin_relay_status_t status, int err,
            void *user_data) {
  DoneState *s = static_cast<DoneState *>(user_data);
  s->status = status;
  s->err = err;
  s->calls += 1;
  if (s->destroy_in_cb) {
    odin_relay_destroy(relay);
  }
  if (s->loop != nullptr) {
    odin_event_loop_stop(s->loop);
  }
}

void WatchdogCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                void *user_data) {
  DoneState *s = static_cast<DoneState *>(user_data);
  s->timed_out = true;
  odin_event_timer_stop(timer);
  odin_event_loop_stop(loop);
}

void NoopIoCb(odin_event_loop_t *loop, odin_event_io_t *io, int fd,
              unsigned int events, void *user_data) {
  (void)loop;
  (void)io;
  (void)fd;
  (void)events;
  (void)user_data;
}

void SetNonblock(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  ASSERT_NE(flags, -1) << std::strerror(errno);
  ASSERT_EQ(fcntl(fd, F_SETFL, flags | O_NONBLOCK), 0) << std::strerror(errno);
}

// socketpair(AF_UNIX): relay end (*relay_fd) is always nonblocking (§3.2.1);
// the peer end (*peer_fd) is nonblocking only when peer_nonblock is set.
void MakeUnixPair(int *relay_fd, int *peer_fd, bool peer_nonblock) {
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0)
      << std::strerror(errno);
  SetNonblock(fds[1]);
  if (peer_nonblock) {
    SetNonblock(fds[0]);
  }
  *relay_fd = fds[1];
  *peer_fd = fds[0];
}

// Loopback TCP connected pair via listen/connect/accept; the relay end
// (*relay_fd) is the accepted socket and is nonblocking. Used where a peer
// close must surface as an RST-driven read fault rather than a graceful EOF.
void MakeTcpPair(int *relay_fd, int *peer_fd) {
  const int lfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(lfd, 0) << std::strerror(errno);
  int reuse = 1;
  (void)setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ASSERT_EQ(bind(lfd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)),
            0)
      << std::strerror(errno);
  ASSERT_EQ(listen(lfd, 1), 0) << std::strerror(errno);
  socklen_t alen = sizeof(addr);
  ASSERT_EQ(getsockname(lfd, reinterpret_cast<struct sockaddr *>(&addr), &alen),
            0)
      << std::strerror(errno);

  const int cfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(cfd, 0) << std::strerror(errno);
  ASSERT_EQ(
      connect(cfd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)), 0)
      << std::strerror(errno);
  const int afd = accept(lfd, nullptr, nullptr);
  ASSERT_GE(afd, 0) << std::strerror(errno);
  ASSERT_EQ(close(lfd), 0) << std::strerror(errno);
  SetNonblock(afd);
  *relay_fd = afd;
  *peer_fd = cfd;
}

// SO_LINGER{1,0} + close: aborts the connection, sending an RST so the peer's
// next read fails (ECONNRESET on TCP).
void CloseWithRst(int fd) {
  struct linger lg;
  lg.l_onoff = 1;
  lg.l_linger = 0;
  (void)setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
  (void)close(fd);
}

void PinSocketBuf(int fd, int size) {
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
}

// Arrival barrier (§6): poll both relay fds for read/error/hangup until both
// report ready, without read() or odin_transport_error so the pending RST the
// relay later observes survives.
void PollBothReady(int fd_a, int fd_b) {
  struct pollfd pfds[2];
  pfds[0].fd = fd_a;
  pfds[1].fd = fd_b;
  bool ready_a = false;
  bool ready_b = false;
  while (!(ready_a && ready_b)) {
    pfds[0].events = POLLIN | POLLERR | POLLHUP;
    pfds[1].events = POLLIN | POLLERR | POLLHUP;
    pfds[0].revents = 0;
    pfds[1].revents = 0;
    const int n = poll(pfds, 2, -1);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      ADD_FAILURE() << "poll: " << std::strerror(errno);
      return;
    }
    if (pfds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
      ready_a = true;
    }
    if (pfds[1].revents & (POLLIN | POLLERR | POLLHUP)) {
      ready_b = true;
    }
  }
}

bool WriteAll(int fd, const void *data, size_t len) {
  const uint8_t *p = static_cast<const uint8_t *>(data);
  size_t off = 0;
  while (off < len) {
    const ssize_t n = write(fd, p + off, len - off);
    if (n > 0) {
      off += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

// Blocking read of fd to EOF; returns collected bytes.
std::string DrainToEof(int fd) {
  std::string out;
  char buf[4096];
  for (;;) {
    const ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      out.append(buf, static_cast<size_t>(n));
      continue;
    }
    if (n == 0) {
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    break;
  }
  return out;
}

bool FdOpen(int fd) { return fcntl(fd, F_GETFD) != -1; }

// --- Fake transport (T1-T7, T16) -------------------------------------------
//
// Embeds odin_transport_t as its first member; its vtable slots serve scripted
// read/write/shutdown_write/error results, record each set_interest mask and
// each destroy call, and accumulate written bytes. Readiness is injected by
// calling odin_relay_ready(&fake.base, events, r) directly.

enum ReadKind { kReadData, kReadEof, kReadAgain, kReadFail };

struct ReadStep {
  ReadKind kind;
  std::string data;
  int err;
};

ReadStep ReadData(const std::string &s) { return ReadStep{kReadData, s, 0}; }
ReadStep ReadEof() { return ReadStep{kReadEof, std::string(), 0}; }
ReadStep ReadAgain() { return ReadStep{kReadAgain, std::string(), 0}; }
ReadStep ReadFail(int e) { return ReadStep{kReadFail, std::string(), e}; }

enum { kWriteAccept = 0, kWriteAgain = 1, kWriteFail = 2 };

struct FakeTransport {
  odin_transport_t base;
  std::deque<ReadStep> reads;
  bool read_infinite = false; // when deque empty, yield a full-len chunk
  int write_mode = kWriteAccept;
  int write_errno = 0;
  std::string written; // accepted bytes, in order
  int shutdown_rc = 0; // 0 success, -1 fail
  int shutdown_errno = 0;
  int shutdown_calls = 0;
  int error_result = 0;                // odin_transport_error() return
  std::vector<unsigned int> interests; // each set_interest mask, in order
  int destroy_calls = 0;
};

odin_transport_io_t FakeReadFn(odin_transport_t *t, void *buf, size_t len,
                               size_t *out_n) {
  FakeTransport *f = reinterpret_cast<FakeTransport *>(t);
  if (!f->reads.empty()) {
    const ReadStep s = f->reads.front();
    f->reads.pop_front();
    switch (s.kind) {
    case kReadData: {
      size_t n = s.data.size();
      if (n > len) {
        n = len;
      }
      std::memcpy(buf, s.data.data(), n);
      *out_n = n;
      return ODIN_TRANSPORT_OK;
    }
    case kReadEof:
      *out_n = 0;
      return ODIN_TRANSPORT_EOF;
    case kReadAgain:
      return ODIN_TRANSPORT_AGAIN;
    case kReadFail:
      errno = s.err;
      return ODIN_TRANSPORT_IO_ERROR;
    }
  }
  if (f->read_infinite) {
    std::memset(buf, 0xAB, len);
    *out_n = len;
    return ODIN_TRANSPORT_OK;
  }
  return ODIN_TRANSPORT_AGAIN;
}

odin_transport_io_t FakeWriteFn(odin_transport_t *t, const void *buf,
                                size_t len, size_t *out_n) {
  FakeTransport *f = reinterpret_cast<FakeTransport *>(t);
  if (f->write_mode == kWriteAgain) {
    return ODIN_TRANSPORT_AGAIN;
  }
  if (f->write_mode == kWriteFail) {
    errno = f->write_errno;
    return ODIN_TRANSPORT_IO_ERROR;
  }
  f->written.append(static_cast<const char *>(buf), len);
  *out_n = len;
  return ODIN_TRANSPORT_OK;
}

int FakeShutdownFn(odin_transport_t *t) {
  FakeTransport *f = reinterpret_cast<FakeTransport *>(t);
  f->shutdown_calls += 1;
  if (f->shutdown_rc != 0) {
    errno = f->shutdown_errno;
    return -1;
  }
  return 0;
}

int FakeSetInterestFn(odin_transport_t *t, unsigned int events) {
  FakeTransport *f = reinterpret_cast<FakeTransport *>(t);
  f->interests.push_back(events);
  return 0;
}

int FakeErrorFn(odin_transport_t *t) {
  FakeTransport *f = reinterpret_cast<FakeTransport *>(t);
  return f->error_result;
}

void FakeDestroyFn(odin_transport_t *t) {
  FakeTransport *f = reinterpret_cast<FakeTransport *>(t);
  f->destroy_calls += 1;
}

const odin_transport_vtable_t kFakeVtable = {
    FakeReadFn,        FakeWriteFn, FakeShutdownFn,
    FakeSetInterestFn, FakeErrorFn, FakeDestroyFn,
};

unsigned int LastInterest(const FakeTransport &f) {
  return f.interests.empty() ? 0u : f.interests.back();
}

} // namespace

// T1 — Bidirectional in-order forwarding; relay destroys no transport.
TEST(OdinRelayTest, T1) {
  FakeTransport a{};
  a.base.vt = &kFakeVtable;
  FakeTransport b{};
  b.base.vt = &kFakeVtable;
  a.reads.push_back(ReadData("hello"));
  a.reads.push_back(ReadAgain());
  b.reads.push_back(ReadData("world"));
  b.reads.push_back(ReadAgain());

  DoneState state;
  odin_relay_t *r = nullptr;
  ASSERT_EQ(odin_relay_create(OnDone, &state, &r), 0) << std::strerror(errno);
  ASSERT_EQ(odin_relay_start(r, &a.base, &b.base), 0) << std::strerror(errno);

  odin_relay_ready(&a.base, ODIN_TRANSPORT_READ, r);
  odin_relay_ready(&b.base, ODIN_TRANSPORT_READ, r);
  odin_relay_ready(&b.base, ODIN_TRANSPORT_WRITE, r);
  odin_relay_ready(&a.base, ODIN_TRANSPORT_WRITE, r);

  EXPECT_EQ(b.written, std::string("hello"));
  EXPECT_EQ(a.written, std::string("world"));
  EXPECT_EQ(a.destroy_calls, 0);
  EXPECT_EQ(b.destroy_calls, 0);
  EXPECT_EQ(state.calls, 0);

  odin_relay_destroy(r);
}

// T2 — Backpressure gates then resumes a source's READ.
TEST(OdinRelayTest, T2) {
  FakeTransport a{};
  a.base.vt = &kFakeVtable;
  FakeTransport b{};
  b.base.vt = &kFakeVtable;
  a.read_infinite = true;     // a.read always yields data
  b.write_mode = kWriteAgain; // the sink of dir A stalls

  DoneState state;
  odin_relay_t *r = nullptr;
  ASSERT_EQ(odin_relay_create(OnDone, &state, &r), 0) << std::strerror(errno);
  ASSERT_EQ(odin_relay_start(r, &a.base, &b.base), 0) << std::strerror(errno);

  // One infinite read fills dir A's whole free run, saturating the ring to CAP.
  odin_relay_ready(&a.base, ODIN_TRANSPORT_READ, r);

  // While dir A's ring is full, the last set_interest recorded for a clears
  // READ (the backpressure stop). Empty interests means no mask change was
  // recorded at all -- the P1 stub red.
  ASSERT_FALSE(a.interests.empty());
  EXPECT_EQ(LastInterest(a) & ODIN_TRANSPORT_READ, 0u);

  // b accepts; an a WRITE readiness flushes the ring below CAP.
  b.write_mode = kWriteAccept;
  odin_relay_ready(&b.base, ODIN_TRANSPORT_WRITE, r);

  // dir A drained below CAP -> a's READ interest re-arms.
  EXPECT_TRUE(LastInterest(a) & ODIN_TRANSPORT_READ);
  EXPECT_EQ(state.calls, 0);

  odin_relay_destroy(r);
}

// T3 — Half-close propagation + dual-EOF completion.
TEST(OdinRelayTest, T3) {
  FakeTransport a{};
  a.base.vt = &kFakeVtable;
  FakeTransport b{};
  b.base.vt = &kFakeVtable;
  a.reads.push_back(ReadData("abc"));
  a.reads.push_back(ReadEof());
  b.reads.push_back(ReadData("xyz"));
  b.reads.push_back(ReadEof());

  DoneState state;
  odin_relay_t *r = nullptr;
  ASSERT_EQ(odin_relay_create(OnDone, &state, &r), 0) << std::strerror(errno);
  ASSERT_EQ(odin_relay_start(r, &a.base, &b.base), 0) << std::strerror(errno);

  odin_relay_ready(&a.base, ODIN_TRANSPORT_READ, r);  // read "abc"
  odin_relay_ready(&b.base, ODIN_TRANSPORT_READ, r);  // read "xyz"
  odin_relay_ready(&b.base, ODIN_TRANSPORT_WRITE, r); // flush "abc" -> b
  odin_relay_ready(&a.base, ODIN_TRANSPORT_WRITE, r); // flush "xyz" -> a
  odin_relay_ready(&a.base, ODIN_TRANSPORT_READ, r);  // a EOF -> shutdown b
  odin_relay_ready(&b.base, ODIN_TRANSPORT_READ, r);  // b EOF -> shutdown a

  EXPECT_EQ(b.written, std::string("abc"));
  EXPECT_EQ(a.written, std::string("xyz"));
  EXPECT_EQ(b.shutdown_calls, 1);
  EXPECT_EQ(a.shutdown_calls, 1);
  EXPECT_EQ(state.calls, 1);
  EXPECT_EQ(state.status, ODIN_RELAY_OK);
  EXPECT_EQ(state.err, 0);

  odin_relay_destroy(r);
}

// T4 — Write fault → one ERROR; destroy-in-on_done leaves transports.
TEST(OdinRelayTest, T4) {
  FakeTransport a{};
  a.base.vt = &kFakeVtable;
  FakeTransport b{};
  b.base.vt = &kFakeVtable;
  a.reads.push_back(ReadData("z")); // buffer a byte into dir A
  b.write_mode = kWriteFail;
  b.write_errno = EPIPE;

  DoneState state;
  state.destroy_in_cb = true;
  odin_relay_t *r = nullptr;
  ASSERT_EQ(odin_relay_create(OnDone, &state, &r), 0) << std::strerror(errno);
  ASSERT_EQ(odin_relay_start(r, &a.base, &b.base), 0) << std::strerror(errno);

  odin_relay_ready(&a.base, ODIN_TRANSPORT_READ, r);  // read "z" into dir A
  odin_relay_ready(&b.base, ODIN_TRANSPORT_WRITE, r); // write -> b fails EPIPE

  EXPECT_EQ(state.calls, 1);
  EXPECT_EQ(state.status, ODIN_RELAY_ERROR);
  EXPECT_EQ(state.err, EPIPE);
  EXPECT_EQ(a.destroy_calls, 0);
  EXPECT_EQ(b.destroy_calls, 0);
  // r was destroyed inside on_done; do not touch it.
}

// T5 — Read fault → one ERROR with errno.
TEST(OdinRelayTest, T5) {
  FakeTransport a{};
  a.base.vt = &kFakeVtable;
  FakeTransport b{};
  b.base.vt = &kFakeVtable;
  a.reads.push_back(ReadFail(ECONNRESET));

  DoneState state;
  odin_relay_t *r = nullptr;
  ASSERT_EQ(odin_relay_create(OnDone, &state, &r), 0) << std::strerror(errno);
  ASSERT_EQ(odin_relay_start(r, &a.base, &b.base), 0) << std::strerror(errno);

  odin_relay_ready(&a.base, ODIN_TRANSPORT_READ, r);

  EXPECT_EQ(state.calls, 1);
  EXPECT_EQ(state.status, ODIN_RELAY_ERROR);
  EXPECT_EQ(state.err, ECONNRESET);

  odin_relay_destroy(r);
}

// T6 — ERROR readiness: latched error surfaces; benign ERROR keeps relaying.
TEST(OdinRelayTest, T6) {
  // (a) An ERROR readiness with no synchronous failure probes
  // odin_transport_error.
  {
    FakeTransport a{};
    a.base.vt = &kFakeVtable;
    FakeTransport b{};
    b.base.vt = &kFakeVtable;
    a.error_result = ECONNRESET; // a.read defaults to AGAIN

    DoneState state;
    odin_relay_t *r = nullptr;
    ASSERT_EQ(odin_relay_create(OnDone, &state, &r), 0) << std::strerror(errno);
    ASSERT_EQ(odin_relay_start(r, &a.base, &b.base), 0) << std::strerror(errno);

    odin_relay_ready(&a.base, ODIN_TRANSPORT_ERROR, r);

    EXPECT_EQ(state.calls, 1);
    EXPECT_EQ(state.status, ODIN_RELAY_ERROR);
    EXPECT_EQ(state.err, ECONNRESET);

    odin_relay_destroy(r);
  }
  // (b) A benign ERROR alongside a progressing read latches no error.
  {
    FakeTransport a{};
    a.base.vt = &kFakeVtable;
    FakeTransport b{};
    b.base.vt = &kFakeVtable;
    a.reads.push_back(ReadData("d"));
    a.error_result = 0;

    DoneState state;
    odin_relay_t *r = nullptr;
    ASSERT_EQ(odin_relay_create(OnDone, &state, &r), 0) << std::strerror(errno);
    ASSERT_EQ(odin_relay_start(r, &a.base, &b.base), 0) << std::strerror(errno);

    odin_relay_ready(&a.base, ODIN_TRANSPORT_READ | ODIN_TRANSPORT_ERROR, r);
    EXPECT_EQ(state.calls, 0); // outcome stays open

    odin_relay_ready(&b.base, ODIN_TRANSPORT_WRITE, r); // flush "d" -> b
    EXPECT_EQ(b.written, std::string("d"));
    EXPECT_EQ(state.calls, 0);

    odin_relay_destroy(r);
  }
}

// T7 — destroy on a still-running relay stops both watches.
TEST(OdinRelayTest, T7) {
  FakeTransport a{};
  a.base.vt = &kFakeVtable;
  FakeTransport b{};
  b.base.vt = &kFakeVtable;

  DoneState state;
  odin_relay_t *r = nullptr;
  ASSERT_EQ(odin_relay_create(OnDone, &state, &r), 0) << std::strerror(errno);
  ASSERT_EQ(odin_relay_start(r, &a.base, &b.base), 0) << std::strerror(errno);

  // start registered READ on each; destroy must record one set_interest(0)
  // more.
  const size_t na = a.interests.size();
  const size_t nb = b.interests.size();

  odin_relay_destroy(r);

  ASSERT_EQ(a.interests.size(), na + 1);
  EXPECT_EQ(a.interests.back(), 0u);
  ASSERT_EQ(b.interests.size(), nb + 1);
  EXPECT_EQ(b.interests.back(), 0u);
  EXPECT_EQ(a.destroy_calls, 0);
  EXPECT_EQ(b.destroy_calls, 0);
}

// T8 — End-to-end OK over two fd transports; caller fds stay open.
TEST(OdinRelayTest, T8) {
  RelayRunDeadline::Run([] {
    int fd_a = -1;
    int pa = -1;
    int fd_b = -1;
    int pb = -1;
    MakeUnixPair(&fd_a, &pa, false);
    MakeUnixPair(&fd_b, &pb, false);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    ASSERT_TRUE(WriteAll(pa, "ping", 4));
    ASSERT_TRUE(WriteAll(pb, "pong", 4));
    ASSERT_EQ(shutdown(pa, SHUT_WR), 0) << std::strerror(errno);
    ASSERT_EQ(shutdown(pb, SHUT_WR), 0) << std::strerror(errno);

    DoneState state;
    state.loop = loop;
    odin_relay_t *r = nullptr;
    ASSERT_EQ(odin_relay_create(OnDone, &state, &r), 0) << std::strerror(errno);
    odin_transport_t *a = nullptr;
    odin_transport_t *b = nullptr;
    ASSERT_EQ(odin_fd_transport_create(loop, fd_a, odin_relay_ready, r, &a), 0)
        << std::strerror(errno);
    ASSERT_EQ(odin_fd_transport_create(loop, fd_b, odin_relay_ready, r, &b), 0)
        << std::strerror(errno);

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state, &watchdog),
        0);
    ASSERT_EQ(odin_relay_start(r, a, b), 0) << std::strerror(errno);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    ASSERT_EQ(state.calls, 1);
    EXPECT_EQ(state.status, ODIN_RELAY_OK);
    EXPECT_EQ(state.err, 0);
    EXPECT_FALSE(state.timed_out);

    EXPECT_EQ(DrainToEof(pb), std::string("ping"));
    EXPECT_EQ(DrainToEof(pa), std::string("pong"));

    EXPECT_TRUE(FdOpen(fd_a));
    EXPECT_TRUE(FdOpen(fd_b));
    odin_relay_destroy(r);
    odin_transport_destroy(a);
    odin_transport_destroy(b);
    EXPECT_EQ(close(fd_a), 0);
    EXPECT_EQ(close(fd_b), 0);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(pb), 0);
    odin_event_loop_destroy(loop);
  });
}

// T9 — End-to-end half-close over fd transports.
TEST(OdinRelayTest, T9) {
  RelayRunDeadline::Run([] {
    int fd_a = -1;
    int pa = -1;
    int fd_b = -1;
    int pb = -1;
    MakeUnixPair(&fd_a, &pa, false);
    MakeUnixPair(&fd_b, &pb, false);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    ASSERT_TRUE(WriteAll(pa, "abc", 3));
    ASSERT_EQ(shutdown(pa, SHUT_WR), 0) << std::strerror(errno);

    std::string reader_got;
    std::thread reader([pb, &reader_got] {
      std::string got;
      char buf[64];
      for (;;) {
        const ssize_t n = read(pb, buf, sizeof(buf));
        if (n > 0) {
          got.append(buf, static_cast<size_t>(n));
          continue;
        }
        if (n == 0) {
          break;
        }
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      reader_got = got;
      if (got == "abc") {
        (void)WriteAll(pb, "xyz", 3);
        (void)shutdown(pb, SHUT_WR);
      }
    });

    DoneState state;
    state.loop = loop;
    odin_relay_t *r = nullptr;
    ASSERT_EQ(odin_relay_create(OnDone, &state, &r), 0) << std::strerror(errno);
    odin_transport_t *a = nullptr;
    odin_transport_t *b = nullptr;
    ASSERT_EQ(odin_fd_transport_create(loop, fd_a, odin_relay_ready, r, &a), 0)
        << std::strerror(errno);
    ASSERT_EQ(odin_fd_transport_create(loop, fd_b, odin_relay_ready, r, &b), 0)
        << std::strerror(errno);

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state, &watchdog),
        0);
    ASSERT_EQ(odin_relay_start(r, a, b), 0) << std::strerror(errno);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    // Unblock the reader's read(pb) so the row is assertion-red against the
    // no-forwarding stub rather than hanging.
    odin_relay_destroy(r);
    odin_transport_destroy(a);
    odin_transport_destroy(b);
    EXPECT_EQ(close(fd_a), 0);
    EXPECT_EQ(close(fd_b), 0);
    reader.join();

    ASSERT_EQ(state.calls, 1);
    EXPECT_EQ(state.status, ODIN_RELAY_OK);
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(reader_got, std::string("abc"));
    EXPECT_EQ(DrainToEof(pa), std::string("xyz"));

    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(pb), 0);
    odin_event_loop_destroy(loop);
  });
}

// T10 — Genuine read fault (RST) over fd transport.
TEST(OdinRelayTest, T10) {
  RelayRunDeadline::Run([] {
    int fd_a = -1;
    int pa = -1;
    int fd_b = -1;
    int pb = -1;
    MakeTcpPair(&fd_a, &pa);
    MakeUnixPair(&fd_b, &pb, false);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    CloseWithRst(pa); // RST -> relay read(fd_a) fails ECONNRESET

    DoneState state;
    state.loop = loop;
    odin_relay_t *r = nullptr;
    ASSERT_EQ(odin_relay_create(OnDone, &state, &r), 0) << std::strerror(errno);
    odin_transport_t *a = nullptr;
    odin_transport_t *b = nullptr;
    ASSERT_EQ(odin_fd_transport_create(loop, fd_a, odin_relay_ready, r, &a), 0)
        << std::strerror(errno);
    ASSERT_EQ(odin_fd_transport_create(loop, fd_b, odin_relay_ready, r, &b), 0)
        << std::strerror(errno);

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state, &watchdog),
        0);
    ASSERT_EQ(odin_relay_start(r, a, b), 0) << std::strerror(errno);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    ASSERT_EQ(state.calls, 1);
    EXPECT_EQ(state.status, ODIN_RELAY_ERROR);
    EXPECT_EQ(state.err, ECONNRESET);
    EXPECT_FALSE(state.timed_out);

    odin_relay_destroy(r);
    odin_transport_destroy(a);
    odin_transport_destroy(b);
    EXPECT_EQ(close(fd_a), 0);
    EXPECT_EQ(close(fd_b), 0);
    EXPECT_EQ(close(pb), 0);
    odin_event_loop_destroy(loop);
  });
}

// T11 — Same-batch double error + destroy-in-on_done → exactly one on_done.
TEST(OdinRelayTest, T11) {
  RelayRunDeadline::Run([] {
    int fd_a = -1;
    int pa = -1;
    int fd_b = -1;
    int pb = -1;
    MakeTcpPair(&fd_a, &pa);
    MakeTcpPair(&fd_b, &pb);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    CloseWithRst(pa);
    CloseWithRst(pb);

    DoneState state;
    state.loop = loop;
    state.destroy_in_cb = true;
    odin_relay_t *r = nullptr;
    ASSERT_EQ(odin_relay_create(OnDone, &state, &r), 0) << std::strerror(errno);
    odin_transport_t *a = nullptr;
    odin_transport_t *b = nullptr;
    ASSERT_EQ(odin_fd_transport_create(loop, fd_a, odin_relay_ready, r, &a), 0)
        << std::strerror(errno);
    ASSERT_EQ(odin_fd_transport_create(loop, fd_b, odin_relay_ready, r, &b), 0)
        << std::strerror(errno);

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state, &watchdog),
        0);
    ASSERT_EQ(odin_relay_start(r, a, b), 0) << std::strerror(errno);

    odin_event_io_t *io_a = nullptr;
    odin_event_io_t *io_b = nullptr;
    ASSERT_EQ(odin_fd_transport_test_io(a, &io_a), 0) << std::strerror(errno);
    ASSERT_EQ(odin_fd_transport_test_io(b, &io_b), 0) << std::strerror(errno);
    PollBothReady(fd_a, fd_b);
    const odin_event_loop_test_ready_t entries[] = {
        {io_a, ODIN_EVENT_ERROR},
        {io_b, ODIN_EVENT_ERROR},
    };
    ASSERT_EQ(odin_event_loop_test_queue_backend_events(loop, entries, 2), 0)
        << std::strerror(errno);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    EXPECT_EQ(state.calls, 1);
    EXPECT_EQ(state.status, ODIN_RELAY_ERROR);
    EXPECT_EQ(state.err, ECONNRESET);
    EXPECT_FALSE(state.timed_out);

    // r was destroyed inside on_done; reclaim the transports and fds.
    odin_transport_destroy(a);
    odin_transport_destroy(b);
    EXPECT_EQ(close(fd_a), 0);
    EXPECT_EQ(close(fd_b), 0);
    odin_event_loop_destroy(loop);
  });
}

// T12 — start rollback when the second endpoint's interest fails.
TEST(OdinRelayTest, T12) {
  RelayRunDeadline::Run([] {
    int fd_a = -1;
    int pa = -1;
    int fd_b = -1;
    int pb = -1;
    MakeUnixPair(&fd_a, &pa, false);
    MakeUnixPair(&fd_b, &pb, false);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    odin_event_io_t *ext = nullptr;
    ASSERT_EQ(odin_event_io_start(loop, fd_b, ODIN_EVENT_READ, NoopIoCb,
                                  nullptr, &ext),
              0)
        << std::strerror(errno);

    DoneState state;
    state.loop = loop;
    odin_relay_t *r = nullptr;
    ASSERT_EQ(odin_relay_create(OnDone, &state, &r), 0) << std::strerror(errno);
    odin_transport_t *a = nullptr;
    odin_transport_t *b = nullptr;
    ASSERT_EQ(odin_fd_transport_create(loop, fd_a, odin_relay_ready, r, &a), 0)
        << std::strerror(errno);
    ASSERT_EQ(odin_fd_transport_create(loop, fd_b, odin_relay_ready, r, &b), 0)
        << std::strerror(errno);

    errno = 0;
    EXPECT_EQ(odin_relay_start(r, a, b), -1);
    EXPECT_EQ(errno, EEXIST);

    odin_event_io_stop(ext);

    ASSERT_EQ(odin_relay_start(r, a, b), 0) << std::strerror(errno);

    ASSERT_TRUE(WriteAll(pa, "ping", 4));
    ASSERT_TRUE(WriteAll(pb, "pong", 4));
    ASSERT_EQ(shutdown(pa, SHUT_WR), 0) << std::strerror(errno);
    ASSERT_EQ(shutdown(pb, SHUT_WR), 0) << std::strerror(errno);

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state, &watchdog),
        0);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    ASSERT_EQ(state.calls, 1);
    EXPECT_EQ(state.status, ODIN_RELAY_OK);
    EXPECT_EQ(state.err, 0);
    EXPECT_FALSE(state.timed_out);

    odin_relay_destroy(r);
    odin_transport_destroy(a);
    odin_transport_destroy(b);
    EXPECT_EQ(close(fd_a), 0);
    EXPECT_EQ(close(fd_b), 0);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(pb), 0);
    odin_event_loop_destroy(loop);
  });
}

// T13 — Large payload: ring saturates to CAP, clears then re-arms READ,
// byte-exact across wrap.
TEST(OdinRelayTest, T13) {
  RelayRunDeadline::Run([] {
    int fd_a = -1;
    int pa = -1;
    int fd_b = -1;
    int pb = -1;
    MakeUnixPair(&fd_a, &pa, false);
    MakeUnixPair(&fd_b, &pb, false);
    // Pin all four buffers small (well below CAP and the 256 KiB payload) so
    // the saturation gate (written >= CAP) opens only when the relay's own ring
    // fills -- impossible without forwarding.
    PinSocketBuf(pa, 8192);
    PinSocketBuf(fd_a, 8192);
    PinSocketBuf(fd_b, 8192);
    PinSocketBuf(pb, 8192);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    ASSERT_EQ(shutdown(pb, SHUT_WR), 0) << std::strerror(errno); // dir B idle

    constexpr size_t kPayload = 262144;
    std::atomic<size_t> written{0};
    size_t written_at_gate = kPayload;
    std::string collected;

    std::thread writer([pa, &written] {
      std::vector<uint8_t> pattern(kPayload);
      for (size_t i = 0; i < kPayload; ++i) {
        pattern[i] = static_cast<uint8_t>(i & 0xff);
      }
      size_t off = 0;
      while (off < kPayload) {
        size_t want = kPayload - off;
        if (want > 4096) {
          want = 4096;
        }
        const ssize_t n = write(pa, pattern.data() + off, want);
        if (n > 0) {
          off += static_cast<size_t>(n);
          written.store(off, std::memory_order_release);
          continue;
        }
        if (n < 0 && errno == EINTR) {
          continue;
        }
        break;
      }
      (void)shutdown(pa, SHUT_WR);
    });

    std::thread reader([pb, &written, &written_at_gate, &collected] {
      for (;;) {
        const size_t w = written.load(std::memory_order_acquire);
        if (w >= kCap) {
          std::this_thread::sleep_for(std::chrono::milliseconds(30));
          if (written.load(std::memory_order_acquire) == w) {
            written_at_gate = w;
            break;
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      char buf[4096];
      for (;;) {
        const ssize_t n = read(pb, buf, sizeof(buf));
        if (n > 0) {
          collected.append(buf, static_cast<size_t>(n));
          continue;
        }
        if (n == 0) {
          break;
        }
        if (errno == EINTR) {
          continue;
        }
        break;
      }
    });

    DoneState state;
    state.loop = loop;
    odin_relay_t *r = nullptr;
    ASSERT_EQ(odin_relay_create(OnDone, &state, &r), 0) << std::strerror(errno);
    odin_transport_t *a = nullptr;
    odin_transport_t *b = nullptr;
    ASSERT_EQ(odin_fd_transport_create(loop, fd_a, odin_relay_ready, r, &a), 0)
        << std::strerror(errno);
    ASSERT_EQ(odin_fd_transport_create(loop, fd_b, odin_relay_ready, r, &b), 0)
        << std::strerror(errno);

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 1000000, 0, WatchdogCb, &state, &watchdog),
        0);
    ASSERT_EQ(odin_relay_start(r, a, b), 0) << std::strerror(errno);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);
    writer.join();
    reader.join();

    EXPECT_EQ(state.calls, 1);
    EXPECT_EQ(state.status, ODIN_RELAY_OK);
    EXPECT_FALSE(state.timed_out);
    EXPECT_LT(written_at_gate, kPayload);
    ASSERT_EQ(collected.size(), kPayload);
    bool match = true;
    for (size_t i = 0; i < kPayload; ++i) {
      if (static_cast<uint8_t>(collected[i]) !=
          static_cast<uint8_t>(i & 0xff)) {
        match = false;
        break;
      }
    }
    EXPECT_TRUE(match);

    odin_relay_destroy(r);
    odin_transport_destroy(a);
    odin_transport_destroy(b);
    EXPECT_EQ(close(fd_a), 0);
    EXPECT_EQ(close(fd_b), 0);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(pb), 0);
    odin_event_loop_destroy(loop);
  });
}

// T14 — Same-batch double error, no destroy-in-on_done → exactly one on_done.
TEST(OdinRelayTest, T14) {
  RelayRunDeadline::Run([] {
    int fd_a = -1;
    int pa = -1;
    int fd_b = -1;
    int pb = -1;
    MakeTcpPair(&fd_a, &pa);
    MakeTcpPair(&fd_b, &pb);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    CloseWithRst(pa);
    CloseWithRst(pb);

    DoneState state;
    state.loop = loop;
    odin_relay_t *r = nullptr;
    ASSERT_EQ(odin_relay_create(OnDone, &state, &r), 0) << std::strerror(errno);
    odin_transport_t *a = nullptr;
    odin_transport_t *b = nullptr;
    ASSERT_EQ(odin_fd_transport_create(loop, fd_a, odin_relay_ready, r, &a), 0)
        << std::strerror(errno);
    ASSERT_EQ(odin_fd_transport_create(loop, fd_b, odin_relay_ready, r, &b), 0)
        << std::strerror(errno);

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state, &watchdog),
        0);
    ASSERT_EQ(odin_relay_start(r, a, b), 0) << std::strerror(errno);

    odin_event_io_t *io_a = nullptr;
    odin_event_io_t *io_b = nullptr;
    ASSERT_EQ(odin_fd_transport_test_io(a, &io_a), 0) << std::strerror(errno);
    ASSERT_EQ(odin_fd_transport_test_io(b, &io_b), 0) << std::strerror(errno);
    PollBothReady(fd_a, fd_b);
    const odin_event_loop_test_ready_t entries[] = {
        {io_a, ODIN_EVENT_ERROR},
        {io_b, ODIN_EVENT_ERROR},
    };
    ASSERT_EQ(odin_event_loop_test_queue_backend_events(loop, entries, 2), 0)
        << std::strerror(errno);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    EXPECT_EQ(state.calls, 1);
    EXPECT_EQ(state.status, ODIN_RELAY_ERROR);
    EXPECT_EQ(state.err, ECONNRESET);
    EXPECT_FALSE(state.timed_out);

    odin_relay_destroy(r);
    odin_transport_destroy(a);
    odin_transport_destroy(b);
    EXPECT_EQ(close(fd_a), 0);
    EXPECT_EQ(close(fd_b), 0);
    odin_event_loop_destroy(loop);
  });
}

namespace {
struct T15State {
  DoneState done;
  odin_relay_t *relay = nullptr;
  int pa = -1;
  int pb = -1;
  bool x_ok = false;
};

void T15TimerCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                void *user_data) {
  (void)loop;
  (void)timer;
  T15State *s = static_cast<T15State *>(user_data);
  char buf[8];
  const ssize_t n = read(s->pb, buf, sizeof(buf));
  s->x_ok = (n == 1 && buf[0] == 'x');
  odin_relay_destroy(s->relay); // abort the still-running relay
  (void)write(s->pa, "y", 1);
  (void)write(s->pb, "z", 1);
}
} // namespace

// T15 — destroy aborts a still-running relay over fd transports; no later
// readiness re-enters freed state.
TEST(OdinRelayTest, T15) {
  RelayRunDeadline::Run([] {
    int fd_a = -1;
    int fd_b = -1;
    T15State s;
    MakeUnixPair(&fd_a, &s.pa, true);
    MakeUnixPair(&fd_b, &s.pb, true);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    s.done.loop = loop;

    ASSERT_TRUE(WriteAll(s.pa, "x", 1));

    odin_relay_t *r = nullptr;
    ASSERT_EQ(odin_relay_create(OnDone, &s.done, &r), 0)
        << std::strerror(errno);
    odin_transport_t *a = nullptr;
    odin_transport_t *b = nullptr;
    ASSERT_EQ(odin_fd_transport_create(loop, fd_a, odin_relay_ready, r, &a), 0)
        << std::strerror(errno);
    ASSERT_EQ(odin_fd_transport_create(loop, fd_b, odin_relay_ready, r, &b), 0)
        << std::strerror(errno);
    s.relay = r;

    ASSERT_EQ(odin_relay_start(r, a, b), 0) << std::strerror(errno);

    odin_event_timer_t *t30 = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 30000, 0, T15TimerCb, &s, &t30), 0);
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 100000, 0, WatchdogCb, &s.done, &watchdog),
        0);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    EXPECT_TRUE(s.x_ok); // read(pb) == "x"; red against the no-watch stub
    EXPECT_EQ(s.done.calls, 0);
    EXPECT_TRUE(s.done.timed_out);

    odin_transport_destroy(a);
    odin_transport_destroy(b);
    EXPECT_EQ(close(fd_a), 0);
    EXPECT_EQ(close(fd_b), 0);
    EXPECT_EQ(close(s.pa), 0);
    EXPECT_EQ(close(s.pb), 0);
    odin_event_loop_destroy(loop);
  });
}

// T16 — Failed half-close → one aggregated ERROR with errno.
TEST(OdinRelayTest, T16) {
  FakeTransport a{};
  a.base.vt = &kFakeVtable;
  FakeTransport b{};
  b.base.vt = &kFakeVtable;
  a.reads.push_back(ReadEof()); // dir A EOF, ring empty -> drive half-closes
  b.shutdown_rc = -1;           // shutdown_write(b) fails
  b.shutdown_errno = EPIPE;

  DoneState state;
  odin_relay_t *r = nullptr;
  ASSERT_EQ(odin_relay_create(OnDone, &state, &r), 0) << std::strerror(errno);
  ASSERT_EQ(odin_relay_start(r, &a.base, &b.base), 0) << std::strerror(errno);

  odin_relay_ready(&a.base, ODIN_TRANSPORT_READ, r);

  EXPECT_EQ(state.calls, 1);
  EXPECT_EQ(state.status, ODIN_RELAY_ERROR);
  EXPECT_EQ(state.err, EPIPE);
  EXPECT_EQ(b.shutdown_calls, 1);

  odin_relay_destroy(r);
}

// NOLINTEND(misc-const-correctness, misc-use-internal-linkage)
