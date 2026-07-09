// odin/testing/server_session_unittests.cpp
//
// Unit tests T1-T22 from §5 of odin/docs/rfc_020_server_session.md.
//
// Each row runs under the same fork + waitpid 2 s deadline fixture RFC-012 §6
// and RFC-019 §6 established (replicated below as ServerSessionRunDeadline);
// every row that enters odin_event_loop_run additionally arms a one-shot
// 300 ms watchdog. T1 and T12 are synchronous (no loop run) and rely on the
// parent's 2 s deadline alone.
//
// All rows are SKIPPED by default. Set ODIN_SERVER_SESSION_RED=1 in the
// environment to execute the assertions; against the P1 stub every row
// surfaces red through an in-child assertion failure.

#include "odin/server_session.h"

#include <algorithm>
#include <ares.h>
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
#include <pthread.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "odin/event_loop.h"
#include "odin/protocol.h"
#include "odin/testing/connect_session_internal_test.h"
#include "odin/testing/dns_resolver_internal_test.h"
#include "odin/testing/event_loop_internal_test.h"
#include "odin/transport_fd.h"
#if defined(ODIN_SERVER_SESSION_TESTING)
#include "odin/testing/server_session_internal_test.h"
#endif

#include "gtest/gtest.h"

// NOLINTBEGIN(misc-const-correctness, misc-use-internal-linkage)

namespace {

class ServerSessionRunDeadline {
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
      FAIL() << "ServerSessionRunDeadline exceeded 2 seconds";
    }
    ASSERT_TRUE(WIFEXITED(wstatus));
    EXPECT_EQ(WEXITSTATUS(wstatus), 0);
  }
};

struct ServerSessionState {
  int on_close_calls = 0;
  int on_close_err = 0;
  bool timed_out = false;
  bool destroy_in_cb = false;
  odin_event_loop_t *loop = nullptr;
};

void OnClose(odin_server_session_t *ss, int err, void *user_data) {
  ServerSessionState *s = static_cast<ServerSessionState *>(user_data);
  s->on_close_calls += 1;
  s->on_close_err = err;
  if (s->destroy_in_cb) {
    odin_server_session_destroy(ss);
  }
  if (s->loop != nullptr) {
    odin_event_loop_stop(s->loop);
  }
}

void WatchdogCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                void *user_data) {
  ServerSessionState *s = static_cast<ServerSessionState *>(user_data);
  s->timed_out = true;
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

// Bind an ephemeral loopback port then close: the port is unused until the
// kernel recycles it, so a subsequent connect is refused with ECONNREFUSED.
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

// Build the 14-byte CONNECT_REQ for ("127.0.0.1", 9, port) by flattening the
// codec's iov[3] into a contiguous std::string. host can be any 1-255 bytes.
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

// Block-read exactly len bytes from fd subject to deadline_ms; return bytes
// read (may be < len on EOF/error).
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

// Drain available bytes from fd into out subject to deadline; returns when
// the peer half-closes or the deadline expires. Used by relay-side rows that
// need to capture upstream-received bytes.
void DrainUntilEof(int fd, std::string *out, int deadline_ms) {
  const auto start = std::chrono::steady_clock::now();
  uint8_t buf[256];
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    const int elapsed = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
            .count());
    if (elapsed >= deadline_ms) {
      return;
    }
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int prc = poll(&pfd, 1, deadline_ms - elapsed);
    if (prc <= 0) {
      return;
    }
    const ssize_t n = read(fd, buf, sizeof(buf));
    if (n == 0) {
      return;
    }
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      return;
    }
    out->append(reinterpret_cast<const char *>(buf), static_cast<size_t>(n));
  }
}

void DummyIoCb(odin_event_loop_t *loop, odin_event_io_t *io, int fd,
               unsigned int events, void *user_data) {
  (void)loop;
  (void)io;
  (void)fd;
  (void)events;
  (void)user_data;
}

struct PostState {
  odin_server_session_t *ss = nullptr;
  int observed_state = 0;
  bool fired = false;
};

void DestroyAfterCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                    void *user_data) {
  (void)loop;
  odin_server_session_t **pss =
      static_cast<odin_server_session_t **>(user_data);
  odin_server_session_destroy(*pss);
  odin_event_timer_stop(timer);
  odin_event_loop_stop(loop);
}

void TestEndCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
               void *user_data) {
  (void)user_data;
  odin_event_timer_stop(timer);
  odin_event_loop_stop(loop);
}

} // namespace

// T1 — Constructor success arms the downstream READ watch; destroy closes pb.
TEST(OdinServerSessionTest, T1) {
  ServerSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    (void)signal(SIGPIPE, SIG_IGN);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    odin_event_loop_test_liveness_t liv_pre;
    odin_event_loop_test_liveness_t liv_post;
    ASSERT_EQ(odin_event_loop_test_liveness(&liv_pre), 0);

    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create(loop, pb, OnClose, &state, &ss), 0)
        << std::strerror(errno);
    ASSERT_EQ(odin_event_loop_test_liveness(&liv_post), 0);
    EXPECT_EQ(liv_post.io_handles - liv_pre.io_handles, 1u);

#if defined(ODIN_SERVER_SESSION_TESTING)
    EXPECT_EQ(odin_server_session_test_state(ss),
              ODIN_SERVER_SESSION_TEST_STATE_HANDSHAKE);
#endif

    odin_server_session_destroy(ss);

    const ssize_t wn = write(pa, "z", 1);
    const int wer = errno;
    EXPECT_EQ(wn, -1);
    EXPECT_EQ(wer, EPIPE);
    EXPECT_EQ(state.on_close_calls, 0);

    EXPECT_EQ(close(pa), 0);
    odin_event_loop_destroy(loop);
  });
}

// T2 — Happy-path round-trip with no pipelined tail.
TEST(OdinServerSessionTest, T2) {
  ServerSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0) << std::strerror(errno);

    std::thread srv_thread([lfd] {
      struct pollfd pfd;
      pfd.fd = lfd;
      pfd.events = POLLIN;
      (void)poll(&pfd, 1, 1500);
      const int srv = accept(lfd, nullptr, nullptr);
      if (srv < 0) {
        return;
      }
      // Read forwarded downstream-hello (16 bytes).
      std::string got;
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
      uint8_t buf[64];
      while (got.size() < 16 && std::chrono::steady_clock::now() < deadline) {
        struct pollfd pf;
        pf.fd = srv;
        pf.events = POLLIN;
        const int prc = poll(&pf, 1, 100);
        if (prc <= 0) {
          continue;
        }
        const ssize_t n = read(srv, buf, sizeof(buf));
        if (n <= 0) {
          break;
        }
        got.append(reinterpret_cast<const char *>(buf), static_cast<size_t>(n));
      }
      EXPECT_EQ(got, std::string("downstream-hello"));
      (void)write(srv, "upstream-hello", 14);
      (void)shutdown(srv, SHUT_WR);
      // Drain anything else (relay propagation).
      std::string scratch;
      DrainUntilEof(srv, &scratch, 500);
      close(srv);
    });

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create(loop, pb, OnClose, &state, &ss), 0);

    const std::string req = EncodedReq("127.0.0.1", port);
    ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 300000, 0, WatchdogCb, &state, &watchdog),
        0);

    // Test-side thread: after seeing the RESP, write downstream-hello and
    // close write, then drain upstream-hello.
    std::atomic<bool> read_done{false};
    std::string upstream_observed;
    std::thread test_thread([pa, &read_done, &upstream_observed] {
      uint8_t resp[4] = {0};
      const size_t n = ReadExactly(pa, resp, 4, 1500);
      if (n != 4) {
        read_done.store(true);
        return;
      }
      (void)write(pa, "downstream-hello", 16);
      (void)shutdown(pa, SHUT_WR);
      DrainUntilEof(pa, &upstream_observed, 1500);
      read_done.store(true);
    });

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    srv_thread.join();

    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, 0);
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(upstream_observed, std::string("upstream-hello"));

    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

// T3 — Happy path with no tail and dual half-close.
TEST(OdinServerSessionTest, T3) {
  ServerSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);

    std::thread srv_thread([lfd] {
      struct pollfd pfd;
      pfd.fd = lfd;
      pfd.events = POLLIN;
      (void)poll(&pfd, 1, 1500);
      const int srv = accept(lfd, nullptr, nullptr);
      if (srv < 0) {
        return;
      }
      (void)shutdown(srv, SHUT_WR);
      std::string scratch;
      DrainUntilEof(srv, &scratch, 1500);
      close(srv);
    });

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create(loop, pb, OnClose, &state, &ss), 0);

    const std::string req = EncodedReq("127.0.0.1", port);
    ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 300000, 0, WatchdogCb, &state, &watchdog),
        0);

    std::thread test_thread([pa] {
      uint8_t resp[4] = {0};
      const size_t n = ReadExactly(pa, resp, 4, 1500);
      EXPECT_EQ(n, 4u);
      EXPECT_EQ(resp[0], 0x01);
      EXPECT_EQ(resp[1], 0x02);
      EXPECT_EQ(resp[2], 0x00);
      EXPECT_EQ(resp[3], 0x00);
      (void)shutdown(pa, SHUT_WR);
      std::string scratch;
      DrainUntilEof(pa, &scratch, 1500);
    });

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    srv_thread.join();

    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, 0);
    EXPECT_FALSE(state.timed_out);

    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

// T4 — Happy path with non-empty pipelined tail; tail reaches upstream BEFORE
// any relay-forwarded byte.
TEST(OdinServerSessionTest, T4) {
  ServerSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);

    std::string first_17;
    std::string downstream_after;
    std::thread srv_thread([lfd, &first_17, &downstream_after] {
      struct pollfd pfd;
      pfd.fd = lfd;
      pfd.events = POLLIN;
      (void)poll(&pfd, 1, 1500);
      const int srv = accept(lfd, nullptr, nullptr);
      if (srv < 0) {
        return;
      }
      uint8_t buf[17];
      const size_t n = ReadExactly(srv, buf, 17, 1500);
      first_17.assign(reinterpret_cast<const char *>(buf), n);
      uint8_t buf2[16];
      const size_t n2 = ReadExactly(srv, buf2, 16, 1500);
      downstream_after.assign(reinterpret_cast<const char *>(buf2), n2);
      (void)write(srv, "upstream-after", 14);
      (void)shutdown(srv, SHUT_WR);
      std::string scratch;
      DrainUntilEof(srv, &scratch, 500);
      close(srv);
    });

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create(loop, pb, OnClose, &state, &ss), 0);

    const std::string req = EncodedReq("127.0.0.1", port);
    std::string combined = req + std::string("PIPELINED-TAIL-17");
    ASSERT_TRUE(WriteAll(pa, combined.data(), combined.size()));

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 300000, 0, WatchdogCb, &state, &watchdog),
        0);

    std::string upstream_observed;
    std::thread test_thread([pa, &upstream_observed] {
      uint8_t resp[4] = {0};
      const size_t n = ReadExactly(pa, resp, 4, 1500);
      EXPECT_EQ(n, 4u);
      EXPECT_EQ(resp[0], 0x01);
      EXPECT_EQ(resp[1], 0x02);
      EXPECT_EQ(resp[2], 0x00);
      EXPECT_EQ(resp[3], 0x00);
      (void)write(pa, "downstream-after", 16);
      (void)shutdown(pa, SHUT_WR);
      DrainUntilEof(pa, &upstream_observed, 1500);
    });

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    srv_thread.join();

    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, 0);
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(first_17, std::string("PIPELINED-TAIL-17"));
    EXPECT_EQ(downstream_after, std::string("downstream-after"));
    EXPECT_EQ(upstream_observed, std::string("upstream-after"));

    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

