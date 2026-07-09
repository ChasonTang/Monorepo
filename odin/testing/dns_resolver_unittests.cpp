// odin/testing/dns_resolver_unittests.cpp
//
// Unit tests T1-T20 from §5 of odin/docs/rfc_030_async_dns_resolver.md.

#include "odin/dns_resolver.h"

#include <ares.h>

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "odin/event_loop.h"
#include "odin/testing/dns_resolver_internal_test.h"
#include "odin/testing/event_loop_internal_test.h"

#include "gtest/gtest.h"

extern std::string g_test_argv0;

// NOLINTBEGIN(misc-const-correctness, misc-use-internal-linkage)

namespace {

std::string ActiveFilter() { return GTEST_FLAG_GET(filter); }

bool IsDirectChildOnlyFilter(const std::string &filter, const char *test_name) {
  const std::string exact =
      std::string("OdinDnsResolverExecChild.") + test_name;
  return filter == exact || filter == "OdinDnsResolverExecChild.*";
}

void AssertParentCaresInitUnchanged() {
  ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0) << std::strerror(errno);
  odin_dns_resolver_test_cares_observation_t obs;
  ASSERT_EQ(odin_dns_resolver_test_cares_observation(&obs), 0)
      << std::strerror(errno);
  ASSERT_EQ(obs.ares_library_init_calls, static_cast<size_t>(0));
}

#define ODIN_DNS_REQUIRE_CHILD_MODE(test_name, expected_mode)                  \
  do {                                                                         \
    const std::string filter = ActiveFilter();                                 \
    const std::string exact_filter =                                           \
        std::string("OdinDnsResolverExecChild.") + (test_name);                \
    const char *mode = std::getenv("ODIN_DNS_EXEC_CHILD");                     \
    if (mode == nullptr || mode[0] == '\0') {                                  \
      if (IsDirectChildOnlyFilter(filter, (test_name))) {                      \
        FAIL() << "missing ODIN_DNS_EXEC_CHILD";                               \
      }                                                                        \
      GTEST_SKIP() << "ordinary full-suite child-only skip";                   \
    }                                                                          \
    ASSERT_EQ(filter, exact_filter);                                           \
    ASSERT_STREQ(mode, (expected_mode));                                       \
  } while (0)

class DnsRunDeadline {
public:
  template <typename Fn> static void Run(Fn fn) {
    const pid_t pid = fork();
    ASSERT_NE(pid, -1) << std::strerror(errno);
    if (pid == 0) {
      (void)odin_dns_resolver_test_reset_liveness();
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
      FAIL() << "DnsRunDeadline exceeded 2 seconds";
    }
    ASSERT_TRUE(WIFEXITED(wstatus));
    EXPECT_EQ(WEXITSTATUS(wstatus), 0);
  }
};

class ExecArgv {
public:
  ExecArgv(const char *binary, const char *filter) {
    storage_.push_back(binary);
    storage_.push_back(std::string("--gtest_filter=") + filter);
    rebuild();
  }

  char *const *argv() {
    if (ptrs_.empty() || ptrs_.back() != nullptr) {
      ptrs_.push_back(nullptr);
    }
    return ptrs_.data();
  }

private:
  void rebuild() {
    ptrs_.clear();
    for (std::string &s : storage_) {
      ptrs_.push_back(&s[0]);
    }
  }

  std::vector<std::string> storage_;
  std::vector<char *> ptrs_;
};

void RunExecChild(const char *filter, const char *mode) {
  ASSERT_FALSE(g_test_argv0.empty());
  const pid_t pid = fork();
  ASSERT_NE(pid, -1) << std::strerror(errno);
  if (pid == 0) {
    setenv("ODIN_DNS_EXEC_CHILD", mode, 1);
    ExecArgv argv(g_test_argv0.c_str(), filter);
    execv(g_test_argv0.c_str(), argv.argv());
    _exit(127);
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
    FAIL() << "DNS exec child exceeded 2 seconds";
  }
  ASSERT_TRUE(WIFEXITED(wstatus));
  EXPECT_EQ(WEXITSTATUS(wstatus), 0);
}

struct CapturedCallback {
  odin_dns_query_t *query = nullptr;
  odin_dns_status_t status = ODIN_DNS_OK;
  int err = 0;
  bool addrs_was_null = true;
  size_t addr_count = 0;
  std::vector<odin_dns_addr_t> addrs;
  void *user_data = nullptr;
  bool start_returned = false;
};

struct CallbackState {
  int calls = 0;
  int stop_after = 1;
  bool destroy_query = false;
  bool start_returned = false;
  odin_event_loop_t *loop = nullptr;
  std::vector<CapturedCallback> records;
};

void OnDns(odin_dns_query_t *query, odin_dns_status_t status, int err,
           const odin_dns_addr_t *addrs, size_t addr_count, void *user_data) {
  CallbackState *state = static_cast<CallbackState *>(user_data);
  CapturedCallback record;
  record.query = query;
  record.status = status;
  record.err = err;
  record.addrs_was_null = addrs == nullptr;
  record.addr_count = addr_count;
  record.user_data = user_data;
  record.start_returned = state->start_returned;
  if (addrs != nullptr) {
    record.addrs.assign(addrs, addrs + addr_count);
  }
  state->records.push_back(record);
  state->calls += 1;
  if (state->loop != nullptr && state->calls >= state->stop_after) {
    odin_event_loop_stop(state->loop);
  }
  if (state->destroy_query) {
    odin_dns_query_destroy(query);
  }
}

void WatchdogCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                void *user_data) {
  bool *timed_out = static_cast<bool *>(user_data);
  *timed_out = true;
  odin_event_timer_stop(timer);
  odin_event_loop_stop(loop);
}

void RunLoopUntil(odin_event_loop_t *loop, CallbackState *state,
                  int expected_calls) {
  bool timed_out = false;
  odin_event_timer_t *watchdog = nullptr;
  ASSERT_EQ(odin_event_timer_start(loop, 1500000, 0, WatchdogCb, &timed_out,
                                   &watchdog),
            0)
      << std::strerror(errno);
  state->loop = loop;
  state->stop_after = expected_calls;
  ASSERT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);
  if (!timed_out) {
    odin_event_timer_stop(watchdog);
  }
  ASSERT_FALSE(timed_out);
  EXPECT_EQ(state->calls, expected_calls);
}

void RunShortLoopExpectNoCallback(odin_event_loop_t *loop,
                                  CallbackState *state) {
  bool timed_out = false;
  odin_event_timer_t *watchdog = nullptr;
  ASSERT_EQ(
      odin_event_timer_start(loop, 20000, 0, WatchdogCb, &timed_out, &watchdog),
      0)
      << std::strerror(errno);
  state->loop = loop;
  ASSERT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);
  EXPECT_TRUE(timed_out);
  EXPECT_EQ(state->calls, 0);
}

void RunLoopForNoCallback(odin_event_loop_t *loop, CallbackState *state,
                          uint64_t duration_us) {
  bool timed_out = false;
  odin_event_timer_t *watchdog = nullptr;
  ASSERT_EQ(odin_event_timer_start(loop, duration_us, 0, WatchdogCb, &timed_out,
                                   &watchdog),
            0)
      << std::strerror(errno);
  state->loop = loop;
  ASSERT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);
  if (!timed_out) {
    odin_event_timer_stop(watchdog);
  }
  EXPECT_TRUE(timed_out);
  EXPECT_EQ(state->calls, 0);
}

void ExpectZeroLiveness() {
  odin_dns_resolver_test_liveness_t live;
  ASSERT_EQ(odin_dns_resolver_test_liveness(&live), 0) << std::strerror(errno);
  EXPECT_EQ(live.resolvers, static_cast<size_t>(0));
  EXPECT_EQ(live.queries, static_cast<size_t>(0));
  EXPECT_EQ(live.watches, static_cast<size_t>(0));
  EXPECT_EQ(live.timers, static_cast<size_t>(0));
  EXPECT_EQ(live.cares_channels, static_cast<size_t>(0));
  EXPECT_EQ(live.cares_results, static_cast<size_t>(0));
}

void ExpectNoLiveQueryResources() {
  odin_dns_resolver_test_liveness_t live;
  ASSERT_EQ(odin_dns_resolver_test_liveness(&live), 0) << std::strerror(errno);
  EXPECT_EQ(live.queries, static_cast<size_t>(0));
  EXPECT_EQ(live.watches, static_cast<size_t>(0));
  EXPECT_EQ(live.timers, static_cast<size_t>(0));
  EXPECT_EQ(live.cares_channels, static_cast<size_t>(0));
  EXPECT_EQ(live.cares_results, static_cast<size_t>(0));
}

void ExpectRejectedStart(odin_event_loop_t *loop, int rc, int expected_errno,
                         CallbackState *state) {
  EXPECT_EQ(rc, -1);
  EXPECT_EQ(errno, expected_errno);
  ExpectNoLiveQueryResources();
  RunShortLoopExpectNoCallback(loop, state);
  ExpectNoLiveQueryResources();
}

void BasicLoopResolver(odin_event_loop_t **out_loop,
                       odin_dns_resolver_t **out_resolver,
                       const odin_dns_resolver_config_t *config = nullptr) {
  ASSERT_EQ(odin_event_loop_create(out_loop), 0) << std::strerror(errno);
  ASSERT_EQ(odin_dns_resolver_create(*out_loop, config, out_resolver), 0)
      << std::strerror(errno);
}

void PreinitCaresBeforeThreads() {
  odin_event_loop_t *loop = nullptr;
  odin_dns_resolver_t *resolver = nullptr;
  BasicLoopResolver(&loop, &resolver);
  odin_dns_resolver_test_cares_observation_t obs;
  ASSERT_EQ(odin_dns_resolver_test_cares_observation(&obs), 0)
      << std::strerror(errno);
  ASSERT_EQ(obs.ares_library_init_calls, static_cast<size_t>(1));
  odin_dns_resolver_destroy(resolver);
  odin_event_loop_destroy(loop);
  ExpectZeroLiveness();
}

