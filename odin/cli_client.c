/* odin/cli_client.c -- RFC-024 CLI client runner. */

#include "odin/cli_client.h"

#include <arpa/inet.h>
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

#include "odin/accept_loop.h"
#include "odin/client_session.h"
#include "odin/client_xqc_runtime.h"
#include "odin/event_loop.h"
#include "odin/protocol.h"

#if defined(ODIN_CLI_CLIENT_TESTING)
#include "odin/testing/accept_loop_internal_test.h"
#include "odin/testing/cli_client_internal_test.h"
#include "odin/testing/event_loop_internal_test.h"
#endif

#define ODIN_CLI_CLIENT_SIGNAL_POLL_INTERVAL_US 50000u

typedef struct cli_client_state_t cli_client_state_t;

typedef struct cli_client_session_entry_t {
  struct cli_client_session_entry_t *next;
  odin_client_session_t *session;
  cli_client_state_t *owner;
} cli_client_session_entry_t;

struct cli_client_state_t {
  int listen_fd;
  int test_wakeup_fd;
  uint16_t actual_port;
  const char *server_host;
  size_t server_host_len;
  uint16_t server_port;
  odin_event_loop_t *loop;
  odin_accept_loop_t *accept_loop;
  odin_xqc_client_runtime_t *quic_rt;
  odin_event_timer_t *signal_timer;
  int sigint_replaced;
  int sigterm_replaced;
  struct sigaction old_sigint;
  struct sigaction old_sigterm;
  int accept_loop_error_seen;
  int accept_loop_errno;
  int shutdown_requested;
  cli_client_session_entry_t *sessions;
  size_t inflight_sessions;
#if defined(ODIN_CLI_CLIENT_TESTING)
  size_t closed_sessions;
#endif
};

static volatile sig_atomic_t g_odin_cli_client_signal_seen;

#if defined(ODIN_CLI_CLIENT_TESTING)
static odin_cli_client_test_failpoint_t g_failpoint;
static int g_failpoint_errno;
static size_t g_live_listeners;
static size_t g_live_accept_loops;
static size_t g_live_sessions;
static size_t g_last_cleanup_sessions;
static size_t g_live_xqc_client_runtimes;
static size_t g_quic_runtime_create_calls;
static size_t g_quic_runtime_start_calls;
static size_t g_quic_runtime_add_connection_calls;
static size_t g_quic_runtime_force_destroy_calls;
static int g_last_bind_addr_recorded;
static struct sockaddr_in g_last_bind_addr;
static int g_last_bound_addr_recorded;
static struct sockaddr_in g_last_bound_addr;
static int g_last_xqc_add_recorded;
static odin_cli_client_test_xqc_add_record_t g_last_xqc_add;
static int g_last_runtime_config_recorded;
static odin_cli_client_test_runtime_config_record_t g_last_runtime_config;
static int g_progress_fd = -1;
static size_t g_progress_min_inflight_sessions;
static int g_progress_reported;
static int g_runtime_trigger_fd = -1;
static int g_runtime_trigger_released;
static int g_idle_snapshot_fd = -1;
static size_t g_idle_snapshot_min_closed_sessions;
static int g_idle_snapshot_reported;

static int test_consume_failpoint(odin_cli_client_test_failpoint_t fp) {
  if (g_failpoint != fp) {
    return 0;
  }
  const int err = g_failpoint_errno;
  g_failpoint = ODIN_CLI_CLIENT_TEST_FAIL_NONE;
  g_failpoint_errno = 0;
  errno = err;
  return -1;
}

static int test_runtime_failpoint_armed(void) {
  switch (g_failpoint) {
  case ODIN_CLI_CLIENT_TEST_FAIL_EVENT_LOOP_RUN:
  case ODIN_CLI_CLIENT_TEST_TRIGGER_UNEXPECTED_STOP:
  case ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_ERROR:
  case ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_GETFL_ERROR:
  case ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_SETFL_ERROR:
    return 1;
  default:
    return 0;
  }
}
#endif

static void cli_client_signal_handler(int signum) {
  g_odin_cli_client_signal_seen = signum;
}

static int copy_ipv4_server_endpoint(const char *server_host,
                                     size_t server_host_len,
                                     char *server_host_cstr,
                                     struct in_addr *server_addr) {
  if (server_host_len == 0 || server_host_len > ODIN_PROTO_HOST_MAX) {
    errno = EINVAL;
    return -1;
  }
  memcpy(server_host_cstr, server_host, server_host_len);
  server_host_cstr[server_host_len] = '\0';
  if (inet_pton(AF_INET, server_host_cstr, server_addr) != 1) {
    errno = EINVAL;
    return -1;
  }
  return 0;
}