// T5 — Dial fails with real ECONNREFUSED (loopback closed port).
TEST(OdinServerSessionTest, T5) {
  ServerSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    const uint16_t closed_port = UnusedLoopbackPort();

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create(loop, pb, OnClose, &state, &ss), 0);

    const std::string req = EncodedReq("127.0.0.1", closed_port);
    ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 300000, 0, WatchdogCb, &state, &watchdog),
        0);

    std::thread test_thread([pa] {
      uint8_t resp[4] = {0};
      const size_t n = ReadExactly(pa, resp, 4, 1500);
      EXPECT_EQ(n, 4u);
      EXPECT_EQ(resp[0], 0x01);
      EXPECT_EQ(resp[1], 0x02);
      EXPECT_EQ(resp[2], 0x00);
      EXPECT_EQ(resp[3], 0x01);
    });

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();

    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, ECONNREFUSED);
    EXPECT_FALSE(state.timed_out);

    EXPECT_EQ(close(pa), 0);
    odin_event_loop_destroy(loop);
  });
}

// T6 — Synthetic dial errors map to RESP codes.
TEST(OdinServerSessionTest, T6) {
  struct Case {
    int errnum;
    uint8_t expected_byte;
  };
  const Case cases[] = {{EHOSTUNREACH, 0x02}, {ETIMEDOUT, 0x03}, {EPERM, 0x04}};
  for (const auto &c : cases) {
    ServerSessionRunDeadline::Run([&] {
      int pa = -1;
      int pb = -1;
      MakeUnixPair(&pa, &pb);

      odin_event_loop_t *loop = nullptr;
      ASSERT_EQ(odin_event_loop_create(&loop), 0);

      ServerSessionState state;
      state.loop = loop;
      odin_server_session_t *ss = nullptr;
      ASSERT_EQ(odin_server_session_create(loop, pb, OnClose, &state, &ss), 0);

#if defined(ODIN_SERVER_SESSION_TESTING)
      ASSERT_EQ(odin_server_session_test_fail_next_dial(ss, c.errnum), 0);
#endif

      const std::string req = EncodedReq("127.0.0.1", 65535);
      ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));

      odin_event_timer_t *watchdog = nullptr;
      ASSERT_EQ(odin_event_timer_start(loop, 300000, 0, WatchdogCb, &state,
                                       &watchdog),
                0);

      std::thread test_thread([pa, &c] {
        uint8_t resp[4] = {0};
        const size_t n = ReadExactly(pa, resp, 4, 1500);
        EXPECT_EQ(n, 4u);
        EXPECT_EQ(resp[0], 0x01);
        EXPECT_EQ(resp[1], 0x02);
        EXPECT_EQ(resp[2], 0x00);
        EXPECT_EQ(resp[3], c.expected_byte);
      });

      EXPECT_EQ(odin_event_loop_run(loop), 0);
      test_thread.join();

      EXPECT_EQ(state.on_close_calls, 1);
      EXPECT_EQ(state.on_close_err, c.errnum);
      EXPECT_FALSE(state.timed_out);

      EXPECT_EQ(close(pa), 0);
      odin_event_loop_destroy(loop);
    });
  }
}

// T7 — Session ERROR during REQ-read (peer half-closes before full REQ).
TEST(OdinServerSessionTest, T7) {
  ServerSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create(loop, pb, OnClose, &state, &ss), 0);

    // Build the full REQ for "127.0.0.1" so byte 2 reads as host_len = 9,
    // but only write the first 3 bytes then half-close.
    const std::string req = EncodedReq("127.0.0.1", 80);
    ASSERT_TRUE(WriteAll(pa, req.data(), 3));
    (void)shutdown(pa, SHUT_WR);

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 300000, 0, WatchdogCb, &state, &watchdog),
        0);

    EXPECT_EQ(odin_event_loop_run(loop), 0);

    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, ECONNRESET);
    EXPECT_FALSE(state.timed_out);

    uint8_t buf[4];
    const ssize_t rn = read(pa, buf, sizeof(buf));
    EXPECT_LE(rn, 0);

    EXPECT_EQ(close(pa), 0);
    odin_event_loop_destroy(loop);
  });
}

// T8 — Session ERROR during RESP-write (peer closes after REQ).
TEST(OdinServerSessionTest, T8) {
  ServerSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    (void)signal(SIGPIPE, SIG_IGN);

    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);

    std::thread srv_thread([lfd] {
      struct pollfd pfd;
      pfd.fd = lfd;
      pfd.events = POLLIN;
      (void)poll(&pfd, 1, 1500);
      const int srv = accept(lfd, nullptr, nullptr);
      if (srv >= 0) {
        close(srv);
      }
    });

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create(loop, pb, OnClose, &state, &ss), 0);

    const std::string req = EncodedReq("127.0.0.1", port);
    ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));
    EXPECT_EQ(close(pa), 0);

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 300000, 0, WatchdogCb, &state, &watchdog),
        0);

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    srv_thread.join();

    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, EPIPE);
    EXPECT_FALSE(state.timed_out);

    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

// T9 — Destroy from inside on_close (real ECONNREFUSED).
TEST(OdinServerSessionTest, T9) {
  ServerSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    const uint16_t closed_port = UnusedLoopbackPort();

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    ServerSessionState state;
    state.loop = loop;
    state.destroy_in_cb = true;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create(loop, pb, OnClose, &state, &ss), 0);

    const std::string req = EncodedReq("127.0.0.1", closed_port);
    ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 300000, 0, WatchdogCb, &state, &watchdog),
        0);

    EXPECT_EQ(odin_event_loop_run(loop), 0);

    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, ECONNREFUSED);
    EXPECT_FALSE(state.timed_out);

    EXPECT_EQ(close(pa), 0);
    odin_event_loop_destroy(loop);
  });
}

namespace {

struct InspectorState {
  odin_server_session_t *ss = nullptr;
  int observed_state = 0;
  bool fired = false;
};

void InspectorTask(odin_event_loop_t *loop, void *user_data) {
  (void)loop;
  InspectorState *is = static_cast<InspectorState *>(user_data);
#if defined(ODIN_SERVER_SESSION_TESTING)
  is->observed_state = odin_server_session_test_state(is->ss);
#endif
  is->fired = true;
}

} // namespace

// T10 — Destroy from outside callback in S_DIALING (blackhole 192.0.2.1).
TEST(OdinServerSessionTest, T10) {
  ServerSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    (void)signal(SIGPIPE, SIG_IGN);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create(loop, pb, OnClose, &state, &ss), 0);

    const std::string req = EncodedReq("192.0.2.1", 80);
    ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));

    InspectorState insp;
    insp.ss = ss;
    odin_event_timer_t *insp_timer = nullptr;
    // 20 ms inspector before the 50 ms destroy trigger.
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
                                     static_cast<void *>(&ss), &destroy_timer),
              0);

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 300000, 0, WatchdogCb, &state, &watchdog),
        0);

    EXPECT_EQ(odin_event_loop_run(loop), 0);

    EXPECT_TRUE(insp.fired);
#if defined(ODIN_SERVER_SESSION_TESTING)
    EXPECT_EQ(insp.observed_state, ODIN_SERVER_SESSION_TEST_STATE_DIALING);
#endif
    EXPECT_EQ(state.on_close_calls, 0);
    EXPECT_FALSE(state.timed_out);

    uint8_t buf[4];
    const ssize_t rn = read(pa, buf, sizeof(buf));
    EXPECT_LE(rn, 0);

    // pb closed by destroy.
    (void)write(pa, "y", 1);
    const ssize_t wn2 = write(pa, "y", 1);
    EXPECT_EQ(wn2, -1);
    EXPECT_EQ(errno, EPIPE);

    EXPECT_EQ(close(pa), 0);
    odin_event_loop_destroy(loop);
  });
}

namespace {

struct T11Ctx {
  odin_server_session_t *ss = nullptr;
  std::atomic<bool> *trigger_destroy = nullptr;
  std::atomic<bool> *observed_state_set = nullptr;
  int *observed_state = nullptr;
};

void T11InspectorTimer(odin_event_loop_t *loop, odin_event_timer_t *t,
                       void *user_data) {
  (void)loop;
  T11Ctx *c = static_cast<T11Ctx *>(user_data);
  if (c->trigger_destroy->load()) {
    odin_event_timer_stop(t);
#if defined(ODIN_SERVER_SESSION_TESTING)
    *c->observed_state = odin_server_session_test_state(c->ss);
#endif
    c->observed_state_set->store(true);
    odin_server_session_destroy(c->ss);
    odin_event_loop_stop(loop);
  }
}

} // namespace

// T11 — Destroy from outside callback in S_RELAY.
TEST(OdinServerSessionTest, T11) {
  ServerSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    (void)signal(SIGPIPE, SIG_IGN);

    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);

    std::atomic<bool> srv_exchanged{false};
    std::thread srv_thread([lfd, &srv_exchanged] {
      struct pollfd pfd;
      pfd.fd = lfd;
      pfd.events = POLLIN;
      (void)poll(&pfd, 1, 1500);
      const int srv = accept(lfd, nullptr, nullptr);
      if (srv < 0) {
        return;
      }
      uint8_t b = 0;
      (void)ReadExactly(srv, &b, 1, 1500);
      (void)write(srv, "y", 1);
      srv_exchanged.store(true);
      // Block on a long read until destroy tears the relay down.
      uint8_t buf[64];
      (void)ReadExactly(srv, buf, 64, 1500);
      close(srv);
    });

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create(loop, pb, OnClose, &state, &ss), 0);

    const std::string req = EncodedReq("127.0.0.1", port);
    ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));

    std::atomic<bool> trigger_destroy{false};
    std::atomic<bool> observed_state_set{false};
    int observed_state = 0;
    T11Ctx ctx;
    ctx.ss = ss;
    ctx.trigger_destroy = &trigger_destroy;
    ctx.observed_state_set = &observed_state_set;
    ctx.observed_state = &observed_state;
    odin_event_timer_t *poll_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 10000, 10000, T11InspectorTimer,
                                     &ctx, &poll_timer),
              0);

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 1500000, 0, WatchdogCb, &state, &watchdog),
        0);

    std::thread test_thread([pa, &srv_exchanged, &trigger_destroy] {
      uint8_t resp[4] = {0};
      const size_t n = ReadExactly(pa, resp, 4, 1500);
      EXPECT_EQ(n, 4u);
      (void)write(pa, "x", 1);
      uint8_t r = 0;
      (void)ReadExactly(pa, &r, 1, 1500);
      EXPECT_EQ(r, 'y');
      while (!srv_exchanged.load()) {
        usleep(1000);
      }
      trigger_destroy.store(true);
    });

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    srv_thread.join();

