# RFC-022: CLI Server Runtime Wiring

## 1. Summary

Wire `odin-server` OK invocations to the existing IPv4 listener, event-loop, and server-runtime stack, with a CLI-owned default SSRF deny filter and graceful signal shutdown.

## 2. Goals

- **G1.** For every successful `odin-server` OK invocation, the process binds a nonblocking IPv4 TCP listener on `0.0.0.0:<listen_port>`, reports the actual bound port on stderr, and runs the implemented server runtime so a raw Odin control connection is handled by the existing RFC-020 session path.

- **G2.** For every server startup or runtime setup failure in the CLI-owned setup sequence, the process emits one deterministic stderr line, releases every partially created CLI-owned object, leaves no listening socket behind, and returns exit code `1`.

- **G3.** A `SIGINT` or `SIGTERM` delivered after successful server startup causes the owner-thread event loop to stop, destroys the runtime, destroys the event loop, closes the listening fd, restores replaced signal handlers, and returns exit code `0`.

- **G4.** Non-public IPv4 CONNECT_REQ destinations sent to the CLI server are rejected before any outbound connection is attempted, while public IPv4 destinations are allowed to continue into the existing RFC-020 dial path.

## 3. Design

### 3.1 Overview

`odin_cli_main` remains the single entry point behind the `odin` binary and the `odin-client` / `odin-server` symlinks. The parser and all non-server CLI status rows stay in `odin/cli.c`: client OK still prints the current banner and returns immediately, help still writes usage to `out`, and parse errors still write usage to `err` and return `2`.

The only changed control flow is the `ODIN_CLI_OK` plus `ODIN_CLI_MODE_SERVER` branch. That branch delegates to an internal server runner. The runner creates a CLI-owned listener, event loop, runtime, signal-stop timer, and default dial filter, then prints the startup banner and enters `odin_event_loop_run`. Accepted connections are owned by `odin_server_runtime`; the CLI never closes accepted fds, session fds, dial fds, or runtime-owned transports.

```
odin-server argv
    |
    v
odin_cli_parse -> OK/SERVER
    |
    v
CLI server runner
    |-- socket + SO_REUSEADDR + O_NONBLOCK + bind/listen/getsockname
    |-- odin_event_loop_create
    |-- odin_server_runtime_create(listener fd)
    |-- odin_server_runtime_set_dial_filter(default deny filter)
    |-- install SIGINT/SIGTERM handlers and owner-thread polling timer
    |-- fprintf(stderr, "odin: mode=server listen=<actual_port>\n")
    v
odin_event_loop_run
    |
    v
cleanup: runtime -> loop -> close(listener fd) -> restore handlers
```

### 3.2 Detailed Design

#### 3.2.1 Server CLI Startup, Ownership, and Errors

Contract surface - `odin/cli.h` keeps the existing public entry point:

```c
#include <stdio.h>

int odin_cli_main(int argc, char *const *argv, FILE *out, FILE *err);
```

The server branch is factored into an internal helper:

```c
/* odin/cli_server.h, internal to the odin target. */
#include <stdint.h>
#include <stdio.h>

int odin_cli_run_server(uint16_t listen_port, FILE *err);
```

The resulting `odin_cli_main` status map is:

```text
odin_cli_parse result                         observable behavior
-------------------------------------------  ---------------------------------------------
ODIN_CLI_OK, mode == ODIN_CLI_MODE_CLIENT    existing client banner to err; return 0
ODIN_CLI_OK, mode == ODIN_CLI_MODE_SERVER    odin_cli_run_server(args.listen_port, err)
ODIN_CLI_HELP                                existing usage to out; return 0
ODIN_CLI_ERR_*                               existing deterministic error + usage to err; return 2
```

**Unstated contract.** `odin_cli_run_server` is an owner-thread, blocking call. It returns `0` only after a graceful signal-driven stop that leaves `state.shutdown_requested != 0`; it returns `1` for setup failure, runtime callback failure, or `odin_event_loop_run` failure. The helper does not write to `out`. It writes the success banner to `err` only after the listener has successfully bound and listened, the actual port is known, the event loop exists, the runtime exists, the default dial filter is installed on the runtime, and the signal-stop mechanism is armed. This prevents a caller or test from treating a printed port as ready while a later setup step can still fail.

`listen_port == 0` means kernel-selected ephemeral port. The helper uses `getsockname` after `bind` / `listen` to obtain the actual port, so the success line is exactly `odin: mode=server listen=<actual_port>\n` with `<actual_port>` in decimal and never `0` for an ephemeral bind that succeeded. The listener address is `AF_INET`, `INADDR_ANY`, and the parsed port in network byte order. The socket setup order is fixed: `socket(AF_INET, SOCK_STREAM, 0)`, `setsockopt(SOL_SOCKET, SO_REUSEADDR, 1)`, `fcntl(F_GETFL)`, `fcntl(F_SETFL, flags | O_NONBLOCK)`, `bind(0.0.0.0:<port>)`, `listen(SOMAXCONN)`, `getsockname`. Linux and macOS both compile this POSIX sequence; this environment executes the macOS host binary only, while the Linux binary is cross-compiled and checked by review.

Among server-stack objects, the CLI owns exactly the objects it creates: the listening fd, the `odin_event_loop_t`, and the `odin_server_runtime_t`. `odin_server_runtime_destroy` still does not close the listening fd; the CLI closes that fd after destroying the runtime and event loop. The signal polling timer is an event-loop timer handle, so `odin_event_loop_destroy` releases it with the loop-owned timer set; the CLI does not close or free a separate timer object. The CLI never closes fds accepted by the runtime, and never closes fds owned by server sessions, dial attempts, relays, or fd transports.

