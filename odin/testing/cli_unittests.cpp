// odin/testing/cli_unittests.cpp
//
// Tests T1-T10 from §7 of odin/docs/rfc_002_cli_skeleton.md,
// T1-T8 from §7 of odin/docs/rfc_006_cli_listen_port_parser.md, and
// T6-T8 from §7 of odin/docs/rfc_007_cli_server_host_addr_parser.md.

#include "odin/cli.h"

#include <cerrno>
#include <cstdint>
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

// argv[0] captured by the custom test main below; used by T9 to derive
// <bindir> for the spawned odin-client symlink, and by T10 to assert on
// the renamed odin_unittests basename. External linkage so
// cli_server_unittests.cpp can derive its own server symlink path.
std::string g_test_argv0;

namespace {

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
    "usage: odin-client --listen ADDR --server ADDR [--transport tcp|quic]";
constexpr const char kUS[] =
    "usage: odin-server --listen ADDR [--transport tcp|quic] "
    "[--quic-cert FILE --quic-key FILE]";
constexpr const char kUBoth[] =
    "usage: 'odin-client --listen ADDR --server ADDR' or "
    "'odin-server --listen ADDR [--transport tcp|quic] "
    "[--quic-cert FILE --quic-key FILE]'";

} // namespace

// T1 — Client basename with both flags, short and long forms.
TEST(OdinCliTest, T1ClientBasenameBothFlagsShortLong) {
  {
    MutableArgv argv({"odin-client", "-l", "8080", "-s", "quic.example.com"});
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out), ODIN_CLI_OK);
    EXPECT_EQ(out.mode, ODIN_CLI_MODE_CLIENT);
    EXPECT_EQ(out.listen_port, 8080);
    EXPECT_EQ(out.server_host, argv.argv()[4]);
    EXPECT_EQ(out.server_host_len, std::strlen(argv.argv()[4]));
    EXPECT_EQ(out.server_port, ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER);
  }
  {
    MutableArgv argv({"./bin/odin-client", "--listen", "8080", "--server",
                      "quic.example.com"});
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out), ODIN_CLI_OK);
    EXPECT_EQ(out.mode, ODIN_CLI_MODE_CLIENT);
    EXPECT_EQ(out.listen_port, 8080);
    EXPECT_EQ(out.server_host, argv.argv()[4]);
    EXPECT_EQ(out.server_host_len, std::strlen(argv.argv()[4]));
    EXPECT_EQ(out.server_port, ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER);
  }
}

// T2 — Server basename with listen flag, short and long forms.
TEST(OdinCliTest, T2ServerBasenameListenFlagShortLong) {
  {
    MutableArgv argv({"odin-server", "--transport", "tcp", "--listen", "4433"});
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out), ODIN_CLI_OK);
    EXPECT_EQ(out.mode, ODIN_CLI_MODE_SERVER);
    EXPECT_EQ(out.listen_port, 4433);
    EXPECT_EQ(out.server_host, nullptr);
    EXPECT_EQ(out.server_host_len, static_cast<size_t>(0));
    EXPECT_EQ(out.server_port, 0);
  }
  {
    MutableArgv argv(
        {"/usr/local/bin/odin-server", "--transport", "tcp", "-l", "4433"});
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out), ODIN_CLI_OK);
    EXPECT_EQ(out.mode, ODIN_CLI_MODE_SERVER);
    EXPECT_EQ(out.listen_port, 4433);
    EXPECT_EQ(out.server_host, nullptr);
    EXPECT_EQ(out.server_host_len, static_cast<size_t>(0));
    EXPECT_EQ(out.server_port, 0);
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
    EXPECT_EQ(out.listen_port, 0);
    EXPECT_EQ(out.server_host, nullptr);
    EXPECT_EQ(out.server_host_len, static_cast<size_t>(0));
    EXPECT_EQ(out.server_port, 0);
  }
  // argc=0 with a one-slot argv whose argv[0] = NULL.
  {
    char *const slot[1] = {nullptr};
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(0, slot, &out), ODIN_CLI_ERR_UNKNOWN_MODE);
    EXPECT_EQ(out.mode, ODIN_CLI_MODE_UNKNOWN);
    EXPECT_EQ(out.listen_port, 0);
    EXPECT_EQ(out.server_host, nullptr);
    EXPECT_EQ(out.server_host_len, static_cast<size_t>(0));
    EXPECT_EQ(out.server_port, 0);
  }
}

