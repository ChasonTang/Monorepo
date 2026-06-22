# RFC-025: Server-Side Odin QUIC Stream Runtime

## 1. Summary

Add `odin/server_xqc_runtime.c` and `odin/server_xqc_runtime.h`, a server-side xquic runtime that binds an `odin_xqc_udp` UDP server, registers the Odin ALPN `odin/1`, accepts QUIC connections and bidirectional streams, and hands each stream to the existing server CONNECT pipeline through `odin_xqc_stream_transport`.

## 2. Goals

- **G1.** Provide a server QUIC runtime that creates an xquic server UDP driver, registers exactly one Odin ALPN value (`odin/1`), and starts/stops the UDP driver through explicit owner-thread runtime calls.
- **G2.** For every accepted xquic connection owned by the QUIC runtime, keep the active connection CID registered with `odin_xqc_udp` for UDP WRITE-recovery while the connection remains usable; unregister every stale or closed CID exactly once, and close the connection if a new active CID cannot be registered.
- **G3.** For every incoming bidirectional xquic stream on the Odin ALPN, run the existing server-side CONNECT_REQ decode, upstream dial, CONNECT_RESP encode, and relay pipeline over `odin_xqc_stream_transport`; non-bidirectional or setup-failed streams are closed without creating a live session.
- **G4.** Expose the same server dial-filter policy hook on the QUIC runtime and apply it before any peer-supplied CONNECT target can trigger an outbound TCP dial.

## 3. Design

### 3.1 Overview

This RFC adds a sibling runtime to the current TCP `odin_server_runtime`; it does not route `odin-server`, the `odin_cli_server` target, `odin/cli_server.c`, `odin/server_runtime.c`, or `odin/server_runtime.h` through QUIC. The new runtime owns xquic server integration: it creates `odin_xqc_udp` with `XQC_ENGINE_SERVER`, registers the Odin ALPN callbacks, tracks accepted xquic connections and streams, and uses `odin_xqc_stream_transport_create` for each accepted bidirectional stream. The existing `odin_server_session` state machine remains the CONNECT pipeline; this RFC adds a transport-factory entry point so that pipeline can be constructed over either the current fd transport or an xquic stream transport.

```
UDP packets
   |
   v
odin_xqc_udp  -- xquic engine callbacks -->  odin/server_xqc_runtime
                                                   |
                                                   | bidirectional stream
                                                   v
                                  odin_xqc_stream_transport
                                                   |
                                                   v
                                  odin_server_session CONNECT pipeline
                                      CONNECT_REQ -> dial -> CONNECT_RESP -> relay

Existing TCP path stays separate:

listen fd -> odin_server_runtime -> odin_server_session fd constructor
```

### 3.2 Detailed Design

#### 3.2.1 Server Session Transport Factory

`odin_server_session_create` currently creates its downstream fd transport internally at [odin/server_session.c](/Users/tangjiacheng/Downloads/Monorepo/odin/server_session.c:116), and the readiness trampoline it installs is private at [odin/server_session.c](/Users/tangjiacheng/Downloads/Monorepo/odin/server_session.c:211). Because `odin_xqc_stream_transport_create` also binds its `odin_transport_ready_cb` at construction time ([odin/transport_xqc.h](/Users/tangjiacheng/Downloads/Monorepo/odin/transport_xqc.h:22)), the server session needs a transport-factory entry point rather than a prebuilt-transport entry point.

```c
/* odin/server_session.h additions */
#include "odin/transport.h"

typedef int (*odin_server_session_transport_factory_cb)(
    odin_transport_ready_cb on_ready, void *ready_user_data,
    void *factory_user_data, odin_transport_t **out);

int odin_server_session_create_with_transport(
    odin_event_loop_t *loop,
    odin_server_session_transport_factory_cb create_downstream,
    void *factory_user_data, odin_server_session_close_cb on_close,
    void *user_data, odin_server_session_t **out);
```

```c
/* odin/testing/connect_session_internal_test.h additions;
 * visible only when ODIN_CONNECT_SESSION_TESTING is defined. */
int odin_connect_session_test_fail_next_create_server(int errnum);
unsigned int odin_connect_session_test_live_count(void);
```

```c
/* odin/testing/server_session_internal_test.h addition;
 * visible only when ODIN_SERVER_SESSION_TESTING is defined. */
unsigned int odin_server_session_test_live_count(void);
```

**Unstated contract.** `loop`, `create_downstream`, `on_close`, and `out` are non-null preconditions; invalid inputs return `-1` with `errno = EINVAL` and leave `*out` untouched. The factory is called exactly once during construction with the server session's permanent readiness callback and the newly allocated `odin_server_session_t *` as `ready_user_data`; it must return a downstream `odin_transport_t *` whose callback is not invoked before the factory returns. On success, the session owns the returned transport wrapper and destroys it on terminal or abort. On failure after the factory returned a transport, the session destroys that transport but has no additional lower-level close callback; the existing fd constructor keeps its current fd-close behavior by continuing to store `conn_fd` in the session, while the xquic stream factory relies on `odin_xqc_stream_transport`'s documented wrapper-only destroy.

`odin_xqc_stream_transport` is a synchronous-readiness transport: enabling `ODIN_TRANSPORT_WRITE` can call the server session readiness callback before `odin_transport_set_interest` returns. The server session therefore treats downstream interest changes produced from inside `session_on_req_decoded` as post-drive work. While `server_session_ready` is inside an outer `odin_connect_session_drive(ss->s, ...)` frame, `handle_dial_result` may call `odin_connect_session_server_set_error_code(ss->s, ...)` and update the server-session state, but it must not call `odin_transport_set_interest(ss->downstream_t, ...)`. Instead it records the wanted downstream mask in an internal pending slot. After `odin_connect_session_drive` returns, `server_session_ready` applies the pending mask, or recomputes `odin_connect_session_wants(ss->s)` when no pending mask exists, and then performs no further reads from `ss->s` because that setter may synchronously re-enter WRITE readiness, flush CONNECT_RESP, run `session_on_done`, and destroy `ss->s`. `handle_dial_result` calls reached outside an active connect-session drive, such as dial completion, still apply downstream interest immediately because no outer `odin_connect_session_drive` frame can resume and read the destroyed `odin_connect_session_t`.

Under `ODIN_CONNECT_SESSION_TESTING`, `odin_connect_session_test_fail_next_create_server(errnum)` is consumed inside `odin_connect_session_create_server` before its allocation branch; it is not a server-session mirror hook, and T10 uses it only to make the production `if odin_connect_session_create_server(...) != 0` cleanup branch run. `odin_connect_session_test_live_count()` reports a test-translation-unit counter of live connect-session handles: the counter increments only in the final success branch of `odin_connect_session_create_client` and `odin_connect_session_create_server`, after all constructor validation, allocation, copies, and field initialization have succeeded and immediately before writing `*out = s`; it decrements in `odin_connect_session_destroy` after the `s == NULL` guard and before `free(s)`. Failed create paths, including the one-shot fail hook consumed before the server allocation branch, leave the counter unchanged. Under `ODIN_SERVER_SESSION_TESTING`, `odin_server_session_test_live_count()` reports a test-translation-unit counter of live server-session handles: the counter increments only in the final success branch of `odin_server_session_create` and `odin_server_session_create_with_transport`, after downstream transport creation, connect-session creation, initial downstream READ interest, and field initialization have succeeded and immediately before writing `*out = ss`; it decrements in `finish_destroy` immediately before `free(ss)`. Failed server-session create paths leave the counter unchanged, and T8 uses this counter with `odin_connect_session_test_live_count()` to assert that active-entry deferred runtime destroy tears down the live server/connect session before final `runtime_conn_close_notify`. No test hook may bypass the production `odin_transport_set_interest(ss.downstream_t, ODIN_TRANSPORT_READ)` call; that branch is covered with a test-local fake downstream transport whose vtable returns `-1` with `errno` from `set_interest`. `odin_server_session_create` remains exported with the same signature at [odin/server_session.h](/Users/tangjiacheng/Downloads/Monorepo/odin/server_session.h:71) and continues to build an fd transport for the existing TCP runtime.

**Mechanism.**

```
create_with_transport(loop, factory, factory_ud, on_close, user_data, out):
  validate inputs
  allocate ss with conn_fd = -1, dial_fd = -1, state = S_HANDSHAKE
  if factory(server_session_ready, ss, factory_ud, &ss.downstream_t) != 0:
    free ss; preserve errno; return -1
  if odin_connect_session_create_server(session_on_req_decoded,
                                        session_on_done, ss, &ss.s) != 0:
    destroy ss.downstream_t; free ss; preserve errno; return -1
  if odin_transport_set_interest(ss.downstream_t, ODIN_TRANSPORT_READ) != 0:
    destroy connect session and downstream transport; free ss; preserve errno; return -1
  *out = ss; return 0

create(loop, conn_fd, on_close, user_data, out):
  call the existing fd-specific construction path, or call the shared helper
  with a factory that invokes odin_fd_transport_create and records conn_fd
  so finish_destroy/fire_terminal still close conn_fd exactly once

# Internal odin_server_session_t additions:
#   unsigned int connect_drive_depth
#   unsigned int pending_downstream_interest
#   int pending_downstream_interest_armed

server_session_ready(t, events, ss):
  ss_enter(ss)
  if ss.on_close_fired: ss_leave(ss); return
  if ss.s != NULL:
    ss.connect_drive_depth += 1
    d = odin_connect_session_drive(ss.s, t, events)
    ss.connect_drive_depth -= 1
    if ss.s != NULL and d == ODIN_CONNECT_SESSION_DRIVE_CONTINUE:
      if ss.pending_downstream_interest_armed:
        mask = ss.pending_downstream_interest
        ss.pending_downstream_interest_armed = 0
      else:
        mask = odin_connect_session_wants(ss.s)
      odin_transport_set_interest(t, mask)
      # Do not read ss.s after this setter; xqc may synchronously re-enter
      # WRITE readiness and destroy the connect session before it returns.
    ss_leave(ss)
    return
  if ss.relay != NULL:
    odin_relay_ready(t, events, ss.relay)
  ss_leave(ss)

handle_dial_result(ss, err):
  if err == 0:
    ss.state = S_WRITING_OK_RESP
    odin_connect_session_server_set_error_code(ss.s, 0)
  else:
    ss.state = S_WRITING_ERR_RESP
    ss.pending_dial_err = err
    odin_connect_session_server_set_error_code(
        ss.s, map_dial_errno_to_resp_code(err))
  mask = odin_connect_session_wants(ss.s)
  if ss.connect_drive_depth != 0:
    ss.pending_downstream_interest = mask
    ss.pending_downstream_interest_armed = 1
    return
  odin_transport_set_interest(ss.downstream_t, mask)
```

Satisfies: G3 via the factory that lets the existing CONNECT pipeline own an xquic stream transport without exposing the private readiness trampoline; G4 via preserving the current `odin_server_session_set_dial_filter` path for both fd and xquic downstream transports.

#### 3.2.2 QUIC Runtime Public API and Startup

```c
/* odin/server_xqc_runtime.h */
#include <sys/socket.h>

#include "odin/event_loop.h"
#include "odin/server_session.h"
#include "odin/xqc_udp.h"
#include <xquic/xquic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ODIN_XQC_SERVER_ALPN "odin/1"

typedef struct odin_xqc_server_runtime_t odin_xqc_server_runtime_t;

typedef struct odin_xqc_server_runtime_config_t {
  odin_event_loop_t *loop;
  const struct sockaddr *local_addr;
  socklen_t local_addrlen;
  const xqc_config_t *engine_config;
  const xqc_engine_ssl_config_t *ssl_config;
  const xqc_engine_callback_t *engine_callbacks;
} odin_xqc_server_runtime_config_t;

int odin_xqc_server_runtime_create(
    const odin_xqc_server_runtime_config_t *config,
    odin_xqc_server_runtime_t **out);
int odin_xqc_server_runtime_start(odin_xqc_server_runtime_t *rt);
int odin_xqc_server_runtime_stop(odin_xqc_server_runtime_t *rt);
void odin_xqc_server_runtime_set_dial_filter(
    odin_xqc_server_runtime_t *rt, odin_server_session_dial_filter_cb cb,
    void *user_data);
void odin_xqc_server_runtime_destroy(odin_xqc_server_runtime_t *rt);

#ifdef __cplusplus
}
#endif
```

