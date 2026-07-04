// odin/testing/cli_server_unittests.cpp
//
// RFC-022 §5 process-level (T1-T4) and unit-level (T5-T7) tests for the
// CLI server runner. Skipped unless ODIN_CLI_SERVER_RED=1 is set in the
// environment during P1; the gate is removed in P2 once the real runner
// lands and these rows assert for real.

#include "odin/cli.h"
#include "odin/cli_server.h"
#include "odin/event_loop.h"
#include "odin/protocol.h"
#include "odin/testing/cli_server_internal_test.h"
#include "odin/testing/event_loop_internal_test.h"
#include "odin/testing/server_runtime_internal_test.h"

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
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "gtest/gtest.h"

// NOLINTBEGIN(misc-const-correctness, misc-use-internal-linkage)

namespace {

void SetNonblock(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  ASSERT_NE(flags, -1) << std::strerror(errno);
  ASSERT_EQ(fcntl(fd, F_SETFL, flags | O_NONBLOCK), 0) << std::strerror(errno);
}

ssize_t ReadWithDeadline(int fd, void *buf, size_t len, int deadline_ms) {
  uint8_t *p = static_cast<uint8_t *>(buf);
  size_t off = 0;
  const auto start = std::chrono::steady_clock::now();
  while (off < len) {
    const auto now = std::chrono::steady_clock::now();
    const int elapsed = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
            .count());
    if (elapsed >= deadline_ms) {
      return static_cast<ssize_t>(off);
    }
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int prc = poll(&pfd, 1, deadline_ms - elapsed);
    if (prc <= 0) {
      return static_cast<ssize_t>(off);
    }
    const ssize_t n = read(fd, p + off, len - off);
    if (n == 0) {
      return static_cast<ssize_t>(off);
    }
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      return -1;
    }
    off += static_cast<size_t>(n);
  }
  return static_cast<ssize_t>(off);
}

std::string ReadLineWithDeadline(int fd, int deadline_ms) {
  std::string out;
  const auto start = std::chrono::steady_clock::now();
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    const int elapsed = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
            .count());
    if (elapsed >= deadline_ms) {
      return out;
    }
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int prc = poll(&pfd, 1, deadline_ms - elapsed);
    if (prc <= 0) {
      return out;
    }
    char c;
    const ssize_t n = read(fd, &c, 1);
    if (n == 0) {
      return out;
    }
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      return out;
    }
    out.push_back(c);
    if (c == '\n') {
      return out;
    }
  }
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

int OpenIpv4ListenerAny(uint16_t want_port, uint16_t *out_port) {
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
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(want_port);
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
  if (out_port != nullptr) {
    *out_port = ntohs(addr.sin_port);
  }
  return lfd;
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

int TcpConnectLoopback(uint16_t port, int deadline_ms) {
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
  const int rc =
      connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
  if (rc == 0) {
    return fd;
  }
  if (errno != EINPROGRESS) {
    const int saved = errno;
    close(fd);
    errno = saved;
    return -1;
  }
  struct pollfd pfd;
  pfd.fd = fd;
  pfd.events = POLLOUT;
  const int prc = poll(&pfd, 1, deadline_ms);
  if (prc <= 0) {
    close(fd);
    if (prc == 0) {
      errno = ETIMEDOUT;
    }
    return -1;
  }
  int so_err = 0;
  socklen_t slen = sizeof(so_err);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &slen) != 0) {
    const int saved = errno;
    close(fd);
    errno = saved;
    return -1;
  }
  if (so_err != 0) {
    close(fd);
    errno = so_err;
    return -1;
  }
  return fd;
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

bool ParseServerStartupLine(const std::string &line, uint16_t *out_port) {
  static const char kPrefix[] = "odin: mode=server listen=";
  if (line.rfind(kPrefix, 0) != 0) {
    return false;
  }
  if (line.empty() || line.back() != '\n') {
    return false;
  }
  const std::string num =
      line.substr(std::strlen(kPrefix), line.size() - std::strlen(kPrefix) - 1);
  if (num.empty()) {
    return false;
  }
  unsigned long port = 0;
  for (char c : num) {
    if (c < '0' || c > '9') {
      return false;
    }
    port = port * 10 + static_cast<unsigned long>(c - '0');
    if (port > 65535) {
      return false;
    }
  }
  *out_port = static_cast<uint16_t>(port);
  return true;
}

struct ChildHandle {
  pid_t pid;
  int stderr_fd;
};

int RunTcpServerConfig(uint16_t listen_port, FILE *err) {
  const odin_cli_server_config_t config = {
      listen_port,
      ODIN_CLI_SERVER_TRANSPORT_TCP,
      nullptr,
      nullptr,
  };
  return odin_cli_run_server(&config, err);
}