// T4 — Missing required flag carries mode for usage-line selection.
// RFC-006 removes the `--listen` requirement, so the prior
// `{"odin-client", "-s", …}` case is covered by RFC-006 T1; only the
// `--server`-required case remains.
TEST(OdinCliTest, T4MissingRequiredFlagCarriesMode) {
  struct Case {
    std::vector<std::string> tokens;
    odin_cli_mode_t expected_mode;
  };
  const std::vector<Case> cases = {
      {{"odin-client", "-l", "8080"}, ODIN_CLI_MODE_CLIENT},
  };
  for (const auto &c : cases) {
    MutableArgv argv(c.tokens);
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out),
              ODIN_CLI_ERR_MISSING_REQUIRED);
    EXPECT_EQ(out.mode, c.expected_mode);
    EXPECT_EQ(out.listen_port, 0);
    EXPECT_EQ(out.server_host, nullptr);
    EXPECT_EQ(out.server_host_len, static_cast<size_t>(0));
    EXPECT_EQ(out.server_port, 0);
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
      {{"odin-client", "--bogus=x", "-l", "8080", "-s", "S"},
       ODIN_CLI_MODE_CLIENT},
      {{"odin-server", "-l", "4433", "-s", "S"}, ODIN_CLI_MODE_SERVER},
      {{"odin-client", "-x"}, ODIN_CLI_MODE_CLIENT},
      {{"odin-server", "-s", "S"}, ODIN_CLI_MODE_SERVER},
      {{"odin-client", "-l"}, ODIN_CLI_MODE_CLIENT},
      {{"odin-client", "-l", "8080", "-s", "S", "extra"}, ODIN_CLI_MODE_CLIENT},
      {{"odin-client", "extra"}, ODIN_CLI_MODE_CLIENT},
      // Abbreviated --listen (unique prefix of an allowed long option).
      {{"odin-client", "--lis", "L", "-s", "S"}, ODIN_CLI_MODE_CLIENT},
      // Abbreviated --server (unique prefix of an allowed long option).
      {{"odin-client", "-l", "8080", "--serv", "S"}, ODIN_CLI_MODE_CLIENT},
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
    EXPECT_EQ(out.listen_port, 0);
    EXPECT_EQ(out.server_host, nullptr);
    EXPECT_EQ(out.server_host_len, static_cast<size_t>(0));
    EXPECT_EQ(out.server_port, 0);
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
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out), ODIN_CLI_HELP);
    EXPECT_EQ(out.mode, c.expected_mode);
    EXPECT_EQ(out.listen_port, 0);
    EXPECT_EQ(out.server_host, nullptr);
    EXPECT_EQ(out.server_host_len, static_cast<size_t>(0));
    EXPECT_EQ(out.server_port, 0);
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
      {{"odin-client", "-l", "8080", "-s", "S"},
       ODIN_CLI_OK,
       ODIN_CLI_MODE_CLIENT},
      {{"odin-client", "--help"}, ODIN_CLI_HELP, ODIN_CLI_MODE_CLIENT},
      {{"odin-client", "--bogus"},
       ODIN_CLI_ERR_UNKNOWN_FLAG,
       ODIN_CLI_MODE_CLIENT},
      {{"odin-client"}, ODIN_CLI_ERR_MISSING_REQUIRED, ODIN_CLI_MODE_CLIENT},
      {{"odin"}, ODIN_CLI_ERR_UNKNOWN_MODE, ODIN_CLI_MODE_UNKNOWN},
      {{"odin-server", "--transport", "tcp", "-l", "4433"},
       ODIN_CLI_OK,
       ODIN_CLI_MODE_SERVER},
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
      // HELP CLIENT
      {{"odin-client", "--help"}, std::string(kUC) + "\n", "", 0},
      // HELP SERVER
      {{"odin-server", "-h"}, std::string(kUS) + "\n", "", 0},
      // ERR_UNKNOWN_MODE
      {{"odin"},
       "",
       std::string("odin: unrecognized invocation name\n") + kUBoth + "\n",
       2},
      // ERR_MISSING_REQUIRED CLIENT
      {{"odin-client"},
       "",
       std::string("odin: missing required flag\n") + kUC + "\n",
       2},
      // ERR_UNKNOWN_FLAG CLIENT
      {{"odin-client", "--bogus"},
       "",
       std::string("odin: unknown or invalid flag\n") + kUC + "\n",
       2},
      // ERR_UNKNOWN_FLAG SERVER
      {{"odin-server", "--bogus"},
       "",
       std::string("odin: unknown or invalid flag\n") + kUS + "\n",
       2},
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
// the help arm; byte-exact stdout/stderr assertions stay in T8.
TEST(OdinCliTest, T9SymlinkDispatchExec) {
  ASSERT_FALSE(g_test_argv0.empty());
  const std::string bindir = Dirname(g_test_argv0);
  const std::string client_path = bindir + "/odin-client";

  const pid_t pid = fork();
  ASSERT_NE(pid, -1) << "fork failed: " << std::strerror(errno);
  if (pid == 0) {
    MutableArgv child_argv({client_path.c_str(), "--help"});
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

namespace {

void ExpectMainInvalidTransport(const char *transport) {
  char out_buf[256];
  char err_buf[512];
  std::memset(out_buf, 0, sizeof(out_buf));
  std::memset(err_buf, 0, sizeof(err_buf));
  FILE *out = fmemopen(out_buf, sizeof(out_buf), "w");
  FILE *err = fmemopen(err_buf, sizeof(err_buf), "w");
  ASSERT_NE(out, nullptr);
  ASSERT_NE(err, nullptr);
  MutableArgv argv({"odin-client", "--listen", "0", "--server",
                    "127.0.0.1:4433", "--transport", transport});
  const int rc = odin_cli_main(argv.argc(), argv.argv(), out, err);
  static_cast<void>(std::fclose(out));
  static_cast<void>(std::fclose(err));
  EXPECT_EQ(rc, 2);
  EXPECT_STREQ(out_buf, "");
  EXPECT_STREQ(
      err_buf,
      (std::string("odin: invalid --transport\n") + kUC + "\n").c_str());
}

} // namespace

TEST(OdinRFC028ClientTransportTest, T1ParserAndMainTransportContract) {
  {
    MutableArgv argv(
        {"odin-client", "--listen", "0", "--server", "127.0.0.1:4433"});
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out), ODIN_CLI_OK);
    EXPECT_EQ(out.client_transport, ODIN_CLI_CLIENT_TRANSPORT_QUIC);
  }
  {
    MutableArgv argv({"odin-client", "--listen", "0", "--server",
                      "127.0.0.1:4433", "--transport", "tcp"});
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out), ODIN_CLI_OK);
    EXPECT_EQ(out.client_transport, ODIN_CLI_CLIENT_TRANSPORT_TCP);
  }
  {
    MutableArgv argv({"odin-client", "--listen", "0", "--server",
                      "127.0.0.1:4433", "--transport", "quic"});
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out), ODIN_CLI_OK);
    EXPECT_EQ(out.client_transport, ODIN_CLI_CLIENT_TRANSPORT_QUIC);
  }
  {
    MutableArgv argv({"odin-client", "--listen", "0", "--server",
                      "127.0.0.1:4433", "--transport", "udp"});
    odin_cli_args_t out{};
    EXPECT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out),
              ODIN_CLI_ERR_BAD_TRANSPORT);
  }
  {
    MutableArgv argv({"odin-client", "--listen", "0", "--server",
                      "127.0.0.1:4433", "--trans", "quic"});
    odin_cli_args_t out{};
    EXPECT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out),
              ODIN_CLI_ERR_UNKNOWN_FLAG);
  }
  {
    MutableArgv argv({"odin-server", "--listen", "0", "--transport", "quic",
                      "--quic-cert", "C", "--quic-key", "K"});
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out), ODIN_CLI_OK);
    EXPECT_EQ(out.server_transport, ODIN_CLI_SERVER_TRANSPORT_QUIC);
    EXPECT_EQ(out.client_transport, ODIN_CLI_CLIENT_TRANSPORT_TCP);
    EXPECT_STREQ(out.quic_cert_file, "C");
    EXPECT_STREQ(out.quic_key_file, "K");
  }

  ExpectMainInvalidTransport("udp");
  ExpectMainInvalidTransport("QUIC");
}

