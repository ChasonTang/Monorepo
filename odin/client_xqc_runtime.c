/* odin/client_xqc_runtime.c -- RFC-027 client-side xquic runtime. */

#include "odin/client_xqc_runtime.h"

#include <errno.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "odin/client_session.h"
#include "odin/transport.h"
#include "odin/transport_xqc.h"

#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
#include "odin/testing/client_xqc_runtime_internal_test.h"
#endif

typedef struct odin_xqc_client_pending_fd_t odin_xqc_client_pending_fd_t;
typedef struct odin_xqc_client_stream_ctx_t odin_xqc_client_stream_ctx_t;

struct odin_xqc_client_pending_fd_t {
  odin_xqc_client_pending_fd_t *next;
  int fd;
};

struct odin_xqc_client_stream_ctx_t {
  odin_xqc_client_runtime_t *rt;
  odin_xqc_client_stream_ctx_t *session_prev;
  odin_xqc_client_stream_ctx_t *session_next;
  odin_xqc_client_stream_ctx_t *map_prev;
  odin_xqc_client_stream_ctx_t *map_next;
  xqc_stream_t *stream;
  odin_transport_t *transport;
  odin_client_session_t *cs;
};

struct odin_xqc_client_runtime_t {
  odin_event_loop_t *loop;
  odin_xqc_udp_t *xu;
  xqc_transport_callbacks_t transport_callbacks;
  xqc_app_proto_callbacks_t app_callbacks;
  struct sockaddr_storage peer_addr_storage;
  socklen_t peer_addrlen;
  char *server_host;
  unsigned char *token;
  unsigned int token_len;
  xqc_conn_settings_t conn_settings;
  xqc_conn_ssl_config_t conn_ssl_config;
  char empty_session_ticket;
  int no_crypto_flag;

  xqc_connection_t *conn;
  xqc_cid_t current_cid;
  int cid_registered;
  int connect_started;
  int udp_running;
  int startup_connecting;
  int handshake_done;
  int closing;
  int destroy_pending;
  int destroy_close_requested;
  int force_destroy_active;
  int finish_destroy_in_progress;
  int alpn_registered;
  int connect_errno;

  odin_xqc_client_pending_fd_t *pending_head;
  odin_xqc_client_pending_fd_t *pending_tail;
  odin_xqc_client_stream_ctx_t *sessions;
  odin_xqc_client_stream_ctx_t *streams_by_transport;

#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
  int fail_next_stream_context_alloc_armed;
  int fail_next_stream_context_alloc_errno;
  int fail_next_pending_queue_append_armed;
  int fail_next_pending_queue_append_errno;
  odin_xqc_client_runtime_t *test_live_prev;
  odin_xqc_client_runtime_t *test_live_next;
#endif
};

static int runtime_conn_create_notify(xqc_connection_t *conn,
                                      const xqc_cid_t *cid,
                                      void *conn_user_data,
                                      void *conn_proto_data);
static int runtime_conn_close_notify(xqc_connection_t *conn,
                                     const xqc_cid_t *cid, void *conn_user_data,
                                     void *conn_proto_data);
static void runtime_conn_handshake_finished(xqc_connection_t *conn,
                                            void *conn_user_data,
                                            void *conn_proto_data);
static void runtime_conn_update_cid(xqc_connection_t *conn,
                                    const xqc_cid_t *retire_cid,
                                    const xqc_cid_t *new_cid,
                                    void *conn_user_data);
static xqc_int_t runtime_stream_read_notify(xqc_stream_t *stream,
                                            void *strm_user_data);
static xqc_int_t runtime_stream_write_notify(xqc_stream_t *stream,
                                             void *strm_user_data);
static xqc_int_t runtime_stream_close_notify(xqc_stream_t *stream,
                                             void *strm_user_data);
static void runtime_stream_closing_notify(xqc_stream_t *stream,
                                          xqc_int_t err_code,
                                          void *strm_user_data);
static void
runtime_client_session_upstream_destroying(odin_transport_t *transport,
                                           void *factory_user_data);
static void runtime_client_session_on_close(odin_client_session_t *cs, int err,
                                            void *user_data);

#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
static odin_xqc_client_runtime_test_record_t g_client_xqc_test_record;
static odin_xqc_client_runtime_test_ops_t g_client_xqc_test_ops;
static int g_config_copy_alloc_site;
static int g_config_copy_alloc_errno;
static odin_xqc_client_runtime_t *g_client_xqc_test_live_runtimes;

static void runtime_test_register_live(odin_xqc_client_runtime_t *rt) {
  rt->test_live_prev = NULL;
  rt->test_live_next = g_client_xqc_test_live_runtimes;
  if (g_client_xqc_test_live_runtimes != NULL) {
    g_client_xqc_test_live_runtimes->test_live_prev = rt;
  }
  g_client_xqc_test_live_runtimes = rt;
}

static void runtime_test_unregister_live(odin_xqc_client_runtime_t *rt) {
  if (rt->test_live_prev != NULL) {
    rt->test_live_prev->test_live_next = rt->test_live_next;
  } else if (g_client_xqc_test_live_runtimes == rt) {
    g_client_xqc_test_live_runtimes = rt->test_live_next;
  }
  if (rt->test_live_next != NULL) {
    rt->test_live_next->test_live_prev = rt->test_live_prev;
  }
  rt->test_live_prev = NULL;
  rt->test_live_next = NULL;
}

static void runtime_test_clear_live_hooks(void) {
  for (odin_xqc_client_runtime_t *rt = g_client_xqc_test_live_runtimes;
       rt != NULL; rt = rt->test_live_next) {
    rt->fail_next_stream_context_alloc_armed = 0;
    rt->fail_next_stream_context_alloc_errno = 0;
    rt->fail_next_pending_queue_append_armed = 0;
    rt->fail_next_pending_queue_append_errno = 0;
  }
}

static odin_xqc_client_runtime_test_call_t *
runtime_test_append_call(odin_xqc_client_runtime_test_call_kind_t kind) {
  if (g_client_xqc_test_record.call_count >=
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CAP) {
    g_client_xqc_test_record.dropped_call_count += 1;
    return NULL;
  }
  odin_xqc_client_runtime_test_call_t *call =
      &g_client_xqc_test_record.calls[g_client_xqc_test_record.call_count++];
  memset(call, 0, sizeof(*call));
  call->kind = kind;
  return call;
}

static int runtime_test_config_copy_alloc_should_fail(
    odin_xqc_client_runtime_test_config_copy_alloc_t site) {
  if (g_config_copy_alloc_site != (int)site) {
    return 0;
  }
  const int errnum = g_config_copy_alloc_errno;
  g_config_copy_alloc_site = 0;
  g_config_copy_alloc_errno = 0;
  errno = errnum;
  return 1;
}

void odin_xqc_client_runtime_test_reset(void) {
  runtime_test_clear_live_hooks();
  memset(&g_client_xqc_test_record, 0, sizeof(g_client_xqc_test_record));
  memset(&g_client_xqc_test_ops, 0, sizeof(g_client_xqc_test_ops));
  g_config_copy_alloc_site = 0;
  g_config_copy_alloc_errno = 0;
}

