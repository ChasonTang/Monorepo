// odin/testing/client_session_unittests.cpp
//
// Unit tests T1-T29 plus T2b/T2c/T11b/T15b/T25b from §5 of
// odin/docs/rfc_023_client_session.md.

#include "odin/client_session.h"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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
#include "odin/protocol.h"
#include "odin/testing/event_loop_internal_test.h"
#if defined(ODIN_CLIENT_SESSION_TESTING)
#include "odin/testing/client_session_internal_test.h"
#endif

#include "gtest/gtest.h"

// NOLINTBEGIN(misc-const-correctness, misc-use-internal-linkage)

namespace {

constexpr char kHttpReq[] =
    "CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n";
constexpr char kHttp200[] = "HTTP/1.1 200 Connection Established\r\n\r\n";
constexpr char kHttp400[] = "HTTP/1.1 400 Bad Request\r\n\r\n";
constexpr char kHttp405[] =
    "HTTP/1.1 405 Method Not Allowed\r\nAllow: CONNECT\r\n\r\n";
constexpr char kHttp414[] = "HTTP/1.1 414 URI Too Long\r\n\r\n";
constexpr char kHttp505[] =
    "HTTP/1.1 505 HTTP Version Not Supported\r\n\r\n";

class ClientSessionRunDeadline {
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
      FAIL() << "ClientSessionRunDeadline exceeded 2 seconds";
    }
    ASSERT_TRUE(WIFEXITED(wstatus));
    EXPECT_EQ(WEXITSTATUS(wstatus), 0);
  }
};

struct ClientSessionState {
  int on_close_calls = 0;
  int on_close_err = 0;
  bool timed_out = false;
  bool destroy_in_cb = false;
  odin_event_loop_t *loop = nullptr;
  int allow_call_count_a = 0;
  int allow_call_count_b = 0;
  int deny_call_count = 0;
  int observed_state = 0;
};

void OnClose(odin_client_session_t *cs, int err, void *user_data) {
  ClientSessionState *s = static_cast<ClientSessionState *>(user_data);
  s->on_close_calls += 1;
  s->on_close_err = err;
  if (s->destroy_in_cb) {
    odin_client_session_destroy(cs);
  }
  if (s->loop != nullptr) {
    odin_event_loop_stop(s->loop);
  }
}

void WatchdogCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                void *user_data) {
  ClientSessionState *s = static_cast<ClientSessionState *>(user_data);
  s->timed_out = true;
  odin_event_timer_stop(timer);
  odin_event_loop_stop(loop);
}

void TestEndCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
               void *user_data) {
  (void)user_data;
  odin_event_timer_stop(timer);
  odin_event_loop_stop(loop);
}

void SetNonblock(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  ASSERT_NE(flags, -1) << std::strerror(errno);
  ASSERT_EQ(fcntl(fd, F_SETFL, flags | O_NONBLOCK), 0) << std::strerror(errno);
}

void MakeUnixPair(int *pa, int *pb) {
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0)
      << std::strerror(errno);
  const int buf_size = 65536;
  (void)setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &buf_size,
                   sizeof(buf_size));
  (void)setsockopt(fds[0], SOL_SOCKET, SO_RCVBUF, &buf_size,
                   sizeof(buf_size));
  (void)setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &buf_size,
                   sizeof(buf_size));
  (void)setsockopt(fds[1], SOL_SOCKET, SO_RCVBUF, &buf_size,
                   sizeof(buf_size));
  SetNonblock(fds[0]);
  SetNonblock(fds[1]);
  *pa = fds[0];
  *pb = fds[1];
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

uint16_t UnusedLoopbackPort() {
  const int s = socket(AF_INET, SOCK_STREAM, 0);
  EXPECT_GE(s, 0) << std::strerror(errno);
  const int reuse = 1;
  (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  EXPECT_EQ(bind(s, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)),
            0)
      << std::strerror(errno);
  socklen_t alen = sizeof(addr);
  EXPECT_EQ(getsockname(s, reinterpret_cast<struct sockaddr *>(&addr), &alen),
            0)
      << std::strerror(errno);
  EXPECT_EQ(close(s), 0) << std::strerror(errno);
  return ntohs(addr.sin_port);
}

void CloseWithRst(int fd) {
  struct linger lg;
  lg.l_onoff = 1;
  lg.l_linger = 0;
  (void)setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
  (void)close(fd);
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
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
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

std::string ReadString(int fd, size_t len, int deadline_ms) {
  std::string out(len, '\0');
  const size_t n = ReadExactly(fd, &out[0], len, deadline_ms);
  out.resize(n);
  return out;
}

bool DrainUntilEof(int fd, std::string *out, int deadline_ms) {
  const auto start = std::chrono::steady_clock::now();
  uint8_t buf[256];
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    const int elapsed = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
            .count());
    if (elapsed >= deadline_ms) {
      return false;
    }
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int prc = poll(&pfd, 1, deadline_ms - elapsed);
    if (prc <= 0) {
      return false;
    }
    const ssize_t n = read(fd, buf, sizeof(buf));
    if (n == 0) {
      return true;
    }
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      return false;
    }
    out->append(reinterpret_cast<const char *>(buf), static_cast<size_t>(n));
  }
}

int AcceptWithPoll(int lfd, int deadline_ms) {
  struct pollfd pfd;
  pfd.fd = lfd;
  pfd.events = POLLIN;
  const int prc = poll(&pfd, 1, deadline_ms);
  if (prc <= 0) {
    return -1;
  }
  return accept(lfd, nullptr, nullptr);
}

std::string EncodedClientReq() {
  odin_proto_iov_t iov[3];
  uint8_t hdr[3];
  uint8_t portbe[2];
  const odin_proto_status_t st = odin_proto_encode_connect_req(
      "example.com", 11, 443, iov, hdr, portbe);
  EXPECT_EQ(st, ODIN_PROTO_OK);
  std::string out;
  for (int i = 0; i < 3; ++i) {
    out.append(static_cast<const char *>(iov[i].base), iov[i].len);
  }
  return out;
}

std::string EncodedResp(uint16_t code) {
  odin_proto_connect_resp_frame_t resp;
  odin_proto_encode_connect_resp(code, &resp);
  return std::string(reinterpret_cast<const char *>(resp.bytes),
                     sizeof(resp.bytes));
}

void StartWatchdog(odin_event_loop_t *loop, ClientSessionState *state,
                   uint64_t delay_us = 300000) {
  odin_event_timer_t *watchdog = nullptr;
  ASSERT_EQ(
      odin_event_timer_start(loop, delay_us, 0, WatchdogCb, state, &watchdog),
      0)
      << std::strerror(errno);
}

void DummyIoCb(odin_event_loop_t *loop, odin_event_io_t *io, int fd,
               unsigned int events, void *user_data) {
  (void)loop;
  (void)io;
  (void)fd;
  (void)events;
  (void)user_data;
}

struct StopAtHandshakeCtx {
  odin_client_session_t *cs;
  bool reached;
};

void StopAtHandshakeCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                       void *user_data) {
  StopAtHandshakeCtx *ctx = static_cast<StopAtHandshakeCtx *>(user_data);
#if defined(ODIN_CLIENT_SESSION_TESTING)
  if (odin_client_session_test_state(ctx->cs) ==
      ODIN_CLIENT_SESSION_TEST_STATE_HANDSHAKE) {
    ctx->reached = true;
    odin_event_timer_stop(timer);
    odin_event_loop_stop(loop);
  }
#else
  (void)ctx;
  (void)timer;
#endif
}

int AllowIncCb(const struct sockaddr *addr, socklen_t addrlen,
               void *user_data) {
  (void)addr;
  (void)addrlen;
  int *slot = static_cast<int *>(user_data);
  *slot += 1;
  return 0;
}

int DenyIncCb(const struct sockaddr *addr, socklen_t addrlen, void *user_data) {
  (void)addr;
  (void)addrlen;
  int *slot = static_cast<int *>(user_data);
  *slot += 1;
  return EACCES;
}

int DenyLoopbackCb(const struct sockaddr *addr, socklen_t addrlen,
                   void *user_data) {
  (void)user_data;
  if (addrlen == sizeof(struct sockaddr_in) &&
      addr->sa_family == AF_INET) {
    const struct sockaddr_in *sin =
        reinterpret_cast<const struct sockaddr_in *>(addr);
    if (sin->sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
      return EACCES;
    }
  }
  return 0;
}

int DestroyInsideFilterCb(const struct sockaddr *addr, socklen_t addrlen,
                          void *user_data) {
  (void)addr;
  (void)addrlen;
  odin_client_session_destroy(
      static_cast<odin_client_session_t *>(user_data));
  return EACCES;
}

int T1DummyCb(const struct sockaddr *addr, socklen_t addrlen, void *user_data) {
  (void)addr;
  (void)addrlen;
  (void)user_data;
  return 0;
}

void HappyServerThread(int lfd, std::string *req_observed = nullptr) {
  const int srv = AcceptWithPoll(lfd, 1500);
  if (srv < 0) {
    return;
  }
  const std::string req = ReadString(srv, EncodedClientReq().size(), 1500);
  if (req_observed != nullptr) {
    *req_observed = req;
  }
  EXPECT_EQ(req, EncodedClientReq());
  const std::string resp = EncodedResp(0);
  EXPECT_TRUE(WriteAll(srv, resp.data(), resp.size()));
  const std::string got = ReadString(srv, 16, 1500);
  EXPECT_EQ(got, std::string("downstream-hello"));
  (void)write(srv, "upstream-hello", 14);
  (void)shutdown(srv, SHUT_WR);
  std::string scratch;
  DrainUntilEof(srv, &scratch, 500);
  close(srv);
}

void SaturateFd(int fd) {
  std::string junk(4096, 'x');
  while (true) {
    const ssize_t n = write(fd, junk.data(), junk.size());
    if (n > 0) {
      continue;
    }
    ASSERT_EQ(n, -1);
    ASSERT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK)
        << std::strerror(errno);
    return;
  }
}

} // namespace

TEST(OdinClientSessionTest, T1) {
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    (void)signal(SIGPIPE, SIG_IGN);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    odin_event_loop_test_liveness_t liv_pre;
    odin_event_loop_test_liveness_t liv_post;
    odin_event_loop_test_liveness_t liv_mid;
    ASSERT_EQ(odin_event_loop_test_liveness(&liv_pre), 0);

    ClientSessionState state;
    state.loop = loop;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9, 80,
                                         OnClose, &state, &cs),
              0)
        << std::strerror(errno);
    ASSERT_EQ(odin_event_loop_test_liveness(&liv_post), 0);
    EXPECT_EQ(liv_post.io_handles - liv_pre.io_handles, 1u);
#if defined(ODIN_CLIENT_SESSION_TESTING)
    EXPECT_EQ(odin_client_session_test_state(cs),
              ODIN_CLIENT_SESSION_TEST_STATE_PARSING);
#endif

    odin_client_session_destroy(nullptr);
    odin_client_session_set_dial_filter(nullptr, nullptr, nullptr);
    odin_client_session_set_dial_filter(nullptr, T1DummyCb, &state);
#if defined(ODIN_CLIENT_SESSION_TESTING)
    EXPECT_EQ(odin_client_session_test_state(cs),
              ODIN_CLIENT_SESSION_TEST_STATE_PARSING);
