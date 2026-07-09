// odin/testing/server_xqc_runtime_unittests.cpp

#include "odin/server_xqc_runtime.h"

#include <algorithm>
#include <ares.h>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "odin/event_loop.h"
#include "odin/protocol.h"
#include "odin/testing/dns_resolver_internal_test.h"
#include "odin/transport.h"
#if defined(ODIN_CONNECT_SESSION_TESTING)
#include "odin/testing/connect_session_internal_test.h"
#endif
#if defined(ODIN_SERVER_SESSION_TESTING)
#include "odin/testing/server_session_internal_test.h"
#endif
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
#include "odin/testing/server_xqc_runtime_internal_test.h"
#endif
#if defined(ODIN_TRANSPORT_XQC_TESTING)
#include "odin/testing/transport_xqc_internal_test.h"
#endif
#if defined(ODIN_XQC_UDP_TESTING)
#include "odin/testing/xqc_udp_internal_test.h"
#endif

#include "gtest/gtest.h"

// NOLINTBEGIN(misc-const-correctness, misc-use-internal-linkage,
// performance-no-int-to-ptr)

namespace {

struct FakeFdTransport {
  odin_transport_t base;
  int destroy_calls = 0;
  int set_interest_calls = 0;
  int set_interest_errno = 0;
  unsigned int last_interest = 0;
};

odin_transport_io_t FakeTransportRead(odin_transport_t *, void *, size_t,
                                      size_t *) {
  return ODIN_TRANSPORT_AGAIN;
}

odin_transport_io_t FakeTransportWrite(odin_transport_t *, const void *, size_t,
                                       size_t *) {
  return ODIN_TRANSPORT_AGAIN;
}

int FakeTransportShutdownWrite(odin_transport_t *) { return 0; }

int FakeTransportSetInterest(odin_transport_t *t, unsigned int events) {
  FakeFdTransport *ft = reinterpret_cast<FakeFdTransport *>(t);
  ft->set_interest_calls += 1;
  ft->last_interest = events;
  if (ft->set_interest_errno != 0) {
    errno = ft->set_interest_errno;
    return -1;
  }
  return 0;
}

int FakeTransportError(odin_transport_t *) { return 0; }

void FakeTransportDestroy(odin_transport_t *t) {
  reinterpret_cast<FakeFdTransport *>(t)->destroy_calls += 1;
}

const odin_transport_vtable_t kFakeTransportVtable = {
    FakeTransportRead,        FakeTransportWrite, FakeTransportShutdownWrite,
    FakeTransportSetInterest, FakeTransportError, FakeTransportDestroy,
};

struct FactoryState {
  FakeFdTransport *transport = nullptr;
  int calls = 0;
  int fail_errno = 0;
};

int FakeFactory(odin_transport_ready_cb, void *, void *factory_user_data,
                odin_transport_t **out) {
  FactoryState *state = static_cast<FactoryState *>(factory_user_data);
  state->calls += 1;
  if (state->fail_errno != 0) {
    errno = state->fail_errno;
    return -1;
  }
  state->transport->base.vt = &kFakeTransportVtable;
  *out = &state->transport->base;
  return 0;
}

void ServerSessionClose(odin_server_session_t *, int, void *) {}

struct sockaddr_in Loopback4(uint16_t port) {
  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  return addr;
}

xqc_cid_t Cid(uint8_t b) {
  xqc_cid_t cid;
  std::memset(&cid, 0, sizeof(cid));
  cid.cid_len = 1;
  cid.cid_buf[0] = b;
  return cid;
}

bool CidEq(const xqc_cid_t &a, const xqc_cid_t &b) {
  return a.cid_len == b.cid_len &&
         std::memcmp(a.cid_buf, b.cid_buf, a.cid_len) == 0;
}

std::string EncodedReq(const std::string &host, uint16_t port) {
  odin_proto_iov_t iov[3];
  uint8_t hdr[3];
  uint8_t portbe[2];
  EXPECT_EQ(odin_proto_encode_connect_req(host.data(), host.size(), port, iov,
                                          hdr, portbe),
            ODIN_PROTO_OK);
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

struct RecvStep {
  ssize_t ret = 0;
  uint8_t fin = 0;
  std::string data;
};

struct SendStep {
  ssize_t ret = 0;
};

struct SendRecord {
  std::string data;
  size_t size = 0;
  uint8_t fin = 0;
  bool null_data = false;
};

struct FakeConn {
  void *transport_user_data = nullptr;
  void *alp_user_data = nullptr;
};

struct FakeStream {
  FakeConn *conn = nullptr;
  xqc_stream_direction_t direction = XQC_STREAM_BIDI;
  void *user_data = nullptr;
  std::deque<RecvStep> recv_steps;
  std::deque<SendStep> send_steps;
  std::vector<SendRecord> sends;
  std::vector<void *> user_data_values;
  bool *send_must_be_inside_flag = nullptr;
  std::string send_must_be_inside_bytes;
  int send_must_be_inside_checks = 0;
  int close_calls = 0;
};

xqc_connection_t *AsConn(FakeConn *conn) {
  return reinterpret_cast<xqc_connection_t *>(conn);
}

FakeConn *FromConn(xqc_connection_t *conn) {
  return reinterpret_cast<FakeConn *>(conn);
}

xqc_stream_t *AsStream(FakeStream *stream) {
  return reinterpret_cast<xqc_stream_t *>(stream);
}

FakeStream *FromStream(xqc_stream_t *stream) {
  return reinterpret_cast<FakeStream *>(stream);
}

void QueueRecv(FakeStream *stream, const std::string &data, ssize_t ret,
               uint8_t fin) {
  stream->recv_steps.push_back(RecvStep{ret, fin, data});
}

void QueueSend(FakeStream *stream, ssize_t ret) {
  stream->send_steps.push_back(SendStep{ret});
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
  const size_t copy =
      std::min(step.data.size(), static_cast<size_t>(recv_buf_size));
  if (copy != 0) {
    std::copy_n(reinterpret_cast<const unsigned char *>(step.data.data()), copy,
                recv_buf);
  }
  return step.ret;
}

ssize_t FakeSend(xqc_stream_t *stream, unsigned char *send_data,
                 size_t send_data_size, uint8_t fin) {
  FakeStream *fake = FromStream(stream);
  ssize_t ret = static_cast<ssize_t>(send_data_size);
  if (!fake->send_steps.empty()) {
    const SendStep step = fake->send_steps.front();
    fake->send_steps.pop_front();
    ret = step.ret;
  }
  if (fake->send_must_be_inside_flag != nullptr && send_data != nullptr &&
      send_data_size == fake->send_must_be_inside_bytes.size() &&
      std::memcmp(send_data, fake->send_must_be_inside_bytes.data(),
                  send_data_size) == 0) {
    EXPECT_TRUE(*fake->send_must_be_inside_flag);
    fake->send_must_be_inside_checks += 1;
  }
  SendRecord rec;
  rec.size = ret > 0 ? static_cast<size_t>(ret) : 0;
  rec.fin = fin;
  rec.null_data = send_data == nullptr;
  if (send_data != nullptr && rec.size != 0) {
    rec.data.assign(reinterpret_cast<const char *>(send_data), rec.size);
  }
  fake->sends.push_back(rec);
  return ret;
}

void FakeSetStreamUserData(xqc_stream_t *stream, void *user_data) {
  FakeStream *fake = FromStream(stream);
  fake->user_data = user_data;
  fake->user_data_values.push_back(user_data);
}

std::string DataSends(const FakeStream &stream) {
  std::string out;
  for (const SendRecord &rec : stream.sends) {
    if (!rec.null_data && rec.fin == 0 && rec.size != 0) {
      out.append(rec.data);
    }
  }
  return out;
}

bool HasFinSend(const FakeStream &stream) {
  for (const SendRecord &rec : stream.sends) {
    if (rec.null_data && rec.fin == 1) {
      return true;
    }
  }
  return false;
}

bool StartsWith(const std::string &s, const std::string &prefix) {
  return s.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), s.begin());
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
    const int saved = errno;
    close(lfd);
    errno = saved;
    return -1;
  }
  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(lfd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) !=
      0) {
    const int saved = errno;
    close(lfd);
    errno = saved;
    return -1;
  }
  if (listen(lfd, 16) != 0) {
    const int saved = errno;
    close(lfd);
    errno = saved;
    return -1;
  }
  socklen_t alen = sizeof(addr);
  if (getsockname(lfd, reinterpret_cast<struct sockaddr *>(&addr), &alen) !=
      0) {
    const int saved = errno;
    close(lfd);
    errno = saved;
    return -1;
  }
  *out_port = ntohs(addr.sin_port);
  return lfd;
}

bool WriteAllFd(int fd, const void *buf, size_t len, int deadline_ms) {
  const uint8_t *p = static_cast<const uint8_t *>(buf);
  size_t off = 0;
  const auto start = std::chrono::steady_clock::now();
  while (off < len) {
    const auto now = std::chrono::steady_clock::now();
    const int elapsed = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
            .count());
    if (elapsed >= deadline_ms) {
      return false;
    }
    const ssize_t n = write(fd, p + off, len - off);
    if (n > 0) {
      off += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      struct pollfd pfd;
      pfd.fd = fd;
      pfd.events = POLLOUT;
      (void)poll(&pfd, 1, deadline_ms - elapsed);
      continue;
    }
    return false;
  }
  return true;
}

int AcceptWithDeadline(int lfd, int deadline_ms, int *out_errno) {
  const auto start = std::chrono::steady_clock::now();
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    const int elapsed = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
            .count());
    if (elapsed >= deadline_ms) {
      *out_errno = ETIMEDOUT;
      return -1;
    }
    struct pollfd pfd;
    pfd.fd = lfd;
    pfd.events = POLLIN;
    const int prc = poll(&pfd, 1, deadline_ms - elapsed);
    if (prc == 0) {
      *out_errno = ETIMEDOUT;
      return -1;
    }
    if (prc < 0) {
      if (errno == EINTR) {
        continue;
      }
      *out_errno = errno;
      return -1;
    }
    const int fd = accept(lfd, nullptr, nullptr);
    if (fd >= 0) {
      return fd;
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
      continue;
    }
    *out_errno = errno;
    return -1;
  }
}

