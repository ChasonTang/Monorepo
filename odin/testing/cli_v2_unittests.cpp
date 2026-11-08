// RFC-033 T01-T23. Exercise the public v2 API with the real argcv dependency.
#include "odin/cli.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

namespace {

using Tokens = std::vector<std::string>;

const Tokens kClient = {"odin-client", "--server", "example.com", "--ca-file",
                        "ca.pem"};
const Tokens kServer = {"odin-server", "--quic-cert", "cert.pem", "--quic-key",
                        "key.pem"};

// Own writable strings and build pointers only after their storage is stable.
class Arguments {
public:
  explicit Arguments(const Tokens &tokens) : strings_(tokens) {
    for (auto &token : strings_) {
      pointers_.push_back(token.data());
    }
    pointers_.push_back(nullptr);
  }

  Arguments(const Arguments &) = delete;
  Arguments &operator=(const Arguments &) = delete;

  int argc() const { return static_cast<int>(strings_.size()); }
  char *const *argv() const { return pointers_.data(); }
  const char *at(size_t index) const { return pointers_.at(index); }

private:
  Tokens strings_;
  std::vector<char *> pointers_;
};

// Q01: every field, exact borrowed addresses, host length, and slice contents.
void ExpectConfig(const odin_cli_args_v2_t &actual,
                  const odin_cli_args_v2_t &expected, std::string_view host) {
  EXPECT_EQ(actual.listen_port, expected.listen_port);
  EXPECT_EQ(actual.server_port, expected.server_port);
  EXPECT_EQ(actual.server_host_type, expected.server_host_type);
  EXPECT_EQ(actual.quic_cert_file, expected.quic_cert_file);
  EXPECT_EQ(actual.quic_key_file, expected.quic_key_file);
  EXPECT_EQ(actual.quic_ca_file, expected.quic_ca_file);
  ASSERT_EQ(actual.server_host, expected.server_host);
  ASSERT_EQ(actual.server_host_len, expected.server_host_len);
  if (actual.server_host != nullptr) {
    EXPECT_EQ(std::string_view(actual.server_host, actual.server_host_len),
              host);
  }
}

struct ClientConfig {
  std::string host = "example.com";
  odin_cli_host_type_t type = ODIN_CLI_HOST_NAME;
  size_t offset = 0;
  uint16_t listen = 8080;
  uint16_t port = 4433;
  size_t server_index = 2;
  size_t ca_index = 4;
};

struct ServerConfig {
  uint16_t listen = 4433;
  size_t cert_index = 2;
  size_t key_index = 4;
};

void ExpectClientConfig(const Arguments &args, const odin_cli_args_v2_t &out,
                        const ClientConfig &config = {}) {
  const odin_cli_args_v2_t expected = {
      args.at(config.server_index) + config.offset,
      config.host.size(),
      nullptr,
      nullptr,
      args.at(config.ca_index),
      config.listen,
      config.port,
      config.type,
  };
  ExpectConfig(out, expected, config.host);
}

void ExpectServerConfig(const Arguments &args, const odin_cli_args_v2_t &out,
                        const ServerConfig &config = {}) {
  const odin_cli_args_v2_t expected = {
      nullptr,
      0,
      args.at(config.cert_index),
      args.at(config.key_index),
      nullptr,
      config.listen,
      0,
      ODIN_CLI_HOST_NONE,
  };
  ExpectConfig(out, expected, {});
}

void ExpectClient(const Tokens &tokens, const ClientConfig &config = {}) {
  SCOPED_TRACE(testing::PrintToString(tokens));
  const Arguments args(tokens);
  odin_cli_args_v2_t out;
  ASSERT_EQ(odin_cli_parse_v2(args.argc(), args.argv(), &out),
            ODIN_CLI_OK_CLIENT);
  ExpectClientConfig(args, out, config);
  EXPECT_STREQ(out.quic_ca_file, tokens.at(config.ca_index).c_str());
}

void ExpectServer(const Tokens &tokens, const ServerConfig &config = {}) {
  SCOPED_TRACE(testing::PrintToString(tokens));
  const Arguments args(tokens);
  odin_cli_args_v2_t out;
  ASSERT_EQ(odin_cli_parse_v2(args.argc(), args.argv(), &out),
            ODIN_CLI_OK_SERVER);
  ExpectServerConfig(args, out, config);
  EXPECT_STREQ(out.quic_cert_file, tokens.at(config.cert_index).c_str());
  EXPECT_STREQ(out.quic_key_file, tokens.at(config.key_index).c_str());
}

// O02 is inexpensive to check alongside each expected help/error status.
constexpr odin_cli_args_v2_t kUnchanged = {
    "old-host", 8,    "old-cert", "old-key",
    "old-ca",   1234, 4321,       ODIN_CLI_HOST_IPV6,
};

void ExpectStatus(const Tokens &tokens, odin_cli_status_t status) {
  SCOPED_TRACE(testing::PrintToString(tokens));
  const Arguments args(tokens);
  odin_cli_args_v2_t out = kUnchanged;
  EXPECT_EQ(odin_cli_parse_v2(args.argc(), args.argv(), &out), status);
  ExpectConfig(out, kUnchanged, "old-host");
}

void ExpectHost(const std::string &server, const std::string &host,
                odin_cli_host_type_t type, uint16_t port = 4433,
                size_t offset = 0) {
  Tokens tokens = kClient;
  tokens[2] = server;
  ClientConfig config;
  config.host = host;
  config.type = type;
  config.port = port;
  config.offset = offset;
  ExpectClient(tokens, config);
}

TEST(OdinCliV2Test, T01ModesBasenamesAndDefaults) {
  ExpectClient(kClient);
  ExpectServer(kServer);
  Tokens client = kClient;
  client[0] = "/usr/local/bin/odin-client";
  ExpectClient(client);
  Tokens server = kServer;
  server[0] = "./bin/odin-server";
  ExpectServer(server);
}

TEST(OdinCliV2Test, T02UnknownModePrecedesHelpAndSyntax) {
  for (const char *name :
       {"", "/bin/", "odin", "ODIN-CLIENT", "odin-client.exe"}) {
    Tokens tokens = kClient;
    tokens[0] = name;
    ExpectStatus(tokens, ODIN_CLI_ERR_UNKNOWN_MODE);
    tokens.insert(tokens.end(), {"--help", "--bogus"});
    ExpectStatus(tokens, ODIN_CLI_ERR_UNKNOWN_MODE);
  }
  ExpectStatus({}, ODIN_CLI_ERR_UNKNOWN_MODE);
}

TEST(OdinCliV2Test, T03MissingRequiredOptions) {
  for (const Tokens &base : {kClient, kServer}) {
    for (const int option_index : {1, 3}) {
      Tokens tokens = base;
      tokens.erase(tokens.begin() + option_index,
                   tokens.begin() + option_index + 2);
      ExpectStatus(tokens, ODIN_CLI_ERR_MISSING_REQUIRED);
    }
    ExpectStatus({base[0]}, ODIN_CLI_ERR_MISSING_REQUIRED);
  }
}

TEST(OdinCliV2Test, T04RejectOtherModesOptions) {
  for (const char *option : {"--quic-cert", "--quic-key"}) {
    Tokens tokens = kClient;
    tokens.insert(tokens.end(), {option, "x"});
    ExpectStatus(tokens, ODIN_CLI_ERR_UNKNOWN_FLAG);
  }
  for (const char *option : {"--server", "--ca-file"}) {
    Tokens tokens = kServer;
    tokens.insert(tokens.end(), {option, "x"});
    ExpectStatus(tokens, ODIN_CLI_ERR_UNKNOWN_FLAG);
  }
}

TEST(OdinCliV2Test, T05MissingValueMapping) {
  Tokens tokens = kClient;
  tokens.push_back("--listen");
  ExpectStatus(tokens, ODIN_CLI_ERR_MISSING_VALUE);
}

TEST(OdinCliV2Test, T06EmptyFinalValuesAreInvalid) {
  for (const Tokens &base : {kClient, kServer}) {
    for (const size_t value_index : {2u, 4u}) {
      Tokens tokens = base;
      tokens[value_index] = "";
      ExpectStatus(tokens, ODIN_CLI_ERR_INVALID_VALUE);
    }
    Tokens tokens = base;
    tokens.insert(tokens.end(), {"--listen", ""});
    ExpectStatus(tokens, ODIN_CLI_ERR_INVALID_VALUE);
    tokens[2] = "";
    tokens[4] = "";
    ExpectStatus(tokens, ODIN_CLI_ERR_INVALID_VALUE);
  }
}

TEST(OdinCliV2Test, T07HelpForEachMode) {
  ExpectStatus({"odin-client", "--help"}, ODIN_CLI_HELP_CLIENT);
  ExpectStatus({"odin-server", "--help"}, ODIN_CLI_HELP_SERVER);
  Tokens client = kClient;
  client.push_back("--help");
  ExpectStatus(client, ODIN_CLI_HELP_CLIENT);
  Tokens server = kServer;
  server.push_back("--help");
  ExpectStatus(server, ODIN_CLI_HELP_SERVER);
}

TEST(OdinCliV2Test, T08HelpPrecedesBusinessErrors) {
  ExpectStatus({"odin-client", "--help", "--listen", "abc"},
               ODIN_CLI_HELP_CLIENT);
  ExpectStatus({"odin-server", "--quic-cert", "", "--help"},
               ODIN_CLI_HELP_SERVER);
  for (const char *server :
       {"[::1", "[not-ip]", "bad_host", "example.com:65536"}) {
    ExpectStatus({"odin-client", "--server", server, "--help"},
                 ODIN_CLI_HELP_CLIENT);
  }
}

TEST(OdinCliV2Test, T09SyntaxPrecedesHelpAndBusinessErrors) {
  ExpectStatus({"odin-client", "--help", "--listen"},
               ODIN_CLI_ERR_MISSING_VALUE);
  ExpectStatus({"odin-server", "--help", "--ca-file", "x"},
               ODIN_CLI_ERR_UNKNOWN_FLAG);
  ExpectStatus({"odin-client", "--listen", "abc", "--listen"},
               ODIN_CLI_ERR_MISSING_VALUE);
  ExpectStatus({"odin-server", "--listen", "abc", "--ca-file", "x"},
               ODIN_CLI_ERR_UNKNOWN_FLAG);
}

TEST(OdinCliV2Test, T10BusinessErrorOrderIsUnspecified) {
  const std::vector<Tokens> cases = {
      {"odin-client", "--listen", "abc"},
      {"odin-server", "--quic-cert", ""},
      {"odin-client", "--server", "[not-ip]"},
      {"odin-client", "--server", "bad_host"},
  };
  for (const auto &tokens : cases) {
    SCOPED_TRACE(testing::PrintToString(tokens));
    const Arguments args(tokens);
    odin_cli_args_v2_t out = kUnchanged;
    const auto status = odin_cli_parse_v2(args.argc(), args.argv(), &out);
    EXPECT_TRUE(status == ODIN_CLI_ERR_MISSING_REQUIRED ||
                status == ODIN_CLI_ERR_INVALID_VALUE);
    ExpectConfig(out, kUnchanged, "old-host");
  }
}

TEST(OdinCliV2Test, T11OnlyFinalValuesAffectBusinessValidation) {
  for (const bool valid_last : {true, false}) {
    for (const Tokens &base : {kClient, kServer}) {
      Tokens tokens = base;
      tokens.insert(tokens.end(), {"--listen", valid_last ? "bad" : "9000",
                                   "--listen", valid_last ? "9000" : "bad"});
      if (!valid_last) {
        ExpectStatus(tokens, ODIN_CLI_ERR_INVALID_VALUE);
      } else if (base[0] == "odin-client") {
        ClientConfig config;
        config.listen = 9000;
        ExpectClient(tokens, config);
      } else {
        ServerConfig config;
        config.listen = 9000;
        ExpectServer(tokens, config);
      }
    }

    const Tokens tokens = {
        "odin-client",
        "--ca-file",
        "ca.pem",
        "--server",
        valid_last ? "bad_host" : "[::1]:8443",
        "--server",
        valid_last ? "[::1]:8443" : "bad_host",
    };
    if (valid_last) {
      ClientConfig config;
      config.host = "::1";
      config.type = ODIN_CLI_HOST_IPV6;
      config.offset = 1;
      config.port = 8443;
      config.server_index = 6;
      config.ca_index = 2;
      ExpectClient(tokens, config);
    } else {
      ExpectStatus(tokens, ODIN_CLI_ERR_INVALID_VALUE);
    }
  }

  struct PathCase {
    Tokens base;
    int option_index;
  };
  for (const auto &test :
       {PathCase{kClient, 3}, PathCase{kServer, 1}, PathCase{kServer, 3}}) {
    for (const bool valid_last : {true, false}) {
      Tokens tokens = test.base;
      const std::string option = tokens[test.option_index];
      tokens.erase(tokens.begin() + test.option_index,
                   tokens.begin() + test.option_index + 2);
      tokens.insert(tokens.end(), {option, valid_last ? "" : "final.pem",
                                   option, valid_last ? "final.pem" : ""});
      if (!valid_last) {
        ExpectStatus(tokens, ODIN_CLI_ERR_INVALID_VALUE);
      } else if (test.base[0] == "odin-client") {
        ClientConfig config;
        config.ca_index = 6;
        ExpectClient(tokens, config);
      } else {
        ServerConfig config;
        config.cert_index = test.option_index == 1 ? 6 : 2;
        config.key_index = test.option_index == 1 ? 2 : 6;
        ExpectServer(tokens, config);
      }
    }
  }
}

TEST(OdinCliV2Test, T12ListenAndServerPortsAreIndependent) {
  Tokens client = kClient;
  client[2] = "example.com:8443";
  client.insert(client.end(), {"--listen", "9000"});
  ClientConfig client_config;
  client_config.listen = 9000;
  client_config.port = 8443;
  ExpectClient(client, client_config);
  Tokens server = kServer;
  server.insert(server.end(), {"--listen", "9000"});
  ServerConfig server_config;
  server_config.listen = 9000;
  ExpectServer(server, server_config);
}

TEST(OdinCliV2Test, T13ExplicitPortValues) {
  struct PortCase {
    const char *text;
    uint16_t value;
  };
  for (const auto &port :
       {PortCase{"0", 0}, PortCase{"65535", 65535}, PortCase{"000080", 80}}) {
    Tokens client = kClient;
    client.insert(client.end(), {"--listen", port.text});
    ClientConfig client_config;
    client_config.listen = port.value;
    ExpectClient(client, client_config);
    Tokens server = kServer;
    server.insert(server.end(), {"--listen", port.text});
    ServerConfig server_config;
    server_config.listen = port.value;
    ExpectServer(server, server_config);
    ExpectHost(std::string("example.com:") + port.text, "example.com",
               ODIN_CLI_HOST_NAME, port.value);
    ExpectHost(std::string("[::1]:") + port.text, "::1", ODIN_CLI_HOST_IPV6,
               port.value, 1);
  }
}

TEST(OdinCliV2Test, T14PortConversionFailures) {
  for (const char *port : {"65536", "abc"}) {
    for (const Tokens &base : {kClient, kServer}) {
      Tokens tokens = base;
      tokens.insert(tokens.end(), {"--listen", port});
      ExpectStatus(tokens, ODIN_CLI_ERR_INVALID_VALUE);
    }
    for (const char *prefix : {"example.com:", "[::1]:"}) {
      Tokens tokens = kClient;
      tokens[2] = std::string(prefix) + port;
      ExpectStatus(tokens, ODIN_CLI_ERR_INVALID_VALUE);
    }
  }
}

TEST(OdinCliV2Test, T15MalformedServerAndEmptyPortSuffix) {
  for (const char *server : {"example.com:", "[::1]:", "[::1"}) {
    Tokens tokens = kClient;
    tokens[2] = server;
    ExpectStatus(tokens, ODIN_CLI_ERR_INVALID_VALUE);
  }
}

TEST(OdinCliV2Test, T16NamesPreserveOriginalText) {
  for (const char *host : {"localhost", "Example.COM.", "1-2", "123.example"}) {
    ExpectHost(host, host, ODIN_CLI_HOST_NAME);
  }
  ExpectHost("face:443", "face", ODIN_CLI_HOST_NAME, 443);
}

TEST(OdinCliV2Test, T17Ipv4Classification) {
  ExpectHost("127.0.0.1", "127.0.0.1", ODIN_CLI_HOST_IPV4);
  ExpectHost("127.0.0.1:80", "127.0.0.1", ODIN_CLI_HOST_IPV4, 80);
}

TEST(OdinCliV2Test, T18Ipv6ClassificationAndBorrowedSlices) {
  ExpectHost("::1", "::1", ODIN_CLI_HOST_IPV6);
  ExpectHost("[::1]", "::1", ODIN_CLI_HOST_IPV6, 4433, 1);
  ExpectHost("::1:443", "::1:443", ODIN_CLI_HOST_IPV6);
  ExpectHost("[::1]:8443", "::1", ODIN_CLI_HOST_IPV6, 8443, 1);
  ExpectHost("[::ffff:192.0.2.1]:8443", "::ffff:192.0.2.1", ODIN_CLI_HOST_IPV6,
             8443, 1);
}

TEST(OdinCliV2Test, T19NumericNamesFallBackWithoutNormalization) {
  ExpectHost("256.1.1.1", "256.1.1.1", ODIN_CLI_HOST_NAME);
  ExpectHost("01.2.3.4:80", "01.2.3.4", ODIN_CLI_HOST_NAME, 80);
  ExpectHost("1.2.3.4.", "1.2.3.4.", ODIN_CLI_HOST_NAME);
  ExpectStatus({"odin-client", "--server", "256.1.1.1"},
               ODIN_CLI_ERR_MISSING_REQUIRED);
}

TEST(OdinCliV2Test, T20InvalidHostsCannotFallBack) {
  for (const char *server :
       {"[not-ip]", "[127.0.0.1]:80", "a:b:443", "bad_host"}) {
    Tokens tokens = kClient;
    tokens[2] = server;
    ExpectStatus(tokens, ODIN_CLI_ERR_INVALID_VALUE);
  }
}

TEST(OdinCliV2Test, T21LongHostSliceExcludesPortAndPreservesRootDot) {
  const std::string host = std::string(63, 'a') + "." + std::string(63, 'b') +
                           "." + std::string(63, 'c') + "." +
                           std::string(61, 'd') + ".";
  ASSERT_EQ(host.size(), 254u);
  ExpectHost(host + ":80", host, ODIN_CLI_HOST_NAME, 80);
}

TEST(OdinCliV2Test,
     T22PathsAreBorrowedVerbatimWithoutAvailabilityOrLengthLimits) {
  Tokens client = kClient;
  client[4] = "~/$HOME/*.pem";
  ExpectClient(client);

  Tokens cert = kServer;
  cert[2] = "a//含 空格.pem";
  ASSERT_FALSE(std::filesystem::exists(cert[2]));
  ExpectServer(cert);

  Tokens key = kServer;
  key[4] = std::string(8192, 'a');
  ExpectServer(key);
}

TEST(OdinCliV2Test, T23SuccessfulCallsReplaceEveryField) {
  const Arguments name(kClient);
  Tokens ipv4_tokens = kClient;
  ipv4_tokens[2] = "127.0.0.1:80";
  const Arguments ipv4(ipv4_tokens);
  Tokens ipv6_tokens = kClient;
  ipv6_tokens[2] = "[::1]:8443";
  const Arguments ipv6(ipv6_tokens);
  const Arguments server(kServer);
  odin_cli_args_v2_t out;

  ASSERT_EQ(odin_cli_parse_v2(name.argc(), name.argv(), &out),
            ODIN_CLI_OK_CLIENT);
  ExpectClientConfig(name, out);
  ASSERT_EQ(odin_cli_parse_v2(ipv4.argc(), ipv4.argv(), &out),
            ODIN_CLI_OK_CLIENT);
  ClientConfig config;
  config.host = "127.0.0.1";
  config.type = ODIN_CLI_HOST_IPV4;
  config.port = 80;
  ExpectClientConfig(ipv4, out, config);
  ASSERT_EQ(odin_cli_parse_v2(ipv6.argc(), ipv6.argv(), &out),
            ODIN_CLI_OK_CLIENT);
  config.host = "::1";
  config.type = ODIN_CLI_HOST_IPV6;
  config.offset = 1;
  config.port = 8443;
  ExpectClientConfig(ipv6, out, config);
  ASSERT_EQ(odin_cli_parse_v2(server.argc(), server.argv(), &out),
            ODIN_CLI_OK_SERVER);
  ExpectServerConfig(server, out);
  ASSERT_EQ(odin_cli_parse_v2(name.argc(), name.argv(), &out),
            ODIN_CLI_OK_CLIENT);
  ExpectClientConfig(name, out);
}

} // namespace