All setup failures route through one cleanup function. The cleanup order is: stop the signal timer if it has a live handle, destroy runtime if non-null, destroy event loop if non-null, close the listening fd if `>= 0`, restore any replaced signal handlers, then return `1`. This matches the graceful-shutdown order after the owner-thread stop request: runtime, loop, listening fd, handlers. The cleanup function preserves the first failed step name for stderr and does not print `strerror`, because libc wording and localization are not stable enough for deterministic tests. Exact failure lines are:

```text
odin: server startup failed at socket
odin: server startup failed at setsockopt(SO_REUSEADDR)
odin: server startup failed at fcntl(F_GETFL)
odin: server startup failed at fcntl(F_SETFL)
odin: server startup failed at bind
odin: server startup failed at listen
odin: server startup failed at getsockname
odin: server startup failed at event_loop_create
odin: server startup failed at server_runtime_create
odin: server startup failed at sigaction(SIGINT)
odin: server startup failed at sigaction(SIGTERM)
odin: server startup failed at signal_timer_start
odin: server runtime failed at accept_loop
odin: server runtime failed at event_loop_run
```

The runtime error callback is the bridge from `odin_server_runtime` terminal errors to the CLI. It records `state.runtime_error_seen = 1`, stores the errno for debugging-only local state, and calls `odin_event_loop_stop(state.loop)` on the owner thread. After `odin_event_loop_run` returns, the CLI prints `odin: server runtime failed at accept_loop\n`, performs the same cleanup order, and returns `1`. If `odin_event_loop_run` itself returns `-1`, the CLI prints `odin: server runtime failed at event_loop_run\n`, performs the same cleanup, and returns `1`.

**Mechanism.**

```text
odin_cli_main(argc, argv, out, err):
  status, args = odin_cli_parse(argc, argv)
  if status == OK and args.mode == CLIENT:
    print existing client banner to err
    flush streams
    return 0
  if status == OK and args.mode == SERVER:
    flush out
    return odin_cli_run_server(args.listen_port, err)
  handle HELP and ERR statuses with the existing mapping

odin_cli_run_server(listen_port, err):
  state = zeroed server state
  state.listen_fd = -1

  state.listen_fd = socket(AF_INET, SOCK_STREAM, 0)
  if fail: return startup_fail("socket")
  if setsockopt(SO_REUSEADDR) fails: return startup_fail("setsockopt(SO_REUSEADDR)")
  flags = fcntl(F_GETFL)
  if fail: return startup_fail("fcntl(F_GETFL)")
  if fcntl(F_SETFL, flags | O_NONBLOCK) fails: return startup_fail("fcntl(F_SETFL)")
  if bind(INADDR_ANY, listen_port) fails: return startup_fail("bind")
  if listen(SOMAXCONN) fails: return startup_fail("listen")
  actual_port = getsockname_port(state.listen_fd)
  if fail: return startup_fail("getsockname")

  if odin_event_loop_create(&state.loop) != 0:
    return startup_fail("event_loop_create")
  if odin_server_runtime_create(state.loop, state.listen_fd,
                                cli_runtime_on_error, &state,
                                &state.runtime) != 0:
    return startup_fail("server_runtime_create")
  odin_server_runtime_set_dial_filter(state.runtime,
                                      odin_cli_default_server_dial_filter,
                                      NULL)
  if install_signal_handlers(&state) fails:
    return startup_fail(failing_sigaction_name)
  if odin_event_timer_start(state.loop, 50000, 50000,
                            cli_signal_poll_timer, &state,
                            &state.signal_timer) != 0:
    return startup_fail("signal_timer_start")

  fprintf(err, "odin: mode=server listen=%u\n", actual_port)
  fflush(err)

  run_rc = odin_event_loop_run(state.loop)
  if state.runtime_error_seen:
    print "odin: server runtime failed at accept_loop\n"
    cleanup
    return 1
  if run_rc != 0:
    print "odin: server runtime failed at event_loop_run\n"
    cleanup
    return 1
  cleanup
  return state.shutdown_requested ? 0 : 1
```

Satisfies: G1 via the listener setup, actual-port banner, event-loop run, and runtime creation; G2 via the fixed failure-line table and ordered cleanup; G3 via the shared cleanup path after a clean stop; G4 via the `odin_server_runtime_set_dial_filter` call before the success banner and before `odin_event_loop_run`.

#### 3.2.2 Default CLI Dial Filter

Contract surface:

```c
#include <sys/socket.h>

static int odin_cli_default_server_dial_filter(const struct sockaddr *addr,
                                               socklen_t addrlen,
                                               void *user_data);
```

The test-only mirror is visible only when `ODIN_CLI_SERVER_TESTING` is defined:

```c
/* odin/cli_server_internal_test.h */
#include <sys/socket.h>

int odin_cli_server_test_default_dial_filter(const struct sockaddr *addr,
                                             socklen_t addrlen);
```

**Unstated contract.** Production sessions reach this filter only through RFC-020's `odin_server_session_set_dial_filter` hook: the session has already decoded a CONNECT_REQ host slice and port, parsed the host with `inet_pton(AF_INET, host_cstr, &sa.sin_addr)`, copied the peer-supplied port into `sa.sin_port`, built a `sockaddr_in`, and is about to call `odin_dial_start`. This filter therefore treats `AF_INET` as the production contract and evaluates the destination address while preserving the peer-supplied port in the `sockaddr_in` passed through the hook. A `NULL` address, non-`AF_INET` family, or too-short `addrlen` is denied with `EAFNOSUPPORT` rather than allowed by accident.

The default CLI policy does not restrict destination ports for otherwise allowed public IPv4 addresses. Any port in the decoded CONNECT_REQ is delegated to the existing RFC-020 dial path when the address check returns `0`. This keeps RFC-022 scoped to the required SSRF denylist while leaving a future upstream policy free to inspect both `sin_addr` and `sin_port` through the same dial-filter hook.

