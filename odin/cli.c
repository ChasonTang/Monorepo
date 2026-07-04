/* odin/cli.c
 *
 * §4.2.2 status → side-effects table for odin_cli_main (pinned). Every
 * row below is what odin_cli_main writes and returns when odin_cli_parse
 * yields the corresponding status.
 *
 *   <U_C>    = "usage: odin-client --listen ADDR --server ADDR"
 *   <U_S>    = "usage: odin-server --listen ADDR "
 *              "--quic-cert FILE --quic-key FILE"
 *   <U_BOTH> = "usage: 'odin-client --listen ADDR --server ADDR' or "
 *              "'odin-server --listen ADDR --quic-cert FILE "
 *              "--quic-key FILE'"
 *
 * | status               | mode    | out         | err | return |
 * |----------------------|---------|-------------|----------------------------------------------------|--------|
 * | OK                   | CLIENT  | -           | "odin: mode=client
 * listen=<P> server=<S>\n"        |   0    | | OK                   | SERVER  |
 * -           | "odin: mode=server listen=<P>\n"                   |   0    |
 * | HELP                 | M       | "<U_M>\n"   | - |   0    | |
 * ERR_UNKNOWN_MODE     | UNKNOWN | -           | "odin: unrecognized invocation
 * name\n<U_BOTH>\n"   |   2    | | ERR_MISSING_REQUIRED | M       | - | "odin:
 * missing required flag\n<U_M>\n"             |   2    | | ERR_UNKNOWN_FLAG | M
 * | -           | "odin: unknown or invalid flag\n<U_M>\n"           |   2    |
 * | ERR_BAD_LISTEN_PORT  | M       | -           | "odin: invalid --listen
 * port\n<U_M>\n"           |   2    |
 *
 * Both streams are flushed before odin_cli_main returns; success writes to
 * `err` so future proxy data never shares `out`. Running `out/odin`
 * directly (no symlink basename) yields ERR_UNKNOWN_MODE / 2.
 */

#include "odin/cli.h"

#include <getopt.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "odin/cli_client.h"
#include "odin/cli_server.h"
#include "odin/host_addr.h"
#include "odin/parse_util.h"

typedef enum {
  OK_PARSED,
  BAD_PORT_USE_DEFAULT,
  BAD_PORT,
} parse_listen_port_status_t;

typedef struct {
  parse_listen_port_status_t status;
  uint16_t port;
} parse_listen_port_result_t;

static parse_listen_port_result_t parse_listen_port(const char *s) {
  if (s == NULL || s[0] == '\0') {
    parse_listen_port_result_t r = {BAD_PORT_USE_DEFAULT, 0};
    return r;
  }
  const odin_parse_util_port_result_t pr =
      odin_parse_util_port((const uint8_t *)s, strlen(s));
  if (pr.status != ODIN_PARSE_UTIL_PORT_OK) {
    parse_listen_port_result_t r = {BAD_PORT, 0};
    return r;
  }
  parse_listen_port_result_t r = {OK_PARSED, pr.port};
  return r;
}

static const char *cli_basename(const char *path) {
  const char *last = path;
  for (const char *p = path; *p != '\0'; ++p) {
    if (*p == '/') {
      last = p + 1;
    }
  }
  return last;
}