**Unstated contract.** `config`, `config->loop`, `config->local_addr`, `config->engine_callbacks`, and `out` are non-null preconditions; invalid inputs return `-1` with `errno = EINVAL` and leave `*out` untouched. The server runtime owns the full xquic transport callback table it passes to `odin_xqc_udp_create`; callers do not supply per-connection transport callbacks through this API. `create` initializes that table from zeroed storage, installs only the runtime-owned server connection callbacks (`server_accept`, `server_refuse`, and `conn_update_cid_notify`), passes `XQC_ENGINE_SERVER` to `odin_xqc_udp_create`, and registers `ODIN_XQC_SERVER_ALPN` with one `xqc_app_proto_callbacks_t` containing the ALPN connection and stream callbacks in §3.2.3 and §3.2.4. `odin_xqc_udp_create` then owns the packet-send callback overrides from RFC-017, so every xquic connection using this runtime keeps the required connection user-data value `odin_xqc_udp_xqc_user_data(xu)`. Application state is reached through `odin_xqc_udp_app_user_data(xu)`, ALPN connection user data, or stream user data, not through preserved caller transport callbacks. If `odin_xqc_udp_create` fails for an otherwise valid runtime config, creation frees the runtime shell, preserves the callee's `errno`, leaves `*out` untouched, registers no ALPN, and does not call `odin_xqc_udp_destroy` because no UDP driver was returned; a later valid create may still succeed. If ALPN registration fails, creation rolls back by destroying the UDP driver, freeing the runtime, and returning `-1` with `errno = EIO`. `start` and `stop` forward to `odin_xqc_udp_start` and `odin_xqc_udp_stop`; both are idempotent for a live runtime, and `start(NULL)` / `stop(NULL)` return `-1` with `errno = EINVAL`. `set_dial_filter(NULL, cb, user_data)` is a no-op. For a non-null runtime, `set_dial_filter` matches `odin_server_runtime_set_dial_filter`: it is owner-thread and replace-only; each later call overwrites the stored callback/user-data pair; `cb == NULL` clears the slot. The then-current pair is propagated to each future `odin_server_session` during `runtime_stream_create_notify`, so the setter does not mutate already-created stream sessions. "Future" is stream-scoped rather than connection-scoped: a setter call after `server_accept` but before a later bidirectional stream is created on that accepted connection applies to that stream, while streams created before the setter keep their existing per-session pair. `destroy(NULL)` is a no-op. `odin_xqc_server_runtime_destroy(rt)` is legal from inside a propagated dial filter running on an xqc stream callback stack. Each runtime-owned xquic callback is an active runtime entry; when destroy is requested while an active entry is on the stack, the runtime records the destroy request, stops the UDP driver, snapshots each connection's pre-destroy `closing` state, initializes the per-connection `destroy_close_requested` flag to false for contexts first observed by this destroy request, marks linked connection contexts closing, and defers stream-session destruction, connection close calls, CID unregister decisions, ALPN unregister, UDP-driver destroy, and `rt` free until the outermost runtime callback returns. `destroy_close_requested` is distinct from "CID registered": the flag prevents a later post-destroy callback unwind from issuing a duplicate destroy-driven `xqc_conn_close`, while the CID remains registered until `runtime_conn_close_notify` or `runtime_server_refuse` performs the final unregister. The dial filter's return value still controls the current server-session branch, so a deny return starts no outbound dial; if that branch removes the stream before the deferred destroy drain runs, the drain observes the stream context absent and does not destroy or close it a second time. All APIs are owner-thread APIs under the existing event-loop contract. This module itself contains no platform-specific branch; macOS host tests execute the abstract runtime contract, while Linux, alternate macOS arch, and iOS artifacts compile the same code but are not runtime-verified in this environment.

**Mechanism.**

```
create(config, out):
  validate inputs
  rt = calloc
  rt.loop = config.loop
  rt.dial_filter = NULL
  rt.transport_callbacks = zeroed xqc_transport_callbacks_t
  rt.transport_callbacks.server_accept = runtime_server_accept
  rt.transport_callbacks.server_refuse = runtime_server_refuse
  rt.transport_callbacks.conn_update_cid_notify = runtime_conn_update_cid
  udp_config = {
    loop = config.loop,
    local_addr = config.local_addr,
    local_addrlen = config.local_addrlen,
    engine_type = XQC_ENGINE_SERVER,
    engine_config = config.engine_config,
    ssl_config = config.ssl_config,
    engine_callbacks = config.engine_callbacks,
    transport_callbacks = &rt.transport_callbacks,
    app_user_data = rt,
  }
  if odin_xqc_udp_create(&udp_config, &rt.xu) != 0:
    free rt; preserve errno; return -1
  if xqc_engine_register_alpn(odin_xqc_udp_engine(rt.xu), "odin/1", 6,
                              &rt.app_callbacks, rt) != XQC_OK:
    odin_xqc_udp_destroy(rt.xu); free rt; errno = EIO; return -1
  *out = rt; return 0

start(rt):
  return odin_xqc_udp_start(rt.xu)

stop(rt):
  return odin_xqc_udp_stop(rt.xu)

set_dial_filter(rt, cb, user_data):
  if rt == NULL: return
  rt.dial_filter = cb
  if cb == NULL:
    rt.dial_filter_ud = NULL
  else:
    rt.dial_filter_ud = user_data

runtime_callback_enter(rt):
  rt.active_entries += 1

runtime_callback_leave(rt):
  rt.active_entries -= 1
  if rt.active_entries == 0 and rt.destroy_pending:
    if rt.connections is empty:
      finish_destroy(rt); return
    drain_destroy_pending_connections(rt)
    if rt.connections is empty:
      finish_destroy(rt); return

destroy(rt):
  if rt == NULL: return
  stop rt.xu
  if rt has live connections:
    mark destroy_pending
    for each live connection:
      if ctx has no destroy snapshot:
        remember ctx.closing as the pre-destroy closing state
        ctx.destroy_close_requested = 0
      mark ctx closing
    if rt has active runtime callback entries:
      return
    drain_destroy_pending_connections(rt)
    return
  unregister "odin/1"; destroy rt.xu; free rt

drain_destroy_pending_connections(rt):
  for each live connection:
      was_closing = ctx pre-destroy closing state
      destroy and unlink every live stream session
      if !was_closing and ctx has a registered CID and
         !ctx.destroy_close_requested:
        ctx.destroy_close_requested = 1
        call xqc_conn_close for that CID
```

Runtime destroy marks every live connection context closing before aborting any stream session or calling `xqc_conn_close`, then defers freeing the runtime and connection contexts until xquic reports connection close or refuse. The pre-destroy `was_closing` snapshot is observable: a context that was already closing, or that already recorded no registered CID after CID-update failure, is left linked for its eventual final notification but does not receive a second `xqc_conn_close` or CID unregister during runtime destroy. `destroy_close_requested` is set before the first destroy-driven `xqc_conn_close`, so a later post-destroy callback that reaches `runtime_callback_leave` and re-runs the drain cannot request another close for the same still-registered CID. Deferred destruction is list-wide, not first-close-wide; if destroy sees one registered connection plus one already-closing/no-CID context, closing the registered connection first cannot unregister ALPN or destroy the UDP driver until the already-closing context also reports a final notification. `runtime_conn_close_notify` and `runtime_server_refuse` never call `finish_destroy(rt)` from inside their callback bodies; they unlink/free only their connection context, then the active-entry leave wrapper performs the final `finish_destroy(rt)` only after decrementing `active_entries` and observing `destroy_pending && connections_empty`. That ordering is the source of §3.2.4's post-destroy stream-create guard: a new bidirectional `stream_create_notify` that arrives after destroy has begun but before a final connection notification sees the closing context, calls `xqc_stream_close(stream)`, creates no transport or server session, and makes no CID registration change. Destroying each pre-existing stream session destroys its `odin_xqc_stream_transport` wrapper, whose destroy path calls `odin_xqc_stream_set_user_data_call(s->stream, NULL)` before freeing the wrapper; any later xquic read/write/closing callback for those streams therefore carries `NULL` user data and is handled by §3.2.4's null no-op rules while the runtime waits for final connection notification. The active-entry deferred drain must finish that stream cleanup before issuing the deferred `xqc_conn_close`: saved transport pointers are absent from both the per-connection stream list and the runtime transport map, the server and connect live-count probes have returned to their pre-stream baselines, and the final notification observes no remaining stream context to destroy. Non-null stale transport-pointer tolerance is required only for `runtime_stream_close_notify`, which performs a live-map lookup before touching any stream context.

Satisfies: G1 via the public runtime API, the fixed ALPN string, `XQC_ENGINE_SERVER` driver creation, ALPN registration, explicit start/stop behavior, and deferred physical runtime destruction after the final live connection close/refuse notification when connection contexts are linked; G4 via the `odin_xqc_server_runtime_set_dial_filter` replace/clear/future-stream contract and the stored runtime slot consumed by §3.2.4.

#### 3.2.3 Connection and CID Lifecycle

The runtime owns the xquic connection-level callbacks needed to bridge accepted QUIC connections to `odin_xqc_udp` and to ALPN stream handling.

```c
/* xquic callback slots installed or wrapped by odin/server_xqc_runtime.c */
int runtime_server_accept(xqc_engine_t *engine, xqc_connection_t *conn,
                          const xqc_cid_t *cid, void *user_data);
void runtime_server_refuse(xqc_engine_t *engine, xqc_connection_t *conn,
                           const xqc_cid_t *cid, void *user_data);
void runtime_conn_update_cid(xqc_connection_t *conn,
                             const xqc_cid_t *retire_cid,
                             const xqc_cid_t *new_cid,
                             void *conn_user_data);
int runtime_conn_create_notify(xqc_connection_t *conn, const xqc_cid_t *cid,
                               void *conn_user_data,
                               void *conn_proto_data);
int runtime_conn_close_notify(xqc_connection_t *conn, const xqc_cid_t *cid,
                              void *conn_user_data,
                              void *conn_proto_data);
```

```c
/* odin/testing/server_xqc_runtime_internal_test.h addition;
 * visible only when ODIN_XQC_SERVER_RUNTIME_TESTING is defined. */
int odin_xqc_server_runtime_test_fail_next_conn_context_alloc(
    odin_xqc_server_runtime_t *rt, int errnum);
```

**Unstated contract.** `runtime_server_accept(engine, conn, cid, user_data)` receives `user_data` from `odin_xqc_udp` packet processing. It treats that value as an `odin_xqc_udp_t *`, gets the owning runtime with `odin_xqc_udp_app_user_data`, allocates one connection context, registers `cid` with `odin_xqc_udp_register_conn`, sets xquic transport user data to `odin_xqc_udp_xqc_user_data(xu)` with `xqc_conn_set_transport_user_data`, sets ALPN user data to the connection context with `xqc_conn_set_alp_user_data`, links the context into `rt.connections`, and returns `0`. Allocation failure returns `-1` before CID registration, xquic user-data installation, list linking, or CID unregister; CID registration failure returns `-1` after freeing the unlinked context and before installing xquic user data. In both cases, no stream callback can later create a session for that failed connection and a later accept can still succeed. `runtime_conn_create_notify` is a consistency check: it accepts only when `conn_proto_data` already names the connection context installed by `server_accept`, then returns `0` without allocating a second owner. `runtime_server_refuse` is a final xquic destruction notification for a linked accepted context that closed before the normal ALPN close path; it finds the context by `xqc_connection_t *`, unregisters its current CID only if one remains registered, unlinks it, and frees it. If that refuse removes the last context while runtime destroy is pending, it still does not call `finish_destroy(rt)` inside the callback body; `runtime_callback_leave` performs the active-entry-safe finalization after the callback returns. `runtime_conn_update_cid` must not let xquic continue with an active CID that `odin_xqc_udp` cannot recover on UDP WRITE readiness: xquic calls `conn_update_cid_notify` before copying `new_cid` into its user CID, then copies it unconditionally after the callback returns. The runtime therefore first registers `new_cid`. On success, it unregisters `retire_cid` and updates the context's current CID to `new_cid`; there is no caller callback to forward because §3.2.2 makes the server runtime the transport callback table owner. On registration failure, it treats the connection as fatally closing: it marks the context closing, destroys and unlinks all live stream sessions, calls `xqc_conn_close(odin_xqc_udp_engine(rt.xu), &ctx.current_cid)` while xquic still accepts the old current CID, unregisters that old CID immediately, and records that no CID remains registered for the context. Any later `runtime_conn_update_cid` for a closing context, including one after CID-update failure and one after runtime destroy has marked the context closing, returns before register, unregister, or `xqc_conn_close`. The closing context remains linked only so later xquic final notification can free it without double-unregistering. `runtime_conn_close_notify` destroys any remaining stream sessions for that connection, unregisters the current CID only if one is still registered, unlinks the context, frees it, and leaves destroy-pending runtime completion to the active-entry leave wrapper. xquic's normal ordering closes streams before `conn_close_notify`; the runtime nevertheless keeps this final live-stream cleanup branch as a defensive direct-callback cleanup path and covers it in T18. No public API exposes peer addresses or packet bytes from this layer.