#if defined(ODIN_SERVER_SESSION_TESTING)
    EXPECT_EQ(observed_state, ODIN_SERVER_SESSION_TEST_STATE_RELAY);
#endif
    EXPECT_EQ(state.on_close_calls, 0);
    EXPECT_FALSE(state.timed_out);

    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

// T12 — Constructor failure: set_interest -> EEXIST.
TEST(OdinServerSessionTest, T12) {
  ServerSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    odin_event_io_t *blocker = nullptr;
    ASSERT_EQ(odin_event_io_start(loop, pb, ODIN_EVENT_READ, DummyIoCb, nullptr,
                                  &blocker),
              0);

    ServerSessionState state;
    state.loop = loop;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    odin_server_session_t *ss = reinterpret_cast<odin_server_session_t *>(
        static_cast<uintptr_t>(0xDEADBEEFu));

    const int rc = odin_server_session_create(loop, pb, OnClose, &state, &ss);
    const int saved = errno;
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(saved, EEXIST);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    EXPECT_EQ(ss, reinterpret_cast<odin_server_session_t *>(
                      static_cast<uintptr_t>(0xDEADBEEFu)));
    EXPECT_EQ(state.on_close_calls, 0);
    EXPECT_GE(fcntl(pb, F_GETFD), 0);

    odin_event_io_stop(blocker);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(pb), 0);
    odin_event_loop_destroy(loop);
  });
}

// T13 — Host slice not a v4 literal -> synthesized EHOSTUNREACH.
TEST(OdinServerSessionTest, T13) {
  ServerSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create(loop, pb, OnClose, &state, &ss), 0);

#if defined(ODIN_DNS_RESOLVER_TESTING)
    odin_dns_resolver_test_cares_step_t step = {};
    step.op = ODIN_DNS_TEST_CARES_RESULT_EMPTY_SUCCESS;
    ASSERT_EQ(odin_dns_resolver_test_push_cares_step(&step), 0);
#endif

    const std::string req = EncodedReq("not-an-ip", 80);
    ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 300000, 0, WatchdogCb, &state, &watchdog),
        0);

    std::thread test_thread([pa] {
      uint8_t resp[4] = {0};
      const size_t n = ReadExactly(pa, resp, 4, 1500);
      EXPECT_EQ(n, 4u);
      EXPECT_EQ(resp[0], 0x01);
      EXPECT_EQ(resp[1], 0x02);
      EXPECT_EQ(resp[2], 0x00);
      EXPECT_EQ(resp[3], 0x02);
    });

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();

    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, EHOSTUNREACH);
    EXPECT_FALSE(state.timed_out);

    EXPECT_EQ(close(pa), 0);
    odin_event_loop_destroy(loop);
  });
}

namespace {

// SO_LINGER{1,0} + close: aborts connection, peer's read fails ECONNRESET.
void CloseWithRst(int fd) {
  struct linger lg;
  lg.l_onoff = 1;
  lg.l_linger = 0;
  (void)setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
  (void)close(fd);
}

} // namespace

// T14 — Upstream peer RST mid-relay.
TEST(OdinServerSessionTest, T14) {
  ServerSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);

    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);

    std::thread srv_thread([lfd] {
      struct pollfd pfd;
      pfd.fd = lfd;
      pfd.events = POLLIN;
      (void)poll(&pfd, 1, 1500);
      const int srv = accept(lfd, nullptr, nullptr);
      if (srv >= 0) {
        CloseWithRst(srv);
      }
    });

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create(loop, pb, OnClose, &state, &ss), 0);

    const std::string req = EncodedReq("127.0.0.1", port);
    ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 300000, 0, WatchdogCb, &state, &watchdog),
        0);

    std::thread test_thread([pa] {
      uint8_t resp[4] = {0};
      const size_t n = ReadExactly(pa, resp, 4, 1500);
      EXPECT_EQ(n, 4u);
      EXPECT_EQ(resp[0], 0x01);
      EXPECT_EQ(resp[1], 0x02);
      EXPECT_EQ(resp[2], 0x00);
      EXPECT_EQ(resp[3], 0x00);
    });

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    srv_thread.join();

    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, ECONNRESET);
    EXPECT_FALSE(state.timed_out);

    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

// T15 — Synthetic tail-write fault.
TEST(OdinServerSessionTest, T15) {
  ServerSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);

    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);

    std::atomic<size_t> srv_bytes{0};
    std::thread srv_thread([lfd, &srv_bytes] {
      struct pollfd pfd;
      pfd.fd = lfd;
      pfd.events = POLLIN;
      (void)poll(&pfd, 1, 1500);
      const int srv = accept(lfd, nullptr, nullptr);
      if (srv < 0) {
        return;
      }
      std::string scratch;
      DrainUntilEof(srv, &scratch, 1500);
      srv_bytes.store(scratch.size());
      close(srv);
    });

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create(loop, pb, OnClose, &state, &ss), 0);
#if defined(ODIN_SERVER_SESSION_TESTING)
    ASSERT_EQ(odin_server_session_test_fail_next_tail_write(ss, EAGAIN), 0);
#endif

    const std::string req = EncodedReq("127.0.0.1", port);
    std::string combined = req + std::string("PIPELINED-TAIL-17");
    ASSERT_TRUE(WriteAll(pa, combined.data(), combined.size()));

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 300000, 0, WatchdogCb, &state, &watchdog),
        0);

    std::thread test_thread([pa] {
      uint8_t resp[4] = {0};
      const size_t n = ReadExactly(pa, resp, 4, 1500);
      EXPECT_EQ(n, 4u);
      EXPECT_EQ(resp[0], 0x01);
      EXPECT_EQ(resp[1], 0x02);
      EXPECT_EQ(resp[2], 0x00);
      EXPECT_EQ(resp[3], 0x00);
    });

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    srv_thread.join();

    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, EAGAIN);
    EXPECT_FALSE(state.timed_out);
    EXPECT_EQ(srv_bytes.load(), 0u);

    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

// T16 — Synthetic upstream fd_transport_create failure.
TEST(OdinServerSessionTest, T16) {
  ServerSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);

    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);

    std::thread srv_thread([lfd] {
      struct pollfd pfd;
      pfd.fd = lfd;
      pfd.events = POLLIN;
      (void)poll(&pfd, 1, 1500);
      const int srv = accept(lfd, nullptr, nullptr);
      if (srv >= 0) {
        close(srv);
      }
    });

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create(loop, pb, OnClose, &state, &ss), 0);
#if defined(ODIN_SERVER_SESSION_TESTING)
    ASSERT_EQ(odin_server_session_test_fail_next_upstream_transport_create(
                  ss, EMFILE),
              0);
#endif

    const std::string req = EncodedReq("127.0.0.1", port);
    ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 300000, 0, WatchdogCb, &state, &watchdog),
        0);

    std::thread test_thread([pa] {
      uint8_t resp[4] = {0};
      const size_t n = ReadExactly(pa, resp, 4, 1500);
      EXPECT_EQ(n, 4u);
      EXPECT_EQ(resp[0], 0x01);
      EXPECT_EQ(resp[1], 0x02);
      EXPECT_EQ(resp[2], 0x00);
      EXPECT_EQ(resp[3], 0x04);
    });

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    srv_thread.join();

    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, EMFILE);
    EXPECT_FALSE(state.timed_out);

    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

// T17 — Synthetic relay_create failure.
TEST(OdinServerSessionTest, T17) {
  ServerSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);

    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);

    std::thread srv_thread([lfd] {
      struct pollfd pfd;
      pfd.fd = lfd;
      pfd.events = POLLIN;
      (void)poll(&pfd, 1, 1500);
      const int srv = accept(lfd, nullptr, nullptr);
      if (srv >= 0) {
        close(srv);
      }
    });

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create(loop, pb, OnClose, &state, &ss), 0);
#if defined(ODIN_SERVER_SESSION_TESTING)
    ASSERT_EQ(odin_server_session_test_fail_next_relay_create(ss, ENOMEM), 0);
#endif

    const std::string req = EncodedReq("127.0.0.1", port);
    ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 300000, 0, WatchdogCb, &state, &watchdog),
        0);

    std::thread test_thread([pa] {
      uint8_t resp[4] = {0};
      const size_t n = ReadExactly(pa, resp, 4, 1500);
      EXPECT_EQ(n, 4u);
      EXPECT_EQ(resp[0], 0x01);
      EXPECT_EQ(resp[1], 0x02);
      EXPECT_EQ(resp[2], 0x00);
      EXPECT_EQ(resp[3], 0x00);
    });

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    srv_thread.join();

    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, ENOMEM);
    EXPECT_FALSE(state.timed_out);

    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

// T18 — Synthetic relay_start failure.
TEST(OdinServerSessionTest, T18) {
  ServerSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);

    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);

    std::thread srv_thread([lfd] {
      struct pollfd pfd;
      pfd.fd = lfd;
      pfd.events = POLLIN;
      (void)poll(&pfd, 1, 1500);
      const int srv = accept(lfd, nullptr, nullptr);
      if (srv >= 0) {
        close(srv);
      }
    });

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create(loop, pb, OnClose, &state, &ss), 0);
#if defined(ODIN_SERVER_SESSION_TESTING)
    ASSERT_EQ(odin_server_session_test_fail_next_relay_start(ss, EEXIST), 0);
#endif

    const std::string req = EncodedReq("127.0.0.1", port);
    ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 300000, 0, WatchdogCb, &state, &watchdog),
        0);

    std::thread test_thread([pa] {
      uint8_t resp[4] = {0};
      const size_t n = ReadExactly(pa, resp, 4, 1500);
      EXPECT_EQ(n, 4u);
      EXPECT_EQ(resp[0], 0x01);
      EXPECT_EQ(resp[1], 0x02);
      EXPECT_EQ(resp[2], 0x00);
      EXPECT_EQ(resp[3], 0x00);
    });

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();
    srv_thread.join();

    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, EEXIST);
    EXPECT_FALSE(state.timed_out);

    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

namespace {

int DenyLoopbackCb(const struct sockaddr *addr, socklen_t alen, void *ud) {
  (void)alen;
  (void)ud;
  if (addr->sa_family == AF_INET) {
    const struct sockaddr_in *sin =
        reinterpret_cast<const struct sockaddr_in *>(addr);
    if (sin->sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
      return EACCES;
    }
  }
  return 0;
}

} // namespace

