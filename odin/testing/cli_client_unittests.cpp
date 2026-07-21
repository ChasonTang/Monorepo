// odin/testing/cli_client_unittests.cpp
//
// RFC-024 §5 process-level and unit-level tests for the CLI client runner.

#include "odin/cli.h"
#include "odin/cli_client.h"
#include "odin/testing/cli_client_internal_test.h"
#include "odin/testing/client_session_internal_test.h"
#include "odin/testing/client_xqc_runtime_internal_test.h"
#include "odin/testing/dns_resolver_internal_test.h"
#include "odin/testing/event_loop_internal_test.h"
#include "odin/testing/xqc_udp_internal_test.h"

#include <ares.h>

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

extern std::string g_test_argv0;

namespace {

constexpr int kShortDeadlineMs = 1500;
constexpr int kLongDeadlineMs = 4000;
constexpr int kRfc032FakeFailureExit = 122;

volatile sig_atomic_t g_child_sigint_count = 0;
volatile sig_atomic_t g_child_sigterm_count = 0;

std::string Dirname(const std::string &path) {
  const auto pos = path.find_last_of('/');
  if (pos == std::string::npos) {
    return ".";
  }
  return path.substr(0, pos);
}

std::string BuildCertDir() {
  return Dirname(g_test_argv0) + "/gen/thor/odin_test_certs";
}

std::string ClientCaFile() { return BuildCertDir() + "/root-ca.pem"; }

std::string ActiveFilter() { return GTEST_FLAG_GET(filter); }

std::vector<std::string> QuicClientArgs(const std::string &server) {
  return {"odin-client", "--listen",  "0",           "--server",
          server,        "--ca-file", ClientCaFile()};
}

std::vector<std::string> QuicClientArgs() {
  return QuicClientArgs("127.0.0.1:4433");
}

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

} // namespace

namespace {

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
  odin_cli_client_test_dns_timing_t dns_timing;
  struct sockaddr_in bind_addr;
  int bind_addr_ok;
  struct sockaddr_in bound_addr;
  int bound_addr_ok;
  odin_dns_resolver_test_liveness_t dns;
  odin_dns_resolver_test_cares_observation_t dns_obs;
  odin_xqc_client_runtime_test_record_t runtime_record;
  int fake_failures;
  char fake_failure[256];
  int process_exited;
  int process_status;
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
int g_rfc032_fake_failures;
char g_rfc032_fake_failure[256];

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
  (void)odin_cli_client_test_dns_timing(&snap->dns_timing);
  snap->bind_addr_ok =
      odin_cli_client_test_last_bind_addr(&snap->bind_addr) == 0 ? 1 : 0;
  snap->bound_addr_ok =
      odin_cli_client_test_last_bound_addr(&snap->bound_addr) == 0 ? 1 : 0;
  (void)odin_dns_resolver_test_liveness(&snap->dns);
  (void)odin_dns_resolver_test_cares_observation(&snap->dns_obs);
  snap->runtime_record = *odin_xqc_client_runtime_test_record();
  snap->fake_failures = g_rfc032_fake_failures;
  memcpy(snap->fake_failure, g_rfc032_fake_failure, sizeof(snap->fake_failure));
}

Rfc028QuicDirectRun RunRfc028QuicDirect(
    const std::vector<std::string> &tokens,
    odin_cli_client_test_failpoint_t failpoint = ODIN_CLI_CLIENT_TEST_FAIL_NONE,
    int fail_errno = 0, bool trigger = false, bool reset_cli = true) {
  if (reset_cli) {
    odin_cli_client_test_reset_liveness();
  }
  odin_event_loop_test_reset_liveness();
  EXPECT_EQ(odin_dns_resolver_test_reset_liveness(), 0) << std::strerror(errno);
  g_rfc032_fake_failures = 0;
  std::memset(g_rfc032_fake_failure, 0, sizeof(g_rfc032_fake_failure));
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
    if (odin_dns_resolver_test_reset_liveness() != 0) {
      _exit(122);
    }
    g_rfc032_fake_failures = 0;
    std::memset(g_rfc032_fake_failure, 0, sizeof(g_rfc032_fake_failure));
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
  EXPECT_EQ(snap.cli.live_xqc_client_runtimes, 0u);
  EXPECT_EQ(snap.event_loop.loops, 0u);
  EXPECT_EQ(snap.event_loop.io_handles, 0u);
  EXPECT_EQ(snap.event_loop.timers, 0u);
  EXPECT_EQ(snap.event_loop.task_nodes, 0u);
}

} // namespace

TEST(OdinRFC028ClientTransportTest, T3QuicStartupCreatesRuntimeAfterListener) {
  const std::string ca = ClientCaFile();
  Rfc028QuicChild child = SpawnRfc028QuicChild(QuicClientArgs());
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
  EXPECT_STREQ(snap.runtime_config.quic_ca_file_value, ca.c_str());
  EXPECT_EQ(snap.cli.quic_runtime_create_calls, 1u);
  EXPECT_EQ(snap.cli.quic_runtime_start_calls, 1u);
  EXPECT_EQ(snap.cli.quic_runtime_force_destroy_calls, 1u);
  EXPECT_EQ(snap.runtime_record.default_create_calls, 1u);
  EXPECT_EQ(snap.runtime_record.last_default_create.engine_config, nullptr);
  EXPECT_STREQ(snap.runtime_record.last_default_create.ca_file_value,
               ca.c_str());
  EXPECT_EQ(snap.runtime_record.last_default_create.token, nullptr);
  EXPECT_EQ(snap.runtime_record.last_default_create.token_len, 0u);
  EXPECT_EQ(snap.runtime_record.last_default_create.conn_ssl_config_value
                .cert_verify_flag,
            XQC_TLS_CERT_FLAG_NEED_VERIFY);
  EXPECT_NE(snap.runtime_record.last_default_create.transport_callbacks_value
                .cert_verify_cb,
            nullptr);
  EXPECT_EQ(snap.runtime_record.last_default_create.no_crypto_flag, 0);
  EXPECT_EQ(snap.runtime_record.ca_store_load_successes, 1u);
  EXPECT_EQ(snap.runtime_record.ca_store_free_calls, 1u);
  EXPECT_EQ(snap.runtime_record.runtime_free_calls, 1u);
  ExpectRfc028QuicClean(snap);
}

