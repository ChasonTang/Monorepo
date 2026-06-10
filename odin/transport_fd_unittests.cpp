// odin/transport_fd_unittests.cpp
//
// Unit tests T2-T14 from §6 of
// odin/docs/rfc_013_transport_interface_fd_impl.md.
//
// Each row exercises the public odin_transport_* API against an
// odin_fd_transport_create instance. Every row runs under the same fork +
// waitpid 2 s deadline fixture the sibling relay suite uses (replicated below
// as TransportRunDeadline); the loop-running rows additionally arm a per-row
// watchdog timer that stops the loop so a readiness that never arrives fails on
// the deadline instead of hanging the suite.

#include "odin/transport_fd.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "odin/event_loop.h"
#include "odin/event_loop_internal_test.h"
#include "odin/transport.h"
#if defined(ODIN_TRANSPORT_FD_TESTING)
#include "odin/transport_fd_internal_test.h"
#endif

#include "gtest/gtest.h"

// NOLINTBEGIN(misc-const-correctness, misc-use-internal-linkage)

namespace {

// Replicated fork + waitpid 2 s deadline fixture (RFC-010 §6). The child runs
// the loop and all assertions, then _exit(HasFailure() ? 1 : 0); the parent
// fails the row unless the child exits 0 within the deadline.
class TransportRunDeadline {
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
      FAIL() << "TransportRunDeadline exceeded 2 seconds";
    }
    ASSERT_TRUE(WIFEXITED(wstatus));
    EXPECT_EQ(WEXITSTATUS(wstatus), 0);
  }
};

struct ReadyState {
  int calls = 0;
  unsigned int events = 0;
  bool timed_out = false;
  odin_event_loop_t *loop = nullptr;
};

void OnReady(odin_transport_t *t, unsigned int events, void *user_data) {
  (void)t;
  ReadyState *s = static_cast<ReadyState *>(user_data);
  s->calls += 1;
  s->events |= events;
  if (s->loop != nullptr) {
    odin_event_loop_stop(s->loop);
  }
}

void WatchdogCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                void *user_data) {
  ReadyState *s = static_cast<ReadyState *>(user_data);
  s->timed_out = true;
  odin_event_timer_stop(timer);
  odin_event_loop_stop(loop);
}

void SetNonblock(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  ASSERT_NE(flags, -1) << std::strerror(errno);
  ASSERT_EQ(fcntl(fd, F_SETFL, flags | O_NONBLOCK), 0) << std::strerror(errno);
}

// socketpair(AF_UNIX): transport end (*fd) is always nonblocking; the peer end
// (*peer) is nonblocking only when peer_nonblock is set.
void MakeUnixPair(int *fd, int *peer, bool peer_nonblock) {
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0)
      << std::strerror(errno);
  SetNonblock(fds[1]);
  if (peer_nonblock) {
    SetNonblock(fds[0]);
  }
  *fd = fds[1];
  *peer = fds[0];
}

// Loopback TCP connected pair; the transport end (*fd) is the accepted,
// nonblocking socket. Used where a peer close must surface as an RST-driven
// fault rather than a graceful EOF.
void MakeTcpPair(int *fd, int *peer) {
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
  *fd = afd;
  *peer = cfd;
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

bool FdOpen(int fd) { return fcntl(fd, F_GETFD) != -1; }

} // namespace

