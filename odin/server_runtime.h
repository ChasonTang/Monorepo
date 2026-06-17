/* odin/server_runtime.h
 *
 * Per-listener server runtime (RFC-021).
 *
 * Given a live odin_event_loop_t and a caller-owned, already-bind(2) +
 * listen(2) + O_NONBLOCK listening fd, odin_server_runtime_create registers
 * an internal RFC-019 accept loop over listen_fd and creates one RFC-020
 * server session for every accepted connection. The runtime tracks in-flight
 * sessions until their internal on_close fires, then removes and destroys the
 * matching session. The runtime never closes listen_fd on success, failure,
 * terminal error, or destroy.
 *
 * Constructor failures return -1 with errno set, leave *out untouched, roll
 * back partial state, and never invoke on_runtime_error. A later accept-loop
 * terminal error invokes on_runtime_error(rt, err, user_data) at most once
 * with err verbatim; in-flight sessions keep running and listen_fd remains
 * caller-owned. odin_server_runtime_destroy(NULL) is a no-op. Destroy is safe
 * outside runtime callbacks, from inside on_runtime_error via deferred
 * teardown, and from inside a propagated dial_filter through RFC-020's
 * per-session deferred destroy chain.
 *
 * odin_server_runtime_set_dial_filter is owner-thread, replace-only, and a
 * no-op for rt == NULL. A later call overwrites the stored callback/user_data
 * pair; cb == NULL clears the slot. The stored pair is propagated to each
 * future session at creation time and does not affect already in-flight
 * sessions. If a per-connection server-session create fails, the runtime
 * silently closes that accepted conn_fd, adds no in-flight entry, invokes no
 * caller callback, and keeps accepting future connections.
 */

#ifndef ODIN_SERVER_RUNTIME_H_
#define ODIN_SERVER_RUNTIME_H_

#include "odin/event_loop.h"
#include "odin/server_session.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct odin_server_runtime_t odin_server_runtime_t;

typedef void (*odin_server_runtime_error_cb)(odin_server_runtime_t *rt, int err,
                                             void *user_data);

int odin_server_runtime_create(odin_event_loop_t *loop, int listen_fd,
                               odin_server_runtime_error_cb on_runtime_error,
                               void *user_data, odin_server_runtime_t **out);

void odin_server_runtime_set_dial_filter(odin_server_runtime_t *rt,
                                         odin_server_session_dial_filter_cb cb,
                                         void *user_data);

void odin_server_runtime_destroy(odin_server_runtime_t *rt);

#ifdef __cplusplus
}
#endif

#endif /* ODIN_SERVER_RUNTIME_H_ */