TEST(OdinCliClientCaFileTest, T3RunnerForwardsRequiredCaFileAndRejectsOmit) {
  const std::string ca = ClientCaFile();
  Rfc028QuicChild supplied = SpawnRfc028QuicChild(QuicClientArgs());
  ChildGuard supplied_guard(supplied.pid);
  std::string line = ReadLineWithDeadline(supplied.stderr_fd, 2000);
  uint16_t proxy_port = 0;
  std::string server;
  ASSERT_TRUE(ParseQuicStartupLine(line, &proxy_port, &server)) << line;
  EXPECT_EQ(server, "127.0.0.1:4433");
  Rfc028QuicChildSnapshot supplied_snap =
      FinishRfc028QuicChild(&supplied, SIGTERM);
  supplied_guard.disarm();
  EXPECT_EQ(supplied_snap.rc, 0);
  close(supplied.stderr_fd);
  ASSERT_TRUE(supplied_snap.runtime_config_ok);
  EXPECT_STREQ(supplied_snap.runtime_config.quic_ca_file_value, ca.c_str());
  EXPECT_STREQ(supplied_snap.runtime_record.last_default_create.ca_file_value,
               ca.c_str());
  EXPECT_EQ(supplied_snap.runtime_record.last_default_create
                .conn_ssl_config_value.cert_verify_flag,
            XQC_TLS_CERT_FLAG_NEED_VERIFY);
  EXPECT_NE(supplied_snap.runtime_record.last_default_create
                .transport_callbacks_value.cert_verify_cb,
            nullptr);
  EXPECT_EQ(supplied_snap.runtime_record.ca_store_load_successes, 1u);
  EXPECT_EQ(supplied_snap.runtime_record.ca_store_free_calls, 1u);
  ExpectRfc028QuicClean(supplied_snap);

  Rfc028QuicDirectRun omitted = RunRfc028QuicDirect(
      {"odin-client", "--listen", "0", "--server", "127.0.0.1:4433"});
  EXPECT_EQ(omitted.rc, 2);
  EXPECT_EQ(omitted.err,
            "odin: missing required flag\n"
            "usage: 'odin-client --listen ADDR --server ADDR --ca-file FILE' "
            "or 'odin-server --listen ADDR --quic-cert FILE --quic-key "
            "FILE'\n");
  EXPECT_EQ(omitted.snapshot.runtime_record.default_create_calls, 0u);
  ExpectRfc028QuicClean(omitted.snapshot);
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
    Rfc028QuicDirectRun run = RunRfc028QuicDirect(QuicClientArgs(), c.fp, EIO);
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
  Rfc028QuicChild child = SpawnRfc028QuicChild(QuicClientArgs());
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
      QuicClientArgs(),
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
    Rfc028QuicDirectRun run =
        RunRfc028QuicDirect(QuicClientArgs(), c.fp, c.errnum, c.trigger);
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
    Rfc028QuicChild child =
        SpawnRfc028QuicChild(QuicClientArgs(), c.fp, EIO, 0, true);
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
        QuicClientArgs(), ODIN_CLI_CLIENT_TEST_FAIL_NONE, 0, 1);
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
    Rfc028QuicDirectRun run = RunRfc028QuicDirect(QuicClientArgs(), c.fp, EIO);
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

  odin_cli_client_config_t quic{};
  quic.listen_port = 0;
  quic.server_host = "127.0.0.1";
  quic.server_host_len = std::strlen(quic.server_host);
  quic.server_port = 4433;
  char missing_ca_buf[128] = {0};
  FILE *missing_ca_err = fmemopen(missing_ca_buf, sizeof(missing_ca_buf), "w");
  ASSERT_NE(missing_ca_err, nullptr);
  EXPECT_EQ(odin_cli_run_client(&quic, missing_ca_err), 1);
  (void)fclose(missing_ca_err);
  EXPECT_STREQ(missing_ca_buf, "odin: client startup failed at config\n");

  quic.quic_ca_file = "";
  char empty_ca_buf[128] = {0};
  FILE *empty_ca_err = fmemopen(empty_ca_buf, sizeof(empty_ca_buf), "w");
  ASSERT_NE(empty_ca_err, nullptr);
  EXPECT_EQ(odin_cli_run_client(&quic, empty_ca_err), 1);
  (void)fclose(empty_ca_err);
  EXPECT_STREQ(empty_ca_buf, "odin: client startup failed at config\n");

  const std::string ca = ClientCaFile();
  quic.quic_ca_file = ca.c_str();
  EXPECT_EQ(odin_cli_run_client(&quic, nullptr), 1);

  odin_cli_client_test_liveness_t live{};
  odin_event_loop_test_liveness_t elive{};
  ASSERT_EQ(odin_cli_client_test_liveness(&live), 0);
  ASSERT_EQ(odin_event_loop_test_liveness(&elive), 0);
  EXPECT_EQ(live.live_listeners, 0u);
  EXPECT_EQ(live.live_accept_loops, 0u);
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
      QuicClientArgs(), ODIN_CLI_CLIENT_TEST_FAIL_NONE, 0, false, false);
  EXPECT_EQ(run.rc, 1);
  EXPECT_EQ(run.err,
            "odin: client startup failed at xqc_client_runtime_create\n");
  ExpectRfc028QuicClean(run.snapshot);
}

namespace {

struct Rfc032FakeExpect {
  char server_host[ODIN_PROTO_HOST_MAX + 1];
  int peer_family;
  char peer_addr[INET6_ADDRSTRLEN];
  uint16_t peer_port;
  int local_family;
  char local_addr[INET6_ADDRSTRLEN];
  uint16_t local_port;
  size_t connect_calls;
};

struct Rfc032Setup {
  bool install_fake;
  Rfc032FakeExpect fake;
  std::vector<odin_dns_resolver_test_cares_step_t> steps;
  std::vector<std::vector<odin_dns_addr_t>> addr_results;
  bool invalid_addr_push_first;
  odin_cli_client_test_failpoint_t failpoint;
  int fail_errno;
  bool trigger;
  size_t progress_min_quic_adds;
};

size_t g_rfc032_connect_calls;
Rfc032FakeExpect g_rfc032_expect;

void Rfc032CopyString(char *dst, size_t dst_len, const char *src) {
  std::memset(dst, 0, dst_len);
  if (src == nullptr) {
    return;
  }
  std::strncpy(dst, src, dst_len - 1u);
}

Rfc032FakeExpect Rfc032Expect(const char *server_host, int peer_family,
                              const char *peer_addr, uint16_t peer_port,
                              int local_family, const char *local_addr,
                              uint16_t local_port, size_t connect_calls) {
  Rfc032FakeExpect out{};
  Rfc032CopyString(out.server_host, sizeof(out.server_host), server_host);
  out.peer_family = peer_family;
  Rfc032CopyString(out.peer_addr, sizeof(out.peer_addr), peer_addr);
  out.peer_port = peer_port;
  out.local_family = local_family;
  Rfc032CopyString(out.local_addr, sizeof(out.local_addr), local_addr);
  out.local_port = local_port;
  out.connect_calls = connect_calls;
  return out;
}

odin_dns_resolver_test_cares_step_t Rfc032Step(int op, int status) {
  odin_dns_resolver_test_cares_step_t step{};
  step.op = op;
  step.status = status;
  return step;
}

odin_dns_addr_t Rfc032Addr4(const char *ip, uint16_t port, socklen_t addrlen) {
  odin_dns_addr_t out{};
  auto *addr = reinterpret_cast<struct sockaddr_in *>(&out.addr);
  addr->sin_family = AF_INET;
  addr->sin_port = htons(port);
  EXPECT_EQ(inet_pton(AF_INET, ip, &addr->sin_addr), 1);
  out.addrlen = addrlen;
  out.ttl = 60;
  return out;
}

odin_dns_addr_t Rfc032Addr6(const char *ip, uint16_t port, socklen_t addrlen) {
  odin_dns_addr_t out{};
  auto *addr = reinterpret_cast<struct sockaddr_in6 *>(&out.addr);
  addr->sin6_family = AF_INET6;
  addr->sin6_port = htons(port);
  EXPECT_EQ(inet_pton(AF_INET6, ip, &addr->sin6_addr), 1);
  out.addrlen = addrlen;
  out.ttl = 60;
  return out;
}

odin_dns_addr_t Rfc032AddrUnix() {
  odin_dns_addr_t out{};
  out.addr.ss_family = AF_UNIX;
  out.addrlen = static_cast<socklen_t>(sizeof(struct sockaddr_storage));
  out.ttl = 60;
  return out;
}

void Rfc032RecordFakeFailure(const char *msg) {
  if (g_rfc032_fake_failures == 0) {
    Rfc032CopyString(g_rfc032_fake_failure, sizeof(g_rfc032_fake_failure), msg);
  }
  g_rfc032_fake_failures += 1;
}

void Rfc032Check(bool ok, const char *msg) {
  if (!ok) {
    Rfc032RecordFakeFailure(msg);
  }
}

bool Rfc032SockaddrMatches(const struct sockaddr *addr, socklen_t addrlen,
                           int family, const char *ip, uint16_t port) {
  if (addr == nullptr || addr->sa_family != family) {
    return false;
  }
  char text[INET6_ADDRSTRLEN] = {0};
  if (family == AF_INET) {
    if (addrlen != static_cast<socklen_t>(sizeof(struct sockaddr_in))) {
      return false;
    }
    const auto *in = reinterpret_cast<const struct sockaddr_in *>(addr);
    if (ntohs(in->sin_port) != port) {
      return false;
    }
    return inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text)) != nullptr &&
           std::strcmp(text, ip) == 0;
  }
  if (family == AF_INET6) {
    if (addrlen != static_cast<socklen_t>(sizeof(struct sockaddr_in6))) {
      return false;
    }
    const auto *in6 = reinterpret_cast<const struct sockaddr_in6 *>(addr);
    if (ntohs(in6->sin6_port) != port) {
      return false;
    }
    return inet_ntop(AF_INET6, &in6->sin6_addr, text, sizeof(text)) !=
               nullptr &&
           std::strcmp(text, ip) == 0;
  }
  return false;
}

