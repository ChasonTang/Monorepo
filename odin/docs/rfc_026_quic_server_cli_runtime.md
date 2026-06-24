# RFC-026: QUIC Server CLI Runtime Mode

## 1. Summary

Add an explicit QUIC transport mode to `odin-server` so the existing single `odin` binary can start `odin_xqc_server_runtime` with UDP binding, required XQUIC TLS key/certificate paths, deterministic startup/error reporting, graceful signal cleanup, and the same default server dial filter as the TCP server path.

## 2. Goals

- **G1.** For every `odin-server` invocation that does not explicitly select QUIC, the process keeps the current TCP server behavior: same default listen port, same `--listen` parsing, same TCP startup banner, same signal cleanup, and no required QUIC TLS options.

- **G2.** For every `odin-server` invocation that explicitly selects QUIC, the CLI requires nonempty QUIC certificate and private-key path arguments, binds an IPv4 UDP socket on `0.0.0.0:<listen_port>`, reports the actual bound UDP port on stderr, and starts `odin_xqc_server_runtime` with `ODIN_XQC_SERVER_ALPN`.

- **G3.** For every parser, QUIC startup, QUIC runtime-start, or event-loop-run failure introduced by this mode, the process emits one deterministic user-visible error line, releases every CLI-owned object created before the failure, and returns the documented nonzero exit code.

- **G4.** A `SIGINT` or `SIGTERM` delivered after successful QUIC startup stops the owner-thread event loop, destroys the QUIC runtime, destroys the event loop, restores replaced signal handlers, releases the UDP binding, and returns exit code `0`.

- **G5.** CONNECT_REQ destinations received through QUIC use the exact same default server dial filter as CONNECT_REQ destinations received through the current TCP CLI server path: non-public IPv4 destinations are denied before any outbound dial is attempted, while public IPv4 destinations continue into the existing server-session dial path.

## 3. Design

### 3.1 Overview

`odin_cli_main` remains the only entry point behind the `odin` binary and the `odin-client` / `odin-server` symlinks. The parser gains server-only QUIC selection and TLS path fields, but the client parser and all existing non-server status mappings stay in `odin/cli.c`.

The server runner becomes a transport-selected internal helper. Its TCP branch is the current RFC-022 runner, including the current banner format and cleanup order. Its QUIC branch builds an IPv4 UDP local address, creates an event loop, creates `odin_xqc_server_runtime`, installs the shared default dial filter, starts the runtime, obtains the actual bound UDP port through a new runtime local-address accessor, installs the same signal-stop timer mechanism used by TCP, prints the QUIC startup banner, and enters `odin_event_loop_run`.

```
odin-server argv
    |
    v
odin_cli_parse
    |-- no --transport or --transport tcp --> TCP runner (unchanged behavior)
    |
    '-- --transport quic + --quic-cert + --quic-key
            |
            v
       QUIC server runner
            |-- sockaddr_in { AF_INET, INADDR_ANY, listen_port }
            |-- odin_event_loop_create
            |-- odin_xqc_server_runtime_create(ssl_config, local_addr)
            |-- odin_xqc_server_runtime_set_dial_filter(shared default filter)
            |-- odin_xqc_server_runtime_start
            |-- odin_xqc_server_runtime_local_addr -> actual UDP port
            |-- install SIGINT/SIGTERM handlers and owner-thread polling timer
            |-- fprintf(stderr, "odin: mode=server transport=quic listen=<port>\n")
            v
       odin_event_loop_run
            |
            v
       cleanup: force-destroy xqc runtime -> event loop -> restore handlers
```

### 3.2 Detailed Design

#### 3.2.1 Server CLI Parser Contract

Current parser state has no server transport or QUIC TLS fields at [odin/cli.h](/Users/tangjiacheng/Downloads/Monorepo/odin/cli.h:85), and the current server long-option table accepts only `--listen` and `--help` at [odin/cli.c](/Users/tangjiacheng/Downloads/Monorepo/odin/cli.c:85). This RFC extends that surface as follows:

```c
/* odin/cli.h additions */
typedef enum odin_cli_server_transport_t {
  ODIN_CLI_SERVER_TRANSPORT_TCP = 0,
  ODIN_CLI_SERVER_TRANSPORT_QUIC,
} odin_cli_server_transport_t;

typedef enum odin_cli_status_t {
  ODIN_CLI_OK = 0,
  ODIN_CLI_HELP,
  ODIN_CLI_ERR_UNKNOWN_MODE,
  ODIN_CLI_ERR_MISSING_REQUIRED,
  ODIN_CLI_ERR_UNKNOWN_FLAG,
  ODIN_CLI_ERR_BAD_LISTEN_PORT,
  ODIN_CLI_ERR_BAD_SERVER,
  ODIN_CLI_ERR_BAD_TRANSPORT,
  ODIN_CLI_ERR_BAD_QUIC_TLS,
} odin_cli_status_t;

typedef struct odin_cli_args_t {
  odin_cli_mode_t mode;
  uint16_t listen_port;
  const char *server_host;
  size_t server_host_len;
  uint16_t server_port;
  odin_cli_server_transport_t server_transport;
  const char *quic_cert_file;
  const char *quic_key_file;
} odin_cli_args_t;
```

Server-mode long options become:

```c
static const struct option kServerLong[] = {
    {"listen", required_argument, NULL, 'l'},
    {"transport", required_argument, NULL, 1000},
    {"quic-cert", required_argument, NULL, 1001},
    {"quic-key", required_argument, NULL, 1002},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
};
```

`odin_cli_main` maps the new parse statuses to:

```text
status                         mode    return  out  err
-----------------------------  ------  ------  ---  ----------------------------------------------------
ODIN_CLI_ERR_BAD_TRANSPORT     SERVER  2       -    odin: invalid --transport\n<U_S>\n
ODIN_CLI_ERR_BAD_QUIC_TLS      SERVER  2       -    odin: invalid QUIC TLS configuration\n<U_S>\n
```

`<U_S>` is:

```text
usage: odin-server --listen ADDR [--transport tcp|quic] [--quic-cert FILE --quic-key FILE]
```

**Unstated contract.** The new server transport field is meaningful only when `out.mode == ODIN_CLI_MODE_SERVER`; it is zeroed to TCP on entry and remains TCP for all current TCP OK invocations. `--transport` accepts exactly `tcp` or `quic`, case-sensitive. `--transport tcp` is explicit TCP and has the same runtime behavior as omitting `--transport`. `--transport quic` is the only selector that enables QUIC. `--quic-cert` and `--quic-key` are server-only flags; a client invocation using either flag still returns `ODIN_CLI_ERR_UNKNOWN_FLAG`. A QUIC OK requires both TLS path arguments to be present and nonempty. Supplying either TLS path while the selected transport is TCP, including the default TCP transport when `--transport` is omitted, returns `ODIN_CLI_ERR_BAD_QUIC_TLS` rather than silently ignoring a credential path. The parser aliases both path values from `argv` exactly like the existing client `--server` host slice; it does not allocate, canonicalize, open, or read the paths. XQUIC copies nonempty server paths during TLS-context creation, and current XQUIC rejects missing server `private_key_file` or `cert_file` in `xqc_tls_ctx_set_config` at [xquic/src/tls/xqc_tls_ctx.c](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/tls/xqc_tls_ctx.c:238). Exact long-option spelling remains required; abbreviated forms such as `--trans`, `--quic-ce`, and `--quic-ke` return `ODIN_CLI_ERR_UNKNOWN_FLAG`.

**Mechanism.**

```text
parse(argc, argv, out):
  zero out, including server_transport = TCP
  select mode by basename as today
  if mode == SERVER:
    parse --listen, --transport, --quic-cert, --quic-key, --help
  preserve current precedence through BAD_LISTEN_PORT
  if transport_arg exists and transport_arg not in {"tcp", "quic"}:
    return BAD_TRANSPORT
  if selected transport is QUIC:
    if quic_cert missing/empty or quic_key missing/empty:
      return BAD_QUIC_TLS
  else if quic_cert exists or quic_key exists:
    return BAD_QUIC_TLS
  fill listen_port and, for server mode, server_transport/TLS path fields
  return OK

main(argc, argv, out, err):
  if OK/CLIENT: current client path
  if OK/SERVER:
    cfg = {args.listen_port, args.server_transport,
           args.quic_cert_file, args.quic_key_file}
    return odin_cli_run_server(&cfg, err)
  map BAD_TRANSPORT and BAD_QUIC_TLS to the deterministic lines above and return 2
```

Satisfies: G1 via the default TCP transport value and unchanged TCP parse path; G2 via explicit QUIC selection and nonempty TLS path requirements; G3 via deterministic parse errors for invalid transport and invalid QUIC TLS configuration.

#### 3.2.2 Transport-Selected Server Runner

The current `odin_cli_main` calls a TCP-only `odin_cli_run_server(args.listen_port, err)` at [odin/cli.c](/Users/tangjiacheng/Downloads/Monorepo/odin/cli.c:267), and the current helper signature is pinned at [odin/cli_server.h](/Users/tangjiacheng/Downloads/Monorepo/odin/cli_server.h:28). Replace that internal signature with:

```c
/* odin/cli_server.h */
#include "odin/cli.h"
#include <stdint.h>
#include <stdio.h>

typedef struct odin_cli_server_config_t {
  uint16_t listen_port;
  odin_cli_server_transport_t transport;
  const char *quic_cert_file;
  const char *quic_key_file;
} odin_cli_server_config_t;

int odin_cli_run_server(const odin_cli_server_config_t *config, FILE *err);
```

**Unstated contract.** `err` is a non-null internal precondition. `config == NULL` or an unknown `config->transport` value writes `odin: server startup failed at config\n` and returns `1` before creating any socket, event loop, or runtime. `config->transport == ODIN_CLI_SERVER_TRANSPORT_TCP` delegates to the existing TCP runner mechanism and preserves the exact TCP success banner `odin: mode=server listen=<actual_port>\n`. `config->transport == ODIN_CLI_SERVER_TRANSPORT_QUIC` requires nonempty `quic_cert_file` and `quic_key_file`; a missing value at this layer writes `odin: quic server startup failed at tls_config\n` and returns `1`, although the normal `odin_cli_main` path catches that earlier as `ODIN_CLI_ERR_BAD_QUIC_TLS`. The helper writes nothing to stdout in either transport mode.

