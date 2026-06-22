/* odin/testing/server_xqc_runtime_internal_test.h */

#ifndef ODIN_SERVER_XQC_RUNTIME_INTERNAL_TEST_H_
#define ODIN_SERVER_XQC_RUNTIME_INTERNAL_TEST_H_

#if defined(ODIN_XQC_SERVER_RUNTIME_TESTING)

#include "odin/server_xqc_runtime.h"
#include "odin/xqc_udp.h"
#include <xquic/xquic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CAP 128u

typedef enum odin_xqc_server_runtime_test_call_kind_t {
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_CREATE = 1,
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_START,
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_STOP,
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_DESTROY,
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_REGISTER_CONN,
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN,
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_ENGINE_REGISTER_ALPN,
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN,
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_SET_TRANSPORT_USER_DATA,
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_SET_ALP_USER_DATA,
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_CLOSE,
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_GET_DIRECTION,
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_GET_CONN_ALP_USER_DATA_BY_STREAM,
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_CLOSE
} odin_xqc_server_runtime_test_call_kind_t;

typedef struct odin_xqc_server_runtime_test_udp_create_record_t {
  odin_event_loop_t *loop;
  const struct sockaddr *local_addr;
  socklen_t local_addrlen;
  xqc_engine_type_t engine_type;
  const xqc_config_t *engine_config;
  const xqc_engine_ssl_config_t *ssl_config;
  const xqc_engine_callback_t *engine_callbacks;
  xqc_engine_callback_t engine_callbacks_value;
  const xqc_transport_callbacks_t *transport_callbacks;
  xqc_transport_callbacks_t transport_callbacks_value;
  void *app_user_data;
} odin_xqc_server_runtime_test_udp_create_record_t;

typedef struct odin_xqc_server_runtime_test_call_t {
  odin_xqc_server_runtime_test_call_kind_t kind;
  xqc_engine_t *engine;
  odin_xqc_udp_t *xu;
  xqc_connection_t *conn;
  xqc_stream_t *stream;
  xqc_cid_t cid;
  const char *alpn;
  size_t alpn_len;
  xqc_app_proto_callbacks_t *app_callbacks;
  void *user_data;
  xqc_stream_direction_t direction;
  int int_result;
  xqc_int_t xqc_result;
} odin_xqc_server_runtime_test_call_t;

typedef struct odin_xqc_server_runtime_test_record_t {
  unsigned int call_count;
  unsigned int dropped_call_count;
  odin_xqc_server_runtime_test_call_t
      calls[ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CAP];
  unsigned int udp_create_calls;
  odin_xqc_server_runtime_test_udp_create_record_t last_udp_create;
} odin_xqc_server_runtime_test_record_t;

typedef struct odin_xqc_server_runtime_test_ops_t {
  xqc_int_t (*engine_register_alpn)(
      xqc_engine_t *engine, const char *alpn, size_t alpn_len,
      xqc_app_proto_callbacks_t *app_callbacks, void *user_data);
  xqc_int_t (*engine_unregister_alpn)(xqc_engine_t *engine, const char *alpn,
                                      size_t alpn_len);
  void (*conn_set_transport_user_data)(xqc_connection_t *conn,
                                       void *user_data);
  void (*conn_set_alp_user_data)(xqc_connection_t *conn, void *user_data);
  xqc_int_t (*conn_close)(xqc_engine_t *engine, const xqc_cid_t *cid);
  xqc_stream_direction_t (*stream_get_direction)(xqc_stream_t *stream);
  void *(*get_conn_alp_user_data_by_stream)(xqc_stream_t *stream);
  xqc_int_t (*stream_close)(xqc_stream_t *stream);
  int (*udp_register_conn)(odin_xqc_udp_t *xu, const xqc_cid_t *cid);
  void (*udp_unregister_conn)(odin_xqc_udp_t *xu, const xqc_cid_t *cid);
} odin_xqc_server_runtime_test_ops_t;

void odin_xqc_server_runtime_test_reset(void);
void odin_xqc_server_runtime_test_set_ops(
    const odin_xqc_server_runtime_test_ops_t *ops);
const odin_xqc_server_runtime_test_record_t *
odin_xqc_server_runtime_test_record(void);
int odin_xqc_server_runtime_test_fail_next_conn_context_alloc(
    odin_xqc_server_runtime_t *rt, int errnum);
int odin_xqc_server_runtime_test_fail_next_stream_context_alloc(
    odin_xqc_server_runtime_t *rt, int errnum);

#ifdef __cplusplus
}
#endif

#endif /* defined(ODIN_XQC_SERVER_RUNTIME_TESTING) */

#endif /* ODIN_SERVER_XQC_RUNTIME_INTERNAL_TEST_H_ */
