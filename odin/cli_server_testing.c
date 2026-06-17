/* odin/cli_server_testing.c
 *
 * Test target compiles this file instead of the production cli_server.c.
 * It includes cli_server.c verbatim so the test hooks can read and
 * mutate cli_server's static failpoint, liveness, bind-address, signal
 * progress, and dial-start probe state directly.
 */

#include "cli_server.c" // NOLINT(bugprone-suspicious-include)

#include "odin/cli_server_internal_test.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int odin_cli_server_test_default_dial_filter(const struct sockaddr *addr,
                                             socklen_t addrlen) {
  return default_filter_check(addr, addrlen);
}

int odin_cli_server_test_fail_next(odin_cli_server_test_failpoint_t fp,
                                   int errnum) {
  if (errnum <= 0) {
    errno = EINVAL;
    return -1;
  }
  switch (fp) {
  case ODIN_CLI_SERVER_TEST_FAIL_SOCKET:
  case ODIN_CLI_SERVER_TEST_FAIL_SETSOCKOPT_REUSEADDR:
  case ODIN_CLI_SERVER_TEST_FAIL_FCNTL_GETFL:
  case ODIN_CLI_SERVER_TEST_FAIL_FCNTL_SETFL:
  case ODIN_CLI_SERVER_TEST_FAIL_BIND:
  case ODIN_CLI_SERVER_TEST_FAIL_LISTEN:
  case ODIN_CLI_SERVER_TEST_FAIL_GETSOCKNAME:
  case ODIN_CLI_SERVER_TEST_FAIL_EVENT_LOOP_CREATE:
  case ODIN_CLI_SERVER_TEST_FAIL_SERVER_RUNTIME_CREATE:
  case ODIN_CLI_SERVER_TEST_FAIL_SIGACTION_SIGINT:
  case ODIN_CLI_SERVER_TEST_FAIL_SIGACTION_SIGTERM:
  case ODIN_CLI_SERVER_TEST_FAIL_SIGNAL_TIMER_START:
  case ODIN_CLI_SERVER_TEST_FAIL_EVENT_LOOP_RUN:
  case ODIN_CLI_SERVER_TEST_TRIGGER_RUNTIME_ERROR:
    g_failpoint = fp;
    g_failpoint_errno = errnum;
    return 0;
  default:
    errno = EINVAL;
    return -1;
  }
}

void odin_cli_server_test_reset_liveness(void) {
  g_live_listeners = 0;
  g_live_runtimes = 0;
  g_last_cleanup_runtime_inflight = 0;
  g_last_bind_addr_recorded = 0;
  memset(&g_last_bind_addr, 0, sizeof(g_last_bind_addr));
  g_progress_fd = -1;
  g_progress_reported = 0;
  g_probe_fd = -1;
  g_probe_errno = 0;
  /* Also clears any pending failpoint so a previous test that forked a
   * child to consume the failpoint cannot leak armed state into the
   * next test in the same process. */
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  g_failpoint = (odin_cli_server_test_failpoint_t)0;
  g_failpoint_errno = 0;
}

int odin_cli_server_test_liveness(odin_cli_server_test_liveness_t *out) {
  if (out == NULL) {
    errno = EINVAL;
    return -1;
  }
  out->live_listeners = g_live_listeners;
  out->live_runtimes = g_live_runtimes;
  out->last_cleanup_runtime_inflight = g_last_cleanup_runtime_inflight;
  return 0;
}

int odin_cli_server_test_last_bind_addr(struct sockaddr_in *out) {
  if (out == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (!g_last_bind_addr_recorded) {
    errno = ENOENT;
    return -1;
  }
  *out = g_last_bind_addr;
  return 0;
}

int odin_cli_server_test_set_progress_fd(int fd) {
  g_progress_fd = fd;
  g_progress_reported = 0;
  return 0;
}

void odin_cli_server_test_set_dial_start_probe_fd(int fd, int errnum) {
  g_probe_fd = fd;
  g_probe_errno = errnum;
}

int odin_cli_server_test_maybe_probe_dial_start(const struct sockaddr *addr,
                                                socklen_t addrlen) {
  if (g_probe_fd < 0) {
    return 0;
  }
  if (addr != NULL && addrlen >= (socklen_t)sizeof(struct sockaddr_in)) {
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
    if (sin->sin_family == AF_INET) {
      odin_cli_server_test_dial_start_t rec;
      rec.family = AF_INET;
      rec.ipv4_addr_nbo = sin->sin_addr.s_addr;
      rec.port_nbo = sin->sin_port;
      (void)write(g_probe_fd, &rec, sizeof(rec));
    }
  }
  const int err = g_probe_errno;
  g_probe_fd = -1;
  g_probe_errno = 0;
  errno = err;
  return -1;
}
