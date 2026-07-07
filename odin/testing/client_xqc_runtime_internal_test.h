/* odin/testing/client_xqc_runtime_internal_test.h */

#ifndef ODIN_CLIENT_XQC_RUNTIME_INTERNAL_TEST_H_
#define ODIN_CLIENT_XQC_RUNTIME_INTERNAL_TEST_H_

#if defined(ODIN_XQC_CLIENT_RUNTIME_TESTING)

#include "odin/client_xqc_runtime.h"
#include "odin/xqc_udp.h"
#include <stdint.h>
#include <xquic/xquic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CAP 128u

typedef enum odin_xqc_client_runtime_test_call_kind_t {
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_CREATE = 1,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_START,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_STOP,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_REGISTER_CONN,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_REGISTER_ALPN,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_XQC_CONNECT,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_SET_ALP_USER_DATA,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_GET_CONN_ALP_USER_DATA_BY_STREAM,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CREATE_BIDI,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_RUNTIME_FREE
} odin_xqc_client_runtime_test_call_kind_t;

typedef struct odin_xqc_client_runtime_test_call_t {
  odin_xqc_client_runtime_test_call_kind_t kind;
  xqc_engine_t *engine;
  odin_xqc_udp_t *xu;
  xqc_connection_t *conn;
  xqc_stream_t *stream;
  xqc_cid_t cid;
  const char *alpn;
  size_t alpn_len;
  xqc_app_proto_callbacks_t *app_callbacks;
  xqc_transport_callbacks_t transport_callbacks_value;
  void *user_data;
  xqc_int_t xqc_result;
  int int_result;
} odin_xqc_client_runtime_test_call_t;

typedef struct odin_xqc_client_runtime_test_udp_create_record_t {
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
} odin_xqc_client_runtime_test_udp_create_record_t;

typedef struct odin_xqc_client_runtime_test_default_create_record_t {
  const char *ca_file;
  size_t ca_file_len;
  char ca_file_value[4096];
  const xqc_config_t *engine_config;
  const xqc_engine_ssl_config_t *engine_ssl_config;
  xqc_engine_ssl_config_t engine_ssl_config_value;
  const xqc_engine_callback_t *engine_callbacks;
  xqc_engine_callback_t engine_callbacks_value;
  const xqc_transport_callbacks_t *transport_callbacks;
  xqc_transport_callbacks_t transport_callbacks_value;
  const xqc_conn_settings_t *conn_settings;
  xqc_conn_settings_t conn_settings_value;
  const xqc_conn_ssl_config_t *conn_ssl_config;
  xqc_conn_ssl_config_t conn_ssl_config_value;
  const unsigned char *token;
  unsigned int token_len;
  int no_crypto_flag;
} odin_xqc_client_runtime_test_default_create_record_t;

typedef struct odin_xqc_client_runtime_test_temp_counters_t {
  uint64_t leaf_x509_allocs;
  uint64_t leaf_x509_frees;
  uint64_t intermediate_x509_allocs;
  uint64_t intermediate_x509_frees;
  uint64_t intermediate_stack_allocs;
  uint64_t intermediate_stack_frees;
  uint64_t store_ctx_allocs;
  uint64_t store_ctx_frees;
} odin_xqc_client_runtime_test_temp_counters_t;

typedef enum odin_xqc_client_runtime_test_x509_store_ctx_failpoint_t {
  ODIN_XQC_CLIENT_RUNTIME_TEST_X509_STORE_CTX_FAIL_INVALID = 0,
  ODIN_XQC_CLIENT_RUNTIME_TEST_X509_STORE_CTX_FAIL_NEW = 1,
  ODIN_XQC_CLIENT_RUNTIME_TEST_X509_STORE_CTX_FAIL_INIT = 2,
  ODIN_XQC_CLIENT_RUNTIME_TEST_X509_STORE_CTX_FAIL_SET_DEFAULT = 3
} odin_xqc_client_runtime_test_x509_store_ctx_failpoint_t;

