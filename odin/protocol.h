#ifndef ODIN_PROTOCOL_H_
#define ODIN_PROTOCOL_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public-API visibility. The project compiles with -fvisibility=hidden by
   default (see //build:compiler_defaults) so a symbol that needs to cross
   the component()->shared_library boundary must be tagged explicitly.
   Mirrors c-ares' CARES_EXTERN / xquic's XQC_EXPORT pattern. */
#if defined(__GNUC__) || defined(__clang__)
#  define ODIN_EXPORT __attribute__((visibility("default")))
#else
#  define ODIN_EXPORT
#endif

#define ODIN_ALPN              "odin/1"
#define ODIN_ALPN_LEN          ((size_t)6)
#define ODIN_HEADER_SIZE       ((size_t)3)
#define ODIN_MAX_AUTHORITY_LEN ((size_t)260)

// CONNECT_RESPONSE wire size is fixed (header + 1-byte status).
#define ODIN_CONNECT_RESPONSE_SIZE ((size_t)4)

// Wire size of an encoded CONNECT_REQUEST whose authority is `authority_len`
// bytes. Caller passes a buffer of at least this many bytes to the encoder.
static inline size_t odin_connect_request_size(size_t authority_len) {
  return ODIN_HEADER_SIZE + authority_len;
}

typedef enum {
  ODIN_FRAME_CONNECT_REQUEST  = 0x01,
  ODIN_FRAME_CONNECT_RESPONSE = 0x02
} odin_frame_type;

typedef enum {
  ODIN_STATUS_OK                   = 0x00,
  ODIN_STATUS_BAD_REQUEST          = 0x01,
  ODIN_STATUS_DNS_FAILURE          = 0x02,
  ODIN_STATUS_UPSTREAM_UNREACHABLE = 0x03,
  ODIN_STATUS_INTERNAL_ERROR       = 0x04
} odin_connect_status;

typedef enum {
  ODIN_DECODE_OK,
  ODIN_DECODE_NEED_MORE_DATA,
  ODIN_DECODE_UNKNOWN_FRAME_TYPE,
  ODIN_DECODE_AUTHORITY_TOO_LONG,
  ODIN_DECODE_INVALID_FRAME
} odin_decode_result;

// Encoders. The caller supplies an output buffer of at least
// odin_connect_request_size(authority_len) / ODIN_CONNECT_RESPONSE_SIZE
// bytes; the encoder writes the wire bytes verbatim and assumes the
// authority is already validated (length cap enforcement is the decoder's
// job, per §7).
ODIN_EXPORT void odin_encode_connect_request(const char* authority,
                                             size_t      authority_len,
                                             uint8_t*    out);

ODIN_EXPORT void odin_encode_connect_response(odin_connect_status status,
                                              uint8_t*            out);

// Decoders. On ODIN_DECODE_OK:
//   - decode_connect_request: *authority points into `in + ODIN_HEADER_SIZE`
//     (zero-copy; caller must consume before `in` is freed); *authority_len
//     is the byte length; *consumed = ODIN_HEADER_SIZE + *authority_len.
//   - decode_connect_response: *status holds the parsed status byte;
//     *consumed = ODIN_CONNECT_RESPONSE_SIZE.
// On ODIN_DECODE_NEED_MORE_DATA, *consumed is left untouched; caller appends
// more bytes and retries. On any other error return, output parameters are
// left untouched.
ODIN_EXPORT odin_decode_result odin_decode_connect_request(
    const uint8_t* in,
    size_t         in_len,
    const char**   authority,
    size_t*        authority_len,
    size_t*        consumed);

ODIN_EXPORT odin_decode_result odin_decode_connect_response(
    const uint8_t*       in,
    size_t               in_len,
    odin_connect_status* status,
    size_t*              consumed);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ODIN_PROTOCOL_H_ */