#endif
    ASSERT_EQ(odin_event_loop_test_liveness(&liv_mid), 0);
    EXPECT_EQ(liv_mid.io_handles, liv_post.io_handles);

    odin_client_session_destroy(cs);
    const ssize_t wn = write(pa, "z", 1);
    const int wer = errno;
    EXPECT_EQ(wn, -1);
    EXPECT_EQ(wer, EPIPE);
    EXPECT_EQ(state.on_close_calls, 0);

    EXPECT_EQ(close(pa), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinClientSessionTest, T2) {
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);

    std::string req_observed;
    std::thread srv_thread(HappyServerThread, lfd, &req_observed);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ClientSessionState state;
    state.loop = loop;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9, port,
                                         OnClose, &state, &cs),
              0);
    odin_client_session_set_dial_filter(cs, AllowIncCb,
                                        &state.allow_call_count_a);
    odin_client_session_set_dial_filter(cs, AllowIncCb,
                                        &state.allow_call_count_b);

    ASSERT_TRUE(WriteAll(pa, kHttpReq, strlen(kHttpReq)));
    StartWatchdog(loop, &state);

    std::string http_resp;
    std::string upstream_observed;
    std::thread test_thread([pa, &http_resp, &upstream_observed] {
      http_resp = ReadString(pa, strlen(kHttp200), 1500);
      (void)write(pa, "downstream-hello", 16);
      (void)shutdown(pa, SHUT_WR);
      DrainUntilEof(pa, &upstream_observed, 1500);
    });

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    srv_thread.join();

    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, 0);
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(state.allow_call_count_a, 0);
    EXPECT_EQ(state.allow_call_count_b, 1);
    EXPECT_EQ(http_resp, std::string(kHttp200));
    EXPECT_EQ(req_observed, EncodedClientReq());
    EXPECT_EQ(upstream_observed, std::string("upstream-hello"));

    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinClientSessionTest, T2b) {
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);

    std::string req_observed;
    std::thread srv_thread(HappyServerThread, lfd, &req_observed);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ClientSessionState state;
    state.loop = loop;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9, port,
                                         OnClose, &state, &cs),
              0);
    ASSERT_TRUE(WriteAll(pa, "CONNECT exa", 11));
    odin_event_timer_t *second_write = nullptr;
    ASSERT_EQ(odin_event_timer_start(
                  loop, 20000, 0,
                  [](odin_event_loop_t *l, odin_event_timer_t *t, void *ud) {
                    (void)l;
                    const int fd = *static_cast<int *>(ud);
                    const char rest[] =
                        "mple.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n";
                    (void)WriteAll(fd, rest, strlen(rest));
                    odin_event_timer_stop(t);
                  },
                  &pa, &second_write),
              0);
    StartWatchdog(loop, &state);

    std::string http_resp;
    std::string upstream_observed;
    std::thread test_thread([pa, &http_resp, &upstream_observed] {
      http_resp = ReadString(pa, strlen(kHttp200), 1500);
      (void)write(pa, "downstream-hello", 16);
      (void)shutdown(pa, SHUT_WR);
      DrainUntilEof(pa, &upstream_observed, 1500);
    });

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    srv_thread.join();

    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, 0);
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(http_resp, std::string(kHttp200));
    EXPECT_EQ(req_observed, EncodedClientReq());
    EXPECT_EQ(upstream_observed, std::string("upstream-hello"));

    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinClientSessionTest, T2c) {
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    std::string req_observed;
    std::thread srv_thread(HappyServerThread, lfd, &req_observed);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ClientSessionState state;
    state.loop = loop;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9, port,
                                         OnClose, &state, &cs),
              0);
    odin_client_session_set_dial_filter(cs, DenyIncCb,
                                        &state.deny_call_count);
    odin_client_session_set_dial_filter(cs, nullptr, nullptr);

    ASSERT_TRUE(WriteAll(pa, kHttpReq, strlen(kHttpReq)));
    StartWatchdog(loop, &state);
    std::string http_resp;
    std::string upstream_observed;
    std::thread test_thread([pa, &http_resp, &upstream_observed] {
      http_resp = ReadString(pa, strlen(kHttp200), 1500);
      (void)write(pa, "downstream-hello", 16);
      (void)shutdown(pa, SHUT_WR);
      DrainUntilEof(pa, &upstream_observed, 1500);
    });

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    srv_thread.join();

    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, 0);
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(state.deny_call_count, 0);
    EXPECT_EQ(http_resp, std::string(kHttp200));
    EXPECT_EQ(req_observed, EncodedClientReq());
    EXPECT_EQ(upstream_observed, std::string("upstream-hello"));

    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinClientSessionTest, T3) {
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);

    std::string first_tail;
    std::string downstream_after;
    std::thread srv_thread([lfd, &first_tail, &downstream_after] {
      const int srv = AcceptWithPoll(lfd, 1500);
      if (srv < 0) {
        return;
      }
      EXPECT_EQ(ReadString(srv, EncodedClientReq().size(), 1500),
                EncodedClientReq());
      const std::string resp = EncodedResp(0);
      EXPECT_TRUE(WriteAll(srv, resp.data(), resp.size()));
      first_tail = ReadString(srv, 17, 1500);
      downstream_after = ReadString(srv, 16, 1500);
      (void)write(srv, "upstream-after", 14);
      (void)shutdown(srv, SHUT_WR);
      std::string scratch;
      DrainUntilEof(srv, &scratch, 500);
      close(srv);
    });

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ClientSessionState state;
    state.loop = loop;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9, port,
                                         OnClose, &state, &cs),
              0);
    const std::string combined =
        std::string(kHttpReq) + std::string("PIPELINED-TAIL-17");
    ASSERT_TRUE(WriteAll(pa, combined.data(), combined.size()));
    StartWatchdog(loop, &state);

    std::string http_resp;
    std::string upstream_observed;
    std::thread test_thread([pa, &http_resp, &upstream_observed] {
      http_resp = ReadString(pa, strlen(kHttp200), 1500);
      (void)write(pa, "downstream-after", 16);
      (void)shutdown(pa, SHUT_WR);
      DrainUntilEof(pa, &upstream_observed, 1500);
    });

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    srv_thread.join();

    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, 0);
    EXPECT_EQ(http_resp, std::string(kHttp200));
    EXPECT_EQ(first_tail, std::string("PIPELINED-TAIL-17"));
    EXPECT_EQ(downstream_after, std::string("downstream-after"));
    EXPECT_EQ(upstream_observed, std::string("upstream-after"));

    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinClientSessionTest, T4) {
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);

    std::string downstream_after;
    std::thread srv_thread([lfd, &downstream_after] {
      const int srv = AcceptWithPoll(lfd, 1500);
      if (srv < 0) {
        return;
      }
      EXPECT_EQ(ReadString(srv, EncodedClientReq().size(), 1500),
                EncodedClientReq());
      const std::string resp = EncodedResp(0) + "UPSTREAM-PIPED-17";
      EXPECT_TRUE(WriteAll(srv, resp.data(), resp.size()));
      downstream_after = ReadString(srv, 16, 1500);
      (void)write(srv, "upstream-after", 14);
      (void)shutdown(srv, SHUT_WR);
      std::string scratch;
      DrainUntilEof(srv, &scratch, 500);
      close(srv);
    });

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ClientSessionState state;
    state.loop = loop;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9, port,
                                         OnClose, &state, &cs),
              0);
    ASSERT_TRUE(WriteAll(pa, kHttpReq, strlen(kHttpReq)));
    StartWatchdog(loop, &state);

    std::string http_resp;
    std::string first_tail;
    std::string upstream_observed;
    std::thread test_thread([pa, &http_resp, &first_tail, &upstream_observed] {
      http_resp = ReadString(pa, strlen(kHttp200), 1500);
      first_tail = ReadString(pa, 17, 1500);
      (void)write(pa, "downstream-after", 16);
      (void)shutdown(pa, SHUT_WR);
      DrainUntilEof(pa, &upstream_observed, 1500);
    });

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    srv_thread.join();

    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, 0);
    EXPECT_EQ(http_resp, std::string(kHttp200));
    EXPECT_EQ(first_tail, std::string("UPSTREAM-PIPED-17"));
    EXPECT_EQ(downstream_after, std::string("downstream-after"));
    EXPECT_EQ(upstream_observed, std::string("upstream-after"));

    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

struct ParseCase {
  const char *name;
  std::string input;
  std::string response;
  size_t read_len;
};

void RunParseCase(const ParseCase &c) {
  int pa = -1;
  int pb = -1;
  MakeUnixPair(&pa, &pb);
  odin_event_loop_t *loop = nullptr;
  ASSERT_EQ(odin_event_loop_create(&loop), 0);
  ClientSessionState state;
  state.loop = loop;
  odin_client_session_t *cs = nullptr;
  ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9, 80, OnClose,
                                       &state, &cs),
            0);
  ASSERT_TRUE(WriteAll(pa, c.input.data(), c.input.size())) << c.name;
  StartWatchdog(loop, &state);
  std::string http_resp;
  std::thread test_thread([pa, &http_resp, &c] {
    http_resp = ReadString(pa, c.read_len, 1500);
  });
  EXPECT_EQ(odin_event_loop_run(loop), 0);
  test_thread.join();
  EXPECT_EQ(state.on_close_calls, 1) << c.name;
  EXPECT_EQ(state.on_close_err, EPROTO) << c.name;
  EXPECT_EQ(http_resp, c.response) << c.name;
  EXPECT_FALSE(state.timed_out) << c.name;
  EXPECT_EQ(close(pa), 0);
  odin_event_loop_destroy(loop);
}

TEST(OdinClientSessionTest, T5) {
  ClientSessionRunDeadline::Run([] {
    RunParseCase({"T5", "GET / HTTP/1.1\r\n\r\n", kHttp405,
                  strlen(kHttp405)});
  });
}