static int validate_ipv4_server_endpoint(const char *server_host,
                                         size_t server_host_len) {
  char server_host_cstr[ODIN_PROTO_HOST_MAX + 1];
  struct in_addr addr;
  return copy_ipv4_server_endpoint(server_host, server_host_len,
                                   server_host_cstr, &addr);
}

#if defined(ODIN_CLI_CLIENT_TESTING)
static int set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int open_test_wakeup_fd(uint16_t port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  if (set_nonblocking(fd) != 0) {
    const int saved = errno;
    (void)close(fd);
    errno = saved;
    return -1;
  }
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  const int rc =
      connect(fd, (const struct sockaddr *)&addr, (socklen_t)sizeof(addr));
  if (rc == 0 || (rc < 0 && errno == EINPROGRESS)) {
    return fd;
  }
  const int saved = errno;
  (void)close(fd);
  errno = saved;
  return -1;
}
#endif

static void
record_bound_addr_after_getsockname(const struct sockaddr_in *bound) {
#if defined(ODIN_CLI_CLIENT_TESTING)
  g_last_bound_addr = *bound;
  g_last_bound_addr_recorded = 1;
#else
  (void)bound;
#endif
}

static int quic_runtime_create_default_call(
    const odin_xqc_client_runtime_default_config_t *config,
    odin_xqc_client_runtime_t **out) {
#if defined(ODIN_CLI_CLIENT_TESTING)
  memset(&g_last_runtime_config, 0, sizeof(g_last_runtime_config));
  if (config != NULL) {
    g_last_runtime_config.local_addr = config->local_addr;
    g_last_runtime_config.local_addrlen = config->local_addrlen;
    if (config->local_addr != NULL &&
        config->local_addrlen <=
            (socklen_t)sizeof(g_last_runtime_config.local_addr_value)) {
      memcpy(&g_last_runtime_config.local_addr_value, config->local_addr,
             config->local_addrlen);
    }
    g_last_runtime_config.peer_addr = config->peer_addr;
    g_last_runtime_config.peer_addrlen = config->peer_addrlen;
    if (config->peer_addr != NULL &&
        config->peer_addrlen <=
            (socklen_t)sizeof(g_last_runtime_config.peer_addr_value)) {
      memcpy(&g_last_runtime_config.peer_addr_value, config->peer_addr,
             config->peer_addrlen);
    }
    g_last_runtime_config.server_host = config->server_host;
    if (config->server_host != NULL) {
      g_last_runtime_config.server_host_len = strlen(config->server_host);
      if (g_last_runtime_config.server_host_len > ODIN_PROTO_HOST_MAX) {
        g_last_runtime_config.server_host_len = ODIN_PROTO_HOST_MAX;
      }
      memcpy(g_last_runtime_config.server_host_value, config->server_host,
             g_last_runtime_config.server_host_len);
      g_last_runtime_config
          .server_host_value[g_last_runtime_config.server_host_len] = '\0';
    }
    g_last_runtime_config_recorded = 1;
  }
  if (test_consume_failpoint(
          ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_CREATE) != 0) {
    return -1;
  }
#endif
  const int rc = odin_xqc_client_runtime_create_default(config, out);
#if defined(ODIN_CLI_CLIENT_TESTING)
  if (rc == 0) {
    g_live_xqc_client_runtimes += 1;
  }
  g_quic_runtime_create_calls += 1;
#endif
  return rc;
}

static int quic_runtime_start_call(odin_xqc_client_runtime_t *rt) {
#if defined(ODIN_CLI_CLIENT_TESTING)
  if (test_consume_failpoint(
          ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_START) != 0) {
    return -1;
  }
  g_quic_runtime_start_calls += 1;
#endif
  return odin_xqc_client_runtime_start(rt);
}

static int quic_runtime_add_connection_call(odin_xqc_client_runtime_t *rt,
                                            int conn_fd) {
#if defined(ODIN_CLI_CLIENT_TESTING)
  memset(&g_last_xqc_add, 0, sizeof(g_last_xqc_add));
  g_last_xqc_add.fd = conn_fd;
  const int flags = fcntl(conn_fd, F_GETFL, 0);
  g_last_xqc_add.fd_is_nonblocking = flags >= 0 && (flags & O_NONBLOCK) != 0;
  g_last_xqc_add_recorded = 1;
  g_quic_runtime_add_connection_calls += 1;
  if (g_progress_fd >= 0 && !g_progress_reported &&
      g_quic_runtime_add_connection_calls >= g_progress_min_inflight_sessions) {
    const char b = 1;
    if (write(g_progress_fd, &b, 1) == 1) {
      g_progress_reported = 1;
    }
  }
  if (test_consume_failpoint(
          ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_ADD_CONNECTION) != 0) {
    return -1;
  }
#endif
  return odin_xqc_client_runtime_add_connection(rt, conn_fd);
}

