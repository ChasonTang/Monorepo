#include "odin/event_loop_internal_test.h"

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "gtest/gtest.h"

// NOLINTBEGIN(misc-const-correctness, misc-use-internal-linkage)

namespace {

class EventLoopRunDeadline {
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
      FAIL() << "EventLoopRunDeadline exceeded 2 seconds";
    }
    ASSERT_TRUE(WIFEXITED(wstatus));
    EXPECT_EQ(WEXITSTATUS(wstatus), 0);
  }
};

void ExpectOk(int rc) { EXPECT_EQ(rc, 0) << std::strerror(errno); }

void AssertOk(int rc) { ASSERT_EQ(rc, 0) << std::strerror(errno); }

void CreateNonblockingSocketpair(int fds[2]) {
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0)
      << std::strerror(errno);
  for (int i = 0; i < 2; ++i) {
    const int flags = fcntl(fds[i], F_GETFL, 0);
    ASSERT_NE(flags, -1) << std::strerror(errno);
    ASSERT_EQ(fcntl(fds[i], F_SETFL, flags | O_NONBLOCK), 0)
        << std::strerror(errno);
  }
}

void CloseFd(int fd) {
  if (fd >= 0) {
    EXPECT_EQ(close(fd), 0) << std::strerror(errno);
  }
}

void ClosePair(int fds[2]) {
  CloseFd(fds[0]);
  CloseFd(fds[1]);
}

void WriteOne(int fd, char byte) {
  ASSERT_EQ(write(fd, &byte, 1), 1) << std::strerror(errno);
}

void ReadOne(int fd, char expected) {
  char byte = 0;
  ASSERT_EQ(read(fd, &byte, 1), 1) << std::strerror(errno);
  EXPECT_EQ(byte, expected);
}

void AppendInt(int *values, int *count, int value) {
  values[*count] = value;
  *count += 1;
}

void AppendEvent(unsigned int *values, int *count, unsigned int value) {
  values[*count] = value;
  *count += 1;
}

void ExpectNoArmedTimer(const odin_event_loop_test_wait_record_t &wait) {
  if (wait.backend == ODIN_EVENT_LOOP_TEST_BACKEND_LINUX) {
    EXPECT_EQ(wait.linux_timerfd.armed, 0);
  } else {
    EXPECT_EQ(wait.macos_kevent.timeout_is_null, 1);
  }
}

void StopTask(odin_event_loop_t *loop, void *user_data) {
  int *calls = static_cast<int *>(user_data);
  *calls += 1;
  odin_event_loop_stop(loop);
}

} // namespace

TEST(OdinEventLoopTest, T1) {
  EventLoopRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    int calls = 0;
    AssertOk(odin_event_loop_create(&loop));
    ExpectOk(odin_event_post(loop, StopTask, &calls));
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(calls, 1);
    odin_event_loop_destroy(loop);
  });
}

namespace {
struct T2State {
  int fds[2];
  int calls;
  int observed_fd[2];
  unsigned int observed_mask[2];
  void *observed_user[2];
  bool timed_out;
  odin_event_io_t *io;
  odin_event_timer_t *watchdog;
};

void T2ReadCb(odin_event_loop_t *loop, odin_event_io_t *io, int fd,
              unsigned int events, void *user_data) {
  T2State *state = static_cast<T2State *>(user_data);
  state->observed_fd[state->calls] = fd;
  state->observed_mask[state->calls] = events;
  state->observed_user[state->calls] = user_data;
  state->calls += 1;
  if (state->calls == 2) {
    char byte = 0;
    EXPECT_EQ(read(fd, &byte, 1), 1);
    EXPECT_EQ(byte, 'x');
    odin_event_io_stop(io);
    odin_event_timer_stop(state->watchdog);
    odin_event_loop_stop(loop);
  }
}

void T2WatchdogCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                  void *user_data) {
  T2State *state = static_cast<T2State *>(user_data);
  state->timed_out = true;
  odin_event_timer_stop(timer);
  odin_event_loop_stop(loop);
}
} // namespace

TEST(OdinEventLoopTest, T2) {
  EventLoopRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    T2State state = {};
    CreateNonblockingSocketpair(state.fds);
    AssertOk(odin_event_loop_create(&loop));
    AssertOk(odin_event_io_start(loop, state.fds[1], ODIN_EVENT_READ, T2ReadCb,
                                 &state, &state.io));
    AssertOk(odin_event_timer_start(loop, 100000, 0, T2WatchdogCb, &state,
                                    &state.watchdog));
    WriteOne(state.fds[0], 'x');
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(state.calls, 2);
    EXPECT_EQ(state.observed_fd[0], state.fds[1]);
    EXPECT_EQ(state.observed_fd[1], state.fds[1]);
    EXPECT_NE(state.observed_mask[0] & ODIN_EVENT_READ, 0u);
    EXPECT_NE(state.observed_mask[1] & ODIN_EVENT_READ, 0u);
    EXPECT_EQ(state.observed_user[0], &state);
    EXPECT_EQ(state.observed_user[1], &state);
    EXPECT_FALSE(state.timed_out);
    odin_event_loop_destroy(loop);
    ClosePair(state.fds);
  });
}