void odin_xqc_client_runtime_test_set_ops(
    const odin_xqc_client_runtime_test_ops_t *ops) {
  if (ops == NULL) {
    memset(&g_client_xqc_test_ops, 0, sizeof(g_client_xqc_test_ops));
    return;
  }
  g_client_xqc_test_ops = *ops;
}

const odin_xqc_client_runtime_test_record_t *
odin_xqc_client_runtime_test_record(void) {
  return &g_client_xqc_test_record;
}

int odin_xqc_client_runtime_test_state(
    const odin_xqc_client_runtime_t *rt,
    odin_xqc_client_runtime_test_state_t *out) {
  if (rt == NULL || out == NULL) {
    errno = EINVAL;
    return -1;
  }
  size_t pending_fds = 0;
  for (const odin_xqc_client_pending_fd_t *node = rt->pending_head;
       node != NULL; node = node->next) {
    pending_fds += 1u;
  }
  size_t live_sessions = 0;
  for (const odin_xqc_client_stream_ctx_t *stream_ctx = rt->sessions;
       stream_ctx != NULL; stream_ctx = stream_ctx->session_next) {
    live_sessions += 1u;
  }
  out->conn = rt->conn;
  out->current_cid = rt->current_cid;
  out->cid_registered = rt->cid_registered;
  out->handshake_done = rt->handshake_done;
  out->closing = rt->closing;
  out->connect_errno = rt->connect_errno;
  out->pending_fds = pending_fds;
  out->live_sessions = live_sessions;
  return 0;
}

int odin_xqc_client_runtime_test_fail_next_stream_context_alloc(
    odin_xqc_client_runtime_t *rt, int errnum) {
  if (rt == NULL || errnum <= 0) {
    errno = EINVAL;
    return -1;
  }
  rt->fail_next_stream_context_alloc_armed = 1;
  rt->fail_next_stream_context_alloc_errno = errnum;
  return 0;
}

int odin_xqc_client_runtime_test_fail_next_pending_queue_append(
    odin_xqc_client_runtime_t *rt, int errnum) {
  if (rt == NULL || errnum <= 0) {
    errno = EINVAL;
    return -1;
  }
  rt->fail_next_pending_queue_append_armed = 1;
  rt->fail_next_pending_queue_append_errno = errnum;
  return 0;
}

int odin_xqc_client_runtime_test_fail_config_copy_alloc(
    odin_xqc_client_runtime_test_config_copy_alloc_t site, int errnum) {
  if (site != ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_SERVER_HOST &&
      site != ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_TOKEN &&
      site != ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_SESSION_TICKET &&
      site != ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_TRANSPORT_PARAMETERS) {
    errno = EINVAL;
    return -1;
  }
  if (errnum <= 0) {
    errno = EINVAL;
    return -1;
  }
  g_config_copy_alloc_site = (int)site;
  g_config_copy_alloc_errno = errnum;
  return 0;
}
#endif

static void runtime_noop_save_token(const unsigned char *token,
                                    uint32_t token_len, void *conn_user_data) {
  const int saved = errno;
  (void)token;
  (void)token_len;
  (void)conn_user_data;
  errno = saved;
}

static void runtime_noop_save_string(const char *data, size_t data_len,
                                     void *conn_user_data) {
  const int saved = errno;
  (void)data;
  (void)data_len;
  (void)conn_user_data;
  errno = saved;
}

static int runtime_udp_create_call(const odin_xqc_udp_config_t *config,
                                   odin_xqc_udp_t **out) {
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
  odin_xqc_client_runtime_test_call_t *call =
      runtime_test_append_call(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_CREATE);
  if (call != NULL) {
    call->user_data = config != NULL ? config->app_user_data : NULL;
  }
  g_client_xqc_test_record.udp_create_calls += 1;
  memset(&g_client_xqc_test_record.last_udp_create, 0,
         sizeof(g_client_xqc_test_record.last_udp_create));
  if (config != NULL) {
    g_client_xqc_test_record.last_udp_create.loop = config->loop;
    g_client_xqc_test_record.last_udp_create.local_addr = config->local_addr;
    g_client_xqc_test_record.last_udp_create.local_addrlen =
        config->local_addrlen;
    g_client_xqc_test_record.last_udp_create.engine_type = config->engine_type;
    g_client_xqc_test_record.last_udp_create.engine_config =
        config->engine_config;
    g_client_xqc_test_record.last_udp_create.ssl_config = config->ssl_config;
    g_client_xqc_test_record.last_udp_create.engine_callbacks =
        config->engine_callbacks;
    if (config->engine_callbacks != NULL) {
      g_client_xqc_test_record.last_udp_create.engine_callbacks_value =
          *config->engine_callbacks;
    }
    g_client_xqc_test_record.last_udp_create.transport_callbacks =
        config->transport_callbacks;
    if (config->transport_callbacks != NULL) {
      g_client_xqc_test_record.last_udp_create.transport_callbacks_value =
          *config->transport_callbacks;
    }
    g_client_xqc_test_record.last_udp_create.app_user_data =
        config->app_user_data;
  }
#endif
  const int rc = odin_xqc_udp_create(config, out);
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
  if (call != NULL) {
    call->int_result = rc;
    call->xu = out != NULL && rc == 0 ? *out : NULL;
  }
#endif
  return rc;
}

static int runtime_udp_start_call(odin_xqc_udp_t *xu) {
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
  odin_xqc_client_runtime_test_call_t *call =
      runtime_test_append_call(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_START);
  if (call != NULL) {
    call->xu = xu;
  }
#endif
  const int rc = odin_xqc_udp_start(xu);
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
  if (call != NULL) {
    call->int_result = rc;
  }
#endif
  return rc;
}

static int runtime_udp_stop_call(odin_xqc_udp_t *xu) {
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
  odin_xqc_client_runtime_test_call_t *call =
      runtime_test_append_call(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_STOP);
  if (call != NULL) {
    call->xu = xu;
  }
#endif
  const int rc = odin_xqc_udp_stop(xu);
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
  if (call != NULL) {
    call->int_result = rc;
  }
#endif
  return rc;
}

static void runtime_udp_destroy_call(odin_xqc_udp_t *xu) {
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
  odin_xqc_client_runtime_test_call_t *call =
      runtime_test_append_call(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY);
  if (call != NULL) {
    call->xu = xu;
  }
#endif
  odin_xqc_udp_destroy(xu);
}

static int runtime_udp_register_conn_call(odin_xqc_udp_t *xu,
                                          const xqc_cid_t *cid) {
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
  odin_xqc_client_runtime_test_call_t *call = runtime_test_append_call(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_REGISTER_CONN);
  if (call != NULL) {
    call->xu = xu;
    if (cid != NULL) {
      call->cid = *cid;
    }
  }
  int rc = 0;
  if (g_client_xqc_test_ops.udp_register_conn != NULL) {
    rc = g_client_xqc_test_ops.udp_register_conn(xu, cid);
  } else {
    rc = odin_xqc_udp_register_conn(xu, cid);
  }
  if (call != NULL) {
    call->int_result = rc;
  }
  return rc;
#else
  return odin_xqc_udp_register_conn(xu, cid);
#endif
}

