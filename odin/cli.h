/* CLI parser and stream-injectable entry point for the odin binary. */

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
  ODIN_CLI_ERR_MISSING_VALUE = 7,
  ODIN_CLI_ERR_INVALID_VALUE = 8,
} odin_cli_status_t;

/* Successful parses start the selected runtime. Help writes the mode's usage
 * to out and returns 0; parse errors write the mode's usage to err (combined
 * usage for unknown mode) and return 2 without starting a runtime. Both
 * streams must be writable. */
int odin_cli_main(int argc, char *_Nullable const *_Nonnull argv, FILE *out,
                  FILE *err);

typedef enum odin_cli_host_type_t {
  ODIN_CLI_HOST_NONE = 0,
  ODIN_CLI_HOST_NAME = 1,
  ODIN_CLI_HOST_IPV4 = 2,
  ODIN_CLI_HOST_IPV6 = 3,
} odin_cli_host_type_t;

typedef struct odin_cli_args_t {
  const char *_Nullable server_host;
  size_t server_host_len;
  const char *_Nullable quic_cert_file;
  const char *_Nullable quic_key_file;
  const char *_Nullable quic_ca_file;
  uint16_t listen_port;
  uint16_t server_port;
  odin_cli_host_type_t server_host_type;
} odin_cli_args_t;

/* RFC-033 parser used by odin_cli_main.
 * Accept only exact odin-client / odin-server basenames and the mode's long
 * options, using --option value or --help. Repeated options use the last value.
 * Unknown mode precedes the first syntax error, then help, then the first
 * business error. Missing values and invalid final values have distinct
 * error statuses.
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
odin_cli_status_t odin_cli_parse(int argc, char *_Nullable const *_Nonnull argv,
                                 odin_cli_args_t *out);

#pragma clang assume_nonnull end

#ifdef __cplusplus
}
#endif

#endif /* ODIN_CLI_H_ */
