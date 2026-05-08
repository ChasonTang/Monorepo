#include "odin/protocol.h"

#include <string.h>

static void write_header(uint8_t* out, odin_frame_type type, uint16_t length) {
  out[0] = (uint8_t)type;
  out[1] = (uint8_t)((length >> 8) & 0xFF);
  out[2] = (uint8_t)(length & 0xFF);
}

static uint16_t read_length_be(const uint8_t* in) {
  return (uint16_t)(((uint16_t)in[1] << 8) | (uint16_t)in[2]);
}

void odin_encode_connect_request(const char* authority,
                                 size_t      authority_len,
                                 uint8_t*    out) {
  write_header(out, ODIN_FRAME_CONNECT_REQUEST, (uint16_t)authority_len);
  if (authority_len > 0) {
    memcpy(out + ODIN_HEADER_SIZE, authority, authority_len);
  }
}

void odin_encode_connect_response(odin_connect_status status, uint8_t* out) {
  write_header(out, ODIN_FRAME_CONNECT_RESPONSE, 1);
  out[ODIN_HEADER_SIZE] = (uint8_t)status;
}

odin_decode_result odin_decode_connect_request(const uint8_t* in,
                                               size_t         in_len,
                                               const char**   authority,
                                               size_t*        authority_len,
                                               size_t*        consumed) {
  if (in_len < ODIN_HEADER_SIZE) {
    return ODIN_DECODE_NEED_MORE_DATA;
  }
  if (in[0] != (uint8_t)ODIN_FRAME_CONNECT_REQUEST) {
    return ODIN_DECODE_UNKNOWN_FRAME_TYPE;
  }
  uint16_t length = read_length_be(in);
  /* Cap before the payload-availability check so a peer announcing
     length=0xFFFF cannot force a 64 KiB allocation per stream (§7). */
  if ((size_t)length > ODIN_MAX_AUTHORITY_LEN) {
    return ODIN_DECODE_AUTHORITY_TOO_LONG;
  }
  if (in_len < ODIN_HEADER_SIZE + (size_t)length) {
    return ODIN_DECODE_NEED_MORE_DATA;
  }
  *authority     = (const char*)(in + ODIN_HEADER_SIZE);
  *authority_len = length;
  *consumed      = ODIN_HEADER_SIZE + length;
  return ODIN_DECODE_OK;
}

odin_decode_result odin_decode_connect_response(const uint8_t*       in,
                                                size_t               in_len,
                                                odin_connect_status* status,
                                                size_t*              consumed) {
  if (in_len < ODIN_HEADER_SIZE) {
    return ODIN_DECODE_NEED_MORE_DATA;
  }
  if (in[0] != (uint8_t)ODIN_FRAME_CONNECT_RESPONSE) {
    return ODIN_DECODE_UNKNOWN_FRAME_TYPE;
  }
  uint16_t length = read_length_be(in);
  if (length != 1) {
    return ODIN_DECODE_INVALID_FRAME;
  }
  if (in_len < ODIN_CONNECT_RESPONSE_SIZE) {
    return ODIN_DECODE_NEED_MORE_DATA;
  }
  *status   = (odin_connect_status)in[ODIN_HEADER_SIZE];
  *consumed = ODIN_CONNECT_RESPONSE_SIZE;
  return ODIN_DECODE_OK;
}
