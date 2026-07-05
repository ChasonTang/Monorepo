// odin/testing/cli_server_quic_unittests.cpp
//
// RFC-026 T1-T10 for the QUIC server CLI runtime.

#include "odin/cli.h"
#include "odin/cli_server.h"
#include "odin/event_loop.h"
#include "odin/protocol.h"
#include "odin/server_xqc_runtime.h"
#include "odin/testing/cli_server_internal_test.h"
#include "odin/testing/event_loop_internal_test.h"
#include "odin/testing/server_xqc_runtime_internal_test.h"
#include "odin/testing/transport_xqc_internal_test.h"
#include "odin/testing/xqc_udp_internal_test.h"
#include "odin/udp.h"
#include "odin/xqc_udp.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <functional>
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

// NOLINTBEGIN(misc-const-correctness)

extern "C" {
extern char **environ;
}

extern std::string g_test_argv0;

namespace {

volatile sig_atomic_t g_counted_sigint;
volatile sig_atomic_t g_counted_sigterm;
int g_t5_engine_create_port_fd = -1;

constexpr const char kServerUsage[] =
    "usage: odin-server --listen ADDR --quic-cert FILE --quic-key FILE";
constexpr const char kCertPath[] = "thor/out/odin-server.pem";
constexpr const char kKeyPath[] = "thor/out/odin-server-key.pem";

class MutableArgv {
public:
  MutableArgv(std::initializer_list<const char *> tokens) {
    storage_.reserve(tokens.size());
    for (const char *t : tokens) {
      storage_.emplace_back(t);
    }
    rebuild();
  }

  explicit MutableArgv(const std::vector<std::string> &tokens)
      : storage_(tokens) {
    rebuild();
  }

