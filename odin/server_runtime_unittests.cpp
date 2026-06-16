// odin/server_runtime_unittests.cpp
//
// Unit tests T1-T13 from §5 of odin/docs/rfc_021_server_runtime.md.

#include "odin/server_runtime.h"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "odin/accept_loop_internal_test.h"
#include "odin/event_loop.h"
#include "odin/event_loop_internal_test.h"
#include "odin/protocol.h"
#include "odin/server_runtime_internal_test.h"

#include "gtest/gtest.h"

// NOLINTBEGIN(misc-const-correctness, misc-use-internal-linkage)

namespace {

class ServerRuntimeRunDeadline {
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
      FAIL() << "ServerRuntimeRunDeadline exceeded 2 seconds";
    }
    ASSERT_TRUE(WIFEXITED(wstatus));
    EXPECT_EQ(WEXITSTATUS(wstatus), 0);
  }
};

struct ServerRuntimeState {
  int on_runtime_error_calls = 0;
  int on_runtime_error_err = 0;
  bool timed_out = false;
  bool destroy_in_cb = false;
  bool stop_in_error = false;
  bool is_terminal_observed_in_cb = false;
  odin_event_loop_t *loop = nullptr;
  std::atomic<bool> peer_done{false};
  std::atomic<bool> all_peers_done{false};
  std::atomic<bool> peer2_done{false};
  std::atomic<bool> peer_resp_seen{false};
  std::atomic<bool> peer3_resp_seen{false};
  std::atomic<int> peer3_fd{-1};
};

class FilterState {
 public:
  FilterState() { std::memset(&last_addr_, 0, sizeof(last_addr_)); }

  void Record(const struct sockaddr *addr) {
    calls_.fetch_add(1);
    last_addr_ = *reinterpret_cast<const struct sockaddr_in *>(addr);
  }

  int calls() const { return calls_.load(); }

  const struct sockaddr_in &last_addr() const { return last_addr_; }

 private:
  std::atomic<int> calls_{0};
  struct sockaddr_in last_addr_;
};

void OnRuntimeError(odin_server_runtime_t *rt, int err, void *user_data) {
  ServerRuntimeState *s = static_cast<ServerRuntimeState *>(user_data);
  s->on_runtime_error_calls += 1;
  s->on_runtime_error_err = err;
  s->is_terminal_observed_in_cb =
      odin_server_runtime_test_is_terminal(rt) != 0;
  if (s->destroy_in_cb) {
    odin_server_runtime_destroy(rt);
  }
  if (s->stop_in_error && s->loop != nullptr) {
    odin_event_loop_stop(s->loop);
  }
}

void WatchdogCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                void *user_data) {
  ServerRuntimeState *s = static_cast<ServerRuntimeState *>(user_data);
  s->timed_out = true;
  odin_event_timer_stop(timer);
  odin_event_loop_stop(loop);
}

void DummyIoCb(odin_event_loop_t *loop, odin_event_io_t *io, int fd,
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

void SetBlocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  ASSERT_NE(flags, -1) << std::strerror(errno);
  ASSERT_EQ(fcntl(fd, F_SETFL, flags & ~O_NONBLOCK), 0)
      << std::strerror(errno);
}

void SetRecvTimeout200ms(int fd) {
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 200000;
  ASSERT_EQ(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)), 0)
      << std::strerror(errno);
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

int OpenLoopbackClient(uint16_t port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  SetNonblock(fd);
  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) !=
      0) {
    if (errno != EINPROGRESS) {
      close(fd);
      return -1;
    }
  }
  return fd;
}

int ConnectLoopbackBlocking(uint16_t port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) !=
      0) {
    close(fd);
    return -1;
  }
  return fd;
}

int WaitAccept(int lfd, int deadline_ms) {
  struct pollfd pfd;
  pfd.fd = lfd;
  pfd.events = POLLIN;
  const int prc = poll(&pfd, 1, deadline_ms);
  if (prc <= 0) {
    return -1;
  }
  return accept(lfd, nullptr, nullptr);
}

void ExpectListenerStillWorks(int lfd, uint16_t port) {
  const int peer = ConnectLoopbackBlocking(port);
  ASSERT_GE(peer, 0) << std::strerror(errno);
  const int accepted = WaitAccept(lfd, 1000);
  ASSERT_GE(accepted, 0) << std::strerror(errno);
  close(accepted);
  close(peer);
}

std::string EncodedReq(const std::string &host, uint16_t port) {
  odin_proto_iov_t iov[3];
  uint8_t hdr[3];
  uint8_t portbe[2];
  const odin_proto_status_t st = odin_proto_encode_connect_req(
      host.data(), host.size(), port, iov, hdr, portbe);
  EXPECT_EQ(st, ODIN_PROTO_OK);
  std::string out;
  for (int i = 0; i < 3; ++i) {
    out.append(static_cast<const char *>(iov[i].base), iov[i].len);
  }
  return out;
}

bool WriteAll(int fd, const void *buf, size_t len) {
  const uint8_t *p = static_cast<const uint8_t *>(buf);
  size_t off = 0;
  while (off < len) {
    const ssize_t n = write(fd, p + off, len - off);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOTCONN) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        (void)poll(&pfd, 1, 1000);
        continue;
      }
      return false;
    }
    off += static_cast<size_t>(n);
  }
  return true;
}

size_t ReadExactly(int fd, void *buf, size_t len, int deadline_ms) {
  uint8_t *p = static_cast<uint8_t *>(buf);
  size_t off = 0;
  const auto start = std::chrono::steady_clock::now();
  while (off < len) {
    const auto now = std::chrono::steady_clock::now();
    const int elapsed = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
            .count());
    if (elapsed >= deadline_ms) {
      return off;
    }
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int prc = poll(&pfd, 1, deadline_ms - elapsed);
    if (prc <= 0) {
      return off;
    }
    const ssize_t n = read(fd, p + off, len - off);
    if (n == 0) {
      return off;
    }
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      return off;
    }
    off += static_cast<size_t>(n);
  }
  return off;
}

