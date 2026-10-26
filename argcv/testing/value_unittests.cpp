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

} // namespace
