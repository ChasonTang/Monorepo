/* odin/connect_session.h
 *
 * Transport-agnostic CONNECT-handshake session (RFC-018).
 *
 * The session performs one round of the RFC-001 control-frame handshake on a
 * caller-supplied odin_transport_t (RFC-013) by issuing every read and write
 * through the odin_transport_* dispatchers. It binds no
 * odin_transport_ready_cb, registers no event-loop watch, never calls
 * odin_transport_set_interest, and never owns or destroys the transport: the
 * orchestrator owns the transport, owns the watch, installs its own permanent
 * on_ready body at transport construction (a trampoline that dispatches to
 * odin_connect_session_drive while the session is alive and to
 * odin_relay_ready after the session is destroyed), and calls
 * odin_connect_session_drive(s, t, events) from inside that body each time a
 * readiness arrives.
 *
 * Lifecycle:
 *   Client mode: odin_connect_session_create_client encodes one CONNECT_REQ
 *   from (host, host_len, port); odin_connect_session_drive writes it through
 *   odin_transport_write and then reads through odin_transport_read until one
 *   CONNECT_RESP decodes; on_done(OK, 0) fires with the decoded error_code
 *   retrievable via odin_connect_session_client_error_code(s) and the trailing
 *   post-RESP byte slice (up to 256 bytes) retrievable as an aliased
 *   (ptr, len) view via odin_connect_session_client_tail(s, ...).
 *
 *   Server mode: odin_connect_session_create_server is empty; the first
 *   odin_connect_session_drive reads through odin_transport_read until one
 *   CONNECT_REQ decodes; on_req_decoded fires with host/port exposed via
 *   odin_connect_session_server_host(s, ...) and
 * odin_connect_session_server_port(s) and the trailing post-REQ byte slice (up
 * to 254 bytes for decodable REQs) exposed via
 * odin_connect_session_server_tail(s, ...); the session suspends with
 * odin_connect_session_wants(s) == 0 until the caller invokes
 *   odin_connect_session_server_set_error_code(s, error_code), then writes one
 *   CONNECT_RESP through odin_transport_write and fires on_done(OK, 0).
 *
 *   Faults aggregate into a single on_done(ERROR, err): a synchronous
 *   ODIN_TRANSPORT_IO_ERROR propagates the transport's errno; an
 *   ODIN_TRANSPORT_EOF during a still-decoding read maps to err = ECONNRESET;
 *   an ODIN_TRANSPORT_ERROR readiness with no synchronous fault is classified
 *   via odin_transport_error (0 is benign and re-arms; non-zero becomes err);
 *   a decoder ODIN_PROTO_ERR_* maps to err = EPROTO.
 *
 * Threading: all entry points and both callbacks run on the orchestrator's
 * thread; the session adds no locks.
 */

#ifndef ODIN_CONNECT_SESSION_H_
#define ODIN_CONNECT_SESSION_H_

#include <stddef.h>
#include <stdint.h>

#include "odin/protocol.h"
#include "odin/transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct odin_connect_session_t odin_connect_session_t;

typedef enum odin_connect_session_status_t {
  ODIN_CONNECT_SESSION_OK = 0,
  ODIN_CONNECT_SESSION_ERROR,
} odin_connect_session_status_t;

typedef enum odin_connect_session_drive_t {
  ODIN_CONNECT_SESSION_DRIVE_CONTINUE = 0,
  ODIN_CONNECT_SESSION_DRIVE_DONE,
} odin_connect_session_drive_t;

typedef void (*odin_connect_session_done_cb)(
    odin_connect_session_t *s, odin_connect_session_status_t status, int err,
    void *user_data);

typedef void (*odin_connect_session_req_decoded_cb)(odin_connect_session_t *s,
                                                    void *user_data);

int odin_connect_session_create_client(const char *host, size_t host_len,
                                       uint16_t port,
                                       odin_connect_session_done_cb on_done,
                                       void *user_data,
                                       odin_connect_session_t **out);

int odin_connect_session_create_server(
    odin_connect_session_req_decoded_cb on_req_decoded,
    odin_connect_session_done_cb on_done, void *user_data,
    odin_connect_session_t **out);

odin_connect_session_drive_t
odin_connect_session_drive(odin_connect_session_t *s, odin_transport_t *t,
                           unsigned int events);

unsigned int odin_connect_session_wants(const odin_connect_session_t *s);

void odin_connect_session_server_set_error_code(odin_connect_session_t *s,
                                                uint16_t error_code);

void odin_connect_session_server_host(const odin_connect_session_t *s,
                                      const char **out_host,
                                      size_t *out_host_len);
uint16_t odin_connect_session_server_port(const odin_connect_session_t *s);
void odin_connect_session_server_tail(const odin_connect_session_t *s,
                                      const uint8_t **out_ptr, size_t *out_len);

uint16_t
odin_connect_session_client_error_code(const odin_connect_session_t *s);
void odin_connect_session_client_tail(const odin_connect_session_t *s,
                                      const uint8_t **out_ptr, size_t *out_len);

void odin_connect_session_destroy(odin_connect_session_t *s);

#ifdef __cplusplus
}
#endif

#endif /* ODIN_CONNECT_SESSION_H_ */
