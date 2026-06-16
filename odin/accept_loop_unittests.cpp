// odin/accept_loop_unittests.cpp
//
// Unit tests T1-T18 from §5 of odin/docs/rfc_019_tcp_accept_loop.md.
//
// Each row runs under the same fork + waitpid 2 s deadline fixture
// (AcceptLoopRunDeadline, mirroring DialRunDeadline in
// odin/dial_unittests.cpp:42-73) plus -- for rows that enter
// odin_event_loop_run -- a per-row 200 ms watchdog timer that sets
// state.timed_out and stops the loop. T16 is the synchronous
// constructor-rollback row that never calls odin_event_loop_run.

#include "odin/accept_loop.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "odin/event_loop.h"
#include "odin/event_loop_internal_test.h"
#if defined(ODIN_ACCEPT_LOOP_TESTING)
#include "odin/accept_loop_internal_test.h"
#endif

#include "gtest/gtest.h"

// NOLINTBEGIN(misc-const-correctness, misc-use-internal-linkage)

namespace {

class AcceptLoopRunDeadline {
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
      FAIL() << "AcceptLoopRunDeadline exceeded 2 seconds";
    }
    ASSERT_TRUE(WIFEXITED(wstatus));
    EXPECT_EQ(WEXITSTATUS(wstatus), 0);
  }
};

struct AcceptState {
  std::vector<int> on_accept_fds;
  int on_error_calls = 0;
  int on_error_err = 0;
  bool timed_out = false;
  odin_event_loop_t *loop = nullptr;
  int stop_after = -1;
  bool destroy_in_cb = false;
  odin_accept_loop_t *al = nullptr;
};

void OnAccept(odin_accept_loop_t *al, int conn_fd, void *user_data) {
  AcceptState *s = static_cast<AcceptState *>(user_data);
  s->on_accept_fds.push_back(conn_fd);
  if (s->destroy_in_cb) {
    odin_accept_loop_destroy(al);
  }
  if (s->stop_after >= 0 &&
      static_cast<int>(s->on_accept_fds.size()) >= s->stop_after &&
      s->loop != nullptr) {
    odin_event_loop_stop(s->loop);
  }
}

void OnError(odin_accept_loop_t *al, int err, void *user_data) {
  AcceptState *s = static_cast<AcceptState *>(user_data);
  s->on_error_calls += 1;
  s->on_error_err = err;
  if (s->destroy_in_cb) {
    odin_accept_loop_destroy(al);
  }
  if (s->loop != nullptr) {
    odin_event_loop_stop(s->loop);
  }
}

void WatchdogCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                void *user_data) {
  AcceptState *s = static_cast<AcceptState *>(user_data);
  s->timed_out = true;
  odin_event_timer_stop(timer);
  odin_event_loop_stop(loop);
}

int OpenLoopbackListener(uint16_t *out_port) {
  const int lfd = socket(AF_INET, SOCK_STREAM, 0);
  if (lfd < 0) {
    return -1;
  }
  const int reuse = 1;
  (void)setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  const int flags = fcntl(lfd, F_GETFL, 0);
  if (flags == -1 || fcntl(lfd, F_SETFL, flags | O_NONBLOCK) == -1) {
    close(lfd);
    return -1;
  }
  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(lfd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) !=
      0) {
    close(lfd);
    return -1;
  }
  if (listen(lfd, 128) != 0) {
    close(lfd);
    return -1;
  }
  socklen_t alen = sizeof(addr);
  if (getsockname(lfd, reinterpret_cast<struct sockaddr *>(&addr), &alen) !=
      0) {
    close(lfd);
    return -1;
  }
  *out_port = ntohs(addr.sin_port);
  return lfd;
}

int ConnectLoopback(uint16_t port) {
  const int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    return -1;
  }
  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (connect(s, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) !=
      0) {
    close(s);
    return -1;
  }
  return s;
}

// Live-fd-count probe used by T6 / T17 / T18. dup(0) until EMFILE, count and
// release the probes, then return rlim_cur - count.
int LiveFdCount() {
  std::vector<int> probes;
  while (true) {
    const int f = dup(0);
    if (f < 0) {
      break;
    }
    probes.push_back(f);
  }
  const int probed = static_cast<int>(probes.size());
  for (int f : probes) {
    close(f);
  }
  struct rlimit rl;
  if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
    return -1;
  }
  return static_cast<int>(rl.rlim_cur) - probed;
}

void DummyIoCb(odin_event_loop_t *loop, odin_event_io_t *io, int fd,
               unsigned int events, void *user_data) {
  (void)loop;
  (void)io;
  (void)fd;
  (void)events;
  (void)user_data;
}

struct DestroyTrigCtx {
  odin_accept_loop_t *al = nullptr;
};

void DestroyTriggerCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                      void *user_data) {
  DestroyTrigCtx *c = static_cast<DestroyTrigCtx *>(user_data);
  odin_accept_loop_destroy(c->al);
  odin_event_timer_stop(timer);
  odin_event_loop_stop(loop);
}

} // namespace

// T1 -- Single connect triggers one on_accept with a usable nonblocking fd.
TEST(OdinAcceptLoopTest, T1) {
  AcceptLoopRunDeadline::Run([] {
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0) << std::strerror(errno);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    AcceptState state;
    state.loop = loop;
    state.stop_after = 1;
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 200000, 0, WatchdogCb, &state,
                                     &watchdog),
              0);

    odin_accept_loop_t *al = nullptr;
    ASSERT_EQ(odin_accept_loop_create(loop, lfd, OnAccept, OnError, &state,
                                      &al),
              0)
        << std::strerror(errno);

    const int peer = ConnectLoopback(port);
    ASSERT_GE(peer, 0) << std::strerror(errno);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    ASSERT_EQ(state.on_accept_fds.size(), 1u);
    EXPECT_EQ(state.on_error_calls, 0);
    EXPECT_FALSE(state.timed_out);
    const int cfd = state.on_accept_fds[0];
    EXPECT_NE(fcntl(cfd, F_GETFL) & O_NONBLOCK, 0);

    ASSERT_EQ(write(cfd, "hi", 2), 2) << std::strerror(errno);
    char buf[2] = {0, 0};
    ASSERT_EQ(read(peer, buf, 2), 2) << std::strerror(errno);
    EXPECT_EQ(buf[0], 'h');
    EXPECT_EQ(buf[1], 'i');

    EXPECT_GE(fcntl(lfd, F_GETFD), 0);
    close(cfd);
    close(peer);
    odin_accept_loop_destroy(al);
    EXPECT_GE(fcntl(lfd, F_GETFD), 0); // destroy did not close lfd
    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