The QUIC branch binds IPv4 UDP on `0.0.0.0:<listen_port>`, matching the current TCP path's IPv4 `INADDR_ANY` listen policy. `listen_port == 0` means kernel-selected ephemeral UDP port. The success banner is printed only after event-loop creation, QUIC runtime creation, default dial-filter installation, runtime start, local-address discovery, signal-handler installation, and signal-timer start have all succeeded. Its exact format is `odin: mode=server transport=quic listen=<actual_port>\n`, with `<actual_port>` in decimal and never `0` after a successful ephemeral bind.

The QUIC branch uses `xqc_engine_ssl_config_t.private_key_file` and `xqc_engine_ssl_config_t.cert_file`, which are the XQUIC server key and certificate fields at [xquic/include/xquic/xquic.h](/Users/tangjiacheng/Downloads/Monorepo/xquic/include/xquic/xquic.h:1231). `ciphers`, `groups`, session timeout, and session-ticket fields remain zero so XQUIC applies its defaults except where it already treats missing session-ticket data as nonfatal. The CLI passes a zeroed `xqc_engine_callback_t`; `odin_xqc_udp_create` currently fills `set_event_timer` and a default monotonic timestamp before `xqc_engine_create` at [odin/xqc_udp.c](/Users/tangjiacheng/Downloads/Monorepo/odin/xqc_udp.c:401). The existing `odin_xqc_server_runtime_create`, `start`, `set_dial_filter`, and `destroy` APIs remain available at [odin/server_xqc_runtime.h](/Users/tangjiacheng/Downloads/Monorepo/odin/server_xqc_runtime.h:30); §3.2.5 adds `odin_xqc_server_runtime_force_destroy` for CLI process shutdown that must synchronously release UDP even with linked connections.

**Mechanism.**

```text
run_server(config, err):
  if config invalid: return startup_fail("server", "config")
  if config.transport == TCP:
    return run_tcp_server(config.listen_port, err)  # current RFC-022 body
  if config.transport != QUIC:
    return startup_fail("server", "config")
  return run_quic_server(config, err)

run_quic_server(config, err):
  state = zeroed, state.transport = QUIC
  if cert/key missing: return startup_fail("quic server", "tls_config")
  if odin_event_loop_create(&state.loop) != 0:
    return startup_fail("quic server", "event_loop_create")

  local = sockaddr_in(AF_INET, INADDR_ANY, htons(config.listen_port))
  ssl = zeroed xqc_engine_ssl_config_t
  ssl.private_key_file = (char *)config.quic_key_file
  ssl.cert_file = (char *)config.quic_cert_file
  callbacks = zeroed xqc_engine_callback_t
  rt_config = {
    loop = state.loop,
    local_addr = &local,
    local_addrlen = sizeof(local),
    engine_config = NULL,
    ssl_config = &ssl,
    engine_callbacks = &callbacks,
  }
  if odin_xqc_server_runtime_create(&rt_config, &state.xqc_runtime) != 0:
    return startup_fail("quic server", "xqc_server_runtime_create")
  install shared default dial filter on state.xqc_runtime
  if odin_xqc_server_runtime_start(state.xqc_runtime) != 0:
    return startup_fail("quic server", "xqc_server_runtime_start")
  if odin_xqc_server_runtime_local_addr(state.xqc_runtime, &bound, &bound_len) != 0:
    return startup_fail("quic server", "xqc_server_runtime_local_addr")
  actual_port = ntohs(((sockaddr_in *)&bound)->sin_port)
  install signal handlers and signal poll timer
  print "odin: mode=server transport=quic listen=<actual_port>\n"
  run_rc = odin_event_loop_run(state.loop)
  if run_rc != 0:
    print "odin: quic server runtime failed at event_loop_run\n"
    cleanup
    return 1
  cleanup
  return state.shutdown_requested ? 0 : 1
```

Satisfies: G1 via a TCP branch that preserves the current helper body and banner; G2 via the QUIC branch's TLS config, UDP local address, XQUIC runtime creation, runtime start, and actual-port banner; G3 via deterministic config/startup/runtime failure points.

#### 3.2.3 Bound UDP Address Accessors

`odin_xqc_udp_create` already stores the bound UDP local address after `odin_udp_local_addr` succeeds at [odin/xqc_udp.c](/Users/tangjiacheng/Downloads/Monorepo/odin/xqc_udp.c:421), but no public accessor exposes it. Add:

```c
/* odin/xqc_udp.h */
int odin_xqc_udp_local_addr(odin_xqc_udp_t *xu, struct sockaddr *addr,
                            socklen_t *addrlen);

/* odin/server_xqc_runtime.h */
int odin_xqc_server_runtime_local_addr(odin_xqc_server_runtime_t *rt,
                                       struct sockaddr *addr,
                                       socklen_t *addrlen);
```

**Unstated contract.** Both accessors are owner-thread APIs. `xu` or `rt`, `addr`, and `addrlen` are non-null preconditions; invalid input returns `-1` with `errno = EINVAL`. `*addrlen` is an in/out capacity. If the caller's capacity is smaller than the stored local-address length, the accessor sets `*addrlen` to the required length, returns `-1` with `errno = ENOBUFS`, and does not partially copy. On success it copies exactly the stored address bytes and sets `*addrlen` to the copied length. The accessor is valid after runtime creation and before or after `odin_xqc_server_runtime_start`; starting the runtime does not rebind the UDP socket. The server-runtime wrapper forwards to the UDP-driver accessor and does not expose the underlying `odin_xqc_udp_t *`.

**Mechanism.**

```text
xqc_udp_local_addr(xu, addr, addrlen):
  validate inputs
  required = xu.local_addrlen
  if *addrlen < required:
    *addrlen = required
    errno = ENOBUFS
    return -1
  memcpy(addr, &xu.local_addr, required)
  *addrlen = required
  return 0

xqc_server_runtime_local_addr(rt, addr, addrlen):
  validate inputs
  return odin_xqc_udp_local_addr(rt.xu, addr, addrlen)
```

Satisfies: G2 via an observable way for the CLI to report the actual UDP port selected by a successful runtime bind; G3 via synchronous validation and deterministic `ENOBUFS` failure.

#### 3.2.4 Shared Default Server Dial Filter

The current CLI default server filter lives in `odin/cli_server.c` and denies non-public IPv4 ranges before `odin_dial_start`; the core check and wrapper are at [odin/cli_server.c](/Users/tangjiacheng/Downloads/Monorepo/odin/cli_server.c:88) and [odin/cli_server.c](/Users/tangjiacheng/Downloads/Monorepo/odin/cli_server.c:110). This RFC keeps one implementation and installs that same callback on both transport runtimes:

```c
static int odin_cli_default_server_dial_filter(const struct sockaddr *addr,
                                               socklen_t addrlen,
                                               void *user_data);
```

The deny table remains:

```text
range            result
---------------  ------
0.0.0.0/8        EACCES
10.0.0.0/8       EACCES
100.64.0.0/10    EACCES
127.0.0.0/8      EACCES
169.254.0.0/16   EACCES
172.16.0.0/12    EACCES
192.0.0.0/24     EACCES
192.0.2.0/24     EACCES
192.168.0.0/16   EACCES
198.18.0.0/15    EACCES
198.51.100.0/24  EACCES
203.0.113.0/24   EACCES
224.0.0.0/4      EACCES
240.0.0.0/4      EACCES
```

`NULL`, too-short `addrlen`, and non-`AF_INET` inputs return `EAFNOSUPPORT`; every other IPv4 address returns `0`.

**Unstated contract.** TCP and QUIC install the same function pointer with `user_data == NULL`, not two diverging implementations. The TCP branch calls `odin_server_runtime_set_dial_filter` after `odin_server_runtime_create` succeeds and before its success banner, as it does today at [odin/cli_server.c](/Users/tangjiacheng/Downloads/Monorepo/odin/cli_server.c:350). The QUIC branch calls `odin_xqc_server_runtime_set_dial_filter` after `odin_xqc_server_runtime_create` succeeds and before `odin_xqc_server_runtime_start`, so the runtime has the filter before any UDP packet can create a stream session. The filter remains IPv4-only because `odin_server_session` currently parses CONNECT_REQ hosts into `sockaddr_in` before dialing; an IPv6 CONNECT policy is outside this RFC.

**Mechanism.**

```text
install_default_filter(state):
  if state.transport == TCP:
    odin_server_runtime_set_dial_filter(
        state.tcp_runtime, odin_cli_default_server_dial_filter, NULL)
  else:
    odin_xqc_server_runtime_set_dial_filter(
        state.xqc_runtime, odin_cli_default_server_dial_filter, NULL)
```

Satisfies: G5 via one shared callback installed on both runtime types before either can process peer CONNECT_REQ bytes.

#### 3.2.5 Signal Stop, Errors, and Cleanup

The QUIC branch reuses the existing async-signal-safe handler and owner-thread polling-timer shape from the TCP path: the signal handler writes a `sig_atomic_t`, and the event-loop timer performs `odin_event_loop_stop` from the loop owner thread.

Add one server-runtime cleanup entry point for process-level shutdown:

```c
/* odin/server_xqc_runtime.h */
void odin_xqc_server_runtime_force_destroy(odin_xqc_server_runtime_t *rt);
```

QUIC failure lines are:

```text
odin: quic server startup failed at tls_config
odin: quic server startup failed at event_loop_create
odin: quic server startup failed at xqc_server_runtime_create
odin: quic server startup failed at xqc_server_runtime_start
odin: quic server startup failed at xqc_server_runtime_local_addr
odin: quic server startup failed at sigaction(SIGINT)
odin: quic server startup failed at sigaction(SIGTERM)
odin: quic server startup failed at signal_timer_start
odin: quic server runtime failed at event_loop_run
```

