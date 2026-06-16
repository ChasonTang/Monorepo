/* odin/server_runtime_internal_test.h */

#ifndef ODIN_SERVER_RUNTIME_INTERNAL_TEST_H_
#define ODIN_SERVER_RUNTIME_INTERNAL_TEST_H_

#if defined(ODIN_SERVER_RUNTIME_TESTING)

#include "odin/accept_loop.h"
#include "odin/server_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

int odin_server_runtime_test_inflight_count(const odin_server_runtime_t *rt);
int odin_server_runtime_test_is_terminal(const odin_server_runtime_t *rt);
odin_accept_loop_t *
odin_server_runtime_test_get_accept_loop(odin_server_runtime_t *rt);
int odin_server_runtime_test_fail_next_session_create(
    odin_server_runtime_t *rt);
int odin_server_runtime_test_fail_next_entry_alloc(odin_server_runtime_t *rt);

#ifdef __cplusplus
}
#endif

#endif /* defined(ODIN_SERVER_RUNTIME_TESTING) */

#endif /* ODIN_SERVER_RUNTIME_INTERNAL_TEST_H_ */
