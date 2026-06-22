# RFC-024: CLI Client Runtime Wiring

## Revision Notes (delete before merge)

### Cycle 1 — 2026-06-18

Addressed Finding 1 Major at §5 T2 by requiring two sequential successful CONNECTs through one client process after the first session closes.
Addressed Findings 2–4 Major at §3.2.4 / §5 T3,T6 by adding an integrated `odin_client_session_create` failure hook, real accept-loop/event-loop runtime triggers, and in-flight cleanup assertions.
Addressed Finding 5 Major at §4 by adding the local CONNECT target trust boundary and server-side policy delegation; accepted the Minor by adding the missing §5 post-syscall bind-failure matrix cell.

### Cycle 2 — 2026-06-18

Addressed the reopened Major at §5 T6 and §6 P2 by mirroring T5's post-return snapshot/release fixture for the then-current runtime-failure subcases.
Added explicit T6 signal-handler restoration, child-paused listener-refusal, EOF/reset, and liveness observation mechanics so cleanup assertions are executable before child exit.

### Cycle 3 — 2026-06-18

Addressed the Major at §5.0 / T5 / T6 by requiring two simultaneous in-flight sessions for graceful and runtime-failure cleanup, with `last_cleanup_sessions >= 2`.
Addressed the Major at §5.0 / T5 by making T5 cover both SIGINT and SIGTERM runtime shutdown behavior.
Addressed the Major at §5 T3 / §3.2.4 by adding an owner-thread idle snapshot pipe after successful `on_close`, before any signal-driven cleanup.

### Cycle 4 — 2026-06-18

Addressed the reopened Major at §5 T3 / §3.2.4 by driving successful B relay completion before reading the owner-thread idle snapshot.
Rewrote T3 setup and expected results to half-close the B downstream and fake upstream sides, deadline-wait for EOF/reset, and assert the idle snapshot before `SIGTERM`.

### Cycle 5 — 2026-06-18

Addressed the Major at §3.2.2 / §3.2.4 / §5 T6 / §6 by adding macOS-only accept OK then `fcntl(F_GETFL)` / `fcntl(F_SETFL)` runtime-error subcases through `odin_accept_loop_test_fail_next_fcntl`.
Addressed the Major at §3.2.1 / §3.2.4 / §5 T6 / §6 by adding `ODIN_CLI_CLIENT_TEST_TRIGGER_UNEXPECTED_STOP` coverage for `run_rc == 0 && !state.shutdown_requested`.

### Cycle 6 — 2026-06-18

Addressed Finding 1 Major at §5 T2 / §6 by moving the successful fake Odin server to distinct loopback literal `127.0.0.2` and adding a `127.0.0.1` sentry assertion.
Addressed Finding 2 Major at §5 T7 / §6 by adding a `SIGNAL_TIMER_START` startup-failure subcase that proves both preinstalled handlers are restored.
Addressed Finding 3 Major at §3.2.4 / §6 by adding C++ linkage guards and pinning event-loop / accept-loop internal-test includes plus the non-Linux `fcntl` hook guard.
Addressed Finding 4 Major at §3.2.1 / §6 by requiring every client startup/runtime failure print helper to `fflush(err)` before returning through the direct client branch.

### Cycle 7 — 2026-06-18

Addressed the Major at §5 T7 / §5.0 / §6 by adding a production-path bind-collision subcase that asserts the real `bind(2)` failure line and preserves the parent listener.
Addressed the Major at §4 S3 / §5 T2 / §6 by adding a peer CONNECT-target sentry so local target dials are detected separately from configured-server dials.
Addressed the Major at §3.2.1 / §6 P1 by preserving the existing Server arm `rc = odin_cli_run_server(...)` plus final-flush control flow.
Accepted the Minor at §5 narrative / §6 P2 by making T4 consistently a forked test-binary child rather than a spawned `odin-client` artifact.

### Cycle 8 — 2026-06-18

Accepted the Minor at §3.2.2 by refreshing stale source citations for `fire_terminal`, `odin_client_session_destroy` / `finish_destroy`, and `accept_one`.
Touched only §3.2.2 citation text; no behavioral, test-matrix, or implementation-plan changes.

### Cycle 9 — 2026-06-18

Addressed Finding 1 Major at §3.2.4 / §5 T6 / §6 by adding a parent-controlled runtime trigger pipe so failpoints fire only after both CONNECT_REQ frames are decoded.
Addressed Finding 2 Major at §4 S3 / §5 T2 / §6 by scoping S3 to client-runner no-local-dial behavior and citing the existing server-side dial-filter regression tests.
Accepted the Minor at §3.2.4 / §5 T3,T6,T7 by splitting no-errno runtime triggers into `odin_cli_client_test_trigger_next`, naming concrete errno arguments for errno-bearing failpoints, and adding invalid-helper assertions.

### Cycle 10 — 2026-06-18

Addressed Finding 1 Major at §5 narrative and T1,T2,T3,T5,T6,T7 by requiring a bounded child-wait helper, deadlined release-pipe writes, and explicit release-plus-wait sequencing.
Addressed Finding 2 Major at §5 T7 / §5.0 by expanding invalid-hook assertions to cover progress fd, idle-snapshot fd, and `odin_client_session_test_fail_next_create(0)` EINVAL branches.

## 1. Summary

Wire successful `odin-client` invocations to a long-running loopback HTTPS_PROXY runtime that accepts local TCP clients, creates RFC-023 client sessions for each accepted connection, and shuts down cleanly on `SIGINT` / `SIGTERM`.

## 2. Goals

- **G1.** For every successful Client-mode OK invocation whose parsed `--server` host is an IPv4 literal, the process binds a nonblocking IPv4 TCP listener on `127.0.0.1:<listen_port>`, reports the actual bound port in the existing client banner format after the runtime is ready, and remains alive to serve local HTTPS_PROXY connections until a signal or runtime failure ends the run.

- **G2.** For every accepted local connection while the client runtime is running, the process either hands that nonblocking connection to one RFC-023 client session configured with the parsed `(server_host, server_host_len, server_port)` endpoint and later destroys that session from its `on_close` callback, or silently closes the newly accepted fd when local session setup fails, without stopping the accept loop or rejecting later connections.

- **G3.** For every client startup failure or client runtime failure, the process emits one deterministic stderr line, releases every CLI-owned listener, event-loop, accept-loop, signal-timer, and in-flight-session resource it created, restores replaced signal handlers, and returns exit code `1`.

- **G4.** A `SIGINT` or `SIGTERM` delivered after successful client startup causes the owner-thread event loop to stop, every in-flight client session to be destroyed, the accept loop and event loop to be destroyed, the listening fd to be closed, replaced signal handlers to be restored, and the process to return exit code `0`.

## 3. Design

### 3.1 Overview

`odin_cli_main` remains the single public CLI entry point. The only changed `odin_cli_main` control-flow row is `ODIN_CLI_OK` plus `ODIN_CLI_MODE_CLIENT`: instead of printing the client banner inline and returning, it delegates to an internal client runner. The runner validates the parsed server endpoint as an IPv4 literal, creates a loopback listener, event loop, accept loop, signal-stop timer, and per-connection client-session wiring, then prints the byte-compatible IPv4 client banner and enters `odin_event_loop_run`.

The parser still accepts the same `--server` grammar as RFC-007, including reg-name and bracketed IPv6 forms. This RFC deliberately does not add DNS resolution or IPv6 dialing for the configured Odin server endpoint. Values that parse successfully but are not IPv4 literals fail in the client runner before the success banner with `odin: client startup failed at server_endpoint\n`.

```
odin-client argv
    |
    v
odin_cli_parse -> OK/CLIENT
    |
    v
CLI client runner
    |-- validate server_host as IPv4 literal for RFC-023 client_session
    |-- socket + SO_REUSEADDR + O_NONBLOCK + bind/listen/getsockname
    |-- odin_event_loop_create
    |-- odin_accept_loop_create(listener fd)
    |-- install SIGINT/SIGTERM handlers and owner-thread polling timer
    |-- fprintf(stderr, "odin: mode=client listen=<actual_port> server=<host>:<port>\n")
    v
odin_event_loop_run
    |
    | accept_loop on_accept(conn_fd)
    |    -> odin_client_session_create(loop, conn_fd, server_host, server_host_len, server_port, ...)
    |    -> track session until on_close destroys it
    |
    v
cleanup: sessions -> accept loop -> signal timer -> event loop -> listener fd -> signal handlers
```

### 3.2 Detailed Design

#### 3.2.1 Client CLI Startup, Endpoint Validation, Ownership, and Errors

Contract surface - `odin/cli.h` keeps the existing public entry point:

```c
#include <stdio.h>

int odin_cli_main(int argc, char *const *argv, FILE *out, FILE *err);
```

The client branch is factored into an internal helper:

```c
/* odin/cli_client.h, internal to the odin target. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

int odin_cli_run_client(uint16_t listen_port, const char *server_host,
                        size_t server_host_len, uint16_t server_port,
                        FILE *err);
```

The resulting `odin_cli_main` status map is:

```text
odin_cli_parse result                         observable behavior
-------------------------------------------  ---------------------------------------------
ODIN_CLI_OK, mode == ODIN_CLI_MODE_CLIENT    odin_cli_run_client(args.listen_port,
                                                                  args.server_host,
                                                                  args.server_host_len,
                                                                  args.server_port,
                                                                  err)
ODIN_CLI_OK, mode == ODIN_CLI_MODE_SERVER    existing odin_cli_run_server(args.listen_port, err)
ODIN_CLI_HELP                                existing usage to out; return 0
ODIN_CLI_ERR_*                               existing deterministic error + usage to err; return 2
```

**Unstated contract.** `odin_cli_run_client` is an owner-thread, blocking call. It writes nothing to `out`. It returns `0` only after a graceful signal-driven stop; it returns `1` for startup failure, accept-loop runtime failure, or `odin_event_loop_run` failure. It does not call or include `odin_client_listen_open` or `odin_client_listen_handshake`; the old RFC-009 blocking listener / single-connection handshake remains available to its direct unit tests, but it is not on the runtime traffic path.