**Unstated contract.** TCP failure lines and cleanup order stay byte-for-byte compatible with the existing RFC-022 path. QUIC setup failure cleanup stops the signal timer if armed, force-destroys the QUIC runtime if non-null, destroys the event loop if non-null, restores any replaced signal handlers, flushes `err`, and returns `1`. QUIC graceful signal cleanup uses the same object order after `odin_event_loop_run` returns with `state.shutdown_requested != 0`, then returns `0`.

`odin_xqc_server_runtime_destroy` keeps its existing callback-safe deferred-destroy behavior: when live connections remain linked, it marks destroy pending and may return before `runtime_finish_destroy` closes the UDP driver. The CLI therefore does not use `destroy` for process-level QUIC cleanup. `odin_xqc_server_runtime_force_destroy` is an owner-thread API for non-callback cleanup points, including after `odin_event_loop_run` returns and before the loop is destroyed. `force_destroy(NULL)` is a no-op. For a non-null runtime, `force_destroy` returns only after all runtime-owned stream sessions, connection contexts, ALPN registration, XQC UDP state, XQUIC engine state, and the UDP socket have been released, even when connections or streams are still linked at shutdown. No XQUIC or Odin runtime callback may be active when it is called, and no callback may use `rt` after it returns. The CLI still does not close a UDP fd directly.

`runtime_finish_destroy` is a single-entry finalizer. It sets `rt->finish_destroy_in_progress = 1` before unregistering ALPN or destroying the UDP driver, and `runtime_maybe_finish_destroy` returns without action while that flag is set. This guard is required because `odin_xqc_udp_destroy` calls `xqc_engine_destroy`. XQUIC engine destruction can synchronously destroy streams before the connection close notification at [xquic/src/transport/xqc_conn.c](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/transport/xqc_conn.c:1538), and `xqc_destroy_stream` invokes `stream_close_notify` with the current XQUIC stream user-data at [xquic/src/transport/xqc_stream.c](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/transport/xqc_stream.c:787). It can also invoke the runtime's `stream_closing_notify`, `conn_close_notify`, or `server_refuse` callbacks before `odin_xqc_udp_destroy` returns. Those callbacks may enter and leave the runtime while `destroy_pending` is already set; callback leave must not recursively call `runtime_finish_destroy`, unregister ALPN twice, destroy the UDP driver twice, or free `rt` before the outer finalizer returns.

During `force_destroy`, the runtime sets `rt->force_destroy_active = 1` before it detaches any connection or stream context and keeps that flag set until the outer `runtime_finish_destroy` frees `rt`. `runtime_maybe_finish_destroy` also returns without action while `force_destroy_active` is set, so a synchronous callback from engine destruction cannot finish or free the runtime while `force_destroy` is inerting XQUIC-referenced state. For every linked stream, `force_destroy` first destroys the `odin_server_session`; that destroys the `odin_xqc_stream_transport`, whose destroy path clears the XQUIC stream user-data slot with `odin_xqc_stream_set_user_data_call(s->stream, NULL)` at [odin/transport_xqc.c](/Users/tangjiacheng/Downloads/Monorepo/odin/transport_xqc.c:267). For every linked connection, `force_destroy` then calls `runtime_conn_set_alp_user_data_call(ctx->conn, NULL)`, using the existing wrapper at [odin/server_xqc_runtime.c](/Users/tangjiacheng/Downloads/Monorepo/odin/server_xqc_runtime.c:367), so `runtime_conn_ctx_from_stream` cannot later recover that context through `xqc_get_conn_alp_user_data_by_stream`. Stream and connection contexts move to a private force-destroy pending-free list after those XQUIC user-data slots are cleared, and the pending-free list is drained only after `runtime_udp_destroy_call` returns.

While `force_destroy_active` is set, `runtime_server_accept` returns failure before allocating or registering a new connection context, `runtime_conn_create_notify` returns failure, and `runtime_conn_update_cid` returns before CID registration or connection close. A `runtime_stream_create_notify` that arrives after `force_destroy` has cleared connection ALP user data cannot recover the runtime context and follows the existing missing-context close-only path; this RFC does not add a separate `force_destroy_active` branch to that callback. `runtime_stream_close_notify` and `runtime_stream_closing_notify` are destroy-time no-ops for inerted streams: they must not dereference a freed Odin stream context, must not call an `odin_xqc_stream_transport_*_notify` helper with a stale non-null stream user-data pointer, and must not finish or free the runtime recursively. `runtime_conn_close_notify` and `runtime_server_refuse` may still run from XQUIC engine destruction, but they are cleanup-only: they unregister any still-registered CID for a still-linked context, mark the context final, and leave actual context freeing to the force-destroy pending-free drain after `runtime_udp_destroy_call` returns. Their `runtime_callback_leave` path observes `force_destroy_active` or `finish_destroy_in_progress` and cannot finish or free the runtime recursively.

The POSIX signal handler never calls XQUIC, event-loop, stdio, allocation, or close APIs. On macOS the polling timer is backed by the existing kqueue event-loop backend; on Linux it is backed by the existing timerfd event-loop backend; iOS and alternate-architecture macOS compile the same owner-thread contract but are not runtime-verified in this environment.

**Mechanism.**

```text
quic_cleanup(state):
  if state.signal_timer != NULL:
    odin_event_timer_stop(state.signal_timer)
    state.signal_timer = NULL
  if state.xqc_runtime != NULL:
    odin_xqc_server_runtime_force_destroy(state.xqc_runtime)
    state.xqc_runtime = NULL
  if state.loop != NULL:
    odin_event_loop_destroy(state.loop)
    state.loop = NULL
  restore_signal_handlers(state)

xqc_server_runtime_force_destroy(rt):
  if rt == NULL:
    return
  require owner thread and no active runtime callback entry
  rt.force_destroy_active = 1
  runtime_mark_destroy_pending(rt)  # stops UDP interest/timer state
  for each linked connection ctx:
    for each stream_ctx linked to ctx:
      destroy stream_ctx.ss, if present  # clears xqc stream user data
      stream_ctx.ss = NULL
      stream_ctx.transport = NULL
      unlink stream_ctx from runtime stream maps
      move stream_ctx to private force-destroy pending-free list
    runtime_conn_set_alp_user_data_call(ctx.conn, NULL)
    if ctx.cid_registered:
      runtime_udp_unregister_conn_call(rt.xu, &ctx.current_cid)
    if !ctx.destroy_close_requested:
      runtime_conn_close_call(odin_xqc_udp_engine(rt.xu), &ctx.current_cid)
    runtime_conn_ctx_unlink(ctx)
    move ctx to private force-destroy pending-free list
  runtime_finish_destroy(rt)  # guarded finalizer; unregisters ALPN, destroys UDP, drains pending-free lists

runtime_finish_destroy(rt):
  if rt.finish_destroy_in_progress:
    return
  rt.finish_destroy_in_progress = 1
  if rt.alpn_registered:
    runtime_engine_unregister_alpn_call(...)
    rt.alpn_registered = 0
  if rt.xu != NULL:
    runtime_udp_destroy_call(rt.xu)  # may synchronously run stream close/closing and conn close/refuse callbacks
    rt.xu = NULL
  free force-destroy pending stream contexts and connection contexts
  free(rt)

runtime_maybe_finish_destroy(rt):
  if rt.force_destroy_active or rt.finish_destroy_in_progress:
    return 0
  if rt.destroy_pending && rt.active_entries == 0 && rt.connections == NULL:
    runtime_finish_destroy(rt)
    return 1
  ...

signal_timer(loop, timer, state):
  if no signal has been observed:
    testing progress probes may run
    return
  state.shutdown_requested = 1
  state.signal_timer = NULL
  odin_event_timer_stop(timer)
  odin_event_loop_stop(loop)
```

Satisfies: G3 via deterministic failure lines and ordered cleanup; G4 via async-signal-safe signal observation, owner-thread stop, force-destroyed QUIC runtime/UDP state, event-loop destruction, and handler restoration.

#### 3.2.6 Test Hook and Scope-Check Contract

Extend the existing `ODIN_CLI_SERVER_TESTING` hook surface in `odin/testing/cli_server_internal_test.h`:

```c
#if defined(ODIN_CLI_SERVER_TESTING)

#include "odin/server_session.h"
#include "odin/server_xqc_runtime.h"

typedef enum odin_cli_server_test_failpoint_t {
  /* existing TCP failpoints keep their numeric values */
  ODIN_CLI_SERVER_TEST_FAIL_XQC_SERVER_RUNTIME_CREATE = 100,
  ODIN_CLI_SERVER_TEST_FAIL_XQC_SERVER_RUNTIME_START,
  ODIN_CLI_SERVER_TEST_FAIL_XQC_SERVER_RUNTIME_LOCAL_ADDR,
  ODIN_CLI_SERVER_TEST_FAIL_QUIC_EVENT_LOOP_RUN,
} odin_cli_server_test_failpoint_t;

typedef struct odin_cli_server_test_liveness_t {
  size_t live_listeners;
  size_t live_runtimes;
  size_t last_cleanup_runtime_inflight;
  size_t live_xqc_runtimes;
} odin_cli_server_test_liveness_t;

typedef struct odin_cli_server_test_filter_record_t {
  unsigned int tcp_set_count;
  unsigned int quic_set_count;
  odin_server_session_dial_filter_cb tcp_cb;
  odin_server_session_dial_filter_cb quic_cb;
  void *tcp_user_data;
  void *quic_user_data;
} odin_cli_server_test_filter_record_t;

int odin_cli_server_test_filter_record(
    odin_cli_server_test_filter_record_t *out);
int odin_cli_server_test_set_quic_start_probe(
    void (*cb)(odin_xqc_server_runtime_t *rt, void *user_data),
    void *user_data);

#endif /* defined(ODIN_CLI_SERVER_TESTING) */
```

T9 reuses the existing `ODIN_DIAL_TESTING && ODIN_CLI_SERVER_TESTING` dial-start probe from `odin/testing/cli_server_internal_test.h`:

