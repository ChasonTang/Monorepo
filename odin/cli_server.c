/* odin/cli_server.c -- RFC-022 server runner.
 *
 * Binds an IPv4 listener on 0.0.0.0:<listen_port>, creates the event
 * loop, server runtime, default SSRF dial filter, and signal-driven
 * stop polling timer, then runs the loop. All setup failures route
 * through one cleanup that releases CLI-owned objects in reverse
 * creation order and prints one deterministic line on err.
 */

#include "odin/cli_server.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "odin/event_loop.h"
#include "odin/server_runtime.h"
#include "odin/server_session.h"

#if defined(ODIN_CLI_SERVER_TESTING)
#include "odin/cli_server_internal_test.h"
#if defined(ODIN_SERVER_RUNTIME_TESTING)
#include "odin/server_runtime_internal_test.h"
#endif
#endif

#define ODIN_CLI_SERVER_SIGNAL_POLL_INTERVAL_US 50000u

typedef struct cli_server_state_t {
  int listen_fd;
  odin_event_loop_t *loop;
  odin_server_runtime_t *runtime;
  odin_event_timer_t *signal_timer;
  int sigint_replaced;
  int sigterm_replaced;
  struct sigaction old_sigint;
  struct sigaction old_sigterm;
  int runtime_error_seen;
  int runtime_error_errno;
  int shutdown_requested;
} cli_server_state_t;

static volatile sig_atomic_t g_odin_cli_server_signal_seen;

#if defined(ODIN_CLI_SERVER_TESTING)
static odin_cli_server_test_failpoint_t g_failpoint;
static int g_failpoint_errno;
static size_t g_live_listeners;
static size_t g_live_runtimes;
static size_t g_last_cleanup_runtime_inflight;
static int g_last_bind_addr_recorded;
static struct sockaddr_in g_last_bind_addr;
static int g_progress_fd = -1;
static int g_progress_reported;
static int g_probe_fd = -1;
static int g_probe_errno;

static int test_consume_failpoint(odin_cli_server_test_failpoint_t fp) {
  if (g_failpoint != fp) {
    return 0;
  }
  const int err = g_failpoint_errno;
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  g_failpoint = (odin_cli_server_test_failpoint_t)0;
  g_failpoint_errno = 0;
  errno = err;
  return -1;
}
#endif

static void cli_signal_handler(int signum) {
  g_odin_cli_server_signal_seen = signum;
}

static int range_match(uint32_t ip, uint32_t base, int prefix) {
  const uint32_t mask =
      prefix == 0 ? 0u : (uint32_t)(0xffffffffu << (32 - prefix));
  return (ip & mask) == (base & mask);
}

static int default_filter_check(const struct sockaddr *addr, socklen_t addrlen) {
  if (addr == NULL || addrlen < (socklen_t)sizeof(struct sockaddr_in)) {
    return EAFNOSUPPORT;
  }
  const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
  if (sin->sin_family != AF_INET) {
    return EAFNOSUPPORT;
  }
  const uint32_t ip = ntohl(sin->sin_addr.s_addr);
  if (range_match(ip, 0x00000000u, 8) || range_match(ip, 0x0A000000u, 8) ||
      range_match(ip, 0x64400000u, 10) || range_match(ip, 0x7F000000u, 8) ||
      range_match(ip, 0xA9FE0000u, 16) || range_match(ip, 0xAC100000u, 12) ||
      range_match(ip, 0xC0000000u, 24) || range_match(ip, 0xC0000200u, 24) ||
      range_match(ip, 0xC0A80000u, 16) || range_match(ip, 0xC6120000u, 15) ||
      range_match(ip, 0xC6336400u, 24) || range_match(ip, 0xCB007100u, 24) ||
      range_match(ip, 0xE0000000u, 4) || range_match(ip, 0xF0000000u, 4)) {
    return EACCES;
  }
  return 0;
}

static int odin_cli_default_server_dial_filter(const struct sockaddr *addr,
                                               socklen_t addrlen,
                                               void *user_data) {
  (void)user_data;
  return default_filter_check(addr, addrlen);
}

static int startup_fail(cli_server_state_t *state, FILE *err,
                        const char *step);

static void cli_runtime_on_error(odin_server_runtime_t *rt, int err,
                                 void *user_data) {
  (void)rt;
  cli_server_state_t *state = (cli_server_state_t *)user_data;
  state->runtime_error_seen = 1;
  state->runtime_error_errno = err;
  odin_event_loop_stop(state->loop);
}