xqc_engine_t *Rfc032QuicEngineCreate(
    xqc_engine_type_t engine_type, const xqc_config_t *engine_config,
    const xqc_engine_ssl_config_t *ssl_config,
    const xqc_engine_callback_t *engine_callback,
    const xqc_transport_callbacks_t *transport_cbs, void *user_data) {
  (void)engine_config;
  (void)ssl_config;
  (void)engine_callback;
  (void)transport_cbs;
  Rfc032Check(engine_type == XQC_ENGINE_CLIENT, "engine type");
  g_rfc028_quic_fake.xqc_user_data = user_data;
  return g_rfc028_quic_fake.engine;
}

void Rfc032QuicEngineDestroy(xqc_engine_t *engine) {
  Rfc032Check(engine == g_rfc028_quic_fake.engine, "engine destroy");
}

xqc_int_t Rfc032QuicEngineRegisterAlpn(xqc_engine_t *engine, const char *alpn,
                                       size_t alpn_len,
                                       xqc_app_proto_callbacks_t *app_callbacks,
                                       void *user_data) {
  (void)user_data;
  Rfc032Check(engine == g_rfc028_quic_fake.engine, "register engine");
  Rfc032Check(std::string(alpn, alpn_len) == ODIN_XQC_CLIENT_ALPN,
              "register alpn");
  g_rfc028_quic_fake.app_callbacks = app_callbacks;
  return XQC_OK;
}

xqc_int_t Rfc032QuicEngineUnregisterAlpn(xqc_engine_t *engine, const char *alpn,
                                         size_t alpn_len) {
  Rfc032Check(engine == g_rfc028_quic_fake.engine, "unregister engine");
  Rfc032Check(std::string(alpn, alpn_len) == ODIN_XQC_CLIENT_ALPN,
              "unregister alpn");
  return XQC_OK;
}

const xqc_cid_t *Rfc032QuicConnect(
    xqc_engine_t *engine, const xqc_conn_settings_t *conn_settings,
    const unsigned char *token, unsigned int token_len, const char *server_host,
    int no_crypto_flag, const xqc_conn_ssl_config_t *conn_ssl_config,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen, const char *alpn,
    void *user_data) {
  (void)conn_settings;
  (void)token;
  (void)token_len;
  (void)conn_ssl_config;
  g_rfc032_connect_calls += 1;
  Rfc032Check(engine == g_rfc028_quic_fake.engine, "connect engine");
  Rfc032Check(server_host != nullptr &&
                  std::strcmp(server_host, g_rfc032_expect.server_host) == 0,
              "connect server_host");
  Rfc032Check(no_crypto_flag == 0, "connect crypto flag");
  Rfc032Check(Rfc032SockaddrMatches(
                  peer_addr, peer_addrlen, g_rfc032_expect.peer_family,
                  g_rfc032_expect.peer_addr, g_rfc032_expect.peer_port),
              "connect peer address");
  Rfc032Check(alpn != nullptr && std::strcmp(alpn, ODIN_XQC_CLIENT_ALPN) == 0,
              "connect alpn");
  Rfc032Check(g_rfc028_quic_fake.app_callbacks != nullptr, "connect callbacks");
  if (g_rfc028_quic_fake.app_callbacks != nullptr) {
    Rfc032Check(g_rfc028_quic_fake.app_callbacks->conn_cbs.conn_create_notify(
                    g_rfc028_quic_fake.conn, &g_rfc028_quic_fake.cid, user_data,
                    nullptr) == 0,
                "connect create notify");
  }
  return &g_rfc028_quic_fake.cid;
}

void Rfc032QuicConnSetAlpUserData(xqc_connection_t *conn, void *user_data) {
  (void)user_data;
  Rfc032Check(conn == g_rfc028_quic_fake.conn, "set alp user data");
}

xqc_int_t Rfc032QuicConnClose(xqc_engine_t *engine, const xqc_cid_t *cid) {
  (void)cid;
  Rfc032Check(engine == g_rfc028_quic_fake.engine, "conn close engine");
  return XQC_OK;
}

int Rfc032QuicUdpRegisterConn(odin_xqc_udp_t *xu, const xqc_cid_t *cid) {
  (void)xu;
  Rfc032Check(cid != nullptr, "udp register cid");
  return 0;
}

void Rfc032QuicUdpUnregisterConn(odin_xqc_udp_t *xu, const xqc_cid_t *cid) {
  (void)xu;
  (void)cid;
}

void InstallRfc032QuicOps(const Rfc032FakeExpect &expect) {
  std::memset(&g_rfc028_quic_fake, 0, sizeof(g_rfc028_quic_fake));
  std::memset(g_rfc032_fake_failure, 0, sizeof(g_rfc032_fake_failure));
  g_rfc032_fake_failures = 0;
  g_rfc032_connect_calls = 0;
  g_rfc032_expect = expect;
  g_rfc028_quic_fake.engine =
      reinterpret_cast<xqc_engine_t *>(&g_rfc028_quic_fake.engine_storage);
  g_rfc028_quic_fake.conn =
      reinterpret_cast<xqc_connection_t *>(&g_rfc028_quic_fake.conn_storage);
  g_rfc028_quic_fake.cid.cid_len = 1;
  g_rfc028_quic_fake.cid.cid_buf[0] = 0x32;
  odin_xqc_client_runtime_test_reset();
  static const odin_xqc_udp_test_ops_t kUdpOps = {
      Rfc032QuicEngineCreate,
      Rfc032QuicEngineDestroy,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
  };
  odin_xqc_udp_test_set_ops(&kUdpOps);
  static const odin_xqc_client_runtime_test_ops_t kClientOps = {
      Rfc032QuicEngineRegisterAlpn,
      Rfc032QuicEngineUnregisterAlpn,
      Rfc032QuicConnect,
      Rfc032QuicConnSetAlpUserData,
      Rfc032QuicConnClose,
      nullptr,
      nullptr,
      nullptr,
      Rfc032QuicUdpRegisterConn,
      Rfc032QuicUdpUnregisterConn,
  };
  odin_xqc_client_runtime_test_set_ops(&kClientOps);
}

void FinishRfc032FakeExpectations() {
  if (g_rfc032_connect_calls != g_rfc032_expect.connect_calls) {
    Rfc032RecordFakeFailure("connect call count");
  }
}

