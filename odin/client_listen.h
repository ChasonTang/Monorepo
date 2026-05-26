/* odin/client_listen.h
 *
 * Odin Client TCP listener and single-connection CONNECT handshake
 * (RFC-009 §4.2). Two pure-C entry points:
 *
 *   int odin_client_listen_open(uint16_t port)
 *     Opens a blocking AF_INET / SOCK_STREAM socket, enables SO_REUSEADDR,
 *     binds it to 127.0.0.1:port via INADDR_LOOPBACK — 绑定地址固定 127.0.0.1
 *     per the requirement, with no flag, env var, or compile-time switch to
 *     bind any other address — and calls listen(fd, 1). Returns the listening
 *     fd on success or -1 with errno preserved on any of the four syscall
 *     failures (socket / setsockopt / bind / listen); any partial fd is
 *     closed before the return. port == 0 yields a kernel-picked ephemeral
 *     port readable via getsockname(2).
 *
 *   odin_client_listen_status_t odin_client_listen_handshake(int conn_fd)
 *     Consumes conn_fd (always closes it before returning); reads into a
 *     stack-resident uint8_t buf[ODIN_HTTP_REQUEST_MAX] accumulator in a
 *     read(2) loop calling odin_http_parse_connect after every successful
 *     read; on the parser's first terminal odin_http_status_t writes the
 *     slice from odin_http_response_for_status(s) via a retry-on-EINTR /
 *     short-write helper, then closes via shutdown(SHUT_WR) → read-and-drain
 *     to EOF → close — 写完 200 后让对端读完再关, applied uniformly so error
 *     responses are also delivered before the FIN.
 */

#ifndef ODIN_CLIENT_LISTEN_H_
#define ODIN_CLIENT_LISTEN_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int odin_client_listen_open(uint16_t port);

typedef enum odin_client_listen_status_t {
  ODIN_CLIENT_LISTEN_OK = 0,
  ODIN_CLIENT_LISTEN_REJECTED,
  ODIN_CLIENT_LISTEN_PEER_CLOSED,
  ODIN_CLIENT_LISTEN_IO_ERROR,
} odin_client_listen_status_t;

odin_client_listen_status_t odin_client_listen_handshake(int conn_fd);

#ifdef __cplusplus
}
#endif

#endif /* ODIN_CLIENT_LISTEN_H_ */
