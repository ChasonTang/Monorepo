// odin/testing/dial_unittests.cpp
//
// Unit tests T1-T7 from §6 of odin/docs/rfc_012_nonblocking_socket_dial.md.
//
// Each row runs the event loop, so every row executes under the same fork +
// waitpid 2 s deadline fixture RFC-010 §6 established (replicated below as
// DialRunDeadline) plus a per-row 100 ms watchdog timer. The dial watches a
// single fd and needs no peer thread to make connect resolve, so every row is
// single-threaded and surfaces red through an executed in-child assertion.

#include "odin/dial.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "odin/event_loop.h"
#if defined(ODIN_DIAL_TESTING)
#include "odin/testing/dial_internal_test.h"
#endif

#include "gtest/gtest.h"

// NOLINTBEGIN(misc-const-correctness, misc-use-internal-linkage)

namespace {

// Replicated fork + waitpid 2 s deadline fixture (RFC-010 §6). The child runs
// the loop and all assertions, then _exit(HasFailure() ? 1 : 0); the parent
// fails the row unless the child exits 0 within the deadline.
class DialRunDeadline {
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
      FAIL() << "DialRunDeadline exceeded 2 seconds";
    }
    ASSERT_TRUE(WIFEXITED(wstatus));
    EXPECT_EQ(WEXITSTATUS(wstatus), 0);
  }
};

struct DialState {
  int calls = 0;
  odin_dial_status_t status = ODIN_DIAL_OK;
  int err = 0;
  int fd = -2;
  bool timed_out = false;
  bool destroy_in_cb = false;
  odin_event_loop_t *loop = nullptr;
};

// Records status/err/fd/calls, optionally destroys the dial from inside the
// callback (T7), then stops the loop.
void OnDial(odin_dial_t *dial, odin_dial_status_t status, int fd, int err,
            void *user_data) {
  DialState *s = static_cast<DialState *>(user_data);
  s->status = status;
  s->err = err;
  s->fd = fd;
  s->calls += 1;
  if (s->destroy_in_cb) {
    odin_dial_destroy(dial);
  }
  if (s->loop != nullptr) {
    odin_event_loop_stop(s->loop);
  }
}

void WatchdogCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                void *user_data) {
  DialState *s = static_cast<DialState *>(user_data);
  s->timed_out = true;
  odin_event_timer_stop(timer);
  odin_event_loop_stop(loop);
}

// Loopback TCP listener; *out_lfd is the listening socket and *out_addr its
// bound 127.0.0.1:<ephemeral> address (read back via getsockname).
void MakeTcpListener(int *out_lfd, struct sockaddr_in *out_addr) {
  const int lfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(lfd, 0) << std::strerror(errno);
  const int reuse = 1;
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
  *out_lfd = lfd;
  *out_addr = addr;
}

// An unused loopback address: bind a 127.0.0.1:0 SO_REUSEADDR socket to claim
// an ephemeral port, read it back, then close so a later connect is refused.
void UnusedLoopbackAddr(struct sockaddr_in *out_addr) {
  const int s = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(s, 0) << std::strerror(errno);
  const int reuse = 1;
  (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ASSERT_EQ(bind(s, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)),
            0)
      << std::strerror(errno);
  socklen_t alen = sizeof(addr);
  ASSERT_EQ(getsockname(s, reinterpret_cast<struct sockaddr *>(&addr), &alen),
            0)
      << std::strerror(errno);
  ASSERT_EQ(close(s), 0) << std::strerror(errno);
  *out_addr = addr;
}

void MakeUnixPath(char *buf, size_t buflen, const char *tag) {
  (void)std::snprintf(buf, buflen, "/tmp/odin_dial_%s_%d.sock", tag,
                      static_cast<int>(getpid()));
}

// T6 carries the dial pointer to the deferred-destroy timer.
struct T6State {
  DialState done;
  odin_dial_t *dial = nullptr;
};

void T6DestroyCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                 void *user_data) {
  (void)loop;
  T6State *s = static_cast<T6State *>(user_data);
  odin_dial_destroy(s->dial); // abort the in-flight dial; never calls on_done
  odin_event_timer_stop(timer);
}

} // namespace