// T19 — §4 S1 enforcement: filter denies loopback.
TEST(OdinServerSessionTest, T19) {
  ServerSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);

    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create(loop, pb, OnClose, &state, &ss), 0);
    odin_server_session_set_dial_filter(ss, DenyLoopbackCb, nullptr);

    const std::string req = EncodedReq("127.0.0.1", port);
    ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 300000, 0, WatchdogCb, &state, &watchdog),
        0);

    std::thread test_thread([pa] {
      uint8_t resp[4] = {0};
      const size_t n = ReadExactly(pa, resp, 4, 1500);
      EXPECT_EQ(n, 4u);
      EXPECT_EQ(resp[0], 0x01);
      EXPECT_EQ(resp[1], 0x02);
      EXPECT_EQ(resp[2], 0x00);
      EXPECT_EQ(resp[3], 0x04);
    });

    EXPECT_EQ(odin_event_loop_run(loop), 0);
    test_thread.join();

    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, EACCES);
    EXPECT_FALSE(state.timed_out);

    const int arc = accept(lfd, nullptr, nullptr);
    const int aerr = errno;
    EXPECT_EQ(arc, -1);
    EXPECT_TRUE(aerr == EAGAIN || aerr == EWOULDBLOCK);

    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

// T20 — Session ERROR while dial in flight (S_DIALING).
TEST(OdinServerSessionTest, T20) {
  ServerSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    (void)signal(SIGPIPE, SIG_IGN);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create(loop, pb, OnClose, &state, &ss), 0);
#if defined(ODIN_SERVER_SESSION_TESTING)
    ASSERT_EQ(
        odin_server_session_test_inject_session_error_on_dial(ss, ECONNRESET),
        0);
#endif

    const std::string req = EncodedReq("192.0.2.1", 80);
    ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 300000, 0, WatchdogCb, &state, &watchdog),
        0);

    EXPECT_EQ(odin_event_loop_run(loop), 0);

    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, ECONNRESET);
    EXPECT_FALSE(state.timed_out);

    uint8_t buf[4];
    const ssize_t rn = read(pa, buf, sizeof(buf));
    EXPECT_LE(rn, 0);

    (void)write(pa, "z", 1);
    const ssize_t wn = write(pa, "z", 1);
    const int wer = errno;
    EXPECT_EQ(wn, -1);
    EXPECT_EQ(wer, EPIPE);

    EXPECT_EQ(close(pa), 0);
    odin_event_loop_destroy(loop);
  });
}

// T21 — Session ERROR during ERR-RESP write.
TEST(OdinServerSessionTest, T21) {
  ServerSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    (void)signal(SIGPIPE, SIG_IGN);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create(loop, pb, OnClose, &state, &ss), 0);
#if defined(ODIN_SERVER_SESSION_TESTING)
    ASSERT_EQ(odin_server_session_test_fail_next_dial(ss, EHOSTUNREACH), 0);
#endif

    const std::string req = EncodedReq("127.0.0.1", 65535);
    ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));
    EXPECT_EQ(close(pa), 0);
    pa = -1;

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 300000, 0, WatchdogCb, &state, &watchdog),
        0);

    EXPECT_EQ(odin_event_loop_run(loop), 0);

    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, EPIPE);
    EXPECT_NE(state.on_close_err, EHOSTUNREACH);
    EXPECT_FALSE(state.timed_out);

    odin_event_loop_destroy(loop);
  });
}

namespace {

int DestroyInsideFilterCb(const struct sockaddr *addr, socklen_t alen,
                          void *ud) {
  (void)addr;
  (void)alen;
  odin_server_session_destroy(static_cast<odin_server_session_t *>(ud));
  return EACCES;
}

} // namespace

// T22 — Destroy from inside dial_filter.
TEST(OdinServerSessionTest, T22) {
  ServerSessionRunDeadline::Run([] {
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    (void)signal(SIGPIPE, SIG_IGN);

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);

    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create(loop, pb, OnClose, &state, &ss), 0);
    odin_server_session_set_dial_filter(ss, DestroyInsideFilterCb, ss);

    const std::string req = EncodedReq("127.0.0.1", 80);
    ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));

    odin_event_timer_t *end_timer = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 50000, 0, TestEndCb, nullptr, &end_timer),
        0);

    odin_event_timer_t *watchdog = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 300000, 0, WatchdogCb, &state, &watchdog),
        0);

    EXPECT_EQ(odin_event_loop_run(loop), 0);

    EXPECT_EQ(state.on_close_calls, 0);
    EXPECT_FALSE(state.timed_out);

    uint8_t buf[4];
    const ssize_t rn = read(pa, buf, sizeof(buf));
    EXPECT_LE(rn, 0);

    (void)write(pa, "y", 1);
    const ssize_t wn = write(pa, "y", 1);
    const int wer = errno;
    EXPECT_EQ(wn, -1);
    EXPECT_EQ(wer, EPIPE);

    EXPECT_EQ(close(pa), 0);
    odin_event_loop_destroy(loop);
  });
}

namespace {

struct ServerDnsAnswer {
  int family = AF_INET;
  unsigned char bytes[16] = {};
};

class ServerDnsFixture {
public:
  ServerDnsFixture() {
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
    running_ = true;
    EXPECT_EQ(pthread_create(&thread_, nullptr, ThreadMain, this), 0);
  }

  ~ServerDnsFixture() {
    pthread_mutex_lock(&mu_);
    running_ = false;
    pthread_mutex_unlock(&mu_);
    if (fd_ >= 0) {
      close(fd_);
    }
    if (thread_ != 0) {
      pthread_join(thread_, nullptr);
    }
    pthread_mutex_destroy(&mu_);
  }

  std::string servers_csv() const {
    return std::string("127.0.0.1:") + std::to_string(port_);
  }

  std::vector<std::string> Questions() {
    pthread_mutex_lock(&mu_);
    std::vector<std::string> out = questions_;
    pthread_mutex_unlock(&mu_);
    return out;
  }

private:
  static void *ThreadMain(void *arg) {
    static_cast<ServerDnsFixture *>(arg)->Run();
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

  static ServerDnsAnswer A(unsigned char a, unsigned char b, unsigned char c,
                           unsigned char d) {
    ServerDnsAnswer out;
    out.family = AF_INET;
    out.bytes[0] = a;
    out.bytes[1] = b;
    out.bytes[2] = c;
    out.bytes[3] = d;
    return out;
  }

  static ServerDnsAnswer AAAALoopback() {
    ServerDnsAnswer out;
    out.family = AF_INET6;
    out.bytes[15] = 1;
    return out;
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

  static bool ShouldDrop(const std::string &name) {
    return name == "aliaspeer" || name == "noresponse.test" ||
           name == "slow.test";
  }

  static bool IsNxDomain(const std::string &name) { return name == "nx.test"; }

  static std::vector<ServerDnsAnswer> Answers(const std::string &name,
                                              uint16_t qtype) {
    if (qtype == 1) {
      if (name == "denied-first.test") {
        return {A(192, 0, 2, 7), A(127, 0, 0, 1)};
      }
      if (name == "fail-first.test") {
        return {A(192, 0, 2, 77), A(127, 0, 0, 1)};
      }
      if (name == "destroy-filter.test") {
        return {A(127, 0, 0, 1), A(127, 0, 0, 1)};
      }
      if (name == "target.test" || name == "deny.test" ||
          name == "staged.test" || name == "fail-dial.test" ||
          name == "closed.test") {
        return {A(127, 0, 0, 1)};
      }
    }
    if (qtype == 28 && name == "v6.test") {
      return {AAAALoopback()};
    }
    return {};
  }

  static void AppendAnswer(std::vector<unsigned char> *out,
                           const ServerDnsAnswer &answer) {
    out->push_back(0xc0);
    out->push_back(0x0c);
    PutU16(out, answer.family == AF_INET ? 1 : 28);
    PutU16(out, 1);
    PutU32(out, 60);
    if (answer.family == AF_INET) {
      PutU16(out, 4);
      out->insert(out->end(), answer.bytes, answer.bytes + 4);
    } else {
      PutU16(out, 16);
      out->insert(out->end(), answer.bytes, answer.bytes + 16);
    }
  }

  void Reply(const unsigned char *buf, size_t question_end,
             const std::string &name, uint16_t qtype, const sockaddr_in &peer,
             socklen_t peer_len) {
    const bool nxdomain = IsNxDomain(name);
    const std::vector<ServerDnsAnswer> answers =
        nxdomain ? std::vector<ServerDnsAnswer>() : Answers(name, qtype);
    std::vector<unsigned char> out;
    out.reserve(192);
    out.push_back(buf[0]);
    out.push_back(buf[1]);
    PutU16(&out, nxdomain ? 0x8183 : 0x8180);
    PutU16(&out, 1);
    PutU16(&out, static_cast<uint16_t>(answers.size()));
    PutU16(&out, 0);
    PutU16(&out, 0);
    out.insert(out.end(), &buf[12], &buf[question_end]);
    for (const ServerDnsAnswer &answer : answers) {
      AppendAnswer(&out, answer);
    }
    (void)sendto(fd_, out.data(), out.size(), 0,
                 reinterpret_cast<const sockaddr *>(&peer), peer_len);
  }

  void Run() {
    while (true) {
      pthread_mutex_lock(&mu_);
      const bool running = running_;
      pthread_mutex_unlock(&mu_);
      if (!running) {
        return;
      }
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
      Reply(buf, question_end, name, qtype, peer, peer_len);
    }
  }

  int fd_ = -1;
  uint16_t port_ = 0;
  pthread_t thread_ = 0;
  bool running_ = false;
  pthread_mutex_t mu_;
  std::vector<std::string> questions_;
};

void ExpectDnsLiveCounts(const odin_dns_resolver_test_liveness_t &live,
                         size_t resolvers, size_t queries, size_t watches,
                         size_t timers, size_t channels, size_t results) {
  EXPECT_EQ(live.resolvers, resolvers);
  EXPECT_EQ(live.queries, queries);
  EXPECT_EQ(live.watches, watches);
  EXPECT_EQ(live.timers, timers);
  EXPECT_EQ(live.cares_channels, channels);
  EXPECT_EQ(live.cares_results, results);
}

void ExpectRespCode(int fd, uint16_t code) {
  uint8_t resp[4] = {0};
  const size_t n = ReadExactly(fd, resp, sizeof(resp), 1500);
  ASSERT_EQ(n, sizeof(resp));
  EXPECT_EQ(resp[0], 0x01);
  EXPECT_EQ(resp[1], 0x02);
  EXPECT_EQ(resp[2], static_cast<uint8_t>(code >> 8));
  EXPECT_EQ(resp[3], static_cast<uint8_t>(code & 0xff));
}

void RunServerLoop(odin_event_loop_t *loop, ServerSessionState *state,
                   uint64_t timeout_us = 300000) {
  odin_event_timer_t *watchdog = nullptr;
  ASSERT_EQ(
      odin_event_timer_start(loop, timeout_us, 0, WatchdogCb, state, &watchdog),
      0)
      << std::strerror(errno);
  ASSERT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);
  EXPECT_FALSE(state->timed_out);
}

void CreateFixtureResolver(odin_event_loop_t *loop, ServerDnsFixture *fixture,
                           odin_dns_resolver_t **resolver,
                           int timeout_ms = 250) {
  const std::string servers = fixture->servers_csv();
  odin_dns_resolver_config_t config = {servers.c_str(), timeout_ms, 1};
  ASSERT_EQ(odin_dns_resolver_create(loop, &config, resolver), 0)
      << std::strerror(errno);
}

int RecordAndAllowFilter(const struct sockaddr *addr, socklen_t addrlen,
                         void *user_data) {
  std::vector<std::string> *seen =
      static_cast<std::vector<std::string> *>(user_data);
  char buf[INET6_ADDRSTRLEN] = {};
  uint16_t port = 0;
  if (addr->sa_family == AF_INET) {
    const sockaddr_in *sin = reinterpret_cast<const sockaddr_in *>(addr);
    inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
    port = ntohs(sin->sin_port);
  } else if (addr->sa_family == AF_INET6) {
    const sockaddr_in6 *sin6 = reinterpret_cast<const sockaddr_in6 *>(addr);
    inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf));
    port = ntohs(sin6->sin6_port);
  }
  seen->push_back(std::string(buf) + ":" + std::to_string(port) + "/" +
                  std::to_string(addrlen));
  return 0;
}