  int argc() const { return static_cast<int>(storage_.size()); }
  char *const *argv() { return ptrs_.data(); }

private:
  void rebuild() {
    ptrs_.clear();
    ptrs_.reserve(storage_.size());
    for (std::string &s : storage_) {
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

int WriteAll(int fd, const void *buf, size_t len) {
  const uint8_t *p = static_cast<const uint8_t *>(buf);
  size_t off = 0;
  while (off < len) {
    const ssize_t n = write(fd, p + off, len - off);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    off += static_cast<size_t>(n);
  }
  return 0;
}

std::string ReadLineWithDeadline(int fd, int deadline_ms) {
  std::string out;
  const auto start = std::chrono::steady_clock::now();
  for (;;) {
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
    char c = 0;
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

std::string DrainFd(int fd) {
  std::string out;
  char buf[256];
  for (;;) {
    const ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0) {
      return out;
    }
    out.append(buf, static_cast<size_t>(n));
  }
}

int WaitChildBounded(pid_t pid, int deadline_ms, int *wstatus_out) {
  const auto start = std::chrono::steady_clock::now();
  int wstatus = 0;
  for (;;) {
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

struct ChildHandle {
  pid_t pid = -1;
  int stdout_fd = -1;
  int stderr_fd = -1;
};

ChildHandle SpawnOdinServer(const std::vector<std::string> &extra_args) {
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
    return {};
  }
  if (pid == 0) {
    close(out_pipe[0]);
    close(err_pipe[0]);
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);
    close(out_pipe[1]);
    close(err_pipe[1]);
    const std::string server_path = Dirname(g_test_argv0) + "/odin-server";
    std::vector<std::string> tokens;
    tokens.push_back(server_path);
    for (const std::string &arg : extra_args) {
      tokens.push_back(arg);
    }
    std::vector<char *> argv;
    argv.reserve(tokens.size() + 1);
    for (std::string &t : tokens) {
      argv.push_back(&t[0]);
    }
    argv.push_back(nullptr);
    execve(server_path.c_str(), argv.data(), environ);
    _exit(127);
  }
  close(out_pipe[1]);
  close(err_pipe[1]);
  return {pid, out_pipe[0], err_pipe[0]};
}

bool ParseQuicStartupLine(const std::string &line, uint16_t *out_port) {
  static const char kPrefix[] = "odin: mode=server transport=quic listen=";
  if (line.rfind(kPrefix, 0) != 0 || line.empty() || line.back() != '\n') {
    return false;
  }
  const std::string n =
      line.substr(std::strlen(kPrefix), line.size() - std::strlen(kPrefix) - 1);
  unsigned long port = 0;
  for (char c : n) {
    if (c < '0' || c > '9') {
      return false;
    }
    port = port * 10 + static_cast<unsigned long>(c - '0');
    if (port > 65535) {
      return false;
    }
  }
  *out_port = static_cast<uint16_t>(port);
  return !n.empty();
}

int OpenUdpAny(uint16_t want_port, uint16_t *out_port) {
  const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    return -1;
  }
  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(want_port);
  if (bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
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

int TryBindUdpAny(uint16_t port) {
  uint16_t ignored = 0;
  return OpenUdpAny(port, &ignored);
}

int OpenLoopbackListener(uint16_t *out_port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  const int reuse = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    close(fd);
    return -1;
  }
  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0 ||
      listen(fd, 16) != 0) {
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
  *out_port = ntohs(addr.sin_port);
  return fd;
}

std::string EncodedReq(const std::string &host, uint16_t port) {
  odin_proto_iov_t iov[3];
  uint8_t hdr[3];
  uint8_t portbe[2];
  EXPECT_EQ(odin_proto_encode_connect_req(host.data(), host.size(), port, iov,
                                          hdr, portbe),
            ODIN_PROTO_OK);
  std::string out;
  for (const auto &part : iov) {
    out.append(static_cast<const char *>(part.base), part.len);
  }
  return out;
}

std::string EncodedResp(uint16_t code) {
  odin_proto_connect_resp_frame_t resp;
  odin_proto_encode_connect_resp(code, &resp);
  return std::string(reinterpret_cast<const char *>(resp.bytes),
                     sizeof(resp.bytes));
}

struct MainResult {
  int rc = -1;
  std::string out;
  std::string err;
};

struct LivenessSnapshot {
  odin_cli_server_test_liveness_t cli{};
  odin_event_loop_test_liveness_t event{};
  int cli_rc = -1;
  int event_rc = -1;
};

LivenessSnapshot SnapshotLiveness() {
  LivenessSnapshot snapshot;
  snapshot.cli_rc = odin_cli_server_test_liveness(&snapshot.cli);
  snapshot.event_rc = odin_event_loop_test_liveness(&snapshot.event);
  return snapshot;
}

void ExpectZeroLiveness(const LivenessSnapshot &snapshot) {
  EXPECT_EQ(snapshot.cli_rc, 0);
  EXPECT_EQ(snapshot.event_rc, 0);
  EXPECT_EQ(snapshot.cli.live_xqc_runtimes, 0u);
  EXPECT_EQ(snapshot.event.loops, 0u);
  EXPECT_EQ(snapshot.event.io_handles, 0u);
  EXPECT_EQ(snapshot.event.timers, 0u);
  EXPECT_EQ(snapshot.event.task_nodes, 0u);
}

void CountSigint(int) { g_counted_sigint += 1; }

void CountSigterm(int) { g_counted_sigterm += 1; }

class ScopedCountingSignalHandlers {
public:
  ScopedCountingSignalHandlers() {
    g_counted_sigint = 0;
    g_counted_sigterm = 0;
    struct sigaction sa_int;
    std::memset(&sa_int, 0, sizeof(sa_int));
    sa_int.sa_handler = CountSigint;
    sigemptyset(&sa_int.sa_mask);
    EXPECT_EQ(sigaction(SIGINT, &sa_int, &old_sigint_), 0);
    struct sigaction sa_term;
    std::memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = CountSigterm;
    sigemptyset(&sa_term.sa_mask);
    EXPECT_EQ(sigaction(SIGTERM, &sa_term, &old_sigterm_), 0);
  }

  ~ScopedCountingSignalHandlers() {
    (void)sigaction(SIGTERM, &old_sigterm_, nullptr);
    (void)sigaction(SIGINT, &old_sigint_, nullptr);
  }

private:
  struct sigaction old_sigint_;
  struct sigaction old_sigterm_;
};

MainResult RunMain(const std::vector<std::string> &tokens) {
  char out_buf[2048] = {0};
  char err_buf[4096] = {0};
  FILE *out = fmemopen(out_buf, sizeof(out_buf), "w");
  FILE *err = fmemopen(err_buf, sizeof(err_buf), "w");
  EXPECT_NE(out, nullptr);
  EXPECT_NE(err, nullptr);
  MutableArgv argv(tokens);
  const int rc = odin_cli_main(argv.argc(), argv.argv(), out, err);
  (void)fclose(out);
  (void)fclose(err);
  return {rc, out_buf, err_buf};
}

struct FakeConn {
  void *transport_user_data = nullptr;
  void *alp_user_data = nullptr;
};

struct RecvStep {
  std::string data;
  ssize_t ret = 0;
  uint8_t fin = 0;
};

struct SendRecord {
  std::string data;
  size_t size = 0;
  uint8_t fin = 0;
  bool null_data = false;
};

struct FakeStream {
  FakeConn *conn = nullptr;
  xqc_stream_direction_t direction = XQC_STREAM_BIDI;
  void *user_data = nullptr;
  std::deque<RecvStep> recv_steps;
  std::vector<SendRecord> sends;
  std::vector<void *> user_data_values;
  int close_calls = 0;
};

xqc_connection_t *AsConn(FakeConn *conn) {
  return reinterpret_cast<xqc_connection_t *>(conn);
}

xqc_stream_t *AsStream(FakeStream *stream) {
  return reinterpret_cast<xqc_stream_t *>(stream);
}

FakeStream *FromStream(xqc_stream_t *stream) {
  return reinterpret_cast<FakeStream *>(stream);
}

struct QuicHarness {
  odin_event_loop_t *loop = nullptr;
  odin_xqc_server_runtime_t *rt = nullptr;
  char engine_storage = 0;
  xqc_engine_t *engine = reinterpret_cast<xqc_engine_t *>(&engine_storage);
  void *xu_user_data = nullptr;
  xqc_transport_callbacks_t transport_callbacks;
  xqc_app_proto_callbacks_t *app_callbacks = nullptr;
  struct sockaddr_in local_addr;
  xqc_engine_callback_t engine_callbacks;
  xqc_config_t engine_config;
  xqc_engine_ssl_config_t ssl_config;
  int engine_create_calls = 0;
  int engine_destroy_calls = 0;
  int alpn_unregister_calls = 0;
  std::vector<xqc_cid_t> registered;
  std::vector<xqc_cid_t> unregistered;
  std::vector<xqc_cid_t> closed;
  std::function<void(xqc_engine_t *)> engine_destroy_action;
};

QuicHarness *g_harness = nullptr;

xqc_engine_t *FakeEngineCreate(xqc_engine_type_t, const xqc_config_t *,
                               const xqc_engine_ssl_config_t *,
                               const xqc_engine_callback_t *,
                               const xqc_transport_callbacks_t *transport_cbs,
                               void *user_data) {
  if (g_harness == nullptr) {
    return nullptr;
  }
  g_harness->engine_create_calls += 1;
  g_harness->xu_user_data = user_data;
  g_harness->transport_callbacks = *transport_cbs;
  return g_harness->engine;
}

xqc_engine_t *T5EngineCreateFail(xqc_engine_type_t, const xqc_config_t *,
                                 const xqc_engine_ssl_config_t *,
                                 const xqc_engine_callback_t *,
                                 const xqc_transport_callbacks_t *,
                                 void *user_data) {
  odin_udp_t *udp = nullptr;
  uint16_t port = 0;
  if (odin_xqc_udp_test_udp(static_cast<odin_xqc_udp_t *>(user_data), &udp) ==
      0) {
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    socklen_t len = sizeof(addr);
    if (odin_udp_local_addr(udp, reinterpret_cast<struct sockaddr *>(&addr),
                            &len) == 0) {
      port = ntohs(addr.sin_port);
    }
  }
  if (g_t5_engine_create_port_fd >= 0) {
    (void)WriteAll(g_t5_engine_create_port_fd, &port, sizeof(port));
  }
  errno = EIO;
  return nullptr;
}

void FakeEngineDestroy(xqc_engine_t *engine) {
  if (g_harness == nullptr || engine != g_harness->engine) {
    return;
  }
  g_harness->engine_destroy_calls += 1;
  if (g_harness->engine_destroy_action) {
    g_harness->engine_destroy_action(engine);
  }
}

xqc_int_t FakeEngineRegisterAlpn(xqc_engine_t *engine, const char *alpn,
                                 size_t alpn_len,
                                 xqc_app_proto_callbacks_t *app_callbacks,
                                 void *user_data) {
  if (g_harness == nullptr || engine != g_harness->engine) {
    return -XQC_EPARAM;
  }
  EXPECT_EQ(std::string(alpn, alpn_len), ODIN_XQC_SERVER_ALPN);
  EXPECT_NE(user_data, nullptr);
  g_harness->app_callbacks = app_callbacks;
  return XQC_OK;
}

xqc_int_t FakeEngineUnregisterAlpn(xqc_engine_t *engine, const char *alpn,
                                   size_t alpn_len) {
  if (g_harness != nullptr && engine == g_harness->engine) {
    g_harness->alpn_unregister_calls += 1;
    EXPECT_EQ(std::string(alpn, alpn_len), ODIN_XQC_SERVER_ALPN);
  }
  return XQC_OK;
}

void FakeConnSetTransportUserData(xqc_connection_t *conn, void *user_data) {
  reinterpret_cast<FakeConn *>(conn)->transport_user_data = user_data;
}

void FakeConnSetAlpUserData(xqc_connection_t *conn, void *user_data) {
  reinterpret_cast<FakeConn *>(conn)->alp_user_data = user_data;
}

xqc_int_t FakeConnClose(xqc_engine_t *, const xqc_cid_t *cid) {
  if (g_harness != nullptr && cid != nullptr) {
    g_harness->closed.push_back(*cid);
  }
  return XQC_OK;
}

xqc_stream_direction_t FakeStreamGetDirection(xqc_stream_t *stream) {
  return FromStream(stream)->direction;
}

void *FakeGetConnAlpUserDataByStream(xqc_stream_t *stream) {
  FakeStream *fake = FromStream(stream);
  return fake->conn != nullptr ? fake->conn->alp_user_data : nullptr;
}

xqc_int_t FakeStreamClose(xqc_stream_t *stream) {
  FromStream(stream)->close_calls += 1;
  return XQC_OK;
}

int FakeUdpRegisterConn(odin_xqc_udp_t *, const xqc_cid_t *cid) {
  if (g_harness != nullptr && cid != nullptr) {
    g_harness->registered.push_back(*cid);
  }
  return 0;
}

void FakeUdpUnregisterConn(odin_xqc_udp_t *, const xqc_cid_t *cid) {
  if (g_harness != nullptr && cid != nullptr) {
    g_harness->unregistered.push_back(*cid);
  }
}

ssize_t FakeRecv(xqc_stream_t *stream, unsigned char *recv_buf,
                 size_t recv_buf_size, uint8_t *fin) {
  FakeStream *fake = FromStream(stream);
  if (fake->recv_steps.empty()) {
    *fin = 0;
    return -XQC_EAGAIN;
  }
  const RecvStep step = fake->recv_steps.front();
  fake->recv_steps.pop_front();
  *fin = step.fin;
  const size_t copy = std::min(step.data.size(), recv_buf_size);
  if (copy != 0) {
    std::copy_n(reinterpret_cast<const unsigned char *>(step.data.data()), copy,
                recv_buf);
  }
  return step.ret;
}

ssize_t FakeSend(xqc_stream_t *stream, unsigned char *send_data,
                 size_t send_data_size, uint8_t fin) {
  FakeStream *fake = FromStream(stream);
  SendRecord rec;
  rec.size = send_data_size;
  rec.fin = fin;
  rec.null_data = send_data == nullptr;
  if (send_data != nullptr && send_data_size != 0) {
    rec.data.assign(reinterpret_cast<const char *>(send_data), send_data_size);
  }
  fake->sends.push_back(rec);
  return static_cast<ssize_t>(send_data_size);
}

void FakeSetStreamUserData(xqc_stream_t *stream, void *user_data) {
  FakeStream *fake = FromStream(stream);
  fake->user_data = user_data;
  fake->user_data_values.push_back(user_data);
}

std::string DataSends(const FakeStream &stream) {
  std::string out;
  for (const SendRecord &rec : stream.sends) {
    if (!rec.null_data && rec.fin == 0) {
      out.append(rec.data);
    }
  }
  return out;
}

xqc_cid_t Cid(uint8_t tag) {
  xqc_cid_t cid;
  std::memset(&cid, 0, sizeof(cid));
  cid.cid_len = 1;
  cid.cid_buf[0] = tag;
  return cid;
}

void InstallHarnessOps(QuicHarness *h) {
  g_harness = h;
  static const odin_xqc_udp_test_ops_t kUdpOps = {
      FakeEngineCreate, FakeEngineDestroy, nullptr, nullptr,
      nullptr,          nullptr,           nullptr};
  odin_xqc_udp_test_set_ops(&kUdpOps);
  static const odin_xqc_server_runtime_test_ops_t kRuntimeOps = {
      FakeEngineRegisterAlpn,
      FakeEngineUnregisterAlpn,
      FakeConnSetTransportUserData,
      FakeConnSetAlpUserData,
      FakeConnClose,
      FakeStreamGetDirection,
      FakeGetConnAlpUserDataByStream,
      FakeStreamClose,
      FakeUdpRegisterConn,
      FakeUdpUnregisterConn,
  };
  odin_xqc_server_runtime_test_reset();
  odin_xqc_server_runtime_test_set_ops(&kRuntimeOps);
  static const odin_xqc_stream_transport_test_ops_t kStreamOps = {
      FakeRecv,
      FakeSend,
      FakeSetStreamUserData,
  };
  odin_xqc_stream_transport_test_set_ops(&kStreamOps);
}

void ClearHarnessOps() {
  odin_xqc_stream_transport_test_set_ops(nullptr);
  odin_xqc_server_runtime_test_set_ops(nullptr);
  odin_xqc_udp_test_set_ops(nullptr);
  g_harness = nullptr;
}

void InitHarness(QuicHarness *h) {
  std::memset(&h->transport_callbacks, 0, sizeof(h->transport_callbacks));
  std::memset(&h->engine_callbacks, 0, sizeof(h->engine_callbacks));
  std::memset(&h->engine_config, 0, sizeof(h->engine_config));
  std::memset(&h->ssl_config, 0, sizeof(h->ssl_config));
  std::memset(&h->local_addr, 0, sizeof(h->local_addr));
  h->local_addr.sin_family = AF_INET;
  h->local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  h->local_addr.sin_port = 0;
  ASSERT_EQ(odin_event_loop_create(&h->loop), 0) << std::strerror(errno);
  InstallHarnessOps(h);
}

void CreateHarnessRuntime(QuicHarness *h) {
  odin_xqc_server_runtime_config_t cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.loop = h->loop;
  cfg.local_addr = reinterpret_cast<const struct sockaddr *>(&h->local_addr);
  cfg.local_addrlen = sizeof(h->local_addr);
  cfg.engine_config = &h->engine_config;
  cfg.ssl_config = &h->ssl_config;
  cfg.engine_callbacks = &h->engine_callbacks;
  ASSERT_EQ(odin_xqc_server_runtime_create(&cfg, &h->rt), 0)
      << std::strerror(errno);
  ASSERT_NE(h->rt, nullptr);
  ASSERT_NE(h->xu_user_data, nullptr);
  ASSERT_NE(h->app_callbacks, nullptr);
}

void DestroyHarness(QuicHarness *h) {
  if (h->rt != nullptr) {
    odin_xqc_server_runtime_destroy(h->rt);
    h->rt = nullptr;
  }
  ClearHarnessOps();
  if (h->loop != nullptr) {
    odin_event_loop_destroy(h->loop);
    h->loop = nullptr;
  }
}

void AcceptConn(QuicHarness *h, FakeConn *conn, const xqc_cid_t &cid) {
  ASSERT_EQ(h->transport_callbacks.server_accept(h->engine, AsConn(conn), &cid,
                                                 h->xu_user_data),
            0);
  ASSERT_NE(conn->alp_user_data, nullptr);
  ASSERT_EQ(h->app_callbacks->conn_cbs.conn_create_notify(
                AsConn(conn), &cid, h->xu_user_data, conn->alp_user_data),
            0);
}

void CreateBidiStream(QuicHarness *h, FakeConn *conn, FakeStream *stream) {
  stream->conn = conn;
  stream->direction = XQC_STREAM_BIDI;
  ASSERT_EQ(h->app_callbacks->stream_cbs.stream_create_notify(
                AsStream(stream), stream->user_data),
            XQC_OK);
  ASSERT_NE(stream->user_data, nullptr);
}

void QueueRecv(FakeStream *stream, const std::string &data, uint8_t fin) {
  stream->recv_steps.push_back(
      RecvStep{data, static_cast<ssize_t>(data.size()), fin});
}

unsigned int CountRuntimeCalls(odin_xqc_server_runtime_test_call_kind_t kind) {
  const odin_xqc_server_runtime_test_record_t *record =
      odin_xqc_server_runtime_test_record();
  unsigned int count = 0;
  for (unsigned int i = 0; i < record->call_count; ++i) {
    if (record->calls[i].kind == kind) {
      count += 1;
    }
  }
  return count;
}

int FirstRuntimeCall(odin_xqc_server_runtime_test_call_kind_t kind) {
  const odin_xqc_server_runtime_test_record_t *record =
      odin_xqc_server_runtime_test_record();
  for (unsigned int i = 0; i < record->call_count; ++i) {
    if (record->calls[i].kind == kind) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

unsigned int
CountRuntimeCallsWithUserData(odin_xqc_server_runtime_test_call_kind_t kind,
                              bool null_user_data) {
  const odin_xqc_server_runtime_test_record_t *record =
      odin_xqc_server_runtime_test_record();
  unsigned int count = 0;
  for (unsigned int i = 0; i < record->call_count; ++i) {
    const auto &call = record->calls[i];
    if (call.kind == kind && ((call.user_data == nullptr) == null_user_data)) {
      count += 1;
    }
  }
  return count;
}

int FirstRuntimeCallWithUserData(odin_xqc_server_runtime_test_call_kind_t kind,
                                 bool null_user_data) {
  const odin_xqc_server_runtime_test_record_t *record =
      odin_xqc_server_runtime_test_record();
  for (unsigned int i = 0; i < record->call_count; ++i) {
    const auto &call = record->calls[i];
    if (call.kind == kind && ((call.user_data == nullptr) == null_user_data)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

struct T5ChildReport {
  int rc = -1;
  LivenessSnapshot live{};
};

enum T7Variant {
  kT7Baseline = 0,
  kT7LiveStream = 1,
  kT7FinalConnClose = 2,
  kT7FinalServerRefuse = 3,
  kT7GuardServerAccept = 4,
  kT7GuardConnCreate = 5,
  kT7GuardUpdateCid = 6,
  kT7InertStreamCreate = 7,
};

const char *T7VariantName(T7Variant variant) {
  switch (variant) {
  case kT7Baseline:
    return "baseline";
  case kT7LiveStream:
    return "live-stream";
  case kT7FinalConnClose:
    return "final-conn-close";
  case kT7FinalServerRefuse:
    return "final-server-refuse";
  case kT7GuardServerAccept:
    return "guard-server-accept";
  case kT7GuardConnCreate:
    return "guard-conn-create";
  case kT7GuardUpdateCid:
    return "guard-update-cid";
  case kT7InertStreamCreate:
    return "inert-stream-create";
  }
  return "unknown";
}

bool T7VariantUsesFakeRuntime(T7Variant variant) {
  return variant != kT7Baseline;
}

bool T7VariantUsesProbe(T7Variant variant) { return variant != kT7Baseline; }

bool T7VariantUsesEngineDestroyAction(T7Variant variant) {
  return variant >= kT7FinalConnClose;
}

struct T7ChildReport {
  int rc = -1;
  int sigint_count = -1;
  int sigterm_count = -1;
  LivenessSnapshot live{};
  int probe_ran = 0;
  int probe_accept_result = 0;
  int probe_conn_create_result = 0;
  int probe_stream_create_result = 0;
  int probe_stream_user_data_nonnull = 0;
  int live_stream_user_data_null_after_cleanup = 0;
  int engine_destroy_calls = 0;
  int alpn_unregister_calls = 0;
  unsigned int udp_destroy_calls = 0;
  unsigned int engine_unregister_calls = 0;
  unsigned int udp_register_calls = 0;
  unsigned int udp_unregister_calls = 0;
  unsigned int conn_close_calls = 0;
  unsigned int stream_close_calls = 0;
  unsigned int conn_set_alp_null_calls = 0;
  unsigned int conn_set_alp_nonnull_calls = 0;
  int conn_set_alp_null_before_udp_destroy = 0;
  int stream_user_data_null_at_destroy = 0;
  int stream_closing_before_final = 0;
  int stream_close_before_final = 0;
  int conn_close_final_called = 0;
  int server_refuse_final_called = 0;
  int final_conn_proto_data_null = 0;
  int final_refuse_conn_alp_null = 0;
  int server_accept_result = 0;
  int conn_create_result = 0;
  int update_cid_called = 0;
  unsigned int guard_register_delta = 0;
  unsigned int guard_unregister_delta = 0;
  unsigned int guard_conn_close_delta = 0;
  unsigned int guard_nonnull_alp_delta = 0;
  int inert_result = 0;
  int inert_conn_alp_null = 0;
  int inert_close_calls = 0;
  int inert_user_data_installs = 0;
};

struct T7ChildState {
  T7Variant variant = kT7Baseline;
  int probe_fd = -1;
  QuicHarness *h = nullptr;
  T7ChildReport *report = nullptr;
  FakeConn conn;
  FakeStream stream;
  FakeStream inert_stream;
  xqc_cid_t cid{};
};

void InitCliHarnessOnly(QuicHarness *h) {
  std::memset(&h->transport_callbacks, 0, sizeof(h->transport_callbacks));
  std::memset(&h->engine_callbacks, 0, sizeof(h->engine_callbacks));
  std::memset(&h->engine_config, 0, sizeof(h->engine_config));
  std::memset(&h->ssl_config, 0, sizeof(h->ssl_config));
  std::memset(&h->local_addr, 0, sizeof(h->local_addr));
  InstallHarnessOps(h);
}

void FillRuntimeSummary(T7ChildReport *report) {
  report->udp_destroy_calls =
      CountRuntimeCalls(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_DESTROY);
  report->engine_unregister_calls = CountRuntimeCalls(
      ODIN_XQC_SERVER_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN);
  report->udp_register_calls =
      CountRuntimeCalls(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_REGISTER_CONN);
  report->udp_unregister_calls =
      CountRuntimeCalls(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN);
  report->conn_close_calls =
      CountRuntimeCalls(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_CLOSE);
  report->stream_close_calls =
      CountRuntimeCalls(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_CLOSE);
  report->conn_set_alp_null_calls = CountRuntimeCallsWithUserData(
      ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_SET_ALP_USER_DATA, true);
  report->conn_set_alp_nonnull_calls = CountRuntimeCallsWithUserData(
      ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_SET_ALP_USER_DATA, false);
  const int clear_idx = FirstRuntimeCallWithUserData(
      ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_SET_ALP_USER_DATA, true);
  const int destroy_idx =
      FirstRuntimeCall(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_DESTROY);
  report->conn_set_alp_null_before_udp_destroy =
      clear_idx >= 0 && destroy_idx >= 0 && clear_idx < destroy_idx;
}

void RecordGuardDeltas(T7ChildReport *report, unsigned int reg_before,
                       unsigned int unreg_before, unsigned int close_before,
                       unsigned int nonnull_alp_before) {
  report->guard_register_delta =
      CountRuntimeCalls(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_REGISTER_CONN) -
      reg_before;
  report->guard_unregister_delta =
      CountRuntimeCalls(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN) -
      unreg_before;
  report->guard_conn_close_delta =
      CountRuntimeCalls(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_CLOSE) -
      close_before;
  report->guard_nonnull_alp_delta =
      CountRuntimeCallsWithUserData(
          ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_SET_ALP_USER_DATA, false) -
      nonnull_alp_before;
}

void T7RunEngineDestroyAction(T7ChildState *state) {
  QuicHarness *h = state->h;
  T7ChildReport *report = state->report;
  if (h == nullptr || report == nullptr) {
    return;
  }

  if (state->variant == kT7FinalConnClose ||
      state->variant == kT7FinalServerRefuse) {
    int seq = 0;
    void *stream_ud = state->stream.user_data;
    report->stream_user_data_null_at_destroy = stream_ud == nullptr;
    h->app_callbacks->stream_cbs.stream_closing_notify(
        AsStream(&state->stream), XQC_ESTREAM_RESET, stream_ud);
    const int closing_seq = ++seq;
    (void)h->app_callbacks->stream_cbs.stream_close_notify(
        AsStream(&state->stream), stream_ud);
    const int close_seq = ++seq;
    if (state->variant == kT7FinalConnClose) {
      report->final_conn_proto_data_null = state->conn.alp_user_data == nullptr;
      (void)h->app_callbacks->conn_cbs.conn_close_notify(
          AsConn(&state->conn), &state->cid, h->xu_user_data,
          state->conn.alp_user_data);
      report->conn_close_final_called = 1;
    } else {
      report->final_refuse_conn_alp_null = state->conn.alp_user_data == nullptr;
      h->transport_callbacks.server_refuse(h->engine, AsConn(&state->conn),
                                           &state->cid, h->xu_user_data);
      report->server_refuse_final_called = 1;
    }
    const int final_seq = ++seq;
    report->stream_closing_before_final = closing_seq < final_seq;
    report->stream_close_before_final = close_seq < final_seq;
    return;
  }

  const unsigned int reg_before =
      CountRuntimeCalls(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_REGISTER_CONN);
  const unsigned int unreg_before =
      CountRuntimeCalls(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN);
  const unsigned int close_before =
      CountRuntimeCalls(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_CLOSE);
  const unsigned int nonnull_alp_before = CountRuntimeCallsWithUserData(
      ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_SET_ALP_USER_DATA, false);

  if (state->variant == kT7GuardServerAccept) {
    FakeConn fresh;
    const xqc_cid_t fresh_cid = Cid(0xE1);
    report->server_accept_result = h->transport_callbacks.server_accept(
        h->engine, AsConn(&fresh), &fresh_cid, h->xu_user_data);
    RecordGuardDeltas(report, reg_before, unreg_before, close_before,
                      nonnull_alp_before);
    return;
  }
  if (state->variant == kT7GuardConnCreate) {
    FakeConn fresh;
    const xqc_cid_t fresh_cid = Cid(0xE2);
    report->conn_create_result = h->app_callbacks->conn_cbs.conn_create_notify(
        AsConn(&fresh), &fresh_cid, h->xu_user_data,
        reinterpret_cast<void *>(0x1234));
    RecordGuardDeltas(report, reg_before, unreg_before, close_before,
                      nonnull_alp_before);
    return;
  }
  if (state->variant == kT7GuardUpdateCid) {
    FakeConn fresh;
    const xqc_cid_t old_cid = Cid(0xE3);
    const xqc_cid_t new_cid = Cid(0xE4);
    h->transport_callbacks.conn_update_cid_notify(AsConn(&fresh), &old_cid,
                                                  &new_cid, h->xu_user_data);
    report->update_cid_called = 1;
    RecordGuardDeltas(report, reg_before, unreg_before, close_before,
                      nonnull_alp_before);
    return;
  }
  if (state->variant == kT7InertStreamCreate) {
    report->inert_conn_alp_null = state->conn.alp_user_data == nullptr;
    state->inert_stream.conn = &state->conn;
    report->inert_result = h->app_callbacks->stream_cbs.stream_create_notify(
        AsStream(&state->inert_stream), state->inert_stream.user_data);
    report->inert_close_calls = state->inert_stream.close_calls;
    report->inert_user_data_installs =
        static_cast<int>(state->inert_stream.user_data_values.size());
  }
}

void T7QuicStartProbe(odin_xqc_server_runtime_t *rt, void *user_data) {
  T7ChildState *state = static_cast<T7ChildState *>(user_data);
  if (state == nullptr || state->h == nullptr || state->report == nullptr) {
    return;
  }
  state->h->rt = rt;
  state->report->probe_ran = 1;

  const bool needs_linked_conn = state->variant == kT7LiveStream ||
                                 state->variant == kT7FinalConnClose ||
                                 state->variant == kT7FinalServerRefuse ||
                                 state->variant == kT7InertStreamCreate;
  if (needs_linked_conn) {
    state->cid = Cid(0xA1);
    state->report->probe_accept_result =
        state->h->transport_callbacks.server_accept(
            state->h->engine, AsConn(&state->conn), &state->cid,
            state->h->xu_user_data);
    state->report->probe_conn_create_result =
        state->h->app_callbacks->conn_cbs.conn_create_notify(
            AsConn(&state->conn), &state->cid, state->h->xu_user_data,
            state->conn.alp_user_data);
  }

  const bool needs_stream = state->variant == kT7LiveStream ||
                            state->variant == kT7FinalConnClose ||
                            state->variant == kT7FinalServerRefuse;
  if (needs_stream) {
    state->stream.conn = &state->conn;
    state->stream.direction = XQC_STREAM_BIDI;
    state->report->probe_stream_create_result =
        state->h->app_callbacks->stream_cbs.stream_create_notify(
            AsStream(&state->stream), state->stream.user_data);
    state->report->probe_stream_user_data_nonnull =
        state->stream.user_data != nullptr;
  }

  if (state->probe_fd >= 0) {
    const char b = 1;
    (void)WriteAll(state->probe_fd, &b, 1);
  }
}

void RunT7ChildCase(int delivered_sig, T7Variant variant) {
  int out_pipe[2];
  int err_pipe[2];
  int progress_pipe[2];
  int probe_pipe[2];
  int report_pipe[2];
  int release_pipe[2];
  ASSERT_EQ(pipe(out_pipe), 0);
  ASSERT_EQ(pipe(err_pipe), 0);
  ASSERT_EQ(pipe(progress_pipe), 0);
  ASSERT_EQ(pipe(probe_pipe), 0);
  ASSERT_EQ(pipe(report_pipe), 0);
  ASSERT_EQ(pipe(release_pipe), 0);

  const pid_t pid = fork();
  ASSERT_NE(pid, -1);
  if (pid == 0) {
    close(out_pipe[0]);
    close(err_pipe[0]);
    close(progress_pipe[0]);
    close(probe_pipe[0]);
    close(report_pipe[0]);
    close(release_pipe[1]);
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);
    close(out_pipe[1]);
    close(err_pipe[1]);

    odin_cli_server_test_reset_liveness();
    odin_event_loop_test_reset_liveness();
    odin_xqc_server_runtime_test_reset();
    (void)odin_cli_server_test_set_progress_fd(progress_pipe[1]);

    T7ChildReport report;
    QuicHarness h;
    T7ChildState state;
    if (T7VariantUsesFakeRuntime(variant)) {
      InitCliHarnessOnly(&h);
      state.variant = variant;
      state.probe_fd = probe_pipe[1];
      state.h = &h;
      state.report = &report;
      if (T7VariantUsesEngineDestroyAction(variant)) {
        h.engine_destroy_action = [&](xqc_engine_t *) {
          T7RunEngineDestroyAction(&state);
        };
      }
      (void)odin_cli_server_test_set_quic_start_probe(T7QuicStartProbe, &state);
    } else {
      close(probe_pipe[1]);
    }

    g_counted_sigint = 0;
    g_counted_sigterm = 0;
    struct sigaction sa_int;
    std::memset(&sa_int, 0, sizeof(sa_int));
    sa_int.sa_handler = CountSigint;
    sigemptyset(&sa_int.sa_mask);
    (void)sigaction(SIGINT, &sa_int, nullptr);
    struct sigaction sa_term;
    std::memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = CountSigterm;
    sigemptyset(&sa_term.sa_mask);
    (void)sigaction(SIGTERM, &sa_term, nullptr);

    MutableArgv argv({"odin-server", "--listen", "0", "--quic-cert", kCertPath,
                      "--quic-key", kKeyPath});
    const int rc = odin_cli_main(argv.argc(), argv.argv(), stdout, stderr);
    (void)fflush(stdout);
    (void)fflush(stderr);

    report.rc = rc;
    report.live = SnapshotLiveness();
    FillRuntimeSummary(&report);
    if (T7VariantUsesFakeRuntime(variant)) {
      report.live_stream_user_data_null_after_cleanup =
          state.stream.user_data == nullptr;
      report.engine_destroy_calls = h.engine_destroy_calls;
      report.alpn_unregister_calls = h.alpn_unregister_calls;
      ClearHarnessOps();
    }

    (void)raise(SIGINT);
    (void)raise(SIGTERM);
    report.sigint_count = static_cast<int>(g_counted_sigint);
    report.sigterm_count = static_cast<int>(g_counted_sigterm);
    (void)WriteAll(report_pipe[1], &report, sizeof(report));
    char go = 0;
    (void)read(release_pipe[0], &go, 1);
    _exit(rc);
  }

  close(out_pipe[1]);
  close(err_pipe[1]);
  close(progress_pipe[1]);
  close(probe_pipe[1]);
  close(report_pipe[1]);
  close(release_pipe[0]);

  const std::string line = ReadLineWithDeadline(err_pipe[0], 5000);
  uint16_t port = 0;
  ASSERT_TRUE(ParseQuicStartupLine(line, &port))
      << T7VariantName(variant) << " signal=" << delivered_sig
      << " line=" << line;
  EXPECT_GT(port, 0);

  char progress = 0;
  ASSERT_EQ(ReadWithDeadline(progress_pipe[0], &progress, 1, 5000), 1)
      << T7VariantName(variant);
  if (T7VariantUsesProbe(variant)) {
    char probe = 0;
    ASSERT_EQ(ReadWithDeadline(probe_pipe[0], &probe, 1, 5000), 1)
        << T7VariantName(variant);
  }

  EXPECT_EQ(waitpid(pid, nullptr, WNOHANG), 0) << T7VariantName(variant);
  ASSERT_EQ(kill(pid, delivered_sig), 0) << T7VariantName(variant);

  T7ChildReport report;
  const ssize_t got =
      ReadWithDeadline(report_pipe[0], &report, sizeof(report), 5000);
  if (got != static_cast<ssize_t>(sizeof(report))) {
    (void)kill(pid, SIGKILL);
    FAIL() << "timed out waiting for T7 child report: "
           << T7VariantName(variant);
  }

  EXPECT_EQ(report.rc, 0) << T7VariantName(variant);
  EXPECT_EQ(report.sigint_count, 1) << T7VariantName(variant);
  EXPECT_EQ(report.sigterm_count, 1) << T7VariantName(variant);
  ExpectZeroLiveness(report.live);
  const int dup = TryBindUdpAny(port);
  ASSERT_GE(dup, 0) << T7VariantName(variant) << " " << std::strerror(errno);
  close(dup);

  const char go = 1;
  ASSERT_EQ(WriteAll(release_pipe[1], &go, 1), 0);
  int wstatus = 0;
  ASSERT_EQ(WaitChildBounded(pid, 3000, &wstatus), 0) << T7VariantName(variant);
  EXPECT_TRUE(WIFEXITED(wstatus)) << T7VariantName(variant);
  EXPECT_EQ(WEXITSTATUS(wstatus), 0) << T7VariantName(variant);
  EXPECT_EQ(DrainFd(out_pipe[0]), "") << T7VariantName(variant);
  const std::string rest = DrainFd(err_pipe[0]);
  EXPECT_EQ(rest.find("failed"), std::string::npos)
      << T7VariantName(variant) << " stderr_rest=" << rest;

  if (T7VariantUsesProbe(variant)) {
    EXPECT_EQ(report.probe_ran, 1) << T7VariantName(variant);
  }
  if (variant == kT7LiveStream || variant == kT7FinalConnClose ||
      variant == kT7FinalServerRefuse) {
    EXPECT_EQ(report.probe_accept_result, 0) << T7VariantName(variant);
    EXPECT_EQ(report.probe_conn_create_result, 0) << T7VariantName(variant);
    EXPECT_EQ(report.probe_stream_create_result, XQC_OK)
        << T7VariantName(variant);
    EXPECT_EQ(report.probe_stream_user_data_nonnull, 1)
        << T7VariantName(variant);
    EXPECT_EQ(report.live_stream_user_data_null_after_cleanup, 1)
        << T7VariantName(variant);
  }
  if (T7VariantUsesFakeRuntime(variant)) {
    EXPECT_EQ(report.udp_destroy_calls, 1u) << T7VariantName(variant);
    EXPECT_EQ(report.engine_unregister_calls, 1u) << T7VariantName(variant);
    EXPECT_EQ(report.engine_destroy_calls, 1) << T7VariantName(variant);
    EXPECT_EQ(report.alpn_unregister_calls, 1) << T7VariantName(variant);
  }

  if (variant == kT7FinalConnClose || variant == kT7FinalServerRefuse) {
    EXPECT_EQ(report.stream_user_data_null_at_destroy, 1)
        << T7VariantName(variant);
    EXPECT_EQ(report.stream_closing_before_final, 1) << T7VariantName(variant);
    EXPECT_EQ(report.stream_close_before_final, 1) << T7VariantName(variant);
    EXPECT_EQ(report.conn_set_alp_null_before_udp_destroy, 1)
        << T7VariantName(variant);
    EXPECT_EQ(report.conn_set_alp_null_calls, 1u) << T7VariantName(variant);
    EXPECT_EQ(report.conn_set_alp_nonnull_calls, 1u) << T7VariantName(variant);
    EXPECT_EQ(report.udp_unregister_calls, 1u) << T7VariantName(variant);
    EXPECT_EQ(report.conn_close_calls, 1u) << T7VariantName(variant);
  }
  if (variant == kT7FinalConnClose) {
    EXPECT_EQ(report.conn_close_final_called, 1);
    EXPECT_EQ(report.server_refuse_final_called, 0);
    EXPECT_EQ(report.final_conn_proto_data_null, 1);
  }
  if (variant == kT7FinalServerRefuse) {
    EXPECT_EQ(report.server_refuse_final_called, 1);
    EXPECT_EQ(report.conn_close_final_called, 0);
    EXPECT_EQ(report.final_refuse_conn_alp_null, 1);
  }
  if (variant == kT7GuardServerAccept || variant == kT7GuardConnCreate ||
      variant == kT7GuardUpdateCid) {
    if (variant == kT7GuardServerAccept) {
      EXPECT_EQ(report.server_accept_result, -1);
    } else if (variant == kT7GuardConnCreate) {
      EXPECT_EQ(report.conn_create_result, -1);
    } else {
      EXPECT_EQ(report.update_cid_called, 1);
    }
    EXPECT_EQ(report.guard_register_delta, 0u) << T7VariantName(variant);
    EXPECT_EQ(report.guard_unregister_delta, 0u) << T7VariantName(variant);
    EXPECT_EQ(report.guard_conn_close_delta, 0u) << T7VariantName(variant);
    EXPECT_EQ(report.guard_nonnull_alp_delta, 0u) << T7VariantName(variant);
    EXPECT_EQ(report.udp_register_calls, 0u) << T7VariantName(variant);
    EXPECT_EQ(report.udp_unregister_calls, 0u) << T7VariantName(variant);
    EXPECT_EQ(report.conn_close_calls, 0u) << T7VariantName(variant);
    EXPECT_EQ(report.conn_set_alp_nonnull_calls, 0u) << T7VariantName(variant);
  }
  if (variant == kT7InertStreamCreate) {
    EXPECT_EQ(report.probe_accept_result, 0);
    EXPECT_EQ(report.probe_conn_create_result, 0);
    EXPECT_EQ(report.inert_conn_alp_null, 1);
    EXPECT_EQ(report.inert_result, XQC_OK);
    EXPECT_EQ(report.inert_close_calls, 1);
    EXPECT_EQ(report.inert_user_data_installs, 0);
    EXPECT_EQ(report.stream_close_calls, 1u);
    EXPECT_EQ(report.conn_set_alp_null_before_udp_destroy, 1);
    EXPECT_EQ(report.conn_set_alp_null_calls, 1u);
    EXPECT_EQ(report.conn_set_alp_nonnull_calls, 1u);
  }

  close(out_pipe[0]);
  close(err_pipe[0]);
  close(progress_pipe[0]);
  close(probe_pipe[0]);
  close(report_pipe[0]);
  close(release_pipe[1]);
}

odin_server_session_dial_filter_cb CaptureQuicCliFilter() {
  odin_cli_server_test_reset_liveness();
  odin_event_loop_test_reset_liveness();
  odin_xqc_server_runtime_test_reset();
  if (odin_cli_server_test_fail_next(
          ODIN_CLI_SERVER_TEST_FAIL_SIGNAL_TIMER_START, EIO) != 0) {
    return nullptr;
  }
  const MainResult r = RunMain({"odin-server", "--listen", "0", "--quic-cert",
                                kCertPath, "--quic-key", kKeyPath});
  EXPECT_EQ(r.rc, 1);
  odin_cli_server_test_filter_record_t record{};
  EXPECT_EQ(odin_cli_server_test_filter_record(&record), 0);
  EXPECT_EQ(record.quic_set_count, 1u);
  return record.quic_cb;
}

} // namespace

TEST(OdinCliServerQuicParserTest, T1QuicDefault) {
  struct Case {
    std::vector<std::string> tokens;
    uint16_t expected_port;
    odin_cli_server_transport_t expected_transport;
  };
  const std::vector<Case> cases = {
      {{"odin-server", "--quic-cert", "C", "--quic-key", "K"},
       ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER,
       ODIN_CLI_SERVER_TRANSPORT_QUIC},
      {{"odin-server", "--listen", "", "--quic-cert", "C", "--quic-key", "K"},
       ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER,
       ODIN_CLI_SERVER_TRANSPORT_QUIC},
      {{"odin-server", "--listen", "0", "--quic-cert", "C", "--quic-key", "K"},
       0,
       ODIN_CLI_SERVER_TRANSPORT_QUIC},
  };
  for (const Case &c : cases) {
    MutableArgv argv(c.tokens);
    odin_cli_args_t out{};
    EXPECT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out), ODIN_CLI_OK);
    EXPECT_EQ(out.mode, ODIN_CLI_MODE_SERVER);
    EXPECT_EQ(out.listen_port, c.expected_port);
    EXPECT_EQ(out.server_transport, c.expected_transport);
    EXPECT_STREQ(out.quic_cert_file, "C");
    EXPECT_STREQ(out.quic_key_file, "K");
  }
}

TEST(OdinCliServerQuicParserTest, T2QuicParserAndUsageContract) {
  {
    MutableArgv argv({"odin-server", "--listen", "9443", "--quic-cert", "C",
                      "--quic-key", "K"});
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out), ODIN_CLI_OK);
    EXPECT_EQ(out.listen_port, 9443);
    EXPECT_EQ(out.server_transport, ODIN_CLI_SERVER_TRANSPORT_QUIC);
    EXPECT_EQ(out.quic_cert_file, argv.argv()[4]);
    EXPECT_EQ(out.quic_key_file, argv.argv()[6]);
  }

  for (const char *transport : {"udp", "QUIC", ""}) {
    MutableArgv argv({"odin-server", "--transport", transport});
    odin_cli_args_t out{};
    EXPECT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out),
              ODIN_CLI_ERR_UNKNOWN_FLAG)
        << transport;
  }

  const std::vector<std::vector<std::string>> bad_tls = {
      {"odin-server", "--quic-cert", "C"},
      {"odin-server", "--quic-key", "K"},
      {"odin-server", "--quic-cert", "", "--quic-key", "K"},
      {"odin-server", "--quic-cert", "C", "--quic-key", ""},
      {"odin-server", "--listen", "0", "--quic-cert", "C"},
      {"odin-server", "--listen", "0", "--quic-key", "K"},
  };
  for (const auto &tokens : bad_tls) {
    MutableArgv argv(tokens);
    odin_cli_args_t out{};
    EXPECT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out),
              ODIN_CLI_ERR_BAD_QUIC_TLS);
  }