bool HasFamily(const CapturedCallback &record, int family) {
  for (const odin_dns_addr_t &addr : record.addrs) {
    if (addr.addr.ss_family == family) {
      return true;
    }
  }
  return false;
}

void ExpectAddressCount(const CapturedCallback &record, size_t expected) {
  EXPECT_FALSE(record.addrs_was_null);
  EXPECT_EQ(record.addr_count, expected);
  EXPECT_EQ(record.addrs.size(), expected);
}

void ExpectErrorNoAddresses(const CapturedCallback &record, int expected_err) {
  EXPECT_EQ(record.status, ODIN_DNS_ERROR);
  EXPECT_EQ(record.err, expected_err);
  EXPECT_TRUE(record.addrs_was_null);
  EXPECT_EQ(record.addr_count, static_cast<size_t>(0));
  EXPECT_TRUE(record.addrs.empty());
}

void ExpectIpv4Result(const CapturedCallback &record, const char *addr,
                      uint16_t port, int ttl) {
  in_addr expected;
  ASSERT_EQ(inet_pton(AF_INET, addr, &expected), 1);
  bool found = false;
  for (const odin_dns_addr_t &dns_addr : record.addrs) {
    if (dns_addr.addr.ss_family != AF_INET) {
      continue;
    }
    const sockaddr_in *sin =
        reinterpret_cast<const sockaddr_in *>(&dns_addr.addr);
    found = true;
    EXPECT_EQ(dns_addr.addrlen, static_cast<socklen_t>(sizeof(sockaddr_in)));
    EXPECT_EQ(sin->sin_port, htons(port));
    EXPECT_EQ(std::memcmp(&sin->sin_addr, &expected, sizeof(expected)), 0);
    if (ttl >= 0) {
      EXPECT_EQ(dns_addr.ttl, ttl);
    }
  }
  EXPECT_TRUE(found);
}

void ExpectIpv6Result(const CapturedCallback &record, const char *addr,
                      uint16_t port, int ttl) {
  in6_addr expected;
  ASSERT_EQ(inet_pton(AF_INET6, addr, &expected), 1);
  bool found = false;
  for (const odin_dns_addr_t &dns_addr : record.addrs) {
    if (dns_addr.addr.ss_family != AF_INET6) {
      continue;
    }
    const sockaddr_in6 *sin6 =
        reinterpret_cast<const sockaddr_in6 *>(&dns_addr.addr);
    found = true;
    EXPECT_EQ(dns_addr.addrlen, static_cast<socklen_t>(sizeof(sockaddr_in6)));
    EXPECT_EQ(sin6->sin6_port, htons(port));
    EXPECT_EQ(std::memcmp(&sin6->sin6_addr, &expected, sizeof(expected)), 0);
    if (ttl >= 0) {
      EXPECT_EQ(dns_addr.ttl, ttl);
    }
  }
  EXPECT_TRUE(found);
}

void ExpectNextTimerDelay(odin_event_loop_t *loop, uint64_t now_us,
                          uint64_t delay_us) {
  odin_event_loop_test_wait_record_t wait = {};
  ASSERT_EQ(odin_event_loop_test_prepare_wait(loop, &wait), 0)
      << std::strerror(errno);
  if (wait.backend == ODIN_EVENT_LOOP_TEST_BACKEND_LINUX) {
    const uint64_t due_us = now_us + delay_us;
    EXPECT_EQ(wait.linux_timerfd.armed, 1);
    EXPECT_EQ(wait.linux_timerfd.abs_sec,
              static_cast<int64_t>(due_us / 1000000u));
    EXPECT_EQ(wait.linux_timerfd.abs_nsec,
              static_cast<long>((due_us % 1000000u) * 1000u));
  } else {
    EXPECT_EQ(wait.macos_kevent.timeout_is_null, 0);
    EXPECT_EQ(wait.macos_kevent.rel_sec,
              static_cast<int64_t>(delay_us / 1000000u));
    EXPECT_EQ(wait.macos_kevent.rel_nsec,
              static_cast<long>((delay_us % 1000000u) * 1000u));
  }
}

void ExpectDnsOnlyInitOptions(
    const odin_dns_resolver_test_cares_observation_t &obs) {
  EXPECT_TRUE((obs.last_init_options_optmask & ARES_OPT_FLAGS) != 0);
  EXPECT_TRUE((obs.last_init_options_optmask & ARES_OPT_LOOKUPS) != 0);
  EXPECT_EQ((obs.last_init_options_optmask & ARES_OPT_EVENT_THREAD), 0);
  EXPECT_TRUE((obs.last_init_options_flags & ARES_FLAG_NOALIASES) != 0);
  EXPECT_TRUE((obs.last_init_options_flags & ARES_FLAG_NOSEARCH) != 0);
  EXPECT_STREQ(obs.last_init_options_lookups, "b");
}

void PushStep(int op, int status = ARES_SUCCESS) {
  odin_dns_resolver_test_cares_step_t step = {};
  step.op = op;
  step.status = status;
  ASSERT_EQ(odin_dns_resolver_test_push_cares_step(&step), 0)
      << std::strerror(errno);
}

void PushTimeoutStep(int64_t sec, int64_t usec) {
  odin_dns_resolver_test_cares_step_t step = {};
  step.op = ODIN_DNS_TEST_CARES_TIMEOUT_TIMEVAL;
  step.timeout_tv_sec = sec;
  step.timeout_tv_usec = usec;
  ASSERT_EQ(odin_dns_resolver_test_push_cares_step(&step), 0)
      << std::strerror(errno);
}

void PushSockState(int readable, int writable) {
  odin_dns_resolver_test_cares_step_t step = {};
  step.op = ODIN_DNS_TEST_CARES_SOCK_STATE;
  step.readable = readable;
  step.writable = writable;
  ASSERT_EQ(odin_dns_resolver_test_push_cares_step(&step), 0)
      << std::strerror(errno);
}

void PushResultStatus(int status, unsigned int expected_events = 0) {
  odin_dns_resolver_test_cares_step_t step = {};
  step.op = ODIN_DNS_TEST_CARES_RESULT_STATUS;
  step.status = status;
  step.expect_process_events = expected_events;
  ASSERT_EQ(odin_dns_resolver_test_push_cares_step(&step), 0)
      << std::strerror(errno);
}

void PushProcessStatus(int status, unsigned int expected_events = 0,
                       bool expect_null = false) {
  odin_dns_resolver_test_cares_step_t step = {};
  step.op = ODIN_DNS_TEST_CARES_PROCESS_FDS_STATUS;
  step.status = status;
  step.expect_process_events = expected_events;
  step.expect_process_null_events = expect_null ? 1 : 0;
  ASSERT_EQ(odin_dns_resolver_test_push_cares_step(&step), 0)
      << std::strerror(errno);
}

class DnsFixture {
public:
  DnsFixture() {
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    EXPECT_NE(fd_, -1) << std::strerror(errno);
    timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    EXPECT_EQ(setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)), 0)
        << std::strerror(errno);
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    EXPECT_EQ(bind(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), 0)
        << std::strerror(errno);
    socklen_t len = sizeof(addr);
    EXPECT_EQ(getsockname(fd_, reinterpret_cast<sockaddr *>(&addr), &len), 0)
        << std::strerror(errno);
    port_ = ntohs(addr.sin_port);
    EXPECT_EQ(pthread_mutex_init(&mu_, nullptr), 0);
    EXPECT_EQ(pthread_cond_init(&cv_, nullptr), 0);
    running_ = true;
    EXPECT_EQ(pthread_create(&thread_, nullptr, ThreadMain, this), 0);
  }

  ~DnsFixture() {
    pthread_mutex_lock(&mu_);
    running_ = false;
    released_ = true;
    pthread_cond_broadcast(&cv_);
    pthread_mutex_unlock(&mu_);
    if (fd_ >= 0) {
      close(fd_);
    }
    if (thread_ != 0) {
      pthread_join(thread_, nullptr);
    }
    pthread_cond_destroy(&cv_);
    pthread_mutex_destroy(&mu_);
  }

  std::string servers_csv() const {
    return std::string("127.0.0.1:") + std::to_string(port_);
  }

  bool SawQuestion(const std::string &name) {
    pthread_mutex_lock(&mu_);
    bool found = false;
    for (const std::string &question : questions_) {
      if (question == name) {
        found = true;
        break;
      }
    }
    pthread_mutex_unlock(&mu_);
    return found;
  }

  std::vector<std::string> Questions() {
    pthread_mutex_lock(&mu_);
    std::vector<std::string> out = questions_;
    pthread_mutex_unlock(&mu_);
    return out;
  }

  void Release() {
    pthread_mutex_lock(&mu_);
    released_ = true;
    pthread_cond_broadcast(&cv_);
    pthread_mutex_unlock(&mu_);
  }