struct DenyFirstFilterState {
  std::vector<std::string> seen;
  uint16_t port = 0;
};

int DenyFirstFilter(const struct sockaddr *addr, socklen_t, void *user_data) {
  DenyFirstFilterState *state = static_cast<DenyFirstFilterState *>(user_data);
  const sockaddr_in *sin = reinterpret_cast<const sockaddr_in *>(addr);
  char buf[INET_ADDRSTRLEN] = {};
  inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
  state->seen.push_back(buf);
  EXPECT_EQ(ntohs(sin->sin_port), state->port);
  if (std::strcmp(buf, "192.0.2.7") == 0) {
    return EACCES;
  }
  return 0;
}

struct ExactDenyFilterState {
  int calls = 0;
  bool saw_expected = false;
  uint16_t port = 0;
  odin_server_session_t *destroy_ss = nullptr;
};

int ExactDenyFilter(const struct sockaddr *addr, socklen_t, void *user_data) {
  ExactDenyFilterState *state = static_cast<ExactDenyFilterState *>(user_data);
  state->calls += 1;
  if (state->destroy_ss != nullptr) {
    odin_server_session_destroy(state->destroy_ss);
    return EACCES;
  }
  if (addr->sa_family == AF_INET) {
    const sockaddr_in *sin = reinterpret_cast<const sockaddr_in *>(addr);
    if (sin->sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
        ntohs(sin->sin_port) == state->port) {
      state->saw_expected = true;
      return EACCES;
    }
  }
  return 0;
}

struct FakeDownstreamTransport {
  odin_transport_t base;
  odin_transport_ready_cb on_ready = nullptr;
  void *ready_user_data = nullptr;
  std::string reads;
  size_t read_off = 0;
  std::vector<size_t> write_sizes;
  size_t write_index = 0;
  std::string writes;
  unsigned int interest = 0;
  int set_interest_errno = 0;
  int destroy_calls = 0;
};

odin_transport_io_t FakeDownstreamRead(odin_transport_t *t, void *buf,
                                       size_t len, size_t *out_n) {
  FakeDownstreamTransport *ft = reinterpret_cast<FakeDownstreamTransport *>(t);
  if (ft->read_off >= ft->reads.size()) {
    *out_n = 0;
    return ODIN_TRANSPORT_EOF;
  }
  const size_t n = std::min(len, ft->reads.size() - ft->read_off);
  std::memcpy(buf, ft->reads.data() + ft->read_off, n);
  ft->read_off += n;
  *out_n = n;
  return ODIN_TRANSPORT_OK;
}

odin_transport_io_t FakeDownstreamWrite(odin_transport_t *t, const void *buf,
                                        size_t len, size_t *out_n) {
  FakeDownstreamTransport *ft = reinterpret_cast<FakeDownstreamTransport *>(t);
  if (ft->write_index >= ft->write_sizes.size()) {
    return ODIN_TRANSPORT_AGAIN;
  }
  const size_t requested = ft->write_sizes[ft->write_index++];
  if (requested == 0) {
    return ODIN_TRANSPORT_AGAIN;
  }
  const size_t n = std::min(len, requested);
  ft->writes.append(static_cast<const char *>(buf), n);
  *out_n = n;
  return ODIN_TRANSPORT_OK;
}

int FakeDownstreamShutdownWrite(odin_transport_t *) { return 0; }

int FakeDownstreamSetInterest(odin_transport_t *t, unsigned int events) {
  FakeDownstreamTransport *ft = reinterpret_cast<FakeDownstreamTransport *>(t);
  ft->interest = events;
  if (ft->set_interest_errno != 0) {
    errno = ft->set_interest_errno;
    return -1;
  }
  return 0;
}

int FakeDownstreamError(odin_transport_t *) { return 0; }

void FakeDownstreamDestroy(odin_transport_t *t) {
  reinterpret_cast<FakeDownstreamTransport *>(t)->destroy_calls += 1;
}

const odin_transport_vtable_t kFakeDownstreamVtable = {
    FakeDownstreamRead,        FakeDownstreamWrite, FakeDownstreamShutdownWrite,
    FakeDownstreamSetInterest, FakeDownstreamError, FakeDownstreamDestroy,
};

int FakeDownstreamFactory(odin_transport_ready_cb on_ready,
                          void *ready_user_data, void *factory_user_data,
                          odin_transport_t **out) {
  FakeDownstreamTransport *ft =
      static_cast<FakeDownstreamTransport *>(factory_user_data);
  ft->base.vt = &kFakeDownstreamVtable;
  ft->on_ready = on_ready;
  ft->ready_user_data = ready_user_data;
  *out = &ft->base;
  return 0;
}

struct FdFactoryState {
  int fd = -1;
  odin_event_loop_t *loop = nullptr;
  int calls = 0;
  int fail_errno = 0;
};

int FdFactory(odin_transport_ready_cb on_ready, void *ready_user_data,
              void *factory_user_data, odin_transport_t **out) {
  FdFactoryState *state = static_cast<FdFactoryState *>(factory_user_data);
  state->calls += 1;
  if (state->fail_errno != 0) {
    errno = state->fail_errno;
    return -1;
  }
  return odin_fd_transport_create(state->loop, state->fd, on_ready,
                                  ready_user_data, out);
}

} // namespace

TEST(OdinServerDnsTest, T1DnsNameResolvesAndRelays) {
  ServerSessionRunDeadline::Run([] {
    ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
    ServerDnsFixture fixture;
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0) << std::strerror(errno);

    std::string upstream_got;
    std::thread srv([lfd, &upstream_got] {
      struct pollfd pfd{lfd, POLLIN, 0};
      (void)poll(&pfd, 1, 1500);
      const int fd = accept(lfd, nullptr, nullptr);
      if (fd < 0) {
        return;
      }
      char buf[18] = {};
      const size_t n = ReadExactly(fd, buf, 18, 1500);
      upstream_got.assign(buf, n);
      (void)write(fd, "upstream-dns", 12);
      (void)shutdown(fd, SHUT_WR);
      std::string scratch;
      DrainUntilEof(fd, &scratch, 500);
      close(fd);
    });

    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    odin_dns_resolver_t *resolver = nullptr;
    CreateFixtureResolver(loop, &fixture, &resolver);
    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create_with_resolver(loop, pb, resolver,
                                                       OnClose, &state, &ss),
              0)
        << std::strerror(errno);
    const std::string req =
        EncodedReq("target.test", port) + std::string("downstream-payload");
    ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));
    std::string downstream_got;
    std::thread client([pa, &downstream_got] {
      ExpectRespCode(pa, ODIN_SERVER_SESSION_RESP_CODE_OK);
      (void)shutdown(pa, SHUT_WR);
      DrainUntilEof(pa, &downstream_got, 1500);
    });
    RunServerLoop(loop, &state);
    client.join();
    srv.join();
    EXPECT_EQ(upstream_got, "downstream-payload");
    EXPECT_EQ(downstream_got, "upstream-dns");
    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, 0);
    odin_dns_resolver_test_liveness_t live;
    ASSERT_EQ(odin_dns_resolver_test_liveness(&live), 0);
    ExpectDnsLiveCounts(live, 1, 0, 0, 0, 0, 0);
    odin_dns_resolver_destroy(resolver);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinServerDnsTest, T2NumericLiteralUsesResolverPath) {
  ServerSessionRunDeadline::Run([] {
    ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
    ServerDnsFixture fixture;
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    std::thread srv([lfd] {
      struct pollfd pfd{lfd, POLLIN, 0};
      (void)poll(&pfd, 1, 1500);
      const int fd = accept(lfd, nullptr, nullptr);
      if (fd >= 0) {
        (void)shutdown(fd, SHUT_WR);
        close(fd);
      }
    });
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    odin_dns_resolver_t *resolver = nullptr;
    CreateFixtureResolver(loop, &fixture, &resolver);
    odin_dns_resolver_test_cares_observation_t obs0;
    ASSERT_EQ(odin_dns_resolver_test_cares_observation(&obs0), 0);
    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create_with_resolver(loop, pb, resolver,
                                                       OnClose, &state, &ss),
              0);
    const std::string req = EncodedReq("127.0.0.1", port);
    ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));
    std::thread client([pa] {
      ExpectRespCode(pa, ODIN_SERVER_SESSION_RESP_CODE_OK);
      (void)shutdown(pa, SHUT_WR);
    });
    RunServerLoop(loop, &state);
    client.join();
    srv.join();
    odin_dns_resolver_test_cares_observation_t obs1;
    ASSERT_EQ(odin_dns_resolver_test_cares_observation(&obs1), 0);
    EXPECT_EQ(obs1.getaddrinfo_calls, obs0.getaddrinfo_calls + 1);
    EXPECT_EQ(obs1.last_ai_family, AF_UNSPEC);
    EXPECT_TRUE(fixture.Questions().empty());
    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, 0);
    odin_dns_resolver_destroy(resolver);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinServerDnsTest, T3DnsAndDialErrorsMapToResponses) {
  struct Case {
    const char *name;
    uint16_t port;
    int push_empty_success;
    int timeout_ms;
    uint16_t code;
    int err;
  };
  for (const Case &c :
       {Case{"nx.test", 80, 0, 250, ODIN_SERVER_SESSION_RESP_CODE_EHOSTUNREACH,
             EHOSTUNREACH},
        Case{"empty.test", 80, 1, 250,
             ODIN_SERVER_SESSION_RESP_CODE_EHOSTUNREACH, EHOSTUNREACH},
        Case{"noresponse.test", 80, 0, 50,
             ODIN_SERVER_SESSION_RESP_CODE_ETIMEDOUT, ETIMEDOUT},
        Case{"closed.test", 0, 0, 250,
             ODIN_SERVER_SESSION_RESP_CODE_ECONNREFUSED, ECONNREFUSED}}) {
    ServerSessionRunDeadline::Run([&] {
      ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
      ServerDnsFixture fixture;
      const uint16_t port =
          c.port == 0 ? UnusedLoopbackPort() : static_cast<uint16_t>(c.port);
      if (c.push_empty_success) {
        odin_dns_resolver_test_cares_step_t step = {};
        step.op = ODIN_DNS_TEST_CARES_RESULT_EMPTY_SUCCESS;
        ASSERT_EQ(odin_dns_resolver_test_push_cares_step(&step), 0);
      }
      int pa = -1;
      int pb = -1;
      MakeUnixPair(&pa, &pb);
      odin_event_loop_t *loop = nullptr;
      ASSERT_EQ(odin_event_loop_create(&loop), 0);
      odin_dns_resolver_t *resolver = nullptr;
      CreateFixtureResolver(loop, &fixture, &resolver, c.timeout_ms);
      ServerSessionState state;
      state.loop = loop;
      odin_server_session_t *ss = nullptr;
      ASSERT_EQ(odin_server_session_create_with_resolver(loop, pb, resolver,
                                                         OnClose, &state, &ss),
                0);
      const std::string req = EncodedReq(c.name, port);
      ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));
      std::thread client([pa, &c] { ExpectRespCode(pa, c.code); });
      RunServerLoop(loop, &state, 600000);
      client.join();
      EXPECT_EQ(state.on_close_calls, 1);
      EXPECT_EQ(state.on_close_err, c.err);
      odin_dns_resolver_test_liveness_t live;
      ASSERT_EQ(odin_dns_resolver_test_liveness(&live), 0);
      ExpectDnsLiveCounts(live, 1, 0, 0, 0, 0, 0);
      odin_dns_resolver_destroy(resolver);
      EXPECT_EQ(close(pa), 0);
      odin_event_loop_destroy(loop);
    });
  }
}