  const std::vector<std::vector<std::string>> mixed_precedence = {
      {"odin-server", "--listen", "abc", "--quic-cert", "C"},
      {"odin-server", "--listen", "abc", "--quic-cert", "C", "--quic-key", "K"},
  };
  for (const auto &tokens : mixed_precedence) {
    MutableArgv argv(tokens);
    odin_cli_args_t out{};
    EXPECT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out),
              ODIN_CLI_ERR_BAD_LISTEN_PORT);
    const MainResult r = RunMain(tokens);
    EXPECT_EQ(r.rc, 2);
    EXPECT_EQ(r.out, "");
    EXPECT_EQ(r.err, std::string("odin: invalid --listen port\n") +
                         kServerUsage + "\n");
  }

  for (const auto &tokens : std::vector<std::vector<std::string>>{
           {"odin-server", "--trans", "quic"},
           {"odin-server", "--quic-ce", "C"},
           {"odin-server", "--quic-ke", "K"},
           {"odin-client", "--quic-cert", "C", "--server", "S"},
           {"odin-client", "--quic-key", "K", "--server", "S"},
       }) {
    MutableArgv argv(tokens);
    odin_cli_args_t out{};
    EXPECT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out),
              ODIN_CLI_ERR_UNKNOWN_FLAG);
  }

  MainResult help = RunMain({"odin-server", "--help"});
  EXPECT_EQ(help.rc, 0);
  EXPECT_EQ(help.out, std::string(kServerUsage) + "\n");
  EXPECT_EQ(help.err, "");

  MainResult unknown_transport = RunMain({"odin-server", "--transport", "udp"});
  EXPECT_EQ(unknown_transport.rc, 2);
  EXPECT_EQ(unknown_transport.out, "");
  EXPECT_EQ(unknown_transport.err,
            std::string("odin: unknown or invalid flag\n") + kServerUsage +
                "\n");

  MainResult bad_quic_tls = RunMain({"odin-server", "--quic-cert", "C"});
  EXPECT_EQ(bad_quic_tls.rc, 2);
  EXPECT_EQ(bad_quic_tls.out, "");
  EXPECT_EQ(bad_quic_tls.err,
            std::string("odin: invalid QUIC TLS configuration\n") +
                kServerUsage + "\n");
}