The helper validates the configured Odin server endpoint before opening the listener. `server_host[0..server_host_len)` must be a non-empty IPv4 presentation literal accepted by `inet_pton(AF_INET, server_host_cstr, &server_addr) == 1`; otherwise startup fails with:

```text
odin: client startup failed at server_endpoint
```

No DNS lookup, `/etc/hosts` lookup, c-ares lookup, bracketed IPv6 handling, or IPv6 `sockaddr_in6` construction is added in this RFC. The validation mirrors the current RFC-023 constructor behavior: `odin_client_session_create` copies the server host slice into a bounded C string and calls `inet_pton(AF_INET, ...)` before building its stored `sockaddr_in` (`odin/client_session.c:124-144`). Validating once in the runner prevents a user-visible success banner for a configuration that every later accepted connection would reject synchronously.

`listen_port == 0` means kernel-selected ephemeral port. The listener is an `AF_INET` / `SOCK_STREAM` socket bound to `127.0.0.1:<listen_port>` and made nonblocking before it is passed to `odin_accept_loop_create`. The fixed setup order is `socket(AF_INET, SOCK_STREAM, 0)`, `setsockopt(SOL_SOCKET, SO_REUSEADDR, 1)`, `fcntl(F_GETFL)`, `fcntl(F_SETFL, flags | O_NONBLOCK)`, `bind(127.0.0.1:<port>)`, `listen(SOMAXCONN)`, `getsockname`. `getsockname` supplies the actual port printed in the banner, so a successful ephemeral bind never prints `listen=0`.

The success line is printed to `err` only after all of these are true: server endpoint validation succeeded, the listener is bound / listening / nonblocking, the event loop exists, the accept loop is armed, both signal handlers are installed, and the signal polling timer is armed. For IPv4 server hosts, the line is byte-compatible with the current client success banner produced by `odin/cli.c:260-269`:

```text
odin: mode=client listen=<actual_port> server=<server_host>:<server_port>
```

All setup failures route through one cleanup function and one deterministic line. Each startup and runtime failure print helper immediately calls `fflush(err)` after writing its deterministic line and before returning, matching `odin_cli_run_server`; this is required because the Client arm returns directly from `odin_cli_run_client` instead of falling through `odin_cli_main`'s existing final `fflush(out)` / `fflush(err)` block. The Server arm keeps its current shape: set `rc = odin_cli_run_server(args.listen_port, err)`, then fall through the common final flushes. The cleanup function preserves the first failed step name and does not print `strerror`, because libc wording and localization are not stable test or UX contracts. Exact client startup failure steps are:

```text
odin: client startup failed at server_endpoint
odin: client startup failed at socket
odin: client startup failed at setsockopt(SO_REUSEADDR)
odin: client startup failed at fcntl(F_GETFL)
odin: client startup failed at fcntl(F_SETFL)
odin: client startup failed at bind
odin: client startup failed at listen
odin: client startup failed at getsockname
odin: client startup failed at event_loop_create
odin: client startup failed at accept_loop_create
odin: client startup failed at sigaction(SIGINT)
odin: client startup failed at sigaction(SIGTERM)
odin: client startup failed at signal_timer_start
```

Runtime failure lines are:

```text
odin: client runtime failed at accept_loop
odin: client runtime failed at event_loop_run
```

The `event_loop_run` line covers both event-loop backend failure (`run_rc != 0`) and the defensive unexpected-clean-stop state (`run_rc == 0` while neither `state.shutdown_requested` nor `state.accept_loop_error_seen` is set). No production path intentionally reaches the latter; keeping it as a runtime failure prevents an owner-thread callback that stops the loop outside the signal or accept-error paths from returning a false-success `0`.

Among client-runtime objects, the CLI owns exactly the objects it creates: the listening fd, the `odin_event_loop_t`, the `odin_accept_loop_t`, the signal timer handle, and the linked list of in-flight client-session entries. `odin_accept_loop_destroy` does not close the listening fd (`odin/accept_loop.h` documents caller ownership of `listen_fd`), and `odin_client_session_destroy` owns and closes each accepted `conn_fd` only after `odin_client_session_create` succeeds. On the `odin_client_session_create` `-1` path, the CLI still owns the newly accepted `conn_fd` and closes it immediately.

The cleanup order after a stopped run is: destroy all in-flight client sessions, destroy the accept loop, stop the signal timer if it is still live, destroy the event loop, close the listening fd, and restore signal handlers. Startup-failure cleanup uses the same guarded order for whatever subset has been created. The signal timer is stopped before `odin_event_loop_destroy` because it is a loop-owned timer handle; no test or production path calls `odin_event_loop_stop` from a non-owner thread.

The socket and signal setup sequence is POSIX and compiles on macOS and Linux. The event loop and accept loop consumed here have platform-specific backends (`kqueue` / `accept + fcntl` on macOS, `epoll + timerfd` / `accept4` on Linux); this RFC runtime-verifies the macOS host binary and cross-compiles the Linux binary without executing it in this environment.

**Mechanism.**

```text
odin_cli_main(argc, argv, out, err):
  status, args = odin_cli_parse(argc, argv)
  rc = 2
  switch status:
    OK:
      if args.mode == CLIENT:
        fflush(out)
        return odin_cli_run_client(args.listen_port,
                                   args.server_host, args.server_host_len,
                                   args.server_port, err)
      fflush(out)
      rc = odin_cli_run_server(args.listen_port, err)
      break
    HELP and ERR statuses:
      handle with the existing mapping and set rc
  fflush(out)
  fflush(err)
  return rc

odin_cli_run_client(listen_port, server_host, server_host_len, server_port, err):
  state = zeroed client state
  state.listen_fd = -1

  if validate_ipv4_server_endpoint(server_host, server_host_len) != 0:
    return startup_fail("server_endpoint")

  state.listen_fd = socket(AF_INET, SOCK_STREAM, 0)
  if fail: return startup_fail("socket")
  if setsockopt(SO_REUSEADDR) fails: return startup_fail("setsockopt(SO_REUSEADDR)")
  flags = fcntl(F_GETFL)
  if fail: return startup_fail("fcntl(F_GETFL)")
  if fcntl(F_SETFL, flags | O_NONBLOCK) fails: return startup_fail("fcntl(F_SETFL)")
  if bind(127.0.0.1, listen_port) fails: return startup_fail("bind")
  if listen(SOMAXCONN) fails: return startup_fail("listen")
  actual_port = getsockname_port(state.listen_fd)
  if fail: return startup_fail("getsockname")

  if odin_event_loop_create(&state.loop) != 0:
    return startup_fail("event_loop_create")
  if odin_accept_loop_create(state.loop, state.listen_fd,
                             cli_client_on_accept,
                             cli_client_on_accept_loop_error,
                             &state, &state.accept_loop) != 0:
    return startup_fail("accept_loop_create")
  if install_signal_handlers(&state) fails:
    return startup_fail(failing_sigaction_name)
  if odin_event_timer_start(state.loop, 50000, 50000,
                            cli_client_signal_poll_timer, &state,
                            &state.signal_timer) != 0:
    return startup_fail("signal_timer_start")

  fprintf(err, "odin: mode=client listen=%u server=%.*s:%u\n",
          actual_port, (int)server_host_len, server_host, server_port)
  fflush(err)

  run_rc = odin_event_loop_run(state.loop)
  if state.accept_loop_error_seen:
    print "odin: client runtime failed at accept_loop\n"
    fflush(err)
    cleanup_all
    return 1
  if run_rc != 0 or !state.shutdown_requested:
    print "odin: client runtime failed at event_loop_run\n"
    fflush(err)
    cleanup_all
    return 1
  cleanup_all
  return 0
```

Satisfies: G1 via the loopback nonblocking listener, actual-port banner, IPv4 endpoint validation, and event-loop run; G3 via the fixed failure-line table and cleanup order.

#### 3.2.2 Accepted-Connection Session Lifecycle

Contract surface - consumed public APIs verified in current code:

```c
/* odin/accept_loop.h */
typedef void (*odin_accept_loop_accept_cb)(odin_accept_loop_t *al, int conn_fd,
                                           void *user_data);
typedef void (*odin_accept_loop_error_cb)(odin_accept_loop_t *al, int err,
                                          void *user_data);

int odin_accept_loop_create(odin_event_loop_t *loop, int listen_fd,
                            odin_accept_loop_accept_cb on_accept,
                            odin_accept_loop_error_cb on_error, void *user_data,
                            odin_accept_loop_t **out);

/* odin/client_session.h */
typedef void (*odin_client_session_close_cb)(odin_client_session_t *cs, int err,
                                             void *user_data);

int odin_client_session_create(odin_event_loop_t *loop, int conn_fd,
                               const char *server_host, size_t server_host_len,
                               uint16_t server_port,
                               odin_client_session_close_cb on_close,
                               void *user_data,
                               odin_client_session_t **out);

void odin_client_session_destroy(odin_client_session_t *cs);
```

The client runner tracks sessions with one internal entry per successful client-session construction:

```c
typedef struct cli_client_session_entry_t {
  struct cli_client_session_entry_t *next;
  odin_client_session_t *session;
  struct cli_client_state_t *owner;
} cli_client_session_entry_t;
```

**Unstated contract.** `odin_accept_loop` owns neither the listening fd nor accepted connection fds after `on_accept` begins; `odin/accept_loop.h` states that caller ownership of each `conn_fd` begins at callback entry and that the accepted fd is nonblocking. The client runner therefore either transfers the fd to `odin_client_session_create` on success or closes it itself on every pre-session failure.

