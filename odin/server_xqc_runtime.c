/* odin/server_xqc_runtime.c -- RFC-025 server-side xquic runtime. */

#include "odin/server_xqc_runtime.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "odin/transport.h"
#include "odin/transport_xqc.h"

#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
#include "odin/testing/server_xqc_runtime_internal_test.h"
#endif

typedef struct odin_xqc_server_conn_ctx_t odin_xqc_server_conn_ctx_t;
typedef struct odin_xqc_server_stream_ctx_t odin_xqc_server_stream_ctx_t;

struct odin_xqc_server_stream_ctx_t {
  odin_xqc_server_conn_ctx_t *conn_ctx;
  odin_xqc_server_stream_ctx_t *conn_prev;
  odin_xqc_server_stream_ctx_t *conn_next;
  odin_xqc_server_stream_ctx_t *rt_prev;
  odin_xqc_server_stream_ctx_t *rt_next;
  odin_xqc_server_stream_ctx_t *force_next;
  xqc_stream_t *stream;
  odin_transport_t *transport;
  odin_server_session_t *ss;
};

struct odin_xqc_server_conn_ctx_t {
  odin_xqc_server_runtime_t *rt;
  odin_xqc_server_conn_ctx_t *prev;
  odin_xqc_server_conn_ctx_t *next;
  odin_xqc_server_conn_ctx_t *force_next;
  odin_xqc_server_stream_ctx_t *streams;
  xqc_connection_t *conn;
  xqc_cid_t current_cid;
  int cid_registered;
  int closing;
  int destroy_snapshot_valid;
  int pre_destroy_closing;
  int destroy_close_requested;
};

struct odin_xqc_server_runtime_t {
  odin_event_loop_t *loop;
  odin_xqc_udp_t *xu;
  xqc_transport_callbacks_t transport_callbacks;
  xqc_app_proto_callbacks_t app_callbacks;
  odin_xqc_server_conn_ctx_t *connections;
  odin_xqc_server_stream_ctx_t *streams_by_transport;
  odin_server_session_dial_filter_cb dial_filter;
  void *dial_filter_ud;
  unsigned int active_entries;
  int destroy_pending;
  int drain_active;
  int alpn_registered;
  int force_destroy_active;
  int finish_destroy_in_progress;
  odin_xqc_server_conn_ctx_t *force_conns;
  odin_xqc_server_stream_ctx_t *force_streams;
};

static int runtime_server_accept(xqc_engine_t *engine, xqc_connection_t *conn,
                                 const xqc_cid_t *cid, void *user_data);
static void runtime_server_refuse(xqc_engine_t *engine, xqc_connection_t *conn,
                                  const xqc_cid_t *cid, void *user_data);
static void runtime_conn_update_cid(xqc_connection_t *conn,
                                    const xqc_cid_t *retire_cid,
                                    const xqc_cid_t *new_cid,
                                    void *conn_user_data);
static int runtime_conn_create_notify(xqc_connection_t *conn,
                                      const xqc_cid_t *cid,
                                      void *conn_user_data,
                                      void *conn_proto_data);
static int runtime_conn_close_notify(xqc_connection_t *conn,
                                     const xqc_cid_t *cid, void *conn_user_data,
                                     void *conn_proto_data);
static xqc_int_t runtime_stream_create_notify(xqc_stream_t *stream,
                                              void *strm_user_data);
static xqc_int_t runtime_stream_read_notify(xqc_stream_t *stream,
                                            void *strm_user_data);
static xqc_int_t runtime_stream_write_notify(xqc_stream_t *stream,
                                             void *strm_user_data);
static xqc_int_t runtime_stream_close_notify(xqc_stream_t *stream,
                                             void *strm_user_data);
static void runtime_stream_closing_notify(xqc_stream_t *stream,
                                          xqc_int_t err_code,
                                          void *strm_user_data);
static void runtime_stream_session_on_close(odin_server_session_t *ss, int err,
                                            void *user_data);

#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
static odin_xqc_server_runtime_test_record_t g_server_xqc_test_record;
static odin_xqc_server_runtime_test_ops_t g_server_xqc_test_ops;
static int g_fail_next_conn_context_alloc_armed;
static int g_fail_next_conn_context_alloc_errno;
static odin_xqc_server_runtime_t *g_fail_next_conn_context_alloc_rt;
static int g_fail_next_stream_context_alloc_armed;
static int g_fail_next_stream_context_alloc_errno;
static odin_xqc_server_runtime_t *g_fail_next_stream_context_alloc_rt;

static odin_xqc_server_runtime_test_call_t *
runtime_test_append_call(odin_xqc_server_runtime_test_call_kind_t kind) {
  if (g_server_xqc_test_record.call_count >=
      ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CAP) {
    g_server_xqc_test_record.dropped_call_count += 1;
    return NULL;
  }
  odin_xqc_server_runtime_test_call_t *call =
      &g_server_xqc_test_record.calls[g_server_xqc_test_record.call_count++];
  memset(call, 0, sizeof(*call));
  call->kind = kind;
  return call;
}

void odin_xqc_server_runtime_test_reset(void) {
  memset(&g_server_xqc_test_record, 0, sizeof(g_server_xqc_test_record));
  memset(&g_server_xqc_test_ops, 0, sizeof(g_server_xqc_test_ops));
  g_fail_next_conn_context_alloc_armed = 0;
  g_fail_next_conn_context_alloc_errno = 0;
  g_fail_next_conn_context_alloc_rt = NULL;
  g_fail_next_stream_context_alloc_armed = 0;
  g_fail_next_stream_context_alloc_errno = 0;
  g_fail_next_stream_context_alloc_rt = NULL;
}

