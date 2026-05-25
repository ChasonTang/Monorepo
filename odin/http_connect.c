/* odin/http_connect.c — HTTPS_PROXY CONNECT request parser (RFC-003). */

#include "odin/http_connect.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

const char kOdinHttpConnectEstablished[] =
    "HTTP/1.1 200 Connection Established\r\n\r\n";
const size_t kOdinHttpConnectEstablishedLen =
    sizeof(kOdinHttpConnectEstablished) - 1;

const char kOdinHttpBadGateway[] = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
const size_t kOdinHttpBadGatewayLen = sizeof(kOdinHttpBadGateway) - 1;

/* Returns the index of the '\r' in the first CRLF in buf[start..end), or
 * SIZE_MAX if none. */
static size_t find_crlf(const uint8_t *buf, size_t start, size_t end) {
  for (size_t i = start; i + 1 < end; ++i) {
    if (buf[i] == '\r' && buf[i + 1] == '\n') {
      return i;
    }
  }
  return SIZE_MAX;
}

/* Returns one past the '\n' of the first CRLFCRLF in buf[start..end), or
 * SIZE_MAX if none. */
static size_t find_double_crlf(const uint8_t *buf, size_t start, size_t end) {
  for (size_t i = start; i + 3 < end; ++i) {
    if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
        buf[i + 3] == '\n') {
      return i + 4;
    }
  }
  return SIZE_MAX;
}

/* Returns the index of the first occurrence of byte b in buf[start..end), or
 * SIZE_MAX if none. */
static size_t find_byte(const uint8_t *buf, size_t start, size_t end,
                        uint8_t b) {
  for (size_t i = start; i < end; ++i) {
    if (buf[i] == b) {
      return i;
    }
  }
  return SIZE_MAX;
}

odin_http_status_t odin_http_parse_connect(const uint8_t *buf, size_t n,
                                           size_t *out_consumed,
                                           odin_http_connect_t *out) {
  assert(buf != NULL);
  assert(out_consumed != NULL);
  assert(out != NULL);

  /* Method fast-fail: "CONNECT " is 8 bytes. */
  if (n >= 8) {
    if (memcmp(buf, "CONNECT ", 8) != 0) {
      return ODIN_HTTP_ERR_BAD_METHOD;
    }
  } else {
    /* Partial prefix — still a valid prefix of "CONNECT "? */
    if (memcmp(buf, "CONNECT ", n) != 0) {
      return ODIN_HTTP_ERR_BAD_METHOD;
    }
    return ODIN_HTTP_NEED_MORE;
  }

  /* Locate the request-line CRLF in buf[8..n). */
  size_t rl_end = find_crlf(buf, 8, n);
  if (rl_end == SIZE_MAX) {
    return (n >= ODIN_HTTP_REQUEST_MAX) ? ODIN_HTTP_ERR_REQUEST_TOO_LARGE
                                        : ODIN_HTTP_NEED_MORE;
  }

  /* Locate the single SP that separates request-target from HTTP-version. */
  size_t sp = find_byte(buf, 8, rl_end, 0x20);
  if (sp == SIZE_MAX) {
    return ODIN_HTTP_ERR_BAD_VERSION;
  }

  /* Validate HTTP-version: buf[sp+1..rl_end) must be "HTTP/1.0" or
   * "HTTP/1.1" (exactly 8 bytes). */
  if (rl_end - (sp + 1) != 8 || (memcmp(buf + sp + 1, "HTTP/1.0", 8) != 0 &&
                                 memcmp(buf + sp + 1, "HTTP/1.1", 8) != 0)) {
    return ODIN_HTTP_ERR_BAD_VERSION;
  }

  /* Parse request-target = buf[8..sp). */
  size_t target_start = 8;
  size_t target_end = sp;

  size_t host_off, host_len, port_start;

  if (buf[target_start] == '[') {
    /* IPv6 bracketed form: "[v6-token]:port" */
    size_t rb = find_byte(buf, target_start + 1, target_end, ']');
    if (rb == SIZE_MAX || rb + 1 >= target_end || buf[rb + 1] != ':') {
      return ODIN_HTTP_ERR_BAD_REQUEST_TARGET;
    }
    host_off = target_start + 1;
    host_len = rb - (target_start + 1);
    port_start = rb + 2;
  } else {
    /* reg-name / IPv4-literal form: "host-token:port" */
    size_t cl = find_byte(buf, target_start, target_end, ':');
    if (cl == SIZE_MAX) {
      return ODIN_HTTP_ERR_BAD_REQUEST_TARGET;
    }
    host_off = target_start;
    host_len = cl - target_start;
    port_start = cl + 1;
  }

  /* Validate port: 1-to-5 digits, value <= 65535. */
  size_t port_len = target_end - port_start;
  if (port_len < 1 || port_len > 5) {
    return ODIN_HTTP_ERR_PORT_INVALID;
  }
  uint32_t port_value = 0;
  for (size_t i = port_start; i < target_end; ++i) {
    if (buf[i] < '0' || buf[i] > '9') {
      return ODIN_HTTP_ERR_PORT_INVALID;
    }
    port_value = port_value * 10 + (uint32_t)(buf[i] - '0');
  }
  if (port_value > 65535) {
    return ODIN_HTTP_ERR_PORT_INVALID;
  }

  /* Validate host length. */
  if (host_len < 1 || host_len > ODIN_HTTP_HOST_MAX) {
    return ODIN_HTTP_ERR_HOST_LEN_INVALID;
  }

  /* Locate the terminating CRLFCRLF starting from rl_end. */
  size_t end = find_double_crlf(buf, rl_end, n);
  if (end == SIZE_MAX) {
    return (n >= ODIN_HTTP_REQUEST_MAX) ? ODIN_HTTP_ERR_REQUEST_TOO_LARGE
                                        : ODIN_HTTP_NEED_MORE;
  }
  if (end > ODIN_HTTP_REQUEST_MAX) {
    return ODIN_HTTP_ERR_REQUEST_TOO_LARGE;
  }

  *out_consumed = end;
  out->host_off = host_off;
  out->host_len = host_len;
  out->port = (uint16_t)port_value;
  return ODIN_HTTP_OK;
}
