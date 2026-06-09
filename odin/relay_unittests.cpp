// odin/relay_unittests.cpp
//
// Unit tests T1-T11 from §6 of odin/docs/rfc_011_bidirectional_byte_relay.md.
//
// Each row runs the event loop, so every row executes under the same fork +
// waitpid 2 s deadline fixture RFC-010 §6 established (replicated below as
// RelayRunDeadline) plus a per-row watchdog timer.

#include "odin/relay.h"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
#if defined(ODIN_RELAY_TESTING)
#include "odin/relay_internal_test.h"
#endif

#include "gtest/gtest.h"

// NOLINTBEGIN(misc-const-correctness, misc-use-internal-linkage)

namespace {

// The relay's fixed per-direction buffer capacity (§3.2.2 CAP). Mirrored here
// for T2's saturation gate; the relay does not export it.
constexpr size_t kCap = 65536;

// Replicated fork + waitpid 2 s deadline fixture (RFC-010 §6). The child runs
// the loop and all assertions, then _exit(HasFailure() ? 1 : 0); the parent
// fails the row unless the child exits 0 within the deadline.
class RelayRunDeadline {
 public:
  template <typename Fn>
  static void Run(Fn fn) {
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
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0) << std::strerror(errno);
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
  ASSERT_EQ(connect(cfd, reinterpret_cast<struct sockaddr *>(&addr),
                    sizeof(addr)),
            0)
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
// report ready, without read() or getsockopt(SO_ERROR) so the pending RST the
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

} // namespace

// T1 — Bidirectional in-order delivery, dual graceful half-close.
TEST(OdinRelayTest, T1) {
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
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state,
                                     &watchdog),
              0);

    odin_relay_t *r = nullptr;
    EXPECT_EQ(odin_relay_start(loop, fd_a, fd_b, OnDone, &state, &r), 0)
        << std::strerror(errno);
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
    EXPECT_EQ(close(fd_a), 0);
    EXPECT_EQ(close(fd_b), 0);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(pb), 0);
    odin_event_loop_destroy(loop);
  });
}

// T2 — Backpressure: a deferred consumer forces the 64 KiB ring to saturate,
// then 256 KiB drains complete and in order.
TEST(OdinRelayTest, T2) {
  RelayRunDeadline::Run([] {
    int fd_a = -1;
    int pa = -1;
    int fd_b = -1;
    int pb = -1;
    MakeUnixPair(&fd_a, &pa, false);
    MakeUnixPair(&fd_b, &pb, false);
    // Pin all four buffers small (well below CAP and the 256 KiB payload) so
    // the only way the saturation gate (written >= CAP) opens is the relay's
    // own ring filling -- impossible without forwarding.
    PinSocketBuf(pa, 8192);
    PinSocketBuf(fd_a, 8192);
    PinSocketBuf(fd_b, 8192);
    PinSocketBuf(pb, 8192);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    ASSERT_EQ(shutdown(pb, SHUT_WR), 0) << std::strerror(errno); // B idle

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
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 1000000, 0, WatchdogCb, &state,
                                     &watchdog),
              0);
    odin_relay_t *r = nullptr;
    EXPECT_EQ(odin_relay_start(loop, fd_a, fd_b, OnDone, &state, &r), 0)
        << std::strerror(errno);
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
      if (static_cast<uint8_t>(collected[i]) != static_cast<uint8_t>(i & 0xff)) {
        match = false;
        break;
      }
    }
    EXPECT_TRUE(match);

    odin_relay_destroy(r);
    EXPECT_EQ(close(fd_a), 0);
    EXPECT_EQ(close(fd_b), 0);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(pb), 0);
    odin_event_loop_destroy(loop);
  });
}

// T3 — Half-close propagation; opposite direction keeps flowing after one side
// closes.
TEST(OdinRelayTest, T3) {
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
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state,
                                     &watchdog),
              0);
    odin_relay_t *r = nullptr;
    EXPECT_EQ(odin_relay_start(loop, fd_a, fd_b, OnDone, &state, &r), 0)
        << std::strerror(errno);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    // Unblock the reader's read(pb) (and the child's own read(pa)) so the row
    // is assertion-red against the no-forwarding stub rather than hanging.
    EXPECT_EQ(close(fd_a), 0);
    EXPECT_EQ(close(fd_b), 0);
    reader.join();

    EXPECT_EQ(state.calls, 1);
    EXPECT_EQ(state.status, ODIN_RELAY_OK);
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(reader_got, std::string("abc"));
    EXPECT_EQ(DrainToEof(pa), std::string("xyz"));

    odin_relay_destroy(r);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(pb), 0);
    odin_event_loop_destroy(loop);
  });
}