`cli_client_on_accept` is an owner-thread callback. It allocates one tracking entry, calls `odin_client_session_create(state->loop, conn_fd, state->server_host, state->server_host_len, state->server_port, cli_client_session_on_close, entry, &entry->session)`, pushes the entry onto `state->sessions` only after create succeeds, and increments `state->inflight_sessions`. If entry allocation fails or `odin_client_session_create` returns `-1`, it closes `conn_fd`, frees any entry allocation, emits no stderr, and returns without stopping or destroying the accept loop. The next readiness from `odin_accept_loop` is handled normally.

`cli_client_session_on_close` is the final callback from a successfully created RFC-023 session. It removes the entry from the in-flight list, decrements `state->inflight_sessions`, calls `odin_client_session_destroy(cs)` from inside `on_close` (legal under RFC-023 and implemented by `fire_terminal` in `odin/client_session.c:622-658` as final-action callback discipline), frees the entry, and does not stop the event loop. This is the only normal per-session completion action; the process keeps accepting future local clients.

`cli_client_on_accept_loop_error` is the bridge from RFC-019 terminal accept-loop errors to CLI runtime failure. It records `state->accept_loop_error_seen = 1`, stores the errno for debugging-only local state, and calls `odin_event_loop_stop(state->loop)` on the owner thread. After `odin_event_loop_run` returns, §3.2.1 prints `odin: client runtime failed at accept_loop\n`, runs cleanup, and returns `1`.

During graceful cleanup, `destroy_all_sessions` repeatedly pops the current list head and calls `odin_client_session_destroy(entry->session)`. No `on_close` callback is invoked by `destroy`; `odin_client_session_destroy` / `finish_destroy` in `odin/client_session.c:191-231` make outside-callback destroy an abort path that tears down owned sub-objects and closes owned fds without firing `on_close`. After the list is empty, `odin_accept_loop_destroy` stops the listener watch. The order prevents a newly accepted connection from creating another session while cleanup is tearing the list down.

The accepted-connection nonblocking guarantee is platform-conditional inside the consumed accept loop: Linux uses `accept4(SOCK_NONBLOCK)`, and macOS uses `accept(2)` followed by `fcntl(F_SETFL, O_NONBLOCK)` in `accept_one` (`odin/accept_loop.c:68-126`). This RFC's host tests exercise the macOS branch, including terminal failures after `accept(2)` but before `on_accept`: synthetic `F_GETFL` and `F_SETFL` failures close the just-accepted fd, route to `cli_client_on_accept_loop_error`, create no client session for that fd, and stop the run with the `accept_loop` failure line. The Linux branch is compiled in the Linux binary but not run in this environment.

**Mechanism.**

```text
cli_client_on_accept(al, conn_fd, user_data):
  state = user_data
  entry = calloc(1, sizeof *entry)
  if entry == NULL:
    close(conn_fd)
    return
  entry.owner = state
  if odin_client_session_create(state.loop, conn_fd,
                                state.server_host, state.server_host_len,
                                state.server_port,
                                cli_client_session_on_close,
                                entry, &entry.session) != 0:
    free(entry)
    close(conn_fd)
    return
  entry.next = state.sessions
  state.sessions = entry
  state.inflight_sessions += 1

cli_client_session_on_close(cs, err, user_data):
  entry = user_data
  state = entry.owner
  remove entry from state.sessions
  state.inflight_sessions -= 1
  odin_client_session_destroy(cs)
  free(entry)

cli_client_on_accept_loop_error(al, err, user_data):
  state = user_data
  state.accept_loop_error_seen = 1
  state.accept_loop_errno = err
  odin_event_loop_stop(state.loop)

destroy_all_sessions(state):
  while state.sessions != NULL:
    entry = state.sessions
    state.sessions = entry.next
    state.inflight_sessions -= 1
    odin_client_session_destroy(entry.session)
    free(entry)
```

Satisfies: G2 via the accepted-fd success and local-failure arms, the session list, and destroy-from-`on_close`; G3 via the accept-loop error callback that stops the run and the session cleanup order; G4 via `destroy_all_sessions` before accept-loop and event-loop teardown.

#### 3.2.3 Graceful Signal Stop

Contract surface:

```c
#include <signal.h>

static volatile sig_atomic_t g_odin_cli_client_signal_seen;

static void cli_client_signal_handler(int signum);
static void cli_client_signal_poll_timer(odin_event_loop_t *loop,
                                         odin_event_timer_t *timer,
                                         void *user_data);
```

**Unstated contract.** The POSIX signal handler is async-signal-safe: it only writes the received signal number to `g_odin_cli_client_signal_seen`. It never calls `odin_event_loop_stop`, `fprintf`, `malloc`, `close`, or any other non-async-signal-safe function. A recurring owner-thread timer polls the flag every 50 ms. When the flag is nonzero, the timer callback sets `state->shutdown_requested = 1`, clears and stops the timer handle, and calls `odin_event_loop_stop(loop)` from the loop owner thread.

The helper saves the previous `SIGINT` and `SIGTERM` handlers before replacing them. If replacing `SIGTERM` fails after `SIGINT` has been replaced, cleanup restores `SIGINT` before returning `1`. On every post-install exit path - startup failure after one handler is installed, runtime failure, and graceful shutdown - cleanup restores each handler this helper replaced. As in RFC-022 server wiring, the implementation uses `sigaction` with no `SA_RESTART`; interrupted backend waits are handled by the event-loop backend, while the polling timer supplies the owner-thread stop.

On macOS the signal timer is driven by the `kqueue` timeout path; on Linux it is driven by the event loop's timerfd path. Only the macOS host binary is runtime-verified in this environment; the Linux timerfd branch is cross-compiled but not executed here.

**Mechanism.**

```text
signal_handler(signum):
  g_odin_cli_client_signal_seen = signum

install_signal_handlers(state):
  g_odin_cli_client_signal_seen = 0
  save old SIGINT action
  install signal_handler for SIGINT
  mark SIGINT replaced
  save old SIGTERM action
  install signal_handler for SIGTERM
  mark SIGTERM replaced

signal_timer(loop, timer, user_data):
  state = user_data
  if g_odin_cli_client_signal_seen == 0:
    return
  state.shutdown_requested = 1
  state.signal_timer = NULL
  odin_event_timer_stop(timer)
  odin_event_loop_stop(loop)

restore_signal_handlers(state):
  if SIGTERM was replaced: sigaction(SIGTERM, old_term)
  if SIGINT was replaced: sigaction(SIGINT, old_int)
```

Satisfies: G4 via the async-signal-safe handler, owner-thread stop request, and restoration of replaced handlers on every cleanup path.

#### 3.2.4 Test-Only Client CLI Hooks

```c
/* odin/testing/cli_client_internal_test.h, visible only when ODIN_CLI_CLIENT_TESTING. */
#if defined(ODIN_CLI_CLIENT_TESTING)
#include <netinet/in.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum odin_cli_client_test_failpoint_t {
  ODIN_CLI_CLIENT_TEST_FAIL_SOCKET = 1,
  ODIN_CLI_CLIENT_TEST_FAIL_SETSOCKOPT_REUSEADDR,
  ODIN_CLI_CLIENT_TEST_FAIL_FCNTL_GETFL,
  ODIN_CLI_CLIENT_TEST_FAIL_FCNTL_SETFL,
  ODIN_CLI_CLIENT_TEST_FAIL_BIND,
  ODIN_CLI_CLIENT_TEST_FAIL_LISTEN,
  ODIN_CLI_CLIENT_TEST_FAIL_GETSOCKNAME,
  ODIN_CLI_CLIENT_TEST_FAIL_EVENT_LOOP_CREATE,
  ODIN_CLI_CLIENT_TEST_FAIL_ACCEPT_LOOP_CREATE,
  ODIN_CLI_CLIENT_TEST_FAIL_SIGACTION_SIGINT,
  ODIN_CLI_CLIENT_TEST_FAIL_SIGACTION_SIGTERM,
  ODIN_CLI_CLIENT_TEST_FAIL_SIGNAL_TIMER_START,
  ODIN_CLI_CLIENT_TEST_FAIL_EVENT_LOOP_RUN,
  ODIN_CLI_CLIENT_TEST_TRIGGER_UNEXPECTED_STOP,
  ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_ERROR,
  ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_GETFL_ERROR,
  ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_SETFL_ERROR,
  ODIN_CLI_CLIENT_TEST_FAIL_NEXT_SESSION_ENTRY_ALLOC,
  ODIN_CLI_CLIENT_TEST_FAIL_NEXT_CLIENT_SESSION_CREATE
} odin_cli_client_test_failpoint_t;

typedef struct odin_cli_client_test_liveness_t {
  size_t live_listeners;
  size_t live_accept_loops;
  size_t live_sessions;
  size_t last_cleanup_sessions;
} odin_cli_client_test_liveness_t;

int odin_cli_client_test_fail_next(odin_cli_client_test_failpoint_t fp,
                                   int errnum);
int odin_cli_client_test_trigger_next(odin_cli_client_test_failpoint_t fp);
void odin_cli_client_test_reset_liveness(void);
int odin_cli_client_test_liveness(odin_cli_client_test_liveness_t *out);
int odin_cli_client_test_last_bind_addr(struct sockaddr_in *out);
int odin_cli_client_test_set_progress_fd(int fd,
                                         size_t min_inflight_sessions);
int odin_cli_client_test_set_runtime_trigger_fd(int fd);
int odin_cli_client_test_set_idle_snapshot_fd(int fd,
                                              size_t min_closed_sessions);

#ifdef __cplusplus
}
#endif

#endif /* defined(ODIN_CLI_CLIENT_TESTING) */

/* odin/testing/client_session_internal_test.h, visible only when
 * ODIN_CLIENT_SESSION_TESTING. */
#if defined(ODIN_CLIENT_SESSION_TESTING)

#ifdef __cplusplus
extern "C" {
#endif

int odin_client_session_test_fail_next_create(int errnum);

#ifdef __cplusplus
}
#endif

#endif /* defined(ODIN_CLIENT_SESSION_TESTING) */
```

```c
/* odin/cli_client.c, inside #if defined(ODIN_CLI_CLIENT_TESTING). */
#include "odin/testing/accept_loop_internal_test.h"
#include "odin/testing/event_loop_internal_test.h"
```

