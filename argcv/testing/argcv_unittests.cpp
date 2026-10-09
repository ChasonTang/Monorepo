// Include first to verify that C++ callers need no prerequisite headers or
// external extern "C" wrapper. This file links against the C implementation.
#include "argcv/argcv.h"

#include <atomic>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace {

constexpr argcv_option_t kOptions[] = {
    {"verbose", false}, {"host", true}, {"port", true}};
constexpr size_t kOptionCount = sizeof(kOptions) / sizeof(kOptions[0]);

// Own writable, distinct string storage and the required trailing null pointer.
// Construct pointers only after the strings have reached their final locations.
class Arguments {
public:
  explicit Arguments(const std::vector<const char *> &values) : strings{"app"} {
    for (const char *value : values) {
      strings.emplace_back(value);
    }
    for (auto &value : strings) {
      pointers.push_back(value.data());
    }
    pointers.push_back(nullptr);
  }

  Arguments(std::initializer_list<const char *> values)
      : Arguments(std::vector<const char *>(values)) {}

  Arguments(const Arguments &) = delete;
  Arguments &operator=(const Arguments &) = delete;

  int argc() const { return static_cast<int>(strings.size()); }

  // Tests inspect both buffers to verify that parsing leaves input unchanged.
  // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
  std::vector<std::string> strings;
  std::vector<char *> pointers;
  // NOLINTEND(misc-non-private-member-variables-in-classes)
};

void ExpectError(const argcv_error_t &error, size_t option_index = SIZE_MAX,
                 int argv_index = -1) {
  EXPECT_EQ(error.option_index, option_index);
  EXPECT_EQ(error.argv_index, argv_index);
}

void ExpectResults(const argcv_result_t *results, const Arguments &args,
                   bool verbose = false, int host = -1, int port = -1) {
  EXPECT_EQ(results[0].present, verbose);
  EXPECT_EQ(results[1].value, host < 0 ? nullptr : args.pointers[host]);
  EXPECT_EQ(results[2].value, port < 0 ? nullptr : args.pointers[port]);
}

struct Expected {
  argcv_status_t status;
  size_t option_index;
  int argv_index;
  bool verbose;
  int host;
  int port;
};

constexpr Expected Ok(bool verbose = false, int host = -1, int port = -1) {
  return {ARGCV_OK, SIZE_MAX, -1, verbose, host, port};
}

constexpr Expected Error(argcv_status_t status, size_t option_index,
                         int argv_index) {
  return {status, option_index, argv_index, false, -1, -1};
}

struct ParseCase {
  const char *id;
  std::vector<const char *> arguments;
  Expected expected;
};

class ArgcvParsingTest : public testing::TestWithParam<ParseCase> {};

// TEST_P generates a public override of GoogleTest's private TestBody method.
// NOLINTNEXTLINE(misc-override-with-different-visibility)
TEST_P(ArgcvParsingTest, RfcCase) {
  const auto &test = GetParam();
  Arguments args(test.arguments);
  argcv_result_t results[kOptionCount];
  argcv_error_t error;
  ASSERT_EQ(argcv_parse(args.argc(), args.pointers.data(), kOptions,
                        kOptionCount, results, &error),
            test.expected.status);
  ExpectError(error, test.expected.option_index, test.expected.argv_index);
  if (test.expected.status == ARGCV_OK) {
    ExpectResults(results, args, test.expected.verbose, test.expected.host,
                  test.expected.port);
  }
}

