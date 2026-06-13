// odin/udp_unittests.cpp
//
// Unit tests T1-T15 from §5 of odin/docs/rfc_015_udp_endpoint.md.

#include "odin/udp.h"

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
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "odin/event_loop.h"
#include "odin/event_loop_internal_test.h"
#include "odin/udp_internal_test.h"

#include "gtest/gtest.h"

// NOLINTBEGIN(misc-const-correctness, misc-use-internal-linkage)

namespace {

class UdpRunDeadline {
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
      FAIL() << "UdpRunDeadline exceeded 2 seconds";
    }
    ASSERT_TRUE(WIFEXITED(wstatus));
    EXPECT_EQ(WEXITSTATUS(wstatus), 0);
  }
};

void CloseFd(int fd) {
  if (fd >= 0) {
    EXPECT_EQ(close(fd), 0) << std::strerror(errno);
  }
}

struct sockaddr_in Loopback4(uint16_t port) {
  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  return addr;
}

struct sockaddr_in TestNet4(uint16_t port) {
  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  EXPECT_EQ(inet_pton(AF_INET, "192.0.2.1", &addr.sin_addr), 1);
  addr.sin_port = htons(port);
  return addr;
}

struct sockaddr_in6 Loopback6(uint16_t port) {
  struct sockaddr_in6 addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin6_family = AF_INET6;
  addr.sin6_addr = in6addr_loopback;
  addr.sin6_port = htons(port);
  return addr;
}

void MakeUdp4Peer(int *fd_out, struct sockaddr_in *out_addr) {
  const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  ASSERT_GE(fd, 0) << std::strerror(errno);
  struct sockaddr_in addr = Loopback4(0);
  ASSERT_EQ(bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)),
            0)
      << std::strerror(errno);
  socklen_t len = sizeof(addr);
  ASSERT_EQ(getsockname(fd, reinterpret_cast<struct sockaddr *>(&addr), &len),
            0)
      << std::strerror(errno);
  EXPECT_EQ(len, sizeof(addr));
  *fd_out = fd;
  *out_addr = addr;
}

bool ProbeIpv6Loopback(std::string *reason) {
  const int fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    *reason = std::string("IPv6 loopback UDP socket unavailable: ") +
              std::strerror(errno);
    return false;
  }
  struct sockaddr_in6 addr = Loopback6(0);
  if (bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
    const int saved = errno;
    CloseFd(fd);
    *reason = std::string("IPv6 loopback UDP bind unavailable: ") +
              std::strerror(saved);
    return false;
  }
  CloseFd(fd);
  return true;
}

void MakeUdp6Peer(int *fd_out, struct sockaddr_in6 *out_addr) {
  const int fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  ASSERT_GE(fd, 0) << std::strerror(errno);
  struct sockaddr_in6 addr = Loopback6(0);
  ASSERT_EQ(bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)),
            0)
      << std::strerror(errno);
  socklen_t len = sizeof(addr);
  ASSERT_EQ(getsockname(fd, reinterpret_cast<struct sockaddr *>(&addr), &len),
            0)
      << std::strerror(errno);
  EXPECT_EQ(len, sizeof(addr));
  *fd_out = fd;
  *out_addr = addr;
}

void GetUdp4Endpoint(odin_udp_t *u, int *fd_out, struct sockaddr_in *addr_out) {
  const int fd = odin_udp_test_fd(u);
  ASSERT_GE(fd, 0) << std::strerror(errno);
  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  socklen_t len = sizeof(addr);
  ASSERT_EQ(getsockname(fd, reinterpret_cast<struct sockaddr *>(&addr), &len),
            0)
      << std::strerror(errno);
  EXPECT_EQ(len, sizeof(addr));
  *fd_out = fd;
  *addr_out = addr;
}