size_t ReadExactlyFd(int fd, void *buf, size_t len, int deadline_ms) {
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

void DrainUntilEofFd(int fd, std::string *out, bool *saw_eof, int deadline_ms) {
  *saw_eof = false;
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
      *saw_eof = true;
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

void ExpectNoAccept(int lfd) {
  struct pollfd pfd;
  pfd.fd = lfd;
  pfd.events = POLLIN;
  const int prc = poll(&pfd, 1, 120);
  ASSERT_GE(prc, 0) << std::strerror(errno);
  if (prc == 0) {
    return;
  }
  const int fd = accept(lfd, nullptr, nullptr);
  if (fd >= 0) {
    close(fd);
  }
  FAIL() << "unexpected upstream accept";
}

struct PumpCtx {
  std::function<bool()> *predicate = nullptr;
  odin_event_timer_t *poll_timer = nullptr;
  odin_event_timer_t *watchdog = nullptr;
  bool reached = false;
  bool timed_out = false;
};

void StopPumpTimers(PumpCtx *ctx) {
  if (ctx->poll_timer != nullptr) {
    odin_event_timer_stop(ctx->poll_timer);
    ctx->poll_timer = nullptr;
  }
  if (ctx->watchdog != nullptr) {
    odin_event_timer_stop(ctx->watchdog);
    ctx->watchdog = nullptr;
  }
}

void PumpPollCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                void *user_data) {
  (void)timer;
  PumpCtx *ctx = static_cast<PumpCtx *>(user_data);
  if ((*ctx->predicate)()) {
    ctx->reached = true;
    StopPumpTimers(ctx);
    odin_event_loop_stop(loop);
  }
}

void PumpWatchdogCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                    void *user_data) {
  (void)timer;
  PumpCtx *ctx = static_cast<PumpCtx *>(user_data);
  ctx->timed_out = true;
  StopPumpTimers(ctx);
  odin_event_loop_stop(loop);
}

void RunUntil(odin_event_loop_t *loop, std::function<bool()> predicate) {
  if (predicate()) {
    return;
  }
  PumpCtx ctx;
  ctx.predicate = &predicate;
  ASSERT_EQ(odin_event_timer_start(loop, 5000, 5000, PumpPollCb, &ctx,
                                   &ctx.poll_timer),
            0)
      << std::strerror(errno);
  ASSERT_EQ(odin_event_timer_start(loop, 1500000, 0, PumpWatchdogCb, &ctx,
                                   &ctx.watchdog),
            0)
      << std::strerror(errno);
  ASSERT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);
  EXPECT_TRUE(ctx.reached);
  EXPECT_FALSE(ctx.timed_out);
}

struct UpstreamPeer {
  int lfd = -1;
  std::thread thread;
  std::atomic<bool> accepted{false};
  std::atomic<bool> tail_read{false};
  std::atomic<bool> eof_seen{false};
  std::atomic<bool> reply_written{false};
  std::atomic<bool> done{false};
  std::string tail;
  std::string extra_before_eof;
  int accept_errno = 0;

  void Start() {
    thread = std::thread([this] {
      int err = 0;
      const int fd = AcceptWithDeadline(lfd, 1500, &err);
      if (fd < 0) {
        accept_errno = err;
        done.store(true);
        return;
      }
      accepted.store(true);
      char tail_buf[4];
      const size_t n = ReadExactlyFd(fd, tail_buf, sizeof(tail_buf), 1500);
      tail.assign(tail_buf, n);
      if (n == sizeof(tail_buf)) {
        tail_read.store(true);
      }
      bool saw_eof = false;
      DrainUntilEofFd(fd, &extra_before_eof, &saw_eof, 1500);
      eof_seen.store(saw_eof);
      if (WriteAllFd(fd, "reply", 5, 1500)) {
        reply_written.store(true);
      }
      (void)shutdown(fd, SHUT_WR);
      std::string scratch;
      bool downstream_eof = false;
      DrainUntilEofFd(fd, &scratch, &downstream_eof, 500);
      close(fd);
      done.store(true);
    });
  }

  void Join() {
    if (thread.joinable()) {
      thread.join();
    }
  }
};

struct RuntimeHarness {
  odin_event_loop_t *loop = nullptr;
  odin_xqc_server_runtime_t *rt = nullptr;
  char engine_storage = 0;
  xqc_engine_t *engine = reinterpret_cast<xqc_engine_t *>(&engine_storage);
  void *xu_user_data = nullptr;
  void *alpn_user_data = nullptr;
  xqc_app_proto_callbacks_t *app_callbacks = nullptr;
  xqc_transport_callbacks_t transport_callbacks;
  struct sockaddr_in local_addr;
  xqc_config_t engine_config;
  xqc_engine_ssl_config_t ssl_config;
  xqc_engine_callback_t engine_callbacks;
  int engine_create_calls = 0;
  int engine_destroy_calls = 0;
  bool engine_create_returns_null = false;
  int engine_create_errno = EIO;
  bool alpn_register_fails = false;
  uint8_t fail_register_cid = 0;
  std::vector<xqc_cid_t> fake_registered;
  std::vector<xqc_cid_t> fake_unregistered;
  std::vector<xqc_cid_t> conn_close_cids;
  int alpn_unregister_calls = 0;
  int start_calls_before = 0;
  int stop_calls_before = 0;
};

RuntimeHarness *g_harness = nullptr;

xqc_engine_t *FakeEngineCreate(xqc_engine_type_t engine_type,
                               const xqc_config_t *engine_config,
                               const xqc_engine_ssl_config_t *ssl_config,
                               const xqc_engine_callback_t *engine_callback,
                               const xqc_transport_callbacks_t *transport_cbs,
                               void *user_data) {
  (void)engine_type;
  (void)engine_config;
  (void)ssl_config;
  (void)engine_callback;
  if (g_harness == nullptr) {
    return nullptr;
  }
  g_harness->engine_create_calls += 1;
  g_harness->xu_user_data = user_data;
  g_harness->transport_callbacks = *transport_cbs;
  if (g_harness->engine_create_returns_null) {
    errno = g_harness->engine_create_errno;
    return nullptr;
  }
  return g_harness->engine;
}

void FakeEngineDestroy(xqc_engine_t *engine) {
  if (g_harness != nullptr && engine == g_harness->engine) {
    g_harness->engine_destroy_calls += 1;
  }
}

xqc_int_t FakeEngineRegisterAlpn(xqc_engine_t *engine, const char *alpn,
                                 size_t alpn_len,
                                 xqc_app_proto_callbacks_t *app_callbacks,
                                 void *user_data) {
  if (g_harness == nullptr || engine != g_harness->engine) {
    return -XQC_EPARAM;
  }
  if (g_harness->alpn_register_fails) {
    return -XQC_EPARAM;
  }
  EXPECT_EQ(std::string(alpn, alpn_len), ODIN_XQC_SERVER_ALPN);
  g_harness->app_callbacks = app_callbacks;
  g_harness->alpn_user_data = user_data;
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
  FromConn(conn)->transport_user_data = user_data;
}

void FakeConnSetAlpUserData(xqc_connection_t *conn, void *user_data) {
  FromConn(conn)->alp_user_data = user_data;
}