void ExpectRespBytes(const uint8_t resp[4], uint16_t code) {
  EXPECT_EQ(resp[0], ODIN_PROTO_VERSION_V1);
  EXPECT_EQ(resp[1], ODIN_PROTO_FRAME_CONNECT_RESP);
  EXPECT_EQ(resp[2], static_cast<uint8_t>(code >> 8));
  EXPECT_EQ(resp[3], static_cast<uint8_t>(code & 0xff));
  size_t consumed = 0;
  uint16_t decoded = 0xffffu;
  EXPECT_EQ(odin_proto_decode_connect_resp(resp, 4, &consumed, &decoded),
            ODIN_PROTO_OK);
  EXPECT_EQ(consumed, 4u);
  EXPECT_EQ(decoded, code);
}

void ReadRespExpect(int fd, uint16_t code) {
  uint8_t resp[4] = {0};
  const size_t n = ReadExactly(fd, resp, sizeof(resp), 1500);
  EXPECT_EQ(n, sizeof(resp));
  if (n == sizeof(resp)) {
    ExpectRespBytes(resp, code);
  }
}

void DrainOkResp(int fd) {
  uint8_t resp[4] = {0};
  ASSERT_EQ(ReadExactly(fd, resp, sizeof(resp), 300), sizeof(resp));
  ExpectRespBytes(resp, ODIN_SERVER_SESSION_RESP_CODE_OK);
}

void ExpectEof(int fd) {
  char c = 0;
  const ssize_t n = read(fd, &c, 1);
  EXPECT_EQ(n, 0) << "read returned " << n << " errno=" << errno << " "
                  << std::strerror(errno);
}

void StartUpstreamExchangeThread(int lfd, const char *upstream_msg,
                                 const char *downstream_msg,
                                 std::atomic<bool> *done) {
  std::thread([lfd, upstream_msg, downstream_msg, done] {
    const int srv = WaitAccept(lfd, 1500);
    EXPECT_GE(srv, 0) << std::strerror(errno);
    if (srv < 0) {
      return;
    }
    EXPECT_TRUE(WriteAll(srv, upstream_msg, std::strlen(upstream_msg)));
    char buf[128] = {0};
    const size_t want = std::strlen(downstream_msg);
    EXPECT_EQ(ReadExactly(srv, buf, want, 1500), want);
    EXPECT_EQ(std::memcmp(buf, downstream_msg, want), 0);
    (void)shutdown(srv, SHUT_WR);
    close(srv);
    if (done != nullptr) {
      done->store(true);
    }
  }).detach();
}

void StartPeerExchangeThread(uint16_t listen_port, uint16_t upstream_port,
                             const char *downstream_msg,
                             const char *upstream_msg,
                             std::atomic<bool> *done) {
  std::thread([listen_port, upstream_port, downstream_msg, upstream_msg, done] {
    const int peer = OpenLoopbackClient(listen_port);
    EXPECT_GE(peer, 0) << std::strerror(errno);
    if (peer < 0) {
      return;
    }
    const std::string req = EncodedReq("127.0.0.1", upstream_port);
    EXPECT_TRUE(WriteAll(peer, req.data(), req.size()));
    ReadRespExpect(peer, ODIN_SERVER_SESSION_RESP_CODE_OK);
    EXPECT_TRUE(WriteAll(peer, downstream_msg, std::strlen(downstream_msg)));
    (void)shutdown(peer, SHUT_WR);
    char buf[128] = {0};
    const size_t want = std::strlen(upstream_msg);
    EXPECT_EQ(ReadExactly(peer, buf, want, 1500), want);
    EXPECT_EQ(std::memcmp(buf, upstream_msg, want), 0);
    close(peer);
    done->store(true);
  }).detach();
}

struct PollFlagCtx {
  std::atomic<bool> *flag = nullptr;
};

void PollFlagStopCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                    void *user_data) {
  PollFlagCtx *c = static_cast<PollFlagCtx *>(user_data);
  if (c->flag->load()) {
    odin_event_timer_stop(timer);
    odin_event_loop_stop(loop);
  }
}

struct T6PollCtx {
  ServerRuntimeState *state = nullptr;
};

void T6PollStopCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                  void *user_data) {
  T6PollCtx *c = static_cast<T6PollCtx *>(user_data);
  if (c->state->peer_done.load() && c->state->on_runtime_error_calls == 1) {
    odin_event_timer_stop(timer);
    odin_event_loop_stop(loop);
  }
}

struct DestroyActiveCtx {
  odin_server_runtime_t *rt = nullptr;
  int expected_inflight = 0;
};

void DestroyActiveCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                     void *user_data) {
  DestroyActiveCtx *c = static_cast<DestroyActiveCtx *>(user_data);
  ASSERT_EQ(odin_server_runtime_test_inflight_count(c->rt),
            c->expected_inflight);
  odin_server_runtime_destroy(c->rt);
  odin_event_timer_stop(timer);
  odin_event_loop_stop(loop);
}

struct AcceptFaultCtx {
  odin_server_runtime_t *rt = nullptr;
  uint16_t listen_port = 0;
  int expected_inflight = -1;
  int peer_fd = -1;
};

void AcceptFaultCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                   void *user_data) {
  (void)loop;
  AcceptFaultCtx *c = static_cast<AcceptFaultCtx *>(user_data);
  if (c->expected_inflight >= 0) {
    ASSERT_EQ(odin_server_runtime_test_inflight_count(c->rt),
              c->expected_inflight);
  }
  ASSERT_EQ(odin_accept_loop_test_fail_next_accept(
                odin_server_runtime_test_get_accept_loop(c->rt), EBADF),
            0)
      << std::strerror(errno);
  c->peer_fd = OpenLoopbackClient(c->listen_port);
  ASSERT_GE(c->peer_fd, 0) << std::strerror(errno);
  odin_event_timer_stop(timer);
}