**Unstated contract.** Production builds do not declare or define the `odin_cli_client_test_*` symbols or the `odin_client_session_test_fail_next_create` symbol. The test target compiles `odin/testing/cli_client_testing.c`, which includes `odin/cli_client.c` with `ODIN_CLI_CLIENT_TESTING` defined, matching the existing `cli_server_testing.c` pattern. The same `odin_unittests` target applies `odin_cli_client_testing_config`, the existing `odin_client_session_testing_config`, the existing `odin_accept_loop_testing_config`, and the existing `odin_event_loop_testing_config` target-wide so the CLI test wrapper, the client-session test wrapper, and C++ tests see the gated declarations they use.

The `ODIN_CLI_CLIENT_TESTING` block in `odin/cli_client.c` includes `odin/testing/event_loop_internal_test.h` for `odin_event_loop_test_fail_next_backend_wait` and `odin/testing/accept_loop_internal_test.h` for `odin_accept_loop_test_fail_next_accept`. Calls to `odin_accept_loop_test_fail_next_fcntl` are compiled only under `#if !defined(__linux__)`, matching `odin/testing/accept_loop_internal_test.h`; the Linux branch rejects the two fcntl trigger failpoints with `errno = EOPNOTSUPP` before any reference to the macOS-only symbol.

`odin_cli_client_test_fail_next` stores one process-local errno-bearing failpoint and errno for the next relevant `odin_cli_run_client` call. Invalid failpoint values, `ODIN_CLI_CLIENT_TEST_TRIGGER_UNEXPECTED_STOP`, or `errnum <= 0` return `-1` with `errno = EINVAL` and do not arm state. `ODIN_CLI_CLIENT_TEST_FAIL_NEXT_SESSION_ENTRY_ALLOC` is armed with `ENOMEM` in §5 T3; `ODIN_CLI_CLIENT_TEST_FAIL_NEXT_CLIENT_SESSION_CREATE` is armed with `EIO` in §5 T3. Both accepted-connection local-failure arms set `errno` to the stored value before taking the silent close branch, so the helper has no ignored errno slot. On Linux, the macOS-only `ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_GETFL_ERROR` and `ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_SETFL_ERROR` values return `-1` with `errno = EOPNOTSUPP` and do not arm state. Startup failpoints are consumed at the named step and route through the same cleanup and deterministic stderr path as a real failure before the success banner is printed. `ODIN_CLI_CLIENT_TEST_FAIL_EVENT_LOOP_RUN`, `ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_ERROR`, and the two accept-loop `fcntl` trigger values are errno-bearing post-banner failpoints: they are eligible only after the listener, event loop, accept loop, signal handlers, and signal timer are live and the success banner has been flushed.

`odin_cli_client_test_trigger_next` stores one process-local no-errno trigger. The only valid value in this RFC is `ODIN_CLI_CLIENT_TEST_TRIGGER_UNEXPECTED_STOP`; any other value returns `-1` with `errno = EINVAL` and does not arm state. This helper intentionally has no `errnum` argument because the trigger only calls `odin_event_loop_stop(state->loop)` and the expected result cannot observe an errno.

`ODIN_CLI_CLIENT_TEST_FAIL_EVENT_LOOP_RUN` is consumed by calling `odin_event_loop_test_fail_next_backend_wait(state->loop, err)` from the owner thread; `odin_event_loop_run` itself observes the backend-wait failure, returns `-1`, and the runner prints `odin: client runtime failed at event_loop_run\n`. `ODIN_CLI_CLIENT_TEST_TRIGGER_UNEXPECTED_STOP` is consumed by calling `odin_event_loop_stop(state->loop)` from the owner-thread timer without setting `state.shutdown_requested` or `state.accept_loop_error_seen`; `odin_event_loop_run` returns `0`, and the defensive §3.2.1 branch prints `odin: client runtime failed at event_loop_run\n`. `ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_ERROR` is consumed by calling `odin_accept_loop_test_fail_next_accept(state->accept_loop, err)` and then opening a temporary nonblocking loopback connection to `127.0.0.1:<actual_port>` from the owner thread. The listener readability dispatches through the real RFC-019 accept loop, which classifies the synthetic `accept` errno, calls the `on_error` callback and `user_data` installed by `odin_accept_loop_create`, and reaches `cli_client_on_accept_loop_error`; the CLI cleanup closes the temporary wake fd if it is still open.

On macOS, `ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_GETFL_ERROR` and `ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_SETFL_ERROR` are consumed by calling `odin_accept_loop_test_fail_next_fcntl(state->accept_loop, F_GETFL, err)` or `odin_accept_loop_test_fail_next_fcntl(state->accept_loop, F_SETFL, err)` and then opening the same temporary nonblocking loopback wake connection. Unlike `odin_accept_loop_test_fail_next_accept`, these hooks let the real `accept(2)` syscall succeed first; the accept-loop implementation closes the half-configured accepted fd, classifies the synthetic `fcntl` errno, fires its `on_error` callback, and never calls `cli_client_on_accept` for that wake fd.

`ODIN_CLI_CLIENT_TEST_FAIL_NEXT_SESSION_ENTRY_ALLOC` and `ODIN_CLI_CLIENT_TEST_FAIL_NEXT_CLIENT_SESSION_CREATE` are CLI-local accepted-connection failpoints. They are consumed inside `cli_client_on_accept` after `odin_accept_loop` has handed the CLI a nonblocking `conn_fd`. Both arms close that accepted fd silently, do not print stderr, do not stop the event loop, and leave the accept loop running; the next accepted connection can still create a session. `ODIN_CLI_CLIENT_TEST_FAIL_NEXT_CLIENT_SESSION_CREATE` remains only a mirror for the runner's close/free branch. The integrated constructor-failure path is `odin_client_session_test_fail_next_create(errnum)`: it is implemented in `odin/client_session.c` under `ODIN_CLIENT_SESSION_TESTING`, consumed at the top of the real `odin_client_session_create` function, returns `-1` with `errno = errnum`, leaves `conn_fd` caller-owned, and lets `cli_client_on_accept` exercise the production return-value branch.

The liveness hook is deterministic test evidence for CLI-owned resources. `live_listeners` increments after `socket` succeeds and decrements after the CLI closes the listening fd. `live_accept_loops` increments after `odin_accept_loop_create` succeeds and decrements after `odin_accept_loop_destroy` returns. `live_sessions` increments after a session entry is pushed and decrements when that entry is removed by `on_close` or shutdown cleanup. `last_cleanup_sessions` records the number of in-flight sessions observed immediately before graceful or failure cleanup destroys the session list. Event-loop liveness remains covered by the existing `odin_event_loop_test_liveness` counters.

`odin_cli_client_test_last_bind_addr` copies the exact `sockaddr_in` passed to the latest bind attempt, so tests can prove `127.0.0.1` rather than `0.0.0.0`. `odin_cli_client_test_set_progress_fd(fd, min_inflight_sessions)` arms a pipe fd and threshold; the owner-thread signal timer writes one byte after `state.inflight_sessions >= min_inflight_sessions`, letting a parent process wait until one or more client sessions are in flight before sending a signal or before releasing runtime-failure failpoints. `fd < 0` or `min_inflight_sessions == 0` returns `-1` with `errno = EINVAL` and does not arm state.

`odin_cli_client_test_set_runtime_trigger_fd(fd)` arms a separate nonblocking read fd used only by runtime-failure subcases. When this fd is configured, the owner-thread timer must not consume any post-banner failpoint until it has written the progress byte if a progress fd is configured, and then successfully read one trigger byte from this fd. Tests write that trigger byte only after both fake Odin upstream sockets have accepted and decoded their CONNECT_REQ frames. `fd < 0` returns `-1` with `errno = EINVAL` and does not arm state. The test fixture sets the fd nonblocking before calling the helper; if the timer read would block or reaches EOF before a byte is read, the trigger remains unreleased and the fixture deadline fails rather than consuming the failpoint early.

`odin_cli_client_test_set_idle_snapshot_fd(fd, min_closed_sessions)` arms a separate pipe fd for rows that need proof while `odin_cli_main` is still blocked in the running loop. `cli_client_session_on_close` increments a test-only `closed_sessions` counter after it removes and destroys a session. On each owner-thread signal-timer tick, if the idle snapshot fd is armed, no snapshot has been written, `state.closed_sessions >= min_closed_sessions`, and `state.inflight_sessions == 0`, the timer writes one `odin_cli_client_test_liveness_t` snapshot to the fd and keeps the runtime running. The hook only observes completed sessions; tests using it must drive RFC-023 relay completion through downstream/upstream close or half-close behavior before waiting for the snapshot. `fd < 0` or `min_closed_sessions == 0` returns `-1` with `errno = EINVAL` and does not arm state.

**Mechanism.**