xqc_int_t FakeConnClose(xqc_engine_t *engine, const xqc_cid_t *cid) {
  EXPECT_EQ(engine, g_harness->engine);
  if (cid != nullptr) {
    g_harness->conn_close_cids.push_back(*cid);
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
  if (g_harness != nullptr && cid != nullptr && cid->cid_len == 1 &&
      cid->cid_buf[0] == g_harness->fail_register_cid) {
    errno = ENOMEM;
    return -1;
  }
  if (g_harness != nullptr && cid != nullptr) {
    g_harness->fake_registered.push_back(*cid);
  }
  return 0;
}

void FakeUdpUnregisterConn(odin_xqc_udp_t *, const xqc_cid_t *cid) {
  if (g_harness != nullptr && cid != nullptr) {
    g_harness->fake_unregistered.push_back(*cid);
  }
}

void InstallTransportOps() {
#if defined(ODIN_TRANSPORT_XQC_TESTING)
  static const odin_xqc_stream_transport_test_ops_t kOps = {
      FakeRecv,
      FakeSend,
      FakeSetStreamUserData,
  };
  odin_xqc_stream_transport_test_set_ops(&kOps);
#endif
}

void InstallRuntimeOps(RuntimeHarness *h) {
  g_harness = h;
#if defined(ODIN_XQC_UDP_TESTING)
  static const odin_xqc_udp_test_ops_t kUdpOps = {
      FakeEngineCreate, FakeEngineDestroy, nullptr, nullptr,
      nullptr,          nullptr,           nullptr,
  };
  odin_xqc_udp_test_set_ops(&kUdpOps);
#endif
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
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
#endif
  InstallTransportOps();
}

void ClearOps() {
#if defined(ODIN_TRANSPORT_XQC_TESTING)
  odin_xqc_stream_transport_test_set_ops(nullptr);
#endif
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  odin_xqc_server_runtime_test_set_ops(nullptr);
#endif
#if defined(ODIN_XQC_UDP_TESTING)
  odin_xqc_udp_test_set_ops(nullptr);
#endif
  g_harness = nullptr;
}

void InitHarness(RuntimeHarness *h) {
  std::memset(&h->transport_callbacks, 0, sizeof(h->transport_callbacks));
  std::memset(&h->engine_config, 0, sizeof(h->engine_config));
  std::memset(&h->ssl_config, 0, sizeof(h->ssl_config));
  std::memset(&h->engine_callbacks, 0, sizeof(h->engine_callbacks));
  h->local_addr = Loopback4(0);
  ASSERT_EQ(odin_event_loop_create(&h->loop), 0) << std::strerror(errno);
  InstallRuntimeOps(h);
}

odin_xqc_server_runtime_config_t MakeRuntimeConfig(RuntimeHarness *h) {
  odin_xqc_server_runtime_config_t config;
  config.loop = h->loop;
  config.local_addr = reinterpret_cast<const struct sockaddr *>(&h->local_addr);
  config.local_addrlen = sizeof(h->local_addr);
  config.engine_config = &h->engine_config;
  config.ssl_config = &h->ssl_config;
  config.engine_callbacks = &h->engine_callbacks;
  return config;
}

void CreateRuntime(RuntimeHarness *h) {
  const odin_xqc_server_runtime_config_t config = MakeRuntimeConfig(h);
  ASSERT_EQ(odin_xqc_server_runtime_create(&config, &h->rt), 0)
      << std::strerror(errno);
  ASSERT_NE(h->rt, nullptr);
  ASSERT_NE(h->xu_user_data, nullptr);
  ASSERT_NE(h->app_callbacks, nullptr);
}

void DestroyHarness(RuntimeHarness *h) {
  if (h->rt != nullptr) {
    odin_xqc_server_runtime_destroy(h->rt);
    h->rt = nullptr;
  }
  ClearOps();
  odin_event_loop_destroy(h->loop);
  h->loop = nullptr;
}

void AcceptConn(RuntimeHarness *h, FakeConn *conn, const xqc_cid_t &cid) {
  ASSERT_EQ(h->transport_callbacks.server_accept(h->engine, AsConn(conn), &cid,
                                                 h->xu_user_data),
            0);
  ASSERT_NE(conn->alp_user_data, nullptr);
  ASSERT_EQ(conn->transport_user_data, h->xu_user_data);
  ASSERT_EQ(h->app_callbacks->conn_cbs.conn_create_notify(
                AsConn(conn), &cid, h->xu_user_data, conn->alp_user_data),
            0);
}

void CloseConn(RuntimeHarness *h, FakeConn *conn, const xqc_cid_t &cid) {
  ASSERT_EQ(h->app_callbacks->conn_cbs.conn_close_notify(
                AsConn(conn), &cid, h->xu_user_data, conn->alp_user_data),
            0);
  conn->alp_user_data = nullptr;
}

void CreateBidiStream(RuntimeHarness *h, FakeConn *conn, FakeStream *stream) {
  stream->conn = conn;
  stream->direction = XQC_STREAM_BIDI;
  ASSERT_EQ(h->app_callbacks->stream_cbs.stream_create_notify(
                AsStream(stream), stream->user_data),
            XQC_OK);
  ASSERT_NE(stream->user_data, nullptr);
}

unsigned int CountCalls(odin_xqc_server_runtime_test_call_kind_t kind) {
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  const odin_xqc_server_runtime_test_record_t *record =
      odin_xqc_server_runtime_test_record();
  unsigned int count = 0;
  for (unsigned int i = 0; i < record->call_count; ++i) {
    if (record->calls[i].kind == kind) {
      count += 1;
    }
  }
  return count;
#else
  (void)kind;
  return 0;
#endif
}

int FirstCallIndex(odin_xqc_server_runtime_test_call_kind_t kind,
                   const xqc_cid_t &cid) {
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  const odin_xqc_server_runtime_test_record_t *record =
      odin_xqc_server_runtime_test_record();
  for (unsigned int i = 0; i < record->call_count; ++i) {
    if (record->calls[i].kind == kind && CidEq(record->calls[i].cid, cid)) {
      return static_cast<int>(i);
    }
  }
#else
  (void)kind;
  (void)cid;
#endif
  return -1;
}

unsigned int CountCallsForStream(odin_xqc_server_runtime_test_call_kind_t kind,
                                 FakeStream *stream) {
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  const odin_xqc_server_runtime_test_record_t *record =
      odin_xqc_server_runtime_test_record();
  unsigned int count = 0;
  for (unsigned int i = 0; i < record->call_count; ++i) {
    if (record->calls[i].kind == kind &&
        record->calls[i].stream == AsStream(stream)) {
      count += 1;
    }
  }
  return count;
#else
  (void)kind;
  (void)stream;
  return 0;
#endif
}

struct FilterState {
  int calls = 0;
  int err = 0;
  odin_xqc_server_runtime_t *destroy_rt = nullptr;
  bool expect_addr = false;
  uint16_t expect_port = 0;
  bool saw_expected_addr = false;
  const void *expected_user_data = nullptr;
  bool check_user_data = false;
  RuntimeHarness *h = nullptr;
};

int TestDialFilter(const struct sockaddr *addr, socklen_t addrlen,
                   void *user_data) {
  FilterState *state = static_cast<FilterState *>(user_data);
  state->calls += 1;
  if (state->check_user_data) {
    EXPECT_EQ(user_data, state->expected_user_data);
  }
  if (state->expect_addr) {
    EXPECT_NE(addr, nullptr);
    if (addr != nullptr) {
      EXPECT_EQ(addrlen, static_cast<socklen_t>(sizeof(struct sockaddr_in)));
      EXPECT_EQ(addr->sa_family, AF_INET);
      const struct sockaddr_in *sin =
          reinterpret_cast<const struct sockaddr_in *>(addr);
      EXPECT_EQ(sin->sin_addr.s_addr, htonl(INADDR_LOOPBACK));
      EXPECT_EQ(ntohs(sin->sin_port), state->expect_port);
      if (sin->sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
          ntohs(sin->sin_port) == state->expect_port) {
        state->saw_expected_addr = true;
      }
    }
  }
  if (state->destroy_rt != nullptr) {
    if (state->h != nullptr) {
      EXPECT_TRUE(state->h->conn_close_cids.empty());
      EXPECT_EQ(state->h->alpn_unregister_calls, 0);
      EXPECT_EQ(state->h->engine_destroy_calls, 0);
    }
    odin_xqc_server_runtime_destroy(state->destroy_rt);
  }
  return state->err;
}

void QueueCompleteReq(FakeStream *stream, uint16_t port,
                      const std::string &tail, uint8_t fin) {
  const std::string req = EncodedReq("127.0.0.1", port) + tail;
  QueueRecv(stream, req, static_cast<ssize_t>(req.size()), fin);
}

void RunValidStream(RuntimeHarness *h, FakeConn *conn, FakeStream *stream,
                    bool short_resp = false) {
  uint16_t port = 0;
  const int lfd = OpenLoopbackListener(&port);
  ASSERT_GE(lfd, 0) << std::strerror(errno);
  UpstreamPeer peer;
  peer.lfd = lfd;
  peer.Start();

  CreateBidiStream(h, conn, stream);
  QueueCompleteReq(stream, port, "tail", 1);
  if (short_resp) {
    QueueSend(stream, 2);
    QueueSend(stream, -XQC_EAGAIN);
  }
  ASSERT_EQ(h->app_callbacks->stream_cbs.stream_read_notify(AsStream(stream),
                                                            stream->user_data),
            XQC_OK);

  RunUntil(h->loop, [&] { return peer.accepted.load(); });

  const std::string ok_resp = EncodedResp(ODIN_SERVER_SESSION_RESP_CODE_OK);
  if (short_resp) {
    RunUntil(h->loop, [&] { return DataSends(*stream) == "\x01\x02"; });
    EXPECT_FALSE(peer.tail_read.load());
    QueueSend(stream, 2);
    ASSERT_EQ(h->app_callbacks->stream_cbs.stream_write_notify(
                  AsStream(stream), stream->user_data),
              XQC_OK);
  }

  RunUntil(h->loop, [&] { return StartsWith(DataSends(*stream), ok_resp); });
  RunUntil(h->loop, [&] { return peer.tail_read.load(); });
  RunUntil(h->loop, [&] { return peer.eof_seen.load(); });
  RunUntil(h->loop, [&] {
    return DataSends(*stream).find("reply") != std::string::npos;
  });
  RunUntil(h->loop, [&] { return stream->user_data == nullptr; });

  peer.Join();
  EXPECT_EQ(peer.accept_errno, 0);
  EXPECT_TRUE(peer.accepted.load());
  EXPECT_TRUE(peer.tail_read.load());
  EXPECT_TRUE(peer.eof_seen.load());
  EXPECT_TRUE(peer.reply_written.load());
  EXPECT_TRUE(peer.done.load());
  EXPECT_EQ(peer.tail, "tail");
  EXPECT_EQ(peer.extra_before_eof, "");
  EXPECT_TRUE(StartsWith(DataSends(*stream), ok_resp));
  EXPECT_NE(DataSends(*stream).find("reply"), std::string::npos);
  EXPECT_TRUE(HasFinSend(*stream));
  EXPECT_EQ(stream->close_calls, 0);
  EXPECT_EQ(close(lfd), 0);
}

void FinishStagedValidStream(RuntimeHarness *h, FakeStream *stream,
                             UpstreamPeer *peer, const std::string &req) {
  QueueRecv(stream, req.substr(4) + "tail",
            static_cast<ssize_t>(req.size() - 4 + 4), 1);
  ASSERT_EQ(h->app_callbacks->stream_cbs.stream_read_notify(AsStream(stream),
                                                            stream->user_data),
            XQC_OK);
  RunUntil(h->loop, [&] { return peer->accepted.load(); });
  const std::string ok_resp = EncodedResp(ODIN_SERVER_SESSION_RESP_CODE_OK);
  RunUntil(h->loop, [&] { return StartsWith(DataSends(*stream), ok_resp); });
  RunUntil(h->loop, [&] { return peer->tail_read.load(); });
  RunUntil(h->loop, [&] { return peer->eof_seen.load(); });
  RunUntil(h->loop, [&] {
    return DataSends(*stream).find("reply") != std::string::npos;
  });
  RunUntil(h->loop, [&] { return stream->user_data == nullptr; });
}

void RunDeniedStream(RuntimeHarness *h, FakeStream *stream, uint16_t port,
                     uint16_t resp_code) {
  QueueCompleteReq(stream, port, "", 0);
  ASSERT_EQ(h->app_callbacks->stream_cbs.stream_read_notify(AsStream(stream),
                                                            stream->user_data),
            XQC_OK);
  const std::string resp = EncodedResp(resp_code);
  RunUntil(h->loop, [&] {
    return StartsWith(DataSends(*stream), resp) && stream->user_data == nullptr;
  });
  EXPECT_EQ(DataSends(*stream), EncodedResp(resp_code));
  EXPECT_EQ(stream->user_data, nullptr);
}

} // namespace

namespace {

class RuntimeDnsRunDeadline {
public:
  template <typename Fn> static void Run(Fn fn) {
    const pid_t pid = fork();
    ASSERT_NE(pid, -1) << std::strerror(errno);
    if (pid == 0) {
      (void)odin_dns_resolver_test_reset_liveness();
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
      FAIL() << "RuntimeDnsRunDeadline exceeded 2 seconds";
    }
    ASSERT_TRUE(WIFEXITED(wstatus));
    EXPECT_EQ(WEXITSTATUS(wstatus), 0);
  }
};

void ExpectRuntimeDnsLiveBaseline(
    const odin_dns_resolver_test_liveness_t &base) {
  odin_dns_resolver_test_liveness_t post;
  ASSERT_EQ(odin_dns_resolver_test_liveness(&post), 0);
  EXPECT_EQ(post.resolvers, base.resolvers);
  EXPECT_EQ(post.queries, base.queries);
  EXPECT_EQ(post.watches, base.watches);
  EXPECT_EQ(post.timers, base.timers);
  EXPECT_EQ(post.cares_channels, base.cares_channels);
  EXPECT_EQ(post.cares_results, base.cares_results);
}

} // namespace