// T2 — Read delivers buffered bytes.
TEST(OdinFdTransportTest, T2) {
  TransportRunDeadline::Run([] {
    int fd = -1;
    int peer = -1;
    MakeUnixPair(&fd, &peer, false);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    ASSERT_TRUE(WriteAll(peer, "hello", 5));

    ReadyState state;
    state.loop = loop;
    odin_transport_t *t = nullptr;
    ASSERT_EQ(odin_fd_transport_create(loop, fd, OnReady, &state, &t), 0)
        << std::strerror(errno);

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state, &watchdog),
        0);

    ASSERT_EQ(odin_transport_set_interest(t, ODIN_TRANSPORT_READ), 0)
        << std::strerror(errno);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    EXPECT_GT(state.calls, 0);
    EXPECT_TRUE(state.events & ODIN_TRANSPORT_READ);
    EXPECT_FALSE(state.timed_out);

    char buf[64];
    size_t n = 0;
    EXPECT_EQ(odin_transport_read(t, buf, sizeof(buf), &n), ODIN_TRANSPORT_OK);
    EXPECT_EQ(n, 5u);
    EXPECT_EQ(std::string(buf, 5), std::string("hello"));

    odin_transport_destroy(t);
    EXPECT_EQ(close(fd), 0);
    EXPECT_EQ(close(peer), 0);
    odin_event_loop_destroy(loop);
  });
}

// T3 — Read reports orderly EOF.
TEST(OdinFdTransportTest, T3) {
  TransportRunDeadline::Run([] {
    int fd = -1;
    int peer = -1;
    MakeUnixPair(&fd, &peer, false);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    ASSERT_EQ(shutdown(peer, SHUT_WR), 0) << std::strerror(errno);

    ReadyState state;
    odin_transport_t *t = nullptr;
    ASSERT_EQ(odin_fd_transport_create(loop, fd, OnReady, &state, &t), 0)
        << std::strerror(errno);

    char buf[64];
    size_t n = 99;
    EXPECT_EQ(odin_transport_read(t, buf, sizeof(buf), &n), ODIN_TRANSPORT_EOF);
    EXPECT_EQ(n, 0u);

    odin_transport_destroy(t);
    EXPECT_EQ(close(fd), 0);
    EXPECT_EQ(close(peer), 0);
    odin_event_loop_destroy(loop);
  });
}

// T4 — Read with no data yields AGAIN.
TEST(OdinFdTransportTest, T4) {
  TransportRunDeadline::Run([] {
    int fd = -1;
    int peer = -1;
    MakeUnixPair(&fd, &peer, false);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    ReadyState state;
    odin_transport_t *t = nullptr;
    ASSERT_EQ(odin_fd_transport_create(loop, fd, OnReady, &state, &t), 0)
        << std::strerror(errno);

    char buf[64];
    size_t n = 0;
    EXPECT_EQ(odin_transport_read(t, buf, sizeof(buf), &n),
              ODIN_TRANSPORT_AGAIN);

    odin_transport_destroy(t);
    EXPECT_EQ(close(fd), 0);
    EXPECT_EQ(close(peer), 0);
    odin_event_loop_destroy(loop);
  });
}

// T5 — Write emits bytes and reports count.
TEST(OdinFdTransportTest, T5) {
  TransportRunDeadline::Run([] {
    int fd = -1;
    int peer = -1;
    MakeUnixPair(&fd, &peer, false);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    ReadyState state;
    odin_transport_t *t = nullptr;
    ASSERT_EQ(odin_fd_transport_create(loop, fd, OnReady, &state, &t), 0)
        << std::strerror(errno);

    size_t n = 0;
    ASSERT_EQ(odin_transport_write(t, "hi", 2, &n), ODIN_TRANSPORT_OK)
        << std::strerror(errno);
    EXPECT_EQ(n, 2u);

    char buf[8];
    const ssize_t r = read(peer, buf, sizeof(buf));
    EXPECT_EQ(r, 2);
    EXPECT_EQ(std::string(buf, 2), std::string("hi"));

    odin_transport_destroy(t);
    EXPECT_EQ(close(fd), 0);
    EXPECT_EQ(close(peer), 0);
    odin_event_loop_destroy(loop);
  });
}

