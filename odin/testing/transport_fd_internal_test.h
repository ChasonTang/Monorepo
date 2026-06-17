/* odin/testing/transport_fd_internal_test.h */

#ifndef ODIN_TRANSPORT_FD_INTERNAL_TEST_H_
#define ODIN_TRANSPORT_FD_INTERNAL_TEST_H_

#if defined(ODIN_TRANSPORT_FD_TESTING)

#include "odin/event_loop.h"
#include "odin/transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Snapshots the fd transport's current watch handle into *out, returning 0.
 * Returns -1 with errno == ENOENT when no watch is active. Used by the §6 rows
 * to confirm set_interest's start/stop lifecycle and to feed the live handle to
 * odin_event_loop_test_queue_backend_events.
 */
int odin_fd_transport_test_io(odin_transport_t *t, odin_event_io_t **out);

#ifdef __cplusplus
}
#endif

#endif /* defined(ODIN_TRANSPORT_FD_TESTING) */

#endif /* ODIN_TRANSPORT_FD_INTERNAL_TEST_H_ */
