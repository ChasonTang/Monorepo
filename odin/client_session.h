/* odin/client_session.h
 *
 * Client-side per-connection orchestrator session (RFC-023).
 */

#ifndef ODIN_CLIENT_SESSION_H_
#define ODIN_CLIENT_SESSION_H_

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "odin/event_loop.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct odin_client_session_t odin_client_session_t;

typedef void (*odin_client_session_close_cb)(odin_client_session_t *cs, int err,
                                             void *user_data);

typedef int (*odin_client_session_dial_filter_cb)(const struct sockaddr *addr,
                                                  socklen_t addrlen,
                                                  void *user_data);

int odin_client_session_create(odin_event_loop_t *loop, int conn_fd,
                               const char *server_host, size_t server_host_len,
                               uint16_t server_port,
                               odin_client_session_close_cb on_close,
                               void *user_data, odin_client_session_t **out);

void odin_client_session_set_dial_filter(odin_client_session_t *cs,
                                         odin_client_session_dial_filter_cb cb,
                                         void *user_data);

void odin_client_session_destroy(odin_client_session_t *cs);

#ifdef __cplusplus
}
#endif

#endif /* ODIN_CLIENT_SESSION_H_ */