TEST(OdinServerDnsTest, T4FilterSkipsDeniedFirstAnswer) {
  ServerSessionRunDeadline::Run([] {
    ServerDnsFixture fixture;
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    std::thread srv([lfd] {
      struct pollfd pfd{lfd, POLLIN, 0};
      (void)poll(&pfd, 1, 1500);
      const int fd = accept(lfd, nullptr, nullptr);
      if (fd >= 0) {
        (void)shutdown(fd, SHUT_WR);
        close(fd);
      }
    });
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    odin_dns_resolver_t *resolver = nullptr;
    CreateFixtureResolver(loop, &fixture, &resolver);
    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create_with_resolver(loop, pb, resolver,
                                                       OnClose, &state, &ss),
              0);
    DenyFirstFilterState filter;
    filter.port = port;
    odin_server_session_set_dial_filter(ss, DenyFirstFilter, &filter);
    const std::string req = EncodedReq("denied-first.test", port);
    ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));
    std::thread client([pa] {
      ExpectRespCode(pa, ODIN_SERVER_SESSION_RESP_CODE_OK);
      (void)shutdown(pa, SHUT_WR);
    });
    RunServerLoop(loop, &state);
    client.join();
    srv.join();
    ASSERT_EQ(filter.seen.size(), 2u);
    EXPECT_EQ(filter.seen[0], "192.0.2.7");
    EXPECT_EQ(filter.seen[1], "127.0.0.1");
    EXPECT_EQ(state.on_close_err, 0);
    odin_dns_resolver_destroy(resolver);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinServerDnsTest, T5AllResolvedAddressesDeniedBeforeConnect) {
  ServerSessionRunDeadline::Run([] {
    ServerDnsFixture fixture;
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    odin_dns_resolver_t *resolver = nullptr;
    CreateFixtureResolver(loop, &fixture, &resolver);
    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create_with_resolver(loop, pb, resolver,
                                                       OnClose, &state, &ss),
              0);
    ExactDenyFilterState filter;
    filter.port = port;
    odin_server_session_set_dial_filter(ss, ExactDenyFilter, &filter);
    const std::string req = EncodedReq("deny.test", port);
    ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));
    std::thread client(
        [pa] { ExpectRespCode(pa, ODIN_SERVER_SESSION_RESP_CODE_OTHER); });
    RunServerLoop(loop, &state);
    client.join();
    EXPECT_EQ(filter.calls, 1);
    EXPECT_TRUE(filter.saw_expected);
    EXPECT_EQ(state.on_close_err, EACCES);
    const int arc = accept(lfd, nullptr, nullptr);
    EXPECT_EQ(arc, -1);
    EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
    odin_dns_resolver_destroy(resolver);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinServerDnsTest, T6EmbeddedNulHostRejectedBeforeDns) {
  ServerSessionRunDeadline::Run([] {
    ServerDnsFixture fixture;
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    odin_dns_resolver_t *resolver = nullptr;
    CreateFixtureResolver(loop, &fixture, &resolver);
    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create_with_resolver(loop, pb, resolver,
                                                       OnClose, &state, &ss),
              0);
    odin_dns_resolver_test_cares_observation_t obs0;
    ASSERT_EQ(odin_dns_resolver_test_cares_observation(&obs0), 0);
    const std::string bad("bad\0name", 8);
    const std::string req = EncodedReq(bad, port);
    ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));
    std::thread client(
        [pa] { ExpectRespCode(pa, ODIN_SERVER_SESSION_RESP_CODE_OTHER); });
    RunServerLoop(loop, &state);
    client.join();
    odin_dns_resolver_test_cares_observation_t obs1;
    ASSERT_EQ(odin_dns_resolver_test_cares_observation(&obs1), 0);
    EXPECT_EQ(obs1.getaddrinfo_calls, obs0.getaddrinfo_calls);
    EXPECT_EQ(state.on_close_err, EINVAL);
    const int arc = accept(lfd, nullptr, nullptr);
    EXPECT_EQ(arc, -1);
    EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
    odin_dns_resolver_destroy(resolver);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinServerDnsTest, T7LocalNameRewritingSuppressed) {
  ServerSessionRunDeadline::Run([] {
    ServerDnsFixture fixture;
    char path[] = "/tmp/odin-hostaliases-XXXXXX";
    const int alias_fd = mkstemp(path);
    ASSERT_GE(alias_fd, 0);
    ASSERT_TRUE(WriteAll(alias_fd, "aliaspeer rewritten.test\n", 25));
    close(alias_fd);
    setenv("HOSTALIASES", path, 1);
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    odin_dns_resolver_t *resolver = nullptr;
    CreateFixtureResolver(loop, &fixture, &resolver, 50);
    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create_with_resolver(loop, pb, resolver,
                                                       OnClose, &state, &ss),
              0);
    const std::string req = EncodedReq("aliaspeer", 80);
    ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));
    std::thread client(
        [pa] { ExpectRespCode(pa, ODIN_SERVER_SESSION_RESP_CODE_ETIMEDOUT); });
    RunServerLoop(loop, &state, 600000);
    client.join();
    unsetenv("HOSTALIASES");
    unlink(path);
    const std::vector<std::string> questions = fixture.Questions();
    ASSERT_FALSE(questions.empty());
    for (const std::string &q : questions) {
      EXPECT_EQ(q, "aliaspeer");
    }
    odin_dns_resolver_test_cares_observation_t obs;
    ASSERT_EQ(odin_dns_resolver_test_cares_observation(&obs), 0);
    EXPECT_TRUE((obs.last_init_options_optmask & ARES_OPT_FLAGS) != 0);
    EXPECT_TRUE((obs.last_init_options_optmask & ARES_OPT_LOOKUPS) != 0);
    EXPECT_STREQ(obs.last_init_options_lookups, "b");
    EXPECT_TRUE((obs.last_init_options_flags & ARES_FLAG_NOALIASES) != 0);
    EXPECT_TRUE((obs.last_init_options_flags & ARES_FLAG_NOSEARCH) != 0);
    EXPECT_TRUE((obs.last_ai_flags & ARES_AI_NOSORT) != 0);
    EXPECT_EQ(state.on_close_err, ETIMEDOUT);
    odin_dns_resolver_destroy(resolver);
    EXPECT_EQ(close(pa), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinServerDnsTest, T8DestroyWhileResolvingAbortsQuery) {
  ServerSessionRunDeadline::Run([] {
    ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
    ServerDnsFixture fixture;
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    (void)signal(SIGPIPE, SIG_IGN);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    odin_dns_resolver_t *resolver = nullptr;
    CreateFixtureResolver(loop, &fixture, &resolver, 250);
    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create_with_resolver(loop, pb, resolver,
                                                       OnClose, &state, &ss),
              0);
    const std::string req = EncodedReq("slow.test", 80);
    ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));
    odin_event_timer_t *destroy_timer = nullptr;
    ASSERT_EQ(odin_event_timer_start(loop, 50000, 0, DestroyAfterCb,
                                     static_cast<void *>(&ss), &destroy_timer),
              0);
    ASSERT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(state.on_close_calls, 0);
    (void)write(pa, "x", 1);
    const ssize_t wn = write(pa, "x", 1);
    EXPECT_EQ(wn, -1);
    EXPECT_EQ(errno, EPIPE);
    odin_dns_resolver_test_liveness_t live;
    ASSERT_EQ(odin_dns_resolver_test_liveness(&live), 0);
    ExpectDnsLiveCounts(live, 1, 0, 0, 0, 0, 0);
    odin_dns_resolver_destroy(resolver);
    ASSERT_EQ(odin_dns_resolver_test_liveness(&live), 0);
    ExpectDnsLiveCounts(live, 0, 0, 0, 0, 0, 0);
    EXPECT_EQ(close(pa), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinServerDnsTest, T9ConstructorValidationAndPrivateLifecycle) {
  ServerSessionRunDeadline::Run([] {
    ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    odin_dns_resolver_t *resolver = nullptr;
    ASSERT_EQ(odin_dns_resolver_create(loop, nullptr, &resolver), 0);
    odin_server_session_t *sentinel =
        reinterpret_cast<odin_server_session_t *>(0xDEADBEEF);
    odin_server_session_t *out = sentinel;
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    EXPECT_EQ(odin_server_session_create_with_resolver(nullptr, pb, resolver,
                                                       OnClose, nullptr, &out),
              -1);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(out, sentinel);
    EXPECT_GE(fcntl(pb, F_GETFD), 0);
    EXPECT_EQ(odin_server_session_create_with_resolver(loop, pb, nullptr,
                                                       OnClose, nullptr, &out),
              -1);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(odin_server_session_create_with_resolver(loop, pb, resolver,
                                                       nullptr, nullptr, &out),
              -1);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(odin_server_session_create_with_resolver(
                  loop, pb, resolver, OnClose, nullptr, nullptr),
              -1);
    EXPECT_EQ(errno, EINVAL);

    FakeDownstreamTransport fake;
    EXPECT_EQ(odin_server_session_create_with_transport_and_resolver(
                  nullptr, FakeDownstreamFactory, &fake, resolver, OnClose,
                  nullptr, &out),
              -1);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(odin_server_session_create_with_transport_and_resolver(
                  loop, nullptr, &fake, resolver, OnClose, nullptr, &out),
              -1);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(fake.destroy_calls, 0);
    EXPECT_EQ(odin_server_session_create_with_transport_and_resolver(
                  loop, FakeDownstreamFactory, &fake, nullptr, OnClose, nullptr,
                  &out),
              -1);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(odin_server_session_create_with_transport_and_resolver(
                  loop, FakeDownstreamFactory, &fake, resolver, nullptr,
                  nullptr, &out),
              -1);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(odin_server_session_create_with_transport_and_resolver(
                  loop, FakeDownstreamFactory, &fake, resolver, OnClose,
                  nullptr, nullptr),
              -1);
    EXPECT_EQ(errno, EINVAL);

    odin_dns_resolver_test_liveness_t before;
    ASSERT_EQ(odin_dns_resolver_test_liveness(&before), 0);
    EXPECT_EQ(odin_server_session_create(nullptr, pb, OnClose, nullptr, &out),
              -1);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(odin_server_session_create(loop, pb, nullptr, nullptr, &out), -1);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(odin_server_session_create(loop, pb, OnClose, nullptr, nullptr),
              -1);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(
        odin_server_session_create_with_transport(
            nullptr, FakeDownstreamFactory, &fake, OnClose, nullptr, &out),
        -1);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(odin_server_session_create_with_transport(loop, nullptr, &fake,
                                                        OnClose, nullptr, &out),
              -1);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(odin_server_session_create_with_transport(
                  loop, FakeDownstreamFactory, &fake, nullptr, nullptr, &out),
              -1);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(
        odin_server_session_create_with_transport(
            loop, FakeDownstreamFactory, &fake, OnClose, nullptr, nullptr),
        -1);
    EXPECT_EQ(errno, EINVAL);
    odin_dns_resolver_test_liveness_t after_invalid;
    ASSERT_EQ(odin_dns_resolver_test_liveness(&after_invalid), 0);
    EXPECT_EQ(after_invalid.resolver_create_calls,
              before.resolver_create_calls);
    EXPECT_EQ(after_invalid.resolver_destroy_calls,
              before.resolver_destroy_calls);

    int pc = -1;
    int pd = -1;
    MakeUnixPair(&pc, &pd);
    out = nullptr;
    ASSERT_EQ(odin_server_session_create(loop, pd, OnClose, nullptr, &out), 0)
        << std::strerror(errno);
    odin_dns_resolver_test_liveness_t after_create;
    ASSERT_EQ(odin_dns_resolver_test_liveness(&after_create), 0);
    EXPECT_EQ(after_create.resolver_create_calls,
              after_invalid.resolver_create_calls + 1);
    EXPECT_EQ(after_create.resolvers, after_invalid.resolvers + 1);
    odin_server_session_destroy(out);
    odin_dns_resolver_test_liveness_t after_destroy;
    ASSERT_EQ(odin_dns_resolver_test_liveness(&after_destroy), 0);
    EXPECT_EQ(after_destroy.resolver_destroy_calls,
              after_invalid.resolver_destroy_calls + 1);
    EXPECT_EQ(after_destroy.resolvers, after_invalid.resolvers);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(pb), 0);
    EXPECT_EQ(close(pc), 0);
    odin_dns_resolver_destroy(resolver);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinServerDnsTest, T11DestroyFromDnsCompletionFilterStopsSelection) {
  ServerSessionRunDeadline::Run([] {
    ServerDnsFixture fixture;
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    (void)signal(SIGPIPE, SIG_IGN);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    odin_dns_resolver_t *resolver = nullptr;
    CreateFixtureResolver(loop, &fixture, &resolver);
    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create_with_resolver(loop, pb, resolver,
                                                       OnClose, &state, &ss),
              0);
    ExactDenyFilterState filter;
    filter.destroy_ss = ss;
    odin_server_session_set_dial_filter(ss, ExactDenyFilter, &filter);
    const std::string req = EncodedReq("destroy-filter.test", port);
    ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));
    odin_event_timer_t *end_timer = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 100000, 0, TestEndCb, nullptr, &end_timer),
        0);
    ASSERT_EQ(odin_event_loop_run(loop), 0);
    EXPECT_EQ(filter.calls, 1);
    EXPECT_EQ(state.on_close_calls, 0);
    uint8_t resp[4];
    EXPECT_EQ(ReadExactly(pa, resp, sizeof(resp), 50), 0u);
    const int arc = accept(lfd, nullptr, nullptr);
    EXPECT_EQ(arc, -1);
    EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
    odin_dns_resolver_test_liveness_t live;
    ASSERT_EQ(odin_dns_resolver_test_liveness(&live), 0);
    ExpectDnsLiveCounts(live, 1, 0, 0, 0, 0, 0);
    odin_dns_resolver_destroy(resolver);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinServerDnsTest, T12Ipv6ResultReachesFilterAndDial) {
  ServerSessionRunDeadline::Run([] {
    const int lfd = socket(AF_INET6, SOCK_STREAM, 0);
    if (lfd < 0) {
      GTEST_SKIP() << "IPv6 loopback socket unsupported";
    }
    int flags = fcntl(lfd, F_GETFL, 0);
    ASSERT_NE(flags, -1);
    ASSERT_EQ(fcntl(lfd, F_SETFL, flags | O_NONBLOCK), 0);
    sockaddr_in6 addr6;
    std::memset(&addr6, 0, sizeof(addr6));
    addr6.sin6_family = AF_INET6;
    addr6.sin6_addr = in6addr_loopback;
    ASSERT_EQ(bind(lfd, reinterpret_cast<sockaddr *>(&addr6), sizeof(addr6)), 0)
        << std::strerror(errno);
    ASSERT_EQ(listen(lfd, 16), 0);
    socklen_t alen = sizeof(addr6);
    ASSERT_EQ(getsockname(lfd, reinterpret_cast<sockaddr *>(&addr6), &alen), 0);
    const uint16_t port = ntohs(addr6.sin6_port);
    std::thread srv([lfd] {
      struct pollfd pfd{lfd, POLLIN, 0};
      (void)poll(&pfd, 1, 1500);
      const int fd = accept(lfd, nullptr, nullptr);
      if (fd >= 0) {
        (void)shutdown(fd, SHUT_WR);
        close(fd);
      }
    });
    ServerDnsFixture fixture;
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    odin_dns_resolver_t *resolver = nullptr;
    CreateFixtureResolver(loop, &fixture, &resolver);
    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create_with_resolver(loop, pb, resolver,
                                                       OnClose, &state, &ss),
              0);
    std::vector<std::string> seen;
    odin_server_session_set_dial_filter(ss, RecordAndAllowFilter, &seen);
    const std::string req = EncodedReq("v6.test", port);
    ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));
    std::thread client([pa] {
      ExpectRespCode(pa, ODIN_SERVER_SESSION_RESP_CODE_OK);
      (void)shutdown(pa, SHUT_WR);
    });
    RunServerLoop(loop, &state);
    client.join();
    srv.join();
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_NE(seen[0].find("::1:" + std::to_string(port)), std::string::npos);
    odin_dns_resolver_test_cares_observation_t obs;
    ASSERT_EQ(odin_dns_resolver_test_cares_observation(&obs), 0);
    EXPECT_EQ(obs.last_ai_family, AF_UNSPEC);
    EXPECT_EQ(state.on_close_err, 0);
    odin_dns_resolver_destroy(resolver);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinServerDnsTest, T13ResolverStartAndDialStartFailuresMapCleanly) {
  ServerSessionRunDeadline::Run([] {
    for (int subcase = 0; subcase < 2; ++subcase) {
      ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
      ServerDnsFixture fixture;
      int pa = -1;
      int pb = -1;
      MakeUnixPair(&pa, &pb);
      uint16_t port = 0;
      const int lfd = OpenLoopbackListener(&port);
      ASSERT_GE(lfd, 0);
      odin_event_loop_t *loop = nullptr;
      ASSERT_EQ(odin_event_loop_create(&loop), 0);
      odin_dns_resolver_t *resolver = nullptr;
      CreateFixtureResolver(loop, &fixture, &resolver);
      ServerSessionState state;
      state.loop = loop;
      odin_server_session_t *ss = nullptr;
      ASSERT_EQ(odin_server_session_create_with_resolver(loop, pb, resolver,
                                                         OnClose, &state, &ss),
                0);
      if (subcase == 0) {
        odin_dns_resolver_test_cares_step_t step = {};
        step.op = ODIN_DNS_TEST_CARES_INIT_OPTIONS;
        step.status = ARES_ENOMEM;
        ASSERT_EQ(odin_dns_resolver_test_push_cares_step(&step), 0);
        const std::string req = EncodedReq("target.test", port);
        ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));
      } else {
#if defined(ODIN_SERVER_SESSION_TESTING)
        ASSERT_EQ(odin_server_session_test_fail_next_dial(ss, EMFILE), 0);
#endif
        const std::string req = EncodedReq("fail-dial.test", port);
        ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));
      }
      std::thread client(
          [pa] { ExpectRespCode(pa, ODIN_SERVER_SESSION_RESP_CODE_OTHER); });
      RunServerLoop(loop, &state);
      client.join();
      EXPECT_EQ(state.on_close_err, subcase == 0 ? ENOMEM : EMFILE);
      const int arc = accept(lfd, nullptr, nullptr);
      EXPECT_EQ(arc, -1);
      EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
      odin_dns_resolver_destroy(resolver);
      EXPECT_EQ(close(pa), 0);
      EXPECT_EQ(close(lfd), 0);
      odin_event_loop_destroy(loop);
    }
  });
}

