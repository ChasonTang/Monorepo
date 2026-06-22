// odin/testing/cli_client_loopback_alias_unittests.cpp
//
// RFC-024 T2 split out from odin_unittests because it needs an additional
// loopback address on macOS. Before running this binary on macOS, add:
//
//   sudo ifconfig lo0 alias 127.0.0.2/32
//
// To remove it after testing:
//
//   sudo ifconfig lo0 -alias 127.0.0.2

#include "odin/protocol.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "gtest/gtest.h"

extern "C" {
extern char **environ;
}

namespace {

constexpr int kShortDeadlineMs = 1500;
constexpr int kLongDeadlineMs = 4000;
constexpr char kHttp200[] = "HTTP/1.1 200 Connection Established\r\n\r\n";

std::string g_test_argv0;

class MutableArgv {
public:
  explicit MutableArgv(const std::vector<std::string> &tokens)
      : storage_(tokens) {
    for (auto &s : storage_) {
      ptrs_.push_back(&s[0]);
    }
    ptrs_.push_back(nullptr);
  }

  char *const *argv_terminated() { return ptrs_.data(); }

private:
  std::vector<std::string> storage_;
  std::vector<char *> ptrs_;
};

std::string Dirname(const std::string &path) {
  const auto pos = path.find_last_of('/');
  if (pos == std::string::npos) {
    return ".";
  }
  return path.substr(0, pos);
}

class ChildGuard {
public:
  explicit ChildGuard(pid_t pid) : pid_(pid) {}
  ~ChildGuard() {
    if (pid_ <= 0) {
      return;
    }
    int st = 0;
    const pid_t got = waitpid(pid_, &st, WNOHANG);
    if (got == 0) {
      kill(pid_, SIGKILL);
      (void)waitpid(pid_, &st, 0);
    }
  }
  void disarm() { pid_ = -1; }

private:
  pid_t pid_;
};

int SetNonblockFd(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

bool WaitFd(int fd, short events, int deadline_ms, short *out_revents) {
  const auto start = std::chrono::steady_clock::now();
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
    pfd.events = events;
    pfd.revents = 0;
    const int rc = poll(&pfd, 1, deadline_ms - elapsed);
    if (rc > 0) {
      if (out_revents != nullptr) {
        *out_revents = pfd.revents;
      }
      return true;
    }
    if (rc == 0) {
      return false;
    }
    if (errno != EINTR) {
      return false;
    }
  }
}

bool WriteAllDeadline(int fd, const void *buf, size_t len, int deadline_ms) {
  const uint8_t *p = static_cast<const uint8_t *>(buf);
  size_t off = 0;
  const auto start = std::chrono::steady_clock::now();
  while (off < len) {
    const ssize_t n = write(fd, p + off, len - off);
    if (n > 0) {
      off += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      const auto now = std::chrono::steady_clock::now();
      const int elapsed = static_cast<int>(
          std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
              .count());
      if (elapsed >= deadline_ms) {
        return false;
      }
      if (!WaitFd(fd, POLLOUT, deadline_ms - elapsed, nullptr)) {
        return false;
      }
      continue;
    }
    return false;
  }
  return true;
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
    if (!WaitFd(fd, POLLIN, deadline_ms - elapsed, nullptr)) {
      return static_cast<ssize_t>(off);
    }
    const ssize_t n = read(fd, p + off, len - off);
    if (n > 0) {
      off += static_cast<size_t>(n);
      continue;
    }
    if (n == 0) {
      return static_cast<ssize_t>(off);
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
      continue;
    }
    return -1;
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
    if (!WaitFd(fd, POLLIN, deadline_ms - elapsed, nullptr)) {
      return out;
    }
    char c = 0;
    const ssize_t n = read(fd, &c, 1);
    if (n == 1) {
      out.push_back(c);
      if (c == '\n') {
        return out;
      }
      continue;
    }
    if (n == 0) {
      return out;
    }
    if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
      return out;
    }
  }
}

