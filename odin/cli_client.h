/* odin/cli_client.h
 *
 * Internal CLI client runner.
 */

#ifndef ODIN_CLI_CLIENT_H_
#define ODIN_CLI_CLIENT_H_

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

int odin_cli_run_client(uint16_t listen_port, const char *server_host,
                        size_t server_host_len, uint16_t server_port,
                        FILE *err);

#ifdef __cplusplus
}
#endif

#endif /* ODIN_CLI_CLIENT_H_ */