namespace {
struct T3State {
  int fds[2];
  unsigned int sequence[2];
  int count;
};

void T3IoCb(odin_event_loop_t *loop, odin_event_io_t *io, int fd,
            unsigned int events, void *user_data) {
  T3State *state = static_cast<T3State *>(user_data);
  if (events & ODIN_EVENT_WRITE) {
    AppendEvent(state->sequence, &state->count, ODIN_EVENT_WRITE);
    ExpectOk(odin_event_io_update(io, ODIN_EVENT_READ));
    WriteOne(state->fds[0], 'r');
  } else if (events & ODIN_EVENT_READ) {
    AppendEvent(state->sequence, &state->count, ODIN_EVENT_READ);
    ReadOne(fd, 'r');
    odin_event_io_stop(io);
    odin_event_loop_stop(loop);
  }
}
} // namespace

TEST(OdinEventLoopTest, T3) {
  EventLoopRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    T3State state = {};
    odin_event_io_t *io = nullptr;
    CreateNonblockingSocketpair(state.fds);
    AssertOk(odin_event_loop_create(&loop));
    AssertOk(odin_event_io_start(loop, state.fds[1], ODIN_EVENT_WRITE, T3IoCb,
                                 &state, &io));
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    ASSERT_EQ(state.count, 2);
    EXPECT_EQ(state.sequence[0], ODIN_EVENT_WRITE);
    EXPECT_EQ(state.sequence[1], ODIN_EVENT_READ);
    odin_event_loop_destroy(loop);
    ClosePair(state.fds);
  });
}

namespace {
struct T4State {
  int timer_calls;
  int stop_task_calls;
};

void T4StopTask(odin_event_loop_t *loop, void *user_data) {
  T4State *state = static_cast<T4State *>(user_data);
  state->stop_task_calls += 1;
  odin_event_loop_stop(loop);
}

void T4TimerCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
               void *user_data) {
  (void)timer;
  T4State *state = static_cast<T4State *>(user_data);
  state->timer_calls += 1;
  ExpectOk(odin_event_post(loop, T4StopTask, state));
}
} // namespace

TEST(OdinEventLoopTest, T4) {
  EventLoopRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    odin_event_timer_t *timer = nullptr;
    T4State state = {};
    AssertOk(odin_event_loop_create(&loop));
    AssertOk(odin_event_timer_start(loop, 0, 0, T4TimerCb, &state, &timer));
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(state.timer_calls, 1);
    EXPECT_EQ(state.stop_task_calls, 1);
    EXPECT_EQ(odin_event_loop_test_live_timer_count(loop), 0u);
    odin_event_loop_test_wait_record_t wait = {};
    ExpectOk(odin_event_loop_test_prepare_wait(loop, &wait));
    ExpectNoArmedTimer(wait);
    odin_event_loop_destroy(loop);
  });
}

namespace {
struct T5State {
  odin_event_timer_t *timer;
  int timer_calls;
};

void T5ResetTask(odin_event_loop_t *loop, void *user_data) {
  (void)loop;
  T5State *state = static_cast<T5State *>(user_data);
  ExpectOk(odin_event_timer_reset(state->timer, 0, 0));
}

void T5TimerCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
               void *user_data) {
  (void)timer;
  T5State *state = static_cast<T5State *>(user_data);
  state->timer_calls += 1;
  odin_event_loop_stop(loop);
}
} // namespace

TEST(OdinEventLoopTest, T5) {
  EventLoopRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    T5State state = {};
    AssertOk(odin_event_loop_create(&loop));
    AssertOk(odin_event_timer_start(loop, 60000000, 0, T5TimerCb, &state,
                                    &state.timer));
    AssertOk(odin_event_post(loop, T5ResetTask, &state));
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(state.timer_calls, 1);
    odin_event_loop_destroy(loop);
  });
}

namespace {
struct OrderState {
  int values[8];
  int count;
  pthread_t owner;
};

void T6Task1(odin_event_loop_t *loop, void *user_data) {
  (void)loop;
  OrderState *state = static_cast<OrderState *>(user_data);
  EXPECT_TRUE(pthread_equal(pthread_self(), state->owner));
  AppendInt(state->values, &state->count, 1);
}

void T6Task2(odin_event_loop_t *loop, void *user_data) {
  OrderState *state = static_cast<OrderState *>(user_data);
  EXPECT_TRUE(pthread_equal(pthread_self(), state->owner));
  AppendInt(state->values, &state->count, 2);
  odin_event_loop_stop(loop);
}
} // namespace