int Rfc032ConfigureRun(const Rfc032Setup &setup) {
  odin_cli_client_test_reset_liveness();
  odin_event_loop_test_reset_liveness();
  if (odin_dns_resolver_test_reset_liveness() != 0) {
    return 120;
  }
  std::memset(g_rfc032_fake_failure, 0, sizeof(g_rfc032_fake_failure));
  g_rfc032_fake_failures = 0;
  g_rfc032_connect_calls = 0;
  std::memset(&g_rfc032_expect, 0, sizeof(g_rfc032_expect));
  if (setup.install_fake) {
    InstallRfc032QuicOps(setup.fake);
  } else {
    ClearRfc028QuicOps();
    odin_xqc_client_runtime_test_reset();
  }
  if (setup.invalid_addr_push_first) {
    errno = 0;
    if (odin_dns_resolver_test_push_addr_result(nullptr, 1) != -1 ||
        errno != EINVAL) {
      Rfc032RecordFakeFailure("invalid addr push");
    }
  }
  for (const auto &step : setup.steps) {
    if (odin_dns_resolver_test_push_cares_step(&step) != 0) {
      Rfc032RecordFakeFailure("push c-ares step");
    }
  }
  for (const auto &result : setup.addr_results) {
    const odin_dns_addr_t *data = result.empty() ? nullptr : result.data();
    if (odin_dns_resolver_test_push_addr_result(data, result.size()) != 0) {
      Rfc032RecordFakeFailure("push addr result");
    }
  }
  if (setup.failpoint != ODIN_CLI_CLIENT_TEST_FAIL_NONE) {
    const int rc =
        setup.trigger
            ? odin_cli_client_test_trigger_next(setup.failpoint)
            : odin_cli_client_test_fail_next(setup.failpoint, setup.fail_errno);
    if (rc != 0) {
      Rfc032RecordFakeFailure("arm failpoint");
    }
  }
  return 0;
}

Rfc032Setup Rfc032BaseSetup(const Rfc032FakeExpect &expect) {
  Rfc032Setup setup{};
  setup.install_fake = true;
  setup.fake = expect;
  setup.failpoint = ODIN_CLI_CLIENT_TEST_FAIL_NONE;
  return setup;
}

Rfc032Setup Rfc032HostnameSuccessSetup(const Rfc032FakeExpect &expect) {
  Rfc032Setup setup = Rfc032BaseSetup(expect);
  setup.steps.push_back(
      Rfc032Step(ODIN_DNS_TEST_CARES_RESULT_STATUS, ARES_SUCCESS));
  return setup;
}

Rfc028QuicDirectRun
RunRfc032DirectConfig(const odin_cli_client_config_t &config,
                      const Rfc032Setup &setup) {
  EXPECT_EQ(Rfc032ConfigureRun(setup), 0);
  char err_buf[512] = {0};
  FILE *err = fmemopen(err_buf, sizeof(err_buf), "w");
  EXPECT_NE(err, nullptr);
  const int rc = odin_cli_run_client(&config, err);
  (void)fclose(err);
  FinishRfc032FakeExpectations();
  Rfc028QuicDirectRun out;
  out.rc = rc;
  out.err = err_buf;
  FillRfc028QuicSnapshot(rc, &out.snapshot);
  ClearRfc028QuicOps();
  return out;
}

Rfc028QuicChild
SpawnRfc032ChildImpl(const std::vector<std::string> *tokens,
                     const odin_cli_client_config_t *direct_config,
                     const Rfc032Setup &setup) {
  int err_pipe[2];
  int snap_pipe[2];
  int progress_pipe[2] = {-1, -1};
  EXPECT_EQ(pipe(err_pipe), 0);
  EXPECT_EQ(pipe(snap_pipe), 0);
  if (setup.progress_min_quic_adds > 0) {
    EXPECT_EQ(pipe(progress_pipe), 0);
  }
  const pid_t pid = fork();
  EXPECT_NE(pid, -1);
  if (pid == 0) {
    close(err_pipe[0]);
    close(snap_pipe[0]);
    if (setup.progress_min_quic_adds > 0) {
      close(progress_pipe[0]);
    }
    dup2(err_pipe[1], STDERR_FILENO);
    close(err_pipe[1]);
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
    (void)sigaction(SIGINT, &sa_int, nullptr);
    (void)sigaction(SIGTERM, &sa_term, nullptr);
    const int setup_rc = Rfc032ConfigureRun(setup);
    if (setup_rc != 0) {
      _exit(setup_rc);
    }
    if (setup.progress_min_quic_adds > 0 &&
        odin_cli_client_test_set_progress_fd(
            progress_pipe[1], setup.progress_min_quic_adds) != 0) {
      _exit(121);
    }
    int rc = 1;
    if (tokens != nullptr) {
      MutableArgv argv(*tokens);
      rc = odin_cli_main(argv.argc(), argv.argv(), stdout, stderr);
    } else {
      rc = odin_cli_run_client(direct_config, stderr);
    }
    FinishRfc032FakeExpectations();
    Rfc028QuicChildSnapshot snap;
    FillRfc028QuicSnapshot(rc, &snap);
    (void)raise(SIGINT);
    (void)raise(SIGTERM);
    snap.sigint_count = static_cast<int>(g_child_sigint_count);
    snap.sigterm_count = static_cast<int>(g_child_sigterm_count);
    (void)write(snap_pipe[1], &snap, sizeof(snap));
    _exit(snap.fake_failures == 0 ? rc : kRfc032FakeFailureExit);
  }
  close(err_pipe[1]);
  close(snap_pipe[1]);
  if (setup.progress_min_quic_adds > 0) {
    close(progress_pipe[1]);
  }
  return {pid, err_pipe[0], snap_pipe[0], progress_pipe[0], -1};
}

Rfc028QuicChild SpawnRfc032Child(const std::vector<std::string> &tokens,
                                 const Rfc032Setup &setup) {
  return SpawnRfc032ChildImpl(&tokens, nullptr, setup);
}

Rfc028QuicChild
SpawnRfc032DirectConfigChild(const odin_cli_client_config_t &config,
                             const Rfc032Setup &setup) {
  return SpawnRfc032ChildImpl(nullptr, &config, setup);
}

bool TryFinishRfc032Child(Rfc028QuicChild *child, int signal_to_send,
                          Rfc028QuicChildSnapshot *snap) {
  if (signal_to_send != 0) {
    EXPECT_EQ(kill(child->pid, signal_to_send), 0);
  }
  std::memset(snap, 0, sizeof(*snap));
  const ssize_t n = ReadWithDeadline(child->snapshot_fd, snap, sizeof(*snap),
                                     kLongDeadlineMs);
  int st = 0;
  EXPECT_EQ(WaitChildBounded(child->pid, kLongDeadlineMs, &st), 0);
  snap->process_exited = WIFEXITED(st) ? 1 : 0;
  snap->process_status =
      WIFEXITED(st) ? WEXITSTATUS(st) : (WIFSIGNALED(st) ? -WTERMSIG(st) : -1);
  close(child->snapshot_fd);
  child->pid = -1;
  return n == static_cast<ssize_t>(sizeof(*snap));
}

Rfc028QuicChildSnapshot FinishRfc032Child(Rfc028QuicChild *child,
                                          int signal_to_send) {
  Rfc028QuicChildSnapshot snap;
  EXPECT_TRUE(TryFinishRfc032Child(child, signal_to_send, &snap));
  EXPECT_EQ(snap.process_exited, 1);
  EXPECT_TRUE(snap.process_status == snap.rc ||
              snap.process_status == kRfc032FakeFailureExit);
  return snap;
}