ChildHandle SpawnTcpServer(uint16_t listen_port) {
  int err_pipe[2];
  EXPECT_EQ(pipe(err_pipe), 0) << std::strerror(errno);
  const pid_t pid = fork();
  if (pid == -1) {
    close(err_pipe[0]);
    close(err_pipe[1]);
    return {-1, -1};
  }
  if (pid == 0) {
    close(err_pipe[0]);
    dup2(err_pipe[1], STDERR_FILENO);
    close(err_pipe[1]);
    const int rc = RunTcpServerConfig(listen_port, stderr);
    _exit(rc);
  }
  close(err_pipe[1]);
  return {pid, err_pipe[0]};
}

int WaitChildBounded(pid_t pid, int deadline_ms, int *wstatus_out) {
  const auto start = std::chrono::steady_clock::now();
  int wstatus = 0;
  while (true) {
    const pid_t got = waitpid(pid, &wstatus, WNOHANG);
    if (got == pid) {
      if (wstatus_out != nullptr) {
        *wstatus_out = wstatus;
      }
      return 0;
    }
    if (got == -1 && errno != EINTR) {
      return -1;
    }
    const auto now = std::chrono::steady_clock::now();
    const int elapsed = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
            .count());
    if (elapsed >= deadline_ms) {
      return -1;
    }
    usleep(10000);
  }
}

void KillAndReap(pid_t pid) {
  kill(pid, SIGKILL);
  int wstatus = 0;
  (void)waitpid(pid, &wstatus, 0);
}

} // namespace

// T1 - Ephemeral server startup reports actual port and becomes reachable.
TEST(OdinCliServerProcessTest, T1EphemeralStartupReportsActualPort) {
  ChildHandle child = SpawnTcpServer(0);
  ASSERT_NE(child.pid, -1);
  ASSERT_GE(child.stderr_fd, 0);

  const std::string line = ReadLineWithDeadline(child.stderr_fd, 2000);
  uint16_t port = 0;
  ASSERT_TRUE(ParseServerStartupLine(line, &port)) << "line=" << line;
  EXPECT_GT(port, 0);

  const int client = TcpConnectLoopback(port, 1500);
  if (client < 0) {
    KillAndReap(child.pid);
    close(child.stderr_fd);
    FAIL() << "TCP connect to listen=" << port
           << " failed: " << std::strerror(errno);
  }

  EXPECT_EQ(kill(child.pid, SIGTERM), 0);
  int wstatus = 0;
  if (WaitChildBounded(child.pid, 2000, &wstatus) != 0) {
    KillAndReap(child.pid);
    close(client);
    close(child.stderr_fd);
    FAIL() << "child did not exit within 2 s after SIGTERM";
  }
  close(client);
  close(child.stderr_fd);
  EXPECT_TRUE(WIFEXITED(wstatus));
  EXPECT_EQ(WEXITSTATUS(wstatus), 0);
}

// T2 - SIGINT graceful shutdown.
TEST(OdinCliServerProcessTest, T2SigintGracefulShutdown) {
  ChildHandle child = SpawnTcpServer(0);
  ASSERT_NE(child.pid, -1);

  const std::string line = ReadLineWithDeadline(child.stderr_fd, 2000);
  uint16_t port = 0;
  ASSERT_TRUE(ParseServerStartupLine(line, &port)) << "line=" << line;

  // Pre-signal: child must still be running and listener must accept.
  int wstatus = 0;
  ASSERT_EQ(waitpid(child.pid, &wstatus, WNOHANG), 0)
      << "child exited before signal: status=" << wstatus;
  const int pre_client = TcpConnectLoopback(port, 1500);
  if (pre_client < 0) {
    KillAndReap(child.pid);
    close(child.stderr_fd);
    FAIL() << "pre-SIGINT connect to listen=" << port << " failed";
  }
  close(pre_client);

  EXPECT_EQ(kill(child.pid, SIGINT), 0);
  if (WaitChildBounded(child.pid, 2000, &wstatus) != 0) {
    KillAndReap(child.pid);
    close(child.stderr_fd);
    FAIL() << "child did not exit within 2 s after SIGINT";
  }
  EXPECT_TRUE(WIFEXITED(wstatus));
  EXPECT_EQ(WEXITSTATUS(wstatus), 0);

  std::string remaining = ReadLineWithDeadline(child.stderr_fd, 200);
  EXPECT_EQ(remaining.find("startup failed"), std::string::npos)
      << "extra stderr=" << remaining;

  const int post = TcpConnectLoopback(port, 500);
  if (post >= 0) {
    close(post);
    ADD_FAILURE() << "post-exit connect unexpectedly succeeded";
  }
  close(child.stderr_fd);
}

