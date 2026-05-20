/* odin/cli.h
 *
 * Single-binary CLI parser surface and stream-injectable entry point for
 * the `odin` binary (RFC-002). Pure parser surface — `odin_cli_parse`
 * zeroes `*out`, allocates nothing, performs no I/O. The status table
 * mapped by `odin_cli_main` is pinned in odin/cli.c.
 *
 * Status semantics for odin_cli_parse:
 *   - argc < 1, argv[0] == NULL, or empty basename → ERR_UNKNOWN_MODE;
 *     `*out` is fully zeroed on this path.
 *   - After a valid `odin-client` / `odin-server` basename, `out.mode`
 *     persists on OK, HELP, ERR_UNKNOWN_FLAG, ERR_MISSING_REQUIRED.
 *   - Only OK fills `listen_addr` / `server_addr`; those pointers alias
 *     the corresponding `argv` slots (no copy, no allocation).
 *   - `--help` / `-h` wins after a valid basename, returning HELP.
 *   - Long option names are accepted only when spelled exactly
 *     (`--listen`, `--server`, `--help`); abbreviated unique prefixes
 *     (e.g. `--lis`, `--he`) return ERR_UNKNOWN_FLAG.
 *   - Unknown options, missing option arguments, and stray positional
 *     operands return ERR_UNKNOWN_FLAG before the missing-required check
 *     (precedence is permutation-invariant).
 *   - `optind` / `opterr` (and BSD `optreset`) are saved and restored on
 *     every return path; the parser sets `opterr = 0` internally to
 *     suppress libc stderr.
 */

#ifndef ODIN_CLI_H_
#define ODIN_CLI_H_

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum odin_cli_mode_t {
  ODIN_CLI_MODE_UNKNOWN = 0,
  ODIN_CLI_MODE_CLIENT,
  ODIN_CLI_MODE_SERVER,
} odin_cli_mode_t;

typedef enum odin_cli_status_t {
  ODIN_CLI_OK = 0,
  ODIN_CLI_HELP,
  ODIN_CLI_ERR_UNKNOWN_MODE,
  ODIN_CLI_ERR_MISSING_REQUIRED,
  ODIN_CLI_ERR_UNKNOWN_FLAG,
} odin_cli_status_t;

typedef struct odin_cli_args_t {
  odin_cli_mode_t mode;
  const char *listen_addr;
  const char *server_addr;
} odin_cli_args_t;

odin_cli_status_t odin_cli_parse(int argc, char *const *argv,
                                 odin_cli_args_t *out);

int odin_cli_main(int argc, char *const *argv, FILE *out, FILE *err);

#ifdef __cplusplus
}
#endif

#endif /* ODIN_CLI_H_ */
