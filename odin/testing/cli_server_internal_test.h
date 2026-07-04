/* odin/testing/cli_server_internal_test.h
 *
 * Test-only declarations for the CLI server runner. Visible only when
 * ODIN_CLI_SERVER_TESTING is defined; production builds neither declare
 * nor define these symbols.
 */

#ifndef ODIN_CLI_SERVER_INTERNAL_TEST_H_
#define ODIN_CLI_SERVER_INTERNAL_TEST_H_

#if defined(ODIN_CLI_SERVER_TESTING)

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "odin/server_session.h"
#include "odin/server_xqc_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum odin_cli_server_test_failpoint_t {
  ODIN_CLI_SERVER_TEST_FAIL_EVENT_LOOP_CREATE = 8,
  ODIN_CLI_SERVER_TEST_FAIL_SIGACTION_SIGINT = 10,
  ODIN_CLI_SERVER_TEST_FAIL_SIGACTION_SIGTERM = 11,
  ODIN_CLI_SERVER_TEST_FAIL_SIGNAL_TIMER_START = 12,
  ODIN_CLI_SERVER_TEST_FAIL_XQC_SERVER_RUNTIME_CREATE = 100,
  ODIN_CLI_SERVER_TEST_FAIL_XQC_SERVER_RUNTIME_START = 101,
  ODIN_CLI_SERVER_TEST_FAIL_XQC_SERVER_RUNTIME_LOCAL_ADDR = 102,
  ODIN_CLI_SERVER_TEST_FAIL_QUIC_EVENT_LOOP_RUN = 103,
} odin_cli_server_test_failpoint_t;

typedef struct odin_cli_server_test_liveness_t {
  size_t live_xqc_runtimes;
} odin_cli_server_test_liveness_t;

typedef struct odin_cli_server_test_filter_record_t {
  unsigned int quic_set_count;
  odin_server_session_dial_filter_cb quic_cb;
  void *quic_user_data;
} odin_cli_server_test_filter_record_t;

typedef struct odin_cli_server_test_dial_start_t {
  int family;
  uint32_t ipv4_addr_nbo;
  uint16_t port_nbo;
} odin_cli_server_test_dial_start_t;

int odin_cli_server_test_default_dial_filter(const struct sockaddr *addr,
                                             socklen_t addrlen);

int odin_cli_server_test_fail_next(odin_cli_server_test_failpoint_t fp,
                                   int errnum);
void odin_cli_server_test_reset_liveness(void);
int odin_cli_server_test_liveness(odin_cli_server_test_liveness_t *out);
int odin_cli_server_test_last_bind_addr(struct sockaddr_in *out);
int odin_cli_server_test_set_progress_fd(int fd);
void odin_cli_server_test_set_dial_start_probe_fd(int fd, int errnum);
int odin_cli_server_test_maybe_probe_dial_start(const struct sockaddr *addr,
                                                socklen_t addrlen);
int odin_cli_server_test_filter_record(
    odin_cli_server_test_filter_record_t *out);
int odin_cli_server_test_set_quic_start_probe(
    void (*cb)(odin_xqc_server_runtime_t *rt, void *user_data),
    void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* defined(ODIN_CLI_SERVER_TESTING) */

#endif /* ODIN_CLI_SERVER_INTERNAL_TEST_H_ */