// T3 - Bind collision returns 1 and does not disturb the existing listener.
TEST(OdinCliServerProcessTest, T3BindCollisionDoesNotDisturb) {
  uint16_t port = 0;
  const int parent_listener = OpenIpv4ListenerAny(0, &port);
  ASSERT_GE(parent_listener, 0) << std::strerror(errno);

  ChildHandle child = SpawnTcpServer(port);
  ASSERT_NE(child.pid, -1);

  int wstatus = 0;
  if (WaitChildBounded(child.pid, 3000, &wstatus) != 0) {
    KillAndReap(child.pid);
    close(child.stderr_fd);
    close(parent_listener);
    FAIL() << "child did not exit within 3 s on bind collision";
  }
  EXPECT_TRUE(WIFEXITED(wstatus));
  EXPECT_EQ(WEXITSTATUS(wstatus), 1);

  std::string stderr_buf;
  char buf[256];
  while (true) {
    const ssize_t n = read(child.stderr_fd, buf, sizeof(buf));
    if (n <= 0) {
      break;
    }
    stderr_buf.append(buf, static_cast<size_t>(n));
  }
  EXPECT_EQ(stderr_buf, "odin: server startup failed at bind\n");
  close(child.stderr_fd);

  const int peer = TcpConnectLoopback(port, 1500);
  ASSERT_GE(peer, 0) << "parent listener stopped accepting after child exit: "
                     << std::strerror(errno);
  close(peer);
  close(parent_listener);
}

// T4-deny - Default CLI filter denies loopback to upstream.
TEST(OdinCliServerProcessTest, T4DenyLoopbackUpstream) {
  (void)signal(SIGPIPE, SIG_IGN);

  uint16_t upstream_port = 0;
  const int upstream_listener = OpenLoopbackListener(&upstream_port);
  ASSERT_GE(upstream_listener, 0) << std::strerror(errno);

  ChildHandle child = SpawnTcpServer(0);
  ASSERT_NE(child.pid, -1);

  const std::string line = ReadLineWithDeadline(child.stderr_fd, 2000);
  uint16_t listen_port = 0;
  ASSERT_TRUE(ParseServerStartupLine(line, &listen_port)) << "line=" << line;

  const int client = TcpConnectLoopback(listen_port, 1500);
  ASSERT_GE(client, 0) << std::strerror(errno);

  const std::string req = EncodedReq("127.0.0.1", upstream_port);
  ASSERT_TRUE(WriteAll(client, req.data(), req.size()));

  uint8_t resp[4] = {0};
  const ssize_t got = ReadWithDeadline(client, resp, sizeof(resp), 2000);
  EXPECT_EQ(got, 4);
  if (got == 4) {
    EXPECT_EQ(resp[0], 0x01);
    EXPECT_EQ(resp[1], 0x02);
    EXPECT_EQ(resp[2], 0x00);
    EXPECT_EQ(resp[3], 0x04);
  }

  // Upstream listener must observe zero accepted connections (probe 200 ms).
  struct pollfd pfd;
  pfd.fd = upstream_listener;
  pfd.events = POLLIN;
  const int prc = poll(&pfd, 1, 200);
  EXPECT_EQ(prc, 0) << "upstream listener saw connection";

  close(client);
  EXPECT_EQ(kill(child.pid, SIGTERM), 0);
  int wstatus = 0;
  if (WaitChildBounded(child.pid, 2000, &wstatus) != 0) {
    KillAndReap(child.pid);
    close(child.stderr_fd);
    close(upstream_listener);
    FAIL() << "child did not exit within 2 s after SIGTERM";
  }
  close(child.stderr_fd);
  close(upstream_listener);
  EXPECT_TRUE(WIFEXITED(wstatus));
  EXPECT_EQ(WEXITSTATUS(wstatus), 0);
}

