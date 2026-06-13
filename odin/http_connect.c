/* odin/http_connect.c — HTTPS_PROXY CONNECT request parser (RFC-003). */

#include "odin/http_connect.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "odin/parse_util.h"

static const char kRespOk[] = "HTTP/1.1 200 Connection Established\r\n\r\n";
static const char kResp400[] = "HTTP/1.1 400 Bad Request\r\n\r\n";
static const char kResp405[] =
    "HTTP/1.1 405 Method Not Allowed\r\nAllow: CONNECT\r\n\r\n";
static const char kResp414[] = "HTTP/1.1 414 URI Too Long\r\n\r\n";
static const char kResp505[] =
    "HTTP/1.1 505 HTTP Version Not Supported\r\n\r\n";

odin_http_response_t odin_http_response_for_status(odin_http_status_t status) {
  switch (status) {
  case ODIN_HTTP_OK:
    return (odin_http_response_t){kRespOk, sizeof(kRespOk) - 1};
  case ODIN_HTTP_ERR_BAD_METHOD:
    return (odin_http_response_t){kResp405, sizeof(kResp405) - 1};
  case ODIN_HTTP_ERR_BAD_REQUEST_TARGET:
  case ODIN_HTTP_ERR_HOST_LEN_INVALID:
  case ODIN_HTTP_ERR_PORT_INVALID:
    return (odin_http_response_t){kResp400, sizeof(kResp400) - 1};
  case ODIN_HTTP_ERR_BAD_VERSION:
    return (odin_http_response_t){kResp505, sizeof(kResp505) - 1};
  case ODIN_HTTP_ERR_REQUEST_TOO_LARGE:
    return (odin_http_response_t){kResp414, sizeof(kResp414) - 1};
  case ODIN_HTTP_NEED_MORE:
  default:
    assert(0 &&
           "odin_http_response_for_status: non-terminal or unknown status");
    return (odin_http_response_t){kResp400, sizeof(kResp400) - 1};
  }
}

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

  odin_parse_util_hostport_t hp;
  const odin_parse_util_hostport_status_t hp_status =
      odin_parse_util_split_hostport(buf + target_start,
                                     target_end - target_start, &hp);
  if (hp_status != ODIN_PARSE_UTIL_HOSTPORT_OK || hp.port_present == 0) {
    return ODIN_HTTP_ERR_BAD_REQUEST_TARGET;
  }
  const size_t host_off = target_start + hp.host_off;
  const size_t host_len = hp.host_len;
  const size_t port_start = target_start + hp.port_off;

  /* Validate port: 1-to-5 digits, value <= 65535. */
  const odin_parse_util_port_result_t pr =
      odin_parse_util_port(buf + port_start, hp.port_len);
  if (pr.status != ODIN_PARSE_UTIL_PORT_OK) {
    return ODIN_HTTP_ERR_PORT_INVALID;
  }
  const uint16_t port_value = pr.port;

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
  out->port = port_value;
  return ODIN_HTTP_OK;
}
