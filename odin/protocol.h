/* odin/protocol.h
 *
 * Codec for the Odin proxy's per-stream control frames. Pure byte-buffer
 * surface — no I/O, no allocation, no QUIC, no global state.
 *
 * Wire format (v1):
 *
 *   CONNECT_REQ (Client -> Server, frame_type = 0x01):
 *     +---------+------------+----------+----------------+----------+
 *     | version | frame_type | host_len |   host bytes   |   port   |
 *     | 1 byte  |   1 byte   |  1 byte  | host_len bytes |  2 bytes |
 *     +---------+------------+----------+----------------+----------+
 *     Total: 5 + host_len bytes; host_len in [1, 255]; max 260 bytes.
 *
 *   CONNECT_RESP (Server -> Client, frame_type = 0x02):
 *     +---------+------------+--------------+
 *     | version | frame_type |  error_code  |
 *     | 1 byte  |   1 byte   |    2 bytes   |
 *     +---------+------------+--------------+
 *     Total: 4 bytes (fixed).
 *
 * Field semantics (v1):
 *   version    = 0x01 (else ERR_BAD_VERSION).
 *   frame_type = 0x01 (CONNECT_REQ) or 0x02 (CONNECT_RESP); else
 *                ERR_BAD_FRAME_TYPE.
 *   host_len   in [1, 255]; zero -> ERR_HOST_LEN_INVALID.
 *   error_code 0x0000 = OK; non-zero is a Server-assigned failure reason
 *              (v1 reserves the 16-bit space; specific assignments belong
 *              to follow-up RFCs).
 *
 * Multi-byte integers (port, error_code) are big-endian; 1-byte fields are
 * endianness-agnostic. host bytes are opaque — DNS hostname syntax and
 * IP-literal grammar are the Server's concern, and the codec does not
 * reject embedded NUL. Each frame is self-delimiting from its own bytes:
 * CONNECT_REQ's total size is 5 + host_len (byte 2),
 * CONNECT_RESP's is fixed at 4. QUIC streams are byte-oriented, so the
 * decoders are prefix parsers that consume one frame and leave any
 * trailing bytes for the next layer. No padding, no reserved bits in v1;
 * the 1-byte version is the sole forward-compatibility lever (a future
 * v2 bumps to 0x02, v1 decoders see ERR_BAD_VERSION).
 */

#ifndef ODIN_PROTOCOL_H_
#define ODIN_PROTOCOL_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ODIN_PROTO_VERSION_V1 0x01
#define ODIN_PROTO_FRAME_CONNECT_REQ 0x01
#define ODIN_PROTO_FRAME_CONNECT_RESP 0x02
#define ODIN_PROTO_HOST_MAX 255
#define ODIN_PROTO_CONNECT_REQ_MAX (5 + ODIN_PROTO_HOST_MAX) /* 260 */
#define ODIN_PROTO_CONNECT_RESP_SIZE 4

typedef enum {
  ODIN_PROTO_OK = 0,
  ODIN_PROTO_NEED_MORE,
  ODIN_PROTO_ERR_BUF_TOO_SMALL,
  ODIN_PROTO_ERR_HOST_LEN_INVALID,
  ODIN_PROTO_ERR_BAD_VERSION,
  ODIN_PROTO_ERR_BAD_FRAME_TYPE,
} odin_proto_status_t;

typedef struct {
  const void *base;
  size_t len;
} odin_proto_iov_t;

odin_proto_status_t odin_proto_encode_connect_req(const char *host,
                                                  size_t host_len,
                                                  uint16_t port,
                                                  odin_proto_iov_t out_iov[3],
                                                  uint8_t scratch_header[3],
                                                  uint8_t scratch_port[2]);

typedef struct {
  size_t host_off;
  size_t host_len;
  uint16_t port;
} odin_proto_connect_req_view_t;

odin_proto_status_t
odin_proto_decode_connect_req(const uint8_t *buf, size_t n,
                              size_t *out_consumed,
                              odin_proto_connect_req_view_t *out);

typedef struct {
  uint8_t bytes[ODIN_PROTO_CONNECT_RESP_SIZE];
} odin_proto_connect_resp_frame_t;

void odin_proto_encode_connect_resp(uint16_t error_code,
                                    odin_proto_connect_resp_frame_t *out);

odin_proto_status_t odin_proto_decode_connect_resp(const uint8_t *buf, size_t n,
                                                   size_t *out_consumed,
                                                   uint16_t *out_error_code);

#ifdef __cplusplus
}
#endif

#endif /* ODIN_PROTOCOL_H_ */
