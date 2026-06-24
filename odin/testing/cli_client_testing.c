/* odin/testing/cli_client_testing.c */

#include "odin/cli_client.c" // NOLINT(bugprone-suspicious-include)

#include "odin/testing/cli_client_internal_test.h"

#include <errno.h>
#include <string.h>

static int valid_fail_next_fp(odin_cli_client_test_failpoint_t fp) {
  if (fp < ODIN_CLI_CLIENT_TEST_FAIL_SOCKET ||
      fp > ODIN_CLI_CLIENT_TEST_FAIL_NEXT_CLIENT_SESSION_CREATE) {
    errno = EINVAL;
    return 0;
  }
#if defined(__linux__)
  if (fp == ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_GETFL_ERROR ||
      fp == ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_SETFL_ERROR) {
    errno = EOPNOTSUPP;
    return 0;
  }
#endif
  return 1;
}

int odin_cli_client_test_fail_next(odin_cli_client_test_failpoint_t fp,
                                   int errnum) {
  if (errnum <= 0 || fp == ODIN_CLI_CLIENT_TEST_TRIGGER_UNEXPECTED_STOP) {
    errno = EINVAL;
    return -1;
  }
  if (!valid_fail_next_fp(fp)) {
    return -1;
  }
  g_failpoint = fp;
  g_failpoint_errno = errnum;
  return 0;
}

int odin_cli_client_test_trigger_next(odin_cli_client_test_failpoint_t fp) {
  if (fp != ODIN_CLI_CLIENT_TEST_TRIGGER_UNEXPECTED_STOP) {
    errno = EINVAL;
    return -1;
  }
  g_failpoint = fp;
  g_failpoint_errno = 0;
  return 0;
}

void odin_cli_client_test_reset_liveness(void) {
  g_live_listeners = 0;
  g_live_accept_loops = 0;
  g_live_sessions = 0;
  g_last_cleanup_sessions = 0;
  g_last_bind_addr_recorded = 0;
  memset(&g_last_bind_addr, 0, sizeof(g_last_bind_addr));
  g_progress_fd = -1;
  g_progress_min_inflight_sessions = 0;
  g_progress_reported = 0;
  g_runtime_trigger_fd = -1;
  g_runtime_trigger_released = 0;
  g_idle_snapshot_fd = -1;
  g_idle_snapshot_min_closed_sessions = 0;
  g_idle_snapshot_reported = 0;
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  g_failpoint = (odin_cli_client_test_failpoint_t)0;
  g_failpoint_errno = 0;
}

int odin_cli_client_test_liveness(odin_cli_client_test_liveness_t *out) {
  if (out == NULL) {
    errno = EINVAL;
    return -1;
  }
  out->live_listeners = g_live_listeners;
  out->live_accept_loops = g_live_accept_loops;
  out->live_sessions = g_live_sessions;
  out->last_cleanup_sessions = g_last_cleanup_sessions;
  return 0;
}

int odin_cli_client_test_last_bind_addr(struct sockaddr_in *out) {
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

int odin_cli_client_test_set_progress_fd(int fd, size_t min_inflight_sessions) {
  if (fd < 0 || min_inflight_sessions == 0) {
    errno = EINVAL;
    return -1;
  }
  g_progress_fd = fd;
  g_progress_min_inflight_sessions = min_inflight_sessions;
  g_progress_reported = 0;
  return 0;
}

int odin_cli_client_test_set_runtime_trigger_fd(int fd) {
  if (fd < 0) {
    errno = EINVAL;
    return -1;
  }
  g_runtime_trigger_fd = fd;
  g_runtime_trigger_released = 0;
  return 0;
}

int odin_cli_client_test_set_idle_snapshot_fd(int fd,
                                              size_t min_closed_sessions) {
  if (fd < 0 || min_closed_sessions == 0) {
    errno = EINVAL;
    return -1;
  }
  g_idle_snapshot_fd = fd;
  g_idle_snapshot_min_closed_sessions = min_closed_sessions;
  g_idle_snapshot_reported = 0;
  return 0;
}
