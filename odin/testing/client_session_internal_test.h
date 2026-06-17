/* odin/testing/client_session_internal_test.h */

#ifndef ODIN_CLIENT_SESSION_INTERNAL_TEST_H_
#define ODIN_CLIENT_SESSION_INTERNAL_TEST_H_

#if defined(ODIN_CLIENT_SESSION_TESTING)

#include "odin/client_session.h"

#ifdef __cplusplus
extern "C" {
#endif

int odin_client_session_test_fail_next_dial(odin_client_session_t *cs,
                                            int errnum);
int odin_client_session_test_fail_next_upstream_transport_create(
    odin_client_session_t *cs, int errnum);
int odin_client_session_test_fail_next_connect_session_create(
    odin_client_session_t *cs, int errnum);
int odin_client_session_test_fail_next_http_parse_tail_write(
    odin_client_session_t *cs, int errnum);
int odin_client_session_test_fail_next_client_tail_write(
    odin_client_session_t *cs, int errnum);
int odin_client_session_test_fail_next_relay_create(odin_client_session_t *cs,
                                                    int errnum);
int odin_client_session_test_fail_next_relay_start(odin_client_session_t *cs,
                                                   int errnum);
int odin_client_session_test_arm_next_kqueue_read_fault_at_relay_start(
    odin_client_session_t *cs, int errnum);
int odin_client_session_test_state(const odin_client_session_t *cs);

#define ODIN_CLIENT_SESSION_TEST_STATE_PARSING 1
#define ODIN_CLIENT_SESSION_TEST_STATE_DIALING 2
#define ODIN_CLIENT_SESSION_TEST_STATE_HANDSHAKE 3
#define ODIN_CLIENT_SESSION_TEST_STATE_WRITING_OK_HTTP 4
#define ODIN_CLIENT_SESSION_TEST_STATE_WRITING_ERR_HTTP 5
#define ODIN_CLIENT_SESSION_TEST_STATE_RELAY 6
#define ODIN_CLIENT_SESSION_TEST_STATE_TERMINAL 7

#ifdef __cplusplus
}
#endif

#endif /* defined(ODIN_CLIENT_SESSION_TESTING) */

#endif /* ODIN_CLIENT_SESSION_INTERNAL_TEST_H_ */
