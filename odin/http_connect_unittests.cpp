// odin/http_connect_unittests.cpp
//
// Unit tests T1-T10 from §7 of odin/docs/rfc_003_http_connect_parser.md.

#include "odin/http_connect.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "gtest/gtest.h"

namespace {

constexpr size_t kSentinelConsumed = static_cast<size_t>(0xDEADBEEFul);
constexpr size_t kSentinelHostOff = static_cast<size_t>(0xCAFEBABEul);
constexpr size_t kSentinelHostLen = static_cast<size_t>(0xFEEDFACEul);
constexpr uint16_t kSentinelPort = 0xBEEF;

odin_http_connect_t sentinel_out() {
  odin_http_connect_t o;
  o.host_off = kSentinelHostOff;
  o.host_len = kSentinelHostLen;
  o.port = kSentinelPort;
  return o;
}

} // namespace

// T1 — Round-trip success: hostname, IPv4 literal, and minimal forms.
TEST(OdinHttpConnectTest, T1RoundTripSuccess) {

  struct Case {
    const char *req;
    size_t n;
    const char *host;
    size_t host_len;
    uint16_t port;
  };
  const Case cases[] = {
      {"CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n", 59,
       "example.com", 11, 443},
      {"CONNECT 127.0.0.1:8443 HTTP/1.0\r\n\r\n", 35, "127.0.0.1", 9, 8443},
      {"CONNECT a:1 HTTP/1.1\r\n\r\n", 24, "a", 1, 1},
  };

  for (const auto &c : cases) {
    const auto *buf = reinterpret_cast<const uint8_t *>(c.req);
    size_t consumed = kSentinelConsumed;
    odin_http_connect_t out = sentinel_out();
    ASSERT_EQ(odin_http_parse_connect(buf, c.n, &consumed, &out), ODIN_HTTP_OK)
        << "req=" << c.req;
    EXPECT_EQ(consumed, c.n) << "req=" << c.req;
    EXPECT_EQ(out.host_len, c.host_len) << "req=" << c.req;
    EXPECT_EQ(out.port, c.port) << "req=" << c.req;
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(buf + out.host_off),
                          out.host_len),
              std::string(c.host))
        << "req=" << c.req;
  }
}

// T2 — IPv6 bracketed authority strips brackets from reported host.
TEST(OdinHttpConnectTest, T2IPv6BracketStrip) {

  struct Case {
    const char *req;
    size_t n;
    const char *host;
    size_t host_len;
    uint16_t port;
  };
  const Case cases[] = {
      {"CONNECT [::1]:443 HTTP/1.1\r\n\r\n", 30, "::1", 3, 443},
      {"CONNECT [2001:db8::1]:8080 HTTP/1.1\r\n\r\n", 39, "2001:db8::1", 11,
       8080},
  };

  for (const auto &c : cases) {
    const auto *buf = reinterpret_cast<const uint8_t *>(c.req);
    // Snapshot buf bytes to verify unmodified after parse.
    const std::string snap(c.req, c.n);

    size_t consumed = kSentinelConsumed;
    odin_http_connect_t out = sentinel_out();
    ASSERT_EQ(odin_http_parse_connect(buf, c.n, &consumed, &out), ODIN_HTTP_OK)
        << "req=" << c.req;
    EXPECT_EQ(consumed, c.n) << "req=" << c.req;
    EXPECT_EQ(out.host_len, c.host_len) << "req=" << c.req;
    EXPECT_EQ(out.port, c.port) << "req=" << c.req;
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(buf + out.host_off),
                          out.host_len),
              std::string(c.host))
        << "req=" << c.req;
    // buf must be unmodified.
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(buf), c.n), snap)
        << "buf modified for req=" << c.req;
  }
}

// T3 — Headers consumed; post-CRLFCRLF bytes remain for byte forwarding.
TEST(OdinHttpConnectTest, T3HeadersConsumedTrailingBytesUntouched) {

  // 92-byte request followed by 16-byte tail (total 108).
  const char *req_str = "CONNECT example.com:443 HTTP/1.1\r\n"
                        "Host: example.com:443\r\n"
                        "Proxy-Authorization: Bearer abc\r\n"
                        "\r\n";
  const size_t req_len = 92;
  const char *tail = "GET / HTTP/1.1\r\n";
  const size_t tail_len = 16;
  const size_t total = req_len + tail_len; // 108

  // Sanity-check lengths.
  ASSERT_EQ(std::strlen(req_str), req_len);
  ASSERT_EQ(std::strlen(tail), tail_len);

  uint8_t buf[108];
  std::memcpy(buf, req_str, req_len);
  std::memcpy(buf + req_len, tail, tail_len);

  // Snapshot trailing bytes.
  uint8_t saved_tail[16];
  std::memcpy(saved_tail, buf + req_len, tail_len);

  size_t consumed = kSentinelConsumed;
  odin_http_connect_t out = sentinel_out();
  ASSERT_EQ(odin_http_parse_connect(buf, total, &consumed, &out), ODIN_HTTP_OK);
  EXPECT_EQ(consumed, req_len);
  EXPECT_EQ(out.host_len, static_cast<size_t>(11));
  EXPECT_EQ(out.port, static_cast<uint16_t>(443));
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(buf + out.host_off),
                        out.host_len),
            "example.com");
  // Trailing bytes must be unmodified.
  EXPECT_EQ(std::memcmp(buf + req_len, saved_tail, tail_len), 0);
}