The deny decision uses the IPv4 address in host byte order. It returns `EACCES` for every denied destination:

```text
range                      reason
-------------------------  --------------------------------------------------
0.0.0.0/8                  unspecified or current-network destinations
10.0.0.0/8                 RFC1918 private
100.64.0.0/10              shared carrier-grade NAT, not public Internet
127.0.0.0/8                loopback
169.254.0.0/16             link-local, including 169.254.169.254 metadata
172.16.0.0/12              RFC1918 private
192.0.0.0/24               IETF protocol assignments, not public Internet
192.0.2.0/24               TEST-NET-1 documentation range
192.168.0.0/16             RFC1918 private
198.18.0.0/15              benchmarking range
198.51.100.0/24            TEST-NET-2 documentation range
203.0.113.0/24             TEST-NET-3 documentation range
224.0.0.0/4                multicast
240.0.0.0/4                reserved, including 255.255.255.255 broadcast
```

Every other IPv4 address returns `0`. A `0` return means the existing RFC-020 path proceeds unchanged: `session_on_req_decoded` calls `odin_dial_start` with the parsed `sockaddr_in`, and the eventual CONNECT_RESP mapping remains owned by RFC-020. A nonzero return means RFC-020 synthesizes a dial failure with that errno, sends the existing failure CONNECT_RESP mapping, and does not issue an outbound `connect(2)`.

**Mechanism.**

```text
default_filter(addr, addrlen, user_data):
  ignore user_data
  if addr == NULL or addrlen < sizeof(sockaddr_in): return EAFNOSUPPORT
  sin = (const sockaddr_in *)addr
  if sin.family != AF_INET: return EAFNOSUPPORT
  ip = ntohl(sin.addr.s_addr)
  if ip matches any deny range in the table: return EACCES
  return 0
```

Satisfies: G4 via a deny-by-range filter that returns a concrete errno for non-public IPv4 destinations and `0` for public IPv4 destinations.

#### 3.2.3 Graceful Signal Stop

Contract surface:

```c
#include <signal.h>

static volatile sig_atomic_t odin_cli_server_signal_seen;

static void odin_cli_server_signal_handler(int signum);
static void odin_cli_server_signal_timer(odin_event_loop_t *loop,
                                         odin_event_timer_t *timer,
                                         void *user_data);
```

**Unstated contract.** The POSIX signal handler never calls `odin_event_loop_stop`, `fprintf`, `malloc`, `close`, or any other non-async-signal-safe helper. It only stores the received signal number in `odin_cli_server_signal_seen`. The owner-thread recurring event-loop timer polls that flag every 50 ms. When the flag is nonzero, the timer callback sets `state.shutdown_requested = 1`, stops the timer handle with `odin_event_timer_stop(timer)`, clears `state.signal_timer`, and calls `odin_event_loop_stop(loop)` from the loop owner thread.

The helper saves the previous `SIGINT` and `SIGTERM` handlers before replacing them. If replacing `SIGTERM` fails after `SIGINT` has been replaced, cleanup restores `SIGINT` before returning `1`. On every post-startup exit path, including startup failure after one handler is replaced, runtime failure, and graceful signal shutdown, cleanup restores each handler that this helper replaced. The implementation uses `sigaction` with no `SA_RESTART`; an interrupted backend wait is harmless because the event loop already treats `EINTR` as a retry, and the polling timer provides the owner-thread stop. On macOS the timer is driven by the kqueue timeout path; on Linux it is driven by the event loop's timerfd path. Only the macOS host binary is runtime-verified in this environment; the Linux timerfd branch is cross-compiled but not executed here.

**Mechanism.**

```text
signal_handler(signum):
  odin_cli_server_signal_seen = signum

install_signal_handlers(state):
  odin_cli_server_signal_seen = 0
  save old SIGINT action
  install signal_handler for SIGINT
  mark SIGINT replaced
  save old SIGTERM action
  install signal_handler for SIGTERM
  mark SIGTERM replaced

signal_timer(loop, timer, user_data):
  state = user_data
  if odin_cli_server_signal_seen == 0:
    return
  state.shutdown_requested = 1
  state.signal_timer = NULL
  odin_event_timer_stop(timer)
  odin_event_loop_stop(loop)

restore_signal_handlers(state):
  if SIGTERM was replaced: sigaction(SIGTERM, old_term)
  if SIGINT was replaced: sigaction(SIGINT, old_int)
```

Satisfies: G3 via an async-signal-safe handler, owner-thread stop request, and restoration of replaced handlers on every cleanup path.

#### 3.2.4 Test-Only Server CLI Hooks