// T6 — shutdown_write half-closes; reverse direction stays open.
TEST(OdinFdTransportTest, T6) {
  TransportRunDeadline::Run([] {
    int fd = -1;
    int peer = -1;
    MakeUnixPair(&fd, &peer, false);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    ReadyState state;
    odin_transport_t *t = nullptr;
    ASSERT_EQ(odin_fd_transport_create(loop, fd, OnReady, &state, &t), 0)
        << std::strerror(errno);

    ASSERT_EQ(odin_transport_shutdown_write(t), 0) << std::strerror(errno);

    // Peer's read side sees the half-close as EOF.
    char pbuf[8];
    const ssize_t pr = read(peer, pbuf, sizeof(pbuf));
    EXPECT_EQ(pr, 0);

    // Reverse direction still flows: peer writes, transport reads.
    ASSERT_TRUE(WriteAll(peer, "xy", 2));
    char buf[8];
    size_t n = 0;
    EXPECT_EQ(odin_transport_read(t, buf, sizeof(buf), &n), ODIN_TRANSPORT_OK);
    EXPECT_EQ(n, 2u);
    EXPECT_EQ(std::string(buf, 2), std::string("xy"));

    odin_transport_destroy(t);
    EXPECT_EQ(close(fd), 0);
    EXPECT_EQ(close(peer), 0);
    odin_event_loop_destroy(loop);
  });
}

// T7 — set_interest starts, updates, and stops the watch.
TEST(OdinFdTransportTest, T7) {
  TransportRunDeadline::Run([] {
    int fd = -1;
    int peer = -1;
    MakeUnixPair(&fd, &peer, false);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    ReadyState state;
    odin_transport_t *t = nullptr;
    ASSERT_EQ(odin_fd_transport_create(loop, fd, OnReady, &state, &t), 0)
        << std::strerror(errno);

    odin_event_io_t *io = nullptr;
    ASSERT_EQ(odin_transport_set_interest(t, ODIN_TRANSPORT_READ), 0)
        << std::strerror(errno);
    EXPECT_EQ(odin_fd_transport_test_io(t, &io), 0) << std::strerror(errno);

    ASSERT_EQ(odin_transport_set_interest(t, ODIN_TRANSPORT_READ |
                                                 ODIN_TRANSPORT_WRITE),
              0)
        << std::strerror(errno);
    EXPECT_EQ(odin_fd_transport_test_io(t, &io), 0) << std::strerror(errno);

    ASSERT_EQ(odin_transport_set_interest(t, 0), 0) << std::strerror(errno);
    errno = 0;
    EXPECT_EQ(odin_fd_transport_test_io(t, &io), -1);
    EXPECT_EQ(errno, ENOENT);

    odin_transport_destroy(t);
    EXPECT_EQ(close(fd), 0);
    EXPECT_EQ(close(peer), 0);
    odin_event_loop_destroy(loop);
  });
}

// T8 — Readiness forwards ERROR; a benign ERROR latches no error.
TEST(OdinFdTransportTest, T8) {
  TransportRunDeadline::Run([] {
    int fd = -1;
    int peer = -1;
    MakeUnixPair(&fd, &peer, false);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    ReadyState state;
    state.loop = loop;
    odin_transport_t *t = nullptr;
    ASSERT_EQ(odin_fd_transport_create(loop, fd, OnReady, &state, &t), 0)
        << std::strerror(errno);

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state, &watchdog),
        0);

    ASSERT_EQ(odin_transport_set_interest(t, ODIN_TRANSPORT_READ), 0)
        << std::strerror(errno);
    odin_event_io_t *io = nullptr;
    ASSERT_EQ(odin_fd_transport_test_io(t, &io), 0) << std::strerror(errno);

    const odin_event_loop_test_ready_t entries[] = {
        {io, ODIN_EVENT_READ | ODIN_EVENT_ERROR},
    };
    ASSERT_EQ(odin_event_loop_test_queue_backend_events(loop, entries, 1), 0)
        << std::strerror(errno);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    EXPECT_GT(state.calls, 0);
    EXPECT_TRUE(state.events & ODIN_TRANSPORT_READ);
    EXPECT_TRUE(state.events & ODIN_TRANSPORT_ERROR);
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(odin_transport_error(t), 0);

    odin_transport_destroy(t);
    EXPECT_EQ(close(fd), 0);
    EXPECT_EQ(close(peer), 0);
    odin_event_loop_destroy(loop);
  });
}

