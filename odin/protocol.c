/* odin/protocol.c — RFC-001 codec implementation. */

#include "odin/protocol.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

void odin_proto_encode_connect_resp(uint16_t error_code,
                                    odin_proto_connect_resp_frame_t *out) {
  assert(out != NULL);

  out->bytes[0] = ODIN_PROTO_VERSION_V1;
  out->bytes[1] = ODIN_PROTO_FRAME_CONNECT_RESP;
  out->bytes[2] = (uint8_t)((error_code >> 8) & 0xFF);
  out->bytes[3] = (uint8_t)(error_code & 0xFF);
}

odin_proto_status_t odin_proto_decode_connect_resp(const uint8_t *buf, size_t n,
                                                   size_t *out_consumed,
                                                   uint16_t *out_error_code) {
  assert(buf != NULL);
  assert(out_consumed != NULL);
  assert(out_error_code != NULL);

  if (n < 1) {
    return ODIN_PROTO_NEED_MORE;
  }
  if (buf[0] != ODIN_PROTO_VERSION_V1) {
    return ODIN_PROTO_ERR_BAD_VERSION;
  }
  if (n < 2) {
    return ODIN_PROTO_NEED_MORE;
  }
  if (buf[1] != ODIN_PROTO_FRAME_CONNECT_RESP) {
    return ODIN_PROTO_ERR_BAD_FRAME_TYPE;
  }
  if (n < ODIN_PROTO_CONNECT_RESP_SIZE) {
    return ODIN_PROTO_NEED_MORE;
  }

  *out_error_code = (uint16_t)(((uint16_t)buf[2] << 8) | (uint16_t)buf[3]);
  *out_consumed = ODIN_PROTO_CONNECT_RESP_SIZE;
  return ODIN_PROTO_OK;
}

odin_proto_status_t odin_proto_encode_connect_req(const char *host,
                                                  size_t host_len,
                                                  uint16_t port,
                                                  odin_proto_iov_t out_iov[3],
                                                  uint8_t scratch_header[3],
                                                  uint8_t scratch_port[2]) {
  assert(host != NULL);
  assert(out_iov != NULL);
  assert(scratch_header != NULL);
  assert(scratch_port != NULL);

  if (host_len < 1 || host_len > ODIN_PROTO_HOST_MAX) {
    return ODIN_PROTO_ERR_HOST_LEN_INVALID;
  }

  scratch_header[0] = ODIN_PROTO_VERSION_V1;
  scratch_header[1] = ODIN_PROTO_FRAME_CONNECT_REQ;
  scratch_header[2] = (uint8_t)host_len;
  scratch_port[0] = (uint8_t)((port >> 8) & 0xFFu);
  scratch_port[1] = (uint8_t)(port & 0xFFu);

  out_iov[0].base = scratch_header;
  out_iov[0].len = 3;
  out_iov[1].base = host;
  out_iov[1].len = host_len;
  out_iov[2].base = scratch_port;
  out_iov[2].len = 2;
  return ODIN_PROTO_OK;
}

odin_proto_status_t
odin_proto_decode_connect_req(const uint8_t *buf, size_t n,
                              size_t *out_consumed,
                              odin_proto_connect_req_view_t *out) {
  assert(buf != NULL);
  assert(out_consumed != NULL);
  assert(out != NULL);

  if (n < 1) {
    return ODIN_PROTO_NEED_MORE;
  }
  if (buf[0] != ODIN_PROTO_VERSION_V1) {
    return ODIN_PROTO_ERR_BAD_VERSION;
  }
  if (n < 2) {
    return ODIN_PROTO_NEED_MORE;
  }
  if (buf[1] != ODIN_PROTO_FRAME_CONNECT_REQ) {
    return ODIN_PROTO_ERR_BAD_FRAME_TYPE;
  }
  if (n < 3) {
    return ODIN_PROTO_NEED_MORE;
  }
  const uint8_t host_len = buf[2];
  if (host_len < 1) {
    return ODIN_PROTO_ERR_HOST_LEN_INVALID;
  }
  const size_t total = (size_t)5 + (size_t)host_len;
  if (n < total) {
    return ODIN_PROTO_NEED_MORE;
  }

  out->host_off = 3;
  out->host_len = host_len;
  out->port = (uint16_t)(((uint16_t)buf[3 + host_len] << 8) |
                         (uint16_t)buf[4 + host_len]);
  *out_consumed = total;
  return ODIN_PROTO_OK;
}
