// CLI entry-point output, executable dispatch, and shared test main.

#include "odin/cli.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "gtest/gtest.h"

extern "C" {
extern char **environ;
}

// Shared by runtime integration tests to locate sibling binaries and TLS
// fixtures. Captured by the custom test main below.
std::string g_test_argv0;

namespace {

// Owns mutable argv tokens and a NULL-terminated pointer vector for the CLI
// entry point and execve. argc() excludes the trailing NULL.
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

private:
  void rebuild_ptrs() {
    ptrs_.clear();
    ptrs_.reserve(storage_.size() + 1);
    for (auto &s : storage_) {
      ptrs_.push_back(s.data());
    }
    ptrs_.push_back(nullptr);
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
    "usage: odin-client --listen ADDR --server ADDR --ca-file FILE";
constexpr const char kUS[] =
    "usage: odin-server --listen ADDR --quic-cert FILE --quic-key FILE";
constexpr const char kUBoth[] =
    "usage: 'odin-client --listen ADDR --server ADDR --ca-file FILE' or "
    "'odin-server --listen ADDR --quic-cert FILE --quic-key FILE'";

} // namespace

// Help succeeds on stdout; parse errors print the recognized mode's usage
// (combined usage for unknown mode) to stderr and return 2 before runtime.
TEST(OdinCliTest, MainHelpAndParseErrors) {
  struct Row {
    std::vector<std::string> tokens;
    std::string expected_out;
    std::string expected_err;
    int expected_return;
  };
  const std::string client_usage = std::string(kUC) + "\n";
  const std::string server_usage = std::string(kUS) + "\n";
  const std::string unknown_usage = std::string(kUBoth) + "\n";
  const std::vector<Row> rows = {
      {{"odin-client", "--help"}, std::string(kUC) + "\n", "", 0},
      {{"odin-server", "--help"}, std::string(kUS) + "\n", "", 0},
      {{"odin-client", "--help", "--listen", "abc"},
       std::string(kUC) + "\n",
       "",
       0},
      {{}, "", unknown_usage, 2},
      {{""}, "", unknown_usage, 2},
      {{"odin"}, "", unknown_usage, 2},
      {{"odin", "--help", "--bogus"}, "", unknown_usage, 2},
      {{"/bin/odin-client/", "--help"}, "", unknown_usage, 2},
      {{"odin-client"}, "", client_usage, 2},
      {{"odin-server", "--quic-cert", "C"}, "", server_usage, 2},
      {{"/usr/local/bin/odin-client"}, "", client_usage, 2},
      {{"./bin/odin-server", "--bogus"}, "", server_usage, 2},
      {{"odin-client", "--bogus"}, "", client_usage, 2},
      {{"odin-server", "--bogus"}, "", server_usage, 2},
      {{"odin-client", "--listen"}, "", client_usage, 2},
      {{"odin-server", "--listen", "--help"}, "", server_usage, 2},
      {{"odin-client", "--server", "bad_host", "--ca-file", "CA"},
       "",
       client_usage,
       2},
      {{"odin-client", "--server", "127.0.0.1:4433", "--ca-file", ""},
       "",
       client_usage,
       2},
      {{"odin-server", "--quic-cert", "C", "--quic-key", "K", "--listen", ""},
       "",
       server_usage,
       2},
      {{"odin-server", "--quic-cert", "C", "--quic-key", "K", "--listen",
        "abc"},
       "",
       server_usage,
       2},
      {{"odin-client", "--help", "--bogus"}, "", client_usage, 2},
      {{"odin-server", "-h"}, "", server_usage, 2},
      {{"odin-server", "--listen=4433"}, "", server_usage, 2},
  };

  char out_buf[512];
  char err_buf[512];
  for (const auto &r : rows) {
    SCOPED_TRACE(testing::PrintToString(r.tokens));
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
// the help arm; byte-exact stdout/stderr assertions stay in the mapping test
// above.
TEST(OdinCliTest, T9SymlinkDispatchExec) {
  ASSERT_FALSE(g_test_argv0.empty());
  const std::string bindir = Dirname(g_test_argv0);
  const std::string client_path = bindir + "/odin-client";

  const pid_t pid = fork();
  ASSERT_NE(pid, -1) << "fork failed: " << std::strerror(errno);
  if (pid == 0) {
    MutableArgv child_argv({client_path.c_str(), "--help"});
    execve(client_path.c_str(), child_argv.argv(), environ);
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