void ExpectNoRfc032FakeFailures(const Rfc028QuicChildSnapshot &snap) {
  EXPECT_EQ(snap.fake_failures, 0) << snap.fake_failure;
}

void ExpectRfc032DnsClean(const Rfc028QuicChildSnapshot &snap) {
  EXPECT_EQ(snap.dns.resolvers, 0u);
  EXPECT_EQ(snap.dns.queries, 0u);
  EXPECT_EQ(snap.dns.watches, 0u);
  EXPECT_EQ(snap.dns.timers, 0u);
  EXPECT_EQ(snap.dns.cares_channels, 0u);
  EXPECT_EQ(snap.dns.cares_results, 0u);
}

void ExpectRfc032Clean(const Rfc028QuicChildSnapshot &snap) {
  ExpectRfc028QuicClean(snap);
  ExpectRfc032DnsClean(snap);
}

void ExpectRfc032PreDnsRejected(const Rfc028QuicChildSnapshot &snap) {
  ExpectRfc032Clean(snap);
  EXPECT_EQ(snap.dns_obs.getaddrinfo_calls, 0u);
  EXPECT_EQ(snap.cli.quic_runtime_create_calls, 0u);
  EXPECT_EQ(snap.cli.quic_runtime_start_calls, 0u);
  EXPECT_EQ(snap.runtime_config_ok, 0);
}

void ExpectRfc032RuntimeEndpoint(const Rfc028QuicChildSnapshot &snap,
                                 int peer_family, const char *peer_ip,
                                 uint16_t peer_port, int local_family,
                                 const char *local_ip, uint16_t local_port,
                                 const char *server_host) {
  ASSERT_TRUE(snap.runtime_config_ok);
  EXPECT_TRUE(Rfc032SockaddrMatches(reinterpret_cast<const struct sockaddr *>(
                                        &snap.runtime_config.peer_addr_value),
                                    snap.runtime_config.peer_addrlen,
                                    peer_family, peer_ip, peer_port));
  EXPECT_TRUE(Rfc032SockaddrMatches(reinterpret_cast<const struct sockaddr *>(
                                        &snap.runtime_config.local_addr_value),
                                    snap.runtime_config.local_addrlen,
                                    local_family, local_ip, local_port));
  EXPECT_STREQ(snap.runtime_config.server_host_value, server_host);
}

void ExpectRfc032FailureRun(const Rfc028QuicChildSnapshot &snap,
                            const std::string &stderr_text, const char *step) {
  EXPECT_EQ(snap.rc, 1);
  EXPECT_EQ(stderr_text,
            std::string("odin: client startup failed at ") + step + "\n");
  EXPECT_EQ(stderr_text.find("mode=client"), std::string::npos);
  EXPECT_EQ(snap.cli.quic_runtime_create_calls, 0u);
  EXPECT_EQ(snap.cli.quic_runtime_start_calls, 0u);
  ExpectNoRfc032FakeFailures(snap);
  ExpectRfc032Clean(snap);
}

void ExpectRfc032DnsTimingCallbackDestroyed(
    const Rfc028QuicChildSnapshot &snap) {
  EXPECT_EQ(snap.dns_timing.dns_on_done_calls, 1u);
  EXPECT_EQ(snap.dns_timing.query_destroyed_in_callback_calls, 1u);
  EXPECT_EQ(snap.dns_timing.live_queries_at_callback_exit, 0u);
}

int RunRfc032LibraryInitExecChild() {
  const pid_t pid = fork();
  EXPECT_NE(pid, -1);
  if (pid == 0) {
    setenv("ODIN_RFC032_EXEC_CHILD", "T7_LIBRARY_INIT", 1);
    execl(g_test_argv0.c_str(), g_test_argv0.c_str(),
          "--gtest_filter=OdinRFC032ClientDnsExecChild.T7LibraryInit", nullptr);
    _exit(127);
  }
  int st = 0;
  EXPECT_EQ(WaitChildBounded(pid, kLongDeadlineMs, &st), 0);
  if (!WIFEXITED(st)) {
    return -1;
  }
  return WEXITSTATUS(st);
}

bool Rfc032IsDirectChildOnlyFilter(const std::string &filter) {
  return filter == "OdinRFC032ClientDnsExecChild.T7LibraryInit" ||
         filter == "OdinRFC032ClientDnsExecChild.*";
}

} // namespace

class OdinRFC032ClientDnsTest : public ::testing::Test {};

TEST_F(OdinRFC032ClientDnsTest, T1HostnameResolvesToIpv4BeforeQuicStartup) {
  Rfc032Setup setup = Rfc032HostnameSuccessSetup(Rfc032Expect(
      "odin.test", AF_INET, "127.0.0.1", 8443, AF_INET, "0.0.0.0", 0, 1));
  Rfc028QuicChild child =
      SpawnRfc032Child(QuicClientArgs("odin.test:8443"), setup);
  ChildGuard guard(child.pid);
  const std::string line = ReadLineWithDeadline(child.stderr_fd, 2000);
  uint16_t proxy_port = 0;
  std::string server;
  ASSERT_TRUE(ParseQuicStartupLine(line, &proxy_port, &server)) << line;
  EXPECT_EQ(server, "odin.test:8443");
  Rfc028QuicChildSnapshot snap = FinishRfc032Child(&child, SIGTERM);
  guard.disarm();
  EXPECT_EQ(ReadAllAvailable(child.stderr_fd, 100), "");
  close(child.stderr_fd);
  EXPECT_EQ(snap.rc, 0);
  ExpectNoRfc032FakeFailures(snap);
  EXPECT_EQ(snap.dns_obs.getaddrinfo_calls, 1u);
  EXPECT_EQ(snap.dns_obs.last_ai_family, AF_UNSPEC);
  ExpectRfc032RuntimeEndpoint(snap, AF_INET, "127.0.0.1", 8443, AF_INET,
                              "0.0.0.0", 0, "odin.test");
  EXPECT_EQ(snap.cli.quic_runtime_create_calls, 1u);
  EXPECT_EQ(snap.cli.quic_runtime_start_calls, 1u);
  ExpectRfc032DnsTimingCallbackDestroyed(snap);
  ExpectRfc032Clean(snap);
}

TEST_F(OdinRFC032ClientDnsTest, T2Ipv4LiteralStillUsesAsyncResolverPath) {
  Rfc032Setup setup = Rfc032BaseSetup(Rfc032Expect(
      "127.0.0.1", AF_INET, "127.0.0.1", 4433, AF_INET, "0.0.0.0", 0, 1));
  Rfc028QuicChild child =
      SpawnRfc032Child(QuicClientArgs("127.0.0.1:4433"), setup);
  ChildGuard guard(child.pid);
  const std::string line = ReadLineWithDeadline(child.stderr_fd, 2000);
  uint16_t proxy_port = 0;
  std::string server;
  ASSERT_TRUE(ParseQuicStartupLine(line, &proxy_port, &server)) << line;
  EXPECT_EQ(server, "127.0.0.1:4433");
  Rfc028QuicChildSnapshot snap = FinishRfc032Child(&child, SIGTERM);
  guard.disarm();
  EXPECT_EQ(ReadAllAvailable(child.stderr_fd, 100), "");
  close(child.stderr_fd);
  EXPECT_EQ(snap.rc, 0);
  ExpectNoRfc032FakeFailures(snap);
  EXPECT_EQ(snap.dns_obs.getaddrinfo_calls, 1u);
  ExpectRfc032RuntimeEndpoint(snap, AF_INET, "127.0.0.1", 4433, AF_INET,
                              "0.0.0.0", 0, "127.0.0.1");
  ExpectRfc032Clean(snap);
}

