// odin/testing/client_xqc_runtime_unittests.cpp
//
// RFC-027 final tests T1-T21 for the client-side xquic runtime.

#include "odin/client_xqc_runtime.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include "odin/client_session.h"
#include "odin/event_loop.h"
#include "odin/protocol.h"
#include "odin/testing/client_session_internal_test.h"
#include "odin/testing/client_xqc_runtime_internal_test.h"
#include "odin/testing/connect_session_internal_test.h"
#include "odin/testing/event_loop_internal_test.h"
#include "odin/testing/server_xqc_runtime_internal_test.h"
#include "odin/testing/transport_xqc_internal_test.h"
#include "odin/testing/xqc_udp_internal_test.h"
#include "odin/transport.h"
#include "odin/xqc_udp.h"

#include "gtest/gtest.h"

// NOLINTBEGIN(misc-const-correctness, misc-use-internal-linkage)

namespace {

constexpr char kHttpReq[] = "CONNECT example.com:443 HTTP/1.1\r\n\r\n";
constexpr char kHttp200[] = "HTTP/1.1 200 Connection Established\r\n\r\n";
constexpr char kHttp400[] = "HTTP/1.1 400 Bad Request\r\n\r\n";
constexpr char kHttp405[] =
    "HTTP/1.1 405 Method Not Allowed\r\nAllow: CONNECT\r\n\r\n";
constexpr char kLateLocal[] = "late-local!";

std::string FindRepoRoot();
void RunLoopFor(odin_event_loop_t *loop, uint64_t usec);

struct FakeConn {
  void *alp_user_data = nullptr;
};

struct FakeStream {
  FakeConn *conn = nullptr;
  std::deque<std::string> recv_chunks;
  std::deque<ssize_t> send_results;
  std::vector<std::string> sends;
  std::vector<void *> user_data_values;
  void *user_data = nullptr;
  int user_data_clear_order = 0;
  int close_order = 0;
  int close_calls = 0;
  int recv_calls = 0;
  int send_calls = 0;
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

xqc_cid_t Cid(uint8_t byte) {
  xqc_cid_t cid;
  std::memset(&cid, 0, sizeof(cid));
  cid.cid_len = 1;
  cid.cid_buf[0] = byte;
  return cid;
}

bool CidEq(const xqc_cid_t &a, const xqc_cid_t &b) {
  return a.cid_len == b.cid_len &&
         std::memcmp(a.cid_buf, b.cid_buf, a.cid_len) == 0;
}

struct sockaddr_in Loopback4(uint16_t port) {
  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
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

void SetNonblock(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  ASSERT_NE(flags, -1) << std::strerror(errno);
  ASSERT_EQ(fcntl(fd, F_SETFL, flags | O_NONBLOCK), 0) << std::strerror(errno);
}

void MakePair(int *owned, int *peer) {
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0)
      << std::strerror(errno);
  SetNonblock(fds[0]);
  SetNonblock(fds[1]);
  *owned = fds[0];
  *peer = fds[1];
}

bool SetNonblockNoAssert(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    return false;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

int OpenLoopbackListener(uint16_t *port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  const int one = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr = Loopback4(0);
  if (bind(fd, reinterpret_cast<const struct sockaddr *>(&addr),
           sizeof(addr)) != 0 ||
      listen(fd, 4) != 0) {
    const int saved = errno;
    close(fd);
    errno = saved;
    return -1;
  }
  socklen_t len = sizeof(addr);
  if (getsockname(fd, reinterpret_cast<struct sockaddr *>(&addr), &len) != 0 ||
      !SetNonblockNoAssert(fd)) {
    const int saved = errno;
    close(fd);
    errno = saved;
    return -1;
  }
  *port = ntohs(addr.sin_port);
  return fd;
}

std::string DrainFdNow(int fd) {
  std::string out;
  char buf[512];
  for (;;) {
    const ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      out.append(buf, static_cast<size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    return out;
  }
}

bool PollPeekEquals(int fd, const std::string &expected, int timeout_ms) {
  struct pollfd pfd;
  pfd.fd = fd;
  pfd.events = POLLIN;
  const int prc = poll(&pfd, 1, timeout_ms);
  if (prc <= 0 || (pfd.revents & POLLIN) == 0) {
    return false;
  }
  std::vector<char> buf(expected.size());
  const ssize_t n = recv(fd, buf.data(), buf.size(), MSG_PEEK);
  return n == static_cast<ssize_t>(expected.size()) &&
         std::string(buf.data(), buf.size()) == expected;
}

template <typename Predicate>
bool RunUntil(odin_event_loop_t *loop, Predicate predicate,
              int deadline_ms = 3000) {
  const auto start = std::chrono::steady_clock::now();
  while (!predicate()) {
    const auto now = std::chrono::steady_clock::now();
    const int elapsed = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
            .count());
    if (elapsed >= deadline_ms) {
      return false;
    }
    RunLoopFor(loop, 1000);
  }
  return true;
}

bool WriteAllFd(int fd, const void *buf, size_t len) {
  const char *p = static_cast<const char *>(buf);
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
        (void)poll(&pfd, 1, 100);
        continue;
      }
      return false;
    }
    off += static_cast<size_t>(n);
  }
  return true;
}

std::string ReadAvailableFd(int fd, int deadline_ms) {
  std::string out;
  const auto start = std::chrono::steady_clock::now();
  char buf[512];
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
    const ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      out.append(buf, static_cast<size_t>(n));
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

bool PeerSawEof(int fd) {
  char ch = 0;
  for (int i = 0; i < 20; ++i) {
    const ssize_t n = read(fd, &ch, 1);
    if (n == 0) {
      return true;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
      usleep(1000);
      continue;
    }
    return false;
  }
  return false;
}

std::string EncodedConnectReq(const char *host = "example.com",
                              uint16_t port = 443) {
  odin_proto_iov_t iov[3];
  uint8_t header[3];
  uint8_t port_bytes[2];
  const size_t host_len = std::strlen(host);
  EXPECT_EQ(odin_proto_encode_connect_req(host, host_len, port, iov, header,
                                          port_bytes),
            ODIN_PROTO_OK);
  std::string out;
  for (const odin_proto_iov_t &part : iov) {
    out.append(static_cast<const char *>(part.base), part.len);
  }
  return out;
}

std::string EncodedConnectResp(uint16_t code = 0) {
  odin_proto_connect_resp_frame_t resp;
  odin_proto_encode_connect_resp(code, &resp);
  return std::string(reinterpret_cast<const char *>(resp.bytes),
                     sizeof(resp.bytes));
}

std::string HttpConnectReq(const char *host, uint16_t port) {
  return std::string("CONNECT ") + host + ":" + std::to_string(port) +
         " HTTP/1.1\r\n\r\n";
}

void StopTimerCb(odin_event_loop_t *loop, odin_event_timer_t *timer,
                 void *user_data) {
  (void)user_data;
  odin_event_timer_stop(timer);
  odin_event_loop_stop(loop);
}

void RunLoopFor(odin_event_loop_t *loop, uint64_t usec = 20000) {
  odin_event_timer_t *timer = nullptr;
  ASSERT_EQ(odin_event_timer_start(loop, usec, 0, StopTimerCb, nullptr, &timer),
            0)
      << std::strerror(errno);
  ASSERT_EQ(odin_event_loop_run(loop), 0) << std::strerror(errno);
}

int CertVerifyOk(const unsigned char *certs[], const size_t cert_len[],
                 size_t certs_len, void *conn_user_data) {
  (void)certs;
  (void)cert_len;
  (void)certs_len;
  (void)conn_user_data;
  return 0;
}

void SaveTokenSentinel(const unsigned char *token, uint32_t token_len,
                       void *conn_user_data) {
  (void)token;
  (void)token_len;
  (void)conn_user_data;
}

void SaveStringSentinel(const char *data, size_t data_len,
                        void *conn_user_data) {
  (void)data;
  (void)data_len;
  (void)conn_user_data;
}

ssize_t RealStreamRecv(xqc_stream_t *stream, unsigned char *recv_buf,
                       size_t recv_buf_size, uint8_t *fin) {
  return xqc_stream_recv(stream, recv_buf, recv_buf_size, fin);
}

ssize_t RealStreamSend(xqc_stream_t *stream, unsigned char *send_data,
                       size_t send_data_size, uint8_t fin) {
  return xqc_stream_send(stream, send_data, send_data_size, fin);
}

void RealStreamSetUserData(xqc_stream_t *stream, void *user_data) {
  xqc_stream_set_user_data(stream, user_data);
}

struct ProductionCertState {
  int calls = 0;
  size_t certs_len = 0;
  void *conn_user_data = nullptr;
  const char *ca_file = nullptr;
  bool ca_verified = false;
  bool host_verified = false;
};

ProductionCertState *g_production_cert_state = nullptr;

X509 *ReadPemX509File(const char *path) {
  BIO *bio = BIO_new_file(path, "r");
  if (bio == nullptr) {
    return nullptr;
  }
  X509 *cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  return cert;
}

X509 *ReadDerX509(const unsigned char *data, size_t len) {
  if (data == nullptr || len > static_cast<size_t>(LONG_MAX)) {
    return nullptr;
  }
  const unsigned char *p = data;
  return d2i_X509(nullptr, &p, static_cast<long>(len));
}

bool PushDerX509(STACK_OF(X509) * stack, const unsigned char *data,
                 size_t len) {
  X509 *cert = ReadDerX509(data, len);
  if (cert == nullptr) {
    return false;
  }
  if (!sk_X509_push(stack, cert)) {
    X509_free(cert);
    return false;
  }
  return true;
}

bool VerifyWithCaFile(const unsigned char *certs[], const size_t cert_len[],
                      size_t certs_len, const char *ca_file,
                      bool *host_verified) {
  if (host_verified != nullptr) {
    *host_verified = false;
  }
  if (certs == nullptr || cert_len == nullptr || certs_len == 0 ||
      ca_file == nullptr) {
    return false;
  }

  X509 *leaf = ReadDerX509(certs[0], cert_len[0]);
  X509 *ca = ReadPemX509File(ca_file);
  X509_STORE *store = X509_STORE_new();
  X509_STORE_CTX *ctx = X509_STORE_CTX_new();
  STACK_OF(X509) *intermediates = nullptr;
  bool ok = false;

  if (leaf == nullptr || ca == nullptr || store == nullptr || ctx == nullptr) {
    goto done;
  }
  if (X509_check_host(leaf, "localhost", std::strlen("localhost"), 0,
                      nullptr) != 1) {
    goto done;
  }
  if (host_verified != nullptr) {
    *host_verified = true;
  }
  if (!X509_STORE_add_cert(store, ca)) {
    goto done;
  }
  if (certs_len > 1) {
    intermediates = sk_X509_new_null();
    if (intermediates == nullptr) {
      goto done;
    }
    for (size_t i = 1; i < certs_len; ++i) {
      if (!PushDerX509(intermediates, certs[i], cert_len[i])) {
        goto done;
      }
    }
  }
  if (!X509_STORE_CTX_init(ctx, store, leaf, intermediates)) {
    goto done;
  }
  ok = X509_verify_cert(ctx) == 1;

done:
  sk_X509_pop_free(intermediates, X509_free);
  X509_STORE_CTX_free(ctx);
  X509_STORE_free(store);
  X509_free(ca);
  X509_free(leaf);
  return ok;
}

int ProductionCertVerify(const unsigned char *certs[], const size_t cert_len[],
                         size_t certs_len, void *conn_user_data) {
  ProductionCertState *state = g_production_cert_state;
  if (state != nullptr) {
    state->calls += 1;
    state->certs_len = certs_len;
    state->conn_user_data = conn_user_data;
    state->ca_verified = VerifyWithCaFile(
        certs, cert_len, certs_len, state->ca_file, &state->host_verified);
  }
  EXPECT_NE(certs, nullptr);
  EXPECT_GT(certs_len, 0u);
  EXPECT_NE(state, nullptr);
  EXPECT_TRUE(state != nullptr && state->host_verified);
  EXPECT_TRUE(state != nullptr && state->ca_verified);
  return state != nullptr && state->ca_verified ? 0 : -1;
}

struct ProductionDialFilterState {
  int calls = 0;
  uint16_t expected_port = 0;
  bool saw_expected_loopback = false;
};

int ProductionDialFilter(const struct sockaddr *addr, socklen_t addrlen,
                         void *user_data) {
  ProductionDialFilterState *state =
      static_cast<ProductionDialFilterState *>(user_data);
  state->calls += 1;
  EXPECT_NE(addr, nullptr);
  if (addr != nullptr && addr->sa_family == AF_INET) {
    EXPECT_EQ(addrlen, static_cast<socklen_t>(sizeof(struct sockaddr_in)));
    const struct sockaddr_in *sin =
        reinterpret_cast<const struct sockaddr_in *>(addr);
    if (sin->sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
        ntohs(sin->sin_port) == state->expected_port) {
      state->saw_expected_loopback = true;
    }
  }
  return 0;
}

// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct ClientHarness {
  odin_event_loop_t *loop = nullptr;
  odin_xqc_client_runtime_t *rt = nullptr;
  char engine_storage = 0;
  xqc_engine_t *engine = reinterpret_cast<xqc_engine_t *>(&engine_storage);
  void *xu_user_data = nullptr;
  void *alpn_user_data = nullptr;
  xqc_app_proto_callbacks_t *app_callbacks = nullptr;
  xqc_transport_callbacks_t transport_callbacks;
  xqc_config_t engine_config;
  xqc_engine_ssl_config_t engine_ssl_config;
  xqc_engine_callback_t engine_callbacks;
  xqc_conn_settings_t conn_settings;
  xqc_conn_ssl_config_t conn_ssl_config;
  xqc_transport_callbacks_t caller_transport_callbacks;
  struct sockaddr_in local_addr;
  struct sockaddr_in peer_addr;
  FakeConn conn_a;
  FakeConn conn_b;
  FakeConn conn_c;
  xqc_cid_t cid_a = Cid(0xA1);
  xqc_cid_t cid_b = Cid(0xB2);
  xqc_cid_t cid_c = Cid(0xC3);
  std::deque<FakeStream *> streams_to_create;
  std::vector<xqc_cid_t> registered;
  std::vector<xqc_cid_t> unregistered;
  std::vector<xqc_cid_t> conn_close_cids;
  int engine_create_calls = 0;
  int engine_destroy_calls = 0;
  int engine_destroy_callback_close_calls = 0;
  int alpn_unregister_calls = 0;
  int connect_calls = 0;
  int fail_register_errno = 0;
  uint8_t fail_register_cid = 0;
  bool engine_create_returns_null = false;
  int engine_create_errno = EIO;
  bool alpn_register_fails = false;
  bool connect_returns_null = false;
  bool connect_invokes_create = true;
  bool connect_invokes_close_before_null = false;
  bool stream_create_returns_null = false;
  int stream_create_errno = 0;
  bool stale_errno_before_stream_create = false;
  bool engine_destroy_invokes_close = false;
  bool connect_uses_conn_b = false;
  std::string expected_server_host = "odin.test";
  std::vector<unsigned char> expected_token;
  bool expect_token_bytes = false;
  int expected_no_crypto_flag = 7;
  int expected_conn_settings_ping_on = 0;
  uint32_t expected_conn_settings_so_sndbuf = 0;
  uint8_t expected_cert_verify_flag = 0;
  std::string expected_session_ticket;
  bool expect_session_ticket_bytes = false;
  std::string expected_transport_parameters;
  bool expect_transport_parameters_bytes = false;
  struct sockaddr_storage expected_peer_storage;
  socklen_t expected_peer_addrlen = 0;
  bool expect_peer_bytes = false;
  int event_order = 0;
};

ClientHarness *g_client_harness = nullptr;

xqc_engine_t *FakeEngineCreate(xqc_engine_type_t engine_type,
                               const xqc_config_t *engine_config,
                               const xqc_engine_ssl_config_t *ssl_config,
                               const xqc_engine_callback_t *engine_callback,
                               const xqc_transport_callbacks_t *transport_cbs,
                               void *user_data) {
  (void)engine_config;
  (void)ssl_config;
  (void)engine_callback;
  EXPECT_EQ(engine_type, XQC_ENGINE_CLIENT);
  ClientHarness *h = g_client_harness;
  if (h == nullptr) {
    errno = EIO;
    return nullptr;
  }
  h->engine_create_calls += 1;
  h->xu_user_data = user_data;
  h->transport_callbacks = *transport_cbs;
  if (h->engine_create_returns_null) {
    errno = h->engine_create_errno;
    return nullptr;
  }
  return h->engine;
}

void FakeEngineDestroy(xqc_engine_t *engine) {
  ClientHarness *h = g_client_harness;
  if (h != nullptr && engine == h->engine) {
    h->engine_destroy_calls += 1;
    if (h->engine_destroy_invokes_close && h->app_callbacks != nullptr &&
        h->xu_user_data != nullptr) {
      h->engine_destroy_callback_close_calls += 1;
      EXPECT_EQ(h->app_callbacks->conn_cbs.conn_close_notify(
                    AsConn(&h->conn_a), &h->cid_a, h->xu_user_data, h->rt),
                0);
    }
  }
}

xqc_int_t FakeEngineRegisterAlpn(xqc_engine_t *engine, const char *alpn,
                                 size_t alpn_len,
                                 xqc_app_proto_callbacks_t *app_callbacks,
                                 void *user_data) {
  ClientHarness *h = g_client_harness;
  if (h == nullptr || engine != h->engine) {
    return -XQC_EPARAM;
  }
  EXPECT_EQ(std::string(alpn, alpn_len), ODIN_XQC_CLIENT_ALPN);
  if (h->alpn_register_fails) {
    return -XQC_EPARAM;
  }
  h->app_callbacks = app_callbacks;
  h->alpn_user_data = user_data;
  return XQC_OK;
}

xqc_int_t FakeEngineUnregisterAlpn(xqc_engine_t *engine, const char *alpn,
                                   size_t alpn_len) {
  ClientHarness *h = g_client_harness;
  if (h != nullptr && engine == h->engine) {
    h->alpn_unregister_calls += 1;
    EXPECT_EQ(std::string(alpn, alpn_len), ODIN_XQC_CLIENT_ALPN);
  }
  return XQC_OK;
}

const xqc_cid_t *
FakeXqcConnect(xqc_engine_t *engine, const xqc_conn_settings_t *conn_settings,
               const unsigned char *token, unsigned int token_len,
               const char *server_host, int no_crypto_flag,
               const xqc_conn_ssl_config_t *conn_ssl_config,
               const struct sockaddr *peer, socklen_t peer_addrlen,
               const char *alpn, void *user_data) {
  (void)token;
  (void)token_len;
  ClientHarness *h = g_client_harness;
  EXPECT_NE(h, nullptr);
  EXPECT_EQ(engine, h->engine);
  EXPECT_NE(conn_settings, nullptr);
  EXPECT_STREQ(server_host, h->expected_server_host.c_str());
  EXPECT_EQ(no_crypto_flag, h->expected_no_crypto_flag);
  EXPECT_EQ(conn_settings->ping_on, h->expected_conn_settings_ping_on);
  EXPECT_EQ(conn_settings->so_sndbuf, h->expected_conn_settings_so_sndbuf);
  EXPECT_EQ(conn_ssl_config->cert_verify_flag, h->expected_cert_verify_flag);
  EXPECT_NE(conn_ssl_config->session_ticket_data, nullptr);
  if (h->expect_token_bytes) {
    EXPECT_EQ(token_len, h->expected_token.size());
    if (token == nullptr) {
      ADD_FAILURE() << "expected non-null token";
      return nullptr;
    }
    EXPECT_EQ(std::vector<unsigned char>(token, token + token_len),
              h->expected_token);
  }
  if (h->expect_session_ticket_bytes) {
    EXPECT_EQ(conn_ssl_config->session_ticket_len,
              h->expected_session_ticket.size());
    if (conn_ssl_config->session_ticket_data == nullptr) {
      ADD_FAILURE() << "expected non-null session ticket";
      return nullptr;
    }
    EXPECT_EQ(std::string(conn_ssl_config->session_ticket_data,
                          conn_ssl_config->session_ticket_len),
              h->expected_session_ticket);
  }
  if (h->expect_transport_parameters_bytes) {
    EXPECT_EQ(conn_ssl_config->transport_parameter_data_len,
              h->expected_transport_parameters.size());
    if (conn_ssl_config->transport_parameter_data == nullptr) {
      ADD_FAILURE() << "expected non-null transport parameters";
      return nullptr;
    }
    EXPECT_EQ(std::string(conn_ssl_config->transport_parameter_data,
                          conn_ssl_config->transport_parameter_data_len),
              h->expected_transport_parameters);
  }
  EXPECT_NE(peer, nullptr);
  if (peer == nullptr) {
    return nullptr;
  }
  if (h->expect_peer_bytes) {
    EXPECT_EQ(peer_addrlen, h->expected_peer_addrlen);
    if (peer_addrlen >
        static_cast<socklen_t>(sizeof(h->expected_peer_storage))) {
      ADD_FAILURE() << "peer address length exceeds expected storage";
      return nullptr;
    }
    EXPECT_EQ(std::memcmp(peer, &h->expected_peer_storage, peer_addrlen), 0);
  }
  if (peer->sa_family == AF_INET) {
    EXPECT_EQ(peer_addrlen, static_cast<socklen_t>(sizeof(struct sockaddr_in)));
  } else if (peer->sa_family == AF_INET6) {
    EXPECT_EQ(peer_addrlen,
              static_cast<socklen_t>(sizeof(struct sockaddr_in6)));
  } else {
    ADD_FAILURE() << "unexpected peer address family";
  }
  EXPECT_STREQ(alpn, ODIN_XQC_CLIENT_ALPN);
  EXPECT_EQ(user_data, h->xu_user_data);
  h->connect_calls += 1;
  FakeConn *conn_to_create = h->connect_uses_conn_b ? &h->conn_b : &h->conn_a;
  const xqc_cid_t *cid_to_create =
      h->connect_uses_conn_b ? &h->cid_b : &h->cid_a;
  if (h->connect_invokes_create) {
    const int create_rc = h->app_callbacks->conn_cbs.conn_create_notify(
        AsConn(conn_to_create), cid_to_create, h->xu_user_data, nullptr);
    const bool register_failure_expected =
        h->fail_register_errno != 0 && cid_to_create != nullptr &&
        cid_to_create->cid_len == 1 &&
        cid_to_create->cid_buf[0] == h->fail_register_cid;
    EXPECT_EQ(create_rc, register_failure_expected ? -1 : 0);
    if (create_rc != 0) {
      return nullptr;
    }
  }
  if (h->connect_returns_null) {
    if (h->connect_invokes_close_before_null) {
      EXPECT_EQ(h->app_callbacks->conn_cbs.conn_close_notify(
                    AsConn(conn_to_create), cid_to_create, h->xu_user_data,
                    h->alpn_user_data),
                0);
    }
    return nullptr;
  }
  return cid_to_create;
}