INSTANTIATE_TEST_SUITE_P(
    Rfc, ArgcvParsingTest,
    testing::ValuesIn<ParseCase>({
        {"R02", {}, Ok()},
        {"P01", {"--verbose"}, Ok(true)},
        {"P02", {"--host", "localhost"}, Ok(false, 2)},
        {"P03",
         {"--verbose", "--host", "localhost", "--port", "8080"},
         Ok(true, 3, 5)},
        {"P04",
         {"--port", "8080", "--host", "localhost", "--verbose"},
         Ok(true, 4, 2)},
        {"P05", {"--port", "invalid-port"}, Ok(false, -1, 2)},
        {"P06", {"--host", ""}, Ok(false, 2)},
        {"P07", {"--host", " a b "}, Ok(false, 2)},
        {"P08", {"--port", "-1"}, Ok(false, -1, 2)},
        {"P09", {"--host", "-"}, Ok(false, 2)},
        {"P10", {"--host", "--", "--verbose"}, Ok(true, 2)},
        {"P11", {"--host", "--host=x"}, Ok(false, 2)},
        {"P12", {"--host", "-v"}, Ok(false, 2)},
        {"P13", {"--host", "--Host"}, Ok(false, 2)},
        {"P14", {"--host", "---host"}, Ok(false, 2)},
        {"P15", {"--host", "--ver"}, Ok(false, 2)},
        {"P16", {"--host", "verbose"}, Ok(false, 2)},
        {"P17", {"--host", "主机"}, Ok(false, 2)},
        {"P18", {"--host", "--unknown", "--verbose"}, Ok(true, 2)},
        {"P19", {"--host", "--verbose"}, Error(ARGCV_MISSING_VALUE, 1, 1)},
        {"P20", {"--host", "--port"}, Error(ARGCV_MISSING_VALUE, 1, 1)},
        {"P21", {"--host", "--host"}, Error(ARGCV_MISSING_VALUE, 1, 1)},
        {"P22", {"--host"}, Error(ARGCV_MISSING_VALUE, 1, 1)},
        {"P23",
         {"--host", "a", "--port", "--host"},
         Error(ARGCV_MISSING_VALUE, 2, 3)},
        {"P24", {"--verbose", "--verbose"}, Ok(true)},
        {"P25", {"--host", "a", "--host", "b"}, Ok(false, 4)},
        {"P27", {"--host", "", "--host", "b"}, Ok(false, 4)},
        {"P28", {"--host", "a", "--host", ""}, Ok(false, 4)},
        {"P29", {"--host", "a", "--host", "--unknown"}, Ok(false, 4)},
        {"P30",
         {"--host", "a", "--verbose", "--host", "b", "--verbose", "--host",
          "c"},
         Ok(true, 8)},
        {"E01",
         {"--unknown", "value"},
         Error(ARGCV_UNKNOWN_OPTION, SIZE_MAX, 1)},
        {"E02",
         {"--verbose", "--host", "localhost", "--unknown", "value"},
         Error(ARGCV_UNKNOWN_OPTION, SIZE_MAX, 4)},
        {"E03", {"file.txt"}, Error(ARGCV_UNKNOWN_OPTION, SIZE_MAX, 1)},
        {"E04",
         {"--verbose", "true"},
         Error(ARGCV_UNKNOWN_OPTION, SIZE_MAX, 2)},
        {"E05", {"-v"}, Error(ARGCV_UNKNOWN_OPTION, SIZE_MAX, 1)},
        {"E06", {"--"}, Error(ARGCV_UNKNOWN_OPTION, SIZE_MAX, 1)},
        {"E07", {""}, Error(ARGCV_UNKNOWN_OPTION, SIZE_MAX, 1)},
        {"E08", {"--host=x"}, Error(ARGCV_UNKNOWN_OPTION, SIZE_MAX, 1)},
        {"E09", {"--ver"}, Error(ARGCV_UNKNOWN_OPTION, SIZE_MAX, 1)},
        {"E10", {"--Host"}, Error(ARGCV_UNKNOWN_OPTION, SIZE_MAX, 1)},
        {"E11", {"---host"}, Error(ARGCV_UNKNOWN_OPTION, SIZE_MAX, 1)},
        {"E13",
         {"--host", "--unknown", "value"},
         Error(ARGCV_UNKNOWN_OPTION, SIZE_MAX, 3)},
        {"E14", {"--host", "a", "--host"}, Error(ARGCV_MISSING_VALUE, 1, 3)},
        {"E15",
         {"--host", "a", "--host", "--verbose"},
         Error(ARGCV_MISSING_VALUE, 1, 3)},
        {"O05",
         {"--unknown", "--host"},
         Error(ARGCV_UNKNOWN_OPTION, SIZE_MAX, 1)},
        {"O06",
         {"--host", "--verbose", "--unknown"},
         Error(ARGCV_MISSING_VALUE, 1, 1)},
        {"O07",
         {"--verbose", "--host", "--verbose"},
         Error(ARGCV_MISSING_VALUE, 1, 2)},
        {"O08",
         {"--host", "--verbose", "--host", "a"},
         Error(ARGCV_MISSING_VALUE, 1, 1)},
    }),
    [](const testing::TestParamInfo<ParseCase> &info) {
      return info.param.id;
    });