// T4 — Prefix parser: NEED_MORE on every partial prefix; OK at full; bare-LF
// never satisfies strict-CRLF.
TEST(OdinHttpConnectTest, T4PrefixParserNeedMore) {

  // 59-byte well-formed request from T1 case A.
  const char *req =
      "CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n";
  const size_t total = 59;
  ASSERT_EQ(std::strlen(req), total);

  const auto *buf = reinterpret_cast<const uint8_t *>(req);

  for (size_t n = 0; n < total; ++n) {
    size_t consumed = kSentinelConsumed;
    odin_http_connect_t out = sentinel_out();
    EXPECT_EQ(odin_http_parse_connect(buf, n, &consumed, &out),
              ODIN_HTTP_NEED_MORE)
        << "n=" << n;
    EXPECT_EQ(consumed, kSentinelConsumed) << "n=" << n;
    EXPECT_EQ(out.host_off, kSentinelHostOff) << "n=" << n;
    EXPECT_EQ(out.host_len, kSentinelHostLen) << "n=" << n;
    EXPECT_EQ(out.port, kSentinelPort) << "n=" << n;
  }

  // At n = total, must be OK.
  {
    size_t consumed = kSentinelConsumed;
    odin_http_connect_t out = sentinel_out();
    EXPECT_EQ(odin_http_parse_connect(buf, total, &consumed, &out),
              ODIN_HTTP_OK);
    EXPECT_EQ(consumed, total);
  }

  // Bare-LF buffer: "CONNECT a:1 HTTP/1.1\n\n" (22 bytes) — no \r\n anywhere,
  // parser keeps returning NEED_MORE at n=22.
  {
    const char *bare_lf = "CONNECT a:1 HTTP/1.1\n\n";
    const size_t bare_n = 22;
    ASSERT_EQ(std::strlen(bare_lf), bare_n);
    const auto *bare_buf = reinterpret_cast<const uint8_t *>(bare_lf);
    size_t consumed = kSentinelConsumed;
    odin_http_connect_t out = sentinel_out();
    EXPECT_EQ(odin_http_parse_connect(bare_buf, bare_n, &consumed, &out),
              ODIN_HTTP_NEED_MORE);
    EXPECT_EQ(consumed, kSentinelConsumed);
    EXPECT_EQ(out.host_off, kSentinelHostOff);
    EXPECT_EQ(out.host_len, kSentinelHostLen);
    EXPECT_EQ(out.port, kSentinelPort);
  }
}

// T5 — Bad method fast-fails on the first divergent prefix byte; HTAB after
// method violates single-SP separator.
TEST(OdinHttpConnectTest, T5BadMethodFastFail) {

  // Full-input bad-method cases.
  const char *const full_bad[] = {
      "GET / HTTP/1.1\r\n\r\n",
      "connect a:1 HTTP/1.1\r\n\r\n",
      "CONNECTx a:1 HTTP/1.1\r\n\r\n",
      "CONNECT\ta:1\tHTTP/1.1\r\n\r\n",
  };
  for (const char *req : full_bad) {
    const auto *buf = reinterpret_cast<const uint8_t *>(req);
    const size_t n = std::strlen(req);
    size_t consumed = kSentinelConsumed;
    odin_http_connect_t out = sentinel_out();
    EXPECT_EQ(odin_http_parse_connect(buf, n, &consumed, &out),
              ODIN_HTTP_ERR_BAD_METHOD)
        << "req=" << req;
    EXPECT_EQ(consumed, kSentinelConsumed) << "req=" << req;
    EXPECT_EQ(out.host_off, kSentinelHostOff) << "req=" << req;
    EXPECT_EQ(out.host_len, kSentinelHostLen) << "req=" << req;
    EXPECT_EQ(out.port, kSentinelPort) << "req=" << req;
  }

  // "GE" (2 B) — first byte already divergent from 'C' -> ERR_BAD_METHOD.
  {
    const auto *buf = reinterpret_cast<const uint8_t *>("GE");
    size_t consumed = kSentinelConsumed;
    odin_http_connect_t out = sentinel_out();
    EXPECT_EQ(odin_http_parse_connect(buf, 2, &consumed, &out),
              ODIN_HTTP_ERR_BAD_METHOD);
    EXPECT_EQ(consumed, kSentinelConsumed);
    EXPECT_EQ(out.host_off, kSentinelHostOff);
    EXPECT_EQ(out.host_len, kSentinelHostLen);
    EXPECT_EQ(out.port, kSentinelPort);
  }

  // "CONN" (4 B) — still a valid prefix of "CONNECT " -> NEED_MORE.
  {
    const auto *buf = reinterpret_cast<const uint8_t *>("CONN");
    size_t consumed = kSentinelConsumed;
    odin_http_connect_t out = sentinel_out();
    EXPECT_EQ(odin_http_parse_connect(buf, 4, &consumed, &out),
              ODIN_HTTP_NEED_MORE);
    EXPECT_EQ(consumed, kSentinelConsumed);
    EXPECT_EQ(out.host_off, kSentinelHostOff);
    EXPECT_EQ(out.host_len, kSentinelHostLen);
    EXPECT_EQ(out.port, kSentinelPort);
  }
}

