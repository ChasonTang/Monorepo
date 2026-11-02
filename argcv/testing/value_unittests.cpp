// Include first to verify that C++ callers need no prerequisite headers or
// external extern "C" wrapper. This file links against the C implementation.
#include "argcv/value.h"

#include <array>
#include <initializer_list>
#include <string>

#include "gtest/gtest.h"

namespace {

constexpr uint16_t kUnchanged = 43210;

struct ParseU16Case {
  const char *id;
  std::string text;
  bool success;
  uint16_t expected;
};

class ArgcvParseU16Test : public testing::TestWithParam<ParseU16Case> {};

// TEST_P generates a public override of GoogleTest's private TestBody method.
// NOLINTNEXTLINE(misc-override-with-different-visibility)
TEST_P(ArgcvParseU16Test, RfcCase) {
  const auto &test = GetParam();
  const auto &before = test.text;
  std::string text = before;
  uint16_t value = kUnchanged;
  // C++17 string::data() is writable and nonnull even for the empty string.
  EXPECT_EQ(argcv_parse_u16(text.data(), text.size(), &value), test.success);
  EXPECT_EQ(value, test.expected);
  EXPECT_EQ(text, before);
}

INSTANTIATE_TEST_SUITE_P(
    Rfc, ArgcvParseU16Test,
    testing::ValuesIn<ParseU16Case>({
        {"V01", "0", true, 0},
        {"V02", "789", true, 789},
        {"V03", "65535", true, 65535},
        {"V04", "010", true, 10},
        {"V05", "000000", true, 0},
        {"V06", std::string(1024, '0') + "65535", true, 65535},
        {"E01", "", false, kUnchanged},
        {"E02", "65536", false, kUnchanged},
        {"E03", "4294967296", false, kUnchanged},
        {"E04", std::string(1024, '0') + "65536", false, kUnchanged},
        {"E05Plus", "+42", false, kUnchanged},
        {"E05Minus", "-0", false, kUnchanged},
        {"E06LeadingSpace", " 42", false, kUnchanged},
        {"E06MiddleSpace", "4 2", false, kUnchanged},
        {"E06TrailingSpace", "42 ", false, kUnchanged},
        {"E06Tab", "\t42", false, kUnchanged},
        {"E06CrLf", "42\r\n", false, kUnchanged},
        {"E07Beginning", "q42", false, kUnchanged},
        {"E07Middle", "4q2", false, kUnchanged},
        {"E07End", "42q", false, kUnchanged},
        {"E08Hex", "0x2A", false, kUnchanged},
        {"E08DecimalPoint", "42.0", false, kUnchanged},
        {"E08Exponent", "4e1", false, kUnchanged},
        {"E08Separator", "1_000", false, kUnchanged},
        {"E09", "\xEF\xBC\x94\xEF\xBC\x92", false, kUnchanged},
        {"E10", "\xFF", false, kUnchanged},
        {"E11EmbeddedNul", std::string("\x34\x00\x32", 3), false, kUnchanged},
        {"E11TrailingNul", std::string("42\0", 3), false, kUnchanged},
        {"E12BelowDigit", "1/", false, kUnchanged},
        {"E12AboveDigit", "1:", false, kUnchanged},
    }),
    [](const testing::TestParamInfo<ParseU16Case> &info) {
      return info.param.id;
    });

TEST(ArgcvValueTest, S01NonTerminatedSlice) {
  std::array<char, 3> text = {'3', '1', '4'};
  const auto before = text;
  uint16_t value = kUnchanged;
  EXPECT_TRUE(argcv_parse_u16(text.data(), text.size(), &value));
  EXPECT_EQ(value, 314);
  EXPECT_EQ(text, before);
}

TEST(ArgcvValueTest, S02ExactSubslice) {
  for (const size_t len : {2u, 3u}) {
    SCOPED_TRACE(len);
    std::array<char, 4> text = {'q', '4', '2', 'q'};
    const auto before = text;
    uint16_t value = kUnchanged;
    EXPECT_EQ(argcv_parse_u16(text.data() + 1, len, &value), len == 2);
    EXPECT_EQ(value, len == 2 ? 42 : kUnchanged);
    EXPECT_EQ(text, before);
  }
}

TEST(ArgcvValueTest, S03OnlyWriteOutputObject) {
  for (const bool success : {true, false}) {
    SCOPED_TRACE(success);
    const std::string before = success ? "42" : "65536";
    // Keep a distinct writable buffer to detect accidental input writes.
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    std::string text = before;
    std::array<uint16_t, 3> values = {111, kUnchanged, 222};
    auto expected = values;
    if (success) {
      expected[1] = 42;
    }
    EXPECT_EQ(argcv_parse_u16(text.data(), text.size(), &values[1]), success);
    EXPECT_EQ(values, expected);
    EXPECT_EQ(text, before);
  }
}

TEST(ArgcvValueTest, S04ReuseOutputAfterSuccessAndFailure) {
  const ParseU16Case steps[] = {
      {"success", "42", true, 42},
      {"failure", "+42", false, 42},
      {"zero", "0", true, 0},
  };
  uint16_t value = kUnchanged;
  for (const auto &step : steps) {
    SCOPED_TRACE(step.id);
    const auto &before = step.text;
    std::string text = before;
    EXPECT_EQ(argcv_parse_u16(text.data(), text.size(), &value), step.success);
    EXPECT_EQ(value, step.expected);
    EXPECT_EQ(text, before);
  }
}

TEST(ArgcvValueTest, S05IgnoreDigitBeyondSlice) {
  std::array<char, 3> text = {'4', '2', '9'};
  const auto before = text;
  uint16_t value = kUnchanged;
  EXPECT_TRUE(argcv_parse_u16(text.data(), 2, &value));
  EXPECT_EQ(value, 42);
  EXPECT_EQ(text, before);
}

struct HostportSlices {
  size_t host_offset;
  size_t host_len;
  size_t port_offset;
  size_t port_len;
  bool host_requires_ipv6;
};

constexpr argcv_hostport_parts_t kUnchangedHostport = {"old-host", 8,
                                                       "old-port", 8, true};

template <size_t N>
void ExpectSplitHostport(char (&text)[N], size_t len, bool success,
                         const HostportSlices &expected,
                         argcv_hostport_parts_t &out) {
  ASSERT_LE(len, N);
  // Preserve the entire writable array, including bytes outside the slice.
  const std::string before(text, N);
  const auto previous = out;
  SCOPED_TRACE(before.substr(0, len));
  const bool result = argcv_split_hostport(text, len, &out);
  EXPECT_EQ(std::string(text, N), before);
  ASSERT_EQ(result, success);
  if (!result) {
    EXPECT_EQ(static_cast<const void *>(out.host),
              static_cast<const void *>(previous.host));
    EXPECT_EQ(out.host_len, previous.host_len);
    EXPECT_EQ(static_cast<const void *>(out.port),
              static_cast<const void *>(previous.port));
    EXPECT_EQ(out.port_len, previous.port_len);
    EXPECT_EQ(out.host_requires_ipv6, previous.host_requires_ipv6);
    return;
  }

  // Compare addresses as void pointers so a failure cannot make GoogleTest
  // print an unterminated slice as a C string.
  ASSERT_EQ(static_cast<const void *>(out.host),
            static_cast<const void *>(text + expected.host_offset));
  ASSERT_EQ(out.host_len, expected.host_len);
  const char *port =
      expected.port_len == 0 ? nullptr : text + expected.port_offset;
  ASSERT_EQ(static_cast<const void *>(out.port),
            static_cast<const void *>(port));
  ASSERT_EQ(out.port_len, expected.port_len);
  EXPECT_EQ(out.host_requires_ipv6, expected.host_requires_ipv6);
  EXPECT_GT(out.host_len, 0u);
  EXPECT_EQ(out.port != nullptr, out.port_len > 0);
  EXPECT_EQ(std::string(out.host, out.host_len),
            before.substr(expected.host_offset, expected.host_len));
  if (out.port != nullptr) {
    EXPECT_EQ(std::string(out.port, out.port_len),
              before.substr(expected.port_offset, expected.port_len));
  }
}

template <size_t N>
void ExpectSplitHostport(char (&text)[N], size_t len, bool success,
                         const HostportSlices &expected = {}) {
  auto out = kUnchangedHostport;
  ExpectSplitHostport(text, len, success, expected, out);
}

struct SplitHostportCase {
  const char *id;
  // Fits every RFC string plus its terminator; tests copy this writable array.
  char text[18];
  size_t len;
  bool success;
  HostportSlices expected;
};

class ArgcvSplitHostportRfcTest
    : public testing::TestWithParam<SplitHostportCase> {};

// TEST_P generates a public override of GoogleTest's private TestBody method.
// NOLINTNEXTLINE(misc-override-with-different-visibility)
TEST_P(ArgcvSplitHostportRfcTest, RfcCase) {
  auto test = GetParam();
  ExpectSplitHostport(test.text, test.len, test.success, test.expected);
}

INSTANTIATE_TEST_SUITE_P(
    Rfc, ArgcvSplitHostportRfcTest,
    testing::ValuesIn<SplitHostportCase>({
        {"V01MinimalHost", "a", 1, true, {0, 1, 0, 0, false}},
        {"V02MinimalBracketedHost", "[a]", 3, true, {1, 1, 0, 0, true}},
        {"V03HostOnly", "host", 4, true, {0, 4, 0, 0, false}},
        {"V04HostPort", "host:80", 7, true, {0, 4, 5, 2, false}},
        {"V05BracketedHost", "[::1]", 5, true, {1, 3, 0, 0, true}},
        {"V06BracketedHostPort", "[::1]:443", 9, true, {1, 3, 6, 3, true}},
        {"V07TwoColons", "::", 2, true, {0, 2, 0, 0, true}},
        {"V08BareIpv6", "::1", 3, true, {0, 3, 0, 0, true}},
        {"V09NoGuessedPort", "::1:443", 7, true, {0, 7, 0, 0, true}},
        {"V10HexHostPort", "face:443", 8, true, {0, 4, 5, 3, false}},
        {"V11HexIpv6", "face::1", 7, true, {0, 7, 0, 0, true}},
        {"V12ColonsInPort", "[::1]:4:4:3", 11, true, {1, 3, 6, 5, true}},
        {"V13ColonPort", "[a]::", 5, true, {1, 1, 4, 1, true}},
        {"V14MultipleColons", "host:4:4:3", 10, true, {0, 10, 0, 0, true}},
        {"V15TrailingColonIpv6", "::1:", 4, true, {0, 4, 0, 0, true}},
        {"V16NondigitPort", "host:abc", 8, true, {0, 4, 5, 3, false}},
        {"V17OutOfRangePort", "host:65536", 10, true, {0, 4, 5, 5, false}},
        {"V18LeadingZeros", "host:000443", 11, true, {0, 4, 5, 6, false}},
        {"V19ZeroPort", "host:0", 6, true, {0, 4, 5, 1, false}},
        {"V20Whitespace", " : ", 3, true, {0, 1, 2, 1, false}},
        {"V21BracketedHostname",
         "[example.com]:443",
         17,
         true,
         {1, 11, 14, 3, true}},
        {"V22ClosingBracketInHost", "ho]st:443", 9, true, {0, 5, 6, 3, false}},
        {"V23BracketsInPort", "h:[x]", 5, true, {0, 1, 2, 3, false}},
        {"V24NonRecursiveBrackets", "[a[b]:443", 9, true, {1, 3, 6, 3, true}},
        {"E01EmptyInput", "", 0, false, {}},
        {"E02OnlyColon", ":", 1, false, {}},
        {"E03EmptyHost", ":443", 4, false, {}},
        {"E04EmptyBracketedHost", "[]", 2, false, {}},
        {"E05EmptyBracketedHostWithPort", "[]:443", 6, false, {}},
        {"E06EmptyPort", "host:", 5, false, {}},
        {"E07EmptyBracketedPort", "[::1]:", 6, false, {}},
        {"E08OnlyOpeningBracket", "[", 1, false, {}},
        {"E09MissingClosingBracket", "[::1", 4, false, {}},
        {"E10UnexpectedSuffix", "[::1]443", 8, false, {}},
        {"E11ExtraClosingBracket", "[::1]]:443", 10, false, {}},
        {"L01EmptyPrefix", "[::1]:443", 0, false, {}},
        {"L02OpeningBracketPrefix", "[::1]:443", 1, false, {}},
        {"L03ExcludeClosingBracket", "[::1]:443", 4, false, {}},
        {"L04IncludeClosingBracket", "[::1]:443", 5, true, {1, 3, 0, 0, true}},
        {"L05ExcludePort", "[::1]:443", 6, false, {}},
        {"L06PartialPort", "[::1]:443", 7, true, {1, 3, 6, 1, true}},
        {"L07WholePort", "[::1]:443", 9, true, {1, 3, 6, 3, true}},
        {"L08ExcludeFirstColon", "h:4:4", 1, true, {0, 1, 0, 0, false}},
        {"L09IncludeFirstColon", "h:4:4", 2, false, {}},
        {"L10ExcludeSecondColon", "h:4:4", 3, true, {0, 1, 2, 1, false}},
        {"L11IncludeSecondColon", "h:4:4", 5, true, {0, 5, 0, 0, true}},
        {"L12SingleColonPrefix", "::1", 1, false, {}},
        {"L13TwoColonPrefix", "::1", 2, true, {0, 2, 0, 0, true}},
        {"L14WholeIpv6", "::1", 3, true, {0, 3, 0, 0, true}},
    }),
    [](const testing::TestParamInfo<SplitHostportCase> &info) {
      return info.param.id;
    });

TEST(ArgcvSplitHostportTest, BracketWithoutTerminator) {
  char text[] = {'['};
  ExpectSplitHostport(text, sizeof(text), false);
}

TEST(ArgcvSplitHostportTest, NulIsNonemptyBracketedHost) {
  char text[] = {'[', '\0', ']'};
  ExpectSplitHostport(text, sizeof(text), true, {1, 1, 0, 0, true});
}

TEST(ArgcvSplitHostportTest, HostPortWithoutTerminator) {
  char text[] = {'h', ':', '4', '4', '3'};
  ExpectSplitHostport(text, sizeof(text), true, {0, 1, 2, 3, false});
}

TEST(ArgcvSplitHostportTest, EmbeddedNulInHost) {
  char text[] = {'h', '\0', ':', '4', '4', '3'};
  ExpectSplitHostport(text, sizeof(text), true, {0, 2, 3, 3, false});
}

TEST(ArgcvSplitHostportTest, EmbeddedNulInPort) {
  char text[] = {'h', ':', '4', '\0', '3'};
  ExpectSplitHostport(text, sizeof(text), true, {0, 1, 2, 3, false});
}

TEST(ArgcvSplitHostportTest, NulIsNonemptyPort) {
  char text[] = {'h', ':', '\0'};
  ExpectSplitHostport(text, sizeof(text), true, {0, 1, 2, 1, false});
}

TEST(ArgcvSplitHostportTest, SecondColonAfterNul) {
  char text[] = {'h', ':', '4', '\0', ':'};
  ExpectSplitHostport(text, sizeof(text), true, {0, 5, 0, 0, true});
}

TEST(ArgcvSplitHostportTest, NulAfterClosingBracketIsInvalidSuffix) {
  char text[] = {'[', ':', ':', '1', ']', '\0'};
  ExpectSplitHostport(text, sizeof(text), false);
}

TEST(ArgcvSplitHostportTest, ReuseOutputAfterSuccessAndFailure) {
  // All input storage stays alive while out may still refer to it.
  char bracketed_port[] = "[::1]:443";
  char empty_port[] = "host:";
  char ipv6[] = "::1";
  char host_port[] = "host:80";
  char missing_bracket[] = "[::1";
  char host[] = "host";
  char empty_bracketed_port[] = "[::1]:";
  char bracketed_host[] = "[::1]";
  auto out = kUnchangedHostport;

  ExpectSplitHostport(bracketed_port, 9, true, {1, 3, 6, 3, true}, out);
  ExpectSplitHostport(empty_port, 5, false, {}, out);
  ExpectSplitHostport(ipv6, 3, true, {0, 3, 0, 0, true}, out);
  ExpectSplitHostport(host_port, 7, true, {0, 4, 5, 2, false}, out);
  ExpectSplitHostport(missing_bracket, 4, false, {}, out);
  ExpectSplitHostport(host, 4, true, {0, 4, 0, 0, false}, out);
  ExpectSplitHostport(empty_bracketed_port, 6, false, {}, out);
  ExpectSplitHostport(bracketed_port, 9, true, {1, 3, 6, 3, true}, out);
  ExpectSplitHostport(host_port, 7, true, {0, 4, 5, 2, false}, out);
  ExpectSplitHostport(bracketed_host, 5, true, {1, 3, 0, 0, true}, out);
}

void ExpectValidateIpv6(const char *buffer, size_t buffer_len, size_t offset,
                        size_t len, bool success) {
  ASSERT_LT(offset, buffer_len);
  ASSERT_LE(len, buffer_len - offset);
  // Snapshot all storage, including any terminator and out-of-slice bytes.
  const std::string before(buffer, buffer_len);
  SCOPED_TRACE(before.substr(offset, len));
  EXPECT_EQ(argcv_validate_ipv6(buffer + offset, len), success);
  EXPECT_EQ(std::string(buffer, buffer_len), before);
}

template <size_t N>
void ExpectValidateIpv6(char (&buffer)[N], size_t offset, size_t len,
                        bool success) {
  ExpectValidateIpv6(buffer, N, offset, len, success);
}

void ExpectValidateIpv6(std::string &text, bool success) {
  // Include the string's terminator in the snapshot, but not in the slice.
  ExpectValidateIpv6(text.data(), text.size() + 1, 0, text.size(), success);
}

struct ValidateIpv6Case {
  const char *id;
  char text[46];
  size_t len;
  bool success;
};

class ArgcvValidateIpv6RfcTest
    : public testing::TestWithParam<ValidateIpv6Case> {};

// TEST_P generates a public override of GoogleTest's private TestBody method.
// NOLINTNEXTLINE(misc-override-with-different-visibility)
TEST_P(ArgcvValidateIpv6RfcTest, RfcCase) {
  auto test = GetParam();
  ExpectValidateIpv6(test.text, 0, test.len, test.success);
}

// V01, M02, E02, and X22 use exact-size objects below for ASan. Compression
// representatives V10-V12, M05-M07, E10-E12, and X15-X16 are covered by
// G01/G02.
INSTANTIATE_TEST_SUITE_P(
    Rfc, ArgcvValidateIpv6RfcTest,
    testing::ValuesIn<ValidateIpv6Case>({
        {"V02", "::1", 3, true},
        {"V03", "2001:db8::1", 11, true},
        {"V04", "1:2:3:4:5:6:7:8", 15, true},
        {"V05", "0:0:0:0:0:0:0:0", 15, true},
        {"V06", "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", 39, true},
        {"V07", "2001:0DB8:0000:0000:0000:0000:0000:0001", 39, true},
        {"V08", "a:B:c:D:e:F:0:1", 15, true},
        {"V09", "0:00:000:0000:1:01:001:0001", 27, true},
        {"V13", "fe80::1", 7, true},
        {"V14", "ff02::1", 7, true},
        {"V15", "::1:443", 7, true},
        {"M01", "::192.0.2.1", 11, true},
        {"M03", "0:0:0:0:0:ffff:192.0.2.1", 24, true},
        {"M04", "1:2:3:4:5:6:192.0.2.1", 21, true},
        {"M08", "::ffff:0.9.10.99", 16, true},
        {"M09", "::ffff:100.199.249.255", 22, true},
        {"M10", "::ffff:255.255.255.255", 22, true},
        {"M11", "ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255", 45, true},
        {"M12", "::0.0.0.0", 9, true},
        {"M13", "2001:db8::192.0.2.1", 19, true},
        {"E01", "", 0, false},
        {"E03", ":1", 2, false},
        {"E04", "1:", 2, false},
        {"E05", ":::", 3, false},
        {"E06", "1:::2", 5, false},
        {"E07", "1::2::3", 7, false},
        {"E08", "1:2:3:4:5:6:7", 13, false},
        {"E09", "1:2:3:4:5:6:7:8:9", 17, false},
        {"E13", "12345::", 7, false},
        {"E14", "::00000", 7, false},
        {"E15", "2001:db8::g", 11, false},
        {"E16", "0x1::", 5, false},
        {"E17", "+1::", 4, false},
        {"E18", "-1::", 4, false},
        {"E19", " ::1", 4, false},
        {"E20", "::1 ", 4, false},
        {"E21", ":: 1", 4, false},
        {"E22", "\t::1", 4, false},
        {"E23", "::1\r\n", 5, false},
        {"E24", "::1junk", 7, false},
        {"E25", "[::1]", 5, false},
        {"E26", "[::1]:443", 9, false},
        {"E27", "2001:db8::/64", 13, false},
        {"E28", "fe80::1%eth0", 12, false},
        {"E29", "fe80::1%3", 9, false},
        {"E30", "fe80::1%25eth0", 14, false},
        {"E31", "v1.fe80", 7, false},
        {"E32", "192.0.2.1", 9, false},
        {"E33", "example.com", 11, false},
        {"E34", "1", 1, false},
        {"E35", "::1:", 4, false},
        {"E36", ":1::", 4, false},
        {"X01", "::ffff:256.0.2.1", 16, false},
        {"X02", "::ffff:192.0.2.256", 18, false},
        {"X03", "::ffff:192.00.2.1", 17, false},
        {"X04", "::ffff:192.0.2.01", 17, false},
        {"X05", "::ffff:000.0.0.0", 16, false},
        {"X06", "::ffff:192.0.2", 14, false},
        {"X07", "::ffff:192.0.2.1.5", 18, false},
        {"X08", "::ffff:192..2.1", 15, false},
        {"X09", "::ffff:.192.0.2.1", 17, false},
        {"X10", "::ffff:192.0.2.1.", 17, false},
        {"X11", "::ffff:192.0.2.1:1", 18, false},
        {"X12", "192.0.2.1::", 11, false},
        {"X13", "1:2:3:4:5:192.0.2.1", 19, false},
        {"X14", "1:2:3:4:5:6:7:192.0.2.1", 23, false},
        {"X17", "::ffff:0xc0.0.2.1", 17, false},
        {"X18", "::ffff:192.0.2.+1", 17, false},
        {"X19", "::ffff:192.0.2.-1", 17, false},
        {"X20", "::ffff:192.0.2.4294967296", 25, false},
        {"X21", "::ffff:192.0.2.a", 16, false},
    }),
    [](const testing::TestParamInfo<ValidateIpv6Case> &info) {
      return info.param.id;
    });

TEST(ArgcvValidateIpv6Test, E02SingleColonWithoutTerminator) {
  char text[] = {':'};
  ExpectValidateIpv6(text, 0, sizeof(text), false);
}

TEST(ArgcvValidateIpv6Test, V01CompressionWithoutTerminator) {
  char text[] = {':', ':'};
  ExpectValidateIpv6(text, 0, sizeof(text), true);
}

TEST(ArgcvValidateIpv6Test, M02Ipv4TailWithoutTerminator) {
  char text[] = {':', ':', 'f', 'f', 'f', 'f', ':', '1',
                 '9', '2', '.', '0', '.', '2', '.', '1'};
  ExpectValidateIpv6(text, 0, sizeof(text), true);
}

TEST(ArgcvValidateIpv6Test, X22EmptyFourthOctetWithoutTerminator) {
  char text[] = {':', ':', 'f', 'f', 'f', 'f', ':', '1',
                 '9', '2', '.', '0', '.', '2', '.'};
  ExpectValidateIpv6(text, 0, sizeof(text), false);
}

TEST(ArgcvValidateIpv6Test, S01NonTerminatedSlice) {
  char text[] = {':', ':', '1'};
  ExpectValidateIpv6(text, 0, sizeof(text), true);
}

TEST(ArgcvValidateIpv6Test, S02ToS04ExactSubslice) {
  char text[] = {'x', ':', ':', '1', 'y'};
  ExpectValidateIpv6(text, 1, 3, true);
  ExpectValidateIpv6(text, 1, 4, false);
  ExpectValidateIpv6(text, 0, 4, false);
}

TEST(ArgcvValidateIpv6Test, S05EmptySlice) {
  char text[] = {'x'};
  ExpectValidateIpv6(text, 0, 0, false);
}

TEST(ArgcvValidateIpv6Test, S06WhitespaceAndNulOutsideSlice) {
  char text[] = {' ', ':', ':', '1', '\0'};
  ExpectValidateIpv6(text, 1, 3, true);
}

TEST(ArgcvValidateIpv6Test, S07ToS09NulInsideSlice) {
  char end[] = {':', ':', '1', '\0'};
  char middle[] = {':', '\0', ':', '1'};
  char start[] = {'\0', ':', ':', '1'};
  ExpectValidateIpv6(end, 0, sizeof(end), false);
  ExpectValidateIpv6(middle, 0, sizeof(middle), false);
  ExpectValidateIpv6(start, 0, sizeof(start), false);
}

TEST(ArgcvValidateIpv6Test, S10S11NonHexByteAtBoundary) {
  char text[] = {':', ':', '1', 'g'};
  ExpectValidateIpv6(text, 0, 3, true);
  ExpectValidateIpv6(text, 0, 4, false);
}

TEST(ArgcvValidateIpv6Test, S12S13ColonAtBoundary) {
  char text[] = {':', ':', '1', ':'};
  ExpectValidateIpv6(text, 0, 3, true);
  ExpectValidateIpv6(text, 0, 4, false);
}

TEST(ArgcvValidateIpv6Test, S14S15FifthHexDigitAtBoundary) {
  char text[] = {':', ':', '1', '2', '3', '4', '5'};
  ExpectValidateIpv6(text, 0, 6, true);
  ExpectValidateIpv6(text, 0, 7, false);
}

TEST(ArgcvValidateIpv6Test, S16FullwidthDigit) {
  char text[] = {static_cast<char>(0xEF), static_cast<char>(0xBC),
                 static_cast<char>(0x91), ':', ':'};
  ExpectValidateIpv6(text, 0, sizeof(text), false);
}

TEST(ArgcvValidateIpv6Test, S17HighBitByte) {
  char text[] = {':', ':', static_cast<char>(0xFF)};
  ExpectValidateIpv6(text, 0, sizeof(text), false);
}

TEST(ArgcvValidateIpv6Test, S18HostInsideEndpoint) {
  char text[] = {'x', '[', ':', ':', '1', ']', ':', '8', '0', 'y'};
  ExpectValidateIpv6(text, 2, 3, true);
}

TEST(ArgcvValidateIpv6Test, S19NulAfterIpv4Tail) {
  char text[] = {':', ':', 'f', 'f', 'f', 'f', ':', '1', '9',
                 '2', '.', '0', '.', '2', '.', '1', '\0'};
  ExpectValidateIpv6(text, 0, sizeof(text), false);
}

TEST(ArgcvValidateIpv6Test, S20S21OctetDigitAtBoundary) {
  char text[] = {':', ':', 'f', 'f', 'f', 'f', ':', '1',
                 '.', '2', '.', '3', '.', '2', '5', '6'};
  ExpectValidateIpv6(text, 0, 15, true);
  ExpectValidateIpv6(text, 0, 16, false);
}

TEST(ArgcvValidateIpv6Test, S22S23FourthOctetAtBoundary) {
  char text[] = {':', ':', 'f', 'f', 'f', 'f', ':',
                 '1', '.', '2', '.', '3', '.', '4'};
  ExpectValidateIpv6(text, 0, 12, false);
  ExpectValidateIpv6(text, 0, 14, true);
}

std::string JoinIpv6Groups(size_t count, char digit) {
  std::string text;
  for (size_t i = 0; i < count; ++i) {
    if (i != 0) {
      text += ':';
    }
    text += digit;
  }
  return text;
}

TEST(ArgcvValidateIpv6Test, G01CompressedHexGroups) {
  for (size_t i = 0; i <= 9; ++i) {
    for (size_t j = 0; i + j <= 9; ++j) {
      std::string text = JoinIpv6Groups(i, '1') + "::" + JoinIpv6Groups(j, '2');
      ExpectValidateIpv6(text, i + j <= 7);
    }
  }
}

TEST(ArgcvValidateIpv6Test, G02CompressedGroupsWithIpv4Tail) {
  for (size_t i = 0; i <= 7; ++i) {
    for (size_t j = 0; i + j <= 7; ++j) {
      std::string text = JoinIpv6Groups(i, '1') + "::";
      if (j != 0) {
        text += JoinIpv6Groups(j, '2') + ':';
      }
      text += "192.0.2.1";
      ExpectValidateIpv6(text, i + j <= 5);
    }
  }
}

TEST(ArgcvValidateIpv6Test, G03EveryByteValue) {
  const std::string hex_digits = "0123456789abcdefABCDEF";
  for (unsigned int byte = 0; byte <= 0xFF; ++byte) {
    SCOPED_TRACE(byte);
    char text[] = {':', ':', static_cast<char>(byte)};
    ExpectValidateIpv6(text, 0, sizeof(text),
                       hex_digits.find(text[2]) != std::string::npos);
  }
}

TEST(ArgcvValidateIpv6Test, G04EveryOctetPositionAndNumericBoundary) {
  const struct {
    const char *text;
    bool success;
  } spellings[] = {
      {"0", true},   {"9", true},   {"10", true},   {"99", true},
      {"100", true}, {"199", true}, {"200", true},  {"249", true},
      {"250", true}, {"255", true}, {"256", false}, {"999", false},
      {"00", false}, {"01", false}, {"001", false}, {"0000", false},
      {"+1", false}, {"-1", false}, {"0x1", false}, {"4294967296", false},
  };
  for (size_t position = 0; position < 4; ++position) {
    for (const auto &spelling : spellings) {
      std::array<std::string, 4> octets = {"1", "2", "3", "4"};
      octets[position] = spelling.text;
      std::string text = "::ffff:" + octets[0] + '.' + octets[1] + '.' +
                         octets[2] + '.' + octets[3];
      ExpectValidateIpv6(text, spelling.success);
    }
  }
}

TEST(ArgcvValidateIpv6Test, G05OverlongInputs) {
  for (auto text :
       {std::string(1024, '0') + "::", "::ffff:1.2.3." + std::string(1024, '9'),
        std::string("ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255x")}) {
    ExpectValidateIpv6(text, false);
  }
}

TEST(ArgcvValidateIpv6Test, R01IndependentCalls) {
  char loopback[] = "::1";
  char invalid_colons[] = ":::";
  char unspecified[] = "::";
  char invalid_tail[] = "::ffff:192.0.2.01";
  char valid_tail[] = "::ffff:192.0.2.1";
  for (int pass = 0; pass < 2; ++pass) {
    SCOPED_TRACE(pass);
    ExpectValidateIpv6(loopback, 0, sizeof(loopback) - 1, true);
    ExpectValidateIpv6(invalid_colons, 0, sizeof(invalid_colons) - 1, false);
    ExpectValidateIpv6(unspecified, 0, sizeof(unspecified) - 1, true);
    ExpectValidateIpv6(invalid_tail, 0, sizeof(invalid_tail) - 1, false);
    ExpectValidateIpv6(valid_tail, 0, sizeof(valid_tail) - 1, true);
  }
}

} // namespace
