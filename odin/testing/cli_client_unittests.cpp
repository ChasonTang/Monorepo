// odin/testing/cli_client_unittests.cpp
//
// RFC-024 §5 process-level and unit-level tests for the CLI client runner.

#include "odin/cli.h"
#include "odin/cli_client.h"
#include "odin/protocol.h"
#include "odin/testing/cli_client_internal_test.h"
#include "odin/testing/client_session_internal_test.h"
#include "odin/testing/client_xqc_runtime_internal_test.h"
#include "odin/testing/event_loop_internal_test.h"
#include "odin/testing/xqc_udp_internal_test.h"

#include <arpa/inet.h>
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
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "gtest/gtest.h"

// NOLINTBEGIN(misc-const-correctness, misc-use-internal-linkage)

namespace {

constexpr int kShortDeadlineMs = 1500;
constexpr int kLongDeadlineMs = 4000;
constexpr char kHttp200[] = "HTTP/1.1 200 Connection Established\r\n\r\n";

volatile sig_atomic_t g_child_sigint_count = 0;
volatile sig_atomic_t g_child_sigterm_count = 0;

void CountingSigint(int) { g_child_sigint_count += 1; }
void CountingSigterm(int) { g_child_sigterm_count += 1; }

class MutableArgv {
public:
  explicit MutableArgv(const std::vector<std::string> &tokens)
      : storage_(tokens) {
    rebuild_ptrs();
  }

  MutableArgv(std::initializer_list<const char *> tokens) {
    for (const char *t : tokens) {
      storage_.emplace_back(t);
    }
    rebuild_ptrs();
  }

  int argc() const { return static_cast<int>(storage_.size()); }
  char *const *argv() { return ptrs_.data(); }
  char *const *argv_terminated() {
    if (ptrs_.empty() || ptrs_.back() != nullptr) {
      ptrs_.push_back(nullptr);
    }
    return ptrs_.data();
  }

private:
  void rebuild_ptrs() {
    ptrs_.clear();
    for (auto &s : storage_) {
      ptrs_.push_back(&s[0]);
    }
  }

