/* odin/client_xqc_runtime.h */

#ifndef ODIN_CLIENT_XQC_RUNTIME_H_
#define ODIN_CLIENT_XQC_RUNTIME_H_

#include <sys/socket.h>

#include "odin/event_loop.h"
#include "odin/xqc_udp.h"
#include <xquic/xquic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ODIN_XQC_CLIENT_ALPN "odin/1"

typedef struct odin_xqc_client_runtime_t odin_xqc_client_runtime_t;

typedef struct odin_xqc_client_runtime_config_t {
  odin_event_loop_t *loop;
  const struct sockaddr *local_addr;
  socklen_t local_addrlen;
  const struct sockaddr *peer_addr;
  socklen_t peer_addrlen;
  const char *server_host;
  const xqc_config_t *engine_config;
  const xqc_engine_ssl_config_t *engine_ssl_config;
  const xqc_engine_callback_t *engine_callbacks;
  const xqc_transport_callbacks_t *transport_callbacks;
  const xqc_conn_settings_t *conn_settings;
  const xqc_conn_ssl_config_t *conn_ssl_config;
  const unsigned char *token;
  unsigned int token_len;
  int no_crypto_flag;
} odin_xqc_client_runtime_config_t;

int odin_xqc_client_runtime_create(
    const odin_xqc_client_runtime_config_t *config,
    odin_xqc_client_runtime_t **out);
int odin_xqc_client_runtime_start(odin_xqc_client_runtime_t *rt);
int odin_xqc_client_runtime_stop(odin_xqc_client_runtime_t *rt);
int odin_xqc_client_runtime_add_connection(odin_xqc_client_runtime_t *rt,
                                           int conn_fd);
void odin_xqc_client_runtime_destroy(odin_xqc_client_runtime_t *rt);

#ifdef __cplusplus
}
#endif

#endif /* ODIN_CLIENT_XQC_RUNTIME_H_ */
