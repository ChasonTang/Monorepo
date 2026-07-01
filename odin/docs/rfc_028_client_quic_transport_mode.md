# RFC-028: Client QUIC Transport Mode

## Revision Notes (delete before merge)

### Cycle 1 — 2026-06-30

Addressed Round 1 Major findings 1-7 and the advisory S1/T5 mismatch by revising §3.2.2-§3.2.5, §4, §5, and §6.
Added self-contained client-runner include surface, force-destroy callback-order pseudocode, expanded startup/config coverage rows, valid server parser subcase, and explicit scope-check phase gates.

### Cycle 2 — 2026-06-30

Addressed Round 2 Major finding 1 at §5 T13 and §6 P1/P4.
Added executable NULL-receiver coverage for `odin_xqc_client_runtime_force_destroy(NULL)` and mapped it in the §5 coverage matrix.

### Cycle 3 — 2026-06-30

Addressed Round 3 Major findings 1-5 at §3.2.4, §3.2.5, §5 T3/T8/T10/T13, and §6 P3/P4.
Added copied XQC config host bytes, runtime-free test records, macOS post-accept fcntl subcases, the UDP engine-destroy hook contract, and event-loop liveness assertions through `odin_event_loop_test_liveness`.

### Cycle 4 — 2026-06-30

Addressed Round 4 Major finding 1 at §3.2.5 and §6 P1/P4.
Moved runtime-free test declaration ownership into P1 so the force-destroy record surface compiles before P4 wires `runtime_finish_destroy_once`.

### Cycle 5 — 2026-06-30

Addressed Round 5 Major finding 1 at §3.2.4, §3.2.5, §5 T13, and §6 P1/P3/P4.
Added a test-only `force_destroy_null_calls` observable so T13 fails red until the NULL receiver entry point records the no-op, while teardown records remain unchanged.

### Cycle 6 — 2026-06-30

Addressed Round 6 Major findings at §5 T1, §6 P3/P4, and §3.2.5 force-destroy phase references.
Expanded T1 to cover `odin_cli_main` invalid client transports and moved T10/T13 plus force-destroy safety into P3; accepted the §5.0 post-syscall matrix minor.

### Cycle 7 — 2026-06-30

Addressed Round 7 Major findings at §3.2.5, §5 T14/T15, §5.0, and §6 P1-P3.
Added parser precedence/TLS-flag rejection coverage, switch-based client failpoint validation, and a gap-value test for enum value 99.

### Cycle 8 — 2026-07-01

Addressed Round 8 Major finding 1 at §5 T14/T16, §5.0, and §6 P1-P3.
Added T16 for unknown client flag precedence over invalid transport without expanding T14 beyond eight named subcases.
Addressed Round 8 Major finding 2 at §5 T15, §5.0, and §6 P1/P3 by adding NULL-output pending-failpoint assertions.

### Cycle 9 — 2026-07-01

Addressed Round 9 Major finding 1 at §3.2.2, §3.2.5, §5 T3, and §6 P1/P3 by moving default XQUIC config construction behind `odin_xqc_client_runtime_create_default`.
Addressed Round 9 Major finding 2 at §3.2.5, §4 S1, §5 T3, and §6 P1/P3 by adding a post-`getsockname` listener-address test hook while preserving the existing pre-bind hook.

### Cycle 10 — 2026-07-01

Addressed Round 10 Major finding 1 at §3.2.2, §5.0, §5 T17/T18, and §6 P1/P3.
Added direct runtime default-create invalid-input coverage for null config/output/address fields plus address length/family validation, with no runtime/UDP side effects.

### Cycle 11 — 2026-07-01

Addressed Round 11 Major finding 1 at §5.0, §5 T18, and §6 P1/P3.
Expanded T18 from invalid-only address-shape coverage to include valid AF_INET6 local and peer default-create success cases with default-record, cleanup, and zero-leak assertions.

## 1. Summary

Add `--transport tcp|quic` to `odin-client`, keeping TCP as the default path while explicit QUIC mode starts `odin_xqc_client_runtime` and hands each accepted local HTTPS_PROXY TCP fd to that runtime.

## 2. Goals

- **G1.** Every successful `odin-client` invocation that omits `--transport` or selects `--transport tcp` preserves the current TCP runtime behavior, TCP success banner, signal cleanup, and accepted-connection session path.

- **G2.** The client parser accepts exactly `--transport tcp` and `--transport quic`, defaults to TCP when omitted, reports invalid transport values deterministically, and leaves server-mode transport parsing unchanged.

- **G3.** Every successful `odin-client --transport quic` invocation binds the local loopback nonblocking TCP listener, creates and starts one `odin_xqc_client_runtime` toward the configured IPv4 Odin server endpoint, prints a QUIC-specific ready banner after all startup resources are live, and remains in the event loop until a signal or runtime failure stops it.

- **G4.** For every accepted local connection in QUIC mode, the runner calls `odin_xqc_client_runtime_add_connection(rt, conn_fd)` exactly once; a successful call transfers fd ownership to the runtime, and a failed call leaves ownership with the runner, which closes that fd silently and keeps accepting later connections.

- **G5.** QUIC startup failure, accept-loop failure, event-loop failure, signal shutdown, ownership transfer, and cleanup are deterministic: the process emits at most one documented failure line for each failed run, releases every CLI-owned listener, accept loop, event loop, signal timer, signal handler replacement, and QUIC runtime resource it created, and returns the documented exit code.

## 3. Design

### 3.1 Overview

`odin_cli_main` remains the single public CLI entry point. The client parser gains a client transport field and one exact long option, `--transport`. TCP remains the default and keeps the current `odin/cli_client.c` path. QUIC mode reuses the same local loopback TCP listener, accept loop, signal timer, and event-loop ownership shape as the TCP runner, but replaces per-connection `odin_client_session_create` calls with `odin_xqc_client_runtime_add_connection`.

`odin/cli_client.c` becomes the transport-selected client runner. It validates the configured Odin server as an IPv4 literal before opening local resources, because the current TCP path requires an IPv4 `sockaddr_in` and the QUIC CLI path needs the same concrete peer address for `odin_xqc_client_runtime_default_config_t.peer_addr`. This RFC adds no DNS lookup, `/etc/hosts` lookup, IPv6 CLI dialing, client certificate flags, or no-crypto flag.

```
odin-client argv
    |
    v
odin_cli_parse -> client_transport = TCP (default) or QUIC
    |
    |-- TCP  -> current RFC-024 runner and banner
    |
    '-- QUIC -> loopback TCP listener
             -> odin_event_loop_create
             -> odin_accept_loop_create
             -> odin_xqc_client_runtime_create_default/start
             -> signal handlers + polling timer
             -> "odin: mode=client transport=quic listen=<port> server=<host>:<port>\n"
             -> event loop
                  accept(conn_fd) -> odin_xqc_client_runtime_add_connection(rt, conn_fd)
             -> cleanup
```

### 3.2 Detailed Design

#### 3.2.1 Client Parser Transport Contract

Extend the parser surface in `odin/cli.h`:

```c
typedef enum odin_cli_client_transport_t {
  ODIN_CLI_CLIENT_TRANSPORT_TCP = 0,
  ODIN_CLI_CLIENT_TRANSPORT_QUIC,
} odin_cli_client_transport_t;

typedef struct odin_cli_args_t {
  odin_cli_mode_t mode;
  uint16_t listen_port;
  const char *server_host;
  size_t server_host_len;
  uint16_t server_port;
  odin_cli_client_transport_t client_transport;
  odin_cli_server_transport_t server_transport;
  const char *quic_cert_file;
  const char *quic_key_file;
} odin_cli_args_t;
```

Client long options become:

```c
static const struct option kClientLong[] = {
    {"listen", required_argument, NULL, 'l'},
    {"server", required_argument, NULL, 's'},
    {"transport", required_argument, NULL, 1000},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
};
```

Client usage and invalid transport mapping become:

```text
usage: odin-client --listen ADDR --server ADDR [--transport tcp|quic]

ODIN_CLI_ERR_BAD_TRANSPORT, mode == CLIENT:
  err = "odin: invalid --transport\n<U_C>\n"
  return 2
```