struct TerminalDestroyCtx {
  odin_server_runtime_t *rt = nullptr;
};

void TerminalDestroyCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                       void *user_data) {
  TerminalDestroyCtx *c = static_cast<TerminalDestroyCtx *>(user_data);
  ASSERT_EQ(odin_server_runtime_test_is_terminal(c->rt), 1);
  ASSERT_EQ(odin_server_runtime_test_inflight_count(c->rt), 1);
  odin_server_runtime_destroy(c->rt);
  odin_event_timer_stop(timer);
  odin_event_loop_stop(loop);
}

struct StartPeer2Ctx {
  odin_server_runtime_t *rt = nullptr;
  ServerRuntimeState *state = nullptr;
  uint16_t listen_port = 0;
  uint16_t upstream_port = 0;
  const char *downstream_msg = nullptr;
  const char *upstream_msg = nullptr;
};

void StartPeer2Cb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                  void *user_data) {
  (void)loop;
  StartPeer2Ctx *c = static_cast<StartPeer2Ctx *>(user_data);
  EXPECT_EQ(odin_server_runtime_test_inflight_count(c->rt), 0);
  EXPECT_EQ(c->state->on_runtime_error_calls, 0);
  StartPeerExchangeThread(c->listen_port, c->upstream_port, c->downstream_msg,
                          c->upstream_msg, &c->state->peer2_done);
  odin_event_timer_stop(timer);
}

int DenyLoopbackCb(const struct sockaddr *addr, socklen_t addrlen,
                   void *user_data) {
  (void)addrlen;
  FilterState *s = static_cast<FilterState *>(user_data);
  s->Record(addr);
  if (s->last_addr().sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
    return EACCES;
  }
  return 0;
}

int DenyAllRecordingCb(const struct sockaddr *addr, socklen_t addrlen,
                       void *user_data) {
  (void)addrlen;
  FilterState *s = static_cast<FilterState *>(user_data);
  s->Record(addr);
  return EACCES;
}

struct T8ReplaceCtx {
  odin_server_runtime_t *rt = nullptr;
  ServerRuntimeState *state = nullptr;
  FilterState *filter_a = nullptr;
  FilterState *filter_b = nullptr;
  uint16_t listen_port = 0;
  uint16_t upstream_port = 0;
};

void T8ReplaceCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                 void *user_data) {
  (void)loop;
  T8ReplaceCtx *c = static_cast<T8ReplaceCtx *>(user_data);
  EXPECT_EQ(c->filter_a->calls(), 1);
  odin_server_runtime_set_dial_filter(c->rt, DenyAllRecordingCb, c->filter_b);
  std::thread([c] {
    const int peer = OpenLoopbackClient(c->listen_port);
    EXPECT_GE(peer, 0) << std::strerror(errno);
    if (peer < 0) {
      return;
    }
    const std::string req = EncodedReq("127.0.0.1", c->upstream_port);
    EXPECT_TRUE(WriteAll(peer, req.data(), req.size()));
    ReadRespExpect(peer, ODIN_SERVER_SESSION_RESP_CODE_OTHER);
    close(peer);
    c->state->peer2_done.store(true);
  }).detach();
  odin_event_timer_stop(timer);
}

void T8ClearCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
               void *user_data) {
  (void)loop;
  T8ReplaceCtx *c = static_cast<T8ReplaceCtx *>(user_data);
  EXPECT_EQ(c->filter_b->calls(), 1);
  EXPECT_TRUE(c->state->peer2_done.load());
  odin_server_runtime_set_dial_filter(c->rt, NULL, NULL);
  std::thread([c] {
    const int peer = OpenLoopbackClient(c->listen_port);
    EXPECT_GE(peer, 0) << std::strerror(errno);
    if (peer < 0) {
      return;
    }
    const std::string req = EncodedReq("127.0.0.1", c->upstream_port);
    EXPECT_TRUE(WriteAll(peer, req.data(), req.size()));
    ReadRespExpect(peer, ODIN_SERVER_SESSION_RESP_CODE_OK);
    c->state->peer3_fd.store(peer);
    c->state->peer3_resp_seen.store(true);
  }).detach();
  odin_event_timer_stop(timer);
}

struct DestroyFromFilterState {
  odin_server_runtime_t *rt = nullptr;
  std::atomic<int> filter_calls{0};
  std::atomic<bool> destroy_done{false};
};

int DestroyFromFilterCb(const struct sockaddr *addr, socklen_t addrlen,
                        void *user_data) {
  (void)addr;
  (void)addrlen;
  DestroyFromFilterState *s =
      static_cast<DestroyFromFilterState *>(user_data);
  s->filter_calls.fetch_add(1);
  odin_server_runtime_destroy(s->rt);
  s->destroy_done.store(true);
  return 0;
}

struct DestroyFilterPollCtx {
  DestroyFromFilterState *filter_state = nullptr;
};

void DestroyFilterPollCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                         void *user_data) {
  DestroyFilterPollCtx *c = static_cast<DestroyFilterPollCtx *>(user_data);
  if (c->filter_state->destroy_done.load()) {
    odin_event_timer_stop(timer);
    odin_event_loop_stop(loop);
  }
}

} // namespace