private:
  static void *ThreadMain(void *arg) {
    static_cast<DnsFixture *>(arg)->Run();
    return nullptr;
  }

  static uint16_t ReadU16(const unsigned char *p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
  }

  static void PutU16(std::vector<unsigned char> *out, uint16_t value) {
    out->push_back(static_cast<unsigned char>(value >> 8));
    out->push_back(static_cast<unsigned char>(value & 0xff));
  }

  static void PutU32(std::vector<unsigned char> *out, uint32_t value) {
    out->push_back(static_cast<unsigned char>(value >> 24));
    out->push_back(static_cast<unsigned char>((value >> 16) & 0xff));
    out->push_back(static_cast<unsigned char>((value >> 8) & 0xff));
    out->push_back(static_cast<unsigned char>(value & 0xff));
  }

  bool ParseQuestion(const unsigned char *buf, size_t len, std::string *name,
                     uint16_t *qtype, size_t *question_end) {
    if (len < 12) {
      return false;
    }
    size_t off = 12;
    std::string parsed;
    while (off < len && buf[off] != 0) {
      const unsigned int label_len = buf[off++];
      if (label_len > 63 || off + label_len > len) {
        return false;
      }
      if (!parsed.empty()) {
        parsed.push_back('.');
      }
      parsed.append(reinterpret_cast<const char *>(&buf[off]), label_len);
      off += label_len;
    }
    if (off + 5 > len) {
      return false;
    }
    off += 1;
    *qtype = ReadU16(&buf[off]);
    off += 4;
    *name = parsed;
    *question_end = off;
    return true;
  }

  void NoteQuestion(const std::string &name) {
    pthread_mutex_lock(&mu_);
    questions_.push_back(name);
    pthread_mutex_unlock(&mu_);
  }

  bool ShouldDrop(const std::string &name) {
    return name == "aliaspeer" || name == "abort.test" || name == "hold.test" ||
           name == "noresponse.test" || name == "timeout.test" ||
           name == "timer.test" || name == "remove.test" ||
           name == "write.test" || name == "fatal.test" ||
           name == "overflow.test" || name == "reset.test" ||
           name == "nullstop.test";
  }

  bool IsNxDomain(const std::string &name) {
    return name == "nx.test" || name == "missing.test" || name == "no.test";
  }

  bool IsNoData(const std::string &name) { return name == "nodata.test"; }

  bool WaitForReleaseIfNeeded(const std::string &name) {
    if (name != "sibling.test") {
      return true;
    }
    pthread_mutex_lock(&mu_);
    while (running_ && !released_) {
      pthread_cond_wait(&cv_, &mu_);
    }
    const bool should_reply = running_;
    pthread_mutex_unlock(&mu_);
    return should_reply;
  }

  void AppendAnswer(std::vector<unsigned char> *out, const std::string &name,
                    uint16_t qtype, uint16_t port_tag) {
    (void)port_tag;
    out->push_back(0xc0);
    out->push_back(0x0c);
    PutU16(out, qtype);
    PutU16(out, 1);
    PutU32(out, 60);
    if (qtype == 1) {
      PutU16(out, 4);
      if (name == "probe.test") {
        out->push_back(127);
        out->push_back(0);
        out->push_back(0);
        out->push_back(1);
      } else if (name == "dual.test") {
        out->push_back(203);
        out->push_back(0);
        out->push_back(113);
        out->push_back(7);
      } else if (name == "callback-destroy.test") {
        out->push_back(203);
        out->push_back(0);
        out->push_back(113);
        out->push_back(9);
      } else if (name == "ok.test") {
        out->push_back(203);
        out->push_back(0);
        out->push_back(113);
        out->push_back(10);
      } else if (name == "sibling.test") {
        out->push_back(203);
        out->push_back(0);
        out->push_back(113);
        out->push_back(11);
      } else if (name == "completed.test") {
        out->push_back(203);
        out->push_back(0);
        out->push_back(113);
        out->push_back(12);
      } else {
        out->push_back(192);
        out->push_back(0);
        out->push_back(2);
        out->push_back(44);
      }
    } else {
      PutU16(out, 16);
      const unsigned char dual_addr[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
                                           0,    0,    0,    0,    0, 0, 0, 7};
      if (name == "dual.test") {
        out->insert(out->end(), dual_addr, dual_addr + sizeof(dual_addr));
      } else {
        for (int i = 0; i < 15; ++i) {
          out->push_back(0);
        }
        out->push_back(1);
      }
    }
  }

  void Reply(const unsigned char *buf, size_t len, size_t question_end,
             const std::string &name, uint16_t qtype, const sockaddr_in &peer,
             socklen_t peer_len) {
    const bool nxdomain = IsNxDomain(name);
    const bool has_answer =
        !nxdomain && !IsNoData(name) && (qtype == 1 || qtype == 28);
    std::vector<unsigned char> out;
    out.reserve(128);
    out.push_back(buf[0]);
    out.push_back(buf[1]);
    PutU16(&out, nxdomain ? 0x8183 : 0x8180);
    PutU16(&out, 1);
    PutU16(&out, has_answer ? 1 : 0);
    PutU16(&out, 0);
    PutU16(&out, 0);
    out.insert(out.end(), &buf[12], &buf[question_end]);
    if (has_answer) {
      AppendAnswer(&out, name, qtype, 0);
    }
    (void)sendto(fd_, out.data(), out.size(), 0,
                 reinterpret_cast<const sockaddr *>(&peer), peer_len);
    (void)len;
  }

  void Run() {
    while (running_) {
      unsigned char buf[512];
      sockaddr_in peer;
      socklen_t peer_len = sizeof(peer);
      const ssize_t n =
          recvfrom(fd_, buf, sizeof(buf), 0,
                   reinterpret_cast<sockaddr *>(&peer), &peer_len);
      if (n < 0) {
        continue;
      }
      std::string name;
      uint16_t qtype = 0;
      size_t question_end = 0;
      if (!ParseQuestion(buf, static_cast<size_t>(n), &name, &qtype,
                         &question_end)) {
        continue;
      }
      NoteQuestion(name);
      if (ShouldDrop(name)) {
        continue;
      }
      if (!WaitForReleaseIfNeeded(name)) {
        continue;
      }
      Reply(buf, static_cast<size_t>(n), question_end, name, qtype, peer,
            peer_len);
    }
  }

  int fd_ = -1;
  uint16_t port_ = 0;
  pthread_t thread_ = 0;
  volatile bool running_ = false;
  bool released_ = false;
  pthread_mutex_t mu_;
  pthread_cond_t cv_;
  std::vector<std::string> questions_;
};

void StartNumericSuccess(const char *name, uint16_t port, int family,
                         CallbackState *cb, odin_event_loop_t **out_loop,
                         odin_dns_resolver_t **out_resolver,
                         odin_dns_query_t **out_query,
                         const odin_dns_resolver_config_t *config = nullptr) {
  BasicLoopResolver(out_loop, out_resolver, config);
  cb->start_returned = false;
  ASSERT_EQ(odin_dns_resolve_start(*out_resolver, name, std::strlen(name), port,
                                   family, OnDns, cb, out_query),
            0)
      << std::strerror(errno);
  cb->start_returned = true;
  EXPECT_EQ(cb->calls, 0);
}

void ExpectInjectedStatus(int status, int expected_errno) {
  odin_event_loop_t *loop = nullptr;
  odin_dns_resolver_t *resolver = nullptr;
  BasicLoopResolver(&loop, &resolver);
  PushResultStatus(status);
  CallbackState cb;
  odin_dns_query_t *query = nullptr;
  ASSERT_EQ(odin_dns_resolve_start(resolver, "status.test", 11, 80, AF_INET,
                                   OnDns, &cb, &query),
            0)
      << std::strerror(errno);
  EXPECT_EQ(cb.calls, 0);
  RunLoopUntil(loop, &cb, 1);
  ASSERT_EQ(cb.records.size(), static_cast<size_t>(1));
  EXPECT_EQ(cb.records[0].status, ODIN_DNS_ERROR);
  ExpectErrorNoAddresses(cb.records[0], expected_errno);
  odin_dns_query_destroy(query);
  odin_dns_resolver_destroy(resolver);
  odin_event_loop_destroy(loop);
  ExpectZeroLiveness();
}

odin_dns_query_t *StartFixtureQuery(odin_dns_resolver_t *resolver,
                                    const char *name, uint16_t port, int family,
                                    CallbackState *cb) {
  odin_dns_query_t *query = nullptr;
  cb->start_returned = false;
  if (odin_dns_resolve_start(resolver, name, std::strlen(name), port, family,
                             OnDns, cb, &query) != 0) {
    ADD_FAILURE() << std::strerror(errno);
    return nullptr;
  }
  cb->start_returned = true;
  return query;
}

void DispatchReady(odin_event_io_t *io, unsigned int events,
                   odin_event_loop_t *loop) {
  odin_event_loop_test_ready_t ready;
  ready.io = io;
  ready.events = events;
  ASSERT_EQ(odin_event_loop_test_dispatch_backend_events(loop, &ready, 1), 0)
      << std::strerror(errno);
}

struct AbortReleaseState {
  odin_dns_query_t *query = nullptr;
  DnsFixture *fixture = nullptr;
};

void AbortAndReleaseCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                       void *user_data) {
  (void)loop;
  AbortReleaseState *state = static_cast<AbortReleaseState *>(user_data);
  odin_event_timer_stop(timer);
  odin_dns_query_destroy(state->query);
  state->query = nullptr;
  state->fixture->Release();
}

struct DestroyResolverState {
  odin_dns_resolver_t **resolver = nullptr;
};

void DestroyResolverCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                       void *user_data) {
  (void)loop;
  DestroyResolverState *state = static_cast<DestroyResolverState *>(user_data);
  odin_event_timer_stop(timer);
  odin_dns_resolver_destroy(*state->resolver);
  *state->resolver = nullptr;
}

} // namespace

TEST(OdinDnsResolverTest, T1) {
  AssertParentCaresInitUnchanged();
  DnsRunDeadline::Run([] {
    odin_dns_query_destroy(nullptr);
    odin_dns_resolver_destroy(nullptr);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    odin_dns_resolver_config_t bad_timeout = {nullptr, -1, 0};
    odin_dns_resolver_t *resolver_sentinel =
        reinterpret_cast<odin_dns_resolver_t *>(0x1);
    EXPECT_EQ(odin_dns_resolver_create(loop, &bad_timeout, &resolver_sentinel),
              -1);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(resolver_sentinel, reinterpret_cast<odin_dns_resolver_t *>(0x1));

    odin_dns_resolver_config_t bad_tries = {nullptr, 0, -1};
    EXPECT_EQ(odin_dns_resolver_create(loop, &bad_tries, &resolver_sentinel),
              -1);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(resolver_sentinel, reinterpret_cast<odin_dns_resolver_t *>(0x1));

    EXPECT_EQ(odin_dns_resolver_test_liveness(nullptr), -1);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(odin_dns_resolver_test_cares_observation(nullptr), -1);
    EXPECT_EQ(errno, EINVAL);

    odin_dns_resolver_t *resolver = nullptr;
    ASSERT_EQ(odin_dns_resolver_create(loop, nullptr, &resolver), 0)
        << std::strerror(errno);
    odin_dns_resolver_destroy(resolver);

    odin_dns_resolver_config_t zero_config = {nullptr, 0, 0};
    resolver = nullptr;
    ASSERT_EQ(odin_dns_resolver_create(loop, &zero_config, &resolver), 0)
        << std::strerror(errno);
    odin_dns_resolver_destroy(resolver);

    odin_dns_query_destroy(nullptr);
    odin_dns_resolver_destroy(nullptr);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();
  });
  AssertParentCaresInitUnchanged();
}

