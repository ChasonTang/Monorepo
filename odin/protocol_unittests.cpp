// odin/protocol_unittests.cpp
//
// Unit tests from RFC-001, RFC-004, and RFC-005 for the Odin control-frame
// codec. The zero-copy request codec and fixed-frame response encoder own
// request encode/decode and response encode coverage.

#include "odin/protocol.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace {

constexpr size_t kSentinelSize = static_cast<size_t>(0xDEADBEEFul);
constexpr uint16_t kSentinelU16 = 0xCAFE;
constexpr uint8_t kScratchFill = 0xCC;
constexpr size_t kIovLenSentinel = static_cast<size_t>(0xFEEDFACEul);

const uint8_t kIovBaseSentinelByte = 0xEE;
const void *const kIovBaseSentinel = &kIovBaseSentinelByte;

void SentinelFillIov(odin_proto_iov_t iov[3]) {
  for (size_t i = 0; i < 3; ++i) {
    iov[i].base = kIovBaseSentinel;
    iov[i].len = kIovLenSentinel;
  }
}

bool FlattenConnectReqForTest(const char *host, size_t host_len, uint16_t port,
                              uint8_t *buf, size_t cap, size_t *out_n) {
  odin_proto_iov_t iov[3];
  uint8_t scratch_header[3];
  uint8_t scratch_port[2];
  const odin_proto_status_t status = odin_proto_encode_connect_req(
      host, host_len, port, iov, scratch_header, scratch_port);
  if (status != ODIN_PROTO_OK) {
    return false;
  }

  const size_t total = iov[0].len + iov[1].len + iov[2].len;
  if (cap < total) {
    return false;
  }

  size_t off = 0;
  for (size_t i = 0; i < 3; ++i) {
    const uint8_t *p = static_cast<const uint8_t *>(iov[i].base);
    std::memcpy(buf + off, p, iov[i].len);
    off += iov[i].len;
  }
  *out_n = total;
  return true;
}

void ExpectReqViewUnmodified(const odin_proto_connect_req_view_t &view) {
  EXPECT_EQ(view.host_off, kSentinelSize);
  EXPECT_EQ(view.host_len, kSentinelSize);
  EXPECT_EQ(view.port, kSentinelU16);
}

} // namespace