TEST(OdinCliServerQuicRuntimeTest, T3QuicStartupBindsUdpAndReportsPort) {
  ASSERT_FALSE(g_test_argv0.empty());
  ChildHandle child = SpawnOdinServer(
      {"--listen", "0", "--quic-cert", kCertPath, "--quic-key", kKeyPath});
  ASSERT_NE(child.pid, -1);
  const std::string line = ReadLineWithDeadline(child.stderr_fd, 4000);
  uint16_t port = 0;
  ASSERT_TRUE(ParseQuicStartupLine(line, &port)) << line;
  EXPECT_GT(port, 0);
  const int dup = TryBindUdpAny(port);
  EXPECT_EQ(dup, -1);
  EXPECT_EQ(errno, EADDRINUSE);
  EXPECT_EQ(kill(child.pid, SIGTERM), 0);
  int wstatus = 0;
  ASSERT_EQ(WaitChildBounded(child.pid, 3000, &wstatus), 0);
  EXPECT_TRUE(WIFEXITED(wstatus));
  EXPECT_EQ(WEXITSTATUS(wstatus), 0);
  EXPECT_EQ(DrainFd(child.stdout_fd), "");
  const std::string rest = DrainFd(child.stderr_fd);
  EXPECT_EQ(rest.find("odin: quic server startup failed"), std::string::npos);
  close(child.stdout_fd);
  close(child.stderr_fd);
}