```text
test_fail_next(fp, errnum):
  if fp is unknown or fp is ODIN_CLI_CLIENT_TEST_TRIGGER_UNEXPECTED_STOP or
     errnum <= 0:
    errno = EINVAL
    return -1
  if defined(__linux__) and fp is one of the ACCEPT_LOOP_FCNTL trigger values:
    errno = EOPNOTSUPP
    return -1
  global_failpoint = fp
  global_errno = errnum
  return 0

test_trigger_next(fp):
  if fp != ODIN_CLI_CLIENT_TEST_TRIGGER_UNEXPECTED_STOP:
    errno = EINVAL
    return -1
  global_failpoint = fp
  global_errno = 0
  return 0

maybe_fail(fp):
  if global_failpoint != fp: return 0
  err = global_errno
  global_failpoint = 0
  global_errno = 0
  errno = err
  return -1

signal_timer_test_branch(loop, state):
  if progress_fd >= 0 and !progress_reported and
     state.inflight_sessions >= progress_min_inflight_sessions:
    write one byte to progress_fd
    progress_reported = 1
  if idle_snapshot_fd >= 0 and !idle_snapshot_reported and
     state.closed_sessions >= idle_snapshot_min_closed_sessions and
     state.inflight_sessions == 0:
    snapshot = copy CLI client liveness counters
    write snapshot to idle_snapshot_fd
    idle_snapshot_reported = 1
  if runtime failpoint is armed and progress_fd >= 0 and !progress_reported:
    return
  if runtime failpoint is armed and runtime_trigger_fd >= 0 and
     !runtime_trigger_released:
    if read(runtime_trigger_fd, one byte) == 1:
      runtime_trigger_released = 1
    else:
      return
  if global_failpoint == ODIN_CLI_CLIENT_TEST_FAIL_EVENT_LOOP_RUN:
    consume err
    odin_event_loop_test_fail_next_backend_wait(state.loop, err)
  if global_failpoint == ODIN_CLI_CLIENT_TEST_TRIGGER_UNEXPECTED_STOP:
    consume trigger
    odin_event_loop_stop(state.loop)
  if global_failpoint == ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_ERROR:
    consume err
    odin_accept_loop_test_fail_next_accept(state.accept_loop, err)
    state.test_wakeup_fd = nonblocking_connect(127.0.0.1, state.actual_port)
  if !defined(__linux__) and
     global_failpoint == ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_GETFL_ERROR:
    consume err
    odin_accept_loop_test_fail_next_fcntl(state.accept_loop, F_GETFL, err)
    state.test_wakeup_fd = nonblocking_connect(127.0.0.1, state.actual_port)
  if !defined(__linux__) and
     global_failpoint == ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_SETFL_ERROR:
    consume err
    odin_accept_loop_test_fail_next_fcntl(state.accept_loop, F_SETFL, err)
    state.test_wakeup_fd = nonblocking_connect(127.0.0.1, state.actual_port)

client_session_test_fail_next_create(errnum):
  if errnum <= 0: errno = EINVAL; return -1
  client_session_global_create_errno = errnum
  return 0

odin_client_session_create(...):
  if client_session_global_create_errno != 0:
    err = client_session_global_create_errno
    client_session_global_create_errno = 0
    errno = err
    return -1
  continue with production constructor
```

Satisfies: G1 via bind-address capture and post-banner readiness tests; G2 via accepted-connection session-create failure failpoints and session liveness counters; G3 via deterministic setup/runtime failpoints and liveness counters; G4 via in-flight-session progress and post-cleanup liveness snapshots.

## 4. Security

- **S1.**
  - **Threat:** Exposing the local HTTPS_PROXY listener on a non-loopback interface would let other hosts reach the unauthenticated client proxy and relay requests through the configured Odin server.
  - **Mitigation:** §3.2.1 binds the client listener to `127.0.0.1:<listen_port>` only, with no flag, environment variable, or compile-time option in this RFC that widens it to `INADDR_ANY`.
  - **Enforcement:** §5 rows T1 and T7. T1 proves a live client runtime reports and accepts connections on loopback; T7's bind-address capture asserts the actual `sockaddr_in` uses `htonl(INADDR_LOOPBACK)` for every setup path that reaches `bind`.

- **S2.**
  - **Threat:** The CLI-supplied `--server` value drives the outbound Odin server destination for every accepted local proxy connection. If this RFC implicitly accepted hostnames or IPv6 without a supported resolver / address-family contract, a success banner could advertise a runtime that later fails each session or dials an address family the code has not specified.
  - **Mitigation:** §3.2.1 validates `server_host` with `inet_pton(AF_INET, ...)` before listener creation and before the success banner. Non-IPv4 parsed values fail deterministically with `odin: client startup failed at server_endpoint\n`; this RFC adds no DNS or IPv6 behavior.
  - **Enforcement:** §5 row T4 fires parsed-but-non-IPv4 values (`example.com:443` and `[::1]:443`) through the Client OK path and asserts failure before any success banner or listener.

- **S3.**
  - **Threat:** A local process that can connect to the loopback HTTPS_PROXY listener controls the HTTP `CONNECT host:port` target bytes that RFC-023 encodes into the Odin `CONNECT_REQ`. A client-runner bug that locally resolves or dials those peer-supplied target bytes would let that local process drive outbound TCP attempts from the client process instead of only from the configured Odin server side.
  - **Mitigation:** This runner does not resolve, filter, or dial the peer-supplied CONNECT target; it passes only the parsed `--server` endpoint to `odin_client_session_create` and lets RFC-023 forward the peer target as CONNECT_REQ payload. Trigger reachability is limited by S1's loopback bind. Outbound authorization for the peer target is explicitly delegated outside this RFC to the server-side policy layer: current code installs `odin_cli_default_server_dial_filter` through `odin_server_runtime_set_dial_filter`, propagates it with `odin_server_session_set_dial_filter`, and consults it before `odin_dial_start`.
  - **Enforcement:** §5 row T2 asserts the client-runner mitigation: peer-supplied CONNECT targets are forwarded byte-for-byte to the fake Odin server as `CONNECT_REQ` frames, and one live local target sentry accepts zero connections. The delegated server-side policy is not a new T-row in this RFC; §6 keeps these existing regression tests green by exact name: `OdinCliServerProcessTest.T4DenyLoopbackUpstream`, `OdinCliServerUnitTest.T5DefaultFilterDenyAllowMatrix`, `OdinServerRuntimeTest.T7`, and `OdinServerSessionTest.T19`.

## 5. Testing Strategy

Rows T1 and T2 spawn the host `odin-client` artifact when black-box process behavior is the contract; T7 includes one production bind-collision subcase that also spawns the host artifact. Rows that need test-only failpoints, including T4, fork `odin_unittests` children that call `odin_cli_main` with `argv[0] == "odin-client"` so the child links `cli_client_testing.c`. Every fixture that may write after peer close installs `signal(SIGPIPE, SIG_IGN)` before the write. Every stderr pipe, progress pipe, snapshot pipe, client socket, fake Odin server socket, and accepted upstream socket is read with `poll` / `select` deadlines or socket receive timeouts; no row relies on `waitpid` alone while the parent is blocked in a plain `read`, `accept`, or socket receive. Every child reap uses the shared bounded wait helper: poll `waitpid(pid, &status, WNOHANG)` until the per-row deadline, and on timeout send `SIGKILL`, reap the child with the same helper, and fail the assertion with the last observed step. Any parent write to a child release pipe is also deadlined; after a successful release write, the parent immediately waits through the same bounded wait helper. Existing parser, help, usage, error, symlink-help, server-runtime, server-session, and server CLI test suites remain regression gates in §6, but they are not new `T#` rows because their assertions are intentionally unchanged and therefore cannot provide a new red-to-green transition.

### 5.0 Coverage Matrix

