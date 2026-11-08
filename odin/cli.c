/* CLI parser and entry point: parse arguments, then run or print usage. */

#include "odin/cli.h"

#include <stdbool.h>
#include <string.h>

#include "argcv/argcv.h"
#include "argcv/value.h"
#include "odin/cli_client.h"
#include "odin/cli_server.h"

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

typedef enum {
  CLI_MODE_UNKNOWN,
  CLI_MODE_CLIENT,
  CLI_MODE_SERVER,
} cli_mode_t;

static cli_mode_t cli_mode(int argc, char *const *argv) {
  if (argc == 0) {
    return CLI_MODE_UNKNOWN;
  }
  const char *slash = strrchr(argv[0], '/');
  const char *basename = slash == NULL ? argv[0] : slash + 1;
  if (strcmp(basename, "odin-client") == 0) {
    return CLI_MODE_CLIENT;
  }
  if (strcmp(basename, "odin-server") == 0) {
    return CLI_MODE_SERVER;
  }
  return CLI_MODE_UNKNOWN;
}

odin_cli_status_t odin_cli_parse(int argc, char *const *argv,
                                 odin_cli_args_t *out) {
  const cli_mode_t mode = cli_mode(argc, argv);
  if (mode == CLI_MODE_UNKNOWN) {
    return ODIN_CLI_ERR_UNKNOWN_MODE;
  }
  const bool is_client = mode == CLI_MODE_CLIENT;

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
  odin_cli_args_t parsed = {0};
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

int odin_cli_main(int argc, char *const *argv, FILE *out, FILE *err) {
  odin_cli_args_t args;
  const odin_cli_status_t status = odin_cli_parse(argc, argv, &args);

  static const char kUC[] =
      "usage: odin-client --listen ADDR --server ADDR --ca-file FILE";
  static const char kUS[] =
      "usage: odin-server --listen ADDR --quic-cert FILE --quic-key FILE";
  static const char kUBoth[] =
      "usage: 'odin-client --listen ADDR --server ADDR --ca-file FILE' or "
      "'odin-server --listen ADDR --quic-cert FILE --quic-key FILE'";

  int rc = 2;
  switch (status) {
  case ODIN_CLI_OK_CLIENT: {
    const odin_cli_client_config_t config = {
        .server_host = args.server_host,
        .server_host_len = args.server_host_len,
        .quic_ca_file = args.quic_ca_file,
        .listen_port = args.listen_port,
        .server_port = args.server_port,
    };
    (void)fflush(out);
    return odin_cli_run_client(&config, err);
  }
  case ODIN_CLI_OK_SERVER: {
    const odin_cli_server_config_t config = {
        .quic_cert_file = args.quic_cert_file,
        .quic_key_file = args.quic_key_file,
        .listen_port = args.listen_port,
    };
    (void)fflush(out);
    rc = odin_cli_run_server(&config, err);
    break;
  }
  case ODIN_CLI_HELP_CLIENT:
    (void)fputs(kUC, out);
    (void)fputc('\n', out);
    rc = 0;
    break;
  case ODIN_CLI_HELP_SERVER:
    (void)fputs(kUS, out);
    (void)fputc('\n', out);
    rc = 0;
    break;
  case ODIN_CLI_ERR_UNKNOWN_MODE:
    (void)fputs(kUBoth, err);
    (void)fputc('\n', err);
    break;
  case ODIN_CLI_ERR_MISSING_REQUIRED:
  case ODIN_CLI_ERR_UNKNOWN_FLAG:
  case ODIN_CLI_ERR_MISSING_VALUE:
  case ODIN_CLI_ERR_INVALID_VALUE:
    (void)fputs(cli_mode(argc, argv) == CLI_MODE_CLIENT ? kUC : kUS, err);
    (void)fputc('\n', err);
    break;
  }

  (void)fflush(out);
  (void)fflush(err);
  return rc;
}