TEST(OdinCliServerQuicRuntimeTest, T4QuicUdpBindCollisionReportsFailure) {
  uint16_t port = 0;
  const int parent_udp = OpenUdpAny(0, &port);
  ASSERT_GE(parent_udp, 0) << std::strerror(errno);
  ChildHandle child =
      SpawnOdinServer({"--listen", std::to_string(port), "--quic-cert",
                       kCertPath, "--quic-key", kKeyPath});
  ASSERT_NE(child.pid, -1);
  int wstatus = 0;
  ASSERT_EQ(WaitChildBounded(child.pid, 4000, &wstatus), 0);
  EXPECT_TRUE(WIFEXITED(wstatus));
  EXPECT_EQ(WEXITSTATUS(wstatus), 1);
  EXPECT_EQ(DrainFd(child.stdout_fd), "");
  EXPECT_EQ(DrainFd(child.stderr_fd),
            "odin: quic server startup failed at xqc_server_runtime_create\n");
  close(child.stdout_fd);
  close(child.stderr_fd);
  const int dup = TryBindUdpAny(port);
  EXPECT_EQ(dup, -1);
  EXPECT_EQ(errno, EADDRINUSE);
  close(parent_udp);
}

TEST(OdinCliServerQuicRuntimeTest, T5QuicTlsAndPostBindFailuresCleanUdp) {
  for (bool engine_create_fail : {false, true}) {
    int out_pipe[2];
    int err_pipe[2];
    int report_pipe[2];
    int release_pipe[2];
    int port_pipe[2];
    ASSERT_EQ(pipe(out_pipe), 0);
    ASSERT_EQ(pipe(err_pipe), 0);
    ASSERT_EQ(pipe(report_pipe), 0);
    ASSERT_EQ(pipe(release_pipe), 0);
    ASSERT_EQ(pipe(port_pipe), 0);

    const pid_t pid = fork();
    ASSERT_NE(pid, -1);
    if (pid == 0) {
      close(out_pipe[0]);
      close(err_pipe[0]);
      close(report_pipe[0]);
      close(release_pipe[1]);
      close(port_pipe[0]);
      dup2(out_pipe[1], STDOUT_FILENO);
      dup2(err_pipe[1], STDERR_FILENO);
      close(out_pipe[1]);
      close(err_pipe[1]);

      odin_cli_server_test_reset_liveness();
      odin_event_loop_test_reset_liveness();
      odin_xqc_server_runtime_test_reset();
      if (engine_create_fail) {
        g_t5_engine_create_port_fd = port_pipe[1];
        const odin_xqc_udp_test_ops_t ops = {T5EngineCreateFail,
                                             nullptr,
                                             nullptr,
                                             nullptr,
                                             nullptr,
                                             nullptr,
                                             nullptr};
        odin_xqc_udp_test_set_ops(&ops);
      }

      std::vector<std::string> tokens = {
          "odin-server",
          "--listen",
          "0",
          "--quic-cert",
          engine_create_fail ? kCertPath : "/tmp/odin-no-such.crt",
          "--quic-key",
          engine_create_fail ? kKeyPath : "/tmp/odin-no-such.key",
      };
      MutableArgv argv(tokens);
      const int rc = odin_cli_main(argv.argc(), argv.argv(), stdout, stderr);
      (void)fflush(stdout);
      (void)fflush(stderr);

      T5ChildReport report;
      report.rc = rc;
      report.live = SnapshotLiveness();
      (void)WriteAll(report_pipe[1], &report, sizeof(report));
      char go = 0;
      (void)read(release_pipe[0], &go, 1);
      _exit(rc);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);
    close(report_pipe[1]);
    close(release_pipe[0]);
    close(port_pipe[1]);

    uint16_t bound_port = 0;
    if (engine_create_fail) {
      ASSERT_EQ(
          ReadWithDeadline(port_pipe[0], &bound_port, sizeof(bound_port), 3000),
          static_cast<ssize_t>(sizeof(bound_port)));
      EXPECT_GT(bound_port, 0);
    }

    T5ChildReport report;
    const ssize_t got =
        ReadWithDeadline(report_pipe[0], &report, sizeof(report), 4000);
    if (got != static_cast<ssize_t>(sizeof(report))) {
      (void)kill(pid, SIGKILL);
      FAIL() << "timed out waiting for T5 child report";
    }
    EXPECT_EQ(report.rc, 1);
    ExpectZeroLiveness(report.live);

    if (engine_create_fail) {
      const int dup = TryBindUdpAny(bound_port);
      ASSERT_GE(dup, 0) << std::strerror(errno);
      close(dup);
    }

    const char go = 1;
    ASSERT_EQ(WriteAll(release_pipe[1], &go, 1), 0);
    int wstatus = 0;
    ASSERT_EQ(WaitChildBounded(pid, 3000, &wstatus), 0);
    EXPECT_TRUE(WIFEXITED(wstatus));
    EXPECT_EQ(WEXITSTATUS(wstatus), 1);
    EXPECT_EQ(DrainFd(out_pipe[0]), "");
    EXPECT_EQ(
        DrainFd(err_pipe[0]),
        "odin: quic server startup failed at xqc_server_runtime_create\n");

    close(out_pipe[0]);
    close(err_pipe[0]);
    close(report_pipe[0]);
    close(release_pipe[1]);
    close(port_pipe[0]);
  }
}