TEST_F(OdinRFC032ClientDnsTest, T3Ipv6LiteralResolvesAndUsesIpv6UdpBind) {
  Rfc032Setup setup = Rfc032BaseSetup(
      Rfc032Expect("::1", AF_INET6, "::1", 9443, AF_INET6, "::", 0, 1));
  Rfc028QuicChild child = SpawnRfc032Child(QuicClientArgs("[::1]:9443"), setup);
  ChildGuard guard(child.pid);
  const std::string line = ReadLineWithDeadline(child.stderr_fd, 2000);
  uint16_t proxy_port = 0;
  std::string server;
  ASSERT_TRUE(ParseQuicStartupLine(line, &proxy_port, &server)) << line;
  EXPECT_EQ(server, "[::1]:9443");
  Rfc028QuicChildSnapshot snap = FinishRfc032Child(&child, SIGTERM);
  guard.disarm();
  EXPECT_EQ(ReadAllAvailable(child.stderr_fd, 100), "");
  close(child.stderr_fd);
  EXPECT_EQ(snap.rc, 0);
  ExpectNoRfc032FakeFailures(snap);
  EXPECT_EQ(snap.dns_obs.getaddrinfo_calls, 1u);
  ExpectRfc032RuntimeEndpoint(snap, AF_INET6, "::1", 9443, AF_INET6, "::", 0,
                              "::1");
  ExpectRfc032Clean(snap);
}

TEST_F(OdinRFC032ClientDnsTest, T4DnsResultErrorsFailBeforeBanner) {
  struct Case {
    const char *server;
    int status;
  };
  const Case cases[] = {
      {"missing.test:4433", ARES_ENOTFOUND},
      {"slow.test:4433", ARES_ETIMEOUT},
  };
  for (const Case &c : cases) {
    SCOPED_TRACE(c.server);
    Rfc032Setup setup = Rfc032BaseSetup(
        Rfc032Expect("", AF_INET, "127.0.0.1", 4433, AF_INET, "0.0.0.0", 0, 0));
    setup.steps.push_back(
        Rfc032Step(ODIN_DNS_TEST_CARES_RESULT_STATUS, c.status));
    Rfc028QuicChild child = SpawnRfc032Child(QuicClientArgs(c.server), setup);
    ChildGuard guard(child.pid);
    Rfc028QuicChildSnapshot snap = FinishRfc032Child(&child, 0);
    guard.disarm();
    const std::string err = ReadAllAvailable(child.stderr_fd, 100);
    close(child.stderr_fd);
    ExpectRfc032FailureRun(snap, err, "server_dns");
    ExpectRfc032DnsTimingCallbackDestroyed(snap);
  }
}

TEST_F(OdinRFC032ClientDnsTest, T5DnsOkWithNoUsableAddressesFails) {
  {
    Rfc032Setup setup = Rfc032BaseSetup(
        Rfc032Expect("", AF_INET, "127.0.0.1", 4433, AF_INET, "0.0.0.0", 0, 0));
    setup.steps.push_back(
        Rfc032Step(ODIN_DNS_TEST_CARES_RESULT_EMPTY_SUCCESS, ARES_SUCCESS));
    Rfc028QuicChild child =
        SpawnRfc032Child(QuicClientArgs("empty.test:4433"), setup);
    ChildGuard guard(child.pid);
    Rfc028QuicChildSnapshot snap = FinishRfc032Child(&child, 0);
    guard.disarm();
    const std::string err = ReadAllAvailable(child.stderr_fd, 100);
    close(child.stderr_fd);
    ExpectRfc032FailureRun(snap, err, "server_dns");
    ExpectRfc032DnsTimingCallbackDestroyed(snap);
  }
  {
    Rfc032Setup setup = Rfc032BaseSetup(
        Rfc032Expect("", AF_INET, "127.0.0.1", 4433, AF_INET, "0.0.0.0", 0, 0));
    setup.invalid_addr_push_first = true;
    setup.addr_results.push_back(
        {Rfc032AddrUnix(),
         Rfc032Addr4("127.0.0.1", 1111,
                     static_cast<socklen_t>(sizeof(struct sockaddr_in) - 1)),
         Rfc032Addr6("::1", 2222,
                     static_cast<socklen_t>(sizeof(struct sockaddr_in6) - 1))});
    Rfc028QuicChild child =
        SpawnRfc032Child(QuicClientArgs("unsupported.test:4433"), setup);
    ChildGuard guard(child.pid);
    Rfc028QuicChildSnapshot snap = FinishRfc032Child(&child, 0);
    guard.disarm();
    const std::string err = ReadAllAvailable(child.stderr_fd, 100);
    close(child.stderr_fd);
    ExpectRfc032FailureRun(snap, err, "server_dns");
    EXPECT_EQ(snap.dns_obs.consumed_addr_results, 1u);
    EXPECT_EQ(snap.dns_obs.last_consumed_addr_count, 3u);
    ExpectRfc032DnsTimingCallbackDestroyed(snap);
  }
}

TEST_F(OdinRFC032ClientDnsTest, T6InvalidHostSliceRejectedBeforeResources) {
  const std::string ca = ClientCaFile();
  Rfc032Setup setup = Rfc032BaseSetup(
      Rfc032Expect("", AF_INET, "127.0.0.1", 4433, AF_INET, "0.0.0.0", 0, 0));
  odin_cli_client_config_t config{};
  config.listen_port = 0;
  config.server_host = "";
  config.server_host_len = 0;
  config.server_port = 4433;
  config.quic_ca_file = ca.c_str();
  Rfc028QuicDirectRun zero = RunRfc032DirectConfig(config, setup);
  EXPECT_EQ(zero.rc, 1);
  EXPECT_EQ(zero.err, "odin: client startup failed at server_endpoint\n");
  ExpectRfc032PreDnsRejected(zero.snapshot);

  std::string too_long(ODIN_PROTO_HOST_MAX + 1u, 'a');
  config.server_host = too_long.c_str();
  config.server_host_len = too_long.size();
  Rfc028QuicDirectRun long_run = RunRfc032DirectConfig(config, setup);
  EXPECT_EQ(long_run.rc, 1);
  EXPECT_EQ(long_run.err, "odin: client startup failed at server_endpoint\n");
  ExpectRfc032PreDnsRejected(long_run.snapshot);

  config.server_host = nullptr;
  config.server_host_len = 4;
  Rfc028QuicChild null_child = SpawnRfc032DirectConfigChild(config, setup);
  ChildGuard null_guard(null_child.pid);
  Rfc028QuicChildSnapshot null_snap;
  ASSERT_TRUE(TryFinishRfc032Child(&null_child, 0, &null_snap));
  null_guard.disarm();
  const std::string null_err = ReadAllAvailable(null_child.stderr_fd, 100);
  close(null_child.stderr_fd);
  EXPECT_EQ(null_snap.rc, 1);
  EXPECT_EQ(null_err, "odin: client startup failed at server_endpoint\n");
  ExpectRfc032PreDnsRejected(null_snap);

  const char embedded[] = {'g', 'o', 'o', 'd', '\0', 'b', 'a', 'd'};
  config.server_host = embedded;
  config.server_host_len = sizeof(embedded);
  Rfc028QuicDirectRun embedded_run = RunRfc032DirectConfig(config, setup);
  EXPECT_EQ(embedded_run.rc, 1);
  EXPECT_EQ(embedded_run.err,
            "odin: client startup failed at server_endpoint\n");
  ExpectRfc032PreDnsRejected(embedded_run.snapshot);

  const char valid_prefix[] = {'1', '2', '7',  '.', '0', '.', '0',
                               '.', '1', '\0', 'b', 'a', 'd'};
  config.server_host = valid_prefix;
  config.server_host_len = sizeof(valid_prefix);
  Rfc032Setup fail_setup = setup;
  fail_setup.failpoint = ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_CREATE;
  fail_setup.fail_errno = EIO;
  Rfc028QuicDirectRun prefix_run = RunRfc032DirectConfig(config, fail_setup);
  EXPECT_EQ(prefix_run.rc, 1);
  EXPECT_EQ(prefix_run.err, "odin: client startup failed at server_endpoint\n");
  odin_cli_client_test_failpoint_t pending = ODIN_CLI_CLIENT_TEST_FAIL_NONE;
  ASSERT_EQ(odin_cli_client_test_pending_failpoint(&pending), 0);
  EXPECT_EQ(pending, ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_CREATE);
  ExpectRfc032PreDnsRejected(prefix_run.snapshot);
  odin_cli_client_test_reset_liveness();
}

