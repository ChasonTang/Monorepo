// odin/testing/cli_client_unittests.cpp
//
// RFC-024 §5 process-level and unit-level tests for the CLI client runner.

#include "odin/cli.h"
#include "odin/protocol.h"
#include "odin/testing/cli_client_internal_test.h"
#include "odin/testing/client_session_internal_test.h"
#include "odin/testing/event_loop_internal_test.h"

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

extern "C" {
extern char **environ;
}

extern std::string g_test_argv0;

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
                                          RuntimeFailKind kind, int signal_num) {
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
            ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_GETFL_ERROR,
            EINVAL);
        break;
      case RuntimeFailKind::kFcntlSetfl:
        hook_rc = odin_cli_client_test_fail_next(
            ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_SETFL_ERROR,
            EINVAL);
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

    char a0[] = "odin-client";
    char a1[] = "--listen";
    char a2[] = "0";
    char a3[] = "--server";
    std::string server = "127.0.0.1:" + std::to_string(server_port);
    char *a4 = &server[0];
    char *const argv[] = {a0, a1, a2, a3, a4, nullptr};
    const int rc = odin_cli_main(5, argv, stdout, stderr);

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
  return {pid, err_pipe[0], snap_pipe[0], release_pipe[1], progress_pipe[0],
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

TEST(OdinCliClientProcessTest, T1EphemeralStartupReportsReadyLoopback) {
  ASSERT_FALSE(g_test_argv0.empty());
  const std::string client_path = Dirname(g_test_argv0) + "/odin-client";

  SpawnedChild child = SpawnOdinClient(
      client_path, {"--listen", "0", "--server", "127.0.0.1:4433"});
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
    const int fake =
        OpenIpv4Listener("127.0.0.1", 0, true, &upstream_port);
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
      char a0[] = "odin-client";
      char a1[] = "--listen";
      char a2[] = "0";
      char a3[] = "--server";
      std::string server = "127.0.0.1:" + std::to_string(upstream_port);
      char *a4 = &server[0];
      char *const argv[] = {a0, a1, a2, a3, a4, nullptr};
      const int rc = odin_cli_main(5, argv, stdout, stderr);
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
    EXPECT_TRUE(DrainUntilEofOrResetExpectingNoBytes(client_a,
                                                     kShortDeadlineMs));
    close(client_a);
    ExpectNoAccept(fake, 150);

    int client_b = -1;
    int upstream_b = -1;
    ExerciseOneSuccessfulConnect(proxy_port, fake,
                                 "CONNECT later.example:8443 HTTP/1.1\r\n\r\n",
                                 "later.example", 8443, &client_b,
                                 &upstream_b);
    shutdown(client_b, SHUT_WR);
    shutdown(upstream_b, SHUT_WR);
    EXPECT_TRUE(DrainUntilEofOrReset(client_b, kShortDeadlineMs));
    EXPECT_TRUE(DrainUntilEofOrReset(upstream_b, kShortDeadlineMs));

    odin_cli_client_test_liveness_t idle{};
    ASSERT_EQ(ReadWithDeadline(snap_pipe[0], &idle, sizeof(idle),
                               kLongDeadlineMs),
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
  const std::vector<std::string> servers = {"example.com:443", "[::1]:443"};
  for (const auto &server_arg : servers) {
    SCOPED_TRACE(server_arg);
    MutableArgv parse_argv({"odin-client", "--listen", "0", "--server",
                            server_arg.c_str()});
    odin_cli_args_t parsed{};
    ASSERT_EQ(odin_cli_parse(parse_argv.argc(), parse_argv.argv(), &parsed),
              ODIN_CLI_OK);

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
      MutableArgv argv({"odin-client", "--listen", "0", "--server",
                        server_arg.c_str()});
      const int rc = odin_cli_main(argv.argc(), argv.argv(), out, err);
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
    const int fake =
        OpenIpv4Listener("127.0.0.1", 0, true, &upstream_port);
    ASSERT_GE(fake, 0) << std::strerror(errno);
    PausedRuntimeChild child =
        ForkPausedRuntimeChild(upstream_port, RuntimeFailKind::kSignalOnly, sig);
    ASSERT_NE(child.pid, -1);
    ChildGuard guard(child.pid);
    const std::string startup = ReadLineWithDeadline(child.stderr_fd, 2000);
    uint16_t proxy_port = 0;
    std::string server;
    ASSERT_TRUE(ParseClientStartupLine(startup, &proxy_port, &server))
        << startup;
    InflightPair inflight = StartTwoInflightSessions(proxy_port, fake);
    char progress = 0;
    ASSERT_EQ(ReadWithDeadline(child.progress_fd, &progress, 1,
                               kLongDeadlineMs),
              1);
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
    const int fake =
        OpenIpv4Listener("127.0.0.1", 0, true, &upstream_port);
    ASSERT_GE(fake, 0) << std::strerror(errno);
    PausedRuntimeChild child =
        ForkPausedRuntimeChild(upstream_port, c.kind, 0);
    ASSERT_NE(child.pid, -1);
    ChildGuard guard(child.pid);
    const std::string startup = ReadLineWithDeadline(child.stderr_fd, 2000);
    uint16_t proxy_port = 0;
    std::string server;
    ASSERT_TRUE(ParseClientStartupLine(startup, &proxy_port, &server))
        << startup;
    InflightPair inflight = StartTwoInflightSessions(proxy_port, fake);
    char progress = 0;
    ASSERT_EQ(ReadWithDeadline(child.progress_fd, &progress, 1,
                               kLongDeadlineMs),
              1);
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
  ASSERT_FALSE(g_test_argv0.empty());
  const std::string client_path = Dirname(g_test_argv0) + "/odin-client";

  uint16_t occupied_port = 0;
  const int occupied =
      OpenIpv4Listener("127.0.0.1", 0, false, &occupied_port);
  ASSERT_GE(occupied, 0) << std::strerror(errno);
  SpawnedChild child = SpawnOdinClient(
      client_path,
      {"--listen", std::to_string(occupied_port), "--server",
       "127.0.0.1:4433"});
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
  EXPECT_EQ(odin_cli_client_test_fail_next(
                static_cast<odin_cli_client_test_failpoint_t>(0), EIO),
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
    char a0[] = "odin-client";
    char a1[] = "--listen";
    char a2[] = "0";
    char a3[] = "--server";
    std::string server = "127.0.0.1:" + std::to_string(invalid_server_port);
    char *a4 = &server[0];
    char *const argv[] = {a0, a1, a2, a3, a4, nullptr};
    const int rc = odin_cli_main(5, argv, stdout, stderr);
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
    char a0[] = "odin-client";
    char a1[] = "--listen";
    std::string ps = std::to_string(port);
    char *a2 = &ps[0];
    char a3[] = "--server";
    char a4[] = "127.0.0.1:4433";
    char *const argv[] = {a0, a1, a2, a3, a4, nullptr};
    const int rc = odin_cli_main(5, argv, stdout, err);
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