// T4 — Genuine write error aggregates to one ERROR teardown; destroy-in-callback
// is UAF-safe.
TEST(OdinRelayTest, T4) {
  RelayRunDeadline::Run([] {
    (void)signal(SIGPIPE, SIG_IGN);
    int fd_a = -1;
    int pa = -1;
    int fd_b = -1;
    int pb = -1;
    MakeUnixPair(&fd_a, &pa, false);
    MakeUnixPair(&fd_b, &pb, false);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    ASSERT_TRUE(WriteAll(pb, "z", 1));
    EXPECT_EQ(close(pa), 0); // fd_a EOF; relay's later write(fd_a) -> EPIPE

    DoneState state;
    state.loop = loop;
    state.destroy_in_cb = true;
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state,
                                     &watchdog),
              0);
    odin_relay_t *r = nullptr;
    EXPECT_EQ(odin_relay_start(loop, fd_a, fd_b, OnDone, &state, &r), 0)
        << std::strerror(errno);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    ASSERT_EQ(state.calls, 1);
    EXPECT_EQ(state.status, ODIN_RELAY_ERROR);
    EXPECT_EQ(state.err, EPIPE);
    EXPECT_FALSE(state.timed_out);

    EXPECT_EQ(close(fd_a), 0);
    EXPECT_EQ(close(fd_b), 0);
    EXPECT_EQ(close(pb), 0);
    odin_event_loop_destroy(loop);
  });
}

// T5 — Relay never closes caller fds; destroy frees the relay without closing
// fds.
TEST(OdinRelayTest, T5) {
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
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state,
                                     &watchdog),
              0);
    odin_relay_t *r = nullptr;
    EXPECT_EQ(odin_relay_start(loop, fd_a, fd_b, OnDone, &state, &r), 0)
        << std::strerror(errno);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    ASSERT_EQ(state.calls, 1);
    EXPECT_EQ(state.status, ODIN_RELAY_OK);
    EXPECT_TRUE(FdOpen(fd_a));
    EXPECT_TRUE(FdOpen(fd_b));
    odin_relay_destroy(r);
    EXPECT_TRUE(FdOpen(fd_a));
    EXPECT_TRUE(FdOpen(fd_b));

    EXPECT_EQ(close(fd_a), 0);
    EXPECT_EQ(close(fd_b), 0);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(pb), 0);
    odin_event_loop_destroy(loop);
  });
}

// T6 — odin_relay_destroy from inside on_done is safe.
TEST(OdinRelayTest, T6) {
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
    state.destroy_in_cb = true;
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state,
                                     &watchdog),
              0);
    odin_relay_t *r = nullptr;
    EXPECT_EQ(odin_relay_start(loop, fd_a, fd_b, OnDone, &state, &r), 0)
        << std::strerror(errno);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    ASSERT_EQ(state.calls, 1);
    EXPECT_EQ(state.status, ODIN_RELAY_OK);
    EXPECT_FALSE(state.timed_out);
    EXPECT_TRUE(FdOpen(fd_a));
    EXPECT_TRUE(FdOpen(fd_b));

    EXPECT_EQ(close(fd_a), 0);
    EXPECT_EQ(close(fd_b), 0);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(pb), 0);
    odin_event_loop_destroy(loop);
  });
}

// T7 — Genuine read fault (ECONNRESET) aggregates to one ERROR teardown.
TEST(OdinRelayTest, T7) {
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
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state,
                                     &watchdog),
              0);
    odin_relay_t *r = nullptr;
    EXPECT_EQ(odin_relay_start(loop, fd_a, fd_b, OnDone, &state, &r), 0)
        << std::strerror(errno);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    ASSERT_EQ(state.calls, 1);
    EXPECT_EQ(state.status, ODIN_RELAY_ERROR);
    EXPECT_EQ(state.err, ECONNRESET);
    EXPECT_FALSE(state.timed_out);

    odin_relay_destroy(r);
    EXPECT_EQ(close(fd_a), 0);
    EXPECT_EQ(close(fd_b), 0);
    EXPECT_EQ(close(pb), 0);
    odin_event_loop_destroy(loop);
  });
}

#if defined(ODIN_RELAY_TESTING)

// T8 — Same-batch double-error with destroy-in-callback: exactly one on_done,
// UAF-safe (joint same-batch invariant).
TEST(OdinRelayTest, T8) {
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
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state,
                                     &watchdog),
              0);
    odin_relay_t *r = nullptr;
    ASSERT_EQ(odin_relay_start(loop, fd_a, fd_b, OnDone, &state, &r), 0)
        << std::strerror(errno);

    odin_event_io_t *io_a = nullptr;
    odin_event_io_t *io_b = nullptr;
    ASSERT_EQ(odin_relay_test_io_handles(r, &io_a, &io_b), 0)
        << std::strerror(errno);
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

    EXPECT_EQ(close(fd_a), 0);
    EXPECT_EQ(close(fd_b), 0);
    odin_event_loop_destroy(loop);
  });
}