**Mechanism.**

```
runtime_server_accept(engine, conn, cid, xu_user_data):
  xu = xu_user_data
  rt = odin_xqc_udp_app_user_data(xu)
  if ODIN_XQC_SERVER_RUNTIME_TESTING conn-context alloc hook is armed:
    errno = hook errnum; return -1
  ctx = calloc connection context
  if ctx == NULL: return -1
  if odin_xqc_udp_register_conn(xu, cid) != 0:
    free ctx; return -1
  ctx.rt = rt; ctx.conn = conn; ctx.current_cid = *cid
  link ctx into rt.connections
  xqc_conn_set_transport_user_data(conn, odin_xqc_udp_xqc_user_data(xu))
  xqc_conn_set_alp_user_data(conn, ctx)
  return 0

runtime_conn_create_notify(conn, cid, conn_user_data, conn_proto_data):
  if conn_proto_data does not match a linked connection context:
    return -1
  return 0

runtime_server_refuse(engine, conn, cid, user_data):
  ctx = find connection context by conn
  if ctx != NULL:
    if ctx has a registered CID:
      odin_xqc_udp_unregister_conn(ctx.rt.xu, &ctx.current_cid)
    unlink and free ctx
  # No finish_destroy here; runtime_callback_leave finalizes if this was
  # the last linked context and destroy is pending.

runtime_conn_update_cid(conn, retired, new_cid, conn_user_data):
  rt = odin_xqc_udp_app_user_data(conn_user_data)
  ctx = find connection context by conn
  if ctx == NULL: return
  if ctx is closing: return
  if odin_xqc_udp_register_conn(rt.xu, new_cid) != 0:
    mark ctx closing
    destroy and unlink every live stream session in ctx.streams
    xqc_conn_close(odin_xqc_udp_engine(rt.xu), &ctx.current_cid)
    odin_xqc_udp_unregister_conn(rt.xu, &ctx.current_cid)
    record that ctx has no registered CID
    return
  odin_xqc_udp_unregister_conn(rt.xu, retired)
  ctx.current_cid = *new_cid

runtime_conn_close_notify(conn, cid, conn_user_data, conn_proto_data):
  ctx = conn_proto_data or find by conn
  if ctx == NULL: return 0
  destroy every live stream session in ctx.streams
  if ctx has a registered CID:
    odin_xqc_udp_unregister_conn(ctx.rt.xu, &ctx.current_cid)
  unlink and free ctx
  # No finish_destroy here; runtime_callback_leave finalizes if this was
  # the last linked context and destroy is pending.
  return 0
```

Satisfies: G2 via accept-time registration, update-time register-before-unregister ordering on success, fatal close-and-unregister behavior when a new active CID cannot be registered, refuse/close cleanup including defensive live-stream teardown, and active-entry-safe destroy-pending completion after the final close or refuse notification.

#### 3.2.4 Stream Callback Handoff to the Server Pipeline

The Odin ALPN stream callbacks either forward xquic readiness into the existing stream transport wrapper or create/destroy server sessions around accepted bidirectional streams.

```c
/* xquic stream callback slots registered for ODIN_XQC_SERVER_ALPN */
xqc_int_t runtime_stream_create_notify(xqc_stream_t *stream,
                                       void *strm_user_data);
xqc_int_t runtime_stream_read_notify(xqc_stream_t *stream,
                                     void *strm_user_data);
xqc_int_t runtime_stream_write_notify(xqc_stream_t *stream,
                                      void *strm_user_data);
xqc_int_t runtime_stream_close_notify(xqc_stream_t *stream,
                                      void *strm_user_data);
void runtime_stream_closing_notify(xqc_stream_t *stream, xqc_int_t err_code,
                                   void *strm_user_data);
```

```c
/* odin/testing/server_xqc_runtime_internal_test.h;
 * visible only when ODIN_XQC_SERVER_RUNTIME_TESTING is defined. */
int odin_xqc_server_runtime_test_fail_next_stream_context_alloc(
    odin_xqc_server_runtime_t *rt, int errnum);
```

```c
/* odin/testing/transport_xqc_internal_test.h addition;
 * visible only when ODIN_TRANSPORT_XQC_TESTING is defined. */
int odin_xqc_stream_transport_test_fail_next_create(int errnum);
```

**Unstated contract.** Callback return values are part of the xquic dispatch contract. `stream_create_notify` and `stream_close_notify` return `XQC_OK` in every runtime branch, including valid stream setup, close-only branches, setup-failure cleanup, stale close, and null close. `stream_read_notify` and `stream_write_notify` pass xquic's `strm_user_data` directly to `odin_xqc_stream_transport_read_notify` and `odin_xqc_stream_transport_write_notify` and return the exact helper result; those helpers already treat `NULL` stream user data as a benign no-op and currently return `XQC_OK` for every covered notify path. §5 asserts the returned `xqc_int_t` for every direct create/read/write/close callback invocation, so a side-effect-correct callback that returns `-1` fails before it can be shipped under real xquic dispatch. The direct forwarding contract does not permit non-null stale transport pointers for read/write callbacks: `odin_xqc_stream_transport` destroy clears xquic's stream user-data slot before freeing the wrapper, and xquic read/write dispatch reads the current slot value.

`runtime_stream_read_notify` may return only after more server-session work has completed than a plain READ callback suggests. For a complete CONNECT_REQ denied by the dial filter, the xqc transport READ callback enters `server_session_ready`, `session_on_req_decoded` records the denied CONNECT_RESP, §3.2.1 defers the downstream WRITE interest until `odin_connect_session_drive` returns, and that post-drive interest setter may synchronously deliver xqc WRITE readiness and flush the denied CONNECT_RESP before `stream_read_notify` returns. This synchronous flush is allowed; the safety invariant is that the WRITE kick is not emitted while an outer `odin_connect_session_drive` frame can still resume and read `ss->s`. The runtime must not add a separate buffering layer or call the server session directly to avoid this re-entrancy; it relies on the §3.2.1 post-drive interest discipline.

The runtime stores no separate "CONNECT_REQ complete" latch: when the existing server decoder has accumulated only a prefix in `buf_used` and the xqc transport reports `-XQC_EAGAIN`, the session remains live with READ interest so a later `stream_read_notify` resumes the same decoder state. `stream_closing_notify` forwards reset/error notification to `odin_xqc_stream_transport_closing_notify`; like read/write, closing callbacks are safe after destroy only through current `NULL` stream user data, not through stale non-null wrapper pointers. For a live stream, a nonzero closing/reset notify is a terminal server-session error path: the transport reports the error to the session, the session's close callback reaches `runtime_stream_session_on_close` with `err != 0`, and the runtime records exactly one `xqc_stream_close(stream_ctx->stream)` before freeing the stream context.

`stream_create_notify` accepts only `XQC_STREAM_BIDI` on a non-closing connection context; for `XQC_STREAM_UNI`, a missing ALPN context, or a context already closing because runtime destroy or CID-update failure is in progress, it calls `xqc_stream_close(stream)` and returns `XQC_OK` without creating a transport or server session. For a bidirectional stream, the runtime allocates a stream context; allocation failure closes the xquic stream, creates no xqc transport or server session, links no stream context, installs no xquic stream user data, returns `XQC_OK`, and leaves the parent connection usable for later streams. After stream-context allocation succeeds, the runtime creates an `odin_server_session` through §3.2.1's transport factory, and the factory creates `odin_xqc_stream_transport_create(stream, on_ready, ready_user_data, &transport)`. The stream context is linked by connection and by transport pointer because xquic's stream user data slot is occupied by the transport pointer that the RFC-016 helpers require.

When the server session's `on_close` fires, the session has already destroyed the xquic stream transport wrapper and cleared xquic stream user data as part of its terminal teardown. The runtime then unlinks the stream context before touching xquic again. If `err != 0`, the runtime calls `xqc_stream_close(stream_ctx->stream)` to reset/finish the underlying xquic stream after the error response has been flushed, after decoder failure made a response impossible, or after xquic delivered a closing/reset notify. If `err == 0`, the relay-OK path does not call `xqc_stream_close`; it relies on the existing relay shutdown path to send the xqc FIN. The runtime then calls `odin_server_session_destroy(ss)` and frees the context. A later xquic `stream_close_notify` with `NULL`, or a direct/test `runtime_stream_close_notify` with a no-longer-linked transport pointer, is a benign no-op because close uses the runtime's live transport map before destroying a session. Setup failure closes the xquic stream with `xqc_stream_close` and leaves the parent connection alive for later streams unless the parent connection is already closing.

Under `ODIN_TRANSPORT_XQC_TESTING`, `odin_xqc_stream_transport_test_fail_next_create(errnum)` is a one-shot hook consumed inside `odin_xqc_stream_transport_create`; when armed, the next create sets `errno = errnum` and returns `-1` before allocating `odin_xqc_stream_transport_t`, before calling `odin_xqc_stream_set_user_data_call`, and therefore before any `xqc_stream_set_user_data` side effect. It is not a runtime-factory mirror hook, so T10 reaches the production factory call and observes the callee's `-1` return.

**Mechanism.**

```
runtime_stream_create_notify(stream, strm_user_data):
  ctx = xqc_get_conn_alp_user_data_by_stream(stream)
  if ctx == NULL: xqc_stream_close(stream); return XQC_OK
  if ctx is closing: xqc_stream_close(stream); return XQC_OK
  if xqc_stream_get_direction(stream) != XQC_STREAM_BIDI:
    xqc_stream_close(stream); return XQC_OK
  if ODIN_XQC_SERVER_RUNTIME_TESTING stream-context alloc hook is armed:
    errno = hook errnum; stream_ctx = NULL
  else:
    stream_ctx = calloc stream context
  if stream_ctx == NULL:
    xqc_stream_close(stream)
    return XQC_OK
  stream_ctx.conn_ctx = ctx; stream_ctx.stream = stream
  if odin_server_session_create_with_transport(ctx.rt.loop,
       xqc_stream_transport_factory, stream_ctx,
       runtime_stream_session_on_close, stream_ctx, &stream_ctx.ss) != 0:
    xqc_stream_close(stream)
    free stream_ctx
    return XQC_OK
  # propagate the then-current runtime slot; NULL clears the per-session hook
  odin_server_session_set_dial_filter(stream_ctx.ss, ctx.rt.dial_filter,
                                      ctx.rt.dial_filter_ud)
  link stream_ctx into ctx.streams and rt.streams_by_transport
  return XQC_OK

xqc_stream_transport_factory(on_ready, ready_ud, factory_ud, out):
  stream_ctx = factory_ud
  if odin_xqc_stream_transport_create(stream_ctx.stream, on_ready, ready_ud,
                                      out) != 0:
    return -1
  stream_ctx.transport = *out
  return 0

runtime_stream_read_notify(stream, strm_user_data):
  # after transport destroy, xquic dispatch supplies current user_data == NULL
  # denied CONNECT_RESP may synchronously flush through §3.2.1's post-drive
  # WRITE-interest drain before this helper returns
  return odin_xqc_stream_transport_read_notify(stream, strm_user_data)

runtime_stream_write_notify(stream, strm_user_data):
  # after transport destroy, xquic dispatch supplies current user_data == NULL
  return odin_xqc_stream_transport_write_notify(stream, strm_user_data)

runtime_stream_closing_notify(stream, err_code, strm_user_data):
  # after transport destroy, xquic dispatch supplies current user_data == NULL
  odin_xqc_stream_transport_closing_notify(stream, err_code, strm_user_data)

runtime_stream_session_on_close(ss, err, stream_ctx):
  unlink stream_ctx from connection and transport lists
  if err != 0:
    xqc_stream_close(stream_ctx.stream)
  odin_server_session_destroy(ss)
  free stream_ctx

runtime_stream_close_notify(stream, strm_user_data):
  if strm_user_data == NULL: return XQC_OK
  stream_ctx = find by transport pointer strm_user_data
  if stream_ctx == NULL: return XQC_OK
  odin_server_session_destroy(stream_ctx.ss)
  unlink and free stream_ctx
  return XQC_OK
```