// T4-allow - Forked child; public IPv4 reaches dial boundary via probe.
TEST(OdinCliServerProcessTest, T4AllowPublicIpv4ReachesDialBoundary) {
  (void)signal(SIGPIPE, SIG_IGN);

  int stderr_pipe[2];
  int probe_pipe[2];
  int port_pipe[2];
  ASSERT_EQ(pipe(stderr_pipe), 0);
  ASSERT_EQ(pipe(probe_pipe), 0);
  ASSERT_EQ(pipe(port_pipe), 0);

  const pid_t pid = fork();
  ASSERT_NE(pid, -1);
  if (pid == 0) {
    close(stderr_pipe[0]);
    close(probe_pipe[0]);
    close(port_pipe[0]);
    dup2(stderr_pipe[1], STDERR_FILENO);
    odin_cli_server_test_set_dial_start_probe_fd(probe_pipe[1], ETIMEDOUT);
    const int rc = RunTcpServerConfig(0, stderr);
    (void)write(port_pipe[1], &rc, sizeof(rc));
    _exit(rc);
  }
  close(stderr_pipe[1]);
  close(probe_pipe[1]);
  close(port_pipe[1]);

  const std::string line = ReadLineWithDeadline(stderr_pipe[0], 2000);
  uint16_t listen_port = 0;
  if (!ParseServerStartupLine(line, &listen_port)) {
    KillAndReap(pid);
    close(stderr_pipe[0]);
    close(probe_pipe[0]);
    close(port_pipe[0]);
    FAIL() << "no startup line; line=" << line;
  }

  const int client = TcpConnectLoopback(listen_port, 1500);
  if (client < 0) {
    KillAndReap(pid);
    close(stderr_pipe[0]);
    close(probe_pipe[0]);
    close(port_pipe[0]);
    FAIL() << "TCP connect failed: " << std::strerror(errno);
  }

  const std::string req = EncodedReq("93.184.216.34", 443);
  ASSERT_TRUE(WriteAll(client, req.data(), req.size()));

  odin_cli_server_test_dial_start_t rec = {};
  const ssize_t got_probe =
      ReadWithDeadline(probe_pipe[0], &rec, sizeof(rec), 2000);
  EXPECT_EQ(got_probe, static_cast<ssize_t>(sizeof(rec)));
  if (got_probe == static_cast<ssize_t>(sizeof(rec))) {
    EXPECT_EQ(rec.family, AF_INET);
    EXPECT_EQ(rec.ipv4_addr_nbo, inet_addr("93.184.216.34"));
    EXPECT_EQ(rec.port_nbo, htons(443));
  }

  uint8_t resp[4] = {0};
  const ssize_t got_resp = ReadWithDeadline(client, resp, sizeof(resp), 2000);
  EXPECT_EQ(got_resp, 4);
  if (got_resp == 4) {
    EXPECT_EQ(resp[0], 0x01);
    EXPECT_EQ(resp[1], 0x02);
    EXPECT_EQ(resp[2], 0x00);
    EXPECT_EQ(resp[3], 0x03);
  }

  close(client);
  EXPECT_EQ(kill(pid, SIGTERM), 0);
  int wstatus = 0;
  if (WaitChildBounded(pid, 2000, &wstatus) != 0) {
    KillAndReap(pid);
    close(stderr_pipe[0]);
    close(probe_pipe[0]);
    close(port_pipe[0]);
    FAIL() << "child did not exit within 2 s";
  }
  close(stderr_pipe[0]);
  close(probe_pipe[0]);
  close(port_pipe[0]);
  EXPECT_TRUE(WIFEXITED(wstatus));
  EXPECT_EQ(WEXITSTATUS(wstatus), 0);
}

// T5 - Default filter deny and allow matrix via the test mirror.
TEST(OdinCliServerUnitTest, T5DefaultFilterDenyAllowMatrix) {

  auto MakeIpv4 = [](const char *ip) {
    struct sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(443);
    EXPECT_EQ(inet_pton(AF_INET, ip, &sa.sin_addr), 1);
    return sa;
  };

  const char *const kDenied[] = {
      "0.0.0.0",         "0.255.255.255",   "10.0.0.0",
      "10.1.2.3",        "10.255.255.255",  "100.64.0.0",
      "100.127.255.255", "127.0.0.0",       "127.0.0.1",
      "127.255.255.255", "169.254.0.0",     "169.254.1.1",
      "169.254.169.254", "169.254.255.255", "172.16.0.0",
      "172.31.255.255",  "192.0.0.0",       "192.0.0.1",
      "192.0.0.255",     "192.0.2.0",       "192.0.2.1",
      "192.0.2.255",     "192.168.0.0",     "192.168.0.1",
      "192.168.255.255", "198.18.0.0",      "198.19.255.255",
      "198.51.100.0",    "198.51.100.1",    "198.51.100.255",
      "203.0.113.0",     "203.0.113.1",     "203.0.113.255",
      "224.0.0.0",       "224.0.0.1",       "239.255.255.255",
      "240.0.0.0",       "240.0.0.1",       "255.255.255.255",
  };
  for (const char *ip : kDenied) {
    const struct sockaddr_in sa = MakeIpv4(ip);
    const int rc = odin_cli_server_test_default_dial_filter(
        reinterpret_cast<const struct sockaddr *>(&sa), sizeof(sa));
    EXPECT_EQ(rc, EACCES) << "ip=" << ip;
  }

  const char *const kAllowed[] = {
      "1.0.0.0",      "9.255.255.255",   "11.0.0.0",    "100.63.255.255",
      "100.128.0.0",  "126.255.255.255", "128.0.0.0",   "169.253.255.255",
      "169.255.0.0",  "172.15.255.255",  "172.32.0.0",  "191.255.255.255",
      "192.0.1.0",    "192.0.1.255",     "192.0.3.0",   "192.167.255.255",
      "192.169.0.0",  "198.17.255.255",  "198.20.0.0",  "198.51.99.255",
      "198.51.101.0", "203.0.112.255",   "203.0.114.0", "223.255.255.255",
      "8.8.8.8",      "93.184.216.34",
  };
  for (const char *ip : kAllowed) {
    const struct sockaddr_in sa = MakeIpv4(ip);
    const int rc = odin_cli_server_test_default_dial_filter(
        reinterpret_cast<const struct sockaddr *>(&sa), sizeof(sa));
    EXPECT_EQ(rc, 0) << "ip=" << ip;
  }

  // NULL addr.
  EXPECT_EQ(
      odin_cli_server_test_default_dial_filter(nullptr, sizeof(sockaddr_in)),
      EAFNOSUPPORT);

  // Short addrlen on a valid IPv4 sockaddr.
  const struct sockaddr_in short_sa = MakeIpv4("8.8.8.8");
  EXPECT_EQ(odin_cli_server_test_default_dial_filter(
                reinterpret_cast<const struct sockaddr *>(&short_sa),
                sizeof(short_sa) - 1),
            EAFNOSUPPORT);

  // AF_INET6 family.
  struct sockaddr_in6 sa6;
  std::memset(&sa6, 0, sizeof(sa6));
  sa6.sin6_family = AF_INET6;
  sa6.sin6_port = htons(443);
  EXPECT_EQ(odin_cli_server_test_default_dial_filter(
                reinterpret_cast<const struct sockaddr *>(&sa6), sizeof(sa6)),
            EAFNOSUPPORT);
}