// T6 — Malformed request-target shapes.
TEST(OdinHttpConnectTest, T6MalformedRequestTarget) {

  const char *const cases[] = {
      "CONNECT  HTTP/1.1\r\n\r\n",            // empty target between SPs
      "CONNECT example.com HTTP/1.1\r\n\r\n", // no :port
      "CONNECT [::1:443 HTTP/1.1\r\n\r\n",    // unbalanced [
      "CONNECT [::1]443 HTTP/1.1\r\n\r\n",    // bracket without :
  };

  for (const char *req : cases) {
    const auto *buf = reinterpret_cast<const uint8_t *>(req);
    const size_t n = std::strlen(req);
    size_t consumed = kSentinelConsumed;
    odin_http_connect_t out = sentinel_out();
    EXPECT_EQ(odin_http_parse_connect(buf, n, &consumed, &out),
              ODIN_HTTP_ERR_BAD_REQUEST_TARGET)
        << "req=" << req;
    EXPECT_EQ(consumed, kSentinelConsumed) << "req=" << req;
    EXPECT_EQ(out.host_off, kSentinelHostOff) << "req=" << req;
    EXPECT_EQ(out.host_len, kSentinelHostLen) << "req=" << req;
    EXPECT_EQ(out.port, kSentinelPort) << "req=" << req;
  }
}

// T7 — Port string invalid.
TEST(OdinHttpConnectTest, T7PortInvalid) {

  const char *const cases[] = {
      "CONNECT a:65536 HTTP/1.1\r\n\r\n",  "CONNECT a:99999 HTTP/1.1\r\n\r\n",
      "CONNECT a:123456 HTTP/1.1\r\n\r\n", "CONNECT a:abc HTTP/1.1\r\n\r\n",
      "CONNECT a: HTTP/1.1\r\n\r\n",       "CONNECT a:b:443 HTTP/1.1\r\n\r\n",
  };

  for (const char *req : cases) {
    const auto *buf = reinterpret_cast<const uint8_t *>(req);
    const size_t n = std::strlen(req);
    size_t consumed = kSentinelConsumed;
    odin_http_connect_t out = sentinel_out();
    EXPECT_EQ(odin_http_parse_connect(buf, n, &consumed, &out),
              ODIN_HTTP_ERR_PORT_INVALID)
        << "req=" << req;
    EXPECT_EQ(consumed, kSentinelConsumed) << "req=" << req;
    EXPECT_EQ(out.host_off, kSentinelHostOff) << "req=" << req;
    EXPECT_EQ(out.host_len, kSentinelHostLen) << "req=" << req;
    EXPECT_EQ(out.port, kSentinelPort) << "req=" << req;
  }
}

// T8 — Bad HTTP-version token.
TEST(OdinHttpConnectTest, T8BadVersion) {

  const char *const cases[] = {
      "CONNECT a:1 HTTP/2.0\r\n\r\n",
      "CONNECT a:1 http/1.1\r\n\r\n",
      "CONNECT a:1 HTTP/1.\r\n\r\n",
      "CONNECT a:1\r\n\r\n",
  };

  for (const char *req : cases) {
    const auto *buf = reinterpret_cast<const uint8_t *>(req);
    const size_t n = std::strlen(req);
    size_t consumed = kSentinelConsumed;
    odin_http_connect_t out = sentinel_out();
    EXPECT_EQ(odin_http_parse_connect(buf, n, &consumed, &out),
              ODIN_HTTP_ERR_BAD_VERSION)
        << "req=" << req;
    EXPECT_EQ(consumed, kSentinelConsumed) << "req=" << req;
    EXPECT_EQ(out.host_off, kSentinelHostOff) << "req=" << req;
    EXPECT_EQ(out.host_len, kSentinelHostLen) << "req=" << req;
    EXPECT_EQ(out.port, kSentinelPort) << "req=" << req;
  }
}

