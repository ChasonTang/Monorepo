/* odin/testing/cli_client_internal_test.h */

#ifndef ODIN_CLI_CLIENT_INTERNAL_TEST_H_
#define ODIN_CLI_CLIENT_INTERNAL_TEST_H_

#if defined(ODIN_CLI_CLIENT_TESTING)

#include <netinet/in.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum odin_cli_client_test_failpoint_t {
  ODIN_CLI_CLIENT_TEST_FAIL_SOCKET = 1,
  ODIN_CLI_CLIENT_TEST_FAIL_SETSOCKOPT_REUSEADDR,
  ODIN_CLI_CLIENT_TEST_FAIL_FCNTL_GETFL,
  ODIN_CLI_CLIENT_TEST_FAIL_FCNTL_SETFL,
  ODIN_CLI_CLIENT_TEST_FAIL_BIND,
  ODIN_CLI_CLIENT_TEST_FAIL_LISTEN,
  ODIN_CLI_CLIENT_TEST_FAIL_GETSOCKNAME,
  ODIN_CLI_CLIENT_TEST_FAIL_EVENT_LOOP_CREATE,
  ODIN_CLI_CLIENT_TEST_FAIL_ACCEPT_LOOP_CREATE,
  ODIN_CLI_CLIENT_TEST_FAIL_SIGACTION_SIGINT,
  ODIN_CLI_CLIENT_TEST_FAIL_SIGACTION_SIGTERM,
  ODIN_CLI_CLIENT_TEST_FAIL_SIGNAL_TIMER_START,
  ODIN_CLI_CLIENT_TEST_FAIL_EVENT_LOOP_RUN,
  ODIN_CLI_CLIENT_TEST_TRIGGER_UNEXPECTED_STOP,
  ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_ERROR,
  ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_GETFL_ERROR,
  ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_SETFL_ERROR,
  ODIN_CLI_CLIENT_TEST_FAIL_NEXT_SESSION_ENTRY_ALLOC,
  ODIN_CLI_CLIENT_TEST_FAIL_NEXT_CLIENT_SESSION_CREATE
} odin_cli_client_test_failpoint_t;

typedef struct odin_cli_client_test_liveness_t {
  size_t live_listeners;
  size_t live_accept_loops;
  size_t live_sessions;
  size_t last_cleanup_sessions;
} odin_cli_client_test_liveness_t;

int odin_cli_client_test_fail_next(odin_cli_client_test_failpoint_t fp,
                                   int errnum);
int odin_cli_client_test_trigger_next(odin_cli_client_test_failpoint_t fp);
void odin_cli_client_test_reset_liveness(void);
int odin_cli_client_test_liveness(odin_cli_client_test_liveness_t *out);
int odin_cli_client_test_last_bind_addr(struct sockaddr_in *out);
int odin_cli_client_test_set_progress_fd(int fd, size_t min_inflight_sessions);
int odin_cli_client_test_set_runtime_trigger_fd(int fd);
int odin_cli_client_test_set_idle_snapshot_fd(int fd,
                                              size_t min_closed_sessions);

#ifdef __cplusplus
}
#endif

#endif /* defined(ODIN_CLI_CLIENT_TESTING) */

#endif /* ODIN_CLI_CLIENT_INTERNAL_TEST_H_ */
