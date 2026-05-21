// odin/protocol_unittests.cpp
//
// Unit tests T1-T8 from §7 of odin/docs/rfc_001_control_frame_codec.md.

#include "odin/protocol.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "gtest/gtest.h"

namespace {

constexpr size_t kSentinelSize = static_cast<size_t>(0xDEADBEEFul);
constexpr uint16_t kSentinelU16 = 0xCAFE;
constexpr uint8_t kHostFill = 0xAA;

} // namespace

// T1 — CONNECT_REQ round-trip across the canonical case and the
// (host_len, port) boundary corners.
TEST(OdinProtoTest, T1ConnectReqRoundTrip) {
  // Case A: host="example.com", port=443.
  {
    const char host[] = "example.com";
    const size_t host_len = 11;
    const uint16_t port = 443;
    uint8_t buf[ODIN_PROTO_CONNECT_REQ_MAX] = {0};
    size_t out_n = 0;
    ASSERT_EQ(odin_proto_encode_connect_req(host, host_len, port, buf,
                                            sizeof(buf), &out_n),
              ODIN_PROTO_OK);
    EXPECT_EQ(out_n, static_cast<size_t>(16));
    const uint8_t expected[] = {
        0x01, 0x01, 0x0B, 'e', 'x', 'a', 'm',  'p',
        'l',  'e',  '.',  'c', 'o', 'm', 0x01, 0xBB,
    };
    EXPECT_EQ(std::memcmp(buf, expected, sizeof(expected)), 0);

    char host_out[ODIN_PROTO_HOST_MAX + 1] = {0};
    size_t consumed = 0;
    size_t out_host_len = 0;
    uint16_t out_port = 0;
    ASSERT_EQ(odin_proto_decode_connect_req(buf, out_n, &consumed, host_out,
                                            sizeof(host_out), &out_host_len,
                                            &out_port),
              ODIN_PROTO_OK);
    EXPECT_EQ(consumed, static_cast<size_t>(16));
    EXPECT_EQ(out_host_len, host_len);
    EXPECT_EQ(out_port, port);
    EXPECT_STREQ(host_out, host);
  }

  // Case B: host="a", port=0 (host_len=1 + port=0 boundaries).
  {
    const char host[] = "a";
    const size_t host_len = 1;
    const uint16_t port = 0;
    uint8_t buf[ODIN_PROTO_CONNECT_REQ_MAX] = {0};
    size_t out_n = 0;
    ASSERT_EQ(odin_proto_encode_connect_req(host, host_len, port, buf,
                                            sizeof(buf), &out_n),
              ODIN_PROTO_OK);
    EXPECT_EQ(out_n, static_cast<size_t>(6));
    const uint8_t expected[] = {0x01, 0x01, 0x01, 'a', 0x00, 0x00};
    EXPECT_EQ(std::memcmp(buf, expected, sizeof(expected)), 0);

    char host_out[ODIN_PROTO_HOST_MAX + 1] = {0};
    size_t consumed = 0;
    size_t out_host_len = 0;
    uint16_t out_port = 0;
    ASSERT_EQ(odin_proto_decode_connect_req(buf, out_n, &consumed, host_out,
                                            sizeof(host_out), &out_host_len,
                                            &out_port),
              ODIN_PROTO_OK);
    EXPECT_EQ(consumed, static_cast<size_t>(6));
    EXPECT_EQ(out_host_len, host_len);
    EXPECT_EQ(out_port, port);
    EXPECT_STREQ(host_out, host);
  }

  // Case C: host="x"*255, port=65535 (host_len=255 + port=65535 boundaries).
  {
    const std::string host(ODIN_PROTO_HOST_MAX, 'x');
    const size_t host_len = ODIN_PROTO_HOST_MAX;
    const uint16_t port = 65535;
    uint8_t buf[ODIN_PROTO_CONNECT_REQ_MAX] = {0};
    size_t out_n = 0;
    ASSERT_EQ(odin_proto_encode_connect_req(host.data(), host_len, port, buf,
                                            sizeof(buf), &out_n),
              ODIN_PROTO_OK);
    EXPECT_EQ(out_n, static_cast<size_t>(260));
    EXPECT_EQ(buf[0], 0x01);
    EXPECT_EQ(buf[1], 0x01);
    EXPECT_EQ(buf[2], 0xFF);
    for (size_t i = 0; i < host_len; ++i) {
      ASSERT_EQ(buf[3 + i], static_cast<uint8_t>('x'));
    }
    EXPECT_EQ(buf[258], 0xFF);
    EXPECT_EQ(buf[259], 0xFF);

    char host_out[ODIN_PROTO_HOST_MAX + 1] = {0};
    size_t consumed = 0;
    size_t out_host_len = 0;
    uint16_t out_port = 0;
    ASSERT_EQ(odin_proto_decode_connect_req(buf, out_n, &consumed, host_out,
                                            sizeof(host_out), &out_host_len,
                                            &out_port),
              ODIN_PROTO_OK);
    EXPECT_EQ(consumed, static_cast<size_t>(260));
    EXPECT_EQ(out_host_len, host_len);
    EXPECT_EQ(out_port, port);
    EXPECT_EQ(std::string(host_out, out_host_len), host);
  }
}

