// odin/cli_unittests.cpp
//
// Tests T1-T10 from §7 of odin/docs/rfc_002_cli_skeleton.md.

#include "odin/cli.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <initializer_list>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "gtest/gtest.h"

extern "C" {
extern char **environ;
}

namespace {

// argv[0] captured by the custom test main below; used by T9 to derive
// <bindir> for the spawned odin-client symlink, and by T10 to assert on
// the renamed odin_unittests basename.
std::string g_test_argv0;

// Owns mutable storage for argv tokens and exposes a NULL-terminable
// std::vector<char *> to the C API and to execve. No std::string content
// is mutated after construction; only the pointer vector grows when
// argv_terminated() appends the NULL.
class MutableArgv {
 public:
  MutableArgv(std::initializer_list<const char *> tokens) {
    storage_.reserve(tokens.size());
    for (const char *t : tokens) {
      storage_.emplace_back(t);
    }
    rebuild_ptrs();
  }

  explicit MutableArgv(const std::vector<std::string> &tokens)
      : storage_(tokens) {
    rebuild_ptrs();
  }

  int argc() const { return static_cast<int>(storage_.size()); }
  char *const *argv() { return ptrs_.data(); }
  // execve / posix_spawn want a NULL-terminated vector.
  char *const *argv_terminated() {
    if (ptrs_.empty() || ptrs_.back() != nullptr) {
      ptrs_.push_back(nullptr);
    }
    return ptrs_.data();
  }

 private:
  void rebuild_ptrs() {
    ptrs_.clear();
    ptrs_.reserve(storage_.size());
    for (auto &s : storage_) {
      ptrs_.push_back(&s[0]);
    }
  }

  std::vector<std::string> storage_;
  std::vector<char *> ptrs_;
};

// Non-mutating bindir helper for T9. Operates on a const std::string &
// rather than POSIX dirname(char *), which mutates its argument on glibc.
std::string Dirname(const std::string &path) {
  const auto pos = path.find_last_of('/');
  if (pos == std::string::npos) {
    return std::string(".");
  }
  return path.substr(0, pos);
}

std::string Basename(const std::string &path) {
  const auto pos = path.find_last_of('/');
  if (pos == std::string::npos) {
    return path;
  }
  return path.substr(pos + 1);
}

constexpr const char kUC[] =
    "usage: odin-client --listen ADDR --server ADDR";
constexpr const char kUS[] = "usage: odin-server --listen ADDR";
constexpr const char kUBoth[] =
    "usage: 'odin-client --listen ADDR --server ADDR' or "
    "'odin-server --listen ADDR'";

}  // namespace

// T1 — Client basename with both flags, short and long forms.
TEST(OdinCliTest, T1ClientBasenameBothFlagsShortLong) {
  {
    MutableArgv argv({"odin-client", "-l", "127.0.0.1:8443", "-s",
                      "quic.example.com:4433"});
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out), ODIN_CLI_OK);
    EXPECT_EQ(out.mode, ODIN_CLI_MODE_CLIENT);
    EXPECT_EQ(out.listen_addr, argv.argv()[2]);
    EXPECT_EQ(out.server_addr, argv.argv()[4]);
  }
  {
    MutableArgv argv({"./bin/odin-client", "--listen", "127.0.0.1:8443",
                      "--server", "quic.example.com:4433"});
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out), ODIN_CLI_OK);
    EXPECT_EQ(out.mode, ODIN_CLI_MODE_CLIENT);
    EXPECT_EQ(out.listen_addr, argv.argv()[2]);
    EXPECT_EQ(out.server_addr, argv.argv()[4]);
  }
}

// T2 — Server basename with listen flag, short and long forms.
TEST(OdinCliTest, T2ServerBasenameListenFlagShortLong) {
  {
    MutableArgv argv({"odin-server", "--listen", "0.0.0.0:4433"});
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out), ODIN_CLI_OK);
    EXPECT_EQ(out.mode, ODIN_CLI_MODE_SERVER);
    EXPECT_EQ(out.listen_addr, argv.argv()[2]);
    EXPECT_EQ(out.server_addr, nullptr);
  }
  {
    MutableArgv argv({"/usr/local/bin/odin-server", "-l", "0.0.0.0:4433"});
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out), ODIN_CLI_OK);
    EXPECT_EQ(out.mode, ODIN_CLI_MODE_SERVER);
    EXPECT_EQ(out.listen_addr, argv.argv()[2]);
    EXPECT_EQ(out.server_addr, nullptr);
  }
}