TEST(OdinDnsResolverTest, T2) {
  AssertParentCaresInitUnchanged();
  DnsRunDeadline::Run([] {
    PreinitCaresBeforeThreads();
    DnsFixture fixture;
    std::string servers = fixture.servers_csv();
    odin_dns_resolver_config_t config = {servers.c_str(), 250, 1};
    odin_event_loop_t *loop = nullptr;
    odin_dns_resolver_t *resolver = nullptr;
    BasicLoopResolver(&loop, &resolver, &config);
    servers.assign("127.0.0.1:1");
    CallbackState cb;
    odin_dns_query_t *query =
        StartFixtureQuery(resolver, "probe.test", 0, AF_INET, &cb);
    RunLoopUntil(loop, &cb, 1);
    ASSERT_EQ(cb.records.size(), static_cast<size_t>(1));
    EXPECT_EQ(cb.records[0].status, ODIN_DNS_OK);
    EXPECT_EQ(cb.records[0].err, 0);
    ExpectAddressCount(cb.records[0], 1);
    EXPECT_TRUE(HasFamily(cb.records[0], AF_INET));
    ExpectIpv4Result(cb.records[0], "127.0.0.1", 0, 60);
    EXPECT_TRUE(fixture.SawQuestion("probe.test"));

    odin_dns_resolver_test_cares_observation_t obs;
    ASSERT_EQ(odin_dns_resolver_test_cares_observation(&obs), 0);
    EXPECT_EQ(obs.getaddrinfo_calls, static_cast<size_t>(1));
    EXPECT_EQ(obs.ares_freeaddrinfo_calls, static_cast<size_t>(1));
    EXPECT_TRUE((obs.last_ai_flags & ARES_AI_NOSORT) != 0);
    EXPECT_TRUE((obs.last_ai_flags & ARES_AI_NUMERICSERV) != 0);
    EXPECT_EQ(obs.last_ai_family, AF_INET);

    odin_dns_query_destroy(query);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();
  });
  AssertParentCaresInitUnchanged();
}

TEST(OdinDnsResolverTest, T3) {
  AssertParentCaresInitUnchanged();
  DnsRunDeadline::Run([] {
    PreinitCaresBeforeThreads();
    DnsFixture fixture;
    const std::string servers = fixture.servers_csv();
    odin_dns_resolver_config_t config = {servers.c_str(), 250, 1};

    auto run_subcase = [&](int family, size_t expected_count, bool expect_v4,
                           bool expect_v6) {
      ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
      odin_event_loop_t *loop = nullptr;
      odin_dns_resolver_t *resolver = nullptr;
      BasicLoopResolver(&loop, &resolver, &config);
      CallbackState cb;
      odin_dns_query_t *query =
          StartFixtureQuery(resolver, "dual.test", 65535, family, &cb);
      RunLoopUntil(loop, &cb, 1);
      ASSERT_EQ(cb.records.size(), static_cast<size_t>(1));
      const CapturedCallback &record = cb.records[0];
      EXPECT_EQ(record.status, ODIN_DNS_OK);
      ExpectAddressCount(record, expected_count);
      EXPECT_EQ(HasFamily(record, AF_INET), expect_v4);
      EXPECT_EQ(HasFamily(record, AF_INET6), expect_v6);
      if (expect_v4) {
        ExpectIpv4Result(record, "203.0.113.7", 65535, 60);
      }
      if (expect_v6) {
        ExpectIpv6Result(record, "2001:db8::7", 65535, 60);
      }

      odin_dns_resolver_test_cares_observation_t obs;
      ASSERT_EQ(odin_dns_resolver_test_cares_observation(&obs), 0);
      EXPECT_EQ(obs.getaddrinfo_calls, static_cast<size_t>(1));
      EXPECT_EQ(obs.last_ai_family, family);
      EXPECT_TRUE((obs.last_ai_flags & ARES_AI_NUMERICSERV) != 0);
      EXPECT_TRUE((obs.last_ai_flags & ARES_AI_NOSORT) != 0);

      odin_dns_query_destroy(query);
      odin_dns_resolver_destroy(resolver);
      odin_event_loop_destroy(loop);
      ExpectZeroLiveness();
    };

    run_subcase(AF_UNSPEC, 2, true, true);
    run_subcase(AF_INET, 1, true, false);
    run_subcase(AF_INET6, 1, false, true);
  });
  AssertParentCaresInitUnchanged();
}

TEST(OdinDnsResolverTest, T4) {
  AssertParentCaresInitUnchanged();
  DnsRunDeadline::Run([] {
    PreinitCaresBeforeThreads();
    DnsFixture fixture;
    const std::string numeric_servers = fixture.servers_csv();
    odin_dns_resolver_config_t fixture_config = {numeric_servers.c_str(), 0, 0};
    odin_dns_resolver_config_t zero = {nullptr, 0, 0};
    odin_dns_resolver_config_t nonzero = {nullptr, 250, 1};

    auto run_success = [&](const char *literal, int family,
                           const odin_dns_resolver_config_t *config,
                           bool expect_nonzero_config) {
      ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
      odin_event_loop_t *loop = nullptr;
      odin_dns_resolver_t *resolver = nullptr;
      odin_dns_query_t *query = nullptr;
      CallbackState cb;
      StartNumericSuccess(literal, 1, family, &cb, &loop, &resolver, &query,
                          config);
      odin_dns_resolver_test_liveness_t live;
      ASSERT_EQ(odin_dns_resolver_test_liveness(&live), 0);
      EXPECT_EQ(live.timers, static_cast<size_t>(1));
      RunLoopUntil(loop, &cb, 1);
      ASSERT_EQ(cb.records.size(), static_cast<size_t>(1));
      const CapturedCallback &record = cb.records[0];
      EXPECT_TRUE(record.start_returned);
      EXPECT_EQ(record.status, ODIN_DNS_OK);
      ExpectAddressCount(record, 1);
      if (family == AF_INET) {
        ExpectIpv4Result(record, literal, 1, -1);
        EXPECT_FALSE(HasFamily(record, AF_INET6));
      } else {
        ExpectIpv6Result(record, literal, 1, -1);
        EXPECT_FALSE(HasFamily(record, AF_INET));
      }

      odin_dns_resolver_test_cares_observation_t obs;
      ASSERT_EQ(odin_dns_resolver_test_cares_observation(&obs), 0);
      ExpectDnsOnlyInitOptions(obs);
      if (expect_nonzero_config) {
        EXPECT_TRUE((obs.last_init_options_optmask & ARES_OPT_TIMEOUTMS) != 0);
        EXPECT_TRUE((obs.last_init_options_optmask & ARES_OPT_TRIES) != 0);
        EXPECT_EQ(obs.last_init_options_timeout_ms, 250);
        EXPECT_EQ(obs.last_init_options_tries, 1);
      } else {
        EXPECT_EQ((obs.last_init_options_optmask & ARES_OPT_TIMEOUTMS), 0);
        EXPECT_EQ((obs.last_init_options_optmask & ARES_OPT_TRIES), 0);
      }
      EXPECT_TRUE(fixture.Questions().empty());

      odin_dns_query_destroy(query);
      odin_dns_resolver_destroy(resolver);
      odin_event_loop_destroy(loop);
      ExpectZeroLiveness();
    };

    run_success("192.0.2.10", AF_INET, &fixture_config, false);
    run_success("2001:db8::10", AF_INET6, &fixture_config, false);
    run_success("192.0.2.14", AF_INET, nullptr, false);
    run_success("192.0.2.15", AF_INET, &zero, false);
    run_success("192.0.2.13", AF_INET, &nonzero, true);

    ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
    odin_event_loop_t *loop = nullptr;
    odin_dns_resolver_t *resolver = nullptr;
    odin_dns_query_t *query = nullptr;
    CallbackState cb;
    StartNumericSuccess("192.0.2.11", 1, AF_INET, &cb, &loop, &resolver,
                        &query);
    EXPECT_EQ(cb.calls, 0);
    odin_dns_query_destroy(query);
    RunShortLoopExpectNoCallback(loop, &cb);
    EXPECT_TRUE(fixture.Questions().empty());
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();
  });
  AssertParentCaresInitUnchanged();
}

TEST(OdinDnsResolverTest, T5) {
  AssertParentCaresInitUnchanged();
  DnsRunDeadline::Run([] {
    PreinitCaresBeforeThreads();
    DnsFixture fixture;
    const std::string servers = fixture.servers_csv();
    odin_dns_resolver_config_t config = {servers.c_str(), 250, 1};
    odin_event_loop_t *loop = nullptr;
    odin_dns_resolver_t *resolver = nullptr;
    BasicLoopResolver(&loop, &resolver, &config);
    CallbackState cb;
    odin_dns_query_t *query =
        StartFixtureQuery(resolver, "missing.test", 80, AF_INET, &cb);
    RunLoopUntil(loop, &cb, 1);
    ASSERT_EQ(cb.records.size(), static_cast<size_t>(1));
    EXPECT_EQ(cb.records[0].query, query);
    EXPECT_EQ(cb.records[0].user_data, &cb);
    ExpectErrorNoAddresses(cb.records[0], EHOSTUNREACH);
    odin_dns_query_destroy(query);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();

    ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
    BasicLoopResolver(&loop, &resolver, &config);
    cb = CallbackState();
    query = StartFixtureQuery(resolver, "nodata.test", 80, AF_INET, &cb);
    RunLoopUntil(loop, &cb, 1);
    ASSERT_EQ(cb.records.size(), static_cast<size_t>(1));
    EXPECT_EQ(cb.records[0].query, query);
    EXPECT_EQ(cb.records[0].user_data, &cb);
    ExpectErrorNoAddresses(cb.records[0], EHOSTUNREACH);
    odin_dns_query_destroy(query);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();
  });
  AssertParentCaresInitUnchanged();
}