TEST_F(OdinRFC032ClientDnsTest, T7ResolverCreateAndStartFailuresCleanUp) {
  EXPECT_EQ(RunRfc032LibraryInitExecChild(), 0);

  Rfc032Setup setup = Rfc032BaseSetup(
      Rfc032Expect("", AF_INET, "127.0.0.1", 4433, AF_INET, "0.0.0.0", 0, 0));
  setup.steps.push_back(
      Rfc032Step(ODIN_DNS_TEST_CARES_INIT_OPTIONS, ARES_EFORMERR));
  Rfc028QuicChild child =
      SpawnRfc032Child(QuicClientArgs("initopt.test:4433"), setup);
  ChildGuard guard(child.pid);
  Rfc028QuicChildSnapshot snap = FinishRfc032Child(&child, 0);
  guard.disarm();
  const std::string err = ReadAllAvailable(child.stderr_fd, 100);
  close(child.stderr_fd);
  ExpectRfc032FailureRun(snap, err, "server_dns");
}

TEST_F(OdinRFC032ClientDnsTest,
       T8DnsPhaseEventLoopFailureCleansUpWithoutAccepting) {
  uint16_t known_port = 0;
  const int holder = OpenIpv4Listener("127.0.0.1", 0, true, &known_port);
  ASSERT_GE(holder, 0) << std::strerror(errno);
  close(holder);
  Rfc032Setup setup = Rfc032BaseSetup(
      Rfc032Expect("", AF_INET, "127.0.0.1", 4433, AF_INET, "0.0.0.0", 0, 0));
  setup.steps.push_back(
      Rfc032Step(ODIN_DNS_TEST_CARES_RESULT_PENDING, ARES_SUCCESS));
  setup.failpoint = ODIN_CLI_CLIENT_TEST_FAIL_DNS_EVENT_LOOP_RUN;
  setup.fail_errno = EIO;
  std::vector<std::string> args = {
      "odin-client", "--listen",          std::to_string(known_port),
      "--server",    "pending.test:4433", "--ca-file",
      ClientCaFile()};
  Rfc028QuicChild child = SpawnRfc032Child(args, setup);
  ChildGuard guard(child.pid);
  const int probe = TcpConnectLoopback(known_port, 100);
  if (probe >= 0) {
    close(probe);
  }
  Rfc028QuicChildSnapshot snap = FinishRfc032Child(&child, 0);
  guard.disarm();
  const std::string err = ReadAllAvailable(child.stderr_fd, 100);
  close(child.stderr_fd);
  ExpectRfc032FailureRun(snap, err, "server_dns");
  EXPECT_EQ(snap.dns_timing.dns_query_pending_before_dns_event_loop_run, 1);
  EXPECT_EQ(snap.dns_timing.accept_loop_create_calls_before_dns_event_loop_run,
            0u);
  EXPECT_EQ(snap.dns_timing.live_accept_loops_before_dns_event_loop_run, 0u);
  EXPECT_EQ(snap.dns_timing
                .quic_runtime_add_connection_calls_before_dns_event_loop_run,
            0u);
  EXPECT_EQ(snap.dns_timing.dns_event_loop_run_rc, -1);
  EXPECT_EQ(snap.dns_timing.dns_done_after_dns_event_loop_run, 0);
  EXPECT_GT(snap.dns_timing.live_resolvers_after_dns_event_loop_run, 0u);
  EXPECT_GT(snap.dns_timing.live_queries_after_dns_event_loop_run, 0u);
}

TEST_F(OdinRFC032ClientDnsTest, T9PostDnsRuntimeStartupFailuresKeepDnsCleaned) {
  struct Case {
    const char *step;
    odin_cli_client_test_failpoint_t fp;
  };
  const Case cases[] = {
      {"xqc_client_runtime_create",
       ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_CREATE},
      {"xqc_client_runtime_start",
       ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_START},
  };
  for (const Case &c : cases) {
    SCOPED_TRACE(c.step);
    Rfc032Setup setup = Rfc032HostnameSuccessSetup(Rfc032Expect(
        "odin.test", AF_INET, "127.0.0.1", 4433, AF_INET, "0.0.0.0", 0, 0));
    setup.failpoint = c.fp;
    setup.fail_errno = EIO;
    Rfc028QuicChild child =
        SpawnRfc032Child(QuicClientArgs("odin.test:4433"), setup);
    ChildGuard guard(child.pid);
    Rfc028QuicChildSnapshot snap = FinishRfc032Child(&child, 0);
    guard.disarm();
    const std::string err = ReadAllAvailable(child.stderr_fd, 100);
    close(child.stderr_fd);
    EXPECT_EQ(snap.rc, 1);
    EXPECT_EQ(err,
              std::string("odin: client startup failed at ") + c.step + "\n");
    EXPECT_EQ(err.find("mode=client"), std::string::npos);
    ExpectNoRfc032FakeFailures(snap);
    EXPECT_EQ(snap.dns_timing.live_resolvers_before_accept_loop_create, 0u);
    EXPECT_EQ(snap.dns_timing.live_queries_before_accept_loop_create, 0u);
    EXPECT_EQ(snap.dns_timing.live_resolvers_before_runtime_create, 0u);
    EXPECT_EQ(snap.dns_timing.live_queries_before_runtime_create, 0u);
    if (c.fp == ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_START) {
      EXPECT_EQ(snap.dns_timing.live_resolvers_before_runtime_start, 0u);
      EXPECT_EQ(snap.dns_timing.live_queries_before_runtime_start, 0u);
    }
    ExpectRfc032Clean(snap);
  }
}