// T2 — CONNECT_RESP round-trip at error_code = 0x0000 and 0xFFFF.
TEST(OdinProtoTest, T2ConnectRespRoundTrip) {
  const uint16_t codes[] = {0x0000, 0xFFFF};
  const uint8_t expected_lo[] = {0x01, 0x02, 0x00, 0x00};
  const uint8_t expected_hi[] = {0x01, 0x02, 0xFF, 0xFF};
  const uint8_t *const expected[] = {expected_lo, expected_hi};

  for (size_t i = 0; i < 2; ++i) {
    uint8_t buf[ODIN_PROTO_CONNECT_RESP_SIZE] = {0};
    size_t out_n = 0;
    ASSERT_EQ(
        odin_proto_encode_connect_resp(codes[i], buf, sizeof(buf), &out_n),
        ODIN_PROTO_OK)
        << "i=" << i;
    EXPECT_EQ(out_n, static_cast<size_t>(4));
    EXPECT_EQ(std::memcmp(buf, expected[i], 4), 0);

    size_t consumed = 0;
    uint16_t out_code = 0;
    ASSERT_EQ(odin_proto_decode_connect_resp(buf, out_n, &consumed, &out_code),
              ODIN_PROTO_OK)
        << "i=" << i;
    EXPECT_EQ(consumed, static_cast<size_t>(4));
    EXPECT_EQ(out_code, codes[i]);
  }
}

// T3 — Both decoders reject an unknown version byte immediately at n=1.
TEST(OdinProtoTest, T3DecodersRejectUnknownVersion) {
  const uint8_t bad_versions[] = {0x00, 0x02};

  for (const uint8_t v : bad_versions) {
    // CONNECT_REQ shaped buffer (host_len=1, total=6).
    uint8_t req[6] = {v, 0x01, 0x01, 'a', 0x01, 0xBB};
    const size_t req_lengths[] = {1, sizeof(req)};
    for (const size_t n : req_lengths) {
      char host_out[ODIN_PROTO_HOST_MAX + 1];
      std::memset(host_out, kHostFill, sizeof(host_out));
      size_t consumed = kSentinelSize;
      size_t out_host_len = kSentinelSize;
      uint16_t out_port = kSentinelU16;
      EXPECT_EQ(odin_proto_decode_connect_req(req, n, &consumed, host_out,
                                              sizeof(host_out), &out_host_len,
                                              &out_port),
                ODIN_PROTO_ERR_BAD_VERSION)
          << "v=" << static_cast<int>(v) << " n=" << n;
      EXPECT_EQ(consumed, kSentinelSize);
      EXPECT_EQ(out_host_len, kSentinelSize);
      EXPECT_EQ(out_port, kSentinelU16);
      for (size_t i = 0; i < sizeof(host_out); ++i) {
        ASSERT_EQ(static_cast<uint8_t>(host_out[i]), kHostFill);
      }
    }

    // CONNECT_RESP shaped buffer (total=4).
    uint8_t resp[4] = {v, 0x02, 0x12, 0x34};
    const size_t resp_lengths[] = {1, sizeof(resp)};
    for (const size_t n : resp_lengths) {
      size_t consumed = kSentinelSize;
      uint16_t out_code = kSentinelU16;
      EXPECT_EQ(odin_proto_decode_connect_resp(resp, n, &consumed, &out_code),
                ODIN_PROTO_ERR_BAD_VERSION)
          << "v=" << static_cast<int>(v) << " n=" << n;
      EXPECT_EQ(consumed, kSentinelSize);
      EXPECT_EQ(out_code, kSentinelU16);
    }
  }
}