void GetUdp6Endpoint(odin_udp_t *u, int *fd_out,
                     struct sockaddr_in6 *addr_out) {
  const int fd = odin_udp_test_fd(u);
  ASSERT_GE(fd, 0) << std::strerror(errno);
  struct sockaddr_in6 addr;
  std::memset(&addr, 0, sizeof(addr));
  socklen_t len = sizeof(addr);
  ASSERT_EQ(getsockname(fd, reinterpret_cast<struct sockaddr *>(&addr), &len),
            0)
      << std::strerror(errno);
  EXPECT_EQ(len, sizeof(addr));
  *fd_out = fd;
  *addr_out = addr;
}

struct ReadyState {
  odin_event_loop_t *loop = nullptr;
  odin_udp_t *u = nullptr;
  int calls = 0;
  unsigned int events = 0;
  bool timed_out = false;
  bool closed = false;
  odin_udp_io_t recv_rc = ODIN_UDP_IO_ERROR;
  odin_udp_io_t recv2_rc = ODIN_UDP_IO_ERROR;
  size_t n = 999;
  size_t n2 = 999;
  char buf[128] = {};
  char buf2[32] = {};
  struct sockaddr_storage src;
  struct sockaddr_storage src2;
  socklen_t srclen = sizeof(src);
  socklen_t srclen2 = sizeof(src2);
};

void WatchdogCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                void *user_data) {
  (void)timer;
  ReadyState *s = static_cast<ReadyState *>(user_data);
  s->timed_out = true;
  odin_event_loop_stop(loop);
}

void RecvReadyCb(odin_udp_t *u, unsigned int events, void *user_data) {
  ReadyState *s = static_cast<ReadyState *>(user_data);
  s->calls += 1;
  s->events |= events;
  s->srclen = sizeof(s->src);
  s->recv_rc = odin_udp_recv(u, s->buf, sizeof(s->buf), &s->n,
                             reinterpret_cast<struct sockaddr *>(&s->src),
                             &s->srclen);
  odin_event_loop_stop(s->loop);
}

void CloseReadyCb(odin_udp_t *u, unsigned int events, void *user_data) {
  ReadyState *s = static_cast<ReadyState *>(user_data);
  s->calls += 1;
  s->events |= events;
  s->srclen = sizeof(s->src);
  s->recv_rc = odin_udp_recv(u, s->buf, sizeof(s->buf), &s->n,
                             reinterpret_cast<struct sockaddr *>(&s->src),
                             &s->srclen);
  odin_udp_close(u);
  s->u = nullptr;
  s->closed = true;
  odin_event_loop_stop(s->loop);
}

void WriteReadyCb(odin_udp_t *u, unsigned int events, void *user_data) {
  ReadyState *s = static_cast<ReadyState *>(user_data);
  s->calls += 1;
  s->events |= events;
  EXPECT_EQ(odin_udp_set_interest(u, 0), 0) << std::strerror(errno);
  odin_event_loop_stop(s->loop);
}

void TruncateReadyCb(odin_udp_t *u, unsigned int events, void *user_data) {
  ReadyState *s = static_cast<ReadyState *>(user_data);
  s->calls += 1;
  s->events |= events;
  s->srclen = sizeof(s->src);
  s->recv_rc = odin_udp_recv(u, s->buf, 16, &s->n,
                             reinterpret_cast<struct sockaddr *>(&s->src),
                             &s->srclen);
  s->srclen2 = sizeof(s->src2);
  s->recv2_rc = odin_udp_recv(u, s->buf2, 16, &s->n2,
                              reinterpret_cast<struct sockaddr *>(&s->src2),
                              &s->srclen2);
  odin_event_loop_stop(s->loop);
}

void ErrorReadyCb(odin_udp_t *u, unsigned int events, void *user_data) {
  (void)u;
  ReadyState *s = static_cast<ReadyState *>(user_data);
  s->calls += 1;
  s->events |= events;
  odin_event_loop_stop(s->loop);
}

void ArmWatchdog(odin_event_loop_t *loop, ReadyState *state) {
  odin_event_timer_t *watchdog = nullptr;
  ASSERT_EQ(odin_event_timer_start(loop, 100000, 0, WatchdogCb, state,
                                   &watchdog),
            0)
      << std::strerror(errno);
}