```c
/* odin/cli_server_internal_test.h, visible only when ODIN_CLI_SERVER_TESTING. */
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

typedef enum odin_cli_server_test_failpoint_t {
  ODIN_CLI_SERVER_TEST_FAIL_SOCKET = 1,
  ODIN_CLI_SERVER_TEST_FAIL_SETSOCKOPT_REUSEADDR,
  ODIN_CLI_SERVER_TEST_FAIL_FCNTL_GETFL,
  ODIN_CLI_SERVER_TEST_FAIL_FCNTL_SETFL,
  ODIN_CLI_SERVER_TEST_FAIL_BIND,
  ODIN_CLI_SERVER_TEST_FAIL_LISTEN,
  ODIN_CLI_SERVER_TEST_FAIL_GETSOCKNAME,
  ODIN_CLI_SERVER_TEST_FAIL_EVENT_LOOP_CREATE,
  ODIN_CLI_SERVER_TEST_FAIL_SERVER_RUNTIME_CREATE,
  ODIN_CLI_SERVER_TEST_FAIL_SIGACTION_SIGINT,
  ODIN_CLI_SERVER_TEST_FAIL_SIGACTION_SIGTERM,
  ODIN_CLI_SERVER_TEST_FAIL_SIGNAL_TIMER_START,
  ODIN_CLI_SERVER_TEST_FAIL_EVENT_LOOP_RUN,
  ODIN_CLI_SERVER_TEST_TRIGGER_RUNTIME_ERROR
} odin_cli_server_test_failpoint_t;

typedef struct odin_cli_server_test_liveness_t {
  size_t live_listeners;
  size_t live_runtimes;
  size_t last_cleanup_runtime_inflight;
} odin_cli_server_test_liveness_t;

typedef struct odin_cli_server_test_dial_start_t {
  int family;
  uint32_t ipv4_addr_nbo;
  uint16_t port_nbo;
} odin_cli_server_test_dial_start_t;

int odin_cli_server_test_fail_next(odin_cli_server_test_failpoint_t fp,
                                   int errnum);
void odin_cli_server_test_reset_liveness(void);
int odin_cli_server_test_liveness(odin_cli_server_test_liveness_t *out);
int odin_cli_server_test_last_bind_addr(struct sockaddr_in *out);
int odin_cli_server_test_set_progress_fd(int fd);
void odin_cli_server_test_set_dial_start_probe_fd(int fd, int errnum);
int odin_cli_server_test_maybe_probe_dial_start(const struct sockaddr *addr,
                                                socklen_t addrlen);
```

**Unstated contract.** Production builds do not declare or define these symbols. The hook stores one process-local failpoint and errno for the next `odin_cli_run_server` call in the test binary. Each startup failpoint is consumed at the named step, sets `errno = errnum`, and routes through the same cleanup and stderr path as the real step before the success banner is printed.

`ODIN_CLI_SERVER_TEST_FAIL_EVENT_LOOP_RUN` and `ODIN_CLI_SERVER_TEST_TRIGGER_RUNTIME_ERROR` are post-banner failpoints: they are not consumed until the listener, event loop, runtime, default dial filter, signal handlers, and signal timer are live and the success banner has been flushed. `ODIN_CLI_SERVER_TEST_FAIL_EVENT_LOOP_RUN` then bypasses the blocking backend wait and returns as if `odin_event_loop_run` failed with `errno = errnum`. `ODIN_CLI_SERVER_TEST_TRIGGER_RUNTIME_ERROR` is consumed by a test-only branch in the already-live owner-thread signal timer after `odin_event_loop_run` has set `loop->running`; that branch invokes the same CLI runtime-error callback that `odin_server_runtime` would invoke, and the callback's `odin_event_loop_stop(state.loop)` request is therefore effective. No test hook invokes the runtime-error callback before `odin_event_loop_run` begins.

The liveness hook is deterministic test evidence for CLI-owned objects, not a production API. `odin_cli_server_test_reset_liveness` clears the counters, progress fd, and dial-start probe, `odin_cli_server_test_liveness` copies the current counters, and `odin_cli_server_test_last_bind_addr` copies the exact `sockaddr_in` passed to the most recent successful `bind` attempt. `live_listeners` increments immediately after `socket` succeeds and the CLI owns a listener fd, then decrements immediately after the CLI closes that fd. `live_runtimes` increments only after `odin_server_runtime_create` succeeds and decrements immediately after `odin_server_runtime_destroy` returns. When `ODIN_SERVER_RUNTIME_TESTING` is enabled in the same test target, cleanup records `last_cleanup_runtime_inflight = odin_server_runtime_test_inflight_count(state.runtime)` immediately before destroying a non-null runtime, so a forked test can prove the runtime had accepted sessions at cleanup time before the child process exits. `odin_cli_server_test_set_progress_fd(fd)` arms a child-owned pipe fd; the owner-thread signal timer writes one byte to that fd once `odin_server_runtime_test_inflight_count(state.runtime) > 0`, letting a parent wait until a raw client has been accepted before it sends SIGINT or SIGTERM. Event-loop, timer, I/O-handle, and task-node liveness remain covered by the existing `odin_event_loop_test_liveness` counters from `odin/event_loop_internal_test.h`.

`odin_cli_server_test_set_dial_start_probe_fd(fd, errnum)` arms a process-local, one-shot synthetic dial observation used only by forked CLI-server tests. When both `ODIN_CLI_SERVER_TESTING` and `ODIN_DIAL_TESTING` are defined, `odin/dial.c` includes `odin/cli_server_internal_test.h` and calls `odin_cli_server_test_maybe_probe_dial_start(addr, addrlen)` as the first test-only branch in `odin_dial_start`, before `calloc` and before `socket(2)`. If no probe is armed, `odin_cli_server_test_maybe_probe_dial_start` returns `0` and `odin_dial_start` proceeds unchanged. If a probe is armed, the function writes one `odin_cli_server_test_dial_start_t` record to `fd` for a valid `AF_INET` `sockaddr_in`, capturing the family, destination address in network byte order, and destination port in network byte order. If the address is not a valid IPv4 `sockaddr_in`, the armed probe is still consumed but no record is written; T4 arms the probe only around the public-IPv4 CONNECT_REQ path, so a missing record is a test failure through the probe-pipe deadline. In both cases, the function clears the probe, sets `errno = errnum`, and returns `-1`; `odin_dial_start` propagates that `-1` without allocating a dial object, creating an outbound socket, or invoking the dial callback. This proves that a public IPv4 CONNECT_REQ reached the RFC-020 dial boundary through the runtime-installed CLI filter without depending on external network reachability.

**Mechanism.**