```c
typedef struct odin_cli_server_test_dial_start_t {
  int family;
  uint32_t ipv4_addr_nbo;
  uint16_t port_nbo;
} odin_cli_server_test_dial_start_t;

void odin_cli_server_test_set_dial_start_probe_fd(int fd, int errnum);
int odin_cli_server_test_maybe_probe_dial_start(const struct sockaddr *addr,
                                                socklen_t addrlen);
```

The gated call site is the `ODIN_DIAL_TESTING && ODIN_CLI_SERVER_TESTING` branch at [odin/dial.c](/Users/tangjiacheng/Downloads/Monorepo/odin/dial.c:113), before `odin_dial_start` allocates an `odin_dial_t`. `//odin/testing:odin_unittests` enables this surface through `:odin_cli_server_testing_config` and `:odin_dial_testing_config`, which define `ODIN_CLI_SERVER_TESTING` and `ODIN_DIAL_TESTING`.

Extend `ODIN_XQC_SERVER_RUNTIME_TESTING` with a setter record and a call-record callback field:

```c
typedef enum odin_xqc_server_runtime_test_call_kind_t {
  /* existing values stay stable */
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_SET_DIAL_FILTER = 15,
} odin_xqc_server_runtime_test_call_kind_t;

typedef struct odin_xqc_server_runtime_test_call_t {
  /* existing fields stay stable, including void *user_data */
  odin_server_session_dial_filter_cb dial_filter_cb;
} odin_xqc_server_runtime_test_call_t;
```

The gated call site is inside `odin_xqc_server_runtime_set_dial_filter` at [odin/server_xqc_runtime.c](/Users/tangjiacheng/Downloads/Monorepo/odin/server_xqc_runtime.c:765). Under `ODIN_XQC_SERVER_RUNTIME_TESTING`, a non-null runtime appends exactly one `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_SET_DIAL_FILTER` record after the `rt == NULL` no-op guard and before storing `rt->dial_filter` or `rt->dial_filter_ud`. The record captures the raw setter arguments as `dial_filter_cb == cb` and `record.user_data == user_data_arg`; production storage still applies the existing clear rule after the record, `rt->dial_filter = cb` and `rt->dial_filter_ud = cb == NULL ? NULL : user_data_arg`. A NULL runtime emits no record.

The existing `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_ENGINE_REGISTER_ALPN` record from `odin/testing/server_xqc_runtime_internal_test.h` remains the CLI-path ALPN oracle: each call record carries `alpn` and `alpn_len`, so T8 can assert that the explicit QUIC CLI path registers exactly `ODIN_XQC_SERVER_ALPN` before UDP start.

T5 and T7 reuse the existing `ODIN_XQC_UDP_TESTING` hook surface from `odin/testing/xqc_udp_internal_test.h`:

```c
typedef struct odin_xqc_udp_test_ops_t {
  xqc_engine_t *(*engine_create)(xqc_engine_type_t engine_type,
                                 const xqc_config_t *engine_config,
                                 const xqc_engine_ssl_config_t *ssl_config,
                                 const xqc_engine_callback_t *engine_callback,
                                 const xqc_transport_callbacks_t *transport_cbs,
                                 void *user_data);
  void (*engine_destroy)(xqc_engine_t *engine);
} odin_xqc_udp_test_ops_t;

void odin_xqc_udp_test_set_ops(const odin_xqc_udp_test_ops_t *ops);
int odin_xqc_udp_test_udp(odin_xqc_udp_t *xu, odin_udp_t **out);
```

The gated create call site is `xqc_udp_engine_create_call` at [odin/xqc_udp.c](/Users/tangjiacheng/Downloads/Monorepo/odin/xqc_udp.c:49). During `odin_xqc_udp_create`, that wrapper is invoked after UDP bind and local-address capture with `user_data == xu` at [odin/xqc_udp.c](/Users/tangjiacheng/Downloads/Monorepo/odin/xqc_udp.c:431), which lets T5 record the bound UDP socket through `odin_xqc_udp_test_udp` before forcing `xqc_engine_create` failure. The gated destroy call site is `xqc_udp_engine_destroy_call` at [odin/xqc_udp.c](/Users/tangjiacheng/Downloads/Monorepo/odin/xqc_udp.c:65), invoked by `odin_xqc_udp_finish_destroy` before `free(xu)` at [odin/xqc_udp.c](/Users/tangjiacheng/Downloads/Monorepo/odin/xqc_udp.c:490), which lets T7 inject XQUIC stream closing/close callbacks, separate connection close/refuse final callbacks, reachable force-destroy guard callbacks, and the inerted stream-create close-only callback during force cleanup. `//odin/testing:odin_unittests` enables this surface through the existing `:odin_xqc_udp_testing_config` GN config, which defines `ODIN_XQC_UDP_TESTING`.

T7 also reuses the existing `ODIN_TRANSPORT_XQC_TESTING` stream user-data hook from `odin/testing/transport_xqc_internal_test.h`:

```c
typedef struct odin_xqc_stream_transport_test_ops_t {
  ssize_t (*recv)(xqc_stream_t *stream, unsigned char *recv_buf,
                  size_t recv_buf_size, uint8_t *fin);
  ssize_t (*send)(xqc_stream_t *stream, unsigned char *send_data,
                  size_t send_data_size, uint8_t fin);
  void (*set_user_data)(xqc_stream_t *stream, void *user_data);
} odin_xqc_stream_transport_test_ops_t;

void odin_xqc_stream_transport_test_set_ops(
    const odin_xqc_stream_transport_test_ops_t *ops);
```

The gated call site is `odin_xqc_stream_set_user_data_call` at [odin/transport_xqc.c](/Users/tangjiacheng/Downloads/Monorepo/odin/transport_xqc.c:85), invoked by `odin_xqc_stream_transport_create` to install the transport user-data at [odin/transport_xqc.c](/Users/tangjiacheng/Downloads/Monorepo/odin/transport_xqc.c:302) and by `odin_xqc_destroy` to clear it at [odin/transport_xqc.c](/Users/tangjiacheng/Downloads/Monorepo/odin/transport_xqc.c:267). T7's fake stream stores this slot through the hook and `ODIN_XQC_UDP_TESTING.engine_destroy` reads the current stored value before invoking the stream callbacks. `//odin/testing:odin_unittests` enables this surface through the existing `:odin_transport_xqc_testing_config` GN config, which defines `ODIN_TRANSPORT_XQC_TESTING`.

**Unstated contract.** Production builds do not declare or define these symbols. `odin_cli_server_test_fail_next(fp, errnum)` returns `-1/EINVAL` and arms nothing when `errnum <= 0` or `fp` is not one of the existing TCP failpoints or the new QUIC failpoints listed above. On success it stores exactly one process-local failpoint, replacing any previously armed failpoint. `test_consume_failpoint(fp)` clears the stored failpoint and errno before returning failure to the production call site; a nonmatching call site leaves it armed. `odin_cli_server_test_reset_liveness` clears liveness counters, filter records, progress/probe fds, the QUIC start probe, and any pending failpoint.

The CLI filter record is written from the same production call site that invokes the TCP or QUIC runtime setter, then the real runtime setter is still called. The QUIC start probe runs on the owner thread after the QUIC runtime exists, the default filter has been installed, `odin_xqc_server_runtime_start` has succeeded, `odin_xqc_server_runtime_local_addr` has reported the bound port, and before signal handlers, the signal timer, and the success banner are installed. The start probe is for inspection or deterministic injection of fake `ODIN_XQC_SERVER_RUNTIME_TESTING` connection/stream callbacks; it must not be documented or used as a loop-stop mechanism because `odin_event_loop_run` clears `stop_requested` on entry. `odin_cli_server_test_set_quic_start_probe(NULL, user_data)` clears the probe and ignores `user_data`; a non-null callback is consumed once and cleared immediately before invocation. `live_xqc_runtimes` increments only after `odin_xqc_server_runtime_create` succeeds and decrements immediately after `odin_xqc_server_runtime_force_destroy` returns in CLI cleanup.

The current `odin_server_xqc_runtime_scope_check` was written to keep the TCP CLI separate from XQUIC; this RFC narrows it so `odin/server_runtime.c`, `odin/server_runtime.h`, and the `odin_server_runtime` GN target stay free of XQUIC references, while `odin/cli_server.c` and the `odin_cli_server` target may include and depend on `odin_server_xqc_runtime` for the explicit QUIC mode.

Failpoint consumption sites:

| Failpoint | Consumed at | Failure line |
|-----------|-------------|--------------|
| `ODIN_CLI_SERVER_TEST_FAIL_EVENT_LOOP_CREATE` | immediately before `odin_event_loop_create(&state.loop)` in the selected server branch | `event_loop_create` |
| `ODIN_CLI_SERVER_TEST_FAIL_XQC_SERVER_RUNTIME_CREATE` | immediately before `odin_xqc_server_runtime_create(&rt_config, &state.xqc_runtime)` | `xqc_server_runtime_create` |
| `ODIN_CLI_SERVER_TEST_FAIL_XQC_SERVER_RUNTIME_START` | immediately before `odin_xqc_server_runtime_start(state.xqc_runtime)` | `xqc_server_runtime_start` |
| `ODIN_CLI_SERVER_TEST_FAIL_XQC_SERVER_RUNTIME_LOCAL_ADDR` | immediately before `odin_xqc_server_runtime_local_addr(state.xqc_runtime, &bound, &bound_len)` | `xqc_server_runtime_local_addr` |
| `ODIN_CLI_SERVER_TEST_FAIL_SIGACTION_SIGINT` | inside `install_signal_handlers`, immediately before `sigaction(SIGINT, ...)` | `sigaction(SIGINT)` |
| `ODIN_CLI_SERVER_TEST_FAIL_SIGACTION_SIGTERM` | inside `install_signal_handlers`, immediately before `sigaction(SIGTERM, ...)` | `sigaction(SIGTERM)` |
| `ODIN_CLI_SERVER_TEST_FAIL_SIGNAL_TIMER_START` | immediately before `odin_event_timer_start(..., cli_signal_poll_timer, ...)` | `signal_timer_start` |
| `ODIN_CLI_SERVER_TEST_FAIL_QUIC_EVENT_LOOP_RUN` | after the success banner is printed and immediately before `odin_event_loop_run(state.loop)` would be called | `event_loop_run` |