TEST(OdinXqcServerRuntimeDnsTest, T10RuntimeReusesOneResolver) {
  RuntimeDnsRunDeadline::Run([] {
    RuntimeHarness h;
    InitHarness(&h);
    odin_dns_resolver_test_liveness_t base;
    ASSERT_EQ(odin_dns_resolver_test_liveness(&base), 0);
    CreateRuntime(&h);
    odin_dns_resolver_test_liveness_t after_rt;
    ASSERT_EQ(odin_dns_resolver_test_liveness(&after_rt), 0);
    EXPECT_EQ(after_rt.resolvers, base.resolvers + 1);
    EXPECT_EQ(after_rt.resolver_create_calls, base.resolver_create_calls + 1);

    FakeConn conn;
    const xqc_cid_t cid = Cid(0xD1);
    AcceptConn(&h, &conn, cid);
#if defined(ODIN_SERVER_SESSION_TESTING)
    const unsigned int session_base = odin_server_session_test_live_count();
#endif
    FakeStream stream;
    CreateBidiStream(&h, &conn, &stream);
#if defined(ODIN_SERVER_SESSION_TESTING)
    EXPECT_EQ(odin_server_session_test_live_count(), session_base + 1);
#endif
    odin_dns_resolver_test_liveness_t after_stream;
    ASSERT_EQ(odin_dns_resolver_test_liveness(&after_stream), 0);
    EXPECT_EQ(after_stream.resolvers, after_rt.resolvers);

    odin_xqc_server_runtime_destroy(h.rt);
    EXPECT_EQ(stream.user_data, nullptr);
#if defined(ODIN_SERVER_SESSION_TESTING)
    EXPECT_EQ(odin_server_session_test_live_count(), session_base);
#endif
    CloseConn(&h, &conn, cid);
    h.rt = nullptr;
    odin_dns_resolver_test_liveness_t post;
    ASSERT_EQ(odin_dns_resolver_test_liveness(&post), 0);
    EXPECT_EQ(post.resolver_destroy_calls, base.resolver_destroy_calls + 1);
    ExpectRuntimeDnsLiveBaseline(base);
    ClearOps();
    odin_event_loop_destroy(h.loop);
  });
}

TEST(OdinXqcServerRuntimeDnsTest, T15RuntimeResolverRollback) {
  for (int subcase = 0; subcase < 3; ++subcase) {
    RuntimeDnsRunDeadline::Run([subcase] {
      RuntimeHarness h;
      InitHarness(&h);
      odin_dns_resolver_test_liveness_t base;
      ASSERT_EQ(odin_dns_resolver_test_liveness(&base), 0);
      if (subcase == 0) {
        odin_dns_resolver_test_cares_step_t step = {};
        step.op = ODIN_DNS_TEST_CARES_LIBRARY_INIT;
        step.status = ARES_ENOMEM;
        ASSERT_EQ(odin_dns_resolver_test_push_cares_step(&step), 0);
      } else if (subcase == 1) {
        h.engine_create_returns_null = true;
        h.engine_create_errno = EIO;
      } else {
        h.alpn_register_fails = true;
      }
      odin_xqc_server_runtime_t *out =
          reinterpret_cast<odin_xqc_server_runtime_t *>(0xDEADBEEF);
      const odin_xqc_server_runtime_config_t config = MakeRuntimeConfig(&h);
      EXPECT_EQ(odin_xqc_server_runtime_create(&config, &out), -1);
      EXPECT_EQ(out, reinterpret_cast<odin_xqc_server_runtime_t *>(0xDEADBEEF));
      if (subcase == 0) {
        EXPECT_EQ(errno, ENOMEM);
        odin_dns_resolver_test_cares_observation_t obs;
        ASSERT_EQ(odin_dns_resolver_test_cares_observation(&obs), 0);
        EXPECT_EQ(obs.ares_library_init_calls, 1u);
        EXPECT_EQ(CountCalls(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_CREATE), 0u);
        odin_dns_resolver_test_liveness_t post;
        ASSERT_EQ(odin_dns_resolver_test_liveness(&post), 0);
        EXPECT_EQ(post.resolver_create_calls, base.resolver_create_calls);
        EXPECT_EQ(post.resolver_destroy_calls, base.resolver_destroy_calls);
      } else {
        if (subcase == 1) {
          EXPECT_EQ(errno, EIO);
          EXPECT_EQ(CountCalls(
                        ODIN_XQC_SERVER_RUNTIME_TEST_CALL_ENGINE_REGISTER_ALPN),
                    0u);
        } else {
          EXPECT_EQ(errno, EIO);
          EXPECT_EQ(h.engine_destroy_calls, 1);
        }
        odin_dns_resolver_test_liveness_t post;
        ASSERT_EQ(odin_dns_resolver_test_liveness(&post), 0);
        EXPECT_EQ(post.resolver_create_calls, base.resolver_create_calls + 1);
        EXPECT_EQ(post.resolver_destroy_calls, base.resolver_destroy_calls + 1);
        ExpectRuntimeDnsLiveBaseline(base);
      }
      ClearOps();
      odin_event_loop_destroy(h.loop);
    });
  }
}

TEST(OdinServerSessionTransportTest, T1) {
  odin_event_loop_t *loop = nullptr;
  ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
  FakeFdTransport transport;
  transport.base.vt = &kFakeTransportVtable;
  FactoryState factory{&transport};
  odin_server_session_t *sentinel =
      reinterpret_cast<odin_server_session_t *>(0x1234);
  odin_server_session_t *ss = sentinel;

  errno = 0;
  EXPECT_EQ(
      odin_server_session_create_with_transport(
          nullptr, FakeFactory, &factory, ServerSessionClose, nullptr, &ss),
      -1);
  EXPECT_EQ(errno, EINVAL);
  EXPECT_EQ(ss, sentinel);

  errno = 0;
  EXPECT_EQ(odin_server_session_create_with_transport(
                loop, nullptr, &factory, ServerSessionClose, nullptr, &ss),
            -1);
  EXPECT_EQ(errno, EINVAL);
  EXPECT_EQ(ss, sentinel);

  errno = 0;
  EXPECT_EQ(odin_server_session_create_with_transport(
                loop, FakeFactory, &factory, nullptr, nullptr, &ss),
            -1);
  EXPECT_EQ(errno, EINVAL);
  EXPECT_EQ(ss, sentinel);

  errno = 0;
  EXPECT_EQ(
      odin_server_session_create_with_transport(
          loop, FakeFactory, &factory, ServerSessionClose, nullptr, nullptr),
      -1);
  EXPECT_EQ(errno, EINVAL);
  EXPECT_EQ(ss, sentinel);

  factory.fail_errno = EIO;
  errno = 0;
  EXPECT_EQ(odin_server_session_create_with_transport(
                loop, FakeFactory, &factory, ServerSessionClose, nullptr, &ss),
            -1);
  EXPECT_EQ(errno, EIO);
  EXPECT_EQ(transport.destroy_calls, 0);
  factory.fail_errno = 0;

#if defined(ODIN_CONNECT_SESSION_TESTING)
  const unsigned int connect_base = odin_connect_session_test_live_count();
  ASSERT_EQ(odin_connect_session_test_fail_next_create_server(ENOMEM), 0);
  errno = 0;
  EXPECT_EQ(odin_server_session_create_with_transport(
                loop, FakeFactory, &factory, ServerSessionClose, nullptr, &ss),
            -1);
  EXPECT_EQ(errno, ENOMEM);
  EXPECT_EQ(transport.destroy_calls, 1);
  EXPECT_EQ(odin_connect_session_test_live_count(), connect_base);
#endif

  transport.set_interest_errno = EIO;
  errno = 0;
  EXPECT_EQ(odin_server_session_create_with_transport(
                loop, FakeFactory, &factory, ServerSessionClose, nullptr, &ss),
            -1);
  EXPECT_EQ(errno, EIO);
  EXPECT_EQ(transport.last_interest, ODIN_TRANSPORT_READ);
  transport.set_interest_errno = 0;

#if defined(ODIN_SERVER_SESSION_TESTING)
  const unsigned int session_base = odin_server_session_test_live_count();
#endif
  ASSERT_EQ(odin_server_session_create_with_transport(
                loop, FakeFactory, &factory, ServerSessionClose, nullptr, &ss),
            0)
      << std::strerror(errno);
  ASSERT_NE(ss, nullptr);
  EXPECT_EQ(transport.last_interest, ODIN_TRANSPORT_READ);
#if defined(ODIN_SERVER_SESSION_TESTING)
  EXPECT_EQ(odin_server_session_test_live_count(), session_base + 1);
#endif
  odin_server_session_destroy(ss);
#if defined(ODIN_SERVER_SESSION_TESTING)
  EXPECT_EQ(odin_server_session_test_live_count(), session_base);
#endif
  odin_event_loop_destroy(loop);
}

TEST(OdinXqcServerRuntimeTest, T1) {
  RuntimeHarness h;
  InitHarness(&h);
  CreateRuntime(&h);
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  const odin_xqc_server_runtime_test_record_t *record =
      odin_xqc_server_runtime_test_record();
  ASSERT_EQ(record->udp_create_calls, 1u);
  EXPECT_EQ(record->last_udp_create.loop, h.loop);
  EXPECT_EQ(record->last_udp_create.local_addr,
            reinterpret_cast<const struct sockaddr *>(&h.local_addr));
  EXPECT_EQ(record->last_udp_create.local_addrlen, sizeof(h.local_addr));
  EXPECT_EQ(record->last_udp_create.engine_type, XQC_ENGINE_SERVER);
  EXPECT_EQ(record->last_udp_create.engine_config, &h.engine_config);
  EXPECT_EQ(record->last_udp_create.ssl_config, &h.ssl_config);
  EXPECT_EQ(record->last_udp_create.engine_callbacks, &h.engine_callbacks);
  EXPECT_NE(record->last_udp_create.transport_callbacks_value.server_accept,
            nullptr);
  EXPECT_NE(record->last_udp_create.transport_callbacks_value.server_refuse,
            nullptr);
  EXPECT_NE(
      record->last_udp_create.transport_callbacks_value.conn_update_cid_notify,
      nullptr);
  EXPECT_EQ(record->last_udp_create.transport_callbacks_value.save_token,
            nullptr);
  EXPECT_EQ(record->last_udp_create.app_user_data, h.rt);
  EXPECT_EQ(CountCalls(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_ENGINE_REGISTER_ALPN),
            1u);
#endif
  EXPECT_EQ(odin_xqc_server_runtime_start(h.rt), 0);
  EXPECT_EQ(odin_xqc_server_runtime_start(h.rt), 0);
  EXPECT_EQ(odin_xqc_server_runtime_stop(h.rt), 0);
  EXPECT_EQ(odin_xqc_server_runtime_stop(h.rt), 0);
  odin_xqc_server_runtime_destroy(h.rt);
  h.rt = nullptr;
  EXPECT_EQ(h.alpn_unregister_calls, 1);
  EXPECT_EQ(h.engine_destroy_calls, 1);
  DestroyHarness(&h);
}

