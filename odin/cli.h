/* odin/cli.h
 *
 * Single-binary CLI parser surface and stream-injectable entry point for
 * the `odin` binary (RFC-002, extended by RFC-006 and RFC-007). Pure
 * parser surface — `odin_cli_parse` zeroes `*out`, allocates nothing,
 * performs no I/O. The status table mapped by `odin_cli_main` is pinned
 * in odin/cli.c.
 *
 * argv must include a NULL terminator at argv[argc], as provided to main.
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

#pragma clang assume_nonnull begin

#define ODIN_CLI_DEFAULT_LISTEN_PORT_CLIENT 8080
#define ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER 4433

typedef enum odin_cli_status_t {
  ODIN_CLI_OK_CLIENT = 0,
  ODIN_CLI_OK_SERVER = 1,
  ODIN_CLI_HELP_CLIENT = 2,
  ODIN_CLI_HELP_SERVER = 3,
  ODIN_CLI_ERR_UNKNOWN_MODE = 4,
  ODIN_CLI_ERR_MISSING_REQUIRED = 5,
  ODIN_CLI_ERR_UNKNOWN_FLAG = 6,
  ODIN_CLI_ERR_BAD_LISTEN_PORT = 7,
  ODIN_CLI_ERR_BAD_SERVER = 8,
  ODIN_CLI_ERR_BAD_QUIC_TLS = 9,
  ODIN_CLI_ERR_MISSING_VALUE = 10,
  ODIN_CLI_ERR_INVALID_VALUE = 11,
} odin_cli_status_t;

typedef struct odin_cli_args_t {
  uint16_t listen_port;
  const char *_Nullable server_host;
  size_t server_host_len;
  uint16_t server_port;
  const char *_Nullable quic_cert_file;
  const char *_Nullable quic_key_file;
  const char *_Nullable quic_ca_file;
} odin_cli_args_t;

odin_cli_status_t odin_cli_parse(int argc, char *_Nullable const *_Nonnull argv,
                                 odin_cli_args_t *out);

int odin_cli_main(int argc, char *_Nullable const *_Nonnull argv, FILE *out,
                  FILE *err);

typedef enum odin_cli_host_type_t {
  ODIN_CLI_HOST_NONE = 0,
  ODIN_CLI_HOST_NAME = 1,
  ODIN_CLI_HOST_IPV4 = 2,
  ODIN_CLI_HOST_IPV6 = 3,
} odin_cli_host_type_t;

typedef struct odin_cli_args_v2_t {
  const char *_Nullable server_host;
  size_t server_host_len;
  const char *_Nullable quic_cert_file;
  const char *_Nullable quic_key_file;
  const char *_Nullable quic_ca_file;
  uint16_t listen_port;
  uint16_t server_port;
  odin_cli_host_type_t server_host_type;
} odin_cli_args_v2_t;

/* RFC-033: an independent parser; odin_cli_main continues to use v1.
 * Accept only exact odin-client / odin-server basenames and the mode's long
 * options, using --option value or --help. Repeated options use the last value.
 * Unknown mode precedes the first syntax error, then help, then the first
 * business error. Missing values and invalid final values use the v2 statuses
 * above; errors 7..9 remain exclusive to v1.
 *
 * Client requires --server and --ca-file; server requires --quic-cert and
 * --quic-key. All final values must be nonempty. --listen defaults to the
 * per-mode constant; a server address without a port defaults to 4433.
 * Explicit ports accept ASCII decimal 0..65535, including leading zeros.
 * Bracketed or multi-colon hosts must be IPv6; other hosts must be valid ASCII
 * labels and are classified as IPV4 only when strict IPv4 syntax also passes.
 * File paths are accepted verbatim without checking their availability.
 *
 * Preconditions: argc >= 0; argv has argc + 1 elements, valid NUL-terminated
 * strings before argv[argc], and argv[argc] == NULL. out is writable and does
 * not overlap input storage. Input stays valid and unchanged during the call;
 * callers prevent conflicting access to output. argc == 0 is UNKNOWN_MODE.
 *
 * Only OK_CLIENT / OK_SERVER replaces every output field; all other returns
 * leave *out unchanged. Output needs no initialization but is readable only
 * after success if previously uninitialized. Unused fields are NULL / 0.
 * Ports use host byte order. Host and paths borrow the original argv strings;
 * host is a slice, possibly not NUL-terminated. Keep the corresponding input
 * alive while using a configuration, including one retained after an error.
 * Reparse after modifying host bytes before using the validated host type.
 *
 * No input mutation, allocation, I/O, DNS, diagnostics, or retained state.
 * Calls with independent output storage do not interfere with each other.
 */
odin_cli_status_t odin_cli_parse_v2(int argc,
                                    char *_Nullable const *_Nonnull argv,
                                    odin_cli_args_v2_t *out);

#pragma clang assume_nonnull end

#ifdef __cplusplus
}
#endif

#endif /* ODIN_CLI_H_ */