TEST(OdinDnsResolverTest, T6) {
  AssertParentCaresInitUnchanged();
  DnsRunDeadline::Run([] {
    PreinitCaresBeforeThreads();
    DnsFixture fixture;
    const std::string servers = fixture.servers_csv();
    odin_dns_resolver_config_t config = {servers.c_str(), 250, 1};

    auto run_timeout = [&](const char *name) {
      odin_event_loop_t *loop = nullptr;
      odin_dns_resolver_t *resolver = nullptr;
      BasicLoopResolver(&loop, &resolver, &config);
      CallbackState cb;
      odin_dns_query_t *query =
          StartFixtureQuery(resolver, name, 80, AF_INET, &cb);
      RunLoopUntil(loop, &cb, 1);
      ASSERT_EQ(cb.records.size(), static_cast<size_t>(1));
      EXPECT_EQ(cb.records[0].query, query);
      EXPECT_EQ(cb.records[0].user_data, &cb);
      ExpectErrorNoAddresses(cb.records[0], ETIMEDOUT);

      odin_dns_resolver_test_cares_observation_t obs;
      ASSERT_EQ(odin_dns_resolver_test_cares_observation(&obs), 0);
      ExpectDnsOnlyInitOptions(obs);
      EXPECT_TRUE((obs.last_init_options_optmask & ARES_OPT_TIMEOUTMS) != 0);
      EXPECT_TRUE((obs.last_init_options_optmask & ARES_OPT_TRIES) != 0);
      EXPECT_EQ(obs.last_init_options_timeout_ms, 250);
      EXPECT_EQ(obs.last_init_options_tries, 1);

      odin_dns_query_destroy(query);
      odin_dns_resolver_destroy(resolver);
      odin_event_loop_destroy(loop);
      ExpectZeroLiveness();
    };

    run_timeout("timeout.test");

    ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
    const size_t questions_before_alias = fixture.Questions().size();
    char aliases_path[] = "/tmp/odin_dns_aliases.XXXXXX";
    const int alias_fd = mkstemp(aliases_path);
    ASSERT_NE(alias_fd, -1) << std::strerror(errno);
    const char alias_body[] = "aliaspeer rewritten.test\n";
    ASSERT_EQ(write(alias_fd, alias_body, sizeof(alias_body) - 1),
              static_cast<ssize_t>(sizeof(alias_body) - 1));
    close(alias_fd);
    setenv("HOSTALIASES", aliases_path, 1);
    run_timeout("aliaspeer");
    unsetenv("HOSTALIASES");
    unlink(aliases_path);

    const std::vector<std::string> questions = fixture.Questions();
    ASSERT_GT(questions.size(), questions_before_alias);
    for (size_t i = questions_before_alias; i < questions.size(); ++i) {
      EXPECT_EQ(questions[i], "aliaspeer");
    }
  });
  AssertParentCaresInitUnchanged();
}

TEST(OdinDnsResolverTest, T7) {
  AssertParentCaresInitUnchanged();
  DnsRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    odin_dns_resolver_t *resolver = nullptr;
    BasicLoopResolver(&loop, &resolver);
    odin_dns_query_t *const sentinel_value =
        reinterpret_cast<odin_dns_query_t *>(0x1);
    odin_dns_query_t *sentinel = sentinel_value;
    CallbackState cb;
    int rc = odin_dns_resolve_start(resolver, nullptr, 1, 80, AF_INET, OnDns,
                                    &cb, &sentinel);
    ExpectRejectedStart(loop, rc, EINVAL, &cb);
    EXPECT_EQ(sentinel, sentinel_value);

    cb = CallbackState();
    sentinel = sentinel_value;
    rc = odin_dns_resolve_start(resolver, "x", 0, 80, AF_INET, OnDns, &cb,
                                &sentinel);
    ExpectRejectedStart(loop, rc, EINVAL, &cb);
    EXPECT_EQ(sentinel, sentinel_value);

    std::string name255(255, 'a');
    PushResultStatus(ARES_ECONNREFUSED);
    cb = CallbackState();
    odin_dns_query_t *query = nullptr;
    EXPECT_EQ(odin_dns_resolve_start(resolver, name255.data(), name255.size(),
                                     80, AF_INET, OnDns, &cb, &query),
              0);
    odin_dns_query_destroy(query);

    std::string name256(256, 'a');
    cb = CallbackState();
    sentinel = sentinel_value;
    rc = odin_dns_resolve_start(resolver, name256.data(), name256.size(), 80,
                                AF_INET, OnDns, &cb, &sentinel);
    ExpectRejectedStart(loop, rc, EINVAL, &cb);
    EXPECT_EQ(sentinel, sentinel_value);

    char embedded[] = {'a', '\0', 'b'};
    cb = CallbackState();
    sentinel = sentinel_value;
    rc = odin_dns_resolve_start(resolver, embedded, sizeof(embedded), 80,
                                AF_INET, OnDns, &cb, &sentinel);
    ExpectRejectedStart(loop, rc, EINVAL, &cb);
    EXPECT_EQ(sentinel, sentinel_value);

    cb = CallbackState();
    sentinel = sentinel_value;
    rc = odin_dns_resolve_start(resolver, "x", 1, 80, AF_INET, nullptr, &cb,
                                &sentinel);
    ExpectRejectedStart(loop, rc, EINVAL, &cb);
    EXPECT_EQ(sentinel, sentinel_value);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();
  });
  AssertParentCaresInitUnchanged();
}

TEST(OdinDnsResolverTest, T8) {
  AssertParentCaresInitUnchanged();
  DnsRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    odin_dns_resolver_t *resolver = nullptr;
    BasicLoopResolver(&loop, &resolver);
    odin_dns_query_t *const sentinel_value =
        reinterpret_cast<odin_dns_query_t *>(0x1);
    odin_dns_query_t *sentinel = sentinel_value;
    CallbackState cb;
    int rc = odin_dns_resolve_start(resolver, "x.test", 6, 80, 255, OnDns, &cb,
                                    &sentinel);
    ExpectRejectedStart(loop, rc, EAFNOSUPPORT, &cb);
    EXPECT_EQ(sentinel, sentinel_value);

    cb = CallbackState();
    sentinel = sentinel_value;
    errno = ERANGE;
    rc = odin_dns_resolve_start(resolver, "x.test", 6, 80, AF_INET, nullptr,
                                &cb, &sentinel);
    ExpectRejectedStart(loop, rc, EINVAL, &cb);
    EXPECT_EQ(sentinel, sentinel_value);

    cb = CallbackState();
    rc = odin_dns_resolve_start(resolver, "x.test", 6, 80, AF_INET, OnDns, &cb,
                                nullptr);
    ExpectRejectedStart(loop, rc, EINVAL, &cb);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();

    ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
    odin_dns_resolver_config_t bad = {"127.0.0.1:badport", 0, 0};
    BasicLoopResolver(&loop, &resolver, &bad);
    cb = CallbackState();
    sentinel = sentinel_value;
    rc = odin_dns_resolve_start(resolver, "127.0.0.1", 9, 80, AF_INET, OnDns,
                                &cb, &sentinel);
    ExpectRejectedStart(loop, rc, EINVAL, &cb);
    EXPECT_EQ(sentinel, sentinel_value);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();
  });
  AssertParentCaresInitUnchanged();
}

TEST(OdinDnsResolverTest, T9) {
  AssertParentCaresInitUnchanged();
  DnsRunDeadline::Run([] {
    PreinitCaresBeforeThreads();
    DnsFixture fixture;
    const std::string servers = fixture.servers_csv();
    odin_dns_resolver_config_t config = {servers.c_str(), 250, 1};
    odin_event_loop_t *loop = nullptr;
    odin_dns_resolver_t *resolver = nullptr;
    BasicLoopResolver(&loop, &resolver, &config);
    CallbackState cb;
    odin_dns_query_t *abort_query =
        StartFixtureQuery(resolver, "abort.test", 80, AF_INET, &cb);
    odin_dns_query_t *sibling =
        StartFixtureQuery(resolver, "sibling.test", 80, AF_INET, &cb);
    odin_dns_resolver_test_liveness_t live;
    ASSERT_EQ(odin_dns_resolver_test_liveness(&live), 0);
    EXPECT_EQ(live.queries, static_cast<size_t>(2));
    EXPECT_EQ(live.cares_channels, static_cast<size_t>(2));
    EXPECT_GT(live.watches + live.timers, static_cast<size_t>(0));
    AbortReleaseState abort_state;
    abort_state.query = abort_query;
    abort_state.fixture = &fixture;
    odin_event_timer_t *abort_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 20000, 0, AbortAndReleaseCb,
                                     &abort_state, &abort_timer),
              0)
        << std::strerror(errno);
    RunLoopUntil(loop, &cb, 1);
    ASSERT_EQ(cb.records.size(), static_cast<size_t>(1));
    EXPECT_EQ(abort_state.query, nullptr);
    EXPECT_EQ(cb.records[0].query, sibling);
    EXPECT_EQ(cb.records[0].status, ODIN_DNS_OK);
    ExpectAddressCount(cb.records[0], 1);
    ExpectIpv4Result(cb.records[0], "203.0.113.11", 80, 60);
    odin_dns_resolver_test_cares_observation_t obs;
    ASSERT_EQ(odin_dns_resolver_test_cares_observation(&obs), 0);
    EXPECT_GE(obs.ares_destroy_calls, static_cast<size_t>(2));
    odin_dns_query_destroy(sibling);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();
  });
  AssertParentCaresInitUnchanged();
}

TEST(OdinDnsResolverTest, T10) {
  AssertParentCaresInitUnchanged();
  DnsRunDeadline::Run([] {
    PreinitCaresBeforeThreads();
    DnsFixture fixture;
    const std::string servers = fixture.servers_csv();
    odin_dns_resolver_config_t config = {servers.c_str(), 250, 1};
    odin_event_loop_t *loop = nullptr;
    odin_dns_resolver_t *resolver = nullptr;
    BasicLoopResolver(&loop, &resolver, &config);
    CallbackState cb;
    cb.destroy_query = true;
    odin_dns_query_t *query =
        StartFixtureQuery(resolver, "callback-destroy.test", 443, AF_INET, &cb);
    RunLoopUntil(loop, &cb, 1);
    ASSERT_EQ(cb.records.size(), static_cast<size_t>(1));
    EXPECT_EQ(cb.records[0].query, query);
    EXPECT_EQ(cb.records[0].user_data, &cb);
    EXPECT_EQ(cb.records[0].status, ODIN_DNS_OK);
    ExpectAddressCount(cb.records[0], 1);
    ExpectIpv4Result(cb.records[0], "203.0.113.9", 443, 60);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();
  });
  AssertParentCaresInitUnchanged();
}