TEST(OdinXqcServerRuntimeTest, T2) {
  RuntimeHarness h;
  InitHarness(&h);
  odin_xqc_server_runtime_t *out =
      reinterpret_cast<odin_xqc_server_runtime_t *>(0x99);
  errno = 0;
  EXPECT_EQ(odin_xqc_server_runtime_create(nullptr, &out), -1);
  EXPECT_EQ(errno, EINVAL);
  EXPECT_EQ(out, reinterpret_cast<odin_xqc_server_runtime_t *>(0x99));
  const odin_xqc_server_runtime_config_t config = MakeRuntimeConfig(&h);
  errno = 0;
  EXPECT_EQ(odin_xqc_server_runtime_create(&config, nullptr), -1);
  EXPECT_EQ(errno, EINVAL);

  odin_xqc_server_runtime_config_t invalid_config = config;
  invalid_config.loop = nullptr;
  out = reinterpret_cast<odin_xqc_server_runtime_t *>(0x98);
  errno = 0;
  EXPECT_EQ(odin_xqc_server_runtime_create(&invalid_config, &out), -1);
  EXPECT_EQ(errno, EINVAL);
  EXPECT_EQ(out, reinterpret_cast<odin_xqc_server_runtime_t *>(0x98));

  invalid_config = config;
  invalid_config.local_addr = nullptr;
  out = reinterpret_cast<odin_xqc_server_runtime_t *>(0x97);
  errno = 0;
  EXPECT_EQ(odin_xqc_server_runtime_create(&invalid_config, &out), -1);
  EXPECT_EQ(errno, EINVAL);
  EXPECT_EQ(out, reinterpret_cast<odin_xqc_server_runtime_t *>(0x97));

  invalid_config = config;
  invalid_config.engine_callbacks = nullptr;
  out = reinterpret_cast<odin_xqc_server_runtime_t *>(0x96);
  errno = 0;
  EXPECT_EQ(odin_xqc_server_runtime_create(&invalid_config, &out), -1);
  EXPECT_EQ(errno, EINVAL);
  EXPECT_EQ(out, reinterpret_cast<odin_xqc_server_runtime_t *>(0x96));

  EXPECT_EQ(odin_xqc_server_runtime_start(nullptr), -1);
  EXPECT_EQ(errno, EINVAL);
  EXPECT_EQ(odin_xqc_server_runtime_stop(nullptr), -1);
  EXPECT_EQ(errno, EINVAL);
  odin_xqc_server_runtime_set_dial_filter(nullptr, TestDialFilter, nullptr);
  odin_xqc_server_runtime_destroy(nullptr);

  h.engine_create_returns_null = true;
  h.engine_create_errno = EIO;
  out = reinterpret_cast<odin_xqc_server_runtime_t *>(0x77);
  errno = 0;
  EXPECT_EQ(odin_xqc_server_runtime_create(&config, &out), -1);
  EXPECT_EQ(errno, EIO);
  EXPECT_EQ(out, reinterpret_cast<odin_xqc_server_runtime_t *>(0x77));
  EXPECT_EQ(CountCalls(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_ENGINE_REGISTER_ALPN),
            0u);
  h.engine_create_returns_null = false;

  out = reinterpret_cast<odin_xqc_server_runtime_t *>(0x76);
  errno = 0;
  ASSERT_EQ(odin_xqc_server_runtime_create(&config, &out), 0)
      << std::strerror(errno);
  ASSERT_NE(out, nullptr);
  EXPECT_NE(out, reinterpret_cast<odin_xqc_server_runtime_t *>(0x76));
  EXPECT_EQ(CountCalls(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_ENGINE_REGISTER_ALPN),
            1u);
  odin_xqc_server_runtime_destroy(out);
  const int engine_destroy_calls_before_alpn_failure = h.engine_destroy_calls;

  h.alpn_register_fails = true;
  out = reinterpret_cast<odin_xqc_server_runtime_t *>(0x66);
  errno = 0;
  EXPECT_EQ(odin_xqc_server_runtime_create(&config, &out), -1);
  EXPECT_EQ(errno, EIO);
  EXPECT_EQ(out, reinterpret_cast<odin_xqc_server_runtime_t *>(0x66));
  EXPECT_EQ(h.engine_destroy_calls,
            engine_destroy_calls_before_alpn_failure + 1);
  h.alpn_register_fails = false;
  DestroyHarness(&h);
}

TEST(OdinXqcServerRuntimeTest, T3) {
  RuntimeHarness h;
  InitHarness(&h);
  CreateRuntime(&h);
  FakeConn conn;
  const xqc_cid_t cid = Cid(0xA1);
  AcceptConn(&h, &conn, cid);
  EXPECT_EQ(CountCalls(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_REGISTER_CONN),
            1u);
  EXPECT_EQ(CountCalls(
                ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_SET_TRANSPORT_USER_DATA),
            1u);
  EXPECT_EQ(
      CountCalls(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_SET_ALP_USER_DATA), 1u);
  CloseConn(&h, &conn, cid);
  ASSERT_EQ(h.fake_unregistered.size(), 1u);
  EXPECT_TRUE(CidEq(h.fake_unregistered[0], cid));
  DestroyHarness(&h);
}

TEST(OdinXqcServerRuntimeTest, T4) {
  RuntimeHarness h;
  InitHarness(&h);
  CreateRuntime(&h);
  FakeConn conn;
  const xqc_cid_t cid_a = Cid(0xA1);
  const xqc_cid_t cid_b = Cid(0xB2);
  AcceptConn(&h, &conn, cid_a);
  h.transport_callbacks.conn_update_cid_notify(AsConn(&conn), &cid_a, &cid_b,
                                               h.xu_user_data);
  const int reg_b = FirstCallIndex(
      ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_REGISTER_CONN, cid_b);
  const int unreg_a = FirstCallIndex(
      ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, cid_a);
  ASSERT_GE(reg_b, 0);
  ASSERT_GE(unreg_a, 0);
  EXPECT_LT(reg_b, unreg_a);
  CloseConn(&h, &conn, cid_b);
  EXPECT_TRUE(CidEq(h.fake_unregistered.back(), cid_b));

  FakeConn conn_fail;
  const xqc_cid_t cid_f = Cid(0xF1);
  const xqc_cid_t cid_g = Cid(0xF2);
  const xqc_cid_t cid_h = Cid(0xF3);
  AcceptConn(&h, &conn_fail, cid_f);
  FakeStream live_on_failed_update;
  CreateBidiStream(&h, &conn_fail, &live_on_failed_update);
  ASSERT_NE(live_on_failed_update.user_data, nullptr);
#if defined(ODIN_SERVER_SESSION_TESTING) &&                                    \
    defined(ODIN_CONNECT_SESSION_TESTING)
  const unsigned int session_count_with_live_stream =
      odin_server_session_test_live_count();
  const unsigned int connect_count_with_live_stream =
      odin_connect_session_test_live_count();
#endif
  h.fail_register_cid = cid_g.cid_buf[0];
  h.transport_callbacks.conn_update_cid_notify(AsConn(&conn_fail), &cid_f,
                                               &cid_g, h.xu_user_data);
  EXPECT_EQ(live_on_failed_update.user_data, nullptr);
#if defined(ODIN_SERVER_SESSION_TESTING) &&                                    \
    defined(ODIN_CONNECT_SESSION_TESTING)
  EXPECT_EQ(odin_server_session_test_live_count(),
            session_count_with_live_stream - 1);
  EXPECT_EQ(odin_connect_session_test_live_count(),
            connect_count_with_live_stream - 1);
#endif
  ASSERT_FALSE(h.conn_close_cids.empty());
  EXPECT_TRUE(CidEq(h.conn_close_cids.back(), cid_f));
  const size_t close_count = h.conn_close_cids.size();
  const size_t register_count_after_fail = h.fake_registered.size();
  const size_t unregister_count_after_fail = h.fake_unregistered.size();
  h.transport_callbacks.conn_update_cid_notify(AsConn(&conn_fail), &cid_f,
                                               &cid_h, h.xu_user_data);
  EXPECT_EQ(h.conn_close_cids.size(), close_count);
  EXPECT_EQ(h.fake_registered.size(), register_count_after_fail);
  EXPECT_EQ(h.fake_unregistered.size(), unregister_count_after_fail);
  FakeStream after_fail;
  after_fail.conn = &conn_fail;
  ASSERT_EQ(h.app_callbacks->stream_cbs.stream_create_notify(
                AsStream(&after_fail), after_fail.user_data),
            XQC_OK);
  EXPECT_EQ(after_fail.close_calls, 1);
  CloseConn(&h, &conn_fail, cid_f);

  FakeConn conn_z;
  const xqc_cid_t cid_z = Cid(0x39);
  const xqc_cid_t cid_zz = Cid(0x3A);
  const size_t register_count_before_stale = h.fake_registered.size();
  const size_t unregister_count_before_stale = h.fake_unregistered.size();
  const size_t close_count_before_stale = h.conn_close_cids.size();
#if defined(ODIN_SERVER_SESSION_TESTING) &&                                    \
    defined(ODIN_CONNECT_SESSION_TESTING)
  const unsigned int session_count_before_stale =
      odin_server_session_test_live_count();
  const unsigned int connect_count_before_stale =
      odin_connect_session_test_live_count();
#endif
  h.transport_callbacks.conn_update_cid_notify(AsConn(&conn_z), &cid_z, &cid_zz,
                                               h.xu_user_data);
  h.transport_callbacks.server_refuse(h.engine, AsConn(&conn_z), &cid_z,
                                      h.xu_user_data);
  ASSERT_EQ(h.app_callbacks->conn_cbs.conn_close_notify(
                AsConn(&conn_z), &cid_z, h.xu_user_data, nullptr),
            0);
  EXPECT_EQ(h.fake_registered.size(), register_count_before_stale);
  EXPECT_EQ(h.fake_unregistered.size(), unregister_count_before_stale);
  EXPECT_EQ(h.conn_close_cids.size(), close_count_before_stale);
#if defined(ODIN_SERVER_SESSION_TESTING) &&                                    \
    defined(ODIN_CONNECT_SESSION_TESTING)
  EXPECT_EQ(odin_server_session_test_live_count(), session_count_before_stale);
  EXPECT_EQ(odin_connect_session_test_live_count(), connect_count_before_stale);
#endif

  FakeConn conn_refuse;
  const xqc_cid_t cid_r = Cid(0x44);
  AcceptConn(&h, &conn_refuse, cid_r);
  h.transport_callbacks.server_refuse(h.engine, AsConn(&conn_refuse), &cid_r,
                                      h.xu_user_data);
  EXPECT_TRUE(CidEq(h.fake_unregistered.back(), cid_r));
  DestroyHarness(&h);
}