int WaitChildBounded(pid_t pid, int deadline_ms, int *wstatus_out) {
  const auto start = std::chrono::steady_clock::now();
  while (true) {
    int st = 0;
    const pid_t got = waitpid(pid, &st, WNOHANG);
    if (got == pid) {
      if (wstatus_out != nullptr) {
        *wstatus_out = st;
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

int OpenIpv4Listener(const char *ip, uint16_t want_port, bool reuse,
                     uint16_t *out_port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  if (reuse) {
    const int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  }
  if (SetNonblockFd(fd) != 0) {
    const int saved = errno;
    close(fd);
    errno = saved;
    return -1;
  }
  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(want_port);
  if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
    close(fd);
    errno = EINVAL;
    return -1;
  }
  if (bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
    const int saved = errno;
    close(fd);
    errno = saved;
    return -1;
  }
  if (listen(fd, SOMAXCONN) != 0) {
    const int saved = errno;
    close(fd);
    errno = saved;
    return -1;
  }
  socklen_t len = sizeof(addr);
  if (getsockname(fd, reinterpret_cast<struct sockaddr *>(&addr), &len) != 0) {
    const int saved = errno;
    close(fd);
    errno = saved;
    return -1;
  }
  if (out_port != nullptr) {
    *out_port = ntohs(addr.sin_port);
  }
  return fd;
}

int TcpConnectIpv4(const char *ip, uint16_t port, int deadline_ms) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  if (SetNonblockFd(fd) != 0) {
    const int saved = errno;
    close(fd);
    errno = saved;
    return -1;
  }
  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
    close(fd);
    errno = EINVAL;
    return -1;
  }
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
  if (!WaitFd(fd, POLLOUT, deadline_ms, nullptr)) {
    close(fd);
    errno = ETIMEDOUT;
    return -1;
  }
  int soerr = 0;
  socklen_t slen = sizeof(soerr);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) != 0) {
    const int saved = errno;
    close(fd);
    errno = saved;
    return -1;
  }
  if (soerr != 0) {
    close(fd);
    errno = soerr;
    return -1;
  }
  return fd;
}

int TcpConnectLoopback(uint16_t port, int deadline_ms) {
  return TcpConnectIpv4("127.0.0.1", port, deadline_ms);
}

int AcceptWithDeadline(int listener, int deadline_ms) {
  const auto start = std::chrono::steady_clock::now();
  while (true) {
    const int fd = accept(listener, nullptr, nullptr);
    if (fd >= 0) {
      (void)SetNonblockFd(fd);
      return fd;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      return -1;
    }
    const auto now = std::chrono::steady_clock::now();
    const int elapsed = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
            .count());
    if (elapsed >= deadline_ms) {
      errno = ETIMEDOUT;
      return -1;
    }
    (void)WaitFd(listener, POLLIN, deadline_ms - elapsed, nullptr);
  }
}

void ExpectNoAccept(int listener, int deadline_ms) {
  struct pollfd pfd;
  pfd.fd = listener;
  pfd.events = POLLIN;
  pfd.revents = 0;
  const int rc = poll(&pfd, 1, deadline_ms);
  EXPECT_EQ(rc, 0) << "listener unexpectedly readable";
  if (rc > 0) {
    const int fd = accept(listener, nullptr, nullptr);
    if (fd >= 0) {
      close(fd);
    }
  }
}

bool ReadConnectReq(int fd, std::string *host, uint16_t *port) {
  uint8_t buf[ODIN_PROTO_CONNECT_REQ_MAX];
  size_t used = 0;
  while (used < sizeof(buf)) {
    const ssize_t n = ReadWithDeadline(fd, buf + used, 1, kShortDeadlineMs);
    if (n != 1) {
      return false;
    }
    used += 1;
    size_t consumed = 0;
    odin_proto_connect_req_view_t view;
    std::memset(&view, 0, sizeof(view));
    const odin_proto_status_t st =
        odin_proto_decode_connect_req(buf, used, &consumed, &view);
    if (st == ODIN_PROTO_NEED_MORE) {
      continue;
    }
    if (st != ODIN_PROTO_OK || consumed != used) {
      return false;
    }
    host->assign(reinterpret_cast<const char *>(buf + view.host_off),
                 view.host_len);
    *port = view.port;
    return true;
  }
  return false;
}