// RFC-001 T3, CONNECT_RESP half — response decoder rejects an unknown version
// byte immediately at n=1.
TEST(OdinProtoTest, T3ConnectRespDecoderRejectsUnknownVersion) {
  const uint8_t bad_versions[] = {0x00, 0x02};

  for (const uint8_t v : bad_versions) {
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

// RFC-001 T4, CONNECT_RESP half — response decoder rejects an unknown or
// wrong-direction frame_type at n=2.
TEST(OdinProtoTest, T4ConnectRespDecoderRejectsWrongFrameType) {
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

// RFC-001 T2, CONNECT_RESP decoder half — boundary error_code values decode
// from their canonical wire bytes.
TEST(OdinProtoTest, T2ConnectRespDecoderDecodesBoundaryCodes) {
  struct Case {
    uint8_t frame[ODIN_PROTO_CONNECT_RESP_SIZE];
    uint16_t code;
  };
  const Case cases[] = {
      {{0x01, 0x02, 0x00, 0x00}, 0x0000},
      {{0x01, 0x02, 0xFF, 0xFF}, 0xFFFF},
  };

  for (const Case &c : cases) {
    size_t consumed = 0;
    uint16_t out_code = 0;
    EXPECT_EQ(odin_proto_decode_connect_resp(c.frame, sizeof(c.frame),
                                             &consumed, &out_code),
              ODIN_PROTO_OK);
    EXPECT_EQ(consumed, static_cast<size_t>(ODIN_PROTO_CONNECT_RESP_SIZE));
    EXPECT_EQ(out_code, c.code);
  }
}

// RFC-001 T6, CONNECT_RESP half — response decoder returns NEED_MORE on every
// partial prefix and OK at the full frame length.
TEST(OdinProtoTest, T6ConnectRespNeedMoreOnPartialPrefix) {
  odin_proto_connect_resp_frame_t resp_frame = {};
  odin_proto_encode_connect_resp(0x1234, &resp_frame);

  for (size_t n = 0; n < ODIN_PROTO_CONNECT_RESP_SIZE; ++n) {
    size_t consumed = kSentinelSize;
    uint16_t out_code = kSentinelU16;
    EXPECT_EQ(odin_proto_decode_connect_resp(resp_frame.bytes, n, &consumed,
                                             &out_code),
              ODIN_PROTO_NEED_MORE)
        << "n=" << n;
    EXPECT_EQ(consumed, kSentinelSize);
    EXPECT_EQ(out_code, kSentinelU16);
  }

  size_t consumed = 0;
  uint16_t out_code = 0;
  EXPECT_EQ(odin_proto_decode_connect_resp(resp_frame.bytes,
                                           ODIN_PROTO_CONNECT_RESP_SIZE,
                                           &consumed, &out_code),
            ODIN_PROTO_OK);
  EXPECT_EQ(consumed, static_cast<size_t>(ODIN_PROTO_CONNECT_RESP_SIZE));
  EXPECT_EQ(out_code, static_cast<uint16_t>(0x1234));
}

// RFC-004 CONNECT_REQ codec tests T1-T5.

// T1 — encode emits exact protocol bytes; host slot aliases the caller's
// pointer.
TEST(OdinProtoReqTest, T1EncodeExactBytesAndAliasing) {
  struct Case {
    std::string host;
    size_t host_len;
    uint16_t port;
    size_t total;
  };
  const std::string host_a = "example.com";
  const std::string host_b = "a";
  const std::string host_c(ODIN_PROTO_HOST_MAX, 'x');
  const Case cases[] = {
      {host_a, 11, 443, 16},
      {host_b, 1, 0, 6},
      {host_c, ODIN_PROTO_HOST_MAX, 65535, 260},
  };

  for (const Case &c : cases) {
    odin_proto_iov_t iov[3];
    uint8_t scratch_header[3] = {0};
    uint8_t scratch_port[2] = {0};
    ASSERT_EQ(odin_proto_encode_connect_req(c.host.data(), c.host_len, c.port,
                                            iov, scratch_header, scratch_port),
              ODIN_PROTO_OK)
        << "host_len=" << c.host_len;

    EXPECT_EQ(iov[0].len, static_cast<size_t>(3));
    EXPECT_EQ(iov[1].len, c.host_len);
    EXPECT_EQ(iov[2].len, static_cast<size_t>(2));
    EXPECT_EQ(iov[1].base, static_cast<const void *>(c.host.data()));

    std::vector<uint8_t> flat;
    flat.reserve(c.total);
    for (size_t i = 0; i < 3; ++i) {
      const uint8_t *p = static_cast<const uint8_t *>(iov[i].base);
      flat.insert(flat.end(), p, p + iov[i].len);
    }
    ASSERT_EQ(flat.size(), c.total);

    std::vector<uint8_t> expected;
    expected.reserve(c.total);
    expected.push_back(ODIN_PROTO_VERSION_V1);
    expected.push_back(ODIN_PROTO_FRAME_CONNECT_REQ);
    expected.push_back(static_cast<uint8_t>(c.host_len));
    expected.insert(expected.end(), c.host.begin(), c.host.end());
    expected.push_back(static_cast<uint8_t>((c.port >> 8) & 0xFFu));
    expected.push_back(static_cast<uint8_t>(c.port & 0xFFu));
    ASSERT_EQ(expected.size(), c.total);
    EXPECT_EQ(flat, expected) << "host_len=" << c.host_len;
  }
}

// T2 — encode rejects host_len out of [1, 255]; writes nothing on error.
TEST(OdinProtoReqTest, T2EncodeRejectsBadHostLen) {
  // host_len=0
  {
    odin_proto_iov_t iov[3];
    SentinelFillIov(iov);
    uint8_t scratch_header[3];
    uint8_t scratch_port[2];
    std::memset(scratch_header, kScratchFill, sizeof(scratch_header));
    std::memset(scratch_port, kScratchFill, sizeof(scratch_port));
    EXPECT_EQ(odin_proto_encode_connect_req("", 0, 443, iov, scratch_header,
                                            scratch_port),
              ODIN_PROTO_ERR_HOST_LEN_INVALID);
    for (size_t i = 0; i < 3; ++i) {
      EXPECT_EQ(iov[i].base, kIovBaseSentinel);
      EXPECT_EQ(iov[i].len, kIovLenSentinel);
    }
    for (size_t i = 0; i < sizeof(scratch_header); ++i) {
      ASSERT_EQ(scratch_header[i], kScratchFill);
    }
    for (size_t i = 0; i < sizeof(scratch_port); ++i) {
      ASSERT_EQ(scratch_port[i], kScratchFill);
    }
  }

  // host_len=256
  {
    const std::string host(256, 'x');
    odin_proto_iov_t iov[3];
    SentinelFillIov(iov);
    uint8_t scratch_header[3];
    uint8_t scratch_port[2];
    std::memset(scratch_header, kScratchFill, sizeof(scratch_header));
    std::memset(scratch_port, kScratchFill, sizeof(scratch_port));
    EXPECT_EQ(odin_proto_encode_connect_req(host.data(), 256, 443, iov,
                                            scratch_header, scratch_port),
              ODIN_PROTO_ERR_HOST_LEN_INVALID);
    for (size_t i = 0; i < 3; ++i) {
      EXPECT_EQ(iov[i].base, kIovBaseSentinel);
      EXPECT_EQ(iov[i].len, kIovLenSentinel);
    }
    for (size_t i = 0; i < sizeof(scratch_header); ++i) {
      ASSERT_EQ(scratch_header[i], kScratchFill);
    }
    for (size_t i = 0; i < sizeof(scratch_port); ++i) {
      ASSERT_EQ(scratch_port[i], kScratchFill);
    }
  }
}

// T3 — decode round-trip yields the aliasing view and the slice matches the
// original host bytes.
TEST(OdinProtoReqTest, T3DecodeAliasingView) {
  struct Case {
    std::string host;
    size_t host_len;
    uint16_t port;
    size_t total;
  };
  const std::string host_a = "example.com";
  const std::string host_b = "a";
  const std::string host_c(ODIN_PROTO_HOST_MAX, 'x');
  const Case cases[] = {
      {host_a, 11, 443, 16},
      {host_b, 1, 0, 6},
      {host_c, ODIN_PROTO_HOST_MAX, 65535, 260},
  };

  for (const Case &c : cases) {
    uint8_t frame[ODIN_PROTO_CONNECT_REQ_MAX] = {0};
    size_t frame_n = 0;
    ASSERT_TRUE(FlattenConnectReqForTest(c.host.data(), c.host_len, c.port,
                                         frame, sizeof(frame), &frame_n));
    ASSERT_EQ(frame_n, c.total);

    size_t consumed = 0;
    odin_proto_connect_req_view_t view = {};
    EXPECT_EQ(odin_proto_decode_connect_req(frame, frame_n, &consumed, &view),
              ODIN_PROTO_OK)
        << "host_len=" << c.host_len;
    EXPECT_EQ(consumed, c.total);
    EXPECT_EQ(view.host_off, static_cast<size_t>(3));
    EXPECT_EQ(view.host_len, c.host_len);
    EXPECT_EQ(view.port, c.port);
    EXPECT_EQ(std::memcmp(&frame[view.host_off], c.host.data(), view.host_len),
              0)
        << "host_len=" << c.host_len;
  }
}

// T4 — decode rejects bad version / frame_type / host_len; *out and
// *consumed unmodified.
TEST(OdinProtoReqTest, T4DecodeRejectsBadBytes) {
  uint8_t canonical[16] = {0};
  size_t canonical_n = 0;
  ASSERT_TRUE(FlattenConnectReqForTest("example.com", 11, 443, canonical,
                                       sizeof(canonical), &canonical_n));
  ASSERT_EQ(canonical_n, static_cast<size_t>(16));

  auto check = [](const uint8_t *buf, size_t n, odin_proto_status_t expected) {
    size_t consumed = kSentinelSize;
    odin_proto_connect_req_view_t view;
    view.host_off = kSentinelSize;
    view.host_len = kSentinelSize;
    view.port = kSentinelU16;
    EXPECT_EQ(odin_proto_decode_connect_req(buf, n, &consumed, &view), expected)
        << "n=" << n;
    EXPECT_EQ(consumed, kSentinelSize);
    ExpectReqViewUnmodified(view);
  };

  // Bad version: buf[0] = 0x00, 0x02; at n=1 and n=16.
  for (const uint8_t v :
       {static_cast<uint8_t>(0x00), static_cast<uint8_t>(0x02)}) {
    uint8_t buf[16];
    std::memcpy(buf, canonical, sizeof(buf));
    buf[0] = v;
    check(buf, 1, ODIN_PROTO_ERR_BAD_VERSION);
    check(buf, 16, ODIN_PROTO_ERR_BAD_VERSION);
  }

  // Bad frame_type: buf[1] = 0x00, 0x02, 0x03; at n=2 and n=16.
  for (const uint8_t ft :
       {static_cast<uint8_t>(0x00), static_cast<uint8_t>(0x02),
        static_cast<uint8_t>(0x03)}) {
    uint8_t buf[16];
    std::memcpy(buf, canonical, sizeof(buf));
    buf[1] = ft;
    check(buf, 2, ODIN_PROTO_ERR_BAD_FRAME_TYPE);
    check(buf, 16, ODIN_PROTO_ERR_BAD_FRAME_TYPE);
  }

  // Bad host_len: buf[2] = 0x00; at n=5 with otherwise-syntactically-valid
  // frame.
  {
    uint8_t buf[5] = {0x01, 0x01, 0x00, 0x00, 0x00};
    check(buf, 5, ODIN_PROTO_ERR_HOST_LEN_INVALID);
  }
}

// T5 — decode returns NEED_MORE on every partial prefix, OK at full frame,
// and leaves trailing bytes untouched.
TEST(OdinProtoReqTest, T5DecodeNeedMoreAndTrailing) {
  uint8_t buf[116] = {0};
  size_t frame_n = 0;
  ASSERT_TRUE(
      FlattenConnectReqForTest("example.com", 11, 443, buf, 16, &frame_n));
  ASSERT_EQ(frame_n, static_cast<size_t>(16));
  for (size_t i = 0; i < 100; ++i) {
    buf[16 + i] = static_cast<uint8_t>(i ^ 0x55);
  }
  uint8_t saved_trailing[100];
  std::memcpy(saved_trailing, &buf[16], 100);

  for (size_t n = 0; n < 16; ++n) {
    size_t consumed = kSentinelSize;
    odin_proto_connect_req_view_t view;
    view.host_off = kSentinelSize;
    view.host_len = kSentinelSize;
    view.port = kSentinelU16;
    EXPECT_EQ(odin_proto_decode_connect_req(buf, n, &consumed, &view),
              ODIN_PROTO_NEED_MORE)
        << "n=" << n;
    EXPECT_EQ(consumed, kSentinelSize);
    ExpectReqViewUnmodified(view);
  }

  size_t consumed = 0;
  odin_proto_connect_req_view_t view = {};
  EXPECT_EQ(odin_proto_decode_connect_req(buf, 116, &consumed, &view),
            ODIN_PROTO_OK);
  EXPECT_EQ(consumed, static_cast<size_t>(16));
  EXPECT_EQ(view.host_off, static_cast<size_t>(3));
  EXPECT_EQ(view.host_len, static_cast<size_t>(11));
  EXPECT_EQ(view.port, static_cast<uint16_t>(443));
  EXPECT_LE(view.host_off + view.host_len, consumed);
  EXPECT_LE(consumed, static_cast<size_t>(116));
  EXPECT_EQ(std::memcmp(&buf[16], saved_trailing, 100), 0);
}

// RFC-005 CONNECT_RESP encoder tests T1-T3.

// T1 — encode emits exact boundary-code bytes.
TEST(OdinProtoRespTest, T1EmitsBoundaryCodeBytes) {
  odin_proto_connect_resp_frame_t lo = {};
  odin_proto_encode_connect_resp(0x0000, &lo);
  const uint8_t expected_lo[] = {0x01, 0x02, 0x00, 0x00};
  EXPECT_EQ(std::memcmp(lo.bytes, expected_lo, sizeof(expected_lo)), 0);

  odin_proto_connect_resp_frame_t hi = {};
  odin_proto_encode_connect_resp(0xFFFF, &hi);
  const uint8_t expected_hi[] = {0x01, 0x02, 0xFF, 0xFF};
  EXPECT_EQ(std::memcmp(hi.bytes, expected_hi, sizeof(expected_hi)), 0);
}

// T2 — response frame type is exactly four bytes and writes stay inside the
// frame
// field.
TEST(OdinProtoRespTest, T2FrameSizeAndCanaries) {
  struct Wrapper {
    uint8_t before;
    odin_proto_connect_resp_frame_t frame;
    uint8_t after;
  };

  Wrapper wrapper = {};
  wrapper.before = 0xA5;
  wrapper.after = 0x5A;
  ASSERT_EQ(sizeof(odin_proto_connect_resp_frame_t),
            static_cast<size_t>(ODIN_PROTO_CONNECT_RESP_SIZE));

  odin_proto_encode_connect_resp(0x1234, &wrapper.frame);

  const uint8_t expected[] = {0x01, 0x02, 0x12, 0x34};
  EXPECT_EQ(std::memcmp(wrapper.frame.bytes, expected, sizeof(expected)), 0);
  EXPECT_EQ(wrapper.before, static_cast<uint8_t>(0xA5));
  EXPECT_EQ(wrapper.after, static_cast<uint8_t>(0x5A));
}

// T3 — encoder output interops with the existing decoder and preserves trailing
// bytes.
TEST(OdinProtoRespTest, T3DecoderInteropAndTrailing) {
  uint8_t buffer[ODIN_PROTO_CONNECT_RESP_SIZE + 100] = {0};
  odin_proto_connect_resp_frame_t frame = {};
  odin_proto_encode_connect_resp(0xABCD, &frame);
  std::memcpy(buffer, frame.bytes, sizeof(frame.bytes));
  for (size_t i = 0; i < 100; ++i) {
    buffer[ODIN_PROTO_CONNECT_RESP_SIZE + i] = static_cast<uint8_t>(i ^ 0x55);
  }
  uint8_t saved_trailing[100];
  std::memcpy(saved_trailing, &buffer[ODIN_PROTO_CONNECT_RESP_SIZE], 100);

  size_t consumed = 0;
  uint16_t out_code = 0;
  EXPECT_EQ(odin_proto_decode_connect_resp(buffer, sizeof(buffer), &consumed,
                                           &out_code),
            ODIN_PROTO_OK);
  EXPECT_EQ(consumed, static_cast<size_t>(ODIN_PROTO_CONNECT_RESP_SIZE));
  EXPECT_EQ(out_code, static_cast<uint16_t>(0xABCD));
  EXPECT_EQ(
      std::memcmp(&buffer[ODIN_PROTO_CONNECT_RESP_SIZE], saved_trailing, 100),
      0);
}