TEST(OdinDnsResolverTest, T11) {
  AssertParentCaresInitUnchanged();
  DnsRunDeadline::Run([] {
    PreinitCaresBeforeThreads();
    DnsFixture fixture;
    const std::string servers = fixture.servers_csv();
    odin_dns_resolver_config_t config = {servers.c_str(), 250, 1};
    odin_event_loop_t *loop = nullptr;
    odin_dns_resolver_t *resolver = nullptr;
    BasicLoopResolver(&loop, &resolver, &config);
    CallbackState cb;
    cb.stop_after = 2;
    odin_dns_query_t *ok =
        StartFixtureQuery(resolver, "ok.test", 80, AF_INET, &cb);
    odin_dns_query_t *no =
        StartFixtureQuery(resolver, "no.test", 80, AF_INET, &cb);
    RunLoopUntil(loop, &cb, 2);
    ASSERT_EQ(cb.records.size(), static_cast<size_t>(2));
    bool saw_ok = false;
    bool saw_no = false;
    for (const CapturedCallback &record : cb.records) {
      if (record.query == ok) {
        saw_ok = true;
        EXPECT_EQ(record.status, ODIN_DNS_OK);
        ExpectAddressCount(record, 1);
        ExpectIpv4Result(record, "203.0.113.10", 80, 60);
      } else if (record.query == no) {
        saw_no = true;
        ExpectErrorNoAddresses(record, EHOSTUNREACH);
      }
    }
    EXPECT_TRUE(saw_ok);
    EXPECT_TRUE(saw_no);
    odin_dns_query_destroy(ok);
    odin_dns_query_destroy(no);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();
  });
  AssertParentCaresInitUnchanged();
}

TEST(OdinDnsResolverTest, T12) {
  AssertParentCaresInitUnchanged();
  DnsRunDeadline::Run([] {
    PreinitCaresBeforeThreads();
    DnsFixture fixture;
    const std::string servers = fixture.servers_csv();
    odin_dns_resolver_config_t config = {servers.c_str(), 250, 1};
    odin_event_loop_t *loop = nullptr;
    odin_dns_resolver_t *resolver = nullptr;
    CallbackState cb;
    BasicLoopResolver(&loop, &resolver, &config);
    odin_dns_query_t *hold =
        StartFixtureQuery(resolver, "hold.test", 80, AF_INET, &cb);
    odin_dns_query_t *noresponse =
        StartFixtureQuery(resolver, "noresponse.test", 80, AF_INET, &cb);
    ASSERT_NE(hold, nullptr);
    ASSERT_NE(noresponse, nullptr);
    odin_dns_resolver_test_liveness_t live;
    ASSERT_EQ(odin_dns_resolver_test_liveness(&live), 0);
    EXPECT_EQ(live.queries, static_cast<size_t>(2));
    EXPECT_EQ(live.cares_channels, static_cast<size_t>(2));
    DestroyResolverState destroy_state;
    destroy_state.resolver = &resolver;
    odin_event_timer_t *destroy_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 20000, 0, DestroyResolverCb,
                                     &destroy_state, &destroy_timer),
              0)
        << std::strerror(errno);
    RunLoopForNoCallback(loop, &cb, 100000);
    EXPECT_EQ(resolver, nullptr);
    odin_dns_resolver_test_cares_observation_t obs;
    ASSERT_EQ(odin_dns_resolver_test_cares_observation(&obs), 0);
    EXPECT_GE(obs.ares_destroy_calls, static_cast<size_t>(2));
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();

    ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
    odin_dns_query_t *query = nullptr;
    cb = CallbackState();
    StartNumericSuccess("192.0.2.12", 80, AF_INET, &cb, &loop, &resolver,
                        &query);
    odin_dns_resolver_destroy(resolver);
    RunShortLoopExpectNoCallback(loop, &cb);
    ASSERT_EQ(odin_dns_resolver_test_cares_observation(&obs), 0);
    EXPECT_GE(obs.ares_destroy_calls, static_cast<size_t>(1));
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();

    ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
    BasicLoopResolver(&loop, &resolver, &config);
    cb = CallbackState();
    query = StartFixtureQuery(resolver, "completed.test", 80, AF_INET, &cb);
    RunLoopUntil(loop, &cb, 1);
    ASSERT_EQ(cb.records.size(), static_cast<size_t>(1));
    EXPECT_EQ(cb.records[0].query, query);
    EXPECT_EQ(cb.records[0].status, ODIN_DNS_OK);
    ExpectAddressCount(cb.records[0], 1);
    ExpectIpv4Result(cb.records[0], "203.0.113.12", 80, 60);
    odin_dns_resolver_destroy(resolver);
    ASSERT_EQ(odin_dns_resolver_test_cares_observation(&obs), 0);
    EXPECT_GE(obs.ares_destroy_calls, static_cast<size_t>(1));
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();
  });
  AssertParentCaresInitUnchanged();
}

TEST(OdinDnsResolverTest, T13) {
  AssertParentCaresInitUnchanged();
  DnsRunDeadline::Run([] {
    PreinitCaresBeforeThreads();
    DnsFixture fixture;
    const std::string servers = fixture.servers_csv();
    odin_dns_resolver_config_t config = {servers.c_str(), 50, 1};
    odin_event_loop_t *loop = nullptr;
    odin_dns_resolver_t *resolver = nullptr;
    BasicLoopResolver(&loop, &resolver, &config);
    CallbackState cb;
    odin_dns_query_t *query =
        StartFixtureQuery(resolver, "noresponse.test", 80, AF_INET, &cb);
    odin_event_io_t *io = nullptr;
    int fd = -1;
    ASSERT_EQ(odin_dns_resolver_test_first_watch(query, &io, &fd), 0)
        << std::strerror(errno);
    PushResultStatus(ARES_ECONNREFUSED, ARES_FD_EVENT_READ);
    DispatchReady(io, ODIN_EVENT_ERROR, loop);
    ASSERT_EQ(cb.calls, 1);
    ExpectErrorNoAddresses(cb.records[0], ECONNREFUSED);
    odin_dns_query_destroy(query);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();
  });
  AssertParentCaresInitUnchanged();
}