TEST(OdinCliServerQuicRuntimeTest, T6QuicSetupRuntimeFailureCleanupMatrix) {
  odin_cli_server_test_reset_liveness();
  odin_event_loop_test_reset_liveness();
  odin_xqc_server_runtime_test_reset();

  uint16_t sentinel_port = 0;
  const int sentinel = OpenUdpAny(0, &sentinel_port);
  ASSERT_GE(sentinel, 0);
  odin_xqc_server_runtime_force_destroy(nullptr);
  EXPECT_EQ(odin_xqc_server_runtime_test_record()->call_count, 0u);
  EXPECT_EQ(odin_xqc_server_runtime_test_record()->udp_create_calls, 0u);
  ExpectZeroLiveness(SnapshotLiveness());
  int dup_sentinel = TryBindUdpAny(sentinel_port);
  EXPECT_EQ(dup_sentinel, -1);
  EXPECT_EQ(errno, EADDRINUSE);
  close(sentinel);
  dup_sentinel = TryBindUdpAny(sentinel_port);
  ASSERT_GE(dup_sentinel, 0);
  close(dup_sentinel);

  char err_buf[512] = {0};
  FILE *err = fmemopen(err_buf, sizeof(err_buf), "w");
  ASSERT_NE(err, nullptr);
  EXPECT_EQ(odin_cli_run_server(nullptr, err), 1);
  (void)fclose(err);
  EXPECT_STREQ(err_buf, "odin: server startup failed at config\n");
  ExpectZeroLiveness(SnapshotLiveness());
  EXPECT_EQ(odin_xqc_server_runtime_test_record()->udp_create_calls, 0u);

  odin_cli_server_config_t bad_transport = {
      0,
      static_cast<odin_cli_server_transport_t>(99),
      nullptr,
      nullptr,
  };
  std::memset(err_buf, 0, sizeof(err_buf));
  err = fmemopen(err_buf, sizeof(err_buf), "w");
  ASSERT_NE(err, nullptr);
  EXPECT_EQ(odin_cli_run_server(&bad_transport, err), 1);
  (void)fclose(err);
  EXPECT_STREQ(err_buf, "odin: server startup failed at config\n");
  ExpectZeroLiveness(SnapshotLiveness());
  EXPECT_EQ(odin_xqc_server_runtime_test_record()->udp_create_calls, 0u);

  for (const odin_cli_server_config_t &cfg :
       std::vector<odin_cli_server_config_t>{
           {0, ODIN_CLI_SERVER_TRANSPORT_QUIC, nullptr, kKeyPath},
           {0, ODIN_CLI_SERVER_TRANSPORT_QUIC, kCertPath, nullptr},
           {0, ODIN_CLI_SERVER_TRANSPORT_QUIC, "", kKeyPath},
           {0, ODIN_CLI_SERVER_TRANSPORT_QUIC, kCertPath, ""},
       }) {
    std::memset(err_buf, 0, sizeof(err_buf));
    err = fmemopen(err_buf, sizeof(err_buf), "w");
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(odin_cli_run_server(&cfg, err), 1);
    (void)fclose(err);
    EXPECT_STREQ(err_buf, "odin: quic server startup failed at tls_config\n");
    ExpectZeroLiveness(SnapshotLiveness());
    EXPECT_EQ(odin_xqc_server_runtime_test_record()->udp_create_calls, 0u);
  }

  errno = 0;
  EXPECT_EQ(odin_cli_server_test_fail_next(
                static_cast<odin_cli_server_test_failpoint_t>(0), EIO),
            -1);
  EXPECT_EQ(errno, EINVAL);
  errno = 0;
  EXPECT_EQ(odin_cli_server_test_fail_next(
                ODIN_CLI_SERVER_TEST_FAIL_XQC_SERVER_RUNTIME_CREATE, 0),
            -1);
  EXPECT_EQ(errno, EINVAL);

  struct FailCase {
    odin_cli_server_test_failpoint_t fp;
    const char *line;
  };
  const std::vector<FailCase> cases = {
      {ODIN_CLI_SERVER_TEST_FAIL_EVENT_LOOP_CREATE,
       "odin: quic server startup failed at event_loop_create\n"},
      {ODIN_CLI_SERVER_TEST_FAIL_XQC_SERVER_RUNTIME_CREATE,
       "odin: quic server startup failed at xqc_server_runtime_create\n"},
      {ODIN_CLI_SERVER_TEST_FAIL_XQC_SERVER_RUNTIME_START,
       "odin: quic server startup failed at xqc_server_runtime_start\n"},
      {ODIN_CLI_SERVER_TEST_FAIL_XQC_SERVER_RUNTIME_LOCAL_ADDR,
       "odin: quic server startup failed at xqc_server_runtime_local_addr\n"},
      {ODIN_CLI_SERVER_TEST_FAIL_SIGACTION_SIGINT,
       "odin: quic server startup failed at sigaction(SIGINT)\n"},
      {ODIN_CLI_SERVER_TEST_FAIL_SIGACTION_SIGTERM,
       "odin: quic server startup failed at sigaction(SIGTERM)\n"},
      {ODIN_CLI_SERVER_TEST_FAIL_SIGNAL_TIMER_START,
       "odin: quic server startup failed at signal_timer_start\n"},
      {ODIN_CLI_SERVER_TEST_FAIL_QUIC_EVENT_LOOP_RUN,
       "odin: quic server runtime failed at event_loop_run\n"},
  };
  for (const FailCase &fc : cases) {
    uint16_t port = 0;
    const int tmp = OpenUdpAny(0, &port);
    ASSERT_GE(tmp, 0);
    close(tmp);
    odin_cli_server_test_reset_liveness();
    odin_event_loop_test_reset_liveness();
    odin_xqc_server_runtime_test_reset();
    ASSERT_EQ(odin_cli_server_test_fail_next(fc.fp, EIO), 0);
    MainResult r;
    {
      ScopedCountingSignalHandlers handlers;
      r = RunMain({"odin-server", "--listen", std::to_string(port),
                   "--quic-cert", kCertPath, "--quic-key", kKeyPath});
      EXPECT_EQ(raise(SIGINT), 0);
      EXPECT_EQ(raise(SIGTERM), 0);
      EXPECT_EQ(g_counted_sigint, 1);
      EXPECT_EQ(g_counted_sigterm, 1);
    }
    EXPECT_EQ(r.rc, 1) << static_cast<int>(fc.fp);
    EXPECT_EQ(r.out, "");
    std::string expected_err = fc.line;
    if (fc.fp == ODIN_CLI_SERVER_TEST_FAIL_QUIC_EVENT_LOOP_RUN) {
      expected_err =
          "odin: mode=server transport=quic listen=" + std::to_string(port) +
          "\n" + fc.line;
    }
    EXPECT_EQ(r.err, expected_err)
        << static_cast<int>(fc.fp) << " stderr=" << r.err;
    ExpectZeroLiveness(SnapshotLiveness());
    const int probe = TryBindUdpAny(port);
    ASSERT_GE(probe, 0) << static_cast<int>(fc.fp) << " "
                        << std::strerror(errno);
    close(probe);
  }
}