```text
test_fail_next(fp, errnum):
  if fp is unknown or errnum <= 0: errno = EINVAL; return -1
  global_failpoint = fp
  global_errno = errnum
  return 0

maybe_fail(fp):
  if global_failpoint != fp: return 0
  err = global_errno
  global_failpoint = 0
  global_errno = 0
  errno = err
  return -1

signal_timer_test_branch(loop, state):
  # Called by signal_timer before its no-signal early return in testing builds.
  if progress_fd >= 0 and not progress_reported and state.runtime != NULL and
     odin_server_runtime_test_inflight_count(state.runtime) > 0:
    write one byte to progress_fd
    progress_reported = 1
  if global_failpoint != ODIN_CLI_SERVER_TEST_TRIGGER_RUNTIME_ERROR: return
  err = global_errno
  global_failpoint = 0
  global_errno = 0
  cli_runtime_on_error(state.runtime, err, state)

odin_dial_start(loop, addr, addrlen, on_done, user_data, out):
  # Present only when ODIN_CLI_SERVER_TESTING && ODIN_DIAL_TESTING.
  if odin_cli_server_test_maybe_probe_dial_start(addr, addrlen) != 0:
    return -1
  continue with the existing RFC-020 dial implementation

maybe_probe_dial_start(addr, addrlen):
  # Defined in cli_server_testing.c; called by dial_testing.c's odin/dial.c body.
  if probe_fd < 0: return 0
  if addr != NULL and addrlen >= sizeof(sockaddr_in):
    sin = (const sockaddr_in *)addr
    if sin.sin_family == AF_INET:
      write {AF_INET, sin.sin_addr.s_addr, sin.sin_port} to probe_fd
  err = probe_errno
  probe_fd = -1
  probe_errno = 0
  errno = err
  return -1
```

Satisfies: G1 via the bind-address capture that proves the listener helper uses `INADDR_ANY`; G2 via deterministic coverage of setup and post-setup cleanup branches that are otherwise impractical to trigger from a subprocess without destabilizing the test process; G3 via pre-process-exit liveness counters for the CLI-owned listener/runtime plus the existing event-loop liveness counters; G4 via the one-shot dial-start probe that proves public IPv4 requests pass the CLI filter and reach the RFC-020 dial boundary.

## 4. Security

- **S1.**
  - **Threat:** Server-Side Request Forgery through the peer-supplied CONNECT_REQ host and port. Without a CLI-installed policy, a peer can send a host such as `127.0.0.1`, `10.0.0.1`, or `169.254.169.254` with any decoded port; RFC-020 parses that pair into a `sockaddr_in` and would otherwise call `odin_dial_start`, causing the server process to open an outbound connection to an internal, local, metadata, or otherwise non-public destination and port.
  - **Mitigation:** §3.2.1 installs `odin_cli_default_server_dial_filter` with `odin_server_runtime_set_dial_filter` before the event loop can accept sessions, and §3.2.2 defines the filter's address deny ranges, nonzero errno result, and explicit decision to allow all ports on otherwise allowed public IPv4 addresses. RFC-021 propagates that runtime-level filter to each newly created session before the session can decode peer bytes, and RFC-020 consults the per-session filter with the full `sockaddr_in` after IPv4 parsing and before `odin_dial_start`.
  - **Enforcement:** §5 rows T4 and T5. T4 fires the trigger through live `odin-server` process fixtures: the loopback subcase asserts that a denied CONNECT_REQ receives the expected failure CONNECT_RESP while a loopback upstream listener receives no connection, and the public-IPv4 subcase asserts that an allowed CONNECT_REQ reaches the RFC-020 dial boundary through the installed runtime filter. T5 exercises every default-filter deny branch, boundary rejection branch, and public-IPv4 allow branch through the test-only mirror.

## 5. Testing Strategy