TEST(OdinEventLoopTest, T6) {
  EventLoopRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    OrderState state = {};
    state.owner = pthread_self();
    AssertOk(odin_event_loop_create(&loop));
    ExpectOk(odin_event_post(loop, T6Task1, &state));
    ExpectOk(odin_event_post(loop, T6Task2, &state));
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    ASSERT_EQ(state.count, 2);
    EXPECT_EQ(state.values[0], 1);
    EXPECT_EQ(state.values[1], 2);
    odin_event_loop_destroy(loop);
  });
}

void NoopIoCb(odin_event_loop_t *loop, odin_event_io_t *io, int fd,
              unsigned int events, void *user_data) {
  (void)loop;
  (void)io;
  (void)fd;
  (void)events;
  (void)user_data;
}

TEST(OdinEventLoopTest, T7) {
  odin_event_loop_t *loop = nullptr;
  int fds[2];
  odin_event_io_t *io = nullptr;
  odin_event_io_t *dup = reinterpret_cast<odin_event_io_t *>(0x1);
  CreateNonblockingSocketpair(fds);
  AssertOk(odin_event_loop_create(&loop));
  EXPECT_EQ(odin_event_io_start(loop, fds[1], ODIN_EVENT_READ, NoopIoCb,
                                nullptr, &io),
            0);
  errno = 0;
  EXPECT_EQ(odin_event_io_start(loop, fds[1], ODIN_EVENT_READ, NoopIoCb,
                                nullptr, &dup),
            -1);
  EXPECT_EQ(errno, EEXIST);
  odin_event_loop_destroy(loop);
  WriteOne(fds[0], 'd');
  ReadOne(fds[1], 'd');
  ClosePair(fds);
}

namespace {
struct T8State {
  odin_event_io_t *io_b;
  int a_calls;
  int b_calls;
};

void T8ACb(odin_event_loop_t *loop, odin_event_io_t *io, int fd,
           unsigned int events, void *user_data) {
  (void)loop;
  (void)io;
  (void)fd;
  (void)events;
  T8State *state = static_cast<T8State *>(user_data);
  state->a_calls += 1;
  odin_event_io_stop(state->io_b);
}

void T8BCb(odin_event_loop_t *loop, odin_event_io_t *io, int fd,
           unsigned int events, void *user_data) {
  (void)loop;
  (void)io;
  (void)fd;
  (void)events;
  T8State *state = static_cast<T8State *>(user_data);
  state->b_calls += 1;
}
} // namespace

TEST(OdinEventLoopTest, T8) {
  odin_event_loop_t *loop = nullptr;
  int a[2];
  int b[2];
  T8State state = {};
  odin_event_io_t *io_a = nullptr;
  CreateNonblockingSocketpair(a);
  CreateNonblockingSocketpair(b);
  AssertOk(odin_event_loop_create(&loop));
  AssertOk(
      odin_event_io_start(loop, a[1], ODIN_EVENT_READ, T8ACb, &state, &io_a));
  AssertOk(odin_event_io_start(loop, b[1], ODIN_EVENT_READ, T8BCb, &state,
                               &state.io_b));
  const odin_event_loop_test_ready_t entries[] = {
      {io_a, ODIN_EVENT_READ},
      {state.io_b, ODIN_EVENT_READ},
  };
  ExpectOk(odin_event_loop_test_dispatch_backend_events(loop, entries, 2));
  EXPECT_EQ(state.a_calls, 1);
  EXPECT_EQ(state.b_calls, 0);
  odin_event_loop_destroy(loop);
  ClosePair(a);
  ClosePair(b);
}

namespace {
struct T9State {
  int timer_calls;
};

void T9TimerCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
               void *user_data) {
  T9State *state = static_cast<T9State *>(user_data);
  state->timer_calls += 1;
  if (state->timer_calls == 3) {
    odin_event_timer_stop(timer);
    odin_event_loop_stop(loop);
  }
}
} // namespace

TEST(OdinEventLoopTest, T9) {
  EventLoopRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    odin_event_timer_t *timer = nullptr;
    T9State state = {};
    AssertOk(odin_event_loop_create(&loop));
    AssertOk(odin_event_timer_start(loop, 0, 1000, T9TimerCb, &state, &timer));
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(state.timer_calls, 3);
    odin_event_loop_destroy(loop);
  });
}

namespace {
struct T10State {
  int timer_calls;
};

void T10TimerCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                void *user_data) {
  T10State *state = static_cast<T10State *>(user_data);
  state->timer_calls += 1;
  if (state->timer_calls == 1) {
    ExpectOk(odin_event_timer_reset(timer, 0, 0));
  } else {
    odin_event_loop_stop(loop);
  }
}
} // namespace