**Unstated contract.** `client_transport` is zeroed to `ODIN_CLI_CLIENT_TRANSPORT_TCP` before parsing, so every existing Client OK invocation that omits `--transport` remains TCP. `--transport tcp` is explicit TCP and has the same runtime behavior and success banner as omission. `--transport quic` is the only selector that enters the QUIC runner. Values are case-sensitive; any value other than `tcp` or `quic` returns `ODIN_CLI_ERR_BAD_TRANSPORT`. The option belongs to both client and server modes, but the client field and server field are independent: client parsing never sets `server_transport`, and server parsing never sets `client_transport`. Existing precedence remains: HELP, unknown flag, bad listen port, bad server, missing required, bad transport, OK. Exact long-option spelling remains required, so abbreviated forms such as `--trans` still return `ODIN_CLI_ERR_UNKNOWN_FLAG`.

`--quic-cert` and `--quic-key` remain server-only flags. A client invocation that supplies either still returns `ODIN_CLI_ERR_UNKNOWN_FLAG`; this RFC does not add client TLS-path CLI options.

**Mechanism.**

```text
parse(argc, argv, out):
  zero out, including client_transport = TCP and server_transport = TCP
  select mode by basename as today
  if mode == CLIENT:
    parse --listen, --server, --transport, --help
  if transport_arg exists:
    if transport_arg == "tcp": parsed_client_transport = TCP
    else if transport_arg == "quic": parsed_client_transport = QUIC
    else: bad_transport = 1
  preserve existing status precedence through missing-required
  if bad_transport: return ODIN_CLI_ERR_BAD_TRANSPORT
  on OK, fill out.client_transport for client mode

main(argc, argv, out, err):
  if OK/CLIENT:
    config = {listen_port, server_host, server_host_len, server_port,
              client_transport}
    fflush(out)
    return odin_cli_run_client(&config, err)
  server and non-OK mappings remain otherwise unchanged
```

Satisfies: G1 via the TCP default value; G2 via exact client `--transport` parsing, usage, and error mapping.

#### 3.2.2 Transport-Selected Client Startup and QUIC Runtime Config

Replace the internal client runner signature with a config struct:

```c
/* odin/cli_client.h */
#include "odin/cli.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct odin_cli_client_config_t {
  uint16_t listen_port;
  const char *server_host;
  size_t server_host_len;
  uint16_t server_port;
  odin_cli_client_transport_t transport;
} odin_cli_client_config_t;

int odin_cli_run_client(const odin_cli_client_config_t *config, FILE *err);
```

QUIC client creation uses an Odin-owned default helper, not raw XQUIC config assembly in the CLI:

```c
/* odin/client_xqc_runtime.h */
typedef struct odin_xqc_client_runtime_default_config_t {
  odin_event_loop_t *loop;
  const struct sockaddr *local_addr;
  socklen_t local_addrlen;
  const struct sockaddr *peer_addr;
  socklen_t peer_addrlen;
  const char *server_host;
} odin_xqc_client_runtime_default_config_t;

int odin_xqc_client_runtime_create_default(
    const odin_xqc_client_runtime_default_config_t *config,
    odin_xqc_client_runtime_t **out);
```

The QUIC branch consumes the runtime default-create/start/add API and the deterministic cleanup API pinned in §3.2.4.

QUIC success banner:

```text
odin: mode=client transport=quic listen=<actual_port> server=<server_host>:<server_port>
```

QUIC startup failure steps preserve the existing TCP listener/event-loop strings and add the XQC runtime strings:

```text
odin: client startup failed at config
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
odin: client startup failed at xqc_client_runtime_create
odin: client startup failed at xqc_client_runtime_start
odin: client startup failed at sigaction(SIGINT)
odin: client startup failed at sigaction(SIGTERM)
odin: client startup failed at signal_timer_start
```

**Unstated contract.** `config` and `err` are non-null internal preconditions. `config == NULL`, `err == NULL`, or an unknown transport value returns `1` after writing `odin: client startup failed at config\n` to `err` when `err` is non-null. TCP mode delegates to the current RFC-024 body and preserves the exact TCP banner `odin: mode=client listen=<actual_port> server=<server_host>:<server_port>\n`; it does not create or include `odin_xqc_client_runtime`.

Both transports validate `server_host[0..server_host_len)` as a non-empty IPv4 presentation literal before listener creation. Parsed hostnames and bracketed IPv6 values still fail with `odin: client startup failed at server_endpoint\n` before a success banner. QUIC mode builds `peer_addr` as `sockaddr_in{AF_INET, parsed_server_addr, htons(server_port)}` and passes a NUL-terminated copy of the same host slice as `odin_xqc_client_runtime_default_config_t.server_host`. The QUIC UDP local address passed to the helper is `sockaddr_in{AF_INET, INADDR_ANY, 0}`, so the kernel chooses the client UDP source port.

`odin_cli_client` never constructs `odin_xqc_client_runtime_config_t`, never includes `<xquic/xquic.h>`, and never names raw `xqc_*` or `XQC_*` tokens. `odin_xqc_client_runtime_create_default` lives inside `odin_client_xqc_runtime`; it validates the Odin-level default config, creates zeroed/default raw XQUIC values locally (`xqc_engine_ssl_config_t`, `xqc_engine_callback_t`, `xqc_conn_settings_t`, and `xqc_conn_ssl_config_t`), sets `engine_config = NULL`, `transport_callbacks = NULL`, `token = NULL`, `token_len = 0`, and `no_crypto_flag = 0`, then calls the existing `odin_xqc_client_runtime_create` with non-null pointers for the raw config fields it requires. Before constructing those raw values or calling the lower factory, the helper rejects `config == NULL`, `out == NULL`, `loop == NULL`, `local_addr == NULL`, `peer_addr == NULL`, `server_host == NULL`, local or peer `sa_family` values other than `AF_INET`/`AF_INET6`, and local or peer lengths that do not exactly match `sizeof(struct sockaddr_in)` or `sizeof(struct sockaddr_in6)` for the address family; every such rejection returns `-1` with `errno == EINVAL`, leaves `*out` unchanged when `out` is non-null, records no default-create success, and calls no runtime or UDP creation path. This uses the runtime's existing defaulting for client cache callbacks and does not add certificate verification policy; the helper leaves `conn_ssl_config.cert_verify_flag` unset unless a later RFC adds client trust configuration.

The QUIC success banner is printed only after server endpoint validation, local TCP listener bind/listen/getsockname, event-loop creation, accept-loop creation, `odin_xqc_client_runtime_create_default`, `odin_xqc_client_runtime_start`, signal-handler installation, and signal-timer start all succeed. Runtime start does not wait for QUIC handshake completion; the runtime owns pre-handshake queuing through `odin_xqc_client_runtime_add_connection`.

The CLI uses `odin_xqc_client_runtime_force_destroy` from §3.2.4 in every QUIC cleanup path, so cleanup is deterministic even if `odin_xqc_client_runtime_stop` would fail or the runtime still has a live connection.

The local TCP listener and signal timer still rely on platform-specific implementations below the abstract APIs: macOS host runs kqueue plus `accept(2)`/`fcntl`, while Linux compiles epoll/timerfd plus `accept4`. This RFC's runtime tests exercise the host macOS backend; Linux, alternate macOS arch, iOS simulator, and iOS device binaries compile these branches but are not executed in this environment.

**Mechanism.**

```text
run_client(config, err):
  if invalid config: return startup_fail("config")
  if config.transport == TCP:
    return run_tcp_client(config, err)  # current RFC-024 body
  if config.transport != QUIC:
    return startup_fail("config")
  return run_quic_client(config, err)

odin_xqc_client_runtime_create_default(config, out):
  if config/out/loop/local_addr/peer_addr/server_host invalid:
    errno = EINVAL
    return -1
  if local_addr or peer_addr family/length invalid:
    errno = EINVAL
    return -1
  engine_ssl_config = zeroed xqc_engine_ssl_config_t
  engine_callbacks = zeroed xqc_engine_callback_t
  conn_settings = zeroed xqc_conn_settings_t
  conn_ssl_config = zeroed xqc_conn_ssl_config_t
  full_config = {config fields, engine_config = NULL,
                 engine_ssl_config = &engine_ssl_config,
                 engine_callbacks = &engine_callbacks,
                 transport_callbacks = NULL,
                 conn_settings = &conn_settings,
                 conn_ssl_config = &conn_ssl_config,
                 token = NULL, token_len = 0, no_crypto_flag = 0}
  under ODIN_XQC_CLIENT_RUNTIME_TESTING, record last_default_create
  return odin_xqc_client_runtime_create(&full_config, out)

run_quic_client(config, err):
  state = zeroed; listen_fd = -1; test_wakeup_fd = -1
  if validate_ipv4_server_endpoint(config.server_host, len) != 0:
    return startup_fail("server_endpoint")
  create nonblocking 127.0.0.1:<listen_port> TCP listener as TCP does
  actual_port = getsockname(listener)
  create event loop
  create accept loop on listener with quic_on_accept/quic_on_accept_loop_error
  peer = sockaddr_in(AF_INET, parsed_server_ipv4, config.server_port)
  local_udp = sockaddr_in(AF_INET, INADDR_ANY, 0)
  default_runtime_config = {loop, local_udp, peer, copied server_host}
  if odin_xqc_client_runtime_create_default(&default_runtime_config,
                                            &state.quic_rt) != 0:
    return startup_fail("xqc_client_runtime_create")
  if odin_xqc_client_runtime_start(state.quic_rt) != 0:
    return startup_fail("xqc_client_runtime_start")
  install signal handlers and signal poll timer
  print QUIC success banner and fflush(err)
  run_rc = odin_event_loop_run(state.loop)
  classify accept_loop_error_seen, run_rc, and shutdown_requested as §3.2.3
  quic_cleanup_all(state)
```