void FakeConnSetAlpUserData(xqc_connection_t *conn, void *user_data) {
  FromConn(conn)->alp_user_data = user_data;
}

xqc_int_t FakeConnClose(xqc_engine_t *engine, const xqc_cid_t *cid) {
  ClientHarness *h = g_client_harness;
  EXPECT_EQ(engine, h->engine);
  if (cid != nullptr) {
    h->conn_close_cids.push_back(*cid);
  }
  return XQC_OK;
}

void *FakeGetConnAlpUserDataByStream(xqc_stream_t *stream) {
  FakeStream *fake = FromStream(stream);
  return fake->conn != nullptr ? fake->conn->alp_user_data : nullptr;
}

xqc_stream_t *FakeStreamCreate(xqc_connection_t *conn,
                               xqc_stream_direction_t dir, void *user_data) {
  (void)user_data;
  ClientHarness *h = g_client_harness;
  EXPECT_EQ(dir, XQC_STREAM_BIDI);
  if (h->stale_errno_before_stream_create) {
    errno = EBUSY;
  }
  if (h->stream_create_returns_null) {
    if (h->stream_create_errno != 0) {
      errno = h->stream_create_errno;
    }
    return nullptr;
  }
  if (h->streams_to_create.empty()) {
    errno = ENOMEM;
    return nullptr;
  }
  FakeStream *stream = h->streams_to_create.front();
  h->streams_to_create.pop_front();
  stream->conn = FromConn(conn);
  return AsStream(stream);
}

xqc_int_t FakeStreamClose(xqc_stream_t *stream) {
  FakeStream *s = FromStream(stream);
  s->close_calls += 1;
  ClientHarness *h = g_client_harness;
  if (h != nullptr) {
    s->close_order = ++h->event_order;
  }
  return XQC_OK;
}

int FakeUdpRegisterConn(odin_xqc_udp_t *, const xqc_cid_t *cid) {
  ClientHarness *h = g_client_harness;
  if (h->fail_register_errno != 0 && cid != nullptr && cid->cid_len == 1 &&
      cid->cid_buf[0] == h->fail_register_cid) {
    errno = h->fail_register_errno;
    return -1;
  }
  if (cid != nullptr) {
    h->registered.push_back(*cid);
  }
  return 0;
}

void FakeUdpUnregisterConn(odin_xqc_udp_t *, const xqc_cid_t *cid) {
  ClientHarness *h = g_client_harness;
  if (h != nullptr && cid != nullptr) {
    h->unregistered.push_back(*cid);
  }
}

ssize_t FakeStreamRecv(xqc_stream_t *stream, unsigned char *recv_buf,
                       size_t recv_buf_size, uint8_t *fin) {
  FakeStream *s = FromStream(stream);
  s->recv_calls += 1;
  if (s->recv_chunks.empty()) {
    *fin = 0;
    return -XQC_EAGAIN;
  }
  std::string chunk = s->recv_chunks.front();
  s->recv_chunks.pop_front();
  const size_t copy = std::min(chunk.size(), recv_buf_size);
  std::copy_n(reinterpret_cast<const unsigned char *>(chunk.data()), copy,
              recv_buf);
  *fin = 0;
  return static_cast<ssize_t>(copy);
}

ssize_t FakeStreamSend(xqc_stream_t *stream, unsigned char *send_data,
                       size_t send_data_size, uint8_t fin) {
  (void)fin;
  FakeStream *s = FromStream(stream);
  s->send_calls += 1;
  if (send_data_size > ODIN_PROTO_CONNECT_REQ_MAX + 4096u) {
    ADD_FAILURE() << "unexpected fake stream send size " << send_data_size;
    return -XQC_EPARAM;
  }
  ssize_t result = static_cast<ssize_t>(send_data_size);
  if (!s->send_results.empty()) {
    result = s->send_results.front();
    s->send_results.pop_front();
  }
  if (result < 0) {
    return result;
  }
  const size_t written = std::min(static_cast<size_t>(result), send_data_size);
  if (send_data != nullptr && written > 0) {
    s->sends.emplace_back(reinterpret_cast<const char *>(send_data), written);
  }
  return static_cast<ssize_t>(written);
}

void FakeStreamSetUserData(xqc_stream_t *stream, void *user_data) {
  FakeStream *s = FromStream(stream);
  s->user_data = user_data;
  s->user_data_values.push_back(user_data);
  ClientHarness *h = g_client_harness;
  if (h != nullptr && user_data == nullptr) {
    s->user_data_clear_order = ++h->event_order;
  }
}

std::string JoinedSends(const FakeStream &stream) {
  std::string out;
  for (const std::string &send : stream.sends) {
    out += send;
  }
  return out;
}

unsigned int CountClientCalls(odin_xqc_client_runtime_test_call_kind_t kind) {
  const odin_xqc_client_runtime_test_record_t *record =
      odin_xqc_client_runtime_test_record();
  unsigned int count = 0;
  for (unsigned int i = 0; i < record->call_count; ++i) {
    if (record->calls[i].kind == kind) {
      count += 1;
    }
  }
  return count;
}

void ExpectEventLoopLivenessZero() {
  odin_event_loop_test_liveness_t live{};
  ASSERT_EQ(odin_event_loop_test_liveness(&live), 0);
  EXPECT_EQ(live.loops, 0u);
  EXPECT_EQ(live.io_handles, 0u);
  EXPECT_EQ(live.timers, 0u);
  EXPECT_EQ(live.task_nodes, 0u);
}

unsigned int
CountClientCallsForCid(odin_xqc_client_runtime_test_call_kind_t kind,
                       const xqc_cid_t &cid) {
  const odin_xqc_client_runtime_test_record_t *record =
      odin_xqc_client_runtime_test_record();
  unsigned int count = 0;
  for (unsigned int i = 0; i < record->call_count; ++i) {
    if (record->calls[i].kind == kind && CidEq(record->calls[i].cid, cid)) {
      count += 1;
    }
  }
  return count;
}

unsigned int
CountClientCallsForStream(odin_xqc_client_runtime_test_call_kind_t kind,
                          xqc_stream_t *stream) {
  const odin_xqc_client_runtime_test_record_t *record =
      odin_xqc_client_runtime_test_record();
  unsigned int count = 0;
  for (unsigned int i = 0; i < record->call_count; ++i) {
    if (record->calls[i].kind == kind && record->calls[i].stream == stream) {
      count += 1;
    }
  }
  return count;
}

