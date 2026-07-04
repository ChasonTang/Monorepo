/* odin/testing/cli_client_testing.c */

#include "odin/cli_client.c" // NOLINT(bugprone-suspicious-include)

#include "odin/testing/cli_client_internal_test.h"

#include <errno.h>
#include <string.h>

static int valid_fail_next_fp(odin_cli_client_test_failpoint_t fp) {
  switch (fp) {
  case ODIN_CLI_CLIENT_TEST_FAIL_SOCKET:
  case ODIN_CLI_CLIENT_TEST_FAIL_SETSOCKOPT_REUSEADDR:
  case ODIN_CLI_CLIENT_TEST_FAIL_FCNTL_GETFL:
  case ODIN_CLI_CLIENT_TEST_FAIL_FCNTL_SETFL:
  case ODIN_CLI_CLIENT_TEST_FAIL_BIND:
  case ODIN_CLI_CLIENT_TEST_FAIL_LISTEN:
  case ODIN_CLI_CLIENT_TEST_FAIL_GETSOCKNAME:
  case ODIN_CLI_CLIENT_TEST_FAIL_EVENT_LOOP_CREATE:
  case ODIN_CLI_CLIENT_TEST_FAIL_ACCEPT_LOOP_CREATE:
  case ODIN_CLI_CLIENT_TEST_FAIL_SIGACTION_SIGINT:
  case ODIN_CLI_CLIENT_TEST_FAIL_SIGACTION_SIGTERM:
  case ODIN_CLI_CLIENT_TEST_FAIL_SIGNAL_TIMER_START:
  case ODIN_CLI_CLIENT_TEST_FAIL_EVENT_LOOP_RUN:
  case ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_ERROR:
  case ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_GETFL_ERROR:
  case ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_SETFL_ERROR:
  case ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_CREATE:
  case ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_START:
  case ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_ADD_CONNECTION:
    break;
  default:
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
  g_live_xqc_client_runtimes = 0;
  g_quic_runtime_create_calls = 0;
  g_quic_runtime_start_calls = 0;
  g_quic_runtime_add_connection_calls = 0;
  g_quic_runtime_force_destroy_calls = 0;
  g_last_bind_addr_recorded = 0;
  memset(&g_last_bind_addr, 0, sizeof(g_last_bind_addr));
  g_last_bound_addr_recorded = 0;
  memset(&g_last_bound_addr, 0, sizeof(g_last_bound_addr));
  g_last_xqc_add_recorded = 0;
  memset(&g_last_xqc_add, 0, sizeof(g_last_xqc_add));
  g_last_runtime_config_recorded = 0;
  memset(&g_last_runtime_config, 0, sizeof(g_last_runtime_config));
  g_progress_fd = -1;
  g_progress_min_inflight_sessions = 0;
  g_progress_reported = 0;
  g_runtime_trigger_fd = -1;
  g_runtime_trigger_released = 0;
  g_failpoint = ODIN_CLI_CLIENT_TEST_FAIL_NONE;
  g_failpoint_errno = 0;
}

int odin_cli_client_test_liveness(odin_cli_client_test_liveness_t *out) {
  if (out == NULL) {
    errno = EINVAL;
    return -1;
  }
  out->live_listeners = g_live_listeners;
  out->live_accept_loops = g_live_accept_loops;
  out->live_xqc_client_runtimes = g_live_xqc_client_runtimes;
  out->quic_runtime_create_calls = g_quic_runtime_create_calls;
  out->quic_runtime_start_calls = g_quic_runtime_start_calls;
  out->quic_runtime_add_connection_calls = g_quic_runtime_add_connection_calls;
  out->quic_runtime_force_destroy_calls = g_quic_runtime_force_destroy_calls;
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

int odin_cli_client_test_last_bound_addr(struct sockaddr_in *out) {
  if (out == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (!g_last_bound_addr_recorded) {
    errno = ENOENT;
    return -1;
  }
  *out = g_last_bound_addr;
  return 0;
}

int odin_cli_client_test_last_xqc_add(
    odin_cli_client_test_xqc_add_record_t *out) {
  if (out == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (!g_last_xqc_add_recorded) {
    errno = ENOENT;
    return -1;
  }
  *out = g_last_xqc_add;
  return 0;
}

int odin_cli_client_test_last_runtime_config(
    odin_cli_client_test_runtime_config_record_t *out) {
  if (out == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (!g_last_runtime_config_recorded) {
    errno = ENOENT;
    return -1;
  }
  *out = g_last_runtime_config;
  return 0;
}

int odin_cli_client_test_pending_failpoint(
    odin_cli_client_test_failpoint_t *out) {
  if (out == NULL) {
    errno = EINVAL;
    return -1;
  }
  *out = g_failpoint;
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