TEST(OdinServerRuntimeTest, T1) {
  ServerRuntimeRunDeadline::Run([] {
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0) << std::strerror(errno);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    odin_server_runtime_destroy(NULL);
    odin_server_runtime_set_dial_filter(NULL, NULL, NULL);

    odin_event_loop_test_liveness_t liv_pre = {};
    odin_event_loop_test_liveness_t liv_post = {};
    ASSERT_EQ(odin_event_loop_test_liveness(&liv_pre), 0);

    ServerRuntimeState state;
    odin_server_runtime_t *rt = nullptr;
    ASSERT_EQ(odin_server_runtime_create(loop, lfd, OnRuntimeError, &state,
                                         &rt),
              0)
        << std::strerror(errno);
    ASSERT_EQ(odin_event_loop_test_liveness(&liv_post), 0);
    EXPECT_EQ(liv_post.io_handles - liv_pre.io_handles, 1u);
    EXPECT_EQ(odin_server_runtime_test_inflight_count(rt), 0);
    EXPECT_EQ(odin_server_runtime_test_is_terminal(rt), 0);

    odin_server_runtime_destroy(rt);
    EXPECT_GE(fcntl(lfd, F_GETFD), 0);
    ExpectListenerStillWorks(lfd, port);
    odin_server_runtime_destroy(NULL);
    EXPECT_EQ(state.on_runtime_error_calls, 0);

    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinServerRuntimeTest, T2) {
  ServerRuntimeRunDeadline::Run([] {
    uint16_t listen_port = 0;
    uint16_t upstream_port = 0;
    const int lfd = OpenLoopbackListener(&listen_port);
    ASSERT_GE(lfd, 0);
    const int upstream_lfd = OpenLoopbackListener(&upstream_port);
    ASSERT_GE(upstream_lfd, 0);

    std::atomic<bool> upstream_done{false};
    StartUpstreamExchangeThread(upstream_lfd, "upstream-hello",
                                "downstream-hello", &upstream_done);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ServerRuntimeState state;
    state.loop = loop;
    odin_server_runtime_t *rt = nullptr;
    ASSERT_EQ(odin_server_runtime_create(loop, lfd, OnRuntimeError, &state,
                                         &rt),
              0);

    StartPeerExchangeThread(listen_port, upstream_port, "downstream-hello",
                            "upstream-hello", &state.peer_done);
    PollFlagCtx poll_ctx;
    poll_ctx.flag = &state.peer_done;
    odin_event_timer_t *poll_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 5000, 5000, PollFlagStopCb,
                                     &poll_ctx, &poll_timer),
              0);
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 500000, 0, WatchdogCb, &state, &watchdog),
        0);

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(state.on_runtime_error_calls, 0);
    EXPECT_FALSE(state.timed_out);
    EXPECT_TRUE(upstream_done.load());
    EXPECT_TRUE(state.peer_done.load());
    EXPECT_EQ(odin_server_runtime_test_inflight_count(rt), 0);

    odin_server_runtime_destroy(rt);
    close(upstream_lfd);
    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinServerRuntimeTest, T3) {
  ServerRuntimeRunDeadline::Run([] {
    uint16_t listen_port = 0;
    uint16_t upstream_port = 0;
    const int lfd = OpenLoopbackListener(&listen_port);
    ASSERT_GE(lfd, 0);
    const int upstream_lfd = OpenLoopbackListener(&upstream_port);
    ASSERT_GE(upstream_lfd, 0);

    std::atomic<int> upstream_accepts{0};
    std::thread([upstream_lfd, &upstream_accepts] {
      for (int i = 0; i < 3; ++i) {
        const int srv = WaitAccept(upstream_lfd, 1500);
        EXPECT_GE(srv, 0) << std::strerror(errno);
        if (srv < 0) {
          return;
        }
        EXPECT_TRUE(WriteAll(srv, "upstream", 8));
        char buf[16] = {0};
        EXPECT_EQ(ReadExactly(srv, buf, 10, 1500), 10u);
        EXPECT_EQ(std::memcmp(buf, "downstream", 10), 0);
        (void)shutdown(srv, SHUT_WR);
        close(srv);
        upstream_accepts.fetch_add(1);
      }
    }).detach();

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ServerRuntimeState state;
    state.loop = loop;
    odin_server_runtime_t *rt = nullptr;
    ASSERT_EQ(odin_server_runtime_create(loop, lfd, OnRuntimeError, &state,
                                         &rt),
              0);

    std::thread([listen_port, upstream_port, &state] {
      std::atomic<int> peers_done{0};
      for (int i = 0; i < 3; ++i) {
        std::thread([listen_port, upstream_port, &peers_done] {
          const int peer = OpenLoopbackClient(listen_port);
          EXPECT_GE(peer, 0) << std::strerror(errno);
          if (peer < 0) {
            return;
          }
          const std::string req = EncodedReq("127.0.0.1", upstream_port);
          EXPECT_TRUE(WriteAll(peer, req.data(), req.size()));
          ReadRespExpect(peer, ODIN_SERVER_SESSION_RESP_CODE_OK);
          EXPECT_TRUE(WriteAll(peer, "downstream", 10));
          (void)shutdown(peer, SHUT_WR);
          char buf[16] = {0};
          EXPECT_EQ(ReadExactly(peer, buf, 8, 1500), 8u);
          EXPECT_EQ(std::memcmp(buf, "upstream", 8), 0);
          close(peer);
          peers_done.fetch_add(1);
        }).detach();
      }
      const auto start = std::chrono::steady_clock::now();
      while (peers_done.load() != 3) {
        const auto now = std::chrono::steady_clock::now();
        const int elapsed = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
                .count());
        if (elapsed > 1500) {
          break;
        }
        usleep(1000);
      }
      state.all_peers_done.store(peers_done.load() == 3);
    }).detach();

    PollFlagCtx poll_ctx;
    poll_ctx.flag = &state.all_peers_done;
    odin_event_timer_t *poll_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 5000, 5000, PollFlagStopCb,
                                     &poll_ctx, &poll_timer),
              0);
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 500000, 0, WatchdogCb, &state, &watchdog),
        0);

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(state.on_runtime_error_calls, 0);
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(upstream_accepts.load(), 3);
    EXPECT_TRUE(state.all_peers_done.load());
    EXPECT_EQ(odin_server_runtime_test_inflight_count(rt), 0);

    odin_server_runtime_destroy(rt);
    close(upstream_lfd);
    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinServerRuntimeTest, T4) {
  ServerRuntimeRunDeadline::Run([] {
    (void)signal(SIGPIPE, SIG_IGN);
    uint16_t listen_port = 0;
    uint16_t upstream_port = 0;
    const int lfd = OpenLoopbackListener(&listen_port);
    ASSERT_GE(lfd, 0);
    const int upstream_lfd = OpenLoopbackListener(&upstream_port);
    ASSERT_GE(upstream_lfd, 0);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ServerRuntimeState state;
    state.loop = loop;
    odin_server_runtime_t *rt = nullptr;
    ASSERT_EQ(odin_server_runtime_create(loop, lfd, OnRuntimeError, &state,
                                         &rt),
              0);

    const int peer1 = OpenLoopbackClient(listen_port);
    const int peer2 = OpenLoopbackClient(listen_port);
    ASSERT_GE(peer1, 0);
    ASSERT_GE(peer2, 0);
    SetBlocking(peer1);
    SetBlocking(peer2);
    SetRecvTimeout200ms(peer1);
    SetRecvTimeout200ms(peer2);
    const std::string req = EncodedReq("127.0.0.1", upstream_port);
    ASSERT_TRUE(WriteAll(peer1, req.data(), req.size()));
    ASSERT_TRUE(WriteAll(peer2, req.data(), req.size()));

    DestroyActiveCtx dctx;
    dctx.rt = rt;
    dctx.expected_inflight = 2;
    odin_event_timer_t *destroy_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 100000, 0, DestroyActiveCb, &dctx,
                                     &destroy_timer),
              0);
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 500000, 0, WatchdogCb, &state, &watchdog),
        0);

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(state.on_runtime_error_calls, 0);
    EXPECT_FALSE(state.timed_out);
    DrainOkResp(peer1);
    ExpectEof(peer1);
    DrainOkResp(peer2);
    ExpectEof(peer2);
    EXPECT_GE(fcntl(lfd, F_GETFD), 0);
    ExpectListenerStillWorks(lfd, listen_port);

    close(peer1);
    close(peer2);
    close(upstream_lfd);
    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinServerRuntimeTest, T5) {
  ServerRuntimeRunDeadline::Run([] {
    (void)signal(SIGPIPE, SIG_IGN);
    uint16_t listen_port = 0;
    uint16_t upstream_port = 0;
    const int lfd = OpenLoopbackListener(&listen_port);
    ASSERT_GE(lfd, 0);
    const int upstream_lfd = OpenLoopbackListener(&upstream_port);
    ASSERT_GE(upstream_lfd, 0);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ServerRuntimeState state;
    state.loop = loop;
    state.destroy_in_cb = true;
    state.stop_in_error = true;
    odin_server_runtime_t *rt = nullptr;
    ASSERT_EQ(odin_server_runtime_create(loop, lfd, OnRuntimeError, &state,
                                         &rt),
              0);

    const int peer1 = OpenLoopbackClient(listen_port);
    ASSERT_GE(peer1, 0);
    SetBlocking(peer1);
    SetRecvTimeout200ms(peer1);
    const std::string req = EncodedReq("127.0.0.1", upstream_port);
    ASSERT_TRUE(WriteAll(peer1, req.data(), req.size()));

    AcceptFaultCtx fault;
    fault.rt = rt;
    fault.listen_port = listen_port;
    fault.expected_inflight = 1;
    odin_event_timer_t *fault_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 100000, 0, AcceptFaultCb, &fault,
                                     &fault_timer),
              0);
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 500000, 0, WatchdogCb, &state, &watchdog),
        0);

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(state.on_runtime_error_calls, 1);
    EXPECT_EQ(state.on_runtime_error_err, EBADF);
    EXPECT_FALSE(state.timed_out);
    DrainOkResp(peer1);
    ExpectEof(peer1);
    EXPECT_GE(fcntl(lfd, F_GETFD), 0);

    close(peer1);
    if (fault.peer_fd >= 0) {
      close(fault.peer_fd);
    }
    close(upstream_lfd);
    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinServerRuntimeTest, T6) {
  ServerRuntimeRunDeadline::Run([] {
    uint16_t listen_port = 0;
    uint16_t upstream_port = 0;
    const int lfd = OpenLoopbackListener(&listen_port);
    ASSERT_GE(lfd, 0);
    const int upstream_lfd = OpenLoopbackListener(&upstream_port);
    ASSERT_GE(upstream_lfd, 0);

    std::atomic<bool> upstream_done{false};
    StartUpstreamExchangeThread(upstream_lfd, "upstream-T6", "downstream-T6",
                                &upstream_done);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ServerRuntimeState state;
    state.loop = loop;
    odin_server_runtime_t *rt = nullptr;
    ASSERT_EQ(odin_server_runtime_create(loop, lfd, OnRuntimeError, &state,
                                         &rt),
              0);

    std::thread([listen_port, upstream_port, &state] {
      const int peer = OpenLoopbackClient(listen_port);
      EXPECT_GE(peer, 0) << std::strerror(errno);
      if (peer < 0) {
        return;
      }
      const std::string req = EncodedReq("127.0.0.1", upstream_port);
      EXPECT_TRUE(WriteAll(peer, req.data(), req.size()));
      ReadRespExpect(peer, ODIN_SERVER_SESSION_RESP_CODE_OK);
      usleep(120000);
      EXPECT_TRUE(WriteAll(peer, "downstream-T6", 13));
      (void)shutdown(peer, SHUT_WR);
      char buf[32] = {0};
      EXPECT_EQ(ReadExactly(peer, buf, 11, 1500), 11u);
      EXPECT_EQ(std::memcmp(buf, "upstream-T6", 11), 0);
      close(peer);
      state.peer_done.store(true);
    }).detach();

    AcceptFaultCtx fault;
    fault.rt = rt;
    fault.listen_port = listen_port;
    odin_event_timer_t *fault_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 50000, 0, AcceptFaultCb, &fault,
                                     &fault_timer),
              0);
    T6PollCtx poll_ctx;
    poll_ctx.state = &state;
    odin_event_timer_t *poll_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 5000, 5000, T6PollStopCb, &poll_ctx,
                                     &poll_timer),
              0);
    odin_event_timer_t *stop_timer = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 400000, 0, WatchdogCb, &state,
                               &stop_timer),
        0);
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 500000, 0, WatchdogCb, &state, &watchdog),
        0);

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(state.on_runtime_error_calls, 1);
    EXPECT_EQ(state.on_runtime_error_err, EBADF);
    EXPECT_TRUE(state.is_terminal_observed_in_cb);
    EXPECT_FALSE(state.timed_out);
    EXPECT_TRUE(upstream_done.load());
    EXPECT_TRUE(state.peer_done.load());
    EXPECT_EQ(odin_server_runtime_test_is_terminal(rt), 1);
    EXPECT_EQ(odin_server_runtime_test_inflight_count(rt), 0);
    EXPECT_GE(fcntl(lfd, F_GETFD), 0);
    ExpectListenerStillWorks(lfd, listen_port);

    odin_server_runtime_destroy(rt);
    if (fault.peer_fd >= 0) {
      close(fault.peer_fd);
    }
    close(upstream_lfd);
    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinServerRuntimeTest, T7) {
  ServerRuntimeRunDeadline::Run([] {
    (void)signal(SIGPIPE, SIG_IGN);
    uint16_t listen_port = 0;
    uint16_t upstream_port = 0;
    const int lfd = OpenLoopbackListener(&listen_port);
    ASSERT_GE(lfd, 0);
    const int upstream_lfd = OpenLoopbackListener(&upstream_port);
    ASSERT_GE(upstream_lfd, 0);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ServerRuntimeState state;
    state.loop = loop;
    odin_server_runtime_t *rt = nullptr;
    ASSERT_EQ(odin_server_runtime_create(loop, lfd, OnRuntimeError, &state,
                                         &rt),
              0);
    FilterState filter_state;
    odin_server_runtime_set_dial_filter(rt, DenyLoopbackCb, &filter_state);

    std::thread([listen_port, upstream_port, &state] {
      const int peer = OpenLoopbackClient(listen_port);
      EXPECT_GE(peer, 0) << std::strerror(errno);
      if (peer < 0) {
        return;
      }
      const std::string req = EncodedReq("127.0.0.1", upstream_port);
      EXPECT_TRUE(WriteAll(peer, req.data(), req.size()));
      ReadRespExpect(peer, ODIN_SERVER_SESSION_RESP_CODE_OTHER);
      close(peer);
      state.peer_resp_seen.store(true);
    }).detach();

    PollFlagCtx poll_ctx;
    poll_ctx.flag = &state.peer_resp_seen;
    odin_event_timer_t *poll_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 5000, 5000, PollFlagStopCb,
                                     &poll_ctx, &poll_timer),
              0);
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 500000, 0, WatchdogCb, &state, &watchdog),
        0);

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    const int accept_rc = accept(upstream_lfd, nullptr, nullptr);
    const int accept_errno = errno;
    EXPECT_EQ(state.on_runtime_error_calls, 0);
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(filter_state.calls(), 1);
    EXPECT_EQ(filter_state.last_addr().sin_family, AF_INET);
    EXPECT_EQ(filter_state.last_addr().sin_addr.s_addr, htonl(INADDR_LOOPBACK));
    EXPECT_EQ(accept_rc, -1);
    EXPECT_EQ(accept_errno, EAGAIN);
    EXPECT_EQ(odin_server_runtime_test_inflight_count(rt), 0);

    odin_server_runtime_destroy(rt);
    close(upstream_lfd);
    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinServerRuntimeTest, T8) {
  ServerRuntimeRunDeadline::Run([] {
    (void)signal(SIGPIPE, SIG_IGN);
    uint16_t listen_port = 0;
    uint16_t upstream_port = 0;
    const int lfd = OpenLoopbackListener(&listen_port);
    ASSERT_GE(lfd, 0);
    const int upstream_lfd = OpenLoopbackListener(&upstream_port);
    ASSERT_GE(upstream_lfd, 0);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ServerRuntimeState state;
    state.loop = loop;
    odin_server_runtime_t *rt = nullptr;
    ASSERT_EQ(odin_server_runtime_create(loop, lfd, OnRuntimeError, &state,
                                         &rt),
              0);
    FilterState filter_a;
    FilterState filter_b;
    odin_server_runtime_set_dial_filter(rt, DenyAllRecordingCb, &filter_a);

    std::thread([listen_port, upstream_port] {
      const int peer = OpenLoopbackClient(listen_port);
      EXPECT_GE(peer, 0) << std::strerror(errno);
      if (peer < 0) {
        return;
      }
      const std::string req = EncodedReq("127.0.0.1", upstream_port);
      EXPECT_TRUE(WriteAll(peer, req.data(), req.size()));
      ReadRespExpect(peer, ODIN_SERVER_SESSION_RESP_CODE_OTHER);
      close(peer);
    }).detach();

    T8ReplaceCtx t8_ctx;
    t8_ctx.rt = rt;
    t8_ctx.state = &state;
    t8_ctx.filter_a = &filter_a;
    t8_ctx.filter_b = &filter_b;
    t8_ctx.listen_port = listen_port;
    t8_ctx.upstream_port = upstream_port;
    odin_event_timer_t *replace_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 80000, 0, T8ReplaceCb, &t8_ctx,
                                     &replace_timer),
              0);
    odin_event_timer_t *clear_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 160000, 0, T8ClearCb, &t8_ctx,
                                     &clear_timer),
              0);
    PollFlagCtx poll_ctx;
    poll_ctx.flag = &state.peer3_resp_seen;
    odin_event_timer_t *poll_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 5000, 5000, PollFlagStopCb,
                                     &poll_ctx, &poll_timer),
              0);
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 500000, 0, WatchdogCb, &state, &watchdog),
        0);

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(state.on_runtime_error_calls, 0);
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(filter_a.calls(), 1);
    EXPECT_EQ(filter_b.calls(), 1);
    EXPECT_TRUE(state.peer2_done.load());
    EXPECT_TRUE(state.peer3_resp_seen.load());
    EXPECT_EQ(odin_server_runtime_test_inflight_count(rt), 1);

    odin_server_runtime_destroy(rt);
    const int peer3 = state.peer3_fd.load();
    if (peer3 >= 0) {
      close(peer3);
    }
    close(upstream_lfd);
    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinServerRuntimeTest, T9) {
  ServerRuntimeRunDeadline::Run([] {
    (void)signal(SIGPIPE, SIG_IGN);
    uint16_t listen_port = 0;
    uint16_t upstream_port = 0;
    const int lfd = OpenLoopbackListener(&listen_port);
    ASSERT_GE(lfd, 0);
    const int upstream_lfd = OpenLoopbackListener(&upstream_port);
    ASSERT_GE(upstream_lfd, 0);
    std::atomic<bool> upstream_done{false};
    StartUpstreamExchangeThread(upstream_lfd, "upstream-T9-2",
                                "downstream-T9-2", &upstream_done);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ServerRuntimeState state;
    state.loop = loop;
    odin_server_runtime_t *rt = nullptr;
    ASSERT_EQ(odin_server_runtime_create(loop, lfd, OnRuntimeError, &state,
                                         &rt),
              0);
    ASSERT_EQ(odin_server_runtime_test_fail_next_session_create(rt), 0);

    const int peer1 = OpenLoopbackClient(listen_port);
    ASSERT_GE(peer1, 0);
    SetBlocking(peer1);
    SetRecvTimeout200ms(peer1);

    StartPeer2Ctx peer2_ctx;
    peer2_ctx.rt = rt;
    peer2_ctx.state = &state;
    peer2_ctx.listen_port = listen_port;
    peer2_ctx.upstream_port = upstream_port;
    peer2_ctx.downstream_msg = "downstream-T9-2";
    peer2_ctx.upstream_msg = "upstream-T9-2";
    odin_event_timer_t *peer2_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 100000, 0, StartPeer2Cb,
                                     &peer2_ctx, &peer2_timer),
              0);
    PollFlagCtx poll_ctx;
    poll_ctx.flag = &state.peer2_done;
    odin_event_timer_t *poll_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 5000, 5000, PollFlagStopCb,
                                     &poll_ctx, &poll_timer),
              0);
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 500000, 0, WatchdogCb, &state, &watchdog),
        0);

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(state.on_runtime_error_calls, 0);
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(odin_server_runtime_test_inflight_count(rt), 0);
    ExpectEof(peer1);
    EXPECT_TRUE(upstream_done.load());
    EXPECT_TRUE(state.peer2_done.load());
    EXPECT_EQ(odin_server_runtime_test_is_terminal(rt), 0);

    odin_server_runtime_destroy(rt);
    close(peer1);
    close(upstream_lfd);
    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinServerRuntimeTest, T10) {
  ServerRuntimeRunDeadline::Run([] {
    uint16_t listen_port = 0;
    const int lfd = OpenLoopbackListener(&listen_port);
    ASSERT_GE(lfd, 0);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ServerRuntimeState state;

    odin_event_io_t *blocker_io = nullptr;
    ASSERT_EQ(odin_event_io_start(loop, lfd, ODIN_EVENT_READ, DummyIoCb,
                                  nullptr, &blocker_io),
              0);
    odin_event_loop_test_liveness_t liv_pre = {};
    ASSERT_EQ(odin_event_loop_test_liveness(&liv_pre), 0);

    int sentinel_storage = 0;
    void *sentinel = &sentinel_storage;
    odin_server_runtime_t *rt = static_cast<odin_server_runtime_t *>(sentinel);
    errno = 0;
    EXPECT_EQ(odin_server_runtime_create(loop, lfd, OnRuntimeError, &state,
                                         &rt),
              -1);
    EXPECT_EQ(errno, EEXIST);
    EXPECT_EQ(rt, static_cast<odin_server_runtime_t *>(sentinel));
    EXPECT_EQ(state.on_runtime_error_calls, 0);
    EXPECT_GE(fcntl(lfd, F_GETFD), 0);
    odin_event_loop_test_liveness_t liv_post = {};
    ASSERT_EQ(odin_event_loop_test_liveness(&liv_post), 0);
    EXPECT_EQ(liv_post.io_handles, liv_pre.io_handles);

    odin_event_io_stop(blocker_io);
    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinServerRuntimeTest, T11) {
  ServerRuntimeRunDeadline::Run([] {
    (void)signal(SIGPIPE, SIG_IGN);
    uint16_t listen_port = 0;
    uint16_t upstream_port = 0;
    const int lfd = OpenLoopbackListener(&listen_port);
    ASSERT_GE(lfd, 0);
    const int upstream_lfd = OpenLoopbackListener(&upstream_port);
    ASSERT_GE(upstream_lfd, 0);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ServerRuntimeState state;
    state.loop = loop;
    odin_server_runtime_t *rt = nullptr;
    ASSERT_EQ(odin_server_runtime_create(loop, lfd, OnRuntimeError, &state,
                                         &rt),
              0);

    const int peer1 = OpenLoopbackClient(listen_port);
    ASSERT_GE(peer1, 0);
    SetBlocking(peer1);
    SetRecvTimeout200ms(peer1);
    const std::string req = EncodedReq("127.0.0.1", upstream_port);
    ASSERT_TRUE(WriteAll(peer1, req.data(), req.size()));

    AcceptFaultCtx fault;
    fault.rt = rt;
    fault.listen_port = listen_port;
    fault.expected_inflight = 1;
    odin_event_timer_t *fault_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 50000, 0, AcceptFaultCb, &fault,
                                     &fault_timer),
              0);
    TerminalDestroyCtx destroy_ctx;
    destroy_ctx.rt = rt;
    odin_event_timer_t *destroy_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 200000, 0, TerminalDestroyCb,
                                     &destroy_ctx, &destroy_timer),
              0);
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 500000, 0, WatchdogCb, &state, &watchdog),
        0);

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(state.on_runtime_error_calls, 1);
    EXPECT_EQ(state.on_runtime_error_err, EBADF);
    EXPECT_FALSE(state.timed_out);
    DrainOkResp(peer1);
    ExpectEof(peer1);
    EXPECT_GE(fcntl(lfd, F_GETFD), 0);
    ExpectListenerStillWorks(lfd, listen_port);

    close(peer1);
    if (fault.peer_fd >= 0) {
      close(fault.peer_fd);
    }
    close(upstream_lfd);
    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinServerRuntimeTest, T12) {
  ServerRuntimeRunDeadline::Run([] {
    (void)signal(SIGPIPE, SIG_IGN);
    uint16_t listen_port = 0;
    uint16_t upstream_port = 0;
    const int lfd = OpenLoopbackListener(&listen_port);
    ASSERT_GE(lfd, 0);
    const int upstream_lfd = OpenLoopbackListener(&upstream_port);
    ASSERT_GE(upstream_lfd, 0);
    std::atomic<bool> upstream_done{false};
    StartUpstreamExchangeThread(upstream_lfd, "upstream-T12-2",
                                "downstream-T12-2", &upstream_done);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ServerRuntimeState state;
    state.loop = loop;
    odin_server_runtime_t *rt = nullptr;
    ASSERT_EQ(odin_server_runtime_create(loop, lfd, OnRuntimeError, &state,
                                         &rt),
              0);
    ASSERT_EQ(odin_server_runtime_test_fail_next_entry_alloc(rt), 0);

    const int peer1 = OpenLoopbackClient(listen_port);
    ASSERT_GE(peer1, 0);
    SetBlocking(peer1);
    SetRecvTimeout200ms(peer1);

    StartPeer2Ctx peer2_ctx;
    peer2_ctx.rt = rt;
    peer2_ctx.state = &state;
    peer2_ctx.listen_port = listen_port;
    peer2_ctx.upstream_port = upstream_port;
    peer2_ctx.downstream_msg = "downstream-T12-2";
    peer2_ctx.upstream_msg = "upstream-T12-2";
    odin_event_timer_t *peer2_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 100000, 0, StartPeer2Cb,
                                     &peer2_ctx, &peer2_timer),
              0);
    PollFlagCtx poll_ctx;
    poll_ctx.flag = &state.peer2_done;
    odin_event_timer_t *poll_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 5000, 5000, PollFlagStopCb,
                                     &poll_ctx, &poll_timer),
              0);
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 500000, 0, WatchdogCb, &state, &watchdog),
        0);

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(state.on_runtime_error_calls, 0);
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(odin_server_runtime_test_inflight_count(rt), 0);
    EXPECT_EQ(odin_server_runtime_test_is_terminal(rt), 0);
    ExpectEof(peer1);
    EXPECT_TRUE(upstream_done.load());
    EXPECT_TRUE(state.peer2_done.load());

    odin_server_runtime_destroy(rt);
    close(peer1);
    close(upstream_lfd);
    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinServerRuntimeTest, T13) {
  ServerRuntimeRunDeadline::Run([] {
    (void)signal(SIGPIPE, SIG_IGN);
    uint16_t listen_port = 0;
    uint16_t upstream_port = 0;
    const int lfd = OpenLoopbackListener(&listen_port);
    ASSERT_GE(lfd, 0);
    const int upstream_lfd = OpenLoopbackListener(&upstream_port);
    ASSERT_GE(upstream_lfd, 0);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ServerRuntimeState state;
    state.loop = loop;
    odin_server_runtime_t *rt = nullptr;
    ASSERT_EQ(odin_server_runtime_create(loop, lfd, OnRuntimeError, &state,
                                         &rt),
              0);

    DestroyFromFilterState filter_state;
    filter_state.rt = rt;
    odin_server_runtime_set_dial_filter(rt, DestroyFromFilterCb,
                                        &filter_state);

    const int peer1 = OpenLoopbackClient(listen_port);
    ASSERT_GE(peer1, 0);
    SetBlocking(peer1);
    SetRecvTimeout200ms(peer1);
    const std::string req = EncodedReq("127.0.0.1", upstream_port);
    ASSERT_TRUE(WriteAll(peer1, req.data(), req.size()));

    DestroyFilterPollCtx poll_ctx;
    poll_ctx.filter_state = &filter_state;
    odin_event_timer_t *poll_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 5000, 5000, DestroyFilterPollCb,
                                     &poll_ctx, &poll_timer),
              0);
    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 500000, 0, WatchdogCb, &state, &watchdog),
        0);

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(filter_state.filter_calls.load(), 1);
    EXPECT_TRUE(filter_state.destroy_done.load());
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(state.on_runtime_error_calls, 0);
    ExpectEof(peer1);
    EXPECT_GE(fcntl(lfd, F_GETFD), 0);
    ExpectListenerStillWorks(lfd, listen_port);

    close(peer1);
    close(upstream_lfd);
    close(lfd);
    odin_event_loop_destroy(loop);
  });
}

// NOLINTEND(misc-const-correctness, misc-use-internal-linkage)
