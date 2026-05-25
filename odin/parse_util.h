/* odin/parse_util.h
 *
 * Shared byte-buffer helpers for parsing `host[:port]`-style authorities.
 * Two pure helpers, no I/O, no allocation, no global state:
 *
 *   odin_parse_util_port  — strict ASCII-decimal port validator that
 *     accepts `[0-9]{1,5}` with value `<= 65535`. Leading zeros are
 *     accepted; empty input or any non-digit byte returns BAD with
 *     `port == 0`. The in-loop `> 65535` check fires before the
 *     `uint32_t -> uint16_t` narrowing so values like 99999 reject
 *     without wrapping below the threshold.
 *
 *   odin_parse_util_split_hostport — structural split of `host[:port]`
 *     or `[v6][:port]`. Caller validates port digits, so `"a:b:443"`
 *     splits at the first `:` and trailing-colon-without-digits
 *     (`"a:"`, `"[v6]:"`) splits OK with `port_present == 1` and
 *     `port_len == 0`. BAD for `n == 0`, zero-length host, unbalanced
 *     bracket, or non-`:` byte after `]`. On OK, `host_off in {0, 1}`
 *     (1 post-`[`); `port_off`/`port_len` apply only when
 *     `port_present != 0`. `*out` unmodified on BAD.
 *
 * Both helpers bound-scan over `[0, n)`; neither reads beyond `n`.
 */

#ifndef ODIN_PARSE_UTIL_H_
#define ODIN_PARSE_UTIL_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  ODIN_PARSE_UTIL_PORT_OK = 0,
  ODIN_PARSE_UTIL_PORT_BAD,
} odin_parse_util_port_status_t;

typedef struct {
  odin_parse_util_port_status_t status;
  uint16_t port;
} odin_parse_util_port_result_t;

odin_parse_util_port_result_t odin_parse_util_port(const uint8_t *bytes,
                                                   size_t n);

typedef struct {
  size_t host_off;
  size_t host_len;
  size_t port_off;
  size_t port_len;
  uint8_t port_present;
} odin_parse_util_hostport_t;

typedef enum {
  ODIN_PARSE_UTIL_HOSTPORT_OK = 0,
  ODIN_PARSE_UTIL_HOSTPORT_BAD,
} odin_parse_util_hostport_status_t;

odin_parse_util_hostport_status_t
odin_parse_util_split_hostport(const uint8_t *bytes, size_t n,
                               odin_parse_util_hostport_t *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ODIN_PARSE_UTIL_H_ */
