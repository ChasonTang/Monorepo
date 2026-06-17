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
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "odin/event_loop.h"
#include "odin/protocol.h"
#include "odin/testing/event_loop_internal_test.h"
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

// NOLINTEND(misc-const-correctness, misc-use-internal-linkage)
