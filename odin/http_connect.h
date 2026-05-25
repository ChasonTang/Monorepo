/* odin/http_connect.h
 *
 * Pure byte-buffer parser for the HTTPS_PROXY CONNECT request.
 *
 * Accepted grammar (RFC-003 §4.2.1):
 *
 *   request      = request-line headers-block
 *   request-line = "CONNECT" SP request-target SP HTTP-version CRLF
 *   request-target = host ":" port
 *   host         = host-token / "[" v6-token "]"
 *   host-token   = 1*<any byte other than ':' / SP / CR / LF>
 *   v6-token     = 1*<any byte other than ']' / SP / CR / LF>
 *   port         = 1*5 DIGIT  ; value 0..65535
 *   HTTP-version = "HTTP/1.0" / "HTTP/1.1"
 *   headers-block = opaque bytes terminated by CRLF CRLF
 *   SP           = %x20       ; single space, no HTAB
 *   CRLF         = %x0D %x0A  ; no bare LF, no bare CR
 *
 * Hard caps:
 *   ODIN_HTTP_REQUEST_MAX = 8192  — total bytes through final CRLFCRLF
 *   ODIN_HTTP_HOST_MAX    = 255   — host length after IPv6 bracket strip
 */

#ifndef ODIN_HTTP_CONNECT_H_
#define ODIN_HTTP_CONNECT_H_

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "odin/protocol.h" /* ODIN_PROTO_HOST_MAX */

#ifdef __cplusplus
extern "C" {
#endif

#define ODIN_HTTP_HOST_MAX 255
#define ODIN_HTTP_REQUEST_MAX 8192

static_assert(ODIN_HTTP_HOST_MAX == ODIN_PROTO_HOST_MAX,
              "host caps must match");

typedef enum {
  ODIN_HTTP_OK = 0,
  ODIN_HTTP_NEED_MORE,
  ODIN_HTTP_ERR_REQUEST_TOO_LARGE,
  ODIN_HTTP_ERR_BAD_METHOD,
  ODIN_HTTP_ERR_BAD_REQUEST_TARGET,
  ODIN_HTTP_ERR_BAD_VERSION,
  ODIN_HTTP_ERR_HOST_LEN_INVALID,
  ODIN_HTTP_ERR_PORT_INVALID,
} odin_http_status_t;

typedef struct odin_http_connect_t {
  size_t host_off;
  size_t host_len;
  uint16_t port;
} odin_http_connect_t;

odin_http_status_t odin_http_parse_connect(const uint8_t *buf, size_t n,
                                           size_t *out_consumed,
                                           odin_http_connect_t *out);

/* Fixed CONNECT response lines the client writes back to the socket after
 * tunnel establishment succeeds or fails. Both strings include the trailing
 * CRLFCRLF and exclude the null terminator from the advertised length. */
extern const char kOdinHttpConnectEstablished[];
extern const size_t kOdinHttpConnectEstablishedLen;

extern const char kOdinHttpBadGateway[];
extern const size_t kOdinHttpBadGatewayLen;

#ifdef __cplusplus
}
#endif

#endif /* ODIN_HTTP_CONNECT_H_ */