int FirstClientCallIndex(odin_xqc_client_runtime_test_call_kind_t kind) {
  const odin_xqc_client_runtime_test_record_t *record =
      odin_xqc_client_runtime_test_record();
  for (unsigned int i = 0; i < record->call_count; ++i) {
    if (record->calls[i].kind == kind) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int FirstClientCallForCidIndex(odin_xqc_client_runtime_test_call_kind_t kind,
                               const xqc_cid_t &cid) {
  const odin_xqc_client_runtime_test_record_t *record =
      odin_xqc_client_runtime_test_record();
  for (unsigned int i = 0; i < record->call_count; ++i) {
    if (record->calls[i].kind == kind && CidEq(record->calls[i].cid, cid)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int FirstClientCallForStreamIndex(odin_xqc_client_runtime_test_call_kind_t kind,
                                  xqc_stream_t *stream) {
  const odin_xqc_client_runtime_test_record_t *record =
      odin_xqc_client_runtime_test_record();
  for (unsigned int i = 0; i < record->call_count; ++i) {
    if (record->calls[i].kind == kind && record->calls[i].stream == stream) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int FirstConnSetUserDataIndex(xqc_connection_t *conn, void *user_data) {
  const odin_xqc_client_runtime_test_record_t *record =
      odin_xqc_client_runtime_test_record();
  for (unsigned int i = 0; i < record->call_count; ++i) {
    const odin_xqc_client_runtime_test_call_t &call = record->calls[i];
    if (call.kind == ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_SET_ALP_USER_DATA &&
        call.conn == conn && call.user_data == user_data) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

class OdinXqcClientRuntimeTest : public ::testing::Test {
protected:
  ClientHarness h; // NOLINT(misc-non-private-member-variables-in-classes)

  void SetUp() override {
    std::memset(&h.transport_callbacks, 0, sizeof(h.transport_callbacks));
    std::memset(&h.engine_config, 0, sizeof(h.engine_config));
    std::memset(&h.engine_ssl_config, 0, sizeof(h.engine_ssl_config));
    std::memset(&h.engine_callbacks, 0, sizeof(h.engine_callbacks));
    std::memset(&h.conn_settings, 0, sizeof(h.conn_settings));
    std::memset(&h.conn_ssl_config, 0, sizeof(h.conn_ssl_config));
    std::memset(&h.caller_transport_callbacks, 0,
                sizeof(h.caller_transport_callbacks));
    h.caller_transport_callbacks.cert_verify_cb = CertVerifyOk;
    h.local_addr = Loopback4(0);
    h.peer_addr = Loopback4(8443);
    ResetConnectExpectations();
    ASSERT_EQ(odin_event_loop_create(&h.loop), 0) << std::strerror(errno);
    InstallOps();
  }

  void TearDown() override {
    if (h.rt != nullptr) {
      DestroyRunningRuntime();
    }
    ClearOps();
    odin_event_loop_destroy(h.loop);
  }

  void DestroyFixtureLoop() {
    odin_event_loop_destroy(h.loop);
    h.loop = nullptr;
  }

  void InstallOps() {
    g_client_harness = &h;
    odin_xqc_client_runtime_test_reset();
    static const odin_xqc_udp_test_ops_t kUdpOps = {
        FakeEngineCreate, FakeEngineDestroy, nullptr, nullptr,
        nullptr,          nullptr,           nullptr,
    };
    odin_xqc_udp_test_set_ops(&kUdpOps);
    static const odin_xqc_client_runtime_test_ops_t kClientOps = {
        FakeEngineRegisterAlpn, FakeEngineUnregisterAlpn,
        FakeXqcConnect,         FakeConnSetAlpUserData,
        FakeConnClose,          FakeGetConnAlpUserDataByStream,
        FakeStreamCreate,       FakeStreamClose,
        FakeUdpRegisterConn,    FakeUdpUnregisterConn,
    };
    odin_xqc_client_runtime_test_set_ops(&kClientOps);
    static const odin_xqc_stream_transport_test_ops_t kStreamOps = {
        FakeStreamRecv,
        FakeStreamSend,
        FakeStreamSetUserData,
    };
    odin_xqc_stream_transport_test_set_ops(&kStreamOps);
  }

  void ClearOps() {
    odin_xqc_stream_transport_test_set_ops(nullptr);
    odin_xqc_client_runtime_test_set_ops(nullptr);
    odin_xqc_udp_test_set_ops(nullptr);
    g_client_harness = nullptr;
  }

  void ResetFakeRuntimeState() {
    ASSERT_EQ(h.rt, nullptr);
    h.xu_user_data = nullptr;
    h.alpn_user_data = nullptr;
    h.app_callbacks = nullptr;
    h.conn_a = FakeConn();
    h.conn_b = FakeConn();
    h.conn_c = FakeConn();
    h.streams_to_create.clear();
    h.registered.clear();
    h.unregistered.clear();
    h.conn_close_cids.clear();
    h.engine_create_calls = 0;
    h.engine_destroy_calls = 0;
    h.engine_destroy_callback_close_calls = 0;
    h.alpn_unregister_calls = 0;
    h.connect_calls = 0;
    h.fail_register_errno = 0;
    h.fail_register_cid = 0;
    h.engine_create_returns_null = false;
    h.engine_create_errno = EIO;
    h.alpn_register_fails = false;
    h.connect_returns_null = false;
    h.connect_invokes_create = true;
    h.connect_invokes_close_before_null = false;
    h.stream_create_returns_null = false;
    h.stream_create_errno = 0;
    h.stale_errno_before_stream_create = false;
    h.engine_destroy_invokes_close = false;
    h.connect_uses_conn_b = false;
    h.event_order = 0;
    ResetConnectExpectations();
    InstallOps();
  }

  void SetExpectedPeer(const struct sockaddr *addr, socklen_t addrlen) {
    ASSERT_NE(addr, nullptr);
    ASSERT_LE(addrlen, static_cast<socklen_t>(sizeof(h.expected_peer_storage)));
    std::memset(&h.expected_peer_storage, 0, sizeof(h.expected_peer_storage));
    std::memcpy(&h.expected_peer_storage, addr, addrlen);
    h.expected_peer_addrlen = addrlen;
    h.expect_peer_bytes = true;
  }

  void ResetConnectExpectations() {
    h.expected_server_host = "odin.test";
    h.expected_token.clear();
    h.expect_token_bytes = false;
    h.expected_no_crypto_flag = 7;
    h.expected_conn_settings_ping_on = 0;
    h.expected_conn_settings_so_sndbuf = 0;
    h.expected_cert_verify_flag = 0;
    h.expected_session_ticket.clear();
    h.expect_session_ticket_bytes = false;
    h.expected_transport_parameters.clear();
    h.expect_transport_parameters_bytes = false;
    SetExpectedPeer(reinterpret_cast<const struct sockaddr *>(&h.peer_addr),
                    sizeof(h.peer_addr));
  }

  odin_xqc_client_runtime_config_t Config() {
    odin_xqc_client_runtime_config_t config;
    std::memset(&config, 0, sizeof(config));
    config.loop = h.loop;
    config.local_addr =
        reinterpret_cast<const struct sockaddr *>(&h.local_addr);
    config.local_addrlen = sizeof(h.local_addr);
    config.peer_addr = reinterpret_cast<const struct sockaddr *>(&h.peer_addr);
    config.peer_addrlen = sizeof(h.peer_addr);
    config.server_host = "odin.test";
    config.engine_config = &h.engine_config;
    config.engine_ssl_config = &h.engine_ssl_config;
    config.engine_callbacks = &h.engine_callbacks;
    config.transport_callbacks = &h.caller_transport_callbacks;
    config.conn_settings = &h.conn_settings;
    config.conn_ssl_config = &h.conn_ssl_config;
    config.no_crypto_flag = 7;
    return config;
  }

  void CreateRuntime() {
    odin_xqc_client_runtime_config_t config = Config();
    ASSERT_EQ(odin_xqc_client_runtime_create(&config, &h.rt), 0)
        << std::strerror(errno);
    ASSERT_NE(h.rt, nullptr);
    ASSERT_NE(h.app_callbacks, nullptr);
    ASSERT_NE(h.xu_user_data, nullptr);
  }

  void StartRuntime() {
    CreateRuntime();
    const int connect_before = h.connect_calls;
    ASSERT_EQ(odin_xqc_client_runtime_start(h.rt), 0) << std::strerror(errno);
    ASSERT_EQ(h.connect_calls, connect_before + 1);
  }

  void FireHandshake(FakeConn *conn = nullptr) {
    if (conn == nullptr) {
      conn = &h.conn_a;
    }
    h.app_callbacks->conn_cbs.conn_handshake_finished(AsConn(conn),
                                                      h.xu_user_data, h.rt);
  }

  void FireClose(FakeConn *conn = nullptr, const xqc_cid_t *cid = nullptr) {
    if (conn == nullptr) {
      conn = &h.conn_a;
    }
    if (cid == nullptr) {
      cid = &h.cid_a;
    }
    EXPECT_EQ(h.app_callbacks->conn_cbs.conn_close_notify(AsConn(conn), cid,
                                                          h.xu_user_data, h.rt),
              0);
    conn->alp_user_data = nullptr;
  }

  void DestroyRunningRuntime() {
    odin_xqc_client_runtime_destroy(h.rt);
    if (h.conn_a.alp_user_data != nullptr) {
      FireClose(&h.conn_a, &h.cid_a);
    }
    h.rt = nullptr;
  }

  void QueueStream(FakeStream *stream,
                   const std::string &resp = EncodedConnectResp()) {
    stream->recv_chunks.push_back(resp);
    h.streams_to_create.push_back(stream);
  }

  void AddParsedConnection(FakeStream *stream, int *peer_out) {
    QueueStream(stream);
    int owned = -1;
    int peer = -1;
    MakePair(&owned, &peer);
    ASSERT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned), 0)
        << std::strerror(errno);
    ASSERT_TRUE(WriteAllFd(peer, kHttpReq, sizeof(kHttpReq) - 1));
    RunLoopFor(h.loop);
    ASSERT_FALSE(stream->sends.empty());
    if (peer_out != nullptr) {
      *peer_out = peer;
    } else {
      close(peer);
    }
  }

  void AddStalledConnection(FakeStream *stream, int *peer_out) {
    h.streams_to_create.push_back(stream);
    int owned = -1;
    int peer = -1;
    MakePair(&owned, &peer);
    ASSERT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned), 0)
        << std::strerror(errno);
    ASSERT_TRUE(WriteAllFd(peer, kHttpReq, sizeof(kHttpReq) - 1));
    RunLoopFor(h.loop);
    ASSERT_FALSE(stream->sends.empty());
    EXPECT_EQ(JoinedSends(*stream), EncodedConnectReq());
    ASSERT_NE(stream->user_data, nullptr);
    if (peer_out != nullptr) {
      *peer_out = peer;
    } else {
      close(peer);
    }
  }
};

TEST_F(OdinXqcClientRuntimeTest, T1) {
  CreateRuntime();
  const odin_xqc_client_runtime_test_record_t *record =
      odin_xqc_client_runtime_test_record();
  ASSERT_EQ(record->udp_create_calls, 1u);
  EXPECT_EQ(record->last_udp_create.loop, h.loop);
  EXPECT_EQ(record->last_udp_create.local_addr,
            reinterpret_cast<const struct sockaddr *>(&h.local_addr));
  EXPECT_EQ(record->last_udp_create.local_addrlen, sizeof(h.local_addr));
  EXPECT_EQ(record->last_udp_create.engine_type, XQC_ENGINE_CLIENT);
  EXPECT_EQ(record->last_udp_create.engine_config, &h.engine_config);
  EXPECT_EQ(record->last_udp_create.ssl_config, &h.engine_ssl_config);
  EXPECT_EQ(record->last_udp_create.engine_callbacks, &h.engine_callbacks);
  EXPECT_EQ(record->last_udp_create.app_user_data, h.rt);
  EXPECT_EQ(record->last_udp_create.transport_callbacks_value.cert_verify_cb,
            CertVerifyOk);
  EXPECT_NE(record->last_udp_create.transport_callbacks_value.save_token,
            nullptr);
  EXPECT_NE(record->last_udp_create.transport_callbacks_value.save_session_cb,
            nullptr);
  EXPECT_NE(record->last_udp_create.transport_callbacks_value.save_tp_cb,
            nullptr);
  ASSERT_EQ(odin_xqc_client_runtime_start(h.rt), 0);
  EXPECT_EQ(h.connect_calls, 1);
  EXPECT_EQ(h.registered.size(), 1u);
  EXPECT_TRUE(CidEq(h.registered[0], h.cid_a));
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_XQC_CONNECT),
            1u);
  EXPECT_EQ(odin_xqc_client_runtime_start(h.rt), 0);
  EXPECT_EQ(h.connect_calls, 1);
  EXPECT_EQ(odin_xqc_client_runtime_stop(h.rt), 0);
  int owned = -1;
  int peer = -1;
  MakePair(&owned, &peer);
  errno = 0;
  EXPECT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned), -1);
  EXPECT_EQ(errno, ENOTCONN);
  EXPECT_GE(fcntl(owned, F_GETFD), 0);
  close(owned);
  close(peer);
  EXPECT_EQ(odin_xqc_client_runtime_stop(h.rt), 0);
  EXPECT_EQ(odin_xqc_client_runtime_start(h.rt), 0);
  EXPECT_EQ(h.connect_calls, 1);
  odin_xqc_client_runtime_destroy(h.rt);
  h.rt = nullptr;
  EXPECT_EQ(h.conn_close_cids.size(), 1u);
  FireClose(&h.conn_a, &h.cid_a);
  EXPECT_EQ(h.unregistered.size(), 1u);
  EXPECT_EQ(h.alpn_unregister_calls, 1);
  EXPECT_EQ(h.engine_destroy_calls, 1);
}

TEST_F(OdinXqcClientRuntimeTest, T2) {
  odin_xqc_client_runtime_t *sentinel =
      reinterpret_cast<odin_xqc_client_runtime_t *>(0x1234);
  errno = 0;
  EXPECT_EQ(odin_xqc_client_runtime_create(nullptr, &sentinel), -1);
  EXPECT_EQ(errno, EINVAL);
  EXPECT_EQ(sentinel, reinterpret_cast<odin_xqc_client_runtime_t *>(0x1234));

  odin_xqc_client_runtime_config_t config = Config();
  EXPECT_EQ(odin_xqc_client_runtime_create(&config, nullptr), -1);
  EXPECT_EQ(errno, EINVAL);

  auto expect_invalid_create =
      [&](const odin_xqc_client_runtime_config_t &bad_config) {
        odin_xqc_client_runtime_t *bad_out =
            reinterpret_cast<odin_xqc_client_runtime_t *>(0x2222);
        errno = 0;
        EXPECT_EQ(odin_xqc_client_runtime_create(&bad_config, &bad_out), -1);
        EXPECT_EQ(errno, EINVAL);
        EXPECT_EQ(bad_out,
                  reinterpret_cast<odin_xqc_client_runtime_t *>(0x2222));
      };

  odin_xqc_client_runtime_config_t invalid = config;
  invalid.loop = nullptr;
  expect_invalid_create(invalid);

  invalid = config;
  invalid.local_addr = nullptr;
  expect_invalid_create(invalid);

  invalid = config;
  invalid.peer_addr = nullptr;
  expect_invalid_create(invalid);

  invalid = config;
  invalid.server_host = nullptr;
  expect_invalid_create(invalid);

  invalid = config;
  invalid.engine_ssl_config = nullptr;
  sentinel = reinterpret_cast<odin_xqc_client_runtime_t *>(0x2222);
  EXPECT_EQ(odin_xqc_client_runtime_create(&invalid, &sentinel), -1);
  EXPECT_EQ(errno, EINVAL);
  EXPECT_EQ(odin_xqc_client_runtime_test_record()->udp_create_calls, 0u);

  invalid = config;
  invalid.engine_callbacks = nullptr;
  expect_invalid_create(invalid);

  invalid = config;
  invalid.conn_settings = nullptr;
  expect_invalid_create(invalid);

  invalid = config;
  invalid.conn_ssl_config = nullptr;
  expect_invalid_create(invalid);

  invalid = config;
  invalid.token_len = 1;
  invalid.token = nullptr;
  expect_invalid_create(invalid);

  invalid = config;
  invalid.token_len = 257;
  unsigned char token[257] = {};
  invalid.token = token;
  expect_invalid_create(invalid);

  invalid = config;
  invalid.peer_addrlen = 0;
  expect_invalid_create(invalid);

  invalid = config;
  invalid.peer_addrlen = 1;
  expect_invalid_create(invalid);

  invalid = config;
  invalid.peer_addrlen = sizeof(struct sockaddr) - 1;
  expect_invalid_create(invalid);

  char nested[4] = {'t', 'e', 's', 't'};
  xqc_conn_ssl_config_t bad_ssl = h.conn_ssl_config;
  bad_ssl.session_ticket_len = sizeof(nested);
  bad_ssl.session_ticket_data = nullptr;
  invalid = config;
  invalid.conn_ssl_config = &bad_ssl;
  expect_invalid_create(invalid);

  bad_ssl = h.conn_ssl_config;
  bad_ssl.transport_parameter_data_len = sizeof(nested);
  bad_ssl.transport_parameter_data = nullptr;
  invalid = config;
  invalid.conn_ssl_config = &bad_ssl;
  expect_invalid_create(invalid);

  invalid = config;
  invalid.peer_addrlen = sizeof(struct sockaddr_in) - 1;
  expect_invalid_create(invalid);

  invalid = config;
  invalid.peer_addrlen = sizeof(struct sockaddr_in) + 1;
  expect_invalid_create(invalid);

  invalid = config;
  invalid.peer_addrlen = sizeof(struct sockaddr_in6) + 1;
  expect_invalid_create(invalid);

  struct sockaddr invalid_family;
  std::memset(&invalid_family, 0, sizeof(invalid_family));
  invalid_family.sa_family = AF_UNIX;
  invalid = config;
  invalid.peer_addr = &invalid_family;
  invalid.peer_addrlen = sizeof(invalid_family);
  expect_invalid_create(invalid);

  struct sockaddr_in6 peer6 = Loopback6(8443);
  invalid = config;
  invalid.peer_addr = reinterpret_cast<const struct sockaddr *>(&peer6);
  invalid.peer_addrlen = sizeof(struct sockaddr_in6) - 1;
  expect_invalid_create(invalid);

  invalid = config;
  invalid.peer_addr = reinterpret_cast<const struct sockaddr *>(&peer6);
  invalid.peer_addrlen = sizeof(struct sockaddr_in6) + 1;
  expect_invalid_create(invalid);

  invalid = config;
  invalid.peer_addr = reinterpret_cast<const struct sockaddr *>(&peer6);
  invalid.peer_addrlen = sizeof(struct sockaddr_in);
  expect_invalid_create(invalid);

  invalid = config;
  invalid.peer_addrlen = sizeof(struct sockaddr_in6);
  expect_invalid_create(invalid);

  config.peer_addr = reinterpret_cast<const struct sockaddr *>(&peer6);
  config.peer_addrlen = sizeof(peer6);
  SetExpectedPeer(reinterpret_cast<const struct sockaddr *>(&peer6),
                  sizeof(peer6));
  ASSERT_EQ(odin_xqc_client_runtime_create(&config, &h.rt), 0)
      << std::strerror(errno);
  ASSERT_EQ(odin_xqc_client_runtime_start(h.rt), 0);
  DestroyRunningRuntime();
  InstallOps();
  ResetConnectExpectations();

  config = Config();
  config.engine_config = nullptr;
  ASSERT_EQ(odin_xqc_client_runtime_create(&config, &h.rt), 0)
      << std::strerror(errno);
  EXPECT_EQ(
      odin_xqc_client_runtime_test_record()->last_udp_create.engine_config,
      nullptr);
  ASSERT_EQ(odin_xqc_client_runtime_start(h.rt), 0);
  DestroyRunningRuntime();
  InstallOps();

  config = Config();
  h.engine_config.sendmmsg_on = 1;
  const int engine_create_before_sendmmsg = h.engine_create_calls;
  sentinel = reinterpret_cast<odin_xqc_client_runtime_t *>(0x2424);
  errno = 0;
  EXPECT_EQ(odin_xqc_client_runtime_create(&config, &sentinel), -1);
  EXPECT_EQ(errno, ENOTSUP);
  EXPECT_EQ(sentinel, reinterpret_cast<odin_xqc_client_runtime_t *>(0x2424));
  ASSERT_EQ(odin_xqc_client_runtime_test_record()->udp_create_calls, 1u);
  EXPECT_EQ(odin_xqc_client_runtime_test_record()->last_udp_create.loop,
            h.loop);
  EXPECT_EQ(odin_xqc_client_runtime_test_record()->last_udp_create.local_addr,
            reinterpret_cast<const struct sockaddr *>(&h.local_addr));
  EXPECT_EQ(
      odin_xqc_client_runtime_test_record()->last_udp_create.local_addrlen,
      sizeof(h.local_addr));
  EXPECT_EQ(odin_xqc_client_runtime_test_record()->last_udp_create.engine_type,
            XQC_ENGINE_CLIENT);
  EXPECT_EQ(
      odin_xqc_client_runtime_test_record()->last_udp_create.engine_config,
      &h.engine_config);
  EXPECT_EQ(odin_xqc_client_runtime_test_record()->last_udp_create.ssl_config,
            &h.engine_ssl_config);
  EXPECT_EQ(
      odin_xqc_client_runtime_test_record()->last_udp_create.engine_callbacks,
      &h.engine_callbacks);
  EXPECT_EQ(h.engine_create_calls, engine_create_before_sendmmsg);
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_REGISTER_ALPN),
      0u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY),
            0u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_XQC_CONNECT),
            0u);
  h.engine_config.sendmmsg_on = 0;
  InstallOps();

  config = Config();
  h.conn_ssl_config.cert_verify_flag = XQC_TLS_CERT_FLAG_NEED_VERIFY;
  config.transport_callbacks = nullptr;
  EXPECT_EQ(odin_xqc_client_runtime_create(&config, &sentinel), -1);
  EXPECT_EQ(errno, EINVAL);
  xqc_transport_callbacks_t null_cert_callbacks;
  std::memset(&null_cert_callbacks, 0, sizeof(null_cert_callbacks));
  config.transport_callbacks = &null_cert_callbacks;
  EXPECT_EQ(odin_xqc_client_runtime_create(&config, &sentinel), -1);
  EXPECT_EQ(errno, EINVAL);
  h.conn_ssl_config.cert_verify_flag = 0;

  config = Config();
  config.transport_callbacks = nullptr;
  ASSERT_EQ(odin_xqc_client_runtime_create(&config, &h.rt), 0)
      << std::strerror(errno);
  const odin_xqc_client_runtime_test_record_t *defaulted_record =
      odin_xqc_client_runtime_test_record();
  ASSERT_EQ(defaulted_record->udp_create_calls, 1u);
  EXPECT_EQ(defaulted_record->last_udp_create.loop, h.loop);
  EXPECT_EQ(defaulted_record->last_udp_create.local_addr,
            reinterpret_cast<const struct sockaddr *>(&h.local_addr));
  EXPECT_EQ(defaulted_record->last_udp_create.local_addrlen,
            sizeof(h.local_addr));
  EXPECT_EQ(defaulted_record->last_udp_create.engine_type, XQC_ENGINE_CLIENT);
  EXPECT_EQ(defaulted_record->last_udp_create.engine_config, &h.engine_config);
  EXPECT_EQ(defaulted_record->last_udp_create.ssl_config, &h.engine_ssl_config);
  EXPECT_EQ(defaulted_record->last_udp_create.engine_callbacks,
            &h.engine_callbacks);
  EXPECT_EQ(defaulted_record->last_udp_create.app_user_data, h.rt);
  xqc_transport_callbacks_t defaulted_callbacks =
      defaulted_record->last_udp_create.transport_callbacks_value;
  EXPECT_NE(defaulted_callbacks.save_token, nullptr);
  EXPECT_NE(defaulted_callbacks.save_session_cb, nullptr);
  EXPECT_NE(defaulted_callbacks.save_tp_cb, nullptr);
  EXPECT_EQ(defaulted_callbacks.cert_verify_cb, nullptr);
  const unsigned int calls_before_noops =
      odin_xqc_client_runtime_test_record()->call_count;
  errno = EDOM;
  defaulted_callbacks.save_token(reinterpret_cast<const unsigned char *>("tok"),
                                 3, h.xu_user_data);
  EXPECT_EQ(errno, EDOM);
  defaulted_callbacks.save_session_cb("session", 7, h.xu_user_data);
  EXPECT_EQ(errno, EDOM);
  defaulted_callbacks.save_tp_cb("tp", 2, h.xu_user_data);
  EXPECT_EQ(errno, EDOM);
  EXPECT_EQ(odin_xqc_client_runtime_test_record()->call_count,
            calls_before_noops);
  odin_xqc_client_runtime_destroy(h.rt);
  h.rt = nullptr;
  InstallOps();

  config = Config();
  h.caller_transport_callbacks.save_token = SaveTokenSentinel;
  h.caller_transport_callbacks.save_session_cb = SaveStringSentinel;
  h.caller_transport_callbacks.save_tp_cb = SaveStringSentinel;
  h.caller_transport_callbacks.cert_verify_cb = CertVerifyOk;
  ASSERT_EQ(odin_xqc_client_runtime_create(&config, &h.rt), 0)
      << std::strerror(errno);
  xqc_transport_callbacks_t preserved_callbacks =
      odin_xqc_client_runtime_test_record()
          ->last_udp_create.transport_callbacks_value;
  EXPECT_EQ(preserved_callbacks.save_token, SaveTokenSentinel);
  EXPECT_EQ(preserved_callbacks.save_session_cb, SaveStringSentinel);
  EXPECT_EQ(preserved_callbacks.save_tp_cb, SaveStringSentinel);
  EXPECT_EQ(preserved_callbacks.cert_verify_cb, CertVerifyOk);
  odin_xqc_client_runtime_destroy(h.rt);
  h.rt = nullptr;
  std::memset(&h.caller_transport_callbacks, 0,
              sizeof(h.caller_transport_callbacks));
  h.caller_transport_callbacks.cert_verify_cb = CertVerifyOk;
  InstallOps();

  CreateRuntime();
  odin_xqc_client_runtime_destroy(h.rt);
  h.rt = nullptr;
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE), 0u);
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN),
      0u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_STOP), 0u);
  EXPECT_EQ(CountClientCalls(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN),
            1u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY),
            1u);
  InstallOps();

  EXPECT_EQ(odin_xqc_client_runtime_start(nullptr), -1);
  EXPECT_EQ(errno, EINVAL);
  EXPECT_EQ(odin_xqc_client_runtime_stop(nullptr), -1);
  EXPECT_EQ(errno, EINVAL);
  odin_xqc_client_runtime_destroy(nullptr);

  CreateRuntime();
  const unsigned int stop_before_start_udp_stop_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_STOP);
  const int stop_before_start_connect_before = h.connect_calls;
  ASSERT_EQ(odin_xqc_client_runtime_stop(h.rt), 0);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_STOP),
            stop_before_start_udp_stop_before);
  ASSERT_EQ(odin_xqc_client_runtime_start(h.rt), 0) << std::strerror(errno);
  EXPECT_EQ(h.connect_calls, stop_before_start_connect_before + 1);
  DestroyRunningRuntime();
  InstallOps();

  config = Config();
  ASSERT_EQ(odin_xqc_client_runtime_test_fail_config_copy_alloc(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_SERVER_HOST, ENOMEM),
            0);
  sentinel = reinterpret_cast<odin_xqc_client_runtime_t *>(0x3333);
  EXPECT_EQ(odin_xqc_client_runtime_create(&config, &sentinel), -1);
  EXPECT_EQ(errno, ENOMEM);
  EXPECT_EQ(sentinel, reinterpret_cast<odin_xqc_client_runtime_t *>(0x3333));
  EXPECT_EQ(odin_xqc_client_runtime_test_record()->udp_create_calls, 0u);

  unsigned char token256[256] = {};
  for (size_t i = 0; i < sizeof(token256); ++i) {
    token256[i] = static_cast<unsigned char>(i);
  }
  config = Config();
  config.token = token256;
  config.token_len = sizeof(token256);
  h.expected_token.assign(token256, token256 + sizeof(token256));
  h.expect_token_bytes = true;
  ASSERT_EQ(odin_xqc_client_runtime_create(&config, &h.rt), 0)
      << std::strerror(errno);
  ASSERT_EQ(odin_xqc_client_runtime_start(h.rt), 0);
  DestroyRunningRuntime();
  InstallOps();
  ResetConnectExpectations();

  {
    std::string mutable_host = "odin.test";
    std::vector<unsigned char> mutable_token = {9, 8, 7, 6};
    struct sockaddr_in mutable_peer = Loopback4(9443);
    std::string ticket_bytes = "ticket-copy";
    std::string tp_bytes = "tp-copy";
    xqc_conn_settings_t mutable_settings = h.conn_settings;
    mutable_settings.ping_on = 1;
    mutable_settings.so_sndbuf = 12345;
    xqc_conn_ssl_config_t mutable_ssl = h.conn_ssl_config;
    mutable_ssl.session_ticket_data = &ticket_bytes[0];
    mutable_ssl.session_ticket_len = ticket_bytes.size();
    mutable_ssl.transport_parameter_data = &tp_bytes[0];
    mutable_ssl.transport_parameter_data_len = tp_bytes.size();
    mutable_ssl.cert_verify_flag = XQC_TLS_CERT_FLAG_ALLOW_SELF_SIGNED;

    config = Config();
    config.server_host = mutable_host.c_str();
    config.token = mutable_token.data();
    config.token_len = static_cast<unsigned int>(mutable_token.size());
    config.peer_addr = reinterpret_cast<const struct sockaddr *>(&mutable_peer);
    config.peer_addrlen = sizeof(mutable_peer);
    config.conn_settings = &mutable_settings;
    config.conn_ssl_config = &mutable_ssl;
    config.no_crypto_flag = 23;
    h.expected_token = mutable_token;
    h.expect_token_bytes = true;
    h.expected_no_crypto_flag = 23;
    h.expected_conn_settings_ping_on = 1;
    h.expected_conn_settings_so_sndbuf = 12345;
    h.expected_cert_verify_flag = XQC_TLS_CERT_FLAG_ALLOW_SELF_SIGNED;
    h.expected_session_ticket = ticket_bytes;
    h.expect_session_ticket_bytes = true;
    h.expected_transport_parameters = tp_bytes;
    h.expect_transport_parameters_bytes = true;
    SetExpectedPeer(reinterpret_cast<const struct sockaddr *>(&mutable_peer),
                    sizeof(mutable_peer));

    ASSERT_EQ(odin_xqc_client_runtime_create(&config, &h.rt), 0)
        << std::strerror(errno);
    mutable_host = "bad.test";
    std::fill(mutable_token.begin(), mutable_token.end(), 0xEE);
    mutable_token.clear();
    mutable_token.shrink_to_fit();
    mutable_peer = Loopback4(9555);
    mutable_settings.ping_on = 0;
    mutable_settings.so_sndbuf = 54321;
    mutable_ssl.cert_verify_flag = 0;
    std::fill(ticket_bytes.begin(), ticket_bytes.end(), 'x');
    ticket_bytes.clear();
    ticket_bytes.shrink_to_fit();
    std::fill(tp_bytes.begin(), tp_bytes.end(), 'y');
    tp_bytes.clear();
    tp_bytes.shrink_to_fit();
    config.no_crypto_flag = 99;

    ASSERT_EQ(odin_xqc_client_runtime_start(h.rt), 0) << std::strerror(errno);
    DestroyRunningRuntime();
    InstallOps();
    ResetConnectExpectations();
  }

  unsigned char token4[4] = {1, 2, 3, 4};
  char ticket[] = "ticket";
  char tp[] = "transport";
  xqc_conn_ssl_config_t ssl_with_nested = h.conn_ssl_config;
  ssl_with_nested.session_ticket_data = ticket;
  ssl_with_nested.session_ticket_len = sizeof(ticket);
  ssl_with_nested.transport_parameter_data = tp;
  ssl_with_nested.transport_parameter_data_len = sizeof(tp);

  config = Config();
  config.token = token4;
  config.token_len = sizeof(token4);
  config.conn_ssl_config = &ssl_with_nested;
  ASSERT_EQ(odin_xqc_client_runtime_test_fail_config_copy_alloc(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_TOKEN, ENOMEM),
            0);
  sentinel = reinterpret_cast<odin_xqc_client_runtime_t *>(0x3434);
  EXPECT_EQ(odin_xqc_client_runtime_create(&config, &sentinel), -1);
  EXPECT_EQ(errno, ENOMEM);
  EXPECT_EQ(sentinel, reinterpret_cast<odin_xqc_client_runtime_t *>(0x3434));

  ASSERT_EQ(
      odin_xqc_client_runtime_test_fail_config_copy_alloc(
          ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_SESSION_TICKET, ENOMEM),
      0);
  sentinel = reinterpret_cast<odin_xqc_client_runtime_t *>(0x3535);
  EXPECT_EQ(odin_xqc_client_runtime_create(&config, &sentinel), -1);
  EXPECT_EQ(errno, ENOMEM);
  EXPECT_EQ(sentinel, reinterpret_cast<odin_xqc_client_runtime_t *>(0x3535));

  ASSERT_EQ(odin_xqc_client_runtime_test_fail_config_copy_alloc(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_TRANSPORT_PARAMETERS,
                ENOMEM),
            0);
  sentinel = reinterpret_cast<odin_xqc_client_runtime_t *>(0x3636);
  EXPECT_EQ(odin_xqc_client_runtime_create(&config, &sentinel), -1);
  EXPECT_EQ(errno, ENOMEM);
  EXPECT_EQ(sentinel, reinterpret_cast<odin_xqc_client_runtime_t *>(0x3636));

  EXPECT_EQ(
      odin_xqc_client_runtime_test_fail_config_copy_alloc(
          static_cast<odin_xqc_client_runtime_test_config_copy_alloc_t>(99),
          ENOMEM),
      -1);
  EXPECT_EQ(errno, EINVAL);
  const unsigned int udp_before_invalid_site =
      odin_xqc_client_runtime_test_record()->udp_create_calls;
  config = Config();
  ASSERT_EQ(odin_xqc_client_runtime_create(&config, &h.rt), 0)
      << std::strerror(errno);
  EXPECT_EQ(odin_xqc_client_runtime_test_record()->udp_create_calls,
            udp_before_invalid_site + 1);
  odin_xqc_client_runtime_destroy(h.rt);
  h.rt = nullptr;
  InstallOps();

  ASSERT_EQ(odin_xqc_client_runtime_test_fail_config_copy_alloc(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_TOKEN, ENOMEM),
            0);
  config = Config();
  config.token = nullptr;
  config.token_len = 0;
  ASSERT_EQ(odin_xqc_client_runtime_create(&config, &h.rt), 0)
      << std::strerror(errno);
  const unsigned int udp_after_unreached_token =
      odin_xqc_client_runtime_test_record()->udp_create_calls;
  odin_xqc_client_runtime_destroy(h.rt);
  h.rt = nullptr;
  unsigned char token1[1] = {0xAB};
  config = Config();
  config.token = token1;
  config.token_len = sizeof(token1);
  sentinel = reinterpret_cast<odin_xqc_client_runtime_t *>(0x3737);
  errno = 0;
  EXPECT_EQ(odin_xqc_client_runtime_create(&config, &sentinel), -1);
  EXPECT_EQ(errno, ENOMEM);
  EXPECT_EQ(sentinel, reinterpret_cast<odin_xqc_client_runtime_t *>(0x3737));
  EXPECT_EQ(odin_xqc_client_runtime_test_record()->udp_create_calls,
            udp_after_unreached_token);
  InstallOps();

  h.engine_create_returns_null = true;
  h.engine_create_errno = EIO;
  sentinel = reinterpret_cast<odin_xqc_client_runtime_t *>(0x4444);
  EXPECT_EQ(odin_xqc_client_runtime_create(&config, &sentinel), -1);
  EXPECT_EQ(errno, EIO);
  EXPECT_EQ(sentinel, reinterpret_cast<odin_xqc_client_runtime_t *>(0x4444));
  h.engine_create_returns_null = false;

  h.alpn_register_fails = true;
  const int engine_destroy_before_alpn_failure = h.engine_destroy_calls;
  sentinel = reinterpret_cast<odin_xqc_client_runtime_t *>(0x4545);
  EXPECT_EQ(odin_xqc_client_runtime_create(&config, &sentinel), -1);
  EXPECT_EQ(errno, EIO);
  EXPECT_EQ(sentinel, reinterpret_cast<odin_xqc_client_runtime_t *>(0x4545));
  EXPECT_EQ(h.engine_destroy_calls, engine_destroy_before_alpn_failure + 1);
  h.alpn_register_fails = false;

  config = Config();
