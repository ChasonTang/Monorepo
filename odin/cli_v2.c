#include "odin/cli.h"

#include <stdbool.h>
#include <string.h>

#include "argcv/argcv.h"
#include "argcv/value.h"

enum {
  LISTEN,
  HELP,
  REQUIRED_FIRST,
  REQUIRED_SECOND,
  OPTION_COUNT,
};

/* The final two entries are the required values for each mode. */
static const argcv_option_t kClientOptions[OPTION_COUNT] = {
    [LISTEN] = {"listen", true},
    [HELP] = {"help", false},
    [REQUIRED_FIRST] = {"server", true},
    [REQUIRED_SECOND] = {"ca-file", true},
};

static const argcv_option_t kServerOptions[OPTION_COUNT] = {
    [LISTEN] = {"listen", true},
    [HELP] = {"help", false},
    [REQUIRED_FIRST] = {"quic-cert", true},
    [REQUIRED_SECOND] = {"quic-key", true},
};

odin_cli_status_t odin_cli_parse_v2(int argc, char *const *argv,
                                    odin_cli_args_v2_t *out) {
  if (argc == 0) {
    return ODIN_CLI_ERR_UNKNOWN_MODE;
  }
  const char *slash = strrchr(argv[0], '/');
  const char *basename = slash == NULL ? argv[0] : slash + 1;
  const bool is_client = strcmp(basename, "odin-client") == 0;
  if (!is_client && strcmp(basename, "odin-server") != 0) {
    return ODIN_CLI_ERR_UNKNOWN_MODE;
  }

  argcv_result_t values[OPTION_COUNT];
  const argcv_status_t syntax =
      argcv_parse(argc, argv, is_client ? kClientOptions : kServerOptions,
                  OPTION_COUNT, values, NULL);
  if (syntax != ARGCV_OK) {
    return syntax == ARGCV_MISSING_VALUE ? ODIN_CLI_ERR_MISSING_VALUE
                                         : ODIN_CLI_ERR_UNKNOWN_FLAG;
  }
  if (values[HELP].present) {
    return is_client ? ODIN_CLI_HELP_CLIENT : ODIN_CLI_HELP_SERVER;
  }

  const char *first = values[REQUIRED_FIRST].value;
  const char *second = values[REQUIRED_SECOND].value;
  if (first == NULL || second == NULL) {
    return ODIN_CLI_ERR_MISSING_REQUIRED;
  }
  if (first[0] == '\0' || second[0] == '\0') {
    return ODIN_CLI_ERR_INVALID_VALUE;
  }

  /* Assemble privately so every help/error path preserves the caller's out. */
  odin_cli_args_v2_t parsed = {0};
  parsed.listen_port = is_client ? ODIN_CLI_DEFAULT_LISTEN_PORT_CLIENT
                                 : ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER;
  const char *listen = values[LISTEN].value;
  if (listen != NULL &&
      !argcv_parse_u16(listen, strlen(listen), &parsed.listen_port)) {
    return ODIN_CLI_ERR_INVALID_VALUE;
  }

  if (is_client) {
    argcv_hostport_parts_t parts;
    if (!argcv_split_hostport(first, strlen(first), &parts)) {
      return ODIN_CLI_ERR_INVALID_VALUE;
    }
    if (parts.host_requires_ipv6) {
      if (!argcv_validate_ipv6(parts.host, parts.host_len)) {
        return ODIN_CLI_ERR_INVALID_VALUE;
      }
      parsed.server_host_type = ODIN_CLI_HOST_IPV6;
    } else {
      bool only_digits_and_dots;
      if (!argcv_validate_host_labels(parts.host, parts.host_len,
                                      &only_digits_and_dots)) {
        return ODIN_CLI_ERR_INVALID_VALUE;
      }
      parsed.server_host_type =
          only_digits_and_dots &&
                  argcv_validate_ipv4(parts.host, parts.host_len)
              ? ODIN_CLI_HOST_IPV4
              : ODIN_CLI_HOST_NAME;
    }
    parsed.server_port = ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER;
    if (parts.port != NULL &&
        !argcv_parse_u16(parts.port, parts.port_len, &parsed.server_port)) {
      return ODIN_CLI_ERR_INVALID_VALUE;
    }
    parsed.server_host = parts.host;
    parsed.server_host_len = parts.host_len;
    parsed.quic_ca_file = second;
  } else {
    parsed.quic_cert_file = first;
    parsed.quic_key_file = second;
  }

  *out = parsed;
  return is_client ? ODIN_CLI_OK_CLIENT : ODIN_CLI_OK_SERVER;
}
