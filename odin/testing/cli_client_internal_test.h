/* odin/testing/cli_client_internal_test.h */

#ifndef ODIN_CLI_CLIENT_INTERNAL_TEST_H_
#define ODIN_CLI_CLIENT_INTERNAL_TEST_H_

#if defined(ODIN_CLI_CLIENT_TESTING)

#include "odin/protocol.h"

#include <netinet/in.h>
#include <stddef.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum odin_cli_client_test_failpoint_t {
  ODIN_CLI_CLIENT_TEST_FAIL_NONE = 0,
  ODIN_CLI_CLIENT_TEST_FAIL_SOCKET = 1,
  ODIN_CLI_CLIENT_TEST_FAIL_SETSOCKOPT_REUSEADDR = 2,
  ODIN_CLI_CLIENT_TEST_FAIL_FCNTL_GETFL = 3,
  ODIN_CLI_CLIENT_TEST_FAIL_FCNTL_SETFL = 4,
  ODIN_CLI_CLIENT_TEST_FAIL_BIND = 5,
  ODIN_CLI_CLIENT_TEST_FAIL_LISTEN = 6,
  ODIN_CLI_CLIENT_TEST_FAIL_GETSOCKNAME = 7,
  ODIN_CLI_CLIENT_TEST_FAIL_EVENT_LOOP_CREATE = 8,
  ODIN_CLI_CLIENT_TEST_FAIL_ACCEPT_LOOP_CREATE = 9,
  ODIN_CLI_CLIENT_TEST_FAIL_SIGACTION_SIGINT = 10,
  ODIN_CLI_CLIENT_TEST_FAIL_SIGACTION_SIGTERM = 11,
  ODIN_CLI_CLIENT_TEST_FAIL_SIGNAL_TIMER_START = 12,
  ODIN_CLI_CLIENT_TEST_FAIL_EVENT_LOOP_RUN = 13,
  ODIN_CLI_CLIENT_TEST_TRIGGER_UNEXPECTED_STOP = 14,
  ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_ERROR = 15,
  ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_GETFL_ERROR = 16,
  ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_SETFL_ERROR = 17,
  ODIN_CLI_CLIENT_TEST_FAILPOINT_INVALID = 99,
  ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_CREATE = 100,
  ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_START = 101,
  ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_ADD_CONNECTION = 102,
} odin_cli_client_test_failpoint_t;

typedef struct odin_cli_client_test_liveness_t {
  size_t live_listeners;
  size_t live_accept_loops;
  size_t live_xqc_client_runtimes;
  size_t quic_runtime_create_calls;
  size_t quic_runtime_start_calls;
  size_t quic_runtime_add_connection_calls;
  size_t quic_runtime_force_destroy_calls;
} odin_cli_client_test_liveness_t;

typedef struct odin_cli_client_test_xqc_add_record_t {
  int fd;
  int fd_is_nonblocking;
} odin_cli_client_test_xqc_add_record_t;

typedef struct odin_cli_client_test_runtime_config_record_t {
  const struct sockaddr *local_addr;
  socklen_t local_addrlen;
  struct sockaddr_storage local_addr_value;
  const struct sockaddr *peer_addr;
  socklen_t peer_addrlen;
  struct sockaddr_storage peer_addr_value;
  const char *server_host;
  size_t server_host_len;
  char server_host_value[ODIN_PROTO_HOST_MAX + 1];
  const char *quic_ca_file;
  size_t quic_ca_file_len;
  char quic_ca_file_value[4096];
} odin_cli_client_test_runtime_config_record_t;

int odin_cli_client_test_fail_next(odin_cli_client_test_failpoint_t fp,
                                   int errnum);
int odin_cli_client_test_trigger_next(odin_cli_client_test_failpoint_t fp);
void odin_cli_client_test_reset_liveness(void);
int odin_cli_client_test_liveness(odin_cli_client_test_liveness_t *out);
int odin_cli_client_test_last_bind_addr(struct sockaddr_in *out);
int odin_cli_client_test_last_bound_addr(struct sockaddr_in *out);
int odin_cli_client_test_last_xqc_add(
    odin_cli_client_test_xqc_add_record_t *out);
int odin_cli_client_test_last_runtime_config(
    odin_cli_client_test_runtime_config_record_t *out);
int odin_cli_client_test_pending_failpoint(
    odin_cli_client_test_failpoint_t *out);
int odin_cli_client_test_set_progress_fd(int fd, size_t min_inflight_sessions);
int odin_cli_client_test_set_runtime_trigger_fd(int fd);

#ifdef __cplusplus
}
#endif

#endif /* defined(ODIN_CLI_CLIENT_TESTING) */

#endif /* ODIN_CLI_CLIENT_INTERNAL_TEST_H_ */