Satisfies: G1 via the unchanged TCP branch; G3 via the QUIC listener, runtime config, runtime start, and ready banner; G5 via full startup-step classification and force-destroy availability for deterministic cleanup.

#### 3.2.3 Accepted Fd Handoff, Runtime Failure, Signal Stop, and Cleanup

The QUIC accept callback consumes the existing accept-loop contract and `odin_xqc_client_runtime_add_connection`:

```c
/* odin/accept_loop.h */
typedef void (*odin_accept_loop_accept_cb)(odin_accept_loop_t *al, int conn_fd,
                                           void *user_data);
typedef void (*odin_accept_loop_error_cb)(odin_accept_loop_t *al, int err,
                                          void *user_data);

/* odin/client_xqc_runtime.h */
int odin_xqc_client_runtime_add_connection(odin_xqc_client_runtime_t *rt,
                                           int conn_fd);
```

Runtime failure lines:

```text
odin: client runtime failed at accept_loop
odin: client runtime failed at event_loop_run
```

**Unstated contract.** `odin_accept_loop` owns neither the listening fd nor accepted fds after `on_accept` begins. In QUIC mode, `quic_on_accept` calls `odin_xqc_client_runtime_add_connection(state->quic_rt, conn_fd)` exactly once. Return `0` transfers ownership to the runtime. Return `-1` leaves ownership with the CLI; the CLI preserves no errno, closes `conn_fd`, writes no stderr, and returns so the accept loop can handle later clients. This is the QUIC analogue of the TCP branch's "client session create failed, close only this fd" behavior.

`quic_on_accept_loop_error` matches TCP: it records `accept_loop_error_seen`, stores the errno for test-only inspection, and calls `odin_event_loop_stop(state->loop)` from the owner thread. After `odin_event_loop_run` returns, the runner prints `odin: client runtime failed at accept_loop\n`, cleans up, and returns `1`.

Signal handling is shared with the TCP runner: the POSIX handler writes only a `sig_atomic_t`, and the recurring event-loop timer stops the loop from the owner thread after setting `shutdown_requested = 1`. The signal handler never calls XQUIC, event-loop, stdio, allocation, or fd APIs. Cleanup restores any replaced `SIGINT` and `SIGTERM` handlers on startup failure, runtime failure, and graceful signal exit.

QUIC cleanup order is deterministic: destroy accept loop, stop signal timer if live, force-destroy the XQC client runtime if live, destroy event loop, close test wake fd if live, close listening fd, and restore signal handlers. The accept loop is destroyed before runtime force-destroy so no new accepted fd can be handed to a runtime that is being torn down. The runtime is destroyed before the event loop because runtime sub-objects were created against that loop.

`event_loop_run` classification matches TCP: `accept_loop_error_seen` returns `1` with the accept-loop line; `run_rc != 0` or a clean stop without `shutdown_requested` returns `1` with the event-loop line; signal-driven shutdown returns `0` after cleanup.

**Mechanism.**

```text
quic_on_accept(al, conn_fd, user_data):
  state = user_data
  if odin_xqc_client_runtime_add_connection(state.quic_rt, conn_fd) != 0:
    close(conn_fd)
    return

quic_on_accept_loop_error(al, err, user_data):
  state = user_data
  state.accept_loop_error_seen = 1
  state.accept_loop_errno = err
  odin_event_loop_stop(state.loop)

quic_cleanup_all(state):
  if state.accept_loop != NULL:
    odin_accept_loop_destroy(state.accept_loop)
    state.accept_loop = NULL
  if state.signal_timer != NULL:
    odin_event_timer_stop(state.signal_timer)
    state.signal_timer = NULL
  if state.quic_rt != NULL:
    odin_xqc_client_runtime_force_destroy(state.quic_rt)
    state.quic_rt = NULL
  if state.loop != NULL:
    odin_event_loop_destroy(state.loop)
    state.loop = NULL
  close test_wakeup_fd and listen_fd if live
  restore_signal_handlers(state)
```

Satisfies: G4 via exact accepted-fd ownership transfer and failure behavior; G5 via fatal runtime classification, owner-thread signal stop, and fixed cleanup order.

#### 3.2.4 Client XQC Force Destroy

Add one owner-thread cleanup API to the client runtime:

```c
/* odin/client_xqc_runtime.h */
void odin_xqc_client_runtime_force_destroy(odin_xqc_client_runtime_t *rt);
```

**Unstated contract.** `odin_xqc_client_runtime_force_destroy(NULL)` is a no-op. For non-null runtimes it synchronously releases pending local fds, live client sessions, stream transport wrappers, the active connection/CID, ALPN registration, XQC UDP state, XQUIC engine state, copied config buffers, and the runtime object before returning. It is an owner-thread process-cleanup API; callers must not invoke it from inside an active client-runtime callback. Normal `odin_xqc_client_runtime_destroy` remains the graceful API for in-runtime callback contexts.

Force destroy is callback re-entry safe during `odin_xqc_udp_destroy`: connection callbacks that re-enter while UDP teardown is running see `force_destroy_active` and perform no second close, CID unregister, stream/session destroy, ALPN unregister, UDP destroy, or runtime free. Stream callbacks re-entering after stream transport teardown see `NULL` connection ALPN user data or fail the runtime live-transport lookup and return without touching freed session state. `runtime_finish_destroy` is a single finalizer guarded by `finish_destroy_in_progress`, so a nested callback cannot free the runtime or destroy UDP twice.

**Mechanism.**

```text
odin_xqc_client_runtime_force_destroy(rt):
  if rt == NULL:
    under ODIN_XQC_CLIENT_RUNTIME_TESTING, record force_destroy_null_calls += 1
    return
  rt.force_destroy_active = 1
  rt.destroy_pending = 1
  rt.closing = 1
  runtime_pending_fds_destroy_all(rt)       # closes queued local fds once
  runtime_destroy_all_streams(rt, close_streams = 1)
                                            # destroys client sessions and
                                            # stream transports; transport
                                            # destroy clears stream user data
  if rt.conn != NULL:
    xqc_conn_set_alp_user_data(rt.conn, NULL)
  if rt.cid_registered:
    odin_xqc_udp_unregister_conn(rt.xu, &rt.current_cid)
    rt.cid_registered = 0
  rt.conn = NULL
  rt.connect_started = 0
  rt.handshake_done = 0
  runtime_finish_destroy_once(rt)

runtime_finish_destroy_once(rt):
  if rt.finish_destroy_in_progress: return
  rt.finish_destroy_in_progress = 1
  if rt.alpn_registered and rt.xu != NULL:
    xqc_engine_unregister_alpn(odin_xqc_udp_engine(rt.xu), ODIN_XQC_CLIENT_ALPN)
    rt.alpn_registered = 0
  if rt.xu != NULL:
    odin_xqc_udp_destroy(rt.xu)             # callbacks may re-enter here
    rt.xu = NULL
  free copied server_host/token/conn_ssl_config buffers
  record ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_RUNTIME_FREE and runtime_free_calls += 1
  free rt

runtime callback entry(rt):
  if rt == NULL: return XQC_OK/no-op
  if rt.force_destroy_active: return XQC_OK/no-op
```

Satisfies: G5 via synchronous runtime cleanup, callback re-entry guards, CID/ALPN/UDP single-release ordering, and one finalizer for process cleanup paths.

#### 3.2.5 Test Hooks and Scope Check

Extend the existing client test-only header:

```c
/* odin/testing/cli_client_internal_test.h, under ODIN_CLI_CLIENT_TESTING */
#include "odin/protocol.h" /* ODIN_PROTO_HOST_MAX */
#include <netinet/in.h>
#include <sys/socket.h>

typedef enum odin_cli_client_test_failpoint_t {
  /* existing values remain stable */
  ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_CREATE = 100,
  ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_START,
  ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_ADD_CONNECTION,
} odin_cli_client_test_failpoint_t;

typedef struct odin_cli_client_test_liveness_t {
  size_t live_listeners;
  size_t live_accept_loops;
  size_t live_sessions;
  size_t last_cleanup_sessions;
  size_t live_xqc_client_runtimes;
  size_t quic_runtime_create_calls;
  size_t quic_runtime_start_calls;
  size_t quic_runtime_add_connection_calls;
  size_t quic_runtime_force_destroy_calls;
} odin_cli_client_test_liveness_t;

typedef struct odin_cli_client_test_xqc_add_record_t {
  int fd;
  int fd_is_nonblocking;
} odin_cli_client_test_xqc_add_record_t;

int odin_cli_client_test_last_xqc_add(
    odin_cli_client_test_xqc_add_record_t *out);
int odin_cli_client_test_last_bound_addr(struct sockaddr_in *out);
int odin_cli_client_test_pending_failpoint(
    odin_cli_client_test_failpoint_t *out);
```

The CLI test wrapper records the Odin-level runtime default config at the production call site without exposing raw XQUIC types:

```c
typedef struct odin_cli_client_test_runtime_config_record_t {
  const struct sockaddr *local_addr;
  socklen_t local_addrlen;
  struct sockaddr_storage local_addr_value;
  const struct sockaddr *peer_addr;
  socklen_t peer_addrlen;
  struct sockaddr_storage peer_addr_value;
  const char *server_host;
  size_t server_host_len;
  char server_host_value[ODIN_PROTO_HOST_MAX + 1];
} odin_cli_client_test_runtime_config_record_t;

int odin_cli_client_test_last_runtime_config(
    odin_cli_client_test_runtime_config_record_t *out);
```

Extend the existing client-runtime test record at the default-create and runtime-free sites:

```c
/* odin/testing/client_xqc_runtime_internal_test.h,
   under ODIN_XQC_CLIENT_RUNTIME_TESTING */
typedef struct odin_xqc_client_runtime_test_default_create_record_t {
  const xqc_config_t *engine_config;
  const xqc_engine_ssl_config_t *engine_ssl_config;
  xqc_engine_ssl_config_t engine_ssl_config_value;
  const xqc_engine_callback_t *engine_callbacks;
  xqc_engine_callback_t engine_callbacks_value;
  const xqc_transport_callbacks_t *transport_callbacks;
  const xqc_conn_settings_t *conn_settings;
  xqc_conn_settings_t conn_settings_value;
  const xqc_conn_ssl_config_t *conn_ssl_config;
  xqc_conn_ssl_config_t conn_ssl_config_value;
  const unsigned char *token;
  unsigned int token_len;
  int no_crypto_flag;
} odin_xqc_client_runtime_test_default_create_record_t;

typedef enum odin_xqc_client_runtime_test_call_kind_t {
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_CREATE = 1,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_START,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_STOP,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_DESTROY,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_REGISTER_CONN,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_REGISTER_ALPN,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_XQC_CONNECT,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_SET_ALP_USER_DATA,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_GET_CONN_ALP_USER_DATA_BY_STREAM,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CREATE_BIDI,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_RUNTIME_FREE
} odin_xqc_client_runtime_test_call_kind_t;

typedef struct odin_xqc_client_runtime_test_record_t {
  unsigned int call_count;
  unsigned int dropped_call_count;
  odin_xqc_client_runtime_test_call_t
      calls[ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CAP];
  unsigned int udp_create_calls;
  odin_xqc_client_runtime_test_udp_create_record_t last_udp_create;
  unsigned int default_create_calls;
  odin_xqc_client_runtime_test_default_create_record_t last_default_create;
  unsigned int runtime_free_calls;
  unsigned int force_destroy_null_calls;
} odin_xqc_client_runtime_test_record_t;
```

**Unstated contract.** Production builds do not declare or define these symbols. The `odin/testing:odin_unittests` target already compiles `odin/testing/cli_client_testing.c` with `ODIN_CLI_CLIENT_TESTING`; this RFC keeps that pattern and also relies on the existing target-wide `ODIN_XQC_CLIENT_RUNTIME_TESTING`, `ODIN_XQC_UDP_TESTING`, and `ODIN_EVENT_LOOP_TESTING` configs when rows need runtime or event-loop records.

The new failpoints are consumed at the named production call sites. `FAIL_XQC_CLIENT_RUNTIME_CREATE` fires immediately before `odin_xqc_client_runtime_create_default`; `FAIL_XQC_CLIENT_RUNTIME_START` fires immediately before `odin_xqc_client_runtime_start`; `FAIL_XQC_CLIENT_RUNTIME_ADD_CONNECTION` fires inside `quic_on_accept` immediately before the real add call. `odin_cli_client_test_fail_next` validates failpoints with a switch over every supported enum value, not a numeric range, before checking platform support. Invalid failpoint values, including unused gaps between the current TCP enum tail and `ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_CREATE`, or `errnum <= 0` return `-1/EINVAL` and do not arm state. `odin_cli_client_test_pending_failpoint` is test-only visibility for this contract: it returns `0` and writes the currently armed value, or `(odin_cli_client_test_failpoint_t)0` when none is armed; `out == NULL` returns `-1/EINVAL`. The add-connection failpoint leaves `conn_fd` owned by the CLI and therefore exercises the real close-and-continue branch.

Existing accept-loop trigger failpoints `ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_GETFL_ERROR` and `ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_SETFL_ERROR` are reused by the QUIC runner. They call `odin_accept_loop_test_fail_next_fcntl(state->accept_loop, F_GETFL/F_SETFL, errnum)` on macOS and return `-1/EOPNOTSUPP` on Linux, matching the current TCP client test hook because Linux uses `accept4(SOCK_NONBLOCK)` rather than post-accept `fcntl`.

`live_xqc_client_runtimes` increments only after `odin_xqc_client_runtime_create_default` succeeds and decrements immediately after `odin_xqc_client_runtime_force_destroy` returns. The create/start/add/force-destroy counters are call-site records, not replacement fakes. Event-loop, timer, I/O-handle, and task-node liveness are not mirrored in the CLI struct; rows assert `odin_event_loop_test_liveness` fields `loops`, `io_handles`, `timers`, and `task_nodes` directly.

The CLI runtime-config record copies address values and the default-create server host bytes before calling the real default-create helper. `server_host` is retained only as the observed pointer, while `server_host_value[0..server_host_len)` plus the appended NUL is the assertion surface for the IPv4 host string. Tests use those copied bytes to assert the IPv4 UDP local address, IPv4 peer address, and NUL-terminated server host without dereferencing expired stack storage. The runtime-owned `last_default_create` record captures the helper's raw XQUIC defaults before it calls `odin_xqc_client_runtime_create`; T3 asserts `engine_config == NULL`, `transport_callbacks == NULL`, `token == NULL`, `token_len == 0`, `conn_ssl_config_value.cert_verify_flag == 0`, and `no_crypto_flag == 0` there.

`odin_cli_client_test_last_bind_addr` remains the existing pre-bind hook and records the requested listener address before `bind(2)`, so `--listen 0` records `127.0.0.1:0`. `odin_cli_client_test_last_bound_addr` is the new post-`getsockname` hook and records the kernel-selected listener address only after `getsockname` succeeds, so `--listen 0` records `127.0.0.1:<actual_port>` matching the ready banner.

`runtime_finish_destroy_once` records `ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_RUNTIME_FREE` and increments `runtime_free_calls` immediately before `free(rt)`. This hook lives in the runtime test record, so direct calls such as `odin_xqc_client_runtime_force_destroy(NULL)` bypassing the CLI wrapper can still assert that no runtime object was freed.

The runtime-free enum value, `runtime_free_calls` field, `force_destroy_null_calls` field, and T10/T13 assertion surface land with the P1 red-verification test declarations. Before P3 wires `runtime_finish_destroy_once` to append the runtime-free call record and increment `runtime_free_calls`, T10 compiles and fails under `ODIN_RFC028_RED=1`; before P3 wires the NULL receiver branch in `odin_xqc_client_runtime_force_destroy` to increment `force_destroy_null_calls`, T13 compiles and fails under `ODIN_RFC028_RED=1` even if the temporary P1/P2 body simply returns for NULL.