| Axis | Value | Rows |
|------|-------|------|
| G# | G1 ready loopback runtime and byte-compatible banner | T1, T2 |
| G# | G2 accepted connections create/destroy tracked sessions or close failed setup fds and keep accepting | T2, T3, T5, T6 |
| G# | G3 deterministic failure lines and cleanup | T4, T6, T7 |
| G# | G4 graceful signal shutdown | T5 |
| Security | S1 loopback listener exposure | T1, T7 |
| Security | S2 IPv4-only configured server endpoint | T4 |
| Security | S3 client-runner no-local-dial handling for peer CONNECT targets; server egress policy delegated to existing tests | T2 |
| State | STARTUP, server endpoint validation OK | T1, T2 |
| State | STARTUP, server endpoint validation rejects parsed non-IPv4 | T4 |
| State | STARTUP, listener syscall or object-construction failure | T7 |
| State | STARTUP, production `bind(2)` collision on an occupied loopback port | T7 |
| State | STARTUP, signal handler / timer setup failure | T7 |
| State | RUNNING, accept returns conn_fd and session create succeeds | T2, T3, T5, T6 |
| State | RUNNING, accept returns conn_fd and session-entry allocation fails | T3 |
| State | RUNNING, accept returns conn_fd and `odin_client_session_create` fails | T3 |
| State | RUNNING, session `on_close` fires | T2, T3 |
| State | RUNNING, accept-loop terminal error | T6 |
| State | RUNNING, macOS `accept(2)` returns conn_fd then `fcntl(F_GETFL)` terminal failure closes fd before `on_accept` | T6 |
| State | RUNNING, macOS `accept(2)` returns conn_fd then `fcntl(F_SETFL)` terminal failure closes fd before `on_accept` | T6 |
| State | RUNNING, `odin_event_loop_run` returns failure | T6 |
| State | RUNNING, owner-thread `odin_event_loop_stop` returns cleanly without shutdown or accept-loop error state | T6 |
| State | RUNNING, SIGINT with two in-flight sessions | T5 |
| State | RUNNING, SIGTERM with two in-flight sessions | T5 |
| State | CLEANUP after startup failure | T4, T7 |
| State | CLEANUP after runtime failure with multiple in-flight sessions | T6 |
| State | CLEANUP after graceful shutdown with multiple in-flight sessions | T5 |
| Completion mode | Two sequential local HTTP CONNECT sessions reach OK responses through one client process | T2 |
| Completion mode | Later accepted session succeeds after a prior successful session closed | T2 |
| Completion mode | Later accepted session succeeds after a prior accepted fd was closed by local setup failure | T3 |
| Completion mode | Two simultaneous local HTTP CONNECT sessions remain in flight until graceful or failure cleanup | T5, T6 |
| Completion mode | Client-session staged write/read internals | N/A - owned by RFC-023 and not branched on by this runner |
| Decoder branch | HTTP CONNECT / Odin protocol decoder variants | N/A - this runner treats RFC-023 client sessions as opaque; decoder rows remain in RFC-023 |
| Benign-vs-fatal split | Benign accepted-connection local setup failure closes conn_fd and continues | T3 |
| Benign-vs-fatal split | Fatal accept-loop runtime error stops run and returns `1` | T6 |
| Benign-vs-fatal split | Fatal event-loop run error returns `1` | T6 |
| Constructor / factory precondition | IPv4-only server endpoint validation before banner | T4 |
| Constructor / factory precondition | Test hook invalid failpoint, fd, threshold, and constructor errno arguments rejected with `EINVAL` | T7 |
| Constructor / factory precondition | `accept_loop_create` setup failure rolls back | T7 |
| Constructor / factory precondition | `client_session_create` failure after accept leaves runner alive | T3 |
| Callback-safe lifecycle hand-off | Destroy client session from inside `on_close` | T2, T3 |
| Callback-safe lifecycle hand-off | Destroy every in-flight client session from cleanup | T5, T6 |
| Callback-safe lifecycle hand-off | Stop owner-thread event loop from accept-loop error callback | T6 |
| Callback-safe lifecycle hand-off | Stop owner-thread event loop from SIGINT/SIGTERM signal timer callback | T5 |
| Post-syscall sub-branch | `socket` OK then `setsockopt(SO_REUSEADDR)` fails | T7 |
| Post-syscall sub-branch | `socket` OK then `fcntl(F_GETFL)` fails | T7 |
| Post-syscall sub-branch | `fcntl(F_GETFL)` OK then `fcntl(F_SETFL)` fails | T7 |
| Post-syscall sub-branch | `fcntl(F_SETFL)` OK then `bind` fails | T7 |
| Post-syscall sub-branch | production `bind(2)` returns `EADDRINUSE` for an occupied loopback port | T7 |
| Post-syscall sub-branch | `bind` OK then `listen` fails | T7 |
| Post-syscall sub-branch | `listen` OK then `getsockname` fails | T7 |
| Post-syscall sub-branch | `accept` OK then entry allocation or client-session create fails | T3 |
| Post-syscall sub-branch | macOS `accept` OK then `fcntl(F_GETFL)` fails before `on_accept` | T6 |
| Post-syscall sub-branch | macOS `accept` OK then `fcntl(F_SETFL)` fails before `on_accept` | T6 |
| Post-syscall sub-branch | `sigaction(SIGINT)` OK then `sigaction(SIGTERM)` fails and restores SIGINT | T7 |
| Post-syscall sub-branch | both signal handlers OK then `signal_timer_start` fails and restores SIGINT plus SIGTERM | T7 |

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | Ephemeral client startup reports actual port after runtime is ready | Build `odin_cli_artifacts`; spawn `out/cli_client_mac/odin-client --listen 0 --server 127.0.0.1:4433` with stdout/stderr captured; deadline-read one stderr line; parse the decimal port; connect a TCP client to `127.0.0.1:<port>`; then send `SIGTERM` and wait for the child through the bounded wait helper | First stderr line matches `odin: mode=client listen=<P> server=127.0.0.1:4433\n` with `P > 0`; stdout is empty; TCP `connect` to `127.0.0.1:P` succeeds after the banner; the bounded wait helper reports normal child exit code `0` after `SIGTERM` | G1, S1 | e2e |
| T2 | Two sequential local CONNECTs reach the configured Odin server through RFC-023 session paths | Parent chooses `U` by binding the fake nonblocking Odin server listener on distinct loopback literal `127.0.0.2:0`, then also opens a nonblocking configured-server sentry listener on `127.0.0.1:U`. Parent separately opens a nonblocking CONNECT-target sentry listener on `127.0.0.1:V` with `V != U`. Spawn `odin-client --listen 0 --server 127.0.0.2:U`; after the startup line, connect local proxy client A to the reported port and send `CONNECT example.com:443 HTTP/1.1\r\n\r\n`; the configured `127.0.0.2:U` fake server accepts upstream A, decodes one CONNECT_REQ with `odin_proto_decode_connect_req`, sends CONNECT_RESP OK bytes, then closes upstream A after client A drains the HTTP response; parent closes client A and deadline-waits for both A-side fds to finish. Then connect local proxy client B to the same still-running `odin-client`, send `CONNECT 127.0.0.1:V HTTP/1.1\r\n\r\n`, and repeat the fake-server OK response flow on upstream B; throughout the row, both sentry listeners are polled with deadlines and must accept zero connections | The configured `127.0.0.2:U` fake server observes exactly two CONNECT_REQ frames in order: host `example.com`, port `443`, then host `127.0.0.1`, port `V`; the `127.0.0.1:U` configured-server sentry observes zero TCP connections and zero CONNECT_REQ bytes, so a runner that hardcodes `127.0.0.1` for `odin_client_session_create` fails this row. The `127.0.0.1:V` CONNECT-target sentry also observes zero TCP connections, so a runner that locally resolves or dials the peer-supplied CONNECT target fails this row. Both local proxy clients read byte-exact `HTTP/1.1 200 Connection Established\r\n\r\n`; the second connection is accepted only after the first session has completed and closed; child remains alive until the test sends `SIGTERM`; the bounded wait helper reports normal child exit code `0`. An implementation that uses `odin_client_listen_handshake` instead of RFC-023 would not connect to the configured fake Odin server and fails the CONNECT_REQ assertions | G1, G2, S3 | e2e |
| T3 | Accepted setup failure closes only that fd and later accepts still work | Run three forked-child subcases. In each subcase, child resets liveness, calls `odin_cli_client_test_set_idle_snapshot_fd(snapshot_write_fd, 1)`, then calls `odin_cli_main` as `odin-client --listen 0 --server 127.0.0.1:U`. Subcase A first calls `odin_cli_client_test_fail_next(ODIN_CLI_CLIENT_TEST_FAIL_NEXT_SESSION_ENTRY_ALLOC, ENOMEM)`; Subcase B calls `odin_cli_client_test_fail_next(ODIN_CLI_CLIENT_TEST_FAIL_NEXT_CLIENT_SESSION_CREATE, EIO)` for the CLI-local mirror; Subcase C calls `odin_client_session_test_fail_next_create(ENOMEM)` so the actual `odin_client_session_create` call returns `-1` after accept. Parent connects local client A to the startup port and writes a CONNECT request; A reads EOF or reset within the deadline and the fake Odin server accepts zero upstream connections. After the failed accepted fd, parent connects local client B, sends `CONNECT later.example:8443 HTTP/1.1\r\n\r\n`, the fake Odin server sends CONNECT_RESP OK, and B drains the byte-exact HTTP 200 response. Parent then half-closes client B's write side and the fake upstream B write side, deadline-waits for both B-side sockets to observe EOF or reset so the RFC-023 relay can complete, and only then deadline-reads one `odin_cli_client_test_liveness_t` from the idle snapshot pipe while the child is still blocked in `odin_cli_main`; only after that assertion does parent send `SIGTERM` to end the subcase and wait through the bounded wait helper | In all three subcases, client A receives no HTTP response and the fd closes silently; no stderr runtime failure line is printed; the process remains alive; client B succeeds through the fake Odin server and reads byte-exact `HTTP/1.1 200 Connection Established\r\n\r\n`; after the parent half-closes both B write sides, both B downstream and fake upstream sockets reach EOF or reset within deadlines; the owner-thread idle snapshot is received after that relay-completion wait and before any signal-driven cleanup, and reports `live_sessions == 0`, proving the successful B session was destroyed from `on_close` while the runtime continued. After parent sends `SIGTERM`, the bounded wait helper reports normal child exit code `0`. Subcase C proves the runner handles the real `odin_client_session_create(...) == -1` return path, not only the CLI mirror | G2 | integration |
| T4 | Parsed non-IPv4 server endpoints fail before banner | Invoke `odin_cli_main` in a forked test child as `odin-client --listen 0 --server example.com:443`, and again as `odin-client --listen 0 --server [::1]:443`, with stderr captured and liveness counters reset; wait for each child through the bounded wait helper; also run `odin_cli_parse` on the same argv to prove both are still parser OK inputs | `odin_cli_parse` returns `ODIN_CLI_OK` for both inputs; `odin_cli_main` returns `1`; the bounded wait helper reports normal child exit code `1`; stderr is exactly `odin: client startup failed at server_endpoint\n`; stdout is empty; no success banner prefix appears; `live_listeners == 0`, `live_accept_loops == 0`, `live_sessions == 0`, and event-loop liveness counters remain zero | G3, S2 | unit |
| T5 | SIGINT and SIGTERM graceful shutdown each destroy two in-flight client sessions and restore handlers | Run two forked-child subcases, one sending `SIGINT` and one sending `SIGTERM`. In each subcase, child installs custom counting handlers for `SIGINT` and `SIGTERM`, resets CLI and event-loop liveness counters, calls `odin_cli_client_test_set_progress_fd(progress_write_fd, 2)`, then calls `odin_cli_main` as `odin-client --listen 0 --server 127.0.0.1:U`. Parent starts a fake Odin server, connects local proxy clients A and B to the reported port, sends one CONNECT on each client, accepts upstream A and B, decodes both CONNECT_REQ frames, and deliberately withholds both CONNECT_RESP frames. Parent deadline-reads the progress byte proving `inflight_sessions >= 2`, then sends the subcase signal. Immediately after `odin_cli_main` returns, child snapshots client and event-loop liveness, raises `SIGINT` and `SIGTERM` once each, writes return code, handler counters, and liveness snapshots to a pipe, then blocks on a release pipe before exiting | Parent observes the child still alive before sending the subcase signal. Parent deadline-reads the snapshot while the child remains paused. After the snapshot, a fresh `connect(127.0.0.1:<reported_port>)` fails, proving listener closure before process exit. Both local proxy clients and both fake upstream accepted sockets observe EOF or reset within deadlines. Parent writes one byte to the release pipe with a deadline, then waits through the bounded wait helper. Return code is `0`; bounded wait reports normal child exit code `0`; custom handler counters each increment exactly once after return; `last_cleanup_sessions >= 2`; `live_listeners == 0`, `live_accept_loops == 0`, `live_sessions == 0`; event-loop liveness reports `loops == 0`, `io_handles == 0`, `timers == 0`, and `task_nodes == 0`; stderr contains the startup line and no failure line | G2, G4 | integration |
| T6 | Runtime failures print deterministic lines and clean up multiple in-flight sessions | Run runtime-failure forked-child subcases using the same post-return fixture as T5. In each subcase, child installs custom counting handlers for `SIGINT` and `SIGTERM`, resets CLI and event-loop liveness, calls `odin_cli_client_test_set_progress_fd(progress_write_fd, 2)`, sets `trigger_read_fd` to `O_NONBLOCK`, calls `odin_cli_client_test_set_runtime_trigger_fd(trigger_read_fd)`, arms the runtime failpoint, and calls `odin_cli_main` as `odin-client --listen 0 --server 127.0.0.1:U`. Parent starts a fake Odin server, connects local proxy clients A and B to the reported port, sends one CONNECT on each client, accepts upstream A and B, decodes both CONNECT_REQ frames, and deliberately withholds both CONNECT_RESP frames. Parent also deadline-reads the progress byte proving `inflight_sessions >= 2`; only after both CONNECT_REQ frames are decoded and that progress byte is read does the parent write one byte to the runtime trigger pipe. Subcase A calls `odin_cli_client_test_fail_next(ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_ERROR, EIO)`, whose owner-thread timer branch calls `odin_accept_loop_test_fail_next_accept(state.accept_loop, EIO)` after the progress and trigger bytes and wakes the real listener. Subcases B and C are macOS runtime cells with a Linux skip branch: on Linux, each cell first asserts `odin_cli_client_test_fail_next(ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_GETFL_ERROR, EINVAL)` or `odin_cli_client_test_fail_next(ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_SETFL_ERROR, EINVAL)` returns `-1` with `errno == EOPNOTSUPP`, then calls `GTEST_SKIP()` because Linux has no post-accept `fcntl` step. On macOS, Subcase B calls `odin_cli_client_test_fail_next(ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_GETFL_ERROR, EINVAL)`, whose owner-thread timer branch calls `odin_accept_loop_test_fail_next_fcntl(state.accept_loop, F_GETFL, EINVAL)` after the progress and trigger bytes and wakes the real listener; Subcase C does the same for `ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_SETFL_ERROR` and `F_SETFL`. Subcase D calls `odin_cli_client_test_fail_next(ODIN_CLI_CLIENT_TEST_FAIL_EVENT_LOOP_RUN, EIO)`, whose owner-thread timer branch calls `odin_event_loop_test_fail_next_backend_wait(state.loop, EIO)` after the progress and trigger bytes. Subcase E calls `odin_cli_client_test_trigger_next(ODIN_CLI_CLIENT_TEST_TRIGGER_UNEXPECTED_STOP)`, whose owner-thread timer branch calls `odin_event_loop_stop(state.loop)` after the progress and trigger bytes without setting shutdown or accept-error state. Immediately after `odin_cli_main` returns, child snapshots return code, client liveness, and event-loop liveness; raises `SIGINT` and `SIGTERM` once each; writes return code, handler counters, and liveness snapshots to a snapshot pipe; then blocks on a release pipe before exiting | No executed subcase consumes its runtime failpoint before the parent writes the runtime trigger byte after decoding both CONNECT_REQ frames. Subcases A, B, and C stderr is startup line followed by `odin: client runtime failed at accept_loop\n`; the accept-loop error reached the callback installed by `odin_accept_loop_create`, not a direct callback call. In Subcases B and C, the real macOS `accept(2)` succeeds before the synthetic `fcntl` failure, the accept-loop closes the half-configured wake fd, `cli_client_on_accept` is not called for that wake fd, and the snapshot reports `last_cleanup_sessions == 2`. Subcases D and E stderr is startup line followed by `odin: client runtime failed at event_loop_run\n`; Subcase D proves `odin_event_loop_run` itself returned `-1`, while Subcase E proves the `run_rc == 0 && !shutdown_requested && !accept_loop_error_seen` branch returns `1`. In every executed runtime-failure subcase, parent deadline-reads the snapshot while the child remains paused, verifies the child is still alive, performs a fresh `connect(127.0.0.1:<reported_port>)` that fails before process exit, observes EOF or reset on both local proxy clients and both fake upstream accepted sockets within deadlines, writes one release byte to the release pipe with a deadline, and waits through the bounded wait helper for normal child exit. The snapshot reports return code `1`, custom handler counters each incremented exactly once after return, `last_cleanup_sessions >= 2`, `live_listeners == 0`, `live_accept_loops == 0`, `live_sessions == 0`, and event-loop liveness `loops == 0`, `io_handles == 0`, `timers == 0`, and `task_nodes == 0` | G2, G3 | integration |
| T7 | Startup failure cleanup matrix, production bind collision, and loopback bind capture | First run a production bind-collision subcase with no failpoint armed: parent binds and listens on `127.0.0.1:0` without `SO_REUSEADDR` or `SO_REUSEPORT`, records occupied port `P`, keeps that listener open, spawns `out/cli_client_mac/odin-client --listen P --server 127.0.0.1:4433` with stdout/stderr captured, then waits for the child through the bounded wait helper and probes the parent listener by connecting to `127.0.0.1:P` and accepting that probe connection. Then reset client and event-loop liveness; create a temporary pipe and use its write end as the valid fd for invalid-threshold assertions; assert `odin_cli_client_test_fail_next((odin_cli_client_test_failpoint_t)0, EIO) == -1` with `errno == EINVAL`, `odin_cli_client_test_fail_next(ODIN_CLI_CLIENT_TEST_FAIL_SOCKET, 0) == -1` with `errno == EINVAL`, `odin_cli_client_test_trigger_next(ODIN_CLI_CLIENT_TEST_FAIL_SOCKET) == -1` with `errno == EINVAL`, `odin_cli_client_test_set_runtime_trigger_fd(-1) == -1` with `errno == EINVAL`, `odin_cli_client_test_set_progress_fd(-1, 1) == -1` with `errno == EINVAL`, `odin_cli_client_test_set_progress_fd(valid_pipe_write_fd, 0) == -1` with `errno == EINVAL`, `odin_cli_client_test_set_idle_snapshot_fd(-1, 1) == -1` with `errno == EINVAL`, `odin_cli_client_test_set_idle_snapshot_fd(valid_pipe_write_fd, 0) == -1` with `errno == EINVAL`, and `odin_client_session_test_fail_next_create(0) == -1` with `errno == EINVAL`. Close the temporary pipe after those assertions. For each startup failpoint from `SOCKET` through `SIGNAL_TIMER_START`, arm the failpoint, call `odin_cli_main` as `odin-client --listen P --server 127.0.0.1:4433` where `P` is a free loopback port chosen by a temporary bind, and capture stderr through `fmemopen` when the failpoint returns before the blocking loop. The `SIGACTION_SIGTERM` subcase preinstalls a custom counting `SIGINT` handler before the call. The `SIGNAL_TIMER_START` subcase preinstalls custom counting handlers for both `SIGINT` and `SIGTERM`, calls `odin_cli_main`, then raises `SIGINT` and `SIGTERM` once each after return before reading the counters | The production bind-collision bounded wait reports normal child exit code `1`; stdout is empty; stderr is exactly `odin: client startup failed at bind\n`; no success banner appears; the parent listener still accepts the probe connection after the child exits, proving the child neither stole nor closed the occupied listener. Invalid failpoint, trigger-helper, test-fd, threshold, and constructor-errno calls leave liveness unchanged and do not arm any later hook state. Each valid failpoint returns `1` and emits exactly the matching §3.2.1 startup failure line, flushed to the `fmemopen` stream before return; no success banner appears. After each failpoint subcase, a fresh `connect(127.0.0.1:P)` fails when the failure reached or passed `bind`, proving cleanup closed the partial listener. `odin_cli_client_test_liveness` reports `live_listeners == 0`, `live_accept_loops == 0`, `live_sessions == 0`; event-loop liveness reports zero loop-owned objects. For failpoints at `LISTEN` and later, `odin_cli_client_test_last_bind_addr` reports `sin_family == AF_INET`, `sin_addr.s_addr == htonl(INADDR_LOOPBACK)`, and `ntohs(sin_port) == P`; the `sigaction(SIGTERM)` failure subcase proves the earlier SIGINT handler was restored by raising SIGINT after return and observing the custom preinstalled handler; the `signal_timer_start` failure subcase proves both replaced handlers were restored after both installations succeeded by observing the custom SIGINT and SIGTERM counters each increment exactly once after return | G3, S1 | integration |