TEST(OdinClientSessionTest, T6) {
  ClientSessionRunDeadline::Run([] {
    RunParseCase({"T6", "CONNECT a:1 HTTP/2.0\r\n\r\n", kHttp505,
                  strlen(kHttp505)});
  });
}

TEST(OdinClientSessionTest, T7) {
  const ParseCase cases[] = {
      {"bad-target", "CONNECT example.com HTTP/1.1\r\n\r\n", kHttp400,
       strlen(kHttp400)},
      {"bad-host-len",
       std::string("CONNECT ") + std::string(256, 'x') +
           ":443 HTTP/1.1\r\n\r\n",
       kHttp400, strlen(kHttp400)},
      {"bad-port", "CONNECT a:65536 HTTP/1.1\r\n\r\n", kHttp400,
       strlen(kHttp400)}};
  for (const auto &c : cases) {
    ClientSessionRunDeadline::Run([&] { RunParseCase(c); });
  }
}

TEST(OdinClientSessionTest, T8) {
  ClientSessionRunDeadline::Run([] {
    RunParseCase({"T8", std::string("CONNECT ") + std::string(8192, 'a'),
                  kHttp414, strlen(kHttp414)});
  });
}

TEST(OdinClientSessionTest, T9) {
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ClientSessionState state;
    state.loop = loop;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9, 80,
                                         OnClose, &state, &cs),
              0);
    ASSERT_TRUE(WriteAll(pa, "CON", 3));
    (void)shutdown(pa, SHUT_WR);
    StartWatchdog(loop, &state);
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, ECONNRESET);
    EXPECT_FALSE(state.timed_out);
    uint8_t buf[64];
    const ssize_t rn = read(pa, buf, sizeof(buf));
    EXPECT_TRUE(rn == 0 || (rn == -1 && errno == EAGAIN));
    EXPECT_EQ(close(pa), 0);
    odin_event_loop_destroy(loop);
  });

  ClientSessionRunDeadline::Run([] {
    const int listener_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listener_fd, 0);
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ASSERT_EQ(bind(listener_fd, reinterpret_cast<struct sockaddr *>(&addr),
                   sizeof(addr)),
              0);
    ASSERT_EQ(listen(listener_fd, 1), 0);
    socklen_t alen = sizeof(addr);
    ASSERT_EQ(getsockname(listener_fd,
                          reinterpret_cast<struct sockaddr *>(&addr), &alen),
              0);
    const int dpa = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(dpa, 0);
    ASSERT_EQ(connect(dpa, reinterpret_cast<struct sockaddr *>(&addr),
                      sizeof(addr)),
              0);
    const int dpb = accept(listener_fd, nullptr, nullptr);
    ASSERT_GE(dpb, 0);
    close(listener_fd);
    SetNonblock(dpb);
    (void)signal(SIGPIPE, SIG_IGN);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ClientSessionState state;
    state.loop = loop;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, dpb, "127.0.0.1", 9, 80,
                                         OnClose, &state, &cs),
              0);
    ASSERT_TRUE(WriteAll(dpa, "CON", 3));
    CloseWithRst(dpa);
    StartWatchdog(loop, &state);
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, ECONNRESET);
    EXPECT_FALSE(state.timed_out);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinClientSessionTest, T10) {
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    const uint16_t closed_port = UnusedLoopbackPort();
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ClientSessionState state;
    state.loop = loop;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9,
                                         closed_port, OnClose, &state, &cs),
              0);
    ASSERT_TRUE(WriteAll(pa, kHttpReq, strlen(kHttpReq)));
    StartWatchdog(loop, &state);
    std::string http_resp;
    std::thread test_thread(
        [pa, &http_resp] { http_resp = ReadString(pa, strlen(kHttp400), 1500); });
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, ECONNREFUSED);
    EXPECT_EQ(http_resp, std::string(kHttp400));
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(close(pa), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinClientSessionTest, T11) {
  const int cases[] = {EHOSTUNREACH, ETIMEDOUT, EPERM};
  for (const int errnum : cases) {
    ClientSessionRunDeadline::Run([&] {
      int pa = -1;
      int pb = -1;
      MakeUnixPair(&pa, &pb);
      odin_event_loop_t *loop = nullptr;
      ASSERT_EQ(odin_event_loop_create(&loop), 0);
      ClientSessionState state;
      state.loop = loop;
      odin_client_session_t *cs = nullptr;
      ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9, 65535,
                                           OnClose, &state, &cs),
                0);
#if defined(ODIN_CLIENT_SESSION_TESTING)
      ASSERT_EQ(odin_client_session_test_fail_next_dial(cs, errnum), 0);
#endif
      ASSERT_TRUE(WriteAll(pa, kHttpReq, strlen(kHttpReq)));
      StartWatchdog(loop, &state);
      std::string http_resp;
      std::thread test_thread([pa, &http_resp] {
        http_resp = ReadString(pa, strlen(kHttp400), 1500);
      });
      EXPECT_EQ(odin_event_loop_run(loop), 0);
      test_thread.join();
      EXPECT_EQ(state.on_close_calls, 1);
      EXPECT_EQ(state.on_close_err, errnum);
      EXPECT_EQ(http_resp, std::string(kHttp400));
      EXPECT_FALSE(state.timed_out);
      EXPECT_EQ(close(pa), 0);
      odin_event_loop_destroy(loop);
    });
  }
}

TEST(OdinClientSessionTest, T11b) {
#if defined(__linux__)
  GTEST_SKIP() << "macOS-only kqueue path";
#else
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    (void)signal(SIGPIPE, SIG_IGN);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ClientSessionState state;
    state.loop = loop;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9, port,
                                         OnClose, &state, &cs),
              0);
    ASSERT_EQ(odin_event_loop_test_fail_next_kqueue_change(
                  loop, ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_ADD,
                  ODIN_EVENT_WRITE, EEXIST),
              0);
    ASSERT_TRUE(WriteAll(pa, kHttpReq, strlen(kHttpReq)));
    StartWatchdog(loop, &state);
    std::string http_resp;
    std::thread test_thread([pa, &http_resp] {
      http_resp = ReadString(pa, strlen(kHttp400), 1500);
    });
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, EEXIST);
    EXPECT_EQ(http_resp, std::string(kHttp400));
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
#endif
}