// T3 — Unknown / missing basename.
TEST(OdinCliTest, T3UnknownOrMissingBasename) {
  for (const auto &tokens : std::vector<std::vector<std::string>>{
           {"odin"}, {"./odin"}, {"odin-foo"}, {""}}) {
    MutableArgv argv(tokens);
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out),
              ODIN_CLI_ERR_UNKNOWN_MODE);
    EXPECT_EQ(out.mode, ODIN_CLI_MODE_UNKNOWN);
    EXPECT_EQ(out.listen_addr, nullptr);
    EXPECT_EQ(out.server_addr, nullptr);
  }
  // argc=0 with a one-slot argv whose argv[0] = NULL.
  {
    char *const slot[1] = {nullptr};
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(0, slot, &out), ODIN_CLI_ERR_UNKNOWN_MODE);
    EXPECT_EQ(out.mode, ODIN_CLI_MODE_UNKNOWN);
    EXPECT_EQ(out.listen_addr, nullptr);
    EXPECT_EQ(out.server_addr, nullptr);
  }
}

// T4 — Missing required flag carries mode for usage-line selection.
TEST(OdinCliTest, T4MissingRequiredFlagCarriesMode) {
  struct Case {
    std::vector<std::string> tokens;
    odin_cli_mode_t expected_mode;
  };
  const std::vector<Case> cases = {
      {{"odin-client", "-l", "127.0.0.1:8443"}, ODIN_CLI_MODE_CLIENT},
      {{"odin-client", "-s", "quic.example.com:4433"},
       ODIN_CLI_MODE_CLIENT},
      {{"odin-server"}, ODIN_CLI_MODE_SERVER},
  };
  for (const auto &c : cases) {
    MutableArgv argv(c.tokens);
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out),
              ODIN_CLI_ERR_MISSING_REQUIRED);
    EXPECT_EQ(out.mode, c.expected_mode);
    EXPECT_EQ(out.listen_addr, nullptr);
    EXPECT_EQ(out.server_addr, nullptr);
  }
}

// T5 — Unknown flag precedes missing-required, including server-mode -s
// rejection, the missing-argument case, stray positional operands
// (leading or trailing), and abbreviated long-option spellings that
// getopt_long would otherwise accept as unique prefixes — RFC §4.2.1 / G1
// pins the long names byte-for-byte.
TEST(OdinCliTest, T5UnknownFlagPrecedesMissingRequired) {
  struct Case {
    std::vector<std::string> tokens;
    odin_cli_mode_t expected_mode;
  };
  const std::vector<Case> cases = {
      {{"odin-client", "--bogus=x", "-l", "L", "-s", "S"},
       ODIN_CLI_MODE_CLIENT},
      {{"odin-server", "-l", "L", "-s", "S"}, ODIN_CLI_MODE_SERVER},
      {{"odin-client", "-x"}, ODIN_CLI_MODE_CLIENT},
      {{"odin-server", "-s", "S"}, ODIN_CLI_MODE_SERVER},
      {{"odin-client", "-l"}, ODIN_CLI_MODE_CLIENT},
      {{"odin-client", "-l", "L", "-s", "S", "extra"},
       ODIN_CLI_MODE_CLIENT},
      {{"odin-client", "extra"}, ODIN_CLI_MODE_CLIENT},
      // Abbreviated --listen (unique prefix of an allowed long option).
      {{"odin-client", "--lis", "L", "-s", "S"}, ODIN_CLI_MODE_CLIENT},
      // Abbreviated --server (unique prefix of an allowed long option).
      {{"odin-client", "-l", "L", "--serv", "S"}, ODIN_CLI_MODE_CLIENT},
      // Abbreviated --help must NOT short-circuit to HELP — exact spelling
      // only.
      {{"odin-client", "--he"}, ODIN_CLI_MODE_CLIENT},
      // Server-mode abbreviation of --listen.
      {{"odin-server", "--lis", "L"}, ODIN_CLI_MODE_SERVER},
      // Abbreviated --listen with attached value (--list=L) is rejected
      // too — exact-spelling check covers the --flag=value form.
      {{"odin-client", "--list=L", "-s", "S"}, ODIN_CLI_MODE_CLIENT},
  };
  for (const auto &c : cases) {
    MutableArgv argv(c.tokens);
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out),
              ODIN_CLI_ERR_UNKNOWN_FLAG);
    EXPECT_EQ(out.mode, c.expected_mode);
    EXPECT_EQ(out.listen_addr, nullptr);
    EXPECT_EQ(out.server_addr, nullptr);
  }
}

