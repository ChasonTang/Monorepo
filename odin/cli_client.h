/* odin/cli_client.h
 *
 * Internal CLI client runner. The startup success banner is written to out;
 * diagnostics are written to err.
 */

#ifndef ODIN_CLI_CLIENT_H_
#define ODIN_CLI_CLIENT_H_

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#pragma clang assume_nonnull begin

typedef struct odin_cli_client_config_t {
  const char *_Nullable server_host;
  size_t server_host_len;
  const char *_Nullable quic_ca_file;
  uint16_t listen_port;
  uint16_t server_port;
} odin_cli_client_config_t;

int odin_cli_run_client(const odin_cli_client_config_t *_Nullable config,
                        FILE *_Nullable out, FILE *_Nullable err);

#pragma clang assume_nonnull end

#ifdef __cplusplus
}
#endif

#endif /* ODIN_CLI_CLIENT_H_ */
