/* odin/relay_v2.h
 *
 * Transport-agnostic bidirectional byte relay (RFC-014, relay v2).
 *
 * A rewrite of the RFC-011 byte relay that forwards bytes between two
 * caller-supplied, caller-owned odin_transport_t endpoints (RFC-013) through the
 * odin_transport_* dispatchers instead of raw fds and odin_event_io_*. It
 * preserves the relay's fixed 64 KiB per-direction backpressure buffering,
 * end-of-stream-as-shutdown_write propagation, single-error aggregation, and
 * exactly-once completion. It depends only on odin/transport.h: it issues no
 * syscalls and registers no watches directly.
 *
 * Two-phase lifecycle: odin_relay_v2_create allocates the relay and its two
 * buffers but binds nothing; the caller then builds the two transports with
 * odin_relay_v2_ready as each transport's on_ready and the relay handle as each
 * transport's user_data; finally odin_relay_v2_start binds the two endpoints and
 * registers a READ interest on each. No readiness can arrive before start.
 *
 * Ownership: the relay owns its object and its two buffers only. It never calls
 * odin_transport_destroy on either transport and never closes any fd; the caller
 * owns both transports and their fds and destroys them after on_done. Because
 * destroy may touch the transports to stop their watches when aborting a
 * still-running relay, the caller must call odin_relay_v2_destroy(relay) BEFORE
 * destroying the two transports.
 *
 * Completion: on_done fires exactly once on the owner thread, as the relay's
 * final action during teardown -- no relay state is read or written after
 * on_done returns, so odin_relay_v2_destroy(relay) (and then the two
 * odin_transport_destroy calls) from inside on_done is legal. It reports
 * ODIN_RELAY_V2_OK with err == 0 when both directions reached end-of-stream, or
 * ODIN_RELAY_V2_ERROR with the failing errno when a genuine read/write/
 * half-close (or latched asynchronous transport) error tears the relay down;
 * status is the authoritative signal. A relay that is created but never started
 * never fires on_done. All entry points and odin_relay_v2_ready run on the owner
 * thread; the relay adds no locks.
 */

#ifndef ODIN_RELAY_V2_H_
#define ODIN_RELAY_V2_H_

#include "odin/transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct odin_relay_v2_t odin_relay_v2_t;

typedef enum odin_relay_v2_status_t {
  ODIN_RELAY_V2_OK = 0,
  ODIN_RELAY_V2_ERROR,
} odin_relay_v2_status_t;

typedef void (*odin_relay_v2_done_cb)(odin_relay_v2_t *relay,
                                      odin_relay_v2_status_t status, int err,
                                      void *user_data);

/* Allocates the relay object and its two 64 KiB buffers, stores on_done /
 * user_data, writes *out, and returns 0; binds no transport and registers
 * nothing. Its only failure is ENOMEM (returns -1, errno == ENOMEM, *out
 * untouched). on_done must be non-null. Owner-thread API.
 */
int  odin_relay_v2_create(odin_relay_v2_done_cb on_done, void *user_data,
                          odin_relay_v2_t **out);

/* The relay's exported readiness trampoline: install it as the on_ready of BOTH
 * transports, with user_data set to the odin_relay_v2_t * from create. It is the
 * only readiness entry point and identifies which endpoint fired by comparing t
 * to the two bound transports. Precondition: invoked only with one of the two
 * bound transports and the matching relay handle.
 */
void odin_relay_v2_ready(odin_transport_t *t, unsigned int events,
                         void *user_data);

/* Binds a and b as direction A (a -> b) and direction B (b -> a), registers a
 * READ interest on each via odin_transport_set_interest, and returns 0. On the
 * first set_interest failure returns -1 with errno preserved and no interest
 * registered; if the second fails it rolls the first back to an empty interest
 * before returning -1 with the second call's errno. After either failure path
 * the relay is left re-startable. Owner-thread API.
 */
int  odin_relay_v2_start(odin_relay_v2_t *relay, odin_transport_t *a,
                         odin_transport_t *b);

/* Stops any interest the relay still holds (odin_transport_set_interest(t, 0) on
 * each still-watched endpoint), frees the two buffers and the relay object, and
 * never invokes on_done; odin_relay_v2_destroy(NULL) is a no-op. Callable from
 * within on_done to reclaim a completed relay, or on a still-running relay to
 * abort it. Must be called before destroying the two transports.
 */
void odin_relay_v2_destroy(odin_relay_v2_t *relay);

#ifdef __cplusplus
}
#endif

#endif /* ODIN_RELAY_V2_H_ */
