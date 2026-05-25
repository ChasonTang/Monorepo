// odin/host_addr_unittests.cpp
//
// Tests T4-T5 from §7 of odin/docs/rfc_007_cli_server_host_addr_parser.md.

#include "odin/host_addr.h"

#include <cstddef>
#include <cstdint>
#include <string>

#include "gtest/gtest.h"

#include "odin/cli.h"      // ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER
#include "odin/protocol.h" // ODIN_PROTO_HOST_MAX

namespace {

const char *const kSentinelHost = reinterpret_cast<const char *>(0xCAFEBABEul);
constexpr size_t kSentinelHostLen = static_cast<size_t>(0xFEEDFACEul);
constexpr uint16_t kSentinelPort = 0xBEEF;

odin_host_addr_t sentinel_out() {
  odin_host_addr_t o;
  o.host = kSentinelHost;
  o.host_len = kSentinelHostLen;
  o.port = kSentinelPort;
  return o;
}

} // namespace

// T4 — odin_host_addr_parse OK arms (four shapes + default-port
// fallback + leading-zero port).
TEST(OdinHostAddrTest, T4OkArms) {
  struct Case {
    const char *input;
    size_t expected_host_off;
    size_t expected_host_len;
    uint16_t expected_port;
  };
  const Case cases[] = {
      {"example.com", 0, 11, ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER},
      {"example.com:443", 0, 11, 443},
      {"[::1]", 1, 3, ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER},
      {"[::1]:8080", 1, 3, 8080},
      {"127.0.0.1:9000", 0, 9, 9000},
      {"a:00001", 0, 1, 1},
  };
  for (const auto &c : cases) {
    odin_host_addr_t out = sentinel_out();
    ASSERT_EQ(odin_host_addr_parse(c.input, &out), ODIN_HOST_ADDR_OK)
        << "input=" << c.input;
    EXPECT_EQ(out.host, c.input + c.expected_host_off) << "input=" << c.input;
    EXPECT_EQ(out.host_len, c.expected_host_len) << "input=" << c.input;
    EXPECT_EQ(out.port, c.expected_port) << "input=" << c.input;
  }
}

// T5 — odin_host_addr_parse error arms; *out unchanged on every
// non-OK return.
TEST(OdinHostAddrTest, T5ErrorArms) {
  struct Case {
    const char *input;
    odin_host_addr_status_t expected;
  };
  const Case cases[] = {
      {nullptr, ODIN_HOST_ADDR_ERR_EMPTY},
      {"", ODIN_HOST_ADDR_ERR_EMPTY},
      {"[::1", ODIN_HOST_ADDR_ERR_BAD_TARGET},
      {":443", ODIN_HOST_ADDR_ERR_BAD_TARGET},
      {"[]", ODIN_HOST_ADDR_ERR_BAD_TARGET},
      {"a:99999", ODIN_HOST_ADDR_ERR_PORT_INVALID},
      {"a:abc", ODIN_HOST_ADDR_ERR_PORT_INVALID},
      {"a:b:443", ODIN_HOST_ADDR_ERR_PORT_INVALID},
      {"a:", ODIN_HOST_ADDR_ERR_PORT_INVALID},
      {"[::1]:", ODIN_HOST_ADDR_ERR_PORT_INVALID},
  };
  for (const auto &c : cases) {
    odin_host_addr_t out = sentinel_out();
    EXPECT_EQ(odin_host_addr_parse(c.input, &out), c.expected)
        << "input=" << (c.input ? c.input : "<null>");
    EXPECT_EQ(out.host, kSentinelHost)
        << "input=" << (c.input ? c.input : "<null>");
    EXPECT_EQ(out.host_len, kSentinelHostLen)
        << "input=" << (c.input ? c.input : "<null>");
    EXPECT_EQ(out.port, kSentinelPort)
        << "input=" << (c.input ? c.input : "<null>");
  }
  // Oversized host: ("x" * 256) + ":443" -> ERR_HOST_LEN_INVALID.
  {
    const std::string big = std::string(ODIN_PROTO_HOST_MAX + 1, 'x') + ":443";
    odin_host_addr_t out = sentinel_out();
    EXPECT_EQ(odin_host_addr_parse(big.c_str(), &out),
              ODIN_HOST_ADDR_ERR_HOST_LEN_INVALID);
    EXPECT_EQ(out.host, kSentinelHost);
    EXPECT_EQ(out.host_len, kSentinelHostLen);
    EXPECT_EQ(out.port, kSentinelPort);
  }
}