The T10 UDP-destroy re-entry fixture uses `odin_xqc_udp_test_set_ops` from `odin/testing/xqc_udp_internal_test.h` with `odin_xqc_udp_test_ops_t.engine_destroy` set. The intercepted call site is `xqc_udp_engine_destroy_call(xu->engine)` inside `odin_xqc_udp_finish_destroy` during `odin_xqc_udp_destroy`; the fixture invokes the client-runtime callback path from that `engine_destroy` hook to prove `force_destroy_active` and `finish_destroy_in_progress` suppress second teardown actions. The `odin/testing:odin_unittests` GN target must keep `ODIN_XQC_UDP_TESTING` enabled target-wide for this row.

`check_client_xqc_runtime_scope.py` is narrowed. It forbids `<xquic/xquic.h>`, tokens that start with raw `xqc_` or `XQC_`, and `transport_xqc` references in `odin_cli_client`, and it continues to forbid local-resource syscalls in `odin_client_xqc_runtime` and the client-session factory branch. It no longer forbids Odin-owned `odin_xqc_client_runtime_*` symbols or the `:odin_client_xqc_runtime` dependency in `odin_cli_client`, because explicit QUIC mode intentionally wires the CLI runner to that runtime and no lower XQUIC surface.

**Mechanism.**

```text
valid_fail_next_fp(fp):
  switch fp:
    case FAIL_SOCKET:
    case FAIL_SETSOCKOPT_REUSEADDR:
    case FAIL_FCNTL_GETFL:
    case FAIL_FCNTL_SETFL:
    case FAIL_BIND:
    case FAIL_LISTEN:
    case FAIL_GETSOCKNAME:
    case FAIL_EVENT_LOOP_CREATE:
    case FAIL_ACCEPT_LOOP_CREATE:
    case FAIL_SIGACTION_SIGINT:
    case FAIL_SIGACTION_SIGTERM:
    case FAIL_SIGNAL_TIMER_START:
    case FAIL_EVENT_LOOP_RUN:
    case TRIGGER_ACCEPT_LOOP_ERROR:
    case TRIGGER_ACCEPT_LOOP_FCNTL_GETFL_ERROR:
    case TRIGGER_ACCEPT_LOOP_FCNTL_SETFL_ERROR:
    case FAIL_NEXT_SESSION_ENTRY_ALLOC:
    case FAIL_NEXT_CLIENT_SESSION_CREATE:
    case FAIL_XQC_CLIENT_RUNTIME_CREATE:
    case FAIL_XQC_CLIENT_RUNTIME_START:
    case FAIL_XQC_CLIENT_RUNTIME_ADD_CONNECTION:
      break
    default:
      errno = EINVAL
      return 0
  apply platform support checks for accepted-fd fcntl triggers
  return 1

test_consume_failpoint(fp):
  if armed fp matches:
    save errno, clear armed state, set errno, return -1
  return 0

quic_runtime_create_default_call(config, out):
  record copied config
  if test_consume_failpoint(FAIL_XQC_CLIENT_RUNTIME_CREATE): return -1
  rc = odin_xqc_client_runtime_create_default(config, out)
  if rc == 0: live_xqc_client_runtimes += 1
  quic_runtime_create_calls += 1
  return rc

record_bound_addr_after_getsockname(bound):
  g_last_bound_addr = bound
  g_last_bound_addr_recorded = 1

quic_runtime_start_call(rt):
  if test_consume_failpoint(FAIL_XQC_CLIENT_RUNTIME_START): return -1
  quic_runtime_start_calls += 1
  return odin_xqc_client_runtime_start(rt)

quic_runtime_add_connection_call(rt, fd):
  record fd and O_NONBLOCK status
  quic_runtime_add_connection_calls += 1
  if test_consume_failpoint(FAIL_XQC_CLIENT_RUNTIME_ADD_CONNECTION): return -1
  return odin_xqc_client_runtime_add_connection(rt, fd)

quic_runtime_force_destroy_call(rt):
  if rt != NULL: quic_runtime_force_destroy_calls += 1
  odin_xqc_client_runtime_force_destroy(rt)
  if rt != NULL and live_xqc_client_runtimes > 0:
    live_xqc_client_runtimes -= 1

pending_failpoint(out):
  if out == NULL:
    errno = EINVAL
    return -1
  *out = armed failpoint value, or 0 when none is armed
  return 0
```

Satisfies: G2 via parser test visibility; G3 via runtime-config records; G4 via add-fd records and failure failpoint; G5 via liveness counters and narrowed scope check.

## 4. Security

- **S1.**
  - **Threat:** Exposing the local HTTPS_PROXY listener on a non-loopback interface would let other hosts reach the unauthenticated client proxy and relay requests through the configured Odin server.
  - **Mitigation:** §3.2.2 binds the local TCP listener to `127.0.0.1:<listen_port>` for both TCP and QUIC modes; this RFC adds no flag, environment variable, or compile-time option that widens it to `INADDR_ANY`.
  - **Enforcement:** §5 row T3 asserts the QUIC ready runtime path uses the post-`getsockname` loopback listener address that matches the ready banner, and §5 row T5 asserts listener-startup failure subcases that reach bind setup use the pre-bind loopback TCP address.

- **S2.**
  - **Threat:** The CLI-supplied `--server` value drives the outbound Odin server destination for QUIC mode. Accepting hostnames or IPv6 without a supported resolver/address-family contract could print a ready banner for a runtime that cannot construct its `peer_addr`.
  - **Mitigation:** §3.2.2 validates `server_host` with `inet_pton(AF_INET, ...)` before listener creation and before runtime creation. Parsed non-IPv4 values fail with `odin: client startup failed at server_endpoint\n`.
  - **Enforcement:** §5 row T4 drives parsed hostname and bracketed IPv6 values through `--transport quic` and asserts failure before banner, listener, event loop, or runtime creation.

- **S3.**
  - **Threat:** A local process that can connect to the loopback HTTPS_PROXY listener controls HTTP `CONNECT host:port` bytes. A QUIC client-runner bug that locally resolves or dials those peer-supplied target bytes would let the local process drive outbound TCP attempts from the client instead of sending the target through the configured Odin server.
  - **Mitigation:** §3.2.3 does not parse, resolve, filter, or dial peer-supplied CONNECT targets; it hands the accepted fd to `odin_xqc_client_runtime_add_connection`, and RFC-027 keeps the client-session factory branch free of raw `connect`, `open`, `exec*`, and `dlopen`.
  - **Enforcement:** §5 row T6 sends a CONNECT request for a live loopback sentry through QUIC mode, asserts the accepted fd is handed to the runtime, and asserts the sentry receives zero local connections; §6 P3 also requires the `odin:odin_client_xqc_runtime_scope_check` action on host and cross-compile outputs.

## 5. Testing Strategy

Rows run against host-arch macOS binaries when they execute. Alternate macOS architecture, Linux x86_64, iOS simulator, and iOS device artifacts are cross-compile-only in this environment. Rows assert the platform-agnostic runner contract; backend-specific kqueue/epoll, timerfd, and accept4 details remain below the event-loop and accept-loop APIs.

Every process fixture that writes after possible peer close installs `signal(SIGPIPE, SIG_IGN)`. Every stderr pipe, snapshot pipe, client socket, sentry socket, and child-exit wait uses `poll` / `select` deadlines or socket timeouts; no row relies on `waitpid` while blocked in an unbounded read.

### 5.0 Coverage Matrix