// T4 — Both decoders reject an unknown or wrong-direction frame_type at n=2.
TEST(OdinProtoTest, T4DecodersRejectWrongFrameType) {
  // decode_connect_req: 0x00 (zero), 0x03 (unknown), 0x02 (wrong-known: RESP).
  const uint8_t req_bad_types[] = {0x00, 0x03, 0x02};
  for (const uint8_t ft : req_bad_types) {
    uint8_t req[6] = {0x01, ft, 0x01, 'a', 0x01, 0xBB};
    const size_t lengths[] = {2, sizeof(req)};
    for (const size_t n : lengths) {
      char host_out[ODIN_PROTO_HOST_MAX + 1];
      std::memset(host_out, kHostFill, sizeof(host_out));
      size_t consumed = kSentinelSize;
      size_t out_host_len = kSentinelSize;
      uint16_t out_port = kSentinelU16;
      EXPECT_EQ(odin_proto_decode_connect_req(req, n, &consumed, host_out,
                                              sizeof(host_out), &out_host_len,
                                              &out_port),
                ODIN_PROTO_ERR_BAD_FRAME_TYPE)
          << "ft=" << static_cast<int>(ft) << " n=" << n;
      EXPECT_EQ(consumed, kSentinelSize);
      EXPECT_EQ(out_host_len, kSentinelSize);
      EXPECT_EQ(out_port, kSentinelU16);
      for (size_t i = 0; i < sizeof(host_out); ++i) {
        ASSERT_EQ(static_cast<uint8_t>(host_out[i]), kHostFill);
      }
    }
  }

  // decode_connect_resp: 0x00 (zero), 0x03 (unknown), 0x01 (wrong-known: REQ).
  const uint8_t resp_bad_types[] = {0x00, 0x03, 0x01};
  for (const uint8_t ft : resp_bad_types) {
    uint8_t resp[4] = {0x01, ft, 0x12, 0x34};
    const size_t lengths[] = {2, sizeof(resp)};
    for (const size_t n : lengths) {
      size_t consumed = kSentinelSize;
      uint16_t out_code = kSentinelU16;
      EXPECT_EQ(odin_proto_decode_connect_resp(resp, n, &consumed, &out_code),
                ODIN_PROTO_ERR_BAD_FRAME_TYPE)
          << "ft=" << static_cast<int>(ft) << " n=" << n;
      EXPECT_EQ(consumed, kSentinelSize);
      EXPECT_EQ(out_code, kSentinelU16);
    }
  }
}

