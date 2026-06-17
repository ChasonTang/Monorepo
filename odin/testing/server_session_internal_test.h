/* odin/testing/server_session_internal_test.h */

#ifndef ODIN_SERVER_SESSION_INTERNAL_TEST_H_
#define ODIN_SERVER_SESSION_INTERNAL_TEST_H_

#if defined(ODIN_SERVER_SESSION_TESTING)

#include "odin/server_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One-shot fault injection hooks: each arms a per-call-site flag consumed the
 * next time the orchestrator reaches the corresponding call site, then clears
 * after one use. Arming the same hook twice without an intervening trigger
 * overwrites the prior errno. Production builds expose none of these symbols.
 */
int odin_server_session_test_fail_next_dial(odin_server_session_t *ss,
                                            int errnum);
int odin_server_session_test_fail_next_upstream_transport_create(
    odin_server_session_t *ss, int errnum);
int odin_server_session_test_fail_next_tail_write(odin_server_session_t *ss,
                                                  int errnum);
int odin_server_session_test_fail_next_relay_create(odin_server_session_t *ss,
                                                    int errnum);
int odin_server_session_test_fail_next_relay_start(odin_server_session_t *ss,
                                                   int errnum);
int odin_server_session_test_inject_session_error_on_dial(
    odin_server_session_t *ss, int errnum);

/* Returns the current phase constant; never mutates ss. */
int odin_server_session_test_state(const odin_server_session_t *ss);

#define ODIN_SERVER_SESSION_TEST_STATE_HANDSHAKE 1
#define ODIN_SERVER_SESSION_TEST_STATE_DIALING 2
#define ODIN_SERVER_SESSION_TEST_STATE_WRITING_OK_RESP 3
#define ODIN_SERVER_SESSION_TEST_STATE_WRITING_ERR_RESP 4
#define ODIN_SERVER_SESSION_TEST_STATE_RELAY 5
#define ODIN_SERVER_SESSION_TEST_STATE_TERMINAL 6

#ifdef __cplusplus
}
#endif

#endif /* defined(ODIN_SERVER_SESSION_TESTING) */

#endif /* ODIN_SERVER_SESSION_INTERNAL_TEST_H_ */