Rows T1-T4 are process-level tests that either spawn the `odin-server` artifact or fork a child that invokes `odin_cli_main` with `argv[0] == "odin-server"`, then interact with the server over real loopback sockets. T6's post-banner subcases and T7 use the same fork/deadline helpers for child-process cleanup evidence. Their fixtures install `signal(SIGPIPE, SIG_IGN)` before any write-after-close or EOF-probe step. Every child stderr pipe, progress pipe, returned-state pipe, dial-probe pipe, client socket, and upstream socket is observed with a nonblocking `poll` / `select` helper or `SO_RCVTIMEO`; the default read deadline is 2 s unless a row names a shorter probe such as T4's 200 ms upstream no-accept check. On any pipe or socket timeout, the fixture sends `SIGTERM`, follows with `SIGKILL` if the child remains alive until the wait deadline, closes or releases all fixture fds, reaps the child, and fails the assertion. `waitpid` deadlines are still used for process-exit checks, but no parent blocks in a plain `read`, `accept`, or socket receive while waiting for that deadline to fire. Test-side threads never call `odin_event_loop_stop`; shutdown is requested only by sending `SIGINT` / `SIGTERM` to the child process or through owner-thread test hooks inside the child.

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | Ephemeral server startup reports actual port and becomes reachable | Build `odin_cli_artifacts`; spawn `out/cli_server_mac/odin-server --listen 0` with stderr captured; deadline-read one stderr line; parse the decimal port; connect a TCP client to `127.0.0.1:<port>` with the socket deadline; then send `SIGTERM` and wait | Stderr first line matches `odin: mode=server listen=<P>\n` with `P > 0`; TCP `connect` to `127.0.0.1:P` succeeds; child exits normally with code `0`; no stdout bytes are emitted | G1, G3 | e2e |
| T2 | SIGINT graceful shutdown | Spawn `out/cli_server_mac/odin-server --listen 0`, deadline-read the startup line, assert before sending the signal that `waitpid(pid, &status, WNOHANG) == 0` and a TCP `connect` to the reported port succeeds with the socket deadline, then send `SIGINT` and wait with a 2 s deadline | The pre-signal liveness and `connect` assertions pass, proving the signal is delivered while the CLI server is running; child exits via `WIFEXITED` with status `0`, not via signal; stderr contains exactly the startup line and no startup-failure line; a post-exit `connect` to the reported port fails with `ECONNREFUSED` or `ETIMEDOUT` within the fixture deadline as an end-to-end sanity check, while pre-process-exit listener cleanup is proven by T7 | G3 | e2e |
| T3 | Bind collision returns `1` and does not disturb the existing listener | Parent opens a nonblocking `0.0.0.0:0` listener, records port `P`, then spawns `out/cli_server_mac/odin-server --listen P` with stderr captured | Child exits `1`; stderr is exactly `odin: server startup failed at bind\n`; the parent-owned `INADDR_ANY` listener still accepts a new connection to `127.0.0.1:P` after the child exits; no extra listener remains from the child | G2 | e2e |
| T4 | Default CLI filter denies loopback and allows public IPv4 to the dial boundary | Deny subcase: parent opens a nonblocking upstream listener on `127.0.0.1:U`; spawn `odin-server --listen 0`; deadline-read the startup line; connect a raw client to the reported port; send a CONNECT_REQ for host `127.0.0.1`, port `U`; deadline-read CONNECT_RESP from the client socket; poll the upstream listener for 200 ms. Allow subcase: fork a child in `odin_unittests`, call `odin_cli_server_test_set_dial_start_probe_fd(probe_write_fd, ETIMEDOUT)`, call `odin_cli_main` as `odin-server --listen 0`, deadline-read the startup line, connect a raw client to the reported port, send a CONNECT_REQ for public host `93.184.216.34`, port `443`, deadline-read one probe record, and deadline-read CONNECT_RESP from the client socket | Deny subcase: client reads CONNECT_RESP bytes `{0x01, 0x02, 0x00, 0x04}` (`EACCES` maps to OTHER in RFC-020); upstream listener accepts zero connections; server remains alive until the test sends `SIGTERM`, then exits `0`. Allow subcase: parent reads one `odin_cli_server_test_dial_start_t` record with `family == AF_INET`, `ipv4_addr_nbo == inet_addr("93.184.216.34")`, and `port_nbo == htons(443)`; client reads CONNECT_RESP bytes `{0x01, 0x02, 0x00, 0x03}` from the synthetic `ETIMEDOUT` dial-start failure; no outbound socket is created by the probe; child exits `0` after `SIGTERM` | G1, G4, S1 | e2e |
| T5 | Default filter deny and allow matrix | In `odin_unittests`, call `odin_cli_server_test_default_dial_filter` with `sockaddr_in` values for denied CIDR lower and upper boundaries plus representatives: `0.0.0.0`, `0.255.255.255`, `10.0.0.0`, `10.1.2.3`, `10.255.255.255`, `100.64.0.0`, `100.127.255.255`, `127.0.0.0`, `127.0.0.1`, `127.255.255.255`, `169.254.0.0`, `169.254.1.1`, `169.254.169.254`, `169.254.255.255`, `172.16.0.0`, `172.31.255.255`, `192.0.0.0`, `192.0.0.1`, `192.0.0.255`, `192.0.2.0`, `192.0.2.1`, `192.0.2.255`, `192.168.0.0`, `192.168.0.1`, `192.168.255.255`, `198.18.0.0`, `198.19.255.255`, `198.51.100.0`, `198.51.100.1`, `198.51.100.255`, `203.0.113.0`, `203.0.113.1`, `203.0.113.255`, `224.0.0.0`, `224.0.0.1`, `239.255.255.255`, `240.0.0.0`, `240.0.0.1`, and `255.255.255.255`; allowed adjacent/public cases `1.0.0.0`, `9.255.255.255`, `11.0.0.0`, `100.63.255.255`, `100.128.0.0`, `126.255.255.255`, `128.0.0.0`, `169.253.255.255`, `169.255.0.0`, `172.15.255.255`, `172.32.0.0`, `191.255.255.255`, `192.0.1.0`, `192.0.1.255`, `192.0.3.0`, `192.167.255.255`, `192.169.0.0`, `198.17.255.255`, `198.20.0.0`, `198.51.99.255`, `198.51.101.0`, `203.0.112.255`, `203.0.114.0`, `223.255.255.255`, `8.8.8.8`, and `93.184.216.34`; also call it with `addr == NULL`, with a valid IPv4 `sockaddr_in` but `addrlen == sizeof(sockaddr_in) - 1`, and with an `AF_INET6` sockaddr | Every denied IPv4 case returns `EACCES`; the `NULL`, short-`addrlen`, and `AF_INET6` cases return `EAFNOSUPPORT`; every allowed adjacent/public IPv4 case returns `0` | G4, S1 | unit |
| T6 | Setup failure cleanup matrix | First snapshot CLI and event-loop liveness, then assert `odin_cli_server_test_fail_next((odin_cli_server_test_failpoint_t)0, EIO) == -1` with `errno == EINVAL` and `odin_cli_server_test_fail_next(ODIN_CLI_SERVER_TEST_FAIL_SOCKET, 0) == -1` with `errno == EINVAL`. For each valid failpoint in §3.2.4 from `SOCKET` through `SIGNAL_TIMER_START`, plus `EVENT_LOOP_RUN` and `TRIGGER_RUNTIME_ERROR`, call `odin_cli_server_test_reset_liveness`, call `odin_event_loop_test_reset_liveness`, run `odin_cli_server_test_fail_next(fp, EIO)`, call `odin_cli_main` as `odin-server --listen P` where `P` was selected by briefly binding `0.0.0.0:0` and closing the fixture listener, and capture `err` through `fmemopen` when the failpoint returns before the blocking loop; for post-banner failpoints use the fork deadline fixture, deadline-read the child's liveness snapshot immediately after `odin_cli_main` returns, then block the child on a release pipe before exiting | The two invalid `odin_cli_server_test_fail_next` calls return `-1` with `errno == EINVAL`, do not arm a failpoint, and leave the pre-snapshot CLI and event-loop liveness counters unchanged. For each valid failpoint, return code is `1`; stderr is the exact line from §3.2.1 for that failpoint, except post-banner `EVENT_LOOP_RUN` / `TRIGGER_RUNTIME_ERROR` also include the startup line before their runtime-failure line; after each in-process subcase a fresh `connect(127.0.0.1:P)` fails because the CLI closed the partially created listener; for each forked post-banner subcase, the parent observes `waitpid(pid, &status, WNOHANG) == 0` after the child writes its return snapshot and a fresh `connect(127.0.0.1:P)` fails while the child is still paused, proving listener closure before process teardown; `odin_cli_server_test_liveness` reports `live_listeners == 0` and `live_runtimes == 0`; `odin_event_loop_test_liveness` reports `loops == 0`, `io_handles == 0`, `timers == 0`, and `task_nodes == 0`; for `LISTEN` and later failpoints, `odin_cli_server_test_last_bind_addr` reports `sin_addr.s_addr == htonl(INADDR_ANY)` and `ntohs(sin_port) == P`; repeated subcases leave the test process signal handlers restored | G1, G2, G3 | unit |
| T7 | Graceful shutdown cleans up before child exit and restores handlers | Fork a child that installs custom counting handlers for `SIGINT` and `SIGTERM`, resets CLI and event-loop liveness counters, calls `odin_cli_server_test_set_progress_fd(progress_write_fd)`, then calls `odin_cli_main` directly as `odin-server --listen 0`; parent deadline-reads the startup line, connects one raw idle TCP client to the reported port, deadline-reads the progress byte proving `odin_server_runtime_test_inflight_count(state.runtime) > 0`, asserts via nonblocking poll that the returned pipe has no byte and `waitpid(pid, &status, WNOHANG) == 0`, then sends `SIGTERM`; immediately after `odin_cli_main` returns, the child snapshots `odin_cli_server_test_liveness` and `odin_event_loop_test_liveness`, raises `SIGINT` and `SIGTERM` once each, writes the return code, handler counters, and liveness snapshots to a parent-visible pipe, then blocks on a release pipe before exiting | The pre-signal returned-pipe and `waitpid` assertions prove the signal was delivered while the child was still inside `odin_cli_main` with one accepted idle session; after the return snapshot is deadline-read, `waitpid(pid, &status, WNOHANG) == 0`, proving the child has not exited yet; a new `connect(127.0.0.1:P)` fails while the child is paused, proving the listener fd closed before process teardown; the idle client observes EOF or reset within the socket-read deadline while the child is paused; return code is `0`; both custom counters increment exactly once after `odin_cli_main` returns; CLI liveness reports `live_listeners == 0`, `live_runtimes == 0`, and `last_cleanup_runtime_inflight >= 1`; event-loop liveness reports `loops == 0`, `io_handles == 0`, `timers == 0`, and `task_nodes == 0`; no startup-failure line is printed | G3 | integration |

