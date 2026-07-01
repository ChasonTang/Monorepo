/* odin/cli.h
 *
 * Single-binary CLI parser surface and stream-injectable entry point for
 * the `odin` binary (RFC-002, extended by RFC-006 and RFC-007). Pure
 * parser surface — `odin_cli_parse` zeroes `*out`, allocates nothing,
 * performs no I/O. The status table mapped by `odin_cli_main` is pinned
 * in odin/cli.c.
 *
 * Status semantics for odin_cli_parse:
 *   - argc < 1, argv[0] == NULL, or empty basename → ERR_UNKNOWN_MODE;
 *     `*out` is fully zeroed on this path.
 *   - After a valid `odin-client` / `odin-server` basename, `out.mode`
 *     persists on OK, HELP, ERR_UNKNOWN_FLAG, ERR_BAD_LISTEN_PORT,
 *     ERR_BAD_SERVER, and ERR_MISSING_REQUIRED.
 *   - Only OK fills `listen_port` / `server_host` / `server_host_len` /
 *     `server_port` / `client_transport`. `listen_port` is the parsed decimal
 * port supplied via `--listen`, or — when `--listen` is omitted or supplied as
 *     the empty string — the per-mode default
 *     (ODIN_CLI_DEFAULT_LISTEN_PORT_CLIENT for Client mode,
 *     ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER for Server mode). On every
 *     other status `listen_port` keeps its entry-block zero.
 *     `server_host` aliases the corresponding `argv` slot on Client OK
 *     (no copy, no allocation); offset 0 for bare host / IPv4, offset
 *     1 for bracketed v6. `server_host_len` is the length of that
 *     slice; `server_port` is the parsed port digits, or
 *     ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER when `--server` had no
 *     `:port` suffix. Server mode never sets the three server fields.
 *   - `--help` / `-h` wins after a valid basename, returning HELP.
 *   - Long option names are accepted only when spelled exactly
 *     (`--listen`, `--server`, `--help`); abbreviated unique prefixes
 *     (e.g. `--lis`, `--he`) return ERR_UNKNOWN_FLAG.
 *   - `--listen` accepts a bare ASCII-decimal port string matching
 *     `[0-9]+` with value ≤ 65535 (no sign, no `0x`/`0o` prefix, no
 *     whitespace, no separators). Any other non-empty value returns
 *     ERR_BAD_LISTEN_PORT without writing `listen_port`. `--listen` is
 *     not a required flag; omission or an empty value falls back to the
 *     per-mode default.
 *   - `--server` accepts one of `host`, `host:port`, `[v6]`, or
 *     `[v6]:port` (RFC-007 §4.2.3). Empty value, structural malformation,
 *     bad port digits, or host length exceeding the protocol cap returns
 *     ERR_BAD_SERVER without writing the three server fields.
 *   - Status precedence within a valid basename (highest wins): HELP,
 *     ERR_UNKNOWN_FLAG, ERR_BAD_LISTEN_PORT, ERR_BAD_SERVER,
 *     ERR_MISSING_REQUIRED, OK. ERR_UNKNOWN_MODE precedes any of these
 *     because it fires before any flag parsing.
 *   - Unknown options, missing option arguments, and stray positional
 *     operands return ERR_UNKNOWN_FLAG before the missing-required,
 *     bad-listen-port, and bad-server checks (precedence is
 *     permutation-invariant).
 *   - `optind` / `opterr` (and BSD `optreset`) are saved and restored on
 *     every return path; the parser sets `opterr = 0` internally to
 *     suppress libc stderr.
 */

#ifndef ODIN_CLI_H_
#define ODIN_CLI_H_

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ODIN_CLI_DEFAULT_LISTEN_PORT_CLIENT 8080
#define ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER 4433

typedef enum odin_cli_mode_t {
  ODIN_CLI_MODE_UNKNOWN = 0,
  ODIN_CLI_MODE_CLIENT,
  ODIN_CLI_MODE_SERVER,
} odin_cli_mode_t;

typedef enum odin_cli_server_transport_t {
  ODIN_CLI_SERVER_TRANSPORT_TCP = 0,
  ODIN_CLI_SERVER_TRANSPORT_QUIC,
} odin_cli_server_transport_t;

typedef enum odin_cli_client_transport_t {
  ODIN_CLI_CLIENT_TRANSPORT_TCP = 0,
  ODIN_CLI_CLIENT_TRANSPORT_QUIC = 1,
#if defined(ODIN_CLI_CLIENT_TESTING)
  ODIN_CLI_CLIENT_TRANSPORT_TEST_INVALID = 99,
#endif
} odin_cli_client_transport_t;

typedef enum odin_cli_status_t {
  ODIN_CLI_OK = 0,
  ODIN_CLI_HELP,
  ODIN_CLI_ERR_UNKNOWN_MODE,
  ODIN_CLI_ERR_MISSING_REQUIRED,
  ODIN_CLI_ERR_UNKNOWN_FLAG,
  ODIN_CLI_ERR_BAD_LISTEN_PORT,
  ODIN_CLI_ERR_BAD_SERVER,
  ODIN_CLI_ERR_BAD_TRANSPORT,
  ODIN_CLI_ERR_BAD_QUIC_TLS,
} odin_cli_status_t;

typedef struct odin_cli_args_t {
  odin_cli_mode_t mode;
  uint16_t listen_port;
  const char *server_host;
  size_t server_host_len;
  uint16_t server_port;
  odin_cli_client_transport_t client_transport;
  odin_cli_server_transport_t server_transport;
  const char *quic_cert_file;
  const char *quic_key_file;
} odin_cli_args_t;

odin_cli_status_t odin_cli_parse(int argc, char *const *argv,
                                 odin_cli_args_t *out);

int odin_cli_main(int argc, char *const *argv, FILE *out, FILE *err);

#ifdef __cplusplus
}
#endif

#endif /* ODIN_CLI_H_ */
