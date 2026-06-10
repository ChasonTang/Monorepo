/* odin/dial.h
 *
 * Single-thread, event-loop-driven nonblocking socket dialer (RFC-012).
 *
 * Given a live odin_event_loop_t and an already-resolved struct sockaddr,
 * odin_dial_start creates one nonblocking SOCK_STREAM socket, issues connect(2),
 * and resolves the attempt entirely from the loop -- watching the socket for
 * writability through the RFC-010 odin_event_io_* API and reading
 * getsockopt(SO_ERROR) to classify the outcome, or, when connect(2) fails
 * immediately, carrying that error to the next loop turn through a 0-delay
 * one-shot odin_event_timer. It performs no name resolution and selects no
 * transport, and depends on no Odin module other than odin/event_loop.h.
 *
 * Ownership: odin_dial_start creates the socket and owns it for the duration of
 * the attempt; the dial object is caller-owned. addr must be non-null and point
 * to a valid resolved address whose family the host supports, addrlen the
 * correct length for addr->sa_family, loop a live loop owned by the calling
 * thread, and on_done non-null. All entry points and internal callbacks are
 * owner-thread; the dial adds no locks.
 *
 * Completion: on_done fires exactly once on the owner thread, as the dial's
 * final action -- no dial state is read or written after on_done returns, so
 * odin_dial_destroy(dial) is legal from inside on_done. ODIN_DIAL_OK carries
 * err == 0 and fd set to the connected socket; the caller's ownership of that fd
 * begins at that moment and the dial never closes it. ODIN_DIAL_ERROR carries
 * fd == -1 and err set to the failing connection errno, the socket the dial
 * created already closed; status is the authoritative signal. Even a connect(2)
 * that completes synchronously is reported on a later loop turn, never
 * re-entrantly from within odin_dial_start.
 */

#ifndef ODIN_DIAL_H_
#define ODIN_DIAL_H_

#include <sys/socket.h>

#include "odin/event_loop.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct odin_dial_t odin_dial_t;

typedef enum odin_dial_status_t {
  ODIN_DIAL_OK = 0,
  ODIN_DIAL_ERROR,
} odin_dial_status_t;

typedef void (*odin_dial_cb)(odin_dial_t *dial, odin_dial_status_t status,
                             int fd, int err, void *user_data);

/* Creates the nonblocking SOCK_STREAM socket, issues connect(2), registers the
 * in-flight attempt with the loop (a WRITE watch, or -- for an immediate
 * connect(2) error -- a 0-delay one-shot timer), writes *out, and returns 0. It
 * never invokes on_done itself. On a local setup failure it returns -1 with
 * errno set, writes nothing to *out, and leaves no socket open and no loop
 * registration: ENOMEM when the dial object cannot be allocated, or the errno
 * from socket(2) (e.g. EAFNOSUPPORT), from making the socket nonblocking, or
 * from odin_event_io_start / odin_event_timer_start. A connection failure is
 * never reported this way -- it always flows through on_done. Owner-thread API.
 */
int odin_dial_start(odin_event_loop_t *loop, const struct sockaddr *addr,
                    socklen_t addrlen, odin_dial_cb on_done, void *user_data,
                    odin_dial_t **out);

/* Stops any still-active loop registration (the WRITE watch or the
 * deferred-error timer), closes the socket only if the dial still owns it (an
 * in-flight or aborted attempt -- after ODIN_DIAL_OK the socket has passed to
 * the caller, after ODIN_DIAL_ERROR it is already closed), frees the dial
 * object, and never invokes on_done. odin_dial_destroy(NULL) is a no-op, and the
 * pointer is dead afterward. Callable from within on_done to reclaim a completed
 * dial, or on an in-flight dial to abort it.
 */
void odin_dial_destroy(odin_dial_t *dial);

#ifdef __cplusplus
}
#endif

#endif /* ODIN_DIAL_H_ */