TEST(OdinClientSessionTest, T12) {
  struct Case {
    uint16_t code;
    int errnum;
  };
  const Case cases[] = {{0x0001, ECONNREFUSED}, {0x0002, EHOSTUNREACH},
                        {0x0003, ETIMEDOUT},    {0x0004, EIO},
                        {0xBEEF, EPROTO}};
  for (const auto &c : cases) {
    ClientSessionRunDeadline::Run([&] {
      int pa = -1;
      int pb = -1;
      MakeUnixPair(&pa, &pb);
      uint16_t port = 0;
      const int lfd = OpenLoopbackListener(&port);
      ASSERT_GE(lfd, 0);
      std::thread srv_thread([lfd, &c] {
        const int srv = AcceptWithPoll(lfd, 1500);
        if (srv < 0) {
          return;
        }
        EXPECT_EQ(ReadString(srv, EncodedClientReq().size(), 1500),
                  EncodedClientReq());
        const std::string resp = EncodedResp(c.code);
        EXPECT_TRUE(WriteAll(srv, resp.data(), resp.size()));
        close(srv);
      });
      odin_event_loop_t *loop = nullptr;
      ASSERT_EQ(odin_event_loop_create(&loop), 0);
      ClientSessionState state;
      state.loop = loop;
      odin_client_session_t *cs = nullptr;
      ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9, port,
                                           OnClose, &state, &cs),
                0);
      ASSERT_TRUE(WriteAll(pa, kHttpReq, strlen(kHttpReq)));
      StartWatchdog(loop, &state);
      std::string http_resp;
      std::thread test_thread([pa, &http_resp] {
        http_resp = ReadString(pa, strlen(kHttp400), 1500);
      });
      EXPECT_EQ(odin_event_loop_run(loop), 0);
      test_thread.join();
      srv_thread.join();
      EXPECT_EQ(state.on_close_calls, 1);
      EXPECT_EQ(state.on_close_err, c.errnum);
      EXPECT_EQ(http_resp, std::string(kHttp400));
      EXPECT_FALSE(state.timed_out);
      EXPECT_EQ(close(pa), 0);
      EXPECT_EQ(close(lfd), 0);
      odin_event_loop_destroy(loop);
    });
  }
}

TEST(OdinClientSessionTest, T13) {
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    std::atomic<bool> reset_now{false};
    std::thread srv_thread([lfd, &reset_now] {
      const int srv = AcceptWithPoll(lfd, 1500);
      if (srv >= 0) {
        while (!reset_now.load()) {
          usleep(1000);
        }
        CloseWithRst(srv);
      }
    });
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ClientSessionState state;
    state.loop = loop;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9, port,
                                         OnClose, &state, &cs),
              0);
    (void)signal(SIGPIPE, SIG_IGN);
    ASSERT_TRUE(WriteAll(pa, kHttpReq, strlen(kHttpReq)));
    StopAtHandshakeCtx handshake{cs, false};
    odin_event_timer_t *handshake_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 0, 1, StopAtHandshakeCb,
                                     &handshake, &handshake_timer),
              0);
    StartWatchdog(loop, &state);
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    reset_now.store(true);
    srv_thread.join();
    ASSERT_TRUE(handshake.reached);
    ASSERT_EQ(state.on_close_calls, 0);
    ASSERT_FALSE(state.timed_out);
    std::string http_resp;
    std::thread test_thread([pa, &http_resp] {
      http_resp = ReadString(pa, strlen(kHttp400), 1500);
    });
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, EPIPE);
    EXPECT_EQ(http_resp, std::string(kHttp400));
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinClientSessionTest, T14) {
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    std::thread srv_thread([lfd] {
      const int srv = AcceptWithPoll(lfd, 1500);
      if (srv < 0) {
        return;
      }
      (void)ReadString(srv, EncodedClientReq().size(), 1500);
      CloseWithRst(srv);
    });
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ClientSessionState state;
    state.loop = loop;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9, port,
                                         OnClose, &state, &cs),
              0);
    ASSERT_TRUE(WriteAll(pa, kHttpReq, strlen(kHttpReq)));
    StartWatchdog(loop, &state);
    std::string http_resp;
    std::thread test_thread([pa, &http_resp] {
      http_resp = ReadString(pa, strlen(kHttp400), 1500);
    });
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    srv_thread.join();
    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, ECONNRESET);
    EXPECT_EQ(http_resp, std::string(kHttp400));
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinClientSessionTest, T15) {
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    const uint16_t closed_port = UnusedLoopbackPort();
    (void)signal(SIGPIPE, SIG_IGN);
    SaturateFd(pb);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ClientSessionState state;
    state.loop = loop;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9,
                                         closed_port, OnClose, &state, &cs),
              0);
    ASSERT_TRUE(WriteAll(pa, kHttpReq, strlen(kHttpReq)));
    odin_event_timer_t *close_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(
                  loop, 10000, 0,
                  [](odin_event_loop_t *l, odin_event_timer_t *t, void *ud) {
                    (void)l;
                    close(*static_cast<int *>(ud));
                    odin_event_timer_stop(t);
                  },
                  &pa, &close_timer),
              0);
    StartWatchdog(loop, &state);
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, EPIPE);
    EXPECT_FALSE(state.timed_out);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinClientSessionTest, T15b) {
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    std::string post_resp_observed;
    std::thread srv_thread([lfd, &post_resp_observed] {
      const int srv = AcceptWithPoll(lfd, 1500);
      if (srv < 0) {
        return;
      }
      EXPECT_EQ(ReadString(srv, EncodedClientReq().size(), 1500),
                EncodedClientReq());
      const std::string resp = EncodedResp(0);
      EXPECT_TRUE(WriteAll(srv, resp.data(), resp.size()));
      DrainUntilEof(srv, &post_resp_observed, 300);
      close(srv);
    });
    (void)signal(SIGPIPE, SIG_IGN);
    SaturateFd(pb);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ClientSessionState state;
    state.loop = loop;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9, port,
                                         OnClose, &state, &cs),
              0);
    const std::string combined =
        std::string(kHttpReq) + std::string("PIPELINED-TAIL-17");
    ASSERT_TRUE(WriteAll(pa, combined.data(), combined.size()));
    odin_event_timer_t *close_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(
                  loop, 50000, 0,
                  [](odin_event_loop_t *l, odin_event_timer_t *t, void *ud) {
                    (void)l;
                    close(*static_cast<int *>(ud));
                    odin_event_timer_stop(t);
                  },
                  &pa, &close_timer),
              0);
    StartWatchdog(loop, &state);
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    srv_thread.join();
    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, EPIPE);
    EXPECT_EQ(post_resp_observed.size(), 0u);
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinClientSessionTest, T16) {
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    const uint16_t closed_port = UnusedLoopbackPort();
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ClientSessionState state;
    state.loop = loop;
    state.destroy_in_cb = true;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9,
                                         closed_port, OnClose, &state, &cs),
              0);
    ASSERT_TRUE(WriteAll(pa, kHttpReq, strlen(kHttpReq)));
    StartWatchdog(loop, &state);
    std::string http_resp;
    std::thread test_thread([pa, &http_resp] {
      http_resp = ReadString(pa, strlen(kHttp400), 1500);
    });
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, ECONNREFUSED);
    EXPECT_EQ(http_resp, std::string(kHttp400));
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(close(pa), 0);
    odin_event_loop_destroy(loop);
  });
}

struct InspectorState {
  odin_client_session_t *cs = nullptr;
  int observed_state = 0;
  bool fired = false;
};

void InspectorTask(odin_event_loop_t *loop, void *user_data) {
  (void)loop;
  InspectorState *is = static_cast<InspectorState *>(user_data);
#if defined(ODIN_CLIENT_SESSION_TESTING)
  is->observed_state = odin_client_session_test_state(is->cs);
#endif
  is->fired = true;
}

void DestroyAfterCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                    void *user_data) {
  odin_client_session_t **pcs =
      static_cast<odin_client_session_t **>(user_data);
  odin_client_session_destroy(*pcs);
  odin_event_timer_stop(timer);
  odin_event_loop_stop(loop);
}

TEST(OdinClientSessionTest, T17) {
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    (void)signal(SIGPIPE, SIG_IGN);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ClientSessionState state;
    state.loop = loop;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, pb, "192.0.2.1", 9, 80,
                                         OnClose, &state, &cs),
              0);
    ASSERT_TRUE(WriteAll(pa, kHttpReq, strlen(kHttpReq)));

    InspectorState insp;
    insp.cs = cs;
    odin_event_timer_t *insp_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(
                  loop, 20000, 0,
                  [](odin_event_loop_t *l, odin_event_timer_t *t, void *ud) {
                    odin_event_post(l, InspectorTask, ud);
                    odin_event_timer_stop(t);
                  },
                  &insp, &insp_timer),
              0);
    odin_event_timer_t *destroy_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 50000, 0, DestroyAfterCb,
                                     static_cast<void *>(&cs), &destroy_timer),
              0);
    StartWatchdog(loop, &state);

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_TRUE(insp.fired);
#if defined(ODIN_CLIENT_SESSION_TESTING)
    EXPECT_EQ(insp.observed_state, ODIN_CLIENT_SESSION_TEST_STATE_DIALING);
#endif
    EXPECT_EQ(state.on_close_calls, 0);
    EXPECT_FALSE(state.timed_out);
    uint8_t buf[64];
    const ssize_t rn = read(pa, buf, sizeof(buf));
    EXPECT_TRUE(rn == -1 || rn == 0);
    const ssize_t wn = write(pa, "y", 1);
    EXPECT_EQ(wn, -1);
    EXPECT_EQ(errno, EPIPE);
    EXPECT_EQ(close(pa), 0);
    odin_event_loop_destroy(loop);
  });
}

struct T18Ctx {
  odin_client_session_t *cs = nullptr;
  std::atomic<bool> *trigger_destroy = nullptr;
  std::atomic<bool> *observed_state_set = nullptr;
  int *observed_state = nullptr;
};

void T18InspectorTimer(odin_event_loop_t *loop, odin_event_timer_t *timer,
                       void *user_data) {
  T18Ctx *c = static_cast<T18Ctx *>(user_data);
  if (c->trigger_destroy->load()) {
    odin_event_timer_stop(timer);
#if defined(ODIN_CLIENT_SESSION_TESTING)
    *c->observed_state = odin_client_session_test_state(c->cs);
#endif
    c->observed_state_set->store(true);
    odin_client_session_destroy(c->cs);
    odin_event_loop_stop(loop);
  }
}

TEST(OdinClientSessionTest, T18) {
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    (void)signal(SIGPIPE, SIG_IGN);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    std::atomic<bool> srv_exchanged{false};
    bool srv_saw_eof = false;
    std::string srv_after_destroy;
    std::thread srv_thread([lfd, &srv_exchanged, &srv_saw_eof,
                            &srv_after_destroy] {
      const int srv = AcceptWithPoll(lfd, 1500);
      if (srv < 0) {
        return;
      }
      EXPECT_EQ(ReadString(srv, EncodedClientReq().size(), 1500),
                EncodedClientReq());
      const std::string resp = EncodedResp(0);
      EXPECT_TRUE(WriteAll(srv, resp.data(), resp.size()));
      uint8_t b = 0;
      (void)ReadExactly(srv, &b, 1, 1500);
      (void)write(srv, "y", 1);
      srv_exchanged.store(true);
      srv_saw_eof = DrainUntilEof(srv, &srv_after_destroy, 1500);
      close(srv);
    });

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ClientSessionState state;
    state.loop = loop;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9, port,
                                         OnClose, &state, &cs),
              0);
    ASSERT_TRUE(WriteAll(pa, kHttpReq, strlen(kHttpReq)));

    std::atomic<bool> trigger_destroy{false};
    std::atomic<bool> observed_state_set{false};
    int observed_state = 0;
    T18Ctx ctx;
    ctx.cs = cs;
    ctx.trigger_destroy = &trigger_destroy;
    ctx.observed_state_set = &observed_state_set;
    ctx.observed_state = &observed_state;
    odin_event_timer_t *poll_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 10000, 10000, T18InspectorTimer,
                                     &ctx, &poll_timer),
              0);
    StartWatchdog(loop, &state, 1500000);
    std::thread test_thread([pa, &srv_exchanged, &trigger_destroy] {
      EXPECT_EQ(ReadString(pa, strlen(kHttp200), 1500), std::string(kHttp200));
      (void)write(pa, "x", 1);
      uint8_t c = 0;
      (void)ReadExactly(pa, &c, 1, 1500);
      EXPECT_EQ(c, 'y');
      while (!srv_exchanged.load()) {
        usleep(1000);
      }
      trigger_destroy.store(true);
    });

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    srv_thread.join();

    EXPECT_TRUE(observed_state_set.load());
#if defined(ODIN_CLIENT_SESSION_TESTING)
    EXPECT_EQ(observed_state, ODIN_CLIENT_SESSION_TEST_STATE_RELAY);
#endif
    EXPECT_EQ(state.on_close_calls, 0);
    EXPECT_FALSE(state.timed_out);
    EXPECT_TRUE(srv_saw_eof);
    EXPECT_TRUE(srv_after_destroy.empty());
    const ssize_t wn = write(pa, "z", 1);
    EXPECT_EQ(wn, -1);
    EXPECT_EQ(errno, EPIPE);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinClientSessionTest, T19) {
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    odin_event_io_t *blocker = nullptr;
    ASSERT_EQ(odin_event_io_start(loop, pb, ODIN_EVENT_READ, DummyIoCb,
                                  nullptr, &blocker),
              0);
    ClientSessionState state;
    state.loop = loop;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    odin_client_session_t *cs = reinterpret_cast<odin_client_session_t *>(
        static_cast<uintptr_t>(0xDEADBEEFu));
    const int rc = odin_client_session_create(loop, pb, "127.0.0.1", 9, 80,
                                              OnClose, &state, &cs);
    const int saved = errno;
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(saved, EEXIST);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    EXPECT_EQ(cs, reinterpret_cast<odin_client_session_t *>(
                      static_cast<uintptr_t>(0xDEADBEEFu)));
    EXPECT_EQ(state.on_close_calls, 0);
    EXPECT_GE(fcntl(pb, F_GETFD), 0);
    odin_event_io_stop(blocker);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(pb), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinClientSessionTest, T20) {
  struct Case {
    const char *host;
    size_t len;
  };
  const Case cases[] = {{"a", 0}, {"a", 256}, {"not-an-ip", 9}};
  for (const auto &c : cases) {
    ClientSessionRunDeadline::Run([&] {
      int pa = -1;
      int pb = -1;
      MakeUnixPair(&pa, &pb);
      odin_event_loop_t *loop = nullptr;
      ASSERT_EQ(odin_event_loop_create(&loop), 0);
      ClientSessionState state;
      state.loop = loop;
      // NOLINTNEXTLINE(performance-no-int-to-ptr)
      odin_client_session_t *cs = reinterpret_cast<odin_client_session_t *>(
          static_cast<uintptr_t>(0xDEADBEEFu));
      const int rc = odin_client_session_create(loop, pb, c.host, c.len, 80,
                                                OnClose, &state, &cs);
      const int saved = errno;
      EXPECT_EQ(rc, -1);
      EXPECT_EQ(saved, EINVAL);
      // NOLINTNEXTLINE(performance-no-int-to-ptr)
      EXPECT_EQ(cs, reinterpret_cast<odin_client_session_t *>(
                        static_cast<uintptr_t>(0xDEADBEEFu)));
      EXPECT_EQ(state.on_close_calls, 0);
      EXPECT_GE(fcntl(pb, F_GETFD), 0);
      EXPECT_EQ(close(pa), 0);
      EXPECT_EQ(close(pb), 0);
      odin_event_loop_destroy(loop);
    });
  }
}