namespace {

struct FailpointCase {
  const char *line;
  odin_cli_server_test_failpoint_t fp;
  bool post_banner;
};

const FailpointCase kFailpoints[] = {
    {"odin: server startup failed at socket\n",
     ODIN_CLI_SERVER_TEST_FAIL_SOCKET, false},
    {"odin: server startup failed at setsockopt(SO_REUSEADDR)\n",
     ODIN_CLI_SERVER_TEST_FAIL_SETSOCKOPT_REUSEADDR, false},
    {"odin: server startup failed at fcntl(F_GETFL)\n",
     ODIN_CLI_SERVER_TEST_FAIL_FCNTL_GETFL, false},
    {"odin: server startup failed at fcntl(F_SETFL)\n",
     ODIN_CLI_SERVER_TEST_FAIL_FCNTL_SETFL, false},
    {"odin: server startup failed at bind\n", ODIN_CLI_SERVER_TEST_FAIL_BIND,
     false},
    {"odin: server startup failed at listen\n",
     ODIN_CLI_SERVER_TEST_FAIL_LISTEN, false},
    {"odin: server startup failed at getsockname\n",
     ODIN_CLI_SERVER_TEST_FAIL_GETSOCKNAME, false},
    {"odin: server startup failed at event_loop_create\n",
     ODIN_CLI_SERVER_TEST_FAIL_EVENT_LOOP_CREATE, false},
    {"odin: server startup failed at server_runtime_create\n",
     ODIN_CLI_SERVER_TEST_FAIL_SERVER_RUNTIME_CREATE, false},
    {"odin: server startup failed at sigaction(SIGINT)\n",
     ODIN_CLI_SERVER_TEST_FAIL_SIGACTION_SIGINT, false},
    {"odin: server startup failed at sigaction(SIGTERM)\n",
     ODIN_CLI_SERVER_TEST_FAIL_SIGACTION_SIGTERM, false},
    {"odin: server startup failed at signal_timer_start\n",
     ODIN_CLI_SERVER_TEST_FAIL_SIGNAL_TIMER_START, false},
    {"odin: server runtime failed at event_loop_run\n",
     ODIN_CLI_SERVER_TEST_FAIL_EVENT_LOOP_RUN, true},
    {"odin: server runtime failed at accept_loop\n",
     ODIN_CLI_SERVER_TEST_TRIGGER_RUNTIME_ERROR, true},
};

uint16_t AllocTempPort() {
  uint16_t p = 0;
  const int lfd = OpenIpv4ListenerAny(0, &p);
  if (lfd >= 0) {
    close(lfd);
  }
  return p;
}

} // namespace