TEST(OdinDnsResolverTest, T14) {
  AssertParentCaresInitUnchanged();
  DnsRunDeadline::Run([] {
    PreinitCaresBeforeThreads();
    DnsFixture fixture;
    const std::string servers = fixture.servers_csv();
    odin_dns_resolver_config_t config = {servers.c_str(), 50, 1};

    odin_event_loop_t *loop = nullptr;
    odin_dns_resolver_t *resolver = nullptr;
    BasicLoopResolver(&loop, &resolver, &config);
    CallbackState cb;
    odin_dns_query_t *query =
        StartFixtureQuery(resolver, "remove.test", 80, AF_INET, &cb);
    odin_event_io_t *io = nullptr;
    int fd = -1;
    ASSERT_EQ(odin_dns_resolver_test_first_watch(query, &io, &fd), 0);
    PushSockState(0, 0);
    DispatchReady(io, ODIN_EVENT_READ, loop);
    EXPECT_EQ(cb.calls, 0);
    EXPECT_EQ(odin_dns_resolver_test_first_watch(query, &io, &fd), -1);
    EXPECT_EQ(errno, ENOENT);
    odin_dns_query_destroy(query);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();

    ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
    BasicLoopResolver(&loop, &resolver, &config);
    cb = CallbackState();
    query = StartFixtureQuery(resolver, "write.test", 80, AF_INET, &cb);
    ASSERT_EQ(odin_dns_resolver_test_first_watch(query, &io, &fd), 0);
    PushSockState(0, 1);
    DispatchReady(io, ODIN_EVENT_READ, loop);
    ASSERT_EQ(odin_dns_resolver_test_first_watch(query, &io, &fd), 0);
#if defined(__APPLE__)
    unsigned int mask = 0;
    ASSERT_EQ(odin_event_loop_test_kqueue_registered_mask(loop, fd, &mask), 0);
    EXPECT_EQ(mask, ODIN_EVENT_WRITE);
#endif
    PushResultStatus(ARES_ECONNREFUSED, ARES_FD_EVENT_WRITE);
    DispatchReady(io, ODIN_EVENT_WRITE, loop);
    ASSERT_EQ(cb.calls, 1);
    ExpectErrorNoAddresses(cb.records[0], ECONNREFUSED);
    odin_dns_query_destroy(query);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();

    ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
    BasicLoopResolver(&loop, &resolver, &config);
    cb = CallbackState();
    query = StartFixtureQuery(resolver, "write.test", 80, AF_INET, &cb);
    ASSERT_EQ(odin_dns_resolver_test_first_watch(query, &io, &fd), 0);
    PushSockState(1, 1);
    DispatchReady(io, ODIN_EVENT_READ, loop);
#if defined(__APPLE__)
    ASSERT_EQ(odin_event_loop_test_kqueue_registered_mask(loop, fd, &mask), 0);
    EXPECT_EQ(mask, ODIN_EVENT_READ | ODIN_EVENT_WRITE);
#endif
    odin_dns_query_destroy(query);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();

    ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
    BasicLoopResolver(&loop, &resolver, &config);
    cb = CallbackState();
    query = StartFixtureQuery(resolver, "fatal.test", 80, AF_INET, &cb);
    ASSERT_EQ(odin_dns_resolver_test_first_watch(query, &io, &fd), 0);
    PushProcessStatus(ARES_ENOMEM, ARES_FD_EVENT_READ);
    DispatchReady(io, ODIN_EVENT_READ, loop);
    ASSERT_EQ(cb.calls, 1);
    ExpectErrorNoAddresses(cb.records[0], ENOMEM);
    odin_dns_query_destroy(query);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();

#if defined(__APPLE__)
    ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
    BasicLoopResolver(&loop, &resolver, &config);
    cb = CallbackState();
    ASSERT_EQ(
        odin_event_loop_test_fail_next_kqueue_change(
            loop, ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_ADD, ODIN_EVENT_READ, EIO),
        0);
    query = StartFixtureQuery(resolver, "readfail.test", 80, AF_INET, &cb);
    RunLoopUntil(loop, &cb, 1);
    ASSERT_EQ(cb.calls, 1);
    ExpectErrorNoAddresses(cb.records[0], EIO);
    odin_dns_query_destroy(query);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();

    ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
    BasicLoopResolver(&loop, &resolver, &config);
    cb = CallbackState();
    query = StartFixtureQuery(resolver, "write.test", 80, AF_INET, &cb);
    ASSERT_EQ(odin_dns_resolver_test_first_watch(query, &io, &fd), 0);
    ASSERT_EQ(odin_event_loop_test_fail_next_kqueue_change(
                  loop, ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_ADD,
                  ODIN_EVENT_WRITE, EIO),
              0);
    PushSockState(1, 1);
    DispatchReady(io, ODIN_EVENT_READ, loop);
    ASSERT_EQ(cb.calls, 1);
    ExpectErrorNoAddresses(cb.records[0], EIO);
    odin_dns_query_destroy(query);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();
#endif

    ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
    BasicLoopResolver(&loop, &resolver, &config);
    bool timed_out = false;
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 1500000, 0, WatchdogCb, &timed_out,
                                     &watchdog),
              0)
        << std::strerror(errno);
    ASSERT_EQ(odin_event_loop_test_fail_next_timer_start(loop, ENOMEM), 0);
    cb = CallbackState();
    query = StartFixtureQuery(resolver, "timer.test", 80, AF_INET, &cb);
    cb.loop = loop;
    ASSERT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_FALSE(timed_out);
    EXPECT_EQ(cb.calls, 1);
    ExpectErrorNoAddresses(cb.records[0], ENOMEM);
    odin_dns_query_destroy(query);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();

    ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
    BasicLoopResolver(&loop, &resolver, &config);
    PushTimeoutStep(INT64_MAX, 999999);
    cb = CallbackState();
    query = StartFixtureQuery(resolver, "overflow.test", 80, AF_INET, &cb);
    RunLoopUntil(loop, &cb, 1);
    ExpectErrorNoAddresses(cb.records[0], EOVERFLOW);
    odin_dns_query_destroy(query);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();

    ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
    BasicLoopResolver(&loop, &resolver);
    timed_out = false;
    ASSERT_EQ(odin_event_timer_start(loop, 1500000, 0, WatchdogCb, &timed_out,
                                     &watchdog),
              0)
        << std::strerror(errno);
    ASSERT_EQ(odin_event_loop_test_fail_next_timer_start(loop, ENOMEM), 0);
    cb = CallbackState();
    odin_dns_query_t *sentinel = reinterpret_cast<odin_dns_query_t *>(0x1);
    EXPECT_EQ(odin_dns_resolve_start(resolver, "192.0.2.200", 11, 1, AF_INET,
                                     OnDns, &cb, &sentinel),
              -1);
    EXPECT_EQ(errno, ENOMEM);
    EXPECT_EQ(sentinel, reinterpret_cast<odin_dns_query_t *>(0x1));
    EXPECT_EQ(cb.calls, 0);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();
  });
  AssertParentCaresInitUnchanged();
}

TEST(OdinDnsResolverTest, T15) {
  AssertParentCaresInitUnchanged();
  DnsRunDeadline::Run([] {
    PreinitCaresBeforeThreads();
    odin_event_loop_t *loop = nullptr;
    odin_dns_resolver_t *resolver = nullptr;
    BasicLoopResolver(&loop, &resolver);
    odin_dns_query_t *const sentinel_value =
        reinterpret_cast<odin_dns_query_t *>(0x1);
    odin_dns_query_t *sentinel = sentinel_value;
    CallbackState cb;
    PushResultStatus(ARES_ECANCELLED);
    int rc = odin_dns_resolve_start(resolver, "cancel.test", 11, 80, AF_INET,
                                    OnDns, &cb, &sentinel);
    ExpectRejectedStart(loop, rc, ECANCELED, &cb);
    EXPECT_EQ(sentinel, sentinel_value);

    cb = CallbackState();
    sentinel = sentinel_value;
    PushResultStatus(ARES_EDESTRUCTION);
    rc = odin_dns_resolve_start(resolver, "destroy.test", 12, 80, AF_INET,
                                OnDns, &cb, &sentinel);
    ExpectRejectedStart(loop, rc, ECANCELED, &cb);
    EXPECT_EQ(sentinel, sentinel_value);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();

    ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
    DnsFixture fixture;
    const std::string servers = fixture.servers_csv();
    odin_dns_resolver_config_t config = {servers.c_str(), 50, 1};
    BasicLoopResolver(&loop, &resolver, &config);
    cb = CallbackState();
    odin_dns_query_t *query =
        StartFixtureQuery(resolver, "noresponse.test", 80, AF_INET, &cb);
    odin_event_io_t *io = nullptr;
    int fd = -1;
    ASSERT_EQ(odin_dns_resolver_test_first_watch(query, &io, &fd), 0);
    PushResultStatus(ARES_ECANCELLED, ARES_FD_EVENT_READ);
    DispatchReady(io, ODIN_EVENT_READ, loop);
    ASSERT_EQ(cb.calls, 1);
    ExpectErrorNoAddresses(cb.records[0], ECANCELED);
    odin_dns_query_destroy(query);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();

    ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
    BasicLoopResolver(&loop, &resolver, &config);
    cb = CallbackState();
    query = StartFixtureQuery(resolver, "noresponse.test", 80, AF_INET, &cb);
    ASSERT_EQ(odin_dns_resolver_test_first_watch(query, &io, &fd), 0);
    PushResultStatus(ARES_EDESTRUCTION, ARES_FD_EVENT_READ);
    DispatchReady(io, ODIN_EVENT_READ, loop);
    ASSERT_EQ(cb.calls, 1);
    ExpectErrorNoAddresses(cb.records[0], ECANCELED);
    odin_dns_query_destroy(query);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();

    ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
    BasicLoopResolver(&loop, &resolver);
    PushResultStatus(ARES_SUCCESS);
    ASSERT_EQ(odin_dns_resolver_test_fail_next_result_alloc(), 0);
    cb = CallbackState();
    query = nullptr;
    ASSERT_EQ(odin_dns_resolve_start(resolver, "copyfail.test", 13, 80, AF_INET,
                                     OnDns, &cb, &query),
              0);
    RunLoopUntil(loop, &cb, 1);
    ExpectErrorNoAddresses(cb.records[0], ENOMEM);
    odin_dns_resolver_test_cares_observation_t obs;
    ASSERT_EQ(odin_dns_resolver_test_cares_observation(&obs), 0);
    EXPECT_EQ(obs.ares_freeaddrinfo_calls, static_cast<size_t>(1));
    odin_dns_query_destroy(query);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();
  });
  AssertParentCaresInitUnchanged();
}

TEST(OdinDnsResolverTest, T16) {
  AssertParentCaresInitUnchanged();
  RunExecChild("OdinDnsResolverExecChild.T16LibraryInit", "T16_LIBRARY_INIT");
  DnsRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    odin_dns_resolver_t *resolver = nullptr;
    BasicLoopResolver(&loop, &resolver);
    const int init_statuses[] = {ARES_ENOMEM, ARES_EBADRESP};
    const int init_errors[] = {ENOMEM, EIO};
    for (size_t i = 0; i < 2; ++i) {
      PushStep(ODIN_DNS_TEST_CARES_INIT_OPTIONS, init_statuses[i]);
      odin_dns_query_t *sentinel = reinterpret_cast<odin_dns_query_t *>(0x1);
      EXPECT_EQ(odin_dns_resolve_start(resolver, "setup.test", 10, 80, AF_INET,
                                       OnDns, nullptr, &sentinel),
                -1);
      EXPECT_EQ(errno, init_errors[i]);
      EXPECT_EQ(sentinel, reinterpret_cast<odin_dns_query_t *>(0x1));
    }
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();

    const int set_statuses[] = {ARES_ENOMEM, ARES_EFORMERR, ARES_EBADSTR,
                                ARES_ESERVICE, ARES_EBADRESP};
    const int set_errors[] = {ENOMEM, EINVAL, EINVAL, EINVAL, EIO};
    for (size_t i = 0; i < 5; ++i) {
      ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
      odin_dns_resolver_config_t config = {"127.0.0.1:53", 0, 0};
      BasicLoopResolver(&loop, &resolver, &config);
      PushStep(ODIN_DNS_TEST_CARES_SET_SERVERS, set_statuses[i]);
      odin_dns_query_t *sentinel = reinterpret_cast<odin_dns_query_t *>(0x1);
      EXPECT_EQ(odin_dns_resolve_start(resolver, "127.0.0.1", 9, 80, AF_INET,
                                       OnDns, nullptr, &sentinel),
                -1);
      EXPECT_EQ(errno, set_errors[i]);
      EXPECT_EQ(sentinel, reinterpret_cast<odin_dns_query_t *>(0x1));
      odin_dns_resolver_test_cares_observation_t obs;
      ASSERT_EQ(odin_dns_resolver_test_cares_observation(&obs), 0);
      EXPECT_GE(obs.ares_destroy_calls, static_cast<size_t>(1));
      odin_dns_resolver_destroy(resolver);
      odin_event_loop_destroy(loop);
      ExpectZeroLiveness();
    }
  });
  AssertParentCaresInitUnchanged();
}

