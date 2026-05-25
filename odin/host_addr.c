/* odin/host_addr.c — `--server` host[:port] parser (RFC-007 §4.2.3). */

#include "odin/host_addr.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "odin/cli.h"
#include "odin/parse_util.h"
#include "odin/protocol.h"

odin_host_addr_status_t odin_host_addr_parse(const char *s,
                                             odin_host_addr_t *out) {
  if (s == NULL || s[0] == '\0') {
    return ODIN_HOST_ADDR_ERR_EMPTY;
  }
  const size_t n = strlen(s);
  odin_parse_util_hostport_t hp;
  const odin_parse_util_hostport_status_t r =
      odin_parse_util_split_hostport((const uint8_t *)s, n, &hp);
  if (r != ODIN_PARSE_UTIL_HOSTPORT_OK) {
    return ODIN_HOST_ADDR_ERR_BAD_TARGET;
  }
  if (hp.host_len > ODIN_PROTO_HOST_MAX) {
    return ODIN_HOST_ADDR_ERR_HOST_LEN_INVALID;
  }
  uint16_t port_value;
  if (hp.port_present) {
    const odin_parse_util_port_result_t pr =
        odin_parse_util_port((const uint8_t *)s + hp.port_off, hp.port_len);
    if (pr.status != ODIN_PARSE_UTIL_PORT_OK) {
      return ODIN_HOST_ADDR_ERR_PORT_INVALID;
    }
    port_value = pr.port;
  } else {
    port_value = (uint16_t)ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER;
  }
  out->host = s + hp.host_off;
  out->host_len = hp.host_len;
  out->port = port_value;
  return ODIN_HOST_ADDR_OK;
}