#if defined(__APPLE__)
  CreateRuntime();
  const int kqueue_connect_before = h.connect_calls;
  ASSERT_EQ(
      odin_event_loop_test_fail_next_kqueue_change(
          h.loop, ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_ADD, ODIN_EVENT_READ, EIO),
      0);
  errno = 0;
  EXPECT_EQ(odin_xqc_client_runtime_start(h.rt), -1);
  EXPECT_EQ(errno, EIO);
  EXPECT_EQ(h.connect_calls, kqueue_connect_before);
  int failed_start_owned = -1;
  int failed_start_peer = -1;
  MakePair(&failed_start_owned, &failed_start_peer);
  errno = 0;
  EXPECT_EQ(odin_xqc_client_runtime_add_connection(h.rt, failed_start_owned),
            -1);
  EXPECT_EQ(errno, ENOTCONN);
  EXPECT_GE(fcntl(failed_start_owned, F_GETFD), 0);
  close(failed_start_owned);
  close(failed_start_peer);
  ASSERT_EQ(odin_xqc_client_runtime_start(h.rt), 0) << std::strerror(errno);
  EXPECT_EQ(h.connect_calls, kqueue_connect_before + 1);
  ASSERT_EQ(odin_xqc_client_runtime_stop(h.rt), 0);
  ASSERT_EQ(
      odin_event_loop_test_fail_next_kqueue_change(
          h.loop, ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_ADD, ODIN_EVENT_READ, EIO),
      0);
  errno = 0;
  EXPECT_EQ(odin_xqc_client_runtime_start(h.rt), -1);
  EXPECT_EQ(errno, EIO);
  EXPECT_EQ(h.connect_calls, kqueue_connect_before + 1);
  odin_xqc_client_runtime_test_state_t restart_failed;
  ASSERT_EQ(odin_xqc_client_runtime_test_state(h.rt, &restart_failed), 0);
  EXPECT_EQ(restart_failed.conn, AsConn(&h.conn_a));
  EXPECT_EQ(restart_failed.cid_registered, 1);
  EXPECT_EQ(restart_failed.handshake_done, 0);
  EXPECT_EQ(restart_failed.closing, 0);
  EXPECT_TRUE(CidEq(restart_failed.current_cid, h.cid_a));
  EXPECT_EQ(CountClientCallsForCid(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_REGISTER_CONN, h.cid_a),
            1u);
  EXPECT_EQ(CountClientCallsForCid(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a),
            0u);
  int restart_failed_owned = -1;
  int restart_failed_peer = -1;
  MakePair(&restart_failed_owned, &restart_failed_peer);
  errno = 0;
  EXPECT_EQ(odin_xqc_client_runtime_add_connection(h.rt, restart_failed_owned),
            -1);
  EXPECT_EQ(errno, ENOTCONN);
  EXPECT_GE(fcntl(restart_failed_owned, F_GETFD), 0);
  close(restart_failed_owned);
  close(restart_failed_peer);
  ASSERT_EQ(odin_xqc_client_runtime_start(h.rt), 0) << std::strerror(errno);
  EXPECT_EQ(h.connect_calls, kqueue_connect_before + 1);
  DestroyRunningRuntime();
  InstallOps();
#endif

  config = Config();
  h.connect_returns_null = true;
  h.connect_invokes_create = false;
  CreateRuntime();
  EXPECT_EQ(odin_xqc_client_runtime_start(h.rt), -1);
  EXPECT_EQ(errno, EIO);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_STOP), 1u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE), 0u);
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN),
      0u);
  odin_xqc_client_runtime_test_state_t failed_no_callback_destroy;
  ASSERT_EQ(
      odin_xqc_client_runtime_test_state(h.rt, &failed_no_callback_destroy), 0);
  EXPECT_EQ(failed_no_callback_destroy.conn, nullptr);
  EXPECT_EQ(failed_no_callback_destroy.cid_registered, 0);
  EXPECT_EQ(failed_no_callback_destroy.handshake_done, 0);
  const unsigned int no_callback_destroy_alpn_before = CountClientCalls(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN);
  const unsigned int no_callback_destroy_udp_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY);
  const unsigned int no_callback_destroy_close_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE);
  const unsigned int no_callback_destroy_unregister_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN);
  odin_xqc_client_runtime_destroy(h.rt);
  h.rt = nullptr;
  EXPECT_EQ(CountClientCalls(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN),
            no_callback_destroy_alpn_before + 1u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY),
            no_callback_destroy_udp_before + 1u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE),
            no_callback_destroy_close_before);
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN),
      no_callback_destroy_unregister_before);
  ResetFakeRuntimeState();

  h.connect_returns_null = true;
  h.connect_invokes_create = false;
  const int no_callback_retry_connect_before = h.connect_calls;
  CreateRuntime();
  EXPECT_EQ(odin_xqc_client_runtime_start(h.rt), -1);
  EXPECT_EQ(errno, EIO);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_STOP), 1u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE), 0u);
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN),
      0u);
  odin_xqc_client_runtime_test_state_t failed_no_callback;
  ASSERT_EQ(odin_xqc_client_runtime_test_state(h.rt, &failed_no_callback), 0);
  EXPECT_EQ(failed_no_callback.conn, nullptr);
  EXPECT_EQ(failed_no_callback.cid_registered, 0);
  EXPECT_EQ(failed_no_callback.handshake_done, 0);
  h.connect_returns_null = false;
  h.connect_invokes_create = true;
  ASSERT_EQ(odin_xqc_client_runtime_start(h.rt), 0) << std::strerror(errno);
  EXPECT_EQ(h.connect_calls, no_callback_retry_connect_before + 2);
  EXPECT_EQ(h.conn_a.alp_user_data, h.rt);
  EXPECT_EQ(CountClientCallsForCid(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_REGISTER_CONN, h.cid_a),
            1u);
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN),
      0u);
  DestroyRunningRuntime();
  h.rt = nullptr;
  ResetFakeRuntimeState();

  h.fail_register_errno = ENOMEM;
  h.fail_register_cid = h.cid_a.cid_buf[0];
  const int register_failure_connect_before = h.connect_calls;
  CreateRuntime();
  errno = 0;
  EXPECT_EQ(odin_xqc_client_runtime_start(h.rt), -1);
  EXPECT_EQ(errno, ENOMEM);
  EXPECT_EQ(h.registered.size(), 0u);
  EXPECT_EQ(CountClientCallsForCid(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_REGISTER_CONN, h.cid_a),
            1u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE), 0u);
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN),
      0u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_STOP), 1u);
  odin_xqc_client_runtime_test_state_t failed_register;
  ASSERT_EQ(odin_xqc_client_runtime_test_state(h.rt, &failed_register), 0);
  EXPECT_EQ(failed_register.conn, nullptr);
  EXPECT_EQ(failed_register.cid_registered, 0);
  EXPECT_EQ(failed_register.handshake_done, 0);
  h.fail_register_errno = 0;
  h.fail_register_cid = 0;
  ASSERT_EQ(odin_xqc_client_runtime_start(h.rt), 0) << std::strerror(errno);
  EXPECT_EQ(h.connect_calls, register_failure_connect_before + 2);
  EXPECT_EQ(h.conn_a.alp_user_data, h.rt);
  EXPECT_EQ(h.registered.size(), 1u);
  EXPECT_TRUE(CidEq(h.registered[0], h.cid_a));
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN),
      0u);
  DestroyRunningRuntime();
  h.rt = nullptr;
  ResetFakeRuntimeState();

  h.connect_returns_null = true;
  h.connect_invokes_create = true;
  h.connect_invokes_close_before_null = false;
  const int post_create_connect_before = h.connect_calls;
  const size_t post_create_close_before = h.conn_close_cids.size();
  CreateRuntime();
  EXPECT_EQ(odin_xqc_client_runtime_start(h.rt), -1);
  EXPECT_EQ(errno, EIO);
  EXPECT_EQ(h.conn_close_cids.size(), post_create_close_before + 1u);
  EXPECT_TRUE(CidEq(h.conn_close_cids.back(), h.cid_a));
  EXPECT_EQ(CountClientCallsForCid(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a),
            1u);
  h.connect_returns_null = false;
  h.connect_uses_conn_b = true;
  ASSERT_EQ(odin_xqc_client_runtime_start(h.rt), 0);
  EXPECT_EQ(h.connect_calls, post_create_connect_before + 2);
  EXPECT_EQ(h.conn_b.alp_user_data, h.rt);
  const unsigned int unregister_after_retry =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN);
  EXPECT_EQ(h.app_callbacks->conn_cbs.conn_close_notify(
                AsConn(&h.conn_a), &h.cid_a, h.xu_user_data, h.rt),
            0);
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN),
      unregister_after_retry);
  EXPECT_EQ(CountClientCallsForCid(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a),
            1u);
  odin_xqc_client_runtime_destroy(h.rt);
  FireClose(&h.conn_b, &h.cid_b);
  h.rt = nullptr;
  InstallOps();
  h.connect_uses_conn_b = false;

  h.connect_returns_null = true;
  h.connect_invokes_create = true;
  h.connect_invokes_close_before_null = true;
  const int close_before_null_connect_before = h.connect_calls;
  CreateRuntime();
  EXPECT_EQ(odin_xqc_client_runtime_start(h.rt), -1);
  EXPECT_EQ(errno, EIO);
  EXPECT_EQ(CountClientCallsForCid(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a),
            1u);
  h.connect_returns_null = false;
  h.connect_invokes_close_before_null = false;
  h.connect_uses_conn_b = true;
  ASSERT_EQ(odin_xqc_client_runtime_start(h.rt), 0);
  EXPECT_EQ(h.connect_calls, close_before_null_connect_before + 2);
  odin_xqc_client_runtime_destroy(h.rt);
  FireClose(&h.conn_b, &h.cid_b);
  h.rt = nullptr;
}

TEST_F(OdinXqcClientRuntimeTest, T3) {
  StartRuntime();
  const xqc_cid_t cid_b = h.cid_b;
  h.transport_callbacks.conn_update_cid_notify(AsConn(&h.conn_a), &h.cid_a,
                                               &cid_b, h.xu_user_data);
  ASSERT_EQ(h.registered.size(), 2u);
  ASSERT_EQ(h.unregistered.size(), 1u);
  EXPECT_TRUE(CidEq(h.registered[1], cid_b));
  EXPECT_TRUE(CidEq(h.unregistered[0], h.cid_a));
  FireClose(&h.conn_a, &cid_b);
  EXPECT_TRUE(CidEq(h.unregistered.back(), cid_b));
  odin_xqc_client_runtime_destroy(h.rt);
  h.rt = nullptr;

  h.registered.clear();
  h.unregistered.clear();
  h.conn_close_cids.clear();
  h.event_order = 0;
  InstallOps();

  StartRuntime();
  FireHandshake();
  FakeStream stream;
  int peer = -1;
  AddParsedConnection(&stream, &peer);
  ASSERT_NE(stream.user_data, nullptr);
  void *stale_stream_user_data = stream.user_data;
  h.fail_register_errno = ENOMEM;
  h.fail_register_cid = h.cid_b.cid_buf[0];
  const unsigned int stream_close_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE);
  h.transport_callbacks.conn_update_cid_notify(AsConn(&h.conn_a), &h.cid_a,
                                               &h.cid_b, h.xu_user_data);
  ASSERT_EQ(h.registered.size(), 1u);
  ASSERT_EQ(h.unregistered.size(), 1u);
  ASSERT_EQ(h.conn_close_cids.size(), 1u);
  EXPECT_TRUE(CidEq(h.registered[0], h.cid_a));
  EXPECT_TRUE(CidEq(h.unregistered[0], h.cid_a));
  EXPECT_TRUE(CidEq(h.conn_close_cids[0], h.cid_a));
  EXPECT_EQ(stream.user_data_values.back(), nullptr);
  EXPECT_EQ(stream.close_calls, 1);
  EXPECT_GT(stream.user_data_clear_order, 0);
  EXPECT_GT(stream.close_order, stream.user_data_clear_order);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE),
            stream_close_before + 1);

  const int recv_calls = stream.recv_calls;
  const int send_calls = stream.send_calls;
  const int close_calls = stream.close_calls;
  const size_t unregisters = h.unregistered.size();
  const size_t conn_closes = h.conn_close_cids.size();
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(
                AsStream(&stream), stale_stream_user_data),
            XQC_OK);
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_write_notify(
                AsStream(&stream), stale_stream_user_data),
            XQC_OK);
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_close_notify(
                AsStream(&stream), stale_stream_user_data),
            XQC_OK);
  h.app_callbacks->stream_cbs.stream_closing_notify(
      AsStream(&stream), -XQC_ESTREAM_RESET, stale_stream_user_data);
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(AsStream(&stream),
                                                           nullptr),
            XQC_OK);
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_write_notify(AsStream(&stream),
                                                            nullptr),
            XQC_OK);
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_close_notify(AsStream(&stream),
                                                            nullptr),
            XQC_OK);
  h.app_callbacks->stream_cbs.stream_closing_notify(
      AsStream(&stream), -XQC_ESTREAM_RESET, nullptr);
  EXPECT_EQ(stream.recv_calls, recv_calls);
  EXPECT_EQ(stream.send_calls, send_calls);
  EXPECT_EQ(stream.close_calls, close_calls);
  EXPECT_EQ(h.unregistered.size(), unregisters);
  EXPECT_EQ(h.conn_close_cids.size(), conn_closes);

  const size_t registered = h.registered.size();
  h.transport_callbacks.conn_update_cid_notify(AsConn(&h.conn_a), &h.cid_a,
                                               &h.cid_c, h.xu_user_data);
  h.transport_callbacks.conn_update_cid_notify(AsConn(&h.conn_b), &h.cid_a,
                                               &h.cid_c, h.xu_user_data);
  EXPECT_EQ(h.registered.size(), registered);
  EXPECT_EQ(h.unregistered.size(), unregisters);
  EXPECT_EQ(h.conn_close_cids.size(), conn_closes);
  EXPECT_EQ(stream.close_calls, close_calls);
  FireClose(&h.conn_a, &h.cid_a);
  odin_xqc_client_runtime_destroy(h.rt);
  h.rt = nullptr;
  close(peer);
}

TEST_F(OdinXqcClientRuntimeTest, T5) {
  StartRuntime();
  int owned = -1;
  int peer = -1;
  MakePair(&owned, &peer);
  ASSERT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned), 0);
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CREATE_BIDI),
      0u);
  FakeStream stream;
  const std::string local_tail = "client-tail!";
  const std::string server_tail = "server-tail!";
  stream.recv_chunks.push_back(EncodedConnectResp());
  stream.recv_chunks.push_back(server_tail);
  h.streams_to_create.push_back(&stream);
  ASSERT_TRUE(WriteAllFd(peer, kHttpReq, sizeof(kHttpReq) - 1));
  ASSERT_TRUE(WriteAllFd(peer, local_tail.data(), local_tail.size()));
  FireHandshake();
  ASSERT_TRUE(RunUntil(h.loop, [&] {
    return stream.user_data != nullptr && !stream.sends.empty();
  }));
  EXPECT_EQ(ReadAvailableFd(peer, 50).find(kHttp200), std::string::npos);
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(AsStream(&stream),
                                                           stream.user_data),
            XQC_OK);
  std::string downstream;
  ASSERT_TRUE(RunUntil(h.loop, [&] {
    downstream += DrainFdNow(peer);
    return downstream.find(kHttp200) != std::string::npos &&
           JoinedSends(stream).find(local_tail) != std::string::npos;
  }));
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(AsStream(&stream),
                                                           stream.user_data),
            XQC_OK);
  ASSERT_TRUE(RunUntil(h.loop, [&] {
    downstream += DrainFdNow(peer);
    return downstream.find(std::string(kHttp200) + server_tail) !=
           std::string::npos;
  }));
  ASSERT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CREATE_BIDI),
      1u);
  ASSERT_FALSE(stream.sends.empty());
  EXPECT_EQ(stream.sends.front(), EncodedConnectReq());
  EXPECT_EQ(downstream.find(std::string(kHttp200) + server_tail), 0u);
  EXPECT_NE(JoinedSends(stream).find(local_tail), std::string::npos);
  ASSERT_NE(stream.user_data, nullptr);
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_close_notify(AsStream(&stream),
                                                            stream.user_data),
            XQC_OK);
  EXPECT_EQ(stream.user_data_values.back(), nullptr);
  EXPECT_EQ(stream.close_calls, 0);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE),
            0u);
  DestroyRunningRuntime();
  close(peer);
}

TEST_F(OdinXqcClientRuntimeTest, T6) {
  StartRuntime();
  FireHandshake();
  FakeStream stream;
  const std::string connect_req = EncodedConnectReq();
  const std::string connect_resp = EncodedConnectResp();
  stream.send_results.push_back(7);
  stream.send_results.push_back(-XQC_EAGAIN);
  stream.recv_chunks.push_back(connect_resp.substr(0, 2));
  h.streams_to_create.push_back(&stream);
  int peer = -1;
  int owned = -1;
  MakePair(&owned, &peer);
  ASSERT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned), 0);
  ASSERT_TRUE(WriteAllFd(peer, kHttpReq, sizeof(kHttpReq) - 1));
  RunLoopFor(h.loop);
  ASSERT_NE(stream.user_data, nullptr);
  EXPECT_EQ(stream.send_calls, 2);
  EXPECT_EQ(JoinedSends(stream), connect_req.substr(0, 7));
  EXPECT_EQ(ReadAvailableFd(peer, 50).find(kHttp200), std::string::npos);

  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_write_notify(AsStream(&stream),
                                                            stream.user_data),
            XQC_OK);
  EXPECT_EQ(JoinedSends(stream), connect_req);
  EXPECT_EQ(ReadAvailableFd(peer, 50).find(kHttp200), std::string::npos);

  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(AsStream(&stream),
                                                           stream.user_data),
            XQC_OK);
  EXPECT_EQ(ReadAvailableFd(peer, 50).find(kHttp200), std::string::npos);
  stream.recv_chunks.push_back(connect_resp.substr(2));
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(AsStream(&stream),
                                                           stream.user_data),
            XQC_OK);
  std::string downstream;
  ASSERT_TRUE(RunUntil(h.loop, [&] {
    downstream += DrainFdNow(peer);
    return downstream.find(kHttp200) != std::string::npos;
  }));
  DestroyRunningRuntime();
  close(peer);
}

TEST_F(OdinXqcClientRuntimeTest, T7) {
  int owned = -1;
  int peer = -1;
  MakePair(&owned, &peer);
  errno = 0;
  EXPECT_EQ(odin_xqc_client_runtime_add_connection(nullptr, owned), -1);
  EXPECT_EQ(errno, ENOTCONN);
  EXPECT_GE(fcntl(owned, F_GETFD), 0);
  close(owned);
  close(peer);

  CreateRuntime();
  MakePair(&owned, &peer);
  errno = 0;
  EXPECT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned), -1);
  EXPECT_EQ(errno, ENOTCONN);
  EXPECT_GE(fcntl(owned, F_GETFD), 0);
  close(owned);
  close(peer);
  ASSERT_EQ(odin_xqc_client_runtime_start(h.rt), 0);
  ASSERT_EQ(odin_xqc_client_runtime_stop(h.rt), 0);
  MakePair(&owned, &peer);
  errno = 0;
  EXPECT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned), -1);
  EXPECT_EQ(errno, ENOTCONN);
  EXPECT_GE(fcntl(owned, F_GETFD), 0);
  close(owned);
  close(peer);
  DestroyRunningRuntime();

  ResetFakeRuntimeState();
  StartRuntime();
  FireHandshake();
  h.fail_register_errno = ENOMEM;
  h.fail_register_cid = h.cid_b.cid_buf[0];
  h.transport_callbacks.conn_update_cid_notify(AsConn(&h.conn_a), &h.cid_a,
                                               &h.cid_b, h.xu_user_data);
  EXPECT_EQ(CountClientCallsForCid(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_REGISTER_CONN, h.cid_b),
            1u);
  EXPECT_EQ(CountClientCallsForCid(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a),
            1u);
  EXPECT_EQ(CountClientCallsForCid(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE,
                                   h.cid_a),
            1u);
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CREATE_BIDI),
      0u);
  MakePair(&owned, &peer);
  errno = 0;
  EXPECT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned), -1);
  EXPECT_EQ(errno, ENOTCONN);
  EXPECT_GE(fcntl(owned, F_GETFD), 0);
  close(owned);
  close(peer);
  DestroyRunningRuntime();

  ResetFakeRuntimeState();
  StartRuntime();
  FireHandshake();
  MakePair(&owned, &peer);
  ASSERT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned), 0);
  ASSERT_TRUE(WriteAllFd(peer, "GET / HTTP/1.1\r\n\r\n",
                         sizeof("GET / HTTP/1.1\r\n\r\n") - 1));
  std::string method_response;
  ASSERT_TRUE(RunUntil(h.loop, [&] {
    method_response += DrainFdNow(peer);
    return method_response.size() >= sizeof(kHttp405) - 1u;
  }));
  EXPECT_EQ(method_response, std::string(kHttp405));
  EXPECT_TRUE(PeerSawEof(peer));
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CREATE_BIDI),
      0u);
  close(peer);

  MakePair(&owned, &peer);
  ASSERT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned), 0);
  ASSERT_TRUE(WriteAllFd(peer, "CONNECT example.com:443",
                         sizeof("CONNECT example.com:443") - 1));
  ASSERT_EQ(shutdown(peer, SHUT_WR), 0) << std::strerror(errno);
  RunLoopFor(h.loop);
  EXPECT_TRUE(ReadAvailableFd(peer, 200).empty());
  EXPECT_TRUE(PeerSawEof(peer));
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CREATE_BIDI),
      0u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE),
            0u);
  EXPECT_EQ(
      CountClientCalls(
          ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_GET_CONN_ALP_USER_DATA_BY_STREAM),
      0u);
  DestroyRunningRuntime();
  close(peer);
}

