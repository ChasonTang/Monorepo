/* odin/host_addr.h
 *
 * `--server` host[:port] parser. Wraps the shared `odin_parse_util_*`
 * helpers and adds the optional-`:port` arm plus the host-length cap.
 *
 * Accepted shapes (one of):
 *   host           reg-name or IPv4 literal, no port
 *   host:port      reg-name or IPv4 literal with port
 *   [v6]           bracketed IPv6 literal, no port
 *   [v6]:port      bracketed IPv6 literal with port
 *
 * On OK, `out->host` aliases `s` (offset 0 for bare host / IPv4,
 * offset 1 for bracketed v6 — the bracket bytes are stripped from
 * the reported slice but `s` itself is never mutated);
 * `out->host_len` is in `[1, ODIN_PROTO_HOST_MAX]`;
 * `out->port` is the parsed digits, or
 * `ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER` (4433) when `:port` is absent.
 *
 * Error mapping:
 *   s == NULL or empty       -> ERR_EMPTY
 *   structural malformation  -> ERR_BAD_TARGET
 *   bad port digits          -> ERR_PORT_INVALID
 *   host_len > host cap      -> ERR_HOST_LEN_INVALID
 *
 * `*out` is left unmodified on every non-OK return. Pure: no I/O,
 * no allocation, no global state. Reads `[0, strlen(s))` and never
 * mutates `s`.
 */

#ifndef ODIN_HOST_ADDR_H_
#define ODIN_HOST_ADDR_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  ODIN_HOST_ADDR_OK = 0,
  ODIN_HOST_ADDR_ERR_EMPTY,
  ODIN_HOST_ADDR_ERR_BAD_TARGET,
  ODIN_HOST_ADDR_ERR_PORT_INVALID,
  ODIN_HOST_ADDR_ERR_HOST_LEN_INVALID,
} odin_host_addr_status_t;

typedef struct {
  const char *host;
  size_t host_len;
  uint16_t port;
} odin_host_addr_t;

odin_host_addr_status_t odin_host_addr_parse(const char *s,
                                             odin_host_addr_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ODIN_HOST_ADDR_H_ */