// T6 — --help short-circuits before the required-flag check and before
// any later unknown flag.
TEST(OdinCliTest, T6HelpShortCircuits) {
  struct Case {
    std::vector<std::string> tokens;
    odin_cli_mode_t expected_mode;
  };
  const std::vector<Case> cases = {
      {{"odin-client", "--help"}, ODIN_CLI_MODE_CLIENT},
      {{"odin-server", "-h"}, ODIN_CLI_MODE_SERVER},
      {{"odin-client", "--help", "-x"}, ODIN_CLI_MODE_CLIENT},
  };
  for (const auto &c : cases) {
    MutableArgv argv(c.tokens);
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out),
              ODIN_CLI_HELP);
    EXPECT_EQ(out.mode, c.expected_mode);
    EXPECT_EQ(out.listen_addr, nullptr);
    EXPECT_EQ(out.server_addr, nullptr);
  }
}

// T7 — Repeated parse calls leave optind / opterr undisturbed across
// every return path.
TEST(OdinCliTest, T7GetoptGlobalsRestored) {
  const int snap_optind = optind;
  const int snap_opterr = opterr;

  struct Case {
    std::vector<std::string> tokens;
    odin_cli_status_t expected_status;
    odin_cli_mode_t expected_mode;
  };
  const std::vector<Case> sequence = {
      {{"odin-client", "-l", "L", "-s", "S"}, ODIN_CLI_OK,
       ODIN_CLI_MODE_CLIENT},
      {{"odin-client", "--help"}, ODIN_CLI_HELP, ODIN_CLI_MODE_CLIENT},
      {{"odin-client", "--bogus"}, ODIN_CLI_ERR_UNKNOWN_FLAG,
       ODIN_CLI_MODE_CLIENT},
      {{"odin-client"}, ODIN_CLI_ERR_MISSING_REQUIRED,
       ODIN_CLI_MODE_CLIENT},
      {{"odin"}, ODIN_CLI_ERR_UNKNOWN_MODE, ODIN_CLI_MODE_UNKNOWN},
      {{"odin-server", "-l", "L"}, ODIN_CLI_OK, ODIN_CLI_MODE_SERVER},
  };
  for (const auto &c : sequence) {
    MutableArgv argv(c.tokens);
    odin_cli_args_t out{};
    EXPECT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out),
              c.expected_status);
    EXPECT_EQ(out.mode, c.expected_mode);
    // Restoration check fires after every call, not just the last.
    EXPECT_EQ(optind, snap_optind);
    EXPECT_EQ(opterr, snap_opterr);
  }
}