// T1 — Loopback TCP dial succeeds; connected fd handed to caller, ownership
// transferred.
TEST(OdinDialTest, T1) {
  DialRunDeadline::Run([] {
    int lfd = -1;
    struct sockaddr_in addr;
    MakeTcpListener(&lfd, &addr);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    DialState state;
    state.loop = loop;
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state, &watchdog),
        0);

    odin_dial_t *d = nullptr;
    EXPECT_EQ(odin_dial_start(loop, reinterpret_cast<struct sockaddr *>(&addr),
                              sizeof(addr), OnDial, &state, &d),
              0)
        << std::strerror(errno);
    const int fd0 = odin_dial_test_fd(d);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    ASSERT_EQ(state.calls, 1);
    EXPECT_EQ(state.status, ODIN_DIAL_OK);
    EXPECT_EQ(state.err, 0);
    EXPECT_GE(state.fd, 0);
    EXPECT_EQ(state.fd, fd0); // the same socket the dial created
    EXPECT_FALSE(state.timed_out);

    const int srv = accept(lfd, nullptr, nullptr);
    ASSERT_GE(srv, 0) << std::strerror(errno);
    ASSERT_EQ(write(state.fd, "hi", 2), 2) << std::strerror(errno);
    char buf[2] = {0, 0};
    ASSERT_EQ(read(srv, buf, sizeof(buf)), 2) << std::strerror(errno);
    EXPECT_EQ(buf[0], 'h');
    EXPECT_EQ(buf[1], 'i');

    EXPECT_EQ(odin_dial_test_fd(d), -1); // ownership transferred
    EXPECT_EQ(errno, ENOENT);

    odin_dial_destroy(d);
    EXPECT_EQ(close(state.fd), 0);
    EXPECT_EQ(close(srv), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

// T2 — AF_UNIX dial completes synchronously (connect()==0) yet is still
// delivered via the loop as OK.
TEST(OdinDialTest, T2) {
  DialRunDeadline::Run([] {
    char path[108];
    MakeUnixPath(path, sizeof(path), "t2");
    (void)unlink(path);
    const int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(lfd, 0) << std::strerror(errno);
    struct sockaddr_un un;
    std::memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    (void)std::snprintf(un.sun_path, sizeof(un.sun_path), "%s", path);
    ASSERT_EQ(bind(lfd, reinterpret_cast<struct sockaddr *>(&un), sizeof(un)),
              0)
        << std::strerror(errno);
    ASSERT_EQ(listen(lfd, 1), 0) << std::strerror(errno);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    DialState state;
    state.loop = loop;
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state, &watchdog),
        0);

    odin_dial_t *d = nullptr;
    EXPECT_EQ(odin_dial_start(loop, reinterpret_cast<struct sockaddr *>(&un),
                              sizeof(un), OnDial, &state, &d),
              0)
        << std::strerror(errno);
    const int fd0 = odin_dial_test_fd(d);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    ASSERT_EQ(state.calls, 1);
    EXPECT_EQ(state.status, ODIN_DIAL_OK);
    EXPECT_EQ(state.err, 0);
    EXPECT_GE(state.fd, 0);
    EXPECT_EQ(state.fd, fd0);
    EXPECT_FALSE(state.timed_out);

    const int srv = accept(lfd, nullptr, nullptr);
    ASSERT_GE(srv, 0) << std::strerror(errno);
    ASSERT_EQ(write(state.fd, "x", 1), 1) << std::strerror(errno);
    char buf[1] = {0};
    ASSERT_EQ(read(srv, buf, sizeof(buf)), 1) << std::strerror(errno);
    EXPECT_EQ(buf[0], 'x');

    EXPECT_EQ(odin_dial_test_fd(d), -1); // ownership transferred
    EXPECT_EQ(errno, ENOENT);

    odin_dial_destroy(d);
    EXPECT_EQ(close(state.fd), 0);
    EXPECT_EQ(close(srv), 0);
    EXPECT_EQ(close(lfd), 0);
    (void)unlink(path);
    odin_event_loop_destroy(loop);
  });
}

// T3 — Loopback TCP dial to a closed port fails via the writable then
// getsockopt(SO_ERROR) path; socket closed, no leak.
TEST(OdinDialTest, T3) {
  DialRunDeadline::Run([] {
    struct sockaddr_in addr;
    UnusedLoopbackAddr(&addr);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    DialState state;
    state.loop = loop;
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state, &watchdog),
        0);

    odin_dial_t *d = nullptr;
    EXPECT_EQ(odin_dial_start(loop, reinterpret_cast<struct sockaddr *>(&addr),
                              sizeof(addr), OnDial, &state, &d),
              0)
        << std::strerror(errno);
    const int fd0 = odin_dial_test_fd(d);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    ASSERT_EQ(state.calls, 1);
    EXPECT_EQ(state.status, ODIN_DIAL_ERROR);
    EXPECT_EQ(state.err, ECONNREFUSED);
    EXPECT_EQ(state.fd, -1);
    EXPECT_FALSE(state.timed_out);

    EXPECT_EQ(fcntl(fd0, F_GETFD), -1); // dial closed the socket it created
    EXPECT_EQ(errno, EBADF);

    odin_dial_destroy(d);
    odin_event_loop_destroy(loop);
  });
}