Satisfies: G3 via the bidirectional stream factory path into `odin_server_session_create_with_transport`, the direct read/write/closing-notify forwarding into RFC-016 helpers, the unidirectional close path, setup-failure cleanup, nonzero terminal stream `xqc_stream_close`, relay-OK FIN reliance, and stale-close no-op handling; G4 via the per-stream propagation of the runtime dial filter before the CONNECT_REQ can trigger `odin_dial_start`.

#### 3.2.5 Internal Test Hook Contract

The server QUIC runtime tests drive xquic callbacks and failure branches through a test-only header. Production builds do not expose these symbols, and the wrapper layer records the runtime-owned calls without changing the existing `odin_xqc_udp` or `odin_xqc_stream_transport` production contracts.

```c
/* odin/testing/server_xqc_runtime_internal_test.h;
 * visible only when ODIN_XQC_SERVER_RUNTIME_TESTING is defined. */
#include "odin/server_xqc_runtime.h"
#include "odin/xqc_udp.h"
#include <xquic/xquic.h>

#define ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CAP 128u

typedef enum odin_xqc_server_runtime_test_call_kind_t {
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_CREATE = 1,
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_START,
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_STOP,
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_DESTROY,
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_REGISTER_CONN,
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN,
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_ENGINE_REGISTER_ALPN,
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_ENGINE_UNREGISTER_ALPN,
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_SET_TRANSPORT_USER_DATA,
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_SET_ALP_USER_DATA,
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_CLOSE,
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_GET_DIRECTION,
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_GET_CONN_ALP_USER_DATA_BY_STREAM,
  ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_CLOSE
} odin_xqc_server_runtime_test_call_kind_t;

typedef struct odin_xqc_server_runtime_test_udp_create_record_t {
  odin_event_loop_t *loop;
  const struct sockaddr *local_addr;
  socklen_t local_addrlen;
  xqc_engine_type_t engine_type;
  const xqc_config_t *engine_config;
  const xqc_engine_ssl_config_t *ssl_config;
  const xqc_engine_callback_t *engine_callbacks;
  xqc_engine_callback_t engine_callbacks_value;
  const xqc_transport_callbacks_t *transport_callbacks;
  xqc_transport_callbacks_t transport_callbacks_value;
  void *app_user_data;
} odin_xqc_server_runtime_test_udp_create_record_t;

typedef struct odin_xqc_server_runtime_test_call_t {
  odin_xqc_server_runtime_test_call_kind_t kind;
  xqc_engine_t *engine;
  odin_xqc_udp_t *xu;
  xqc_connection_t *conn;
  xqc_stream_t *stream;
  xqc_cid_t cid;
  const char *alpn;
  size_t alpn_len;
  xqc_app_proto_callbacks_t *app_callbacks;
  void *user_data;
  xqc_stream_direction_t direction;
  int int_result;
  xqc_int_t xqc_result;
} odin_xqc_server_runtime_test_call_t;

typedef struct odin_xqc_server_runtime_test_record_t {
  unsigned int call_count;
  unsigned int dropped_call_count;
  odin_xqc_server_runtime_test_call_t
      calls[ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CAP];
  unsigned int udp_create_calls;
  odin_xqc_server_runtime_test_udp_create_record_t last_udp_create;
} odin_xqc_server_runtime_test_record_t;

typedef struct odin_xqc_server_runtime_test_ops_t {
  xqc_int_t (*engine_register_alpn)(
      xqc_engine_t *engine, const char *alpn, size_t alpn_len,
      xqc_app_proto_callbacks_t *app_callbacks, void *user_data);
  xqc_int_t (*engine_unregister_alpn)(xqc_engine_t *engine, const char *alpn,
                                      size_t alpn_len);
  void (*conn_set_transport_user_data)(xqc_connection_t *conn,
                                       void *user_data);
  void (*conn_set_alp_user_data)(xqc_connection_t *conn, void *user_data);
  xqc_int_t (*conn_close)(xqc_engine_t *engine, const xqc_cid_t *cid);
  xqc_stream_direction_t (*stream_get_direction)(xqc_stream_t *stream);
  void *(*get_conn_alp_user_data_by_stream)(xqc_stream_t *stream);
  xqc_int_t (*stream_close)(xqc_stream_t *stream);
  int (*udp_register_conn)(odin_xqc_udp_t *xu, const xqc_cid_t *cid);
  void (*udp_unregister_conn)(odin_xqc_udp_t *xu, const xqc_cid_t *cid);
} odin_xqc_server_runtime_test_ops_t;

void odin_xqc_server_runtime_test_reset(void);
void odin_xqc_server_runtime_test_set_ops(
    const odin_xqc_server_runtime_test_ops_t *ops);
const odin_xqc_server_runtime_test_record_t *
odin_xqc_server_runtime_test_record(void);
int odin_xqc_server_runtime_test_fail_next_conn_context_alloc(
    odin_xqc_server_runtime_t *rt, int errnum);
int odin_xqc_server_runtime_test_fail_next_stream_context_alloc(
    odin_xqc_server_runtime_t *rt, int errnum);
```

**Unstated contract.** `odin_xqc_server_runtime_test_reset()` clears the record, clears all optional ops, clears one-shot allocation-failure hooks, and is called by each `OdinXqcServerRuntimeTest` fixture setup. `odin_xqc_server_runtime_test_set_ops(NULL)` clears only the optional ops. `odin_xqc_server_runtime_test_record()` returns a pointer to the process-global record until the next reset. The call log is append-only up to `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CAP`; when full, wrappers increment `dropped_call_count` and still execute the production or fake call. The UDP-create wrapper records `last_udp_create` and a value copy of the engine and transport callback tables before calling production `odin_xqc_udp_create`; there is intentionally no server-runtime replacement hook for `odin_xqc_udp_create`, so T2 drives UDP-driver creation failure through the existing `odin_xqc_udp_test_ops_t.engine_create` slot installed with `odin_xqc_udp_test_set_ops`. That failure subcase returns `NULL` and therefore expects no `engine_destroy`. Any test subcase whose `odin_xqc_udp_test_ops_t.engine_create` returns a non-null fake `xqc_engine_t *` for a runtime that may later destroy or roll back the UDP driver must install a paired `odin_xqc_udp_test_ops_t.engine_destroy` fake and assert that fake destroy is observed before clearing UDP test ops; otherwise `xqc_udp_engine_destroy_call` would fall back to real `xqc_engine_destroy` for the fake engine. Runtime-owned `odin_xqc_udp_register_conn` and `odin_xqc_udp_unregister_conn` calls go through the ops slots above so T4/T5 can inject and count registration failures. `xqc_stream_set_user_data` is not a server-runtime call site; T8/T10/T11/T18 record stream user-data install/clear through the `odin_xqc_stream_transport_test_ops_t.set_user_data` slot installed with `odin_xqc_stream_transport_test_set_ops`, which gates `odin_xqc_stream_set_user_data_call` in `odin/transport_xqc.c` under `ODIN_TRANSPORT_XQC_TESTING`. T10 subcase B additionally asserts that the §3.2.4 `odin_xqc_stream_transport_test_fail_next_create` hook produces zero non-null `set_user_data` records and leaves the fake stream's current user-data slot `NULL`, proving the hook fired before allocation and before any stream user-data install.

**Mechanism.**

```
runtime_udp_create_call(config, out):
  record ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_CREATE and copy config fields into last_udp_create
  return odin_xqc_udp_create(config, out)

runtime_udp_start_call(xu), runtime_udp_stop_call(xu),
runtime_udp_destroy_call(xu):
  record the UDP lifecycle call, then call the matching odin_xqc_udp function
  if xu was created with a non-null fake engine from odin_xqc_udp_test_ops_t.engine_create,
    the currently installed UDP test ops must include engine_destroy and the test must observe it

runtime_udp_register_conn_call(xu, cid):
  record ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_REGISTER_CONN
  if test ops.udp_register_conn != NULL: return test ops result
  return odin_xqc_udp_register_conn(xu, cid)

runtime_udp_unregister_conn_call(xu, cid):
  record ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN
  if test ops.udp_unregister_conn != NULL: call test op and return
  odin_xqc_udp_unregister_conn(xu, cid)

runtime_engine_register_alpn_call, runtime_engine_unregister_alpn_call,
runtime_conn_set_transport_user_data_call, runtime_conn_set_alp_user_data_call,
runtime_conn_close_call, runtime_stream_get_direction_call,
runtime_get_conn_alp_user_data_by_stream_call, runtime_stream_close_call:
  record the call with pointer and CID/string arguments
  if the corresponding test op is non-null: call it and record its result
  otherwise call the matching xquic function and record its result
```

Outside `ODIN_XQC_SERVER_RUNTIME_TESTING`, those wrapper names compile to direct production calls with no record, no ops storage, and no exported test symbols.

Satisfies: G1, G2, and G3 via the test-only wrapper contract that makes the startup, CID, stream-direction, close, and destroy paths in §5 observable without changing the production runtime surface.

## 4. Security

- **S1.**
  - **Threat:** A peer-supplied CONNECT_REQ host/port on an accepted QUIC stream can select the outbound TCP address passed to `connect(2)`, creating SSRF exposure if the QUIC runtime bypasses the existing server-side dial-filter hook.
  - **Mitigation:** §3.2.4 propagates the then-current `odin_xqc_server_runtime_set_dial_filter` slot to every newly created QUIC-backed `odin_server_session` with `odin_server_session_set_dial_filter` before that session can drive a decoded CONNECT_REQ into `odin_dial_start`. The actual pre-dial enforcement remains the current server-session hook at [odin/server_session.c](/Users/tangjiacheng/Downloads/Monorepo/odin/server_session.c:295), where the decoded IPv4 sockaddr is checked after `inet_pton` and before `odin_dial_start`.
  - **Enforcement:** §5 row T8 sends a QUIC-stream CONNECT_REQ for a denied loopback target, asserts that the filter receives the parsed sockaddr, asserts the target listener accepts zero connections, and asserts the stream receives the mapped failure CONNECT_RESP.

Trust-boundary enumeration: the CONNECT_REQ host/port in S1 is the only new peer-supplied byte sequence from this RFC that drives outbound resource selection. Raw QUIC packet bytes, peer UDP addresses, and CIDs remain behind `odin_xqc_udp` and xquic; this runtime receives opaque callback objects and exposes no raw-packet policy hook.

## 5. Testing Strategy

Rows that run a live upstream socket install `signal(SIGPIPE, SIG_IGN)` in the fixture before any socket writes. Every blocking read, accept, or child wait in the integration fixture uses a deadline through `poll`, `select`, `SO_RCVTIMEO`, or the existing bounded child-wait helper so a failed runtime path fails by assertion instead of hanging the suite.

Integration rows T6, T7, T8, and T17 run every runtime API call and every xquic callback on the `odin_event_loop_t` owner thread. After a callback that schedules asynchronous `odin_dial_start` completion, CONNECT_RESP writing, relay forwarding, or session close, the fixture arms a one-shot `odin_event_timer_start` watchdog plus a row-specific poll timer that calls `odin_event_loop_stop` when the named milestone is observed, then calls `odin_event_loop_run` and asserts that the milestone, not the watchdog, stopped the loop. The named milestones are dial completion/upstream accept, CONNECT_RESP flush, tail relay to upstream including EOF, downstream reply relay, and final session `on_close`. T8 deny-filter subcases using the immediate xqc send path do not pump for CONNECT_RESP; their fake send callback asserts that denied CONNECT_RESP bytes are emitted while the direct `stream_read_notify` call is still active, and their sentry listener no-accept checks use a deadline.

