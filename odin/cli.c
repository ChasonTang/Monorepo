/* odin/cli.c
 *
 * §4.2.2 status → side-effects table for odin_cli_main (pinned). Every
 * row below is what odin_cli_main writes and returns when odin_cli_parse
 * yields the corresponding status.
 *
 *   <U_C>    = "usage: odin-client --listen ADDR --server ADDR"
 *   <U_S>    = "usage: odin-server --listen ADDR"
 *   <U_BOTH> = "usage: 'odin-client --listen ADDR --server ADDR' or "
 *              "'odin-server --listen ADDR'"
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
#include <stdint.h>
#include <string.h>

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
  uint32_t v = 0;
  for (const char *p = s; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9') {
      parse_listen_port_result_t r = {BAD_PORT, 0};
      return r;
    }
    v = v * 10u + (uint32_t)(*p - '0');
    if (v > 65535u) {
      parse_listen_port_result_t r = {BAD_PORT, 0};
      return r;
    }
  }
  parse_listen_port_result_t r = {OK_PARSED, (uint16_t)v};
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
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
};

odin_cli_status_t odin_cli_parse(int argc, char *const *argv,
                                 odin_cli_args_t *out) {
  out->mode = ODIN_CLI_MODE_UNKNOWN;
  out->listen_port = 0;
  out->server_addr = NULL;

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

  odin_cli_status_t status;
  if (help_seen) {
    status = ODIN_CLI_HELP;
  } else if (unknown_flag_seen) {
    status = ODIN_CLI_ERR_UNKNOWN_FLAG;
  } else if (pr.status == BAD_PORT) {
    status = ODIN_CLI_ERR_BAD_LISTEN_PORT;
  } else if (mode == ODIN_CLI_MODE_CLIENT && server_arg == NULL) {
    status = ODIN_CLI_ERR_MISSING_REQUIRED;
  } else {
    out->listen_port =
        (pr.status == OK_PARSED)
            ? pr.port
            : (uint16_t)(mode == ODIN_CLI_MODE_CLIENT
                             ? ODIN_CLI_DEFAULT_LISTEN_PORT_CLIENT
                             : ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER);
    if (mode == ODIN_CLI_MODE_CLIENT) {
      out->server_addr = server_arg;
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
  static const char kUS[] = "usage: odin-server --listen ADDR";
  static const char kUBoth[] =
      "usage: 'odin-client --listen ADDR --server ADDR' or "
      "'odin-server --listen ADDR'";

  const char *um = (args.mode == ODIN_CLI_MODE_CLIENT) ? kUC : kUS;

  int rc = 2;
  switch (status) {
  case ODIN_CLI_OK:
    if (args.mode == ODIN_CLI_MODE_CLIENT) {
      // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
      (void)fprintf(err, "odin: mode=client listen=%u server=%s\n",
                    (unsigned)args.listen_port, args.server_addr);
    } else {
      // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
      (void)fprintf(err, "odin: mode=server listen=%u\n",
                    (unsigned)args.listen_port);
    }
    rc = 0;
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
  }

  (void)fflush(out);
  (void)fflush(err);
  return rc;
}