void ExpectSource4(const struct sockaddr_storage &src, in_port_t port) {
  ASSERT_EQ(src.ss_family, AF_INET);
  const struct sockaddr_in *sin =
      reinterpret_cast<const struct sockaddr_in *>(&src);
  EXPECT_EQ(sin->sin_port, port);
  EXPECT_EQ(sin->sin_addr.s_addr, htonl(INADDR_LOOPBACK));
}

void ExpectSource6(const struct sockaddr_storage &src, in_port_t port) {
  ASSERT_EQ(src.ss_family, AF_INET6);
  const struct sockaddr_in6 *sin6 =
      reinterpret_cast<const struct sockaddr_in6 *>(&src);
  EXPECT_EQ(sin6->sin6_port, port);
  EXPECT_EQ(std::memcmp(&sin6->sin6_addr, &in6addr_loopback,
                        sizeof(in6addr_loopback)),
            0);
}

} // namespace

TEST(OdinUdpTest, T1) {
  UdpRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    ReadyState state;
    state.loop = loop;

    struct sockaddr_in local = Loopback4(0);
    ASSERT_EQ(odin_udp_open(loop, reinterpret_cast<struct sockaddr *>(&local),
                            sizeof(local), RecvReadyCb, &state, &state.u),
              0)
        << std::strerror(errno);
    int fd0 = -1;
    struct sockaddr_in ep;
    GetUdp4Endpoint(state.u, &fd0, &ep);
    EXPECT_GE(fd0, 0);

    struct sockaddr_in peer_addr;
    int peer = -1;
    MakeUdp4Peer(&peer, &peer_addr);
    ASSERT_EQ(odin_udp_set_interest(state.u, ODIN_UDP_READ), 0)
        << std::strerror(errno);
    ASSERT_EQ(sendto(peer, "hi", 2, 0,
                     reinterpret_cast<struct sockaddr *>(&ep), sizeof(ep)),
              2)
        << std::strerror(errno);
    ArmWatchdog(loop, &state);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    EXPECT_EQ(state.calls, 1);
    EXPECT_NE(state.events & ODIN_UDP_READ, 0u);
    EXPECT_EQ(state.recv_rc, ODIN_UDP_OK);
    EXPECT_EQ(state.n, 2u);
    EXPECT_EQ(std::string(state.buf, 2), std::string("hi"));
    ExpectSource4(state.src, peer_addr.sin_port);
    EXPECT_FALSE(state.timed_out);

    odin_udp_close(state.u);
    CloseFd(peer);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinUdpTest, T2) {
  UdpRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    ReadyState state;
    struct sockaddr_in local = Loopback4(0);
    ASSERT_EQ(odin_udp_open(loop, reinterpret_cast<struct sockaddr *>(&local),
                            sizeof(local), ErrorReadyCb, &state, &state.u),
              0)
        << std::strerror(errno);
    char buf[8];
    size_t n = 0;
    struct sockaddr_storage src;
    socklen_t srclen = sizeof(src);
    EXPECT_EQ(odin_udp_recv(state.u, buf, sizeof(buf), &n,
                            reinterpret_cast<struct sockaddr *>(&src),
                            &srclen),
              ODIN_UDP_AGAIN);
    odin_udp_close(state.u);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinUdpTest, T3) {
  UdpRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    ReadyState state;
    state.loop = loop;
    struct sockaddr_in local = Loopback4(0);
    ASSERT_EQ(odin_udp_open(loop, reinterpret_cast<struct sockaddr *>(&local),
                            sizeof(local), RecvReadyCb, &state, &state.u),
              0)
        << std::strerror(errno);
    int fd0 = -1;
    struct sockaddr_in ep;
    GetUdp4Endpoint(state.u, &fd0, &ep);

    struct sockaddr_in peer_addr;
    int peer = -1;
    MakeUdp4Peer(&peer, &peer_addr);
    ASSERT_EQ(odin_udp_set_interest(state.u, ODIN_UDP_READ), 0)
        << std::strerror(errno);
    ASSERT_EQ(sendto(peer, "", 0, 0, reinterpret_cast<struct sockaddr *>(&ep),
                     sizeof(ep)),
              0)
        << std::strerror(errno);
    ArmWatchdog(loop, &state);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    EXPECT_EQ(state.calls, 1);
    EXPECT_NE(state.events & ODIN_UDP_READ, 0u);
    EXPECT_EQ(state.recv_rc, ODIN_UDP_OK);
    EXPECT_EQ(state.n, 0u);
    ExpectSource4(state.src, peer_addr.sin_port);
    EXPECT_FALSE(state.timed_out);

    odin_udp_close(state.u);
    CloseFd(peer);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinUdpTest, T4) {
  UdpRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    ReadyState state;
    struct sockaddr_in local = Loopback4(0);
    ASSERT_EQ(odin_udp_open(loop, reinterpret_cast<struct sockaddr *>(&local),
                            sizeof(local), ErrorReadyCb, &state, &state.u),
              0)
        << std::strerror(errno);
    struct sockaddr_in dst;
    int peer = -1;
    MakeUdp4Peer(&peer, &dst);
    size_t n = 0;
    ASSERT_EQ(odin_udp_send(state.u, "yo", 2, &n,
                            reinterpret_cast<struct sockaddr *>(&dst),
                            sizeof(dst)),
              ODIN_UDP_OK)
        << std::strerror(errno);
    EXPECT_EQ(n, 2u);
    char buf[8];
    const ssize_t got = recvfrom(peer, buf, sizeof(buf), 0, nullptr, nullptr);
    ASSERT_EQ(got, 2) << std::strerror(errno);
    EXPECT_EQ(std::string(buf, 2), std::string("yo"));
    odin_udp_close(state.u);
    CloseFd(peer);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinUdpTest, T5) {
  UdpRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    ReadyState state;
    struct sockaddr_in local = Loopback4(0);
    ASSERT_EQ(odin_udp_open(loop, reinterpret_cast<struct sockaddr *>(&local),
                            sizeof(local), ErrorReadyCb, &state, &state.u),
              0)
        << std::strerror(errno);
    std::vector<char> big(70000, 'x');
    struct sockaddr_in dst = Loopback4(9);
    size_t n = 0;
    errno = 0;
    const odin_udp_io_t rc = odin_udp_send(
        state.u, big.data(), big.size(), &n,
        reinterpret_cast<struct sockaddr *>(&dst), sizeof(dst));
    const int err = errno;
    EXPECT_EQ(rc, ODIN_UDP_IO_ERROR);
    EXPECT_EQ(err, EMSGSIZE);
    odin_udp_close(state.u);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinUdpTest, T6) {
  UdpRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    ReadyState state;
    odin_event_loop_test_liveness_t live_before;
    odin_event_loop_test_liveness_t live_after;
    ASSERT_EQ(odin_event_loop_test_liveness(&live_before), 0)
        << std::strerror(errno);
    struct sockaddr_un bad;
    std::memset(&bad, 0, sizeof(bad));
    bad.sun_family = AF_UNIX;
    odin_udp_t *const sentinel =
        reinterpret_cast<odin_udp_t *>(-1); // NOLINT(performance-no-int-to-ptr)
    odin_udp_t *u = sentinel;
    errno = 0;
    const int rc = odin_udp_open(
        loop, reinterpret_cast<struct sockaddr *>(&bad), sizeof(bad),
        ErrorReadyCb, &state, &u);
    const int err = errno;
    ASSERT_EQ(odin_event_loop_test_liveness(&live_after), 0)
        << std::strerror(errno);
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(err, EAFNOSUPPORT);
    EXPECT_EQ(u, sentinel);
    EXPECT_EQ(live_after.io_handles, live_before.io_handles);
    if (u != sentinel) {
      odin_udp_close(u);
    }
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinUdpTest, T7) {
  UdpRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

    const int probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_GE(probe, 0) << std::strerror(errno);
    const int expected_fd = probe;
    CloseFd(probe);

    ReadyState state;
    struct sockaddr_in bad = TestNet4(0);
    odin_event_loop_test_liveness_t live_before;
    odin_event_loop_test_liveness_t live_after;
    ASSERT_EQ(odin_event_loop_test_liveness(&live_before), 0)
        << std::strerror(errno);
    odin_udp_t *const sentinel =
        reinterpret_cast<odin_udp_t *>(-1); // NOLINT(performance-no-int-to-ptr)
    odin_udp_t *u = sentinel;
    errno = 0;
    const int rc = odin_udp_open(
        loop, reinterpret_cast<struct sockaddr *>(&bad), sizeof(bad),
        ErrorReadyCb, &state, &u);
    const int err = errno;
    ASSERT_EQ(odin_event_loop_test_liveness(&live_after), 0)
        << std::strerror(errno);
    const int probe2 = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_GE(probe2, 0) << std::strerror(errno);
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(err, EADDRNOTAVAIL);
    EXPECT_EQ(u, sentinel);
    EXPECT_EQ(probe2, expected_fd);
    EXPECT_EQ(live_after.io_handles, live_before.io_handles);
    CloseFd(probe2);
    if (u != sentinel) {
      odin_udp_close(u);
    }
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinUdpTest, T8) {
  UdpRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    ReadyState state;
    state.loop = loop;
    struct sockaddr_in local = Loopback4(0);
    ASSERT_EQ(odin_udp_open(loop, reinterpret_cast<struct sockaddr *>(&local),
                            sizeof(local), CloseReadyCb, &state, &state.u),
              0)
        << std::strerror(errno);
    int fd0 = -1;
    struct sockaddr_in ep;
    GetUdp4Endpoint(state.u, &fd0, &ep);
    ASSERT_EQ(odin_udp_set_interest(state.u, ODIN_UDP_READ), 0)
        << std::strerror(errno);
    odin_event_io_t *io_before = nullptr;
    ASSERT_EQ(odin_udp_test_io(state.u, &io_before), 0) << std::strerror(errno);
    unsigned int mask_before = 0;
    ASSERT_EQ(odin_event_loop_test_kqueue_registered_mask(loop, fd0,
                                                          &mask_before),
              0)
        << std::strerror(errno);
    EXPECT_EQ(mask_before, ODIN_EVENT_READ);

    struct sockaddr_in peer_addr;
    int peer = -1;
    MakeUdp4Peer(&peer, &peer_addr);
    ASSERT_EQ(sendto(peer, "x", 1, 0, reinterpret_cast<struct sockaddr *>(&ep),
                     sizeof(ep)),
              1)
        << std::strerror(errno);
    ArmWatchdog(loop, &state);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    EXPECT_EQ(state.calls, 1);
    EXPECT_TRUE(state.closed);
    errno = 0;
    EXPECT_EQ(fcntl(fd0, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    unsigned int mask_after = 99;
    EXPECT_EQ(odin_event_loop_test_kqueue_registered_mask(loop, fd0,
                                                          &mask_after),
              0)
        << std::strerror(errno);
    EXPECT_EQ(mask_after, 0u);
    EXPECT_FALSE(state.timed_out);

    if (state.u != nullptr) {
      odin_udp_close(state.u);
    }
    CloseFd(peer);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinUdpTest, T9) {
  UdpRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    ReadyState state;
    state.loop = loop;
    struct sockaddr_in local = Loopback4(0);
    ASSERT_EQ(odin_udp_open(loop, reinterpret_cast<struct sockaddr *>(&local),
                            sizeof(local), WriteReadyCb, &state, &state.u),
              0)
        << std::strerror(errno);
    const int fd0 = odin_udp_test_fd(state.u);
    ASSERT_GE(fd0, 0) << std::strerror(errno);
    ASSERT_EQ(odin_udp_set_interest(state.u, ODIN_UDP_READ), 0)
        << std::strerror(errno);
    odin_event_io_t *io_read = nullptr;
    ASSERT_EQ(odin_udp_test_io(state.u, &io_read), 0) << std::strerror(errno);
    ASSERT_EQ(odin_udp_set_interest(state.u, ODIN_UDP_WRITE), 0)
        << std::strerror(errno);
    ArmWatchdog(loop, &state);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    EXPECT_EQ(state.calls, 1);
    EXPECT_NE(state.events & ODIN_UDP_WRITE, 0u);
    EXPECT_EQ(state.events & ODIN_UDP_READ, 0u);
    odin_event_io_t *io_after = nullptr;
    errno = 0;
    EXPECT_EQ(odin_udp_test_io(state.u, &io_after), -1);
    EXPECT_EQ(errno, ENOENT);
    unsigned int mask = 99;
    EXPECT_EQ(odin_event_loop_test_kqueue_registered_mask(loop, fd0, &mask), 0)
        << std::strerror(errno);
    EXPECT_EQ(mask, 0u);
    EXPECT_FALSE(state.timed_out);

    odin_udp_close(state.u);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinUdpTest, T10) {
  UdpRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    ReadyState state;
    state.loop = loop;
    struct sockaddr_in local = Loopback4(0);
    ASSERT_EQ(odin_udp_open(loop, reinterpret_cast<struct sockaddr *>(&local),
                            sizeof(local), TruncateReadyCb, &state, &state.u),
              0)
        << std::strerror(errno);
    int fd0 = -1;
    struct sockaddr_in ep;
    GetUdp4Endpoint(state.u, &fd0, &ep);

    struct sockaddr_in peer_addr;
    int peer = -1;
    MakeUdp4Peer(&peer, &peer_addr);
    char payload[64];
    for (size_t i = 0; i < sizeof(payload); ++i) {
      payload[i] = static_cast<char>('a' + (i % 26));
    }
    ASSERT_EQ(sendto(peer, payload, sizeof(payload), 0,
                     reinterpret_cast<struct sockaddr *>(&ep), sizeof(ep)),
              static_cast<ssize_t>(sizeof(payload)))
        << std::strerror(errno);
    ASSERT_EQ(odin_udp_set_interest(state.u, ODIN_UDP_READ), 0)
        << std::strerror(errno);
    ArmWatchdog(loop, &state);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    EXPECT_EQ(state.recv_rc, ODIN_UDP_OK);
    EXPECT_EQ(state.n, 16u);
    EXPECT_EQ(std::memcmp(state.buf, payload, 16), 0);
    EXPECT_EQ(state.recv2_rc, ODIN_UDP_AGAIN);
    EXPECT_FALSE(state.timed_out);

    odin_udp_close(state.u);
    CloseFd(peer);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinUdpTest, T11) {
  UdpRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    ReadyState state;
    struct sockaddr_in local = Loopback4(0);
    ASSERT_EQ(odin_udp_open(loop, reinterpret_cast<struct sockaddr *>(&local),
                            sizeof(local), ErrorReadyCb, &state, &state.u),
              0)
        << std::strerror(errno);
    struct sockaddr_in dst = Loopback4(9);
    ASSERT_EQ(odin_udp_test_fail_next_sendto(state.u, EAGAIN), 0)
        << std::strerror(errno);
    size_t n = 123;
    errno = 0;
    const odin_udp_io_t rc = odin_udp_send(
        state.u, "retry", 5, &n, reinterpret_cast<struct sockaddr *>(&dst),
        sizeof(dst));
    const int err = errno;
    EXPECT_EQ(rc, ODIN_UDP_AGAIN);
    EXPECT_EQ(err, EAGAIN);
    odin_udp_close(state.u);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinUdpTest, T12) {
  UdpRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    ReadyState state;
    struct sockaddr_in local = Loopback4(0);
    ASSERT_EQ(odin_udp_open(loop, reinterpret_cast<struct sockaddr *>(&local),
                            sizeof(local), ErrorReadyCb, &state, &state.u),
              0)
        << std::strerror(errno);
    const int fd0 = odin_udp_test_fd(state.u);
    ASSERT_GE(fd0, 0) << std::strerror(errno);
    ASSERT_EQ(odin_udp_set_interest(state.u, ODIN_UDP_READ), 0)
        << std::strerror(errno);
    odin_event_io_t *io0 = nullptr;
    ASSERT_EQ(odin_udp_test_io(state.u, &io0), 0) << std::strerror(errno);
    unsigned int mask = 0;
    ASSERT_EQ(odin_event_loop_test_kqueue_registered_mask(loop, fd0, &mask), 0)
        << std::strerror(errno);
    ASSERT_EQ(mask, ODIN_EVENT_READ);

    const unsigned int invalids[] = {
        ODIN_UDP_ERROR,
        ODIN_UDP_READ | ODIN_UDP_ERROR,
        ODIN_UDP_WRITE | 0x80u,
    };
    for (unsigned int invalid : invalids) {
      errno = 0;
      EXPECT_EQ(odin_udp_set_interest(state.u, invalid), -1);
      EXPECT_EQ(errno, EINVAL);
      odin_event_io_t *io_after = nullptr;
      EXPECT_EQ(odin_udp_test_io(state.u, &io_after), 0)
          << std::strerror(errno);
      EXPECT_EQ(io_after, io0);
      mask = 0;
      EXPECT_EQ(odin_event_loop_test_kqueue_registered_mask(loop, fd0, &mask),
                0)
          << std::strerror(errno);
      EXPECT_EQ(mask, ODIN_EVENT_READ);
    }
    ASSERT_EQ(odin_udp_set_interest(state.u, ODIN_UDP_WRITE), 0)
        << std::strerror(errno);
    ASSERT_EQ(odin_event_loop_test_kqueue_registered_mask(loop, fd0, &mask), 0)
        << std::strerror(errno);
    EXPECT_EQ(mask, ODIN_EVENT_WRITE);
    odin_udp_close(state.u);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinUdpTest, T13) {
  UdpRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    ReadyState state;
    state.loop = loop;
    struct sockaddr_in local = Loopback4(0);
    ASSERT_EQ(odin_udp_open(loop, reinterpret_cast<struct sockaddr *>(&local),
                            sizeof(local), ErrorReadyCb, &state, &state.u),
              0)
        << std::strerror(errno);
    ASSERT_EQ(odin_udp_set_interest(state.u, ODIN_UDP_READ), 0)
        << std::strerror(errno);
    odin_event_io_t *io = nullptr;
    ASSERT_EQ(odin_udp_test_io(state.u, &io), 0) << std::strerror(errno);
    const odin_event_loop_test_ready_t entries[] = {
        {io, ODIN_EVENT_ERROR},
    };
    ASSERT_EQ(odin_event_loop_test_queue_backend_events(loop, entries, 1), 0)
        << std::strerror(errno);
    ArmWatchdog(loop, &state);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);
    EXPECT_EQ(state.calls, 1);
    EXPECT_NE(state.events & ODIN_UDP_ERROR, 0u);
    EXPECT_FALSE(state.timed_out);
    odin_udp_close(state.u);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinUdpTest, T14) {
  UdpRunDeadline::Run([] {
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    ReadyState state;
    struct sockaddr_in local = Loopback4(0);
    ASSERT_EQ(odin_udp_open(loop, reinterpret_cast<struct sockaddr *>(&local),
                            sizeof(local), ErrorReadyCb, &state, &state.u),
              0)
        << std::strerror(errno);
    const int fd0 = odin_udp_test_fd(state.u);
    ASSERT_GE(fd0, 0) << std::strerror(errno);
    ASSERT_EQ(odin_event_loop_test_fail_next_kqueue_change(
                  loop, ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_ADD,
                  ODIN_EVENT_WRITE, ENOSPC),
              0)
        << std::strerror(errno);
    errno = 0;
    EXPECT_EQ(odin_udp_set_interest(state.u, ODIN_UDP_READ | ODIN_UDP_WRITE),
              -1);
    EXPECT_EQ(errno, ENOSPC);
    odin_event_io_t *io_after_failed_start = nullptr;
    errno = 0;
    EXPECT_EQ(odin_udp_test_io(state.u, &io_after_failed_start), -1);
    EXPECT_EQ(errno, ENOENT);
    unsigned int mask = 99;
    EXPECT_EQ(odin_event_loop_test_kqueue_registered_mask(loop, fd0, &mask), 0)
        << std::strerror(errno);
    EXPECT_EQ(mask, 0u);

    ASSERT_EQ(odin_udp_set_interest(state.u, ODIN_UDP_READ), 0)
        << std::strerror(errno);
    odin_event_io_t *io0 = nullptr;
    ASSERT_EQ(odin_udp_test_io(state.u, &io0), 0) << std::strerror(errno);
    ASSERT_EQ(odin_event_loop_test_kqueue_registered_mask(loop, fd0, &mask), 0)
        << std::strerror(errno);
    ASSERT_EQ(mask, ODIN_EVENT_READ);
    ASSERT_EQ(odin_event_loop_test_fail_next_kqueue_change(
                  loop, ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_DELETE,
                  ODIN_EVENT_READ, ENOSPC),
              0)
        << std::strerror(errno);
    errno = 0;
    EXPECT_EQ(odin_udp_set_interest(state.u, ODIN_UDP_WRITE), -1);
    EXPECT_EQ(errno, ENOSPC);
    odin_event_io_t *io_after = nullptr;
    EXPECT_EQ(odin_udp_test_io(state.u, &io_after), 0) << std::strerror(errno);
    EXPECT_EQ(io_after, io0);
    ASSERT_EQ(odin_event_loop_test_kqueue_registered_mask(loop, fd0, &mask), 0)
        << std::strerror(errno);
    EXPECT_EQ(mask, ODIN_EVENT_READ);
    ASSERT_EQ(odin_udp_set_interest(state.u, ODIN_UDP_WRITE), 0)
        << std::strerror(errno);
    ASSERT_EQ(odin_event_loop_test_kqueue_registered_mask(loop, fd0, &mask), 0)
        << std::strerror(errno);
    EXPECT_EQ(mask, ODIN_EVENT_WRITE);
    odin_udp_close(state.u);
    odin_event_loop_destroy(loop);
  });
}

TEST(OdinUdpTest, T15) {
  std::string skip_reason;
  if (!ProbeIpv6Loopback(&skip_reason)) {
    GTEST_SKIP() << skip_reason;
  }
  UdpRunDeadline::Run([] {
    struct sockaddr_in6 peer_addr;
    int peer = -1;
    MakeUdp6Peer(&peer, &peer_addr);
    odin_event_loop_t *loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
    ReadyState state;
    state.loop = loop;
    struct sockaddr_in6 local = Loopback6(0);
    ASSERT_EQ(odin_udp_open(loop, reinterpret_cast<struct sockaddr *>(&local),
                            sizeof(local), RecvReadyCb, &state, &state.u),
              0)
        << std::strerror(errno);
    int fd0 = -1;
    struct sockaddr_in6 ep6;
    GetUdp6Endpoint(state.u, &fd0, &ep6);
    EXPECT_GE(fd0, 0);
    EXPECT_EQ(ep6.sin6_family, AF_INET6);
    ASSERT_EQ(odin_udp_set_interest(state.u, ODIN_UDP_READ), 0)
        << std::strerror(errno);
    ASSERT_EQ(sendto(peer, "v6", 2, 0,
                     reinterpret_cast<struct sockaddr *>(&ep6), sizeof(ep6)),
              2)
        << std::strerror(errno);
    ArmWatchdog(loop, &state);
    EXPECT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);

    EXPECT_EQ(state.calls, 1);
    EXPECT_NE(state.events & ODIN_UDP_READ, 0u);
    EXPECT_EQ(state.recv_rc, ODIN_UDP_OK);
    EXPECT_EQ(state.n, 2u);
    EXPECT_EQ(std::string(state.buf, 2), std::string("v6"));
    ExpectSource6(state.src, peer_addr.sin6_port);
    EXPECT_FALSE(state.timed_out);

    odin_udp_close(state.u);
    CloseFd(peer);
    odin_event_loop_destroy(loop);
  });
}

// NOLINTEND(misc-const-correctness, misc-use-internal-linkage)