TEST(OdinXqcServerRuntimeTest, T5) {
  RuntimeHarness h;
  InitHarness(&h);
  CreateRuntime(&h);
  FakeConn conn_alloc;
  const xqc_cid_t cid_a = Cid(0x11);
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  ASSERT_EQ(
      odin_xqc_server_runtime_test_fail_next_conn_context_alloc(h.rt, ENOMEM),
      0);
#endif
  EXPECT_EQ(h.transport_callbacks.server_accept(h.engine, AsConn(&conn_alloc),
                                                &cid_a, h.xu_user_data),
            -1);
  EXPECT_EQ(conn_alloc.alp_user_data, nullptr);

  FakeConn conn_reg;
  const xqc_cid_t cid_b = Cid(0x12);
  h.fail_register_cid = cid_b.cid_buf[0];
  EXPECT_EQ(h.transport_callbacks.server_accept(h.engine, AsConn(&conn_reg),
                                                &cid_b, h.xu_user_data),
            -1);
  EXPECT_EQ(conn_reg.alp_user_data, nullptr);
  EXPECT_EQ(h.app_callbacks->conn_cbs.conn_create_notify(
                AsConn(&conn_reg), &cid_b, h.xu_user_data, nullptr),
            -1);
  int bogus = 0;
  EXPECT_EQ(h.app_callbacks->conn_cbs.conn_create_notify(
                AsConn(&conn_reg), &cid_b, h.xu_user_data, &bogus),
            -1);
  h.fail_register_cid = 0;
  FakeConn conn_ok;
  const xqc_cid_t cid_ok = Cid(0x13);
  AcceptConn(&h, &conn_ok, cid_ok);
  CloseConn(&h, &conn_ok, cid_ok);
  DestroyHarness(&h);
}

TEST(OdinXqcServerRuntimeTest, T6) {
  (void)signal(SIGPIPE, SIG_IGN);
  RuntimeHarness h;
  InitHarness(&h);
  CreateRuntime(&h);
  FakeConn conn;
  AcceptConn(&h, &conn, Cid(0x21));
#if defined(ODIN_SERVER_SESSION_TESTING)
  const unsigned int session_base = odin_server_session_test_live_count();
#endif
  FakeStream stream;
  RunValidStream(&h, &conn, &stream);
#if defined(ODIN_SERVER_SESSION_TESTING)
  EXPECT_EQ(odin_server_session_test_live_count(), session_base);
#endif
  EXPECT_EQ(stream.close_calls, 0);
  EXPECT_EQ(CountCallsForStream(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_CLOSE,
                                &stream),
            0u);
  CloseConn(&h, &conn, Cid(0x21));
  EXPECT_EQ(stream.user_data, nullptr);
#if defined(ODIN_SERVER_SESSION_TESTING)
  EXPECT_EQ(odin_server_session_test_live_count(), session_base);
#endif
  DestroyHarness(&h);
}

TEST(OdinXqcServerRuntimeTest, T7) {
  (void)signal(SIGPIPE, SIG_IGN);
  RuntimeHarness h;
  InitHarness(&h);
  CreateRuntime(&h);
  FakeConn conn;
  AcceptConn(&h, &conn, Cid(0x31));
  FakeStream stream;
  RunValidStream(&h, &conn, &stream, true);
  EXPECT_TRUE(StartsWith(DataSends(stream),
                         EncodedResp(ODIN_SERVER_SESSION_RESP_CODE_OK)));
  EXPECT_EQ(stream.close_calls, 0);
  EXPECT_EQ(CountCallsForStream(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_CLOSE,
                                &stream),
            0u);
  CloseConn(&h, &conn, Cid(0x31));
  DestroyHarness(&h);
}

TEST(OdinXqcServerRuntimeTest, T8) {
  (void)signal(SIGPIPE, SIG_IGN);
  RuntimeHarness h;
  InitHarness(&h);
  CreateRuntime(&h);

  uint16_t deny_port = 0;
  int deny_lfd = OpenLoopbackListener(&deny_port);
  ASSERT_GE(deny_lfd, 0) << std::strerror(errno);
  FilterState deny_a;
  deny_a.err = EACCES;
  deny_a.expect_addr = true;
  deny_a.expect_port = deny_port;
  odin_xqc_server_runtime_set_dial_filter(h.rt, TestDialFilter, &deny_a);
  FakeConn conn_a;
  AcceptConn(&h, &conn_a, Cid(0x41));
  FakeStream stream_a;
  CreateBidiStream(&h, &conn_a, &stream_a);
  RunDeniedStream(&h, &stream_a, deny_port,
                  ODIN_SERVER_SESSION_RESP_CODE_OTHER);
  EXPECT_EQ(deny_a.calls, 1);
  EXPECT_TRUE(deny_a.saw_expected_addr);
  EXPECT_EQ(stream_a.close_calls, 1);
  EXPECT_EQ(CountCallsForStream(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_CLOSE,
                                &stream_a),
            1u);
  ExpectNoAccept(deny_lfd);
  EXPECT_EQ(close(deny_lfd), 0);
  CloseConn(&h, &conn_a, Cid(0x41));

  FilterState deny_b;
  deny_b.err = EACCES;
  FilterState allow_b;
  allow_b.err = 0;
  allow_b.check_user_data = true;
  allow_b.expected_user_data = &allow_b;
  odin_xqc_server_runtime_set_dial_filter(h.rt, TestDialFilter, &deny_b);
  odin_xqc_server_runtime_set_dial_filter(h.rt, TestDialFilter, &allow_b);
  FakeConn conn_b;
  AcceptConn(&h, &conn_b, Cid(0x42));
  FakeStream stream_b;
  RunValidStream(&h, &conn_b, &stream_b);
  EXPECT_EQ(deny_b.calls, 0);
  EXPECT_EQ(allow_b.calls, 1);
  CloseConn(&h, &conn_b, Cid(0x42));

  FilterState deny_c;
  deny_c.err = EACCES;
  odin_xqc_server_runtime_set_dial_filter(h.rt, TestDialFilter, &deny_c);
  odin_xqc_server_runtime_set_dial_filter(h.rt, nullptr, &deny_c);
  FakeConn conn_c;
  AcceptConn(&h, &conn_c, Cid(0x43));
  FakeStream stream_c;
  RunValidStream(&h, &conn_c, &stream_c);
  EXPECT_EQ(deny_c.calls, 0);
  CloseConn(&h, &conn_c, Cid(0x43));

  FakeConn conn_d;
  AcceptConn(&h, &conn_d, Cid(0x44));
  uint16_t after_accept_port = 0;
  int after_accept_lfd = OpenLoopbackListener(&after_accept_port);
  ASSERT_GE(after_accept_lfd, 0) << std::strerror(errno);
  FilterState deny_d;
  deny_d.err = EACCES;
  deny_d.expect_addr = true;
  deny_d.expect_port = after_accept_port;
  odin_xqc_server_runtime_set_dial_filter(h.rt, TestDialFilter, &deny_d);
  FakeStream stream_d;
  CreateBidiStream(&h, &conn_d, &stream_d);
  RunDeniedStream(&h, &stream_d, after_accept_port,
                  ODIN_SERVER_SESSION_RESP_CODE_OTHER);
  EXPECT_EQ(deny_d.calls, 1);
  EXPECT_TRUE(deny_d.saw_expected_addr);
  ExpectNoAccept(after_accept_lfd);
  EXPECT_EQ(close(after_accept_lfd), 0);
  CloseConn(&h, &conn_d, Cid(0x44));

  FilterState allow_e;
  allow_e.err = 0;
  FilterState deny_e;
  deny_e.err = EACCES;
  odin_xqc_server_runtime_set_dial_filter(h.rt, TestDialFilter, &allow_e);
  FakeConn conn_e;
  AcceptConn(&h, &conn_e, Cid(0x45));
  uint16_t existing_port = 0;
  const int existing_lfd = OpenLoopbackListener(&existing_port);
  ASSERT_GE(existing_lfd, 0) << std::strerror(errno);
  allow_e.expect_addr = true;
  allow_e.expect_port = existing_port;
  UpstreamPeer existing_peer;
  existing_peer.lfd = existing_lfd;
  existing_peer.Start();
  FakeStream stream_e;
  CreateBidiStream(&h, &conn_e, &stream_e);
  const std::string existing_req = EncodedReq("127.0.0.1", existing_port);
  QueueRecv(&stream_e, existing_req.substr(0, 4), 4, 0);
  QueueRecv(&stream_e, "", -XQC_EAGAIN, 0);
  ASSERT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(AsStream(&stream_e),
                                                           stream_e.user_data),
            XQC_OK);
  EXPECT_EQ(allow_e.calls, 0);
  odin_xqc_server_runtime_set_dial_filter(h.rt, TestDialFilter, &deny_e);
  FinishStagedValidStream(&h, &stream_e, &existing_peer, existing_req);
  existing_peer.Join();
  EXPECT_EQ(allow_e.calls, 1);
  EXPECT_TRUE(allow_e.saw_expected_addr);
  EXPECT_EQ(deny_e.calls, 0);
  EXPECT_EQ(existing_peer.tail, "tail");
  EXPECT_TRUE(StartsWith(DataSends(stream_e),
                         EncodedResp(ODIN_SERVER_SESSION_RESP_CODE_OK)));
  EXPECT_EQ(close(existing_lfd), 0);
  CloseConn(&h, &conn_e, Cid(0x45));

#if defined(ODIN_SERVER_SESSION_TESTING) &&                                    \
    defined(ODIN_CONNECT_SESSION_TESTING)
  const unsigned int session_base = odin_server_session_test_live_count();
  const unsigned int connect_base = odin_connect_session_test_live_count();
#endif
  uint16_t destroy_port = 0;
  int destroy_lfd = OpenLoopbackListener(&destroy_port);
  ASSERT_GE(destroy_lfd, 0) << std::strerror(errno);
  FilterState destroy_f;
  destroy_f.err = EACCES;
  destroy_f.destroy_rt = h.rt;
  destroy_f.expect_addr = true;
  destroy_f.expect_port = destroy_port;
  destroy_f.h = &h;
  odin_xqc_server_runtime_set_dial_filter(h.rt, TestDialFilter, &destroy_f);
  FakeConn conn_f;
  const xqc_cid_t cid_f = Cid(0x46);
  AcceptConn(&h, &conn_f, cid_f);
  FakeStream stream_f;
  CreateBidiStream(&h, &conn_f, &stream_f);
  void *saved_transport_f = stream_f.user_data;
  ASSERT_NE(saved_transport_f, nullptr);
#if defined(ODIN_SERVER_SESSION_TESTING) &&                                    \
    defined(ODIN_CONNECT_SESSION_TESTING)
  EXPECT_EQ(odin_server_session_test_live_count(), session_base + 1);
  EXPECT_EQ(odin_connect_session_test_live_count(), connect_base + 1);