TEST_F(OdinXqcClientRuntimeTest, T8) {
  StartRuntime();
  ASSERT_EQ(
      odin_xqc_client_runtime_test_fail_next_pending_queue_append(h.rt, ENOMEM),
      0);
  ASSERT_EQ(
      odin_xqc_client_runtime_test_fail_next_stream_context_alloc(h.rt, ENOMEM),
      0);
  odin_xqc_client_runtime_test_reset();
  InstallOps();
  int owned = -1;
  int peer = -1;
  MakePair(&owned, &peer);
  ASSERT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned), 0)
      << std::strerror(errno);
  odin_xqc_client_runtime_destroy(h.rt);
  FireClose();
  h.rt = nullptr;
  close(peer);

  StartRuntime();
  MakePair(&owned, &peer);
  ASSERT_EQ(
      odin_xqc_client_runtime_test_fail_next_pending_queue_append(h.rt, ENOMEM),
      0);
  errno = 0;
  EXPECT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned), -1);
  EXPECT_EQ(errno, ENOMEM);
  EXPECT_GE(fcntl(owned, F_GETFD), 0);
  close(owned);
  close(peer);

  FireHandshake();
  MakePair(&owned, &peer);
  ASSERT_EQ(
      odin_xqc_client_runtime_test_fail_next_stream_context_alloc(h.rt, ENOMEM),
      0);
  errno = 0;
  EXPECT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned), -1);
  EXPECT_EQ(errno, ENOMEM);
  EXPECT_GE(fcntl(owned, F_GETFD), 0);
  close(owned);
  close(peer);

  MakePair(&owned, &peer);
  ASSERT_EQ(odin_client_session_test_fail_next_create(ENOMEM), 0);
  errno = 0;
  EXPECT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned), -1);
  EXPECT_EQ(errno, ENOMEM);
  EXPECT_GE(fcntl(owned, F_GETFD), 0);
  close(owned);
  close(peer);

  FakeStream transport_create_fail;
  h.streams_to_create.push_back(&transport_create_fail);
  ASSERT_EQ(odin_xqc_stream_transport_test_fail_next_create(ENOMEM), 0);
  MakePair(&owned, &peer);
  ASSERT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned), 0);
  ASSERT_TRUE(WriteAllFd(peer, kHttpReq, sizeof(kHttpReq) - 1));
  std::string transport_fail_response;
  ASSERT_TRUE(RunUntil(h.loop, [&] {
    transport_fail_response += DrainFdNow(peer);
    return transport_fail_response.size() >= sizeof(kHttp400) - 1;
  }));
  transport_fail_response += ReadAvailableFd(peer, 50);
  EXPECT_EQ(transport_fail_response, kHttp400);
  EXPECT_EQ(transport_create_fail.close_calls, 1);
  EXPECT_TRUE(transport_create_fail.user_data_values.empty());
  close(peer);

  FakeStream connect_session_fail;
  h.streams_to_create.push_back(&connect_session_fail);
  ASSERT_EQ(odin_connect_session_test_fail_next_create_client(ENOMEM), 0);
  MakePair(&owned, &peer);
  ASSERT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned), 0);
  ASSERT_TRUE(WriteAllFd(peer, kHttpReq, sizeof(kHttpReq) - 1));
  std::string connect_fail_response;
  ASSERT_TRUE(RunUntil(h.loop, [&] {
    connect_fail_response += DrainFdNow(peer);
    return connect_fail_response.size() >= sizeof(kHttp400) - 1;
  }));
  connect_fail_response += ReadAvailableFd(peer, 50);
  EXPECT_EQ(connect_fail_response, kHttp400);
  ASSERT_FALSE(connect_session_fail.user_data_values.empty());
  void *stale_transport = connect_session_fail.user_data_values.front();
  EXPECT_EQ(connect_session_fail.user_data_values.back(), nullptr);
  EXPECT_EQ(connect_session_fail.close_calls, 1);
  EXPECT_GT(connect_session_fail.user_data_clear_order, 0);
  EXPECT_GT(connect_session_fail.close_order,
            connect_session_fail.user_data_clear_order);
  const int recv_before_stale = connect_session_fail.recv_calls;
  const int send_before_stale = connect_session_fail.send_calls;
  const int close_before_stale = connect_session_fail.close_calls;
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(
                AsStream(&connect_session_fail), stale_transport),
            XQC_OK);
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_write_notify(
                AsStream(&connect_session_fail), stale_transport),
            XQC_OK);
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_close_notify(
                AsStream(&connect_session_fail), stale_transport),
            XQC_OK);
  h.app_callbacks->stream_cbs.stream_closing_notify(
      AsStream(&connect_session_fail), -XQC_ESTREAM_RESET, stale_transport);
  EXPECT_EQ(connect_session_fail.recv_calls, recv_before_stale);
  EXPECT_EQ(connect_session_fail.send_calls, send_before_stale);
  EXPECT_EQ(connect_session_fail.close_calls, close_before_stale);
  close(peer);

  h.stream_create_returns_null = true;
  h.stream_create_errno = 0;
  errno = EBUSY;
  FakeStream unused;
  const unsigned int stream_create_before_null =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CREATE_BIDI);
  MakePair(&owned, &peer);
  ASSERT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned), 0);
  ASSERT_TRUE(WriteAllFd(peer, kHttpReq, sizeof(kHttpReq) - 1));
  std::string stream_create_fail_response;
  ASSERT_TRUE(RunUntil(h.loop, [&] {
    stream_create_fail_response += DrainFdNow(peer);
    return stream_create_fail_response.size() >= sizeof(kHttp400) - 1;
  }));
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CREATE_BIDI),
      stream_create_before_null + 1);
  stream_create_fail_response += ReadAvailableFd(peer, 50);
  EXPECT_EQ(stream_create_fail_response, kHttp400);
  h.stream_create_returns_null = false;
  DestroyRunningRuntime();
  close(peer);

  enum class QueuedFailure {
    kClientSessionCreate,
    kStreamContextAlloc,
    kStreamCreate,
  };
  auto run_queued_recovery_subcase = [&](QueuedFailure failure) {
    StartRuntime();
    int owned1 = -1;
    int peer1 = -1;
    int owned2 = -1;
    int peer2 = -1;
    MakePair(&owned1, &peer1);
    MakePair(&owned2, &peer2);
    ASSERT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned1), 0);
    ASSERT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned2), 0);

    FakeStream queued_recovery;
    if (failure == QueuedFailure::kClientSessionCreate) {
      ASSERT_EQ(odin_client_session_test_fail_next_create(ENOMEM), 0);
      QueueStream(&queued_recovery);
      ASSERT_TRUE(WriteAllFd(peer1, kHttpReq, sizeof(kHttpReq) - 1));
      ASSERT_TRUE(WriteAllFd(peer2, kHttpReq, sizeof(kHttpReq) - 1));
    } else if (failure == QueuedFailure::kStreamContextAlloc) {
      ASSERT_EQ(odin_xqc_client_runtime_test_fail_next_stream_context_alloc(
                    h.rt, ENOMEM),
                0);
      QueueStream(&queued_recovery);
      ASSERT_TRUE(WriteAllFd(peer1, kHttpReq, sizeof(kHttpReq) - 1));
      ASSERT_TRUE(WriteAllFd(peer2, kHttpReq, sizeof(kHttpReq) - 1));
    } else {
      h.stream_create_returns_null = true;
      h.stream_create_errno = ENOMEM;
      ASSERT_TRUE(WriteAllFd(peer1, kHttpReq, sizeof(kHttpReq) - 1));
    }

    FireHandshake();
    if (failure == QueuedFailure::kStreamCreate) {
      std::string first_response;
      ASSERT_TRUE(RunUntil(h.loop, [&] {
        first_response += DrainFdNow(peer1);
        return first_response.size() >= sizeof(kHttp400) - 1;
      }));
      first_response += ReadAvailableFd(peer1, 50);
      EXPECT_EQ(first_response, kHttp400);
      ASSERT_TRUE(RunUntil(h.loop, [&] { return PeerSawEof(peer1); }));
      h.stream_create_returns_null = false;
      h.stream_create_errno = 0;
      QueueStream(&queued_recovery);
      ASSERT_TRUE(WriteAllFd(peer2, kHttpReq, sizeof(kHttpReq) - 1));
      ASSERT_TRUE(
          RunUntil(h.loop, [&] { return !queued_recovery.sends.empty(); }));
    } else {
      ASSERT_TRUE(RunUntil(h.loop, [&] {
        return PeerSawEof(peer1) && !queued_recovery.sends.empty();
      }));
    }

    EXPECT_EQ(queued_recovery.sends.front(), EncodedConnectReq());
    const unsigned int expected_stream_creates =
        failure == QueuedFailure::kStreamCreate ? 2u : 1u;
    EXPECT_EQ(
        CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CREATE_BIDI),
        expected_stream_creates);
    DestroyRunningRuntime();
    close(peer1);
    close(peer2);
  };

  ResetFakeRuntimeState();
  run_queued_recovery_subcase(QueuedFailure::kClientSessionCreate);
  ResetFakeRuntimeState();
  run_queued_recovery_subcase(QueuedFailure::kStreamContextAlloc);
  ResetFakeRuntimeState();
  run_queued_recovery_subcase(QueuedFailure::kStreamCreate);
}

TEST_F(OdinXqcClientRuntimeTest, T9) {
  StartRuntime();
  FireHandshake();
  FakeStream reset_stream;
  int reset_peer = -1;
  AddParsedConnection(&reset_stream, &reset_peer);
  ASSERT_NE(reset_stream.user_data, nullptr);
  void *reset_user_data = reset_stream.user_data;
  h.app_callbacks->stream_cbs.stream_closing_notify(
      AsStream(&reset_stream), -XQC_ESTREAM_RESET, reset_user_data);
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(
                AsStream(&reset_stream), reset_user_data),
            XQC_OK);
  ASSERT_TRUE(RunUntil(h.loop, [&] {
    return !reset_stream.user_data_values.empty() &&
           reset_stream.user_data_values.back() == nullptr;
  }));
  EXPECT_EQ(reset_stream.user_data_values.back(), nullptr);
  EXPECT_EQ(reset_stream.close_calls, 1);
  EXPECT_GT(reset_stream.user_data_clear_order, 0);
  EXPECT_GT(reset_stream.close_order, reset_stream.user_data_clear_order);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE),
            1u);
  EXPECT_EQ(CountClientCallsForCid(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a),
            0u);
  close(reset_peer);

  FakeStream stream;
  int peer = -1;
  AddParsedConnection(&stream, &peer);
  ASSERT_NE(stream.user_data, nullptr);
  void *live_user_data = stream.user_data;
  const int recv_before_null = stream.recv_calls;
  const int send_before_null = stream.send_calls;
  const int close_before_null = stream.close_calls;
  const unsigned int lookups_before_null = CountClientCalls(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_GET_CONN_ALP_USER_DATA_BY_STREAM);
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(AsStream(&stream),
                                                           nullptr),
            XQC_OK);
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_write_notify(AsStream(&stream),
                                                            nullptr),
            XQC_OK);
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_close_notify(AsStream(&stream),
                                                            nullptr),
            XQC_OK);
  h.app_callbacks->stream_cbs.stream_closing_notify(
      AsStream(&stream), -XQC_ESTREAM_RESET, nullptr);
  EXPECT_EQ(
      CountClientCalls(
          ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_GET_CONN_ALP_USER_DATA_BY_STREAM),
      lookups_before_null + 4);
  EXPECT_EQ(stream.recv_calls, recv_before_null);
  EXPECT_EQ(stream.send_calls, send_before_null);
  EXPECT_EQ(stream.close_calls, close_before_null);
  EXPECT_EQ(stream.user_data, live_user_data);

  stream.conn->alp_user_data = nullptr;
  const unsigned int lookups_before_missing_alpn = CountClientCalls(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_GET_CONN_ALP_USER_DATA_BY_STREAM);
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(AsStream(&stream),
                                                           live_user_data),
            XQC_OK);
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_write_notify(AsStream(&stream),
                                                            live_user_data),
            XQC_OK);
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_close_notify(AsStream(&stream),
                                                            live_user_data),
            XQC_OK);
  h.app_callbacks->stream_cbs.stream_closing_notify(
      AsStream(&stream), -XQC_ESTREAM_RESET, live_user_data);
  EXPECT_EQ(
      CountClientCalls(
          ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_GET_CONN_ALP_USER_DATA_BY_STREAM),
      lookups_before_missing_alpn + 4);
  EXPECT_EQ(stream.recv_calls, recv_before_null);
  EXPECT_EQ(stream.send_calls, send_before_null);
  EXPECT_EQ(stream.close_calls, close_before_null);
  EXPECT_EQ(stream.user_data, live_user_data);
  stream.conn->alp_user_data = h.rt;

  const unsigned int unregister_before_close = CountClientCallsForCid(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a);
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_close_notify(AsStream(&stream),
                                                            live_user_data),
            XQC_OK);
  EXPECT_EQ(stream.user_data_values.back(), nullptr);
  EXPECT_EQ(stream.close_calls, 0);
  EXPECT_EQ(CountClientCallsForCid(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a),
            unregister_before_close);

  const int recv_before_stale = stream.recv_calls;
  const int send_before_stale = stream.send_calls;
  const int close_before_stale = stream.close_calls;
  const unsigned int lookups_before_stale = CountClientCalls(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_GET_CONN_ALP_USER_DATA_BY_STREAM);
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(AsStream(&stream),
                                                           live_user_data),
            XQC_OK);
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_write_notify(AsStream(&stream),
                                                            live_user_data),
            XQC_OK);
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_close_notify(AsStream(&stream),
                                                            live_user_data),
            XQC_OK);
  h.app_callbacks->stream_cbs.stream_closing_notify(
      AsStream(&stream), -XQC_ESTREAM_RESET, live_user_data);
  EXPECT_EQ(
      CountClientCalls(
          ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_GET_CONN_ALP_USER_DATA_BY_STREAM),
      lookups_before_stale + 4);
  EXPECT_EQ(stream.recv_calls, recv_before_stale);
  EXPECT_EQ(stream.send_calls, send_before_stale);
  EXPECT_EQ(stream.close_calls, close_before_stale);

  FakeStream later;
  int later_peer = -1;
  AddParsedConnection(&later, &later_peer);
  ASSERT_NE(later.user_data, nullptr);
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(AsStream(&later),
                                                           later.user_data),
            XQC_OK);
  std::string later_downstream;
  ASSERT_TRUE(RunUntil(h.loop, [&] {
    later_downstream += DrainFdNow(later_peer);
    return later_downstream.find(kHttp200) != std::string::npos;
  }));
  EXPECT_EQ(later.sends.front(), EncodedConnectReq());
  EXPECT_EQ(later.close_calls, 0);
  EXPECT_EQ(CountClientCallsForCid(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a),
            unregister_before_close);
  DestroyRunningRuntime();
  close(peer);
  close(later_peer);
}

TEST_F(OdinXqcClientRuntimeTest, T10) {
  StartRuntime();
  int owned1 = -1;
  int peer1 = -1;
  int owned2 = -1;
  int peer2 = -1;
  MakePair(&owned1, &peer1);
  MakePair(&owned2, &peer2);
  ASSERT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned1), 0);
  ASSERT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned2), 0);
  const unsigned int conn_close_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE);
  const unsigned int stream_close_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE);
  const unsigned int unregister_before = CountClientCallsForCid(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a);
  const unsigned int alpn_unregister_before = CountClientCalls(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN);
  const unsigned int udp_destroy_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY);
  FireClose();
  EXPECT_TRUE(PeerSawEof(peer1));
  EXPECT_TRUE(PeerSawEof(peer2));
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CREATE_BIDI),
      0u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE),
            conn_close_before);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE),
            stream_close_before);
  EXPECT_EQ(CountClientCallsForCid(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a),
            unregister_before + 1u);
  EXPECT_EQ(CountClientCalls(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN),
            alpn_unregister_before);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY),
            udp_destroy_before);
  close(peer1);
  close(peer2);
  odin_xqc_client_runtime_destroy(h.rt);
  h.rt = nullptr;
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE),
            conn_close_before);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE),
            stream_close_before);
  EXPECT_EQ(CountClientCallsForCid(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a),
            unregister_before + 1u);
  EXPECT_EQ(CountClientCalls(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN),
            alpn_unregister_before + 1u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY),
            udp_destroy_before + 1u);
  const int unregister = FirstClientCallForCidIndex(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a);
  const int alpn_unregister = FirstClientCallIndex(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN);
  const int udp_destroy =
      FirstClientCallIndex(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY);
  ASSERT_GE(unregister, 0);
  ASSERT_GE(alpn_unregister, 0);
  ASSERT_GE(udp_destroy, 0);
  EXPECT_LT(unregister, alpn_unregister);
  EXPECT_LT(alpn_unregister, udp_destroy);
}

TEST_F(OdinXqcClientRuntimeTest, T11) {
  StartRuntime();
  FireHandshake();
  FakeStream stream1;
  FakeStream stream2;
  int peer1 = -1;
  int peer2 = -1;
  AddStalledConnection(&stream1, &peer1);
  AddStalledConnection(&stream2, &peer2);
  ASSERT_NE(stream1.user_data, nullptr);
  ASSERT_NE(stream2.user_data, nullptr);
  const unsigned int conn_close_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE);
  const unsigned int stream_close_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE);
  const unsigned int unregister_before = CountClientCallsForCid(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a);
  const unsigned int alpn_unregister_before = CountClientCalls(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN);
  const unsigned int udp_destroy_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY);
  FireClose();
  EXPECT_TRUE(PeerSawEof(peer1));
  EXPECT_TRUE(PeerSawEof(peer2));
  EXPECT_EQ(stream1.user_data_values.back(), nullptr);
  EXPECT_EQ(stream2.user_data_values.back(), nullptr);
  EXPECT_EQ(stream1.close_calls, 0);
  EXPECT_EQ(stream2.close_calls, 0);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE),
            conn_close_before);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE),
            stream_close_before);
  EXPECT_EQ(CountClientCallsForCid(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a),
            unregister_before + 1u);
  EXPECT_EQ(CountClientCalls(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN),
            alpn_unregister_before);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY),
            udp_destroy_before);
  close(peer1);
  close(peer2);
  odin_xqc_client_runtime_destroy(h.rt);
  h.rt = nullptr;
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE),
            conn_close_before);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE),
            stream_close_before);
  EXPECT_EQ(CountClientCallsForCid(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a),
            unregister_before + 1u);
  EXPECT_EQ(CountClientCalls(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN),
            alpn_unregister_before + 1u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY),
            udp_destroy_before + 1u);
  const int unregister = FirstClientCallForCidIndex(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a);
  const int alpn_unregister = FirstClientCallIndex(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN);
  const int udp_destroy =
      FirstClientCallIndex(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY);
  ASSERT_GE(unregister, 0);
  ASSERT_GE(alpn_unregister, 0);
  ASSERT_GE(udp_destroy, 0);
  EXPECT_LT(unregister, alpn_unregister);
  EXPECT_LT(alpn_unregister, udp_destroy);
}

TEST_F(OdinXqcClientRuntimeTest, T13) {
  StartRuntime();
  int owned1 = -1;
  int peer1 = -1;
  int owned2 = -1;
  int peer2 = -1;
  MakePair(&owned1, &peer1);
  MakePair(&owned2, &peer2);
  ASSERT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned1), 0);
  ASSERT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned2), 0);
  const unsigned int conn_close_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE);
  const unsigned int stream_close_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE);
  const unsigned int unregister_before = CountClientCallsForCid(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a);
  const unsigned int alpn_unregister_before = CountClientCalls(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN);
  const unsigned int udp_destroy_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY);
  odin_xqc_client_runtime_destroy(h.rt);
  EXPECT_TRUE(PeerSawEof(peer1));
  EXPECT_TRUE(PeerSawEof(peer2));
  EXPECT_EQ(h.conn_close_cids.size(), 1u);
  EXPECT_TRUE(CidEq(h.conn_close_cids[0], h.cid_a));
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CREATE_BIDI),
      0u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE),
            conn_close_before + 1u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE),
            stream_close_before);
  EXPECT_EQ(CountClientCallsForCid(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a),
            unregister_before);
  EXPECT_EQ(CountClientCalls(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN),
            alpn_unregister_before);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY),
            udp_destroy_before);
  odin_xqc_client_runtime_test_state_t closing;
  ASSERT_EQ(odin_xqc_client_runtime_test_state(h.rt, &closing), 0);
  EXPECT_EQ(closing.conn, AsConn(&h.conn_a));
  EXPECT_TRUE(CidEq(closing.current_cid, h.cid_a));
  EXPECT_EQ(closing.cid_registered, 1);
  EXPECT_EQ(closing.handshake_done, 0);
  EXPECT_EQ(closing.closing, 1);
  FireClose();
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE),
            conn_close_before + 1u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE),
            stream_close_before);
  EXPECT_EQ(CountClientCallsForCid(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a),
            unregister_before + 1u);
  EXPECT_EQ(CountClientCalls(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN),
            alpn_unregister_before + 1u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY),
            udp_destroy_before + 1u);
  const int conn_close = FirstClientCallForCidIndex(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE, h.cid_a);
  const int unregister = FirstClientCallForCidIndex(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a);
  const int alpn_unregister = FirstClientCallIndex(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN);
  const int udp_destroy =
      FirstClientCallIndex(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY);
  ASSERT_GE(conn_close, 0);
  ASSERT_GE(unregister, 0);
  ASSERT_GE(alpn_unregister, 0);
  ASSERT_GE(udp_destroy, 0);
  EXPECT_LT(conn_close, unregister);
  EXPECT_LT(unregister, alpn_unregister);
  EXPECT_LT(alpn_unregister, udp_destroy);
  h.rt = nullptr;
  close(peer1);
  close(peer2);
}

