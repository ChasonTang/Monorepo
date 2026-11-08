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

typedef struct odin_cli_client_config_t {
  const char *server_host;
  size_t server_host_len;
  const char *quic_ca_file;
  uint16_t listen_port;
  uint16_t server_port;
} odin_cli_client_config_t;

int odin_cli_run_client(const odin_cli_client_config_t *config, FILE *out,
                        FILE *err);

#ifdef __cplusplus
}
#endif

#endif /* ODIN_CLI_CLIENT_H_ */