// T5 — Decoder rejects host_len=0; encoder rejects host_len=0 and host_len=256.
TEST(OdinProtoTest, T5RejectBadHostLen) {
  // Decoder: CONNECT_REQ bytes with buf[2]=0x00.
  {
    uint8_t buf[5] = {0x01, 0x01, 0x00, 0x00, 0x00};
    char host_out[ODIN_PROTO_HOST_MAX + 1];
    std::memset(host_out, kHostFill, sizeof(host_out));
    size_t consumed = kSentinelSize;
    size_t out_host_len = kSentinelSize;
    uint16_t out_port = kSentinelU16;
    EXPECT_EQ(odin_proto_decode_connect_req(buf, sizeof(buf), &consumed,
                                            host_out, sizeof(host_out),
                                            &out_host_len, &out_port),
              ODIN_PROTO_ERR_HOST_LEN_INVALID);
    EXPECT_EQ(consumed, kSentinelSize);
    EXPECT_EQ(out_host_len, kSentinelSize);
    EXPECT_EQ(out_port, kSentinelU16);
    for (size_t i = 0; i < sizeof(host_out); ++i) {
      ASSERT_EQ(static_cast<uint8_t>(host_out[i]), kHostFill);
    }
  }

  // Encoder: host_len=0.
  {
    uint8_t out_buf[ODIN_PROTO_CONNECT_REQ_MAX] = {0};
    std::memset(out_buf, kHostFill, sizeof(out_buf));
    size_t out_n = kSentinelSize;
    EXPECT_EQ(odin_proto_encode_connect_req("", 0, 443, out_buf,
                                            sizeof(out_buf), &out_n),
              ODIN_PROTO_ERR_HOST_LEN_INVALID);
    EXPECT_EQ(out_n, kSentinelSize);
    for (size_t i = 0; i < sizeof(out_buf); ++i) {
      ASSERT_EQ(out_buf[i], kHostFill);
    }
  }

  // Encoder: host_len=256.
  {
    const std::string host(256, 'x');
    uint8_t out_buf[ODIN_PROTO_CONNECT_REQ_MAX + 1] = {0};
    std::memset(out_buf, kHostFill, sizeof(out_buf));
    size_t out_n = kSentinelSize;
    EXPECT_EQ(odin_proto_encode_connect_req(host.data(), 256, 443, out_buf,
                                            sizeof(out_buf), &out_n),
              ODIN_PROTO_ERR_HOST_LEN_INVALID);
    EXPECT_EQ(out_n, kSentinelSize);
    for (size_t i = 0; i < sizeof(out_buf); ++i) {
      ASSERT_EQ(out_buf[i], kHostFill);
    }
  }
}