| Axis | Value | Rows |
|------|-------|------|
| G# | G1 TCP default and explicit TCP preserved | T1, T2 |
| G# | G2 client parser transport contract | T1, T14, T16 |
| G# | G3 QUIC startup, runtime config, and ready banner | T3 |
| G# | G4 accepted fd ownership handoff | T6, T7 |
| G# | G5 deterministic failure, signal, and cleanup | T4, T5, T8, T9, T10, T11, T12, T13, T15, T17, T18 |
| Security | S1 loopback local listener | T3, T5 |
| Security | S2 IPv4-only configured server endpoint | T4 |
| Security | S3 no local dial of peer CONNECT target | T6 |
| State | PARSE, transport omitted | T1, T2 |
| State | PARSE, transport tcp | T1, T2 |
| State | PARSE, transport quic | T1, T3 |
| State | PARSE, invalid client transport | T1, T14 |
| State | PARSE, invalid client transport masked by higher-precedence statuses | T14, T16 |
| State | PARSE, empty transport value | T14 |
| State | PARSE, client TLS flags remain unknown | T14, T16 |
| State | PARSE, unknown client flag precedence over invalid transport | T16 |
| State | STARTUP QUIC, server endpoint validation rejects parsed non-IPv4 | T4 |
| State | STARTUP QUIC, listener/event-loop/runtime/signal setup succeeds | T3 |
| State | STARTUP QUIC, listener syscall setup fails | T5 |
| State | STARTUP QUIC, event-loop/runtime/signal setup fails after partial resources | T11 |
| State | RUNNING QUIC, accept returns conn_fd and runtime add succeeds | T6 |
| State | RUNNING QUIC, accept returns conn_fd and runtime add fails | T7 |
| State | RUNNING QUIC, accept-loop terminal error | T8 |
| State | RUNNING QUIC, accepted fd post-accept fcntl failure on macOS | T8 |
| State | RUNNING QUIC, event loop failure or unexpected clean stop | T8 |
| State | RUNNING QUIC, SIGINT/SIGTERM | T9 |
| State | CLEANUP after QUIC startup failure | T4, T5, T11, T12 |
| State | CLEANUP after QUIC runtime failure | T8 |
| State | CLEANUP after QUIC signal shutdown | T9 |
| State | CLEANUP force-destroys running XQC runtime | T10 |
| Completion mode | TCP happy single CONNECT through current runner | T2 |
| Completion mode | QUIC accepted fd succeeds before handshake completion by runtime-owned queuing | T6 |
| Completion mode | Later QUIC accepted fd succeeds after prior add failure | T7 |
| Completion mode | Staged `s.write_off` / `s.buf_used` client-session internals | N/A - owned by RFC-023/RFC-027 and not branched on by the CLI runner |
| Decoder branch | HTTP CONNECT and Odin protocol decoder variants | N/A - this runner treats accepted fds as opaque in QUIC mode |
| Benign-vs-fatal split | Benign add-connection failure closes only that accepted fd and continues | T7 |
| Benign-vs-fatal split | Fatal accept-loop or event-loop failure stops process and returns `1` | T8 |
| Constructor / factory precondition | Client transport parser validation | T1, T14 |
| Constructor / factory precondition | Client runner invalid config and unknown transport | T12 |
| Constructor / factory precondition | QUIC IPv4 server endpoint validation before resource creation | T4 |
| Constructor / factory precondition | Runtime default-create null config/output/address inputs | T17 |
| Constructor / factory precondition | Runtime default-create invalid address length/family inputs | T18 |
| Constructor / factory valid input | Runtime default-create valid AF_INET6 local and peer address shapes | T18 |
| Test-hook precondition | CLI client failpoint gap values rejected | T15 |
| Test-hook precondition | CLI client pending failpoint NULL output rejected | T15 |
| Lifecycle API precondition | Force-destroy NULL receiver is a no-op | T13 |
| Callback-safe lifecycle hand-off | Runtime force-destroy while XQUIC callbacks may fire during UDP destroy | T10 |
| Post-syscall sub-branch | Listener `socket(2)` fails before listener setup | T5 |
| Post-syscall sub-branch | Listener `setsockopt(SO_REUSEADDR)` fails after socket creation | T5 |
| Post-syscall sub-branch | Listener `fcntl(F_GETFL)` fails before nonblocking update | T5 |
| Post-syscall sub-branch | Listener `fcntl(F_SETFL)` fails during nonblocking update | T5 |
| Post-syscall sub-branch | Listener `bind(2)` fails after socket setup | T5 |
| Post-syscall sub-branch | Listener `listen(2)` fails after bind | T5 |
| Post-syscall sub-branch | Listener `getsockname(2)` fails after listen | T5 |
| Post-syscall sub-branch | Accepted fd `fcntl(F_GETFL)` fails after `accept(2)` on macOS | T8 |
| Post-syscall sub-branch | Accepted fd `fcntl(F_SETFL)` fails after `accept(2)` on macOS | T8 |

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | Client parser and main transport contract | `odin_cli_parse` with subcases: omitted transport, `--transport tcp`, `--transport quic`, `--transport udp`, abbreviated `--trans quic`, and server `--transport quic --quic-cert C --quic-key K`; plus `odin_cli_main` client subcases with required flags and `--transport udp` or `--transport QUIC` | Parse omitted and `tcp` produce `ODIN_CLI_OK` with `client_transport == TCP`; parse `quic` produces `ODIN_CLI_OK` with `client_transport == QUIC`; parse `udp` returns `ODIN_CLI_ERR_BAD_TRANSPORT`; abbreviated client option returns `ODIN_CLI_ERR_UNKNOWN_FLAG`; the valid server argv produces `ODIN_CLI_OK` with `server_transport == QUIC`, populated TLS paths, and `client_transport == TCP`; both main invalid-transport subcases return `2`, write empty stdout, and write exactly `odin: invalid --transport\nusage: odin-client --listen ADDR --server ADDR [--transport tcp\|quic]\n`, proving case-sensitive exactness and the updated client usage string | G1, G2 | unit |
| T2 | TCP path preserved | Host `odin-client --listen 0 --server 127.0.0.1:<fake>` and `--transport tcp` variant, with a fake TCP Odin listener | Both variants print the existing TCP banner without `transport=`, one CONNECT request reaches the fake TCP Odin listener as a CONNECT_REQ, the process exits `0` on `SIGTERM`, and `quic_runtime_create_calls == 0` in the in-process variant | G1 | integration |
| T3 | QUIC startup creates runtime after listener readiness | `odin_cli_main` child with `--listen 0 --server 127.0.0.1:4433 --transport quic`, runtime/event-loop test hooks reset | Stderr first line matches `odin: mode=client transport=quic listen=<port> server=127.0.0.1:4433\n`; pre-bind hook reports `127.0.0.1:0`; post-`getsockname` hook reports `127.0.0.1:<port>` matching the banner; CLI runtime-config record has UDP local `0.0.0.0:0`, peer `127.0.0.1:4433`, `server_host_len == strlen("127.0.0.1")`, and `server_host_value == "127.0.0.1"`; runtime default-create record has `default_create_calls == 1`, `engine_config == NULL`, `transport_callbacks == NULL`, `token == NULL`, `token_len == 0`, `conn_ssl_config_value.cert_verify_flag == 0`, and `no_crypto_flag == 0`; `quic_runtime_create_calls == 1` and `quic_runtime_start_calls == 1` | G3, S1 | integration |
| T4 | QUIC rejects parsed non-IPv4 server before resources | `odin_cli_main` with `--transport quic` and subcases: `--server example.com:443`, `--server [::1]:443` | Parser returns OK for each setup, but runner exits `1`, writes exactly `odin: client startup failed at server_endpoint\n`, writes no success banner, CLI liveness shows zero listener, accept loop, and XQC runtime objects, and `odin_event_loop_test_liveness` shows `loops == 0`, `io_handles == 0`, `timers == 0`, and `task_nodes == 0` | G5, S2 | unit |
| T5 | QUIC listener startup failure matrix cleans resources | Subcases using `ODIN_CLI_CLIENT_TEST_FAIL_SOCKET`, `ODIN_CLI_CLIENT_TEST_FAIL_SETSOCKOPT_REUSEADDR`, `ODIN_CLI_CLIENT_TEST_FAIL_FCNTL_GETFL`, `ODIN_CLI_CLIENT_TEST_FAIL_FCNTL_SETFL`, production bind collision or `ODIN_CLI_CLIENT_TEST_FAIL_BIND`, `ODIN_CLI_CLIENT_TEST_FAIL_LISTEN`, and `ODIN_CLI_CLIENT_TEST_FAIL_GETSOCKNAME` | Each subcase exits `1`, writes exactly the matching line for `socket`, `setsockopt(SO_REUSEADDR)`, `fcntl(F_GETFL)`, `fcntl(F_SETFL)`, `bind`, `listen`, or `getsockname`, writes no success banner, leaves no connectable partial listener, CLI liveness shows zero live listener, accept loop, and XQC runtime objects, and `odin_event_loop_test_liveness` shows zero live loops, I/O handles, timers, and task nodes; bind/listen/getsockname subcases also assert the captured TCP bind address is `127.0.0.1:<listen_port>` | G5, S1 | unit |
| T6 | QUIC accepted fd transfers to runtime and does not locally dial CONNECT target | Running QUIC child from T3; local client sends `CONNECT 127.0.0.1:<sentry> HTTP/1.1\r\n\r\n`; sentry is a loopback TCP listener with accept deadline | `quic_runtime_add_connection_calls == 1`; last add record reports a nonblocking fd; the sentry accepts zero connections before shutdown; on `SIGTERM`, runtime force-destroy closes the add-owned fd and child exits `0` with no extra stderr | G4, S3 | integration |
| T7 | QUIC add failure closes only that accepted fd | Running QUIC child with `FAIL_XQC_CLIENT_RUNTIME_ADD_CONNECTION` armed for the first accepted fd; two local clients connect sequentially | First peer observes EOF/reset with no bytes and no stderr; second accepted fd reaches `odin_xqc_client_runtime_add_connection`; process remains running until `SIGTERM` and exits `0`; CLI and event-loop liveness show no leaked listener, accept loop, loop, timer, I/O handle, task node, or runtime | G4 | integration |
| T8 | QUIC fatal runtime failures clean up | Running QUIC child with subcases: accept-loop terminal error, macOS-only post-accept `F_GETFL` failure via `ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_GETFL_ERROR`, macOS-only post-accept `F_SETFL` failure via `ODIN_CLI_CLIENT_TEST_TRIGGER_ACCEPT_LOOP_FCNTL_SETFL_ERROR`, event-loop backend failure, and owner-thread unexpected stop without shutdown flag; Linux builds assert the two fcntl triggers return `-1/EOPNOTSUPP` and skip those runtime subcases | Each subcase writes the ready banner first, then exactly `odin: client runtime failed at accept_loop\n` or `odin: client runtime failed at event_loop_run\n`, exits `1`, closes the listener, force-destroys the XQC runtime, destroys the event loop, and restores signal handlers; the post-accept fcntl subcases close the accepted fd before callback handoff and leave `quic_runtime_add_connection_calls == 0` | G5 | unit |
| T9 | QUIC signal shutdown mirrors TCP cleanup | Running QUIC child with one add-owned accepted fd; subcases `SIGINT` and `SIGTERM` | Signal handler count proves replaced handlers were restored after return; child exits `0`; listener no longer accepts; add-owned peer fd reaches EOF/reset after runtime force-destroy; CLI liveness shows zero live listener, accept loop, and XQC runtime, and `odin_event_loop_test_liveness` shows zero live loops, I/O handles, timers, and task nodes | G5 | integration |
| T10 | Client XQC force destroy is synchronous | Direct `odin_xqc_client_runtime_unittests` setup with fake xquic connection, registered CID, one queued fd, one live stream session, and `odin_xqc_udp_test_set_ops` installing an `engine_destroy` callback that re-enters client-runtime callbacks during `odin_xqc_udp_destroy`; invoke `odin_xqc_client_runtime_force_destroy(rt)` while UDP is running | Call returns only after queued fd is closed, live session teardown closes the local peer, CID is unregistered once, ALPN is unregistered once, UDP destroy is recorded once, `ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_RUNTIME_FREE` and `runtime_free_calls` are recorded once immediately before object free, and callback re-entry during UDP destroy records no second close, unregister, ALPN unregister, UDP destroy, or runtime free | G5 | unit |
| T11 | QUIC post-listener startup failure matrix cleans resources | Subcases using `ODIN_CLI_CLIENT_TEST_FAIL_EVENT_LOOP_CREATE`, `ODIN_CLI_CLIENT_TEST_FAIL_ACCEPT_LOOP_CREATE`, `ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_CREATE`, `ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_START`, `ODIN_CLI_CLIENT_TEST_FAIL_SIGACTION_SIGINT`, `ODIN_CLI_CLIENT_TEST_FAIL_SIGACTION_SIGTERM`, and `ODIN_CLI_CLIENT_TEST_FAIL_SIGNAL_TIMER_START` | Each subcase exits `1`, writes exactly the matching line for `event_loop_create`, `accept_loop_create`, `xqc_client_runtime_create`, `xqc_client_runtime_start`, `sigaction(SIGINT)`, `sigaction(SIGTERM)`, or `signal_timer_start`, writes no success banner, restores replaced signal handlers where installation began, force-destroys any created XQC runtime, closes the listener, leaves zero live listener, accept loop, and XQC runtime in CLI liveness, and leaves zero live loops, I/O handles, timers, and task nodes in event-loop liveness | G5 | unit |
| T12 | Client runner config preconditions fail before resources | Direct calls to `odin_cli_run_client(NULL, err)`, `odin_cli_run_client(&cfg_unknown_transport, err)`, and `odin_cli_run_client(&cfg_tcp, NULL)` with liveness counters reset | Each call returns `1`; the non-null `err` cases write exactly `odin: client startup failed at config\n`; the `err == NULL` case writes nothing and does not crash; all three cases leave zero live listener, accept loop, TCP session, and XQC runtime in CLI liveness and zero live loops, I/O handles, timers, and task nodes in event-loop liveness | G5 | unit |
| T13 | Client XQC force destroy NULL receiver is inert | Direct client XQC runtime unit-test setup snapshots `force_destroy_null_calls`, runtime call records, `runtime_free_calls`, UDP destroy, ALPN unregister, and CID unregister counters; invoke `odin_xqc_client_runtime_force_destroy(NULL)` | The call returns without crashing, `force_destroy_null_calls == before + 1`, and the runtime call records, `runtime_free_calls`, UDP destroy, ALPN unregister, and CID unregister counters are unchanged | G5 | unit |
| T14 | Client parser precedence and TLS-flag rejection cells | `odin_cli_parse` client subcases: `--help --transport udp`, `--listen nope --server 127.0.0.1:443 --transport udp`, `--listen 0 --server 127.0.0.1:bad --transport udp`, `--listen 0 --transport udp`, `--listen 0 --server 127.0.0.1:443 --transport=`, `--listen 0 --server 127.0.0.1:443 --quic-cert C`, `--listen 0 --server 127.0.0.1:443 --quic-key K`, and `--listen 0 --server 127.0.0.1:443 --transport quic --quic-cert C --quic-key K` | The statuses are, respectively, `ODIN_CLI_HELP`, `ODIN_CLI_ERR_BAD_LISTEN_PORT`, `ODIN_CLI_ERR_BAD_SERVER`, `ODIN_CLI_ERR_MISSING_REQUIRED`, `ODIN_CLI_ERR_BAD_TRANSPORT`, `ODIN_CLI_ERR_UNKNOWN_FLAG`, `ODIN_CLI_ERR_UNKNOWN_FLAG`, and `ODIN_CLI_ERR_UNKNOWN_FLAG`; no subcase writes `client_transport` unless the status is OK, proving invalid transport is evaluated only after higher-precedence parser errors and client TLS flags remain unknown after adding client `--transport` | G2 | unit |
| T15 | CLI client failpoint gap values and NULL pending output are rejected | Reset CLI client test state; call `odin_cli_client_test_pending_failpoint(NULL)`; snapshot `odin_cli_client_test_pending_failpoint(&pending)`; call `odin_cli_client_test_fail_next((odin_cli_client_test_failpoint_t)99, EIO)`; snapshot pending state again; arm `ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_CREATE`; confirm `odin_cli_client_test_pending_failpoint(&pending)` reports the armed value; call `odin_cli_client_test_pending_failpoint(NULL)` again; snapshot pending state again; then run the T11 `xqc_client_runtime_create` startup subcase | Each NULL-output pending call returns `-1` with `errno == EINVAL`; the first one leaves pending as `(odin_cli_client_test_failpoint_t)0`; the gap call returns `-1` with `errno == EINVAL` and pending remains `(odin_cli_client_test_failpoint_t)0`; the valid failpoint call returns `0`; the second NULL-output pending call does not clear the armed value; the final pending snapshot still reports `ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_CREATE` before the runner consumes it; and the startup run writes exactly `odin: client startup failed at xqc_client_runtime_create\n` with the same zero-liveness cleanup assertions as T11 | G5 | unit |
| T16 | Client unknown flag takes precedence over invalid transport | `odin_cli_parse` client argv `--listen 0 --server 127.0.0.1:443 --transport udp --quic-cert C` | Status is `ODIN_CLI_ERR_UNKNOWN_FLAG`, not `ODIN_CLI_ERR_BAD_TRANSPORT`, proving the unknown-flag precedence cell masks a recognized invalid `--transport` value and client TLS flags remain unknown even when an invalid transport is also present | G2 | unit |
| T17 | Runtime default-create null preconditions reject invalid input | Direct `odin_xqc_client_runtime_unittests` calls to `odin_xqc_client_runtime_create_default` with subcases: null `config`, null `out`, null `loop`, null `local_addr`, null `peer_addr`, and null `server_host` | Every subcase returns `-1` with `errno == EINVAL`; every non-null output pointer remains at its sentinel runtime value; `default_create_calls`, `udp_create_calls`, and runtime call records remain unchanged; no XQC runtime is returned or must be destroyed; and after fixture teardown `odin_event_loop_test_liveness` reports zero loops, I/O handles, timers, and task nodes | G5 | unit |
| T18 | Runtime default-create address shapes reject invalid input and accept IPv6 | Direct `odin_xqc_client_runtime_unittests` calls to `odin_xqc_client_runtime_create_default` with otherwise valid inputs and subcases: local `addrlen` one byte short, local `addrlen` one byte long, local unsupported `sa_family`, peer `addrlen` one byte short, peer `addrlen` one byte long, peer unsupported `sa_family`, valid `AF_INET6` local address with IPv4 peer, and valid `AF_INET6` peer address with IPv4 local | The six invalid subcases return `-1` with `errno == EINVAL`; every output pointer remains at its sentinel runtime value; `default_create_calls`, `udp_create_calls`, and runtime call records remain unchanged; no XQC runtime is returned or must be destroyed; and after fixture teardown `odin_event_loop_test_liveness` reports zero loops, I/O handles, timers, and task nodes. Each IPv6 success subcase returns `0`, writes a non-sentinel runtime output, increments `default_create_calls` and `udp_create_calls` once, and records the T3 raw defaults in `last_default_create`; the valid local-IPv6 subcase records `last_udp_create.local_addrlen == sizeof(struct sockaddr_in6)`, and the valid peer-IPv6 subcase starts the runtime so the existing connect test op asserts peer bytes with `peer_addrlen == sizeof(struct sockaddr_in6)`. Each success subcase then force-destroys the runtime and leaves zero live loops, I/O handles, timers, task nodes, UDP objects, and runtime objects | G5 | unit |