void odin_xqc_server_runtime_test_set_ops(
    const odin_xqc_server_runtime_test_ops_t *ops) {
  if (ops == NULL) {
    memset(&g_server_xqc_test_ops, 0, sizeof(g_server_xqc_test_ops));
    return;
  }
  g_server_xqc_test_ops = *ops;
}

const odin_xqc_server_runtime_test_record_t *
odin_xqc_server_runtime_test_record(void) {
  return &g_server_xqc_test_record;
}

int odin_xqc_server_runtime_test_fail_next_conn_context_alloc(
    odin_xqc_server_runtime_t *rt, int errnum) {
  g_fail_next_conn_context_alloc_armed = 1;
  g_fail_next_conn_context_alloc_errno = errnum;
  g_fail_next_conn_context_alloc_rt = rt;
  return 0;
}

int odin_xqc_server_runtime_test_fail_next_stream_context_alloc(
    odin_xqc_server_runtime_t *rt, int errnum) {
  g_fail_next_stream_context_alloc_armed = 1;
  g_fail_next_stream_context_alloc_errno = errnum;
  g_fail_next_stream_context_alloc_rt = rt;
  return 0;
}
#endif

static int runtime_udp_create_call(const odin_xqc_udp_config_t *config,
                                   odin_xqc_udp_t **out) {
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  odin_xqc_server_runtime_test_call_t *call =
      runtime_test_append_call(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_CREATE);
  if (call != NULL) {
    call->user_data = config != NULL ? config->app_user_data : NULL;
  }
  g_server_xqc_test_record.udp_create_calls += 1;
  memset(&g_server_xqc_test_record.last_udp_create, 0,
         sizeof(g_server_xqc_test_record.last_udp_create));
  if (config != NULL) {
    g_server_xqc_test_record.last_udp_create.loop = config->loop;
    g_server_xqc_test_record.last_udp_create.local_addr = config->local_addr;
    g_server_xqc_test_record.last_udp_create.local_addrlen =
        config->local_addrlen;
    g_server_xqc_test_record.last_udp_create.engine_type = config->engine_type;
    g_server_xqc_test_record.last_udp_create.engine_config =
        config->engine_config;
    g_server_xqc_test_record.last_udp_create.ssl_config = config->ssl_config;
    g_server_xqc_test_record.last_udp_create.engine_callbacks =
        config->engine_callbacks;
    if (config->engine_callbacks != NULL) {
      g_server_xqc_test_record.last_udp_create.engine_callbacks_value =
          *config->engine_callbacks;
    }
    g_server_xqc_test_record.last_udp_create.transport_callbacks =
        config->transport_callbacks;
    if (config->transport_callbacks != NULL) {
      g_server_xqc_test_record.last_udp_create.transport_callbacks_value =
          *config->transport_callbacks;
    }
    g_server_xqc_test_record.last_udp_create.app_user_data =
        config->app_user_data;
  }
#endif
  const int rc = odin_xqc_udp_create(config, out);
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  if (call != NULL) {
    call->int_result = rc;
    call->xu = out != NULL && rc == 0 ? *out : NULL;
  }
#endif
  return rc;
}

static int runtime_udp_start_call(odin_xqc_udp_t *xu) {
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  odin_xqc_server_runtime_test_call_t *call =
      runtime_test_append_call(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_START);
  if (call != NULL) {
    call->xu = xu;
  }
#endif
  const int rc = odin_xqc_udp_start(xu);
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  if (call != NULL) {
    call->int_result = rc;
  }
#endif
  return rc;
}

static int runtime_udp_stop_call(odin_xqc_udp_t *xu) {
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  odin_xqc_server_runtime_test_call_t *call =
      runtime_test_append_call(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_STOP);
  if (call != NULL) {
    call->xu = xu;
  }
#endif
  const int rc = odin_xqc_udp_stop(xu);
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  if (call != NULL) {
    call->int_result = rc;
  }
#endif
  return rc;
}

static void runtime_udp_destroy_call(odin_xqc_udp_t *xu) {
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  odin_xqc_server_runtime_test_call_t *call =
      runtime_test_append_call(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_DESTROY);
  if (call != NULL) {
    call->xu = xu;
  }
#endif
  odin_xqc_udp_destroy(xu);
}