static void cli_signal_poll_timer(odin_event_loop_t *loop,
                                  odin_event_timer_t *timer, void *user_data) {
  cli_server_state_t *state = (cli_server_state_t *)user_data;
#if defined(ODIN_CLI_SERVER_TESTING)
#if defined(ODIN_SERVER_RUNTIME_TESTING)
  if (g_progress_fd >= 0 && !g_progress_reported && state->runtime != NULL &&
      odin_server_runtime_test_inflight_count(state->runtime) > 0) {
    const char b = 1;
    (void)write(g_progress_fd, &b, 1);
    g_progress_reported = 1;
  }
#endif
  if (g_failpoint == ODIN_CLI_SERVER_TEST_TRIGGER_RUNTIME_ERROR) {
    const int err = g_failpoint_errno;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    g_failpoint = (odin_cli_server_test_failpoint_t)0;
    g_failpoint_errno = 0;
    cli_runtime_on_error(state->runtime, err, state);
    return;
  }
#endif
  if (g_odin_cli_server_signal_seen == 0) {
    return;
  }
  state->shutdown_requested = 1;
  state->signal_timer = NULL;
  odin_event_timer_stop(timer);
  odin_event_loop_stop(loop);
}

static const char *install_signal_handlers(cli_server_state_t *state) {
  g_odin_cli_server_signal_seen = 0;
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = cli_signal_handler;
  sigemptyset(&sa.sa_mask);
#if defined(ODIN_CLI_SERVER_TESTING)
  if (test_consume_failpoint(ODIN_CLI_SERVER_TEST_FAIL_SIGACTION_SIGINT) !=
      0) {
    return "sigaction(SIGINT)";
  }
#endif
  if (sigaction(SIGINT, &sa, &state->old_sigint) != 0) {
    return "sigaction(SIGINT)";
  }
  state->sigint_replaced = 1;
#if defined(ODIN_CLI_SERVER_TESTING)
  if (test_consume_failpoint(ODIN_CLI_SERVER_TEST_FAIL_SIGACTION_SIGTERM) !=
      0) {
    return "sigaction(SIGTERM)";
  }
#endif
  if (sigaction(SIGTERM, &sa, &state->old_sigterm) != 0) {
    return "sigaction(SIGTERM)";
  }
  state->sigterm_replaced = 1;
  return NULL;
}

static void restore_signal_handlers(cli_server_state_t *state) {
  if (state->sigterm_replaced) {
    (void)sigaction(SIGTERM, &state->old_sigterm, NULL);
    state->sigterm_replaced = 0;
  }
  if (state->sigint_replaced) {
    (void)sigaction(SIGINT, &state->old_sigint, NULL);
    state->sigint_replaced = 0;
  }
}

static void cleanup_all(cli_server_state_t *state) {
  if (state->signal_timer != NULL) {
    odin_event_timer_stop(state->signal_timer);
    state->signal_timer = NULL;
  }
  if (state->runtime != NULL) {
#if defined(ODIN_CLI_SERVER_TESTING) && defined(ODIN_SERVER_RUNTIME_TESTING)
    g_last_cleanup_runtime_inflight =
        (size_t)odin_server_runtime_test_inflight_count(state->runtime);
#endif
    odin_server_runtime_destroy(state->runtime);
    state->runtime = NULL;
#if defined(ODIN_CLI_SERVER_TESTING)
    g_live_runtimes -= 1;
#endif
  }
  if (state->loop != NULL) {
    odin_event_loop_destroy(state->loop);
    state->loop = NULL;
  }
  if (state->listen_fd >= 0) {
    close(state->listen_fd);
    state->listen_fd = -1;
#if defined(ODIN_CLI_SERVER_TESTING)
    g_live_listeners -= 1;
#endif
  }
  restore_signal_handlers(state);
}

static int startup_fail(cli_server_state_t *state, FILE *err,
                        const char *step) {
  // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
  (void)fprintf(err, "odin: server startup failed at %s\n", step);
  (void)fflush(err);
  cleanup_all(state);
  return 1;
}