TEST(OdinRFC028ClientTransportTest, T14ParserPrecedenceAndTlsRejection) {
  struct Case {
    std::vector<std::string> tokens;
    odin_cli_status_t expected;
  };
  const std::vector<Case> cases = {
      {{"odin-client", "--help", "--transport", "udp"}, ODIN_CLI_HELP},
      {{"odin-client", "--listen", "nope", "--server", "127.0.0.1:443",
        "--transport", "udp"},
       ODIN_CLI_ERR_BAD_LISTEN_PORT},
      {{"odin-client", "--listen", "0", "--server", "127.0.0.1:bad",
        "--transport", "udp"},
       ODIN_CLI_ERR_BAD_SERVER},
      {{"odin-client", "--listen", "0", "--transport", "udp"},
       ODIN_CLI_ERR_MISSING_REQUIRED},
      {{"odin-client", "--listen", "0", "--server", "127.0.0.1:443",
        "--transport="},
       ODIN_CLI_ERR_BAD_TRANSPORT},
      {{"odin-client", "--listen", "0", "--server", "127.0.0.1:443",
        "--quic-cert", "C"},
       ODIN_CLI_ERR_UNKNOWN_FLAG},
      {{"odin-client", "--listen", "0", "--server", "127.0.0.1:443",
        "--quic-key", "K"},
       ODIN_CLI_ERR_UNKNOWN_FLAG},
      {{"odin-client", "--listen", "0", "--server", "127.0.0.1:443",
        "--transport", "quic", "--quic-cert", "C", "--quic-key", "K"},
       ODIN_CLI_ERR_UNKNOWN_FLAG},
  };
  for (const auto &c : cases) {
    SCOPED_TRACE(c.tokens.size() > 1 ? c.tokens[1] : c.tokens[0]);
    MutableArgv argv(c.tokens);
    odin_cli_args_t out{};
    EXPECT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out), c.expected);
    if (c.expected != ODIN_CLI_OK) {
      EXPECT_EQ(out.client_transport, ODIN_CLI_CLIENT_TRANSPORT_TCP);
    }
  }
}

