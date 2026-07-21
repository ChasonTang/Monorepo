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
 *   - A successful parse or help request returns the corresponding
 *     CLIENT / SERVER status. Error statuses do not carry a mode.
 *   - Only OK_CLIENT / OK_SERVER fills `listen_port`; OK_CLIENT also fills
 *     `server_host` / `server_host_len` / `server_port`. `listen_port` is
 *     the parsed decimal port supplied via `--listen`, or — when `--listen`
 *     is omitted or supplied as the empty string — the per-mode default
 *     (ODIN_CLI_DEFAULT_LISTEN_PORT_CLIENT for Client mode,
 *     ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER for Server mode). On every
 *     other status `listen_port` keeps its entry-block zero.
 *     `server_host` aliases the corresponding `argv` slot on Client OK
 *     (no copy, no allocation); offset 0 for bare host / IPv4, offset
 *     1 for bracketed v6. `server_host_len` is the length of that
 *     slice; `server_port` is the parsed port digits, or
 *     ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER when `--server` had no
 *     `:port` suffix. Server mode never sets the three server fields.
 *   - `--help` / `-h` wins after a valid basename, returning HELP_CLIENT or
 *     HELP_SERVER.
 *   - Long option names are accepted only when spelled exactly
 *     (`--listen`, `--server`, `--quic-cert`, `--quic-key`, `--help`);
 *     abbreviated unique prefixes
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
 *   - Status precedence within a valid basename (highest wins): HELP_*,
 *     ERR_UNKNOWN_FLAG, ERR_BAD_LISTEN_PORT, ERR_BAD_SERVER,
 *     ERR_MISSING_REQUIRED, OK_*. ERR_UNKNOWN_MODE precedes any of these
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

typedef enum odin_cli_status_t {
  ODIN_CLI_OK_CLIENT = 0,
  ODIN_CLI_OK_SERVER,
  ODIN_CLI_HELP_CLIENT,
  ODIN_CLI_HELP_SERVER,
  ODIN_CLI_ERR_UNKNOWN_MODE,
  ODIN_CLI_ERR_MISSING_REQUIRED,
  ODIN_CLI_ERR_UNKNOWN_FLAG,
  ODIN_CLI_ERR_BAD_LISTEN_PORT,
  ODIN_CLI_ERR_BAD_SERVER,
  ODIN_CLI_ERR_BAD_QUIC_TLS,
} odin_cli_status_t;

typedef struct odin_cli_args_t {
  uint16_t listen_port;
  const char *server_host;
  size_t server_host_len;
  uint16_t server_port;
  const char *quic_cert_file;
  const char *quic_key_file;
  const char *quic_ca_file;
} odin_cli_args_t;

odin_cli_status_t odin_cli_parse(int argc, char *const *argv,
                                 odin_cli_args_t *out);

int odin_cli_main(int argc, char *const *argv, FILE *out, FILE *err);

#ifdef __cplusplus
}
#endif

#endif /* ODIN_CLI_H_ */
