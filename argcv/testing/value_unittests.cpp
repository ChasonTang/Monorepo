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

} // namespace
