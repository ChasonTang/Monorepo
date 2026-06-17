/* odin/cli_server.h
 *
 * Internal to the odin target: the server-OK branch of odin_cli_main.
 *
 * odin_cli_run_server is an owner-thread, blocking call that binds an
 * IPv4 TCP listener on 0.0.0.0:<listen_port>, prints the actual bound
 * port on err once the listener, event loop, server runtime, default
 * SSRF dial filter, signal handlers, and signal-stop polling timer are
 * live, then runs the RFC-019/RFC-020/RFC-021 server stack until
 * SIGINT/SIGTERM is delivered. Returns 0 only after a graceful
 * signal-driven stop; returns 1 for any setup, runtime callback, or
 * event-loop run failure, after writing one deterministic failure line
 * to err. Writes nothing to stdout. listen_port == 0 means
 * kernel-selected ephemeral port; the success banner reports the
 * actual port discovered via getsockname.
 */

#ifndef ODIN_CLI_SERVER_H_
#define ODIN_CLI_SERVER_H_

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

int odin_cli_run_server(uint16_t listen_port, FILE *err);

#ifdef __cplusplus
}
#endif

#endif /* ODIN_CLI_SERVER_H_ */