// T6 - Setup failure cleanup matrix.
TEST(OdinCliServerUnitTest, T6SetupFailureCleanupMatrix) {
  (void)signal(SIGPIPE, SIG_IGN);

  // Invalid failpoint inputs return -1/EINVAL and do not arm.
  odin_cli_server_test_liveness_t snap{};
  odin_event_loop_test_liveness_t es{};
  ASSERT_EQ(odin_cli_server_test_liveness(&snap), 0);
  ASSERT_EQ(odin_event_loop_test_liveness(&es), 0);
  errno = 0;
  EXPECT_EQ(odin_cli_server_test_fail_next(
                // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
                static_cast<odin_cli_server_test_failpoint_t>(0), EIO),
            -1);
  EXPECT_EQ(errno, EINVAL);
  errno = 0;
  EXPECT_EQ(odin_cli_server_test_fail_next(ODIN_CLI_SERVER_TEST_FAIL_SOCKET, 0),
            -1);
  EXPECT_EQ(errno, EINVAL);
  odin_cli_server_test_liveness_t snap2{};
  odin_event_loop_test_liveness_t es2{};
  EXPECT_EQ(odin_cli_server_test_liveness(&snap2), 0);
  EXPECT_EQ(odin_event_loop_test_liveness(&es2), 0);
  EXPECT_EQ(snap2.live_listeners, snap.live_listeners);
  EXPECT_EQ(snap2.live_runtimes, snap.live_runtimes);
  EXPECT_EQ(es2.loops, es.loops);
  EXPECT_EQ(es2.io_handles, es.io_handles);
  EXPECT_EQ(es2.timers, es.timers);
  EXPECT_EQ(es2.task_nodes, es.task_nodes);

  for (const auto &fc : kFailpoints) {
    const uint16_t port = AllocTempPort();
    odin_cli_server_test_reset_liveness();
    odin_event_loop_test_reset_liveness();
    ASSERT_EQ(odin_cli_server_test_fail_next(fc.fp, EIO), 0);

    if (!fc.post_banner) {
      char err_buf[512] = {0};
      FILE *err = fmemopen(err_buf, sizeof(err_buf), "w");
      ASSERT_NE(err, nullptr);
      const int rc = RunTcpServerConfig(port, err);
      (void)fclose(err);
      EXPECT_EQ(rc, 1) << "fp=" << static_cast<int>(fc.fp);
      EXPECT_STREQ(err_buf, fc.line) << "fp=" << static_cast<int>(fc.fp);

      odin_cli_server_test_liveness_t live{};
      odin_event_loop_test_liveness_t elive{};
      EXPECT_EQ(odin_cli_server_test_liveness(&live), 0);
      EXPECT_EQ(odin_event_loop_test_liveness(&elive), 0);
      EXPECT_EQ(live.live_listeners, 0u);
      EXPECT_EQ(live.live_runtimes, 0u);
      EXPECT_EQ(elive.loops, 0u);
      EXPECT_EQ(elive.io_handles, 0u);
      EXPECT_EQ(elive.timers, 0u);
      EXPECT_EQ(elive.task_nodes, 0u);

      if (fc.fp == ODIN_CLI_SERVER_TEST_FAIL_LISTEN ||
          fc.fp == ODIN_CLI_SERVER_TEST_FAIL_GETSOCKNAME ||
          fc.fp == ODIN_CLI_SERVER_TEST_FAIL_EVENT_LOOP_CREATE ||
          fc.fp == ODIN_CLI_SERVER_TEST_FAIL_SERVER_RUNTIME_CREATE ||
          fc.fp == ODIN_CLI_SERVER_TEST_FAIL_SIGACTION_SIGINT ||
          fc.fp == ODIN_CLI_SERVER_TEST_FAIL_SIGACTION_SIGTERM ||
          fc.fp == ODIN_CLI_SERVER_TEST_FAIL_SIGNAL_TIMER_START) {
        struct sockaddr_in last{};
        EXPECT_EQ(odin_cli_server_test_last_bind_addr(&last), 0);
        EXPECT_EQ(last.sin_addr.s_addr, htonl(INADDR_ANY));
        EXPECT_EQ(ntohs(last.sin_port), port);
      }

      // A fresh connect to the requested port must fail (no listener left).
      const int probe = TcpConnectLoopback(port, 300);
      if (probe >= 0) {
        close(probe);
        ADD_FAILURE() << "fp=" << static_cast<int>(fc.fp) << " left a listener";
      }
    } else {
      // Post-banner: fork. Child writes liveness snapshot then pauses on a
      // release pipe before exiting so the parent can prove pre-exit cleanup.
      int snap_pipe[2];
      int release_pipe[2];
      int err_pipe[2];
      ASSERT_EQ(pipe(snap_pipe), 0);
      ASSERT_EQ(pipe(release_pipe), 0);
      ASSERT_EQ(pipe(err_pipe), 0);
      const pid_t pid = fork();
      ASSERT_NE(pid, -1);
      if (pid == 0) {
        close(snap_pipe[0]);
        close(release_pipe[1]);
        close(err_pipe[0]);
        dup2(err_pipe[1], STDERR_FILENO);
        const int rc = RunTcpServerConfig(port, stderr);
        odin_cli_server_test_liveness_t live{};
        odin_event_loop_test_liveness_t elive{};
        (void)odin_cli_server_test_liveness(&live);
        (void)odin_event_loop_test_liveness(&elive);
        (void)write(snap_pipe[1], &rc, sizeof(rc));
        (void)write(snap_pipe[1], &live, sizeof(live));
        (void)write(snap_pipe[1], &elive, sizeof(elive));
        char go = 0;
        (void)read(release_pipe[0], &go, 1);
        _exit(rc);
      }
      close(snap_pipe[1]);
      close(release_pipe[0]);
      close(err_pipe[1]);
      int rc = 0;
      odin_cli_server_test_liveness_t live{};
      odin_event_loop_test_liveness_t elive{};
      const ssize_t r0 = ReadWithDeadline(snap_pipe[0], &rc, sizeof(rc), 3000);
      const ssize_t r1 =
          ReadWithDeadline(snap_pipe[0], &live, sizeof(live), 1000);
      const ssize_t r2 =
          ReadWithDeadline(snap_pipe[0], &elive, sizeof(elive), 1000);
      EXPECT_EQ(r0, static_cast<ssize_t>(sizeof(rc)));
      EXPECT_EQ(r1, static_cast<ssize_t>(sizeof(live)));
      EXPECT_EQ(r2, static_cast<ssize_t>(sizeof(elive)));
      EXPECT_EQ(rc, 1);

      int ws = 0;
      EXPECT_EQ(waitpid(pid, &ws, WNOHANG), 0);
      const int probe = TcpConnectLoopback(port, 300);
      if (probe >= 0) {
        close(probe);
        ADD_FAILURE() << "fp=" << static_cast<int>(fc.fp)
                      << " left listener while paused";
      }
      EXPECT_EQ(live.live_listeners, 0u);
      EXPECT_EQ(live.live_runtimes, 0u);
      EXPECT_EQ(elive.loops, 0u);
      EXPECT_EQ(elive.io_handles, 0u);
      EXPECT_EQ(elive.timers, 0u);
      EXPECT_EQ(elive.task_nodes, 0u);

      char go = 1;
      (void)write(release_pipe[1], &go, 1);
      int wstatus = 0;
      WaitChildBounded(pid, 2000, &wstatus);
      close(snap_pipe[0]);
      close(release_pipe[1]);
      // Drain stderr for visibility on failures (avoid blocking).
      std::string s;
      char buf[256];
      while (true) {
        const ssize_t n = read(err_pipe[0], buf, sizeof(buf));
        if (n <= 0) {
          break;
        }
        s.append(buf, static_cast<size_t>(n));
      }
      close(err_pipe[0]);
      EXPECT_NE(s.find(fc.line), std::string::npos)
          << "fp=" << static_cast<int>(fc.fp) << " stderr=" << s;
    }
  }
}

