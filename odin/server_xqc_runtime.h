/* odin/server_xqc_runtime.h */

#ifndef ODIN_SERVER_XQC_RUNTIME_H_
#define ODIN_SERVER_XQC_RUNTIME_H_

#include <sys/socket.h>

#include "odin/event_loop.h"
#include "odin/server_session.h"
#include "odin/xqc_udp.h"
#include <xquic/xquic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ODIN_XQC_SERVER_ALPN "odin/1"

typedef struct odin_xqc_server_runtime_t odin_xqc_server_runtime_t;

typedef struct odin_xqc_server_runtime_config_t {
  odin_event_loop_t *loop;
  const struct sockaddr *local_addr;
  socklen_t local_addrlen;
  const xqc_config_t *engine_config;
  const xqc_engine_ssl_config_t *ssl_config;
  const xqc_engine_callback_t *engine_callbacks;
} odin_xqc_server_runtime_config_t;

int odin_xqc_server_runtime_create(
    const odin_xqc_server_runtime_config_t *config,
    odin_xqc_server_runtime_t **out);
int odin_xqc_server_runtime_start(odin_xqc_server_runtime_t *rt);
int odin_xqc_server_runtime_stop(odin_xqc_server_runtime_t *rt);
void odin_xqc_server_runtime_set_dial_filter(
    odin_xqc_server_runtime_t *rt, odin_server_session_dial_filter_cb cb,
    void *user_data);
void odin_xqc_server_runtime_destroy(odin_xqc_server_runtime_t *rt);

#ifdef __cplusplus
}
#endif

#endif /* ODIN_SERVER_XQC_RUNTIME_H_ */