// T9 — Genuine read fault surfaces as ERROR (read-derived ECONNRESET).
TEST(OdinFdTransportTest, T9) {
  TransportRunDeadline::Run([] {
    int fd = -1;
    int peer = -1;
    MakeTcpPair(&fd, &peer);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    CloseWithRst(peer); // RST -> transport read(fd) fails ECONNRESET

    ReadyState state;
    state.loop = loop;
    odin_transport_t *t = nullptr;
    ASSERT_EQ(odin_fd_transport_create(loop, fd, OnReady, &state, &t), 0)
        << std::strerror(errno);

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state, &watchdog),
        0);

    ASSERT_EQ(odin_transport_set_interest(t, ODIN_TRANSPORT_READ), 0)
        << std::strerror(errno);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    EXPECT_GT(state.calls, 0);
    EXPECT_FALSE(state.timed_out);

    char buf[64];
    size_t n = 0;
    errno = 0;
    const odin_transport_io_t rc = odin_transport_read(t, buf, sizeof(buf), &n);
    const int e = errno;
    EXPECT_EQ(rc, ODIN_TRANSPORT_IO_ERROR);
    EXPECT_EQ(e, ECONNRESET);

    odin_transport_destroy(t);
    EXPECT_EQ(close(fd), 0);
    odin_event_loop_destroy(loop);
  });
}

// T10 — destroy stops the watch without closing the fd.
TEST(OdinFdTransportTest, T10) {
  TransportRunDeadline::Run([] {
    int fd = -1;
    int peer = -1;
    MakeUnixPair(&fd, &peer, false);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    ASSERT_TRUE(WriteAll(peer, "x", 1)); // read side is ready

    ReadyState state;
    state.loop = loop;
    odin_transport_t *t = nullptr;
    ASSERT_EQ(odin_fd_transport_create(loop, fd, OnReady, &state, &t), 0)
        << std::strerror(errno);

    ASSERT_EQ(odin_transport_set_interest(t, ODIN_TRANSPORT_READ), 0)
        << std::strerror(errno);
    odin_event_io_t *io = nullptr;
    ASSERT_EQ(odin_fd_transport_test_io(t, &io), 0) << std::strerror(errno);
    (void)io;

    odin_transport_destroy(t); // stops the watch and frees; never closes fd

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state, &watchdog),
        0);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    EXPECT_TRUE(FdOpen(fd));
    EXPECT_EQ(state.calls, 0);
    EXPECT_TRUE(state.timed_out);

    EXPECT_EQ(close(fd), 0);
    EXPECT_EQ(close(peer), 0);
    odin_event_loop_destroy(loop);
  });
}

// T11 — set_interest update applies WRITE interest.
TEST(OdinFdTransportTest, T11) {
  TransportRunDeadline::Run([] {
    int fd = -1;
    int peer = -1;
    MakeUnixPair(&fd, &peer, false);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    ReadyState state;
    state.loop = loop;
    odin_transport_t *t = nullptr;
    ASSERT_EQ(odin_fd_transport_create(loop, fd, OnReady, &state, &t), 0)
        << std::strerror(errno);

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state, &watchdog),
        0);

    ASSERT_EQ(odin_transport_set_interest(t, ODIN_TRANSPORT_READ), 0)
        << std::strerror(errno);
    ASSERT_EQ(odin_transport_set_interest(t, ODIN_TRANSPORT_READ |
                                                 ODIN_TRANSPORT_WRITE),
              0)
        << std::strerror(errno);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    EXPECT_GT(state.calls, 0);
    EXPECT_TRUE(state.events & ODIN_TRANSPORT_WRITE);
    EXPECT_FALSE(state.timed_out);

    odin_transport_destroy(t);
    EXPECT_EQ(close(fd), 0);
    EXPECT_EQ(close(peer), 0);
    odin_event_loop_destroy(loop);
  });
}