static void runtime_udp_unregister_conn_call(odin_xqc_udp_t *xu,
                                             const xqc_cid_t *cid) {
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
  odin_xqc_client_runtime_test_call_t *call = runtime_test_append_call(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN);
  if (call != NULL) {
    call->xu = xu;
    if (cid != NULL) {
      call->cid = *cid;
    }
  }
  if (g_client_xqc_test_ops.udp_unregister_conn != NULL) {
    g_client_xqc_test_ops.udp_unregister_conn(xu, cid);
    return;
  }
#endif
  odin_xqc_udp_unregister_conn(xu, cid);
}

static xqc_int_t runtime_engine_register_alpn_call(
    xqc_engine_t *engine, const char *alpn, size_t alpn_len,
    xqc_app_proto_callbacks_t *app_callbacks, void *user_data) {
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
  odin_xqc_client_runtime_test_call_t *call = runtime_test_append_call(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_REGISTER_ALPN);
  if (call != NULL) {
    call->engine = engine;
    call->alpn = alpn;
    call->alpn_len = alpn_len;
    call->app_callbacks = app_callbacks;
    call->user_data = user_data;
  }
  xqc_int_t rc = XQC_OK;
  if (g_client_xqc_test_ops.engine_register_alpn != NULL) {
    rc = g_client_xqc_test_ops.engine_register_alpn(engine, alpn, alpn_len,
                                                    app_callbacks, user_data);
  } else {
    rc = xqc_engine_register_alpn(engine, alpn, alpn_len, app_callbacks,
                                  user_data);
  }
  if (call != NULL) {
    call->xqc_result = rc;
  }
  return rc;
#else
  return xqc_engine_register_alpn(engine, alpn, alpn_len, app_callbacks,
                                  user_data);
#endif
}

static xqc_int_t runtime_engine_unregister_alpn_call(xqc_engine_t *engine,
                                                     const char *alpn,
                                                     size_t alpn_len) {
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
  odin_xqc_client_runtime_test_call_t *call = runtime_test_append_call(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN);
  if (call != NULL) {
    call->engine = engine;
    call->alpn = alpn;
    call->alpn_len = alpn_len;
  }
  xqc_int_t rc = XQC_OK;
  if (g_client_xqc_test_ops.engine_unregister_alpn != NULL) {
    rc = g_client_xqc_test_ops.engine_unregister_alpn(engine, alpn, alpn_len);
  } else {
    rc = xqc_engine_unregister_alpn(engine, alpn, alpn_len);
  }
  if (call != NULL) {
    call->xqc_result = rc;
  }
  return rc;
#else
  return xqc_engine_unregister_alpn(engine, alpn, alpn_len);
#endif
}

static const xqc_cid_t *runtime_xqc_connect_call(
    xqc_engine_t *engine, const xqc_conn_settings_t *conn_settings,
    const unsigned char *token, unsigned int token_len, const char *server_host,
    int no_crypto_flag, const xqc_conn_ssl_config_t *conn_ssl_config,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen, const char *alpn,
    void *user_data) {
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
  odin_xqc_client_runtime_test_call_t *call =
      runtime_test_append_call(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_XQC_CONNECT);
  if (call != NULL) {
    call->engine = engine;
    call->alpn = alpn;
    call->alpn_len = alpn != NULL ? strlen(alpn) : 0;
    call->user_data = user_data;
  }
  const xqc_cid_t *cid = NULL;
  if (g_client_xqc_test_ops.xqc_connect != NULL) {
    cid = g_client_xqc_test_ops.xqc_connect(
        engine, conn_settings, token, token_len, server_host, no_crypto_flag,
        conn_ssl_config, peer_addr, peer_addrlen, alpn, user_data);
  } else {
    cid = xqc_connect(engine, conn_settings, token, token_len, server_host,
                      no_crypto_flag, conn_ssl_config, peer_addr, peer_addrlen,
                      alpn, user_data);
  }
  if (call != NULL) {
    call->int_result = cid != NULL ? 0 : -1;
  }
  return cid;
#else
  return xqc_connect(engine, conn_settings, token, token_len, server_host,
                     no_crypto_flag, conn_ssl_config, peer_addr, peer_addrlen,
                     alpn, user_data);
#endif
}

static void runtime_conn_set_alp_user_data_call(xqc_connection_t *conn,
                                                void *user_data) {
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
  odin_xqc_client_runtime_test_call_t *call = runtime_test_append_call(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_SET_ALP_USER_DATA);
  if (call != NULL) {
    call->conn = conn;
    call->user_data = user_data;
  }
  if (g_client_xqc_test_ops.conn_set_alp_user_data != NULL) {
    g_client_xqc_test_ops.conn_set_alp_user_data(conn, user_data);
    return;
  }
#endif
  xqc_conn_set_alp_user_data(conn, user_data);
}

static xqc_int_t runtime_conn_close_call(xqc_engine_t *engine,
                                         const xqc_cid_t *cid) {
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
  odin_xqc_client_runtime_test_call_t *call =
      runtime_test_append_call(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE);
  if (call != NULL) {
    call->engine = engine;
    if (cid != NULL) {
      call->cid = *cid;
    }
  }
  xqc_int_t rc = XQC_OK;
  if (g_client_xqc_test_ops.conn_close != NULL) {
    rc = g_client_xqc_test_ops.conn_close(engine, cid);
  } else {
    rc = xqc_conn_close(engine, cid);
  }
  if (call != NULL) {
    call->xqc_result = rc;
  }
  return rc;
#else
  return xqc_conn_close(engine, cid);
#endif
}

static void *
runtime_get_conn_alp_user_data_by_stream_call(xqc_stream_t *stream) {
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
  odin_xqc_client_runtime_test_call_t *call = runtime_test_append_call(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_GET_CONN_ALP_USER_DATA_BY_STREAM);
  if (call != NULL) {
    call->stream = stream;
  }
  void *user_data = NULL;
  if (g_client_xqc_test_ops.get_conn_alp_user_data_by_stream != NULL) {
    user_data = g_client_xqc_test_ops.get_conn_alp_user_data_by_stream(stream);
  } else {
    user_data = xqc_get_conn_alp_user_data_by_stream(stream);
  }
  if (call != NULL) {
    call->user_data = user_data;
  }
  return user_data;
#else
  return xqc_get_conn_alp_user_data_by_stream(stream);
#endif
}

static xqc_stream_t *runtime_stream_create_bidi_call(xqc_connection_t *conn) {
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
  odin_xqc_client_runtime_test_call_t *call = runtime_test_append_call(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CREATE_BIDI);
  if (call != NULL) {
    call->conn = conn;
  }
  xqc_stream_t *stream = NULL;
  if (g_client_xqc_test_ops.stream_create_with_direction != NULL) {
    stream = g_client_xqc_test_ops.stream_create_with_direction(
        conn, XQC_STREAM_BIDI, NULL);
  } else {
    stream = xqc_stream_create_with_direction(conn, XQC_STREAM_BIDI, NULL);
  }
  if (call != NULL) {
    call->stream = stream;
  }
  return stream;
#else
  return xqc_stream_create_with_direction(conn, XQC_STREAM_BIDI, NULL);
#endif
}