int odin_cli_run_server(uint16_t listen_port, FILE *err) {
  cli_server_state_t state;
  memset(&state, 0, sizeof(state));
  state.listen_fd = -1;

#if defined(ODIN_CLI_SERVER_TESTING)
  g_progress_reported = 0;
#endif

#if defined(ODIN_CLI_SERVER_TESTING)
  if (test_consume_failpoint(ODIN_CLI_SERVER_TEST_FAIL_SOCKET) != 0) {
    return startup_fail(&state, err, "socket");
  }
#endif
  state.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (state.listen_fd < 0) {
    return startup_fail(&state, err, "socket");
  }
#if defined(ODIN_CLI_SERVER_TESTING)
  g_live_listeners += 1;
#endif

  const int reuse = 1;
#if defined(ODIN_CLI_SERVER_TESTING)
  if (test_consume_failpoint(
          ODIN_CLI_SERVER_TEST_FAIL_SETSOCKOPT_REUSEADDR) != 0) {
    return startup_fail(&state, err, "setsockopt(SO_REUSEADDR)");
  }
#endif
  if (setsockopt(state.listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse,
                 sizeof(reuse)) != 0) {
    return startup_fail(&state, err, "setsockopt(SO_REUSEADDR)");
  }

#if defined(ODIN_CLI_SERVER_TESTING)
  if (test_consume_failpoint(ODIN_CLI_SERVER_TEST_FAIL_FCNTL_GETFL) != 0) {
    return startup_fail(&state, err, "fcntl(F_GETFL)");
  }
#endif
  const int flags = fcntl(state.listen_fd, F_GETFL, 0);
  if (flags == -1) {
    return startup_fail(&state, err, "fcntl(F_GETFL)");
  }
#if defined(ODIN_CLI_SERVER_TESTING)
  if (test_consume_failpoint(ODIN_CLI_SERVER_TEST_FAIL_FCNTL_SETFL) != 0) {
    return startup_fail(&state, err, "fcntl(F_SETFL)");
  }
#endif
  if (fcntl(state.listen_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    return startup_fail(&state, err, "fcntl(F_SETFL)");
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(listen_port);
#if defined(ODIN_CLI_SERVER_TESTING)
  g_last_bind_addr = addr;
  g_last_bind_addr_recorded = 1;
  if (test_consume_failpoint(ODIN_CLI_SERVER_TEST_FAIL_BIND) != 0) {
    return startup_fail(&state, err, "bind");
  }
#endif
  if (bind(state.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    return startup_fail(&state, err, "bind");
  }

#if defined(ODIN_CLI_SERVER_TESTING)
  if (test_consume_failpoint(ODIN_CLI_SERVER_TEST_FAIL_LISTEN) != 0) {
    return startup_fail(&state, err, "listen");
  }
#endif
  if (listen(state.listen_fd, SOMAXCONN) != 0) {
    return startup_fail(&state, err, "listen");
  }

#if defined(ODIN_CLI_SERVER_TESTING)
  if (test_consume_failpoint(ODIN_CLI_SERVER_TEST_FAIL_GETSOCKNAME) != 0) {
    return startup_fail(&state, err, "getsockname");
  }
#endif
  struct sockaddr_in bound;
  socklen_t blen = sizeof(bound);
  if (getsockname(state.listen_fd, (struct sockaddr *)&bound, &blen) != 0) {
    return startup_fail(&state, err, "getsockname");
  }
  const uint16_t actual_port = ntohs(bound.sin_port);

#if defined(ODIN_CLI_SERVER_TESTING)
  if (test_consume_failpoint(ODIN_CLI_SERVER_TEST_FAIL_EVENT_LOOP_CREATE) !=
      0) {
    return startup_fail(&state, err, "event_loop_create");
  }
#endif
  if (odin_event_loop_create(&state.loop) != 0) {
    return startup_fail(&state, err, "event_loop_create");
  }

#if defined(ODIN_CLI_SERVER_TESTING)
  if (test_consume_failpoint(
          ODIN_CLI_SERVER_TEST_FAIL_SERVER_RUNTIME_CREATE) != 0) {
    return startup_fail(&state, err, "server_runtime_create");
  }
#endif
  if (odin_server_runtime_create(state.loop, state.listen_fd,
                                 cli_runtime_on_error, &state,
                                 &state.runtime) != 0) {
    return startup_fail(&state, err, "server_runtime_create");
  }
#if defined(ODIN_CLI_SERVER_TESTING)
  g_live_runtimes += 1;
#endif

  odin_server_runtime_set_dial_filter(
      state.runtime, odin_cli_default_server_dial_filter, NULL);

  const char *sig_fail = install_signal_handlers(&state);
  if (sig_fail != NULL) {
    return startup_fail(&state, err, sig_fail);
  }

#if defined(ODIN_CLI_SERVER_TESTING)
  if (test_consume_failpoint(ODIN_CLI_SERVER_TEST_FAIL_SIGNAL_TIMER_START) !=
      0) {
    return startup_fail(&state, err, "signal_timer_start");
  }
#endif
  if (odin_event_timer_start(state.loop,
                             ODIN_CLI_SERVER_SIGNAL_POLL_INTERVAL_US,
                             ODIN_CLI_SERVER_SIGNAL_POLL_INTERVAL_US,
                             cli_signal_poll_timer, &state,
                             &state.signal_timer) != 0) {
    return startup_fail(&state, err, "signal_timer_start");
  }

  // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
  (void)fprintf(err, "odin: mode=server listen=%u\n", (unsigned)actual_port);
  (void)fflush(err);

  int run_rc = 0;
#if defined(ODIN_CLI_SERVER_TESTING)
  if (test_consume_failpoint(ODIN_CLI_SERVER_TEST_FAIL_EVENT_LOOP_RUN) != 0) {
    run_rc = -1;
  } else {
    run_rc = odin_event_loop_run(state.loop);
  }
#else
  run_rc = odin_event_loop_run(state.loop);
#endif

  if (state.runtime_error_seen) {
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    (void)fprintf(err, "odin: server runtime failed at accept_loop\n");
    (void)fflush(err);
    cleanup_all(&state);
    return 1;
  }
  if (run_rc != 0) {
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    (void)fprintf(err, "odin: server runtime failed at event_loop_run\n");
    (void)fflush(err);
    cleanup_all(&state);
    return 1;
  }
  cleanup_all(&state);
  return state.shutdown_requested ? 0 : 1;
}
