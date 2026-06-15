/* odin/xqc_udp.h
 *
 * xquic UDP event driver (RFC-017).
 *
 * Owns one xqc_engine_t, one odin_udp_t bound to config->local_addr, and one
 * one-shot odin_event_timer_t. Maps UDP receive batches into
 * xqc_engine_packet_process plus xqc_engine_finish_recv, maps xquic engine
 * timer requests to an Odin timer, and maps xquic packet-send callbacks
 * (stateless_reset, write_socket, write_socket_ex, conn_send_packet_before_
 * accept) to odin_udp_send. UDP WRITE-readiness recovery calls
 * xqc_conn_continue_send over caller-registered CIDs after a recoverable
 * write_socket{,_ex} EAGAIN; pre-accept and stateless-reset backpressure are
 * best-effort (no WRITE recovery, no continue-send retry). All APIs are
 * owner-thread APIs under the RFC-010 event-loop contract.
 *
 * Callers register CIDs they want to recover with via odin_xqc_udp_register_
 * conn after xqc_connect or server accept, and unregister with odin_xqc_udp_
 * unregister_conn in connection close. Callers MUST pass
 * odin_xqc_udp_xqc_user_data(xu) as the transport user data for xquic
 * connections that use this driver (via xqc_connect or
 * xqc_conn_set_transport_user_data). odin_xqc_udp_app_user_data(xu) returns
 * the application context configured at create time.
 *
 * odin_xqc_udp_destroy never closes or drains xquic connections: callers must
 * close or otherwise make every connection unavailable and unregister every
 * CID before destroying. Outside any driver-entered xquic callback, destroy
 * also requires no caller-owned direct xquic API call or xquic callback
 * reached through one to be on the stack. Destroy requested from inside a
 * driver-entered xquic callback (packet-process, finish-recv, timer
 * main-logic, continue-send) is deferred until the outermost such call
 * returns.
 */

#ifndef ODIN_XQC_UDP_H_
#define ODIN_XQC_UDP_H_

#include <sys/socket.h>

#include "odin/event_loop.h"
#include "odin/udp.h"
#include <xquic/xquic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ODIN_XQC_UDP_PACKET_CAP 65535u
#define ODIN_XQC_UDP_RECV_BATCH_MAX 64u

typedef struct odin_xqc_udp_t odin_xqc_udp_t;

typedef struct odin_xqc_udp_config_t {
  odin_event_loop_t *loop;
  const struct sockaddr *local_addr;
  socklen_t local_addrlen;
  xqc_engine_type_t engine_type;
  const xqc_config_t *engine_config;
  const xqc_engine_ssl_config_t *ssl_config;
  const xqc_engine_callback_t *engine_callbacks;
  const xqc_transport_callbacks_t *transport_callbacks;
  void *app_user_data;
} odin_xqc_udp_config_t;

int odin_xqc_udp_create(const odin_xqc_udp_config_t *config,
                        odin_xqc_udp_t **out);
int odin_xqc_udp_start(odin_xqc_udp_t *xu);
int odin_xqc_udp_stop(odin_xqc_udp_t *xu);
void odin_xqc_udp_destroy(odin_xqc_udp_t *xu);

xqc_engine_t *odin_xqc_udp_engine(odin_xqc_udp_t *xu);
void *odin_xqc_udp_xqc_user_data(odin_xqc_udp_t *xu);
void *odin_xqc_udp_app_user_data(odin_xqc_udp_t *xu);
int odin_xqc_udp_register_conn(odin_xqc_udp_t *xu, const xqc_cid_t *cid);
void odin_xqc_udp_unregister_conn(odin_xqc_udp_t *xu, const xqc_cid_t *cid);

#ifdef __cplusplus
}
#endif

#endif /* ODIN_XQC_UDP_H_ */