TEST(OdinEventLoopTest, T10) {
  EventLoopRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    odin_event_timer_t *timer = nullptr;
    T10State state = {};
    AssertOk(odin_event_loop_create(&loop));
    AssertOk(
        odin_event_timer_start(loop, 0, 60000000, T10TimerCb, &state, &timer));
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(state.timer_calls, 2);
    odin_event_loop_destroy(loop);
  });
}

namespace {
void T11TaskB(odin_event_loop_t *loop, void *user_data) {
  OrderState *state = static_cast<OrderState *>(user_data);
  AppendInt(state->values, &state->count, 2);
  odin_event_loop_stop(loop);
}

void T11TaskA(odin_event_loop_t *loop, void *user_data) {
  OrderState *state = static_cast<OrderState *>(user_data);
  AppendInt(state->values, &state->count, 1);
  ExpectOk(odin_event_post(loop, T11TaskB, state));
}
} // namespace

TEST(OdinEventLoopTest, T11) {
  EventLoopRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    OrderState state = {};
    AssertOk(odin_event_loop_create(&loop));
    ExpectOk(odin_event_post(loop, T11TaskA, &state));
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    ASSERT_EQ(state.count, 2);
    EXPECT_EQ(state.values[0], 1);
    EXPECT_EQ(state.values[1], 2);
    odin_event_loop_destroy(loop);
  });
}

namespace {
struct T12State {
  odin_event_timer_t *timer_b;
  int a_calls;
  int b_calls;
  int stop_task_calls;
};

void T12StopTask(odin_event_loop_t *loop, void *user_data) {
  T12State *state = static_cast<T12State *>(user_data);
  state->stop_task_calls += 1;
  odin_event_loop_stop(loop);
}

void T12TimerA(odin_event_loop_t *loop, odin_event_timer_t *timer,
               void *user_data) {
  (void)timer;
  T12State *state = static_cast<T12State *>(user_data);
  state->a_calls += 1;
  odin_event_timer_stop(state->timer_b);
  ExpectOk(odin_event_post(loop, T12StopTask, state));
}

void T12TimerB(odin_event_loop_t *loop, odin_event_timer_t *timer,
               void *user_data) {
  (void)loop;
  (void)timer;
  T12State *state = static_cast<T12State *>(user_data);
  state->b_calls += 1;
}
} // namespace

TEST(OdinEventLoopTest, T12) {
  EventLoopRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    odin_event_timer_t *timer_a = nullptr;
    T12State state = {};
    AssertOk(odin_event_loop_create(&loop));
    AssertOk(odin_event_timer_start(loop, 0, 0, T12TimerA, &state, &timer_a));
    AssertOk(
        odin_event_timer_start(loop, 0, 0, T12TimerB, &state, &state.timer_b));
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(state.a_calls, 1);
    EXPECT_EQ(state.b_calls, 0);
    EXPECT_EQ(state.stop_task_calls, 1);
    odin_event_loop_destroy(loop);
  });
}

namespace {
struct T13State {
  int fd;
  unsigned int mask;
  void *user;
  int calls;
};

void T13ReadCb(odin_event_loop_t *loop, odin_event_io_t *io, int fd,
               unsigned int events, void *user_data) {
  T13State *state = static_cast<T13State *>(user_data);
  state->fd = fd;
  state->mask = events;
  state->user = user_data;
  state->calls += 1;
  odin_event_io_stop(io);
  odin_event_loop_stop(loop);
}
} // namespace

TEST(OdinEventLoopTest, T13) {
  EventLoopRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    int fds[2];
    odin_event_io_t *io = nullptr;
    T13State state = {};
    CreateNonblockingSocketpair(fds);
    AssertOk(odin_event_loop_create(&loop));
    AssertOk(odin_event_io_start(loop, fds[1], ODIN_EVENT_READ, T13ReadCb,
                                 &state, &io));
    CloseFd(fds[0]);
    fds[0] = -1;
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(state.calls, 1);
    EXPECT_EQ(state.fd, fds[1]);
    EXPECT_EQ(state.user, &state);
    EXPECT_NE(state.mask & ODIN_EVENT_ERROR, 0u);
    odin_event_loop_destroy(loop);
    CloseFd(fds[1]);
  });
}

void CountTimerCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                  void *user_data) {
  (void)loop;
  (void)timer;
  int *calls = static_cast<int *>(user_data);
  *calls += 1;
}