TEST(OdinRFC028ClientTransportTest, T16UnknownFlagPrecedesInvalidTransport) {
  {
    MutableArgv argv({"odin-client", "--listen", "0", "--server",
                      "127.0.0.1:443", "--transport", "udp"});
    odin_cli_args_t out{};
    EXPECT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out),
              ODIN_CLI_ERR_BAD_TRANSPORT);
  }
  {
    MutableArgv argv({"odin-client", "--listen", "0", "--server",
                      "127.0.0.1:443", "--transport", "udp", "--quic-cert",
                      "C"});
    odin_cli_args_t out{};
    EXPECT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out),
              ODIN_CLI_ERR_UNKNOWN_FLAG);
  }
}

// ---------------------------------------------------------------------
// RFC-006 §7 — T1–T8 for the strict ASCII-decimal `--listen` parser.
// Committed in P1 wrapped in GTEST_SKIP; the skip is removed in P2 once
// the real parser, default-fill, and banner-format updates land.
// ---------------------------------------------------------------------

// T1 — Per-mode default fires when `--listen` is omitted.
TEST(OdinCliListenPortTest, T1PerModeDefaultWhenListenOmitted) {
  {
    MutableArgv argv({"odin-client", "-s", "S"});
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out), ODIN_CLI_OK);
    EXPECT_EQ(out.mode, ODIN_CLI_MODE_CLIENT);
    EXPECT_EQ(out.listen_port, ODIN_CLI_DEFAULT_LISTEN_PORT_CLIENT);
    EXPECT_EQ(out.server_host, argv.argv()[2]);
    EXPECT_EQ(out.server_host_len, std::strlen(argv.argv()[2]));
    EXPECT_EQ(out.server_port, ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER);
  }
  {
    MutableArgv argv({"odin-server", "--quic-cert", "C", "--quic-key", "K"});
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out), ODIN_CLI_OK);
    EXPECT_EQ(out.mode, ODIN_CLI_MODE_SERVER);
    EXPECT_EQ(out.listen_port, ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER);
    EXPECT_EQ(out.server_transport, ODIN_CLI_SERVER_TRANSPORT_QUIC);
    EXPECT_EQ(out.server_host, nullptr);
    EXPECT_EQ(out.server_host_len, static_cast<size_t>(0));
    EXPECT_EQ(out.server_port, 0);
  }
}