#endif
  QueueCompleteReq(&stream_f, destroy_port, "", 0);
  ASSERT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(AsStream(&stream_f),
                                                           stream_f.user_data),
            XQC_OK);
  RunUntil(h.loop, [&] {
    return destroy_f.calls == 1 && stream_f.user_data == nullptr;
  });
  EXPECT_EQ(destroy_f.calls, 1);
  EXPECT_TRUE(destroy_f.saw_expected_addr);
  EXPECT_TRUE(DataSends(stream_f).empty());
  ExpectNoAccept(destroy_lfd);
  EXPECT_EQ(close(destroy_lfd), 0);
  ASSERT_EQ(h.conn_close_cids.size(), 1u);
  EXPECT_TRUE(CidEq(h.conn_close_cids[0], cid_f));
  EXPECT_EQ(stream_f.user_data, nullptr);
#if defined(ODIN_SERVER_SESSION_TESTING) &&                                    \
    defined(ODIN_CONNECT_SESSION_TESTING)
  EXPECT_EQ(odin_server_session_test_live_count(), session_base);
  EXPECT_EQ(odin_connect_session_test_live_count(), connect_base);
#endif
  const int stream_close_before = stream_f.close_calls;
  ASSERT_EQ(h.app_callbacks->stream_cbs.stream_close_notify(AsStream(&stream_f),
                                                            saved_transport_f),
            XQC_OK);
  EXPECT_EQ(stream_f.close_calls, stream_close_before);
#if defined(ODIN_SERVER_SESSION_TESTING) &&                                    \
    defined(ODIN_CONNECT_SESSION_TESTING)
  EXPECT_EQ(odin_server_session_test_live_count(), session_base);
  EXPECT_EQ(odin_connect_session_test_live_count(), connect_base);
#endif
  CloseConn(&h, &conn_f, cid_f);
  h.rt = nullptr;
  EXPECT_EQ(h.alpn_unregister_calls, 1);
  EXPECT_EQ(h.engine_destroy_calls, 1);
  ClearOps();
  odin_event_loop_destroy(h.loop);
}

TEST(OdinXqcServerRuntimeTest, T9) {
  (void)signal(SIGPIPE, SIG_IGN);
  RuntimeHarness h;
  InitHarness(&h);
  CreateRuntime(&h);
  FakeConn conn;
  AcceptConn(&h, &conn, Cid(0x51));
  FakeStream stream;
  stream.conn = &conn;
  stream.direction = XQC_STREAM_UNI;
  ASSERT_EQ(h.app_callbacks->stream_cbs.stream_create_notify(AsStream(&stream),
                                                             stream.user_data),
            XQC_OK);
  EXPECT_EQ(stream.close_calls, 1);
  EXPECT_EQ(stream.user_data, nullptr);
  FakeStream later;
  RunValidStream(&h, &conn, &later);
  CloseConn(&h, &conn, Cid(0x51));
  DestroyHarness(&h);
}

TEST(OdinXqcServerRuntimeTest, T10) {
  (void)signal(SIGPIPE, SIG_IGN);
  RuntimeHarness h;
  InitHarness(&h);
  CreateRuntime(&h);
  FakeConn conn;
  AcceptConn(&h, &conn, Cid(0x61));
  FakeStream alloc_fail;
  alloc_fail.conn = &conn;
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  ASSERT_EQ(
      odin_xqc_server_runtime_test_fail_next_stream_context_alloc(h.rt, ENOMEM),
      0);
#endif
  ASSERT_EQ(h.app_callbacks->stream_cbs.stream_create_notify(
                AsStream(&alloc_fail), alloc_fail.user_data),
            XQC_OK);
  EXPECT_EQ(alloc_fail.close_calls, 1);
  EXPECT_EQ(alloc_fail.user_data, nullptr);
  EXPECT_EQ(CountCallsForStream(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_CLOSE,
                                &alloc_fail),
            1u);
  FakeStream valid_after_alloc_fail;
  RunValidStream(&h, &conn, &valid_after_alloc_fail);

  FakeStream transport_fail;
  transport_fail.conn = &conn;
#if defined(ODIN_TRANSPORT_XQC_TESTING)
  ASSERT_EQ(odin_xqc_stream_transport_test_fail_next_create(ENOMEM), 0);
#endif
  ASSERT_EQ(h.app_callbacks->stream_cbs.stream_create_notify(
                AsStream(&transport_fail), transport_fail.user_data),
            XQC_OK);
  EXPECT_EQ(transport_fail.close_calls, 1);
  EXPECT_EQ(transport_fail.user_data_values.size(), 0u);
  EXPECT_EQ(transport_fail.user_data, nullptr);
  EXPECT_EQ(CountCallsForStream(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_CLOSE,
                                &transport_fail),
            1u);
  FakeStream valid_after_transport_fail;
  RunValidStream(&h, &conn, &valid_after_transport_fail);

  FakeStream connect_fail;
  connect_fail.conn = &conn;
#if defined(ODIN_CONNECT_SESSION_TESTING)
  ASSERT_EQ(odin_connect_session_test_fail_next_create_server(ENOMEM), 0);
#endif
  ASSERT_EQ(h.app_callbacks->stream_cbs.stream_create_notify(
                AsStream(&connect_fail), connect_fail.user_data),
            XQC_OK);
  EXPECT_EQ(connect_fail.close_calls, 1);
  ASSERT_GE(connect_fail.user_data_values.size(), 2u);
  EXPECT_EQ(connect_fail.user_data_values.back(), nullptr);
  EXPECT_EQ(connect_fail.user_data, nullptr);
  EXPECT_EQ(CountCallsForStream(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_CLOSE,
                                &connect_fail),
            1u);
  FakeStream valid_after_connect_fail;
  RunValidStream(&h, &conn, &valid_after_connect_fail);

  FakeStream orphan;
  orphan.direction = XQC_STREAM_BIDI;
  ASSERT_EQ(h.app_callbacks->stream_cbs.stream_create_notify(AsStream(&orphan),
                                                             orphan.user_data),
            XQC_OK);
  EXPECT_EQ(orphan.close_calls, 1);
  EXPECT_EQ(orphan.user_data, nullptr);
  EXPECT_EQ(CountCallsForStream(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_CLOSE,
                                &orphan),
            1u);
  FakeStream valid_after_orphan;
  RunValidStream(&h, &conn, &valid_after_orphan);

#if defined(ODIN_CONNECT_SESSION_TESTING)
  const unsigned int connect_base = odin_connect_session_test_live_count();
#endif
  FakeFdTransport downstream;
  downstream.base.vt = &kFakeTransportVtable;
  downstream.set_interest_errno = EEXIST;
  FactoryState factory{&downstream};
  odin_server_session_t *sentinel =
      reinterpret_cast<odin_server_session_t *>(0x4242);
  odin_server_session_t *ss = sentinel;
  errno = 0;
  EXPECT_EQ(
      odin_server_session_create_with_transport(
          h.loop, FakeFactory, &factory, ServerSessionClose, nullptr, &ss),
      -1);
  EXPECT_EQ(errno, EEXIST);
  EXPECT_EQ(ss, sentinel);
  EXPECT_EQ(downstream.destroy_calls, 1);
#if defined(ODIN_CONNECT_SESSION_TESTING)
  EXPECT_EQ(odin_connect_session_test_live_count(), connect_base);
#endif
  CloseConn(&h, &conn, Cid(0x61));
  DestroyHarness(&h);
}

TEST(OdinXqcServerRuntimeTest, T11) {
  RuntimeHarness h;
  InitHarness(&h);
  CreateRuntime(&h);
  FakeConn conn_a;
  FakeConn conn_b;
  FakeConn conn_c;
  const xqc_cid_t cid_a = Cid(0x71);
  const xqc_cid_t cid_b = Cid(0x72);
  const xqc_cid_t cid_c = Cid(0x73);
  const xqc_cid_t cid_bad = Cid(0x74);
  AcceptConn(&h, &conn_a, cid_a);
  AcceptConn(&h, &conn_b, cid_b);
  ASSERT_EQ(h.transport_callbacks.server_accept(h.engine, AsConn(&conn_c),
                                                &cid_c, h.xu_user_data),
            0);
  ASSERT_NE(conn_c.alp_user_data, nullptr);

  FakeStream stream_a;
  CreateBidiStream(&h, &conn_a, &stream_a);
  void *stale_transport_a = stream_a.user_data;
  ASSERT_NE(stale_transport_a, nullptr);

  FakeStream stream_b;
  CreateBidiStream(&h, &conn_b, &stream_b);
  h.fail_register_cid = cid_bad.cid_buf[0];
  h.transport_callbacks.conn_update_cid_notify(AsConn(&conn_b), &cid_b,
                                               &cid_bad, h.xu_user_data);
  h.fail_register_cid = 0;
  EXPECT_EQ(stream_b.user_data, nullptr);
  ASSERT_EQ(h.conn_close_cids.size(), 1u);
  EXPECT_TRUE(CidEq(h.conn_close_cids[0], cid_b));
  const size_t unregister_after_b = h.fake_unregistered.size();

  odin_xqc_server_runtime_destroy(h.rt);
  EXPECT_EQ(stream_a.user_data, nullptr);
  ASSERT_EQ(h.conn_close_cids.size(), 3u);
  bool saw_a_close = false;
  bool saw_c_close = false;
  for (const xqc_cid_t &cid : h.conn_close_cids) {
    saw_a_close = saw_a_close || CidEq(cid, cid_a);
    saw_c_close = saw_c_close || CidEq(cid, cid_c);
  }
  EXPECT_TRUE(saw_a_close);
  EXPECT_TRUE(saw_c_close);
  EXPECT_EQ(h.alpn_unregister_calls, 0);
  EXPECT_EQ(h.engine_destroy_calls, 0);
  ASSERT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(AsStream(&stream_a),
                                                           nullptr),
            XQC_OK);
  ASSERT_EQ(h.app_callbacks->stream_cbs.stream_write_notify(AsStream(&stream_a),
                                                            nullptr),
            XQC_OK);
  h.app_callbacks->stream_cbs.stream_closing_notify(AsStream(&stream_a),
                                                    XQC_EPROTO, nullptr);
  ASSERT_EQ(h.app_callbacks->stream_cbs.stream_close_notify(AsStream(&stream_a),
                                                            nullptr),
            XQC_OK);
  ASSERT_EQ(h.app_callbacks->stream_cbs.stream_close_notify(AsStream(&stream_a),
                                                            stale_transport_a),
            XQC_OK);

  const size_t close_count_before_post_destroy_stream =
      h.conn_close_cids.size();
  FakeStream post_destroy;
  post_destroy.conn = &conn_a;
  ASSERT_EQ(h.app_callbacks->stream_cbs.stream_create_notify(
                AsStream(&post_destroy), post_destroy.user_data),
            XQC_OK);
  EXPECT_EQ(post_destroy.close_calls, 1);
  EXPECT_EQ(post_destroy.user_data, nullptr);
  EXPECT_EQ(h.conn_close_cids.size(), close_count_before_post_destroy_stream);
  CloseConn(&h, &conn_a, cid_a);
  EXPECT_EQ(h.alpn_unregister_calls, 0);
  EXPECT_EQ(h.engine_destroy_calls, 0);
  CloseConn(&h, &conn_b, cid_b);
  EXPECT_EQ(h.fake_unregistered.size(), unregister_after_b + 1);
  EXPECT_EQ(h.alpn_unregister_calls, 0);
  EXPECT_EQ(h.engine_destroy_calls, 0);
  h.transport_callbacks.server_refuse(h.engine, AsConn(&conn_c), &cid_c,
                                      h.xu_user_data);
  EXPECT_EQ(h.alpn_unregister_calls, 1);
  EXPECT_EQ(h.engine_destroy_calls, 1);
  h.rt = nullptr;
  ClearOps();
  odin_event_loop_destroy(h.loop);
}