TEST(OdinEventLoopTest, T14) {
  odin_event_loop_t *loop = nullptr;
  odin_event_timer_t *timer = nullptr;
  int calls = 0;
  AssertOk(odin_event_loop_create(&loop));
  odin_event_loop_test_set_now_us(loop, 1000000);
  AssertOk(odin_event_timer_start(loop, 500, 0, CountTimerCb, &calls, &timer));
  odin_event_loop_test_wait_record_t wait = {};
  ExpectOk(odin_event_loop_test_prepare_wait(loop, &wait));
  if (wait.backend == ODIN_EVENT_LOOP_TEST_BACKEND_LINUX) {
    EXPECT_EQ(wait.linux_timerfd.armed, 1);
    EXPECT_EQ(wait.linux_timerfd.epoll_timeout_ms, -1);
    EXPECT_EQ(wait.linux_timerfd.abs_sec, 1);
    EXPECT_EQ(wait.linux_timerfd.abs_nsec, 500000);
  } else {
    EXPECT_EQ(wait.macos_kevent.timeout_is_null, 0);
    EXPECT_EQ(wait.macos_kevent.rel_sec, 0);
    EXPECT_EQ(wait.macos_kevent.rel_nsec, 500000);
  }
  EXPECT_EQ(calls, 0);
  odin_event_loop_destroy(loop);
}

namespace {
struct T15State {
  int timer_calls;
  bool second_call;
  int stop_task_calls;
};

void T15StopTask(odin_event_loop_t *loop, void *user_data) {
  T15State *state = static_cast<T15State *>(user_data);
  state->stop_task_calls += 1;
  odin_event_loop_stop(loop);
}

void T15TimerCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                void *user_data) {
  T15State *state = static_cast<T15State *>(user_data);
  state->timer_calls += 1;
  if (state->timer_calls == 1) {
    ExpectOk(odin_event_post(loop, T15StopTask, state));
  } else {
    state->second_call = true;
    odin_event_timer_stop(timer);
    odin_event_loop_stop(loop);
  }
}
} // namespace

TEST(OdinEventLoopTest, T15) {
  EventLoopRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    odin_event_timer_t *timer = nullptr;
    T15State state = {};
    AssertOk(odin_event_loop_create(&loop));
    odin_event_loop_test_set_now_us(loop, UINT64_MAX - 10);
    AssertOk(odin_event_timer_start(loop, 0, 20, T15TimerCb, &state, &timer));
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(state.timer_calls, 1);
    EXPECT_FALSE(state.second_call);
    EXPECT_EQ(state.stop_task_calls, 1);
    EXPECT_EQ(odin_event_loop_test_live_timer_count(loop), 0u);
    odin_event_loop_test_wait_record_t wait = {};
    ExpectOk(odin_event_loop_test_prepare_wait(loop, &wait));
    ExpectNoArmedTimer(wait);
    odin_event_loop_destroy(loop);
  });
}

namespace {
struct T16State {
  int values[8];
  int count;
  int post_b_rc;
};

void T16TaskB(odin_event_loop_t *loop, void *user_data) {
  (void)loop;
  T16State *state = static_cast<T16State *>(user_data);
  AppendInt(state->values, &state->count, 2);
}

void T16TaskA(odin_event_loop_t *loop, void *user_data) {
  T16State *state = static_cast<T16State *>(user_data);
  AppendInt(state->values, &state->count, 1);
  state->post_b_rc = odin_event_post(loop, T16TaskB, state);
  odin_event_loop_stop(loop);
}

void T16TaskC(odin_event_loop_t *loop, void *user_data) {
  T16State *state = static_cast<T16State *>(user_data);
  AppendInt(state->values, &state->count, 3);
  odin_event_loop_stop(loop);
}
} // namespace

TEST(OdinEventLoopTest, T16) {
  EventLoopRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    T16State state = {};
    AssertOk(odin_event_loop_create(&loop));
    ExpectOk(odin_event_post(loop, T16TaskA, &state));
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(state.post_b_rc, 0);
    ASSERT_EQ(state.count, 1);
    EXPECT_EQ(state.values[0], 1);
    ExpectOk(odin_event_post(loop, T16TaskC, &state));
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    ASSERT_EQ(state.count, 2);
    EXPECT_EQ(state.values[0], 1);
    EXPECT_EQ(state.values[1], 3);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinEventLoopTest, T17) {
  odin_event_loop_t *loop = nullptr;
  int calls = 0;
  AssertOk(odin_event_loop_create(&loop));
  ExpectOk(odin_event_post(loop, StopTask, &calls));
  odin_event_loop_destroy(loop);
  EXPECT_EQ(calls, 0);
}

namespace {
void T18TaskA(odin_event_loop_t *loop, void *user_data) {
  OrderState *state = static_cast<OrderState *>(user_data);
  AppendInt(state->values, &state->count, 1);
  odin_event_loop_stop(loop);
}

void T18TaskB(odin_event_loop_t *loop, void *user_data) {
  (void)loop;
  OrderState *state = static_cast<OrderState *>(user_data);
  AppendInt(state->values, &state->count, 2);
}
} // namespace

