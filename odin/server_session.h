/* odin/server_session.h
 *
 * Per-connection orchestrator session (RFC-020).
 *
 * Given a live odin_event_loop_t and a caller-owned nonblocking accepted
 * stream socket conn_fd, the orchestrator wires together the RFC-013 fd
 * transport, the RFC-018 SERVER-mode connect session, the RFC-012 dial, and
 * the RFC-014 relay so the connection runs end-to-end: one trampoline is
 * installed as the downstream (and later upstream) transports' on_ready and
 * dispatches to odin_connect_session_drive while the session is alive and to
 * odin_relay_ready afterwards. On on_req_decoded the orchestrator parses the
 * host slice as an IPv4 literal, consults a caller-installable address
 * filter, and issues odin_dial_start; on dial OK it builds the upstream
 * transport, flushes the session's server_tail synchronously through one
 * odin_transport_write before starting the relay, then runs the relay until
 * both directions half-close; on any phase fault it aggregates into one
 * on_close(ss, err).
 *
 * Ownership: loop, on_close, and out are non-null preconditions. conn_fd is a
 * caller-owned, nonblocking, connected stream socket. On create success the
 * orchestrator takes ownership of conn_fd and closes it exactly once during
 * teardown. On create failure (return -1 with errno set) the caller-supplied
 * conn_fd is NOT closed -- the caller still owns it. user_data is opaque to
 * the orchestrator and is passed verbatim to on_close. Owner-thread API.
 *
 * Completion: after create success, on_close fires at most once on the
 * owner thread, as the orchestrator's final action. err == 0 only on the
 * relay-OK happy path. odin_server_session_destroy(ss) from inside on_close
 * is legal (deferred-free). odin_server_session_destroy(ss) called outside
 * any callback aborts the orchestrator at any phase, tears down every owned
 * object, closes every owned fd, and never invokes on_close.
 * odin_server_session_destroy(NULL) is a no-op.
 *
 * Address filter: odin_server_session_set_dial_filter installs the per-session
 * SSRF mitigation hook the orchestrator consults after inet_pton(AF_INET, ...)
 * succeeds and before odin_dial_start runs. A 0 return permits the dial; a
 * nonzero return synthesizes a dial failure with that errno -- no upstream
 * connect(2) is issued. The default filter is NULL (allow every parsed
 * address); the upstream caller is responsible for installing a deny-list
 * policy appropriate for the deployment. The setter is owner-thread and
 * replace-only; calling with cb == NULL clears any previously installed
 * filter. set_dial_filter is a no-op when ss == NULL.
 */

#ifndef ODIN_SERVER_SESSION_H_
#define ODIN_SERVER_SESSION_H_

#include <sys/socket.h>

#include "odin/event_loop.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct odin_server_session_t odin_server_session_t;

#define ODIN_SERVER_SESSION_RESP_CODE_OK 0x0000u
#define ODIN_SERVER_SESSION_RESP_CODE_ECONNREFUSED 0x0001u
#define ODIN_SERVER_SESSION_RESP_CODE_EHOSTUNREACH 0x0002u
#define ODIN_SERVER_SESSION_RESP_CODE_ETIMEDOUT 0x0003u
#define ODIN_SERVER_SESSION_RESP_CODE_OTHER 0x0004u

typedef void (*odin_server_session_close_cb)(odin_server_session_t *ss, int err,
                                             void *user_data);

typedef int (*odin_server_session_dial_filter_cb)(const struct sockaddr *addr,
                                                  socklen_t addrlen,
                                                  void *user_data);

int odin_server_session_create(odin_event_loop_t *loop, int conn_fd,
                               odin_server_session_close_cb on_close,
                               void *user_data, odin_server_session_t **out);

void odin_server_session_set_dial_filter(odin_server_session_t *ss,
                                         odin_server_session_dial_filter_cb cb,
                                         void *user_data);

void odin_server_session_destroy(odin_server_session_t *ss);

#ifdef __cplusplus
}
#endif

#endif /* ODIN_SERVER_SESSION_H_ */
