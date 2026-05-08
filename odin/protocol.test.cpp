#include "odin/protocol.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace {

// Sentinel value written into *consumed before each decode call. Truncated
// inputs (S4/S5) must leave it untouched per the FrameDecoder contract.
constexpr std::size_t kSentinelConsumed = 0xDEADBEEF;

// S1 — Encode CONNECT_REQUEST with authority "example.com:443" and check
// the exact 18-byte wire output (RFC §8 row 1).
TEST(OdinEncode, S1_ConnectRequest) {
  const char authority[] = "example.com:443";
  const std::size_t alen = sizeof(authority) - 1;  // 15

  std::vector<std::uint8_t> out(odin_connect_request_size(alen));
  odin_encode_connect_request(authority, alen, out.data());

  const std::vector<std::uint8_t> expected = {
      0x01, 0x00, 0x0F, 0x65, 0x78, 0x61, 0x6D, 0x70, 0x6C,
      0x65, 0x2E, 0x63, 0x6F, 0x6D, 0x3A, 0x34, 0x34, 0x33,
  };
  EXPECT_EQ(out, expected);
}

// S2 — Encode CONNECT_RESPONSE with kOk and check the exact 4-byte wire
// output (RFC §8 row 2).
TEST(OdinEncode, S2_ConnectResponseOk) {
  std::vector<std::uint8_t> out(ODIN_CONNECT_RESPONSE_SIZE);
  odin_encode_connect_response(ODIN_STATUS_OK, out.data());

  const std::vector<std::uint8_t> expected = {0x02, 0x00, 0x01, 0x00};
  EXPECT_EQ(out, expected);
}

// S3 — Round-trip: encode CONNECT_REQUEST then CONNECT_RESPONSE into one
// buffer; decode each in turn; check structs match originals and the sum
// of *consumed equals total input length (RFC §8 row 3).
TEST(OdinDecode, S3_RoundTripBothFrames) {
  const char        authority[] = "example.com:443";
  const std::size_t alen        = sizeof(authority) - 1;

  std::vector<std::uint8_t> buf(odin_connect_request_size(alen) +
                                ODIN_CONNECT_RESPONSE_SIZE);
  odin_encode_connect_request(authority, alen, buf.data());
  odin_encode_connect_response(ODIN_STATUS_OK,
                               buf.data() + odin_connect_request_size(alen));
  ASSERT_EQ(buf.size(), 22u);

  const char* decoded_authority     = nullptr;
  std::size_t decoded_authority_len = 0;
  std::size_t req_consumed          = 0;
  EXPECT_EQ(odin_decode_connect_request(buf.data(), buf.size(),
                                        &decoded_authority,
                                        &decoded_authority_len, &req_consumed),
            ODIN_DECODE_OK);
  EXPECT_EQ(std::string(decoded_authority, decoded_authority_len), authority);

  odin_connect_status decoded_status = ODIN_STATUS_INTERNAL_ERROR;
  std::size_t         resp_consumed  = 0;
  EXPECT_EQ(odin_decode_connect_response(buf.data() + req_consumed,
                                         buf.size() - req_consumed,
                                         &decoded_status, &resp_consumed),
            ODIN_DECODE_OK);
  EXPECT_EQ(decoded_status, ODIN_STATUS_OK);

  EXPECT_EQ(req_consumed + resp_consumed, buf.size());
}

// S4 — Truncated header: 2-byte input. Returns ODIN_DECODE_NEED_MORE_DATA
// and leaves *consumed untouched (RFC §8 row 4 + decoder contract).
TEST(OdinDecode, S4_TruncatedHeader) {
  const std::uint8_t input[] = {0x01, 0x00};

  const char* authority     = nullptr;
  std::size_t authority_len = 0;
  std::size_t consumed      = kSentinelConsumed;
  EXPECT_EQ(odin_decode_connect_request(input, sizeof(input), &authority,
                                        &authority_len, &consumed),
            ODIN_DECODE_NEED_MORE_DATA);
  EXPECT_EQ(consumed, kSentinelConsumed);
}

// S5 — Truncated payload: header declares 15 bytes but only 14 follow.
// Returns ODIN_DECODE_NEED_MORE_DATA, *consumed untouched (RFC §8 row 5).
TEST(OdinDecode, S5_TruncatedPayload) {
  std::vector<std::uint8_t> input = {0x01, 0x00, 0x0F};
  input.insert(input.end(), 14, 0x41);  // 14 of the 15 declared payload bytes

  const char* authority     = nullptr;
  std::size_t authority_len = 0;
  std::size_t consumed      = kSentinelConsumed;
  EXPECT_EQ(odin_decode_connect_request(input.data(), input.size(), &authority,
                                        &authority_len, &consumed),
            ODIN_DECODE_NEED_MORE_DATA);
  EXPECT_EQ(consumed, kSentinelConsumed);
}

// S6 — Oversized authority: length=261 (one over the 260 cap), with the
// payload bytes present. Returns ODIN_DECODE_AUTHORITY_TOO_LONG without
// dereferencing the payload (RFC §7 DoS mitigation, §8 row 6).
TEST(OdinDecode, S6_OversizedAuthority) {
  std::vector<std::uint8_t> input = {0x01, 0x01, 0x05};  // length = 261
  input.insert(input.end(), 261, 0x41);

  const char* authority     = nullptr;
  std::size_t authority_len = 0;
  std::size_t consumed      = 0;
  EXPECT_EQ(odin_decode_connect_request(input.data(), input.size(), &authority,
                                        &authority_len, &consumed),
            ODIN_DECODE_AUTHORITY_TOO_LONG);
}

// S7 — Unknown frame type: type bytes 0x00 and 0x03 (neither 0x01 nor
// 0x02). Both return ODIN_DECODE_UNKNOWN_FRAME_TYPE (RFC §8 row 7).
TEST(OdinDecode, S7_UnknownFrameType) {
  const std::uint8_t input_zero[]  = {0x00, 0x00, 0x00};
  const std::uint8_t input_three[] = {0x03, 0x00, 0x00};

  const char* authority     = nullptr;
  std::size_t authority_len = 0;
  std::size_t consumed      = 0;

  EXPECT_EQ(odin_decode_connect_request(input_zero, sizeof(input_zero),
                                        &authority, &authority_len, &consumed),
            ODIN_DECODE_UNKNOWN_FRAME_TYPE);
  EXPECT_EQ(odin_decode_connect_request(input_three, sizeof(input_three),
                                        &authority, &authority_len, &consumed),
            ODIN_DECODE_UNKNOWN_FRAME_TYPE);
}

// S8 — Malformed CONNECT_RESPONSE length: length=2 instead of the
// fixed-1. Returns ODIN_DECODE_INVALID_FRAME (RFC §8 row 8).
TEST(OdinDecode, S8_MalformedConnectResponseLength) {
  const std::uint8_t input[] = {0x02, 0x00, 0x02, 0x00, 0x00};

  odin_connect_status status   = ODIN_STATUS_OK;
  std::size_t         consumed = 0;
  EXPECT_EQ(odin_decode_connect_response(input, sizeof(input), &status,
                                         &consumed),
            ODIN_DECODE_INVALID_FRAME);
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