TEST(OdinEventLoopTest, T18) {
  EventLoopRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    OrderState state = {};
    AssertOk(odin_event_loop_create(&loop));
    ExpectOk(odin_event_post(loop, T18TaskA, &state));
    ExpectOk(odin_event_post(loop, T18TaskB, &state));
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    ASSERT_EQ(state.count, 2);
    EXPECT_EQ(state.values[0], 1);
    EXPECT_EQ(state.values[1], 2);
    odin_event_loop_destroy(loop);
  });
}

namespace {
struct T19State {
  int values[4];
  int count;
};

void T19ACb(odin_event_loop_t *loop, odin_event_io_t *io, int fd,
            unsigned int events, void *user_data) {
  (void)io;
  (void)fd;
  (void)events;
  T19State *state = static_cast<T19State *>(user_data);
  AppendInt(state->values, &state->count, 'A');
  odin_event_loop_stop(loop);
}

void T19BCb(odin_event_loop_t *loop, odin_event_io_t *io, int fd,
            unsigned int events, void *user_data) {
  (void)loop;
  (void)io;
  (void)fd;
  (void)events;
  T19State *state = static_cast<T19State *>(user_data);
  AppendInt(state->values, &state->count, 'B');
}
} // namespace

TEST(OdinEventLoopTest, T19) {
  EventLoopRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    int a[2];
    int b[2];
    T19State state = {};
    odin_event_io_t *io_a = nullptr;
    odin_event_io_t *io_b = nullptr;
    CreateNonblockingSocketpair(a);
    CreateNonblockingSocketpair(b);
    AssertOk(odin_event_loop_create(&loop));
    AssertOk(odin_event_io_start(loop, a[1], ODIN_EVENT_READ, T19ACb, &state,
                                 &io_a));
    AssertOk(odin_event_io_start(loop, b[1], ODIN_EVENT_READ, T19BCb, &state,
                                 &io_b));
    const odin_event_loop_test_ready_t entries[] = {
        {io_a, ODIN_EVENT_READ},
        {io_b, ODIN_EVENT_READ},
    };
    AssertOk(odin_event_loop_test_queue_backend_events(loop, entries, 2));
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    ASSERT_EQ(state.count, 2);
    EXPECT_EQ(state.values[0], 'A');
    EXPECT_EQ(state.values[1], 'B');
    odin_event_loop_destroy(loop);
    ClosePair(a);
    ClosePair(b);
  });
}

namespace {
struct T20State {
  int values[4];
  int count;
};

void T20TimerA(odin_event_loop_t *loop, odin_event_timer_t *timer,
               void *user_data) {
  (void)timer;
  T20State *state = static_cast<T20State *>(user_data);
  AppendInt(state->values, &state->count, 'A');
  odin_event_loop_stop(loop);
}

void T20TimerB(odin_event_loop_t *loop, odin_event_timer_t *timer,
               void *user_data) {
  (void)loop;
  (void)timer;
  T20State *state = static_cast<T20State *>(user_data);
  AppendInt(state->values, &state->count, 'B');
}
} // namespace

TEST(OdinEventLoopTest, T20) {
  EventLoopRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    odin_event_timer_t *timer_a = nullptr;
    odin_event_timer_t *timer_b = nullptr;
    T20State state = {};
    AssertOk(odin_event_loop_create(&loop));
    AssertOk(odin_event_timer_start(loop, 0, 0, T20TimerA, &state, &timer_a));
    AssertOk(odin_event_timer_start(loop, 0, 0, T20TimerB, &state, &timer_b));
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    ASSERT_EQ(state.count, 2);
    EXPECT_EQ(state.values[0], 'A');
    EXPECT_EQ(state.values[1], 'B');
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinEventLoopTest, T21) {
  odin_event_loop_t *loop = nullptr;
  int fds[2];
  odin_event_io_t *io = nullptr;
  CreateNonblockingSocketpair(fds);
  AssertOk(odin_event_loop_create(&loop));
  const unsigned int invalid_starts[] = {
      0,
      ODIN_EVENT_ERROR,
      ODIN_EVENT_READ | ODIN_EVENT_ERROR,
      ODIN_EVENT_WRITE | 0x80u,
  };
  for (unsigned int events : invalid_starts) {
    odin_event_io_t *out = reinterpret_cast<odin_event_io_t *>(0x1);
    errno = 0;
    EXPECT_EQ(
        odin_event_io_start(loop, fds[1], events, NoopIoCb, nullptr, &out), -1);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(out, reinterpret_cast<odin_event_io_t *>(0x1));
  }
  AssertOk(odin_event_io_start(loop, fds[1], ODIN_EVENT_READ, NoopIoCb, nullptr,
                               &io));
  const unsigned int invalid_updates[] = {
      0,
      ODIN_EVENT_ERROR,
      ODIN_EVENT_READ | 0x80u,
  };
  for (unsigned int events : invalid_updates) {
    errno = 0;
    EXPECT_EQ(odin_event_io_update(io, events), -1);
    EXPECT_EQ(errno, EINVAL);
  }
  odin_event_io_stop(io);
  odin_event_loop_destroy(loop);
  ClosePair(fds);
}

