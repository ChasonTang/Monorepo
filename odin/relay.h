/* odin/relay.h
 *
 * Single-thread, event-loop-driven bidirectional byte relay (RFC-011).
 *
 * Given two caller-owned nonblocking connected stream sockets and an
 * odin_event_loop_t, odin_relay_start watches both fds through the RFC-010
 * odin_event_io_* API and forwards bytes in each direction through its own
 * fixed 64 KiB per-direction backpressure buffer, propagates end-of-stream as
 * a half-close (shutdown(SHUT_WR) on the peer fd after flushing), and invokes
 * a completion callback exactly once.
 *
 * Ownership: the caller owns both fds and the relay object. The relay closes
 * neither fd and frees only its own state; it never closes, DNS-resolves, or
 * selects the transport for the fds. fd_a and fd_b must be distinct, open,
 * nonblocking, caller-owned connected stream sockets, and on_done must be
 * non-null. All entry points and internal callbacks are owner-thread; the
 * relay adds no locks.
 *
 * Completion: on_done fires exactly once on the owner thread, as the relay's
 * final action during teardown -- no relay state is read or written after
 * on_done returns, so odin_relay_destroy(relay) is legal from inside on_done.
 * It reports ODIN_RELAY_OK with err == 0 when both directions reached
 * end-of-stream, or ODIN_RELAY_ERROR with the best-effort failing errno when a
 * genuine read/write/shutdown (or asynchronous socket) error tears the relay
 * down; status is the authoritative signal. A peer's graceful close completes
 * a direction rather than tearing the relay down.
 */

#ifndef ODIN_RELAY_H_
#define ODIN_RELAY_H_

#include "odin/event_loop.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct odin_relay_t odin_relay_t;

typedef enum odin_relay_status_t {
  ODIN_RELAY_OK = 0,
  ODIN_RELAY_ERROR,
} odin_relay_status_t;

typedef void (*odin_relay_done_cb)(odin_relay_t *relay,
                                   odin_relay_status_t status, int err,
                                   void *user_data);

/* Registers one READ-only watch per fd (fd_a first, then fd_b), writes *out,
 * and returns 0. On failure returns -1 with errno set, writes nothing to *out,
 * and leaves no watch registered: ENOMEM when the relay, a buffer, or an I/O
 * handle cannot be allocated, otherwise the errno from odin_event_io_start
 * (including EEXIST when either fd already has an active watch). If fd_a's
 * watch was registered before fd_b's failed, start stops the first and frees
 * partial state before returning. Owner-thread API.
 */
int odin_relay_start(odin_event_loop_t *loop, int fd_a, int fd_b,
                     odin_relay_done_cb on_done, void *user_data,
                     odin_relay_t **out);

/* Stops any still-active watches, frees the two buffers and the relay object,
 * and closes neither fd; never invokes on_done. odin_relay_destroy(NULL) is a
 * no-op, and the pointer is dead afterward. Callable from within on_done to
 * reclaim a completed relay, or on a still-running relay to abort it.
 */
void odin_relay_destroy(odin_relay_t *relay);

#ifdef __cplusplus
}
#endif

#endif /* ODIN_RELAY_H_ */