TEST_F(OdinXqcClientRuntimeTest, T14) {
  StartRuntime();
  FireHandshake();
  FakeStream stream1;
  FakeStream stream2;
  int peer1 = -1;
  int peer2 = -1;
  AddParsedConnection(&stream1, &peer1);
  AddParsedConnection(&stream2, &peer2);
  ASSERT_NE(stream1.user_data, nullptr);
  ASSERT_NE(stream2.user_data, nullptr);
  const unsigned int conn_close_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE);
  const unsigned int stream_close_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE);
  const unsigned int unregister_before = CountClientCallsForCid(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a);
  const unsigned int alpn_unregister_before = CountClientCalls(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN);
  const unsigned int udp_destroy_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY);
  odin_xqc_client_runtime_destroy(h.rt);
  EXPECT_EQ(stream1.user_data_values.back(), nullptr);
  EXPECT_EQ(stream2.user_data_values.back(), nullptr);
  EXPECT_EQ(stream1.close_calls, 1);
  EXPECT_EQ(stream2.close_calls, 1);
  EXPECT_GT(stream1.user_data_clear_order, 0);
  EXPECT_GT(stream2.user_data_clear_order, 0);
  EXPECT_GT(stream1.close_order, stream1.user_data_clear_order);
  EXPECT_GT(stream2.close_order, stream2.user_data_clear_order);
  EXPECT_EQ(
      CountClientCallsForStream(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE,
                                AsStream(&stream1)),
      1u);
  EXPECT_EQ(
      CountClientCallsForStream(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE,
                                AsStream(&stream2)),
      1u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE),
            stream_close_before + 2u);
  EXPECT_EQ(h.conn_close_cids.size(), 1u);
  EXPECT_TRUE(CidEq(h.conn_close_cids[0], h.cid_a));
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE),
            conn_close_before + 1u);
  EXPECT_EQ(CountClientCallsForCid(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a),
            unregister_before);
  EXPECT_EQ(CountClientCalls(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN),
            alpn_unregister_before);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY),
            udp_destroy_before);
  odin_xqc_client_runtime_test_state_t closing;
  ASSERT_EQ(odin_xqc_client_runtime_test_state(h.rt, &closing), 0);
  EXPECT_EQ(closing.conn, AsConn(&h.conn_a));
  EXPECT_TRUE(CidEq(closing.current_cid, h.cid_a));
  EXPECT_EQ(closing.cid_registered, 1);
  EXPECT_EQ(closing.handshake_done, 1);
  EXPECT_EQ(closing.closing, 1);
  FireClose();
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE),
            conn_close_before + 1u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE),
            stream_close_before + 2u);
  EXPECT_EQ(CountClientCallsForCid(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a),
            unregister_before + 1u);
  EXPECT_EQ(CountClientCalls(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN),
            alpn_unregister_before + 1u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY),
            udp_destroy_before + 1u);
  const int stream1_close = FirstClientCallForStreamIndex(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE, AsStream(&stream1));
  const int stream2_close = FirstClientCallForStreamIndex(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE, AsStream(&stream2));
  const int conn_close = FirstClientCallForCidIndex(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE, h.cid_a);
  const int unregister = FirstClientCallForCidIndex(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a);
  const int alpn_unregister = FirstClientCallIndex(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN);
  const int udp_destroy =
      FirstClientCallIndex(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY);
  ASSERT_GE(stream1_close, 0);
  ASSERT_GE(stream2_close, 0);
  ASSERT_GE(conn_close, 0);
  ASSERT_GE(unregister, 0);
  ASSERT_GE(alpn_unregister, 0);
  ASSERT_GE(udp_destroy, 0);
  EXPECT_LT(stream1_close, conn_close);
  EXPECT_LT(stream2_close, conn_close);
  EXPECT_LT(conn_close, unregister);
  EXPECT_LT(unregister, alpn_unregister);
  EXPECT_LT(alpn_unregister, udp_destroy);
  h.rt = nullptr;
  close(peer1);
  close(peer2);
}

TEST_F(OdinXqcClientRuntimeTest, T15) {
  (void)signal(SIGPIPE, SIG_IGN);
  ClearOps();
  odin_xqc_client_runtime_test_reset();
  odin_xqc_server_runtime_test_reset();
  odin_xqc_server_runtime_test_set_ops(nullptr);
  odin_xqc_udp_test_set_ops(nullptr);
  static const odin_xqc_stream_transport_test_ops_t kRealStreamOps = {
      RealStreamRecv,
      RealStreamSend,
      RealStreamSetUserData,
  };
  odin_xqc_stream_transport_test_set_ops(&kRealStreamOps);

  uint16_t upstream_port = 0;
  const int upstream_lfd = OpenLoopbackListener(&upstream_port);
  ASSERT_GE(upstream_lfd, 0) << std::strerror(errno);
  ProductionDialFilterState filter;
  filter.expected_port = upstream_port;

  const std::string root = FindRepoRoot();
  const std::string cert = root + "/thor/out/odin-server.pem";
  const std::string key = root + "/thor/out/odin-server-key.pem";
  const std::string ca = root + "/thor/out/root-ca.pem";
  ASSERT_EQ(access(cert.c_str(), R_OK), 0) << cert;
  ASSERT_EQ(access(key.c_str(), R_OK), 0) << key;
  ASSERT_EQ(access(ca.c_str(), R_OK), 0) << ca;

  xqc_engine_ssl_config_t server_ssl;
  std::memset(&server_ssl, 0, sizeof(server_ssl));
  server_ssl.private_key_file = const_cast<char *>(key.c_str());
  server_ssl.cert_file = const_cast<char *>(cert.c_str());
  server_ssl.ciphers = const_cast<char *>(XQC_TLS_CIPHERS);
  server_ssl.groups = const_cast<char *>(XQC_TLS_GROUPS);
  xqc_config_t server_engine_config;
  std::memset(&server_engine_config, 0, sizeof(server_engine_config));
  xqc_engine_callback_t server_engine_callbacks;
  std::memset(&server_engine_callbacks, 0, sizeof(server_engine_callbacks));
  struct sockaddr_in server_bind = Loopback4(0);
  odin_xqc_server_runtime_config_t server_config;
  std::memset(&server_config, 0, sizeof(server_config));
  server_config.loop = h.loop;
  server_config.local_addr =
      reinterpret_cast<const struct sockaddr *>(&server_bind);
  server_config.local_addrlen = sizeof(server_bind);
  server_config.engine_config = &server_engine_config;
  server_config.ssl_config = &server_ssl;
  server_config.engine_callbacks = &server_engine_callbacks;

  odin_xqc_server_runtime_t *server = nullptr;
  ASSERT_EQ(odin_xqc_server_runtime_create(&server_config, &server), 0)
      << std::strerror(errno);
  ASSERT_NE(server, nullptr);
  odin_xqc_server_runtime_set_dial_filter(server, ProductionDialFilter,
                                          &filter);
  ASSERT_EQ(odin_xqc_server_runtime_start(server), 0) << std::strerror(errno);
  struct sockaddr_storage server_addr;
  socklen_t server_addrlen = sizeof(server_addr);
  ASSERT_EQ(odin_xqc_server_runtime_local_addr(
                server, reinterpret_cast<struct sockaddr *>(&server_addr),
                &server_addrlen),
            0)
      << std::strerror(errno);

  ProductionCertState cert_state;
  cert_state.ca_file = ca.c_str();
  g_production_cert_state = &cert_state;
  xqc_transport_callbacks_t client_transport_callbacks;
  std::memset(&client_transport_callbacks, 0,
              sizeof(client_transport_callbacks));
  client_transport_callbacks.cert_verify_cb = ProductionCertVerify;
  xqc_conn_settings_t conn_settings =
      xqc_conn_get_conn_settings_template(XQC_CONN_SETTINGS_DEFAULT);
  xqc_conn_ssl_config_t conn_ssl;
  std::memset(&conn_ssl, 0, sizeof(conn_ssl));
  conn_ssl.cert_verify_flag =
      XQC_TLS_CERT_FLAG_NEED_VERIFY | XQC_TLS_CERT_FLAG_ALLOW_SELF_SIGNED;
  xqc_engine_ssl_config_t client_ssl;
  std::memset(&client_ssl, 0, sizeof(client_ssl));
  client_ssl.ciphers = const_cast<char *>(XQC_TLS_CIPHERS);
  client_ssl.groups = const_cast<char *>(XQC_TLS_GROUPS);
  xqc_config_t client_engine_config;
  std::memset(&client_engine_config, 0, sizeof(client_engine_config));
  xqc_engine_callback_t client_engine_callbacks;
  std::memset(&client_engine_callbacks, 0, sizeof(client_engine_callbacks));
  struct sockaddr_in client_bind = Loopback4(0);
  odin_xqc_client_runtime_config_t client_config;
  std::memset(&client_config, 0, sizeof(client_config));
  client_config.loop = h.loop;
  client_config.local_addr =
      reinterpret_cast<const struct sockaddr *>(&client_bind);
  client_config.local_addrlen = sizeof(client_bind);
  client_config.peer_addr =
      reinterpret_cast<const struct sockaddr *>(&server_addr);
  client_config.peer_addrlen = server_addrlen;
  client_config.server_host = "localhost";
  client_config.engine_config = &client_engine_config;
  client_config.engine_ssl_config = &client_ssl;
  client_config.engine_callbacks = &client_engine_callbacks;
  client_config.transport_callbacks = &client_transport_callbacks;
  client_config.conn_settings = &conn_settings;
  client_config.conn_ssl_config = &conn_ssl;

  odin_xqc_client_runtime_t *client = nullptr;
  ASSERT_EQ(odin_xqc_client_runtime_create(&client_config, &client), 0)
      << std::strerror(errno);
  ASSERT_NE(client, nullptr);
  const odin_xqc_client_runtime_test_record_t *record =
      odin_xqc_client_runtime_test_record();
  ASSERT_EQ(record->udp_create_calls, 1u);
  EXPECT_EQ(record->last_udp_create.loop, h.loop);
  EXPECT_EQ(record->last_udp_create.local_addr,
            reinterpret_cast<const struct sockaddr *>(&client_bind));
  EXPECT_EQ(record->last_udp_create.local_addrlen, sizeof(client_bind));
  EXPECT_EQ(record->last_udp_create.engine_type, XQC_ENGINE_CLIENT);
  EXPECT_EQ(record->last_udp_create.engine_config, &client_engine_config);
  EXPECT_EQ(record->last_udp_create.ssl_config, &client_ssl);
  EXPECT_EQ(record->last_udp_create.engine_callbacks, &client_engine_callbacks);
  EXPECT_EQ(record->last_udp_create.transport_callbacks_value.cert_verify_cb,
            ProductionCertVerify);
  EXPECT_NE(record->last_udp_create.transport_callbacks_value.save_token,
            nullptr);
  EXPECT_NE(record->last_udp_create.transport_callbacks_value.save_session_cb,
            nullptr);
  EXPECT_NE(record->last_udp_create.transport_callbacks_value.save_tp_cb,
            nullptr);
  EXPECT_EQ(record->last_udp_create.app_user_data, client);

  const odin_xqc_server_runtime_test_record_t *server_record =
      odin_xqc_server_runtime_test_record();
  bool saw_server_alpn = false;
  for (unsigned int i = 0; i < server_record->call_count; ++i) {
    const odin_xqc_server_runtime_test_call_t &call = server_record->calls[i];
    if (call.kind == ODIN_XQC_SERVER_RUNTIME_TEST_CALL_ENGINE_REGISTER_ALPN) {
      saw_server_alpn =
          call.alpn_len == sizeof(ODIN_XQC_SERVER_ALPN) - 1u &&
          std::string(call.alpn, call.alpn_len) == ODIN_XQC_SERVER_ALPN;
    }
  }
  EXPECT_TRUE(saw_server_alpn);

  ASSERT_EQ(odin_xqc_client_runtime_start(client), 0) << std::strerror(errno);
  odin_xqc_client_runtime_test_state_t handshake_state;
  std::memset(&handshake_state, 0, sizeof(handshake_state));
  ASSERT_TRUE(RunUntil(
      h.loop,
      [&] {
        if (odin_xqc_client_runtime_test_state(client, &handshake_state) != 0) {
          return false;
        }
        return handshake_state.conn != nullptr &&
               handshake_state.cid_registered == 1 &&
               handshake_state.handshake_done == 1;
      },
      5000))
      << "state conn=" << handshake_state.conn
      << " cid_registered=" << handshake_state.cid_registered
      << " handshake_done=" << handshake_state.handshake_done
      << " closing=" << handshake_state.closing
      << " connect_errno=" << handshake_state.connect_errno
      << " cert calls=" << cert_state.calls
      << " certs_len=" << cert_state.certs_len
      << " ca_verified=" << cert_state.ca_verified
      << " host_verified=" << cert_state.host_verified
      << " conn_user_data=" << cert_state.conn_user_data;
  record = odin_xqc_client_runtime_test_record();
  const odin_xqc_client_runtime_test_call_t *connect_call = nullptr;
  for (unsigned int i = 0; i < record->call_count; ++i) {
    if (record->calls[i].kind ==
        ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_XQC_CONNECT) {
      connect_call = &record->calls[i];
      break;
    }
  }
  ASSERT_NE(connect_call, nullptr);
  if (connect_call != nullptr && connect_call->alpn != nullptr) {
    EXPECT_EQ(connect_call->alpn_len, sizeof(ODIN_XQC_CLIENT_ALPN) - 1u);
    EXPECT_EQ(std::string(connect_call->alpn, connect_call->alpn_len),
              ODIN_XQC_CLIENT_ALPN);
  } else {
    ADD_FAILURE() << "missing production XQC_CONNECT ALPN";
  }

  int owned = -1;
  int local_peer = -1;
  MakePair(&owned, &local_peer);
  ASSERT_EQ(odin_xqc_client_runtime_add_connection(client, owned), 0)
      << std::strerror(errno);
  const std::string req = HttpConnectReq("127.0.0.1", upstream_port);
  const std::string client_tail = "client-tail!";
  const std::string server_tail = "server-tail!";
  ASSERT_TRUE(WriteAllFd(local_peer, req.data(), req.size()));
  ASSERT_TRUE(WriteAllFd(local_peer, client_tail.data(), client_tail.size()));

  int upstream_fd = -1;
  ASSERT_TRUE(RunUntil(
      h.loop,
      [&] {
        if (upstream_fd >= 0) {
          return true;
        }
        upstream_fd = accept(upstream_lfd, nullptr, nullptr);
        if (upstream_fd < 0 &&
            (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
          return false;
        }
        return upstream_fd >= 0;
      },
      5000));
  ASSERT_GE(upstream_fd, 0) << std::strerror(errno);
  ASSERT_TRUE(SetNonblockNoAssert(upstream_fd)) << std::strerror(errno);

  std::string local_data;
  ASSERT_TRUE(RunUntil(
      h.loop,
      [&] {
        local_data += DrainFdNow(local_peer);
        return local_data.find(kHttp200) != std::string::npos;
      },
      5000));
  EXPECT_GT(cert_state.calls, 0);
  EXPECT_GT(cert_state.certs_len, 0u);
  EXPECT_NE(cert_state.conn_user_data, nullptr);
  EXPECT_TRUE(cert_state.ca_verified);
  EXPECT_TRUE(cert_state.host_verified);
  EXPECT_EQ(filter.calls, 1);
  EXPECT_TRUE(filter.saw_expected_loopback);

  std::string upstream_data;
  ASSERT_TRUE(RunUntil(
      h.loop,
      [&] {
        upstream_data += DrainFdNow(upstream_fd);
        return upstream_data.size() >= client_tail.size();
      },
      3000));
  EXPECT_EQ(upstream_data, client_tail);

  ASSERT_TRUE(WriteAllFd(upstream_fd, server_tail.data(), server_tail.size()));
  ASSERT_TRUE(RunUntil(
      h.loop,
      [&] {
        local_data += DrainFdNow(local_peer);
        return local_data.find(std::string(kHttp200) + server_tail) !=
               std::string::npos;
      },
      3000));
  EXPECT_NE(local_data.find(std::string(kHttp200) + server_tail),
            std::string::npos);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_XQC_CONNECT),
            1u);
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CREATE_BIDI),
      1u);

  close(local_peer);
  close(upstream_fd);
  close(upstream_lfd);
  odin_xqc_client_runtime_destroy(client);
  RunLoopFor(h.loop, 50000);
  odin_xqc_server_runtime_force_destroy(server);
  g_production_cert_state = nullptr;
}

TEST_F(OdinXqcClientRuntimeTest, T16) {
  StartRuntime();
  int owned1 = -1;
  int peer1 = -1;
  int owned2 = -1;
  int peer2 = -1;
  MakePair(&owned1, &peer1);
  MakePair(&owned2, &peer2);
  FakeStream stream1;
  FakeStream stream2;
  const std::string req1 = HttpConnectReq("first.example", 443);
  const std::string req2 = HttpConnectReq("second.example", 8443);
  const std::string encoded_req1 = EncodedConnectReq("first.example", 443);
  const std::string encoded_req2 = EncodedConnectReq("second.example", 8443);
  const std::string local_tail1 = "client-tail-1";
  const std::string local_tail2 = "client-tail-2";
  const std::string server_tail1 = "server-tail-1";
  const std::string server_tail2 = "server-tail-2";
  stream1.recv_chunks.push_back(EncodedConnectResp());
  stream1.recv_chunks.push_back(server_tail1);
  stream2.recv_chunks.push_back(EncodedConnectResp());
  stream2.recv_chunks.push_back(server_tail2);
  h.streams_to_create.push_back(&stream1);
  h.streams_to_create.push_back(&stream2);
  ASSERT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned1), 0);
  ASSERT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned2), 0);
  ASSERT_TRUE(WriteAllFd(peer1, req1.data(), req1.size()));
  ASSERT_TRUE(WriteAllFd(peer1, local_tail1.data(), local_tail1.size()));
  ASSERT_TRUE(WriteAllFd(peer2, req2.data(), req2.size()));
  ASSERT_TRUE(WriteAllFd(peer2, local_tail2.data(), local_tail2.size()));
  ASSERT_EQ(odin_xqc_client_runtime_stop(h.rt), 0);
  odin_xqc_client_runtime_test_state_t stopped;
  ASSERT_EQ(odin_xqc_client_runtime_test_state(h.rt, &stopped), 0);
  EXPECT_EQ(stopped.conn, AsConn(&h.conn_a));
  EXPECT_EQ(stopped.cid_registered, 1);
  EXPECT_EQ(stopped.handshake_done, 0);
  EXPECT_TRUE(CidEq(stopped.current_cid, h.cid_a));
  EXPECT_FALSE(PeerSawEof(peer1));
  EXPECT_FALSE(PeerSawEof(peer2));
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CREATE_BIDI),
      0u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE),
            0u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE), 0u);
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN),
      0u);
  RunLoopFor(h.loop);
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CREATE_BIDI),
      0u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE),
            0u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE), 0u);
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN),
      0u);
  ASSERT_EQ(odin_xqc_client_runtime_start(h.rt), 0);
  FireHandshake();
  ASSERT_TRUE(RunUntil(h.loop, [&] {
    return stream1.user_data != nullptr && stream2.user_data != nullptr &&
           !stream1.sends.empty() && !stream2.sends.empty();
  }));
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(AsStream(&stream1),
                                                           stream1.user_data),
            XQC_OK);
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(AsStream(&stream2),
                                                           stream2.user_data),
            XQC_OK);
  std::string downstream1;
  std::string downstream2;
  ASSERT_TRUE(RunUntil(h.loop, [&] {
    downstream1 += DrainFdNow(peer1);
    downstream2 += DrainFdNow(peer2);
    return downstream1.find(kHttp200) != std::string::npos &&
           downstream2.find(kHttp200) != std::string::npos &&
           JoinedSends(stream1).find(local_tail1) != std::string::npos &&
           JoinedSends(stream2).find(local_tail2) != std::string::npos;
  }));
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(AsStream(&stream1),
                                                           stream1.user_data),
            XQC_OK);
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(AsStream(&stream2),
                                                           stream2.user_data),
            XQC_OK);
  ASSERT_TRUE(RunUntil(h.loop, [&] {
    downstream1 += DrainFdNow(peer1);
    downstream2 += DrainFdNow(peer2);
    return downstream1.find(std::string(kHttp200) + server_tail1) !=
               std::string::npos &&
           downstream2.find(std::string(kHttp200) + server_tail2) !=
               std::string::npos;
  }));
  ASSERT_FALSE(stream1.sends.empty());
  ASSERT_FALSE(stream2.sends.empty());
  EXPECT_EQ(stream1.sends.front(), encoded_req1);
  EXPECT_EQ(stream2.sends.front(), encoded_req2);
  EXPECT_NE(JoinedSends(stream1).find(local_tail1), std::string::npos);
  EXPECT_NE(JoinedSends(stream2).find(local_tail2), std::string::npos);
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CREATE_BIDI),
      2u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE),
            0u);
  DestroyRunningRuntime();
  close(peer1);
  close(peer2);
}

TEST_F(OdinXqcClientRuntimeTest, T17) {
  StartRuntime();
  FireHandshake();
  FakeStream stream;
  h.streams_to_create.push_back(&stream);
  const std::string local_tail = "client-tail!";
  const std::string server_tail = "server-tail!";
  const std::string pre_stop_request = std::string(kHttpReq) + local_tail;
  int owned = -1;
  int peer = -1;
  MakePair(&owned, &peer);
  ASSERT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned), 0);
  ASSERT_TRUE(
      WriteAllFd(peer, pre_stop_request.data(), pre_stop_request.size()));
  RunLoopFor(h.loop);
  ASSERT_FALSE(stream.sends.empty());
  EXPECT_EQ(JoinedSends(stream), EncodedConnectReq());
  ASSERT_NE(stream.user_data, nullptr);
  void *user_data = stream.user_data;
  ASSERT_EQ(odin_xqc_client_runtime_stop(h.rt), 0);
  odin_xqc_client_runtime_test_state_t stopped;
  ASSERT_EQ(odin_xqc_client_runtime_test_state(h.rt, &stopped), 0);
  EXPECT_EQ(stopped.conn, AsConn(&h.conn_a));
  EXPECT_EQ(stopped.cid_registered, 1);
  EXPECT_EQ(stopped.handshake_done, 1);
  EXPECT_TRUE(CidEq(stopped.current_cid, h.cid_a));
  EXPECT_EQ(stream.user_data_values.back(), user_data);
  EXPECT_EQ(stream.close_calls, 0);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE),
            0u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE), 0u);
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN),
      0u);
  ASSERT_EQ(odin_xqc_client_runtime_start(h.rt), 0);
  EXPECT_EQ(h.connect_calls, 1);
  stream.recv_chunks.push_back(EncodedConnectResp());
  stream.recv_chunks.push_back(server_tail);
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(AsStream(&stream),
                                                           user_data),
            XQC_OK);
  std::string downstream;
  ASSERT_TRUE(RunUntil(h.loop, [&] {
    downstream += DrainFdNow(peer);
    return downstream.find(kHttp200) != std::string::npos &&
           JoinedSends(stream).find(local_tail) != std::string::npos;
  }));
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(AsStream(&stream),
                                                           user_data),
            XQC_OK);
  ASSERT_TRUE(RunUntil(h.loop, [&] {
    downstream += DrainFdNow(peer);
    return downstream.find(std::string(kHttp200) + server_tail) !=
           std::string::npos;
  }));
  EXPECT_NE(JoinedSends(stream).find(local_tail), std::string::npos);
  EXPECT_EQ(stream.user_data_values.back(), user_data);
  EXPECT_EQ(stream.close_calls, 0);
  DestroyRunningRuntime();
  close(peer);
}