TEST(OdinServerDnsTest, T14PrivateResolverRollbackAcrossConstructorFailures) {
  for (int subcase = 0; subcase < 4; ++subcase) {
    ServerSessionRunDeadline::Run([subcase] {
      auto expect_baseline = [](const odin_dns_resolver_test_liveness_t &base) {
        odin_dns_resolver_test_liveness_t post;
        ASSERT_EQ(odin_dns_resolver_test_liveness(&post), 0);
        EXPECT_EQ(post.resolvers, base.resolvers);
        EXPECT_EQ(post.queries, base.queries);
        EXPECT_EQ(post.watches, base.watches);
        EXPECT_EQ(post.timers, base.timers);
        EXPECT_EQ(post.cares_channels, base.cares_channels);
        EXPECT_EQ(post.cares_results, base.cares_results);
        EXPECT_EQ(post.resolver_create_calls, base.resolver_create_calls + 1);
        EXPECT_EQ(post.resolver_destroy_calls, base.resolver_destroy_calls + 1);
      };

      odin_event_loop_t *loop = nullptr;
      ASSERT_EQ(odin_event_loop_create(&loop), 0);
      ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);

      if (subcase == 0) {
        odin_dns_resolver_test_cares_step_t step = {};
        step.op = ODIN_DNS_TEST_CARES_LIBRARY_INIT;
        step.status = ARES_ENOMEM;
        ASSERT_EQ(odin_dns_resolver_test_push_cares_step(&step), 0);
        int pa = -1;
        int pb = -1;
        MakeUnixPair(&pa, &pb);
        odin_server_session_t *out =
            reinterpret_cast<odin_server_session_t *>(0xDEADBEEF);
        EXPECT_EQ(odin_server_session_create(loop, pb, OnClose, nullptr, &out),
                  -1);
        EXPECT_EQ(errno, ENOMEM);
        EXPECT_EQ(out, reinterpret_cast<odin_server_session_t *>(0xDEADBEEF));
        EXPECT_GE(fcntl(pb, F_GETFD), 0);
        odin_dns_resolver_test_liveness_t live;
        ASSERT_EQ(odin_dns_resolver_test_liveness(&live), 0);
        EXPECT_EQ(live.resolver_create_calls, 0u);
        EXPECT_EQ(live.resolver_destroy_calls, 0u);
        EXPECT_EQ(close(pa), 0);
        EXPECT_EQ(close(pb), 0);
        odin_event_loop_destroy(loop);
        return;
      }

      odin_dns_resolver_test_liveness_t base;
      ASSERT_EQ(odin_dns_resolver_test_liveness(&base), 0);
      odin_server_session_t *out =
          reinterpret_cast<odin_server_session_t *>(0xDEADBEEF);
      if (subcase == 1) {
        FdFactoryState factory_fail;
        factory_fail.loop = loop;
        factory_fail.fail_errno = EIO;
        EXPECT_EQ(odin_server_session_create_with_transport(
                      loop, FdFactory, &factory_fail, OnClose, nullptr, &out),
                  -1);
        EXPECT_EQ(errno, EIO);
        EXPECT_EQ(factory_fail.calls, 1);
        expect_baseline(base);
      } else if (subcase == 2) {
        FakeDownstreamTransport fake;
        fake.set_interest_errno = EEXIST;
        EXPECT_EQ(
            odin_server_session_create_with_transport(
                loop, FakeDownstreamFactory, &fake, OnClose, nullptr, &out),
            -1);
        EXPECT_EQ(errno, EEXIST);
        EXPECT_EQ(fake.destroy_calls, 1);
        expect_baseline(base);
      } else {
        int pa = -1;
        int pb = -1;
        MakeUnixPair(&pa, &pb);
#if defined(ODIN_CONNECT_SESSION_TESTING)
        ASSERT_EQ(odin_connect_session_test_fail_next_create_server(ENOMEM), 0);
#endif
        EXPECT_EQ(odin_server_session_create(loop, pb, OnClose, nullptr, &out),
                  -1);
        EXPECT_EQ(errno, ENOMEM);
        EXPECT_GE(fcntl(pb, F_GETFD), 0);
        expect_baseline(base);
        EXPECT_EQ(close(pa), 0);
        EXPECT_EQ(close(pb), 0);
      }
      odin_event_loop_destroy(loop);
    });
  }
}

