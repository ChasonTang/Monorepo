/* odin/accept_loop.h
 *
 * Event-loop-driven multi-connection TCP accept listener (RFC-019).
 *
 * Given a live odin_event_loop_t and a caller-owned, already-bind(2) +
 * listen(2)
 * + O_NONBLOCK listening fd of any address family the host supports (AF_INET,
 * AF_INET6, AF_UNIX), odin_accept_loop_create registers one level-triggered
 * ODIN_EVENT_READ watch on the listening fd through the RFC-010
 * odin_event_io_* API. On each readiness, the loop drains pending connections
 * by calling accept4(SOCK_NONBLOCK) on Linux or accept(2) + fcntl(F_SETFL,
 * O_NONBLOCK) on macOS up to ODIN_ACCEPT_LOOP_BATCH_MAX = 64 per readiness, and
 * invokes on_accept(al, conn_fd, user_data) once per accepted nonblocking
 * connection -- caller ownership of conn_fd begins at the callback's entry.
 *
 * Errno classification:
 *   EAGAIN / EWOULDBLOCK    end the drain quietly (level-triggered re-fires).
 *   EINTR                   retry the next iteration's accept(2).
 *   ECONNABORTED            skip this connection and continue the drain.
 *   EMFILE / ENFILE /       soft-degrade: stop the READ watch and arm a
 *   ENOBUFS / ENOMEM        0-delay one-shot timer that re-arms the watch on
 *                           the next loop pass (so the level-triggered EMFILE
 *                           storm cannot hot-spin the CPU and still-queued
 *                           connections survive the pause). The listening fd is
 *                           never closed during soft degradation.
 *   any other errno         stop the watch and any pending timer, then fire
 *                           on_error(al, err, user_data) exactly once; the loop
 *                           is terminal afterwards (no further callbacks fire).
 *
 * Ownership: the caller owns listen_fd at all times; the module never closes
 * it. The caller owns each conn_fd from the moment on_accept is invoked; the
 * module never touches it again. The odin_accept_loop_t is caller-owned.
 *
 * Lifecycle: odin_accept_loop_destroy stops the READ watch and any pending
 * soft-degradation timer, frees the handle, and never closes the listening fd.
 * It is safe to call from inside on_accept or on_error -- the drain loop and
 * the terminal-error path capture the callback target into locals before each
 * invocation and check a deferred-destroy flag on return, so the physical free
 * runs after the in-flight callback unwinds.
 * odin_accept_loop_destroy(NULL) is a no-op.
 *
 * Constructor failures (ENOMEM allocating the handle, or any errno from
 * odin_event_io_start) return -1 with errno set, leave *out untouched, and
 * register no watch -- they never flow through on_error.
 *
 * Threading: all entry points and internal callbacks run on the loop's owner
 * thread; the module adds no locks.
 */

#ifndef ODIN_ACCEPT_LOOP_H_
#define ODIN_ACCEPT_LOOP_H_

#include "odin/event_loop.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ODIN_ACCEPT_LOOP_BATCH_MAX 64u

typedef struct odin_accept_loop_t odin_accept_loop_t;

typedef void (*odin_accept_loop_accept_cb)(odin_accept_loop_t *al, int conn_fd,
                                           void *user_data);

typedef void (*odin_accept_loop_error_cb)(odin_accept_loop_t *al, int err,
                                          void *user_data);

int odin_accept_loop_create(odin_event_loop_t *loop, int listen_fd,
                            odin_accept_loop_accept_cb on_accept,
                            odin_accept_loop_error_cb on_error, void *user_data,
                            odin_accept_loop_t **out);

void odin_accept_loop_destroy(odin_accept_loop_t *al);

#ifdef __cplusplus
}
#endif

#endif /* ODIN_ACCEPT_LOOP_H_ */