TEST_F(OdinXqcClientRuntimeTest, T18) {
  StartRuntime();
  int owned = -1;
  int peer = -1;
  MakePair(&owned, &peer);
  ASSERT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned), 0);
  FakeStream stream;
  const std::string server_tail = "server-tail!";
  QueueStream(&stream);
  stream.recv_chunks.push_back(server_tail);
  ASSERT_TRUE(WriteAllFd(peer, kHttpReq, sizeof(kHttpReq) - 1));
  h.app_callbacks->conn_cbs.conn_handshake_finished(AsConn(&h.conn_b),
                                                    h.xu_user_data, h.rt);
  h.app_callbacks->conn_cbs.conn_handshake_finished(AsConn(&h.conn_a),
                                                    h.xu_user_data, nullptr);
  RunLoopFor(h.loop);
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CREATE_BIDI),
      0u);
  EXPECT_EQ(stream.user_data, nullptr);
  EXPECT_TRUE(stream.sends.empty());
  FireHandshake();
  ASSERT_TRUE(RunUntil(h.loop, [&] {
    return stream.user_data != nullptr && !stream.sends.empty();
  }));
  ASSERT_FALSE(stream.sends.empty());
  EXPECT_EQ(stream.sends.front(), EncodedConnectReq());
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(AsStream(&stream),
                                                           stream.user_data),
            XQC_OK);
  std::string downstream;
  ASSERT_TRUE(RunUntil(h.loop, [&] {
    downstream += DrainFdNow(peer);
    return downstream == std::string(kHttp200);
  }));
  EXPECT_EQ(h.app_callbacks->stream_cbs.stream_read_notify(AsStream(&stream),
                                                           stream.user_data),
            XQC_OK);
  const std::string expected_downstream = std::string(kHttp200) + server_tail;
  ASSERT_TRUE(RunUntil(h.loop, [&] {
    downstream += DrainFdNow(peer);
    return downstream.size() >= expected_downstream.size();
  }));
  EXPECT_EQ(downstream, expected_downstream);
  DestroyRunningRuntime();
  close(peer);

  ResetFakeRuntimeState();
  StartRuntime();
  MakePair(&owned, &peer);
  ASSERT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned), 0);
  ASSERT_TRUE(WriteAllFd(peer, kHttpReq, sizeof(kHttpReq) - 1));
  h.fail_register_errno = ENOMEM;
  h.fail_register_cid = h.cid_b.cid_buf[0];
  h.transport_callbacks.conn_update_cid_notify(AsConn(&h.conn_a), &h.cid_a,
                                               &h.cid_b, h.xu_user_data);
  h.app_callbacks->conn_cbs.conn_handshake_finished(AsConn(&h.conn_a),
                                                    h.xu_user_data, h.rt);
  RunLoopFor(h.loop);
  EXPECT_TRUE(PeerSawEof(peer));
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CREATE_BIDI),
      0u);
  odin_xqc_client_runtime_destroy(h.rt);
  h.rt = nullptr;
  close(peer);
}

TEST_F(OdinXqcClientRuntimeTest, T19) {
  auto run_subcase = [&](bool engine_destroy_callback) {
    StartRuntime();
    int owned1 = -1;
    int peer1 = -1;
    int owned2 = -1;
    int peer2 = -1;
    MakePair(&owned1, &peer1);
    MakePair(&owned2, &peer2);
    ASSERT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned1), 0);
    ASSERT_EQ(odin_xqc_client_runtime_add_connection(h.rt, owned2), 0);
    ASSERT_EQ(odin_xqc_client_runtime_stop(h.rt), 0);
    odin_xqc_client_runtime_test_state_t stopped;
    ASSERT_EQ(odin_xqc_client_runtime_test_state(h.rt, &stopped), 0);
    EXPECT_EQ(stopped.conn, AsConn(&h.conn_a));
    EXPECT_EQ(stopped.cid_registered, 1);
    EXPECT_TRUE(CidEq(stopped.current_cid, h.cid_a));
    EXPECT_FALSE(PeerSawEof(peer1));
    EXPECT_FALSE(PeerSawEof(peer2));
    EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_STOP), 1u);
    EXPECT_EQ(
        CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CREATE_BIDI),
        0u);

    h.engine_destroy_invokes_close = engine_destroy_callback;
    odin_xqc_client_runtime_destroy(h.rt);
    h.rt = nullptr;
    EXPECT_TRUE(PeerSawEof(peer1));
    EXPECT_TRUE(PeerSawEof(peer2));
    EXPECT_EQ(h.conn_close_cids.size(), 0u);
    EXPECT_EQ(h.conn_a.alp_user_data, nullptr);
    EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_STOP), 1u);
    EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE),
              0u);
    EXPECT_EQ(
        CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CREATE_BIDI),
        0u);
    EXPECT_EQ(
        CountClientCallsForCid(
            ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a),
        1u);
    EXPECT_EQ(CountClientCalls(
                  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN),
              1u);
    EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY),
              1u);
    const int conn_clear =
        FirstConnSetUserDataIndex(AsConn(&h.conn_a), nullptr);
    const int unregister = FirstClientCallForCidIndex(
        ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a);
    const int udp_destroy =
        FirstClientCallIndex(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY);
    ASSERT_GE(conn_clear, 0);
    ASSERT_GE(unregister, 0);
    ASSERT_GE(udp_destroy, 0);
    EXPECT_LT(conn_clear, udp_destroy);
    EXPECT_LT(unregister, udp_destroy);
    EXPECT_EQ(h.engine_destroy_calls, 1);
    EXPECT_EQ(h.engine_destroy_callback_close_calls,
              engine_destroy_callback ? 1 : 0);
    close(peer1);
    close(peer2);
  };

  run_subcase(false);
  ResetFakeRuntimeState();
  run_subcase(true);
}

TEST_F(OdinXqcClientRuntimeTest, T20) {
  auto run_subcase = [&](bool engine_destroy_callback) {
    StartRuntime();
    FireHandshake();
    FakeStream stream1;
    FakeStream stream2;
    int peer1 = -1;
    int peer2 = -1;
    AddStalledConnection(&stream1, &peer1);
    AddStalledConnection(&stream2, &peer2);
    const int recv_calls1 = stream1.recv_calls;
    const int recv_calls2 = stream2.recv_calls;
    const int send_calls1 = stream1.send_calls;
    const int send_calls2 = stream2.send_calls;
    ASSERT_EQ(odin_xqc_client_runtime_stop(h.rt), 0);
    odin_xqc_client_runtime_test_state_t stopped;
    ASSERT_EQ(odin_xqc_client_runtime_test_state(h.rt, &stopped), 0);
    EXPECT_EQ(stopped.conn, AsConn(&h.conn_a));
    EXPECT_EQ(stopped.cid_registered, 1);
    EXPECT_EQ(stopped.handshake_done, 1);
    EXPECT_TRUE(CidEq(stopped.current_cid, h.cid_a));
    EXPECT_EQ(stream1.user_data_values.back(), stream1.user_data);
    EXPECT_EQ(stream2.user_data_values.back(), stream2.user_data);
    EXPECT_EQ(stream1.close_calls, 0);
    EXPECT_EQ(stream2.close_calls, 0);
    EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_STOP), 1u);
    EXPECT_EQ(
        CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CREATE_BIDI),
        2u);
    EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE),
              0u);

    h.engine_destroy_invokes_close = engine_destroy_callback;
    odin_xqc_client_runtime_destroy(h.rt);
    h.rt = nullptr;
    EXPECT_EQ(h.conn_close_cids.size(), 0u);
    EXPECT_EQ(h.conn_a.alp_user_data, nullptr);
    EXPECT_EQ(stream1.user_data_values.back(), nullptr);
    EXPECT_EQ(stream2.user_data_values.back(), nullptr);
    EXPECT_EQ(stream1.close_calls, 1);
    EXPECT_EQ(stream2.close_calls, 1);
    EXPECT_GT(stream1.user_data_clear_order, 0);
    EXPECT_GT(stream2.user_data_clear_order, 0);
    EXPECT_GT(stream1.close_order, stream1.user_data_clear_order);
    EXPECT_GT(stream2.close_order, stream2.user_data_clear_order);
    EXPECT_EQ(stream1.recv_calls, recv_calls1);
    EXPECT_EQ(stream2.recv_calls, recv_calls2);
    EXPECT_EQ(stream1.send_calls, send_calls1);
    EXPECT_EQ(stream2.send_calls, send_calls2);
    EXPECT_EQ(
        CountClientCallsForStream(
            ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE, AsStream(&stream1)),
        1u);
    EXPECT_EQ(
        CountClientCallsForStream(
            ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE, AsStream(&stream2)),
        1u);
    EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE),
              2u);
    EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE),
              0u);
    EXPECT_EQ(
        CountClientCallsForCid(
            ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a),
        1u);
    EXPECT_EQ(CountClientCalls(
                  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN),
              1u);
    EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY),
              1u);
    const int conn_clear =
        FirstConnSetUserDataIndex(AsConn(&h.conn_a), nullptr);
    const int unregister = FirstClientCallForCidIndex(
        ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a);
    const int udp_destroy =
        FirstClientCallIndex(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY);
    ASSERT_GE(conn_clear, 0);
    ASSERT_GE(unregister, 0);
    ASSERT_GE(udp_destroy, 0);
    EXPECT_LT(conn_clear, udp_destroy);
    EXPECT_LT(unregister, udp_destroy);
    EXPECT_EQ(h.engine_destroy_calls, 1);
    EXPECT_EQ(h.engine_destroy_callback_close_calls,
              engine_destroy_callback ? 1 : 0);
    close(peer1);
    close(peer2);
  };

  run_subcase(false);
  ResetFakeRuntimeState();
  run_subcase(true);
}

TEST_F(OdinXqcClientRuntimeTest, T21) {
  StartRuntime();
  odin_xqc_client_runtime_test_state_t before;
  ASSERT_EQ(odin_xqc_client_runtime_test_state(h.rt, &before), 0);
  EXPECT_EQ(before.conn, AsConn(&h.conn_a));
  EXPECT_TRUE(CidEq(before.current_cid, h.cid_a));
  EXPECT_EQ(before.cid_registered, 1);
  EXPECT_EQ(before.handshake_done, 0);
  EXPECT_EQ(before.closing, 0);
  const unsigned int register_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_REGISTER_CONN);
  const unsigned int conn_close_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE);
  const unsigned int unregister_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN);
  EXPECT_EQ(h.app_callbacks->conn_cbs.conn_create_notify(
                AsConn(&h.conn_b), &h.cid_b, h.xu_user_data, nullptr),
            -1);
  odin_xqc_client_runtime_test_state_t after;
  ASSERT_EQ(odin_xqc_client_runtime_test_state(h.rt, &after), 0);
  EXPECT_EQ(after.connect_errno, EALREADY);
  EXPECT_EQ(after.conn, before.conn);
  EXPECT_TRUE(CidEq(after.current_cid, before.current_cid));
  EXPECT_EQ(after.cid_registered, before.cid_registered);
  EXPECT_EQ(after.handshake_done, before.handshake_done);
  EXPECT_EQ(after.closing, before.closing);
  EXPECT_EQ(
      register_before,
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_REGISTER_CONN));
  EXPECT_EQ(conn_close_before,
            CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE));
  EXPECT_EQ(
      unregister_before,
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN));
  EXPECT_EQ(h.conn_b.alp_user_data, nullptr);

  odin_xqc_client_runtime_destroy(h.rt);
  odin_xqc_client_runtime_test_state_t closing_before;
  ASSERT_EQ(odin_xqc_client_runtime_test_state(h.rt, &closing_before), 0);
  EXPECT_EQ(closing_before.conn, AsConn(&h.conn_a));
  EXPECT_TRUE(CidEq(closing_before.current_cid, h.cid_a));
  EXPECT_EQ(closing_before.cid_registered, 1);
  EXPECT_EQ(closing_before.handshake_done, 0);
  EXPECT_EQ(closing_before.closing, 1);
  const unsigned int closing_register_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_REGISTER_CONN);
  const unsigned int closing_conn_close_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE);
  const unsigned int closing_unregister_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN);
  EXPECT_EQ(h.app_callbacks->conn_cbs.conn_create_notify(
                AsConn(&h.conn_c), &h.cid_c, h.xu_user_data, nullptr),
            -1);
  odin_xqc_client_runtime_test_state_t closing_after;
  ASSERT_EQ(odin_xqc_client_runtime_test_state(h.rt, &closing_after), 0);
  EXPECT_EQ(closing_after.connect_errno, EALREADY);
  EXPECT_EQ(closing_after.conn, closing_before.conn);
  EXPECT_TRUE(CidEq(closing_after.current_cid, closing_before.current_cid));
  EXPECT_EQ(closing_after.cid_registered, closing_before.cid_registered);
  EXPECT_EQ(closing_after.handshake_done, closing_before.handshake_done);
  EXPECT_EQ(closing_after.closing, closing_before.closing);
  EXPECT_EQ(h.conn_c.alp_user_data, nullptr);
  EXPECT_EQ(
      closing_register_before,
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_REGISTER_CONN));
  EXPECT_EQ(closing_conn_close_before,
            CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE));
  EXPECT_EQ(
      closing_unregister_before,
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN));
  FireClose(&h.conn_a, &h.cid_a);
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN),
      closing_unregister_before + 1);
  EXPECT_EQ(CountClientCallsForCid(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a),
            1u);
  h.rt = nullptr;
}

struct FactoryTransport {
  odin_transport_t base;
  odin_transport_ready_cb on_ready = nullptr;
  void *user_data = nullptr;
  std::string read_data;
  size_t read_off = 0;
  std::string writes;
  unsigned int interest = 0;
  bool read_eof = false;
  int destroy_calls = 0;
};

FactoryTransport *FromFactoryTransport(odin_transport_t *t) {
  return reinterpret_cast<FactoryTransport *>(t);
}

odin_transport_io_t FactoryRead(odin_transport_t *t, void *buf, size_t len,
                                size_t *out_n) {
  FactoryTransport *ft = FromFactoryTransport(t);
  if (ft->read_off >= ft->read_data.size()) {
    if (ft->read_eof) {
      return ODIN_TRANSPORT_EOF;
    }
    return ODIN_TRANSPORT_AGAIN;
  }
  const size_t n = std::min(len, ft->read_data.size() - ft->read_off);
  std::memcpy(buf, ft->read_data.data() + ft->read_off, n);
  ft->read_off += n;
  *out_n = n;
  return ODIN_TRANSPORT_OK;
}

odin_transport_io_t FactoryWrite(odin_transport_t *t, const void *buf,
                                 size_t len, size_t *out_n) {
  FactoryTransport *ft = FromFactoryTransport(t);
  ft->writes.append(static_cast<const char *>(buf), len);
  *out_n = len;
  return ODIN_TRANSPORT_OK;
}

int FactoryShutdownWrite(odin_transport_t *) { return 0; }

int FactorySetInterest(odin_transport_t *t, unsigned int events) {
  FactoryTransport *ft = FromFactoryTransport(t);
  ft->interest = events;
  if ((events & ODIN_TRANSPORT_WRITE) != 0) {
    ft->on_ready(t, ODIN_TRANSPORT_WRITE, ft->user_data);
  }
  if ((events & ODIN_TRANSPORT_READ) != 0 &&
      ft->read_off < ft->read_data.size()) {
    ft->on_ready(t, ODIN_TRANSPORT_READ, ft->user_data);
  }
  return 0;
}

int FactoryError(odin_transport_t *) { return 0; }

void FactoryDestroy(odin_transport_t *t) {
  FromFactoryTransport(t)->destroy_calls += 1;
}

const odin_transport_vtable_t kFactoryTransportVtable = {
    FactoryRead,        FactoryWrite, FactoryShutdownWrite,
    FactorySetInterest, FactoryError, FactoryDestroy,
};

struct FactoryState {
  FactoryTransport transport;
  int calls = 0;
  int destroying_calls = 0;
  int errnum = 0;
  bool defer_connect_resp = false;
};

int FactoryCreate(odin_transport_ready_cb on_ready, void *ready_user_data,
                  void *factory_user_data, odin_transport_t **out) {
  FactoryState *state = static_cast<FactoryState *>(factory_user_data);
  state->calls += 1;
  if (state->errnum != 0) {
    errno = state->errnum;
    return -1;
  }
  state->transport.base.vt = &kFactoryTransportVtable;
  state->transport.on_ready = on_ready;
  state->transport.user_data = ready_user_data;
  state->transport.read_data =
      state->defer_connect_resp ? std::string() : EncodedConnectResp();
  *out = &state->transport.base;
  return 0;
}

void FactoryDestroying(odin_transport_t *transport, void *factory_user_data) {
  FactoryState *state = static_cast<FactoryState *>(factory_user_data);
  EXPECT_EQ(transport, &state->transport.base);
  state->destroying_calls += 1;
}

struct ClientSessionFactoryState {
  odin_event_loop_t *loop = nullptr;
  int on_close_calls = 0;
  int on_close_err = 0;
};

void ClientSessionFactoryOnClose(odin_client_session_t *cs, int err,
                                 void *user_data) {
  ClientSessionFactoryState *state =
      static_cast<ClientSessionFactoryState *>(user_data);
  state->on_close_calls += 1;
  state->on_close_err = err;
  odin_client_session_destroy(cs);
  if (state->loop != nullptr) {
    odin_event_loop_stop(state->loop);
  }
}

class OdinClientSessionUpstreamTransportTest : public ::testing::Test {};

TEST_F(OdinClientSessionUpstreamTransportTest, T4) {
  odin_event_loop_t *loop = nullptr;
  ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);

  int owned = -1;
  int peer = -1;
  auto expect_null_precondition =
      [&](odin_event_loop_t *loop_arg,
          odin_client_session_upstream_transport_factory_cb create_arg,
          odin_client_session_close_cb close_arg,
          odin_client_session_t **out_arg) {
        MakePair(&owned, &peer);
        FactoryState factory;
        ClientSessionFactoryState close_state;
        close_state.loop = loop;
        odin_client_session_t *sentinel =
            reinterpret_cast<odin_client_session_t *>(0x55);
        if (out_arg != nullptr) {
          *out_arg = sentinel;
        }
        errno = 0;
        EXPECT_EQ(odin_client_session_create_with_upstream_transport(
                      loop_arg, owned, create_arg, &factory, FactoryDestroying,
                      close_arg, &close_state, out_arg),
                  -1);
        EXPECT_EQ(errno, EINVAL);
        if (out_arg != nullptr) {
          EXPECT_EQ(*out_arg, sentinel);
        }
        EXPECT_EQ(factory.calls, 0);
        EXPECT_EQ(close_state.on_close_calls, 0);
        EXPECT_GE(fcntl(owned, F_GETFD), 0);
        close(owned);
        close(peer);
      };

  odin_client_session_t *out = nullptr;
  expect_null_precondition(nullptr, FactoryCreate, ClientSessionFactoryOnClose,
                           &out);
  expect_null_precondition(loop, nullptr, ClientSessionFactoryOnClose, &out);
  expect_null_precondition(loop, FactoryCreate, nullptr, &out);
  expect_null_precondition(loop, FactoryCreate, ClientSessionFactoryOnClose,
                           nullptr);

  MakePair(&owned, &peer);
  ASSERT_EQ(
      odin_client_session_test_fail_next_downstream_transport_create(ENOMEM),
      0);
  FactoryState factory;
  ClientSessionFactoryState close_state;
  close_state.loop = loop;
  out = reinterpret_cast<odin_client_session_t *>(0x66);
  errno = 0;
  EXPECT_EQ(odin_client_session_create_with_upstream_transport(
                loop, owned, FactoryCreate, &factory, FactoryDestroying,
                ClientSessionFactoryOnClose, &close_state, &out),
            -1);
  EXPECT_EQ(errno, ENOMEM);
  EXPECT_EQ(out, reinterpret_cast<odin_client_session_t *>(0x66));
  EXPECT_GE(fcntl(owned, F_GETFD), 0);
  close(owned);
  close(peer);

#if defined(__APPLE__)
  MakePair(&owned, &peer);
  factory = FactoryState();
  close_state = ClientSessionFactoryState();
  close_state.loop = loop;
  out = reinterpret_cast<odin_client_session_t *>(0x77);
  ASSERT_EQ(
      odin_event_loop_test_fail_next_kqueue_change(
          loop, ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_ADD, ODIN_EVENT_READ, EIO),
      0);
  errno = 0;
  EXPECT_EQ(odin_client_session_create_with_upstream_transport(
                loop, owned, FactoryCreate, &factory, FactoryDestroying,
                ClientSessionFactoryOnClose, &close_state, &out),
            -1);
  EXPECT_EQ(errno, EIO);
  EXPECT_EQ(out, reinterpret_cast<odin_client_session_t *>(0x77));
  EXPECT_EQ(factory.calls, 0);
  EXPECT_EQ(close_state.on_close_calls, 0);
  EXPECT_GE(fcntl(owned, F_GETFD), 0);
  close(owned);
  close(peer);