bool SendConnectRespOk(int fd) {
  odin_proto_connect_resp_frame_t resp;
  odin_proto_encode_connect_resp(0, &resp);
  return WriteAllDeadline(fd, resp.bytes, sizeof(resp.bytes),
                          kShortDeadlineMs);
}

bool ReadExactString(int fd, const char *s) {
  const size_t len = std::strlen(s);
  std::string buf(len, '\0');
  const ssize_t got =
      ReadWithDeadline(fd, &buf[0], buf.size(), kShortDeadlineMs);
  if (got != static_cast<ssize_t>(buf.size())) {
    return false;
  }
  return buf == s;
}

bool DrainUntilEofOrReset(int fd, int deadline_ms) {
  char buf[256];
  const auto start = std::chrono::steady_clock::now();
  while (true) {
    const ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      continue;
    }
    if (n == 0) {
      return true;
    }
    if (errno == ECONNRESET || errno == EPIPE) {
      return true;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      const auto now = std::chrono::steady_clock::now();
      const int elapsed = static_cast<int>(
          std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
              .count());
      if (elapsed >= deadline_ms) {
        return false;
      }
      if (!WaitFd(fd, POLLIN, deadline_ms - elapsed, nullptr)) {
        return false;
      }
      continue;
    }
    return false;
  }
}

bool ParseClientStartupLine(const std::string &line, uint16_t *out_port,
                            std::string *out_server) {
  static const char kPrefix[] = "odin: mode=client listen=";
  if (line.rfind(kPrefix, 0) != 0 || line.empty() || line.back() != '\n') {
    return false;
  }
  const size_t server_pos = line.find(" server=", std::strlen(kPrefix));
  if (server_pos == std::string::npos) {
    return false;
  }
  unsigned long port = 0;
  for (size_t i = std::strlen(kPrefix); i < server_pos; ++i) {
    const char c = line[i];
    if (c < '0' || c > '9') {
      return false;
    }
    port = port * 10u + static_cast<unsigned long>(c - '0');
    if (port > 65535u) {
      return false;
    }
  }
  *out_port = static_cast<uint16_t>(port);
  *out_server = line.substr(server_pos + 8, line.size() - server_pos - 9);
  return true;
}

struct SpawnedChild {
  pid_t pid;
  int stdout_fd;
  int stderr_fd;
};

SpawnedChild SpawnOdinClient(const std::string &client_path,
                             const std::vector<std::string> &args) {
  int out_pipe[2];
  int err_pipe[2];
  EXPECT_EQ(pipe(out_pipe), 0) << std::strerror(errno);
  EXPECT_EQ(pipe(err_pipe), 0) << std::strerror(errno);
  const pid_t pid = fork();
  if (pid == -1) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    return {-1, -1, -1};
  }
  if (pid == 0) {
    close(out_pipe[0]);
    close(err_pipe[0]);
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);
    close(out_pipe[1]);
    close(err_pipe[1]);
    std::vector<std::string> tokens;
    tokens.push_back(client_path);
    for (const auto &arg : args) {
      tokens.push_back(arg);
    }
    MutableArgv argv(tokens);
    execve(client_path.c_str(), argv.argv_terminated(), environ);
    _exit(127);
  }
  close(out_pipe[1]);
  close(err_pipe[1]);
  return {pid, out_pipe[0], err_pipe[0]};
}

void ExerciseOneSuccessfulConnect(uint16_t proxy_port, int fake_listener,
                                  const std::string &request,
                                  const std::string &expected_host,
                                  uint16_t expected_port, int *out_client,
                                  int *out_upstream) {
  *out_client = TcpConnectLoopback(proxy_port, kShortDeadlineMs);
  ASSERT_GE(*out_client, 0) << std::strerror(errno);
  ASSERT_TRUE(WriteAllDeadline(*out_client, request.data(), request.size(),
                               kShortDeadlineMs));
  *out_upstream = AcceptWithDeadline(fake_listener, kShortDeadlineMs);
  ASSERT_GE(*out_upstream, 0) << std::strerror(errno);
  std::string host;
  uint16_t port = 0;
  ASSERT_TRUE(ReadConnectReq(*out_upstream, &host, &port));
  EXPECT_EQ(host, expected_host);
  EXPECT_EQ(port, expected_port);
  ASSERT_TRUE(SendConnectRespOk(*out_upstream));
  EXPECT_TRUE(ReadExactString(*out_client, kHttp200));
}

} // namespace