TEST(OdinEventLoopTest, T22) {
  odin_event_loop_t *loop = nullptr;
  odin_event_loop_test_fd_record_t fds = {};
  AssertOk(odin_event_loop_create(&loop));
  ExpectOk(odin_event_loop_test_backend_fds(loop, &fds));
  ASSERT_NE(fds.backend_fd, -1);
  EXPECT_NE(fcntl(fds.backend_fd, F_GETFD), -1);
  if (fds.timer_fd >= 0) {
    EXPECT_NE(fcntl(fds.timer_fd, F_GETFD), -1);
  }
  odin_event_loop_destroy(loop);
  errno = 0;
  EXPECT_EQ(fcntl(fds.backend_fd, F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);
  if (fds.timer_fd >= 0) {
    errno = 0;
    EXPECT_EQ(fcntl(fds.timer_fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
  }
}

TEST(OdinEventLoopTest, T23) {
  odin_event_loop_t *loop = nullptr;
  AssertOk(odin_event_loop_create(&loop));
#if defined(__linux__)
  errno = 0;
  EXPECT_EQ(odin_event_loop_test_fail_next_kqueue_change(
                loop, ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_ADD, ODIN_EVENT_WRITE,
                ENOSPC),
            -1);
  EXPECT_EQ(errno, EOPNOTSUPP);
  odin_event_loop_destroy(loop);
#else
  int fds[2];
  CreateNonblockingSocketpair(fds);
  odin_event_io_t *io = reinterpret_cast<odin_event_io_t *>(0x1);
  unsigned int mask = 0;
  AssertOk(odin_event_loop_test_fail_next_kqueue_change(
      loop, ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_ADD, ODIN_EVENT_WRITE, ENOSPC));
  errno = 0;
  EXPECT_EQ(odin_event_io_start(loop, fds[1],
                                ODIN_EVENT_READ | ODIN_EVENT_WRITE, NoopIoCb,
                                nullptr, &io),
            -1);
  EXPECT_EQ(errno, ENOSPC);
  EXPECT_EQ(io, reinterpret_cast<odin_event_io_t *>(0x1));
  ExpectOk(odin_event_loop_test_kqueue_registered_mask(loop, fds[1], &mask));
  EXPECT_EQ(mask, 0u);

  AssertOk(odin_event_io_start(loop, fds[1], ODIN_EVENT_READ, NoopIoCb, nullptr,
                               &io));
  ExpectOk(odin_event_loop_test_kqueue_registered_mask(loop, fds[1], &mask));
  EXPECT_EQ(mask, ODIN_EVENT_READ);
  AssertOk(odin_event_loop_test_fail_next_kqueue_change(
      loop, ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_DELETE, ODIN_EVENT_READ,
      ENOSPC));
  errno = 0;
  EXPECT_EQ(odin_event_io_update(io, ODIN_EVENT_WRITE), -1);
  EXPECT_EQ(errno, ENOSPC);
  ExpectOk(odin_event_loop_test_kqueue_registered_mask(loop, fds[1], &mask));
  EXPECT_EQ(mask, ODIN_EVENT_READ);
  odin_event_io_stop(io);
  ExpectOk(odin_event_loop_test_kqueue_registered_mask(loop, fds[1], &mask));
  EXPECT_EQ(mask, 0u);
  odin_event_loop_destroy(loop);
  ClosePair(fds);
#endif
}

TEST(OdinEventLoopTest, T24) {
  odin_event_loop_test_reset_liveness();
  odin_event_loop_test_liveness_t zero_before = {};
  odin_event_loop_test_liveness_t zero_after_null = {};
  odin_event_loop_test_liveness_t before = {};
  odin_event_loop_test_liveness_t after = {};
  ExpectOk(odin_event_loop_test_liveness(&zero_before));
  odin_event_loop_destroy(nullptr);
  ExpectOk(odin_event_loop_test_liveness(&zero_after_null));
  EXPECT_EQ(zero_before.loops, 0u);
  EXPECT_EQ(zero_before.io_handles, 0u);
  EXPECT_EQ(zero_before.timers, 0u);
  EXPECT_EQ(zero_before.task_nodes, 0u);
  EXPECT_EQ(zero_after_null.loops, 0u);
  EXPECT_EQ(zero_after_null.io_handles, 0u);
  EXPECT_EQ(zero_after_null.timers, 0u);
  EXPECT_EQ(zero_after_null.task_nodes, 0u);

  odin_event_loop_t *loop = nullptr;
  int fds[2];
  odin_event_io_t *io = nullptr;
  odin_event_timer_t *timer = nullptr;
  int calls = 0;
  CreateNonblockingSocketpair(fds);
  AssertOk(odin_event_loop_create(&loop));
  AssertOk(odin_event_io_start(loop, fds[1], ODIN_EVENT_READ, NoopIoCb, nullptr,
                               &io));
  AssertOk(
      odin_event_timer_start(loop, 60000000, 0, CountTimerCb, &calls, &timer));
  ExpectOk(odin_event_post(loop, StopTask, &calls));
  ExpectOk(odin_event_loop_test_liveness(&before));
  EXPECT_EQ(before.loops, 1u);
  EXPECT_EQ(before.io_handles, 1u);
  EXPECT_EQ(before.timers, 1u);
  EXPECT_EQ(before.task_nodes, 1u);
  odin_event_loop_destroy(loop);
  ExpectOk(odin_event_loop_test_liveness(&after));
  EXPECT_EQ(after.loops, 0u);
  EXPECT_EQ(after.io_handles, 0u);
  EXPECT_EQ(after.timers, 0u);
  EXPECT_EQ(after.task_nodes, 0u);
  EXPECT_EQ(calls, 0);
  ClosePair(fds);
}

TEST(OdinEventLoopTest, T25) {
  odin_event_loop_test_reset_liveness();
  odin_event_loop_t *out = reinterpret_cast<odin_event_loop_t *>(0x1234);
  odin_event_loop_t *sentinel = out;
  odin_event_loop_test_liveness_t after = {};
  ExpectOk(odin_event_loop_test_fail_next_backend_create(EMFILE));
  errno = 0;
  EXPECT_EQ(odin_event_loop_create(&out), -1);
  EXPECT_EQ(errno, EMFILE);
  EXPECT_EQ(out, sentinel);
  ExpectOk(odin_event_loop_test_liveness(&after));
  EXPECT_EQ(after.loops, 0u);
  EXPECT_EQ(after.io_handles, 0u);
  EXPECT_EQ(after.timers, 0u);
  EXPECT_EQ(after.task_nodes, 0u);
}

TEST(OdinEventLoopTest, T26) {
  EventLoopRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    int calls = 0;
    AssertOk(odin_event_loop_create(&loop));
    ExpectOk(odin_event_loop_test_fail_next_backend_wait(loop, EIO));
    errno = 0;
    ASSERT_EQ(odin_event_loop_run(loop), -1);
    EXPECT_EQ(errno, EIO);
    EXPECT_EQ(calls, 0);
    ExpectOk(odin_event_post(loop, StopTask, &calls));
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(calls, 1);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinEventLoopTest, T27) {
  odin_event_loop_test_reset_liveness();
  odin_event_loop_t *loop = nullptr;
  odin_event_timer_t *timer = nullptr;
  int timer_calls = 0;
  AssertOk(odin_event_loop_create(&loop));
  odin_event_loop_test_set_now_us(loop, 1000000);
  AssertOk(odin_event_timer_start(loop, 60000000, 0, CountTimerCb, &timer_calls,
                                  &timer));
  odin_event_timer_stop(timer);
  odin_event_loop_test_set_now_us(loop, 61000000);
  odin_event_loop_test_wait_record_t wait = {};
  ExpectOk(odin_event_loop_test_prepare_wait(loop, &wait));
  EXPECT_EQ(timer_calls, 0);
  ExpectNoArmedTimer(wait);
  EXPECT_EQ(odin_event_loop_test_live_timer_count(loop), 0u);
  odin_event_loop_test_liveness_t after_prepare = {};
  ExpectOk(odin_event_loop_test_liveness(&after_prepare));
  EXPECT_EQ(after_prepare.loops, 1u);
  EXPECT_EQ(after_prepare.timers, 0u);
  EXPECT_EQ(after_prepare.io_handles, 0u);
  EXPECT_EQ(after_prepare.task_nodes, 0u);
  odin_event_loop_destroy(loop);
  odin_event_loop_test_liveness_t after_destroy = {};
  ExpectOk(odin_event_loop_test_liveness(&after_destroy));
  EXPECT_EQ(after_destroy.loops, 0u);
  EXPECT_EQ(after_destroy.timers, 0u);
  EXPECT_EQ(after_destroy.io_handles, 0u);
  EXPECT_EQ(after_destroy.task_nodes, 0u);
}

// NOLINTEND(misc-const-correctness, misc-use-internal-linkage)