  std::vector<std::string> storage_;
  std::vector<char *> ptrs_;
};

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

void CloseIfOpen(int *fd) {
  if (*fd >= 0) {
    close(*fd);
    *fd = -1;
  }
}

int SetNonblockFd(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

bool SetNonblock(int fd) {
  if (SetNonblockFd(fd) != 0) {
    ADD_FAILURE() << "fcntl(O_NONBLOCK): " << std::strerror(errno);
    return false;
  }
  return true;
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

std::string ReadAllAvailable(int fd, int quiet_deadline_ms) {
  std::string out;
  char buf[512];
  while (WaitFd(fd, POLLIN, quiet_deadline_ms, nullptr)) {
    const ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      out.append(buf, static_cast<size_t>(n));
      continue;
    }
    if (n == 0) {
      break;
    }
    if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
      break;
    }
  }
  return out;
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

uint16_t AllocFreeLoopbackPort() {
  uint16_t port = 0;
  const int fd = OpenIpv4Listener("127.0.0.1", 0, false, &port);
  if (fd >= 0) {
    close(fd);
  }
  return port;
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
      (void)SetNonblock(fd);
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
    odin_proto_connect_req_view_t view{};
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
  return WriteAllDeadline(fd, resp.bytes, sizeof(resp.bytes), kShortDeadlineMs);
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

bool DrainUntilEofOrResetExpectingNoBytes(int fd, int deadline_ms) {
  char buf[256];
  const auto start = std::chrono::steady_clock::now();
  while (true) {
    const ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      return false;
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

int RunTcpClientConfig(uint16_t listen_port, const char *server_host,
                       uint16_t server_port, FILE *err) {
  const odin_cli_client_config_t config = {
      listen_port,
      server_host,
      std::strlen(server_host),
      server_port,
      ODIN_CLI_CLIENT_TRANSPORT_TCP,
  };
  return odin_cli_run_client(&config, err);
}

SpawnedChild SpawnTcpClient(uint16_t listen_port, const char *server_host,
                            uint16_t server_port) {
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
    const int rc =
        RunTcpClientConfig(listen_port, server_host, server_port, stderr);
    _exit(rc);
  }
  close(out_pipe[1]);
  close(err_pipe[1]);
  return {pid, out_pipe[0], err_pipe[0]};
}

struct DirectChildSnapshot {
  int rc;
  int sigint_count;
  int sigterm_count;
  odin_cli_client_test_liveness_t cli;
  odin_event_loop_test_liveness_t event_loop;
};

bool ReadSnapshot(int fd, DirectChildSnapshot *snap) {
  return ReadWithDeadline(fd, snap, sizeof(*snap), kLongDeadlineMs) ==
         static_cast<ssize_t>(sizeof(*snap));
}

bool WriteReleaseByte(int fd) {
  const char b = 1;
  return WriteAllDeadline(fd, &b, 1, kShortDeadlineMs);
}

void InstallCountingHandlers(struct sigaction *old_int,
                             struct sigaction *old_term) {
  g_child_sigint_count = 0;
  g_child_sigterm_count = 0;
  struct sigaction sa_int;
  std::memset(&sa_int, 0, sizeof(sa_int));
  sa_int.sa_handler = CountingSigint;
  sigemptyset(&sa_int.sa_mask);
  struct sigaction sa_term;
  std::memset(&sa_term, 0, sizeof(sa_term));
  sa_term.sa_handler = CountingSigterm;
  sigemptyset(&sa_term.sa_mask);
  ASSERT_EQ(sigaction(SIGINT, &sa_int, old_int), 0);
  ASSERT_EQ(sigaction(SIGTERM, &sa_term, old_term), 0);
}

void RestoreHandlers(const struct sigaction *old_int,
                     const struct sigaction *old_term) {
  (void)sigaction(SIGINT, old_int, nullptr);
  (void)sigaction(SIGTERM, old_term, nullptr);
}

struct InflightPair {
  int client_a = -1;
  int client_b = -1;
  int upstream_a = -1;
  int upstream_b = -1;
};

InflightPair StartTwoInflightSessions(uint16_t proxy_port, int fake_listener) {
  InflightPair s;
  s.client_a = TcpConnectLoopback(proxy_port, kShortDeadlineMs);
  EXPECT_GE(s.client_a, 0) << std::strerror(errno);
  s.client_b = TcpConnectLoopback(proxy_port, kShortDeadlineMs);
  EXPECT_GE(s.client_b, 0) << std::strerror(errno);
  const char req_a[] = "CONNECT a.example:443 HTTP/1.1\r\n\r\n";
  const char req_b[] = "CONNECT b.example:8443 HTTP/1.1\r\n\r\n";
  EXPECT_TRUE(WriteAllDeadline(s.client_a, req_a, std::strlen(req_a),
                               kShortDeadlineMs));
  EXPECT_TRUE(WriteAllDeadline(s.client_b, req_b, std::strlen(req_b),
                               kShortDeadlineMs));
  s.upstream_a = AcceptWithDeadline(fake_listener, kShortDeadlineMs);
  EXPECT_GE(s.upstream_a, 0) << std::strerror(errno);
  std::string host;
  uint16_t port = 0;
  EXPECT_TRUE(ReadConnectReq(s.upstream_a, &host, &port));
  EXPECT_EQ(host, "a.example");
  EXPECT_EQ(port, 443);
  s.upstream_b = AcceptWithDeadline(fake_listener, kShortDeadlineMs);
  EXPECT_GE(s.upstream_b, 0) << std::strerror(errno);
  EXPECT_TRUE(ReadConnectReq(s.upstream_b, &host, &port));
  EXPECT_EQ(host, "b.example");
  EXPECT_EQ(port, 8443);
  return s;
}

void CloseInflight(InflightPair *s) {
  CloseIfOpen(&s->client_a);
  CloseIfOpen(&s->client_b);
  CloseIfOpen(&s->upstream_a);
  CloseIfOpen(&s->upstream_b);
}

void ExpectInflightClosed(InflightPair *s) {
  EXPECT_TRUE(DrainUntilEofOrReset(s->client_a, kShortDeadlineMs));
  EXPECT_TRUE(DrainUntilEofOrReset(s->client_b, kShortDeadlineMs));
  EXPECT_TRUE(DrainUntilEofOrReset(s->upstream_a, kShortDeadlineMs));
  EXPECT_TRUE(DrainUntilEofOrReset(s->upstream_b, kShortDeadlineMs));
}

enum class RuntimeFailKind {
  kSignalOnly,
  kAcceptErrno,
  kFcntlGetfl,
  kFcntlSetfl,
  kEventLoopRun,
  kUnexpectedStop,
};

struct PausedRuntimeChild {
  pid_t pid;
  int stderr_fd;
  int snapshot_fd;
  int release_fd;
  int progress_fd;
  int trigger_write_fd;
};

PausedRuntimeChild ForkPausedRuntimeChild(uint16_t server_port,
                                          RuntimeFailKind kind,
                                          int signal_num) {
  (void)signal_num;
  int err_pipe[2];
  int snap_pipe[2];
  int release_pipe[2];
  int progress_pipe[2];
  int trigger_pipe[2];
  EXPECT_EQ(pipe(err_pipe), 0);
  EXPECT_EQ(pipe(snap_pipe), 0);
  EXPECT_EQ(pipe(release_pipe), 0);
  EXPECT_EQ(pipe(progress_pipe), 0);
  EXPECT_EQ(pipe(trigger_pipe), 0);

  const pid_t pid = fork();
  if (pid == 0) {
    close(err_pipe[0]);
    close(snap_pipe[0]);
    close(release_pipe[1]);
    close(progress_pipe[0]);
    close(trigger_pipe[1]);
    dup2(err_pipe[1], STDERR_FILENO);
    close(err_pipe[1]);

    struct sigaction old_int;
    struct sigaction old_term;
    std::memset(&old_int, 0, sizeof(old_int));
    std::memset(&old_term, 0, sizeof(old_term));
    struct sigaction sa_int;
    std::memset(&sa_int, 0, sizeof(sa_int));
    sa_int.sa_handler = CountingSigint;
    sigemptyset(&sa_int.sa_mask);
    struct sigaction sa_term;
    std::memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = CountingSigterm;
    sigemptyset(&sa_term.sa_mask);
    (void)sigaction(SIGINT, &sa_int, &old_int);
    (void)sigaction(SIGTERM, &sa_term, &old_term);
    g_child_sigint_count = 0;
    g_child_sigterm_count = 0;

    odin_cli_client_test_reset_liveness();
    odin_event_loop_test_reset_liveness();
    if (odin_cli_client_test_set_progress_fd(progress_pipe[1], 2) != 0) {
      _exit(120);
    }
    if (kind != RuntimeFailKind::kSignalOnly) {
      (void)SetNonblockFd(trigger_pipe[0]);
      if (odin_cli_client_test_set_runtime_trigger_fd(trigger_pipe[0]) != 0) {
        _exit(121);
      }
      int hook_rc = 0;
      switch (kind) {
      case RuntimeFailKind::kAcceptErrno:
        hook_rc = odin_cli_client_test_fail_next(
            ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_ERROR, EIO);
        break;
      case RuntimeFailKind::kFcntlGetfl:
        hook_rc = odin_cli_client_test_fail_next(
            ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_GETFL_ERROR, EINVAL);
        break;
      case RuntimeFailKind::kFcntlSetfl:
        hook_rc = odin_cli_client_test_fail_next(
            ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_SETFL_ERROR, EINVAL);
        break;
      case RuntimeFailKind::kEventLoopRun:
        hook_rc = odin_cli_client_test_fail_next(
            ODIN_CLI_CLIENT_TEST_FAIL_EVENT_LOOP_RUN, EIO);
        break;
      case RuntimeFailKind::kUnexpectedStop:
        hook_rc = odin_cli_client_test_trigger_next(
            ODIN_CLI_CLIENT_TEST_TRIGGER_UNEXPECTED_STOP);
        break;
      case RuntimeFailKind::kSignalOnly:
        break;
      }
      if (hook_rc != 0) {
        _exit(122);
      }
    }

    const int rc = RunTcpClientConfig(0, "127.0.0.1", server_port, stderr);

    DirectChildSnapshot snap{};
    snap.rc = rc;
    (void)odin_cli_client_test_liveness(&snap.cli);
    (void)odin_event_loop_test_liveness(&snap.event_loop);
    (void)raise(SIGINT);
    (void)raise(SIGTERM);
    snap.sigint_count = static_cast<int>(g_child_sigint_count);
    snap.sigterm_count = static_cast<int>(g_child_sigterm_count);
    (void)write(snap_pipe[1], &snap, sizeof(snap));
    char release = 0;
    (void)read(release_pipe[0], &release, 1);
    _exit(rc);
  }

  close(err_pipe[1]);
  close(snap_pipe[1]);
  close(release_pipe[0]);
  close(progress_pipe[1]);
  close(trigger_pipe[0]);
  return {pid,
          err_pipe[0],
          snap_pipe[0],
          release_pipe[1],
          progress_pipe[0],
          trigger_pipe[1]};
}

void FinishPausedRuntimeChild(PausedRuntimeChild *child, int expected_rc) {
  ASSERT_TRUE(WriteReleaseByte(child->release_fd));
  int st = 0;
  ASSERT_EQ(WaitChildBounded(child->pid, kLongDeadlineMs, &st), 0);
  EXPECT_TRUE(WIFEXITED(st));
  EXPECT_EQ(WEXITSTATUS(st), expected_rc);
  close(child->stderr_fd);
  close(child->snapshot_fd);
  close(child->release_fd);
  close(child->progress_fd);
  close(child->trigger_write_fd);
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

namespace {

struct Rfc028TcpChildSnapshot {
  int rc;
  odin_cli_client_test_liveness_t cli;
  odin_event_loop_test_liveness_t event_loop;
};

struct Rfc028QuicChildSnapshot {
  int rc;
  int sigint_count;
  int sigterm_count;
  odin_cli_client_test_liveness_t cli;
  odin_event_loop_test_liveness_t event_loop;
  odin_cli_client_test_xqc_add_record_t add_record;
  int add_record_ok;
  odin_cli_client_test_runtime_config_record_t runtime_config;
  int runtime_config_ok;
  struct sockaddr_in bind_addr;
  int bind_addr_ok;
  struct sockaddr_in bound_addr;
  int bound_addr_ok;
  odin_xqc_client_runtime_test_record_t runtime_record;
};

struct Rfc028QuicDirectRun {
  int rc;
  std::string err;
  Rfc028QuicChildSnapshot snapshot;
};

struct Rfc028QuicFakeState {
  char engine_storage;
  char conn_storage;
  xqc_engine_t *engine;
  xqc_connection_t *conn;
  xqc_app_proto_callbacks_t *app_callbacks;
  void *xqc_user_data;
  xqc_cid_t cid;
};

Rfc028QuicFakeState g_rfc028_quic_fake;

xqc_engine_t *Rfc028QuicEngineCreate(
    xqc_engine_type_t engine_type, const xqc_config_t *engine_config,
    const xqc_engine_ssl_config_t *ssl_config,
    const xqc_engine_callback_t *engine_callback,
    const xqc_transport_callbacks_t *transport_cbs, void *user_data) {
  (void)engine_config;
  (void)ssl_config;
  (void)engine_callback;
  (void)transport_cbs;
  EXPECT_EQ(engine_type, XQC_ENGINE_CLIENT);
  g_rfc028_quic_fake.xqc_user_data = user_data;
  return g_rfc028_quic_fake.engine;
}

void Rfc028QuicEngineDestroy(xqc_engine_t *engine) {
  EXPECT_EQ(engine, g_rfc028_quic_fake.engine);
}

xqc_int_t Rfc028QuicEngineRegisterAlpn(xqc_engine_t *engine, const char *alpn,
                                       size_t alpn_len,
                                       xqc_app_proto_callbacks_t *app_callbacks,
                                       void *user_data) {
  (void)user_data;
  EXPECT_EQ(engine, g_rfc028_quic_fake.engine);
  EXPECT_EQ(std::string(alpn, alpn_len), ODIN_XQC_CLIENT_ALPN);
  g_rfc028_quic_fake.app_callbacks = app_callbacks;
  return XQC_OK;
}

xqc_int_t Rfc028QuicEngineUnregisterAlpn(xqc_engine_t *engine, const char *alpn,
                                         size_t alpn_len) {
  EXPECT_EQ(engine, g_rfc028_quic_fake.engine);
  EXPECT_EQ(std::string(alpn, alpn_len), ODIN_XQC_CLIENT_ALPN);
  return XQC_OK;
}

const xqc_cid_t *Rfc028QuicConnect(
    xqc_engine_t *engine, const xqc_conn_settings_t *conn_settings,
    const unsigned char *token, unsigned int token_len, const char *server_host,
    int no_crypto_flag, const xqc_conn_ssl_config_t *conn_ssl_config,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen, const char *alpn,
    void *user_data) {
  (void)conn_settings;
  (void)token;
  (void)token_len;
  (void)conn_ssl_config;
  EXPECT_EQ(engine, g_rfc028_quic_fake.engine);
  EXPECT_STREQ(server_host, "127.0.0.1");
  EXPECT_EQ(no_crypto_flag, 0);
  EXPECT_NE(peer_addr, nullptr);
  EXPECT_EQ(peer_addrlen, static_cast<socklen_t>(sizeof(struct sockaddr_in)));
  EXPECT_STREQ(alpn, ODIN_XQC_CLIENT_ALPN);
  EXPECT_NE(g_rfc028_quic_fake.app_callbacks, nullptr);
  EXPECT_EQ(
      g_rfc028_quic_fake.app_callbacks->conn_cbs.conn_create_notify(
          g_rfc028_quic_fake.conn, &g_rfc028_quic_fake.cid, user_data, nullptr),
      0);
  return &g_rfc028_quic_fake.cid;
}

void Rfc028QuicConnSetAlpUserData(xqc_connection_t *conn, void *user_data) {
  (void)user_data;
  EXPECT_EQ(conn, g_rfc028_quic_fake.conn);
}

xqc_int_t Rfc028QuicConnClose(xqc_engine_t *engine, const xqc_cid_t *cid) {
  (void)cid;
  EXPECT_EQ(engine, g_rfc028_quic_fake.engine);
  return XQC_OK;
}

int Rfc028QuicUdpRegisterConn(odin_xqc_udp_t *xu, const xqc_cid_t *cid) {
  (void)xu;
  EXPECT_NE(cid, nullptr);
  return 0;
}

void Rfc028QuicUdpUnregisterConn(odin_xqc_udp_t *xu, const xqc_cid_t *cid) {
  (void)xu;
  (void)cid;
}

void InstallRfc028QuicOps() {
  std::memset(&g_rfc028_quic_fake, 0, sizeof(g_rfc028_quic_fake));
  g_rfc028_quic_fake.engine =
      reinterpret_cast<xqc_engine_t *>(&g_rfc028_quic_fake.engine_storage);
  g_rfc028_quic_fake.conn =
      reinterpret_cast<xqc_connection_t *>(&g_rfc028_quic_fake.conn_storage);
  g_rfc028_quic_fake.cid.cid_len = 1;
  g_rfc028_quic_fake.cid.cid_buf[0] = 0x28;
  odin_xqc_client_runtime_test_reset();
  static const odin_xqc_udp_test_ops_t kUdpOps = {
      Rfc028QuicEngineCreate,
      Rfc028QuicEngineDestroy,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
  };
  odin_xqc_udp_test_set_ops(&kUdpOps);
  static const odin_xqc_client_runtime_test_ops_t kClientOps = {
      Rfc028QuicEngineRegisterAlpn,
      Rfc028QuicEngineUnregisterAlpn,
      Rfc028QuicConnect,
      Rfc028QuicConnSetAlpUserData,
      Rfc028QuicConnClose,
      nullptr,
      nullptr,
      nullptr,
      Rfc028QuicUdpRegisterConn,
      Rfc028QuicUdpUnregisterConn,
  };
  odin_xqc_client_runtime_test_set_ops(&kClientOps);
}

void ClearRfc028QuicOps() {
  odin_xqc_client_runtime_test_set_ops(nullptr);
  odin_xqc_udp_test_set_ops(nullptr);
}

bool ParseQuicStartupLine(const std::string &line, uint16_t *out_port,
                          std::string *out_server) {
  static const char kPrefix[] = "odin: mode=client transport=quic listen=";
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

void FillRfc028QuicSnapshot(int rc, Rfc028QuicChildSnapshot *snap) {
  std::memset(snap, 0, sizeof(*snap));
  snap->rc = rc;
  (void)odin_cli_client_test_liveness(&snap->cli);
  (void)odin_event_loop_test_liveness(&snap->event_loop);
  snap->add_record_ok =
      odin_cli_client_test_last_xqc_add(&snap->add_record) == 0 ? 1 : 0;
  snap->runtime_config_ok =
      odin_cli_client_test_last_runtime_config(&snap->runtime_config) == 0 ? 1
                                                                           : 0;
  snap->bind_addr_ok =
      odin_cli_client_test_last_bind_addr(&snap->bind_addr) == 0 ? 1 : 0;
  snap->bound_addr_ok =
      odin_cli_client_test_last_bound_addr(&snap->bound_addr) == 0 ? 1 : 0;
  snap->runtime_record = *odin_xqc_client_runtime_test_record();
}

Rfc028QuicDirectRun RunRfc028QuicDirect(
    const std::vector<std::string> &tokens,
    odin_cli_client_test_failpoint_t failpoint = ODIN_CLI_CLIENT_TEST_FAIL_NONE,
    int fail_errno = 0, bool trigger = false, bool reset_cli = true) {
  if (reset_cli) {
    odin_cli_client_test_reset_liveness();
  }
  odin_event_loop_test_reset_liveness();
  InstallRfc028QuicOps();
  if (failpoint != ODIN_CLI_CLIENT_TEST_FAIL_NONE) {
    if (trigger) {
      EXPECT_EQ(odin_cli_client_test_trigger_next(failpoint), 0);
    } else {
      EXPECT_EQ(odin_cli_client_test_fail_next(failpoint, fail_errno), 0);
    }
  }
  char err_buf[512] = {0};
  FILE *err = fmemopen(err_buf, sizeof(err_buf), "w");
  EXPECT_NE(err, nullptr);
  MutableArgv argv(tokens);
  const int rc = odin_cli_main(argv.argc(), argv.argv(), stdout, err);
  (void)fclose(err);
  Rfc028QuicDirectRun out;
  out.rc = rc;
  out.err = err_buf;
  FillRfc028QuicSnapshot(rc, &out.snapshot);
  ClearRfc028QuicOps();
  return out;
}

struct Rfc028QuicChild {
  pid_t pid;
  int stderr_fd;
  int snapshot_fd;
  int progress_fd;
  int trigger_fd;
};

Rfc028QuicChild SpawnRfc028QuicChild(
    const std::vector<std::string> &tokens,
    odin_cli_client_test_failpoint_t failpoint = ODIN_CLI_CLIENT_TEST_FAIL_NONE,
    int fail_errno = 0, size_t progress_min_quic_adds = 0,
    bool runtime_trigger = false) {
  int err_pipe[2];
  int snap_pipe[2];
  int progress_pipe[2] = {-1, -1};
  int trigger_pipe[2] = {-1, -1};
  EXPECT_EQ(pipe(err_pipe), 0);
  EXPECT_EQ(pipe(snap_pipe), 0);
  if (progress_min_quic_adds > 0) {
    EXPECT_EQ(pipe(progress_pipe), 0);
  }
  if (runtime_trigger) {
    EXPECT_EQ(pipe(trigger_pipe), 0);
  }
  const pid_t pid = fork();
  EXPECT_NE(pid, -1);
  if (pid == 0) {
    close(err_pipe[0]);
    close(snap_pipe[0]);
    if (progress_min_quic_adds > 0) {
      close(progress_pipe[0]);
    }
    if (runtime_trigger) {
      close(trigger_pipe[1]);
    }
    dup2(err_pipe[1], STDERR_FILENO);
    close(err_pipe[1]);
    struct sigaction old_int;
    struct sigaction old_term;
    std::memset(&old_int, 0, sizeof(old_int));
    std::memset(&old_term, 0, sizeof(old_term));
    struct sigaction sa_int;
    std::memset(&sa_int, 0, sizeof(sa_int));
    sa_int.sa_handler = CountingSigint;
    sigemptyset(&sa_int.sa_mask);
    struct sigaction sa_term;
    std::memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = CountingSigterm;
    sigemptyset(&sa_term.sa_mask);
    (void)sigaction(SIGINT, &sa_int, &old_int);
    (void)sigaction(SIGTERM, &sa_term, &old_term);
    g_child_sigint_count = 0;
    g_child_sigterm_count = 0;
    odin_cli_client_test_reset_liveness();
    odin_event_loop_test_reset_liveness();
    InstallRfc028QuicOps();
    if (progress_min_quic_adds > 0 &&
        odin_cli_client_test_set_progress_fd(progress_pipe[1],
                                             progress_min_quic_adds) != 0) {
      _exit(120);
    }
    if (runtime_trigger) {
      if (SetNonblockFd(trigger_pipe[0]) != 0 ||
          odin_cli_client_test_set_runtime_trigger_fd(trigger_pipe[0]) != 0) {
        _exit(121);
      }
    }
    if (failpoint != ODIN_CLI_CLIENT_TEST_FAIL_NONE) {
      (void)odin_cli_client_test_fail_next(failpoint, fail_errno);
    }
    MutableArgv argv(tokens);
    const int rc = odin_cli_main(argv.argc(), argv.argv(), stdout, stderr);
    Rfc028QuicChildSnapshot snap;
    FillRfc028QuicSnapshot(rc, &snap);
    (void)raise(SIGINT);
    (void)raise(SIGTERM);
    snap.sigint_count = static_cast<int>(g_child_sigint_count);
    snap.sigterm_count = static_cast<int>(g_child_sigterm_count);
    (void)write(snap_pipe[1], &snap, sizeof(snap));
    _exit(rc);
  }
  close(err_pipe[1]);
  close(snap_pipe[1]);
  if (progress_min_quic_adds > 0) {
    close(progress_pipe[1]);
  }
  if (runtime_trigger) {
    close(trigger_pipe[0]);
  }
  return {pid, err_pipe[0], snap_pipe[0], progress_pipe[0], trigger_pipe[1]};
}

Rfc028QuicChildSnapshot FinishRfc028QuicChild(Rfc028QuicChild *child,
                                              int signal_to_send) {
  if (signal_to_send != 0) {
    EXPECT_EQ(kill(child->pid, signal_to_send), 0);
  }
  Rfc028QuicChildSnapshot snap;
  std::memset(&snap, 0, sizeof(snap));
  EXPECT_EQ(ReadWithDeadline(child->snapshot_fd, &snap, sizeof(snap),
                             kLongDeadlineMs),
            static_cast<ssize_t>(sizeof(snap)));
  int st = 0;
  EXPECT_EQ(WaitChildBounded(child->pid, kLongDeadlineMs, &st), 0);
  EXPECT_TRUE(WIFEXITED(st));
  EXPECT_EQ(WEXITSTATUS(st), snap.rc);
  close(child->snapshot_fd);
  child->pid = -1;
  return snap;
}

void ExpectRfc028QuicClean(const Rfc028QuicChildSnapshot &snap) {
  EXPECT_EQ(snap.cli.live_listeners, 0u);
  EXPECT_EQ(snap.cli.live_accept_loops, 0u);
  EXPECT_EQ(snap.cli.live_sessions, 0u);
  EXPECT_EQ(snap.cli.live_xqc_client_runtimes, 0u);
  EXPECT_EQ(snap.event_loop.loops, 0u);
  EXPECT_EQ(snap.event_loop.io_handles, 0u);
  EXPECT_EQ(snap.event_loop.timers, 0u);
  EXPECT_EQ(snap.event_loop.task_nodes, 0u);
}

} // namespace

TEST(OdinRFC028ClientTransportTest, T2TcpRunnerPreserved) {
  (void)signal(SIGPIPE, SIG_IGN);
  uint16_t upstream_port = 0;
  const int fake = OpenIpv4Listener("127.0.0.1", 0, true, &upstream_port);
  ASSERT_GE(fake, 0) << std::strerror(errno);

  int err_pipe[2];
  int snap_pipe[2];
  ASSERT_EQ(pipe(err_pipe), 0);
  ASSERT_EQ(pipe(snap_pipe), 0);
  const pid_t pid = fork();
  ASSERT_NE(pid, -1);
  if (pid == 0) {
    close(err_pipe[0]);
    close(snap_pipe[0]);
    dup2(err_pipe[1], STDERR_FILENO);
    close(err_pipe[1]);
    odin_cli_client_test_reset_liveness();
    odin_event_loop_test_reset_liveness();
    const int rc = RunTcpClientConfig(0, "127.0.0.1", upstream_port, stderr);
    Rfc028TcpChildSnapshot snap{};
    snap.rc = rc;
    (void)odin_cli_client_test_liveness(&snap.cli);
    (void)odin_event_loop_test_liveness(&snap.event_loop);
    (void)write(snap_pipe[1], &snap, sizeof(snap));
    _exit(rc);
  }
  close(err_pipe[1]);
  close(snap_pipe[1]);
  ChildGuard guard(pid);

  const std::string startup = ReadLineWithDeadline(err_pipe[0], 2000);
  uint16_t proxy_port = 0;
  std::string server;
  ASSERT_TRUE(ParseClientStartupLine(startup, &proxy_port, &server)) << startup;
  EXPECT_EQ(server, "127.0.0.1:" + std::to_string(upstream_port));
  EXPECT_EQ(startup.find("transport="), std::string::npos);

  int client = -1;
  int upstream = -1;
  ExerciseOneSuccessfulConnect(proxy_port, fake,
                               "CONNECT tcp.example:443 HTTP/1.1\r\n\r\n",
                               "tcp.example", 443, &client, &upstream);
  shutdown(client, SHUT_WR);
  shutdown(upstream, SHUT_WR);
  EXPECT_TRUE(DrainUntilEofOrReset(client, kShortDeadlineMs));
  EXPECT_TRUE(DrainUntilEofOrReset(upstream, kShortDeadlineMs));
  close(client);
  close(upstream);
  EXPECT_EQ(kill(pid, SIGTERM), 0);
  Rfc028TcpChildSnapshot snap{};
  ASSERT_EQ(
      ReadWithDeadline(snap_pipe[0], &snap, sizeof(snap), kLongDeadlineMs),
      static_cast<ssize_t>(sizeof(snap)));
  int st = 0;
  ASSERT_EQ(WaitChildBounded(pid, kLongDeadlineMs, &st), 0);
  guard.disarm();
  EXPECT_TRUE(WIFEXITED(st));
  EXPECT_EQ(WEXITSTATUS(st), 0);
  EXPECT_EQ(snap.rc, 0);
  EXPECT_EQ(snap.cli.quic_runtime_create_calls, 0u);
  EXPECT_EQ(snap.cli.live_xqc_client_runtimes, 0u);
  EXPECT_EQ(snap.cli.live_listeners, 0u);
  EXPECT_EQ(snap.cli.live_accept_loops, 0u);
  EXPECT_EQ(snap.cli.live_sessions, 0u);
  EXPECT_EQ(snap.event_loop.loops, 0u);
  EXPECT_EQ(snap.event_loop.io_handles, 0u);
  EXPECT_EQ(snap.event_loop.timers, 0u);
  EXPECT_EQ(snap.event_loop.task_nodes, 0u);
  EXPECT_EQ(ReadAllAvailable(err_pipe[0], 100), "");
  close(err_pipe[0]);
  close(snap_pipe[0]);
  close(fake);
}

TEST(OdinRFC028ClientTransportTest, T3QuicStartupCreatesRuntimeAfterListener) {
  Rfc028QuicChild child = SpawnRfc028QuicChild(
      {"odin-client", "--listen", "0", "--server", "127.0.0.1:4433"});
  ChildGuard guard(child.pid);
  const std::string line = ReadLineWithDeadline(child.stderr_fd, 2000);
  uint16_t proxy_port = 0;
  std::string server;
  ASSERT_TRUE(ParseQuicStartupLine(line, &proxy_port, &server)) << line;
  EXPECT_EQ(server, "127.0.0.1:4433");
  Rfc028QuicChildSnapshot snap = FinishRfc028QuicChild(&child, SIGTERM);
  guard.disarm();
  EXPECT_EQ(snap.rc, 0);
  EXPECT_EQ(ReadAllAvailable(child.stderr_fd, 100), "");
  close(child.stderr_fd);
  ASSERT_TRUE(snap.bind_addr_ok);
  EXPECT_EQ(snap.bind_addr.sin_family, AF_INET);
  EXPECT_EQ(snap.bind_addr.sin_addr.s_addr, htonl(INADDR_LOOPBACK));
  EXPECT_EQ(ntohs(snap.bind_addr.sin_port), 0);
  ASSERT_TRUE(snap.bound_addr_ok);
  EXPECT_EQ(snap.bound_addr.sin_family, AF_INET);
  EXPECT_EQ(snap.bound_addr.sin_addr.s_addr, htonl(INADDR_LOOPBACK));
  EXPECT_EQ(ntohs(snap.bound_addr.sin_port), proxy_port);
  ASSERT_TRUE(snap.runtime_config_ok);
  const auto *local = reinterpret_cast<const struct sockaddr_in *>(
      &snap.runtime_config.local_addr_value);
  const auto *peer = reinterpret_cast<const struct sockaddr_in *>(
      &snap.runtime_config.peer_addr_value);
  EXPECT_EQ(snap.runtime_config.local_addrlen,
            static_cast<socklen_t>(sizeof(struct sockaddr_in)));
  EXPECT_EQ(local->sin_family, AF_INET);
  EXPECT_EQ(local->sin_addr.s_addr, htonl(INADDR_ANY));
  EXPECT_EQ(ntohs(local->sin_port), 0);
  EXPECT_EQ(snap.runtime_config.peer_addrlen,
            static_cast<socklen_t>(sizeof(struct sockaddr_in)));
  EXPECT_EQ(peer->sin_family, AF_INET);
  EXPECT_EQ(peer->sin_addr.s_addr, htonl(INADDR_LOOPBACK));
  EXPECT_EQ(ntohs(peer->sin_port), 4433);
  EXPECT_EQ(snap.runtime_config.server_host_len, std::strlen("127.0.0.1"));
  EXPECT_STREQ(snap.runtime_config.server_host_value, "127.0.0.1");
  EXPECT_EQ(snap.cli.quic_runtime_create_calls, 1u);
  EXPECT_EQ(snap.cli.quic_runtime_start_calls, 1u);
  EXPECT_EQ(snap.cli.quic_runtime_force_destroy_calls, 1u);
  EXPECT_EQ(snap.runtime_record.default_create_calls, 1u);
  EXPECT_EQ(snap.runtime_record.last_default_create.engine_config, nullptr);
  EXPECT_EQ(snap.runtime_record.last_default_create.transport_callbacks,
            nullptr);
  EXPECT_EQ(snap.runtime_record.last_default_create.token, nullptr);
  EXPECT_EQ(snap.runtime_record.last_default_create.token_len, 0u);
  EXPECT_EQ(snap.runtime_record.last_default_create.conn_ssl_config_value
                .cert_verify_flag,
            0);
  EXPECT_EQ(snap.runtime_record.last_default_create.no_crypto_flag, 0);
  EXPECT_EQ(snap.runtime_record.runtime_free_calls, 1u);
  ExpectRfc028QuicClean(snap);
}

TEST(OdinRFC028ClientTransportTest, T4QuicRejectsParsedNonIpv4BeforeResources) {
  const char *servers[] = {"example.com:443", "[::1]:443"};
  for (const char *server : servers) {
    SCOPED_TRACE(server);
    Rfc028QuicDirectRun run = RunRfc028QuicDirect(
        {"odin-client", "--listen", "0", "--server", server});
    EXPECT_EQ(run.rc, 1);
    EXPECT_EQ(run.err, "odin: client startup failed at server_endpoint\n");
    EXPECT_EQ(run.err.find("mode=client"), std::string::npos);
    EXPECT_EQ(run.snapshot.runtime_record.default_create_calls, 0u);
    EXPECT_EQ(run.snapshot.runtime_record.call_count, 0u);
    ExpectRfc028QuicClean(run.snapshot);
  }
}

TEST(OdinRFC028ClientTransportTest, T5QuicListenerStartupFailureMatrix) {
  struct Case {
    const char *step;
    odin_cli_client_test_failpoint_t fp;
    bool expect_bind_addr;
  };
  const Case cases[] = {
      {"socket", ODIN_CLI_CLIENT_TEST_FAIL_SOCKET, false},
      {"setsockopt(SO_REUSEADDR)",
       ODIN_CLI_CLIENT_TEST_FAIL_SETSOCKOPT_REUSEADDR, false},
      {"fcntl(F_GETFL)", ODIN_CLI_CLIENT_TEST_FAIL_FCNTL_GETFL, false},
      {"fcntl(F_SETFL)", ODIN_CLI_CLIENT_TEST_FAIL_FCNTL_SETFL, false},
      {"bind", ODIN_CLI_CLIENT_TEST_FAIL_BIND, true},
      {"listen", ODIN_CLI_CLIENT_TEST_FAIL_LISTEN, true},
      {"getsockname", ODIN_CLI_CLIENT_TEST_FAIL_GETSOCKNAME, true},
  };
  for (const Case &c : cases) {
    SCOPED_TRACE(c.step);
    Rfc028QuicDirectRun run = RunRfc028QuicDirect(
        {"odin-client", "--listen", "0", "--server", "127.0.0.1:4433"}, c.fp,
        EIO);
    EXPECT_EQ(run.rc, 1);
    EXPECT_EQ(run.err,
              std::string("odin: client startup failed at ") + c.step + "\n");
    EXPECT_EQ(run.err.find("mode=client"), std::string::npos);
    if (c.expect_bind_addr) {
      ASSERT_TRUE(run.snapshot.bind_addr_ok);
      EXPECT_EQ(run.snapshot.bind_addr.sin_family, AF_INET);
      EXPECT_EQ(run.snapshot.bind_addr.sin_addr.s_addr, htonl(INADDR_LOOPBACK));
      EXPECT_EQ(ntohs(run.snapshot.bind_addr.sin_port), 0);
    }
    EXPECT_EQ(run.snapshot.runtime_record.default_create_calls, 0u);
    ExpectRfc028QuicClean(run.snapshot);
  }
}

TEST(OdinRFC028ClientTransportTest, T6QuicAcceptedFdTransfersToRuntime) {
  uint16_t sentry_port = 0;
  const int sentry = OpenIpv4Listener("127.0.0.1", 0, true, &sentry_port);
  ASSERT_GE(sentry, 0) << std::strerror(errno);
  Rfc028QuicChild child = SpawnRfc028QuicChild(
      {"odin-client", "--listen", "0", "--server", "127.0.0.1:4433"});
  ChildGuard guard(child.pid);
  const std::string line = ReadLineWithDeadline(child.stderr_fd, 2000);
  uint16_t proxy_port = 0;
  std::string server;
  ASSERT_TRUE(ParseQuicStartupLine(line, &proxy_port, &server)) << line;
  const int client = TcpConnectLoopback(proxy_port, kShortDeadlineMs);
  ASSERT_GE(client, 0) << std::strerror(errno);
  const std::string request =
      "CONNECT 127.0.0.1:" + std::to_string(sentry_port) + " HTTP/1.1\r\n\r\n";
  ASSERT_TRUE(WriteAllDeadline(client, request.data(), request.size(),
                               kShortDeadlineMs));
  ExpectNoAccept(sentry, 200);
  Rfc028QuicChildSnapshot snap = FinishRfc028QuicChild(&child, SIGTERM);
  guard.disarm();
  close(child.stderr_fd);
  close(client);
  close(sentry);
  EXPECT_EQ(snap.rc, 0);
  EXPECT_EQ(snap.cli.quic_runtime_add_connection_calls, 1u);
  ASSERT_TRUE(snap.add_record_ok);
  EXPECT_EQ(snap.add_record.fd_is_nonblocking, 1);
  ExpectRfc028QuicClean(snap);
}

TEST(OdinRFC028ClientTransportTest, T7QuicAddFailureClosesOnlyThatFd) {
  Rfc028QuicChild child = SpawnRfc028QuicChild(
      {"odin-client", "--listen", "0", "--server", "127.0.0.1:4433"},
      ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_ADD_CONNECTION, EIO);
  ChildGuard guard(child.pid);
  const std::string line = ReadLineWithDeadline(child.stderr_fd, 2000);
  uint16_t proxy_port = 0;
  std::string server;
  ASSERT_TRUE(ParseQuicStartupLine(line, &proxy_port, &server)) << line;
  const int first = TcpConnectLoopback(proxy_port, kShortDeadlineMs);
  ASSERT_GE(first, 0) << std::strerror(errno);
  EXPECT_TRUE(DrainUntilEofOrResetExpectingNoBytes(first, kShortDeadlineMs));
  close(first);
  const int second = TcpConnectLoopback(proxy_port, kShortDeadlineMs);
  ASSERT_GE(second, 0) << std::strerror(errno);
  Rfc028QuicChildSnapshot snap = FinishRfc028QuicChild(&child, SIGTERM);
  guard.disarm();
  close(child.stderr_fd);
  EXPECT_TRUE(DrainUntilEofOrReset(second, kShortDeadlineMs));
  close(second);
  EXPECT_EQ(snap.rc, 0);
  EXPECT_EQ(snap.cli.quic_runtime_add_connection_calls, 2u);
  ASSERT_TRUE(snap.add_record_ok);
  EXPECT_EQ(snap.add_record.fd_is_nonblocking, 1);
  ExpectRfc028QuicClean(snap);
}

TEST(OdinRFC028ClientTransportTest, T8QuicFatalRuntimeFailuresCleanUp) {
  struct Case {
    const char *name;
    odin_cli_client_test_failpoint_t fp;
    int errnum;
    bool trigger;
    const char *line;
  };
  const Case cases[] = {
      {"event_loop", ODIN_CLI_CLIENT_TEST_FAIL_EVENT_LOOP_RUN, EIO, false,
       "odin: client runtime failed at event_loop_run\n"},
      {"unexpected_stop", ODIN_CLI_CLIENT_TEST_TRIGGER_UNEXPECTED_STOP, 0, true,
       "odin: client runtime failed at event_loop_run\n"},
      {"accept", ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_ERROR, EIO, false,
       "odin: client runtime failed at accept_loop\n"},
  };
#if defined(__linux__)
  errno = 0;
  EXPECT_EQ(
      odin_cli_client_test_fail_next(
          ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_GETFL_ERROR, EIO),
      -1);
  EXPECT_EQ(errno, EOPNOTSUPP);
  errno = 0;
  EXPECT_EQ(
      odin_cli_client_test_fail_next(
          ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_SETFL_ERROR, EIO),
      -1);
  EXPECT_EQ(errno, EOPNOTSUPP);
#endif
  for (const Case &c : cases) {
    SCOPED_TRACE(c.name);
    Rfc028QuicDirectRun run = RunRfc028QuicDirect(
        {"odin-client", "--listen", "0", "--server", "127.0.0.1:4433"}, c.fp,
        c.errnum, c.trigger);
    EXPECT_EQ(run.rc, 1);
    const size_t banner_end = run.err.find('\n');
    ASSERT_NE(banner_end, std::string::npos) << run.err;
    const std::string banner = run.err.substr(0, banner_end + 1);
    uint16_t proxy_port = 0;
    std::string server;
    ASSERT_TRUE(ParseQuicStartupLine(banner, &proxy_port, &server)) << banner;
    EXPECT_EQ(server, "127.0.0.1:4433");
    EXPECT_EQ(run.err, banner + c.line);
    EXPECT_EQ(run.snapshot.cli.quic_runtime_force_destroy_calls, 1u);
    ExpectRfc028QuicClean(run.snapshot);
  }
#if !defined(__linux__)
  struct FcntlCase {
    const char *name;
    odin_cli_client_test_failpoint_t fp;
  };
  const FcntlCase fcntl_cases[] = {
      {"fcntl_getfl",
       ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_GETFL_ERROR},
      {"fcntl_setfl",
       ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_SETFL_ERROR},
  };
  for (const FcntlCase &c : fcntl_cases) {
    SCOPED_TRACE(c.name);
    Rfc028QuicChild child = SpawnRfc028QuicChild(
        {"odin-client", "--listen", "0", "--server", "127.0.0.1:4433"}, c.fp,
        EIO, 0, true);
    ChildGuard guard(child.pid);
    const std::string banner = ReadLineWithDeadline(child.stderr_fd, 2000);
    uint16_t proxy_port = 0;
    std::string server;
    ASSERT_TRUE(ParseQuicStartupLine(banner, &proxy_port, &server)) << banner;
    EXPECT_EQ(server, "127.0.0.1:4433");
    close(child.trigger_fd);
    child.trigger_fd = -1;
    const int client = TcpConnectLoopback(proxy_port, kShortDeadlineMs);
    ASSERT_GE(client, 0) << std::strerror(errno);
    EXPECT_TRUE(DrainUntilEofOrResetExpectingNoBytes(client, kShortDeadlineMs));
    Rfc028QuicChildSnapshot snap = FinishRfc028QuicChild(&child, 0);
    guard.disarm();
    EXPECT_EQ(ReadLineWithDeadline(child.stderr_fd, 2000),
              "odin: client runtime failed at accept_loop\n");
    EXPECT_EQ(ReadAllAvailable(child.stderr_fd, 100), "");
    close(child.stderr_fd);
    close(client);
    EXPECT_EQ(snap.rc, 1);
    EXPECT_EQ(snap.cli.quic_runtime_add_connection_calls, 0u);
    EXPECT_FALSE(snap.add_record_ok);
    EXPECT_EQ(snap.cli.quic_runtime_force_destroy_calls, 1u);
    ExpectRfc028QuicClean(snap);
  }
#endif
}

TEST(OdinRFC028ClientTransportTest, T9QuicSignalShutdownMirrorsTcpCleanup) {
  const int signals[] = {SIGINT, SIGTERM};
  for (const int sig : signals) {
    SCOPED_TRACE(sig == SIGINT ? "SIGINT" : "SIGTERM");
    Rfc028QuicChild child = SpawnRfc028QuicChild(
        {"odin-client", "--listen", "0", "--server", "127.0.0.1:4433"},
        ODIN_CLI_CLIENT_TEST_FAIL_NONE, 0, 1);
    ChildGuard guard(child.pid);
    const std::string line = ReadLineWithDeadline(child.stderr_fd, 2000);
    uint16_t proxy_port = 0;
    std::string server;
    ASSERT_TRUE(ParseQuicStartupLine(line, &proxy_port, &server)) << line;
    EXPECT_GT(proxy_port, 0);
    const int client = TcpConnectLoopback(proxy_port, kShortDeadlineMs);
    ASSERT_GE(client, 0) << std::strerror(errno);
    char progress = 0;
    ASSERT_EQ(
        ReadWithDeadline(child.progress_fd, &progress, 1, kLongDeadlineMs), 1);
    Rfc028QuicChildSnapshot snap = FinishRfc028QuicChild(&child, sig);
    guard.disarm();
    EXPECT_TRUE(DrainUntilEofOrReset(client, kShortDeadlineMs));
    const int post = TcpConnectLoopback(proxy_port, 300);
    if (post >= 0) {
      close(post);
      ADD_FAILURE() << "listener still accepted after signal cleanup";
    }
    EXPECT_EQ(ReadAllAvailable(child.stderr_fd, 100), "");
    close(child.stderr_fd);
    close(child.progress_fd);
    close(client);
    EXPECT_EQ(snap.rc, 0);
    EXPECT_EQ(snap.sigint_count, 1);
    EXPECT_EQ(snap.sigterm_count, 1);
    EXPECT_EQ(snap.cli.quic_runtime_add_connection_calls, 1u);
    ASSERT_TRUE(snap.add_record_ok);
    EXPECT_EQ(snap.add_record.fd_is_nonblocking, 1);
    EXPECT_EQ(snap.cli.quic_runtime_force_destroy_calls, 1u);
    EXPECT_EQ(snap.runtime_record.runtime_free_calls, 1u);
    ExpectRfc028QuicClean(snap);
  }
}

TEST(OdinRFC028ClientTransportTest, T11QuicPostListenerStartupFailureMatrix) {
  struct Case {
    const char *step;
    odin_cli_client_test_failpoint_t fp;
    bool runtime_created;
  };
  const Case cases[] = {
      {"event_loop_create", ODIN_CLI_CLIENT_TEST_FAIL_EVENT_LOOP_CREATE, false},
      {"accept_loop_create", ODIN_CLI_CLIENT_TEST_FAIL_ACCEPT_LOOP_CREATE,
       false},
      {"xqc_client_runtime_create",
       ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_CREATE, false},
      {"xqc_client_runtime_start",
       ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_START, true},
      {"sigaction(SIGINT)", ODIN_CLI_CLIENT_TEST_FAIL_SIGACTION_SIGINT, true},
      {"sigaction(SIGTERM)", ODIN_CLI_CLIENT_TEST_FAIL_SIGACTION_SIGTERM, true},
      {"signal_timer_start", ODIN_CLI_CLIENT_TEST_FAIL_SIGNAL_TIMER_START,
       true},
  };
  for (const Case &c : cases) {
    SCOPED_TRACE(c.step);
    Rfc028QuicDirectRun run = RunRfc028QuicDirect(
        {"odin-client", "--listen", "0", "--server", "127.0.0.1:4433"}, c.fp,
        EIO);
    EXPECT_EQ(run.rc, 1);
    EXPECT_EQ(run.err,
              std::string("odin: client startup failed at ") + c.step + "\n");
    EXPECT_EQ(run.err.find("mode=client"), std::string::npos);
    EXPECT_EQ(run.snapshot.cli.live_xqc_client_runtimes, 0u);
    EXPECT_EQ(run.snapshot.runtime_record.runtime_free_calls,
              c.runtime_created ? 1u : 0u);
    ExpectRfc028QuicClean(run.snapshot);
  }
}

TEST(OdinRFC028ClientTransportTest, T12ClientRunnerConfigPreconditions) {
  odin_cli_client_test_reset_liveness();
  odin_event_loop_test_reset_liveness();

  char null_buf[128] = {0};
  FILE *null_err = fmemopen(null_buf, sizeof(null_buf), "w");
  ASSERT_NE(null_err, nullptr);
  EXPECT_EQ(odin_cli_run_client(nullptr, null_err), 1);
  (void)fclose(null_err);
  EXPECT_STREQ(null_buf, "odin: client startup failed at config\n");

  odin_cli_client_config_t unknown{};
  unknown.listen_port = 0;
  unknown.server_host = "127.0.0.1";
  unknown.server_host_len = std::strlen(unknown.server_host);
  unknown.server_port = 4433;
  unknown.transport = ODIN_CLI_CLIENT_TRANSPORT_TEST_INVALID;
  char unknown_buf[128] = {0};
  FILE *unknown_err = fmemopen(unknown_buf, sizeof(unknown_buf), "w");
  ASSERT_NE(unknown_err, nullptr);
  EXPECT_EQ(odin_cli_run_client(&unknown, unknown_err), 1);
  (void)fclose(unknown_err);
  EXPECT_STREQ(unknown_buf, "odin: client startup failed at config\n");

  odin_cli_client_config_t tcp{};
  tcp.listen_port = 0;
  tcp.server_host = "127.0.0.1";
  tcp.server_host_len = std::strlen(tcp.server_host);
  tcp.server_port = 4433;
  tcp.transport = ODIN_CLI_CLIENT_TRANSPORT_TCP;
  EXPECT_EQ(odin_cli_run_client(&tcp, nullptr), 1);

  odin_cli_client_test_liveness_t live{};
  odin_event_loop_test_liveness_t elive{};
  ASSERT_EQ(odin_cli_client_test_liveness(&live), 0);
  ASSERT_EQ(odin_event_loop_test_liveness(&elive), 0);
  EXPECT_EQ(live.live_listeners, 0u);
  EXPECT_EQ(live.live_accept_loops, 0u);
  EXPECT_EQ(live.live_sessions, 0u);
  EXPECT_EQ(live.live_xqc_client_runtimes, 0u);
  EXPECT_EQ(elive.loops, 0u);
  EXPECT_EQ(elive.io_handles, 0u);
  EXPECT_EQ(elive.timers, 0u);
  EXPECT_EQ(elive.task_nodes, 0u);
}

TEST(OdinRFC028ClientTransportTest, T15FailpointGapsAndPendingNullRejected) {
  odin_cli_client_test_reset_liveness();
  errno = 0;
  EXPECT_EQ(odin_cli_client_test_pending_failpoint(nullptr), -1);
  EXPECT_EQ(errno, EINVAL);
  odin_cli_client_test_failpoint_t pending = ODIN_CLI_CLIENT_TEST_FAIL_SOCKET;
  ASSERT_EQ(odin_cli_client_test_pending_failpoint(&pending), 0);
  EXPECT_EQ(pending, ODIN_CLI_CLIENT_TEST_FAIL_NONE);
  errno = 0;
  EXPECT_EQ(odin_cli_client_test_fail_next(
                ODIN_CLI_CLIENT_TEST_FAILPOINT_INVALID, EIO),
            -1);
  EXPECT_EQ(errno, EINVAL);
  ASSERT_EQ(odin_cli_client_test_pending_failpoint(&pending), 0);
  EXPECT_EQ(pending, ODIN_CLI_CLIENT_TEST_FAIL_NONE);
  ASSERT_EQ(odin_cli_client_test_fail_next(
                ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_CREATE, EIO),
            0);
  ASSERT_EQ(odin_cli_client_test_pending_failpoint(&pending), 0);
  EXPECT_EQ(pending, ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_CREATE);
  errno = 0;
  EXPECT_EQ(odin_cli_client_test_pending_failpoint(nullptr), -1);
  EXPECT_EQ(errno, EINVAL);
  ASSERT_EQ(odin_cli_client_test_pending_failpoint(&pending), 0);
  EXPECT_EQ(pending, ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_CREATE);
  Rfc028QuicDirectRun run = RunRfc028QuicDirect(
      {"odin-client", "--listen", "0", "--server", "127.0.0.1:4433"},
      ODIN_CLI_CLIENT_TEST_FAIL_NONE, 0, false, false);
  EXPECT_EQ(run.rc, 1);
  EXPECT_EQ(run.err,
            "odin: client startup failed at xqc_client_runtime_create\n");
  ExpectRfc028QuicClean(run.snapshot);
}

TEST(OdinCliClientProcessTest, T1EphemeralStartupReportsReadyLoopback) {
  SpawnedChild child = SpawnTcpClient(0, "127.0.0.1", 4433);
  ASSERT_NE(child.pid, -1);
  ChildGuard guard(child.pid);

  const std::string line = ReadLineWithDeadline(child.stderr_fd, 2000);
  uint16_t port = 0;
  std::string server;
  ASSERT_TRUE(ParseClientStartupLine(line, &port, &server)) << line;
  EXPECT_EQ(server, "127.0.0.1:4433");
  EXPECT_GT(port, 0);

  const int client = TcpConnectLoopback(port, kShortDeadlineMs);
  ASSERT_GE(client, 0) << std::strerror(errno);
  close(client);

  EXPECT_EQ(kill(child.pid, SIGTERM), 0);
  int st = 0;
  ASSERT_EQ(WaitChildBounded(child.pid, kLongDeadlineMs, &st), 0);
  guard.disarm();
  EXPECT_TRUE(WIFEXITED(st));
  EXPECT_EQ(WEXITSTATUS(st), 0);
  EXPECT_EQ(ReadAllAvailable(child.stdout_fd, 100), "");
  close(child.stdout_fd);
  close(child.stderr_fd);
}

TEST(OdinCliClientUnitTest, T3AcceptedSetupFailureClosesOnlyThatFd) {
  (void)signal(SIGPIPE, SIG_IGN);

  struct Subcase {
    const char *name;
    int kind;
  };
  const Subcase cases[] = {
      {"entry_alloc", 0},
      {"cli_local_create", 1},
      {"real_create", 2},
  };

  for (const auto &c : cases) {
    SCOPED_TRACE(c.name);
    uint16_t upstream_port = 0;
    const int fake = OpenIpv4Listener("127.0.0.1", 0, true, &upstream_port);
    ASSERT_GE(fake, 0) << std::strerror(errno);

    int err_pipe[2];
    int snap_pipe[2];
    ASSERT_EQ(pipe(err_pipe), 0);
    ASSERT_EQ(pipe(snap_pipe), 0);
    const pid_t pid = fork();
    ASSERT_NE(pid, -1);
    if (pid == 0) {
      close(err_pipe[0]);
      close(snap_pipe[0]);
      dup2(err_pipe[1], STDERR_FILENO);
      close(err_pipe[1]);
      odin_cli_client_test_reset_liveness();
      odin_event_loop_test_reset_liveness();
      if (odin_cli_client_test_set_idle_snapshot_fd(snap_pipe[1], 1) != 0) {
        _exit(120);
      }
      if (c.kind == 0) {
        (void)odin_cli_client_test_fail_next(
            ODIN_CLI_CLIENT_TEST_FAIL_NEXT_SESSION_ENTRY_ALLOC, ENOMEM);
      } else if (c.kind == 1) {
        (void)odin_cli_client_test_fail_next(
            ODIN_CLI_CLIENT_TEST_FAIL_NEXT_CLIENT_SESSION_CREATE, EIO);
      } else {
        (void)odin_client_session_test_fail_next_create(ENOMEM);
      }
      const int rc = RunTcpClientConfig(0, "127.0.0.1", upstream_port, stderr);
      _exit(rc);
    }
    close(err_pipe[1]);
    close(snap_pipe[1]);
    ChildGuard guard(pid);

    const std::string line = ReadLineWithDeadline(err_pipe[0], 2000);
    uint16_t proxy_port = 0;
    std::string server;
    ASSERT_TRUE(ParseClientStartupLine(line, &proxy_port, &server)) << line;
    ASSERT_GT(proxy_port, 0);

    const int client_a = TcpConnectLoopback(proxy_port, kShortDeadlineMs);
    ASSERT_GE(client_a, 0) << std::strerror(errno);
    const char req_a[] = "CONNECT first.example:443 HTTP/1.1\r\n\r\n";
    ASSERT_TRUE(WriteAllDeadline(client_a, req_a, std::strlen(req_a),
                                 kShortDeadlineMs));
    EXPECT_TRUE(
        DrainUntilEofOrResetExpectingNoBytes(client_a, kShortDeadlineMs));
    close(client_a);
    ExpectNoAccept(fake, 150);

    int client_b = -1;
    int upstream_b = -1;
    ExerciseOneSuccessfulConnect(proxy_port, fake,
                                 "CONNECT later.example:8443 HTTP/1.1\r\n\r\n",
                                 "later.example", 8443, &client_b, &upstream_b);
    shutdown(client_b, SHUT_WR);
    shutdown(upstream_b, SHUT_WR);
    EXPECT_TRUE(DrainUntilEofOrReset(client_b, kShortDeadlineMs));
    EXPECT_TRUE(DrainUntilEofOrReset(upstream_b, kShortDeadlineMs));

    odin_cli_client_test_liveness_t idle{};
    ASSERT_EQ(
        ReadWithDeadline(snap_pipe[0], &idle, sizeof(idle), kLongDeadlineMs),
        static_cast<ssize_t>(sizeof(idle)));
    EXPECT_EQ(idle.live_sessions, 0u);
    EXPECT_EQ(kill(pid, SIGTERM), 0);
    int st = 0;
    ASSERT_EQ(WaitChildBounded(pid, kLongDeadlineMs, &st), 0);
    guard.disarm();
    EXPECT_TRUE(WIFEXITED(st));
    EXPECT_EQ(WEXITSTATUS(st), 0);
    EXPECT_EQ(ReadAllAvailable(err_pipe[0], 100), "");
    close(client_b);
    close(upstream_b);
    close(err_pipe[0]);
    close(snap_pipe[0]);
    close(fake);
  }
}

TEST(OdinCliClientUnitTest, T4ParsedNonIpv4FailsBeforeBanner) {
  const std::vector<std::string> servers = {"example.com", "::1"};
  for (const auto &server_arg : servers) {
    SCOPED_TRACE(server_arg);
    int out_pipe[2];
    int err_pipe[2];
    int snap_pipe[2];
    ASSERT_EQ(pipe(out_pipe), 0);
    ASSERT_EQ(pipe(err_pipe), 0);
    ASSERT_EQ(pipe(snap_pipe), 0);
    const pid_t pid = fork();
    ASSERT_NE(pid, -1);
    if (pid == 0) {
      close(out_pipe[0]);
      close(err_pipe[0]);
      close(snap_pipe[0]);
      FILE *out = fdopen(out_pipe[1], "w");
      FILE *err = fdopen(err_pipe[1], "w");
      odin_cli_client_test_reset_liveness();
      odin_event_loop_test_reset_liveness();
      const int rc = RunTcpClientConfig(0, server_arg.c_str(), 443, err);
      (void)fflush(out);
      (void)fflush(err);
      odin_cli_client_test_liveness_t live{};
      odin_event_loop_test_liveness_t elive{};
      (void)odin_cli_client_test_liveness(&live);
      (void)odin_event_loop_test_liveness(&elive);
      (void)write(snap_pipe[1], &live, sizeof(live));
      (void)write(snap_pipe[1], &elive, sizeof(elive));
      _exit(rc);
    }
    close(out_pipe[1]);
    close(err_pipe[1]);
    close(snap_pipe[1]);
    ChildGuard guard(pid);
    int st = 0;
    ASSERT_EQ(WaitChildBounded(pid, kLongDeadlineMs, &st), 0);
    guard.disarm();
    EXPECT_TRUE(WIFEXITED(st));
    EXPECT_EQ(WEXITSTATUS(st), 1);
    EXPECT_EQ(ReadAllAvailable(err_pipe[0], 100),
              "odin: client startup failed at server_endpoint\n");
    EXPECT_EQ(ReadAllAvailable(out_pipe[0], 100), "");
    odin_cli_client_test_liveness_t live{};
    odin_event_loop_test_liveness_t elive{};
    ASSERT_EQ(ReadWithDeadline(snap_pipe[0], &live, sizeof(live), 500),
              static_cast<ssize_t>(sizeof(live)));
    ASSERT_EQ(ReadWithDeadline(snap_pipe[0], &elive, sizeof(elive), 500),
              static_cast<ssize_t>(sizeof(elive)));
    EXPECT_EQ(live.live_listeners, 0u);
    EXPECT_EQ(live.live_accept_loops, 0u);
    EXPECT_EQ(live.live_sessions, 0u);
    EXPECT_EQ(elive.loops, 0u);
    close(out_pipe[0]);
    close(err_pipe[0]);
    close(snap_pipe[0]);
  }
}

TEST(OdinCliClientUnitTest, T5SignalsDestroyTwoInflightAndRestoreHandlers) {
  (void)signal(SIGPIPE, SIG_IGN);
  const int signals[] = {SIGINT, SIGTERM};
  for (const int sig : signals) {
    SCOPED_TRACE(sig == SIGINT ? "SIGINT" : "SIGTERM");
    uint16_t upstream_port = 0;
    const int fake = OpenIpv4Listener("127.0.0.1", 0, true, &upstream_port);
    ASSERT_GE(fake, 0) << std::strerror(errno);
    PausedRuntimeChild child = ForkPausedRuntimeChild(
        upstream_port, RuntimeFailKind::kSignalOnly, sig);
    ASSERT_NE(child.pid, -1);
    ChildGuard guard(child.pid);
    const std::string startup = ReadLineWithDeadline(child.stderr_fd, 2000);
    uint16_t proxy_port = 0;
    std::string server;
    ASSERT_TRUE(ParseClientStartupLine(startup, &proxy_port, &server))
        << startup;
    InflightPair inflight = StartTwoInflightSessions(proxy_port, fake);
    char progress = 0;
    ASSERT_EQ(
        ReadWithDeadline(child.progress_fd, &progress, 1, kLongDeadlineMs), 1);
    ASSERT_EQ(waitpid(child.pid, nullptr, WNOHANG), 0);
    EXPECT_EQ(kill(child.pid, sig), 0);

    DirectChildSnapshot snap{};
    ASSERT_TRUE(ReadSnapshot(child.snapshot_fd, &snap));
    ASSERT_EQ(waitpid(child.pid, nullptr, WNOHANG), 0);
    const int post = TcpConnectLoopback(proxy_port, 300);
    if (post >= 0) {
      close(post);
      ADD_FAILURE() << "listener still accepted while child paused";
    }
    ExpectInflightClosed(&inflight);
    EXPECT_EQ(snap.rc, 0);
    EXPECT_EQ(snap.sigint_count, 1);
    EXPECT_EQ(snap.sigterm_count, 1);
    EXPECT_GE(snap.cli.last_cleanup_sessions, 2u);
    EXPECT_EQ(snap.cli.live_listeners, 0u);
    EXPECT_EQ(snap.cli.live_accept_loops, 0u);
    EXPECT_EQ(snap.cli.live_sessions, 0u);
    EXPECT_EQ(snap.event_loop.loops, 0u);
    EXPECT_EQ(snap.event_loop.io_handles, 0u);
    EXPECT_EQ(snap.event_loop.timers, 0u);
    EXPECT_EQ(snap.event_loop.task_nodes, 0u);
    EXPECT_EQ(ReadAllAvailable(child.stderr_fd, 100), "");

    FinishPausedRuntimeChild(&child, 0);
    guard.disarm();
    CloseInflight(&inflight);
    close(fake);
  }
}

TEST(OdinCliClientUnitTest, T6RuntimeFailuresCleanUpTwoInflightSessions) {
  (void)signal(SIGPIPE, SIG_IGN);

  struct Subcase {
    const char *name;
    const char *line;
    RuntimeFailKind kind;
    bool exact_two;
  };
  const Subcase cases[] = {
      {"accept", "odin: client runtime failed at accept_loop\n",
       RuntimeFailKind::kAcceptErrno, false},
      {"fcntl_getfl", "odin: client runtime failed at accept_loop\n",
       RuntimeFailKind::kFcntlGetfl, true},
      {"fcntl_setfl", "odin: client runtime failed at accept_loop\n",
       RuntimeFailKind::kFcntlSetfl, true},
      {"event_loop", "odin: client runtime failed at event_loop_run\n",
       RuntimeFailKind::kEventLoopRun, false},
      {"unexpected_stop", "odin: client runtime failed at event_loop_run\n",
       RuntimeFailKind::kUnexpectedStop, false},
  };

  for (const auto &c : cases) {
    SCOPED_TRACE(c.name);
#if defined(__linux__)
    if (c.kind == RuntimeFailKind::kFcntlGetfl) {
      errno = 0;
      EXPECT_EQ(odin_cli_client_test_fail_next(
                    ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_GETFL_ERROR,
                    EINVAL),
                -1);
      EXPECT_EQ(errno, EOPNOTSUPP);
      GTEST_SKIP() << "Linux has no post-accept fcntl branch";
    }
    if (c.kind == RuntimeFailKind::kFcntlSetfl) {
      errno = 0;
      EXPECT_EQ(odin_cli_client_test_fail_next(
                    ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_SETFL_ERROR,
                    EINVAL),
                -1);
      EXPECT_EQ(errno, EOPNOTSUPP);
      GTEST_SKIP() << "Linux has no post-accept fcntl branch";
    }
#endif
    uint16_t upstream_port = 0;
    const int fake = OpenIpv4Listener("127.0.0.1", 0, true, &upstream_port);
    ASSERT_GE(fake, 0) << std::strerror(errno);
    PausedRuntimeChild child = ForkPausedRuntimeChild(upstream_port, c.kind, 0);
    ASSERT_NE(child.pid, -1);
    ChildGuard guard(child.pid);
    const std::string startup = ReadLineWithDeadline(child.stderr_fd, 2000);
    uint16_t proxy_port = 0;
    std::string server;
    ASSERT_TRUE(ParseClientStartupLine(startup, &proxy_port, &server))
        << startup;
    InflightPair inflight = StartTwoInflightSessions(proxy_port, fake);
    char progress = 0;
    ASSERT_EQ(
        ReadWithDeadline(child.progress_fd, &progress, 1, kLongDeadlineMs), 1);
    EXPECT_FALSE(WaitFd(child.snapshot_fd, POLLIN, 200, nullptr))
        << "runtime failpoint returned before trigger byte";
    ASSERT_EQ(waitpid(child.pid, nullptr, WNOHANG), 0);
    EXPECT_EQ(ReadAllAvailable(child.stderr_fd, 100), "");
    ASSERT_TRUE(WriteReleaseByte(child.trigger_write_fd));

    DirectChildSnapshot snap{};
    ASSERT_TRUE(ReadSnapshot(child.snapshot_fd, &snap));
    ASSERT_EQ(waitpid(child.pid, nullptr, WNOHANG), 0);
    const int post = TcpConnectLoopback(proxy_port, 300);
    if (post >= 0) {
      close(post);
      ADD_FAILURE() << "listener still accepted while child paused";
    }
    ExpectInflightClosed(&inflight);
    const std::string failure = ReadLineWithDeadline(child.stderr_fd, 500);
    EXPECT_EQ(failure, c.line);
    EXPECT_EQ(snap.rc, 1);
    EXPECT_EQ(snap.sigint_count, 1);
    EXPECT_EQ(snap.sigterm_count, 1);
    if (c.exact_two) {
      EXPECT_EQ(snap.cli.last_cleanup_sessions, 2u);
    } else {
      EXPECT_GE(snap.cli.last_cleanup_sessions, 2u);
    }
    EXPECT_EQ(snap.cli.live_listeners, 0u);
    EXPECT_EQ(snap.cli.live_accept_loops, 0u);
    EXPECT_EQ(snap.cli.live_sessions, 0u);
    EXPECT_EQ(snap.event_loop.loops, 0u);
    EXPECT_EQ(snap.event_loop.io_handles, 0u);
    EXPECT_EQ(snap.event_loop.timers, 0u);
    EXPECT_EQ(snap.event_loop.task_nodes, 0u);

    FinishPausedRuntimeChild(&child, 1);
    guard.disarm();
    CloseInflight(&inflight);
    close(fake);
  }
}

TEST(OdinCliClientUnitTest, T7StartupFailureMatrixAndBindCapture) {
  (void)signal(SIGPIPE, SIG_IGN);

  uint16_t occupied_port = 0;
  const int occupied = OpenIpv4Listener("127.0.0.1", 0, false, &occupied_port);
  ASSERT_GE(occupied, 0) << std::strerror(errno);
  SpawnedChild child = SpawnTcpClient(occupied_port, "127.0.0.1", 4433);
  ASSERT_NE(child.pid, -1);
  ChildGuard prod_guard(child.pid);
  int st = 0;
  ASSERT_EQ(WaitChildBounded(child.pid, kLongDeadlineMs, &st), 0);
  prod_guard.disarm();
  EXPECT_TRUE(WIFEXITED(st));
  EXPECT_EQ(WEXITSTATUS(st), 1);
  EXPECT_EQ(ReadAllAvailable(child.stderr_fd, 100),
            "odin: client startup failed at bind\n");
  EXPECT_EQ(ReadAllAvailable(child.stdout_fd, 100), "");
  const int probe = TcpConnectLoopback(occupied_port, kShortDeadlineMs);
  ASSERT_GE(probe, 0) << std::strerror(errno);
  const int accepted = AcceptWithDeadline(occupied, kShortDeadlineMs);
  ASSERT_GE(accepted, 0) << std::strerror(errno);
  close(probe);
  close(accepted);
  close(occupied);
  close(child.stdout_fd);
  close(child.stderr_fd);

  odin_cli_client_test_reset_liveness();
  odin_event_loop_test_reset_liveness();
  int temp_pipe[2];
  ASSERT_EQ(pipe(temp_pipe), 0);
  errno = 0;
  EXPECT_EQ(odin_cli_client_test_fail_next(ODIN_CLI_CLIENT_TEST_FAIL_NONE, EIO),
            -1);
  EXPECT_EQ(errno, EINVAL);
  errno = 0;
  EXPECT_EQ(odin_cli_client_test_fail_next(ODIN_CLI_CLIENT_TEST_FAIL_SOCKET, 0),
            -1);
  EXPECT_EQ(errno, EINVAL);
  errno = 0;
  EXPECT_EQ(odin_cli_client_test_trigger_next(ODIN_CLI_CLIENT_TEST_FAIL_SOCKET),
            -1);
  EXPECT_EQ(errno, EINVAL);
  errno = 0;
  EXPECT_EQ(odin_cli_client_test_set_runtime_trigger_fd(-1), -1);
  EXPECT_EQ(errno, EINVAL);
  errno = 0;
  EXPECT_EQ(odin_cli_client_test_set_progress_fd(-1, 1), -1);
  EXPECT_EQ(errno, EINVAL);
  errno = 0;
  EXPECT_EQ(odin_cli_client_test_set_progress_fd(temp_pipe[1], 0), -1);
  EXPECT_EQ(errno, EINVAL);
  errno = 0;
  EXPECT_EQ(odin_cli_client_test_set_idle_snapshot_fd(-1, 1), -1);
  EXPECT_EQ(errno, EINVAL);
  errno = 0;
  EXPECT_EQ(odin_cli_client_test_set_idle_snapshot_fd(temp_pipe[1], 0), -1);
  EXPECT_EQ(errno, EINVAL);
  errno = 0;
  EXPECT_EQ(odin_client_session_test_fail_next_create(0), -1);
  EXPECT_EQ(errno, EINVAL);

  uint16_t invalid_server_port = 0;
  const int invalid_fake =
      OpenIpv4Listener("127.0.0.1", 0, true, &invalid_server_port);
  ASSERT_GE(invalid_fake, 0) << std::strerror(errno);
  int invalid_out_pipe[2];
  int invalid_err_pipe[2];
  ASSERT_EQ(pipe(invalid_out_pipe), 0);
  ASSERT_EQ(pipe(invalid_err_pipe), 0);
  const pid_t invalid_pid = fork();
  ASSERT_NE(invalid_pid, -1);
  if (invalid_pid == 0) {
    close(invalid_out_pipe[0]);
    close(invalid_err_pipe[0]);
    dup2(invalid_out_pipe[1], STDOUT_FILENO);
    dup2(invalid_err_pipe[1], STDERR_FILENO);
    close(invalid_out_pipe[1]);
    close(invalid_err_pipe[1]);
    const int rc =
        RunTcpClientConfig(0, "127.0.0.1", invalid_server_port, stderr);
    _exit(rc);
  }
  close(invalid_out_pipe[1]);
  close(invalid_err_pipe[1]);
  ChildGuard invalid_guard(invalid_pid);
  const std::string invalid_line =
      ReadLineWithDeadline(invalid_err_pipe[0], 2000);
  uint16_t invalid_proxy_port = 0;
  std::string invalid_server;
  ASSERT_TRUE(ParseClientStartupLine(invalid_line, &invalid_proxy_port,
                                     &invalid_server))
      << invalid_line;
  ASSERT_GT(invalid_proxy_port, 0);
  int invalid_client = -1;
  int invalid_upstream = -1;
  ExerciseOneSuccessfulConnect(
      invalid_proxy_port, invalid_fake,
      "CONNECT invalid-clear.example:443 HTTP/1.1\r\n\r\n",
      "invalid-clear.example", 443, &invalid_client, &invalid_upstream);
  shutdown(invalid_client, SHUT_WR);
  shutdown(invalid_upstream, SHUT_WR);
  EXPECT_TRUE(DrainUntilEofOrReset(invalid_client, kShortDeadlineMs));
  EXPECT_TRUE(DrainUntilEofOrReset(invalid_upstream, kShortDeadlineMs));
  close(invalid_client);
  close(invalid_upstream);
  EXPECT_FALSE(WaitFd(temp_pipe[0], POLLIN, 150, nullptr))
      << "invalid helper call armed later hook state";
  EXPECT_EQ(kill(invalid_pid, SIGTERM), 0);
  ASSERT_EQ(WaitChildBounded(invalid_pid, kLongDeadlineMs, &st), 0);
  invalid_guard.disarm();
  EXPECT_TRUE(WIFEXITED(st));
  EXPECT_EQ(WEXITSTATUS(st), 0);
  EXPECT_EQ(ReadAllAvailable(invalid_out_pipe[0], 100), "");
  close(invalid_out_pipe[0]);
  close(invalid_err_pipe[0]);
  close(invalid_fake);
  close(temp_pipe[0]);
  close(temp_pipe[1]);
  odin_cli_client_test_liveness_t invalid_live{};
  ASSERT_EQ(odin_cli_client_test_liveness(&invalid_live), 0);
  EXPECT_EQ(invalid_live.live_listeners, 0u);
  EXPECT_EQ(invalid_live.live_accept_loops, 0u);
  EXPECT_EQ(invalid_live.live_sessions, 0u);

  struct Failcase {
    const char *line;
    odin_cli_client_test_failpoint_t fp;
    bool bind_recorded;
  };
  const Failcase cases[] = {
      {"odin: client startup failed at socket\n",
       ODIN_CLI_CLIENT_TEST_FAIL_SOCKET, false},
      {"odin: client startup failed at setsockopt(SO_REUSEADDR)\n",
       ODIN_CLI_CLIENT_TEST_FAIL_SETSOCKOPT_REUSEADDR, false},
      {"odin: client startup failed at fcntl(F_GETFL)\n",
       ODIN_CLI_CLIENT_TEST_FAIL_FCNTL_GETFL, false},
      {"odin: client startup failed at fcntl(F_SETFL)\n",
       ODIN_CLI_CLIENT_TEST_FAIL_FCNTL_SETFL, false},
      {"odin: client startup failed at bind\n", ODIN_CLI_CLIENT_TEST_FAIL_BIND,
       false},
      {"odin: client startup failed at listen\n",
       ODIN_CLI_CLIENT_TEST_FAIL_LISTEN, true},
      {"odin: client startup failed at getsockname\n",
       ODIN_CLI_CLIENT_TEST_FAIL_GETSOCKNAME, true},
      {"odin: client startup failed at event_loop_create\n",
       ODIN_CLI_CLIENT_TEST_FAIL_EVENT_LOOP_CREATE, true},
      {"odin: client startup failed at accept_loop_create\n",
       ODIN_CLI_CLIENT_TEST_FAIL_ACCEPT_LOOP_CREATE, true},
      {"odin: client startup failed at sigaction(SIGINT)\n",
       ODIN_CLI_CLIENT_TEST_FAIL_SIGACTION_SIGINT, true},
      {"odin: client startup failed at sigaction(SIGTERM)\n",
       ODIN_CLI_CLIENT_TEST_FAIL_SIGACTION_SIGTERM, true},
      {"odin: client startup failed at signal_timer_start\n",
       ODIN_CLI_CLIENT_TEST_FAIL_SIGNAL_TIMER_START, true},
  };

  for (const auto &fc : cases) {
    SCOPED_TRACE(fc.line);
    const uint16_t port = AllocFreeLoopbackPort();
    ASSERT_GT(port, 0);
    odin_cli_client_test_reset_liveness();
    odin_event_loop_test_reset_liveness();
    struct sigaction old_int;
    struct sigaction old_term;
    std::memset(&old_int, 0, sizeof(old_int));
    std::memset(&old_term, 0, sizeof(old_term));
    if (fc.fp == ODIN_CLI_CLIENT_TEST_FAIL_SIGACTION_SIGTERM) {
      struct sigaction sa_int;
      std::memset(&sa_int, 0, sizeof(sa_int));
      sa_int.sa_handler = CountingSigint;
      sigemptyset(&sa_int.sa_mask);
      g_child_sigint_count = 0;
      ASSERT_EQ(sigaction(SIGINT, &sa_int, &old_int), 0);
    } else if (fc.fp == ODIN_CLI_CLIENT_TEST_FAIL_SIGNAL_TIMER_START) {
      InstallCountingHandlers(&old_int, &old_term);
    }
    ASSERT_EQ(odin_cli_client_test_fail_next(fc.fp, EIO), 0);
    char err_buf[512] = {0};
    FILE *err = fmemopen(err_buf, sizeof(err_buf), "w");
    ASSERT_NE(err, nullptr);
    const int rc = RunTcpClientConfig(port, "127.0.0.1", 4433, err);
    (void)fclose(err);
    EXPECT_EQ(rc, 1);
    EXPECT_STREQ(err_buf, fc.line);
    odin_cli_client_test_liveness_t live{};
    odin_event_loop_test_liveness_t elive{};
    EXPECT_EQ(odin_cli_client_test_liveness(&live), 0);
    EXPECT_EQ(odin_event_loop_test_liveness(&elive), 0);
    EXPECT_EQ(live.live_listeners, 0u);
    EXPECT_EQ(live.live_accept_loops, 0u);
    EXPECT_EQ(live.live_sessions, 0u);
    EXPECT_EQ(elive.loops, 0u);
    EXPECT_EQ(elive.io_handles, 0u);
    EXPECT_EQ(elive.timers, 0u);
    EXPECT_EQ(elive.task_nodes, 0u);
    if (fc.bind_recorded) {
      struct sockaddr_in last{};
      EXPECT_EQ(odin_cli_client_test_last_bind_addr(&last), 0);
      EXPECT_EQ(last.sin_family, AF_INET);
      EXPECT_EQ(last.sin_addr.s_addr, htonl(INADDR_LOOPBACK));
      EXPECT_EQ(ntohs(last.sin_port), port);
      const int leftover = TcpConnectLoopback(port, 300);
      if (leftover >= 0) {
        close(leftover);
        ADD_FAILURE() << "partial listener left open";
      }
    }
    if (fc.fp == ODIN_CLI_CLIENT_TEST_FAIL_SIGACTION_SIGTERM) {
      (void)raise(SIGINT);
      EXPECT_EQ(g_child_sigint_count, 1);
      (void)sigaction(SIGINT, &old_int, nullptr);
    } else if (fc.fp == ODIN_CLI_CLIENT_TEST_FAIL_SIGNAL_TIMER_START) {
      (void)raise(SIGINT);
      (void)raise(SIGTERM);
      EXPECT_EQ(g_child_sigint_count, 1);
      EXPECT_EQ(g_child_sigterm_count, 1);
      RestoreHandlers(&old_int, &old_term);
    }
  }
}

// NOLINTEND(misc-const-correctness, misc-use-internal-linkage)