static xqc_int_t runtime_stream_close_call(xqc_stream_t *stream) {
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
  odin_xqc_client_runtime_test_call_t *call =
      runtime_test_append_call(ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE);
  if (call != NULL) {
    call->stream = stream;
  }
  xqc_int_t rc = XQC_OK;
  if (g_client_xqc_test_ops.stream_close != NULL) {
    rc = g_client_xqc_test_ops.stream_close(stream);
  } else {
    rc = xqc_stream_close(stream);
  }
  if (call != NULL) {
    call->xqc_result = rc;
  }
  return rc;
#else
  return xqc_stream_close(stream);
#endif
}

static void runtime_free_copied_config(odin_xqc_client_runtime_t *rt) {
  free(rt->server_host);
  free(rt->token);
  if (rt->conn_ssl_config.session_ticket_len > 0) {
    free(rt->conn_ssl_config.session_ticket_data);
  }
  if (rt->conn_ssl_config.transport_parameter_data_len > 0) {
    free(rt->conn_ssl_config.transport_parameter_data);
  }
}

static int runtime_validate_peer_addr(const struct sockaddr *addr,
                                      socklen_t addrlen) {
  if (addrlen < (socklen_t)sizeof(struct sockaddr) ||
      addrlen > (socklen_t)sizeof(struct sockaddr_in6)) {
    errno = EINVAL;
    return -1;
  }
  if (addr->sa_family == AF_INET) {
    if (addrlen != (socklen_t)sizeof(struct sockaddr_in)) {
      errno = EINVAL;
      return -1;
    }
    return 0;
  }
  if (addr->sa_family == AF_INET6) {
    if (addrlen != (socklen_t)sizeof(struct sockaddr_in6)) {
      errno = EINVAL;
      return -1;
    }
    return 0;
  }
  errno = EINVAL;
  return -1;
}

static int runtime_copy_config(odin_xqc_client_runtime_t *rt,
                               const odin_xqc_client_runtime_config_t *config) {
  const size_t host_len = strlen(config->server_host);
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
  if (runtime_test_config_copy_alloc_should_fail(
          ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_SERVER_HOST)) {
    return -1;
  }
#endif
  rt->server_host = (char *)malloc(host_len + 1u);
  if (rt->server_host == NULL) {
    errno = ENOMEM;
    return -1;
  }
  memcpy(rt->server_host, config->server_host, host_len + 1u);

  if (config->token_len > 0) {
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
    if (runtime_test_config_copy_alloc_should_fail(
            ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_TOKEN)) {
      return -1;
    }
#endif
    rt->token = (unsigned char *)malloc(config->token_len);
    if (rt->token == NULL) {
      errno = ENOMEM;
      return -1;
    }
    memcpy(rt->token, config->token, config->token_len);
  }
  rt->token_len = config->token_len;

  rt->conn_settings = *config->conn_settings;
  rt->conn_ssl_config = *config->conn_ssl_config;
  rt->conn_ssl_config.session_ticket_data = NULL;
  rt->conn_ssl_config.transport_parameter_data = NULL;
  if (config->conn_ssl_config->session_ticket_len > 0) {
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
    if (runtime_test_config_copy_alloc_should_fail(
            ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_SESSION_TICKET)) {
      return -1;
    }
#endif
    rt->conn_ssl_config.session_ticket_data =
        (char *)malloc(config->conn_ssl_config->session_ticket_len);
    if (rt->conn_ssl_config.session_ticket_data == NULL) {
      errno = ENOMEM;
      return -1;
    }
    memcpy(rt->conn_ssl_config.session_ticket_data,
           config->conn_ssl_config->session_ticket_data,
           config->conn_ssl_config->session_ticket_len);
  } else {
    rt->conn_ssl_config.session_ticket_data = &rt->empty_session_ticket;
  }

  if (config->conn_ssl_config->transport_parameter_data_len > 0) {
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
    if (runtime_test_config_copy_alloc_should_fail(
            ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_TRANSPORT_PARAMETERS)) {
      return -1;
    }
#endif
    rt->conn_ssl_config.transport_parameter_data =
        (char *)malloc(config->conn_ssl_config->transport_parameter_data_len);
    if (rt->conn_ssl_config.transport_parameter_data == NULL) {
      errno = ENOMEM;
      return -1;
    }
    memcpy(rt->conn_ssl_config.transport_parameter_data,
           config->conn_ssl_config->transport_parameter_data,
           config->conn_ssl_config->transport_parameter_data_len);
  } else {
    rt->conn_ssl_config.transport_parameter_data = NULL;
  }

  rt->loop = config->loop;
  memcpy(&rt->peer_addr_storage, config->peer_addr, config->peer_addrlen);
  rt->peer_addrlen = config->peer_addrlen;
  rt->no_crypto_flag = config->no_crypto_flag;
  return 0;
}

static void runtime_finish_destroy(odin_xqc_client_runtime_t *rt) {
  if (rt->finish_destroy_in_progress) {
    return;
  }
  rt->finish_destroy_in_progress = 1;
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
  runtime_test_unregister_live(rt);
#endif
  if (rt->alpn_registered && rt->xu != NULL) {
    (void)runtime_engine_unregister_alpn_call(
        odin_xqc_udp_engine(rt->xu), ODIN_XQC_CLIENT_ALPN,
        sizeof(ODIN_XQC_CLIENT_ALPN) - 1u);
    rt->alpn_registered = 0;
  }
  if (rt->xu != NULL) {
    runtime_udp_destroy_call(rt->xu);
    rt->xu = NULL;
  }
  rt->force_destroy_active = 0;
  runtime_free_copied_config(rt);
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
  (void)runtime_test_append_call(
      ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_RUNTIME_FREE);
  g_client_xqc_test_record.runtime_free_calls += 1;
#endif
  free(rt);
}

static void runtime_pending_fds_destroy_all(odin_xqc_client_runtime_t *rt) {
  while (rt->pending_head != NULL) {
    odin_xqc_client_pending_fd_t *node = rt->pending_head;
    rt->pending_head = node->next;
    (void)close(node->fd);
    free(node);
  }
  rt->pending_tail = NULL;
}

static void
runtime_stream_ctx_unlink_session(odin_xqc_client_stream_ctx_t *stream_ctx) {
  odin_xqc_client_runtime_t *rt = stream_ctx->rt;
  if (stream_ctx->session_prev != NULL) {
    stream_ctx->session_prev->session_next = stream_ctx->session_next;
  } else if (rt->sessions == stream_ctx) {
    rt->sessions = stream_ctx->session_next;
  }
  if (stream_ctx->session_next != NULL) {
    stream_ctx->session_next->session_prev = stream_ctx->session_prev;
  }
  stream_ctx->session_prev = NULL;
  stream_ctx->session_next = NULL;
}