TEST(OdinCliClientLoopbackAliasTest,
     SequentialConnectsUseConfiguredServerOnly) {
  (void)signal(SIGPIPE, SIG_IGN);
  ASSERT_FALSE(g_test_argv0.empty());
  const std::string client_path = Dirname(g_test_argv0) + "/odin-client";

  uint16_t upstream_port = 0;
  const int fake = OpenIpv4Listener("127.0.0.2", 0, true, &upstream_port);
  ASSERT_GE(fake, 0) << std::strerror(errno)
                     << "; on macOS add the required alias with: "
                        "sudo ifconfig lo0 alias 127.0.0.2/32";
  const int configured_sentry =
      OpenIpv4Listener("127.0.0.1", upstream_port, true, nullptr);
  ASSERT_GE(configured_sentry, 0) << std::strerror(errno);
  uint16_t target_port = 0;
  const int target_sentry =
      OpenIpv4Listener("127.0.0.1", 0, true, &target_port);
  ASSERT_GE(target_sentry, 0) << std::strerror(errno);
  ASSERT_NE(target_port, upstream_port);

  SpawnedChild child = SpawnOdinClient(
      client_path,
      {"--listen", "0", "--server",
       std::string("127.0.0.2:") + std::to_string(upstream_port)});
  ASSERT_NE(child.pid, -1);
  ChildGuard guard(child.pid);
  const std::string line = ReadLineWithDeadline(child.stderr_fd, 2000);
  uint16_t proxy_port = 0;
  std::string server;
  ASSERT_TRUE(ParseClientStartupLine(line, &proxy_port, &server)) << line;
  EXPECT_EQ(server, std::string("127.0.0.2:") + std::to_string(upstream_port));
  ASSERT_GT(proxy_port, 0);

  int client_a = -1;
  int upstream_a = -1;
  ExerciseOneSuccessfulConnect(proxy_port, fake,
                               "CONNECT example.com:443 HTTP/1.1\r\n\r\n",
                               "example.com", 443, &client_a, &upstream_a);
  ExpectNoAccept(configured_sentry, 100);
  ExpectNoAccept(target_sentry, 100);
  shutdown(client_a, SHUT_WR);
  shutdown(upstream_a, SHUT_WR);
  EXPECT_TRUE(DrainUntilEofOrReset(client_a, kShortDeadlineMs));
  EXPECT_TRUE(DrainUntilEofOrReset(upstream_a, kShortDeadlineMs));
  close(client_a);
  close(upstream_a);

  int client_b = -1;
  int upstream_b = -1;
  ExerciseOneSuccessfulConnect(
      proxy_port, fake,
      std::string("CONNECT 127.0.0.1:") + std::to_string(target_port) +
          " HTTP/1.1\r\n\r\n",
      "127.0.0.1", target_port, &client_b, &upstream_b);
  ExpectNoAccept(configured_sentry, 100);
  ExpectNoAccept(target_sentry, 100);
  close(client_b);
  close(upstream_b);

  EXPECT_EQ(kill(child.pid, SIGTERM), 0);
  int st = 0;
  ASSERT_EQ(WaitChildBounded(child.pid, kLongDeadlineMs, &st), 0);
  guard.disarm();
  EXPECT_TRUE(WIFEXITED(st));
  EXPECT_EQ(WEXITSTATUS(st), 0);
  close(child.stdout_fd);
  close(child.stderr_fd);
  close(fake);
  close(configured_sentry);
  close(target_sentry);
}

int main(int argc, char **argv) {
  if (argc > 0 && argv[0] != nullptr) {
    g_test_argv0 = argv[0];
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