### 5.0 Coverage Matrix

| Axis | Value | Rows |
|---|---|---|
| G# | G1 runtime create/register/start/stop | T1, T2, T11 |
| G# | G2 CID lifecycle | T3, T4, T5, T11, T18 |
| G# | G3 stream handoff and cleanup | T2, T6, T7, T9, T10, T11, T12, T13, T14, T15, T16, T17, T18 |
| G# | G4 dial filter | T8 |
| State | create with valid config | T1 |
| State | create with invalid runtime config, null runtime receiver, invalid session factory input, UDP-driver create failure, or ALPN failure | T2 |
| State | valid runtime config fails before ALPN registration because `odin_xqc_udp_create` fails | T2 |
| State | created runtime receives start/stop | T1 |
| State | accepted connection closes normally | T3 |
| State | accepted connection close notification defensively receives a still-linked live stream | T18 |
| State | accepted connection updates CID, update fails, receives a second CID update while closing, receives stale connection callback, or is refused | T4 |
| State | accepted connection cannot allocate context or cannot register CID | T5 |
| State | live connection receives bidirectional stream | T6, T7, T17 |
| State | live connection receives unidirectional stream | T9 |
| State | live connection receives stream-context allocation failure, setup-failed stream, or orphan stream | T10 |
| State | runtime destroy requested with multiple linked connections, including an already-closing/no-CID context, a pre-ALPN accepted context finalized by `server_refuse`, and post-destroy stream create before final connection notification | T11 |
| State | live stream receives xquic close | T15 |
| State | stream/session already closed then xquic reports close | T15 |
| State | live stream receives xquic closing/reset notify | T16 |
| Policy hook | dial filter deny with synchronous denied CONNECT_RESP WRITE-kick flush, replace, clear-to-allow, after-accept future-stream propagation, already-created stream immutability, and destroy-from-filter deferred teardown with post-drain stream cleanup | T8 |
| Completion mode | happy single-call CONNECT_RESP and relay | T6 |
| Completion mode | staged CONNECT_REQ read resumes from `buf_used` after xqc recv backpressure | T17 |
| Completion mode | staged CONNECT_RESP write resumes from `resp_write_off` after a positive 2-byte xqc send followed by xqc send backpressure | T7 |
| Completion mode | terminal stream disposal uses xqc FIN for relay OK and `xqc_stream_close` for dial-deny, decoder-error, or closing/reset terminal paths | T6, T8, T12, T13, T14, T16 |
| Callback return | every direct `runtime_stream_create_notify`, `runtime_stream_read_notify`, `runtime_stream_write_notify`, and `runtime_stream_close_notify` invocation asserts the returned `xqc_int_t`; create/close branches return `XQC_OK`, and read/write notify returns the xqc transport helper result, which is `XQC_OK` in these rows | T4, T6, T7, T8, T9, T10, T11, T12, T13, T14, T15, T16, T17, T18 |
| Decoder branch | CONNECT_REQ OK | T6, T7, T8, T17 |
| Decoder branch | `ERR_BAD_VERSION` maps through session error | T12 |
| Decoder branch | `ERR_BAD_FRAME_TYPE` maps through session error | T13 |
| Decoder branch | `ERR_HOST_LEN_INVALID` maps through session error | T14 |
| Benign-vs-fatal split | benign stale connection callback, closing-context CID update, stale stream close, or null read/write/closing stream user data | T4, T11, T15 |
| Benign-vs-fatal split | fatal UDP-driver create, setup, and CID registration failures | T2, T4, T5, T10 |
| Test hook contract | server-runtime xquic/UDP wrapper records, optional fake ops, paired UDP fake-engine create/destroy, server/connect live-count probes, and reset semantics | T1, T2, T3, T4, T5, T8, T9, T10, T11, T18 |
| Constructor / factory precondition | runtime config validation, null runtime receiver handling, server-session factory validation, UDP-driver create failure, and ALPN rollback | T2 |
| Constructor / factory precondition | valid ALPN user-data consistency check plus null/mismatched failure branches | T3, T5 |
| Constructor / factory precondition | stream-context allocation, xqc stream transport create, connect-session create, or downstream READ-interest failure | T10 |
| Callback-safe lifecycle hand-off | destroy pending while multiple xquic connection contexts await final close or refuse notify | T11 |
| Callback-safe lifecycle hand-off | destroy requested from inside a propagated dial filter defers destructive runtime drain until the active callback entry unwinds after the post-drive denied CONNECT_RESP WRITE kick, then destroys/unlinks the live stream session before final connection close notification | T8 |
| Callback-safe lifecycle hand-off | live xquic stream close and closing/reset notify tear down one session | T15, T16 |
| Callback-safe lifecycle hand-off | direct connection close notification with a linked live stream destroys that session before CID unregister | T18 |
| Post-syscall sub-branch | no new syscall-success/follow-up-failure branch in this RFC; UDP, event-loop, and TCP-dial syscall sub-branches stay owned by RFC-017, RFC-010, and RFC-012 | T1 |

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | Runtime startup registers Odin ALPN and starts/stops UDP driver | Call `odin_xqc_server_runtime_test_reset()`, install `odin_xqc_server_runtime_test_set_ops` with ALPN register/unregister fakes that return `XQC_OK`, and install `odin_xqc_udp_test_set_ops` with an `engine_create` recorder returning a fake engine plus an `engine_destroy` recorder for that fake engine. Create `odin_xqc_server_runtime` with loopback `127.0.0.1:0`, exact `local_addrlen == sizeof(struct sockaddr_in)`, sentinel `xqc_config_t`, sentinel `xqc_engine_ssl_config_t`, and sentinel `xqc_engine_callback_t`; call `start`, `start` again, `stop`, `stop` again, then `destroy` before clearing UDP test ops | `odin_xqc_server_runtime_test_record()->last_udp_create` shows the exact `loop`, `local_addr` pointer, `local_addrlen`, `engine_type == XQC_ENGINE_SERVER`, `engine_config`, `ssl_config`, `engine_callbacks` pointer, and value copy supplied by the caller. Its copied runtime transport callback table has runtime wrappers installed for `server_accept`, `server_refuse`, and `conn_update_cid_notify`; non-runtime, non-UDP slots such as `conn_closing`, `save_token`, and `cert_verify_cb` are `NULL` before RFC-017 installs packet-send callbacks, proving there is no caller transport callback preservation or caller user-data contract in this runtime API. The `engine_create` recorder receives the same engine/SSL config through the real UDP-driver path and receives `user_data == odin_xqc_udp_xqc_user_data(xu)`; `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_ENGINE_REGISTER_ALPN` is recorded once with byte-exact `odin/1`, length `6`, non-null connection and stream callbacks, and runtime user data; first `start` records/calls `odin_xqc_udp_start`, second `start` is a no-op success, first `stop` records/calls `odin_xqc_udp_stop`, second `stop` is a no-op success; `destroy` records ALPN unregister and UDP-driver destroy once, and the fake `engine_destroy` recorder observes the same fake engine exactly once | G1 | unit |
| T2 | Constructor validation, UDP-driver failure, and ALPN rollback | Run runtime subcases with `config == NULL`, `out == NULL`, null `loop`, null `local_addr`, null `engine_callbacks`, `start(NULL)`, `stop(NULL)`, `odin_xqc_server_runtime_set_dial_filter(NULL, cb, ud)`, `destroy(NULL)`, a valid config with `odin_xqc_udp_test_set_ops` installing an `engine_create` fake that returns `NULL` with `errno = EIO` so the real `odin_xqc_udp_create` fails before ALPN registration, and an ALPN-failure valid config with `odin_xqc_udp_test_set_ops` installing an `engine_create` recorder returning a fake engine plus a paired `engine_destroy` recorder while `odin_xqc_server_runtime_test_ops_t.engine_register_alpn` returns `-XQC_EPARAM`; run server-session factory subcases with null `loop`, null `create_downstream`, null `on_close`, and null `out`; initialize `*out` to a sentinel in every failing create subcase | Invalid create/start/stop subcases return `-1` with `errno == EINVAL` and preserve the sentinel `*out` where present; null setter/destroy subcases make no fake calls and leave counters unchanged. The UDP-driver-failure subcase returns `-1` with `errno == EIO`, leaves `*out` unchanged, records UDP create but no ALPN register, calls no UDP-driver destroy, and a later valid create still succeeds. The ALPN-failure subcase returns `-1` with `errno == EIO`, leaves `*out` unchanged, records the failing `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_ENGINE_REGISTER_ALPN`, destroys the already-created UDP driver exactly once, observes the paired fake `engine_destroy` exactly once for the fake engine, and leaves no registered ALPN | G1, G3 | unit |
| T3 | Accepted connection registers CID and close unregisters it | From T1's registered callbacks, fire `server_accept(engine, connA, cidA, odin_xqc_udp_xqc_user_data(xu))`, then fire ALPN `conn_create_notify`, then `conn_close_notify` | `server_accept` returns `0`; `runtime_conn_create_notify(connA, cidA, odin_xqc_udp_xqc_user_data(xu), ctxA)` returns `0`; `odin_xqc_server_runtime_test_record()` logs one `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_REGISTER_CONN` for `xu/cidA`, one `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_SET_TRANSPORT_USER_DATA` with `connA` and `odin_xqc_udp_xqc_user_data(xu)`, and one `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_SET_ALP_USER_DATA` with `connA` and `ctxA`; close destroys zero live streams, logs one `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN` for `cidA`, unlinks the context, and leaves the runtime alive | G2 | unit |
| T4 | CID update registers current CID and refused/stale cleanup is no-op safe | Success subcase: accept `connA/cidA`, fire `conn_update_cid_notify(connA, cidA, cidB, odin_xqc_udp_xqc_user_data(xu))`, then close. Failure subcase: accept `connF/cidF`, fire `stream_create_notify` for one live bidi stream and assert it returns `XQC_OK`, make `odin_xqc_server_runtime_test_ops_t.udp_register_conn` fail registration of `cidG` with `ENOMEM`, fire the update, snapshot the test record, fire a second `conn_update_cid_notify(connF, cidF, cidH, odin_xqc_udp_xqc_user_data(xu))` while the context is closing, then fire a bidirectional `stream_create_notify` before the eventual close and assert it returns `XQC_OK`, and finally fire `conn_close_notify`. Stale subcase: fire update, refuse, and close callbacks for an unknown `connZ`. Refuse subcase: accept `connR/cidR`, then fire `server_refuse(engine, connR, cidR, odin_xqc_udp_xqc_user_data(xu))` | Success records `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_REGISTER_CONN(cidB)` before `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN(cidA)`, stores `cidB` as current, receives `conn_user_data == odin_xqc_udp_xqc_user_data(xu)`, and close unregisters only `cidB`. No caller transport callback is invoked or preserved because §3.2.2 makes the runtime the transport callback table owner. Failure destroys the live stream session, records one `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_CLOSE(engine, cidF)` before returning from the update callback, unregisters `cidF` exactly once during the failed update, and records no current registered CID. The second update on the closing context produces no new register/unregister records, no second `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_CLOSE`, and no current-CID mutation. The post-failure stream create callback returns `XQC_OK`, closes the stream with no session because the context is closing, and the later close notification frees the context without any second unregister. Stale callbacks make no register/unregister calls and return normally. Refuse unregisters `cidR`, frees the context, and creates no stream session | G2, G3 | unit |
| T5 | Accept allocation and CID registration failures refuse connection without context | Allocation subcase arms `odin_xqc_server_runtime_test_fail_next_conn_context_alloc(rt, ENOMEM)` before firing `server_accept(engine, connA, cidA, odin_xqc_udp_xqc_user_data(xu))`. Registration subcase makes `odin_xqc_server_runtime_test_ops_t.udp_register_conn` return `-1` with `errno = ENOMEM` for `cidB` before firing `server_accept` for `connB/cidB`. Then fire ALPN `conn_create_notify` with null and mismatched `conn_proto_data` values, and finally fire a later valid accept | Both failing `server_accept` calls return `-1`; no ALPN or transport user data is installed on `connA` or `connB`; the connection list remains empty; allocation failure records no register or unregister call; registration failure records one `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_REGISTER_CONN(cidB)` and no unregister for `cidB`; mismatched `conn_create_notify` calls return `-1` and allocate nothing; the later valid accept succeeds and registers its CID | G2 | unit |
| T6 | Bidirectional stream completes CONNECT pipeline in one call | Accept `connA`; create a fake `xqc_stream_t` whose `odin_xqc_server_runtime_test_ops_t.stream_get_direction` returns `XQC_STREAM_BIDI`, whose recv queue returns one complete CONNECT_REQ for upstream listener `127.0.0.1:U` plus tail bytes `"tail"` with `fin = 1` on that read sequence, and whose send path accepts all bytes immediately; fire `stream_create_notify` and assert `XQC_OK`, then fire `stream_read_notify` and assert `XQC_OK`; use the §5 owner-thread pump for dial completion/upstream accept, CONNECT_RESP flush, tail relay, downstream reply relay, and final EOF/session-close milestones while the upstream peer accepts with a deadline, reads exactly `"tail"` and then EOF after the relay propagates the xqc FIN with `shutdown_write`, writes `"reply"`, then closes or half-closes its write side so the relay observes upstream EOF | The runtime creates one xqc stream transport and one server session; every direct stream callback returns `XQC_OK`; the pump reaches each named milestone before its watchdog fires; upstream receives exactly `"tail"` before relay bytes and sees EOF; the stream send log begins with byte-exact CONNECT_RESP OK (`01 02 00 00`), later contains `"reply"`, and finally records the xqc transport FIN from relay shutdown; after both directions have reached EOF and both relay half-closes are propagated, session `on_close` removes and destroys the stream context with relay OK, and no `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_CLOSE` is recorded because relay OK relies on the existing FIN path | G3 | integration |
| T7 | CONNECT_RESP write resumes after positive xqc short send and backpressure | Same owner-thread pump, xqc FIN, callback-return assertions, and upstream EOF/half-close sequence as T6, except the fake stream send path accepts exactly the first two CONNECT_RESP bytes on the first write attempt, returns `-XQC_EAGAIN` on the immediate next send from the same response-drain loop, and accepts only the remaining two CONNECT_RESP bytes after `stream_write_notify` is fired and asserted to return `XQC_OK` | Before `stream_write_notify`, the pump confirms upstream receives no tail bytes and relay has not started, and the stream send log contains only the CONNECT_RESP prefix bytes `01 02`. After `stream_write_notify` returns `XQC_OK`, the pump reaches CONNECT_RESP flush, tail relay, downstream reply relay, and final EOF/session-close milestones; the CONNECT_RESP portion of the stream send log before any relayed reply bytes is exactly `01 02 00 00` with no duplicate `01 02` prefix, upstream receives `"tail"` and EOF, relay forwards `"reply"` back to the stream, and session `on_close` fires only after the upstream EOF and xqc FIN have both been propagated | G3 | integration |
| T8 | Dial filter setter denies, replaces, clears, applies only to future stream sessions, and tolerates destroy from inside the filter | Use fresh accepted connections and deadline-backed upstream or sentry listeners per subcase. Every subcase asserts each direct `stream_create_notify` and `stream_read_notify` return is `XQC_OK`; allow subcases use the §5 owner-thread pump for relay milestones, while deny subcases set a test flag around the direct `stream_read_notify` call and configure the fake xqc send path to accept the whole CONNECT_RESP immediately only if that flag is still set, proving the denied bytes are emitted inside the read callback before any owner-thread pump or separate `stream_write_notify`. Deny subcase: install runtime dial filter `denyA` returning `EACCES`; accept `connA`; create a bidi stream with CONNECT_REQ for sentry listener `127.0.0.1:V`; fire `stream_read_notify` and assert `XQC_OK`; then perform only the deadline-backed no-accept check for the sentry listener. Replace subcase: call `odin_xqc_server_runtime_set_dial_filter(rt, denyB, udB)`, then `odin_xqc_server_runtime_set_dial_filter(rt, allowB, udAllowB)` before creating a stream for upstream listener `127.0.0.1:W` using T6's valid CONNECT_REQ/tail setup and milestones. Clear subcase: call `odin_xqc_server_runtime_set_dial_filter(rt, denyC, udC)`, then `odin_xqc_server_runtime_set_dial_filter(rt, NULL, stale_ud)` before creating a stream for upstream listener `127.0.0.1:X` using T6's setup and milestones. After-accept subcase: accept `connD` while no deny filter is installed, then call `odin_xqc_server_runtime_set_dial_filter(rt, denyD, udD)` before creating a future bidi stream on `connD` for sentry listener `127.0.0.1:Y`; fire `stream_read_notify`, assert `XQC_OK`, and then perform only the deadline-backed no-accept check. Already-created stream subcase: call `odin_xqc_server_runtime_set_dial_filter(rt, allowE, udAllowE)`, create a bidi stream whose recv queue first returns a CONNECT_REQ prefix and `-XQC_EAGAIN`, fire `stream_read_notify` and assert `XQC_OK` so the server session exists but has not dialed, then call `odin_xqc_server_runtime_set_dial_filter(rt, denyE, udDenyE)`, append the remaining CONNECT_REQ bytes plus tail bytes, fire `stream_read_notify` again and assert `XQC_OK`, then pump through the T6 allow milestones. Destroy-from-filter subcase: record baseline `odin_server_session_test_live_count()` and `odin_connect_session_test_live_count()`, install runtime dial filter `destroyF` that calls `odin_xqc_server_runtime_destroy(rt)` and returns `EACCES`; accept `connF`; create a bidi stream with CONNECT_REQ for sentry listener `127.0.0.1:Z` and the same immediate-send fake/read-callback flag assertion; after `stream_create_notify`, save the stream's non-null current fake xquic user-data value as `saved_transportF` and assert both live-count probes increased by one; fire `stream_read_notify` and assert `XQC_OK`; after the deferred drain has recorded the connection close request but before final `conn_close_notify`, fire direct `runtime_stream_close_notify(streamF, saved_transportF)` and assert `XQC_OK`; then fire `conn_close_notify` for `connF` | Deny subcase calls `denyA` once with `AF_INET 127.0.0.1:V`, the fake send callback observes the read-callback flag while accepting byte-exact CONNECT_RESP with `ODIN_SERVER_SESSION_RESP_CODE_OTHER` from the synchronous xqc WRITE kick, records one `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_CLOSE` for that xquic stream, closes with no relay, and the sentry listener accepts zero connections and receives zero bytes. Replace subcase calls `allowB` once with `udAllowB`, never calls `denyB`, and completes the T6 upstream accept, tail relay, CONNECT_RESP OK, and EOF assertions. Clear subcase calls neither `denyC` nor any stale cleared callback, completes the T6 allow path to `127.0.0.1:X`, and proves `cb == NULL` clears the stored hook. After-accept subcase calls `denyD` once even though `connD` was accepted before the setter call, the fake send callback observes the read-callback flag while accepting the denied CONNECT_RESP, the sentry listener accepts zero connections, records one `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_CLOSE` for that denied stream, and no outbound dial occurs. Already-created stream subcase calls only `allowE` with `udAllowE` when the CONNECT_REQ completes, never calls `denyE`, and completes the allow path, proving setter calls do not mutate already-created stream sessions. Destroy-from-filter subcase calls `destroyF` once, makes no outbound dial or sentry accept, observes inside the filter that no `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_CLOSE`, ALPN unregister, UDP-driver destroy, or stream user-data clear has been recorded yet, then the fake send callback observes the read-callback flag while the post-drive xqc WRITE kick emits byte-exact denied CONNECT_RESP and the terminal stream path clears xquic user data before `stream_read_notify` returns. After `stream_read_notify` unwinds, exactly one deferred `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_CLOSE` is recorded for `connF`'s registered CID; the xqc transport `set_user_data` recorder has one clear-to-`NULL` record for `streamF`, the fake stream's current user-data slot is `NULL`, `odin_server_session_test_live_count()` and `odin_connect_session_test_live_count()` have returned to their pre-stream baselines, and direct `runtime_stream_close_notify(streamF, saved_transportF)` returns `XQC_OK` without a stream close, CID unregister, server-session destroy, or live-count change, proving `saved_transportF` is no longer live in the runtime transport map. Final `conn_close_notify` unregisters that CID, unregisters `odin/1`, destroys the UDP driver, and frees the runtime exactly once; it records no additional stream user-data clear, no second stream-session destroy, and no server/connect live-count decrement because the deferred drain already removed the stream context from the per-connection stream list. Every callback-return assertion is `XQC_OK`, every allow-path pump stops on its named milestone before the watchdog, and the P2 ASan gate covers the immediate denied WRITE-kick and destroy-from-filter subcases so premature stream/session/context free fails even if call records are otherwise correct | G4, S1 | integration |
| T9 | Unidirectional stream is closed without session | Accept `connA`; fire `stream_create_notify` for a fake stream whose `odin_xqc_server_runtime_test_ops_t.stream_get_direction` returns `XQC_STREAM_UNI` and assert the callback returns `XQC_OK` | `odin_xqc_server_runtime_test_record()` logs one `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_GET_DIRECTION` and one `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_CLOSE`; no xqc stream transport is created; no server session is created; the connection remains live and a later bidirectional stream can still run T6's setup | G3 | unit |
| T10 | Stream and server-session setup failures close or roll back without poisoning later work | Runtime subcases on one accepted connection: subcase A arms `odin_xqc_server_runtime_test_fail_next_stream_context_alloc(rt, ENOMEM)`; subcase B snapshots the `odin_xqc_stream_transport_test_ops_t.set_user_data` recorder and the fake stream's current xquic user-data slot, then arms `odin_xqc_stream_transport_test_fail_next_create(ENOMEM)` so `odin_xqc_stream_transport_create` itself returns `-1`; subcase C arms `odin_connect_session_test_fail_next_create_server(ENOMEM)` so `odin_connect_session_create_server` itself returns `-1` after the xqc transport factory returned; subcase D makes `odin_xqc_server_runtime_test_ops_t.get_conn_alp_user_data_by_stream` return `NULL` and fires `stream_create_notify` for a bidirectional stream with no ALPN user data. Each runtime failed stream asserts `stream_create_notify` returns `XQC_OK`. After each runtime failed stream, create a fresh valid bidirectional stream using T6's setup and callback-return assertions. Server-session subcase E calls `odin_server_session_create_with_transport` directly with a factory returning a test-local fake `odin_transport_t` whose `set_interest(ODIN_TRANSPORT_READ)` returns `-1` with `errno = EEXIST`; initialize `*out` to a sentinel and record fake destroy calls plus `odin_connect_session_test_live_count()` before and after | Runtime subcases A-D each return `XQC_OK`, record one `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_CLOSE`, create no live session, and leave the connection context linked when one existed. Subcase A creates no transport/session and installs no xquic stream user data. Subcase B proves the runtime factory called `odin_xqc_stream_transport_create` and handled its returned failure, records no non-null `set_user_data` install, leaves the fake stream's current user-data slot `NULL`, and leaves no live or stale transport pointer reachable through the runtime stream maps. Subcase C destroys the transport wrapper returned by the factory and the `odin_xqc_stream_transport_test_ops_t.set_user_data` slot records xquic stream user data cleared before close. The later valid stream completes T6's CONNECT_RESP OK and relay EOF assertions. Subcase E returns `-1` with `errno == EEXIST`, leaves sentinel `*out` untouched, destroys the fake downstream transport exactly once, and restores the §3.2.1 connect-session live count to the pre-call value, proving the successfully created connect session was destroyed during READ-interest rollback | G3 | unit |
| T11 | Runtime destroy waits for final close/refuse across registered, already-closing, and pre-ALPN connections | Create and start runtime, accept `connA/cidA`, `connB/cidB`, and pre-ALPN `connC/cidD` without firing ALPN `conn_create_notify` for `connC`. On `connA`, fire `stream_create_notify` for one bidi stream that is still in CONNECT_REQ decode, assert `XQC_OK`, and save its transport pointer for the close-only stale lookup subcase. On `connB`, fire `stream_create_notify` for one live bidi stream and assert `XQC_OK`, make CID update registration for `cidE` fail with `ENOMEM`, and fire `conn_update_cid_notify(connB, cidB, cidE, odin_xqc_udp_xqc_user_data(xu))` so `connB` is already closing with no registered CID and its stream has already been destroyed; snapshot `connB` close and unregister counts. Call `odin_xqc_server_runtime_destroy(rt)` outside xquic callbacks. After destroy returns, assert `connA`'s fake stream current xquic user data is `NULL`; fire `stream_read_notify` and assert `XQC_OK`, fire `stream_write_notify` and assert `XQC_OK`, fire `stream_closing_notify`, fire `stream_close_notify` with `NULL` and assert `XQC_OK`; fire one direct `stream_close_notify` with the saved stale transport pointer and assert `XQC_OK`; snapshot the connection-close count for `connA`, fire `stream_create_notify` for a new fake bidirectional stream whose ALPN lookup still resolves to `ctxA` and assert `XQC_OK`; then fire `conn_close_notify` for `connA`, assert the runtime has not finished, fire `conn_close_notify` for `connB`, assert the runtime still has not finished because pre-ALPN `ctxC` remains linked, and finally fire `server_refuse(engine, connC, cidD, odin_xqc_udp_xqc_user_data(xu))` | Destroy first stops the UDP driver, marks runtime destroy pending, marks `ctxA` and pre-ALPN `ctxC` closing before stream destruction, destroys and unlinks `connA`'s live stream session, clears xquic stream user data to `NULL`, records exactly one `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_CLOSE(engine, cidA)` and exactly one `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_CLOSE(engine, cidD)`, sets each context's destroy-close-requested guard, keeps both CIDs registered, and does not call `odin_xqc_udp_destroy` while any connection context remains linked. For `connB`, destroy observes `was_closing == true` and no registered CID, records no second `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_CONN_CLOSE`, no second unregister for `cidB`, and no second stream-session destroy. Every create/read/write/close callback in the row returns `XQC_OK`. The null read/write/closing callbacks after destroy no-op through the transport helper contract, the null close callback no-ops through the runtime close branch, and the stale close-only lookup does not touch freed stream context, create a session, unregister a CID, or close a stream a second time. The post-destroy bidirectional stream on `connA` hits the closing guard, records one `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_CLOSE`, creates no xqc stream transport or server session, records no CID register/unregister, and the `connA` close count remains unchanged from the pre-stream snapshot, proving the active-entry leave drain did not issue a duplicate destroy-driven connection close while the CID stayed registered. `connA` close notification unregisters only `cidA` and frees `ctxA`, but does not unregister `odin/1`, destroy the UDP driver, or free the runtime because `ctxB` and `ctxC` remain linked. `connB` close notification observes no registered CID and frees `ctxB` without unregistering any CID, but still does not finish the runtime. Final `server_refuse` unregisters only `cidD`, frees pre-ALPN `ctxC`, returns to the active-entry leave wrapper, and only that leave path unregisters `odin/1`, destroys the UDP driver exactly once, and frees the runtime without use-after-free | G1, G2, G3 | unit |
| T12 | Bad CONNECT_REQ version closes xqc stream session through pipeline error | Accept `connA`; create bidi stream whose recv queue starts with a full CONNECT_REQ-sized frame with version byte `0x7f` and otherwise valid fields; fire `stream_create_notify` and assert `XQC_OK`, then fire `stream_read_notify` and assert `XQC_OK` | The existing connect-session decoder path reports `EPROTO` through the server session; no upstream dial is attempted; stream context is removed and destroyed; one `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_CLOSE` is recorded for that xquic stream because no CONNECT_RESP can be sent after decoder failure; the runtime remains alive for a later valid stream | G3 | unit |
| T13 | Bad CONNECT_REQ frame type closes xqc stream session through pipeline error | Accept `connA`; create bidi stream whose recv queue starts with version `0x01` and frame type `0x7f`; fire `stream_create_notify` and assert `XQC_OK`, then fire `stream_read_notify` and assert `XQC_OK` | The stream follows the same observable error path as T12: no upstream dial, stream context removed, exactly one `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_CLOSE`, runtime remains alive | G3 | unit |
| T14 | Zero host length closes xqc stream session through pipeline error | Accept `connA`; create bidi stream whose recv queue starts with version `0x01`, frame type CONNECT_REQ, and `host_len == 0`; fire `stream_create_notify` and assert `XQC_OK`, then fire `stream_read_notify` and assert `XQC_OK` | The stream follows the same observable error path as T12: no upstream dial, stream context removed, exactly one `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_CLOSE`, runtime remains alive | G3 | unit |
| T15 | Live, stale-close, and null stream callbacks are scoped to one stream | Accept `connA`; create one live bidi stream still in CONNECT_REQ decode, assert its `stream_create_notify` returned `XQC_OK`, and fire `stream_close_notify(stream, live_transport_pointer)` asserting `XQC_OK`. Then complete T6 on a second stream until its server session has removed its stream context and cleared xquic stream user data; fire `stream_close_notify(stream2, NULL)` and assert `XQC_OK`, fire another direct `stream_close_notify(stream2, stale_transport_pointer)` and assert `XQC_OK`, and fire `stream_read_notify(stream2, NULL)` / `stream_write_notify(stream2, NULL)` with each returning `XQC_OK` | The live close destroys exactly that stream's server session, removes its stream context, does not unregister the connection CID, and leaves `connA` available for later streams. The stale-close, null close, null read, and null write callbacks all return `XQC_OK` without creating a session, closing a stream a second time, unregistering a CID, or changing the runtime connection count; a later valid stream on the same connection still succeeds | G3 | unit |
| T16 | Stream closing/reset notify tears down only the affected stream | Accept `connA`; create one live bidi stream with no upstream dial yet and assert `stream_create_notify` returns `XQC_OK`; fire `stream_closing_notify(stream, XQC_EPROTO, live_transport_pointer)`, snapshot stream-close records for that stream, then create a fresh valid bidirectional stream using T6's setup and callback-return assertions | The closing notify forwards through `odin_xqc_stream_transport_closing_notify`, the server session observes the transport error and removes exactly that stream context, no upstream dial is attempted for the reset stream, exactly one `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_STREAM_CLOSE` is recorded for the reset xquic stream, `connA` remains linked with its CID registered, and the later valid stream completes T6's CONNECT_RESP OK and relay assertions | G3 | unit |
| T17 | CONNECT_REQ read resumes after xqc recv backpressure | Install a counting allow dial filter; accept `connA`; create a fake `xqc_stream_t` whose direction is `XQC_STREAM_BIDI`, whose recv queue first returns a prefix of a valid CONNECT_REQ for upstream listener `127.0.0.1:U` and then `-XQC_EAGAIN`; fire `stream_create_notify` and assert `XQC_OK`, fire the first `stream_read_notify` and assert `XQC_OK`; assert the live transport interest still includes `ODIN_TRANSPORT_READ` with `odin_xqc_stream_transport_test_interest`. Then append the remaining CONNECT_REQ bytes plus tail bytes `"tail"` with `fin = 1`, fire a second `stream_read_notify` and assert `XQC_OK`, and use the same §5 owner-thread pump milestones, upstream read-to-EOF, reply, and close/half-close sequence as T6 | After the first read notify returns `XQC_OK`, the dial-filter call count is zero, no outbound dial is attempted, no CONNECT_RESP bytes are sent, no stream/session close fires, and the same session remains live with the prefix retained in `buf_used`. After the second read notify returns `XQC_OK`, the filter call count is one and the pump reaches the normal T6 milestones: upstream receives exactly `"tail"` and EOF, the stream send log begins with CONNECT_RESP OK (`01 02 00 00`), relay forwards `"reply"`, and session `on_close` fires only after both EOF directions complete | G3 | integration |
| T18 | Direct connection close notification cleans up a still-linked live stream | Accept `connA/cidA`; fire `stream_create_notify` for one live bidi stream that is still in CONNECT_REQ decode and assert `XQC_OK`; without firing stream close first, call `runtime_conn_close_notify(connA, cidA, odin_xqc_udp_xqc_user_data(xu), ctxA)` directly from the unit fixture | The close notification destroys exactly that live server session, the xqc transport test `set_user_data` recorder sees the stream user data cleared to `NULL`, the stream context is removed from both runtime maps, `ODIN_XQC_SERVER_RUNTIME_TEST_CALL_UDP_UNREGISTER_CONN(cidA)` is recorded exactly once, the connection context is unlinked and freed, and the ASan P2 gate reports no use-after-free in this defensive branch | G2, G3 | unit |