## 6. Implementation Plan

- **P1. Land red-gated CLI server tests and the internal server-runner seam.**
  - **Scope:** add internal `odin/cli_server.h`, `odin/cli_server.c`, `odin/cli_server_internal_test.h`, and `odin/cli_server_testing.c`. Move only the current server-OK banner behavior behind `odin_cli_run_server` so `odin/cli.c` calls the helper for `ODIN_CLI_OK` / server mode; the P1 helper remains banner-only and returns `0`, preserving the current implementation while creating the seam P2 replaces. Add `ODIN_CLI_SERVER_TESTING` hooks from §3.2.4 as no-op/stubbed hooks that compile and can be called by the red tests, including an `odin_cli_server_test_maybe_probe_dial_start` stub that returns `0` when no probe is armed. Under `#if defined(ODIN_DIAL_TESTING) && defined(ODIN_CLI_SERVER_TESTING)`, make `odin/dial.c` include `odin/cli_server_internal_test.h` and call `odin_cli_server_test_maybe_probe_dial_start(addr, addrlen)` before `socket(2)`. Adjust `odin/BUILD.gn` so production `:odin` links `cli_server.c` through a new `:odin_cli_server` source set, while `odin_unittests` links `cli_server_testing.c` instead of the production helper and sees `cli_server_internal_test.h`. Add `config("odin_cli_server_testing_config") { defines = [ "ODIN_CLI_SERVER_TESTING" ] }` and apply it to `odin_unittests` target-wide alongside `:odin_dial_testing_config`, `:odin_event_loop_testing_config`, and `:odin_server_runtime_testing_config`, so `cli_server_testing.c`, CLI server tests, and `dial_testing.c` share the gated declarations and `dial_testing.c` compiles `odin/dial.c` with both probe-related defines. Update existing in-process CLI mapping tests so no OK `odin-server` invocation expects an immediate `fmemopen` return: in `OdinCliTest.T8MainByteExactMapping`, remove the `// OK SERVER` row `{{"odin-server", "-l", "4433"}, "", "odin: mode=server listen=4433\n", 0}`; in `OdinCliListenPortTest.T7MainBannerPrintsParsedPort`, remove the two server-OK rows `{{"odin-server", "-l", "4433"}, "odin: mode=server listen=4433\n", 0}` and `{{"odin-server"}, "odin: mode=server listen=4433\n", 0}` from the `odin_cli_main` / `fmemopen` table, with server default-port coverage left to parser-only tests and T1's process-level startup check. Client OK, help, parse errors, and symlink dispatch stay in the default suite. Add T1-T7 as `OdinCliServerProcessTest.*` / `OdinCliServerUnitTest.*`, each skipped unless `ODIN_CLI_SERVER_RED=1` is set. Build the CLI server test target with `ODIN_CLI_SERVER_TESTING`, `ODIN_EVENT_LOOP_TESTING`, `ODIN_SERVER_RUNTIME_TESTING`, and `ODIN_DIAL_TESTING` so T4/T6/T7 can use the dial-start probe, existing event-loop liveness counters, and runtime inflight-count hook named in §3.2.4.
  - **Depends on:** None.
  - **Done when:** `./tool/gn gen out/cli_server_mac --args='target_os="mac"'` and `./tool/gn gen out/cli_server_linux_x64 --args='target_os="linux" target_cpu="x64"'` resolve, and `./tool/ninja -C out/cli_server_mac odin_main odin_unittests tests` plus `./tool/ninja -C out/cli_server_linux_x64 odin_main odin_unittests tests` build without error. The red-verification command `ODIN_CLI_SERVER_RED=1 out/cli_server_mac/odin_unittests --gtest_filter='OdinCliServerProcessTest.*:OdinCliServerUnitTest.*'` executes T1-T7 and fails them against the banner-only stub: T1 because the child exits immediately after printing `listen=0` or the requested port instead of staying alive; T2 because the pre-signal liveness or pre-signal `connect` assertion fails before `SIGINT` can be delivered to a running server; T3 because a bind collision against the parent's `INADDR_ANY` listener still returns `0`; T4 because no listener exists to accept either raw CONNECT_REQ and the public-allow subcase never observes a dial-start probe record; T5 because the stub test mirror returns `0` for denied ranges, missing CIDR edges, boundary rejections, and `255.255.255.255`; T6 because invalid failpoint inputs do not report `EINVAL`, valid failpoints are not consumed, `odin_cli_server_test_last_bind_addr` never records `INADDR_ANY`, and cleanup counters do not observe a real listener/runtime lifecycle; T7 because the accepted-session progress byte is never written, the returned-pipe marker fires, or `waitpid(..., WNOHANG)` reports exit before the parent sends `SIGTERM`. The default `out/cli_server_mac/odin_unittests --gtest_brief=1` reports T1-T7 skipped and exits zero with all pre-existing Odin suites green, including client OK, help, parse errors, client symlink dispatch, and the existing module suites. The Linux x64 binary is cross-compiled but not executed in this environment.