**Mechanism.**

```text
test_fail_next(fp, errnum):
  if fp invalid or errnum <= 0:
    errno = EINVAL
    return -1
  g_failpoint = fp
  g_failpoint_errno = errnum
  return 0

test_consume_failpoint(fp):
  if g_failpoint != fp:
    return 0
  err = g_failpoint_errno
  clear g_failpoint/g_failpoint_errno
  errno = err
  return -1

record_and_set_tcp_filter(rt, cb, user_data):
  if testing: record tcp_cb/counter/user_data
  odin_server_runtime_set_dial_filter(rt, cb, user_data)

record_and_set_quic_filter(rt, cb, user_data):
  if testing: record quic_cb/counter/user_data
  odin_xqc_server_runtime_set_dial_filter(rt, cb, user_data)

runtime_set_dial_filter(rt, cb, user_data_arg):
  if rt == NULL:
    return
  if testing: append SET_DIAL_FILTER with dial_filter_cb = cb and user_data = user_data_arg
  rt.dial_filter = cb
  rt.dial_filter_ud = cb == NULL ? NULL : user_data_arg

quic_start_probe_point(state):
  if testing probe is armed:
    cb = probe_cb
    user_data = probe_user_data
    clear probe_cb/probe_user_data
    cb(state.xqc_runtime, user_data)
```

Satisfies: G1 via scope-check preservation for the TCP runtime; G3 and G4 via deterministic liveness and failpoint evidence; G5 via production-call-site filter records and QUIC runtime setter records.

## 4. Security

- **S1.**
  - **Threat:** Server-Side Request Forgery through peer-supplied CONNECT_REQ host and port bytes. A TCP or QUIC peer can request `127.0.0.1`, `10.0.0.1`, `169.254.169.254`, or another non-public IPv4 destination; without the CLI default policy, the server-session path would pass that decoded `sockaddr_in` to `odin_dial_start`.
  - **Mitigation:** §3.2.4 installs the same `odin_cli_default_server_dial_filter` on both the TCP and QUIC runtimes before either runtime can process peer CONNECT_REQ bytes. The filter returns `EACCES` for the non-public ranges listed in §3.2.4 and `EAFNOSUPPORT` for non-IPv4 or malformed sockaddr input. Existing `odin_server_session` enforcement remains the code point that consults the callback before outbound dial.
  - **Enforcement:** §5 rows T8 and T9. T8 asserts the shared callback and full deny/allow matrix. T9 drives peer-supplied CONNECT_REQ bytes through the QUIC runtime stream path with the CLI callback installed, asserts that a loopback target produces the existing denied CONNECT_RESP without any outbound dial, and asserts that a concrete public IPv4 target reaches the existing dial boundary.

The QUIC certificate and key paths are operator-supplied process arguments, not peer-supplied bytes. This RFC does not add a remote path-selection surface: peers cannot choose which file XQUIC opens.

## 5. Testing Strategy

Rows that spawn or fork a running server use the same deadline discipline as the RFC-022 CLI-server tests: stderr pipes, progress pipes, UDP bind probes, client sockets, and child-exit checks use `poll` / `select` or socket deadlines, never an unbounded blocking read while a child is alive. Fixtures install `signal(SIGPIPE, SIG_IGN)` before any write-after-close or socket EOF probe. Test-side threads do not call owner-thread-only event-loop APIs; stop is requested through process signals or owner-thread test hooks.

`odin/testing/certs/odin_quic_test.crt` and `odin/testing/certs/odin_quic_test.key` are committed PEM fixtures used only by tests. Invalid TLS-path rows use nonexistent paths under the test temp directory, not missing CLI arguments, because missing/empty arguments are parser errors covered by T2.

### 5.0 Coverage Matrix

Only reachable cells introduced or branched by this RFC are listed. This RFC adds no new wire decoder, no `s.write_off` / `s.buf_used` completion branch, and no new read/write paired state machine; those existing server-session and XQUIC stream contracts remain covered by RFC-020 and RFC-025 tests.

| Axis | Value | Rows |
|------|-------|------|
| G# | G1 TCP default preserved | T1, T8 |
| G# | G2 explicit QUIC TLS + UDP startup | T2, T3, T8, T10 |
| G# | G3 deterministic errors and cleanup | T2, T4, T5, T6, T10 |
| G# | G4 graceful QUIC signal cleanup | T3, T6, T7 |
| G# | G5 shared default dial filter | T8, T9 |
| S# | S1 SSRF denial before outbound dial | T8, T9 |
| State | Parser: server default or `--transport tcp` | T1, T2 |
| State | Parser: valid QUIC with both TLS paths | T2 |
| State | Parser: invalid transport value | T2 |
| State | Parser: QUIC TLS missing/empty or supplied for TCP | T2 |
| State | Parser: `ODIN_CLI_ERR_BAD_LISTEN_PORT` precedence over `ODIN_CLI_ERR_BAD_TRANSPORT` / `ODIN_CLI_ERR_BAD_QUIC_TLS` | T2 |
| State | Parser: exact long-option spelling rejects abbreviations | T2 |
| State | Runtime: TCP branch selected | T1, T8 |
| State | Runtime: QUIC create/start/local-addr/banner | T3, T10 |
| State | Runtime: QUIC registers `ODIN_XQC_SERVER_ALPN` before UDP start | T8 |
| State | Runtime: QUIC UDP bind collision inside runtime create | T4 |
| State | Runtime: QUIC TLS file-load failure after UDP bind | T5 |
| State | Runtime: QUIC generic setup and event-loop failures | T6 |
| State | Runtime: QUIC signal observed after successful startup | T7 |
| State | Runtime: QUIC `force_destroy(NULL)` no-op | T6 |
| State | Runtime: QUIC force cleanup releases UDP with live connection/stream | T7 |
| State | Runtime: QUIC engine-destroy stream close/closing callbacks during force cleanup cannot use freed Odin stream context | T7 |
| State | Runtime: QUIC engine-destroy `runtime_conn_close_notify` final callback during force cleanup cannot re-enter final destroy or free a pending context early | T7 |
| State | Runtime: QUIC engine-destroy `runtime_server_refuse` final callback during force cleanup cannot re-enter final destroy or free a pending context early | T7 |
| State | Runtime: QUIC `force_destroy_active` guards for `runtime_server_accept`, `runtime_conn_create_notify`, and `runtime_conn_update_cid` prevent new context/CID work | T7 |
| State | Runtime: QUIC inerted `runtime_stream_create_notify` after ALP user-data clear closes the stream without transport/session state | T7 |
| State | Runtime: QUIC public IPv4 CONNECT_REQ reaches dial boundary | T9 |
| Benign-vs-fatal split | Signal timer observes no signal and keeps process running | T7 |
| Benign-vs-fatal split | Signal timer observes signal and stops owner-thread loop | T7 |
| Constructor / factory precondition | CLI runner config/TLS validation | T2, T6 |
| Constructor / factory precondition | Local-address accessor NULL and short-buffer validation | T10 |
| Post-syscall sub-branch | UDP bind succeeds, then XQUIC TLS context creation fails | T5 |
| Post-syscall sub-branch | Runtime create succeeds, then runtime start fails | T6 |
| Post-syscall sub-branch | Runtime create succeeds, then local-address accessor fails | T6, T10 |
| Callback-safe lifecycle hand-off | Owner-thread signal timer performs cleanup-triggering stop | T7 |
| Test hook contract | CLI failpoint validation, reset, and consumption sites | T6 |
| Test hook contract | XQC runtime set-dial-filter record call site, captured callback/user data, and ordering before UDP start | T8 |
| Test hook contract | XQC runtime set-dial-filter NULL receiver emits no `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_SET_DIAL_FILTER` record | T8 |

T8 uses this callback matrix against the recorded TCP and QUIC callbacks, not against a test-only mirror:

```text
denied:
  0.0.0.0, 0.255.255.255,
  10.0.0.0, 10.1.2.3, 10.255.255.255,
  100.64.0.0, 100.127.255.255,
  127.0.0.0, 127.0.0.1, 127.255.255.255,
  169.254.0.0, 169.254.1.1, 169.254.169.254, 169.254.255.255,
  172.16.0.0, 172.31.255.255,
  192.0.0.0, 192.0.0.1, 192.0.0.255,
  192.0.2.0, 192.0.2.1, 192.0.2.255,
  192.168.0.0, 192.168.0.1, 192.168.255.255,
  198.18.0.0, 198.19.255.255,
  198.51.100.0, 198.51.100.1, 198.51.100.255,
  203.0.113.0, 203.0.113.1, 203.0.113.255,
  224.0.0.0, 224.0.0.1, 239.255.255.255,
  240.0.0.0, 240.0.0.1, 255.255.255.255
allowed:
  1.0.0.0, 9.255.255.255, 11.0.0.0,
  100.63.255.255, 100.128.0.0,
  126.255.255.255, 128.0.0.0,
  169.253.255.255, 169.255.0.0,
  172.15.255.255, 172.32.0.0,
  191.255.255.255, 192.0.1.0, 192.0.1.255, 192.0.3.0,
  192.167.255.255, 192.169.0.0,
  198.17.255.255, 198.20.0.0,
  198.51.99.255, 198.51.101.0,
  203.0.112.255, 203.0.114.0,
  223.255.255.255, 8.8.8.8, 93.184.216.34
malformed:
  NULL addr, short IPv4 sockaddr, AF_INET6 sockaddr
```

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | TCP default remains the server default | Parser subcases: `odin-server`, `odin-server --listen ""`, `odin-server --listen 0`, and `odin-server --transport tcp --listen 0`. Process subcase: spawn host `odin-server --listen 0`, read the first stderr line, connect a TCP client to the reported port, then send `SIGTERM` | Every parser subcase returns `ODIN_CLI_OK`, `server_transport == ODIN_CLI_SERVER_TRANSPORT_TCP`, and both QUIC path fields are `NULL`. `odin-server` and `odin-server --listen ""` report `listen_port == ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER` (`4433`). `odin-server --listen 0` and `odin-server --transport tcp --listen 0` report `listen_port == 0`. Process subcase first stderr line is `odin: mode=server listen=<P>\n` with `P > 0`, TCP connect to `127.0.0.1:P` succeeds, no `transport=tcp` token is printed, and child exits `0` after `SIGTERM` | G1 | e2e |
| T2 | QUIC parser and usage contract | Valid argv with distinct `argv` entries `cert_arg = "C"` and `key_arg = "K"`: `odin-server --transport quic --listen 9443 --quic-cert cert_arg --quic-key key_arg`; invalid transport values `udp`, `QUIC`, and empty string; QUIC missing either path; empty `--quic-cert` or `--quic-key`; explicit TCP with TLS paths: `--transport tcp --quic-cert C --quic-key K`; default TCP with TLS paths: `odin-server --quic-cert C --quic-key K`, `odin-server --listen 0 --quic-cert C`, and `odin-server --listen 0 --quic-key K`; mixed-error precedence cases `odin-server --listen abc --transport udp`, `odin-server --listen abc --transport quic --quic-cert C`, and `odin-server --listen abc --transport tcp --quic-cert C --quic-key K`; exact-spelling rejection cases `--trans quic`, `--quic-ce C`, and `--quic-ke K`; client invocations with `--quic-cert C` and `--quic-key K`; server help | Valid QUIC returns `ODIN_CLI_OK`, `listen_port == 9443`, `server_transport == ODIN_CLI_SERVER_TRANSPORT_QUIC`, `quic_cert_file == cert_arg`, and `quic_key_file == key_arg`, proving pointer identity to the original `argv` elements rather than copied strings. Invalid transport values return `ODIN_CLI_ERR_BAD_TRANSPORT`. Missing/empty QUIC TLS values and TLS paths supplied for explicit or default TCP return `ODIN_CLI_ERR_BAD_QUIC_TLS`. Mixed-error precedence cases return `ODIN_CLI_ERR_BAD_LISTEN_PORT`; through `odin_cli_main` they write exactly `odin: invalid --listen port\n<U_S>\n` to stderr, write nothing to stdout, and return `2`. Abbreviated long options and client QUIC flags return `ODIN_CLI_ERR_UNKNOWN_FLAG`. `odin_cli_main` writes the exact new server usage line for help and returns `0`; for `BAD_TRANSPORT` and `BAD_QUIC_TLS`, it writes the two new deterministic error lines to stderr, writes nothing to stdout, and returns `2` | G2, G3 | unit |
| T3 | QUIC startup binds UDP and reports actual port | Spawn host `odin-server --transport quic --listen 0 --quic-cert odin/testing/certs/odin_quic_test.crt --quic-key odin/testing/certs/odin_quic_test.key`; deadline-read the first stderr line; while the child is alive, attempt to bind a new UDP socket to `0.0.0.0:<P>` without `SO_REUSEADDR`; then send `SIGTERM` and wait | First stderr line is `odin: mode=server transport=quic listen=<P>\n` with `P > 0`; the parent UDP bind to the same `0.0.0.0:P` fails with `EADDRINUSE` while the child is alive; child exits `0`; stdout is empty; stderr contains no `odin: quic server startup failed` line | G2, G4 | e2e |
| T4 | QUIC UDP bind collision reports startup failure | Parent binds a UDP socket on `0.0.0.0:0`, records port `P`, then spawns `odin-server --transport quic --listen P --quic-cert odin/testing/certs/odin_quic_test.crt --quic-key odin/testing/certs/odin_quic_test.key` | Child exits `1`; stderr contains exactly `odin: quic server startup failed at xqc_server_runtime_create\n` plus no startup banner; stdout is empty; the parent UDP socket remains bound and usable after the child returns | G3 | e2e |
| T5 | QUIC TLS and post-bind engine failures clean up UDP binding | Subcase A: spawn `odin-server --transport quic --listen 0 --quic-cert <nonexistent.crt> --quic-key <nonexistent.key>` through the forked test fixture that snapshots liveness before child exit. Subcase B: in `odin_unittests`, install an `ODIN_XQC_UDP_TESTING` `engine_create` hook that receives the `odin_xqc_udp_t *` user data, records the bound UDP port through `odin_xqc_udp_test_udp` plus `odin_udp_local_addr`, sets `errno = EIO`, returns `NULL`, and pauses the child after `odin_cli_main` returns but before process exit | Both subcases return `1`; stdout is empty; stderr is exactly `odin: quic server startup failed at xqc_server_runtime_create\n`. Liveness reports `live_xqc_runtimes == 0` and event-loop counters all zero through side-channel snapshots, not stderr parsing. In subcase B, a parent UDP bind to the recorded side-channel port succeeds while the child is paused before exit, proving cleanup released the UDP socket after UDP bind succeeded and before process teardown | G3 | integration |
| T6 | QUIC setup and runtime failure cleanup matrix | In `odin_unittests`, reset CLI/event-loop liveness, call `odin_xqc_server_runtime_test_reset()`, bind a sentinel UDP socket to `0.0.0.0:0`, call `odin_xqc_server_runtime_force_destroy(NULL)`, and keep the sentinel open through the assertion. Then call `odin_cli_run_server(NULL, err)`, `odin_cli_run_server(&cfg_with_unknown_transport, err)`, `odin_cli_run_server(&cfg_quic_missing_cert, err)`, `odin_cli_run_server(&cfg_quic_missing_key, err)`, and `odin_cli_run_server(&cfg_quic_empty_cert_or_key, err)` using `fmemopen` for `err`; call `odin_cli_server_test_fail_next` for invalid failpoint and zero errno; preselect UDP port `P` by briefly binding and closing a UDP socket; then run `odin_cli_main` as QUIC with valid test cert/key, `--listen P`, and one valid failpoint at a time: `ODIN_CLI_SERVER_TEST_FAIL_EVENT_LOOP_CREATE`, `ODIN_CLI_SERVER_TEST_FAIL_XQC_SERVER_RUNTIME_CREATE`, `ODIN_CLI_SERVER_TEST_FAIL_XQC_SERVER_RUNTIME_START`, `ODIN_CLI_SERVER_TEST_FAIL_XQC_SERVER_RUNTIME_LOCAL_ADDR`, `ODIN_CLI_SERVER_TEST_FAIL_SIGACTION_SIGINT`, `ODIN_CLI_SERVER_TEST_FAIL_SIGACTION_SIGTERM`, `ODIN_CLI_SERVER_TEST_FAIL_SIGNAL_TIMER_START`, and `ODIN_CLI_SERVER_TEST_FAIL_QUIC_EVENT_LOOP_RUN`. No failpoint subcase uses `--listen 0`. Post-banner failures use the fork/deadline fixture | The direct `force_destroy(NULL)` subcase returns without a crash, leaves `live_xqc_runtimes` and event-loop liveness counters unchanged at zero, leaves `odin_xqc_server_runtime_test_record()->call_count == 0` and `udp_create_calls == 0`, and leaves the sentinel UDP socket bound: a duplicate bind to its port fails with `EADDRINUSE` while the sentinel is open, then succeeds after the sentinel is closed. The direct null-config and unknown-transport calls return `1`, print exactly `odin: server startup failed at config\n`, and leave liveness counters unchanged. The direct QUIC missing/empty TLS calls return `1`, print exactly `odin: quic server startup failed at tls_config\n`, create no socket, event loop, or runtime, and leave liveness counters unchanged. Invalid failpoint calls return `-1/EINVAL` and arm nothing. Every valid setup failpoint returns `1` and prints the exact §3.2.5 failure line for that step; the event-loop-run failpoint prints the success banner first and then `odin: quic server runtime failed at event_loop_run\n`. After each failpoint subcase, `live_xqc_runtimes == 0`, event-loop liveness counters are zero, replaced signal handlers are restored, and a fresh UDP bind to `0.0.0.0:P` succeeds, so a leaked actual QUIC socket on the test port is detected | G3, G4 | unit |
| T7 | QUIC SIGINT/SIGTERM graceful cleanup and handler restoration | For each delivered signal in `{SIGINT, SIGTERM}`, fork baseline, live-stream, and engine-destroy-matrix child subcases. Each child installs custom counting handlers for `SIGINT` and `SIGTERM`, resets CLI/event-loop liveness, starts `odin_cli_main` as QUIC with valid test cert/key and `--listen 0`, and writes a progress byte from the owner-thread signal timer after startup. The live-stream subcase also arms `odin_cli_server_test_set_quic_start_probe`; the probe uses the `ODIN_XQC_SERVER_RUNTIME_TESTING` fake callback harness to accept one fake connection, create one bidirectional stream whose CONNECT_REQ is still incomplete, asserts the stream user data is non-null, and writes a live-stream progress byte before the banner. The engine-destroy matrix, also executed by the P3 ASan command, arms the same start probe plus `ODIN_XQC_UDP_TESTING.engine_destroy` and runs six variants: final `runtime_conn_close_notify`, final `runtime_server_refuse`, guarded `runtime_server_accept`, guarded `runtime_conn_create_notify`, guarded `runtime_conn_update_cid`, and inerted `runtime_stream_create_notify`. For the two final-callback variants, the probe accepts one fake connection, creates one bidirectional stream whose CONNECT_REQ is still incomplete, records the registered app/transport callbacks and fake stream object, asserts the stream user-data slot is non-null before signal delivery, and records the final-callback inputs; fake engine destroy reads the fake stream's current user-data slot, invokes `stream_closing_notify(stream, XQC_ESTREAM_RESET, stream_user_data)`, invokes `stream_close_notify(stream, stream_user_data)`, and then invokes only that variant's final callback while `odin_xqc_server_runtime_force_destroy` is inside `runtime_udp_destroy_call`. For the three guarded variants, fake engine destroy invokes the named callback while `force_destroy_active` is set, using a fresh fake connection or CID that was not linked before cleanup. For the inerted stream-create variant, fake engine destroy invokes `runtime_stream_create_notify` after the connection ALP user-data clear and with the ALP lookup returning `NULL`, so it exercises the no-context close-only path rather than a separate force-destroy guard. Parent reads the startup line and required progress bytes with deadlines, verifies the child has not exited, sends the parameterized delivered signal, then waits for the child to snapshot liveness, raise `SIGINT` and `SIGTERM` once each after `odin_cli_main` returns, write counters and return code, and pause on a release pipe | Before the delivered signal, the child is still inside `odin_cli_main`, proving the no-signal timer arm did not stop the loop; in the live-stream subcase, the parent also receives the probe byte proving a connection and stream were linked before signal delivery. For both delivered signals and all subcases, return code is `0`; child remains alive long enough for parent to observe that a new UDP bind to the reported port succeeds before process exit; custom handler counters increment exactly once each after `odin_cli_main` returns; `live_xqc_runtimes == 0`; event-loop liveness counters are zero; stderr contains the startup line and no failure line. The two final-callback engine-destroy variants each record that stream closing and stream close callbacks ran before the final callback, that the fake stream user-data slot is `NULL` when fake engine destroy reads it, that the runtime test record contains a `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_SET_ALP_USER_DATA` entry with `user_data == NULL` before `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_DESTROY`, and that exactly one outer final-destroy path ran: one UDP destroy, one ALPN unregister, one runtime liveness decrement, no recursive `runtime_finish_destroy` call, and no AddressSanitizer double-free or use-after-free report in the P3 ASan run. The `runtime_conn_close_notify` variant records no `runtime_server_refuse` invocation, and the `runtime_server_refuse` variant records no `runtime_conn_close_notify` invocation. The guarded variants assert the documented branch result: `runtime_server_accept` and `runtime_conn_create_notify` return failure, and `runtime_conn_update_cid` returns without registering, unregistering, or closing a CID. Across the guarded variants, the runtime record has no new `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_REGISTER_CONN`, no non-null `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_SET_ALP_USER_DATA`, no extra `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_CLOSE`, and the one outer final-destroy path still runs exactly once. The inerted stream-create variant returns `XQC_OK`, records exactly one `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_CLOSE` for the stream, records no stream user-data install, and records no transport/session allocation side effect. A cleanup path that calls deferred `odin_xqc_server_runtime_destroy` and leaves the UDP driver alive with the linked stream fails the live-stream subcase's rebind assertion; a force-destroy path that frees connection or stream context while XQUIC stream callbacks can still recover it fails the final-callback variants under ASan; a force-destroy path that omits one documented guard or the inerted stream-create close-only behavior fails its corresponding engine-destroy-matrix variant through the added record and return-value assertions | G4 | integration |
| T8 | TCP and QUIC install the same default filter before serving | TCP subcase: run `odin_cli_main` as TCP with `ODIN_CLI_SERVER_TEST_FAIL_SIGNAL_TIMER_START` so it reaches the TCP filter install and then fails before blocking. QUIC subcase: run `odin_cli_main` as QUIC with `ODIN_CLI_SERVER_TEST_FAIL_SIGNAL_TIMER_START` so it reaches the QUIC filter install, starts the QUIC runtime, records ALPN registration and UDP start, discovers the local address, and then fails before the success banner. Read `odin_cli_server_test_filter_record`; invoke both recorded callbacks with every address in the T8 callback matrix above. NULL-receiver subcase: after the CLI QUIC record assertions, call `odin_xqc_server_runtime_test_reset()`, set `sentinel_ud` to a non-null pointer, call `odin_xqc_server_runtime_set_dial_filter(NULL, quic_cb, sentinel_ud)`, and read `odin_xqc_server_runtime_test_record()` | `tcp_set_count == 1`, `quic_set_count == 1`, both CLI-record `user_data` fields are `NULL`, and the recorded callback pointers compare equal in the test binary. Both callbacks return `EACCES` for every denied matrix address, `0` for every allowed matrix address, and `EAFNOSUPPORT` for each malformed matrix input. The QUIC runtime test record has exactly one `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_ENGINE_REGISTER_ALPN` whose `alpn_len == sizeof(ODIN_XQC_SERVER_ALPN) - 1` and whose `alpn` bytes are byte-exact `ODIN_XQC_SERVER_ALPN`, exactly one `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_SET_DIAL_FILTER` whose `dial_filter_cb == quic_cb` and `user_data == NULL`, and at least one `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_START`. The ENGINE_REGISTER_ALPN and SET_DIAL_FILTER record indexes are both lower than the first UDP_START record index, which is before the success banner by §3.2.2 ordering. In the NULL-receiver subcase, `odin_xqc_server_runtime_test_record()->call_count == 0`, so no `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_SET_DIAL_FILTER` record is appended for the NULL receiver | G1, G2, G5, S1 | unit |
| T9 | QUIC CONNECT_REQ policy reaches deny and public-allow stream paths | Capture the CLI default filter through T8's production call-site path. Create an `odin_xqc_server_runtime_t` with the server-XQC runtime test harness and install the captured callback with `odin_xqc_server_runtime_set_dial_filter`. Deny subcase: accept a fake bidirectional xqc stream, feed a CONNECT_REQ for `127.0.0.1:<U>` where `U` is a parent-owned loopback TCP listener, and fire runtime read/write callbacks until the response flushes. Public subcase: arm `odin_cli_server_test_set_dial_start_probe_fd(probe_fd, ETIMEDOUT)`, accept a fresh fake bidirectional xqc stream, feed a CONNECT_REQ for `93.184.216.34:443`, and fire callbacks until the synthetic failure response flushes | Deny subcase fake stream send log begins with denied CONNECT_RESP bytes `{0x01, 0x02, 0x00, 0x04}` and the loopback listener accepts zero connections within its 200 ms deadline. Public subcase reads exactly one `odin_cli_server_test_dial_start_t` record with `family == AF_INET`, `ipv4_addr_nbo == htonl(0x5DB8D822)`, and `port_nbo == htons(443)`, then the fake stream send log begins with synthetic `ETIMEDOUT` CONNECT_RESP bytes `{0x01, 0x02, 0x00, 0x03}`. Both subcases enter through runtime stream callbacks with the captured CLI callback installed; no test-only mirror calls bypass the QUIC stream path | G5, S1 | integration |
| T10 | QUIC UDP and runtime local-address accessor validation | Direct unit tests create an `odin_xqc_udp_t` with local `0.0.0.0:0` and fake XQUIC engine hooks, then call `odin_xqc_udp_local_addr` with a full `sockaddr_storage`, with `xu == NULL`, with `addr == NULL`, with `addrlen == NULL`, and with `*addrlen == sizeof(struct sockaddr_in) - 1`. A second subcase creates an `odin_xqc_server_runtime_t` over the same UDP path and calls `odin_xqc_server_runtime_local_addr` with the same full-buffer, null-argument, and short-buffer inputs | For each accessor, the full buffer call returns `0`, reports `AF_INET`, reports a nonzero ephemeral port, and sets `addrlen == sizeof(struct sockaddr_in)`. NULL arguments return `-1/EINVAL`. Short buffer returns `-1/ENOBUFS`, updates `addrlen` to `sizeof(struct sockaddr_in)`, and leaves the caller buffer's sentinel bytes unchanged, proving both the public UDP accessor and the runtime wrapper enforce the same contract | G2, G3 | unit |