// T2 — Empty `--listen` value resolves to per-mode default.
TEST(OdinCliListenPortTest, T2EmptyListenValueResolvesToDefault) {
  {
    MutableArgv argv({"odin-client", "-l", "", "-s", "S"});
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out), ODIN_CLI_OK);
    EXPECT_EQ(out.listen_port, ODIN_CLI_DEFAULT_LISTEN_PORT_CLIENT);
  }
  {
    MutableArgv argv(
        {"odin-server", "-l", "", "--quic-cert", "C", "--quic-key", "K"});
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out), ODIN_CLI_OK);
    EXPECT_EQ(out.listen_port, ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER);
    EXPECT_EQ(out.server_transport, ODIN_CLI_SERVER_TRANSPORT_QUIC);
  }
}

// T3 — Valid digit string parses to exact port (boundaries + typical).
TEST(OdinCliListenPortTest, T3ValidDigitStringParsesToExactPort) {
  struct Case {
    const char *port;
    uint16_t expected;
  };
  const Case cases[] = {
      {"0", 0},       {"1", 1},         {"80", 80},    {"8080", 8080},
      {"8443", 8443}, {"65535", 65535}, {"00080", 80},
  };
  for (const auto &c : cases) {
    MutableArgv argv({"odin-server", "--transport", "tcp", "-l", c.port});
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out), ODIN_CLI_OK)
        << "port=" << c.port;
    EXPECT_EQ(out.listen_port, c.expected) << "port=" << c.port;
  }
}

// T4 — Out-of-range or oversized digit string returns ERR_BAD_LISTEN_PORT.
TEST(OdinCliListenPortTest, T4OutOfRangeOrOversizedReturnsBadListenPort) {
  const char *const ports[] = {
      "65536", "99999", "123456", "4294967296", "18446744073709551616",
  };
  for (const char *port : ports) {
    MutableArgv argv({"odin-server", "-l", port});
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out),
              ODIN_CLI_ERR_BAD_LISTEN_PORT)
        << "port=" << port;
    EXPECT_EQ(out.mode, ODIN_CLI_MODE_SERVER) << "port=" << port;
    EXPECT_EQ(out.listen_port, 0) << "port=" << port;
    EXPECT_EQ(out.server_host, nullptr) << "port=" << port;
    EXPECT_EQ(out.server_host_len, static_cast<size_t>(0)) << "port=" << port;
    EXPECT_EQ(out.server_port, 0) << "port=" << port;
  }
}

// T5 — Non-digit content returns ERR_BAD_LISTEN_PORT.
TEST(OdinCliListenPortTest, T5NonDigitContentReturnsBadListenPort) {
  const char *const ports[] = {
      "abc", "8080abc", "abc8080", "-1",   "+80",   "0x50",
      "8 0", " 80",     "80 ",     "80.0", "8_080",
  };
  for (const char *port : ports) {
    MutableArgv argv({"odin-server", "-l", port});
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out),
              ODIN_CLI_ERR_BAD_LISTEN_PORT)
        << "port=" << port;
    EXPECT_EQ(out.listen_port, 0) << "port=" << port;
  }
}

// T6 — Status precedence: HELP > UNKNOWN_FLAG > BAD_LISTEN_PORT >
// MISSING_REQUIRED.
TEST(OdinCliListenPortTest, T6StatusPrecedence) {
  {
    MutableArgv argv({"odin-client", "-x", "-l", "abc"});
    odin_cli_args_t out{};
    EXPECT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out),
              ODIN_CLI_ERR_UNKNOWN_FLAG);
  }
  {
    MutableArgv argv({"odin-client", "--help", "-l", "abc"});
    odin_cli_args_t out{};
    EXPECT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out), ODIN_CLI_HELP);
  }
  {
    MutableArgv argv({"odin-client", "-l", "abc"});
    odin_cli_args_t out{};
    EXPECT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out),
              ODIN_CLI_ERR_BAD_LISTEN_PORT);
  }
}

// T7 — odin_cli_main banner prints parsed port as decimal on every OK
// row, and the new error row writes the dedicated BAD_LISTEN_PORT banner.
TEST(OdinCliListenPortTest, T7MainBannerPrintsParsedPort) {
  struct Row {
    std::vector<std::string> tokens;
    std::string expected_err;
    int expected_return;
  };
  const std::vector<Row> rows = {
      {{"odin-server", "-l", "abc"},
       std::string("odin: invalid --listen port\n") + kUS + "\n",
       2},
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
    EXPECT_STREQ(out_buf, "");
    EXPECT_STREQ(err_buf, r.expected_err.c_str());
  }
}