static void
runtime_stream_ctx_unlink_map(odin_xqc_client_stream_ctx_t *stream_ctx) {
  odin_xqc_client_runtime_t *rt = stream_ctx->rt;
  if (stream_ctx->map_prev != NULL) {
    stream_ctx->map_prev->map_next = stream_ctx->map_next;
  } else if (rt->streams_by_transport == stream_ctx) {
    rt->streams_by_transport = stream_ctx->map_next;
  }
  if (stream_ctx->map_next != NULL) {
    stream_ctx->map_next->map_prev = stream_ctx->map_prev;
  }
  stream_ctx->map_prev = NULL;
  stream_ctx->map_next = NULL;
}

static void
runtime_stream_ctx_link_session(odin_xqc_client_runtime_t *rt,
                                odin_xqc_client_stream_ctx_t *stream_ctx) {
  stream_ctx->session_next = rt->sessions;
  if (rt->sessions != NULL) {
    rt->sessions->session_prev = stream_ctx;
  }
  rt->sessions = stream_ctx;
}

static void
runtime_stream_ctx_link_map(odin_xqc_client_runtime_t *rt,
                            odin_xqc_client_stream_ctx_t *stream_ctx) {
  stream_ctx->map_next = rt->streams_by_transport;
  if (rt->streams_by_transport != NULL) {
    rt->streams_by_transport->map_prev = stream_ctx;
  }
  rt->streams_by_transport = stream_ctx;
}

static odin_xqc_client_stream_ctx_t *
runtime_find_stream_by_transport(odin_xqc_client_runtime_t *rt,
                                 void *transport) {
  for (odin_xqc_client_stream_ctx_t *stream_ctx = rt->streams_by_transport;
       stream_ctx != NULL; stream_ctx = stream_ctx->map_next) {
    if (stream_ctx->transport == transport) {
      return stream_ctx;
    }
  }
  return NULL;
}

static void
runtime_destroy_stream_ctx_unlinked(odin_xqc_client_stream_ctx_t *stream_ctx,
                                    int close_stream) {
  xqc_stream_t *stream = stream_ctx->stream;
  odin_client_session_t *cs = stream_ctx->cs;
  stream_ctx->cs = NULL;
  if (stream_ctx->transport != NULL) {
    runtime_stream_ctx_unlink_map(stream_ctx);
    stream_ctx->transport = NULL;
  }
  if (cs != NULL) {
    odin_client_session_destroy(cs);
  }
  if (close_stream && stream != NULL) {
    (void)runtime_stream_close_call(stream);
  }
  free(stream_ctx);
}

static void runtime_destroy_stream_ctx(odin_xqc_client_stream_ctx_t *stream_ctx,
                                       int close_stream) {
  runtime_stream_ctx_unlink_session(stream_ctx);
  runtime_destroy_stream_ctx_unlinked(stream_ctx, close_stream);
}

static void runtime_destroy_all_streams(odin_xqc_client_runtime_t *rt,
                                        int close_streams) {
  while (rt->sessions != NULL) {
    odin_xqc_client_stream_ctx_t *stream_ctx = rt->sessions;
    rt->sessions = stream_ctx->session_next;
    if (rt->sessions != NULL) {
      rt->sessions->session_prev = NULL;
    }
    stream_ctx->session_prev = NULL;
    stream_ctx->session_next = NULL;
    runtime_destroy_stream_ctx_unlinked(stream_ctx, close_streams);
  }
}

static int runtime_maybe_finish_destroy(odin_xqc_client_runtime_t *rt) {
  if (!rt->destroy_pending || rt->conn != NULL ||
      rt->finish_destroy_in_progress) {
    return 0;
  }
  runtime_finish_destroy(rt);
  return 1;
}

static int append_pending_local_fd(odin_xqc_client_runtime_t *rt, int conn_fd) {
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
  if (rt->fail_next_pending_queue_append_armed) {
    const int errnum = rt->fail_next_pending_queue_append_errno;
    rt->fail_next_pending_queue_append_armed = 0;
    rt->fail_next_pending_queue_append_errno = 0;
    errno = errnum;
    return -1;
  }
#endif
  odin_xqc_client_pending_fd_t *node =
      (odin_xqc_client_pending_fd_t *)malloc(sizeof(*node));
  if (node == NULL) {
    errno = ENOMEM;
    return -1;
  }
  node->fd = conn_fd;
  node->next = NULL;
  if (rt->pending_tail != NULL) {
    rt->pending_tail->next = node;
  } else {
    rt->pending_head = node;
  }
  rt->pending_tail = node;
  return 0;
}

#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
int odin_xqc_client_runtime_test_append_pending_fd(
    odin_xqc_client_runtime_t *rt, int conn_fd) {
  if (rt == NULL || conn_fd < 0) {
    errno = EINVAL;
    return -1;
  }
  return append_pending_local_fd(rt, conn_fd);
}
#endif

static int xqc_client_upstream_factory(odin_transport_ready_cb on_ready,
                                       void *ready_user_data,
                                       void *factory_user_data,
                                       odin_transport_t **out) {
  odin_xqc_client_stream_ctx_t *stream_ctx =
      (odin_xqc_client_stream_ctx_t *)factory_user_data;
  odin_xqc_client_runtime_t *rt = stream_ctx->rt;
  if (rt->closing || !rt->handshake_done || rt->conn == NULL) {
    errno = ENOTCONN;
    return -1;
  }
  errno = 0;
  xqc_stream_t *stream = runtime_stream_create_bidi_call(rt->conn);
  if (stream == NULL) {
    if (errno == 0) {
      errno = EIO;
    }
    return -1;
  }
  if (odin_xqc_stream_transport_create(stream, on_ready, ready_user_data,
                                       out) != 0) {
    const int saved = errno;
    (void)runtime_stream_close_call(stream);
    errno = saved;
    return -1;
  }
  stream_ctx->stream = stream;
  stream_ctx->transport = *out;
  runtime_stream_ctx_link_map(rt, stream_ctx);
  return 0;
}

static int create_one_client_session(odin_xqc_client_runtime_t *rt,
                                     int conn_fd) {
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
  if (rt->fail_next_stream_context_alloc_armed) {
    const int errnum = rt->fail_next_stream_context_alloc_errno;
    rt->fail_next_stream_context_alloc_armed = 0;
    rt->fail_next_stream_context_alloc_errno = 0;
    errno = errnum;
    return -1;
  }
#endif
  odin_xqc_client_stream_ctx_t *stream_ctx =
      (odin_xqc_client_stream_ctx_t *)calloc(1, sizeof(*stream_ctx));
  if (stream_ctx == NULL) {
    errno = ENOMEM;
    return -1;
  }
  stream_ctx->rt = rt;
  if (odin_client_session_create_with_upstream_transport(
          rt->loop, conn_fd, xqc_client_upstream_factory, stream_ctx,
          runtime_client_session_upstream_destroying,
          runtime_client_session_on_close, stream_ctx, &stream_ctx->cs) != 0) {
    const int saved = errno;
    free(stream_ctx);
    errno = saved;
    return -1;
  }
  runtime_stream_ctx_link_session(rt, stream_ctx);
  return 0;
}

