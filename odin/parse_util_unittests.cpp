// odin/parse_util_unittests.cpp
//
// Tests T1-T3 from §7 of odin/docs/rfc_007_cli_server_host_addr_parser.md.

#include "odin/parse_util.h"

#include <cstddef>
#include <cstdint>
#include <string>

#include "gtest/gtest.h"

namespace {

constexpr size_t kSentinelHostOff = static_cast<size_t>(0xCAFEBABEul);
constexpr size_t kSentinelHostLen = static_cast<size_t>(0xFEEDFACEul);
constexpr size_t kSentinelPortOff = static_cast<size_t>(0xDEADBEEFul);
constexpr size_t kSentinelPortLen = static_cast<size_t>(0xABADCAFEul);
constexpr uint8_t kSentinelPortPresent = 0xA5;

odin_parse_util_hostport_t sentinel_hostport() {
  odin_parse_util_hostport_t o;
  o.host_off = kSentinelHostOff;
  o.host_len = kSentinelHostLen;
  o.port_off = kSentinelPortOff;
  o.port_len = kSentinelPortLen;
  o.port_present = kSentinelPortPresent;
  return o;
}

} // namespace

// T1 — odin_parse_util_port: boundary OK arms and reject arms.
TEST(OdinParseUtilTest, T1PortBoundaryAndReject) {
  struct Case {
    const char *bytes;
    size_t n;
    odin_parse_util_port_status_t expected_status;
    uint16_t expected_port;
  };
  const Case cases[] = {
      {"0", 1, ODIN_PARSE_UTIL_PORT_OK, 0},
      {"65535", 5, ODIN_PARSE_UTIL_PORT_OK, 65535},
      {"00080", 5, ODIN_PARSE_UTIL_PORT_OK, 80},
      {"65536", 5, ODIN_PARSE_UTIL_PORT_BAD, 0},
      {"99999", 5, ODIN_PARSE_UTIL_PORT_BAD, 0},
      {"123456", 6, ODIN_PARSE_UTIL_PORT_BAD, 0},
      {"abc", 3, ODIN_PARSE_UTIL_PORT_BAD, 0},
      {"", 0, ODIN_PARSE_UTIL_PORT_BAD, 0},
  };
  for (const auto &c : cases) {
    const auto *buf = reinterpret_cast<const uint8_t *>(c.bytes);
    const odin_parse_util_port_result_t r = odin_parse_util_port(buf, c.n);
    EXPECT_EQ(r.status, c.expected_status) << "bytes=" << c.bytes;
    EXPECT_EQ(r.port, c.expected_port) << "bytes=" << c.bytes;
  }
}

// T2 — odin_parse_util_split_hostport: accepted shapes (including
// empty-port boundary).
TEST(OdinParseUtilTest, T2SplitHostportAcceptedShapes) {
  struct Case {
    const char *bytes;
    size_t n;
    const char *expected_host;
    const char *expected_port; // may be "", meaning empty port slice
    uint8_t expected_port_present;
  };
  const Case cases[] = {
      {"example.com", 11, "example.com", "", 0},
      {"example.com:443", 15, "example.com", "443", 1},
      {"[::1]", 5, "::1", "", 0},
      {"[::1]:443", 9, "::1", "443", 1},
      {"[2001:db8::1]:8080", 18, "2001:db8::1", "8080", 1},
      {"127.0.0.1:9000", 14, "127.0.0.1", "9000", 1},
      {"a:1", 3, "a", "1", 1},
      {"a:", 2, "a", "", 1},
      {"[::1]:", 6, "::1", "", 1},
  };
  for (const auto &c : cases) {
    const auto *buf = reinterpret_cast<const uint8_t *>(c.bytes);
    odin_parse_util_hostport_t out = sentinel_hostport();
    ASSERT_EQ(odin_parse_util_split_hostport(buf, c.n, &out),
              ODIN_PARSE_UTIL_HOSTPORT_OK)
        << "bytes=" << c.bytes;
    const std::string host(reinterpret_cast<const char *>(buf + out.host_off),
                           out.host_len);
    EXPECT_EQ(host, c.expected_host) << "bytes=" << c.bytes;
    EXPECT_EQ(out.port_present, c.expected_port_present) << "bytes=" << c.bytes;
    if (out.port_present) {
      const std::string port(reinterpret_cast<const char *>(buf + out.port_off),
                             out.port_len);
      EXPECT_EQ(port, c.expected_port) << "bytes=" << c.bytes;
    }
  }
}

// T3 — odin_parse_util_split_hostport: malformed shapes; *out unchanged.
TEST(OdinParseUtilTest, T3SplitHostportMalformed) {
  struct Case {
    const char *bytes;
    size_t n;
  };
  const Case cases[] = {
      {"", 0},         {":", 1},        {":443", 4},
      {"[]", 2},       {"[]:443", 6},   {"[::1", 4},
      {"[::1:443", 8}, {"[::1]443", 8}, {"[::1]X", 6},
  };
  for (const auto &c : cases) {
    const auto *buf = reinterpret_cast<const uint8_t *>(c.bytes);
    odin_parse_util_hostport_t out = sentinel_hostport();
    EXPECT_EQ(odin_parse_util_split_hostport(buf, c.n, &out),
              ODIN_PARSE_UTIL_HOSTPORT_BAD)
        << "bytes=" << c.bytes;
    EXPECT_EQ(out.host_off, kSentinelHostOff) << "bytes=" << c.bytes;
    EXPECT_EQ(out.host_len, kSentinelHostLen) << "bytes=" << c.bytes;
    EXPECT_EQ(out.port_off, kSentinelPortOff) << "bytes=" << c.bytes;
    EXPECT_EQ(out.port_len, kSentinelPortLen) << "bytes=" << c.bytes;
    EXPECT_EQ(out.port_present, kSentinelPortPresent) << "bytes=" << c.bytes;
  }
}