- **P2. Implement the server runner, default filter, cleanup, and signal stop.**
  - **Scope:** replace the P1 banner-only `odin_cli_run_server` helper with the §3.2.1 listener/event-loop/runtime implementation: socket setup with `SO_REUSEADDR`, `O_NONBLOCK`, `0.0.0.0:<listen_port>`, `listen(SOMAXCONN)`, `getsockname` actual-port discovery, `odin_event_loop_create`, `odin_server_runtime_create`, `odin_server_runtime_set_dial_filter`, deterministic startup/runtime failure printing, and ordered cleanup. Implement the §3.2.2 default deny filter and the test mirror. Implement §3.2.3 signal handling with saved `sigaction` records, async-signal-safe handler, recurring owner-thread timer, stop request, and handler restoration. Implement the §3.2.4 failpoints, bind-address capture, liveness counters, accepted-session progress fd, and `odin_cli_server_test_maybe_probe_dial_start` one-shot behavior so every startup, post-setup failure, public-allow dial-boundary, and graceful-cleanup path can be red/green verified without destabilizing the process. Remove the `ODIN_CLI_SERVER_RED` skip gates so T1-T7 assert in the default host test run. Do not add client runtime behavior, XQUIC connection setup, authentication, daemonization, config files, or new public CLI flags.
  - **Depends on:** P1.
  - **Done when:** the P1 build commands still succeed; `out/cli_server_mac/odin_unittests --gtest_filter='OdinCliServerProcessTest.*:OdinCliServerUnitTest.*'` passes T1-T7 un-gated on the host macOS architecture. The unfiltered host run `out/cli_server_mac/odin_unittests --gtest_brief=1` exits zero with existing help, parse-error, client OK, symlink dispatch, and module suites still green. Host-runnable enumeration: T1-T7 all run only in `out/cli_server_mac/odin_unittests`; T1-T3 and T4's loopback-deny subcase spawn the host-runnable `out/cli_server_mac/odin-server` symlink produced by `odin_cli_artifacts`; T4's public-allow subcase forks the test binary and calls `odin_cli_main` as `odin-server` so the one-shot dial-start probe can make the RFC-020 dial boundary deterministic without external network access. Cross-compile-only enumeration: `out/cli_server_linux_x64/odin_unittests` and `out/cli_server_linux_x64/odin` are built but not executed; their Linux event-loop timerfd branch used by §3.2.3 is compiled and code-reviewed, not runtime-verified in this RFC. T6 and T7 provide deterministic cleanup evidence: `live_listeners == 0`, `live_runtimes == 0`, event-loop liveness counters are all zero, T6's forked post-banner failure child and T7's graceful-shutdown child both refuse connections on the reported port while paused before process exit, and T7's accepted idle client observes EOF or reset while `last_cleanup_runtime_inflight >= 1`. The ASan build commands `./tool/gn gen out/cli_server_mac_asan --args='target_os="mac" is_asan=true'`, `./tool/ninja -C out/cli_server_mac_asan odin_unittests`, and `out/cli_server_mac_asan/odin_unittests --gtest_filter='OdinCliServerProcessTest.*:OdinCliServerUnitTest.*'` exit without AddressSanitizer reports as memory-safety backing for those cleanup paths, not as fd-closure proof. Production `out/cli_server_mac/odin` and `out/cli_server_linux_x64/odin` contain no `odin_cli_server_test_*` symbols, and `./tidy_odin.sh` exits clean over the touched Odin files.