TEST(OdinDnsResolverTest, T17) {
  AssertParentCaresInitUnchanged();
  DnsRunDeadline::Run([] {
    PreinitCaresBeforeThreads();
    DnsFixture fixture;
    const std::string servers = fixture.servers_csv();
    odin_dns_resolver_config_t config = {servers.c_str(), 50, 1};
    odin_event_loop_t *loop = nullptr;
    odin_dns_resolver_t *resolver = nullptr;
    BasicLoopResolver(&loop, &resolver, &config);
    odin_event_loop_test_set_now_us(loop, 1000000);
    PushTimeoutStep(0, 250000);
    CallbackState cb;
    odin_dns_query_t *query =
        StartFixtureQuery(resolver, "reset.test", 80, AF_INET, &cb);
    odin_event_io_t *io = nullptr;
    int fd = -1;
    ASSERT_EQ(odin_dns_resolver_test_first_watch(query, &io, &fd), 0);
    PushSockState(1, 0);
    PushTimeoutStep(0, 500000);
    DispatchReady(io, ODIN_EVENT_READ, loop);
    odin_dns_resolver_test_liveness_t live;
    ASSERT_EQ(odin_dns_resolver_test_liveness(&live), 0);
    EXPECT_EQ(live.timers, static_cast<size_t>(1));
    ExpectNextTimerDelay(loop, 1000000, 500000);
    EXPECT_EQ(cb.calls, 0);
    odin_dns_query_destroy(query);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();

    ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
    BasicLoopResolver(&loop, &resolver, &config);
    odin_event_loop_test_set_now_us(loop, 1000000);
    PushTimeoutStep(0, 250000);
    cb = CallbackState();
    query = StartFixtureQuery(resolver, "reset.test", 80, AF_INET, &cb);
    ASSERT_EQ(odin_dns_resolver_test_first_watch(query, &io, &fd), 0);
    odin_event_loop_test_set_now_us(loop, UINT64_MAX - 100);
    PushSockState(1, 0);
    PushTimeoutStep(1, 0);
    DispatchReady(io, ODIN_EVENT_READ, loop);
    ASSERT_EQ(cb.calls, 1);
    ExpectErrorNoAddresses(cb.records[0], EOVERFLOW);
    odin_dns_query_destroy(query);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();

    ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
    BasicLoopResolver(&loop, &resolver, &config);
    PushTimeoutStep(0, 250000);
    cb = CallbackState();
    query = StartFixtureQuery(resolver, "nullstop.test", 80, AF_INET, &cb);
    ASSERT_EQ(odin_dns_resolver_test_first_watch(query, &io, &fd), 0);
    PushSockState(1, 0);
    PushStep(ODIN_DNS_TEST_CARES_TIMEOUT_NULL);
    DispatchReady(io, ODIN_EVENT_READ, loop);
    ASSERT_EQ(odin_dns_resolver_test_liveness(&live), 0);
    EXPECT_EQ(live.timers, static_cast<size_t>(0));
    EXPECT_EQ(cb.calls, 0);
    RunLoopForNoCallback(loop, &cb, 300000);
    odin_dns_query_destroy(query);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();

    ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
    BasicLoopResolver(&loop, &resolver, &config);
    PushTimeoutStep(0, 0);
    cb = CallbackState();
    query = StartFixtureQuery(resolver, "timer.test", 80, AF_INET, &cb);
    PushProcessStatus(ARES_ENOMEM, 0, true);
    RunLoopUntil(loop, &cb, 1);
    ExpectErrorNoAddresses(cb.records[0], ENOMEM);
    odin_dns_query_destroy(query);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
    ExpectZeroLiveness();
  });
  AssertParentCaresInitUnchanged();
}

TEST(OdinDnsResolverTest, T18) {
  AssertParentCaresInitUnchanged();
  RunExecChild("OdinDnsResolverExecChild.T18CachedSuccessConcurrency",
               "T18_CACHED_SUCCESS_CONCURRENCY");
  AssertParentCaresInitUnchanged();
}

TEST(OdinDnsResolverTest, T19) {
  AssertParentCaresInitUnchanged();
  DnsRunDeadline::Run([] {
    ExpectInjectedStatus(ARES_ECONNREFUSED, ECONNREFUSED);
    ExpectInjectedStatus(ARES_EREFUSED, ECONNREFUSED);
    ExpectInjectedStatus(ARES_ENOSERVER, ECONNREFUSED);
    ExpectInjectedStatus(ARES_ENOMEM, ENOMEM);
    ExpectInjectedStatus(ARES_ENOTIMP, EAFNOSUPPORT);
  });
  AssertParentCaresInitUnchanged();
}

TEST(OdinDnsResolverTest, T20) {
  AssertParentCaresInitUnchanged();
  DnsRunDeadline::Run([] {
    ExpectInjectedStatus(ARES_ESERVICE, EINVAL);
    ExpectInjectedStatus(ARES_EFORMERR, EINVAL);
    ExpectInjectedStatus(ARES_EBADSTR, EINVAL);
    ExpectInjectedStatus(ARES_EBADRESP, EIO);
  });
  AssertParentCaresInitUnchanged();
}

TEST(OdinDnsResolverExecChild, T16LibraryInit) {
  ODIN_DNS_REQUIRE_CHILD_MODE("T16LibraryInit", "T16_LIBRARY_INIT");
  ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0) << std::strerror(errno);
  odin_event_loop_t *loop = nullptr;
  ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
  PushStep(ODIN_DNS_TEST_CARES_LIBRARY_INIT, ARES_ENOMEM);
  odin_dns_resolver_t *sentinel = reinterpret_cast<odin_dns_resolver_t *>(0x1);
  EXPECT_EQ(odin_dns_resolver_create(loop, nullptr, &sentinel), -1);
  EXPECT_EQ(errno, ENOMEM);
  EXPECT_EQ(sentinel, reinterpret_cast<odin_dns_resolver_t *>(0x1));
  odin_dns_resolver_t *second = reinterpret_cast<odin_dns_resolver_t *>(0x2);
  EXPECT_EQ(odin_dns_resolver_create(loop, nullptr, &second), -1);
  EXPECT_EQ(errno, ENOMEM);
  EXPECT_EQ(second, reinterpret_cast<odin_dns_resolver_t *>(0x2));
  odin_dns_resolver_test_cares_observation_t obs;
  ASSERT_EQ(odin_dns_resolver_test_cares_observation(&obs), 0)
      << std::strerror(errno);
  EXPECT_EQ(obs.ares_library_init_calls, static_cast<size_t>(1));
  odin_event_loop_destroy(loop);
  ExpectZeroLiveness();
}

struct ThreadCreateState {
  class ThreadBarrier *before_create = nullptr;
  class ThreadBarrier *after_create = nullptr;
  int rc = -1;
  int err = 0;
  int resolver_nonnull = 0;
};

class ThreadBarrier {
public:
  explicit ThreadBarrier(int total) : total_(total) {
    EXPECT_EQ(pthread_mutex_init(&mu_, nullptr), 0);
    EXPECT_EQ(pthread_cond_init(&cv_, nullptr), 0);
  }

  ~ThreadBarrier() {
    pthread_cond_destroy(&cv_);
    pthread_mutex_destroy(&mu_);
  }

  void Wait() {
    pthread_mutex_lock(&mu_);
    const int phase = phase_;
    arrived_ += 1;
    if (arrived_ == total_) {
      arrived_ = 0;
      phase_ += 1;
      pthread_cond_broadcast(&cv_);
    } else {
      while (phase == phase_) {
        pthread_cond_wait(&cv_, &mu_);
      }
    }
    pthread_mutex_unlock(&mu_);
  }

private:
  int total_;
  int arrived_ = 0;
  int phase_ = 0;
  pthread_mutex_t mu_;
  pthread_cond_t cv_;
};

void *CreateResolverThread(void *arg) {
  ThreadCreateState *state = static_cast<ThreadCreateState *>(arg);
  odin_event_loop_t *loop = nullptr;
  if (odin_event_loop_create(&loop) != 0) {
    state->err = errno;
    state->before_create->Wait();
    state->after_create->Wait();
    return nullptr;
  }
  state->before_create->Wait();
  odin_dns_resolver_t *resolver = nullptr;
  state->rc = odin_dns_resolver_create(loop, nullptr, &resolver);
  state->err = state->rc == 0 ? 0 : errno;
  state->resolver_nonnull = resolver != nullptr ? 1 : 0;
  state->after_create->Wait();
  odin_dns_resolver_destroy(resolver);
  odin_event_loop_destroy(loop);
  return nullptr;
}

TEST(OdinDnsResolverExecChild, T18CachedSuccessConcurrency) {
  ODIN_DNS_REQUIRE_CHILD_MODE("T18CachedSuccessConcurrency",
                              "T18_CACHED_SUCCESS_CONCURRENCY");
  ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0) << std::strerror(errno);
  odin_event_loop_t *loop = nullptr;
  odin_dns_resolver_t *resolver = nullptr;
  BasicLoopResolver(&loop, &resolver);
  odin_dns_resolver_destroy(resolver);
  odin_event_loop_destroy(loop);

  ThreadCreateState a;
  ThreadCreateState b;
  ThreadBarrier before_create(2);
  ThreadBarrier after_create(2);
  a.before_create = &before_create;
  a.after_create = &after_create;
  b.before_create = &before_create;
  b.after_create = &after_create;
  pthread_t ta;
  pthread_t tb;
  ASSERT_EQ(pthread_create(&ta, nullptr, CreateResolverThread, &a), 0);
  ASSERT_EQ(pthread_create(&tb, nullptr, CreateResolverThread, &b), 0);
  ASSERT_EQ(pthread_join(ta, nullptr), 0);
  ASSERT_EQ(pthread_join(tb, nullptr), 0);
  EXPECT_EQ(a.rc, 0);
  EXPECT_EQ(a.err, 0);
  EXPECT_EQ(a.resolver_nonnull, 1);
  EXPECT_EQ(b.rc, 0);
  EXPECT_EQ(b.err, 0);
  EXPECT_EQ(b.resolver_nonnull, 1);

  odin_dns_resolver_test_cares_observation_t obs;
  ASSERT_EQ(odin_dns_resolver_test_cares_observation(&obs), 0)
      << std::strerror(errno);
  EXPECT_EQ(obs.ares_library_init_calls, static_cast<size_t>(1));
  ExpectZeroLiveness();
}

// NOLINTEND(misc-const-correctness, misc-use-internal-linkage)