TEST(OdinClientSessionTest, T21) {
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    std::thread srv_thread([lfd] {
      const int srv = AcceptWithPoll(lfd, 1500);
      if (srv < 0) {
        return;
      }
      EXPECT_EQ(ReadString(srv, EncodedClientReq().size(), 1500),
                EncodedClientReq());
      const std::string resp = EncodedResp(0);
      EXPECT_TRUE(WriteAll(srv, resp.data(), resp.size()));
      CloseWithRst(srv);
    });
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ClientSessionState state;
    state.loop = loop;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9, port,
                                         OnClose, &state, &cs),
              0);
    ASSERT_TRUE(WriteAll(pa, kHttpReq, strlen(kHttpReq)));
    StartWatchdog(loop, &state);
    std::string http_resp;
    std::thread test_thread([pa, &http_resp] {
      http_resp = ReadString(pa, strlen(kHttp200), 1500);
    });
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    srv_thread.join();
    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, ECONNRESET);
    EXPECT_EQ(http_resp, std::string(kHttp200));
    uint8_t buf[64];
    const ssize_t rn = read(pa, buf, sizeof(buf));
    EXPECT_EQ(rn, 0);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinClientSessionTest, T22) {
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    std::string observed;
    bool upstream_saw_eof = false;
    std::thread srv_thread([lfd, &observed, &upstream_saw_eof] {
      const int srv = AcceptWithPoll(lfd, 1500);
      if (srv < 0) {
        return;
      }
      EXPECT_EQ(ReadString(srv, EncodedClientReq().size(), 1500),
                EncodedClientReq());
      const std::string resp = EncodedResp(0);
      EXPECT_TRUE(WriteAll(srv, resp.data(), resp.size()));
      upstream_saw_eof = DrainUntilEof(srv, &observed, 1500);
      close(srv);
    });
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ClientSessionState state;
    state.loop = loop;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9, port,
                                         OnClose, &state, &cs),
              0);
#if defined(ODIN_CLIENT_SESSION_TESTING)
    ASSERT_EQ(odin_client_session_test_fail_next_http_parse_tail_write(
                  cs, EAGAIN),
              0);
#endif
    const std::string combined =
        std::string(kHttpReq) + std::string("PIPELINED-TAIL-17");
    ASSERT_TRUE(WriteAll(pa, combined.data(), combined.size()));
    StartWatchdog(loop, &state);
    std::string http_resp;
    std::thread test_thread([pa, &http_resp] {
      http_resp = ReadString(pa, strlen(kHttp200), 1500);
    });
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    srv_thread.join();
    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, EAGAIN);
    EXPECT_EQ(http_resp, std::string(kHttp200));
    EXPECT_TRUE(upstream_saw_eof);
    EXPECT_EQ(observed.size(), 0u);
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinClientSessionTest, T23) {
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    std::thread srv_thread([lfd] {
      const int srv = AcceptWithPoll(lfd, 1500);
      if (srv >= 0) {
        close(srv);
      }
    });
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ClientSessionState state;
    state.loop = loop;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9, port,
                                         OnClose, &state, &cs),
              0);
#if defined(ODIN_CLIENT_SESSION_TESTING)
    ASSERT_EQ(
        odin_client_session_test_fail_next_upstream_transport_create(cs, EMFILE),
        0);
#endif
    ASSERT_TRUE(WriteAll(pa, kHttpReq, strlen(kHttpReq)));
    StartWatchdog(loop, &state);
    std::string http_resp;
    std::thread test_thread([pa, &http_resp] {
      http_resp = ReadString(pa, strlen(kHttp400), 1500);
    });
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    srv_thread.join();
    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, EMFILE);
    EXPECT_EQ(http_resp, std::string(kHttp400));
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinClientSessionTest, T24) {
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    bool upstream_saw_eof = false;
    std::string upstream_after_resp;
    std::thread srv_thread([lfd, &upstream_saw_eof, &upstream_after_resp] {
      const int srv = AcceptWithPoll(lfd, 1500);
      if (srv < 0) {
        return;
      }
      EXPECT_EQ(ReadString(srv, EncodedClientReq().size(), 1500),
                EncodedClientReq());
      const std::string resp = EncodedResp(0);
      EXPECT_TRUE(WriteAll(srv, resp.data(), resp.size()));
      upstream_saw_eof = DrainUntilEof(srv, &upstream_after_resp, 1500);
      close(srv);
    });
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ClientSessionState state;
    state.loop = loop;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9, port,
                                         OnClose, &state, &cs),
              0);
#if defined(ODIN_CLIENT_SESSION_TESTING)
    ASSERT_EQ(odin_client_session_test_fail_next_relay_create(cs, ENOMEM), 0);
#endif
    ASSERT_TRUE(WriteAll(pa, kHttpReq, strlen(kHttpReq)));
    StartWatchdog(loop, &state);
    std::string http_resp;
    std::thread test_thread([pa, &http_resp] {
      http_resp = ReadString(pa, strlen(kHttp200), 1500);
    });
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    srv_thread.join();
    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, ENOMEM);
    EXPECT_EQ(http_resp, std::string(kHttp200));
    EXPECT_FALSE(state.timed_out);
    EXPECT_TRUE(upstream_saw_eof);
    EXPECT_TRUE(upstream_after_resp.empty());
    std::string downstream_after_resp;
    EXPECT_TRUE(DrainUntilEof(pa, &downstream_after_resp, 1500));
    EXPECT_TRUE(downstream_after_resp.empty());
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinClientSessionTest, T25) {
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    std::thread srv_thread([lfd] {
      const int srv = AcceptWithPoll(lfd, 1500);
      if (srv < 0) {
        return;
      }
      EXPECT_EQ(ReadString(srv, EncodedClientReq().size(), 1500),
                EncodedClientReq());
      const std::string resp = EncodedResp(0);
      EXPECT_TRUE(WriteAll(srv, resp.data(), resp.size()));
      close(srv);
    });
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ClientSessionState state;
    state.loop = loop;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9, port,
                                         OnClose, &state, &cs),
              0);