// T12 — Write on a full send buffer yields AGAIN.
TEST(OdinFdTransportTest, T12) {
  TransportRunDeadline::Run([] {
    int fd = -1;
    int peer = -1;
    MakeUnixPair(&fd, &peer, false);
    PinSocketBuf(fd, 4096);
    PinSocketBuf(peer, 4096);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    ReadyState state;
    odin_transport_t *t = nullptr;
    ASSERT_EQ(odin_fd_transport_create(loop, fd, OnReady, &state, &t), 0)
        << std::strerror(errno);

    // Peer never reads; fill the send buffer until a write would block.
    char buf[1024];
    std::memset(buf, 'a', sizeof(buf));
    odin_transport_io_t rc = ODIN_TRANSPORT_OK;
    for (int i = 0; i < 100000 && rc == ODIN_TRANSPORT_OK; ++i) {
      size_t n = 0;
      rc = odin_transport_write(t, buf, sizeof(buf), &n);
    }
    EXPECT_EQ(rc, ODIN_TRANSPORT_AGAIN);

    odin_transport_destroy(t);
    EXPECT_EQ(close(fd), 0);
    EXPECT_EQ(close(peer), 0);
    odin_event_loop_destroy(loop);
  });
}

// T13 — Write to a closed peer yields ERROR (EPIPE).
TEST(OdinFdTransportTest, T13) {
  TransportRunDeadline::Run([] {
    (void)signal(SIGPIPE, SIG_IGN);
    int fd = -1;
    int peer = -1;
    MakeUnixPair(&fd, &peer, false);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    ReadyState state;
    odin_transport_t *t = nullptr;
    ASSERT_EQ(odin_fd_transport_create(loop, fd, OnReady, &state, &t), 0)
        << std::strerror(errno);

    EXPECT_EQ(close(peer), 0);

    size_t n = 0;
    errno = 0;
    const odin_transport_io_t rc = odin_transport_write(t, "hi", 2, &n);
    const int e = errno;
    EXPECT_EQ(rc, ODIN_TRANSPORT_IO_ERROR);
    EXPECT_EQ(e, EPIPE);

    odin_transport_destroy(t);
    EXPECT_EQ(close(fd), 0);
    odin_event_loop_destroy(loop);
  });
}

// T14 — Latched async error surfaces through error() before any read.
TEST(OdinFdTransportTest, T14) {
  TransportRunDeadline::Run([] {
    int fd = -1;
    int peer = -1;
    MakeTcpPair(&fd, &peer);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    CloseWithRst(peer); // RST -> latched SO_ERROR == ECONNRESET

    ReadyState state;
    state.loop = loop;
    odin_transport_t *t = nullptr;
    ASSERT_EQ(odin_fd_transport_create(loop, fd, OnReady, &state, &t), 0)
        << std::strerror(errno);

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state, &watchdog),
        0);

    ASSERT_EQ(odin_transport_set_interest(t, ODIN_TRANSPORT_READ), 0)
        << std::strerror(errno);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    EXPECT_GT(state.calls, 0);
    EXPECT_TRUE(state.events & ODIN_TRANSPORT_ERROR);
    EXPECT_FALSE(state.timed_out);

    // error() before any read/write reads and clears the latched async error.
    EXPECT_EQ(odin_transport_error(t), ECONNRESET);

    odin_transport_destroy(t);
    EXPECT_EQ(close(fd), 0);
    odin_event_loop_destroy(loop);
  });
}

// NOLINTEND(misc-const-correctness, misc-use-internal-linkage)