TEST_F(OdinRFC032ClientDnsTest,
       T10AcceptedLocalConnectionAfterDnsSuccessReachesRuntime) {
  (void)signal(SIGPIPE, SIG_IGN);
  uint16_t sentry_port = 0;
  const int sentry = OpenIpv4Listener("127.0.0.1", 0, true, &sentry_port);
  ASSERT_GE(sentry, 0) << std::strerror(errno);
  Rfc032Setup setup = Rfc032HostnameSuccessSetup(Rfc032Expect(
      "odin.test", AF_INET, "127.0.0.1", 4433, AF_INET, "0.0.0.0", 0, 1));
  setup.progress_min_quic_adds = 1;
  Rfc028QuicChild child =
      SpawnRfc032Child(QuicClientArgs("odin.test:4433"), setup);
  ChildGuard guard(child.pid);
  const std::string line = ReadLineWithDeadline(child.stderr_fd, 2000);
  uint16_t proxy_port = 0;
  std::string server;
  ASSERT_TRUE(ParseQuicStartupLine(line, &proxy_port, &server)) << line;
  EXPECT_EQ(server, "odin.test:4433");
  const int client = TcpConnectLoopback(proxy_port, kShortDeadlineMs);
  ASSERT_GE(client, 0) << std::strerror(errno);
  const std::string request =
      "CONNECT 127.0.0.1:" + std::to_string(sentry_port) + " HTTP/1.1\r\n\r\n";
  ASSERT_TRUE(WriteAllDeadline(client, request.data(), request.size(),
                               kShortDeadlineMs));
  char progress = 0;
  ASSERT_EQ(ReadWithDeadline(child.progress_fd, &progress, 1, kLongDeadlineMs),
            1);
  ExpectNoAccept(sentry, 200);
  Rfc028QuicChildSnapshot snap = FinishRfc032Child(&child, SIGTERM);
  guard.disarm();
  EXPECT_EQ(ReadAllAvailable(child.stderr_fd, 100), "");
  close(child.stderr_fd);
  close(child.progress_fd);
  close(client);
  close(sentry);
  EXPECT_EQ(snap.rc, 0);
  ExpectNoRfc032FakeFailures(snap);
  EXPECT_EQ(snap.cli.quic_runtime_add_connection_calls, 1u);
  ASSERT_TRUE(snap.add_record_ok);
  EXPECT_EQ(snap.add_record.fd_is_nonblocking, 1);
  ExpectRfc032Clean(snap);
}

TEST_F(OdinRFC032ClientDnsTest, T11MultipleDnsResultsSelectFirstUsableAddress) {
  Rfc032Setup setup = Rfc032BaseSetup(Rfc032Expect(
      "ordered.test", AF_INET6, "2001:db8::32", 9443, AF_INET6, "::", 0, 1));
  setup.addr_results.push_back(
      {Rfc032AddrUnix(),
       Rfc032Addr6("2001:db8::32", 1111,
                   static_cast<socklen_t>(sizeof(struct sockaddr_in6))),
       Rfc032Addr4("127.0.0.77", 2222,
                   static_cast<socklen_t>(sizeof(struct sockaddr_in)))});
  Rfc028QuicChild child =
      SpawnRfc032Child(QuicClientArgs("ordered.test:9443"), setup);
  ChildGuard guard(child.pid);
  const std::string line = ReadLineWithDeadline(child.stderr_fd, 2000);
  uint16_t proxy_port = 0;
  std::string server;
  ASSERT_TRUE(ParseQuicStartupLine(line, &proxy_port, &server)) << line;
  EXPECT_EQ(server, "ordered.test:9443");
  Rfc028QuicChildSnapshot snap = FinishRfc032Child(&child, SIGTERM);
  guard.disarm();
  EXPECT_EQ(ReadAllAvailable(child.stderr_fd, 100), "");
  close(child.stderr_fd);
  EXPECT_EQ(snap.rc, 0);
  ExpectNoRfc032FakeFailures(snap);
  EXPECT_EQ(snap.dns_obs.getaddrinfo_calls, 1u);
  EXPECT_EQ(snap.dns_obs.last_ai_family, AF_UNSPEC);
  ExpectRfc032RuntimeEndpoint(snap, AF_INET6, "2001:db8::32", 9443, AF_INET6,
                              "::", 0, "ordered.test");
  ExpectRfc032DnsTimingCallbackDestroyed(snap);
  ExpectRfc032Clean(snap);
}

TEST_F(OdinRFC032ClientDnsTest, T12DnsLoopStopsWithoutDnsCompletion) {
  Rfc032Setup setup = Rfc032BaseSetup(
      Rfc032Expect("", AF_INET, "127.0.0.1", 4433, AF_INET, "0.0.0.0", 0, 0));
  setup.steps.push_back(
      Rfc032Step(ODIN_DNS_TEST_CARES_RESULT_PENDING, ARES_SUCCESS));
  setup.failpoint = ODIN_CLI_CLIENT_TEST_TRIGGER_DNS_EVENT_LOOP_STOP;
  setup.trigger = true;
  Rfc028QuicChild child =
      SpawnRfc032Child(QuicClientArgs("pending-stop.test:4433"), setup);
  ChildGuard guard(child.pid);
  Rfc028QuicChildSnapshot snap = FinishRfc032Child(&child, 0);
  guard.disarm();
  const std::string err = ReadAllAvailable(child.stderr_fd, 100);
  close(child.stderr_fd);
  ExpectRfc032FailureRun(snap, err, "server_dns");
  EXPECT_EQ(snap.dns_timing.dns_query_pending_before_dns_event_loop_run, 1);
  EXPECT_EQ(snap.dns_timing.accept_loop_create_calls_before_dns_event_loop_run,
            0u);
  EXPECT_EQ(snap.dns_timing
                .quic_runtime_add_connection_calls_before_dns_event_loop_run,
            0u);
  EXPECT_EQ(snap.dns_timing.dns_event_loop_run_rc, 0);
  EXPECT_EQ(snap.dns_timing.dns_done_after_dns_event_loop_run, 0);
  EXPECT_EQ(snap.dns_timing.dns_success_after_dns_event_loop_run, 0);
  EXPECT_GT(snap.dns_timing.live_resolvers_after_dns_event_loop_run, 0u);
  EXPECT_GT(snap.dns_timing.live_queries_after_dns_event_loop_run, 0u);
}

TEST(OdinRFC032ClientDnsExecChild, T7LibraryInit) {
  odin_cli_client_test_reset_liveness();
  odin_event_loop_test_reset_liveness();
  odin_xqc_client_runtime_test_reset();
  ASSERT_EQ(odin_dns_resolver_test_reset_liveness(), 0) << std::strerror(errno);
  const std::string filter = ActiveFilter();
  const char *mode = std::getenv("ODIN_RFC032_EXEC_CHILD");
  if (mode == nullptr || mode[0] == '\0') {
    if (Rfc032IsDirectChildOnlyFilter(filter)) {
      FAIL() << "missing ODIN_RFC032_EXEC_CHILD";
    }
    GTEST_SKIP() << "ordinary full-suite child-only skip";
  }
  ASSERT_EQ(filter, "OdinRFC032ClientDnsExecChild.T7LibraryInit");
  ASSERT_STREQ(mode, "T7_LIBRARY_INIT");
  odin_dns_resolver_test_cares_step_t step =
      Rfc032Step(ODIN_DNS_TEST_CARES_LIBRARY_INIT, ARES_ENOMEM);
  ASSERT_EQ(odin_dns_resolver_test_push_cares_step(&step), 0)
      << std::strerror(errno);
  const std::string ca = ClientCaFile();
  MutableArgv argv({"odin-client", "--listen", "0", "--server",
                    "libinit.test:4433", "--ca-file", ca.c_str()});
  char err_buf[512] = {0};
  FILE *err = fmemopen(err_buf, sizeof(err_buf), "w");
  ASSERT_NE(err, nullptr);
  const int rc = odin_cli_main(argv.argc(), argv.argv(), stdout, err);
  (void)fclose(err);
  Rfc028QuicChildSnapshot snap;
  FillRfc028QuicSnapshot(rc, &snap);
  ExpectRfc032FailureRun(snap, err_buf, "server_dns");
  odin_dns_resolver_test_cares_observation_t obs{};
  ASSERT_EQ(odin_dns_resolver_test_cares_observation(&obs), 0);
  EXPECT_EQ(obs.ares_library_init_calls, 1u);
}

// NOLINTEND(misc-const-correctness, misc-use-internal-linkage)