TEST(OdinCliServerQuicRuntimeTest, T7QuicSignalsAndForceDestroyCleanup) {
  ASSERT_FALSE(g_test_argv0.empty());

  for (int sig : {SIGINT, SIGTERM}) {
    for (T7Variant variant :
         {kT7Baseline, kT7LiveStream, kT7FinalConnClose, kT7FinalServerRefuse,
          kT7GuardServerAccept, kT7GuardConnCreate, kT7GuardUpdateCid,
          kT7InertStreamCreate}) {
      RunT7ChildCase(sig, variant);
    }
  }
}

TEST(OdinCliServerQuicSecurityTest, T8SharedDefaultFilterBeforeServing) {
  odin_cli_server_test_reset_liveness();
  odin_xqc_server_runtime_test_reset();
  ASSERT_EQ(odin_cli_server_test_fail_next(
                ODIN_CLI_SERVER_TEST_FAIL_SIGNAL_TIMER_START, EIO),
            0);
  MainResult quic = RunMain({"odin-server", "--listen", "0", "--quic-cert",
                             kCertPath, "--quic-key", kKeyPath});
  EXPECT_EQ(quic.rc, 1);

  odin_cli_server_test_filter_record_t record{};
  ASSERT_EQ(odin_cli_server_test_filter_record(&record), 0);
  EXPECT_EQ(record.quic_set_count, 1u);
  ASSERT_NE(record.quic_cb, nullptr);
  EXPECT_EQ(record.quic_user_data, nullptr);

  auto MakeIpv4 = [](const char *ip) {
    struct sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(443);
    EXPECT_EQ(inet_pton(AF_INET, ip, &sa.sin_addr), 1);
    return sa;
  };
  const char *const denied[] = {
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
  for (const char *ip : denied) {
    const struct sockaddr_in sa = MakeIpv4(ip);
    EXPECT_EQ(record.quic_cb(reinterpret_cast<const struct sockaddr *>(&sa),
                             sizeof(sa), nullptr),
              EACCES)
        << ip;
  }
  const char *const allowed[] = {
      "1.0.0.0",      "9.255.255.255",   "11.0.0.0",    "100.63.255.255",
      "100.128.0.0",  "126.255.255.255", "128.0.0.0",   "169.253.255.255",
      "169.255.0.0",  "172.15.255.255",  "172.32.0.0",  "191.255.255.255",
      "192.0.1.0",    "192.0.1.255",     "192.0.3.0",   "192.167.255.255",
      "192.169.0.0",  "198.17.255.255",  "198.20.0.0",  "198.51.99.255",
      "198.51.101.0", "203.0.112.255",   "203.0.114.0", "223.255.255.255",
      "8.8.8.8",      "93.184.216.34",
  };
  for (const char *ip : allowed) {
    const struct sockaddr_in sa = MakeIpv4(ip);
    EXPECT_EQ(record.quic_cb(reinterpret_cast<const struct sockaddr *>(&sa),
                             sizeof(sa), nullptr),
              0)
        << ip;
  }
  struct sockaddr_in short_sa = MakeIpv4("8.8.8.8");
  struct sockaddr_in6 sa6;
  std::memset(&sa6, 0, sizeof(sa6));
  sa6.sin6_family = AF_INET6;
  EXPECT_EQ(record.quic_cb(nullptr, sizeof(short_sa), nullptr), EAFNOSUPPORT);
  EXPECT_EQ(record.quic_cb(reinterpret_cast<const struct sockaddr *>(&short_sa),
                           sizeof(short_sa) - 1, nullptr),
            EAFNOSUPPORT);
  EXPECT_EQ(record.quic_cb(reinterpret_cast<const struct sockaddr *>(&sa6),
                           sizeof(sa6), nullptr),
            EAFNOSUPPORT);

  const odin_xqc_server_runtime_test_record_t *rt_record =
      odin_xqc_server_runtime_test_record();
  int alpn_idx = -1;
  int set_idx = -1;
  int udp_start_idx = -1;
  unsigned int alpn_count = 0;
  unsigned int set_dial_filter_count = 0;
  for (unsigned int i = 0; i < rt_record->call_count; ++i) {
    const auto &call = rt_record->calls[i];
    if (call.kind == ODIN_XQC_SERVER_RUNTIME_TEST_CALL_ENGINE_REGISTER_ALPN) {
      ++alpn_count;
      alpn_idx = static_cast<int>(i);
      EXPECT_EQ(call.alpn_len, sizeof(ODIN_XQC_SERVER_ALPN) - 1u);
      EXPECT_EQ(std::string(call.alpn, call.alpn_len), ODIN_XQC_SERVER_ALPN);
    } else if (call.kind == ODIN_XQC_SERVER_RUNTIME_TEST_CALL_SET_DIAL_FILTER) {
      ++set_dial_filter_count;
      set_idx = static_cast<int>(i);
      EXPECT_EQ(call.dial_filter_cb, record.quic_cb);
      EXPECT_EQ(call.user_data, nullptr);
    } else if (call.kind == ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_START &&
               udp_start_idx < 0) {
      udp_start_idx = static_cast<int>(i);
    }
  }
  EXPECT_EQ(alpn_count, 1u);
  EXPECT_EQ(set_dial_filter_count, 1u);
  EXPECT_GE(alpn_idx, 0);
  EXPECT_GE(set_idx, 0);
  EXPECT_GE(udp_start_idx, 0);
  EXPECT_LT(alpn_idx, udp_start_idx);
  EXPECT_LT(set_idx, udp_start_idx);

  odin_xqc_server_runtime_test_reset();
  int sentinel = 1;
  odin_xqc_server_runtime_set_dial_filter(nullptr, record.quic_cb, &sentinel);
  EXPECT_EQ(odin_xqc_server_runtime_test_record()->call_count, 0u);
}

TEST(OdinCliServerQuicSecurityTest, T9QuicConnectReqPolicyStreamPaths) {
  (void)signal(SIGPIPE, SIG_IGN);
  odin_server_session_dial_filter_cb cb = CaptureQuicCliFilter();
  ASSERT_NE(cb, nullptr);

  QuicHarness h;
  InitHarness(&h);
  CreateHarnessRuntime(&h);
  odin_xqc_server_runtime_set_dial_filter(h.rt, cb, nullptr);

  uint16_t upstream_port = 0;
  const int upstream = OpenLoopbackListener(&upstream_port);
  ASSERT_GE(upstream, 0) << std::strerror(errno);
  FakeConn deny_conn;
  AcceptConn(&h, &deny_conn, Cid(0xD1));
  FakeStream deny_stream;
  CreateBidiStream(&h, &deny_conn, &deny_stream);
  QueueRecv(&deny_stream, EncodedReq("127.0.0.1", upstream_port), 0);
  ASSERT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(
                AsStream(&deny_stream), deny_stream.user_data),
            XQC_OK);
  EXPECT_TRUE(DataSends(deny_stream)
                  .rfind(EncodedResp(ODIN_SERVER_SESSION_RESP_CODE_OTHER), 0) ==
              0);
  struct pollfd pfd;
  pfd.fd = upstream;
  pfd.events = POLLIN;
  EXPECT_EQ(poll(&pfd, 1, 200), 0);
  close(upstream);

  int probe_pipe[2];
  ASSERT_EQ(pipe(probe_pipe), 0);
  odin_cli_server_test_set_dial_start_probe_fd(probe_pipe[1], ETIMEDOUT);
  FakeConn public_conn;
  AcceptConn(&h, &public_conn, Cid(0xD2));
  FakeStream public_stream;
  CreateBidiStream(&h, &public_conn, &public_stream);
  QueueRecv(&public_stream, EncodedReq("93.184.216.34", 443), 0);
  ASSERT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(
                AsStream(&public_stream), public_stream.user_data),
            XQC_OK);
  odin_cli_server_test_dial_start_t rec{};
  ASSERT_EQ(ReadWithDeadline(probe_pipe[0], &rec, sizeof(rec), 1000),
            static_cast<ssize_t>(sizeof(rec)));
  EXPECT_EQ(rec.family, AF_INET);
  EXPECT_EQ(rec.ipv4_addr_nbo, htonl(0x5DB8D822u));
  EXPECT_EQ(rec.port_nbo, htons(443));
  EXPECT_TRUE(
      DataSends(public_stream)
          .rfind(EncodedResp(ODIN_SERVER_SESSION_RESP_CODE_ETIMEDOUT), 0) == 0);
  close(probe_pipe[0]);
  close(probe_pipe[1]);
  DestroyHarness(&h);
}