## 6. Implementation Plan

- **P1. Land server-session factory and QUIC runtime skeleton with red-verifiable `T1`-`T18`.**
  - **Scope:** add the §3.2.1 declarations to `odin/server_session.h` and a linkable `odin_server_session_create_with_transport` skeleton that validates arguments, calls the caller factory for valid inputs so rollback tests can observe returned-transport cleanup, and then stops before constructing `odin_connect_session_create_server` or driving CONNECT_REQ through the pipeline; keep `odin_server_session_create`'s current fd behavior intact. Add `odin/testing/connect_session_internal_test.h` with `odin_connect_session_test_fail_next_create_server` and `odin_connect_session_test_live_count`, backed by the §3.2.1 counter instrumentation and gated by new `ODIN_CONNECT_SESSION_TESTING`; add `odin_server_session_test_live_count` to the existing `odin/testing/server_session_internal_test.h`, backed by the §3.2.1 server-session counter instrumentation and gated by the existing `ODIN_SERVER_SESSION_TESTING`; and add one-shot `odin_xqc_stream_transport_test_fail_next_create` to `odin/testing/transport_xqc_internal_test.h` under the existing `ODIN_TRANSPORT_XQC_TESTING` gate, consumed before allocation and before `xqc_stream_set_user_data`, so T10 can drive callee-scoped failures instead of server-runtime mirror branches. Add `odin/server_xqc_runtime.c` and `odin/server_xqc_runtime.h` with the §3.2.2 public API, `ODIN_XQC_SERVER_ALPN`, and a bounded skeleton that allocates a runtime and stores the runtime-owned transport callback table but intentionally omits real ALPN registration, CID bookkeeping, stream session creation, dial-filter propagation, terminal xquic stream close on nonzero session errors, active-entry deferred destroy from inside a dial filter, destroy-pending connection cleanup, and the §3.2.5 wrapper coverage. Add `odin/testing/server_xqc_runtime_internal_test.h`, `odin/testing/server_xqc_runtime_testing.c`, and `odin/testing/server_xqc_runtime_unittests.cpp` with T1-T18, gated by `ODIN_XQC_SERVER_RED=1` so the default suite skips them while the red command executes the assertions. The server-runtime internal test header exposes the exact §3.2.5 symbols `odin_xqc_server_runtime_test_ops_t`, `odin_xqc_server_runtime_test_set_ops`, `odin_xqc_server_runtime_test_reset`, `odin_xqc_server_runtime_test_record`, `odin_xqc_server_runtime_test_fail_next_conn_context_alloc`, and `odin_xqc_server_runtime_test_fail_next_stream_context_alloc`; its record covers the runtime-owned calls to `xqc_engine_register_alpn`, `xqc_engine_unregister_alpn`, `xqc_conn_set_transport_user_data`, `xqc_conn_set_alp_user_data`, `xqc_conn_close`, `xqc_stream_get_direction`, `xqc_stream_close`, `xqc_get_conn_alp_user_data_by_stream`, `odin_xqc_udp_register_conn`, and `odin_xqc_udp_unregister_conn`. `xqc_stream_set_user_data` remains covered by the existing `odin_xqc_stream_transport_test_ops_t.set_user_data` slot in `odin/testing/transport_xqc_internal_test.h`; T2's UDP-driver create failure uses the existing `odin/testing/xqc_udp_internal_test.h` `odin_xqc_udp_test_ops_t.engine_create` slot returning `NULL`, with no server-runtime replacement hook for `odin_xqc_udp_create`. T1 and the T2 ALPN-rollback subcase install complete `odin_xqc_udp_test_ops_t` fakes with both `engine_create` and `engine_destroy` whenever `engine_create` returns a fake engine, and their assertions observe fake `engine_destroy` before clearing UDP test ops. The server-runtime-owned xquic/UDP/runtime hook call sites are compiled only under `ODIN_XQC_SERVER_RUNTIME_TESTING`, the UDP-driver engine-create and engine-destroy hooks are compiled only under `ODIN_XQC_UDP_TESTING`, the connect-session hook is compiled only under `ODIN_CONNECT_SESSION_TESTING`, the server-session live-count hook is compiled only under `ODIN_SERVER_SESSION_TESTING`, and the xqc-stream-transport hooks are compiled only under `ODIN_TRANSPORT_XQC_TESTING`. Extend `odin/testing/BUILD.gn` with `odin_xqc_server_runtime_testing_config` and `odin_connect_session_testing_config`, apply both new configs plus the existing `odin_server_session_testing_config`, `odin_transport_xqc_testing_config`, and `odin_xqc_udp_testing_config` to `odin_unittests`, include the testing wrappers in `odin_unittests`, and keep production `odin` without any test-hook symbols. Add production `source_set("odin_server_xqc_runtime")` depending on `:odin_event_loop`, `:odin_server_session`, `:odin_transport_xqc`, `:odin_xqc_udp`, and `//xquic`; add it to `source_set("odin")`. Add a small scope-check action that fails if `odin/server_runtime.c`, `odin/server_runtime.h`, `odin/cli_server.c`, the `odin_server_runtime` GN target, or the `odin_cli_server` GN target includes `xquic`, `server_xqc_runtime`, or `transport_xqc`, preserving the TCP runtime as a separate caller.
  - **Depends on:** None.
  - **Done when:** `./tool/gn gen out/xqc_server_mac --args='target_os="mac"'`, `./tool/gn gen out/xqc_server_mac_arm64 --args='target_os="mac" target_cpu="arm64"'`, `./tool/gn gen out/xqc_server_linux_x64 --args='target_os="linux" target_cpu="x64"'`, `./tool/gn gen out/xqc_server_ios_sim --args='target_os="ios" target_environment="simulator" target_cpu="arm64"'`, and `./tool/gn gen out/xqc_server_ios_device --args='target_os="ios" target_environment="device" target_cpu="arm64"'` resolve; `./tool/ninja -C out/xqc_server_mac odin_main odin_unittests odin_server_xqc_runtime_scope_check tests` builds, and the matching `odin_main`, `odin_unittests`, and `odin_server_xqc_runtime_scope_check` targets build for the four cross-compile output directories. The scope-check action scans `odin/server_runtime.c`, `odin/server_runtime.h`, `odin/cli_server.c`, the `odin_server_runtime` GN target, and the `odin_cli_server` GN target for `xquic`, `server_xqc_runtime`, or `transport_xqc` references. The red-verification command `ODIN_XQC_SERVER_RED=1 out/xqc_server_mac/odin_unittests --gtest_filter='OdinXqcServerRuntimeTest.*:OdinServerSessionTransportTest.*'` executes T1-T18 and fails them against the skeleton: T1 because runtime-config propagation, runtime-owned transport callback table assertions, ALPN registration, and start/stop wrapper records are incomplete; T2 because validation, UDP-driver failure, and ALPN rollback are incomplete; T3-T5 because connection contexts, the valid and invalid `conn_create_notify` branches, connection-context allocation failure, CID registration, and the closing-context CID-update no-op are absent; T4 and T6-T18 because explicit create/read/write/close callback-return assertions are not satisfied by the skeleton's missing stream callback contract; T6-T8 and T17 because no stream is handed to the server session pipeline, no staged CONNECT_REQ resume can occur, T7's positive-short CONNECT_RESP write cannot advance `resp_write_off` to byte 2 and later resume without duplicating the prefix, dial-filter replace/clear/future-stream semantics and destroy-from-filter deferred teardown plus post-unwind stream-drain assertions are absent, the immediate denied CONNECT_RESP WRITE-kick assertion and §3.2.1 post-drive downstream-interest deferral are absent, terminal xquic stream close on denied CONNECT is absent, relay EOF/half-close completion is absent, and the owner-thread pump cannot reach dial completion, CONNECT_RESP flush, relay-progress, or final-EOF milestones; T9-T10 because stream direction, stream-context allocation failure, callee-scoped setup failure before any xqc stream-user-data install, and orphan-stream branches are absent; T11 because destroy does not mark live connection contexts closing, skip already-closing/no-CID contexts, guard duplicate destroy-driven connection close requests, reject post-destroy streams, treat `server_refuse` as a final pre-ALPN notification, or defer runtime destruction to active-entry leave after the final close/refuse notification across multiple linked contexts; T12-T14 because decoder errors do not traverse the xqc transport path or close the xquic stream; T15 because close/null notifies are not guarded; T16 because closing/reset notify is not forwarded into exactly one terminal xqc stream close; T18 because direct `conn_close_notify` does not defensively destroy a still-linked stream session before CID unregister. The default host run `out/xqc_server_mac/odin_unittests --gtest_brief=1` reports T1-T18 skipped and exits zero with all pre-existing Odin tests green, including `OdinServerRuntimeTest.*`, `OdinServerSessionTest.*`, `OdinXqcStreamTransportTest.*`, and `OdinXqcUdpTest.*`. Host-runnable enumeration: T1-T18 run only in `out/xqc_server_mac/odin_unittests` on the host macOS architecture. Cross-compile-only enumeration: `out/xqc_server_mac_arm64/odin_unittests`, `out/xqc_server_linux_x64/odin_unittests`, `out/xqc_server_ios_sim/odin_unittests`, and `out/xqc_server_ios_device/odin_unittests` are built but not executed in this environment; their xquic, UDP, and event-loop branches are compiled but not runtime-verified in P1.