## 6. Implementation Plan

- **P1. Land the QUIC CLI contract surface and red-gated `T1`-`T10`.**
  - **Scope:** add the `odin_cli_server_transport_t`, parser fields, and new status enum names from §3.2.1; change `odin_cli_run_server` to accept `odin_cli_server_config_t` but keep the TCP body as the only implemented branch; add linkable stubs for `odin_xqc_udp_local_addr`, `odin_xqc_server_runtime_local_addr`, and `odin_xqc_server_runtime_force_destroy` that compile but do not satisfy T10 or T7's live-stream cleanup assertion; add the load-bearing `ODIN_CLI_SERVER_TESTING` hook declarations and includes from §3.2.6 with no-op records; add `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_SET_DIAL_FILTER` and the `dial_filter_cb` field to the test record shape without production recording; keep `//odin/testing:odin_unittests` configured with `:odin_xqc_udp_testing_config`, `:odin_xqc_server_runtime_testing_config`, `:odin_transport_xqc_testing_config`, `:odin_cli_server_testing_config`, and `:odin_dial_testing_config` so T5/T7 can use XQC UDP, server-runtime, and stream-user-data hooks and T9 can use the existing dial-start probe; add `odin/testing/certs/odin_quic_test.crt` and `odin/testing/certs/odin_quic_test.key`; add `OdinCliServerQuic*` / `OdinXqcUdpLocalAddrTest.*` / `OdinXqcServerRuntimeLocalAddrTest.*` rows T1-T10 gated by `ODIN_CLI_SERVER_QUIC_RED=1`; update the server usage test expectations behind the same gate so the default suite remains green; keep the current `odin_server_xqc_runtime_scope_check` behavior until P3.
  - **Depends on:** None.
  - **Done when:** `./tool/gn gen out/cli_server_quic_mac --args='target_os="mac"'`, `./tool/gn gen out/cli_server_quic_mac_arm64 --args='target_os="mac" target_cpu="arm64"'`, `./tool/gn gen out/cli_server_quic_linux_x64 --args='target_os="linux" target_cpu="x64"'`, `./tool/gn gen out/cli_server_quic_ios_sim --args='target_os="ios" target_environment="simulator" target_cpu="arm64"'`, and `./tool/gn gen out/cli_server_quic_ios_device --args='target_os="ios" target_environment="device" target_cpu="arm64"'` resolve. `./tool/ninja -C out/cli_server_quic_mac odin_main odin_unittests tests` builds, and matching `odin_main` and `odin_unittests` targets build in the four cross-compile output directories. The red-verification command `ODIN_CLI_SERVER_QUIC_RED=1 out/cli_server_quic_mac/odin_unittests --gtest_filter='OdinCliServerQuic*:OdinXqcUdpLocalAddrTest.*:OdinXqcServerRuntimeLocalAddrTest.*'` executes T1-T10 and fails them against the skeleton: T1 because the row now asserts all preserved TCP parser values, including `ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER` (`4433`) for omitted and empty `--listen`, `listen_port == 0` for explicit `--listen 0`, accepted `--transport tcp --listen 0`, TCP transport selection, and null QUIC paths; T2 because QUIC flags still parse as unknown or do not enforce TLS, `odin_cli_main` does not yet return `2` for the new statuses, and the mixed-error cases still fail to assert `ODIN_CLI_ERR_BAD_LISTEN_PORT` precedence over the new transport/TLS errors; T3-T7 because the QUIC branch is not implemented; T8 because filter records are empty and no byte-exact ENGINE_REGISTER_ALPN plus captured-callback SET_DIAL_FILTER-before-UDP_START record exists, while the same red-gated row also runs the reset-plus-NULL-receiver no-record assertion; T9 because no captured CLI callback can be installed into the QUIC runtime stream path; T10 because the UDP and runtime accessor stubs return failure. The default host run `out/cli_server_quic_mac/odin_unittests --gtest_brief=1` reports T1-T10 skipped and exits zero with pre-existing Odin tests green. Host-runnable enumeration: T1-T10 red assertions run only in `out/cli_server_quic_mac/odin_unittests` on the host macOS architecture. Cross-compile-only enumeration: the macOS arm64, Linux x64, iOS simulator, and iOS device `odin_unittests` binaries are built but not executed; Linux timerfd, Linux UDP socket, iOS socket, and alternate-architecture branches are compile-verified only in P1.