## 6. Implementation Plan

- **P1. Land the client-runner seam and red-verifiable `T1`-`T7`.**
  - **Scope:** add internal `odin/cli_client.h`, `odin/cli_client.c`, `odin/testing/cli_client_internal_test.h`, and `odin/testing/cli_client_testing.c`. Change only the `ODIN_CLI_OK` / `ODIN_CLI_MODE_CLIENT` arm in `odin/cli.c` to flush `out` and return `odin_cli_run_client(args.listen_port, args.server_host, args.server_host_len, args.server_port, err)`; keep the Server arm as the current `rc = odin_cli_run_server(args.listen_port, err)` followed by the existing common final `fflush(out)` / `fflush(err)` block; do not edit `odin/main.c`. The P1 `odin_cli_run_client` helper remains a banner-only stub that preserves the current immediate Client OK behavior for IPv4 and non-IPv4 inputs so the red rows fail against known current behavior. Add `source_set("odin_cli_client")` to `odin/BUILD.gn`, make production `:odin` depend on it, and keep it independent of `odin/client_listen.h` and the `odin_client_listen_*` functions. Add `config("odin_cli_client_testing_config") { defines = [ "ODIN_CLI_CLIENT_TESTING" ] }`, add `cli_client_testing.c` and `cli_client_internal_test.h` with C++ linkage guards to `odin_unittests`, and apply the config target-wide so the test wrapper and C++ tests see the gated declarations. Extend `odin/testing/client_session_internal_test.h` and `odin/client_session.c` with `odin_client_session_test_fail_next_create` under the existing C++-linkage-guarded `ODIN_CLIENT_SESSION_TESTING` gate; `odin_unittests` already applies `odin_client_session_testing_config`, `odin_accept_loop_testing_config`, and `odin_event_loop_testing_config` target-wide, which the new CLI rows use for the cross-module constructor, accept-loop, and event-loop hooks. The `ODIN_CLI_CLIENT_TESTING` block in `odin/cli_client.c` includes `odin/testing/event_loop_internal_test.h` and `odin/testing/accept_loop_internal_test.h`; references to `odin_accept_loop_test_fail_next_fcntl` are compiled only on non-Linux. Add T1-T7 as `OdinCliClientProcessTest.*` / `OdinCliClientUnitTest.*`, each skipped unless `ODIN_CLI_CLIENT_RED=1` is set. Update existing `cli_unittests.cpp` rows for the new blocking Client OK behavior: remove Client OK `fmemopen` rows, keep parser/help/error rows byte-exact, and switch symlink dispatch to `odin-client --help`. Do not change parser grammar, help strings, usage strings, Server-mode runner code, `odin/main.c`, or the old `odin/client_listen.*` module.
  - **Depends on:** None.
  - **Done when:** `./tool/gn gen out/cli_client_mac --args='target_os="mac"'` and `./tool/gn gen out/cli_client_linux_x64 --args='target_os="linux" target_cpu="x64"'` resolve, and `./tool/ninja -C out/cli_client_mac odin_main odin_unittests tests` plus `./tool/ninja -C out/cli_client_linux_x64 odin_main odin_unittests tests` build without error. The red-verification command `ODIN_CLI_CLIENT_RED=1 out/cli_client_mac/odin_unittests --gtest_filter='OdinCliClientProcessTest.*:OdinCliClientUnitTest.*'` executes T1-T7 and fails them against the banner-only stub: T1 because the child exits immediately and no listener accepts; T2 because the configured `127.0.0.2` fake Odin server receives no CONNECT_REQ frames, no second accepted session can happen, and no CONNECT-target sentry assertion can be reached; T3 because no accept path exists for accepted-fd failure, real constructor failure, later recovery, or an owner-thread idle snapshot after normal B relay completion and before signal cleanup; T4 because parsed non-IPv4 inputs print the old success banner and return `0`; T5 because no long-running process has two in-flight sessions to destroy on runtime `SIGINT` or `SIGTERM`; T6 because no two-session runtime-failure cleanup path exists after startup, no parent-controlled runtime trigger gate exists, no macOS post-accept `fcntl` terminal-error trigger exists, and no unexpected clean event-loop stop trigger exists; T7 because production bind collision returns the old success banner / `0`, and startup failpoints, invalid-hook rejection coverage, bind-address liveness, and startup-failure signal-handler restoration probes are not implemented. The default `out/cli_client_mac/odin_unittests --gtest_brief=1` reports T1-T7 skipped and exits zero with all pre-existing Odin suites green after the Client OK row migrations, including parser, help, usage, parse-error, `odin-client --help` symlink dispatch, and RFC-022 server rows. The Linux x64 binary is cross-compiled but not executed in this environment; its accept4 and timerfd branches are compiled but not run.