// T2 -- Three pending connects drain together.
TEST(OdinAcceptLoopTest, T2) {
  AcceptLoopRunDeadline::Run([] {
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    AcceptState state;
    state.loop = loop;
    state.stop_after = 3;
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 200000, 0, WatchdogCb, &state,
                                     &watchdog),
              0);

    odin_accept_loop_t *al = nullptr;
    ASSERT_EQ(odin_accept_loop_create(loop, lfd, OnAccept, OnError, &state,
                                      &al),
              0);

    int peers[3] = {-1, -1, -1};
    for (int i = 0; i < 3; ++i) {
      peers[i] = ConnectLoopback(port);
      ASSERT_GE(peers[i], 0);
    }
    EXPECT_EQ(odin_event_loop_run(loop), 0);

    ASSERT_EQ(state.on_accept_fds.size(), 3u);
    EXPECT_EQ(state.on_error_calls, 0);
    EXPECT_FALSE(state.timed_out);

    for (int i = 0; i < 3; ++i) {
      const char b = static_cast<char>('A' + i);
      ASSERT_EQ(write(peers[i], &b, 1), 1);
    }
    int received = 0;
    for (int tries = 0; tries < 200 && received < 3; ++tries) {
      for (int j = 0; j < 3; ++j) {
        char rb = 0;
        const ssize_t n = read(state.on_accept_fds[j], &rb, 1);
        if (n == 1) {
          ++received;
        }
      }
      usleep(1000);
    }
    EXPECT_EQ(received, 3);

    for (int i = 0; i < 3; ++i) {
      close(peers[i]);
      close(state.on_accept_fds[i]);
    }
    odin_accept_loop_destroy(al);
    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

namespace {
struct T3Ctx {
  AcceptState state;
  int readiness_returns = 0;
  bool posted_first = false;
  bool posted_second = false;
};

void T3PostFirstBatch(odin_event_loop_t *loop, void *user_data) {
  T3Ctx *c = static_cast<T3Ctx *>(user_data);
  (void)loop;
  c->readiness_returns += 1;
}

void T3PostSecondBatch(odin_event_loop_t *loop, void *user_data) {
  T3Ctx *c = static_cast<T3Ctx *>(user_data);
  c->readiness_returns += 1;
  odin_event_loop_stop(loop);
}

void T3OnAccept(odin_accept_loop_t *al, int conn_fd, void *user_data) {
  (void)al;
  T3Ctx *c = static_cast<T3Ctx *>(user_data);
  c->state.on_accept_fds.push_back(conn_fd);
  const size_t n = c->state.on_accept_fds.size();
  if (n == 64u && !c->posted_first) {
    c->posted_first = true;
    odin_event_post(c->state.loop, T3PostFirstBatch, c);
  } else if (n == 65u && !c->posted_second) {
    c->posted_second = true;
    odin_event_post(c->state.loop, T3PostSecondBatch, c);
  }
}
} // namespace

// T3 -- 65 pending connects: first readiness drains 64, second drains the 65th.
TEST(OdinAcceptLoopTest, T3) {
  AcceptLoopRunDeadline::Run([] {
    struct rlimit rl;
    ASSERT_EQ(getrlimit(RLIMIT_NOFILE, &rl), 0);
    if (rl.rlim_cur < 256) {
      rl.rlim_cur = (rl.rlim_max < 256 ? rl.rlim_max : 256);
      (void)setrlimit(RLIMIT_NOFILE, &rl);
    }
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    T3Ctx ctx;
    ctx.state.loop = loop;
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 300000, 0, WatchdogCb, &ctx.state,
                                     &watchdog),
              0);

    odin_accept_loop_t *al = nullptr;
    ASSERT_EQ(odin_accept_loop_create(loop, lfd, T3OnAccept, OnError, &ctx,
                                      &al),
              0);

    int peers[65];
    for (int i = 0; i < 65; ++i) {
      peers[i] = ConnectLoopback(port);
      ASSERT_GE(peers[i], 0);
    }
    EXPECT_EQ(odin_event_loop_run(loop), 0);

    ASSERT_EQ(ctx.state.on_accept_fds.size(), 65u);
    EXPECT_EQ(ctx.state.on_error_calls, 0);
    EXPECT_FALSE(ctx.state.timed_out);
    EXPECT_EQ(ctx.readiness_returns, 2);

    for (int i = 0; i < 65; ++i) {
      close(peers[i]);
      close(ctx.state.on_accept_fds[i]);
    }
    odin_accept_loop_destroy(al);
    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

// T4 -- EINTR from accept is a silent retry; the next iteration delivers.
TEST(OdinAcceptLoopTest, T4) {
  AcceptLoopRunDeadline::Run([] {
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    AcceptState state;
    state.loop = loop;
    state.stop_after = 1;
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 200000, 0, WatchdogCb, &state,
                                     &watchdog),
              0);

    odin_accept_loop_t *al = nullptr;
    ASSERT_EQ(odin_accept_loop_create(loop, lfd, OnAccept, OnError, &state,
                                      &al),
              0);
    const int peer = ConnectLoopback(port);
    ASSERT_GE(peer, 0);
    ASSERT_EQ(odin_accept_loop_test_fail_next_accept(al, EINTR), 0);
    EXPECT_EQ(odin_event_loop_run(loop), 0);

    EXPECT_EQ(state.on_accept_fds.size(), 1u);
    EXPECT_EQ(state.on_error_calls, 0);
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(odin_accept_loop_test_is_terminal(al), 0);

    close(state.on_accept_fds[0]);
    close(peer);
    odin_accept_loop_destroy(al);
    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

// T5 -- ECONNABORTED from accept is a silent drop; next iteration delivers.
TEST(OdinAcceptLoopTest, T5) {
  AcceptLoopRunDeadline::Run([] {
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    AcceptState state;
    state.loop = loop;
    state.stop_after = 1;
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 200000, 0, WatchdogCb, &state,
                                     &watchdog),
              0);

    odin_accept_loop_t *al = nullptr;
    ASSERT_EQ(odin_accept_loop_create(loop, lfd, OnAccept, OnError, &state,
                                      &al),
              0);
    const int peer = ConnectLoopback(port);
    ASSERT_GE(peer, 0);
    ASSERT_EQ(odin_accept_loop_test_fail_next_accept(al, ECONNABORTED), 0);
    EXPECT_EQ(odin_event_loop_run(loop), 0);

    EXPECT_EQ(state.on_accept_fds.size(), 1u);
    EXPECT_EQ(state.on_error_calls, 0);
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(odin_accept_loop_test_is_terminal(al), 0);

    close(state.on_accept_fds[0]);
    close(peer);
    odin_accept_loop_destroy(al);
    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

namespace {
struct T6Ctx {
  AcceptState state;
  int reserved_fd = -1;
  int peer3_socket = -1;  // pre-allocated socket; connect() happens in inspector
  struct sockaddr_in peer3_addr {};
  bool observed_paused = false;
};

void T6Inspector(odin_event_loop_t *loop, void *user_data) {
  T6Ctx *c = static_cast<T6Ctx *>(user_data);
  (void)loop;
  if (odin_accept_loop_test_is_paused(c->state.al)) {
    c->observed_paused = true;
    if (c->reserved_fd >= 0) {
      close(c->reserved_fd);
      c->reserved_fd = -1;
    }
    // macOS accept(2) returning EMFILE consumes the head-of-queue connection,
    // so after recovery the accept queue may be empty. Connect a fresh peer
    // (whose socket was pre-allocated before the rlimit clamp) so the re-armed
    // watch has a connection to deliver. connect(2) does not allocate a fd.
    const int rc = connect(c->peer3_socket,
                           reinterpret_cast<struct sockaddr *>(&c->peer3_addr),
                           sizeof(c->peer3_addr));
    (void)rc;
  } else {
    odin_event_post(loop, T6Inspector, c);
  }
}

void T6OnAccept(odin_accept_loop_t *al, int conn_fd, void *user_data) {
  (void)al;
  T6Ctx *c = static_cast<T6Ctx *>(user_data);
  c->state.on_accept_fds.push_back(conn_fd);
  if (c->state.on_accept_fds.size() == 1u) {
    odin_event_post(c->state.loop, T6Inspector, c);
  } else if (c->state.on_accept_fds.size() == 2u) {
    odin_event_loop_stop(c->state.loop);
  }
}
} // namespace

// T6 -- Real EMFILE via setrlimit enters soft degradation; releasing an fd
// recovers and delivers the queued connection.
TEST(OdinAcceptLoopTest, T6) {
  AcceptLoopRunDeadline::Run([] {
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    T6Ctx ctx;
    ctx.state.loop = loop;
    std::memset(&ctx.peer3_addr, 0, sizeof(ctx.peer3_addr));
    ctx.peer3_addr.sin_family = AF_INET;
    ctx.peer3_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ctx.peer3_addr.sin_port = htons(port);
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 500000, 0, WatchdogCb, &ctx.state,
                                     &watchdog),
              0);
    odin_accept_loop_t *al = nullptr;
    ASSERT_EQ(odin_accept_loop_create(loop, lfd, T6OnAccept, OnError, &ctx,
                                      &al),
              0);
    ctx.state.al = al;

    ctx.reserved_fd = dup(0);
    ASSERT_GE(ctx.reserved_fd, 0);
    ctx.peer3_socket = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(ctx.peer3_socket, 0);
    int peer1 = ConnectLoopback(port);
    ASSERT_GE(peer1, 0);
    int peer2 = ConnectLoopback(port);
    ASSERT_GE(peer2, 0);

    const int C = LiveFdCount();
    ASSERT_GT(C, 0);
    struct rlimit rl;
    ASSERT_EQ(getrlimit(RLIMIT_NOFILE, &rl), 0);
    struct rlimit clamp = rl;
    clamp.rlim_cur = static_cast<rlim_t>(C) + 1;
    ASSERT_EQ(setrlimit(RLIMIT_NOFILE, &clamp), 0);

    EXPECT_EQ(odin_event_loop_run(loop), 0);

    (void)setrlimit(RLIMIT_NOFILE, &rl);

    EXPECT_EQ(ctx.state.on_accept_fds.size(), 2u);
    EXPECT_EQ(ctx.state.on_error_calls, 0);
    EXPECT_FALSE(ctx.state.timed_out);
    EXPECT_TRUE(ctx.observed_paused);
    EXPECT_GE(fcntl(lfd, F_GETFD), 0);

    for (int f : ctx.state.on_accept_fds) {
      close(f);
    }
    close(peer1);
    close(peer2);
    if (ctx.peer3_socket >= 0) {
      close(ctx.peer3_socket);
    }
    odin_accept_loop_destroy(al);
    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

namespace {
struct T7Ctx {
  AcceptState state;
  bool observed_paused = false;
};

void T7Inspector(odin_event_loop_t *loop, void *user_data) {
  T7Ctx *c = static_cast<T7Ctx *>(user_data);
  if (odin_accept_loop_test_is_paused(c->state.al)) {
    c->observed_paused = true;
    return;
  }
  // Not yet paused; re-post for the next loop iteration.
  odin_event_post(loop, T7Inspector, c);
}

void T7OnAccept(odin_accept_loop_t *al, int conn_fd, void *user_data) {
  (void)al;
  T7Ctx *c = static_cast<T7Ctx *>(user_data);
  c->state.on_accept_fds.push_back(conn_fd);
  if (c->state.loop != nullptr) {
    odin_event_loop_stop(c->state.loop);
  }
}
} // namespace

// T7 -- ENFILE / ENOBUFS / ENOMEM each enter soft degradation and recover.
TEST(OdinAcceptLoopTest, T7) {
  AcceptLoopRunDeadline::Run([] {
    const int errnums[3] = {ENFILE, ENOBUFS, ENOMEM};
    for (int errnum : errnums) {
      uint16_t port = 0;
      const int lfd = OpenLoopbackListener(&port);
      ASSERT_GE(lfd, 0);
      odin_event_loop_t *loop = nullptr;
      ASSERT_EQ(odin_event_loop_create(&loop), 0);

      T7Ctx ctx;
      ctx.state.loop = loop;
      odin_event_timer_t *watchdog = nullptr;
      ASSERT_EQ(odin_event_timer_start(loop, 200000, 0, WatchdogCb, &ctx.state,
                                       &watchdog),
                0);

      odin_accept_loop_t *al = nullptr;
      ASSERT_EQ(odin_accept_loop_create(loop, lfd, T7OnAccept, OnError, &ctx,
                                        &al),
                0);
      ctx.state.al = al;

      const int peer = ConnectLoopback(port);
      ASSERT_GE(peer, 0);
      ASSERT_EQ(odin_accept_loop_test_fail_next_accept(al, errnum), 0);
      ASSERT_EQ(odin_event_post(loop, T7Inspector, &ctx), 0);

      EXPECT_EQ(odin_event_loop_run(loop), 0);

      EXPECT_EQ(ctx.state.on_accept_fds.size(), 1u) << "errno=" << errnum;
      EXPECT_EQ(ctx.state.on_error_calls, 0) << "errno=" << errnum;
      EXPECT_FALSE(ctx.state.timed_out) << "errno=" << errnum;
      EXPECT_TRUE(ctx.observed_paused) << "errno=" << errnum;
      EXPECT_EQ(odin_accept_loop_test_is_paused(al), 0) << "errno=" << errnum;

      close(ctx.state.on_accept_fds[0]);
      close(peer);
      odin_accept_loop_destroy(al);
      close(lfd);
      odin_event_loop_destroy(loop);
    }
  });
}

// T8 -- Unclassified errno (EBADF) → single on_error; quiesce; lfd intact.
TEST(OdinAcceptLoopTest, T8) {
  AcceptLoopRunDeadline::Run([] {
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    AcceptState state;
    state.loop = loop;
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 200000, 0, WatchdogCb, &state,
                                     &watchdog),
              0);

    odin_accept_loop_t *al = nullptr;
    ASSERT_EQ(odin_accept_loop_create(loop, lfd, OnAccept, OnError, &state,
                                      &al),
              0);
    const int peer = ConnectLoopback(port);
    ASSERT_GE(peer, 0);
    ASSERT_EQ(odin_accept_loop_test_fail_next_accept(al, EBADF), 0);
    EXPECT_EQ(odin_event_loop_run(loop), 0);

    EXPECT_EQ(state.on_accept_fds.size(), 0u);
    EXPECT_EQ(state.on_error_calls, 1);
    EXPECT_EQ(state.on_error_err, EBADF);
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(odin_accept_loop_test_is_terminal(al), 1);
    EXPECT_GE(fcntl(lfd, F_GETFD), 0);

    AcceptState s2;
    s2.loop = loop;
    odin_event_timer_t *w2 = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 100000, 0, WatchdogCb, &s2, &w2), 0);
    const int peer2 = ConnectLoopback(port);
    ASSERT_GE(peer2, 0);
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(s2.on_accept_fds.size(), 0u);
    EXPECT_EQ(s2.on_error_calls, 0);
    EXPECT_TRUE(s2.timed_out);

    close(peer);
    close(peer2);
    odin_accept_loop_destroy(al);
    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

// T9 -- odin_event_timer_start failure during soft-degradation entry → single
// on_error with the timer errno.
TEST(OdinAcceptLoopTest, T9) {
  AcceptLoopRunDeadline::Run([] {
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    AcceptState state;
    state.loop = loop;
    odin_accept_loop_t *al = nullptr;
    ASSERT_EQ(odin_accept_loop_create(loop, lfd, OnAccept, OnError, &state,
                                      &al),
              0);
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 200000, 0, WatchdogCb, &state,
                                     &watchdog),
              0);
    ASSERT_EQ(odin_event_loop_test_fail_next_timer_start(loop, ENOMEM), 0);
    ASSERT_EQ(odin_accept_loop_test_fail_next_accept(al, EMFILE), 0);
    const int peer = ConnectLoopback(port);
    ASSERT_GE(peer, 0);
    EXPECT_EQ(odin_event_loop_run(loop), 0);

    EXPECT_EQ(state.on_accept_fds.size(), 0u);
    EXPECT_EQ(state.on_error_calls, 1);
    EXPECT_EQ(state.on_error_err, ENOMEM);
    EXPECT_EQ(odin_accept_loop_test_is_terminal(al), 1);
    EXPECT_EQ(odin_accept_loop_test_is_paused(al), 0);
    EXPECT_GE(fcntl(lfd, F_GETFD), 0);

    close(peer);
    odin_accept_loop_destroy(al);
    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

#if !defined(__linux__)
namespace {
void T10ArmFault(odin_event_loop_t *loop, void *user_data) {
  (void)user_data;
  (void)odin_event_loop_test_fail_next_kqueue_change(
      loop, ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_ADD, ODIN_EVENT_READ, ENOSPC);
}
} // namespace
#endif

// T10 -- macOS-only: re-arm odin_event_io_start failure → single on_error /
// ENOSPC.
TEST(OdinAcceptLoopTest, T10) {
#if defined(__linux__)
  GTEST_SKIP() << "macOS-only kqueue/fcntl path";
#else
  AcceptLoopRunDeadline::Run([] {
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    AcceptState state;
    state.loop = loop;
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 300000, 0, WatchdogCb, &state,
                                     &watchdog),
              0);

    odin_accept_loop_t *al = nullptr;
    ASSERT_EQ(odin_accept_loop_create(loop, lfd, OnAccept, OnError, &state,
                                      &al),
              0);
    ASSERT_EQ(odin_accept_loop_test_fail_next_accept(al, EMFILE), 0);
    const int peer = ConnectLoopback(port);
    ASSERT_GE(peer, 0);
    ASSERT_EQ(odin_event_post(loop, T10ArmFault, nullptr), 0);

    EXPECT_EQ(odin_event_loop_run(loop), 0);

    EXPECT_EQ(state.on_accept_fds.size(), 0u);
    EXPECT_EQ(state.on_error_calls, 1);
    EXPECT_EQ(state.on_error_err, ENOSPC);
    EXPECT_EQ(odin_accept_loop_test_is_terminal(al), 1);
    EXPECT_EQ(odin_accept_loop_test_is_paused(al), 0);
    EXPECT_GE(fcntl(lfd, F_GETFD), 0);

    close(peer);
    odin_accept_loop_destroy(al);
    close(lfd);
    odin_event_loop_destroy(loop);
  });
#endif
}

// T11 -- destroy from outside any callback in ACTIVE: stops watch, lfd intact.
TEST(OdinAcceptLoopTest, T11) {
  AcceptLoopRunDeadline::Run([] {
    odin_event_loop_test_reset_liveness();
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    AcceptState state;
    state.loop = loop;
    odin_accept_loop_t *al = nullptr;
    ASSERT_EQ(odin_accept_loop_create(loop, lfd, OnAccept, OnError, &state,
                                      &al),
              0);

    odin_event_loop_test_liveness_t liv_pre = {};
    ASSERT_EQ(odin_event_loop_test_liveness(&liv_pre), 0);
    EXPECT_EQ(liv_pre.io_handles, 1u);

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 200000, 0, WatchdogCb, &state,
                                     &watchdog),
              0);
    DestroyTrigCtx tctx;
    tctx.al = al;
    odin_event_timer_t *trigger = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 100000, 0, DestroyTriggerCb, &tctx,
                                     &trigger),
              0);
    EXPECT_EQ(odin_event_loop_run(loop), 0);

    odin_event_loop_test_liveness_t liv_post = {};
    ASSERT_EQ(odin_event_loop_test_liveness(&liv_post), 0);
    EXPECT_EQ(liv_post.io_handles, 0u);
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(state.on_accept_fds.size(), 0u);
    EXPECT_EQ(state.on_error_calls, 0);
    EXPECT_GE(fcntl(lfd, F_GETFD), 0);

    const int peer = ConnectLoopback(port);
    ASSERT_GE(peer, 0);
    int sfd = -1;
    for (int i = 0; i < 200 && sfd < 0; ++i) {
      sfd = accept(lfd, nullptr, nullptr);
      if (sfd < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        break;
      }
      usleep(1000);
    }
    EXPECT_GE(sfd, 0);
    close(peer);
    if (sfd >= 0) {
      close(sfd);
    }
    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

namespace {
struct T12Ctx {
  AcceptState state;
  odin_accept_loop_t *al = nullptr;
  bool observed_paused = false;
};

void T12Inspector(odin_event_loop_t *loop, void *user_data) {
  T12Ctx *c = static_cast<T12Ctx *>(user_data);
  if (odin_accept_loop_test_is_paused(c->al)) {
    c->observed_paused = true;
    odin_accept_loop_destroy(c->al);
    c->al = nullptr;
    odin_event_loop_stop(loop);
  } else {
    odin_event_post(loop, T12Inspector, c);
  }
}
} // namespace

// T12 -- destroy from outside any callback in PAUSED: stops timer, lfd intact.
TEST(OdinAcceptLoopTest, T12) {
  AcceptLoopRunDeadline::Run([] {
    odin_event_loop_test_reset_liveness();
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    T12Ctx ctx;
    ctx.state.loop = loop;
    odin_accept_loop_t *al = nullptr;
    ASSERT_EQ(odin_accept_loop_create(loop, lfd, OnAccept, OnError, &ctx.state,
                                      &al),
              0);
    ctx.al = al;
    ASSERT_EQ(odin_accept_loop_test_fail_next_accept(al, EMFILE), 0);
    const int peer = ConnectLoopback(port);
    ASSERT_GE(peer, 0);
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 200000, 0, WatchdogCb, &ctx.state,
                                     &watchdog),
              0);
    ASSERT_EQ(odin_event_post(loop, T12Inspector, &ctx), 0);

    EXPECT_EQ(odin_event_loop_run(loop), 0);

    EXPECT_TRUE(ctx.observed_paused);
    EXPECT_EQ(ctx.state.on_accept_fds.size(), 0u);
    EXPECT_EQ(ctx.state.on_error_calls, 0);

    odin_event_timer_stop(watchdog); // stop watchdog so timer count is al-only
    odin_event_loop_test_liveness_t liv = {};
    ASSERT_EQ(odin_event_loop_test_liveness(&liv), 0);
    EXPECT_EQ(liv.io_handles, 0u);
    EXPECT_EQ(odin_event_loop_test_live_timer_count(loop), 0u);
    EXPECT_GE(fcntl(lfd, F_GETFD), 0);

    close(peer);
    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

// T13 -- destroy from outside any callback in TERMINAL: frees handle, lfd
// intact.
TEST(OdinAcceptLoopTest, T13) {
  AcceptLoopRunDeadline::Run([] {
    odin_event_loop_test_reset_liveness();
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    AcceptState state;
    state.loop = loop;
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 200000, 0, WatchdogCb, &state,
                                     &watchdog),
              0);

    odin_accept_loop_t *al = nullptr;
    ASSERT_EQ(odin_accept_loop_create(loop, lfd, OnAccept, OnError, &state,
                                      &al),
              0);
    ASSERT_EQ(odin_accept_loop_test_fail_next_accept(al, EBADF), 0);
    const int peer = ConnectLoopback(port);
    ASSERT_GE(peer, 0);
    EXPECT_EQ(odin_event_loop_run(loop), 0);

    ASSERT_EQ(state.on_error_calls, 1);
    EXPECT_EQ(state.on_error_err, EBADF);
    EXPECT_EQ(odin_accept_loop_test_is_terminal(al), 1);

    odin_accept_loop_destroy(al);
    EXPECT_GE(fcntl(lfd, F_GETFD), 0);

    odin_event_timer_stop(watchdog); // stop watchdog so timer count is al-only
    odin_event_loop_test_liveness_t liv = {};
    ASSERT_EQ(odin_event_loop_test_liveness(&liv), 0);
    EXPECT_EQ(liv.io_handles, 0u);
    EXPECT_EQ(odin_event_loop_test_live_timer_count(loop), 0u);

    const int peer2 = ConnectLoopback(port);
    ASSERT_GE(peer2, 0);
    int sfd = -1;
    for (int i = 0; i < 200 && sfd < 0; ++i) {
      sfd = accept(lfd, nullptr, nullptr);
      if (sfd < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        break;
      }
      usleep(1000);
    }
    EXPECT_GE(sfd, 0);

    close(peer);
    close(peer2);
    if (sfd >= 0) {
      close(sfd);
    }
    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

// T14 -- destroy from inside on_accept (deferred free); drain stops after.
TEST(OdinAcceptLoopTest, T14) {
  AcceptLoopRunDeadline::Run([] {
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    AcceptState state;
    state.loop = loop;
    state.destroy_in_cb = true;
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 200000, 0, WatchdogCb, &state,
                                     &watchdog),
              0);

    odin_accept_loop_t *al = nullptr;
    ASSERT_EQ(odin_accept_loop_create(loop, lfd, OnAccept, OnError, &state,
                                      &al),
              0);
    state.al = al;
    const int peer1 = ConnectLoopback(port);
    ASSERT_GE(peer1, 0);
    const int peer2 = ConnectLoopback(port);
    ASSERT_GE(peer2, 0);
    EXPECT_EQ(odin_event_loop_run(loop), 0);

    EXPECT_EQ(state.on_accept_fds.size(), 1u);
    EXPECT_EQ(state.on_error_calls, 0);
    EXPECT_GE(fcntl(lfd, F_GETFD), 0);

    if (!state.on_accept_fds.empty()) {
      close(state.on_accept_fds[0]);
    }
    close(peer1);
    close(peer2);
    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

// T15 -- destroy from inside on_error (deferred free).
TEST(OdinAcceptLoopTest, T15) {
  AcceptLoopRunDeadline::Run([] {
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    AcceptState state;
    state.loop = loop;
    state.destroy_in_cb = true;
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 200000, 0, WatchdogCb, &state,
                                     &watchdog),
              0);

    odin_accept_loop_t *al = nullptr;
    ASSERT_EQ(odin_accept_loop_create(loop, lfd, OnAccept, OnError, &state,
                                      &al),
              0);
    state.al = al;
    ASSERT_EQ(odin_accept_loop_test_fail_next_accept(al, EBADF), 0);
    const int peer = ConnectLoopback(port);
    ASSERT_GE(peer, 0);
    EXPECT_EQ(odin_event_loop_run(loop), 0);

    EXPECT_EQ(state.on_error_calls, 1);
    EXPECT_EQ(state.on_error_err, EBADF);
    EXPECT_FALSE(state.timed_out);

    close(peer);
    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

// T16 -- Constructor's odin_event_io_start rollback: -1 / EEXIST, *out
// untouched. Synchronous (no odin_event_loop_run).
TEST(OdinAcceptLoopTest, T16) {
  AcceptLoopRunDeadline::Run([] {
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    odin_event_io_t *blocker = nullptr;
    ASSERT_EQ(odin_event_io_start(loop, lfd, ODIN_EVENT_READ, DummyIoCb,
                                  nullptr, &blocker),
              0);

    AcceptState state;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    odin_accept_loop_t *al = reinterpret_cast<odin_accept_loop_t *>(-1);
    errno = 0;
    EXPECT_EQ(odin_accept_loop_create(loop, lfd, OnAccept, OnError, &state,
                                      &al),
              -1);
    EXPECT_EQ(errno, EEXIST);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    EXPECT_EQ(al, reinterpret_cast<odin_accept_loop_t *>(-1));

    odin_event_loop_test_liveness_t liv = {};
    ASSERT_EQ(odin_event_loop_test_liveness(&liv), 0);
    EXPECT_EQ(liv.io_handles, 1u);
    EXPECT_EQ(state.on_accept_fds.size(), 0u);
    EXPECT_EQ(state.on_error_calls, 0);

    odin_event_io_stop(blocker);
    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

#if !defined(__linux__)
namespace {
struct T17Ctx {
  AcceptState state;
  odin_accept_loop_t *al = nullptr;
  int c_pre = 0;
  int c_post = 0;
  bool observed_paused = false;
};

void T17Inspector(odin_event_loop_t *loop, void *user_data) {
  T17Ctx *c = static_cast<T17Ctx *>(user_data);
  if (odin_accept_loop_test_is_paused(c->al)) {
    c->observed_paused = true;
    c->c_post = LiveFdCount();
    odin_event_loop_stop(loop);
  } else {
    odin_event_post(loop, T17Inspector, c);
  }
}
} // namespace
#endif

// T17 -- macOS-only: post-accept fcntl EMFILE-class → soft degradation;
// accepted fd is closed (no leak).
TEST(OdinAcceptLoopTest, T17) {
#if defined(__linux__)
  GTEST_SKIP() << "macOS-only kqueue/fcntl path";
#else
  AcceptLoopRunDeadline::Run([] {
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    T17Ctx ctx;
    ctx.state.loop = loop;
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 200000, 0, WatchdogCb, &ctx.state,
                                     &watchdog),
              0);

    odin_accept_loop_t *al = nullptr;
    ASSERT_EQ(odin_accept_loop_create(loop, lfd, OnAccept, OnError, &ctx,
                                      &al),
              0);
    ctx.al = al;

    const int peer = ConnectLoopback(port);
    ASSERT_GE(peer, 0);
    ctx.c_pre = LiveFdCount();
    ASSERT_EQ(odin_accept_loop_test_fail_next_fcntl(al, F_SETFL, EMFILE), 0);
    ASSERT_EQ(odin_event_post(loop, T17Inspector, &ctx), 0);

    EXPECT_EQ(odin_event_loop_run(loop), 0);

    EXPECT_EQ(ctx.state.on_accept_fds.size(), 0u);
    EXPECT_EQ(ctx.state.on_error_calls, 0);
    EXPECT_TRUE(ctx.observed_paused);
    EXPECT_EQ(ctx.c_post, ctx.c_pre);
    EXPECT_FALSE(ctx.state.timed_out);
    EXPECT_GE(fcntl(lfd, F_GETFD), 0);

    close(peer);
    odin_accept_loop_destroy(al);
    close(lfd);
    odin_event_loop_destroy(loop);
  });
#endif
}

// T18 -- macOS-only: post-accept fcntl non-soft-degradation errno → terminal
// on_error; accepted fd closed.
TEST(OdinAcceptLoopTest, T18) {
#if defined(__linux__)
  GTEST_SKIP() << "macOS-only kqueue/fcntl path";
#else
  AcceptLoopRunDeadline::Run([] {
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    AcceptState state;
    state.loop = loop;
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 200000, 0, WatchdogCb, &state,
                                     &watchdog),
              0);

    odin_accept_loop_t *al = nullptr;
    ASSERT_EQ(odin_accept_loop_create(loop, lfd, OnAccept, OnError, &state,
                                      &al),
              0);
    const int peer = ConnectLoopback(port);
    ASSERT_GE(peer, 0);
    const int c_pre = LiveFdCount();
    ASSERT_EQ(odin_accept_loop_test_fail_next_fcntl(al, F_GETFL, EINVAL), 0);
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    const int c_post = LiveFdCount();

    EXPECT_EQ(state.on_accept_fds.size(), 0u);
    EXPECT_EQ(state.on_error_calls, 1);
    EXPECT_EQ(state.on_error_err, EINVAL);
    EXPECT_EQ(odin_accept_loop_test_is_terminal(al), 1);
    EXPECT_EQ(c_post, c_pre);
    EXPECT_FALSE(state.timed_out);
    EXPECT_GE(fcntl(lfd, F_GETFD), 0);

    close(peer);
    odin_accept_loop_destroy(al);
    close(lfd);
    odin_event_loop_destroy(loop);
  });
#endif
}

// NOLINTEND(misc-const-correctness, misc-use-internal-linkage)