#if defined(ODIN_CLIENT_SESSION_TESTING)
    ASSERT_EQ(odin_client_session_test_fail_next_relay_start(cs, EEXIST), 0);
#endif
    ASSERT_TRUE(WriteAll(pa, kHttpReq, strlen(kHttpReq)));
    StartWatchdog(loop, &state);
    std::string http_resp;
    std::thread test_thread([pa, &http_resp] {
      http_resp = ReadString(pa, strlen(kHttp200), 1500);
    });
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    srv_thread.join();
    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, EEXIST);
    EXPECT_EQ(http_resp, std::string(kHttp200));
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinClientSessionTest, T25b) {
#if defined(__linux__)
  GTEST_SKIP() << "macOS-only kqueue path";
#else
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    (void)signal(SIGPIPE, SIG_IGN);
    std::thread srv_thread([lfd] {
      const int srv = AcceptWithPoll(lfd, 1500);
      if (srv < 0) {
        return;
      }
      EXPECT_EQ(ReadString(srv, EncodedClientReq().size(), 1500),
                EncodedClientReq());
      const std::string resp = EncodedResp(0);
      EXPECT_TRUE(WriteAll(srv, resp.data(), resp.size()));
      close(srv);
    });
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ClientSessionState state;
    state.loop = loop;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9, port,
                                         OnClose, &state, &cs),
              0);
#if defined(ODIN_CLIENT_SESSION_TESTING)
    ASSERT_EQ(odin_client_session_test_arm_next_kqueue_read_fault_at_relay_start(
                  cs, EEXIST),
              0);
#endif
    ASSERT_TRUE(WriteAll(pa, kHttpReq, strlen(kHttpReq)));
    StartWatchdog(loop, &state);
    std::string http_resp;
    std::thread test_thread([pa, &http_resp] {
      http_resp = ReadString(pa, strlen(kHttp200), 1500);
    });
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    srv_thread.join();
    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, EEXIST);
    EXPECT_EQ(http_resp, std::string(kHttp200));
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
#endif
}

TEST(OdinClientSessionTest, T26) {
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ClientSessionState state;
    state.loop = loop;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9, port,
                                         OnClose, &state, &cs),
              0);
    odin_client_session_set_dial_filter(cs, DenyLoopbackCb, nullptr);
    ASSERT_TRUE(WriteAll(pa, kHttpReq, strlen(kHttpReq)));
    StartWatchdog(loop, &state);
    std::string http_resp;
    std::thread test_thread([pa, &http_resp] {
      http_resp = ReadString(pa, strlen(kHttp400), 1500);
    });
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    const int accept_rc = accept(lfd, nullptr, nullptr);
    const int accept_errno = errno;
    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, EACCES);
    EXPECT_EQ(http_resp, std::string(kHttp400));
    EXPECT_EQ(accept_rc, -1);
    EXPECT_EQ(accept_errno, EAGAIN);
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinClientSessionTest, T27) {
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    (void)signal(SIGPIPE, SIG_IGN);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ClientSessionState state;
    state.loop = loop;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9, 80,
                                         OnClose, &state, &cs),
              0);
    odin_client_session_set_dial_filter(cs, DestroyInsideFilterCb, cs);
    ASSERT_TRUE(WriteAll(pa, kHttpReq, strlen(kHttpReq)));
    odin_event_timer_t *end_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 50000, 0, TestEndCb, nullptr,
                                     &end_timer),
              0);
    StartWatchdog(loop, &state);
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(state.on_close_calls, 0);
    EXPECT_FALSE(state.timed_out);
    uint8_t buf[4];
    const ssize_t rn = read(pa, buf, sizeof(buf));
    EXPECT_TRUE(rn == 0 || (rn == -1 && errno == EAGAIN));
    const ssize_t wn = write(pa, "y", 1);
    EXPECT_EQ(wn, -1);
    EXPECT_EQ(errno, EPIPE);
    EXPECT_EQ(close(pa), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinClientSessionTest, T28) {
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    std::thread srv_thread([lfd] {
      const int srv = AcceptWithPoll(lfd, 1500);
      if (srv < 0) {
        return;
      }
      EXPECT_EQ(ReadString(srv, EncodedClientReq().size(), 1500),
                EncodedClientReq());
      const std::string resp = EncodedResp(0) + "UPSTREAM-PIPED-17";
      EXPECT_TRUE(WriteAll(srv, resp.data(), resp.size()));
      close(srv);
    });
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ClientSessionState state;
    state.loop = loop;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9, port,
                                         OnClose, &state, &cs),
              0);
#if defined(ODIN_CLIENT_SESSION_TESTING)
    ASSERT_EQ(odin_client_session_test_fail_next_client_tail_write(cs, EAGAIN),
              0);
#endif
    ASSERT_TRUE(WriteAll(pa, kHttpReq, strlen(kHttpReq)));
    StartWatchdog(loop, &state);
    std::string http_resp;
    std::string tail;
    bool downstream_saw_eof = false;
    std::thread test_thread([pa, &http_resp, &tail, &downstream_saw_eof] {
      http_resp = ReadString(pa, strlen(kHttp200), 1500);
      downstream_saw_eof = DrainUntilEof(pa, &tail, 1500);
    });
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    srv_thread.join();
    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, EAGAIN);
    EXPECT_EQ(http_resp, std::string(kHttp200));
    EXPECT_TRUE(downstream_saw_eof);
    EXPECT_EQ(tail.size(), 0u);
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinClientSessionTest, T29) {
  ClientSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    std::thread srv_thread([lfd] {
      const int srv = AcceptWithPoll(lfd, 1500);
      if (srv >= 0) {
        close(srv);
      }
    });
    odin_event_loop_test_liveness_t liv_pre;
    odin_event_loop_test_liveness_t liv_post;
    ASSERT_EQ(odin_event_loop_test_liveness(&liv_pre), 0);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    ClientSessionState state;
    state.loop = loop;
    odin_client_session_t *cs = nullptr;
    ASSERT_EQ(odin_client_session_create(loop, pb, "127.0.0.1", 9, port,
                                         OnClose, &state, &cs),
              0);
#if defined(ODIN_CLIENT_SESSION_TESTING)
    ASSERT_EQ(
        odin_client_session_test_fail_next_connect_session_create(cs, ENOMEM),
        0);
#endif
    ASSERT_TRUE(WriteAll(pa, kHttpReq, strlen(kHttpReq)));
    StartWatchdog(loop, &state);
    std::string http_resp;
    std::thread test_thread([pa, &http_resp] {
      http_resp = ReadString(pa, strlen(kHttp400), 1500);
    });
    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    srv_thread.join();
    ASSERT_EQ(odin_event_loop_test_liveness(&liv_post), 0);
    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, ENOMEM);
    EXPECT_EQ(http_resp, std::string(kHttp400));
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(liv_post.io_handles, liv_pre.io_handles);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

// NOLINTEND(misc-const-correctness, misc-use-internal-linkage)