// T4 — AF_UNIX dial to a nonexistent path fails immediately and is delivered
// via the deferred-error timer; socket closed.
TEST(OdinDialTest, T4) {
  DialRunDeadline::Run([] {
    char path[108];
    MakeUnixPath(path, sizeof(path), "t4_noexist");
    (void)unlink(path); // never created
    struct sockaddr_un un;
    std::memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    (void)std::snprintf(un.sun_path, sizeof(un.sun_path), "%s", path);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    DialState state;
    state.loop = loop;
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state, &watchdog),
        0);

    odin_dial_t *d = nullptr;
    EXPECT_EQ(odin_dial_start(loop, reinterpret_cast<struct sockaddr *>(&un),
                              sizeof(un), OnDial, &state, &d),
              0)
        << std::strerror(errno);
    const int fd0 = odin_dial_test_fd(d);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    ASSERT_EQ(state.calls, 1);
    EXPECT_EQ(state.status, ODIN_DIAL_ERROR);
    EXPECT_EQ(state.err, ENOENT);
    EXPECT_EQ(state.fd, -1);
    EXPECT_FALSE(state.timed_out);

    EXPECT_EQ(fcntl(fd0, F_GETFD), -1); // socket closed, no leak
    EXPECT_EQ(errno, EBADF);

    odin_dial_destroy(d);
    odin_event_loop_destroy(loop);
  });
}

// T5 — odin_dial_start local setup failure: unsupported family makes socket(2)
// fail; *out untouched, no callback, nothing leaked.
TEST(OdinDialTest, T5) {
  DialRunDeadline::Run([] {
    struct sockaddr bad;
    std::memset(&bad, 0, sizeof(bad));
    bad.sa_family = 255;

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    DialState state;
    state.loop = loop;

    // Recognizable invalid sentinel (never dereferenced): odin_dial_start must
    // leave *out untouched on its setup-failure path.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    odin_dial_t *d = reinterpret_cast<odin_dial_t *>(-1);
    errno = 0;
    EXPECT_EQ(odin_dial_start(loop, &bad, sizeof(bad), OnDial, &state, &d), -1);
    EXPECT_EQ(errno, EAFNOSUPPORT);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    EXPECT_EQ(d, reinterpret_cast<odin_dial_t *>(-1));

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state, &watchdog),
        0);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    EXPECT_EQ(state.calls,
              0); // failed start created no dial, registered nothing
    EXPECT_TRUE(state.timed_out);

    odin_event_loop_destroy(loop);
  });
}

// T6 — odin_dial_destroy aborts an in-flight dial: still-owned socket closed,
// callback never fires.
TEST(OdinDialTest, T6) {
  DialRunDeadline::Run([] {
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    ASSERT_EQ(inet_pton(AF_INET, "192.0.2.1", &addr.sin_addr), 1)
        << std::strerror(errno);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    T6State s;
    s.done.loop = loop;
    odin_event_timer_t *destroyer = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 30000, 0, T6DestroyCb, &s, &destroyer), 0);
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 100000, 0, WatchdogCb, &s.done, &watchdog),
        0);

    odin_dial_t *d = nullptr;
    EXPECT_EQ(odin_dial_start(loop, reinterpret_cast<struct sockaddr *>(&addr),
                              sizeof(addr), OnDial, &s.done, &d),
              0)
        << std::strerror(errno);
    s.dial = d;
    const int fd0 = odin_dial_test_fd(d);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    EXPECT_GE(fd0,
              0); // dial registered an in-flight attempt and owns the socket
    EXPECT_EQ(s.done.calls, 0);         // destroy never invokes the callback
    EXPECT_TRUE(s.done.timed_out);      // only the watchdog stopped the loop
    EXPECT_EQ(fcntl(fd0, F_GETFD), -1); // destroy closed the still-owned socket
    EXPECT_EQ(errno, EBADF);

    odin_event_loop_destroy(loop);
  });
}

// T7 — odin_dial_destroy from within on_done is safe and does not close the
// handed-off fd.
TEST(OdinDialTest, T7) {
  DialRunDeadline::Run([] {
    int lfd = -1;
    struct sockaddr_in addr;
    MakeTcpListener(&lfd, &addr);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    DialState state;
    state.loop = loop;
    state.destroy_in_cb = true;
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 100000, 0, WatchdogCb, &state, &watchdog),
        0);

    odin_dial_t *d = nullptr;
    EXPECT_EQ(odin_dial_start(loop, reinterpret_cast<struct sockaddr *>(&addr),
                              sizeof(addr), OnDial, &state, &d),
              0)
        << std::strerror(errno);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    ASSERT_EQ(state.calls, 1);
    EXPECT_EQ(state.status, ODIN_DIAL_OK);
    EXPECT_GE(state.fd, 0);
    EXPECT_FALSE(state.timed_out);

    const int srv = accept(lfd, nullptr, nullptr);
    ASSERT_GE(srv, 0) << std::strerror(errno);
    ASSERT_EQ(write(state.fd, "z", 1), 1) << std::strerror(errno);
    char buf[1] = {0};
    ASSERT_EQ(read(srv, buf, sizeof(buf)), 1) << std::strerror(errno);
    EXPECT_EQ(buf[0], 'z');
    EXPECT_EQ(fcntl(state.fd, F_GETFD),
              0); // destroy did not close handed-off fd

    EXPECT_EQ(close(state.fd), 0);
    EXPECT_EQ(close(srv), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

// NOLINTEND(misc-const-correctness, misc-use-internal-linkage)