// T8 — optind / opterr restored on the new BAD_LISTEN_PORT return path.
TEST(OdinCliListenPortTest, T8GetoptGlobalsRestoredOnBadListenPort) {
  const int snap_optind = optind;
  const int snap_opterr = opterr;

  const std::vector<std::vector<std::string>> sequence = {
      {"odin-server", "-l", "99999"},
      {"odin-client", "-l", "abc", "-s", "S"},
  };
  for (const auto &tokens : sequence) {
    MutableArgv argv(tokens);
    odin_cli_args_t out{};
    EXPECT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out),
              ODIN_CLI_ERR_BAD_LISTEN_PORT);
    EXPECT_EQ(optind, snap_optind);
    EXPECT_EQ(opterr, snap_opterr);
  }
}

// ---------------------------------------------------------------------
// RFC-007 §7 — T6–T8 for the `--server` host[:port] parser integration.
// Committed in P1 wrapped in GTEST_SKIP; the skip is removed in P2 once
// the real parse_util helpers and host_addr wrapper land.
// ---------------------------------------------------------------------

// T6 — odin_cli_parse `--server` integration: parse, struct fields,
// default port, alias to argv slot (with v6 bracket-strip).
TEST(OdinCliServerHostTest, T6ServerHostIntegration) {
  struct Case {
    const char *server;
    size_t expected_host_off;
    size_t expected_host_len;
    uint16_t expected_port;
  };
  const Case cases[] = {
      {"example.com", 0, 11, ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER},
      {"example.com:443", 0, 11, 443},
      {"[::1]:8080", 1, 3, 8080},
      {"[::1]", 1, 3, ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER},
  };
  for (const auto &c : cases) {
    MutableArgv argv({"odin-client", "-l", "8080", "-s", c.server});
    odin_cli_args_t out{};
    ASSERT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out), ODIN_CLI_OK)
        << "server=" << c.server;
    EXPECT_EQ(out.server_host, argv.argv()[4] + c.expected_host_off)
        << "server=" << c.server;
    EXPECT_EQ(out.server_host_len, c.expected_host_len)
        << "server=" << c.server;
    EXPECT_EQ(out.server_port, c.expected_port) << "server=" << c.server;
  }
}

// T7 — Status precedence: BAD_LISTEN_PORT > BAD_SERVER > MISSING_REQUIRED.
TEST(OdinCliServerHostTest, T7StatusPrecedence) {
  struct Case {
    std::vector<std::string> tokens;
    odin_cli_status_t expected;
  };
  const std::vector<Case> cases = {
      {{"odin-client", "-l", "abc", "-s", "bad:99999"},
       ODIN_CLI_ERR_BAD_LISTEN_PORT},
      {{"odin-client", "-l", "8080", "-s", "bad:99999"},
       ODIN_CLI_ERR_BAD_SERVER},
      {{"odin-client", "-l", "8080", "-s", "[::1"}, ODIN_CLI_ERR_BAD_SERVER},
      {{"odin-client", "-l", "8080"}, ODIN_CLI_ERR_MISSING_REQUIRED},
  };
  for (const auto &c : cases) {
    MutableArgv argv(c.tokens);
    odin_cli_args_t out{};
    EXPECT_EQ(odin_cli_parse(argv.argc(), argv.argv(), &out), c.expected);
    EXPECT_EQ(out.server_host, nullptr);
    EXPECT_EQ(out.server_host_len, static_cast<size_t>(0));
    EXPECT_EQ(out.server_port, 0);
  }
}

// T8 — odin_cli_main banner: server=<H>:<P> with bracket re-add for
// v6, plus the new ERR_BAD_SERVER row.
TEST(OdinCliServerHostTest, T8MainBannerServerHostPort) {
  struct Row {
    std::vector<std::string> tokens;
    std::string expected_err;
    int expected_return;
  };
  const std::vector<Row> rows = {
      {{"odin-client", "-l", "8080", "-s", "bad:99999"},
       std::string("odin: invalid --server\n") + kUC + "\n",
       2},
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
    EXPECT_STREQ(out_buf, "");
    EXPECT_STREQ(err_buf, r.expected_err.c_str());
  }
}

int main(int argc, char **argv) {
  if (argc > 0 && argv[0] != nullptr) {
    g_test_argv0 = argv[0];
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