// T6 — Decoder returns NEED_MORE on every partial prefix and OK at the full
// frame length.
TEST(OdinProtoTest, T6NeedMoreOnPartialPrefix) {
  // CONNECT_REQ for host="example.com", port=443 (16 bytes total).
  uint8_t req_frame[ODIN_PROTO_CONNECT_REQ_MAX] = {0};
  size_t req_total = 0;
  ASSERT_EQ(odin_proto_encode_connect_req("example.com", 11, 443, req_frame,
                                          sizeof(req_frame), &req_total),
            ODIN_PROTO_OK);
  ASSERT_EQ(req_total, static_cast<size_t>(16));

  for (size_t n = 0; n < req_total; ++n) {
    char host_out[ODIN_PROTO_HOST_MAX + 1];
    std::memset(host_out, kHostFill, sizeof(host_out));
    size_t consumed = kSentinelSize;
    size_t out_host_len = kSentinelSize;
    uint16_t out_port = kSentinelU16;
    EXPECT_EQ(odin_proto_decode_connect_req(req_frame, n, &consumed, host_out,
                                            sizeof(host_out), &out_host_len,
                                            &out_port),
              ODIN_PROTO_NEED_MORE)
        << "n=" << n;
    EXPECT_EQ(consumed, kSentinelSize);
    EXPECT_EQ(out_host_len, kSentinelSize);
    EXPECT_EQ(out_port, kSentinelU16);
    for (size_t i = 0; i < sizeof(host_out); ++i) {
      ASSERT_EQ(static_cast<uint8_t>(host_out[i]), kHostFill) << "n=" << n;
    }
  }
  {
    char host_out[ODIN_PROTO_HOST_MAX + 1] = {0};
    size_t consumed = 0;
    size_t out_host_len = 0;
    uint16_t out_port = 0;
    EXPECT_EQ(odin_proto_decode_connect_req(req_frame, req_total, &consumed,
                                            host_out, sizeof(host_out),
                                            &out_host_len, &out_port),
              ODIN_PROTO_OK);
    EXPECT_EQ(consumed, req_total);
    EXPECT_EQ(out_host_len, static_cast<size_t>(11));
    EXPECT_EQ(out_port, static_cast<uint16_t>(443));
    EXPECT_STREQ(host_out, "example.com");
  }

  // CONNECT_RESP (4 bytes total).
  uint8_t resp_frame[ODIN_PROTO_CONNECT_RESP_SIZE] = {0};
  size_t resp_total = 0;
  ASSERT_EQ(odin_proto_encode_connect_resp(0x1234, resp_frame,
                                           sizeof(resp_frame), &resp_total),
            ODIN_PROTO_OK);
  ASSERT_EQ(resp_total, static_cast<size_t>(4));

  for (size_t n = 0; n < resp_total; ++n) {
    size_t consumed = kSentinelSize;
    uint16_t out_code = kSentinelU16;
    EXPECT_EQ(
        odin_proto_decode_connect_resp(resp_frame, n, &consumed, &out_code),
        ODIN_PROTO_NEED_MORE)
        << "n=" << n;
    EXPECT_EQ(consumed, kSentinelSize);
    EXPECT_EQ(out_code, kSentinelU16);
  }
  {
    size_t consumed = 0;
    uint16_t out_code = 0;
    EXPECT_EQ(odin_proto_decode_connect_resp(resp_frame, resp_total, &consumed,
                                             &out_code),
              ODIN_PROTO_OK);
    EXPECT_EQ(consumed, resp_total);
    EXPECT_EQ(out_code, static_cast<uint16_t>(0x1234));
  }
}

// T7 — Decoder consumes one frame and leaves trailing bytes untouched.
TEST(OdinProtoTest, T7ConsumeOneFrameLeaveTrailing) {
  // CONNECT_REQ (16 bytes) + 100 trailing bytes.
  uint8_t req_buf[16 + 100] = {0};
  size_t out_n = 0;
  ASSERT_EQ(odin_proto_encode_connect_req("example.com", 11, 443, req_buf, 16,
                                          &out_n),
            ODIN_PROTO_OK);
  ASSERT_EQ(out_n, static_cast<size_t>(16));
  for (size_t i = 0; i < 100; ++i) {
    req_buf[16 + i] = static_cast<uint8_t>(i ^ 0x55);
  }
  uint8_t saved_trailing[100];
  std::memcpy(saved_trailing, &req_buf[16], 100);

  char host_out[ODIN_PROTO_HOST_MAX + 1] = {0};
  size_t consumed = 0;
  size_t out_host_len = 0;
  uint16_t out_port = 0;
  EXPECT_EQ(odin_proto_decode_connect_req(req_buf, sizeof(req_buf), &consumed,
                                          host_out, sizeof(host_out),
                                          &out_host_len, &out_port),
            ODIN_PROTO_OK);
  EXPECT_EQ(consumed, static_cast<size_t>(16));
  EXPECT_EQ(out_host_len, static_cast<size_t>(11));
  EXPECT_EQ(out_port, static_cast<uint16_t>(443));
  EXPECT_STREQ(host_out, "example.com");
  EXPECT_EQ(std::memcmp(&req_buf[16], saved_trailing, 100), 0);

  // CONNECT_RESP (4 bytes) + 100 trailing bytes.
  uint8_t resp_buf[4 + 100] = {0};
  out_n = 0;
  ASSERT_EQ(odin_proto_encode_connect_resp(0xABCD, resp_buf, 4, &out_n),
            ODIN_PROTO_OK);
  ASSERT_EQ(out_n, static_cast<size_t>(4));
  for (size_t i = 0; i < 100; ++i) {
    resp_buf[4 + i] = static_cast<uint8_t>(i ^ 0x55);
  }
  uint8_t saved_trailing_resp[100];
  std::memcpy(saved_trailing_resp, &resp_buf[4], 100);

  size_t resp_consumed = 0;
  uint16_t out_code = 0;
  EXPECT_EQ(odin_proto_decode_connect_resp(resp_buf, sizeof(resp_buf),
                                           &resp_consumed, &out_code),
            ODIN_PROTO_OK);
  EXPECT_EQ(resp_consumed, static_cast<size_t>(4));
  EXPECT_EQ(out_code, static_cast<uint16_t>(0xABCD));
  EXPECT_EQ(std::memcmp(&resp_buf[4], saved_trailing_resp, 100), 0);
}