## 6. Implementation Plan

- **P1. Land parser contract and red-verifiable rows T1-T18.**
  - **Scope:** update `odin/cli.h`, `odin/cli.c`, test declarations, and test files with the client transport field, usage string expectations, strict red-verification gates for T1-T18, the T14 parser precedence/TLS-flag assertions, the T16 unknown-flag-over-invalid-transport precedence assertion, the T15 pending-failpoint test helper declaration plus NULL-output assertion, the T17/T18 runtime default-create invalid-input assertions, the T18 valid AF_INET6 local/peer success assertions, the `odin_xqc_client_runtime_create_default` declaration/stub, the CLI post-`getsockname` listener-address hook declaration, and any stubs needed for compilation. This phase also adds the `ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_RUNTIME_FREE` enum value, `default_create_calls`, `last_default_create`, `runtime_free_calls`, `force_destroy_null_calls`, and T10/T13/T17/T18 assertion fixtures to the runtime test declaration surface; P1 does not yet wire the default-create helper to validate invalid inputs, accept valid AF_INET6 local/peer address shapes, supply non-null raw config pointers, or record valid default-create fields, `runtime_finish_destroy_once` to record runtime free, `odin_xqc_client_runtime_force_destroy(NULL)` to increment `force_destroy_null_calls`, or `odin_cli_client_test_fail_next` to whitelist failpoints with the §3.2.5 switch. Rows T1-T18 are gated out of the default local suite with `GTEST_SKIP("pending RFC-028 P2/P3")` unless `ODIN_RFC028_RED=1` is set; `ODIN_RFC028_RED=1 out/Default/odin_unittests --gtest_filter='*RFC028*:*ClientTransport*'` executes the assertions and fails against the current TCP-only client runner while the default host `odin_unittests` remains green.
  - **Depends on:** None.
  - **Done when:** Host-runnable: macOS host `odin_unittests` reports T1-T18 skipped in the default run, and the `ODIN_RFC028_RED=1` red-verification command reports T1-T18 failing for the missing parser field, client main invalid-transport usage/case-sensitive mapping, T14 precedence/TLS-flag cells, T16 unknown-flag-over-invalid-transport precedence, QUIC banner, post-`getsockname` listener-address capture, startup failure matrices, runtime default-create/start, add handoff, direct runner config preconditions, cleanup behavior, T10 runtime-free record, T13 NULL-entry `force_destroy_null_calls` record, T15 gap plus NULL-output pending-failpoint rejection, T17/T18 default-create invalid-input rejection, and T18 valid AF_INET6 local/peer success assertions. Cross-compile-only: no alternate-platform branch is required to execute in P1; Linux, alternate macOS arch, iOS simulator, and iOS device artifacts are not run in this environment.

