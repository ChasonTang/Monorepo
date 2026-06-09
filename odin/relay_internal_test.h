/* odin/relay_internal_test.h */

#ifndef ODIN_RELAY_INTERNAL_TEST_H_
#define ODIN_RELAY_INTERNAL_TEST_H_

#if defined(ODIN_RELAY_TESTING)

#include "odin/event_loop.h"
#include "odin/relay.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Snapshots the relay's current fd_a and fd_b watch handles into *out_a and
 * *out_b, returning 0. Returns -1 with errno == ENOENT if either watch is
 * inactive. Used by the §6 same-batch tests to feed the relay's live handles
 * to odin_event_loop_test_queue_backend_events.
 */
int odin_relay_test_io_handles(odin_relay_t *relay, odin_event_io_t **out_a,
                               odin_event_io_t **out_b);

#ifdef __cplusplus
}
#endif

#endif /* defined(ODIN_RELAY_TESTING) */

#endif /* ODIN_RELAY_INTERNAL_TEST_H_ */