int odin_xqc_client_runtime_create(
    const odin_xqc_client_runtime_config_t *config,
    odin_xqc_client_runtime_t **out) {
  if (config == NULL || config->loop == NULL || config->local_addr == NULL ||
      config->peer_addr == NULL || config->server_host == NULL ||
      config->engine_ssl_config == NULL || config->engine_callbacks == NULL ||
      config->conn_settings == NULL || config->conn_ssl_config == NULL ||
      out == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (runtime_validate_peer_addr(config->peer_addr, config->peer_addrlen) !=
      0) {
    return -1;
  }
  if (config->token_len > 0 && config->token == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (config->token_len > 256u) {
    errno = EINVAL;
    return -1;
  }
  if (config->conn_ssl_config->session_ticket_len > 0 &&
      config->conn_ssl_config->session_ticket_data == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (config->conn_ssl_config->transport_parameter_data_len > 0 &&
      config->conn_ssl_config->transport_parameter_data == NULL) {
    errno = EINVAL;
    return -1;
  }

  xqc_transport_callbacks_t transport_callbacks;
  if (config->transport_callbacks != NULL) {
    transport_callbacks = *config->transport_callbacks;
  } else {
    memset(&transport_callbacks, 0, sizeof(transport_callbacks));
  }
  if (transport_callbacks.save_token == NULL) {
    transport_callbacks.save_token = runtime_noop_save_token;
  }
  if (transport_callbacks.save_session_cb == NULL) {
    transport_callbacks.save_session_cb = runtime_noop_save_string;
  }
  if (transport_callbacks.save_tp_cb == NULL) {
    transport_callbacks.save_tp_cb = runtime_noop_save_string;
  }
  if ((config->conn_ssl_config->cert_verify_flag &
       XQC_TLS_CERT_FLAG_NEED_VERIFY) != 0) {
    if (config->transport_callbacks == NULL ||
        transport_callbacks.cert_verify_cb == NULL) {
      errno = EINVAL;
      return -1;
    }
  }
  transport_callbacks.conn_update_cid_notify = runtime_conn_update_cid;

  odin_xqc_client_runtime_t *rt =
      (odin_xqc_client_runtime_t *)calloc(1, sizeof(*rt));
  if (rt == NULL) {
    errno = ENOMEM;
    return -1;
  }
  rt->transport_callbacks = transport_callbacks;
  if (runtime_copy_config(rt, config) != 0) {
    const int saved = errno;
    runtime_free_copied_config(rt);
    free(rt);
    errno = saved;
    return -1;
  }

  rt->app_callbacks.conn_cbs.conn_create_notify = runtime_conn_create_notify;
  rt->app_callbacks.conn_cbs.conn_close_notify = runtime_conn_close_notify;
  rt->app_callbacks.conn_cbs.conn_handshake_finished =
      runtime_conn_handshake_finished;
  rt->app_callbacks.stream_cbs.stream_read_notify = runtime_stream_read_notify;
  rt->app_callbacks.stream_cbs.stream_write_notify =
      runtime_stream_write_notify;
  rt->app_callbacks.stream_cbs.stream_close_notify =
      runtime_stream_close_notify;
  rt->app_callbacks.stream_cbs.stream_closing_notify =
      runtime_stream_closing_notify;

  odin_xqc_udp_config_t udp_config;
  memset(&udp_config, 0, sizeof(udp_config));
  udp_config.loop = config->loop;
  udp_config.local_addr = config->local_addr;
  udp_config.local_addrlen = config->local_addrlen;
  udp_config.engine_type = XQC_ENGINE_CLIENT;
  udp_config.engine_config = config->engine_config;
  udp_config.ssl_config = config->engine_ssl_config;
  udp_config.engine_callbacks = config->engine_callbacks;
  udp_config.transport_callbacks = &rt->transport_callbacks;
  udp_config.app_user_data = rt;
  if (runtime_udp_create_call(&udp_config, &rt->xu) != 0) {
    const int saved = errno;
    runtime_free_copied_config(rt);
    free(rt);
    errno = saved;
    return -1;
  }
  if (runtime_engine_register_alpn_call(odin_xqc_udp_engine(rt->xu),
                                        ODIN_XQC_CLIENT_ALPN,
                                        sizeof(ODIN_XQC_CLIENT_ALPN) - 1u,
                                        &rt->app_callbacks, rt) != XQC_OK) {
    runtime_udp_destroy_call(rt->xu);
    runtime_free_copied_config(rt);
    free(rt);
    errno = EIO;
    return -1;
  }
  rt->alpn_registered = 1;
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
  runtime_test_register_live(rt);
#endif
  *out = rt;
  return 0;
}

int odin_xqc_client_runtime_create_default(
    const odin_xqc_client_runtime_default_config_t *config,
    odin_xqc_client_runtime_t **out) {
  if (config == NULL || out == NULL || config->loop == NULL ||
      config->local_addr == NULL || config->peer_addr == NULL ||
      config->server_host == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (runtime_validate_peer_addr(config->local_addr, config->local_addrlen) !=
      0) {
    return -1;
  }
  if (runtime_validate_peer_addr(config->peer_addr, config->peer_addrlen) !=
      0) {
    return -1;
  }

  xqc_engine_ssl_config_t engine_ssl_config;
  memset(&engine_ssl_config, 0, sizeof(engine_ssl_config));
  xqc_engine_callback_t engine_callbacks;
  memset(&engine_callbacks, 0, sizeof(engine_callbacks));
  xqc_conn_settings_t conn_settings;
  memset(&conn_settings, 0, sizeof(conn_settings));
  xqc_conn_ssl_config_t conn_ssl_config;
  memset(&conn_ssl_config, 0, sizeof(conn_ssl_config));

  odin_xqc_client_runtime_config_t full_config;
  memset(&full_config, 0, sizeof(full_config));
  full_config.loop = config->loop;
  full_config.local_addr = config->local_addr;
  full_config.local_addrlen = config->local_addrlen;
  full_config.peer_addr = config->peer_addr;
  full_config.peer_addrlen = config->peer_addrlen;
  full_config.server_host = config->server_host;
  full_config.engine_config = NULL;
  full_config.engine_ssl_config = &engine_ssl_config;
  full_config.engine_callbacks = &engine_callbacks;
  full_config.transport_callbacks = NULL;
  full_config.conn_settings = &conn_settings;
  full_config.conn_ssl_config = &conn_ssl_config;
  full_config.token = NULL;
  full_config.token_len = 0;
  full_config.no_crypto_flag = 0;

#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
  g_client_xqc_test_record.default_create_calls += 1;
  memset(&g_client_xqc_test_record.last_default_create, 0,
         sizeof(g_client_xqc_test_record.last_default_create));
  g_client_xqc_test_record.last_default_create.engine_config =
      full_config.engine_config;
  g_client_xqc_test_record.last_default_create.engine_ssl_config =
      full_config.engine_ssl_config;
  g_client_xqc_test_record.last_default_create.engine_ssl_config_value =
      engine_ssl_config;
  g_client_xqc_test_record.last_default_create.engine_callbacks =
      full_config.engine_callbacks;
  g_client_xqc_test_record.last_default_create.engine_callbacks_value =
      engine_callbacks;
  g_client_xqc_test_record.last_default_create.transport_callbacks =
      full_config.transport_callbacks;
  g_client_xqc_test_record.last_default_create.conn_settings =
      full_config.conn_settings;
  g_client_xqc_test_record.last_default_create.conn_settings_value =
      conn_settings;
  g_client_xqc_test_record.last_default_create.conn_ssl_config =
      full_config.conn_ssl_config;
  g_client_xqc_test_record.last_default_create.conn_ssl_config_value =
      conn_ssl_config;
  g_client_xqc_test_record.last_default_create.token = full_config.token;
  g_client_xqc_test_record.last_default_create.token_len =
      full_config.token_len;
  g_client_xqc_test_record.last_default_create.no_crypto_flag =
      full_config.no_crypto_flag;
#endif
  return odin_xqc_client_runtime_create(&full_config, out);
}

static void cleanup_failed_start_connection(odin_xqc_client_runtime_t *rt) {
  if (rt->conn != NULL && rt->cid_registered) {
    (void)runtime_conn_close_call(odin_xqc_udp_engine(rt->xu),
                                  &rt->current_cid);
  }
  if (rt->cid_registered) {
    runtime_udp_unregister_conn_call(rt->xu, &rt->current_cid);
    rt->cid_registered = 0;
  }
  rt->conn = NULL;
  rt->handshake_done = 0;
}

int odin_xqc_client_runtime_start(odin_xqc_client_runtime_t *rt) {
  if (rt == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (rt->connect_started && rt->udp_running) {
    return 0;
  }
  if (rt->connect_started && !rt->udp_running) {
    if (runtime_udp_start_call(rt->xu) != 0) {
      rt->udp_running = 0;
      return -1;
    }
    rt->udp_running = 1;
    return 0;
  }
  if (runtime_udp_start_call(rt->xu) != 0) {
    rt->udp_running = 0;
    rt->connect_started = 0;
    return -1;
  }
  rt->udp_running = 1;
  rt->connect_errno = 0;
  rt->startup_connecting = 1;
  const xqc_cid_t *cid = runtime_xqc_connect_call(
      odin_xqc_udp_engine(rt->xu), &rt->conn_settings, rt->token, rt->token_len,
      rt->server_host, rt->no_crypto_flag, &rt->conn_ssl_config,
      (const struct sockaddr *)&rt->peer_addr_storage, rt->peer_addrlen,
      ODIN_XQC_CLIENT_ALPN, odin_xqc_udp_xqc_user_data(rt->xu));
  if (cid == NULL) {
    const int saved = rt->connect_errno != 0 ? rt->connect_errno : EIO;
    if (rt->conn != NULL || rt->cid_registered) {
      cleanup_failed_start_connection(rt);
    }
    rt->startup_connecting = 0;
    (void)runtime_udp_stop_call(rt->xu);
    rt->udp_running = 0;
    rt->connect_started = 0;
    errno = saved;
    return -1;
  }
  (void)cid;
  rt->startup_connecting = 0;
  rt->connect_started = 1;
  return 0;
}

int odin_xqc_client_runtime_stop(odin_xqc_client_runtime_t *rt) {
  if (rt == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (!rt->udp_running) {
    return 0;
  }
  if (runtime_udp_stop_call(rt->xu) != 0) {
    return -1;
  }
  rt->udp_running = 0;
  return 0;
}

int odin_xqc_client_runtime_add_connection(odin_xqc_client_runtime_t *rt,
                                           int conn_fd) {
  if (rt == NULL || rt->closing || !rt->connect_started || !rt->udp_running) {
    errno = ENOTCONN;
    return -1;
  }
  if (!rt->handshake_done) {
    return append_pending_local_fd(rt, conn_fd);
  }
  return create_one_client_session(rt, conn_fd);
}

static void force_destroy_stopped_connection(odin_xqc_client_runtime_t *rt) {
  if (rt->conn != NULL) {
    runtime_conn_set_alp_user_data_call(rt->conn, NULL);
  }
  if (rt->cid_registered) {
    runtime_udp_unregister_conn_call(rt->xu, &rt->current_cid);
    rt->cid_registered = 0;
  }
  rt->conn = NULL;
  rt->handshake_done = 0;
  rt->connect_started = 0;
}

void odin_xqc_client_runtime_destroy(odin_xqc_client_runtime_t *rt) {
  if (rt == NULL) {
    return;
  }
  rt->destroy_pending = 1;
  rt->closing = 1;
  if (rt->conn != NULL && !rt->udp_running) {
    rt->force_destroy_active = 1;
    runtime_destroy_all_streams(rt, 1);
    runtime_pending_fds_destroy_all(rt);
    force_destroy_stopped_connection(rt);
    runtime_finish_destroy(rt);
    return;
  }
  runtime_destroy_all_streams(rt, 1);
  runtime_pending_fds_destroy_all(rt);
  if (rt->conn != NULL) {
    if (!rt->destroy_close_requested && rt->cid_registered) {
      rt->destroy_close_requested = 1;
      (void)runtime_conn_close_call(odin_xqc_udp_engine(rt->xu),
                                    &rt->current_cid);
    }
    return;
  }
  (void)runtime_maybe_finish_destroy(rt);
}

void odin_xqc_client_runtime_force_destroy(odin_xqc_client_runtime_t *rt) {
  if (rt == NULL) {
#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)
    g_client_xqc_test_record.force_destroy_null_calls += 1;
#endif
    return;
  }
  rt->force_destroy_active = 1;
  rt->destroy_pending = 1;
  rt->closing = 1;
  runtime_pending_fds_destroy_all(rt);
  runtime_destroy_all_streams(rt, 1);
  if (rt->conn != NULL) {
    runtime_conn_set_alp_user_data_call(rt->conn, NULL);
  }
  if (rt->cid_registered) {
    runtime_udp_unregister_conn_call(rt->xu, &rt->current_cid);
    rt->cid_registered = 0;
  }
  rt->conn = NULL;
  rt->connect_started = 0;
  rt->handshake_done = 0;
  runtime_finish_destroy(rt);
}

static int runtime_conn_create_notify(xqc_connection_t *conn,
                                      const xqc_cid_t *cid,
                                      void *conn_user_data,
                                      void *conn_proto_data) {
  (void)conn_proto_data;
  odin_xqc_udp_t *xu = (odin_xqc_udp_t *)conn_user_data;
  odin_xqc_client_runtime_t *rt =
      xu != NULL ? (odin_xqc_client_runtime_t *)odin_xqc_udp_app_user_data(xu)
                 : NULL;
  if (rt == NULL) {
    return -1;
  }
  if (rt->force_destroy_active) {
    return 0;
  }
  if (rt->conn != NULL || rt->closing) {
    rt->connect_errno = EALREADY;
    return -1;
  }
  if (runtime_udp_register_conn_call(rt->xu, cid) != 0) {
    rt->connect_errno = errno;
    return -1;
  }
  rt->conn = conn;
  rt->current_cid = *cid;
  rt->cid_registered = 1;
  runtime_conn_set_alp_user_data_call(conn, rt);
  return 0;
}

static int runtime_conn_close_notify(xqc_connection_t *conn,
                                     const xqc_cid_t *cid, void *conn_user_data,
                                     void *conn_proto_data) {
  (void)cid;
  odin_xqc_udp_t *xu = (odin_xqc_udp_t *)conn_user_data;
  odin_xqc_client_runtime_t *rt =
      xu != NULL ? (odin_xqc_client_runtime_t *)odin_xqc_udp_app_user_data(xu)
                 : NULL;
  if (rt == NULL) {
    rt = (odin_xqc_client_runtime_t *)conn_proto_data;
  }
  if (rt == NULL || rt->conn != conn) {
    return 0;
  }
  if (rt->force_destroy_active) {
    return 0;
  }
  if (rt->startup_connecting && !rt->connect_started && !rt->destroy_pending) {
    if (rt->cid_registered) {
      runtime_udp_unregister_conn_call(rt->xu, &rt->current_cid);
      rt->cid_registered = 0;
    }
    rt->conn = NULL;
    rt->handshake_done = 0;
    return 0;
  }
  runtime_destroy_all_streams(rt, 0);
  runtime_pending_fds_destroy_all(rt);
  if (rt->cid_registered) {
    runtime_udp_unregister_conn_call(rt->xu, &rt->current_cid);
    rt->cid_registered = 0;
  }
  rt->conn = NULL;
  rt->connect_started = 0;
  rt->handshake_done = 0;
  rt->closing = 1;
  (void)runtime_maybe_finish_destroy(rt);
  return 0;
}

static void runtime_conn_handshake_finished(xqc_connection_t *conn,
                                            void *conn_user_data,
                                            void *conn_proto_data) {
  odin_xqc_udp_t *xu = (odin_xqc_udp_t *)conn_user_data;
  odin_xqc_client_runtime_t *rt =
      xu != NULL ? (odin_xqc_client_runtime_t *)odin_xqc_udp_app_user_data(xu)
                 : NULL;
  if (rt == NULL || conn_proto_data != rt || rt->conn != conn || rt->closing ||
      rt->force_destroy_active) {
    return;
  }
  rt->handshake_done = 1;
  while (rt->pending_head != NULL) {
    odin_xqc_client_pending_fd_t *node = rt->pending_head;
    rt->pending_head = node->next;
    if (rt->pending_head == NULL) {
      rt->pending_tail = NULL;
    }
    const int fd = node->fd;
    free(node);
    if (create_one_client_session(rt, fd) != 0) {
      (void)close(fd);
    }
  }
}

static void runtime_conn_update_cid(xqc_connection_t *conn,
                                    const xqc_cid_t *retire_cid,
                                    const xqc_cid_t *new_cid,
                                    void *conn_user_data) {
  odin_xqc_udp_t *xu = (odin_xqc_udp_t *)conn_user_data;
  odin_xqc_client_runtime_t *rt =
      xu != NULL ? (odin_xqc_client_runtime_t *)odin_xqc_udp_app_user_data(xu)
                 : NULL;
  if (rt == NULL || rt->conn != conn || rt->closing ||
      rt->force_destroy_active) {
    return;
  }
  if (runtime_udp_register_conn_call(rt->xu, new_cid) != 0) {
    rt->closing = 1;
    runtime_destroy_all_streams(rt, 1);
    runtime_pending_fds_destroy_all(rt);
    if (rt->cid_registered) {
      (void)runtime_conn_close_call(odin_xqc_udp_engine(rt->xu),
                                    &rt->current_cid);
      runtime_udp_unregister_conn_call(rt->xu, &rt->current_cid);
      rt->cid_registered = 0;
    }
    return;
  }
  runtime_udp_unregister_conn_call(rt->xu, retire_cid);
  rt->current_cid = *new_cid;
  rt->cid_registered = 1;
}

static xqc_int_t runtime_stream_read_notify(xqc_stream_t *stream,
                                            void *strm_user_data) {
  odin_xqc_client_runtime_t *rt = (odin_xqc_client_runtime_t *)
      runtime_get_conn_alp_user_data_by_stream_call(stream);
  if (rt == NULL || strm_user_data == NULL || rt->force_destroy_active) {
    return XQC_OK;
  }
  if (runtime_find_stream_by_transport(rt, strm_user_data) == NULL) {
    return XQC_OK;
  }
  return odin_xqc_stream_transport_read_notify(stream, strm_user_data);
}

static xqc_int_t runtime_stream_write_notify(xqc_stream_t *stream,
                                             void *strm_user_data) {
  odin_xqc_client_runtime_t *rt = (odin_xqc_client_runtime_t *)
      runtime_get_conn_alp_user_data_by_stream_call(stream);
  if (rt == NULL || strm_user_data == NULL || rt->force_destroy_active) {
    return XQC_OK;
  }
  if (runtime_find_stream_by_transport(rt, strm_user_data) == NULL) {
    return XQC_OK;
  }
  return odin_xqc_stream_transport_write_notify(stream, strm_user_data);
}

static xqc_int_t runtime_stream_close_notify(xqc_stream_t *stream,
                                             void *strm_user_data) {
  odin_xqc_client_runtime_t *rt = (odin_xqc_client_runtime_t *)
      runtime_get_conn_alp_user_data_by_stream_call(stream);
  if (rt == NULL || strm_user_data == NULL || rt->force_destroy_active) {
    return XQC_OK;
  }
  odin_xqc_client_stream_ctx_t *stream_ctx =
      runtime_find_stream_by_transport(rt, strm_user_data);
  if (stream_ctx != NULL) {
    runtime_destroy_stream_ctx(stream_ctx, 0);
  }
  return XQC_OK;
}

static void runtime_stream_closing_notify(xqc_stream_t *stream,
                                          xqc_int_t err_code,
                                          void *strm_user_data) {
  odin_xqc_client_runtime_t *rt = (odin_xqc_client_runtime_t *)
      runtime_get_conn_alp_user_data_by_stream_call(stream);
  if (rt == NULL || strm_user_data == NULL || rt->force_destroy_active) {
    return;
  }
  if (runtime_find_stream_by_transport(rt, strm_user_data) == NULL) {
    return;
  }
  odin_xqc_stream_transport_closing_notify(stream, err_code, strm_user_data);
}

static void
runtime_client_session_upstream_destroying(odin_transport_t *transport,
                                           void *factory_user_data) {
  odin_xqc_client_stream_ctx_t *stream_ctx =
      (odin_xqc_client_stream_ctx_t *)factory_user_data;
  if (stream_ctx->transport == transport) {
    runtime_stream_ctx_unlink_map(stream_ctx);
    stream_ctx->transport = NULL;
  }
}

static void runtime_client_session_on_close(odin_client_session_t *cs, int err,
                                            void *user_data) {
  odin_xqc_client_stream_ctx_t *stream_ctx =
      (odin_xqc_client_stream_ctx_t *)user_data;
  xqc_stream_t *stream = stream_ctx->stream;
  if (stream_ctx->transport != NULL) {
    runtime_stream_ctx_unlink_map(stream_ctx);
    stream_ctx->transport = NULL;
  }
  runtime_stream_ctx_unlink_session(stream_ctx);
  stream_ctx->cs = NULL;
  odin_client_session_destroy(cs);
  if (err != 0 && stream != NULL) {
    (void)runtime_stream_close_call(stream);
  }
  free(stream_ctx);
}