static int runtime_udp_register_conn_call(odin_xqc_udp_t *xu,
                                          const xqc_cid_t *cid) {
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  odin_xqc_server_runtime_test_call_t *call = runtime_test_append_call(
      ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_REGISTER_CONN);
  if (call != NULL) {
    call->xu = xu;
    if (cid != NULL) {
      call->cid = *cid;
    }
  }
  int rc = 0;
  if (g_server_xqc_test_ops.udp_register_conn != NULL) {
    rc = g_server_xqc_test_ops.udp_register_conn(xu, cid);
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
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  odin_xqc_server_runtime_test_call_t *call = runtime_test_append_call(
      ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN);
  if (call != NULL) {
    call->xu = xu;
    if (cid != NULL) {
      call->cid = *cid;
    }
  }
  if (g_server_xqc_test_ops.udp_unregister_conn != NULL) {
    g_server_xqc_test_ops.udp_unregister_conn(xu, cid);
    return;
  }
#endif
  odin_xqc_udp_unregister_conn(xu, cid);
}

static xqc_int_t runtime_engine_register_alpn_call(
    xqc_engine_t *engine, const char *alpn, size_t alpn_len,
    xqc_app_proto_callbacks_t *app_callbacks, void *user_data) {
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  odin_xqc_server_runtime_test_call_t *call = runtime_test_append_call(
      ODIN_XQC_SERVER_RUNTIME_TEST_CALL_ENGINE_REGISTER_ALPN);
  if (call != NULL) {
    call->engine = engine;
    call->alpn = alpn;
    call->alpn_len = alpn_len;
    call->app_callbacks = app_callbacks;
    call->user_data = user_data;
  }
  xqc_int_t rc = XQC_OK;
  if (g_server_xqc_test_ops.engine_register_alpn != NULL) {
    rc = g_server_xqc_test_ops.engine_register_alpn(engine, alpn, alpn_len,
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
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  odin_xqc_server_runtime_test_call_t *call = runtime_test_append_call(
      ODIN_XQC_SERVER_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN);
  if (call != NULL) {
    call->engine = engine;
    call->alpn = alpn;
    call->alpn_len = alpn_len;
  }
  xqc_int_t rc = XQC_OK;
  if (g_server_xqc_test_ops.engine_unregister_alpn != NULL) {
    rc = g_server_xqc_test_ops.engine_unregister_alpn(engine, alpn, alpn_len);
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

static void runtime_conn_set_transport_user_data_call(xqc_connection_t *conn,
                                                      void *user_data) {
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  odin_xqc_server_runtime_test_call_t *call = runtime_test_append_call(
      ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_SET_TRANSPORT_USER_DATA);
  if (call != NULL) {
    call->conn = conn;
    call->user_data = user_data;
  }
  if (g_server_xqc_test_ops.conn_set_transport_user_data != NULL) {
    g_server_xqc_test_ops.conn_set_transport_user_data(conn, user_data);
    return;
  }
#endif
  xqc_conn_set_transport_user_data(conn, user_data);
}

static void runtime_conn_set_alp_user_data_call(xqc_connection_t *conn,
                                                void *user_data) {
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  odin_xqc_server_runtime_test_call_t *call = runtime_test_append_call(
      ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_SET_ALP_USER_DATA);
  if (call != NULL) {
    call->conn = conn;
    call->user_data = user_data;
  }
  if (g_server_xqc_test_ops.conn_set_alp_user_data != NULL) {
    g_server_xqc_test_ops.conn_set_alp_user_data(conn, user_data);
    return;
  }
#endif
  xqc_conn_set_alp_user_data(conn, user_data);
}

static xqc_int_t runtime_conn_close_call(xqc_engine_t *engine,
                                         const xqc_cid_t *cid) {
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  odin_xqc_server_runtime_test_call_t *call =
      runtime_test_append_call(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_CLOSE);
  if (call != NULL) {
    call->engine = engine;
    if (cid != NULL) {
      call->cid = *cid;
    }
  }
  xqc_int_t rc = XQC_OK;
  if (g_server_xqc_test_ops.conn_close != NULL) {
    rc = g_server_xqc_test_ops.conn_close(engine, cid);
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

static xqc_stream_direction_t
runtime_stream_get_direction_call(xqc_stream_t *stream) {
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  odin_xqc_server_runtime_test_call_t *call = runtime_test_append_call(
      ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_GET_DIRECTION);
  if (call != NULL) {
    call->stream = stream;
  }
  xqc_stream_direction_t direction = XQC_STREAM_UNI;
  if (g_server_xqc_test_ops.stream_get_direction != NULL) {
    direction = g_server_xqc_test_ops.stream_get_direction(stream);
  } else {
    direction = xqc_stream_get_direction(stream);
  }
  if (call != NULL) {
    call->direction = direction;
  }
  return direction;
#else
  return xqc_stream_get_direction(stream);
#endif
}

static void *
runtime_get_conn_alp_user_data_by_stream_call(xqc_stream_t *stream) {
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  odin_xqc_server_runtime_test_call_t *call = runtime_test_append_call(
      ODIN_XQC_SERVER_RUNTIME_TEST_CALL_GET_CONN_ALP_USER_DATA_BY_STREAM);
  if (call != NULL) {
    call->stream = stream;
  }
  void *user_data = NULL;
  if (g_server_xqc_test_ops.get_conn_alp_user_data_by_stream != NULL) {
    user_data = g_server_xqc_test_ops.get_conn_alp_user_data_by_stream(stream);
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

static xqc_int_t runtime_stream_close_call(xqc_stream_t *stream) {
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  odin_xqc_server_runtime_test_call_t *call =
      runtime_test_append_call(ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_CLOSE);
  if (call != NULL) {
    call->stream = stream;
  }
  xqc_int_t rc = XQC_OK;
  if (g_server_xqc_test_ops.stream_close != NULL) {
    rc = g_server_xqc_test_ops.stream_close(stream);
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

static void runtime_callback_enter(odin_xqc_server_runtime_t *rt) {
  rt->active_entries += 1;
}

static void runtime_free_force_pending(odin_xqc_server_runtime_t *rt) {
  while (rt->force_streams != NULL) {
    odin_xqc_server_stream_ctx_t *stream_ctx = rt->force_streams;
    rt->force_streams = stream_ctx->force_next;
    free(stream_ctx);
  }
  while (rt->force_conns != NULL) {
    odin_xqc_server_conn_ctx_t *ctx = rt->force_conns;
    rt->force_conns = ctx->force_next;
    free(ctx);
  }
}

static void runtime_finish_destroy(odin_xqc_server_runtime_t *rt) {
  if (rt->finish_destroy_in_progress) {
    return;
  }
  rt->finish_destroy_in_progress = 1;
  if (rt->alpn_registered && rt->xu != NULL) {
    (void)runtime_engine_unregister_alpn_call(
        odin_xqc_udp_engine(rt->xu), ODIN_XQC_SERVER_ALPN,
        sizeof(ODIN_XQC_SERVER_ALPN) - 1u);
    rt->alpn_registered = 0;
  }
  if (rt->xu != NULL) {
    runtime_udp_destroy_call(rt->xu);
    rt->xu = NULL;
  }
  runtime_free_force_pending(rt);
  free(rt);
}

static void
runtime_stream_ctx_unlink(odin_xqc_server_stream_ctx_t *stream_ctx) {
  odin_xqc_server_conn_ctx_t *conn_ctx = stream_ctx->conn_ctx;
  odin_xqc_server_runtime_t *rt = conn_ctx->rt;
  if (stream_ctx->conn_prev != NULL) {
    stream_ctx->conn_prev->conn_next = stream_ctx->conn_next;
  } else if (conn_ctx->streams == stream_ctx) {
    conn_ctx->streams = stream_ctx->conn_next;
  }
  if (stream_ctx->conn_next != NULL) {
    stream_ctx->conn_next->conn_prev = stream_ctx->conn_prev;
  }
  if (stream_ctx->rt_prev != NULL) {
    stream_ctx->rt_prev->rt_next = stream_ctx->rt_next;
  } else if (rt->streams_by_transport == stream_ctx) {
    rt->streams_by_transport = stream_ctx->rt_next;
  }
  if (stream_ctx->rt_next != NULL) {
    stream_ctx->rt_next->rt_prev = stream_ctx->rt_prev;
  }
  stream_ctx->conn_prev = NULL;
  stream_ctx->conn_next = NULL;
  stream_ctx->rt_prev = NULL;
  stream_ctx->rt_next = NULL;
}

static void
runtime_destroy_stream_session(odin_xqc_server_stream_ctx_t *stream_ctx) {
  odin_server_session_t *ss = stream_ctx->ss;
  stream_ctx->ss = NULL;
  runtime_stream_ctx_unlink(stream_ctx);
  if (ss != NULL) {
    odin_server_session_destroy(ss);
  }
  free(stream_ctx);
}

static void runtime_destroy_all_streams(odin_xqc_server_conn_ctx_t *ctx) {
  while (ctx->streams != NULL) {
    odin_xqc_server_stream_ctx_t *stream_ctx = ctx->streams;
    ctx->streams = stream_ctx->conn_next;
    if (ctx->streams != NULL) {
      ctx->streams->conn_prev = NULL;
    }
    stream_ctx->conn_prev = NULL;
    stream_ctx->conn_next = NULL;
    runtime_destroy_stream_session(stream_ctx);
  }
}

static void runtime_conn_ctx_unlink(odin_xqc_server_conn_ctx_t *ctx) {
  odin_xqc_server_runtime_t *rt = ctx->rt;
  if (ctx->prev != NULL) {
    ctx->prev->next = ctx->next;
  } else if (rt->connections == ctx) {
    rt->connections = ctx->next;
  }
  if (ctx->next != NULL) {
    ctx->next->prev = ctx->prev;
  }
  ctx->prev = NULL;
  ctx->next = NULL;
}

static odin_xqc_server_conn_ctx_t *
runtime_find_conn_by_conn(odin_xqc_server_runtime_t *rt,
                          xqc_connection_t *conn) {
  for (odin_xqc_server_conn_ctx_t *ctx = rt->connections; ctx != NULL;
       ctx = ctx->next) {
    if (ctx->conn == conn) {
      return ctx;
    }
  }
  return NULL;
}

static int runtime_conn_is_linked(odin_xqc_server_conn_ctx_t *ctx) {
  if (ctx == NULL || ctx->rt == NULL) {
    return 0;
  }
  return runtime_find_conn_by_conn(ctx->rt, ctx->conn) == ctx;
}

static odin_xqc_server_conn_ctx_t *
runtime_find_conn_by_proto_data(odin_xqc_server_runtime_t *rt,
                                xqc_connection_t *conn, void *conn_proto_data) {
  for (odin_xqc_server_conn_ctx_t *ctx = rt->connections; ctx != NULL;
       ctx = ctx->next) {
    if (ctx == conn_proto_data && ctx->conn == conn) {
      return ctx;
    }
  }
  return NULL;
}

static odin_xqc_server_stream_ctx_t *
runtime_find_stream_by_transport(odin_xqc_server_runtime_t *rt,
                                 void *transport) {
  for (odin_xqc_server_stream_ctx_t *stream_ctx = rt->streams_by_transport;
       stream_ctx != NULL; stream_ctx = stream_ctx->rt_next) {
    if (stream_ctx->transport == transport) {
      return stream_ctx;
    }
  }
  return NULL;
}

static void
runtime_drain_destroy_pending_connections(odin_xqc_server_runtime_t *rt) {
  if (rt->drain_active) {
    return;
  }
  rt->drain_active = 1;
  odin_xqc_server_conn_ctx_t *ctx = rt->connections;
  while (ctx != NULL) {
    odin_xqc_server_conn_ctx_t *next = ctx->next;
    const int was_closing =
        ctx->destroy_snapshot_valid ? ctx->pre_destroy_closing : ctx->closing;
    runtime_destroy_all_streams(ctx);
    if (!was_closing && ctx->cid_registered && !ctx->destroy_close_requested) {
      ctx->destroy_close_requested = 1;
      (void)runtime_conn_close_call(odin_xqc_udp_engine(rt->xu),
                                    &ctx->current_cid);
    }
    ctx = next;
  }
  rt->drain_active = 0;
}

static int runtime_maybe_finish_destroy(odin_xqc_server_runtime_t *rt) {
  if (!rt->destroy_pending || rt->active_entries != 0) {
    return 0;
  }
  if (rt->force_destroy_active || rt->finish_destroy_in_progress) {
    return 0;
  }
  if (rt->drain_active) {
    return 0;
  }
  if (rt->connections != NULL) {
    runtime_drain_destroy_pending_connections(rt);
  }
  if (rt->connections == NULL) {
    runtime_finish_destroy(rt);
    return 1;
  }
  return 0;
}

static int runtime_callback_leave(odin_xqc_server_runtime_t *rt) {
  rt->active_entries -= 1;
  return runtime_maybe_finish_destroy(rt);
}

static void runtime_mark_destroy_pending(odin_xqc_server_runtime_t *rt) {
  if (rt->xu != NULL) {
    (void)runtime_udp_stop_call(rt->xu);
  }
  if (!rt->destroy_pending) {
    rt->destroy_pending = 1;
    for (odin_xqc_server_conn_ctx_t *ctx = rt->connections; ctx != NULL;
         ctx = ctx->next) {
      if (!ctx->destroy_snapshot_valid) {
        ctx->pre_destroy_closing = ctx->closing;
        ctx->destroy_close_requested = 0;
        ctx->destroy_snapshot_valid = 1;
      }
      ctx->closing = 1;
    }
  }
}

static odin_xqc_server_conn_ctx_t *
runtime_conn_ctx_from_stream(xqc_stream_t *stream) {
  return (odin_xqc_server_conn_ctx_t *)
      runtime_get_conn_alp_user_data_by_stream_call(stream);
}

static int xqc_stream_transport_factory(odin_transport_ready_cb on_ready,
                                        void *ready_user_data,
                                        void *factory_user_data,
                                        odin_transport_t **out) {
  odin_xqc_server_stream_ctx_t *stream_ctx =
      (odin_xqc_server_stream_ctx_t *)factory_user_data;
  if (odin_xqc_stream_transport_create(stream_ctx->stream, on_ready,
                                       ready_user_data, out) != 0) {
    return -1;
  }
  stream_ctx->transport = *out;
  return 0;
}

int odin_xqc_server_runtime_create(
    const odin_xqc_server_runtime_config_t *config,
    odin_xqc_server_runtime_t **out) {
  if (config == NULL || config->loop == NULL || config->local_addr == NULL ||
      config->engine_callbacks == NULL || out == NULL) {
    errno = EINVAL;
    return -1;
  }
  odin_xqc_server_runtime_t *rt =
      (odin_xqc_server_runtime_t *)calloc(1, sizeof(*rt));
  if (rt == NULL) {
    errno = ENOMEM;
    return -1;
  }
  rt->loop = config->loop;
  rt->transport_callbacks.server_accept = runtime_server_accept;
  rt->transport_callbacks.server_refuse = runtime_server_refuse;
  rt->transport_callbacks.conn_update_cid_notify = runtime_conn_update_cid;
  rt->app_callbacks.conn_cbs.conn_create_notify = runtime_conn_create_notify;
  rt->app_callbacks.conn_cbs.conn_close_notify = runtime_conn_close_notify;
  rt->app_callbacks.stream_cbs.stream_read_notify = runtime_stream_read_notify;
  rt->app_callbacks.stream_cbs.stream_write_notify =
      runtime_stream_write_notify;
  rt->app_callbacks.stream_cbs.stream_create_notify =
      runtime_stream_create_notify;
  rt->app_callbacks.stream_cbs.stream_close_notify =
      runtime_stream_close_notify;
  rt->app_callbacks.stream_cbs.stream_closing_notify =
      runtime_stream_closing_notify;

  odin_xqc_udp_config_t udp_config;
  memset(&udp_config, 0, sizeof(udp_config));
  udp_config.loop = config->loop;
  udp_config.local_addr = config->local_addr;
  udp_config.local_addrlen = config->local_addrlen;
  udp_config.engine_type = XQC_ENGINE_SERVER;
  udp_config.engine_config = config->engine_config;
  udp_config.ssl_config = config->ssl_config;
  udp_config.engine_callbacks = config->engine_callbacks;
  udp_config.transport_callbacks = &rt->transport_callbacks;
  udp_config.app_user_data = rt;
  if (runtime_udp_create_call(&udp_config, &rt->xu) != 0) {
    const int saved = errno;
    free(rt);
    errno = saved;
    return -1;
  }
  if (runtime_engine_register_alpn_call(odin_xqc_udp_engine(rt->xu),
                                        ODIN_XQC_SERVER_ALPN,
                                        sizeof(ODIN_XQC_SERVER_ALPN) - 1u,
                                        &rt->app_callbacks, rt) != XQC_OK) {
    runtime_udp_destroy_call(rt->xu);
    free(rt);
    errno = EIO;
    return -1;
  }
  rt->alpn_registered = 1;
  *out = rt;
  return 0;
}

int odin_xqc_server_runtime_start(odin_xqc_server_runtime_t *rt) {
  if (rt == NULL) {
    errno = EINVAL;
    return -1;
  }
  return runtime_udp_start_call(rt->xu);
}

int odin_xqc_server_runtime_stop(odin_xqc_server_runtime_t *rt) {
  if (rt == NULL) {
    errno = EINVAL;
    return -1;
  }
  return runtime_udp_stop_call(rt->xu);
}

int odin_xqc_server_runtime_local_addr(odin_xqc_server_runtime_t *rt,
                                       struct sockaddr *addr,
                                       socklen_t *addrlen) {
  if (rt == NULL) {
    errno = EINVAL;
    return -1;
  }
  return odin_xqc_udp_local_addr(rt->xu, addr, addrlen);
}

void odin_xqc_server_runtime_set_dial_filter(
    odin_xqc_server_runtime_t *rt, odin_server_session_dial_filter_cb cb,
    void *user_data) {
  if (rt == NULL) {
    return;
  }
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  odin_xqc_server_runtime_test_call_t *call = runtime_test_append_call(
      ODIN_XQC_SERVER_RUNTIME_TEST_CALL_SET_DIAL_FILTER);
  if (call != NULL) {
    call->dial_filter_cb = cb;
    call->user_data = user_data;
  }
#endif
  rt->dial_filter = cb;
  rt->dial_filter_ud = cb == NULL ? NULL : user_data;
}

void odin_xqc_server_runtime_destroy(odin_xqc_server_runtime_t *rt) {
  if (rt == NULL) {
    return;
  }
  if (rt->connections != NULL) {
    runtime_mark_destroy_pending(rt);
    if (rt->active_entries == 0) {
      (void)runtime_maybe_finish_destroy(rt);
    }
    return;
  }
  if (rt->xu != NULL) {
    (void)runtime_udp_stop_call(rt->xu);
  }
  runtime_finish_destroy(rt);
}

void odin_xqc_server_runtime_force_destroy(odin_xqc_server_runtime_t *rt) {
  if (rt == NULL) {
    return;
  }
  rt->force_destroy_active = 1;
  runtime_mark_destroy_pending(rt);
  while (rt->connections != NULL) {
    odin_xqc_server_conn_ctx_t *ctx = rt->connections;
    while (ctx->streams != NULL) {
      odin_xqc_server_stream_ctx_t *stream_ctx = ctx->streams;
      odin_server_session_t *ss = stream_ctx->ss;
      stream_ctx->ss = NULL;
      if (ss != NULL) {
        odin_server_session_destroy(ss);
      }
      stream_ctx->transport = NULL;
      runtime_stream_ctx_unlink(stream_ctx);
      stream_ctx->force_next = rt->force_streams;
      rt->force_streams = stream_ctx;
    }
    runtime_conn_set_alp_user_data_call(ctx->conn, NULL);
    if (ctx->cid_registered) {
      runtime_udp_unregister_conn_call(rt->xu, &ctx->current_cid);
      ctx->cid_registered = 0;
    }
    if (!ctx->destroy_close_requested && rt->xu != NULL) {
      ctx->destroy_close_requested = 1;
      (void)runtime_conn_close_call(odin_xqc_udp_engine(rt->xu),
                                    &ctx->current_cid);
    }
    ctx->closing = 1;
    runtime_conn_ctx_unlink(ctx);
    ctx->force_next = rt->force_conns;
    rt->force_conns = ctx;
  }
  runtime_finish_destroy(rt);
}

static int runtime_server_accept(xqc_engine_t *engine, xqc_connection_t *conn,
                                 const xqc_cid_t *cid, void *user_data) {
  (void)engine;
  odin_xqc_udp_t *xu = (odin_xqc_udp_t *)user_data;
  if (xu == NULL) {
    errno = EINVAL;
    return -1;
  }
  odin_xqc_server_runtime_t *rt =
      (odin_xqc_server_runtime_t *)odin_xqc_udp_app_user_data(xu);
  if (rt == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (rt->force_destroy_active) {
    errno = EINVAL;
    return -1;
  }
  runtime_callback_enter(rt);
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  if (g_fail_next_conn_context_alloc_armed &&
      g_fail_next_conn_context_alloc_rt == rt) {
    const int errnum = g_fail_next_conn_context_alloc_errno;
    g_fail_next_conn_context_alloc_armed = 0;
    g_fail_next_conn_context_alloc_errno = 0;
    g_fail_next_conn_context_alloc_rt = NULL;
    errno = errnum;
    (void)runtime_callback_leave(rt);
    return -1;
  }
#endif
  odin_xqc_server_conn_ctx_t *ctx =
      (odin_xqc_server_conn_ctx_t *)calloc(1, sizeof(*ctx));
  if (ctx == NULL) {
    errno = ENOMEM;
    (void)runtime_callback_leave(rt);
    return -1;
  }
  if (runtime_udp_register_conn_call(xu, cid) != 0) {
    const int saved = errno;
    free(ctx);
    errno = saved;
    (void)runtime_callback_leave(rt);
    return -1;
  }
  ctx->rt = rt;
  ctx->conn = conn;
  ctx->current_cid = *cid;
  ctx->cid_registered = 1;
  ctx->next = rt->connections;
  if (rt->connections != NULL) {
    rt->connections->prev = ctx;
  }
  rt->connections = ctx;
  runtime_conn_set_transport_user_data_call(conn,
                                            odin_xqc_udp_xqc_user_data(xu));
  runtime_conn_set_alp_user_data_call(conn, ctx);
  if (rt->destroy_pending) {
    ctx->pre_destroy_closing = ctx->closing;
    ctx->destroy_snapshot_valid = 1;
    ctx->closing = 1;
  }
  (void)runtime_callback_leave(rt);
  return 0;
}

static void runtime_server_refuse(xqc_engine_t *engine, xqc_connection_t *conn,
                                  const xqc_cid_t *cid, void *user_data) {
  (void)engine;
  (void)cid;
  odin_xqc_udp_t *xu = (odin_xqc_udp_t *)user_data;
  if (xu == NULL) {
    return;
  }
  odin_xqc_server_runtime_t *rt =
      (odin_xqc_server_runtime_t *)odin_xqc_udp_app_user_data(xu);
  if (rt == NULL) {
    return;
  }
  runtime_callback_enter(rt);
  odin_xqc_server_conn_ctx_t *ctx = runtime_find_conn_by_conn(rt, conn);
  if (ctx != NULL) {
    if (rt->force_destroy_active) {
      if (ctx->cid_registered) {
        runtime_udp_unregister_conn_call(ctx->rt->xu, &ctx->current_cid);
        ctx->cid_registered = 0;
      }
      ctx->closing = 1;
      (void)runtime_callback_leave(rt);
      return;
    }
    runtime_destroy_all_streams(ctx);
    if (ctx->cid_registered) {
      runtime_udp_unregister_conn_call(ctx->rt->xu, &ctx->current_cid);
      ctx->cid_registered = 0;
    }
    runtime_conn_ctx_unlink(ctx);
    free(ctx);
  }
  (void)runtime_callback_leave(rt);
}

static void runtime_conn_update_cid(xqc_connection_t *conn,
                                    const xqc_cid_t *retire_cid,
                                    const xqc_cid_t *new_cid,
                                    void *conn_user_data) {
  odin_xqc_udp_t *xu = (odin_xqc_udp_t *)conn_user_data;
  if (xu == NULL) {
    return;
  }
  odin_xqc_server_runtime_t *rt =
      (odin_xqc_server_runtime_t *)odin_xqc_udp_app_user_data(xu);
  if (rt == NULL) {
    return;
  }
  if (rt->force_destroy_active) {
    return;
  }
  runtime_callback_enter(rt);
  odin_xqc_server_conn_ctx_t *ctx = runtime_find_conn_by_conn(rt, conn);
  if (ctx == NULL || ctx->closing) {
    (void)runtime_callback_leave(rt);
    return;
  }
  if (runtime_udp_register_conn_call(rt->xu, new_cid) != 0) {
    ctx->closing = 1;
    runtime_destroy_all_streams(ctx);
    (void)runtime_conn_close_call(odin_xqc_udp_engine(rt->xu),
                                  &ctx->current_cid);
    if (ctx->cid_registered) {
      runtime_udp_unregister_conn_call(rt->xu, &ctx->current_cid);
      ctx->cid_registered = 0;
    }
    (void)runtime_callback_leave(rt);
    return;
  }
  runtime_udp_unregister_conn_call(rt->xu, retire_cid);
  ctx->current_cid = *new_cid;
  ctx->cid_registered = 1;
  (void)runtime_callback_leave(rt);
}

static int runtime_conn_create_notify(xqc_connection_t *conn,
                                      const xqc_cid_t *cid,
                                      void *conn_user_data,
                                      void *conn_proto_data) {
  (void)cid;
  odin_xqc_udp_t *xu = (odin_xqc_udp_t *)conn_user_data;
  if (xu == NULL || conn_proto_data == NULL) {
    return -1;
  }
  odin_xqc_server_runtime_t *rt =
      (odin_xqc_server_runtime_t *)odin_xqc_udp_app_user_data(xu);
  if (rt == NULL) {
    return -1;
  }
  if (rt->force_destroy_active) {
    return -1;
  }
  runtime_callback_enter(rt);
  odin_xqc_server_conn_ctx_t *ctx =
      runtime_find_conn_by_proto_data(rt, conn, conn_proto_data);
  const int ok = ctx != NULL;
  (void)runtime_callback_leave(rt);
  return ok ? 0 : -1;
}

static int runtime_conn_close_notify(xqc_connection_t *conn,
                                     const xqc_cid_t *cid, void *conn_user_data,
                                     void *conn_proto_data) {
  (void)cid;
  odin_xqc_udp_t *xu = (odin_xqc_udp_t *)conn_user_data;
  if (xu == NULL) {
    return 0;
  }
  odin_xqc_server_runtime_t *rt =
      (odin_xqc_server_runtime_t *)odin_xqc_udp_app_user_data(xu);
  if (rt == NULL) {
    return 0;
  }
  runtime_callback_enter(rt);
  odin_xqc_server_conn_ctx_t *ctx =
      runtime_find_conn_by_proto_data(rt, conn, conn_proto_data);
  if (ctx == NULL) {
    ctx = runtime_find_conn_by_conn(rt, conn);
  }
  if (ctx != NULL) {
    if (rt->force_destroy_active) {
      if (ctx->cid_registered) {
        runtime_udp_unregister_conn_call(ctx->rt->xu, &ctx->current_cid);
        ctx->cid_registered = 0;
      }
      ctx->closing = 1;
      (void)runtime_callback_leave(rt);
      return 0;
    }
    runtime_destroy_all_streams(ctx);
    if (ctx->cid_registered) {
      runtime_udp_unregister_conn_call(ctx->rt->xu, &ctx->current_cid);
      ctx->cid_registered = 0;
    }
    runtime_conn_ctx_unlink(ctx);
    free(ctx);
  }
  (void)runtime_callback_leave(rt);
  return 0;
}

static xqc_int_t runtime_stream_create_notify(xqc_stream_t *stream,
                                              void *strm_user_data) {
  (void)strm_user_data;
  odin_xqc_server_conn_ctx_t *ctx = runtime_conn_ctx_from_stream(stream);
  if (ctx == NULL || !runtime_conn_is_linked(ctx)) {
    (void)runtime_stream_close_call(stream);
    return XQC_OK;
  }
  odin_xqc_server_runtime_t *rt = ctx->rt;
  runtime_callback_enter(rt);
  if (ctx->closing ||
      runtime_stream_get_direction_call(stream) != XQC_STREAM_BIDI) {
    (void)runtime_stream_close_call(stream);
    (void)runtime_callback_leave(rt);
    return XQC_OK;
  }
#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)
  if (g_fail_next_stream_context_alloc_armed &&
      g_fail_next_stream_context_alloc_rt == rt) {
    const int errnum = g_fail_next_stream_context_alloc_errno;
    g_fail_next_stream_context_alloc_armed = 0;
    g_fail_next_stream_context_alloc_errno = 0;
    g_fail_next_stream_context_alloc_rt = NULL;
    errno = errnum;
    (void)runtime_stream_close_call(stream);
    (void)runtime_callback_leave(rt);
    return XQC_OK;
  }
#endif
  odin_xqc_server_stream_ctx_t *stream_ctx =
      (odin_xqc_server_stream_ctx_t *)calloc(1, sizeof(*stream_ctx));
  if (stream_ctx == NULL) {
    (void)runtime_stream_close_call(stream);
    (void)runtime_callback_leave(rt);
    return XQC_OK;
  }
  stream_ctx->conn_ctx = ctx;
  stream_ctx->stream = stream;
  if (odin_server_session_create_with_transport(
          rt->loop, xqc_stream_transport_factory, stream_ctx,
          runtime_stream_session_on_close, stream_ctx, &stream_ctx->ss) != 0) {
    stream_ctx->transport = NULL;
    (void)runtime_stream_close_call(stream);
    free(stream_ctx);
    (void)runtime_callback_leave(rt);
    return XQC_OK;
  }
  odin_server_session_set_dial_filter(stream_ctx->ss, rt->dial_filter,
                                      rt->dial_filter_ud);
  stream_ctx->conn_next = ctx->streams;
  if (ctx->streams != NULL) {
    ctx->streams->conn_prev = stream_ctx;
  }
  ctx->streams = stream_ctx;
  stream_ctx->rt_next = rt->streams_by_transport;
  if (rt->streams_by_transport != NULL) {
    rt->streams_by_transport->rt_prev = stream_ctx;
  }
  rt->streams_by_transport = stream_ctx;
  (void)runtime_callback_leave(rt);
  return XQC_OK;
}

static xqc_int_t runtime_stream_read_notify(xqc_stream_t *stream,
                                            void *strm_user_data) {
  odin_xqc_server_conn_ctx_t *ctx = runtime_conn_ctx_from_stream(stream);
  if (ctx == NULL || !runtime_conn_is_linked(ctx)) {
    return odin_xqc_stream_transport_read_notify(stream, strm_user_data);
  }
  odin_xqc_server_runtime_t *rt = ctx->rt;
  if (rt->force_destroy_active) {
    return XQC_OK;
  }
  runtime_callback_enter(rt);
  const xqc_int_t rc =
      odin_xqc_stream_transport_read_notify(stream, strm_user_data);
  (void)runtime_callback_leave(rt);
  return rc;
}

static xqc_int_t runtime_stream_write_notify(xqc_stream_t *stream,
                                             void *strm_user_data) {
  odin_xqc_server_conn_ctx_t *ctx = runtime_conn_ctx_from_stream(stream);
  if (ctx == NULL || !runtime_conn_is_linked(ctx)) {
    return odin_xqc_stream_transport_write_notify(stream, strm_user_data);
  }
  odin_xqc_server_runtime_t *rt = ctx->rt;
  if (rt->force_destroy_active) {
    return XQC_OK;
  }
  runtime_callback_enter(rt);
  const xqc_int_t rc =
      odin_xqc_stream_transport_write_notify(stream, strm_user_data);
  (void)runtime_callback_leave(rt);
  return rc;
}

static xqc_int_t runtime_stream_close_notify(xqc_stream_t *stream,
                                             void *strm_user_data) {
  if (strm_user_data == NULL) {
    return XQC_OK;
  }
  odin_xqc_server_conn_ctx_t *ctx = runtime_conn_ctx_from_stream(stream);
  if (ctx == NULL || !runtime_conn_is_linked(ctx)) {
    return XQC_OK;
  }
  odin_xqc_server_runtime_t *rt = ctx->rt;
  if (rt->force_destroy_active) {
    return XQC_OK;
  }
  runtime_callback_enter(rt);
  odin_xqc_server_stream_ctx_t *stream_ctx =
      runtime_find_stream_by_transport(rt, strm_user_data);
  if (stream_ctx != NULL) {
    runtime_destroy_stream_session(stream_ctx);
  }
  (void)runtime_callback_leave(rt);
  return XQC_OK;
}

static void runtime_stream_closing_notify(xqc_stream_t *stream,
                                          xqc_int_t err_code,
                                          void *strm_user_data) {
  odin_xqc_server_conn_ctx_t *ctx = runtime_conn_ctx_from_stream(stream);
  if (ctx == NULL || !runtime_conn_is_linked(ctx)) {
    return;
  }
  odin_xqc_server_runtime_t *rt = ctx->rt;
  if (rt->force_destroy_active) {
    return;
  }
  runtime_callback_enter(rt);
  odin_xqc_stream_transport_closing_notify(stream, err_code, strm_user_data);
  (void)runtime_callback_leave(rt);
}

static void runtime_stream_session_on_close(odin_server_session_t *ss, int err,
                                            void *user_data) {
  odin_xqc_server_stream_ctx_t *stream_ctx =
      (odin_xqc_server_stream_ctx_t *)user_data;
  odin_xqc_server_runtime_t *rt = stream_ctx->conn_ctx->rt;
  runtime_stream_ctx_unlink(stream_ctx);
  if (err != 0) {
    (void)runtime_stream_close_call(stream_ctx->stream);
  }
  odin_server_session_destroy(ss);
  free(stream_ctx);
  (void)runtime_maybe_finish_destroy(rt);
}