typedef struct odin_xqc_client_runtime_test_record_t {
  unsigned int call_count;
  unsigned int dropped_call_count;
  odin_xqc_client_runtime_test_call_t
      calls[ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CAP];
  unsigned int udp_create_calls;
  odin_xqc_client_runtime_test_udp_create_record_t last_udp_create;
  unsigned int default_create_calls;
  odin_xqc_client_runtime_test_default_create_record_t last_default_create;
  unsigned int runtime_free_calls;
  unsigned int force_destroy_null_calls;
  uint64_t ca_store_alloc_attempts;
  uint64_t ca_store_alloc_failures;
  uint64_t ca_store_load_attempts;
  uint64_t ca_store_load_successes;
  uint64_t ca_store_free_calls;
  void *verifier_user_data;
  int runtime_owned_ca_store_present;
  uint64_t cert_verify_successes;
  uint64_t cert_verify_failures;
  int last_verifier_x509_error;
  int last_verifier_setup_failure_kind;
  odin_xqc_client_runtime_test_temp_counters_t verifier_temps;
} odin_xqc_client_runtime_test_record_t;

typedef struct odin_xqc_client_runtime_test_state_t {
  xqc_connection_t *conn;
  xqc_cid_t current_cid;
  int cid_registered;
  int handshake_done;
  int closing;
  int connect_errno;
  size_t pending_fds;
  size_t live_sessions;
  int ca_store_present;
} odin_xqc_client_runtime_test_state_t;

typedef enum odin_xqc_client_runtime_test_config_copy_alloc_t {
  ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_SERVER_HOST = 1,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_TOKEN,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_SESSION_TICKET,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_TRANSPORT_PARAMETERS
} odin_xqc_client_runtime_test_config_copy_alloc_t;

typedef struct odin_xqc_client_runtime_test_ops_t {
  xqc_int_t (*engine_register_alpn)(xqc_engine_t *engine, const char *alpn,
                                    size_t alpn_len,
                                    xqc_app_proto_callbacks_t *app_callbacks,
                                    void *user_data);
  xqc_int_t (*engine_unregister_alpn)(xqc_engine_t *engine, const char *alpn,
                                      size_t alpn_len);
  const xqc_cid_t *(*xqc_connect)(xqc_engine_t *engine,
                                  const xqc_conn_settings_t *conn_settings,
                                  const unsigned char *token,
                                  unsigned int token_len,
                                  const char *server_host, int no_crypto_flag,
                                  const xqc_conn_ssl_config_t *conn_ssl_config,
                                  const struct sockaddr *peer_addr,
                                  socklen_t peer_addrlen, const char *alpn,
                                  void *user_data);
  void (*conn_set_alp_user_data)(xqc_connection_t *conn, void *user_data);
  xqc_int_t (*conn_close)(xqc_engine_t *engine, const xqc_cid_t *cid);
  void *(*get_conn_alp_user_data_by_stream)(xqc_stream_t *stream);
  xqc_stream_t *(*stream_create_with_direction)(xqc_connection_t *conn,
                                                xqc_stream_direction_t dir,
                                                void *user_data);
  xqc_int_t (*stream_close)(xqc_stream_t *stream);
  int (*udp_register_conn)(odin_xqc_udp_t *xu, const xqc_cid_t *cid);
  void (*udp_unregister_conn)(odin_xqc_udp_t *xu, const xqc_cid_t *cid);
} odin_xqc_client_runtime_test_ops_t;

void odin_xqc_client_runtime_test_reset(void);
void odin_xqc_client_runtime_test_set_ops(
    const odin_xqc_client_runtime_test_ops_t *ops);
const odin_xqc_client_runtime_test_record_t *
odin_xqc_client_runtime_test_record(void);
int odin_xqc_client_runtime_test_state(
    const odin_xqc_client_runtime_t *rt,
    odin_xqc_client_runtime_test_state_t *out);
int odin_xqc_client_runtime_test_fail_next_stream_context_alloc(
    odin_xqc_client_runtime_t *rt, int errnum);
int odin_xqc_client_runtime_test_fail_next_pending_queue_append(
    odin_xqc_client_runtime_t *rt, int errnum);
int odin_xqc_client_runtime_test_append_pending_fd(
    odin_xqc_client_runtime_t *rt, int conn_fd);
int odin_xqc_client_runtime_test_fail_config_copy_alloc(
    odin_xqc_client_runtime_test_config_copy_alloc_t site, int errnum);
int odin_xqc_client_runtime_test_fail_next_x509_store_new(int errnum);
int odin_xqc_client_runtime_test_fail_next_x509_store_ctx(
    odin_xqc_client_runtime_test_x509_store_ctx_failpoint_t failpoint);

#ifdef __cplusplus
}
#endif

#endif /* defined(ODIN_XQC_CLIENT_RUNTIME_TESTING) */

#endif /* ODIN_CLIENT_XQC_RUNTIME_INTERNAL_TEST_H_ */