static const struct option kClientLong[] = {
    {"listen", required_argument, NULL, 'l'},
    {"server", required_argument, NULL, 's'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
};

static const struct option kServerLong[] = {
    {"listen", required_argument, NULL, 'l'},
    {"quic-cert", required_argument, NULL, 1001},
    {"quic-key", required_argument, NULL, 1002},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
};

odin_cli_status_t odin_cli_parse(int argc, char *const *argv,
                                 odin_cli_args_t *out) {
  out->mode = ODIN_CLI_MODE_UNKNOWN;
  out->listen_port = 0;
  out->server_host = NULL;
  out->server_host_len = 0;
  out->server_port = 0;
  out->client_transport = ODIN_CLI_CLIENT_TRANSPORT_QUIC;
  out->server_transport = ODIN_CLI_SERVER_TRANSPORT_QUIC;
  out->quic_cert_file = NULL;
  out->quic_key_file = NULL;

  if (argc < 1 || argv[0] == NULL) {
    return ODIN_CLI_ERR_UNKNOWN_MODE;
  }

  const char *bn = cli_basename(argv[0]);
  if (bn[0] == '\0') {
    return ODIN_CLI_ERR_UNKNOWN_MODE;
  }

  odin_cli_mode_t mode;
  const struct option *longopts;
  const char *optstring;
  if (strcmp(bn, "odin-client") == 0) {
    mode = ODIN_CLI_MODE_CLIENT;
    longopts = kClientLong;
    optstring = "+l:s:h";
  } else if (strcmp(bn, "odin-server") == 0) {
    mode = ODIN_CLI_MODE_SERVER;
    longopts = kServerLong;
    optstring = "+l:h";
  } else {
    return ODIN_CLI_ERR_UNKNOWN_MODE;
  }

  out->mode = mode;

  /* Save getopt globals so callers see no side effect on any return path. */
  const int saved_optind = optind;
  const int saved_opterr = opterr;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) ||      \
    defined(__NetBSD__)
  const int saved_optreset = optreset;
  optreset = 1;
  optind = 1;
#else
  optind = 0; /* glibc: setting optind to 0 forces a full re-initialization. */
#endif
  opterr = 0;

  int help_seen = 0;
  int unknown_flag_seen = 0;
  const char *listen_arg = NULL;
  const char *server_arg = NULL;
  const char *quic_cert_arg = NULL;
  const char *quic_key_arg = NULL;

  for (;;) {
    int longindex = -1;
    const int c = getopt_long(argc, argv, optstring, longopts, &longindex);
    if (c == -1) {
      break;
    }
    if (longindex >= 0) {
      /* getopt_long accepts any unique prefix of a long option by default,
       * so --lis, --serv, and --he would otherwise dispatch as --listen,
       * --server, and --help. RFC §4.2.1 / G1 pins the long names
       * byte-for-byte, so accept only the exact spelling --<name> or
       * --<name>=value.
       *
       * Derive the option-token index from post-call getopt state, because
       * a pre-call snapshot of optind is 0 on the first glibc iteration
       * (we set optind = 0 above to force re-init). Both glibc and BSD
       * libc set optarg to point *at* the argv slot only when the value
       * was a separate argument (--name value, two tokens consumed); for
       * --name and --name=value the option sits at argv[optind - 1]. */
      int tok_idx;
      if (optarg != NULL && optind >= 2 && optarg == argv[optind - 1]) {
        tok_idx = optind - 2;
      } else {
        tok_idx = optind - 1;
      }
      const char *tok = (tok_idx > 0 && tok_idx < argc) ? argv[tok_idx] : NULL;
      const char *exp = longopts[longindex].name;
      const size_t exp_len = strlen(exp);
      if (tok == NULL || tok[0] != '-' || tok[1] != '-' ||
          strncmp(tok + 2, exp, exp_len) != 0 ||
          (tok[2 + exp_len] != '\0' && tok[2 + exp_len] != '=')) {
        unknown_flag_seen = 1;
        continue;
      }
    }
    switch (c) {
    case 'l':
      listen_arg = optarg;
      break;
    case 's':
      server_arg = optarg;
      break;
    case 1001:
      quic_cert_arg = optarg;
      break;
    case 1002:
      quic_key_arg = optarg;
      break;
    case 'h':
      help_seen = 1;
      break;
    default:
      /* getopt_long returns '?' on unknown option / missing required
       * argument; the `default` arm folds that with any other unexpected
       * return value. */
      unknown_flag_seen = 1;
      break;
    }
  }
  if (optind < argc) {
    /* Stray positional operand: `+` mode short-circuited at it, but the
     * RFC's §4.2.1 precedence routes that through ERR_UNKNOWN_FLAG too. */
    unknown_flag_seen = 1;
  }

  const parse_listen_port_result_t pr = parse_listen_port(listen_arg);

  odin_host_addr_t sr = {NULL, 0, 0};
  const odin_host_addr_status_t sr_status =
      (mode == ODIN_CLI_MODE_CLIENT && server_arg != NULL)
          ? odin_host_addr_parse(server_arg, &sr)
          : ODIN_HOST_ADDR_OK;
  const int bad_quic_tls =
      mode == ODIN_CLI_MODE_SERVER &&
      (quic_cert_arg == NULL || quic_key_arg == NULL ||
       quic_cert_arg[0] == '\0' || quic_key_arg[0] == '\0');

  odin_cli_status_t status;
  if (help_seen) {
    status = ODIN_CLI_HELP;
  } else if (unknown_flag_seen) {
    status = ODIN_CLI_ERR_UNKNOWN_FLAG;
  } else if (pr.status == BAD_PORT) {
    status = ODIN_CLI_ERR_BAD_LISTEN_PORT;
  } else if (sr_status != ODIN_HOST_ADDR_OK) {
    status = ODIN_CLI_ERR_BAD_SERVER;
  } else if (mode == ODIN_CLI_MODE_CLIENT && server_arg == NULL) {
    status = ODIN_CLI_ERR_MISSING_REQUIRED;
  } else if (bad_quic_tls) {
    status = ODIN_CLI_ERR_BAD_QUIC_TLS;
  } else {
    out->listen_port =
        (pr.status == OK_PARSED)
            ? pr.port
            : (uint16_t)(mode == ODIN_CLI_MODE_CLIENT
                             ? ODIN_CLI_DEFAULT_LISTEN_PORT_CLIENT
                             : ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER);
    if (mode == ODIN_CLI_MODE_CLIENT) {
      out->server_host = sr.host;
      out->server_host_len = sr.host_len;
      out->server_port = sr.port;
      out->client_transport = ODIN_CLI_CLIENT_TRANSPORT_QUIC;
    } else {
      out->server_transport = ODIN_CLI_SERVER_TRANSPORT_QUIC;
      out->quic_cert_file = quic_cert_arg;
      out->quic_key_file = quic_key_arg;
    }
    status = ODIN_CLI_OK;
  }

  optind = saved_optind;
  opterr = saved_opterr;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) ||      \
    defined(__NetBSD__)
  optreset = saved_optreset;