static void quic_runtime_force_destroy_call(odin_xqc_client_runtime_t *rt) {
#if defined(ODIN_CLI_CLIENT_TESTING)
  if (rt != NULL) {
    g_quic_runtime_force_destroy_calls += 1;
  }
#endif
  odin_xqc_client_runtime_force_destroy(rt);
#if defined(ODIN_CLI_CLIENT_TESTING)
  if (rt != NULL && g_live_xqc_client_runtimes > 0) {
    g_live_xqc_client_runtimes -= 1;
  }
#endif
}

static void restore_signal_handlers(cli_client_state_t *state) {
  if (state->sigterm_replaced) {
    (void)sigaction(SIGTERM, &state->old_sigterm, NULL);
    state->sigterm_replaced = 0;
  }
  if (state->sigint_replaced) {
    (void)sigaction(SIGINT, &state->old_sigint, NULL);
    state->sigint_replaced = 0;
  }
}

static const char *install_signal_handlers(cli_client_state_t *state) {
  g_odin_cli_client_signal_seen = 0;
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = cli_client_signal_handler;
  sigemptyset(&sa.sa_mask);
#if defined(ODIN_CLI_CLIENT_TESTING)
  if (test_consume_failpoint(ODIN_CLI_CLIENT_TEST_FAIL_SIGACTION_SIGINT) != 0) {
    return "sigaction(SIGINT)";
  }
#endif
  if (sigaction(SIGINT, &sa, &state->old_sigint) != 0) {
    return "sigaction(SIGINT)";
  }
  state->sigint_replaced = 1;
#if defined(ODIN_CLI_CLIENT_TESTING)
  if (test_consume_failpoint(ODIN_CLI_CLIENT_TEST_FAIL_SIGACTION_SIGTERM) !=
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

static void destroy_all_sessions(cli_client_state_t *state) {
#if defined(ODIN_CLI_CLIENT_TESTING)
  g_last_cleanup_sessions = state->inflight_sessions;
#endif
  while (state->sessions != NULL) {
    cli_client_session_entry_t *entry = state->sessions;
    state->sessions = entry->next;
    if (state->inflight_sessions > 0) {
      state->inflight_sessions -= 1;
    }
#if defined(ODIN_CLI_CLIENT_TESTING)
    if (g_live_sessions > 0) {
      g_live_sessions -= 1;
    }
#endif
    odin_client_session_destroy(entry->session);
    free(entry);
  }
}

static void cleanup_all(cli_client_state_t *state) {
  destroy_all_sessions(state);
  if (state->accept_loop != NULL) {
    odin_accept_loop_destroy(state->accept_loop);
    state->accept_loop = NULL;
#if defined(ODIN_CLI_CLIENT_TESTING)
    if (g_live_accept_loops > 0) {
      g_live_accept_loops -= 1;
    }
#endif
  }
  if (state->signal_timer != NULL) {
    odin_event_timer_stop(state->signal_timer);
    state->signal_timer = NULL;
  }
  if (state->quic_rt != NULL) {
    quic_runtime_force_destroy_call(state->quic_rt);
    state->quic_rt = NULL;
  }
  if (state->loop != NULL) {
    odin_event_loop_destroy(state->loop);
    state->loop = NULL;
  }
  if (state->test_wakeup_fd >= 0) {
    (void)close(state->test_wakeup_fd);
    state->test_wakeup_fd = -1;
  }
  if (state->listen_fd >= 0) {
    (void)close(state->listen_fd);
    state->listen_fd = -1;
#if defined(ODIN_CLI_CLIENT_TESTING)
    if (g_live_listeners > 0) {
      g_live_listeners -= 1;
    }
#endif
  }
  restore_signal_handlers(state);
}

static int startup_fail(cli_client_state_t *state, FILE *err,
                        const char *step) {
  if (err != NULL) {
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    (void)fprintf(err, "odin: client startup failed at %s\n", step);
    (void)fflush(err);
  }
  cleanup_all(state);
  return 1;
}

static void cli_client_session_on_close(odin_client_session_t *cs, int err,
                                        void *user_data) {
  (void)err;
  cli_client_session_entry_t *entry = (cli_client_session_entry_t *)user_data;
  cli_client_state_t *state = entry->owner;
  cli_client_session_entry_t **cur = &state->sessions;
  while (*cur != NULL && *cur != entry) {
    cur = &(*cur)->next;
  }
  if (*cur == entry) {
    *cur = entry->next;
  }
  if (state->inflight_sessions > 0) {
    state->inflight_sessions -= 1;
  }
#if defined(ODIN_CLI_CLIENT_TESTING)
  if (g_live_sessions > 0) {
    g_live_sessions -= 1;
  }
#endif
  odin_client_session_destroy(cs);
#if defined(ODIN_CLI_CLIENT_TESTING)
  state->closed_sessions += 1;
#endif
  free(entry);
}

static void cli_client_on_accept(odin_accept_loop_t *al, int conn_fd,
                                 void *user_data) {
  (void)al;
  cli_client_state_t *state = (cli_client_state_t *)user_data;
#if defined(ODIN_CLI_CLIENT_TESTING)
  if (test_consume_failpoint(
          ODIN_CLI_CLIENT_TEST_FAIL_NEXT_SESSION_ENTRY_ALLOC) != 0) {
    (void)close(conn_fd);
    return;
  }
#endif
  cli_client_session_entry_t *entry =
      (cli_client_session_entry_t *)calloc(1, sizeof(*entry));
  if (entry == NULL) {
    (void)close(conn_fd);
    return;
  }
  entry->owner = state;
#if defined(ODIN_CLI_CLIENT_TESTING)
  if (test_consume_failpoint(
          ODIN_CLI_CLIENT_TEST_FAIL_NEXT_CLIENT_SESSION_CREATE) != 0) {
    const int saved = errno;
    free(entry);
    (void)close(conn_fd);
    errno = saved;
    return;
  }
#endif
  if (odin_client_session_create(state->loop, conn_fd, state->server_host,
                                 state->server_host_len, state->server_port,
                                 cli_client_session_on_close, entry,
                                 &entry->session) != 0) {
    const int saved = errno;
    free(entry);
    (void)close(conn_fd);
    errno = saved;
    return;
  }
  entry->next = state->sessions;
  state->sessions = entry;
  state->inflight_sessions += 1;
#if defined(ODIN_CLI_CLIENT_TESTING)
  g_live_sessions += 1;
#endif
}

static void cli_client_quic_on_accept(odin_accept_loop_t *al, int conn_fd,
                                      void *user_data) {
  (void)al;
  cli_client_state_t *state = (cli_client_state_t *)user_data;
  if (quic_runtime_add_connection_call(state->quic_rt, conn_fd) != 0) {
    (void)close(conn_fd);
  }
}

static void cli_client_on_accept_loop_error(odin_accept_loop_t *al, int err,
                                            void *user_data) {
  (void)al;
  cli_client_state_t *state = (cli_client_state_t *)user_data;
  state->accept_loop_error_seen = 1;
  state->accept_loop_errno = err;
  odin_event_loop_stop(state->loop);
}

#if defined(ODIN_CLI_CLIENT_TESTING)
static void
cli_client_test_maybe_write_progress(const cli_client_state_t *state) {
  if (g_progress_fd < 0 || g_progress_reported ||
      state->inflight_sessions < g_progress_min_inflight_sessions) {
    return;
  }
  const char b = 1;
  if (write(g_progress_fd, &b, 1) == 1) {
    g_progress_reported = 1;
  }
}

static void
cli_client_test_maybe_write_idle_snapshot(const cli_client_state_t *state) {
  if (g_idle_snapshot_fd < 0 || g_idle_snapshot_reported ||
      state->closed_sessions < g_idle_snapshot_min_closed_sessions ||
      state->inflight_sessions != 0) {
    return;
  }
  odin_cli_client_test_liveness_t snap;
  memset(&snap, 0, sizeof(snap));
  snap.live_listeners = g_live_listeners;
  snap.live_accept_loops = g_live_accept_loops;
  snap.live_sessions = g_live_sessions;
  snap.last_cleanup_sessions = g_last_cleanup_sessions;
  if (write(g_idle_snapshot_fd, &snap, sizeof(snap)) == (ssize_t)sizeof(snap)) {
    g_idle_snapshot_reported = 1;
  }
}

static int cli_client_test_runtime_trigger_released(void) {
  if (g_runtime_trigger_fd < 0 || g_runtime_trigger_released) {
    return 1;
  }
  char b = 0;
  const ssize_t n = read(g_runtime_trigger_fd, &b, 1);
  if (n == 1) {
    g_runtime_trigger_released = 1;
    return 1;
  }
  return 0;
}

static void cli_client_test_open_wakeup(cli_client_state_t *state) {
  if (state->test_wakeup_fd >= 0) {
    (void)close(state->test_wakeup_fd);
    state->test_wakeup_fd = -1;
  }
  state->test_wakeup_fd = open_test_wakeup_fd(state->actual_port);
}

#if !defined(__linux__)
static void
cli_client_test_prearm_external_fcntl_trigger(cli_client_state_t *state) {
  if (g_runtime_trigger_fd < 0) {
    return;
  }
  switch (g_failpoint) {
  case ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_GETFL_ERROR: {
    const int err = g_failpoint_errno;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    g_failpoint = (odin_cli_client_test_failpoint_t)0;
    g_failpoint_errno = 0;
    (void)odin_accept_loop_test_fail_next_fcntl(state->accept_loop, F_GETFL,
                                                err);
    return;
  }
  case ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_SETFL_ERROR: {
    const int err = g_failpoint_errno;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    g_failpoint = (odin_cli_client_test_failpoint_t)0;
    g_failpoint_errno = 0;
    (void)odin_accept_loop_test_fail_next_fcntl(state->accept_loop, F_SETFL,
                                                err);
    return;
  }
  default:
    return;
  }
}
#endif

static void cli_client_signal_timer_test_branch(odin_event_loop_t *loop,
                                                cli_client_state_t *state) {
  cli_client_test_maybe_write_progress(state);
  cli_client_test_maybe_write_idle_snapshot(state);
  if (!test_runtime_failpoint_armed()) {
    return;
  }
  if (g_progress_fd >= 0 && !g_progress_reported) {
    return;
  }
  if (!cli_client_test_runtime_trigger_released()) {
    return;
  }
  switch (g_failpoint) {
  case ODIN_CLI_CLIENT_TEST_FAIL_EVENT_LOOP_RUN: {
    const int err = g_failpoint_errno;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    g_failpoint = (odin_cli_client_test_failpoint_t)0;
    g_failpoint_errno = 0;
    (void)odin_event_loop_test_fail_next_backend_wait(state->loop, err);
    return;
  }
  case ODIN_CLI_CLIENT_TEST_TRIGGER_UNEXPECTED_STOP:
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    g_failpoint = (odin_cli_client_test_failpoint_t)0;
    g_failpoint_errno = 0;
    odin_event_loop_stop(loop);
    return;
  case ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_ERROR: {
    const int err = g_failpoint_errno;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    g_failpoint = (odin_cli_client_test_failpoint_t)0;
    g_failpoint_errno = 0;
    (void)odin_accept_loop_test_fail_next_accept(state->accept_loop, err);
    cli_client_test_open_wakeup(state);
    return;
  }
#if !defined(__linux__)
  case ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_GETFL_ERROR: {
    const int err = g_failpoint_errno;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    g_failpoint = (odin_cli_client_test_failpoint_t)0;
    g_failpoint_errno = 0;
    (void)odin_accept_loop_test_fail_next_fcntl(state->accept_loop, F_GETFL,
                                                err);
    cli_client_test_open_wakeup(state);
    return;
  }
  case ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_SETFL_ERROR: {
    const int err = g_failpoint_errno;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    g_failpoint = (odin_cli_client_test_failpoint_t)0;
    g_failpoint_errno = 0;
    (void)odin_accept_loop_test_fail_next_fcntl(state->accept_loop, F_SETFL,
                                                err);
    cli_client_test_open_wakeup(state);
    return;
  }
#endif
  default:
    return;
  }
}
#endif

static void cli_client_signal_poll_timer(odin_event_loop_t *loop,
                                         odin_event_timer_t *timer,
                                         void *user_data) {
  cli_client_state_t *state = (cli_client_state_t *)user_data;
#if defined(ODIN_CLI_CLIENT_TESTING)
  cli_client_signal_timer_test_branch(loop, state);
#endif
  if (g_odin_cli_client_signal_seen == 0) {
    return;
  }
  state->shutdown_requested = 1;
  state->signal_timer = NULL;
  odin_event_timer_stop(timer);
  odin_event_loop_stop(loop);
}

static int run_tcp_client(const odin_cli_client_config_t *config, FILE *err) {
  cli_client_state_t state;
  memset(&state, 0, sizeof(state));
  state.listen_fd = -1;
  state.test_wakeup_fd = -1;
  state.server_host = config->server_host;
  state.server_host_len = config->server_host_len;
  state.server_port = config->server_port;

#if defined(ODIN_CLI_CLIENT_TESTING)
  g_progress_reported = 0;
  g_runtime_trigger_released = 0;
  g_idle_snapshot_reported = 0;
#endif

  if (validate_ipv4_server_endpoint(config->server_host,
                                    config->server_host_len) != 0) {
    return startup_fail(&state, err, "server_endpoint");
  }

#if defined(ODIN_CLI_CLIENT_TESTING)
  if (test_consume_failpoint(ODIN_CLI_CLIENT_TEST_FAIL_SOCKET) != 0) {
    return startup_fail(&state, err, "socket");
  }
#endif
  state.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (state.listen_fd < 0) {
    return startup_fail(&state, err, "socket");
  }
#if defined(ODIN_CLI_CLIENT_TESTING)
  g_live_listeners += 1;
#endif

  const int reuse = 1;
#if defined(ODIN_CLI_CLIENT_TESTING)
  if (test_consume_failpoint(ODIN_CLI_CLIENT_TEST_FAIL_SETSOCKOPT_REUSEADDR) !=
      0) {
    return startup_fail(&state, err, "setsockopt(SO_REUSEADDR)");
  }
#endif
  if (setsockopt(state.listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse,
                 sizeof(reuse)) != 0) {
    return startup_fail(&state, err, "setsockopt(SO_REUSEADDR)");
  }

#if defined(ODIN_CLI_CLIENT_TESTING)
  if (test_consume_failpoint(ODIN_CLI_CLIENT_TEST_FAIL_FCNTL_GETFL) != 0) {
    return startup_fail(&state, err, "fcntl(F_GETFL)");
  }
#endif
  const int flags = fcntl(state.listen_fd, F_GETFL, 0);
  if (flags == -1) {
    return startup_fail(&state, err, "fcntl(F_GETFL)");
  }
#if defined(ODIN_CLI_CLIENT_TESTING)
  if (test_consume_failpoint(ODIN_CLI_CLIENT_TEST_FAIL_FCNTL_SETFL) != 0) {
    return startup_fail(&state, err, "fcntl(F_SETFL)");
  }
#endif
  if (fcntl(state.listen_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    return startup_fail(&state, err, "fcntl(F_SETFL)");
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(config->listen_port);
#if defined(ODIN_CLI_CLIENT_TESTING)
  g_last_bind_addr = addr;
  g_last_bind_addr_recorded = 1;
  if (test_consume_failpoint(ODIN_CLI_CLIENT_TEST_FAIL_BIND) != 0) {
    return startup_fail(&state, err, "bind");
  }
#endif
  if (bind(state.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    return startup_fail(&state, err, "bind");
  }

#if defined(ODIN_CLI_CLIENT_TESTING)
  if (test_consume_failpoint(ODIN_CLI_CLIENT_TEST_FAIL_LISTEN) != 0) {
    return startup_fail(&state, err, "listen");
  }
#endif
  if (listen(state.listen_fd, SOMAXCONN) != 0) {
    return startup_fail(&state, err, "listen");
  }

#if defined(ODIN_CLI_CLIENT_TESTING)
  if (test_consume_failpoint(ODIN_CLI_CLIENT_TEST_FAIL_GETSOCKNAME) != 0) {
    return startup_fail(&state, err, "getsockname");
  }
#endif
  struct sockaddr_in bound;
  socklen_t blen = sizeof(bound);
  if (getsockname(state.listen_fd, (struct sockaddr *)&bound, &blen) != 0) {
    return startup_fail(&state, err, "getsockname");
  }
  state.actual_port = ntohs(bound.sin_port);

#if defined(ODIN_CLI_CLIENT_TESTING)
  if (test_consume_failpoint(ODIN_CLI_CLIENT_TEST_FAIL_EVENT_LOOP_CREATE) !=
      0) {
    return startup_fail(&state, err, "event_loop_create");
  }
#endif
  if (odin_event_loop_create(&state.loop) != 0) {
    return startup_fail(&state, err, "event_loop_create");
  }

#if defined(ODIN_CLI_CLIENT_TESTING)
  if (test_consume_failpoint(ODIN_CLI_CLIENT_TEST_FAIL_ACCEPT_LOOP_CREATE) !=
      0) {
    return startup_fail(&state, err, "accept_loop_create");
  }
#endif
  if (odin_accept_loop_create(state.loop, state.listen_fd, cli_client_on_accept,
                              cli_client_on_accept_loop_error, &state,
                              &state.accept_loop) != 0) {
    return startup_fail(&state, err, "accept_loop_create");
  }
#if defined(ODIN_CLI_CLIENT_TESTING)
  g_live_accept_loops += 1;
#endif

  const char *sig_fail = install_signal_handlers(&state);
  if (sig_fail != NULL) {
    return startup_fail(&state, err, sig_fail);
  }

#if defined(ODIN_CLI_CLIENT_TESTING)
  if (test_consume_failpoint(ODIN_CLI_CLIENT_TEST_FAIL_SIGNAL_TIMER_START) !=
      0) {
    return startup_fail(&state, err, "signal_timer_start");
  }
#endif
  if (odin_event_timer_start(
          state.loop, ODIN_CLI_CLIENT_SIGNAL_POLL_INTERVAL_US,
          ODIN_CLI_CLIENT_SIGNAL_POLL_INTERVAL_US, cli_client_signal_poll_timer,
          &state, &state.signal_timer) != 0) {
    return startup_fail(&state, err, "signal_timer_start");
  }

  // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
  (void)fprintf(err, "odin: mode=client listen=%u server=%.*s:%u\n",
                (unsigned)state.actual_port, (int)config->server_host_len,
                config->server_host, (unsigned)config->server_port);
  (void)fflush(err);

  const int run_rc = odin_event_loop_run(state.loop);
  if (state.accept_loop_error_seen) {
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    (void)fprintf(err, "odin: client runtime failed at accept_loop\n");
    (void)fflush(err);
    cleanup_all(&state);
    return 1;
  }
  if (run_rc != 0 || !state.shutdown_requested) {
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    (void)fprintf(err, "odin: client runtime failed at event_loop_run\n");
    (void)fflush(err);
    cleanup_all(&state);
    return 1;
  }
  cleanup_all(&state);
  return 0;
}

static int run_quic_client(const odin_cli_client_config_t *config, FILE *err) {
  cli_client_state_t state;
  memset(&state, 0, sizeof(state));
  state.listen_fd = -1;
  state.test_wakeup_fd = -1;
  state.server_host = config->server_host;
  state.server_host_len = config->server_host_len;
  state.server_port = config->server_port;

#if defined(ODIN_CLI_CLIENT_TESTING)
  g_progress_reported = 0;
  g_runtime_trigger_released = 0;
  g_idle_snapshot_reported = 0;
#endif

  char server_host_cstr[ODIN_PROTO_HOST_MAX + 1];
  struct in_addr server_addr;
  if (copy_ipv4_server_endpoint(config->server_host, config->server_host_len,
                                server_host_cstr, &server_addr) != 0) {
    return startup_fail(&state, err, "server_endpoint");
  }

#if defined(ODIN_CLI_CLIENT_TESTING)
  if (test_consume_failpoint(ODIN_CLI_CLIENT_TEST_FAIL_SOCKET) != 0) {
    return startup_fail(&state, err, "socket");
  }
#endif
  state.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (state.listen_fd < 0) {
    return startup_fail(&state, err, "socket");
  }
#if defined(ODIN_CLI_CLIENT_TESTING)
  g_live_listeners += 1;
#endif

  const int reuse = 1;
#if defined(ODIN_CLI_CLIENT_TESTING)
  if (test_consume_failpoint(ODIN_CLI_CLIENT_TEST_FAIL_SETSOCKOPT_REUSEADDR) !=
      0) {
    return startup_fail(&state, err, "setsockopt(SO_REUSEADDR)");
  }
#endif
  if (setsockopt(state.listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse,
                 sizeof(reuse)) != 0) {
    return startup_fail(&state, err, "setsockopt(SO_REUSEADDR)");
  }

#if defined(ODIN_CLI_CLIENT_TESTING)
  if (test_consume_failpoint(ODIN_CLI_CLIENT_TEST_FAIL_FCNTL_GETFL) != 0) {
    return startup_fail(&state, err, "fcntl(F_GETFL)");
  }
#endif
  const int flags = fcntl(state.listen_fd, F_GETFL, 0);
  if (flags == -1) {
    return startup_fail(&state, err, "fcntl(F_GETFL)");
  }
#if defined(ODIN_CLI_CLIENT_TESTING)
  if (test_consume_failpoint(ODIN_CLI_CLIENT_TEST_FAIL_FCNTL_SETFL) != 0) {
    return startup_fail(&state, err, "fcntl(F_SETFL)");
  }
#endif
  if (fcntl(state.listen_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    return startup_fail(&state, err, "fcntl(F_SETFL)");
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(config->listen_port);
#if defined(ODIN_CLI_CLIENT_TESTING)
  g_last_bind_addr = addr;
  g_last_bind_addr_recorded = 1;
  if (test_consume_failpoint(ODIN_CLI_CLIENT_TEST_FAIL_BIND) != 0) {
    return startup_fail(&state, err, "bind");
  }
#endif
  if (bind(state.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    return startup_fail(&state, err, "bind");
  }

#if defined(ODIN_CLI_CLIENT_TESTING)
  if (test_consume_failpoint(ODIN_CLI_CLIENT_TEST_FAIL_LISTEN) != 0) {
    return startup_fail(&state, err, "listen");
  }
#endif
  if (listen(state.listen_fd, SOMAXCONN) != 0) {
    return startup_fail(&state, err, "listen");
  }

#if defined(ODIN_CLI_CLIENT_TESTING)
  if (test_consume_failpoint(ODIN_CLI_CLIENT_TEST_FAIL_GETSOCKNAME) != 0) {
    return startup_fail(&state, err, "getsockname");
  }
#endif
  struct sockaddr_in bound;
  socklen_t blen = sizeof(bound);
  if (getsockname(state.listen_fd, (struct sockaddr *)&bound, &blen) != 0) {
    return startup_fail(&state, err, "getsockname");
  }
  state.actual_port = ntohs(bound.sin_port);
  record_bound_addr_after_getsockname(&bound);

#if defined(ODIN_CLI_CLIENT_TESTING)
  if (test_consume_failpoint(ODIN_CLI_CLIENT_TEST_FAIL_EVENT_LOOP_CREATE) !=
      0) {
    return startup_fail(&state, err, "event_loop_create");
  }
#endif
  if (odin_event_loop_create(&state.loop) != 0) {
    return startup_fail(&state, err, "event_loop_create");
  }

#if defined(ODIN_CLI_CLIENT_TESTING)
  if (test_consume_failpoint(ODIN_CLI_CLIENT_TEST_FAIL_ACCEPT_LOOP_CREATE) !=
      0) {
    return startup_fail(&state, err, "accept_loop_create");
  }
#endif
  if (odin_accept_loop_create(
          state.loop, state.listen_fd, cli_client_quic_on_accept,
          cli_client_on_accept_loop_error, &state, &state.accept_loop) != 0) {
    return startup_fail(&state, err, "accept_loop_create");
  }
#if defined(ODIN_CLI_CLIENT_TESTING) && !defined(__linux__)
  cli_client_test_prearm_external_fcntl_trigger(&state);
#endif
#if defined(ODIN_CLI_CLIENT_TESTING)
  g_live_accept_loops += 1;
#endif

  struct sockaddr_in peer_addr;
  memset(&peer_addr, 0, sizeof(peer_addr));
  peer_addr.sin_family = AF_INET;
  peer_addr.sin_addr = server_addr;
  peer_addr.sin_port = htons(config->server_port);

  struct sockaddr_in local_udp;
  memset(&local_udp, 0, sizeof(local_udp));
  local_udp.sin_family = AF_INET;
  local_udp.sin_addr.s_addr = htonl(INADDR_ANY);
  local_udp.sin_port = htons(0);

  odin_xqc_client_runtime_default_config_t runtime_config;
  memset(&runtime_config, 0, sizeof(runtime_config));
  runtime_config.loop = state.loop;
  runtime_config.local_addr = (const struct sockaddr *)&local_udp;
  runtime_config.local_addrlen = (socklen_t)sizeof(local_udp);
  runtime_config.peer_addr = (const struct sockaddr *)&peer_addr;
  runtime_config.peer_addrlen = (socklen_t)sizeof(peer_addr);
  runtime_config.server_host = server_host_cstr;
  if (quic_runtime_create_default_call(&runtime_config, &state.quic_rt) != 0) {
    return startup_fail(&state, err, "xqc_client_runtime_create");
  }
  if (quic_runtime_start_call(state.quic_rt) != 0) {
    return startup_fail(&state, err, "xqc_client_runtime_start");
  }

  const char *sig_fail = install_signal_handlers(&state);
  if (sig_fail != NULL) {
    return startup_fail(&state, err, sig_fail);
  }

#if defined(ODIN_CLI_CLIENT_TESTING)
  if (test_consume_failpoint(ODIN_CLI_CLIENT_TEST_FAIL_SIGNAL_TIMER_START) !=
      0) {
    return startup_fail(&state, err, "signal_timer_start");
  }
#endif
  if (odin_event_timer_start(
          state.loop, ODIN_CLI_CLIENT_SIGNAL_POLL_INTERVAL_US,
          ODIN_CLI_CLIENT_SIGNAL_POLL_INTERVAL_US, cli_client_signal_poll_timer,
          &state, &state.signal_timer) != 0) {
    return startup_fail(&state, err, "signal_timer_start");
  }

  // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
  (void)fprintf(err,
                "odin: mode=client transport=quic listen=%u server=%.*s:%u\n",
                (unsigned)state.actual_port, (int)config->server_host_len,
                config->server_host, (unsigned)config->server_port);
  (void)fflush(err);

  const int run_rc = odin_event_loop_run(state.loop);
  if (state.accept_loop_error_seen) {
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    (void)fprintf(err, "odin: client runtime failed at accept_loop\n");
    (void)fflush(err);
    cleanup_all(&state);
    return 1;
  }
  if (run_rc != 0 || !state.shutdown_requested) {
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    (void)fprintf(err, "odin: client runtime failed at event_loop_run\n");
    (void)fflush(err);
    cleanup_all(&state);
    return 1;
  }
  cleanup_all(&state);
  return 0;
}

int odin_cli_run_client(const odin_cli_client_config_t *config, FILE *err) {
  cli_client_state_t state;
  memset(&state, 0, sizeof(state));
  state.listen_fd = -1;
  state.test_wakeup_fd = -1;

  if (config == NULL || err == NULL ||
      (config->transport != ODIN_CLI_CLIENT_TRANSPORT_TCP &&
       config->transport != ODIN_CLI_CLIENT_TRANSPORT_QUIC)) {
    return startup_fail(&state, err, "config");
  }
  if (config->transport == ODIN_CLI_CLIENT_TRANSPORT_TCP) {
    return run_tcp_client(config, err);
  }
  return run_quic_client(config, err);
}