TEST(OdinServerDnsTest, T16StagedOkConnectRespWriteResumesAfterDns) {
  ServerSessionRunDeadline::Run([] {
    ServerDnsFixture fixture;
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    std::atomic<bool> accepted{false};
    std::atomic<bool> payload_seen{false};
    std::string upstream_got;
    std::thread srv([lfd, &accepted, &payload_seen, &upstream_got] {
      struct pollfd pfd{lfd, POLLIN, 0};
      (void)poll(&pfd, 1, 1500);
      const int fd = accept(lfd, nullptr, nullptr);
      if (fd < 0) {
        return;
      }
      accepted.store(true);
      char buf[7] = {};
      const size_t n = ReadExactly(fd, buf, 7, 1500);
      upstream_got.assign(buf, n);
      payload_seen.store(true);
      (void)shutdown(fd, SHUT_WR);
      close(fd);
    });
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    odin_dns_resolver_t *resolver = nullptr;
    CreateFixtureResolver(loop, &fixture, &resolver);
    FakeDownstreamTransport fake;
    fake.reads = EncodedReq("staged.test", port) + "payload";
    fake.write_sizes = {2, 0, 2};
    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(
        odin_server_session_create_with_transport_and_resolver(
            loop, FakeDownstreamFactory, &fake, resolver, OnClose, &state, &ss),
        0);
    fake.on_ready(&fake.base, ODIN_TRANSPORT_READ, fake.ready_user_data);
    odin_event_timer_t *end_timer = nullptr;
    ASSERT_EQ(
        odin_event_timer_start(loop, 150000, 0, TestEndCb, nullptr, &end_timer),
        0);
    ASSERT_EQ(odin_event_loop_run(loop), 0);
    ASSERT_TRUE((fake.interest & ODIN_TRANSPORT_WRITE) != 0);
    fake.on_ready(&fake.base, ODIN_TRANSPORT_WRITE, fake.ready_user_data);
    EXPECT_EQ(fake.writes, std::string("\x01\x02", 2));
#if defined(ODIN_SERVER_SESSION_TESTING)
    EXPECT_EQ(odin_server_session_test_state(ss),
              ODIN_SERVER_SESSION_TEST_STATE_WRITING_OK_RESP);
#endif
    EXPECT_FALSE(payload_seen.load());
    fake.on_ready(&fake.base, ODIN_TRANSPORT_WRITE, fake.ready_user_data);
    EXPECT_EQ(fake.writes, std::string("\x01\x02\x00\x00", 4));
#if defined(ODIN_SERVER_SESSION_TESTING)
    EXPECT_EQ(odin_server_session_test_state(ss),
              ODIN_SERVER_SESSION_TEST_STATE_RELAY);
#endif
    fake.on_ready(&fake.base, ODIN_TRANSPORT_READ, fake.ready_user_data);
    RunServerLoop(loop, &state, 300000);
    srv.join();
    EXPECT_EQ(upstream_got, "payload");
    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, 0);
    odin_dns_resolver_destroy(resolver);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinServerDnsTest, T17FirstDialStartFailureContinuesToLaterAddress) {
  ServerSessionRunDeadline::Run([] {
    ServerDnsFixture fixture;
    int pa = -1;
    int pb = -1;
    MakeUnixPair(&pa, &pb);
    uint16_t port = 0;
    const int lfd = OpenLoopbackListener(&port);
    ASSERT_GE(lfd, 0);
    std::thread srv([lfd] {
      struct pollfd pfd{lfd, POLLIN, 0};
      (void)poll(&pfd, 1, 1500);
      const int fd = accept(lfd, nullptr, nullptr);
      if (fd >= 0) {
        (void)shutdown(fd, SHUT_WR);
        close(fd);
      }
    });
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0);
    odin_dns_resolver_t *resolver = nullptr;
    CreateFixtureResolver(loop, &fixture, &resolver);
    ServerSessionState state;
    state.loop = loop;
    odin_server_session_t *ss = nullptr;
    ASSERT_EQ(odin_server_session_create_with_resolver(loop, pb, resolver,
                                                       OnClose, &state, &ss),
              0);
#if defined(ODIN_SERVER_SESSION_TESTING)
    ASSERT_EQ(odin_server_session_test_fail_next_dial(ss, EMFILE), 0);
#endif
    const std::string req = EncodedReq("fail-first.test", port);
    ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));
    std::thread client([pa] {
      ExpectRespCode(pa, ODIN_SERVER_SESSION_RESP_CODE_OK);
      (void)shutdown(pa, SHUT_WR);
    });
    RunServerLoop(loop, &state);
    client.join();
    srv.join();
    EXPECT_EQ(state.on_close_calls, 1);
    EXPECT_EQ(state.on_close_err, 0);
    odin_dns_resolver_destroy(resolver);
    EXPECT_EQ(close(pa), 0);
    EXPECT_EQ(close(lfd), 0);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinServerDnsTest, T18ExistingPrivateConstructorsResolveNumericConnect) {
  for (int transport_case = 0; transport_case < 2; ++transport_case) {
    ServerSessionRunDeadline::Run([&] {
      ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0);
      int pa = -1;
      int pb = -1;
      MakeUnixPair(&pa, &pb);
      uint16_t port = 0;
      const int lfd = OpenLoopbackListener(&port);
      ASSERT_GE(lfd, 0);
      std::thread srv([lfd] {
        struct pollfd pfd{lfd, POLLIN, 0};
        (void)poll(&pfd, 1, 1500);
        const int fd = accept(lfd, nullptr, nullptr);
        if (fd >= 0) {
          (void)shutdown(fd, SHUT_WR);
          close(fd);
        }
      });
      odin_event_loop_t *loop = nullptr;
      ASSERT_EQ(odin_event_loop_create(&loop), 0);
      odin_dns_resolver_test_liveness_t live0;
      ASSERT_EQ(odin_dns_resolver_test_liveness(&live0), 0);
      odin_dns_resolver_test_cares_observation_t obs0;
      ASSERT_EQ(odin_dns_resolver_test_cares_observation(&obs0), 0);
      ServerSessionState state;
      state.loop = loop;
      odin_server_session_t *ss = nullptr;
      FdFactoryState factory;
      factory.fd = pb;
      factory.loop = loop;
      if (transport_case == 0) {
        ASSERT_EQ(odin_server_session_create(loop, pb, OnClose, &state, &ss),
                  0);
      } else {
        ASSERT_EQ(odin_server_session_create_with_transport(
                      loop, FdFactory, &factory, OnClose, &state, &ss),
                  0);
      }
      const std::string req = EncodedReq("127.0.0.1", port);
      ASSERT_TRUE(WriteAll(pa, req.data(), req.size()));
      std::thread client([pa] {
        ExpectRespCode(pa, ODIN_SERVER_SESSION_RESP_CODE_OK);
        (void)shutdown(pa, SHUT_WR);
      });
      RunServerLoop(loop, &state);
      client.join();
      srv.join();
      odin_dns_resolver_test_liveness_t live1;
      ASSERT_EQ(odin_dns_resolver_test_liveness(&live1), 0);
      odin_dns_resolver_test_cares_observation_t obs1;
      ASSERT_EQ(odin_dns_resolver_test_cares_observation(&obs1), 0);
      EXPECT_EQ(live1.resolver_create_calls, live0.resolver_create_calls + 1);
      EXPECT_EQ(live1.resolver_destroy_calls, live0.resolver_destroy_calls + 1);
      EXPECT_EQ(obs1.getaddrinfo_calls, obs0.getaddrinfo_calls + 1);
      EXPECT_EQ(obs1.last_ai_family, AF_UNSPEC);
      EXPECT_EQ(state.on_close_calls, 1);
      EXPECT_EQ(state.on_close_err, 0);
      EXPECT_EQ(close(pa), 0);
      EXPECT_EQ(close(lfd), 0);
      odin_event_loop_destroy(loop);
    });
  }
}

// NOLINTEND(misc-const-correctness, misc-use-internal-linkage)