- **P2. Turn parser and TCP-preservation rows green.**
  - **Scope:** implement §3.2.1 parser changes, update client usage text, add `odin_cli_client_config_t`, route TCP mode through the existing runner body, implement the §3.2.2 `config`/`err`/unknown-transport precondition branch, and keep QUIC mode as a deterministic startup stub that still fails before creating runtime resources. Remove the red gate from T1-T2, T12, T14, and T16 only.
  - **Depends on:** P1.
  - **Done when:** Host-runnable: macOS host `odin_unittests` runs T1-T2, T12, T14, and T16 un-gated and they pass; T3-T11, T13, T15, T17, and T18 remain skipped by default and still fail under `ODIN_RFC028_RED=1`. Cross-compile-only: the Linux, alternate macOS arch, iOS simulator, and iOS device `odin` and `odin_unittests` artifacts compile; no T-row branch from those artifacts is executed.

- **P3. Turn QUIC startup, handoff, failure, and force-destroy rows green.**
  - **Scope:** implement §3.2.2 and §3.2.3 QUIC runner startup, Odin-level runtime config, accepted-fd handoff, failure classification, signal cleanup, test hooks, narrowed scope check, `odin_cli_client` build dependency on `odin_client_xqc_runtime`, the runtime-owned `odin_xqc_client_runtime_create_default` helper, and the complete §3.2.4 `odin_xqc_client_runtime_force_destroy` API before wiring it into every QUIC client cleanup path. This phase records the post-`getsockname` listener address after successful `getsockname`, validates every T17/T18 default-create invalid-input case before raw config construction or lower runtime/UDP create calls, accepts T18 valid AF_INET6 local/peer address shapes and records their success-side default-create fields, records the helper's raw default-create fields in `last_default_create`, wires `runtime_finish_destroy_once` to record `ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_RUNTIME_FREE` and increment `runtime_free_calls`, wires `odin_xqc_client_runtime_force_destroy(NULL)` to increment `force_destroy_null_calls` before returning, implements the §3.2.5 switch-based `odin_cli_client_test_fail_next` validation and `odin_cli_client_test_pending_failpoint` including the `out == NULL` branch, adds the UDP-destroy callback re-entry behavior needed by T10 using `odin_xqc_udp_test_set_ops(... engine_destroy ...)` under target-wide `ODIN_XQC_UDP_TESTING`, and lands any remaining §3.2.4 callback guard or single-finalizer corrections that T10 exposes. Remove the red gates from T3-T11, T13, T15, T17, and T18, so no QUIC cleanup row is un-gated while T10, T13, T15, T17, or T18 is still red.
  - **Depends on:** P2.
  - **Done when:** Host-runnable: macOS host `odin_unittests` runs T1-T18 un-gated and they pass, including T3's post-`getsockname` listener-address hook and runtime default-create record, T8 through accept-loop terminal, macOS post-accept `F_GETFL`/`F_SETFL`, event-loop failure, and unexpected-stop hooks; T10 proves synchronous force-destroy plus UDP-destroy callback re-entry safety before any QUIC cleanup row depends on that path; T13 proves the NULL receiver branch; T15 proves unused failpoint enum gaps are rejected by the switch whitelist and `odin_cli_client_test_pending_failpoint(NULL)` preserves armed state while returning `-1/EINVAL`; T17/T18 prove invalid default-create inputs return `-1/EINVAL` without changing sentinel output or creating runtime/UDP side effects; T18 also proves valid AF_INET6 local and peer address shapes each create a runtime, record the default-create fields, force-destroy cleanly, and leave zero runtime, UDP, and event-loop liveness; the default local host suite remains green; and `ninja -C out/Default odin:odin_client_xqc_runtime_scope_check` passes with no raw `xqc_` or `XQC_` token in `odin_cli_client`. Cross-compile-only: Linux `odin_unittests`, `odin`, and `odin:odin_client_xqc_runtime_scope_check` compile/run their build actions but unit binaries are not executed; T8's Linux accept4/epoll/timerfd branches are compiled but not run, the two macOS-only post-accept fcntl triggers compile as `-1/EOPNOTSUPP` hook checks, and T10/T13/T15/T17/T18 force-destroy, failpoint-validation, and default-create-precondition code is compiled but not runtime-verified outside the macOS host binary. Alternate macOS arch, iOS simulator, and iOS device artifacts plus `odin:odin_client_xqc_runtime_scope_check` build actions compile/run but unit binaries are not executed.