// T8 — Encoder/decoder reject an undersized output buffer.
TEST(OdinProtoTest, T8RejectUndersizedBuffer) {
  // Encode CONNECT_REQ with cap=4 (under 5 + host_len for host_len=11 -> 16).
  {
    uint8_t out_buf[4];
    std::memset(out_buf, kHostFill, sizeof(out_buf));
    size_t out_n = kSentinelSize;
    EXPECT_EQ(odin_proto_encode_connect_req("example.com", 11, 443, out_buf,
                                            sizeof(out_buf), &out_n),
              ODIN_PROTO_ERR_BUF_TOO_SMALL);
    EXPECT_EQ(out_n, kSentinelSize);
    for (size_t i = 0; i < sizeof(out_buf); ++i) {
      ASSERT_EQ(out_buf[i], kHostFill);
    }
  }

  // Decode CONNECT_REQ on a complete frame with host_cap=host_len (one short
  // of host_len + 1).
  {
    uint8_t frame[ODIN_PROTO_CONNECT_REQ_MAX] = {0};
    size_t frame_n = 0;
    ASSERT_EQ(odin_proto_encode_connect_req("example.com", 11, 443, frame,
                                            sizeof(frame), &frame_n),
              ODIN_PROTO_OK);
    ASSERT_EQ(frame_n, static_cast<size_t>(16));

    char host_out[11];
    std::memset(host_out, kHostFill, sizeof(host_out));
    size_t consumed = kSentinelSize;
    size_t out_host_len = kSentinelSize;
    uint16_t out_port = kSentinelU16;
    EXPECT_EQ(odin_proto_decode_connect_req(frame, frame_n, &consumed, host_out,
                                            sizeof(host_out), &out_host_len,
                                            &out_port),
              ODIN_PROTO_ERR_BUF_TOO_SMALL);
    EXPECT_EQ(consumed, kSentinelSize);
    EXPECT_EQ(out_host_len, kSentinelSize);
    EXPECT_EQ(out_port, kSentinelU16);
    for (size_t i = 0; i < sizeof(host_out); ++i) {
      ASSERT_EQ(static_cast<uint8_t>(host_out[i]), kHostFill);
    }
  }

  // Encode CONNECT_RESP with cap=3 (under fixed 4).
  {
    uint8_t out_buf[3];
    std::memset(out_buf, kHostFill, sizeof(out_buf));
    size_t out_n = kSentinelSize;
    EXPECT_EQ(odin_proto_encode_connect_resp(0x0000, out_buf, sizeof(out_buf),
                                             &out_n),
              ODIN_PROTO_ERR_BUF_TOO_SMALL);
    EXPECT_EQ(out_n, kSentinelSize);
    for (size_t i = 0; i < sizeof(out_buf); ++i) {
      ASSERT_EQ(out_buf[i], kHostFill);
    }
  }
}