// T8 — odin_cli_main byte-exact out / err / return mapping for every
// §4.2.2 row.
TEST(OdinCliTest, T8MainByteExactMapping) {
  struct Row {
    std::vector<std::string> tokens;
    std::string expected_out;
    std::string expected_err;
    int expected_return;
  };
  const std::vector<Row> rows = {
      // OK CLIENT
      {{"odin-client", "-l", "L", "-s", "S"}, "",
       "odin: mode=client listen=L server=S\n", 0},
      // OK SERVER
      {{"odin-server", "-l", "L"}, "", "odin: mode=server listen=L\n", 0},
      // HELP CLIENT
      {{"odin-client", "--help"}, std::string(kUC) + "\n", "", 0},
      // HELP SERVER
      {{"odin-server", "-h"}, std::string(kUS) + "\n", "", 0},
      // ERR_UNKNOWN_MODE
      {{"odin"}, "",
       std::string("odin: unrecognized invocation name\n") + kUBoth + "\n",
       2},
      // ERR_MISSING_REQUIRED CLIENT
      {{"odin-client"}, "",
       std::string("odin: missing required flag\n") + kUC + "\n", 2},
      // ERR_MISSING_REQUIRED SERVER
      {{"odin-server"}, "",
       std::string("odin: missing required flag\n") + kUS + "\n", 2},
      // ERR_UNKNOWN_FLAG CLIENT
      {{"odin-client", "--bogus"}, "",
       std::string("odin: unknown or invalid flag\n") + kUC + "\n", 2},
      // ERR_UNKNOWN_FLAG SERVER
      {{"odin-server", "--bogus"}, "",
       std::string("odin: unknown or invalid flag\n") + kUS + "\n", 2},
  };

  char out_buf[512];
  char err_buf[512];
  for (const auto &r : rows) {
    std::memset(out_buf, 0, sizeof(out_buf));
    std::memset(err_buf, 0, sizeof(err_buf));
    FILE *out = fmemopen(out_buf, sizeof(out_buf), "w");
    FILE *err = fmemopen(err_buf, sizeof(err_buf), "w");
    ASSERT_NE(out, nullptr);
    ASSERT_NE(err, nullptr);

    MutableArgv argv(r.tokens);
    const int rc = odin_cli_main(argv.argc(), argv.argv(), out, err);
    static_cast<void>(std::fclose(out));
    static_cast<void>(std::fclose(err));

    EXPECT_EQ(rc, r.expected_return);
    EXPECT_STREQ(out_buf, r.expected_out.c_str());
    EXPECT_STREQ(err_buf, r.expected_err.c_str());
  }
}

// T9 — out/odin-client symlink dispatch + exec end-to-end. Proves the
// relative symlink resolves to out/odin and odin_cli_main returns 0 on
// the success arm; byte-exact stdout/stderr assertions stay in T8.
TEST(OdinCliTest, T9SymlinkDispatchExec) {
  ASSERT_FALSE(g_test_argv0.empty());
  const std::string bindir = Dirname(g_test_argv0);
  const std::string client_path = bindir + "/odin-client";

  const pid_t pid = fork();
  ASSERT_NE(pid, -1) << "fork failed: " << std::strerror(errno);
  if (pid == 0) {
    MutableArgv child_argv({client_path.c_str(), "--listen", "L",
                            "--server", "S"});
    execve(client_path.c_str(), child_argv.argv_terminated(), environ);
    // execve only returns on failure.
    _exit(127);
  }
  int wstatus = 0;
  ASSERT_EQ(waitpid(pid, &wstatus, 0), pid);
  ASSERT_TRUE(WIFEXITED(wstatus));
  EXPECT_EQ(WEXITSTATUS(wstatus), 0);
}

// T10 — Renamed unit-test binary still carries the RFC-001 codec tests.
// Reads g_test_argv0 (basename must be odin_unittests after the §5
// rename) and walks gtest's registry to confirm OdinProtoTest is still
// present.
TEST(OdinCliTest, T10CodecTestsCarryUnderRenamedBinary) {
  ASSERT_FALSE(g_test_argv0.empty());
  EXPECT_EQ(Basename(g_test_argv0), "odin_unittests");

  const testing::UnitTest *ut = testing::UnitTest::GetInstance();
  bool found_odin_proto_suite = false;
  for (int i = 0; i < ut->total_test_suite_count(); ++i) {
    if (std::string(ut->GetTestSuite(i)->name()) == "OdinProtoTest") {
      found_odin_proto_suite = true;
      break;
    }
  }
  EXPECT_TRUE(found_odin_proto_suite);
}

int main(int argc, char **argv) {
  if (argc > 0 && argv[0] != nullptr) {
    g_test_argv0 = argv[0];
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
