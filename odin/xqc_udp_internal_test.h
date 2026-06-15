/* odin/xqc_udp_internal_test.h */

#ifndef ODIN_XQC_UDP_INTERNAL_TEST_H_
#define ODIN_XQC_UDP_INTERNAL_TEST_H_

#if defined(ODIN_XQC_UDP_TESTING)

#include <stddef.h>
#include <sys/socket.h>

#include "odin/udp.h"
#include "odin/xqc_udp.h"
#include <xquic/xquic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct odin_xqc_udp_test_ops_t {
  xqc_engine_t *(*engine_create)(xqc_engine_type_t engine_type,
                                 const xqc_config_t *engine_config,
                                 const xqc_engine_ssl_config_t *ssl_config,
                                 const xqc_engine_callback_t *engine_callback,
                                 const xqc_transport_callbacks_t *transport_cbs,
                                 void *user_data);
  void (*engine_destroy)(xqc_engine_t *engine);
  xqc_int_t (*packet_process)(
      xqc_engine_t *engine, const unsigned char *packet_in_buf,
      size_t packet_in_size, const struct sockaddr *local_addr,
      socklen_t local_addrlen, const struct sockaddr *peer_addr,
      socklen_t peer_addrlen, xqc_usec_t recv_time, void *user_data);
  void (*finish_recv)(xqc_engine_t *engine);
  void (*main_logic)(xqc_engine_t *engine);
  xqc_int_t (*conn_continue_send)(xqc_engine_t *engine, const xqc_cid_t *cid);
  xqc_usec_t (*now_us)(void);
} odin_xqc_udp_test_ops_t;

void odin_xqc_udp_test_set_ops(const odin_xqc_udp_test_ops_t *ops);
int odin_xqc_udp_test_udp(odin_xqc_udp_t *xu, odin_udp_t **out);
int odin_xqc_udp_test_timer_active(odin_xqc_udp_t *xu);
int odin_xqc_udp_test_destroy_requested(odin_xqc_udp_t *xu);
int odin_xqc_udp_test_write_blocked(odin_xqc_udp_t *xu);
int odin_xqc_udp_test_last_timer_errno(odin_xqc_udp_t *xu);
xqc_timestamp_pt odin_xqc_udp_test_engine_monotonic_ts(odin_xqc_udp_t *xu);

#ifdef __cplusplus
}
#endif

#endif /* defined(ODIN_XQC_UDP_TESTING) */

#endif /* ODIN_XQC_UDP_INTERNAL_TEST_H_ */