- **P2. Implement the QUIC runtime and turn `T1`-`T18` green.**
  - **Scope:** replace the P1 skeleton with the §3.2.1 transport-factory implementation in `odin/server_session.c` while preserving the existing fd constructor and `odin_server_runtime.c` call sites; implement the server-session `connect_drive_depth` / `pending_downstream_interest` discipline so `session_on_req_decoded` can set the denied CONNECT_RESP while downstream WRITE interest is applied only after `odin_connect_session_drive` returns, and ensure `server_session_ready` does not read `ss->s` after that post-drive setter can synchronously re-enter; implement `odin/server_xqc_runtime.c` per §3.2.2-§3.2.5: create `odin_xqc_udp` with `XQC_ENGINE_SERVER`, install the runtime-owned transport callback table, register/unregister `odin/1`, implement explicit start/stop, accept/refuse/update/close connection contexts, register/unregister CIDs through `odin_xqc_udp`, update the current CID without forwarding caller transport callbacks, close and unregister on CID-update registration failure, make later CID-update callbacks on a closing context no-op, mark every live connection context closing before aborting live stream sessions during runtime destroy, set a per-connection `destroy_close_requested` flag before the first destroy-driven `xqc_conn_close`, skip second close/unregister for already-closing, already-destroy-close-requested, or no-registered-CID contexts, reject post-destroy streams before connection close/refuse, defer physical runtime destruction until the final live xquic connection context has closed or refused, defer destructive runtime drain when `odin_xqc_server_runtime_destroy(rt)` is called from inside a propagated dial filter while an active runtime callback is on the stack, complete destroy-pending finalization only from the active-entry leave path after final `runtime_conn_close_notify` or `runtime_server_refuse`, defensively destroy any still-linked stream sessions from `runtime_conn_close_notify`, create bidirectional xqc stream transports through the factory path, propagate the runtime dial filter to each future stream session with replace-only and `cb == NULL` clear semantics while leaving already-created stream sessions unchanged, close non-bidirectional, stream-context-allocation-failed, setup-failed, and nonzero server-session terminal streams with `xqc_stream_close`, forward read/write/closing notify helpers, make null read/write/closing and stale close notify benign, implement the §3.2.5 test wrappers under `ODIN_XQC_SERVER_RUNTIME_TESTING`, and preserve READ interest across staged CONNECT_REQ `ODIN_PROTO_NEED_MORE` over the xqc stream transport. Remove the `ODIN_XQC_SERVER_RED` skip gate so T1-T18 assert in the default host suite. Keep `odin/server_runtime.c`, `odin/server_runtime.h`, `odin/cli_server.c`, the `odin_server_runtime` target, and the `odin_cli_server` target free of any xquic include, dependency, or call-site change; the TCP runtime continues to use `odin_server_session_create`.
  - **Depends on:** P1.
  - **Done when:** The P1 build commands still succeed; `out/xqc_server_mac/odin_unittests --gtest_filter='OdinXqcServerRuntimeTest.*:OdinServerSessionTransportTest.*'` passes T1-T18 un-gated on the host macOS architecture, and `out/xqc_server_mac/odin_unittests --gtest_filter='OdinServerRuntimeTest.*:OdinServerSessionTest.*:OdinXqcStreamTransportTest.*:OdinXqcUdpTest.*'` remains green. The unfiltered host run `out/xqc_server_mac/odin_unittests --gtest_brief=1` exits zero. Host-runnable enumeration: T1-T18 all run in `out/xqc_server_mac/odin_unittests`; T4 and T6-T18 assert every direct create/read/write/close stream callback return (`XQC_OK` for create/close branches and the xqc transport helper result, `XQC_OK` in these paths); T6-T8 and T17 use host-runnable TCP upstream listeners only as the existing server-session pipeline does, not as a QUIC transport substitute, and each owner-thread `odin_event_loop_run` pump stops on the named dial-completion, CONNECT_RESP-flush, relay-progress, or final-EOF milestone before its watchdog. T7 scripts the fake xqc send path as a two-byte CONNECT_RESP short write, an immediate `-XQC_EAGAIN`, and a later two-byte suffix write from `stream_write_notify`, then asserts no tail relay before resume and one byte-correct `01 02 00 00` CONNECT_RESP prefix with no duplicate `01 02`. T8's deny and destroy-from-filter subcases assert that the immediate fake xqc send path emits byte-exact denied CONNECT_RESP bytes from the post-drive WRITE kick while a test flag proves the direct `stream_read_notify` call is still active, before any owner-thread pump or separate `stream_write_notify`; T8 also asserts, before final `conn_close_notify`, that the deferred drain has cleared fake xquic stream user data to `NULL`, removed the saved transport pointer from the runtime map, and returned `odin_server_session_test_live_count()` and `odin_connect_session_test_live_count()` to their pre-stream baselines; after final `conn_close_notify`, T8 asserts no second stream-session destroy, stream user-data clear, or live-count decrement occurred. T11 asserts that post-destroy callback unwind does not issue a duplicate destroy-driven `xqc_conn_close` for a still-registered CID, that `runtime_conn_close_notify` does not finish the runtime inside its callback body, and that final pre-ALPN `runtime_server_refuse` completes destroy-pending runtime destruction only through active-entry leave. Cross-compile-only enumeration: `out/xqc_server_mac_arm64/odin_unittests`, `out/xqc_server_linux_x64/odin_unittests`, `out/xqc_server_ios_sim/odin_unittests`, and `out/xqc_server_ios_device/odin_unittests` are built but not executed; their alternate event-loop, UDP, socket, xquic, and toolchain branches are verified by successful cross-compile and code review only. `./tool/gn gen out/xqc_server_mac_asan --args='target_os="mac" is_asan=true'`, `./tool/ninja -C out/xqc_server_mac_asan odin_unittests`, and `out/xqc_server_mac_asan/odin_unittests --gtest_filter='OdinXqcServerRuntimeTest.*:OdinServerSessionTransportTest.*'` exit without AddressSanitizer reports, backing T8, T10, T11, T15, T16, and T18's immediate denied WRITE-kick, destroy-from-filter, setup-failure, destroy-pending, null post-destroy callback, stale-close, reset-callback, and direct connection-close live-stream cleanup lifecycle assertions. The scope check reports that `odin/server_runtime.c`, `odin/server_runtime.h`, `odin/cli_server.c`, the `odin_server_runtime` target, and the `odin_cli_server` target have no xquic, `server_xqc_runtime`, or `transport_xqc` dependency; production `out/xqc_server_mac/odin` and cross-compiled `out/xqc_server_linux_x64/odin` contain no `odin_xqc_server_runtime_test_*` symbols; and `./tidy_odin.sh` exits clean over the touched Odin files.
