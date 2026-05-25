/* odin/parse_util.c — shared byte helpers (RFC-007 §4.2.1, §4.2.2). */

#include "odin/parse_util.h"

#include <stddef.h>
#include <stdint.h>

odin_parse_util_port_result_t odin_parse_util_port(const uint8_t *bytes,
                                                   size_t n) {
  odin_parse_util_port_result_t r = {ODIN_PARSE_UTIL_PORT_BAD, 0};
  if (n == 0) {
    return r;
  }
  uint32_t acc = 0;
  for (size_t i = 0; i < n; ++i) {
    if (bytes[i] < '0' || bytes[i] > '9') {
      return r;
    }
    acc = acc * 10u + (uint32_t)(bytes[i] - '0');
    if (acc > 65535u) {
      return r;
    }
  }
  r.status = ODIN_PARSE_UTIL_PORT_OK;
  r.port = (uint16_t)acc;
  return r;
}

odin_parse_util_hostport_status_t
odin_parse_util_split_hostport(const uint8_t *bytes, size_t n,
                               odin_parse_util_hostport_t *out) {
  if (n == 0) {
    return ODIN_PARSE_UTIL_HOSTPORT_BAD;
  }
  if (bytes[0] == '[') {
    size_t rb = (size_t)-1;
    for (size_t i = 1; i < n; ++i) {
      if (bytes[i] == ']') {
        rb = i;
        break;
      }
    }
    if (rb == (size_t)-1 || rb == 1) {
      return ODIN_PARSE_UTIL_HOSTPORT_BAD;
    }
    if (rb + 1 == n) {
      out->host_off = 1;
      out->host_len = rb - 1;
      out->port_off = 0;
      out->port_len = 0;
      out->port_present = 0;
      return ODIN_PARSE_UTIL_HOSTPORT_OK;
    }
    if (bytes[rb + 1] != ':') {
      return ODIN_PARSE_UTIL_HOSTPORT_BAD;
    }
    out->host_off = 1;
    out->host_len = rb - 1;
    out->port_off = rb + 2;
    out->port_len = n - rb - 2;
    out->port_present = 1;
    return ODIN_PARSE_UTIL_HOSTPORT_OK;
  }
  size_t cl = (size_t)-1;
  for (size_t i = 0; i < n; ++i) {
    if (bytes[i] == ':') {
      cl = i;
      break;
    }
  }
  if (cl == (size_t)-1) {
    out->host_off = 0;
    out->host_len = n;
    out->port_off = 0;
    out->port_len = 0;
    out->port_present = 0;
    return ODIN_PARSE_UTIL_HOSTPORT_OK;
  }
  if (cl == 0) {
    return ODIN_PARSE_UTIL_HOSTPORT_BAD;
  }
  out->host_off = 0;
  out->host_len = cl;
  out->port_off = cl + 1;
  out->port_len = n - cl - 1;
  out->port_present = 1;
  return ODIN_PARSE_UTIL_HOSTPORT_OK;
}