- **P2. Implement the long-running client runtime and turn `T1`-`T7` green.**
  - **Scope:** replace the P1 banner-only client helper with the §3.2.1 through §3.2.3 runtime: IPv4-only `server_endpoint` validation, loopback listener setup, actual-port discovery, `odin_event_loop_create`, `odin_accept_loop_create`, accepted-connection session tracking, silent accepted-fd close on entry-allocation or `odin_client_session_create` failure, accept-loop runtime error callback, signal handlers, signal polling timer, deterministic startup/runtime failure lines that immediately `fflush(err)`, and ordered cleanup. Implement the §3.2.4 test hooks, liveness counters, bind-address capture, thresholded progress pipe, parent-controlled runtime trigger pipe, owner-thread idle snapshot pipe, startup/runtime failpoints, cross-module constructor-create failpoint, accept-loop wake failure through `odin_accept_loop_test_fail_next_accept`, macOS post-accept fcntl failures through non-Linux-guarded calls to `odin_accept_loop_test_fail_next_fcntl`, event-loop backend failure through `odin_event_loop_test_fail_next_backend_wait`, unexpected clean stop through `ODIN_CLI_CLIENT_TEST_TRIGGER_UNEXPECTED_STOP`, and accepted-connection setup-failure failpoints. Remove the `ODIN_CLI_CLIENT_RED` skip gates so T1-T7 assert in the default host test run. Add no DNS resolution, IPv6 dialing, authentication, daemonization, config files, new public CLI flags, client-side CONNECT-target allow/deny policy, or calls to `odin_client_listen_open` / `odin_client_listen_handshake`.
  - **Depends on:** P1.
  - **Done when:** the P1 build commands still succeed; `out/cli_client_mac/odin_unittests --gtest_filter='OdinCliClientProcessTest.*:OdinCliClientUnitTest.*'` passes T1-T7 un-gated on the host macOS architecture. The unfiltered host run `out/cli_client_mac/odin_unittests --gtest_brief=1` exits zero with existing parser, help, error, Client-listen unit, client-session unit, accept-loop unit, server-runtime, server-session, and server CLI suites still green, including `OdinCliServerProcessTest.T4DenyLoopbackUpstream`, `OdinCliServerUnitTest.T5DefaultFilterDenyAllowMatrix`, `OdinServerRuntimeTest.T7`, and `OdinServerSessionTest.T19` as the delegated S3 server-side policy regression gate. Host-runnable enumeration: T1-T7 all run in `out/cli_client_mac/odin_unittests`; T1 and T2 spawn the host `odin-client` artifact produced by `//odin:odin_cli_artifacts` at `out/cli_client_mac/odin-client`; T7's production bind-collision subcase also spawns that artifact; T3, T4, T5, T6, and T7's test-hook subcases fork the host test binary so the `ODIN_CLI_CLIENT_TESTING` hooks are available. Cross-compile-only enumeration: `out/cli_client_linux_x64/odin_unittests` and `out/cli_client_linux_x64/odin` are built but not executed; the Linux `epoll`+timerfd signal-timer branch and the Linux `accept4(SOCK_NONBLOCK)` accepted-fd branch are compiled and code-reviewed, not runtime-verified in this RFC. The two T6 post-accept `fcntl` cells are macOS-only because the Linux branch uses `accept4(SOCK_NONBLOCK)` and has no post-accept fcntl step; Linux builds compile the enum values and the `EOPNOTSUPP` rejection path, and those cells are skipped if the Linux test binary is run elsewhere. T2 provides two sequential successful CONNECTs through one process after the first session closes, using parsed `--server 127.0.0.2:U` and a `127.0.0.1:U` sentry to prove the configured host, not a hardcoded loopback literal, receives the CONNECT_REQ frames; the second CONNECT uses target `127.0.0.1:V` with a separate sentry listener that accepts zero connections, proving the runner forwards peer-supplied CONNECT targets instead of dialing them locally. T3 covers both CLI-local accepted setup failure and the real `odin_client_session_create(...) == -1` return path, then half-closes B's downstream and fake upstream write sides, deadline-waits for relay completion, and receives an owner-thread idle snapshot with `live_sessions == 0` while the runtime is still running; T5 runs both SIGINT and SIGTERM subcases and proves each graceful shutdown destroys two simultaneous in-flight sessions with `last_cleanup_sessions >= 2`; T6 uses two simultaneous in-flight sessions in every executed runtime-failure subcase: synthetic accept errno, macOS `fcntl(F_GETFL)`, macOS `fcntl(F_SETFL)`, backend-wait failure, and unexpected clean stop. T6 proves both CONNECT_REQ frames are decoded before the parent writes the runtime trigger byte that permits failpoint consumption; it also proves `last_cleanup_sessions >= 2` in all executed subcases and `last_cleanup_sessions == 2` in the two macOS fcntl subcases. T5, T6, and T7 provide deterministic cleanup evidence through zero client liveness counters, zero event-loop liveness counters, bounded child waits, and restored signal handlers; T5 and T6 additionally prove listener refusal while the child is paused before process exit. T7 specifically proves a production-path occupied-port `bind(2)` failure returns `1`, prints exactly `odin: client startup failed at bind\n`, prints no success banner, leaves the parent listener able to accept, and rejects every documented invalid failpoint, trigger-helper, fd, threshold, and constructor-errno test hook with `EINVAL` without arming hook state. T7 also proves `SIGACTION_SIGTERM` restores the earlier SIGINT handler and `SIGNAL_TIMER_START` restores both replaced handlers after both installations succeeded. T5 and T6 use post-return snapshot pipes and release pipes in every executed paused-child subcase, so the parent verifies the child is paused after `odin_cli_main` returns, observes return code, handler counters, zero liveness, listener refusal, and EOF or reset on both sides of both in-flight sessions before writing a deadlined release byte and waiting through the bounded wait helper. The ASan build commands `./tool/gn gen out/cli_client_mac_asan --args='target_os="mac" is_asan=true'`, `./tool/ninja -C out/cli_client_mac_asan odin_unittests`, and `out/cli_client_mac_asan/odin_unittests --gtest_filter='OdinCliClientProcessTest.*:OdinCliClientUnitTest.*'` exit without AddressSanitizer reports as memory-safety backing for destroy-from-`on_close`, graceful session-list teardown, accept-loop error cleanup including macOS post-accept `fcntl` failure cleanup, unexpected-stop cleanup, and accepted-fd setup-failure recovery. Production `out/cli_client_mac/odin` and `out/cli_client_linux_x64/odin` contain no `odin_cli_client_test_*` symbols and no `odin_client_session_test_fail_next_create` symbol; `rg -n "odin_client_listen_(open|handshake)|client_listen.h" odin/cli_client.c` finds zero matches; `git diff -- odin/main.c` is empty; and `./tidy_odin.sh` exits clean over the touched Odin files.