- **P2. Implement parser semantics and bound-address accessors; turn T1, T2, and T10 green.**
  - **Scope:** implement §3.2.1 parsing for `--transport`, `--quic-cert`, and `--quic-key`; update `odin_cli_main` status mapping and server usage text; keep `odin-server` default and `--transport tcp` on the existing TCP runner with the current TCP banner; implement `odin_xqc_udp_local_addr` and `odin_xqc_server_runtime_local_addr` per §3.2.3; remove the red gate from T1, T2, and T10 while leaving T3-T9 gated until the QUIC runner exists.
  - **Depends on:** P1.
  - **Done when:** the P1 build commands still succeed. `out/cli_server_quic_mac/odin_unittests --gtest_filter='OdinCliServerQuicParserTest.*:OdinXqcUdpLocalAddrTest.*:OdinXqcServerRuntimeLocalAddrTest.*'` passes T1, T2, and T10 un-gated on the host macOS architecture, including T2's mixed-error assertions that `--listen abc` returns `ODIN_CLI_ERR_BAD_LISTEN_PORT` before invalid `--transport` or QUIC TLS branches. `ODIN_CLI_SERVER_QUIC_RED=1 out/cli_server_quic_mac/odin_unittests --gtest_filter='OdinCliServerQuicRuntimeTest.*:OdinCliServerQuicSecurityTest.*'` still executes T3-T9 and reports them red against the unimplemented QUIC runner; the unfiltered default host run exits zero with T3-T9 skipped. Host-runnable enumeration: T1, T2, and T10 run in the host `odin_unittests`; T3-T9 remain red-verifiable only under `ODIN_CLI_SERVER_QUIC_RED=1`. Cross-compile-only enumeration: the four non-host `odin_unittests` binaries are built but not executed; their parser, accessor, Linux timerfd, iOS socket, and alternate-architecture branches are verified by cross-compile and code review only in P2.

- **P3. Implement QUIC server runner, cleanup, and shared-filter installation; turn T3-T9 green.**
  - **Scope:** add `:odin_server_xqc_runtime` to `:odin_cli_server` dependencies; implement §3.2.2's QUIC branch with XQUIC TLS config, IPv4 UDP local address, runtime create/start, runtime local-address banner, and zeroed engine callbacks; implement §3.2.4 shared-filter installation for QUIC; implement §3.2.5 QUIC failure lines, `odin_xqc_server_runtime_force_destroy` including its `NULL` no-op contract, single-entry final destroy guard, engine-destroy stream close/closing, separate `runtime_conn_close_notify` and `runtime_server_refuse` final-callback inerting, `force_destroy_active` guards for `runtime_server_accept`, `runtime_conn_create_notify`, and `runtime_conn_update_cid`, the inerted `runtime_stream_create_notify` no-context close-only branch, cleanup, and signal-stop reuse; implement §3.2.6 liveness, failpoint, filter-record, QUIC start-probe, and XQC runtime setter-record hooks including the pre-store `odin_xqc_server_runtime_set_dial_filter` record and the NULL receiver no-record behavior; use the existing `ODIN_XQC_UDP_TESTING` engine-create hook from §3.2.6 for T5, the existing `ODIN_XQC_UDP_TESTING.engine_destroy` hook for T7's engine-destroy matrix, and the existing `ODIN_DIAL_TESTING && ODIN_CLI_SERVER_TESTING` dial-start probe for T9 without adding production code; narrow `odin_server_xqc_runtime_scope_check` so the TCP runtime files and target remain XQUIC-free while `odin/cli_server.c` and `odin_cli_server` may reference `server_xqc_runtime`; remove the remaining T3-T9 red gates.
  - **Depends on:** P2.
  - **Done when:** the P1 build commands still succeed with the narrowed scope check. `out/cli_server_quic_mac/odin_unittests --gtest_filter='OdinCliServerQuic*:OdinXqcUdpLocalAddrTest.*:OdinXqcServerRuntimeLocalAddrTest.*'` passes T1-T10 un-gated on the host macOS architecture; T3, T4, T5, and T7 spawn or fork the host-runnable `out/cli_server_quic_mac/odin-server` / `odin_cli_main` path, while T8 and T9 execute in `out/cli_server_quic_mac/odin_unittests` with test hooks. The unfiltered host run `out/cli_server_quic_mac/odin_unittests --gtest_brief=1` exits zero with all pre-existing Odin suites green. Host-runnable enumeration: T1-T10 all run in the host `odin_unittests`; T3-T5 and T7 exercise the host `odin-server` process path; T5 also exercises the `ODIN_XQC_UDP_TESTING` engine-create hook inside the host `odin_unittests`; T6 directly exercises the `odin_xqc_server_runtime_force_destroy(NULL)` no-op API contract; T7's live-stream subcase exercises the QUIC start probe and non-null `odin_xqc_server_runtime_force_destroy`; T7's engine-destroy matrix exercises `ODIN_XQC_UDP_TESTING.engine_destroy` while `runtime_finish_destroy` is in progress, uses `ODIN_TRANSPORT_XQC_TESTING.set_user_data` to assert the fake stream user-data clear, separately invokes XQUIC stream closing/close before the final `runtime_conn_close_notify` variant and before the final `runtime_server_refuse` variant, separately invokes the guarded `runtime_server_accept`, `runtime_conn_create_notify`, and `runtime_conn_update_cid` variants while `force_destroy_active` is set, and separately invokes the inerted `runtime_stream_create_notify` no-context close-only variant after ALP user-data clear; T8 asserts byte-exact `ODIN_XQC_SERVER_ALPN` registration and the captured-callback `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_SET_DIAL_FILTER` record through the CLI QUIC path before UDP start, then resets the runtime test record, calls `odin_xqc_server_runtime_set_dial_filter(NULL, quic_cb, sentinel_ud)`, and asserts `odin_xqc_server_runtime_test_record()->call_count == 0`; T9 drives a host in-process fake XQUIC stream through `odin_xqc_server_runtime` and the existing dial-start probe. Cross-compile-only enumeration: `out/cli_server_quic_mac_arm64/odin`, `out/cli_server_quic_linux_x64/odin`, `out/cli_server_quic_ios_sim/odin`, `out/cli_server_quic_ios_device/odin`, and their `odin_unittests` binaries are built but not executed; the Linux timerfd-backed signal polling branch, Linux UDP bind branch, iOS socket branch, XQUIC TLS setup on non-host platforms, and alternate macOS architecture are verified by successful cross-compile plus code review only. `./tool/gn gen out/cli_server_quic_mac_asan --args='target_os="mac" is_asan=true'`, `./tool/ninja -C out/cli_server_quic_mac_asan odin_unittests`, and `out/cli_server_quic_mac_asan/odin_unittests --gtest_filter='OdinCliServerQuic*:OdinXqcUdpLocalAddrTest.*:OdinXqcServerRuntimeLocalAddrTest.*'` exit without AddressSanitizer reports, backing T5-T7 and T9 cleanup/lifecycle assertions including T7's separate engine-destroy final-callback variants, reachable force-destroy-active guard variants, inerted stream-create close-only variant, and final-destroy re-entry cases. `./tool/ninja -C out/cli_server_quic_mac odin:odin_server_xqc_runtime_scope_check`, `./tool/ninja -C out/cli_server_quic_mac_arm64 odin:odin_server_xqc_runtime_scope_check`, `./tool/ninja -C out/cli_server_quic_linux_x64 odin:odin_server_xqc_runtime_scope_check`, `./tool/ninja -C out/cli_server_quic_ios_sim odin:odin_server_xqc_runtime_scope_check`, and `./tool/ninja -C out/cli_server_quic_ios_device odin:odin_server_xqc_runtime_scope_check` all pass and report no XQUIC references in `odin/server_runtime.c`, `odin/server_runtime.h`, or the `odin_server_runtime` target. Production `out/cli_server_quic_mac/odin` and cross-compiled `out/cli_server_quic_linux_x64/odin` contain no `odin_cli_server_test_*`, `odin_xqc_udp_test_*`, or `odin_xqc_server_runtime_test_*` symbols; `./tidy_odin.sh` exits clean over touched Odin files.