struct DescriptorCase {
  const char *id;
  std::vector<argcv_option_t> options;
  std::vector<const char *> arguments;
  argcv_status_t status;
  size_t option_index;
};

class ArgcvDescriptorTest : public testing::TestWithParam<DescriptorCase> {};

// TEST_P generates a public override of GoogleTest's private TestBody method.
// NOLINTNEXTLINE(misc-override-with-different-visibility)
TEST_P(ArgcvDescriptorTest, RfcCase) {
  const auto &test = GetParam();
  Arguments args(test.arguments);
  std::vector<argcv_result_t> results(test.options.size());
  argcv_error_t error;
  ASSERT_EQ(argcv_parse(args.argc(), args.pointers.data(), test.options.data(),
                        test.options.size(), results.data(), &error),
            test.status);
  ExpectError(error, test.option_index);
  if (test.status == ARGCV_OK) {
    for (size_t i = 0; i < test.options.size(); ++i) {
      if (test.options[i].requires_value) {
        EXPECT_EQ(results[i].value, nullptr);
      } else {
        EXPECT_FALSE(results[i].present);
      }
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    Rfc, ArgcvDescriptorTest,
    testing::ValuesIn<DescriptorCase>({
        {"R04", {{"-host", false}}, {}, ARGCV_INVALID_NAME, 0},
        {"R05", {{"", false}}, {}, ARGCV_INVALID_NAME, 0},
        {"R06", {{"--host", false}}, {}, ARGCV_INVALID_NAME, 0},
        {"R07", {{"Host", false}}, {}, ARGCV_INVALID_NAME, 0},
        {"R08", {{"1host", false}}, {}, ARGCV_INVALID_NAME, 0},
        {"R09", {{"dry_run", false}}, {}, ARGCV_INVALID_NAME, 0},
        {"R10", {{"host=x", false}}, {}, ARGCV_INVALID_NAME, 0},
        {"R11", {{"host name", false}}, {}, ARGCV_INVALID_NAME, 0},
        {"R12", {{"主机", false}}, {}, ARGCV_INVALID_NAME, 0},
        {"R13",
         {{"verbose", false}, {"verbose", false}},
         {},
         ARGCV_DUPLICATE_NAME,
         1},
        {"R14",
         {{"a", false}, {"b", false}, {"c", false}, {"b", true}},
         {},
         ARGCV_DUPLICATE_NAME,
         3},
        {"R15",
         {{"a1", false}, {"a", false}, {"z09-", false}, {"a--b", false}},
         {},
         ARGCV_OK,
         SIZE_MAX},
        {"R16", {{"ok", false}, {"aZ", true}}, {}, ARGCV_INVALID_NAME, 1},
        {"R17", {{"ok", false}, {"a中", true}}, {}, ARGCV_INVALID_NAME, 1},
        {"O01",
         {{"host", true}, {"host", true}},
         {"--unknown"},
         ARGCV_DUPLICATE_NAME,
         1},
        {"O02", {{"", false}}, {"--unknown"}, ARGCV_INVALID_NAME, 0},
        {"O03",
         {{"host", true}, {"host", true}, {"", false}},
         {},
         ARGCV_DUPLICATE_NAME,
         1},
        {"O04",
         {{"host", true}, {"", false}, {"host", true}},
         {},
         ARGCV_INVALID_NAME,
         1},
    }),
    [](const testing::TestParamInfo<DescriptorCase> &info) {
      return info.param.id;
    });

TEST(ArgcvTest, R01ZeroDescriptors) {
  Arguments args({});
  const argcv_option_t placeholder = {"unused", false};
  const char sentinel[] = "unchanged";
  argcv_result_t result;
  result.value = sentinel;
  argcv_error_t error;
  ASSERT_EQ(argcv_parse(args.argc(), args.pointers.data(), &placeholder, 0,
                        &result, &error),
            ARGCV_OK);
  ExpectError(error);
  EXPECT_EQ(result.value, sentinel);
}

TEST(ArgcvTest, R03ValidNamesAndDescriptorOrder) {
  const argcv_option_t options[] = {
      {"a", false}, {"a1", true}, {"dry-run", false}};
  Arguments args({"--dry-run", "--a1", "x", "--a"});
  argcv_result_t results[3];
  argcv_error_t error;
  ASSERT_EQ(argcv_parse(args.argc(), args.pointers.data(), options, 3, results,
                        &error),
            ARGCV_OK);
  ExpectError(error);
  EXPECT_TRUE(results[0].present);
  EXPECT_EQ(results[1].value, args.pointers[3]);
  EXPECT_TRUE(results[2].present);
}

TEST(ArgcvTest, P26LastEqualStringKeepsItsOwnPointer) {
  Arguments args({"--host", "a", "--host", "a"});
  ASSERT_NE(args.pointers[2], args.pointers[4]);
  argcv_result_t results[kOptionCount];
  argcv_error_t error;
  ASSERT_EQ(argcv_parse(args.argc(), args.pointers.data(), kOptions,
                        kOptionCount, results, &error),
            ARGCV_OK);
  ExpectError(error);
  ExpectResults(results, args, false, 4);
  EXPECT_NE(results[1].value, args.pointers[2]);
}

TEST(ArgcvTest, E12UnknownOptionWithZeroDescriptors) {
  Arguments args({"--verbose"});
  const argcv_option_t placeholder = {"unused", false};
  argcv_result_t result;
  argcv_error_t error;
  ASSERT_EQ(argcv_parse(args.argc(), args.pointers.data(), &placeholder, 0,
                        &result, &error),
            ARGCV_UNKNOWN_OPTION);
  ExpectError(error, SIZE_MAX, 1);
}

TEST(ArgcvTest, L01ResetPreviouslyInitializedResults) {
  Arguments args({});
  argcv_result_t results[kOptionCount];
  results[0].present = true;
  results[1].value = "old-host";
  results[2].value = "old-port";
  argcv_error_t error;
  ASSERT_EQ(argcv_parse(args.argc(), args.pointers.data(), kOptions,
                        kOptionCount, results, &error),
            ARGCV_OK);
  ExpectError(error);
  ExpectResults(results, args);
}

TEST(ArgcvTest, L02ReuseResultsAfterSuccess) {
  Arguments first({"--host", "a"});
  Arguments second({"--verbose"});
  argcv_result_t results[kOptionCount];
  argcv_error_t error;
  ASSERT_EQ(argcv_parse(first.argc(), first.pointers.data(), kOptions,
                        kOptionCount, results, &error),
            ARGCV_OK);
  ExpectError(error);
  ExpectResults(results, first, false, 2);
  ASSERT_EQ(argcv_parse(second.argc(), second.pointers.data(), kOptions,
                        kOptionCount, results, &error),
            ARGCV_OK);
  ExpectError(error);
  ExpectResults(results, second, true);
}

TEST(ArgcvTest, L03ReuseResultsAndErrorAfterFailure) {
  Arguments first({"--verbose", "--unknown"});
  Arguments second({"--host", "b"});
  argcv_result_t results[kOptionCount];
  argcv_error_t error;
  ASSERT_EQ(argcv_parse(first.argc(), first.pointers.data(), kOptions,
                        kOptionCount, results, &error),
            ARGCV_UNKNOWN_OPTION);
  ExpectError(error, SIZE_MAX, 2);
  ASSERT_EQ(argcv_parse(second.argc(), second.pointers.data(), kOptions,
                        kOptionCount, results, &error),
            ARGCV_OK);
  ExpectError(error);
  ExpectResults(results, second, false, 2);
}

TEST(ArgcvTest, L04OnlyWriteOptionCountResults) {
  Arguments args({"--host", "localhost"});
  argcv_result_t results[kOptionCount + 1];
  const char sentinel[] = "unchanged";
  results[kOptionCount].value = sentinel;
  argcv_error_t error;
  ASSERT_EQ(argcv_parse(args.argc(), args.pointers.data(), kOptions,
                        kOptionCount, results, &error),
            ARGCV_OK);
  ExpectError(error);
  ExpectResults(results, args, false, 2);
  EXPECT_EQ(results[kOptionCount].value, sentinel);
}

TEST(ArgcvTest, L05SkipProgramName) {
  char program[] = "--verbose";
  char *const argv[] = {program, nullptr};
  argcv_result_t results[kOptionCount];
  argcv_error_t error;
  ASSERT_EQ(argcv_parse(1, argv, kOptions, kOptionCount, results, &error),
            ARGCV_OK);
  ExpectError(error);
  ExpectResults(results, Arguments({}));
}

TEST(ArgcvTest, L06ZeroArgc) {
  char *const argv[] = {nullptr};
  argcv_result_t results[kOptionCount];
  argcv_error_t error;
  ASSERT_EQ(argcv_parse(0, argv, kOptions, kOptionCount, results, &error),
            ARGCV_OK);
  ExpectError(error);
  ExpectResults(results, Arguments({}));
}

TEST(ArgcvTest, L07BorrowValueStorage) {
  char program[] = "app";
  char option[] = "--host";
  char host_value[] = "abc";
  char *const argv[] = {program, option, host_value, nullptr};
  argcv_result_t results[kOptionCount];
  argcv_error_t error;
  ASSERT_EQ(argcv_parse(3, argv, kOptions, kOptionCount, results, &error),
            ARGCV_OK);
  ExpectError(error);
  EXPECT_FALSE(results[0].present);
  ASSERT_EQ(results[1].value, host_value);
  EXPECT_EQ(results[2].value, nullptr);
  host_value[0] = 'x';
  EXPECT_STREQ(results[1].value, "xbc");
}

TEST(ArgcvTest, L08LeaveAllInputUnchangedOnSuccessAndFailure) {
  for (const bool success : {true, false}) {
    SCOPED_TRACE(success);
    Arguments args(success
                       ? std::vector<const char *>{"--host", "a"}
                       : std::vector<const char *>{"--verbose", "--unknown"});
    char verbose[] = "verbose";
    char host[] = "host";
    char port[] = "port";
    const argcv_option_t options[] = {
        {verbose, false}, {host, true}, {port, true}};
    const std::vector<argcv_option_t> old_options(options,
                                                  options + kOptionCount);
    const std::vector<std::string> old_names = {verbose, host, port};
    const auto old_pointers = args.pointers;
    const auto old_strings = args.strings;
    argcv_result_t results[kOptionCount];
    argcv_error_t error;
    ASSERT_EQ(argcv_parse(args.argc(), args.pointers.data(), options,
                          kOptionCount, results, &error),
              success ? ARGCV_OK : ARGCV_UNKNOWN_OPTION);
    ExpectError(error, SIZE_MAX, success ? -1 : 2);
    if (success) {
      ExpectResults(results, args, false, 2);
    }
    for (size_t i = 0; i < kOptionCount; ++i) {
      EXPECT_EQ(options[i].name, old_options[i].name);
      EXPECT_EQ(options[i].requires_value, old_options[i].requires_value);
      EXPECT_EQ(std::string(options[i].name), old_names[i]);
    }
    EXPECT_EQ(args.pointers, old_pointers);
    EXPECT_EQ(args.strings, old_strings);
  }
}

TEST(ArgcvTest, L09EmptyProgramName) {
  char program[] = "";
  char *const argv[] = {program, nullptr};
  argcv_result_t results[kOptionCount];
  argcv_error_t error;
  ASSERT_EQ(argcv_parse(1, argv, kOptions, kOptionCount, results, &error),
            ARGCV_OK);
  ExpectError(error);
  ExpectResults(results, Arguments({}));
}

TEST(ArgcvTest, L10ReuseUnionWithDifferentDescriptors) {
  const argcv_option_t host = {"host", true};
  const argcv_option_t verbose = {"verbose", false};
  Arguments value_args({"--host", "a"});
  Arguments flag_args({"--verbose"});
  Arguments empty_args({});
  argcv_result_t result;
  argcv_error_t error;
  ASSERT_EQ(argcv_parse(value_args.argc(), value_args.pointers.data(), &host, 1,
                        &result, &error),
            ARGCV_OK);
  ExpectError(error);
  EXPECT_EQ(result.value, value_args.pointers[2]);
  ASSERT_EQ(argcv_parse(empty_args.argc(), empty_args.pointers.data(), &verbose,
                        1, &result, &error),
            ARGCV_OK);
  ExpectError(error);
  EXPECT_FALSE(result.present);
  ASSERT_EQ(argcv_parse(flag_args.argc(), flag_args.pointers.data(), &verbose,
                        1, &result, &error),
            ARGCV_OK);
  ExpectError(error);
  EXPECT_TRUE(result.present);
  ASSERT_EQ(argcv_parse(empty_args.argc(), empty_args.pointers.data(), &host, 1,
                        &result, &error),
            ARGCV_OK);
  ExpectError(error);
  EXPECT_EQ(result.value, nullptr);
}

TEST(ArgcvTest, L11ConcurrentCallsShareDescriptors) {
  Arguments first({"--host", "a"});
  Arguments second({"--verbose", "--port", "8080"});
  argcv_result_t first_results[kOptionCount];
  argcv_result_t second_results[kOptionCount];
  argcv_error_t first_error;
  argcv_error_t second_error;
  argcv_status_t first_status;
  argcv_status_t second_status;
  std::atomic<int> ready{0};
  auto wait_for_both = [&] {
    ready.fetch_add(1);
    while (ready.load() != 2) {
      std::this_thread::yield();
    }
  };
  std::thread first_thread([&] {
    wait_for_both();
    first_status = argcv_parse(first.argc(), first.pointers.data(), kOptions,
                               kOptionCount, first_results, &first_error);
  });
  std::thread second_thread([&] {
    wait_for_both();
    second_status = argcv_parse(second.argc(), second.pointers.data(), kOptions,
                                kOptionCount, second_results, &second_error);
  });
  first_thread.join();
  second_thread.join();
  ASSERT_EQ(first_status, ARGCV_OK);
  ASSERT_EQ(second_status, ARGCV_OK);
  ExpectError(first_error);
  ExpectError(second_error);
  ExpectResults(first_results, first, false, 2);
  ExpectResults(second_results, second, true, -1, 3);
}

TEST(ArgcvTest, D01SuccessWithoutErrorOutput) {
  Arguments args({"--verbose", "--host", "a", "--host", "b", "--verbose"});
  argcv_result_t results[kOptionCount];
  ASSERT_EQ(argcv_parse(args.argc(), args.pointers.data(), kOptions,
                        kOptionCount, results, nullptr),
            ARGCV_OK);
  ExpectResults(results, args, true, 5);
}

TEST(ArgcvTest, D02UnknownOptionWithoutErrorOutput) {
  Arguments args({"file.txt"});
  argcv_result_t results[kOptionCount];
  EXPECT_EQ(argcv_parse(args.argc(), args.pointers.data(), kOptions,
                        kOptionCount, results, nullptr),
            ARGCV_UNKNOWN_OPTION);
}

TEST(ArgcvTest, D03RegisteredOptionInsteadOfValueWithoutErrorOutput) {
  Arguments args({"--host", "--verbose"});
  argcv_result_t results[kOptionCount];
  EXPECT_EQ(argcv_parse(args.argc(), args.pointers.data(), kOptions,
                        kOptionCount, results, nullptr),
            ARGCV_MISSING_VALUE);
}

TEST(ArgcvTest, D04InvalidNameWithoutErrorOutput) {
  Arguments args({});
  const argcv_option_t option = {"", false};
  argcv_result_t result;
  EXPECT_EQ(argcv_parse(args.argc(), args.pointers.data(), &option, 1, &result,
                        nullptr),
            ARGCV_INVALID_NAME);
}

TEST(ArgcvTest, D05OverwriteBothErrorFieldsOnEveryReturn) {
  Arguments unknown({"--unknown"});
  Arguments missing({"--host"});
  Arguments empty({});
  argcv_result_t results[kOptionCount];
  argcv_error_t error = {0, 0};
  ASSERT_EQ(argcv_parse(unknown.argc(), unknown.pointers.data(), kOptions,
                        kOptionCount, results, &error),
            ARGCV_UNKNOWN_OPTION);
  ExpectError(error, SIZE_MAX, 1);
  ASSERT_EQ(argcv_parse(missing.argc(), missing.pointers.data(), kOptions,
                        kOptionCount, results, &error),
            ARGCV_MISSING_VALUE);
  ExpectError(error, 1, 1);
  ASSERT_EQ(argcv_parse(empty.argc(), empty.pointers.data(), kOptions,
                        kOptionCount, results, &error),
            ARGCV_OK);
  ExpectError(error);
  ExpectResults(results, empty);
}

TEST(ArgcvTest, D06DuplicateNameWithoutErrorOutput) {
  Arguments args({});
  const argcv_option_t options[] = {{"a", false}, {"a", false}};
  argcv_result_t results[2];
  EXPECT_EQ(argcv_parse(args.argc(), args.pointers.data(), options, 2, results,
                        nullptr),
            ARGCV_DUPLICATE_NAME);
}

TEST(ArgcvTest, D07MissingFinalValueWithoutErrorOutput) {
  Arguments args({"--host"});
  argcv_result_t results[kOptionCount];
  EXPECT_EQ(argcv_parse(args.argc(), args.pointers.data(), kOptions,
                        kOptionCount, results, nullptr),
            ARGCV_MISSING_VALUE);
}

TEST(ArgcvTest, PublicLayoutMatchesRfc) {
  EXPECT_EQ(offsetof(argcv_option_t, name), 0u);
  EXPECT_EQ(offsetof(argcv_option_t, requires_value), 8u);
  EXPECT_EQ(alignof(argcv_option_t), 8u);
  EXPECT_EQ(sizeof(argcv_option_t), 16u);
  EXPECT_EQ(offsetof(argcv_result_t, value), 0u);
  EXPECT_EQ(offsetof(argcv_result_t, present), 0u);
  EXPECT_EQ(alignof(argcv_result_t), 8u);
  EXPECT_EQ(sizeof(argcv_result_t), 8u);
  EXPECT_EQ(offsetof(argcv_error_t, option_index), 0u);
  EXPECT_EQ(offsetof(argcv_error_t, argv_index), 8u);
  EXPECT_EQ(alignof(argcv_error_t), 8u);
  EXPECT_EQ(sizeof(argcv_error_t), 16u);
}

} // namespace