#endif

  return status;
}

int odin_cli_main(int argc, char *const *argv, FILE *out, FILE *err) {
  odin_cli_args_t args;
  const odin_cli_status_t status = odin_cli_parse(argc, argv, &args);

  static const char kUC[] = "usage: odin-client --listen ADDR --server ADDR";
  static const char kUS[] =
      "usage: odin-server --listen ADDR --quic-cert FILE --quic-key FILE";
  static const char kUBoth[] =
      "usage: 'odin-client --listen ADDR --server ADDR' or "
      "'odin-server --listen ADDR --quic-cert FILE --quic-key FILE'";

  const char *um = (args.mode == ODIN_CLI_MODE_CLIENT) ? kUC : kUS;

  int rc = 2;
  switch (status) {
  case ODIN_CLI_OK:
    if (args.mode == ODIN_CLI_MODE_CLIENT) {
      const odin_cli_client_config_t config = {
          args.listen_port, args.server_host,      args.server_host_len,
          args.server_port, args.client_transport,
      };
      (void)fflush(out);
      return odin_cli_run_client(&config, err);
    } else {
      const odin_cli_server_config_t config = {
          args.listen_port,
          args.server_transport,
          args.quic_cert_file,
          args.quic_key_file,
      };
      (void)fflush(out);
      rc = odin_cli_run_server(&config, err);
    }
    break;
  case ODIN_CLI_HELP:
    (void)fputs(um, out);
    (void)fputc('\n', out);
    rc = 0;
    break;
  case ODIN_CLI_ERR_UNKNOWN_MODE:
    (void)fputs("odin: unrecognized invocation name\n", err);
    (void)fputs(kUBoth, err);
    (void)fputc('\n', err);
    rc = 2;
    break;
  case ODIN_CLI_ERR_MISSING_REQUIRED:
    (void)fputs("odin: missing required flag\n", err);
    (void)fputs(um, err);
    (void)fputc('\n', err);
    rc = 2;
    break;
  case ODIN_CLI_ERR_UNKNOWN_FLAG:
    (void)fputs("odin: unknown or invalid flag\n", err);
    (void)fputs(um, err);
    (void)fputc('\n', err);
    rc = 2;
    break;
  case ODIN_CLI_ERR_BAD_LISTEN_PORT:
    (void)fputs("odin: invalid --listen port\n", err);
    (void)fputs(um, err);
    (void)fputc('\n', err);
    rc = 2;
    break;
  case ODIN_CLI_ERR_BAD_SERVER:
    (void)fputs("odin: invalid --server\n", err);
    (void)fputs(um, err);
    (void)fputc('\n', err);
    rc = 2;
    break;
  case ODIN_CLI_ERR_BAD_QUIC_TLS:
    (void)fputs("odin: invalid QUIC TLS configuration\n", err);
    (void)fputs(um, err);
    (void)fputc('\n', err);
    rc = 2;
    break;
  }

  (void)fflush(out);
  (void)fflush(err);
  return rc;
}