TEST(OdinXqcUdpLocalAddrTest, T10UdpAccessorValidation) {
  QuicHarness h;
  InitHarness(&h);

  odin_xqc_udp_t *xu = nullptr;
  xqc_transport_callbacks_t transport_cbs;
  std::memset(&transport_cbs, 0, sizeof(transport_cbs));
  odin_xqc_udp_config_t cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.loop = h.loop;
  cfg.local_addr = reinterpret_cast<const struct sockaddr *>(&h.local_addr);
  cfg.local_addrlen = sizeof(h.local_addr);
  cfg.engine_type = XQC_ENGINE_SERVER;
  cfg.engine_callbacks = &h.engine_callbacks;
  cfg.transport_callbacks = &transport_cbs;
  ASSERT_EQ(odin_xqc_udp_create(&cfg, &xu), 0) << std::strerror(errno);
  struct sockaddr_storage storage;
  std::memset(&storage, 0xA5, sizeof(storage));
  socklen_t len = sizeof(storage);
  EXPECT_EQ(odin_xqc_udp_local_addr(
                xu, reinterpret_cast<struct sockaddr *>(&storage), &len),
            0);
  EXPECT_EQ(len, static_cast<socklen_t>(sizeof(struct sockaddr_in)));
  const auto *sin = reinterpret_cast<const struct sockaddr_in *>(&storage);
  EXPECT_EQ(sin->sin_family, AF_INET);
  EXPECT_NE(ntohs(sin->sin_port), 0);
  errno = 0;
  EXPECT_EQ(odin_xqc_udp_local_addr(
                nullptr, reinterpret_cast<struct sockaddr *>(&storage), &len),
            -1);
  EXPECT_EQ(errno, EINVAL);
  errno = 0;
  EXPECT_EQ(odin_xqc_udp_local_addr(xu, nullptr, &len), -1);
  EXPECT_EQ(errno, EINVAL);
  errno = 0;
  EXPECT_EQ(odin_xqc_udp_local_addr(
                xu, reinterpret_cast<struct sockaddr *>(&storage), nullptr),
            -1);
  EXPECT_EQ(errno, EINVAL);
  uint8_t short_buf[sizeof(struct sockaddr_in)];
  std::memset(short_buf, 0xCC, sizeof(short_buf));
  len = sizeof(struct sockaddr_in) - 1;
  errno = 0;
  EXPECT_EQ(odin_xqc_udp_local_addr(
                xu, reinterpret_cast<struct sockaddr *>(short_buf), &len),
            -1);
  EXPECT_EQ(errno, ENOBUFS);
  EXPECT_EQ(len, static_cast<socklen_t>(sizeof(struct sockaddr_in)));
  for (unsigned char b : short_buf) {
    EXPECT_EQ(b, 0xCC);
  }
  odin_xqc_udp_destroy(xu);
  ClearHarnessOps();
  odin_event_loop_destroy(h.loop);
}

TEST(OdinXqcServerRuntimeLocalAddrTest, T10RuntimeAccessorValidation) {
  QuicHarness h;
  InitHarness(&h);
  CreateHarnessRuntime(&h);
  struct sockaddr_storage storage;
  std::memset(&storage, 0xA5, sizeof(storage));
  socklen_t len = sizeof(storage);
  EXPECT_EQ(odin_xqc_server_runtime_local_addr(
                h.rt, reinterpret_cast<struct sockaddr *>(&storage), &len),
            0);
  EXPECT_EQ(len, static_cast<socklen_t>(sizeof(struct sockaddr_in)));
  const auto *sin = reinterpret_cast<const struct sockaddr_in *>(&storage);
  EXPECT_EQ(sin->sin_family, AF_INET);
  EXPECT_NE(ntohs(sin->sin_port), 0);
  errno = 0;
  EXPECT_EQ(odin_xqc_server_runtime_local_addr(
                nullptr, reinterpret_cast<struct sockaddr *>(&storage), &len),
            -1);
  EXPECT_EQ(errno, EINVAL);
  errno = 0;
  EXPECT_EQ(odin_xqc_server_runtime_local_addr(h.rt, nullptr, &len), -1);
  EXPECT_EQ(errno, EINVAL);
  errno = 0;
  EXPECT_EQ(odin_xqc_server_runtime_local_addr(
                h.rt, reinterpret_cast<struct sockaddr *>(&storage), nullptr),
            -1);
  EXPECT_EQ(errno, EINVAL);
  uint8_t short_buf[sizeof(struct sockaddr_in)];
  std::memset(short_buf, 0xCC, sizeof(short_buf));
  len = sizeof(struct sockaddr_in) - 1;
  errno = 0;
  EXPECT_EQ(odin_xqc_server_runtime_local_addr(
                h.rt, reinterpret_cast<struct sockaddr *>(short_buf), &len),
            -1);
  EXPECT_EQ(errno, ENOBUFS);
  EXPECT_EQ(len, static_cast<socklen_t>(sizeof(struct sockaddr_in)));
  for (unsigned char b : short_buf) {
    EXPECT_EQ(b, 0xCC);
  }
  DestroyHarness(&h);
}

// NOLINTEND(misc-const-correctness)