// T9 — Same-batch double-error without destroy; stop-both-watches + the
// torn_down guard jointly yield one on_done.
TEST(OdinRelayTest, T9) {
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
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state,
                                     &watchdog),
              0);
    odin_relay_t *r = nullptr;
    ASSERT_EQ(odin_relay_start(loop, fd_a, fd_b, OnDone, &state, &r), 0)
        << std::strerror(errno);

    odin_event_io_t *io_a = nullptr;
    odin_event_io_t *io_b = nullptr;
    ASSERT_EQ(odin_relay_test_io_handles(r, &io_a, &io_b), 0)
        << std::strerror(errno);
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
    EXPECT_EQ(close(fd_a), 0);
    EXPECT_EQ(close(fd_b), 0);
    odin_event_loop_destroy(loop);
  });
}

#endif // defined(ODIN_RELAY_TESTING)

namespace {
struct T10State {
  DoneState done;
  odin_relay_t *relay = nullptr;
  int pa = -1;
  int pb = -1;
  bool x_ok = false;
};

void T10TimerCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                void *user_data) {
  (void)loop;
  (void)timer;
  T10State *s = static_cast<T10State *>(user_data);
  char buf[8];
  const ssize_t n = read(s->pb, buf, sizeof(buf));
  s->x_ok = (n == 1 && buf[0] == 'x');
  odin_relay_destroy(s->relay); // abort the still-running relay
  (void)write(s->pa, "y", 1);
  (void)write(s->pb, "z", 1);
}
} // namespace

// T10 — destroy aborts a still-running relay; its still-active watches are
// stopped, so no later readiness re-enters the freed relay.
TEST(OdinRelayTest, T10) {
  RelayRunDeadline::Run([] {
    int fd_a = -1;
    int fd_b = -1;
    T10State s;
    MakeUnixPair(&fd_a, &s.pa, true);
    MakeUnixPair(&fd_b, &s.pb, true);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    s.done.loop = loop;

    ASSERT_TRUE(WriteAll(s.pa, "x", 1));

    odin_relay_t *r = nullptr;
    ASSERT_EQ(odin_relay_start(loop, fd_a, fd_b, OnDone, &s.done, &r), 0)
        << std::strerror(errno);
    s.relay = r;

    odin_event_timer_t *t30 = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 30000, 0, T10TimerCb, &s, &t30), 0);
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 100000, 0, WatchdogCb, &s.done,
                                     &watchdog),
              0);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    EXPECT_TRUE(s.x_ok); // read(pb) == "x"; red against the no-watch stub
    EXPECT_EQ(s.done.calls, 0);
    EXPECT_TRUE(s.done.timed_out);

    EXPECT_EQ(close(fd_a), 0);
    EXPECT_EQ(close(fd_b), 0);
    EXPECT_EQ(close(s.pa), 0);
    EXPECT_EQ(close(s.pb), 0);
    odin_event_loop_destroy(loop);
  });
}

// T11 — odin_relay_start partial rollback: a pre-watched fd_b forces the
// relay's second watch to fail EEXIST; *out untouched, the first watch
// un-registered.
TEST(OdinRelayTest, T11) {
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
    ASSERT_EQ(odin_event_io_start(loop, fd_b, ODIN_EVENT_READ, NoopIoCb, nullptr,
                                  &ext),
              0)
        << std::strerror(errno);

    DoneState state;
    state.loop = loop;
    // Recognizable invalid sentinel (never dereferenced): odin_relay_start must
    // leave *out untouched on its EEXIST rollback path.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    odin_relay_t *r = reinterpret_cast<odin_relay_t *>(-1);
    errno = 0;
    EXPECT_EQ(odin_relay_start(loop, fd_a, fd_b, OnDone, &state, &r), -1);
    EXPECT_EQ(errno, EEXIST);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    EXPECT_EQ(r, reinterpret_cast<odin_relay_t *>(-1));

    odin_event_io_stop(ext);

    odin_relay_t *r2 = nullptr;
    ASSERT_EQ(odin_relay_start(loop, fd_a, fd_b, OnDone, &state, &r2), 0)
        << std::strerror(errno);

    ASSERT_TRUE(WriteAll(pa, "ping", 4));
    ASSERT_TRUE(WriteAll(pb, "pong", 4));
    ASSERT_EQ(shutdown(pa, SHUT_WR), 0) << std::strerror(errno);
    ASSERT_EQ(shutdown(pb, SHUT_WR), 0) << std::strerror(errno);

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state,
                                     &watchdog),
              0);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    ASSERT_EQ(state.calls, 1);
    EXPECT_EQ(state.status, ODIN_RELAY_OK);
    EXPECT_EQ(state.err, 0);
    EXPECT_FALSE(state.timed_out);

    odin_relay_destroy(r2);
    EXPECT_EQ(close(fd_a), 0);
    EXPECT_EQ(close(fd_b), 0);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(pb), 0);
    odin_event_loop_destroy(loop);
  });
}

// NOLINTEND(misc-const-correctness, misc-use-internal-linkage)