// T7 - Graceful shutdown cleans up before child exit and restores handlers.
TEST(OdinCliServerUnitTest, T7GracefulShutdownCleanupAndHandlers) {
  (void)signal(SIGPIPE, SIG_IGN);

  int stderr_pipe[2];
  int progress_pipe[2];
  int snap_pipe[2];
  int release_pipe[2];
  ASSERT_EQ(pipe(stderr_pipe), 0);
  ASSERT_EQ(pipe(progress_pipe), 0);
  ASSERT_EQ(pipe(snap_pipe), 0);
  ASSERT_EQ(pipe(release_pipe), 0);

  static std::atomic<int> g_sigint_count{0};
  static std::atomic<int> g_sigterm_count{0};

  const pid_t pid = fork();
  ASSERT_NE(pid, -1);
  if (pid == 0) {
    close(stderr_pipe[0]);
    close(progress_pipe[0]);
    close(snap_pipe[0]);
    close(release_pipe[1]);
    dup2(stderr_pipe[1], STDERR_FILENO);
    odin_cli_server_test_reset_liveness();
    odin_event_loop_test_reset_liveness();
    odin_cli_server_test_set_progress_fd(progress_pipe[1]);

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = [](int s) {
      if (s == SIGINT) {
        g_sigint_count.fetch_add(1);
      } else if (s == SIGTERM) {
        g_sigterm_count.fetch_add(1);
      }
    };
    sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGINT, &sa, nullptr);
    (void)sigaction(SIGTERM, &sa, nullptr);

    const int rc = RunTcpServerConfig(0, stderr);

    odin_cli_server_test_liveness_t live{};
    odin_event_loop_test_liveness_t elive{};
    (void)odin_cli_server_test_liveness(&live);
    (void)odin_event_loop_test_liveness(&elive);

    (void)raise(SIGINT);
    (void)raise(SIGTERM);
    const int sint = g_sigint_count.load();
    const int sterm = g_sigterm_count.load();

    (void)write(snap_pipe[1], &rc, sizeof(rc));
    (void)write(snap_pipe[1], &sint, sizeof(sint));
    (void)write(snap_pipe[1], &sterm, sizeof(sterm));
    (void)write(snap_pipe[1], &live, sizeof(live));
    (void)write(snap_pipe[1], &elive, sizeof(elive));

    char go = 0;
    (void)read(release_pipe[0], &go, 1);
    _exit(rc);
  }
  close(stderr_pipe[1]);
  close(progress_pipe[1]);
  close(snap_pipe[1]);
  close(release_pipe[0]);

  const std::string line = ReadLineWithDeadline(stderr_pipe[0], 2000);
  uint16_t listen_port = 0;
  if (!ParseServerStartupLine(line, &listen_port)) {
    KillAndReap(pid);
    close(stderr_pipe[0]);
    close(progress_pipe[0]);
    close(snap_pipe[0]);
    close(release_pipe[1]);
    FAIL() << "no startup line; got=" << line;
  }

  const int idle_client = TcpConnectLoopback(listen_port, 1500);
  if (idle_client < 0) {
    KillAndReap(pid);
    close(stderr_pipe[0]);
    close(progress_pipe[0]);
    close(snap_pipe[0]);
    close(release_pipe[1]);
    FAIL() << "idle client connect: " << std::strerror(errno);
  }

  uint8_t progress = 0;
  const ssize_t pg = ReadWithDeadline(progress_pipe[0], &progress, 1, 2000);
  EXPECT_EQ(pg, 1);

  // Pre-signal: snap_pipe has no data and child has not exited.
  struct pollfd pfd;
  pfd.fd = snap_pipe[0];
  pfd.events = POLLIN;
  EXPECT_EQ(poll(&pfd, 1, 0), 0) << "snap pipe had data before SIGTERM";
  int ws = 0;
  EXPECT_EQ(waitpid(pid, &ws, WNOHANG), 0);

  EXPECT_EQ(kill(pid, SIGTERM), 0);

  int rc = -1;
  int sint = -1;
  int sterm = -1;
  odin_cli_server_test_liveness_t live{};
  odin_event_loop_test_liveness_t elive{};
  EXPECT_EQ(ReadWithDeadline(snap_pipe[0], &rc, sizeof(rc), 3000),
            static_cast<ssize_t>(sizeof(rc)));
  EXPECT_EQ(ReadWithDeadline(snap_pipe[0], &sint, sizeof(sint), 1000),
            static_cast<ssize_t>(sizeof(sint)));
  EXPECT_EQ(ReadWithDeadline(snap_pipe[0], &sterm, sizeof(sterm), 1000),
            static_cast<ssize_t>(sizeof(sterm)));
  EXPECT_EQ(ReadWithDeadline(snap_pipe[0], &live, sizeof(live), 1000),
            static_cast<ssize_t>(sizeof(live)));
  EXPECT_EQ(ReadWithDeadline(snap_pipe[0], &elive, sizeof(elive), 1000),
            static_cast<ssize_t>(sizeof(elive)));
  EXPECT_EQ(rc, 0);
  EXPECT_EQ(sint, 1);
  EXPECT_EQ(sterm, 1);
  EXPECT_EQ(live.live_listeners, 0u);
  EXPECT_EQ(live.live_runtimes, 0u);
  EXPECT_GE(live.last_cleanup_runtime_inflight, 1u);
  EXPECT_EQ(elive.loops, 0u);
  EXPECT_EQ(elive.io_handles, 0u);
  EXPECT_EQ(elive.timers, 0u);
  EXPECT_EQ(elive.task_nodes, 0u);

  // Child is paused on release pipe — listener must already be torn down.
  EXPECT_EQ(waitpid(pid, &ws, WNOHANG), 0);
  const int probe = TcpConnectLoopback(listen_port, 300);
  if (probe >= 0) {
    close(probe);
    ADD_FAILURE() << "listener still accepting while child paused";
  }
  uint8_t idle_byte = 0;
  const ssize_t r = ReadWithDeadline(idle_client, &idle_byte, 1, 1000);
  EXPECT_LE(r, 0);
  close(idle_client);

  // Release child.
  char go = 1;
  (void)write(release_pipe[1], &go, 1);
  int wstatus = 0;
  WaitChildBounded(pid, 2000, &wstatus);

  std::string rest;
  char buf[256];
  while (true) {
    const ssize_t n = read(stderr_pipe[0], buf, sizeof(buf));
    if (n <= 0) {
      break;
    }
    rest.append(buf, static_cast<size_t>(n));
  }
  EXPECT_EQ(rest.find("startup failed"), std::string::npos) << "extra=" << rest;
  close(stderr_pipe[0]);
  close(progress_pipe[0]);
  close(snap_pipe[0]);
  close(release_pipe[1]);
}

// NOLINTEND(misc-const-correctness, misc-use-internal-linkage)