#endif

  MakePair(&owned, &peer);
  const int probe_fd = dup(owned);
  ASSERT_GE(probe_fd, 0) << std::strerror(errno);
  ASSERT_TRUE(SetNonblockNoAssert(probe_fd)) << std::strerror(errno);
  factory = FactoryState();
  factory.defer_connect_resp = true;
  close_state = ClientSessionFactoryState();
  close_state.loop = loop;
  ASSERT_EQ(odin_client_session_create_with_upstream_transport(
                loop, owned, FactoryCreate, &factory, FactoryDestroying,
                ClientSessionFactoryOnClose, &close_state, &out),
            0)
      << std::strerror(errno);
  ASSERT_NE(out, nullptr);
  ASSERT_TRUE(WriteAllFd(peer, kHttpReq, sizeof(kHttpReq) - 1));
  ASSERT_TRUE(RunUntil(loop, [&] {
    return factory.calls == 1 &&
           factory.transport.writes == EncodedConnectReq();
  }));
  EXPECT_EQ(factory.calls, 1);
  EXPECT_EQ(factory.transport.writes, EncodedConnectReq());
  ASSERT_TRUE(WriteAllFd(peer, kLateLocal, sizeof(kLateLocal) - 1));
  EXPECT_TRUE(PollPeekEquals(probe_fd, std::string(kLateLocal), 200));
  EXPECT_EQ(ReadAvailableFd(peer, 50).find(kHttp200), std::string::npos);
  factory.transport.read_data = EncodedConnectResp();
  factory.transport.on_ready(&factory.transport.base, ODIN_TRANSPORT_READ,
                             factory.transport.user_data);
  std::string downstream;
  ASSERT_TRUE(RunUntil(loop, [&] {
    downstream += DrainFdNow(peer);
    return downstream.find(kHttp200) != std::string::npos &&
           factory.transport.writes.find(kLateLocal) != std::string::npos;
  }));
  EXPECT_NE(downstream.find(kHttp200), std::string::npos);
  EXPECT_NE(factory.transport.writes.find(kLateLocal), std::string::npos);
  ASSERT_EQ(shutdown(peer, SHUT_WR), 0) << std::strerror(errno);
  RunLoopFor(loop);
  factory.transport.read_eof = true;
  factory.transport.on_ready(&factory.transport.base, ODIN_TRANSPORT_READ,
                             factory.transport.user_data);
  ASSERT_TRUE(RunUntil(loop, [&] { return close_state.on_close_calls == 1; }));
  EXPECT_EQ(close_state.on_close_calls, 1);
  EXPECT_EQ(close_state.on_close_err, 0);
  EXPECT_EQ(factory.destroying_calls, 1);
  EXPECT_EQ(factory.transport.destroy_calls, 1);
  close(probe_fd);
  close(peer);

  MakePair(&owned, &peer);
  factory = FactoryState();
  factory.errnum = EMFILE;
  close_state = ClientSessionFactoryState();
  close_state.loop = loop;
  ASSERT_EQ(odin_client_session_create_with_upstream_transport(
                loop, owned, FactoryCreate, &factory, FactoryDestroying,
                ClientSessionFactoryOnClose, &close_state, &out),
            0)
      << std::strerror(errno);
  ASSERT_TRUE(WriteAllFd(peer, kHttpReq, sizeof(kHttpReq) - 1));
  std::string factory_failure;
  ASSERT_TRUE(RunUntil(loop, [&] {
    factory_failure += DrainFdNow(peer);
    return close_state.on_close_calls == 1 &&
           factory_failure.size() >= sizeof(kHttp400) - 1;
  }));
  factory_failure += ReadAvailableFd(peer, 50);
  EXPECT_EQ(factory.calls, 1);
  EXPECT_EQ(factory_failure, kHttp400);
  EXPECT_EQ(close_state.on_close_calls, 1);
  EXPECT_EQ(close_state.on_close_err, EMFILE);
  close(peer);

  MakePair(&owned, &peer);
  factory = FactoryState();
  close_state = ClientSessionFactoryState();
  close_state.loop = loop;
  ASSERT_EQ(odin_client_session_create_with_upstream_transport(
                loop, owned, FactoryCreate, &factory, nullptr,
                ClientSessionFactoryOnClose, &close_state, &out),
            0)
      << std::strerror(errno);
  ASSERT_TRUE(WriteAllFd(peer, kHttpReq, sizeof(kHttpReq) - 1));
  ASSERT_TRUE(RunUntil(loop, [&] {
    return factory.calls == 1 &&
           factory.transport.writes == EncodedConnectReq();
  }));
  EXPECT_EQ(factory.calls, 1);
  EXPECT_EQ(factory.destroying_calls, 0);
  EXPECT_EQ(factory.transport.writes, EncodedConnectReq());
  EXPECT_NE(ReadAvailableFd(peer, 200).find(kHttp200), std::string::npos);
  ASSERT_EQ(shutdown(peer, SHUT_WR), 0) << std::strerror(errno);
  RunLoopFor(loop);
  factory.transport.read_eof = true;
  factory.transport.on_ready(&factory.transport.base, ODIN_TRANSPORT_READ,
                             factory.transport.user_data);
  ASSERT_TRUE(RunUntil(loop, [&] { return close_state.on_close_calls == 1; }));
  EXPECT_EQ(close_state.on_close_calls, 1);
  EXPECT_EQ(close_state.on_close_err, 0);
  EXPECT_EQ(factory.destroying_calls, 0);
  EXPECT_EQ(factory.transport.destroy_calls, 1);
  close(peer);

  MakePair(&owned, &peer);
  factory = FactoryState();
  close_state = ClientSessionFactoryState();
  close_state.loop = loop;
  ASSERT_EQ(odin_client_session_create_with_upstream_transport(
                loop, owned, FactoryCreate, &factory, nullptr,
                ClientSessionFactoryOnClose, &close_state, &out),
            0)
      << std::strerror(errno);
  ASSERT_EQ(odin_connect_session_test_fail_next_create_client(ENOMEM), 0);
  ASSERT_TRUE(WriteAllFd(peer, kHttpReq, sizeof(kHttpReq) - 1));
  std::string post_factory_failure;
  ASSERT_TRUE(RunUntil(loop, [&] {
    post_factory_failure += DrainFdNow(peer);
    return factory.calls == 1 && close_state.on_close_calls == 1 &&
           post_factory_failure.size() >= sizeof(kHttp400) - 1;
  }));
  post_factory_failure += ReadAvailableFd(peer, 50);
  EXPECT_EQ(factory.calls, 1);
  EXPECT_EQ(factory.destroying_calls, 0);
  EXPECT_EQ(factory.transport.destroy_calls, 1);
  EXPECT_TRUE(factory.transport.writes.empty());
  EXPECT_EQ(post_factory_failure, kHttp400);
  EXPECT_EQ(close_state.on_close_calls, 1);
  EXPECT_EQ(close_state.on_close_err, ENOMEM);
  close(peer);
  odin_event_loop_destroy(loop);
}

std::string FindRepoRoot() {
  char cwd_buf[PATH_MAX];
  if (getcwd(cwd_buf, sizeof(cwd_buf)) == nullptr) {
    return ".";
  }
  std::string dir = cwd_buf;
  for (;;) {
    const std::string candidate =
        dir + "/odin/check_client_xqc_runtime_scope.py";
    if (access(candidate.c_str(), R_OK) == 0) {
      return dir;
    }
    const size_t slash = dir.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
      return ".";
    }
    dir.resize(slash);
  }
}

int RunScopeCheck(const std::string &root) {
  const std::string script = root + "/odin/check_client_xqc_runtime_scope.py";
  const std::string stamp =
      std::string("/tmp/odin_client_xqc_scope_") + std::to_string(getpid());
  const pid_t pid = fork();
  if (pid == 0) {
    execlp("python3", "python3", script.c_str(), "--root", root.c_str(),
           "--stamp", stamp.c_str(), static_cast<char *>(nullptr));
    _exit(127);
  }
  if (pid < 0) {
    return -1;
  }
  int status = 0;
  while (waitpid(pid, &status, 0) == -1) {
    if (errno != EINTR) {
      return -1;
    }
  }
  unlink(stamp.c_str());
  if (!WIFEXITED(status)) {
    return -1;
  }
  return WEXITSTATUS(status);
}

class OdinXqcClientRuntimeScopeTest : public ::testing::Test {};

TEST_F(OdinXqcClientRuntimeScopeTest, T12) {
  odin_xqc_client_runtime_test_reset();
  const unsigned int before = odin_xqc_client_runtime_test_record()->call_count;

  const std::string root = FindRepoRoot();
  ASSERT_EQ(RunScopeCheck(root), 0);
  EXPECT_EQ(odin_xqc_client_runtime_test_record()->call_count, before);
}

TEST_F(OdinXqcClientRuntimeTest, RFC028T10ForceDestroyIsSynchronous) {
  StartRuntime();
  FireHandshake();
  FakeStream live_stream;
  int live_peer = -1;
  AddStalledConnection(&live_stream, &live_peer);
  ASSERT_NE(live_stream.user_data, nullptr);
  int queued_owned = -1;
  int queued_peer = -1;
  MakePair(&queued_owned, &queued_peer);
  ASSERT_EQ(odin_xqc_client_runtime_test_append_pending_fd(h.rt, queued_owned),
            0)
      << std::strerror(errno);
  queued_owned = -1;
  odin_xqc_client_runtime_test_state_t before_destroy;
  ASSERT_EQ(odin_xqc_client_runtime_test_state(h.rt, &before_destroy), 0);
  EXPECT_EQ(before_destroy.pending_fds, 1u);
  EXPECT_EQ(before_destroy.live_sessions, 1u);
  EXPECT_FALSE(PeerSawEof(queued_peer));
  EXPECT_FALSE(PeerSawEof(live_peer));
  h.engine_destroy_invokes_close = true;
  const int live_recv_before = live_stream.recv_calls;
  const int live_send_before = live_stream.send_calls;
  const unsigned int stream_create_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CREATE_BIDI);
  EXPECT_EQ(stream_create_before, 1u);
  const unsigned int conn_close_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE);
  const unsigned int stream_close_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE);
  const unsigned int unregister_before = CountClientCallsForCid(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a);
  const unsigned int alpn_unregister_before = CountClientCalls(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN);
  const unsigned int udp_destroy_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY);
  const unsigned int runtime_free_before =
      odin_xqc_client_runtime_test_record()->runtime_free_calls;
  const unsigned int runtime_free_call_before =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_RUNTIME_FREE);
  odin_xqc_client_runtime_force_destroy(h.rt);
  h.rt = nullptr;
  EXPECT_TRUE(PeerSawEof(queued_peer));
  EXPECT_TRUE(PeerSawEof(live_peer));
  EXPECT_EQ(live_stream.user_data_values.back(), nullptr);
  EXPECT_EQ(live_stream.close_calls, 1);
  EXPECT_GT(live_stream.user_data_clear_order, 0);
  EXPECT_GT(live_stream.close_order, live_stream.user_data_clear_order);
  EXPECT_EQ(live_stream.recv_calls, live_recv_before);
  EXPECT_EQ(live_stream.send_calls, live_send_before);
  EXPECT_EQ(
      CountClientCallsForStream(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE,
                                AsStream(&live_stream)),
      1u);
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CREATE_BIDI),
      stream_create_before);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE),
            stream_close_before + 1u);
  EXPECT_EQ(h.engine_destroy_callback_close_calls, 1);
  EXPECT_EQ(h.conn_close_cids.size(), 0u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE),
            conn_close_before);
  EXPECT_EQ(CountClientCallsForCid(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN, h.cid_a),
            unregister_before + 1u);
  EXPECT_EQ(CountClientCalls(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN),
            alpn_unregister_before + 1u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY),
            udp_destroy_before + 1u);
  EXPECT_EQ(odin_xqc_client_runtime_test_record()->runtime_free_calls,
            runtime_free_before + 1u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_RUNTIME_FREE),
            runtime_free_call_before + 1u);
  close(queued_peer);
  close(live_peer);
}

TEST_F(OdinXqcClientRuntimeTest, RFC028T13ForceDestroyNullReceiverIsInert) {
  odin_xqc_client_runtime_test_reset();
  const unsigned int before_null =
      odin_xqc_client_runtime_test_record()->force_destroy_null_calls;
  const unsigned int before_free =
      odin_xqc_client_runtime_test_record()->runtime_free_calls;
  const unsigned int before_calls =
      odin_xqc_client_runtime_test_record()->call_count;
  const unsigned int before_udp_destroy =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY);
  const unsigned int before_alpn_unregister = CountClientCalls(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN);
  const unsigned int before_unregister =
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN);
  odin_xqc_client_runtime_force_destroy(nullptr);
  EXPECT_EQ(odin_xqc_client_runtime_test_record()->force_destroy_null_calls,
            before_null + 1u);
  EXPECT_EQ(odin_xqc_client_runtime_test_record()->runtime_free_calls,
            before_free);
  EXPECT_EQ(odin_xqc_client_runtime_test_record()->call_count, before_calls);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY),
            before_udp_destroy);
  EXPECT_EQ(CountClientCalls(
                ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN),
            before_alpn_unregister);
  EXPECT_EQ(
      CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN),
      before_unregister);
}

TEST_F(OdinXqcClientRuntimeTest, RFC028T17DefaultCreateNullPreconditions) {
  DestroyFixtureLoop();
  ExpectEventLoopLivenessZero();
  odin_xqc_client_runtime_test_reset();
  odin_event_loop_t *loop = nullptr;
  ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
  struct sockaddr_in local = Loopback4(0);
  struct sockaddr_in peer = Loopback4(4433);
  odin_xqc_client_runtime_default_config_t config{};
  config.loop = loop;
  config.local_addr = reinterpret_cast<const struct sockaddr *>(&local);
  config.local_addrlen = sizeof(local);
  config.peer_addr = reinterpret_cast<const struct sockaddr *>(&peer);
  config.peer_addrlen = sizeof(peer);
  config.server_host = "127.0.0.1";

  auto expect_invalid =
      [&](const odin_xqc_client_runtime_default_config_t *bad_config) {
        odin_xqc_client_runtime_t *sentinel =
            reinterpret_cast<odin_xqc_client_runtime_t *>(0x2828);
        errno = 0;
        EXPECT_EQ(odin_xqc_client_runtime_create_default(bad_config, &sentinel),
                  -1);
        EXPECT_EQ(errno, EINVAL);
        EXPECT_EQ(sentinel,
                  reinterpret_cast<odin_xqc_client_runtime_t *>(0x2828));
      };

  expect_invalid(nullptr);
  odin_xqc_client_runtime_t *sentinel =
      reinterpret_cast<odin_xqc_client_runtime_t *>(0x2929);
  errno = 0;
  EXPECT_EQ(odin_xqc_client_runtime_create_default(&config, nullptr), -1);
  EXPECT_EQ(errno, EINVAL);
  EXPECT_EQ(sentinel, reinterpret_cast<odin_xqc_client_runtime_t *>(0x2929));
  odin_xqc_client_runtime_default_config_t invalid = config;
  invalid.loop = nullptr;
  expect_invalid(&invalid);
  invalid = config;
  invalid.local_addr = nullptr;
  expect_invalid(&invalid);
  invalid = config;
  invalid.peer_addr = nullptr;
  expect_invalid(&invalid);
  invalid = config;
  invalid.server_host = nullptr;
  expect_invalid(&invalid);
  EXPECT_EQ(odin_xqc_client_runtime_test_record()->default_create_calls, 0u);
  EXPECT_EQ(odin_xqc_client_runtime_test_record()->udp_create_calls, 0u);
  EXPECT_EQ(odin_xqc_client_runtime_test_record()->call_count, 0u);
  odin_event_loop_destroy(loop);
  ExpectEventLoopLivenessZero();
}

TEST_F(OdinXqcClientRuntimeTest, RFC028T18DefaultCreateAddressShapes) {
  DestroyFixtureLoop();
  ExpectEventLoopLivenessZero();
  odin_xqc_client_runtime_test_reset();
  odin_event_loop_t *loop = nullptr;
  ASSERT_EQ(odin_event_loop_create(&loop), 0) << std::strerror(errno);
  struct sockaddr_in local4 = Loopback4(0);
  struct sockaddr_in peer4 = Loopback4(4433);
  struct sockaddr_in6 local6 = Loopback6(0);
  struct sockaddr_in6 peer6 = Loopback6(4433);
  odin_xqc_client_runtime_default_config_t config{};
  config.loop = loop;
  config.local_addr = reinterpret_cast<const struct sockaddr *>(&local4);
  config.local_addrlen = sizeof(local4);
  config.peer_addr = reinterpret_cast<const struct sockaddr *>(&peer4);
  config.peer_addrlen = sizeof(peer4);
  config.server_host = "127.0.0.1";

  auto expect_invalid = [&](odin_xqc_client_runtime_default_config_t bad) {
    odin_xqc_client_runtime_t *sentinel =
        reinterpret_cast<odin_xqc_client_runtime_t *>(0x3838);
    errno = 0;
    EXPECT_EQ(odin_xqc_client_runtime_create_default(&bad, &sentinel), -1);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(sentinel, reinterpret_cast<odin_xqc_client_runtime_t *>(0x3838));
  };
  odin_xqc_client_runtime_default_config_t invalid = config;
  invalid.local_addrlen = sizeof(local4) - 1u;
  expect_invalid(invalid);
  invalid = config;
  invalid.local_addrlen = sizeof(local4) + 1u;
  expect_invalid(invalid);
  struct sockaddr bad_family{};
  bad_family.sa_family = AF_UNIX;
  invalid = config;
  invalid.local_addr = &bad_family;
  invalid.local_addrlen = sizeof(bad_family);
  expect_invalid(invalid);
  invalid = config;
  invalid.peer_addrlen = sizeof(peer4) - 1u;
  expect_invalid(invalid);
  invalid = config;
  invalid.peer_addrlen = sizeof(peer4) + 1u;
  expect_invalid(invalid);
  invalid = config;
  invalid.peer_addr = &bad_family;
  invalid.peer_addrlen = sizeof(bad_family);
  expect_invalid(invalid);
  EXPECT_EQ(odin_xqc_client_runtime_test_record()->default_create_calls, 0u);
  EXPECT_EQ(odin_xqc_client_runtime_test_record()->udp_create_calls, 0u);
  EXPECT_EQ(odin_xqc_client_runtime_test_record()->call_count, 0u);
  odin_event_loop_destroy(loop);
  ExpectEventLoopLivenessZero();

  InstallOps();
  h.expected_server_host = "127.0.0.1";
  h.expected_no_crypto_flag = 0;
  auto expect_default_fields = [] {
    EXPECT_EQ(odin_xqc_client_runtime_test_record()
                  ->last_default_create.engine_config,
              nullptr);
    EXPECT_EQ(odin_xqc_client_runtime_test_record()
                  ->last_default_create.transport_callbacks,
              nullptr);
    EXPECT_EQ(odin_xqc_client_runtime_test_record()->last_default_create.token,
              nullptr);
    EXPECT_EQ(
        odin_xqc_client_runtime_test_record()->last_default_create.token_len,
        0u);
    EXPECT_EQ(odin_xqc_client_runtime_test_record()
                  ->last_default_create.conn_ssl_config_value.cert_verify_flag,
              0);
    EXPECT_EQ(odin_xqc_client_runtime_test_record()
                  ->last_default_create.no_crypto_flag,
              0);
  };
  auto expect_success = [&](odin_xqc_client_runtime_default_config_t valid,
                            unsigned int expected_successes,
                            bool start_runtime) {
    odin_event_loop_t *success_loop = nullptr;
    ASSERT_EQ(odin_event_loop_create(&success_loop), 0) << std::strerror(errno);
    valid.loop = success_loop;
    odin_xqc_client_runtime_t *rt = nullptr;
    ASSERT_EQ(odin_xqc_client_runtime_create_default(&valid, &rt), 0)
        << std::strerror(errno);
    EXPECT_NE(rt, nullptr);
    EXPECT_EQ(odin_xqc_client_runtime_test_record()->default_create_calls,
              expected_successes);
    EXPECT_EQ(odin_xqc_client_runtime_test_record()->udp_create_calls,
              expected_successes);
    expect_default_fields();
    if (start_runtime) {
      ASSERT_EQ(odin_xqc_client_runtime_start(rt), 0);
    }
    odin_xqc_client_runtime_force_destroy(rt);
    EXPECT_EQ(odin_xqc_client_runtime_test_record()->runtime_free_calls,
              expected_successes);
    EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY),
              expected_successes);
    odin_event_loop_destroy(success_loop);
    ExpectEventLoopLivenessZero();
  };

  odin_xqc_client_runtime_default_config_t valid = config;
  valid.local_addr = reinterpret_cast<const struct sockaddr *>(&local6);
  valid.local_addrlen = sizeof(local6);
  expect_success(valid, 1u, false);
  EXPECT_EQ(
      odin_xqc_client_runtime_test_record()->last_udp_create.local_addrlen,
      static_cast<socklen_t>(sizeof(struct sockaddr_in6)));

  valid = config;
  valid.local_addr = reinterpret_cast<const struct sockaddr *>(&local4);
  valid.local_addrlen = sizeof(local4);
  valid.peer_addr = reinterpret_cast<const struct sockaddr *>(&peer6);
  valid.peer_addrlen = sizeof(peer6);
  SetExpectedPeer(reinterpret_cast<const struct sockaddr *>(&peer6),
                  sizeof(peer6));
  expect_success(valid, 2u, true);
  EXPECT_EQ(odin_xqc_client_runtime_test_record()->default_create_calls, 2u);
  EXPECT_EQ(odin_xqc_client_runtime_test_record()->udp_create_calls, 2u);
  EXPECT_EQ(odin_xqc_client_runtime_test_record()->runtime_free_calls, 2u);
  EXPECT_EQ(CountClientCalls(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY),
            2u);
}

} // namespace

// NOLINTEND(misc-const-correctness, misc-use-internal-linkage)