// T9 — Host length out of range (256-byte host).
TEST(OdinHttpConnectTest, T9HostLenInvalid) {

  // "CONNECT " + ("x" * 256) + ":443 HTTP/1.1\r\n\r\n"
  std::string req = "CONNECT ";
  req += std::string(256, 'x');
  req += ":443 HTTP/1.1\r\n\r\n";

  const auto *buf = reinterpret_cast<const uint8_t *>(req.data());
  const size_t n = req.size();
  size_t consumed = kSentinelConsumed;
  odin_http_connect_t out = sentinel_out();
  EXPECT_EQ(odin_http_parse_connect(buf, n, &consumed, &out),
            ODIN_HTTP_ERR_HOST_LEN_INVALID);
  EXPECT_EQ(consumed, kSentinelConsumed);
  EXPECT_EQ(out.host_off, kSentinelHostOff);
  EXPECT_EQ(out.host_len, kSentinelHostLen);
  EXPECT_EQ(out.port, kSentinelPort);
}

// T10 — Request exceeds ODIN_HTTP_REQUEST_MAX (both incremental-feed and
// one-shot cap branches).
TEST(OdinHttpConnectTest, T10RequestTooLarge) {

  // B = "CONNECT a:1 HTTP/1.1\r\n" + "X-Pad: " + ("a" * 8200) + "\r\n\r\n"
  // request-line is 22 bytes; "X-Pad: " is 7 bytes; 8200 'a' bytes; "\r\n\r\n"
  // is 4 bytes -> total = 22 + 7 + 8200 + 4 = 8233.
  std::string B = "CONNECT a:1 HTTP/1.1\r\n";
  B += "X-Pad: ";
  B += std::string(8200, 'a');
  B += "\r\n\r\n";
  ASSERT_EQ(B.size(), static_cast<size_t>(8233));

  const auto *buf = reinterpret_cast<const uint8_t *>(B.data());

  // (a) Incremental feed: NEED_MORE for n < 8192; ERR_REQUEST_TOO_LARGE for
  // first n >= 8192 where CRLFCRLF is still absent.
  for (size_t n = 0; n <= 8232; ++n) {
    size_t consumed = kSentinelConsumed;
    odin_http_connect_t out = sentinel_out();
    const odin_http_status_t status =
        odin_http_parse_connect(buf, n, &consumed, &out);
    if (n < ODIN_HTTP_REQUEST_MAX) {
      EXPECT_EQ(status, ODIN_HTTP_NEED_MORE) << "n=" << n;
    } else {
      EXPECT_EQ(status, ODIN_HTTP_ERR_REQUEST_TOO_LARGE) << "n=" << n;
    }
    EXPECT_EQ(consumed, kSentinelConsumed) << "n=" << n;
    EXPECT_EQ(out.host_off, kSentinelHostOff) << "n=" << n;
    EXPECT_EQ(out.host_len, kSentinelHostLen) << "n=" << n;
    EXPECT_EQ(out.port, kSentinelPort) << "n=" << n;
  }

  // (b) One-shot: CRLFCRLF located at end=8233 > 8192 -> ERR_REQUEST_TOO_LARGE
  // via post-locate cap branch.
  {
    size_t consumed = kSentinelConsumed;
    odin_http_connect_t out = sentinel_out();
    EXPECT_EQ(odin_http_parse_connect(buf, 8233, &consumed, &out),
              ODIN_HTTP_ERR_REQUEST_TOO_LARGE);
    EXPECT_EQ(consumed, kSentinelConsumed);
    EXPECT_EQ(out.host_off, kSentinelHostOff);
    EXPECT_EQ(out.host_len, kSentinelHostLen);
    EXPECT_EQ(out.port, kSentinelPort);
  }

  // (c) "CONNECT " + ("a" * 8192) with n=8200 — no CRLF, incremental-feed
  // branch fires because n >= ODIN_HTTP_REQUEST_MAX.
  {
    std::string C = "CONNECT ";
    C += std::string(8192, 'a');
    ASSERT_EQ(C.size(), static_cast<size_t>(8200));
    const auto *cbuf = reinterpret_cast<const uint8_t *>(C.data());
    size_t consumed = kSentinelConsumed;
    odin_http_connect_t out = sentinel_out();
    EXPECT_EQ(odin_http_parse_connect(cbuf, 8200, &consumed, &out),
              ODIN_HTTP_ERR_REQUEST_TOO_LARGE);
    EXPECT_EQ(consumed, kSentinelConsumed);
    EXPECT_EQ(out.host_off, kSentinelHostOff);
    EXPECT_EQ(out.host_len, kSentinelHostLen);
    EXPECT_EQ(out.port, kSentinelPort);
  }
}