TEST(OdinXqcServerRuntimeTest, T12) {
  (void)signal(SIGPIPE, SIG_IGN);
  RuntimeHarness h;
  InitHarness(&h);
  CreateRuntime(&h);
  FakeConn conn;
  AcceptConn(&h, &conn, Cid(0x81));
  uint16_t sentry_port = 0;
  const int sentry_lfd = OpenLoopbackListener(&sentry_port);
  ASSERT_GE(sentry_lfd, 0) << std::strerror(errno);
  FakeStream stream;
  CreateBidiStream(&h, &conn, &stream);
  std::string bad = EncodedReq("127.0.0.1", sentry_port);
  bad[0] = '\x7f';
  QueueRecv(&stream, bad, static_cast<ssize_t>(bad.size()), 0);
  ASSERT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(AsStream(&stream),
                                                           stream.user_data),
            XQC_OK);
  EXPECT_EQ(stream.close_calls, 1);
  EXPECT_EQ(stream.user_data, nullptr);
  EXPECT_EQ(CountCallsForStream(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_CLOSE,
                                &stream),
            1u);
  ExpectNoAccept(sentry_lfd);
  EXPECT_EQ(close(sentry_lfd), 0);
  FakeStream later;
  RunValidStream(&h, &conn, &later);
  CloseConn(&h, &conn, Cid(0x81));
  DestroyHarness(&h);
}

TEST(OdinXqcServerRuntimeTest, T13) {
  (void)signal(SIGPIPE, SIG_IGN);
  RuntimeHarness h;
  InitHarness(&h);
  CreateRuntime(&h);
  FakeConn conn;
  AcceptConn(&h, &conn, Cid(0x82));
  uint16_t sentry_port = 0;
  const int sentry_lfd = OpenLoopbackListener(&sentry_port);
  ASSERT_GE(sentry_lfd, 0) << std::strerror(errno);
  FakeStream stream;
  CreateBidiStream(&h, &conn, &stream);
  std::string bad = EncodedReq("127.0.0.1", sentry_port);
  bad[1] = '\x7f';
  QueueRecv(&stream, bad, static_cast<ssize_t>(bad.size()), 0);
  ASSERT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(AsStream(&stream),
                                                           stream.user_data),
            XQC_OK);
  EXPECT_EQ(stream.close_calls, 1);
  EXPECT_EQ(stream.user_data, nullptr);
  EXPECT_EQ(CountCallsForStream(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_CLOSE,
                                &stream),
            1u);
  ExpectNoAccept(sentry_lfd);
  EXPECT_EQ(close(sentry_lfd), 0);
  FakeStream later;
  RunValidStream(&h, &conn, &later);
  CloseConn(&h, &conn, Cid(0x82));
  DestroyHarness(&h);
}

TEST(OdinXqcServerRuntimeTest, T14) {
  (void)signal(SIGPIPE, SIG_IGN);
  RuntimeHarness h;
  InitHarness(&h);
  CreateRuntime(&h);
  FakeConn conn;
  AcceptConn(&h, &conn, Cid(0x83));
  uint16_t sentry_port = 0;
  const int sentry_lfd = OpenLoopbackListener(&sentry_port);
  ASSERT_GE(sentry_lfd, 0) << std::strerror(errno);
  FakeStream stream;
  CreateBidiStream(&h, &conn, &stream);
  std::string bad = EncodedReq("127.0.0.1", sentry_port);
  bad[2] = '\x00';
  QueueRecv(&stream, bad, static_cast<ssize_t>(bad.size()), 0);
  ASSERT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(AsStream(&stream),
                                                           stream.user_data),
            XQC_OK);
  EXPECT_EQ(stream.close_calls, 1);
  EXPECT_EQ(stream.user_data, nullptr);
  EXPECT_EQ(CountCallsForStream(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_CLOSE,
                                &stream),
            1u);
  ExpectNoAccept(sentry_lfd);
  EXPECT_EQ(close(sentry_lfd), 0);
  FakeStream later;
  RunValidStream(&h, &conn, &later);
  CloseConn(&h, &conn, Cid(0x83));
  DestroyHarness(&h);
}

TEST(OdinXqcServerRuntimeTest, T15) {
  (void)signal(SIGPIPE, SIG_IGN);
  RuntimeHarness h;
  InitHarness(&h);
  CreateRuntime(&h);
  FakeConn conn;
  AcceptConn(&h, &conn, Cid(0x91));
  FakeStream stream;
  CreateBidiStream(&h, &conn, &stream);
  void *transport = stream.user_data;
  ASSERT_EQ(h.app_callbacks->stream_cbs.stream_close_notify(AsStream(&stream),
                                                            transport),
            XQC_OK);
  EXPECT_EQ(stream.user_data, nullptr);
  ASSERT_EQ(h.app_callbacks->stream_cbs.stream_close_notify(AsStream(&stream),
                                                            nullptr),
            XQC_OK);
  ASSERT_EQ(h.app_callbacks->stream_cbs.stream_close_notify(AsStream(&stream),
                                                            transport),
            XQC_OK);
  ASSERT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(AsStream(&stream),
                                                           nullptr),
            XQC_OK);
  ASSERT_EQ(h.app_callbacks->stream_cbs.stream_write_notify(AsStream(&stream),
                                                            nullptr),
            XQC_OK);
  FakeStream later;
  RunValidStream(&h, &conn, &later);
  CloseConn(&h, &conn, Cid(0x91));
  DestroyHarness(&h);
}

TEST(OdinXqcServerRuntimeTest, T16) {
  (void)signal(SIGPIPE, SIG_IGN);
  RuntimeHarness h;
  InitHarness(&h);
  CreateRuntime(&h);
  FakeConn conn;
  AcceptConn(&h, &conn, Cid(0xA2));
  FakeStream stream;
  CreateBidiStream(&h, &conn, &stream);
  h.app_callbacks->stream_cbs.stream_closing_notify(
      AsStream(&stream), XQC_EPROTO, stream.user_data);
  EXPECT_EQ(stream.close_calls, 1);
  EXPECT_EQ(stream.user_data, nullptr);
  FakeStream later;
  RunValidStream(&h, &conn, &later);
  CloseConn(&h, &conn, Cid(0xA2));
  DestroyHarness(&h);
}

TEST(OdinXqcServerRuntimeTest, T17) {
  (void)signal(SIGPIPE, SIG_IGN);
  RuntimeHarness h;
  InitHarness(&h);
  CreateRuntime(&h);
  FilterState filter;
  filter.err = 0;
  odin_xqc_server_runtime_set_dial_filter(h.rt, TestDialFilter, &filter);
  FakeConn conn;
  AcceptConn(&h, &conn, Cid(0xB1));
  FakeStream stream;
  CreateBidiStream(&h, &conn, &stream);
  uint16_t port = 0;
  const int lfd = OpenLoopbackListener(&port);
  ASSERT_GE(lfd, 0) << std::strerror(errno);
  filter.expect_addr = true;
  filter.expect_port = port;
  UpstreamPeer peer;
  peer.lfd = lfd;
  peer.Start();

  const std::string req = EncodedReq("127.0.0.1", port);
  QueueRecv(&stream, req.substr(0, 4), 4, 0);
  QueueRecv(&stream, "", -XQC_EAGAIN, 0);
  ASSERT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(AsStream(&stream),
                                                           stream.user_data),
            XQC_OK);
  EXPECT_EQ(filter.calls, 0);
  EXPECT_TRUE(stream.sends.empty());
  EXPECT_EQ(stream.close_calls, 0);
#if defined(ODIN_TRANSPORT_XQC_TESTING)
  EXPECT_NE(odin_xqc_stream_transport_test_interest(
                static_cast<odin_transport_t *>(stream.user_data)) &
                ODIN_TRANSPORT_READ,
            0u);
#endif
  FinishStagedValidStream(&h, &stream, &peer, req);
  EXPECT_EQ(filter.calls, 1);
  EXPECT_TRUE(filter.saw_expected_addr);
  peer.Join();
  EXPECT_EQ(peer.accept_errno, 0);
  EXPECT_EQ(peer.tail, "tail");
  EXPECT_TRUE(peer.eof_seen.load());
  EXPECT_TRUE(StartsWith(DataSends(stream),
                         EncodedResp(ODIN_SERVER_SESSION_RESP_CODE_OK)));
  EXPECT_NE(DataSends(stream).find("reply"), std::string::npos);
  EXPECT_TRUE(HasFinSend(stream));
  EXPECT_EQ(stream.close_calls, 0);
  EXPECT_EQ(close(lfd), 0);
  CloseConn(&h, &conn, Cid(0xB1));
  DestroyHarness(&h);
}

TEST(OdinXqcServerRuntimeTest, T18) {
  RuntimeHarness h;
  InitHarness(&h);
  CreateRuntime(&h);
  FakeConn conn;
  const xqc_cid_t cid = Cid(0xC1);
  AcceptConn(&h, &conn, cid);
#if defined(ODIN_SERVER_SESSION_TESTING)
  const unsigned int session_base = odin_server_session_test_live_count();
#endif
  FakeStream stream;
  CreateBidiStream(&h, &conn, &stream);
  ASSERT_EQ(h.app_callbacks->conn_cbs.conn_close_notify(
                AsConn(&conn), &cid, h.xu_user_data, conn.alp_user_data),
            0);
  EXPECT_EQ(stream.user_data, nullptr);
  ASSERT_FALSE(h.fake_unregistered.empty());
  EXPECT_TRUE(CidEq(h.fake_unregistered.back(), cid));
#if defined(ODIN_SERVER_SESSION_TESTING)
  EXPECT_EQ(odin_server_session_test_live_count(), session_base);
#endif
  conn.alp_user_data = nullptr;
  DestroyHarness(&h);
}

// NOLINTEND(misc-const-correctness, misc-use-internal-linkage,
// performance-no-int-to-ptr)
