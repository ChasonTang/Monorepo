# RFC-027: Client-Side Odin QUIC Stream Runtime

## 1. Summary

Add `odin/client_xqc_runtime.c` and `odin/client_xqc_runtime.h`, a client-side xquic runtime that connects to an Odin QUIC server with ALPN `odin/1`, owns the active client CID lifecycle for UDP WRITE recovery, and hands one bidirectional QUIC stream per local HTTPS_PROXY connection to the existing RFC-023 client session pipeline through `odin_xqc_stream_transport`.

## 2. Goals

- **G1.** Provide a client QUIC runtime that creates an xquic client UDP driver, registers exactly one Odin ALPN value (`odin/1`), starts/stops the UDP driver explicitly, and initiates one xquic connection to a caller-configured Odin server endpoint.
- **G2.** Keep the active client connection CID registered with `odin_xqc_udp` while the xquic connection is usable; unregister stale or closed CIDs exactly once, and close the connection if a new active CID cannot be registered.
- **G3.** For every local HTTPS_PROXY connection accepted by a caller, handed to the runtime, and successfully parsed as an HTTP CONNECT request, create exactly one bidirectional xquic stream, wrap it with `odin_xqc_stream_transport`, and run the existing RFC-023 client CONNECT_REQ / CONNECT_RESP / relay pipeline over that stream.
- **G4.** Preserve the current TCP client path: `odin_client_session_create` and `odin/cli_client.c` continue to use the TCP fd-dial path, while this RFC exposes only the minimal upstream-transport constructor needed by the QUIC runtime.

## 3. Design

### 3.1 Overview

`odin/client_xqc_runtime` is a sibling of `odin/server_xqc_runtime`, not a replacement for `odin/cli_client.c` or the TCP client runtime. The new runtime owns xquic client integration: it creates `odin_xqc_udp` with `XQC_ENGINE_CLIENT`, registers Odin ALPN callbacks, starts one QUIC connection with `xqc_connect`, tracks the current client CID, accepts caller-owned local HTTPS_PROXY stream fds through a small public entry point, and gives each local connection an RFC-023 `odin_client_session` whose upstream transport is produced by an xquic bidirectional stream factory.

The existing `odin_client_session` remains the CONNECT pipeline. This RFC adds an upstream-transport constructor so the same parser, CONNECT_REQ encoder, CONNECT_RESP decoder, HTTP response mapper, tail injection, and relay code can run over either the current fd transport or an xquic stream transport. The existing TCP constructor at [odin/client_session.h](/Users/tangjiacheng/Downloads/Monorepo/odin/client_session.h:28) and the current CLI accept path at [odin/cli_client.c](/Users/tangjiacheng/Downloads/Monorepo/odin/cli_client.c:324) stay in place.

```text
caller-owned local HTTPS_PROXY listener
        |
        | accepted nonblocking conn_fd
        v
odin_xqc_client_runtime_add_connection(rt, conn_fd)
        |
        v
odin_client_session_create_with_upstream_transport
        |
        | after local HTTP CONNECT parses OK
        v
xqc_stream_create_with_direction(conn, XQC_STREAM_BIDI, NULL)
        |
        v
odin_xqc_stream_transport
        |
        v
existing RFC-023 client pipeline
CONNECT_REQ -> CONNECT_RESP -> HTTP 200/400 -> relay

Existing TCP path remains separate:

local listener -> odin/cli_client.c -> odin_client_session_create
                              -> odin_dial_start -> fd transport
```

### 3.2 Detailed Design

#### 3.2.1 Client Session Upstream Transport Constructor

`odin_client_session_create` currently parses the local HTTP CONNECT request, calls `start_dial` at [odin/client_session.c](/Users/tangjiacheng/Downloads/Monorepo/odin/client_session.c:333), builds the upstream fd transport after dial success at [odin/client_session.c](/Users/tangjiacheng/Downloads/Monorepo/odin/client_session.c:395), and creates the CLIENT-mode connect session at [odin/client_session.c](/Users/tangjiacheng/Downloads/Monorepo/odin/client_session.c:420). This RFC adds a second constructor that replaces only the upstream creation step.

```c
/* odin/client_session.h additions */
#include "odin/transport.h"

typedef int (*odin_client_session_upstream_transport_factory_cb)(
    odin_transport_ready_cb on_ready, void *ready_user_data,
    void *factory_user_data, odin_transport_t **out);

typedef void (*odin_client_session_upstream_transport_destroying_cb)(
    odin_transport_t *transport, void *factory_user_data);

int odin_client_session_create_with_upstream_transport(
    odin_event_loop_t *loop, int conn_fd,
    odin_client_session_upstream_transport_factory_cb create_upstream,
    void *factory_user_data,
    odin_client_session_upstream_transport_destroying_cb upstream_destroying,
    odin_client_session_close_cb on_close, void *user_data,
    odin_client_session_t **out);
```

**Unstated contract.** `loop`, `create_upstream`, `on_close`, and `out` are non-null preconditions; `upstream_destroying` may be `NULL`. `conn_fd` is the same caller-owned nonblocking local HTTPS_PROXY stream fd accepted by the TCP constructor. On success the session owns `conn_fd`; on `-1` return the caller still owns `conn_fd`, `*out` is untouched, and `on_close` never fires. That failure-ownership rule covers null preconditions, downstream fd transport creation failure, and initial downstream `ODIN_TRANSPORT_READ` interest setup failure before `*out` publication. The factory is called at most once, only after `odin_http_parse_connect` has returned `ODIN_HTTP_OK`; local HTTP parse errors, local EOF before parse, and constructor rollback never open a QUIC stream. Immediately after `ODIN_HTTP_OK`, and before any factory callback or `odin_connect_session_create_client` call, the factory branch clears downstream interest with `odin_transport_set_interest(cs->downstream_t, 0)`, matching the current `start_dial` path. Downstream READ interest remains disabled throughout factory creation, CONNECT_REQ write, and CONNECT_RESP read; bytes that arrive on the local socket after the parsed CONNECT request are not read, buffered into `odin_connect_session`, or forwarded upstream until CONNECT_RESP succeeds, the HTTP 200 write completes, and `odin_relay_start` takes over downstream READ. The only local bytes forwarded before relay are the parser tail already present in `cs->http_buf` at the `ODIN_HTTP_OK` parse boundary. The factory receives the permanent client-session readiness callback and the newly allocated `odin_client_session_t *` as `ready_user_data`; it must return one upstream `odin_transport_t *` whose callback is not invoked before the factory returns. On factory failure it returns `-1` with `errno` set and leaves `*out` untouched; the client session maps the failure through the existing RFC-023 pre-relay failure path, so the local downstream peer receives `HTTP/1.1 400 Bad Request\r\n\r\n` and `on_close` carries the factory errno. On factory success, the client session owns the returned transport wrapper and destroys it on terminal or abort. If `upstream_destroying` is non-null, the client session calls it exactly once immediately before destroying a factory-created upstream transport, including the abort path where `odin_connect_session_create_client` fails after the factory succeeded. If `upstream_destroying` is `NULL`, those same terminal and post-factory-abort paths skip the callback and still destroy the factory-created upstream exactly once. The callback runs before `odin_transport_destroy`, receives the same `factory_user_data`, must not destroy the transport itself, and is the factory owner's only chance to remove transport-keyed indexes before `odin_xqc_stream_transport` clears stream user data and frees the wrapper.

The new constructor shares the existing downstream fd transport, HTTP parser, `odin_connect_session_create_client`, CONNECT_RESP handling in `session_on_done` at [odin/client_session.c](/Users/tangjiacheng/Downloads/Monorepo/odin/client_session.c:439), relay completion in `relay_on_done` at [odin/client_session.c](/Users/tangjiacheng/Downloads/Monorepo/odin/client_session.c:621), and terminal teardown in `fire_terminal` at [odin/client_session.c](/Users/tangjiacheng/Downloads/Monorepo/odin/client_session.c:629). `odin_client_session_set_dial_filter` remains meaningful only for the original fd-dial constructor, because the factory constructor and its `FACTORY` branch in the shared parse-to-upstream transition never call `start_dial`, `odin_dial_start`, raw `connect`, `open`, `exec*`, or `dlopen`; the only upstream resource selected by the factory branch is the caller-supplied `create_upstream` callback. This constructor itself contains no platform-specific branch; macOS host tests execute the abstract transport-factory contract, while Linux, alternate macOS arch, and iOS artifacts compile the same code but are not runtime-verified in this environment.

**Mechanism.**

```text
create_with_upstream_transport(loop, conn_fd, factory, factory_ud,
                               upstream_destroying, on_close, user_data, out):
  validate loop/factory/on_close/out
  allocate cs with conn_fd, dial_fd = -1, state = S_PARSING
  record upstream_mode = FACTORY, create_upstream = factory,
         create_upstream_ud = factory_ud,
         upstream_destroying = upstream_destroying
  if downstream transport create test hook is armed:
    consume hook; free cs; errno = armed errno; return -1
  if downstream fd transport create with client_session_ready fails:
    free cs; preserve errno; return -1
  if arming downstream READ interest fails:
    destroy downstream transport; free cs; preserve errno; return -1
  write *out and return 0

start_upstream_after_http_ok(cs):
  if cs.upstream_mode == TCP_DIAL:
    run the existing start_dial path unchanged
    return
  odin_transport_set_interest(cs.downstream_t, 0)
  upstream = NULL
  if cs.create_upstream(client_session_ready, cs,
                        cs.create_upstream_ud, &upstream) != 0:
    handle_failure(cs, ODIN_HTTP_ERR_BAD_REQUEST_TARGET, errno)
    return
  if odin_connect_session_create_client(parsed_host, parsed_host_len,
                                        parsed_port, session_on_done,
                                        cs, &cs.s) != 0:
    saved = errno
    destroy_factory_upstream(cs, upstream)
    upstream = NULL
    cs.upstream_t = NULL
    handle_failure(cs, ODIN_HTTP_ERR_BAD_REQUEST_TARGET, saved)
    return
  cs.upstream_t = upstream
  cs.state = S_HANDSHAKE
  odin_transport_set_interest(cs.upstream_t, odin_connect_session_wants(cs.s))

destroy_factory_upstream(cs, upstream):
  if cs.upstream_destroying != NULL:
    cs.upstream_destroying(upstream, cs.create_upstream_ud)
  destroy upstream
```

The downstream pause is performed before factory invocation, so a wrapper that synchronously delivers upstream WRITE readiness cannot race a still-armed downstream READ into `client_session_ready` while `cs.s` is live. After CONNECT-session creation succeeds, `cs.s` is stored, `cs.upstream_t` is published, and `cs.state` is set to `S_HANDSHAKE` before the upstream interest setter runs, because `odin_xqc_stream_transport` may synchronously deliver the deterministic first WRITE readiness when WRITE interest is enabled. `session_on_done` re-arms the downstream transport only for `ODIN_TRANSPORT_WRITE` while the HTTP response is emitted; downstream READ returns only when `odin_relay_start` owns both transports after HTTP 200 and any parser tail already captured at parse time has been handled. Every factory-upstream destroy path, including `fire_terminal` and constructor abort after `odin_connect_session_create_client` failure, uses the same pre-destroy callback helper, where a `NULL` callback means "destroy without owner notification," and clears `cs.upstream_t` before any later `handle_failure` branch can read it. The existing TCP path keeps its current immediate destroy-and-`cs.upstream_t = NULL` discipline because no external runtime indexes fd transports by wrapper pointer.

Satisfies: G3 via the upstream factory hook that lets the existing client CONNECT pipeline own an xquic stream transport; G4 via preserving `odin_client_session_create` as the fd-dial constructor used by the current CLI path.

#### 3.2.2 Client QUIC Runtime API and Startup

```c
/* odin/client_xqc_runtime.h */
#include <sys/socket.h>

#include "odin/event_loop.h"
#include "odin/xqc_udp.h"
#include <xquic/xquic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ODIN_XQC_CLIENT_ALPN "odin/1"

typedef struct odin_xqc_client_runtime_t odin_xqc_client_runtime_t;

typedef struct odin_xqc_client_runtime_config_t {
  odin_event_loop_t *loop;
  const struct sockaddr *local_addr;
  socklen_t local_addrlen;
  const struct sockaddr *peer_addr;
  socklen_t peer_addrlen;
  const char *server_host;
  const xqc_config_t *engine_config;
  const xqc_engine_ssl_config_t *engine_ssl_config;
  const xqc_engine_callback_t *engine_callbacks;
  const xqc_transport_callbacks_t *transport_callbacks;
  const xqc_conn_settings_t *conn_settings;
  const xqc_conn_ssl_config_t *conn_ssl_config;
  const unsigned char *token;
  unsigned int token_len;
  int no_crypto_flag;
} odin_xqc_client_runtime_config_t;

int odin_xqc_client_runtime_create(
    const odin_xqc_client_runtime_config_t *config,
    odin_xqc_client_runtime_t **out);
int odin_xqc_client_runtime_start(odin_xqc_client_runtime_t *rt);
int odin_xqc_client_runtime_stop(odin_xqc_client_runtime_t *rt);
int odin_xqc_client_runtime_add_connection(odin_xqc_client_runtime_t *rt,
                                           int conn_fd);
void odin_xqc_client_runtime_destroy(odin_xqc_client_runtime_t *rt);

#ifdef __cplusplus
}
#endif
```

**Unstated contract.** `config`, `config->loop`, `config->local_addr`, `config->peer_addr`, `config->server_host`, `config->engine_ssl_config`, `config->engine_callbacks`, `config->conn_settings`, `config->conn_ssl_config`, and `out` are non-null preconditions; invalid input returns `-1` with `errno = EINVAL` and leaves `*out` untouched. The `engine_ssl_config` precondition is synchronous and is checked before UDP-driver creation, because `odin_xqc_udp_create` passes it to `xqc_engine_create` and current xquic fails engine creation when `ssl_config == NULL` at [xquic/src/transport/xqc_engine.c](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/transport/xqc_engine.c:522). `config->transport_callbacks` is optional, but the runtime-owned table passed to xquic always has non-null `save_token`, `save_session_cb`, and `save_tp_cb`: caller non-null slots are preserved, and missing slots are filled with no-op callbacks that discard the supplied bytes and do not mutate `errno` or runtime state. xquic declares those slots as client callbacks at [xquic/include/xquic/xquic.h](/Users/tangjiacheng/Downloads/Monorepo/xquic/include/xquic/xquic.h:667), [xquic/include/xquic/xquic.h](/Users/tangjiacheng/Downloads/Monorepo/xquic/include/xquic/xquic.h:672), and [xquic/include/xquic/xquic.h](/Users/tangjiacheng/Downloads/Monorepo/xquic/include/xquic/xquic.h:677); current client code calls `save_token` without a null check at [xquic/src/transport/xqc_frame.c](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/transport/xqc_frame.c:1496) and `save_session_cb` without a null check at [xquic/src/transport/xqc_conn.c](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/transport/xqc_conn.c:6288), while `save_tp_cb` is currently guarded at [xquic/src/transport/xqc_conn.c](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/transport/xqc_conn.c:6136). If `config->conn_ssl_config->cert_verify_flag` includes `XQC_TLS_CERT_FLAG_NEED_VERIFY`, then `config->transport_callbacks` and `config->transport_callbacks->cert_verify_cb` are non-null preconditions because xquic calls `conn->transport_cbs.cert_verify_cb(...)` from `xqc_conn_tls_cert_verify_cb` at [xquic/src/transport/xqc_conn.c](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/transport/xqc_conn.c:6280). `create` rejects `config->peer_addrlen < sizeof(struct sockaddr)` or `config->peer_addrlen > sizeof(struct sockaddr_in6)` before copying address bytes, then branches on `config->peer_addr->sa_family`. It accepts only `AF_INET` with `peer_addrlen == sizeof(struct sockaddr_in)` or `AF_INET6` with `peer_addrlen == sizeof(struct sockaddr_in6)`; incomplete, oversized, family-mismatched, or unknown-family addresses return `-1` with `errno = EINVAL`. This cap is required because xquic stores `peer_addr` in a `sizeof(struct sockaddr_in6)` buffer at [xquic/src/transport/xqc_conn.h](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/transport/xqc_conn.h:298), and `xqc_client_connect` copies `peer_addrlen` bytes into that buffer at [xquic/src/transport/xqc_client.c](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/transport/xqc_client.c:88). `config->token` may be `NULL` only when `token_len == 0`; a nonzero `token_len` with `token == NULL` is invalid. `token_len` must be `<= 256`, matching xquic's `XQC_MAX_TOKEN_LEN` definition at [xquic/src/transport/xqc_defs.h](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/transport/xqc_defs.h:51) and the `xqc_client_connect` rejection branch at [xquic/src/transport/xqc_client.c](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/transport/xqc_client.c:46); over-cap tokens are rejected synchronously with `errno = EINVAL` before copying or UDP creation.

`config->conn_ssl_config->session_ticket_len > 0` requires non-null `session_ticket_data`; invalid nonzero session-ticket pointer/length pairs return `-1/EINVAL` before copying or UDP creation. When `session_ticket_len == 0`, caller `session_ticket_data == NULL` is accepted, but the runtime normalizes the copied `rt.conn_ssl_config.session_ticket_data` to a non-null runtime-owned empty byte before any `xqc_connect`. This avoids passing a null source pointer to xquic's client TLS setup: current xquic allocates `cfg.session_ticket`, then unconditionally copies from `conn_ssl_config->session_ticket_data` with `xqc_memcpy` at [xquic/src/transport/xqc_client.c](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/transport/xqc_client.c:185), and `xqc_memcpy` maps directly to `memcpy` at [xquic/src/common/xqc_str.h](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/common/xqc_str.h:41). `config->conn_ssl_config->transport_parameter_data_len > 0` requires non-null `transport_parameter_data`; invalid nonzero transport-parameter pointer/length pairs return `-1/EINVAL` before copying or UDP creation. A zero-length transport-parameter buffer may leave the copied `rt.conn_ssl_config.transport_parameter_data` as `NULL` because xquic guards that path with both pointer and length checks at [xquic/src/transport/xqc_client.c](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/transport/xqc_client.c:293). These two nested buffers are the only pointer fields inside `xqc_conn_ssl_config_t` at [xquic/include/xquic/xquic.h](/Users/tangjiacheng/Downloads/Monorepo/xquic/include/xquic/xquic.h:1272) and [xquic/include/xquic/xquic.h](/Users/tangjiacheng/Downloads/Monorepo/xquic/include/xquic/xquic.h:1282). `server_host` is a NUL-terminated xquic TLS/SNI hostname string because `xqc_connect` accepts `const char *server_host` at [xquic/include/xquic/xquic.h](/Users/tangjiacheng/Downloads/Monorepo/xquic/include/xquic/xquic.h:1835). `local_addr` is the UDP bind address passed to `odin_xqc_udp_create`; `peer_addr` is the configured Odin QUIC server address passed to `xqc_connect`. Both are caller-supplied configuration, not peer-supplied bytes.

`create` allocates the runtime shell, stores the borrowed `config->loop` in `rt->loop`, copies the complete validated `peer_addr` into `rt->peer_addr_storage`, stores `config->peer_addrlen` in `rt->peer_addrlen`, duplicates `server_host`, copies `token` bytes when `token_len > 0`, copies `*conn_settings`, copies scalar fields from `*conn_ssl_config`, deep-copies `session_ticket_data[0..session_ticket_len)` when `session_ticket_len > 0`, otherwise points `rt.conn_ssl_config.session_ticket_data` at a non-null runtime-owned empty byte with `session_ticket_len == 0`, deep-copies `transport_parameter_data[0..transport_parameter_data_len)` when `transport_parameter_data_len > 0`, copies `no_crypto_flag`, and builds a runtime-owned `xqc_transport_callbacks_t` value. The caller owns `config->loop` and must keep that event loop alive until `odin_xqc_client_runtime_destroy` has completed, because `add_connection`, queued-session conversion, and stream-session teardown create and drive `odin_client_session` objects on `rt->loop`. The copied `rt.conn_ssl_config.session_ticket_data` field is always non-null and points either at heap-copied ticket bytes or at the runtime-owned empty byte for zero-length tickets; `rt.conn_ssl_config.transport_parameter_data` points at heap-copied transport-parameter bytes or is `NULL` when its length is zero. The copied-config allocation order is fixed: duplicate `server_host`, copy `token` when `token_len > 0`, copy requested `session_ticket_data`, then copy requested `transport_parameter_data`. If any allocation for `server_host`, `token`, or either nested `conn_ssl_config` buffer fails, `create` preserves `ENOMEM`, frees every earlier runtime-owned heap copy, frees the runtime shell, records no UDP create, and leaves `*out` untouched; specifically, token-copy failure frees the already-duplicated `server_host`, session-ticket copy failure frees `server_host` and any copied token, and transport-parameter copy failure frees `server_host`, any copied token, and the already-copied non-empty session-ticket buffer before returning. Later UDP-create, ALPN-register, and destroy cleanup paths free heap-copied nested buffers exactly once with the rest of the runtime-owned copied configuration; the zero-length session-ticket empty byte is part of the runtime object and is not freed separately. When `config->transport_callbacks != NULL`, the caller table is copied by value; when it is `NULL`, the runtime starts from a zeroed local value that is not passed onward until defaulting is complete. In both cases, the runtime then fills any missing `save_token`, `save_session_cb`, and `save_tp_cb` slots with no-op callbacks before `odin_xqc_udp_create` can copy the table. The no-op callbacks preserve `errno`, do not read or write `rt`, and ignore all callback arguments. The caller may mutate or release the copied inputs, including the token, `server_host`, `peer_addr`, `conn_settings`, and both nested `conn_ssl_config` buffers, after `create` returns; `start` uses only the runtime-owned copies. `local_addr`, `engine_config`, `engine_ssl_config`, and `engine_callbacks` are used only during the synchronous `odin_xqc_udp_create` call, so the caller must keep them valid only until `odin_xqc_client_runtime_create` returns. The runtime creates `odin_xqc_udp` with `XQC_ENGINE_CLIENT`, overrides `conn_update_cid_notify` with the runtime CID lifecycle callback, and registers `ODIN_XQC_CLIENT_ALPN` with app callbacks for client connection lifecycle and stream readiness. Caller transport callbacks such as `cert_verify_cb`, `save_token`, `save_session_cb`, and `save_tp_cb` remain available to xquic through that runtime-owned table; missing client cache callbacks are safe no-ops, not null function pointers. UDP send callbacks remain owned by `odin_xqc_udp_create`, which overwrites `stateless_reset`, `write_socket`, `write_socket_ex`, and `conn_send_packet_before_accept` at [odin/xqc_udp.c](/Users/tangjiacheng/Downloads/Monorepo/odin/xqc_udp.c:405). It passes `app_user_data = rt` to `odin_xqc_udp_create`, and later passes `odin_xqc_udp_xqc_user_data(rt->xu)` to `xqc_connect`, matching the `odin/xqc_udp.h` contract at [odin/xqc_udp.h](/Users/tangjiacheng/Downloads/Monorepo/odin/xqc_udp.h:19). `odin_xqc_udp_xqc_user_data(xu)` currently returns `xu` at [odin/xqc_udp.c](/Users/tangjiacheng/Downloads/Monorepo/odin/xqc_udp.c:515), so callbacks recover the runtime through `odin_xqc_udp_app_user_data(xu)`.

`config->engine_config` is optional. `engine_config == NULL` is valid and means the runtime passes `NULL` through `odin_xqc_udp_create` to xquic; xquic allocates its default engine config at [xquic/src/transport/xqc_engine.c](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/transport/xqc_engine.c:466) and applies caller engine config only when `engine_config != NULL` at [xquic/src/transport/xqc_engine.c](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/transport/xqc_engine.c:471). If `engine_config != NULL && engine_config->sendmmsg_on != 0`, create relies on the inherited `odin_xqc_udp_create` rejection at [odin/xqc_udp.c](/Users/tangjiacheng/Downloads/Monorepo/odin/xqc_udp.c:387): the runtime wrapper records the UDP-create attempt, `odin_xqc_udp_create` returns `-1` with `errno = ENOTSUP` before allocating an `odin_xqc_udp_t`, and the client runtime frees its copied config, leaves `*out` untouched, records no ALPN registration, and has no UDP driver to destroy.

`start(NULL)` and `stop(NULL)` return `-1` with `errno = EINVAL`; `destroy(NULL)` is a no-op. The runtime tracks `connect_started` separately from `udp_running`, and uses a private `startup_connecting` flag from the first `xqc_connect` call until that call either succeeds or all failed-start cleanup has completed. The first successful `start` starts the UDP driver, sets `udp_running = 1`, sets `startup_connecting = 1`, and then calls `xqc_connect(odin_xqc_udp_engine(rt->xu), &rt->conn_settings, rt->token, rt->token_len, rt->server_host, rt->no_crypto_flag, &rt->conn_ssl_config, (const struct sockaddr *)&rt->peer_addr_storage, rt->peer_addrlen, ODIN_XQC_CLIENT_ALPN, odin_xqc_udp_xqc_user_data(rt->xu))`. If the first `odin_xqc_udp_start` fails, `start` preserves that errno, leaves `udp_running = 0` and `connect_started = 0`, performs no `xqc_connect`, registers no CID, and a later `start` retries from the same never-connected state. `xqc_connect` invokes client `conn_create_notify` synchronously at [xquic/src/transport/xqc_client.c](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/transport/xqc_client.c:98) and returns a pointer to the user SCID at [xquic/src/transport/xqc_client.c](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/transport/xqc_client.c:140); the runtime copies the CID from the callback, not by retaining xquic-owned storage. If `xqc_connect` returns `NULL` before `runtime_conn_create_notify` records a connection, `start` clears `startup_connecting`, stops the UDP driver, clears `udp_running`, leaves `connect_started = 0`, and returns `-1` with `EIO` when xquic reported failure without errno. If `xqc_connect` returns `NULL` after `runtime_conn_create_notify` succeeded, such as the `xqc_engine_add_active_queue` failure branch at [xquic/src/transport/xqc_client.c](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/transport/xqc_client.c:111), `start` treats the already-registered CID as a failed startup connection while `startup_connecting` remains set: if `runtime_conn_close_notify` has not already cleaned it up, it calls `runtime_conn_close_call(odin_xqc_udp_engine(rt->xu), &rt->current_cid)` while the CID is still registered, unregisters `rt->current_cid` exactly once if a synchronous close notification did not already unregister it, clears `rt->conn`, `rt->cid_registered`, and `handshake_done`, then clears `startup_connecting`, stops UDP, and leaves `connect_started = 0` so a later `start` creates a new xquic connection. If xquic already delivered `runtime_conn_close_notify` before `xqc_connect` returned, including destruction during `xqc_engine_conn_logic`, that callback runs the startup rollback close branch: it unregisters the current CID if registered, clears `rt->conn`, `rt->cid_registered`, and `handshake_done`, leaves `rt->closing = 0`, and leaves `connect_started = 0`. The failed-start helper then observes no registered CID, clears `startup_connecting`, stops UDP, and the next `start` may create a new connection. A second `start` while `udp_running == 1` returns `0` without calling `odin_xqc_udp_start` or `xqc_connect` again. A `start` after a successful `stop` calls `odin_xqc_udp_start`, sets `udp_running = 1`, and returns `0` without a second `xqc_connect`; the existing xquic connection, active CID, handshake state, queued fds, and live sessions are left intact. If that restart `odin_xqc_udp_start` fails, `start` preserves errno, leaves `udp_running = 0`, keeps `connect_started = 1` and the already-registered active CID intact, performs no second `xqc_connect`, and a later successful `start` restarts UDP without reconnecting.

`stop` on a valid runtime returns `0` whether the runtime is never-started, running, or already stopped. If `udp_running == 1`, it calls `odin_xqc_udp_stop(rt->xu)` once for that running epoch and clears `udp_running`; if already stopped, it records no second UDP stop. `stop` does not close the xquic connection, unregister the active CID, destroy queued fds, destroy live sessions, clear `connect_started`, or clear `handshake_done`. A stopped runtime cannot make progress, so `add_connection` rejects while stopped until a later `start` restarts the UDP driver.

`add_connection` is the public handoff point future CLI transport selection will call from its local accept callback. On success the runtime owns `conn_fd`; on `-1` return the caller still owns it. Before the first successful `start`, while stopped, or after the runtime is closing, it returns `-1` with `errno = ENOTCONN`. After `start` succeeds but before `conn_handshake_finished`, it appends the fd to the pending queue and creates no client session yet; if the append allocation or one-shot test hook fails, `add_connection` returns `-1` with the preserved errno, leaves the queue unchanged, and the caller still owns `conn_fd`. Queued fds are converted to client sessions in FIFO order only when `conn_handshake_finished` is for the current live connection and the runtime is not closing. After handshake completion, it immediately creates one `odin_client_session_create_with_upstream_transport` handle per fd. All APIs are owner-thread APIs under the existing event-loop contract. This module contains no platform-specific branch; the host macOS test binary executes the runtime contract through fake xquic callbacks and local sockets, while alternate-platform binaries compile the same paths without runtime execution in this environment.

**Mechanism.**

```text
create(config, out):
  validate config and out
  validate config.engine_ssl_config != NULL
  if config.peer_addrlen < sizeof(struct sockaddr):
    errno = EINVAL; return -1
  if config.peer_addrlen > sizeof(struct sockaddr_in6):
    errno = EINVAL; return -1
  if config.peer_addr->sa_family == AF_INET:
    if config.peer_addrlen != sizeof(struct sockaddr_in):
      errno = EINVAL; return -1
  else if config.peer_addr->sa_family == AF_INET6:
    if config.peer_addrlen != sizeof(struct sockaddr_in6):
      errno = EINVAL; return -1
  else:
    errno = EINVAL; return -1
  validate token != NULL when token_len > 0
  if config.token_len > 256:
    errno = EINVAL; return -1
  if config.conn_ssl_config.session_ticket_len > 0 and
     config.conn_ssl_config.session_ticket_data == NULL:
    errno = EINVAL; return -1
  if config.conn_ssl_config.transport_parameter_data_len > 0 and
     config.conn_ssl_config.transport_parameter_data == NULL:
    errno = EINVAL; return -1
  if config.transport_callbacks != NULL:
    copy *config.transport_callbacks into transport_callbacks
  else:
    zero-initialize transport_callbacks
  if transport_callbacks.save_token == NULL:
    set transport_callbacks.save_token to no-op callback
  if transport_callbacks.save_session_cb == NULL:
    set transport_callbacks.save_session_cb to no-op callback
  if transport_callbacks.save_tp_cb == NULL:
    set transport_callbacks.save_tp_cb to no-op callback
  if config.conn_ssl_config.cert_verify_flag has XQC_TLS_CERT_FLAG_NEED_VERIFY:
    if config.transport_callbacks == NULL:
      errno = EINVAL; return -1
    if transport_callbacks.cert_verify_cb == NULL:
      errno = EINVAL; return -1
  set transport_callbacks.conn_update_cid_notify = runtime_conn_update_cid
  rt = calloc
  rt.loop = config.loop
  copy config.peer_addr[0..config.peer_addrlen) into rt.peer_addr_storage
  rt.peer_addrlen = config.peer_addrlen
  duplicate config.server_host into rt.server_host
  copy config.token[0..token_len) into rt.token when token_len > 0
  copy *config.conn_settings into rt.conn_settings
  copy scalar fields from *config.conn_ssl_config into rt.conn_ssl_config
  if config.conn_ssl_config.session_ticket_len > 0:
    allocate and copy session_ticket_data into rt.conn_ssl_config.session_ticket_data
  else:
    set rt.conn_ssl_config.session_ticket_data to a non-null runtime-owned empty byte
  if config.conn_ssl_config.transport_parameter_data_len > 0:
    allocate and copy transport_parameter_data into rt.conn_ssl_config.transport_parameter_data
  if any copied-config allocation failed:
    free copied config buffers; free rt; preserve errno; return -1
  rt.no_crypto_flag = config.no_crypto_flag
  copy transport_callbacks into rt.transport_callbacks
  initialize rt.app_callbacks.conn_cbs.conn_create_notify
  initialize rt.app_callbacks.conn_cbs.conn_close_notify
  initialize rt.app_callbacks.conn_cbs.conn_handshake_finished
  initialize rt.app_callbacks.stream_cbs read/write/close/closing notify
  udp_config = {
    loop = config.loop,
    local_addr = config.local_addr,
    local_addrlen = config.local_addrlen,
    engine_type = XQC_ENGINE_CLIENT,
    engine_config = config.engine_config,
    ssl_config = config.engine_ssl_config,
    engine_callbacks = config.engine_callbacks,
    transport_callbacks = &rt.transport_callbacks,
    app_user_data = rt,
  }
  # config.engine_config may be NULL. If non-null sendmmsg_on is enabled,
  # odin_xqc_udp_create rejects here with ENOTSUP before ALPN registration.
  if odin_xqc_udp_create(&udp_config, &rt.xu) != 0:
    free copied config buffers; free rt; preserve errno; return -1
  if xqc_engine_register_alpn(odin_xqc_udp_engine(rt.xu), "odin/1", 6,
                              &rt.app_callbacks, rt) != XQC_OK:
    odin_xqc_udp_destroy(rt.xu); free copied config buffers; free rt; errno = EIO; return -1
  *out = rt; return 0

start(rt):
  validate rt
  if rt.connect_started and rt.udp_running: return 0
  if rt.connect_started and !rt.udp_running:
    if odin_xqc_udp_start(rt.xu) != 0:
      preserve errno
      rt.udp_running = 0
      return -1
    rt.udp_running = 1
    return 0
  if odin_xqc_udp_start(rt.xu) != 0:
    preserve errno
    rt.udp_running = 0
    rt.connect_started = 0
    return -1
  rt.udp_running = 1
  rt.connect_errno = 0
  rt.startup_connecting = 1
  cid = xqc_connect(odin_xqc_udp_engine(rt.xu), &rt.conn_settings,
                    rt.token, rt.token_len, rt.server_host,
                    rt.no_crypto_flag, &rt.conn_ssl_config,
                    (const struct sockaddr *)&rt.peer_addr_storage,
                    rt.peer_addrlen,
                    "odin/1", odin_xqc_udp_xqc_user_data(rt.xu))
  if cid == NULL:
    saved = rt.connect_errno != 0 ? rt.connect_errno : EIO
    if rt.conn != NULL or rt.cid_registered:
      cleanup_failed_start_connection(rt)
    rt.startup_connecting = 0
    odin_xqc_udp_stop(rt.xu)
    rt.udp_running = 0
    rt.connect_started = 0
    errno = saved
    return -1
  rt.startup_connecting = 0
  rt.connect_started = 1
  return 0

cleanup_failed_start_connection(rt):
  if rt.conn != NULL and rt.cid_registered:
    runtime_conn_close_call(odin_xqc_udp_engine(rt.xu), &rt.current_cid)
  if rt.cid_registered:
    odin_xqc_udp_unregister_conn(rt.xu, &rt.current_cid)
    rt.cid_registered = 0
  rt.conn = NULL
  rt.handshake_done = 0

stop(rt):
  validate rt
  if !rt.udp_running: return 0
  if odin_xqc_udp_stop(rt.xu) != 0: return -1
  rt.udp_running = 0
  return 0

conn_handshake_finished(conn, conn_user_data, conn_proto_data):
  rt = odin_xqc_udp_app_user_data(conn_user_data)
  if rt == NULL or conn_proto_data != rt or rt.conn != conn or rt.closing:
    return
  rt.handshake_done = 1
  while rt.pending_local_fds not empty:
    fd = pop pending fd
    if create_one_client_session(rt, fd) != 0:
      close fd because runtime already accepted ownership
```

Satisfies: G1 via the public runtime API, `XQC_ENGINE_CLIENT` UDP creation, exact `odin/1` ALPN registration, and one `xqc_connect` call to the configured peer; G3 via `add_connection` as the local HTTPS_PROXY fd handoff point.

#### 3.2.3 Connection and CID Lifecycle

The runtime owns the xquic connection callbacks that connect `xqc_connect` to `odin_xqc_udp` CID recovery and to stream creation.

```c
/* xquic callback slots installed by odin/client_xqc_runtime.c */
int runtime_conn_create_notify(xqc_connection_t *conn, const xqc_cid_t *cid,
                               void *conn_user_data,
                               void *conn_proto_data);
int runtime_conn_close_notify(xqc_connection_t *conn, const xqc_cid_t *cid,
                              void *conn_user_data,
                              void *conn_proto_data);
void runtime_conn_handshake_finished(xqc_connection_t *conn,
                                     void *conn_user_data,
                                     void *conn_proto_data);
void runtime_conn_update_cid(xqc_connection_t *conn,
                             const xqc_cid_t *retire_cid,
                             const xqc_cid_t *new_cid,
                             void *conn_user_data);
```

**Unstated contract.** `runtime_conn_create_notify` receives `conn_user_data == odin_xqc_udp_xqc_user_data(rt->xu)`. It recovers `rt`, registers `cid` with `odin_xqc_udp_register_conn`, records `rt->conn = conn`, copies `cid` into `rt->current_cid`, sets `rt->cid_registered = 1`, sets xquic ALPN user data to `rt` with `xqc_conn_set_alp_user_data`, and returns `0`. If `rt->conn` is already non-null or `rt->closing` is already set, the callback returns `-1`, records `rt->connect_errno = EALREADY`, and performs no observable mutation: it does not call `odin_xqc_udp_register_conn`, does not call `runtime_conn_set_alp_user_data_call`, does not replace `rt->conn` or `rt->current_cid`, does not change `rt->cid_registered` or `handshake_done`, and does not trigger close/unregister cleanup for the already-active connection. If CID registration fails, it records the errno for `start`, returns `-1`, and xquic destroys the connection before `xqc_connect` returns; no unregister is attempted because no CID was registered. The runtime does not call `xqc_conn_set_transport_user_data` in this callback, because `xqc_connect` already received `odin_xqc_udp_xqc_user_data(rt->xu)` as its connection user data. Current xquic invokes client `conn_create_notify` during `xqc_client_connect` before `xqc_engine_conn_logic` at [xquic/src/transport/xqc_client.c](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/transport/xqc_client.c:98), so registering here covers UDP WRITE recovery before connection logic can send and encounter UDP backpressure.

`runtime_conn_update_cid` follows the same register-before-unregister rule as the server runtime: register `new_cid` first, then unregister `retire_cid` only after registration succeeds. On registration failure, it marks the runtime closing, destroys every live client session through the §3.2.4 runtime-driven stream cleanup ordering, calls `runtime_conn_close_call(odin_xqc_udp_engine(rt->xu), &rt->current_cid)` while the old CID is still usable, unregisters the old CID once, records that no CID remains registered, and leaves the runtime linked until `conn_close_notify` arrives. Later CID updates on a closing runtime are no-ops. `runtime_conn_handshake_finished` follows the same current-connection discipline before it drains pending fds: xquic passes both `conn` and `conn->proto_data` to this callback at [xquic/src/transport/xqc_conn.c](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/transport/xqc_conn.c:5100), and `xqc_conn_set_alp_user_data` stores the runtime in `conn->proto_data` at [xquic/src/transport/xqc_conn.c](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/transport/xqc_conn.c:1665). The runtime recovers `rt` from `conn_user_data`, then no-ops unless `conn_proto_data == rt`, `rt->conn == conn`, and `rt->closing == 0`; stale or closing handshakes cannot set `handshake_done` or convert queued fds. `runtime_conn_close_notify` first verifies that the closing xquic pointer is still `rt->conn`; a close notification for a connection already retired by failed-start cleanup is stale and returns without destroying sessions, unregistering the current CID, or clearing a retried connection. For the current connection, a close while `startup_connecting == 1`, `connect_started == 0`, and `destroy_pending == 0` is startup rollback, not terminal runtime close: it unregisters the current CID if still registered, clears `rt->conn`, `rt->cid_registered`, and `handshake_done`, preserves `rt->closing = 0`, and returns so a later `start` may retry. For all other current-connection closes, `runtime_conn_close_notify` destroys live sessions and queued fds, unregisters the current CID if still registered, clears `rt->conn`, marks the runtime closing/not connected, and completes deferred destroy when this was the final live connection. xquic destroys streams before `conn_close_notify` at [xquic/src/transport/xqc_conn.c](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/transport/xqc_conn.c:1538), but the runtime still handles direct/test close with still-linked sessions as a defensive cleanup branch.

`stop` is a UDP-driver pause only: it clears `udp_running` through `odin_xqc_udp_stop`, but it does not unregister a live CID, close the xquic connection, clear `handshake_done`, or destroy queued/live client sessions. `destroy` is terminal and always destroys queued local fds and live stream sessions it owns. Running destroy and stopped destroy intentionally differ because a stopped runtime has no UDP interest or timer to drive xquic engine progress. If no xquic connection is live, `destroy` unregisters ALPN, destroys the UDP driver, and frees the runtime synchronously. If a live connection exists and `udp_running == 1`, `destroy` marks the runtime closing, calls `runtime_conn_close_call(odin_xqc_udp_engine(rt->xu), &rt->current_cid)` at most once while the current CID is still registered, and defers CID unregister, ALPN unregister, UDP-driver destroy, and runtime free until `runtime_conn_close_notify` proves the xquic connection reference is inert. If a live connection exists and `udp_running == 0`, `destroy` does not restart UDP and does not wait for `runtime_conn_close_notify`: it marks force destroy active, destroys all owned queued/live sessions, clears the connection ALPN user-data slot with `runtime_conn_set_alp_user_data_call(rt->conn, NULL)`, unregisters `rt->current_cid` exactly once if still registered, clears `rt->conn`, clears `handshake_done`, unregisters ALPN, destroys the UDP driver, and frees the runtime before returning. A close, stream, or CID callback that xquic invokes synchronously from `odin_xqc_udp_destroy` sees either `force_destroy_active` or missing ALPN/live-map state and is cleanup-only: it must not recreate sessions, call transport helpers with stale user data, issue a connection close, unregister the CID a second time, recursively unregister ALPN, recursively destroy UDP, or free the runtime before the outer finalizer returns. This satisfies the `odin_xqc_udp_destroy` rule that callers must make connections unavailable and unregister CIDs before destroying the UDP driver at [odin/xqc_udp.h](/Users/tangjiacheng/Downloads/Monorepo/odin/xqc_udp.h:24) without depending on xquic progress while stopped.

**Mechanism.**

```text
runtime_conn_create_notify(conn, cid, conn_user_data, conn_proto_data):
  xu = conn_user_data
  rt = odin_xqc_udp_app_user_data(xu)
  if rt.conn != NULL or rt.closing:
    rt.connect_errno = EALREADY; return -1
  if odin_xqc_udp_register_conn(rt.xu, cid) != 0:
    rt.connect_errno = errno; return -1
  rt.conn = conn
  rt.current_cid = *cid
  rt.cid_registered = 1
  runtime_conn_set_alp_user_data_call(conn, rt)
  return 0

runtime_conn_update_cid(conn, retired, new_cid, conn_user_data):
  rt = odin_xqc_udp_app_user_data(conn_user_data)
  if rt == NULL or rt.conn != conn or rt.closing: return
  if odin_xqc_udp_register_conn(rt.xu, new_cid) != 0:
    rt.closing = 1
    destroy live sessions with runtime-driven stream cleanup ordering
    destroy queued local fds
    if rt.cid_registered:
      runtime_conn_close_call(odin_xqc_udp_engine(rt.xu), &rt.current_cid)
      odin_xqc_udp_unregister_conn(rt.xu, &rt.current_cid)
      rt.cid_registered = 0
    return
  odin_xqc_udp_unregister_conn(rt.xu, retired)
  rt.current_cid = *new_cid
  rt.cid_registered = 1

runtime_conn_handshake_finished(conn, conn_user_data, conn_proto_data):
  rt = odin_xqc_udp_app_user_data(conn_user_data)
  if rt == NULL or conn_proto_data != rt or rt.conn != conn or rt.closing:
    return
  rt.handshake_done = 1
  drain pending_local_fds in FIFO order into client sessions

runtime_conn_close_notify(conn, cid, conn_user_data, conn_proto_data):
  rt = conn_proto_data or odin_xqc_udp_app_user_data(conn_user_data)
  if rt == NULL or rt.conn != conn:
    return 0
  if rt.force_destroy_active:
    return 0
  if rt.startup_connecting and !rt.connect_started and !rt.destroy_pending:
    if rt.cid_registered:
      odin_xqc_udp_unregister_conn(rt.xu, &rt.current_cid)
      rt.cid_registered = 0
    rt.conn = NULL
    rt.handshake_done = 0
    return 0
  destroy all sessions and queued local fds
  if rt.cid_registered:
    odin_xqc_udp_unregister_conn(rt.xu, &rt.current_cid)
    rt.cid_registered = 0
  rt.conn = NULL
  rt.closing = 1
  maybe_finish_destroy(rt)
  return 0

destroy(rt):
  if rt == NULL: return
  rt.destroy_pending = 1
  rt.closing = 1
  if rt.conn != NULL and !rt.udp_running:
    rt.force_destroy_active = 1
    destroy all sessions and queued local fds
    force_destroy_stopped_connection(rt)
    finish_destroy(rt)
    return
  destroy all sessions and queued local fds
  if rt.conn != NULL:
    if !rt.destroy_close_requested and rt.cid_registered:
      rt.destroy_close_requested = 1
    runtime_conn_close_call(odin_xqc_udp_engine(rt.xu), &rt.current_cid)
    return
  maybe_finish_destroy(rt)

force_destroy_stopped_connection(rt):
  # destroy(rt) already destroyed all sessions and queued local fds
  if rt.conn != NULL:
    runtime_conn_set_alp_user_data_call(rt.conn, NULL)
  if rt.cid_registered:
    odin_xqc_udp_unregister_conn(rt.xu, &rt.current_cid)
    rt.cid_registered = 0
  rt.conn = NULL
  rt.handshake_done = 0
  rt.connect_started = 0
  # finish_destroy clears force_destroy_active only after UDP destroy returns

finish_destroy(rt):
  unregister ALPN if registered
  odin_xqc_udp_destroy(rt.xu)
  rt.force_destroy_active = 0
  free copied config buffers
  free rt
```

Satisfies: G2 via create-time registration, update-time register-before-unregister ordering, fatal close when the new active CID cannot be registered, and close-time unregister.

#### 3.2.4 Stream Handoff to the Client Pipeline

The runtime opens one bidirectional xquic stream for each client session's upstream factory call and forwards xquic readiness into the RFC-016 stream transport helpers.

```c
/* xquic stream callback slots registered for ODIN_XQC_CLIENT_ALPN */
xqc_int_t runtime_stream_read_notify(xqc_stream_t *stream,
                                     void *strm_user_data);
xqc_int_t runtime_stream_write_notify(xqc_stream_t *stream,
                                      void *strm_user_data);
xqc_int_t runtime_stream_close_notify(xqc_stream_t *stream,
                                      void *strm_user_data);
void runtime_stream_closing_notify(xqc_stream_t *stream, xqc_int_t err_code,
                                   void *strm_user_data);
```

**Unstated contract.** The runtime creates outbound streams through `runtime_stream_create_bidi_call(rt->conn)`, whose production fallback is `xqc_stream_create_with_direction(rt->conn, XQC_STREAM_BIDI, NULL)` and whose current implementation maps client bidirectional streams to `XQC_CLI_BID` at [xquic/src/transport/xqc_stream.c](/Users/tangjiacheng/Downloads/Monorepo/xquic/src/transport/xqc_stream.c:555). It passes `NULL` for initial stream user data because `odin_xqc_stream_transport_create` installs the transport pointer later. If stream creation fails, the upstream factory returns `-1` with `errno = EIO` unless a test hook supplied a more specific errno. After stream creation succeeds, the factory calls `odin_xqc_stream_transport_create(stream, on_ready, ready_user_data, &transport)`, which installs the xquic stream user-data slot at [odin/transport_xqc.h](/Users/tangjiacheng/Downloads/Monorepo/odin/transport_xqc.h:22). The client session owns the returned transport and destroys it on terminal; the runtime stores the stream context by transport pointer only while `stream_ctx.transport` is non-null. The factory inserts that map entry before returning success. The `runtime_client_session_upstream_destroying` callback removes the map entry and clears `stream_ctx.transport` immediately before the client session destroys the xqc transport wrapper, including the `odin_connect_session_create_client` failure path. `runtime_client_session_on_close` repeats the unlink as an idempotent cleanup for paths where no upstream transport was created or the pre-destroy callback already ran. Stream callbacks do not recover the runtime from the transport pointer. Every stream callback first calls the runtime wrapper around `xqc_get_conn_alp_user_data_by_stream(stream)` to recover `rt`, because `runtime_conn_create_notify` installed `rt` as the connection ALPN user data. Only after that lookup succeeds does the callback look up `strm_user_data` in `rt`'s live transport map.

`stream_read_notify` and `stream_write_notify` return the exact helper result from `odin_xqc_stream_transport_read_notify` and `odin_xqc_stream_transport_write_notify` only for a transport pointer that is still present in `rt`'s live map. `stream_close_notify` returns `XQC_OK` for live, stale, or `NULL` stream user data; `stream_closing_notify` forwards reset/error notification to `odin_xqc_stream_transport_closing_notify` only for a live transport pointer. A callback whose connection ALPN user data lookup returns `NULL`, whose stream user data is `NULL`, or whose non-null stream user data is absent from the live map is a no-op (`XQC_OK` for read/write/close, no callback for closing). For a live stream, a closing/reset notify becomes a client-session transport error, which eventually tears down the local downstream connection and removes the stream context. For factory-created upstreams, `fire_terminal` calls `runtime_client_session_upstream_destroying`, destroys `cs.upstream_t`, and clears xqc stream user data through `odin_xqc_stream_transport` before invoking `runtime_client_session_on_close`; `runtime_client_session_on_close` is never the first wrapper-destroy point. If a client session completes with `err != 0`, the runtime calls `runtime_stream_close_call(stream)` only after that wrapper-destroy/user-data-clear ordering has occurred; if it completes with `err == 0`, the runtime relies on the existing relay FIN path and does not close the stream explicitly. Runtime-driven connection close, CID-update registration-failure abort, and destroy cleanup use the same ordering for every live stream context: unlink the transport map before wrapper destroy, destroy the `odin_client_session` while the xqc stream is still valid so `odin_xqc_stream_transport` clears stream user data, and then call `runtime_stream_close_call` only for non-OK aborts. A later xquic callback with `NULL` stream user data, with no connection ALPN user data, or with a stale transport pointer no longer in the runtime map is benign.

When `add_connection` queues a local fd before handshake completion, no `odin_client_session_t` exists yet and no local HTTP bytes are parsed. The pending queue append is the ownership-transfer boundary: allocation failure or the one-shot pending-append test hook returns `-1`, preserves errno, leaves the queue unchanged, and leaves `conn_fd` open for the caller. Once append succeeds, the runtime owns that fd. After handshake completion, immediate `add_connection` transfers ownership only after `create_one_client_session` returns `0`; stream-context allocation failure or synchronous `odin_client_session_create_with_upstream_transport` failure returns `-1` and leaves `conn_fd` open for the caller. On handshake completion, the runtime creates queued sessions in FIFO order. If delayed session creation fails after the runtime already accepted ownership of the fd, including synchronous client-session constructor failure before the stream factory can run, the runtime closes that fd and moves to the next queued item; a failed pending item does not poison later local connections.

**Mechanism.**

```text
add_connection(rt, conn_fd):
  if rt == NULL or rt.closing or !rt.connect_started or !rt.udp_running:
    errno = ENOTCONN; return -1
  if !rt.handshake_done:
    if append_pending_local_fd(rt, conn_fd) != 0:
      preserve errno; return -1
    return 0
  return create_one_client_session(rt, conn_fd)

append_pending_local_fd(rt, conn_fd):
  if one-shot pending append failure hook is armed:
    errno = armed errno; disarm hook; return -1
  node = malloc pending fd node
  if node == NULL: errno = ENOMEM; return -1
  node.fd = conn_fd
  append node to pending_local_fds
  return 0

create_one_client_session(rt, conn_fd):
  if one-shot stream context allocation failure hook is armed:
    errno = armed errno; disarm hook; return -1
  stream_ctx = calloc
  if stream_ctx == NULL: errno = ENOMEM; return -1
  stream_ctx.rt = rt
  if odin_client_session_create_with_upstream_transport(
       rt.loop, conn_fd, xqc_client_upstream_factory, stream_ctx,
       runtime_client_session_upstream_destroying,
       runtime_client_session_on_close, stream_ctx, &stream_ctx.cs) != 0:
    free stream_ctx; preserve errno; return -1
  link stream_ctx into rt.sessions
  return 0

xqc_client_upstream_factory(on_ready, ready_ud, factory_ud, out):
  stream_ctx = factory_ud
  rt = stream_ctx.rt
  if rt.closing or !rt.handshake_done or rt.conn == NULL:
    errno = ENOTCONN; return -1
  stream = runtime_stream_create_bidi_call(rt.conn)
  if stream == NULL:
    errno = EIO; return -1
  if odin_xqc_stream_transport_create(stream, on_ready, ready_ud, out) != 0:
    runtime_stream_close_call(stream)
    return -1
  stream_ctx.stream = stream
  stream_ctx.transport = *out
  insert stream_ctx into rt.transport_map keyed by stream_ctx.transport
  return 0

runtime_client_session_upstream_destroying(transport, stream_ctx):
  rt = stream_ctx.rt
  if stream_ctx.transport == transport:
    remove stream_ctx from rt.transport_map
    stream_ctx.transport = NULL

runtime_stream_read_notify(stream, strm_user_data):
  rt = runtime_get_conn_alp_user_data_by_stream_call(stream)
  if rt == NULL or strm_user_data == NULL: return XQC_OK
  if find by transport pointer in rt == NULL: return XQC_OK
  return odin_xqc_stream_transport_read_notify(stream, strm_user_data)

runtime_stream_write_notify(stream, strm_user_data):
  rt = runtime_get_conn_alp_user_data_by_stream_call(stream)
  if rt == NULL or strm_user_data == NULL: return XQC_OK
  if find by transport pointer in rt == NULL: return XQC_OK
  return odin_xqc_stream_transport_write_notify(stream, strm_user_data)

runtime_stream_close_notify(stream, strm_user_data):
  rt = runtime_get_conn_alp_user_data_by_stream_call(stream)
  if rt == NULL: return XQC_OK
  if strm_user_data == NULL: return XQC_OK
  stream_ctx = find by transport pointer in rt
  if stream_ctx == NULL: return XQC_OK
  odin_client_session_destroy(stream_ctx.cs)
  unlink and free stream_ctx
  return XQC_OK

runtime_stream_closing_notify(stream, err_code, strm_user_data):
  rt = runtime_get_conn_alp_user_data_by_stream_call(stream)
  if rt == NULL or strm_user_data == NULL: return
  if find by transport pointer in rt == NULL: return
  odin_xqc_stream_transport_closing_notify(stream, err_code, strm_user_data)

runtime_client_session_on_close(cs, err, stream_ctx):
  rt = stream_ctx.rt
  stream = stream_ctx.stream
  # fire_terminal has already destroyed cs.upstream_t, which cleared xqc
  # stream user data; this unlink is defensive/idempotent bookkeeping.
  if stream_ctx.transport != NULL:
    remove stream_ctx from rt.transport_map
    stream_ctx.transport = NULL
  unlink stream_ctx from rt.sessions
  odin_client_session_destroy(cs)
  if err != 0 and stream != NULL:
    runtime_stream_close_call(stream)
  free stream_ctx
```

Satisfies: G3 via one runtime-owned stream context per local connection, `XQC_STREAM_BIDI` stream creation, `odin_xqc_stream_transport` wrapping, xquic readiness forwarding, and client-session terminal cleanup.

#### 3.2.5 Internal Test Hook Contract

The client QUIC runtime tests drive xquic callbacks and failure branches through a test-only header. Production builds do not expose these symbols.

```c
/* odin/testing/client_xqc_runtime_internal_test.h;
 * visible only when ODIN_XQC_CLIENT_RUNTIME_TESTING is defined. */
#include "odin/client_xqc_runtime.h"
#include "odin/xqc_udp.h"
#include <xquic/xquic.h>

#define ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CAP 128u

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
  ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE
} odin_xqc_client_runtime_test_call_kind_t;

typedef struct odin_xqc_client_runtime_test_call_t {
  odin_xqc_client_runtime_test_call_kind_t kind;
  xqc_engine_t *engine;
  odin_xqc_udp_t *xu;
  xqc_connection_t *conn;
  xqc_stream_t *stream;
  xqc_cid_t cid;
  const char *alpn;
  size_t alpn_len;
  xqc_app_proto_callbacks_t *app_callbacks;
  xqc_transport_callbacks_t transport_callbacks_value;
  void *user_data;
  xqc_int_t xqc_result;
  int int_result;
} odin_xqc_client_runtime_test_call_t;

typedef struct odin_xqc_client_runtime_test_udp_create_record_t {
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
} odin_xqc_client_runtime_test_udp_create_record_t;

typedef struct odin_xqc_client_runtime_test_record_t {
  unsigned int call_count;
  unsigned int dropped_call_count;
  odin_xqc_client_runtime_test_call_t
      calls[ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CAP];
  unsigned int udp_create_calls;
  odin_xqc_client_runtime_test_udp_create_record_t last_udp_create;
} odin_xqc_client_runtime_test_record_t;

typedef struct odin_xqc_client_runtime_test_state_t {
  xqc_connection_t *conn;
  xqc_cid_t current_cid;
  int cid_registered;
  int handshake_done;
  int closing;
  int connect_errno;
} odin_xqc_client_runtime_test_state_t;

typedef enum odin_xqc_client_runtime_test_config_copy_alloc_t {
  ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_SERVER_HOST = 1,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_TOKEN,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_SESSION_TICKET,
  ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_TRANSPORT_PARAMETERS
} odin_xqc_client_runtime_test_config_copy_alloc_t;

typedef struct odin_xqc_client_runtime_test_ops_t {
  xqc_int_t (*engine_register_alpn)(
      xqc_engine_t *engine, const char *alpn, size_t alpn_len,
      xqc_app_proto_callbacks_t *app_callbacks, void *user_data);
  xqc_int_t (*engine_unregister_alpn)(xqc_engine_t *engine, const char *alpn,
                                      size_t alpn_len);
  const xqc_cid_t *(*xqc_connect)(
      xqc_engine_t *engine, const xqc_conn_settings_t *conn_settings,
      const unsigned char *token, unsigned int token_len,
      const char *server_host, int no_crypto_flag,
      const xqc_conn_ssl_config_t *conn_ssl_config,
      const struct sockaddr *peer_addr, socklen_t peer_addrlen,
      const char *alpn, void *user_data);
  void (*conn_set_alp_user_data)(xqc_connection_t *conn, void *user_data);
  xqc_int_t (*conn_close)(xqc_engine_t *engine, const xqc_cid_t *cid);
  void *(*get_conn_alp_user_data_by_stream)(xqc_stream_t *stream);
  xqc_stream_t *(*stream_create_with_direction)(xqc_connection_t *conn,
                                                xqc_stream_direction_t dir,
                                                void *user_data);
  xqc_int_t (*stream_close)(xqc_stream_t *stream);
  int (*udp_register_conn)(odin_xqc_udp_t *xu, const xqc_cid_t *cid);
  void (*udp_unregister_conn)(odin_xqc_udp_t *xu, const xqc_cid_t *cid);
} odin_xqc_client_runtime_test_ops_t;

void odin_xqc_client_runtime_test_reset(void);
void odin_xqc_client_runtime_test_set_ops(
    const odin_xqc_client_runtime_test_ops_t *ops);
const odin_xqc_client_runtime_test_record_t *
odin_xqc_client_runtime_test_record(void);
int odin_xqc_client_runtime_test_state(
    const odin_xqc_client_runtime_t *rt,
    odin_xqc_client_runtime_test_state_t *out);
int odin_xqc_client_runtime_test_fail_next_stream_context_alloc(
    odin_xqc_client_runtime_t *rt, int errnum);
int odin_xqc_client_runtime_test_fail_next_pending_queue_append(
    odin_xqc_client_runtime_t *rt, int errnum);
int odin_xqc_client_runtime_test_fail_config_copy_alloc(
    odin_xqc_client_runtime_test_config_copy_alloc_t site, int errnum);

/* odin/testing/client_session_internal_test.h symbols,
 * visible only when ODIN_CLIENT_SESSION_TESTING is defined. */
int odin_client_session_test_fail_next_dial(odin_client_session_t *cs,
                                            int errnum);
int odin_client_session_test_fail_next_downstream_transport_create(int errnum);
int odin_client_session_test_fail_next_create(int errnum);

/* odin/testing/connect_session_internal_test.h addition,
 * visible only when ODIN_CONNECT_SESSION_TESTING is defined. */
int odin_connect_session_test_fail_next_create_client(int errnum);
```

**Unstated contract.** `test_reset` clears the record, optional ops, one-shot config-copy allocation hooks, stream-context allocation hooks, and pending-append hooks. Runtime-owned wrappers append records before calling production or fake ops. `ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_CREATE` increments `udp_create_calls` and records `last_udp_create` before calling production `odin_xqc_udp_create`; the dedicated record mirrors the server-runtime UDP-create record shape and exposes the loop, local address pointer/length, engine type, xquic config pointers, value copies of the engine and transport callback tables, and `app_user_data`. T1, T2, and T15 use `last_udp_create.transport_callbacks_value` to assert that `save_token`, `save_session_cb`, and `save_tp_cb` are non-null after runtime defaulting and that caller callbacks such as `cert_verify_cb` are preserved. `odin_xqc_client_runtime_test_ops_t.xqc_connect` is the only replacement for `xqc_connect`; there is intentionally no replacement hook for `odin_xqc_udp_create`, so UDP-driver creation failure still uses the existing `ODIN_XQC_UDP_TESTING` `engine_create` hook in `odin/testing/xqc_udp_internal_test.h`. T19 and T20 use the sibling `ODIN_XQC_UDP_TESTING` `engine_destroy` hook from that same header to invoke xquic close notification from inside `odin_xqc_udp_destroy`, proving stopped force cleanup is not dependent on direct callback injection after destroy returns. `//odin/testing:odin_unittests` enables that cross-module hook through `odin_xqc_udp_testing_config`.

The named `fake-xquic fixture` is the required setup for every non-production client-runtime row that passes fake `xqc_engine_t`, `xqc_connection_t`, or `xqc_stream_t` pointers through runtime wrappers. It installs a complete `odin_xqc_client_runtime_test_ops_t` table, not a partial table: recording fakes for ALPN register/unregister, `xqc_connect`, bidirectional stream creation, CID register/unregister, and recording/no-op companion fakes for `conn_set_alp_user_data`, `conn_close`, `get_conn_alp_user_data_by_stream`, and `stream_close`. The companion fakes never dereference fake xquic objects: `conn_set_alp_user_data` records the `(conn, user_data)` pair and returns, `conn_close` records the CID and returns `XQC_OK`, `get_conn_alp_user_data_by_stream` returns the fixture-configured runtime pointer or `NULL`, and `stream_close` records the stream pointer and returns `XQC_OK`. A row may override one fixture callback only with another fake that still avoids production xquic calls on fake objects. Leaving an op `NULL` means the wrapper calls the production xquic function and is permitted only when every object reaching that wrapper is a production xquic object; T15 is the explicit production row and clears client-runtime, server-runtime, and UDP fake ops before it starts. Rows whose fake streams reach `odin_xqc_stream_transport` also install the sibling `ODIN_TRANSPORT_XQC_TESTING` stream-transport ops needed by that row, so production `xqc_stream_recv`, `xqc_stream_send`, `xqc_stream_set_user_data`, and `xqc_stream_close` receive only production streams.

`odin_xqc_client_runtime_test_state(rt, out)` is the only test-visible snapshot of private runtime state. It is compiled only under `ODIN_XQC_CLIENT_RUNTIME_TESTING`; production builds keep `odin_xqc_client_runtime_t` opaque and expose no state getter. The helper validates `rt` and `out`, returns `-1/EINVAL` on null input, writes no call record, and otherwise copies `rt->conn`, `rt->current_cid`, `rt->cid_registered`, `rt->handshake_done`, `rt->closing`, and `rt->connect_errno` into `*out`. T21 uses two snapshots, one before and one after a rejected duplicate `conn_create_notify`, to prove the `EALREADY` error channel and private connection/CID/handshake/closing preservation that cannot be established from call records alone.

`odin_xqc_client_runtime_test_fail_config_copy_alloc(site, errnum)` is consumed only by the named copied-config allocation site on the next `odin_xqc_client_runtime_create` call. Valid `site` values are `ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_SERVER_HOST`, `ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_TOKEN`, `ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_SESSION_TICKET`, and `ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_TRANSPORT_PARAMETERS`; any other value returns `-1/EINVAL` and arms no hook. The hook fires immediately before the selected allocation, consumes itself, makes `create` return `-1` with `errno` set to `errnum`, frees every earlier runtime-owned copy in the fixed order from §3.2.2, records no UDP create, leaves `*out` untouched, and allows later creates to use the production allocation path. If the selected site is not reached, such as the token site with `token_len == 0` or a nested-buffer site with zero length, the hook remains armed until `test_reset` or a later create reaches that site, so a row expecting failure fails instead of silently passing through an unexercised hook.

`odin_xqc_client_runtime_test_fail_next_pending_queue_append` is consumed only by `append_pending_local_fd` before queue mutation and ownership transfer; when armed, the call returns `-1`, sets `errno` to the requested value, leaves the fd with the caller, and allows later append attempts to use the production allocation path. `odin_xqc_client_runtime_test_fail_next_stream_context_alloc` is consumed only by `create_one_client_session` before `calloc` and before fd ownership transfer for immediate post-handshake `add_connection`; when armed, the call returns `-1`, sets `errno` to the requested value, leaves no `odin_client_session_t`, no `stream_ctx`, no xquic stream, and no transport-map entry, and allows later session creation to use the production allocation path. For queued conversion, the runtime already owns the pending fd, but the hook still fires before any session or stream context exists; the failing queued fd is closed by the queued-conversion failure rule and the next queued fd is attempted.

`get_conn_alp_user_data_by_stream` wraps `xqc_get_conn_alp_user_data_by_stream` so stream callback tests can assert runtime recovery happens before live-map lookup or transport helper forwarding. `xqc_stream_set_user_data` remains covered by the existing `ODIN_TRANSPORT_XQC_TESTING` hook in `odin/testing/transport_xqc_internal_test.h`, because stream user-data install/clear belongs to `odin_xqc_stream_transport_create`, not this runtime. Under `ODIN_TRANSPORT_XQC_TESTING`, clearing `odin_xqc_stream_transport_test_set_ops(NULL)` makes null `recv`/`send` return `-XQC_CLOSING` and null `set_user_data` skip the production call; rows that need production stream I/O, including T15, install an explicit `odin_xqc_stream_transport_test_ops_t` pass-through whose `recv`, `send`, and `set_user_data` functions call `xqc_stream_recv`, `xqc_stream_send`, and `xqc_stream_set_user_data` directly. The gated client-runtime call sites live in `odin/client_xqc_runtime.c` under `ODIN_XQC_CLIENT_RUNTIME_TESTING`, and `//odin/testing:odin_unittests` enables them through a new `odin_xqc_client_runtime_testing_config` alongside existing `odin_xqc_udp_testing_config`, `odin_transport_xqc_testing_config`, `odin_client_session_testing_config`, and `odin_connect_session_testing_config`.

`odin_client_session_test_fail_next_dial` already exists in `odin/testing/client_session_internal_test.h` under `ODIN_CLIENT_SESSION_TESTING`; it arms a per-session branch consumed only inside `start_dial` before the production `odin_dial_start` call. T4 arms it after `odin_client_session_create_with_upstream_transport` has returned a session and before local CONNECT bytes are written, then expects the factory success, late-local-bytes, factory failure, and post-factory failure outcomes to occur unchanged; a factory branch that accidentally enters `start_dial` consumes the hook and fails those subcases with the armed errno instead. `odin_client_session_test_fail_next_create` already exists in the same header. This RFC extends that hook's gated first branch to `odin_client_session_create_with_upstream_transport`, matching the existing branch at the top of `odin_client_session_create`; when armed, the new constructor consumes the requested errno, returns `-1`, leaves `*out` untouched, performs no downstream transport creation, and leaves fd ownership with its caller. This RFC also adds `odin_client_session_test_fail_next_downstream_transport_create` under the same internal header and config. Its gated call site is inside `odin_client_session_create_with_upstream_transport` immediately before the downstream `odin_fd_transport_create` call; when armed, the new constructor consumes the requested errno, frees only the unpublished `odin_client_session_t` shell, leaves `*out` untouched, calls no factory or `on_close`, creates no downstream transport, and leaves `conn_fd` owned by the caller. T4 drives initial downstream interest setup failure with the existing `odin_event_loop_test_fail_next_kqueue_change` hook under `ODIN_EVENT_LOOP_TESTING`; `//odin/testing:odin_unittests` enables both client-session and event-loop hooks through `odin_client_session_testing_config` and `odin_event_loop_testing_config`.

`odin_connect_session_test_fail_next_create_client` is a sibling of the existing `odin_connect_session_test_fail_next_create_server` hook in `odin/testing/connect_session_internal_test.h`. Its gated call site is the first branch inside `odin_connect_session_create_client` under `ODIN_CONNECT_SESSION_TESTING`: when armed, it consumes the requested errno, returns `-1`, sets `errno` to that value, leaves `*out` untouched, and performs no transport I/O. Production builds do not compile the hook, and the only target-wide activation is the `odin_connect_session_testing_config` already applied to `//odin/testing:odin_unittests`.

`odin_event_loop_test_fail_next_kqueue_change` already exists in `odin/testing/event_loop_internal_test.h` under `ODIN_EVENT_LOOP_TESTING`. T2 and T4 use it only on host macOS, guarded with `#if defined(__APPLE__)`; non-macOS executions call `GTEST_SKIP()` for those subcases because the hook reports `EOPNOTSUPP` when kqueue is not the event backend. The T2 macOS subcase arms `(ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_ADD, ODIN_EVENT_READ, EIO)` so `odin_xqc_udp_start` fails through the real `odin_udp_set_interest` call path at [odin/xqc_udp.c](/Users/tangjiacheng/Downloads/Monorepo/odin/xqc_udp.c:451) before any client-runtime `xqc_connect` wrapper can run. The T4 macOS subcase arms the same add/read failure before `odin_client_session_create_with_upstream_transport`, so the constructor fails through the real downstream `odin_transport_set_interest(..., ODIN_TRANSPORT_READ)` call after downstream fd transport creation and before `*out` publication. `//odin/testing:odin_unittests` already applies `odin_event_loop_testing_config` target-wide.

`odin_xqc_server_runtime_test_reset`, `odin_xqc_server_runtime_test_set_ops`, and `odin_xqc_server_runtime_test_record` already exist in `odin/testing/server_xqc_runtime_internal_test.h` under `ODIN_XQC_SERVER_RUNTIME_TESTING`. T15 uses those symbols only to clear optional fake ops with `odin_xqc_server_runtime_test_set_ops(NULL)` and record production server-runtime calls while the real `odin_xqc_server_runtime` and xquic/UDP paths run. `//odin/testing:odin_unittests` already applies `odin_xqc_server_runtime_testing_config` target-wide.

**Mechanism.**

```text
runtime_odin_xqc_udp_create_call(config, out):
  record ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_CREATE
  increment udp_create_calls
  copy config loop, local_addr, local_addrlen, engine_type, engine_config,
       ssl_config, engine_callbacks, transport_callbacks, and app_user_data
       into last_udp_create
  if config.engine_callbacks != NULL:
    copy *config.engine_callbacks into last_udp_create.engine_callbacks_value
  if config.transport_callbacks != NULL:
    copy *config.transport_callbacks into
         last_udp_create.transport_callbacks_value
  return odin_xqc_udp_create(config, out)

runtime_xqc_connect_call(args):
  record ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_XQC_CONNECT
  if test ops xqc_connect exists: return test op(args)
  return xqc_connect(args)

runtime_conn_set_alp_user_data_call(conn, user_data):
  record ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_SET_ALP_USER_DATA
  if test op exists:
    test op(conn, user_data)
    return
  xqc_conn_set_alp_user_data(conn, user_data)

runtime_conn_close_call(engine, cid):
  record ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_CONN_CLOSE
  if test op exists: return test op(engine, cid)
  return xqc_conn_close(engine, cid)

runtime_stream_create_bidi_call(conn):
  record ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CREATE_BIDI
  if test op exists: return test op(conn, XQC_STREAM_BIDI, NULL)
  return xqc_stream_create_with_direction(conn, XQC_STREAM_BIDI, NULL)

runtime_get_conn_alp_user_data_by_stream_call(stream):
  record ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_GET_CONN_ALP_USER_DATA_BY_STREAM
  if test op exists: return test op(stream)
  return xqc_get_conn_alp_user_data_by_stream(stream)

runtime_stream_close_call(stream):
  record ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_STREAM_CLOSE
  if test op exists: return test op(stream)
  return xqc_stream_close(stream)

install_fake_xquic_fixture(row):
  install row-specific fake ALPN, connect, stream-create, and CID callbacks
  install no-op conn_set_alp_user_data, conn_close,
          get_conn_alp_user_data_by_stream, and stream_close callbacks
  fail the row if a fake xquic object reaches a wrapper whose op is NULL

odin_xqc_client_runtime_test_state(rt, out):
  if rt == NULL or out == NULL:
    errno = EINVAL; return -1
  out.conn = rt.conn
  out.current_cid = rt.current_cid
  out.cid_registered = rt.cid_registered
  out.handshake_done = rt.handshake_done
  out.closing = rt.closing
  out.connect_errno = rt.connect_errno
  return 0
```

Satisfies: G1, G2, and G3 via test-observable wrappers for runtime-owned xquic and UDP call sites without changing production contracts.

## 4. Security

- **S1.**
  - **Threat:** A client-runtime implementation that drops `conn_ssl_config.cert_verify_flag`, fails to preserve the caller `xqc_transport_callbacks_t.cert_verify_cb`, or passes a null callback while `XQC_TLS_CERT_FLAG_NEED_VERIFY` is set would let a spoofed Odin QUIC server complete the handshake and send attacker-controlled CONNECT_RESP / relay bytes into the local HTTPS_PROXY pipeline.
  - **Mitigation:** §3.2.2 copies `conn_ssl_config`, rejects `XQC_TLS_CERT_FLAG_NEED_VERIFY` unless the caller supplies a non-null `cert_verify_cb`, copies the caller transport callback table by value, fills only missing client cache callbacks with no-op callbacks, and overrides only `conn_update_cid_notify` for CID recovery. §5 T2 validates the rejected null-callback cases and callback preservation in the UDP-create record; §5 T15 runs a production xquic client/server handshake and requires the real cert callback counter to be nonzero.
  - **Enforcement:** §5 T2 fires the create-time `XQC_TLS_CERT_FLAG_NEED_VERIFY` / null `cert_verify_cb` rejection and UDP-create callback-preservation checks; §5 T15 runs the production xquic client/server handshake and asserts the caller cert verification callback is invoked.
  - **Residual risk:** Trust anchors and hostname policy are still selected by the caller through xquic's `conn_ssl_config` and callback implementation; this RFC preserves and requires the callback path but does not define a deployment CA store.

- **S2.**
  - **Threat:** A local HTTPS_PROXY requester can choose the CONNECT target that the remote Odin server receives in CONNECT_REQ; if the client QUIC runtime used those bytes to call local dial, filesystem, process, or dynamic-loader APIs, target selection would happen on the client host before the server-side dial filter could enforce policy.
  - **Mitigation:** §3.2.1 keeps the factory constructor on the existing RFC-023 client pipeline, so the client runtime opens only the caller-configured QUIC server endpoint from §3.2.2 through the intended `xqc_connect` call and the factory branch in `odin/client_session.c` never calls local `start_dial`, `odin_dial_start`, raw `connect`, `open`, `exec*`, or `dlopen` based on peer bytes or CONNECT target bytes. §5 T4 arms the existing `odin_client_session_test_fail_next_dial` hook after factory-constructor creation and proves valid factory success, factory failure, and post-factory failure do not enter `start_dial`; §5 T12 tokenizes both the client-runtime source/header/GN target and the `odin/client_session.c` factory-constructor implementation region to enforce the local-resource API boundary while still allowing the exact `xqc_connect` integration in the runtime. CONNECT-target authorization remains at the server receiving CONNECT_REQ: current `odin_server_session_set_dial_filter` is documented in [odin/server_session.h](/Users/tangjiacheng/Downloads/Monorepo/odin/server_session.h:34) as the hook consulted after CONNECT_REQ target parsing and before `odin_dial_start`; §5 T15 installs a production server dial filter that permits only the test origin sockaddr.
  - **Enforcement:** §5 T4 arms `odin_client_session_test_fail_next_dial` on factory-created sessions and asserts the hook is not consumed on valid factory success, factory failure, or post-factory failure; §5 T7 fires local parse-failure, local EOF, and unusable-runtime inputs and asserts zero xquic stream creation before a completed CONNECT request reaches the upstream factory; §5 T12 rejects forbidden local-resource API tokens in `odin/client_xqc_runtime.c`, `odin/client_xqc_runtime.h`, `source_set("odin_client_xqc_runtime")`, and the `odin/client_session.c` factory-constructor region while allowing `xqc_connect`; §5 T15 drives a production client/server CONNECT over xquic with a server-side dial filter that permits only the test origin sockaddr.
  - **Residual risk:** Deployments that install no server-side dial filter retain the current server-session default allow policy; that policy is outside this client-runtime RFC.

## 5. Testing Strategy

Runtime integration rows execute xquic callbacks and Odin event-loop APIs on the `odin_event_loop_t` owner thread. Rows that pump `odin_event_loop_run` use the existing watchdog/poll-timer fixture pattern from `odin/testing/server_xqc_runtime_unittests.cpp`: a recurring owner-thread timer observes atomic test-side milestones, calls owner-thread-only APIs, and stops the loop; a one-shot watchdog fails the row if the milestone is not reached. Socket reads, pipe reads, and child waits use deadlines. Fixtures install `signal(SIGPIPE, SIG_IGN)` before any write-after-close or EOF probe.

Rows T1-T3, T5-T11, T13-T14, and T16-T21 install the §3.2.5 `fake-xquic fixture` before creating a client runtime, because those rows pass fake xquic engines, connections, or streams through runtime wrappers. T15 is the only client-runtime row that intentionally clears the fake ops and exercises production xquic objects; T4 and T12 do not create fake client-runtime xquic objects.

### 5.0 Coverage Matrix

Only reachable cells introduced or branched by this RFC are listed. This RFC adds no new wire decoder and no new byte-stream relay state machine; RFC-003, RFC-018, RFC-023, and RFC-016 remain the owners of HTTP parsing, CONNECT frame decoding, client-session response mapping, and xqc stream transport I/O details. Paired server-side QUIC runtime behavior is unchanged and already covered by its own runtime tests, so this RFC adds client-side rows only.

| Axis | Value | Rows |
|------|-------|------|
| G# | G1 client xquic startup and ALPN | T1, T2, T15 |
| G# | G2 CID registration, update, unregister, close, running-destroy deferral, and stopped force cleanup | T1, T2, T3, T10, T11, T13, T14, T15, T16, T17, T18, T19, T20, T21 |
| G# | G3 stream-per-successfully-parsed local CONNECT pipeline and owned-fd cleanup | T3, T4, T5, T6, T7, T8, T9, T10, T11, T13, T14, T15, T16, T17, T18, T19, T20 |
| G# | G4 TCP path preserved and scoped away from CLI selection | T4, T12 |
| S# | S1 server authentication callback preservation | T2, T15 |
| S# | S2 CONNECT-target dial-policy delegation | T4, T7, T12, T15 |
| State | Runtime create with valid config | T1 |
| State | Runtime create/start invalid receiver, invalid config, optional `engine_config`, copied start-time config, copied-config allocation rollback and hook boundaries, token bounds, nested `conn_ssl_config` buffers, and AF_INET/AF_INET6 `peer_addrlen` bounds | T2 |
| State | Runtime transport callback table preserves caller callbacks and fills no-op client cache defaults before UDP create | T1, T2, T15 |
| State | UDP driver create failure before ALPN registration, including inherited `engine_config->sendmmsg_on` rejection | T2 |
| State | ALPN registration failure after UDP driver create | T2 |
| State | Start succeeds through synchronous `conn_create_notify` | T1 |
| State | Already-connected or closing `conn_create_notify` returns `-1/EALREADY` without mutating the active connection/CID state observed through the test-state snapshot helper | T21 |
| State | Valid stop idempotency, stopped `add_connection` rejection, and restart without reconnect | T1, T2, T16, T17 |
| State | Queued pre-handshake fds survive stop/restart and convert after handshake | T16 |
| State | Live post-handshake stream sessions survive stop/restart without cleanup | T17 |
| State | Stale or closing handshake-finished callbacks do not drain queued fds | T18 |
| State | first-start and restart `odin_xqc_udp_start` failure rollback | T2 |
| State | `xqc_connect` fails because CID registration fails | T2 |
| State | `xqc_connect` returns `NULL` before `conn_create_notify`, after successful `conn_create_notify`, or after `conn_close_notify` cleaned up the startup connection | T2 |
| State | CID update succeeds | T3 |
| State | CID update registration fails, abort-cleans live xqc stream sessions, and closes old CID | T3 |
| State | CID update arrives while closing or for stale connection | T3 |
| State | Connection close before handshake with queued pending fds | T10 |
| State | Connection close after handshake with live stream sessions | T11 |
| State | Runtime destroy with no live xquic connection | T2 |
| State | Stale connection close notification after failed startup retry | T2 |
| State | Runtime destroy before handshake with queued pending fds | T13 |
| State | Runtime destroy after handshake with live stream sessions | T14 |
| State | Stopped runtime destroy before handshake force-cleans queued pending fds and registered CID without xquic progress | T19 |
| State | Stopped runtime destroy after handshake force-cleans live stream sessions and registered CID without xquic progress | T20 |
| State | `add_connection` before handshake queues the fd | T5, T16 |
| State | pending queue append fails before fd ownership transfer | T8 |
| State | `add_connection` after handshake creates session immediately | T6, T17 |
| State | synchronous client-session constructor fails before stream factory | T8 |
| State | Runtime not started, stopped, or closing rejects `add_connection` without fd ownership | T1, T7 |
| State | Local HTTP parse fails or EOFs before upstream factory | T7 |
| State | xqc stream create, stream context alloc, or stream-transport create fails | T8 |
| State | connect-session create fails after stream transport exists | T8 |
| State | live stream close/reset plus NULL read/write/closing, stale, and missing-ALPN callbacks | T9 |
| Completion mode | happy single-call CONNECT_REQ write, CONNECT_RESP read, HTTP 200, and relay | T5, T16, T17, T18 |
| Completion mode | happy staged-AGAIN-resume for CONNECT_REQ write offset and CONNECT_RESP read buffer | T6 |
| Completion mode | production xquic/UDP handshake to an Odin QUIC server and relay over a real bidirectional QUIC stream | T15 |
| Decoder branch | no new decoder; local HTTP parse-failure and pre-parse EOF passthrough prove the factory is not called before a completed CONNECT request | T7 |
| Benign-vs-fatal split | stream read/write/close callbacks with `NULL`, missing ALPN user data, or stale transport pointer return `XQC_OK`; stream closing with those inputs is a no-op | T9 |
| Benign-vs-fatal split | stream close and stream reset/closing on live transport are fatal to that session only | T9 |
| Constructor / factory lifecycle | client-session upstream factory constructor NULL inputs, downstream fd transport creation failure, initial downstream interest failure, post-parse downstream READ pause through CONNECT_RESP, armed `start_dial` bypass checks, optional `upstream_destroying == NULL` terminal and post-factory-abort cleanup, and factory failure | T4 |
| Constructor / factory precondition | runtime config validation, optional null `engine_config`, sendmmsg-on UDP rejection, client cache callback defaulting, client cert-verify transport callback validation, AF_INET/AF_INET6 peer-address validation, token-length validation, `server_host` / token / `conn_ssl_config` copy validation and rollback, UDP create rollback, ALPN rollback, stop validation, and `xqc_connect` rollback | T2 |
| Callback-safe lifecycle hand-off | client-session `on_close` from a live xqc stream close/reset callback, CID-update registration-failure abort cleanup, and runtime destroy cleanup clear stream user data before any non-OK `STREAM_CLOSE`, and stale callbacks run only after that clear | T3, T9, T14, T20 |
| Post-syscall sub-branch | no new syscall-success/follow-up-failure primitive in this RFC; T2 drives the client-runtime UDP-start rollback contract and T4 drives constructor downstream-interest rollback through the existing event-loop kqueue-change failure hook, while UDP socket, fd transport, and event-loop backend internals stay owned by RFC-015/RFC-017/RFC-013/RFC-010 | T2, T4 |
| Test hook contract | client xqc runtime call records, fake-xquic fixture for non-production rows, dedicated UDP-create record, runtime-state snapshot helper, config-copy allocation hook boundary behavior, pending-append hook, stream-context allocation hook, and reset semantics | T1, T2, T3, T5, T6, T8, T9, T10, T11, T13, T14, T15, T16, T17, T18, T19, T20, T21 |
| Test hook contract | sibling hooks for UDP engine create/destroy and xqc stream user-data install/clear are used through their existing internal headers | T2, T3, T8, T9, T19, T20 |
| Test hook contract | sibling client-session dial, downstream-create, client-session create, and client connect-session create hooks are used through `ODIN_CLIENT_SESSION_TESTING` and `ODIN_CONNECT_SESSION_TESTING` | T4, T8 |
| Test hook contract | sibling event-loop kqueue-change failure hook drives production `odin_xqc_udp_start` rollback and constructor downstream-interest rollback | T2, T4 |

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | Runtime startup registers ALPN, initial CID, and stop/restart state | Reset client-runtime and xqc-UDP test records, then install the §3.2.5 `fake-xquic fixture` with xqc-UDP fake `engine_create`/`engine_destroy`, client-runtime fake ALPN register/unregister, and fake `xqc_connect` that invokes the registered `conn_create_notify` with `cidA` and returns `cidA`. Create runtime with loopback UDP local address, loopback IPv4 peer address, `server_host = "odin.test"`, sentinel xqc configs, and a caller `xqc_transport_callbacks_t` whose only non-null slot is a sentinel `cert_verify_cb`, then call `start`, `start` again, `stop`, `add_connection(rt, stopped_fd)`, `stop` again, `start` again, and `destroy`; after destroy records `CONN_CLOSE`, explicitly fire the registered `conn_close_notify(connA, cidA, xu, rt)` | `last_udp_create` records exactly one UDP create with `loop`, `local_addr`, `local_addrlen`, `engine_config`, `ssl_config`, and `engine_callbacks` pointer-equal to the create config, `engine_type == XQC_ENGINE_CLIENT`, runtime-owned `transport_callbacks` whose value copy preserves the caller `cert_verify_cb`, fills non-null no-op `save_token`, `save_session_cb`, and `save_tp_cb` because the caller left them null, overrides `conn_update_cid_notify`, and `app_user_data == rt`. ALPN register is recorded once with byte-exact `odin/1` and length `6`. First `start` records one UDP start and one `xqc_connect` whose `alpn` bytes equal `odin/1`, whose peer sockaddr is the complete IPv4 socket address from config, and whose user data equals `odin_xqc_udp_xqc_user_data(xu)`. `conn_create_notify` records one `ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_UDP_REGISTER_CONN(cidA)`, stores `connA`, and records one ALP user-data set to `rt`. Second `start` records no UDP start and no second connect. First `stop` returns `0` and records one `UDP_STOP`; stopped `add_connection` returns `-1/ENOTCONN` and `fcntl(stopped_fd, F_GETFD) >= 0`; second `stop` returns `0` and records no second `UDP_STOP`. Restart records one additional UDP start and no second `xqc_connect`. Destroy records `CONN_CLOSE(engine, cidA)` before any CID unregister, ALPN unregister, UDP destroy, or runtime free; the explicit close notification then records one CID unregister for `cidA`, one ALPN unregister, and one UDP destroy | G1, G2 | unit |
| T2 | Runtime validation, copied config, stop preconditions, and startup rollback | Install the §3.2.5 `fake-xquic fixture` for every subcase that creates or starts a runtime. Subcases: `create(NULL)`, null `out`, null `loop`, null `local_addr`, null `peer_addr`, null `server_host`, null `engine_ssl_config`, null `engine_callbacks`, null `conn_settings`, null `conn_ssl_config`, `conn_ssl_config.cert_verify_flag = XQC_TLS_CERT_FLAG_NEED_VERIFY` with null `transport_callbacks`, `conn_ssl_config.cert_verify_flag = XQC_TLS_CERT_FLAG_NEED_VERIFY` with `transport_callbacks.cert_verify_cb == NULL`, `conn_ssl_config.session_ticket_len > 0` with null `session_ticket_data`, `conn_ssl_config.transport_parameter_data_len > 0` with null `transport_parameter_data`, `token_len > 0` with null `token`, `token_len == 256` with a non-null token, `token_len == 257` with a non-null token, `peer_addrlen == 0`, `peer_addrlen == 1`, `peer_addrlen == sizeof(struct sockaddr) - 1`, `AF_INET` with `peer_addrlen == sizeof(struct sockaddr_in) - 1`, `AF_INET` with `peer_addrlen == sizeof(struct sockaddr_in) + 1`, `AF_INET6` with `peer_addrlen == sizeof(struct sockaddr_in6) - 1`, `AF_INET6` with `peer_addrlen == sizeof(struct sockaddr_in6) + 1`, unknown `sa_family`, `AF_INET` with `peer_addrlen == sizeof(struct sockaddr_in6)`, `start(NULL)`, `stop(NULL)`, `destroy(NULL)`, valid create followed by `stop` before `start`, valid create followed by `destroy` before `start`, valid create/start with exact `AF_INET` loopback `peer_addrlen == sizeof(struct sockaddr_in)`, valid create/start with exact `AF_INET6` loopback `peer_addrlen == sizeof(struct sockaddr_in6)`, valid create/start with `engine_config = NULL`, valid create with `engine_config->sendmmsg_on = 1`, valid create with `transport_callbacks = NULL` and no `XQC_TLS_CERT_FLAG_NEED_VERIFY`, valid create/start with `conn_ssl_config.session_ticket_len == 0` and `session_ticket_data == NULL`, valid create with caller sentinel `save_token`, `save_session_cb`, `save_tp_cb`, and `cert_verify_cb`, valid config whose `server_host`, token bytes, `conn_settings`, `conn_ssl_config` scalars, nested `session_ticket_data` bytes, nested `transport_parameter_data` bytes, `peer_addr` bytes, and `no_crypto_flag` scalar are mutated or released after `create` but before `start`, invalid `odin_xqc_client_runtime_test_fail_config_copy_alloc((odin_xqc_client_runtime_test_config_copy_alloc_t)0, ENOMEM)` site, valid config with nonzero token and both nested `conn_ssl_config` buffers where `odin_xqc_client_runtime_test_fail_config_copy_alloc(ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_SERVER_HOST, ENOMEM)` is armed, the same valid config where `ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_TOKEN` is armed so `server_host` has already been duplicated before token allocation fails, the same valid config where `ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_SESSION_TICKET` is armed so `server_host` and token have already been copied before session-ticket allocation fails, the same valid config where `ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_TRANSPORT_PARAMETERS` is armed so `server_host`, token, and session-ticket data have already been copied before transport-parameter allocation fails, valid config with `token_len == 0` where the `ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_TOKEN` site is armed but not reached, followed without `test_reset` by a valid config with nonzero token that reaches the still-armed token site, valid config with xqc-UDP `engine_create` returning `NULL/EIO`, valid config with ALPN register returning `-XQC_EPARAM`, valid create where host macOS `#if defined(__APPLE__)` arms `odin_event_loop_test_fail_next_kqueue_change(loop, ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_ADD, ODIN_EVENT_READ, EIO)` and non-macOS execution `GTEST_SKIP()`s this kqueue subcase, one valid create where fake `xqc_connect` returns `NULL` without invoking `conn_create_notify` and leaves `rt.connect_errno == 0` followed by `destroy` without retry, a second valid create where fake `xqc_connect` returns `NULL` without invoking `conn_create_notify` and leaves `rt.connect_errno == 0` followed by a successful retry, valid create where fake `xqc_connect` invokes `conn_create_notify` successfully with `cidA` and then returns `NULL`, followed by a successful retry with `connB/cidB` and then a stale close notification for `connA`, valid create where fake `xqc_connect` invokes `conn_create_notify(connA, cidA, xu, NULL)`, then `conn_close_notify(connA, cidA, xu, rt)`, then returns `NULL`, followed by a successful retry with `connB/cidB`, valid create where `xqc_connect` invokes `conn_create_notify` but fake `udp_register_conn` returns `-1/ENOMEM`, and a successful start/stop followed by the same kqueue-change hook before restart | Invalid inputs return `-1/EINVAL`, preserve sentinel `*out` where present, and record no UDP create for null `engine_ssl_config`, rejected peer addresses, over-cap tokens, invalid nested `conn_ssl_config` pointer/length pairs, or rejected cert-verify callback configurations. Exact `AF_INET` and `AF_INET6` peer addresses are accepted, copied, and passed byte-exact to fake `xqc_connect`; all one-short, one-long, over-cap, unknown-family, and family/length-mismatch peer-address cases are rejected. `token_len == 256` is accepted and the 256 token bytes are passed byte-exact to fake `xqc_connect`; `token_len == 257` is rejected with no UDP create. Valid null `engine_config` is accepted, records `last_udp_create.engine_config == NULL`, proceeds through ALPN registration and start, and reaches one fake `xqc_connect` so xquic's default engine-config path is covered. The `engine_config->sendmmsg_on = 1` subcase returns `-1/ENOTSUP`, preserves sentinel `*out`, records one client-runtime UDP-create attempt whose `last_udp_create.engine_config` points at the rejected config, records no ALPN register, no UDP destroy, no `xqc_connect`, and no xqc-UDP `engine_create` call, and ASan observes copied config cleanup. The valid null transport-callback subcase records one `last_udp_create` whose runtime-owned transport table value has non-null `save_token`, `save_session_cb`, and `save_tp_cb`, null `cert_verify_cb`, and runtime-owned `conn_update_cid_notify`; direct calls through those three defaulted cache callbacks after setting `errno = EDOM` leave `errno` and the client-runtime call record unchanged. The zero-ticket subcase accepts caller-null `session_ticket_data`; fake `xqc_connect` observes `conn_ssl_config.session_ticket_len == 0` and a non-null session-ticket pointer that remains valid for the runtime lifetime, and ASan observes no invalid free or leak for the runtime-owned empty byte. The caller-sentinel callback subcase records pointer equality in `last_udp_create.transport_callbacks_value` for `save_token`, `save_session_cb`, `save_tp_cb`, and `cert_verify_cb`, proving defaulting did not overwrite caller callbacks. Valid `stop` before `start` returns `0`, records no `UDP_STOP`, and a later `start` can still succeed. Valid `destroy` before `start` unregisters ALPN once, destroys the UDP driver once, records no `CONN_CLOSE`, no CID unregister, and no `UDP_STOP`, and ASan observes runtime-owned copied config cleanup. The post-create mutation subcase proves `xqc_connect` sees the original `server_host`, token bytes, `conn_settings`, `conn_ssl_config` scalar values, nested `session_ticket_data` bytes through a runtime-owned pointer, nested `transport_parameter_data` bytes through a runtime-owned pointer, `peer_addr`, `peer_addrlen`, and `no_crypto_flag` snapshots, not the mutated or released caller storage. The invalid config-copy allocation site returns `-1/EINVAL`, arms no hook, leaves the next valid create able to reach UDP create, and leaves the client-runtime record unchanged before that next create. The `ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_SERVER_HOST` allocation failure returns `-1/ENOMEM` before any copied-config allocation succeeds, preserves sentinel `*out`, records no UDP create, and ASan observes no leaked runtime shell or config storage. The `ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_TOKEN` failure returns `-1/ENOMEM` after `server_host` has been duplicated and before token allocation succeeds, preserves sentinel `*out`, records no UDP create, and ASan observes the duplicated `server_host` is freed. The `ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_SESSION_TICKET` failure returns `-1/ENOMEM` after `server_host` and token have been copied and before session-ticket allocation succeeds, preserves sentinel `*out`, records no UDP create, and ASan observes both earlier copies are freed. The `ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_TRANSPORT_PARAMETERS` failure returns `-1/ENOMEM` after `server_host`, token, and `session_ticket_data` have been copied and before `transport_parameter_data` allocation succeeds, preserves sentinel `*out`, records no UDP create, and ASan observes all earlier copied config storage is freed. The unreachable token-site subcase with `token_len == 0` succeeds without consuming the armed hook; after destroying that runtime without `test_reset`, the next create with nonzero token fails at `ODIN_XQC_CLIENT_RUNTIME_TEST_CONFIG_COPY_TOKEN`, returns `-1/ENOMEM`, preserves sentinel `*out`, records no UDP create for the second create, and proves unreached hooks remain armed. UDP create failure records no ALPN register and no UDP destroy for a nonexistent driver. ALPN failure returns `-1/EIO`, destroys the already-created UDP driver, frees copied nested `conn_ssl_config` buffers, and fake engine destroy is observed once. Host-macOS first-start UDP-start failure returns `-1/EIO`, records no `XQC_CONNECT`, no CID registration, no UDP stop, and leaves `add_connection` returning `-1/ENOTCONN` with fd ownership still with the caller; a later valid start retries once and succeeds. The no-callback `xqc_connect == NULL` destroy subcase returns `-1/EIO`, records one UDP stop, no CID registration, no CID unregister, no live connection, and a following `destroy` unregisters ALPN once and destroys UDP once synchronously with no `CONN_CLOSE`. The no-callback `xqc_connect == NULL` retry subcase returns `-1/EIO`, records one UDP stop, no CID registration, no CID unregister, no live connection, `stop` records no additional UDP stop, and a later valid start retries with one new `XQC_CONNECT`. Post-create-notify `xqc_connect == NULL` without a close callback returns `-1/EIO`, records `CONN_CLOSE(engine, cidA)` before one `UDP_UNREGISTER_CONN(cidA)`, records one UDP stop, leaves no live connection or registered CID, `stop` records no additional UDP stop or CID unregister, and a later valid start retries with one new `XQC_CONNECT`; the stale close notification for `connA` after retry returns normally, records no extra unregister/close/UDP destroy, and leaves `connB/cidB` registered. Post-create-notify plus close-before-return `xqc_connect == NULL` records one `UDP_UNREGISTER_CONN(cidA)` from `conn_close_notify`, records no terminal runtime close state, records one UDP stop, and a later valid start succeeds with `connB/cidB` instead of being rejected as closing or already connected. CID-register failure makes `start` return `-1/ENOMEM`, stops the UDP driver exactly once, records no CID unregister, leaves no live connection, a later `stop` records no additional `UDP_STOP`, and a later valid start can still succeed. Restart UDP-start failure after a prior successful `stop` returns `-1/EIO`, records no second `XQC_CONNECT`, preserves the active CID registration, keeps `add_connection` rejected with `-1/ENOTCONN`, and a later start restarts UDP without reconnecting | G1, G2, S1 | unit |
| T3 | CID update lifecycle | Install the §3.2.5 `fake-xquic fixture` and start runtime with `connA/cidA`. Success subcase fires `conn_update_cid_notify(connA, cidA, cidB, xu)` then close. Failure subcase starts a fresh runtime with `connF/cidF`, fires handshake, creates one live local stream session stalled before CONNECT_RESP, saves its xqc stream and transport pointer, makes `udp_register_conn` fail for `cidG`, fires update, then fires saved stale read/write/close/closing callbacks with the saved transport pointer and with `NULL` stream user data before a second update while closing and final close. Stale subcase fires update for unknown `connZ` | Success records `UDP_REGISTER_CONN(cidB)` before `UDP_UNREGISTER_CONN(cidA)`, stores `cidB`, and close unregisters only `cidB`. Failure destroys the live session through the runtime-driven cleanup ordering: `runtime_client_session_upstream_destroying` removes the transport-map entry before wrapper destroy, the xqc stream user-data clear for the saved stream is recorded before that stream's non-OK `STREAM_CLOSE`, exactly one `STREAM_CLOSE` is recorded for the aborted stream, saved stale read/write/close callbacks with the saved transport pointer or `NULL` return `XQC_OK`, saved stale closing callbacks no-op, no stale callback forwards to transport helpers or triggers extra cleanup, and ASan reports no use-after-free. Failure also records one `CONN_CLOSE(engine, cidF)`, unregisters `cidF` once, records no current registered CID, and the second update produces no register, unregister, or second close. Stale update records no calls and returns normally | G2, G3 | unit |
| T4 | Client-session upstream factory constructor | Directly call `odin_client_session_create_with_upstream_transport` with null precondition subcases and with a fake upstream factory. Each null-precondition subcase uses a live nonblocking socketpair `conn_fd` where that argument can still be supplied, initializes `*out` to a sentinel when `out` is non-null, and records whether `on_close` or the factory fires. Downstream-create failure subcase arms `odin_client_session_test_fail_next_downstream_transport_create(ENOMEM)` before a valid constructor call. Downstream-interest failure subcase on host macOS arms `odin_event_loop_test_fail_next_kqueue_change(loop, ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_ADD, ODIN_EVENT_READ, EIO)` before a valid constructor call; non-macOS execution skips only this subcase with `GTEST_SKIP()`. For the valid callback, late-local-bytes, valid NULL-callback, factory-failure, and post-factory failure subcases, let the constructor return `cs`, then arm `odin_client_session_test_fail_next_dial(cs, EDEADLK)` before sending any local CONNECT bytes. Valid callback subcase passes a sentinel `upstream_destroying` callback, uses a socketpair downstream, sends `CONNECT example.com:443 HTTP/1.1\r\n\r\n`, has the fake upstream transport capture writes and return a CONNECT_RESP OK, then drains the HTTP response. Late-local-bytes subcase duplicates the session-owned socket endpoint before constructor ownership transfer; the non-consuming deadlined probe uses `poll(dup_fd, POLLIN, timeout)` followed by `recv(dup_fd, buf, sizeof buf, MSG_PEEK)` and never uses a consuming read, sends the same complete CONNECT request with no parse tail, lets the fake upstream capture CONNECT_REQ, keeps CONNECT_RESP stalled, writes `late-local!` from the local peer, runs the event loop long enough for any still-armed downstream READ to fire, the same `poll` plus `MSG_PEEK` probe observes that `late-local!` is still queued on the duplicated endpoint, then releases CONNECT_RESP OK and drains HTTP 200 plus relay. Valid NULL-callback terminal subcase repeats the successful exchange with `upstream_destroying = NULL`. Factory-failure subcase returns `-1/EMFILE` after local HTTP parse OK. Post-factory failure subcase passes `upstream_destroying = NULL`, has the fake upstream factory return a transport, then forces `odin_connect_session_create_client` to fail with `ENOMEM` | Null preconditions return `-1/EINVAL`, preserve sentinel `*out` where present, leave `conn_fd` open for the caller, fire no `on_close`, call no factory, and create no upstream transport. Downstream-create failure returns `-1/ENOMEM`, preserves sentinel `*out`, leaves `conn_fd` open, fires no `on_close`, calls no factory, creates no downstream watch, and ASan observes no leaked client-session shell. Host-macOS downstream-interest failure returns `-1/EIO`, preserves sentinel `*out`, leaves `conn_fd` open, fires no `on_close`, calls no factory, destroys the already-created downstream fd transport before return, and ASan observes no leaked watch or session shell. In every dial-hook-armed subcase, the observed result is the factory-branch result rather than `on_close(EDEADLK)`, proving the factory constructor did not enter `start_dial` before success, late local bytes, factory failure, or post-factory failure. Valid callback subcase calls the factory exactly once after HTTP parse OK, sends byte-exact CONNECT_REQ bytes for `example.com:443` to the fake upstream transport, writes byte-exact `HTTP/1.1 200 Connection Established\r\n\r\n` downstream, reaches `on_close(0)` after EOF relay, and calls the sentinel `upstream_destroying` once before transport destroy. Late-local-bytes subcase keeps the fake upstream send log at exactly the CONNECT_REQ while CONNECT_RESP is stalled, writes no HTTP response downstream during the stall, and the deadlined duplicate-endpoint `poll` plus `MSG_PEEK` probe observes `late-local!` still queued; after CONNECT_RESP OK and HTTP 200, the fake upstream send log appends `late-local!` only through relay, proving the probe did not consume the bytes and downstream READ stayed disabled until relay startup. Valid NULL-callback terminal cleanup also reaches `on_close(0)`, destroys the fake upstream exactly once, records no destroying-callback call, and does not crash. Factory failure writes byte-exact `HTTP/1.1 400 Bad Request\r\n\r\n` downstream, fires `on_close(EMFILE)`, and leaves no live upstream transport. Post-factory NULL-callback failure writes byte-exact `HTTP/1.1 400 Bad Request\r\n\r\n` downstream, fires `on_close(ENOMEM)`, destroys the fake upstream exactly once, records no destroying-callback call, and does not crash | G3, G4, S2 | unit |
| T5 | Queued local connection completes pipeline after handshake | Install the §3.2.5 `fake-xquic fixture` and start runtime but do not fire `conn_handshake_finished`. Call `add_connection(rt, conn_fdA)` for a local socketpair and send one complete HTTP CONNECT request plus tail bytes on the local side. Then fire handshake finished, have fake `stream_create_with_direction` return one fake xqc stream whose send accepts all bytes and whose recv returns CONNECT_RESP OK plus server tail; use owner-thread pump until relay completes | `add_connection` returns `0` and runtime owns `conn_fdA` before handshake, but records no stream create until handshake fires. After handshake, exactly one `STREAM_CREATE_BIDI` is recorded with `XQC_STREAM_BIDI`; xqc stream user data is installed by `odin_xqc_stream_transport_create`; the stream send log begins with the CONNECT_REQ for the local target; local downstream reads the byte-exact HTTP 200 response followed by the server tail before relayed bytes; the fake stream receives the local HTTP parse tail before later relayed bytes; final relay EOF fires one session cleanup with no xqc stream close on the OK path | G3 | integration |
| T6 | Staged xqc write/read resumes without duplicating CONNECT bytes | Install the §3.2.5 `fake-xquic fixture`, start runtime, fire handshake, add one local connection, and use fake xqc stream send/recv queues. The first xqc send accepts only the first 7 CONNECT_REQ bytes, the next send returns `-XQC_EAGAIN`, and a later `stream_write_notify` accepts the remaining CONNECT_REQ bytes. The fake recv then returns the first 2 CONNECT_RESP bytes, `-XQC_EAGAIN`, and on a later `stream_read_notify` returns the remaining 2 RESP bytes | Before write notify, no HTTP 200 is written downstream and no relay starts. After write notify and read notify, the concatenated stream send prefix is exactly one CONNECT_REQ with no duplicate first 7 bytes, the decoded CONNECT_RESP OK produces byte-exact HTTP 200 downstream, and relay starts only after the completed response. The row fails if `s.write_off` resets after EAGAIN or if the response buffer drops the first 2 bytes | G3 | integration |
| T7 | No stream opens for local parse failure, local EOF before parse, or unusable runtime | Install the §3.2.5 `fake-xquic fixture` for runtime-backed subcases. Subcase A calls `add_connection(NULL, fd)` and `add_connection(rt_not_started, fd)`. Subcase B starts and then stops a runtime before calling `add_connection`. Subcase C starts/handshakes a runtime with `connA/cidA`, makes fake `udp_register_conn` return `-1/ENOMEM` for `cidB`, fires `conn_update_cid_notify(connA, cidA, cidB, xu)`, then calls `add_connection(rt, fd_closing)`. Subcase D starts/handshakes a valid runtime, adds one local connection, and sends `GET / HTTP/1.1\r\n\r\n`. Subcase E starts/handshakes a fresh valid runtime, adds one local connection, sends only `CONNECT example.com:443`, then closes the local peer before a complete request line or header terminator arrives | Subcases A and B return `-1/ENOTCONN` and the caller can still `fcntl(fd, F_GETFD) >= 0`, proving no fd ownership transfer. Subcase C records the failed `UDP_REGISTER_CONN(cidB)`, one `CONN_CLOSE(engine, cidA)`, one `UDP_UNREGISTER_CONN(cidA)`, no `STREAM_CREATE_BIDI`, and then `add_connection` returns `-1/ENOTCONN` with `fd_closing` still caller-owned. Subcase D writes byte-exact `HTTP/1.1 405 Method Not Allowed\r\nAllow: CONNECT\r\n\r\n` downstream, fires `on_close(EPROTO)`, records zero `STREAM_CREATE_BIDI` calls, and the fake xqc stream callback table is never entered. Subcase E writes no HTTP response, fires `on_close(ECONNRESET)`, records zero `STREAM_CREATE_BIDI` calls, leaves no live client session or pending fd, and the fake xqc stream callback table is never entered | G3, S2 | unit |
| T8 | Pending queue, stream, and session setup failures roll back | With the §3.2.5 `fake-xquic fixture` installed and a handshaken runtime, run five immediate local-connection subcases: `odin_xqc_client_runtime_test_fail_next_stream_context_alloc(rt, ENOMEM)` fails before session creation; `odin_client_session_test_fail_next_create(ENOMEM)` fails the synchronous `odin_client_session_create_with_upstream_transport` call before any stream factory can run; fake `stream_create_with_direction` returns `NULL` with `errno = EIO`; `odin_xqc_stream_transport_test_fail_next_create(ENOMEM)` makes stream transport creation fail after stream creation; `odin_connect_session_test_fail_next_create_client(ENOMEM)` fails after stream transport creation. In the connect-session-create subcase, save the just-created transport pointer and fire stale read/write/close/closing callbacks with that pointer after session close. Then start a fresh runtime but do not fire handshake, arm `odin_xqc_client_runtime_test_fail_next_pending_queue_append(rt, ENOMEM)`, and call `add_connection(rt, fd_pending_fail)`. Finally, start another fresh runtime, queue two fds before handshake, arm one of `odin_client_session_test_fail_next_create(ENOMEM)`, `odin_xqc_client_runtime_test_fail_next_stream_context_alloc(rt, ENOMEM)`, or the stream-create failure for the first queued fd, and fire handshake. After each failed immediate or queued subcase, run a fresh valid local connection through T5's happy setup | Immediate stream-context allocation failure and synchronous client-session constructor failure make `add_connection` return `-1`, create no client session, record no stream create, leave no live stream context, and leave the caller-owned fd open. Immediate stream-create, stream-transport-create, and connect-session-create failures occur after a client session exists, write byte-exact `HTTP/1.1 400 Bad Request\r\n\r\n` downstream, and fire session close with the triggering errno. Stream-create failure installs no xqc stream user data and records no stream close. Stream-transport-create failure records one `STREAM_CLOSE` for the just-created stream and no non-null stream-user-data install. Connect-session-create failure calls `runtime_client_session_upstream_destroying` before `odin_transport_destroy`, removes the transport-map entry, then destroys the xqc stream transport; the stream-user-data clear is recorded before `STREAM_CLOSE`, the saved stale callbacks return/no-op with no transport-helper forwarding, and ASan reports no use-after-free. Pending-queue append failure returns `-1/ENOMEM`, leaves `fd_pending_fail` open for the caller, records no queued fd, no client session, and no stream create, and a later pending append succeeds. In the queued conversion subcases, the first already-owned queued fd is closed and records no live stream context, the second queued fd still creates a stream and completes the CONNECT pipeline, and later valid connections still succeed | G3 | unit |
| T9 | Stream close, reset, null, missing-ALPN, and stale callbacks are scoped to one stream | Install the §3.2.5 `fake-xquic fixture` and start/handshake runtime and create one live local stream session stalled before CONNECT_RESP. Install fake `get_conn_alp_user_data_by_stream` to return `rt` for live stream callbacks and to record every call. Reset subcase fires `runtime_stream_closing_notify(stream_reset, XQC_ESTREAM_RESET, live_transport_reset)`, then creates a second valid stream and lets it complete. Live-close subcase creates a fresh live local stream session stalled before CONNECT_RESP, calls `runtime_stream_close_notify(stream_close, live_transport_close)`, then creates a later valid stream and lets it complete. Stale/null subcase fires `runtime_stream_close_notify(stream_done, NULL)`, `runtime_stream_read_notify(stream_done, NULL)`, `runtime_stream_write_notify(stream_done, NULL)`, `runtime_stream_closing_notify(stream_done, XQC_ESTREAM_RESET, NULL)`, `runtime_stream_close_notify(stream_done, stale_transport)`, `runtime_stream_read_notify(stream_done, stale_transport)`, `runtime_stream_write_notify(stream_done, stale_transport)`, `runtime_stream_closing_notify(stream_done, XQC_ESTREAM_RESET, stale_transport)`, and one read/write/close/closing set where the fake ALPN lookup returns `NULL` | Every stream callback records `ODIN_XQC_CLIENT_RUNTIME_TEST_CALL_GET_CONN_ALP_USER_DATA_BY_STREAM` before any live-map lookup or transport helper forwarding. The live reset/closing notify forwards through `odin_xqc_stream_transport_closing_notify`, the reset session closes its local downstream, records the stream-user-data clear before exactly one `STREAM_CLOSE` for that stream, keeps the connection CID registered, and lets the second stream complete normally. The live `runtime_stream_close_notify` returns `XQC_OK`, destroys only that client session/context, clears that stream user-data slot, records no CID unregister, and the later valid stream still records one `STREAM_CREATE_BIDI` and completes normally. Null, missing-ALPN, and stale read/write/close callbacks each return `XQC_OK`; null, stale, and missing-ALPN closing callbacks are no-ops; these subcases record no transport helper forwarding, no extra session destroy, no extra stream close, no CID unregister, and no AddressSanitizer use-after-free in the ASan gate | G3 | unit |
| T10 | Connection close before handshake cleans up pending queue | Install the §3.2.5 `fake-xquic fixture` and start runtime, call `add_connection` twice before handshake so both fds remain pending, do not fire `conn_handshake_finished`, then directly fire `conn_close_notify(connA, cidA, xu, rt)` | Both pending fds reach EOF or `EBADF` through deadlined probes; no `STREAM_CREATE_BIDI` or client session is recorded; `UDP_UNREGISTER_CONN(current_cid)` is recorded exactly once; the runtime has no pending fds or registered CID after the callback; destroy after close records ALPN unregister and UDP destroy once, with no second CID unregister and no stream close | G2, G3 | integration |
| T11 | Connection close after handshake cleans up live sessions | Install the §3.2.5 `fake-xquic fixture` and start runtime, fire handshake, add two local connections, and create two live stream sessions stalled before CONNECT_RESP; then directly fire `conn_close_notify(connA, cidA, xu, rt)` | Both live session downstream fds reach EOF or `EBADF` through deadlined probes; both xqc transports clear stream user data before their contexts are removed; `UDP_UNREGISTER_CONN(current_cid)` is recorded exactly once; the runtime has no live sessions, pending fds, or registered CID after the callback; destroy after close records ALPN unregister and UDP destroy once, with no second CID unregister or stream close | G2, G3 | integration |
| T12 | TCP client path and CLI/client-runtime scope are preserved | Run a TCP client-session loopback using `odin_client_session_create` with a fake TCP Odin server that returns CONNECT_RESP OK, read the client-runtime test record before and after the TCP run, then run the scope-check action `odin_client_xqc_runtime_scope_check` | The TCP loopback still sends CONNECT_REQ over the accepted TCP upstream fd, receives byte-exact HTTP 200 downstream, relays bytes both ways, and appends zero client-xqc-runtime records. Scope check reports no `xquic`, `client_xqc_runtime`, or `transport_xqc` reference in `odin/cli_client.c`, `odin/cli_client.h`, or the `odin_cli_client` GN target. The same action strips comments/string literals, tokenizes `odin/client_xqc_runtime.c`, `odin/client_xqc_runtime.h`, and the `source_set("odin_client_xqc_runtime")` GN body, allows the exact `xqc_connect` token, and reports no forbidden `odin_dial_start`, raw `connect`, `open`, `exec*`, or `dlopen` token in those client-runtime sources or target. The action also extracts the `odin_client_session_create_with_upstream_transport` body plus the shared parse-to-upstream `FACTORY` branch in `odin/client_session.c`, tokenizes that implementation region, and reports no forbidden `start_dial`, `odin_dial_start`, raw `connect`, `open`, `exec*`, or `dlopen` token there while allowing `odin_connect_session_create_client` and transport-factory symbols | G4, S2 | integration |
| T13 | Runtime destroy before handshake closes owned pending fds after requesting connection close | Install the §3.2.5 `fake-xquic fixture` and start runtime, queue two local fds before handshake, then call `odin_xqc_client_runtime_destroy(rt)`. Assert intermediate records before firing close notification, then explicitly fire `conn_close_notify(connA, cidA, xu, rt)` | Destroy closes both queued local fds, records no stream create, and records one `CONN_CLOSE(engine, cidA)` before any CID unregister, ALPN unregister, UDP destroy, or runtime free. After the explicit close notification, the runtime unregisters `cidA` once, unregisters ALPN once, destroys UDP once, and frees the runtime; ASan reports no double-free or use-after-free | G2, G3 | integration |
| T14 | Runtime destroy after handshake closes live sessions after requesting connection close | Install the §3.2.5 `fake-xquic fixture` and start runtime, fire handshake, create two live stream sessions, then call `odin_xqc_client_runtime_destroy(rt)` before the sessions complete. Assert intermediate records before firing close notification, then explicitly fire `conn_close_notify(connA, cidA, xu, rt)` | Destroy destroys both client sessions, clears both xqc stream user-data slots, records each stream-user-data clear before that stream's `STREAM_CLOSE`, records one `STREAM_CLOSE` per non-OK stream, and records one `CONN_CLOSE(engine, current_cid)` before any CID unregister, ALPN unregister, UDP destroy, or runtime free. After the explicit close notification, the runtime unregisters the current CID once, unregisters ALPN once, destroys UDP once, and frees the runtime; ASan reports no double-free or use-after-free | G2, G3 | integration |
| T15 | Production xquic client connects to Odin QUIC server and relays over a real bidirectional stream | Reset client-runtime and server-runtime records, call `odin_xqc_client_runtime_test_set_ops(NULL)`, `odin_xqc_server_runtime_test_set_ops(NULL)`, and `odin_xqc_udp_test_set_ops(NULL)`, and install no fake replacement ops for client-runtime, server-runtime, or UDP calls. Because `//odin/testing:odin_unittests` defines `ODIN_TRANSPORT_XQC_TESTING`, install an `odin_xqc_stream_transport_test_ops_t` pass-through table whose `recv`, `send`, and `set_user_data` callbacks call `xqc_stream_recv`, `xqc_stream_send`, and `xqc_stream_set_user_data` directly. Start a deadline-protected TCP origin on `127.0.0.1:0` that accepts one connection, reads a 12-byte client tail, writes a 12-byte server tail, and closes. In the owner-thread event loop, create and start a production `odin_xqc_server_runtime` bound to `127.0.0.1:0` with `odin/testing/certs/odin_quic_test.crt` and `odin/testing/certs/odin_quic_test.key`, set a production dial filter that allows only the origin sockaddr, and read the server runtime's bound UDP address with `odin_xqc_server_runtime_local_addr`. Create and start a production `odin_xqc_client_runtime` bound to `127.0.0.1:0` with `peer_addr` set to the server UDP address, `server_host = "localhost"`, `conn_ssl_config.cert_verify_flag` set to the bitwise OR of `XQC_TLS_CERT_FLAG_NEED_VERIFY` and `XQC_TLS_CERT_FLAG_ALLOW_SELF_SIGNED`, and an `xqc_transport_callbacks_t` whose only non-null slot is `cert_verify_cb` pointing to an `xqc_cert_verify_pt` function that increments a test counter and returns `XQC_OK`. Pump the event loop with a watchdog until client handshake completes, add one local HTTPS_PROXY socket fd, write `CONNECT 127.0.0.1:<origin-port> HTTP/1.1\r\n\r\nclient-tail!`, and continue pumping until both tails are observed or a deadline expires | The client runtime records one production UDP create whose `last_udp_create.engine_type == XQC_ENGINE_CLIENT`, whose `last_udp_create.app_user_data` is the client runtime, and whose `last_udp_create.transport_callbacks_value` has the caller `cert_verify_cb` plus non-null runtime no-op `save_token`, `save_session_cb`, and `save_tp_cb`; it also records one production `XQC_CONNECT` call whose ALPN is byte-exact `odin/1`. The server runtime records one production `ODIN_XQC_SERVER_ALPN` registration, the client cert callback counter is nonzero, no fake-op replacement is installed for xquic or UDP calls, and the xqc-stream test ops are pass-through shims into production `xqc_stream_recv`, `xqc_stream_send`, and `xqc_stream_set_user_data`. The local HTTPS_PROXY peer reads byte-exact `HTTP/1.1 200 Connection Established\r\n\r\n` followed by `server-tail!`; the TCP origin reads byte-exact `client-tail!`, proving relay bytes crossed an actual client-created `XQC_STREAM_BIDI` stream into `odin_xqc_server_runtime` and through the existing server CONNECT pipeline using production stream I/O. The server dial filter records that only the allowed origin sockaddr was permitted. Cleanup stops/destroys both runtimes and the origin child/thread within deadlines with no leaked live sessions or registered CIDs | G1, G2, G3, S1, S2 | integration |
| T16 | Queued pre-handshake connections survive stop/restart | Install the §3.2.5 `fake-xquic fixture` and start runtime, do not fire `conn_handshake_finished`, call `add_connection` for two local socketpair fds, and send complete HTTP CONNECT requests with distinct targets and tail bytes from the local peers. Call `stop`, use deadlined local-peer probes plus client-runtime records to assert no queued fd was closed and no session, stream context, `STREAM_CREATE_BIDI`, `STREAM_CLOSE`, CID unregister, or `CONN_CLOSE` occurred while stopped. Call `start` again, then fire `conn_handshake_finished`; fake stream creation returns two streams whose send accepts each CONNECT_REQ and whose recv returns CONNECT_RESP OK plus distinct server tails. Pump the owner-thread loop until both relays complete | Both pre-stop `add_connection` calls return `0` and the runtime owns the fds, but no stream or session exists before the restart handshake. `stop` records exactly one `UDP_STOP`, preserves the active CID registration and pending queue, and does not close either queued fd. Restart records one additional UDP start and no second `xqc_connect`; after handshake, the runtime converts the queued fds in FIFO order, records exactly two `STREAM_CREATE_BIDI` calls with `XQC_STREAM_BIDI`, sends the two CONNECT_REQ frames in the same order as the original `add_connection` calls, delivers each local peer byte-exact `HTTP/1.1 200 Connection Established\r\n\r\n` followed by its matching server tail, and cleans up both sessions normally without any stop-triggered stream close or CID unregister | G2, G3 | integration |
| T17 | Live post-handshake session survives stop/restart | Install the §3.2.5 `fake-xquic fixture` and start runtime, fire handshake, add one local HTTPS_PROXY socket fd, send `CONNECT example.com:443 HTTP/1.1\r\n\r\nclient-tail`, and let the fake stream accept the complete CONNECT_REQ while its recv queue returns `-XQC_EAGAIN` so the client session remains live waiting for CONNECT_RESP. Call `stop`, assert through records and deadlined local-peer probes that no client-session close, xqc stream user-data clear, `STREAM_CLOSE`, CID unregister, or `CONN_CLOSE` occurs while stopped. Call `start` again, make the fake recv queue return CONNECT_RESP OK plus `server-tail`, fire `stream_read_notify`, and pump the owner-thread loop until relay completion | The immediate post-handshake `add_connection` creates exactly one live client session and one `XQC_STREAM_BIDI` stream before `stop`. `stop` records exactly one `UDP_STOP`, keeps `connect_started`, `handshake_done`, the active CID registration, the stream user-data slot, and the live session intact, and records no cleanup callbacks. Restart records one additional UDP start and no second `xqc_connect`; after the read notify, the same live session writes byte-exact `HTTP/1.1 200 Connection Established\r\n\r\n` followed by `server-tail` downstream and relays `client-tail` upstream before normal session cleanup, proving stop was only a UDP-driver pause | G2, G3 | integration |
| T18 | Stale or closing handshake-finished callbacks do not convert queued fds | Install the §3.2.5 `fake-xquic fixture`. Current-connection subcase starts runtime with `connA/cidA`, queues one local socketpair fd before handshake, writes a complete CONNECT request plus tail, then fires `conn_handshake_finished(connZ, xu, rt)` for a stale xquic pointer and `conn_handshake_finished(connA, xu, NULL)` for the current pointer with stale proto data. Use deadlined local-peer probes and records to assert the queued fd remains open and no stream/session exists. Then fire `conn_handshake_finished(connA, xu, rt)` and let a fake stream complete the CONNECT_RESP OK path. Closing subcase starts a fresh runtime, queues one fd, forces a CID-update registration failure to set `rt.closing` and destroy queued fds, then fires `conn_handshake_finished(connA, xu, rt)` | Stale pointer and stale-proto handshake callbacks return without setting `handshake_done`, popping the pending queue, closing the queued fd, or recording `STREAM_CREATE_BIDI`. The later valid current handshake converts the same queued fd exactly once, records one `STREAM_CREATE_BIDI` with `XQC_STREAM_BIDI`, sends the CONNECT_REQ after the valid handshake only, and delivers byte-exact `HTTP/1.1 200 Connection Established\r\n\r\n` plus the fake server tail downstream. In the closing subcase, the close-on-CID-update-failure path destroys the queued fd and marks the runtime closing; the subsequent current-pointer handshake records no stream/session creation, no pending-queue drain, no second fd close, and no change to the closing state | G2, G3 | integration |
| T19 | Stopped runtime destroy before handshake force-cleans owned pending fds | Install the §3.2.5 `fake-xquic fixture` and start runtime with `connA/cidA`, queue two local fds before handshake, call `stop`, assert both queued fds remain open while stopped, then call `odin_xqc_client_runtime_destroy(rt)` without restarting and without directly firing `conn_close_notify` after destroy. Repeat with the existing xqc-UDP `engine_destroy` test hook invoking the registered `conn_close_notify(connA, cidA, xu, rt)` from inside `odin_xqc_udp_destroy` | `stop` records exactly one `UDP_STOP`, preserves `connect_started`, the active CID registration, and both pending fds. Destroy while stopped closes both queued local fds, records no stream/session creation, records no second `UDP_STOP`, records `CONN_SET_ALP_USER_DATA(connA, NULL)` before `UDP_DESTROY`, records exactly one `UDP_UNREGISTER_CONN(cidA)` before `UDP_DESTROY`, records no `CONN_CLOSE`, unregisters ALPN once, destroys UDP once, frees the runtime before `odin_xqc_client_runtime_destroy` returns, and ASan reports no leaked fd node, double-free, or use-after-free. In the engine-destroy callback subcase, the in-destroy close notification is cleanup-only: it records no extra `CONN_CLOSE`, no second CID unregister, no second ALPN unregister, no second UDP destroy, and no recursive runtime free | G2, G3 | integration |
| T20 | Stopped runtime destroy after handshake force-cleans live sessions | Install the §3.2.5 `fake-xquic fixture` and start runtime with `connA/cidA`, fire handshake, create two live stream sessions stalled before CONNECT_RESP, call `stop`, assert both sessions, stream user-data slots, and the active CID remain live while stopped, then call `odin_xqc_client_runtime_destroy(rt)` without restarting and without directly firing `conn_close_notify` after destroy. Repeat with the existing xqc-UDP `engine_destroy` test hook invoking the registered `conn_close_notify(connA, cidA, xu, rt)` from inside `odin_xqc_udp_destroy` | `stop` records exactly one `UDP_STOP`, preserves `connect_started`, `handshake_done`, the active CID registration, both live sessions, and both stream user-data slots. Destroy while stopped destroys both client sessions, clears both xqc stream user-data slots, records each stream-user-data clear before that stream's `STREAM_CLOSE`, records one `STREAM_CLOSE` per non-OK stream, records `CONN_SET_ALP_USER_DATA(connA, NULL)` before `UDP_DESTROY`, records exactly one `UDP_UNREGISTER_CONN(cidA)` before `UDP_DESTROY`, records no `CONN_CLOSE`, unregisters ALPN once, destroys UDP once, frees the runtime before `odin_xqc_client_runtime_destroy` returns, and ASan reports no leaked session, double-free, or use-after-free. In the engine-destroy callback subcase, the in-destroy close notification is cleanup-only: it records no extra stream close, no stale transport-helper forwarding, no second CID unregister, no second ALPN unregister, no second UDP destroy, and no recursive runtime free | G2, G3 | integration |
| T21 | Duplicate create notification is side-effect-free while connected or closing | Install the §3.2.5 `fake-xquic fixture`. Already-connected subcase starts runtime with fake `xqc_connect` invoking `conn_create_notify(connA, cidA, xu, NULL)` successfully and returning `cidA`. Call `odin_xqc_client_runtime_test_state(rt, &before)` and assert `before.conn == connA`, `before.current_cid` matches `cidA` byte-for-byte, `before.cid_registered == 1`, and `before.handshake_done == 0`; snapshot the call record, then directly invoke the registered `conn_create_notify(connB, cidB, xu, NULL)`. Call `odin_xqc_client_runtime_test_state(rt, &after)` before any cleanup. Destroy the runtime and fire `conn_close_notify(connA, cidA, xu, rt)`. Closing subcase starts a fresh runtime with `connC/cidC`, calls `odin_xqc_client_runtime_destroy(rt)` while UDP is still running and before firing close notification, calls `odin_xqc_client_runtime_test_state(rt, &closing_before)` and asserts `closing_before.conn == connC`, `closing_before.current_cid` matches `cidC` byte-for-byte, `closing_before.cid_registered == 1`, and `closing_before.closing == 1`, then directly invokes `conn_create_notify(connD, cidD, xu, NULL)`, calls `odin_xqc_client_runtime_test_state(rt, &closing_after)`, and finally fires `conn_close_notify(connC, cidC, xu, rt)` | In both subcases the second create notification returns `-1`, the after snapshot records `connect_errno == EALREADY`, records no `UDP_REGISTER_CONN` for `cidB` or `cidD`, records no ALPN user-data set for `connB` or `connD`, and the after snapshot preserves the before snapshot's `conn`, byte-for-byte `current_cid`, `cid_registered`, `handshake_done`, and `closing` values. The rejected create notification records no `CONN_CLOSE` or `UDP_UNREGISTER_CONN` side effect. The already-connected subcase later destroys and closes only `connA/cidA`, with one `CONN_CLOSE(engine, cidA)`, one `UDP_UNREGISTER_CONN(cidA)`, one ALPN unregister, and one UDP destroy. The closing subcase keeps the destroy-initiated `CONN_CLOSE(engine, cidC)` as the only connection close, and the final close notification unregisters `cidC`, unregisters ALPN, destroys UDP, and frees the runtime exactly once | G2 | unit |

## 6. Implementation Plan

- **P1. Land client QUIC skeleton and red-verifiable `T1`-`T21`.**
  - **Scope:** add the `odin/client_session.h` declarations from section 3.2.1 and a linkable `odin_client_session_create_with_upstream_transport` skeleton that validates null preconditions but does not call the upstream factory after HTTP parse; keep `odin_client_session_create` and `odin/cli_client.c` building. Add `odin_client_session_test_fail_next_downstream_transport_create` to `odin/testing/client_session_internal_test.h` under `ODIN_CLIENT_SESSION_TESTING`; add `odin_connect_session_test_fail_next_create_client` to `odin/testing/connect_session_internal_test.h` and the gated entry branch in `odin/connect_session.c` under `ODIN_CONNECT_SESSION_TESTING`. Add `odin/client_xqc_runtime.c` and `odin/client_xqc_runtime.h` with the section 3.2.2 public API, `ODIN_XQC_CLIENT_ALPN`, and a bounded skeleton that allocates a runtime shell but intentionally omits real UDP creation, ALPN registration, caller transport callback copying, no-op defaulting for missing client cache callbacks, client cert-verify callback validation, non-null `engine_ssl_config` validation, token-cap validation, copied config validation/deep-copying, safe AF_INET/AF_INET6 peer-address validation, `xqc_connect`, CID bookkeeping, start/stop state, pending local fd queueing, current-connection handshake-finished gating, stream creation, xqc stream transport creation, stream callback forwarding, and runtime destroy cleanup. Add `odin/testing/client_xqc_runtime_internal_test.h` with the fake-op record, §3.2.5 fake-xquic fixture, dedicated UDP-create record, runtime-state snapshot helper, config-copy allocation hook, stream-context allocation hook, and pending-queue append hook; add `odin/testing/client_xqc_runtime_testing.c` and `odin/testing/client_xqc_runtime_unittests.cpp` with T1-T21 gated by `ODIN_XQC_CLIENT_RED=1`; add the new `ODIN_XQC_CLIENT_RUNTIME_TESTING` config to `//odin/testing:odin_unittests` together with existing event-loop, xqc-UDP, xqc-transport, client-session, connect-session, and server-xqc-runtime testing configs. Add `odin/check_client_xqc_runtime_scope.py` and `odin_client_xqc_runtime_scope_check` in a P1 red form that scans `odin/cli_client.c`, `odin/cli_client.h`, the `odin_cli_client` GN target, `odin/client_xqc_runtime.c`, `odin/client_xqc_runtime.h`, the `odin_client_xqc_runtime` GN target, and the `odin/client_session.c` factory-constructor region but exits nonzero after the scan with `pending P2 scope enforcement`; the default build does not depend on that action. Add `source_set("odin_client_xqc_runtime")` depending on `:odin_client_session`, `:odin_event_loop`, `:odin_transport_xqc`, `:odin_xqc_udp`, and `//xquic`; add it to the aggregate `:odin` target, but do not wire it into `odin/cli_client.c`.
  - **Depends on:** None.
  - **Done when:** `./tool/gn gen out/xqc_client_mac --args='target_os="mac"'`, `./tool/gn gen out/xqc_client_mac_arm64 --args='target_os="mac" target_cpu="arm64"'`, `./tool/gn gen out/xqc_client_linux_x64 --args='target_os="linux" target_cpu="x64"'`, `./tool/gn gen out/xqc_client_ios_sim --args='target_os="ios" target_environment="simulator" target_cpu="arm64"'`, and `./tool/gn gen out/xqc_client_ios_device --args='target_os="ios" target_environment="device" target_cpu="arm64"'` resolve. `./tool/ninja -C out/xqc_client_mac odin_main odin_unittests tests` builds, and matching `odin_main` and `odin_unittests` targets build for the four cross-compile output directories. The red-verification command `ODIN_XQC_CLIENT_RED=1 out/xqc_client_mac/odin_unittests --gtest_filter='OdinXqcClientRuntimeTest.*:OdinClientSessionUpstreamTransportTest.*:OdinXqcClientRuntimeScopeTest.*'` executes T1-T21 and fails them against the skeleton: T1-T3 because no UDP driver, ALPN, dedicated UDP-create record population, safe copied start config including borrowed `loop`, `peer_addr_storage`, `peer_addrlen`, `server_host`, token, nested `conn_ssl_config` buffers, and `no_crypto_flag`, caller transport callback copy/preservation, no-op defaulting for `save_token` / `save_session_cb` / `save_tp_cb`, client cert-verify callback validation, `engine_ssl_config` validation, token-cap validation, copied-config allocation rollback and hook-boundary behavior, no-connection destroy cleanup, AF_INET/AF_INET6 peer validation, UDP-start rollback, xqc connect, start/stop/restart state, CID, update, CID-update failure live-stream cleanup, or close lifecycle exists; T4 because the skeleton constructor never calls the upstream factory, never calls the pre-destroy unlink callback, never exercises downstream setup rollback, post-parse downstream READ pause, optional `upstream_destroying == NULL` terminal cleanup, or post-factory failure cleanup, and never writes CONNECT_REQ; T5-T6 and T15-T20 because queued, handshaken, production-loopback, stop-preserved, stopped-destroy, and stale-handshake-gated local connections never create streams or run the CONNECT pipeline, and T15's real `cert_verify_cb` invocation plus null-cache-callback production config cannot be safe without the client runtime passing a defaulted transport callback table to xquic; T7 because closing-runtime rejection, local parse failure, local EOF before parse, and ownership outcomes are not wired; T8-T11, T13, T14, T19, and T20 because pending queue append failure, stream-context allocation failure, synchronous client-session constructor failure, setup-failure, live close/reset and stale transport callbacks, stream callback, connection close, pending queue, and destroy cleanup branches are absent; T16-T20 additionally fail because the skeleton has no queued-fd, live-session, stopped-destroy, or current-connection handshake state to preserve or clean up across stop/restart, destroy-while-stopped, or stale callbacks; T21 because the skeleton has no side-effect-free already-connected or closing `conn_create_notify` rejection whose `EALREADY` and private-state preservation are visible through `odin_xqc_client_runtime_test_state`, and no later cleanup of the preserved original connection/CID. T12 runs the TCP loopback and zero-client-runtime-record assertions before invoking the P1 scope-check action; that action scans the CLI files/target, client-runtime files/target, and `odin/client_session.c` factory-constructor region, then exits nonzero with `pending P2 scope enforcement`, so the row's `exit_status == 0` assertion fails inside the test instead of failing at Ninja target resolution. The default host run `out/xqc_client_mac/odin_unittests --gtest_brief=1` reports T1-T21 skipped and exits zero with all pre-existing Odin tests green. Host-runnable enumeration: T1-T21 red assertions run only in `out/xqc_client_mac/odin_unittests` on the host macOS architecture; the T2 and T4 kqueue-change subcases are guarded with `#if defined(__APPLE__)` and skipped with `GTEST_SKIP()` on non-macOS executions. Cross-compile-only enumeration: `out/xqc_client_mac_arm64/odin_unittests`, `out/xqc_client_linux_x64/odin_unittests`, `out/xqc_client_ios_sim/odin_unittests`, and `out/xqc_client_ios_device/odin_unittests` are built but not executed; their xquic, UDP, socket, and event-loop branches are compile-verified only in P1.
    The T12 P1 red action performs the same CLI, client-runtime, and `odin/client_session.c` factory-region token checks described in T12 but still fails with the intentional `pending P2 scope enforcement` status.
    T2's red assertions also include the valid `engine_config = NULL` create/start path and the `engine_config->sendmmsg_on = 1` `ENOTSUP` path; both fail in P1 because the skeleton has no real UDP-create call-through, no ALPN side-effect checks, and no copied-config cleanup behind that inherited UDP rejection.

- **P2. Implement the client-session upstream factory constructor and TCP scope check; turn T4 and T12 green.**
  - **Scope:** replace the section 3.2.1 skeleton with the real `odin_client_session_create_with_upstream_transport` path: the existing `odin_client_session_test_fail_next_create` first-branch hook under `ODIN_CLIENT_SESSION_TESTING`, the new `odin_client_session_test_fail_next_downstream_transport_create` branch immediately before downstream fd transport creation, downstream fd transport creation rollback, initial downstream `ODIN_TRANSPORT_READ` interest rollback through the existing event-loop kqueue hook, HTTP parse reuse, immediate `odin_transport_set_interest(cs->downstream_t, 0)` on `ODIN_HTTP_OK` before factory or CONNECT-session work, one post-parse factory call that bypasses the existing `start_dial` path even when `odin_client_session_test_fail_next_dial` is armed on the returned session, factory failure mapped through the exact HTTP 400 failure path, `odin_connect_session_create_client`, pre-destroy `upstream_destroying` callback invocation before destroying a factory-created upstream, the `upstream_destroying == NULL` skip path for terminal and post-factory-abort cleanup, and upstream interest arming after `cs.state` and `cs.s` are set while downstream READ stays disabled until the existing HTTP 200 / relay transition owns it. Preserve the existing `odin_client_session_create` fd-dial code path, `odin_client_session_set_dial_filter`, and `odin/cli_client.c` call site. Complete `odin/check_client_xqc_runtime_scope.py` and `odin_client_xqc_runtime_scope_check` by removing the P1 `pending P2 scope enforcement` failure branch; the script scans `odin/cli_client.c`, `odin/cli_client.h`, and the `odin_cli_client` GN target for `xquic`, `client_xqc_runtime`, or `transport_xqc`, tokenizes `odin/client_xqc_runtime.c`, `odin/client_xqc_runtime.h`, and `source_set("odin_client_xqc_runtime")` to reject `odin_dial_start`, raw `connect`, `open`, `exec*`, and `dlopen` while allowing `xqc_connect`, and tokenizes the `odin/client_session.c` factory-constructor body plus FACTORY branch to reject `start_dial`, `odin_dial_start`, raw `connect`, `open`, `exec*`, and `dlopen` while allowing `odin_connect_session_create_client`. Remove the red gate from T4 and T12 while leaving runtime rows T1-T3, T5-T11, and T13-T21 gated.
  - **Depends on:** P1.
  - **Done when:** the P1 build commands still succeed. `out/xqc_client_mac/odin_unittests --gtest_filter='OdinClientSessionUpstreamTransportTest.*:OdinXqcClientRuntimeScopeTest.*'` passes T4 and T12 un-gated on the host macOS architecture, with T4 covering null precondition fd ownership/no-callback behavior, downstream fd transport creation failure, host-macOS initial READ-interest failure, downstream READ pause immediately after `ODIN_HTTP_OK` by writing `late-local!` while CONNECT_RESP is stalled, proving it is still queued with `poll` plus `recv(..., MSG_PEEK)`, and proving the same bytes are forwarded only after HTTP 200 and relay startup, `odin_client_session_test_fail_next_dial` armed after constructor creation with outcomes proving it is not consumed across valid factory success, late local bytes, factory failure, and post-factory failure, non-null `upstream_destroying`, valid `upstream_destroying == NULL` terminal cleanup, and `upstream_destroying == NULL` cleanup after post-factory `odin_connect_session_create_client` failure. `./tool/ninja -C out/xqc_client_mac odin:odin_client_xqc_runtime_scope_check` passes and reports no forbidden CLI token in `odin/cli_client.c`, `odin/cli_client.h`, or the `odin_cli_client` target, no forbidden local-resource API token in `odin/client_xqc_runtime.c`, `odin/client_xqc_runtime.h`, or the `odin_client_xqc_runtime` target, and no forbidden local-resource API token in the `odin/client_session.c` factory-constructor body or FACTORY branch. The red-verification command `ODIN_XQC_CLIENT_RED=1 out/xqc_client_mac/odin_unittests --gtest_filter='OdinXqcClientRuntimeTest.*'` still executes T1-T3, T5-T11, and T13-T21 and reports them red against the unimplemented runtime, including the complete fake-xquic fixture for fake-object rows, the dedicated UDP-create record, transport callback copy, no-op defaulting for missing client cache callbacks, cert-verify callback validation, `engine_ssl_config` validation, token-cap validation, copied config for borrowed `loop`, `peer_addr_storage`, `peer_addrlen`, `server_host`, token, and nested `conn_ssl_config` buffers, all four copied-config allocation rollback branches plus invalid and unreachable hook sites, AF_INET/AF_INET6 peer-validation, no-live-connection destroy cleanup, UDP-start rollback, all `xqc_connect == NULL` rollback branches, stop/restart, stopped-destroy force cleanup, closing-runtime `add_connection` rejection and live-stream abort cleanup through CID-update registration failure, local EOF before parse with zero stream creation, pending-append failure, live close/reset and stale transport callback handling, runtime-owned queued-fd cleanup, queued/live work preservation across stop, stale/closing handshake gating, duplicate create-notify rejection with `odin_xqc_client_runtime_test_state` proving `EALREADY` plus unchanged `conn`, `current_cid`, `cid_registered`, `handshake_done`, and `closing`, and production loopback handshake assertions. The unfiltered default host run exits zero with those runtime rows skipped. Host-runnable enumeration: T4 and T12 run in `out/xqc_client_mac/odin_unittests`; T4's downstream-interest subcase is host-macOS guarded, and T4's late-local-bytes subcase runs on every host execution with a deadline-protected `poll` plus `MSG_PEEK` probe; T12 also runs the host scope-check action. Cross-compile-only enumeration: the four non-host `odin_unittests` binaries and `odin_client_xqc_runtime_scope_check` targets are built but not executed; their alternate socket and event-loop branches are verified by cross-compile and code review only in P2.
    The remaining red T2 runtime assertions explicitly include `engine_config = NULL` acceptance through the UDP-create wrapper and `engine_config->sendmmsg_on = 1` rollback with `ENOTSUP`, no ALPN registration, no UDP destroy, no `xqc_connect`, and copied-config cleanup.

- **P3. Implement the client QUIC runtime and turn T1-T3, T5-T11, and T13-T21 green.**
  - **Scope:** implement `odin/client_xqc_runtime.c` per section 3.2.2-section 3.2.5: copied start-time config including borrowed `loop` storage, `server_host`, token, `peer_addr_storage`, `peer_addrlen`, `no_crypto_flag`, and nested `conn_ssl_config` buffers, non-null `engine_ssl_config` validation, token cap validation, caller `xqc_transport_callbacks_t` subset copy, runtime-owned no-op defaults for missing `save_token`, `save_session_cb`, and `save_tp_cb`, caller preservation for those cache callbacks when supplied, `cert_verify_cb` validation and preservation, AF_INET/AF_INET6 `peer_addrlen` validation capped at `sizeof(struct sockaddr_in6)`, `odin_xqc_udp_create` with `XQC_ENGINE_CLIENT`, dedicated UDP-create test record population, runtime-owned ALPN callback registration, first-start and restart UDP-start rollback, start/stop idempotency, stopped `add_connection` rejection, restart without a second `xqc_connect`, stop-preserved pending fds and live sessions, valid destroy before start and failed-start destroy with no live xquic connection, stopped force cleanup without restart or post-destroy close-notify injection, `xqc_connect` with `ODIN_XQC_CLIENT_ALPN` and `(const struct sockaddr *)&rt->peer_addr_storage`, no-callback and post-`conn_create_notify` `xqc_connect == NULL` rollback including close-notify-before-return startup rollback that leaves `rt.closing` clear for retry, create-time CID registration through `conn_create_notify`, already-connected or closing `conn_create_notify` rejection with `EALREADY` and no CID/ALPN/current-connection mutation observable through `odin_xqc_client_runtime_test_state`, pending queue append failure before fd ownership transfer, stream-context allocation failure before `calloc` and immediate fd ownership transfer, current non-closing handshake-finished gating before draining queued local fds, per-fd queued-conversion failure cleanup, immediate and queued `add_connection` ownership rules including synchronous client-session constructor failure, CID update register-before-unregister, close-on-new-CID-registration-failure with runtime-driven live-stream cleanup, connection close cleanup before and after handshake, runtime destroy cleanup for queued and live sessions with running destroy deferred through connection close and stopped destroy force-inerting xquic state before UDP destroy, one bidirectional xqc stream per successfully parsed local CONNECT request, `odin_xqc_stream_transport_create` wrapping, pre-destroy transport-map unlink for client-session-owned xqc transports, stream-user-data clear before non-OK `xqc_stream_close` on `runtime_client_session_on_close`, CID-update registration-failure abort, and runtime destroy paths, stream read/write/close/closing callback recovery through `xqc_get_conn_alp_user_data_by_stream`, `runtime_client_session_on_close` recovery of `rt` before transport-map unlink, NULL/stale/missing-ALPN read/write/close `XQC_OK` returns, NULL/stale/missing-ALPN closing no-ops, production loopback client/server xquic handshake coverage with caller cert callback invocation and server dial-filter enforcement, and the section 3.2.5 test wrappers, including the fake-xquic fixture and runtime-state snapshot helper. Remove the remaining `ODIN_XQC_CLIENT_RED` gates.
    This phase leaves `engine_config` optional, passes `NULL` through to xquic defaults, and does not pre-reject `sendmmsg_on`; the inherited `odin_xqc_udp_create` `ENOTSUP` branch remains the tested rejection point.
  - **Depends on:** P2.
  - **Done when:** the P1 build commands still succeed. `out/xqc_client_mac/odin_unittests --gtest_filter='OdinXqcClientRuntimeTest.*:OdinClientSessionUpstreamTransportTest.*:OdinXqcClientRuntimeScopeTest.*'` passes T1-T21 un-gated on the host macOS architecture, and the unfiltered host run `out/xqc_client_mac/odin_unittests --gtest_brief=1` exits zero with all pre-existing Odin suites green.
    Host-runnable enumeration: T1-T21 all run in the host `odin_unittests`; T5, T6, T10, T11, and T13-T20 use owner-thread event-loop pumps and deadline-protected local sockets. T1, T2, and T15 assert `last_udp_create` exposes `XQC_ENGINE_CLIENT`, the loop/local address/config pointers, `app_user_data`, and a transport callback table value with non-null `save_token`, `save_session_cb`, and `save_tp_cb` even when the caller supplied none of those slots. T2 also asserts caller-supplied `save_token`, `save_session_cb`, and `save_tp_cb` pointers are preserved, defaulted no-op callbacks preserve `errno`, null `engine_ssl_config` is rejected before UDP create, token lengths 256 and 257 hit the allow/reject boundary, invalid nested `conn_ssl_config` pointer/length pairs are rejected before UDP create, zero-ticket caller-null `session_ticket_data` produces `session_ticket_len == 0` plus a non-null long-lived ticket pointer at fake `xqc_connect`, all four config-copy allocation failure sites (`server_host`, token, `session_ticket_data`, and `transport_parameter_data`) preserve `ENOMEM`, leave `*out` untouched, record no UDP create, and free earlier copied config under ASan, invalid config-copy allocation sites return `-1/EINVAL` without arming the hook, an unreached token-copy hook remains armed until a later token-copy create consumes it, valid destroy before start and destroy after no-callback failed start unregister ALPN and destroy UDP synchronously without `CONN_CLOSE` or CID unregister, post-create mutations or releases of `server_host`, token bytes, `peer_addr`, nested `conn_ssl_config` buffers, and `no_crypto_flag` do not affect `xqc_connect`, the host-macOS kqueue-change subcases are guarded with `#if defined(__APPLE__)` / `GTEST_SKIP()` elsewhere, and fake `xqc_connect` branches cover no-callback `NULL`, post-`conn_create_notify` `NULL`, and post-`conn_create_notify` plus `conn_close_notify` before `NULL` retry. T3 asserts failed CID re-registration with a live stream session removes the transport-map entry before wrapper destroy, clears stream user data before the abort `STREAM_CLOSE`, makes saved stale callbacks benign, unregisters only the old CID, and leaves later closing/stale CID updates side-effect-free. T4 asserts null preconditions, downstream fd transport creation failure, and initial downstream-interest failure leave `conn_fd` open, preserve `*out`, fire no `on_close`, and call no factory; it also arms `odin_client_session_test_fail_next_dial` after factory-constructor creation, proves valid factory success, factory failure, and post-factory failure do not enter `start_dial`, and writes `late-local!` after parse while CONNECT_RESP is stalled, uses `poll` plus `recv(..., MSG_PEEK)` to prove the bytes remain queued, and then proves the same bytes relay only after HTTP 200. T7 asserts closing-runtime rejection is reached by CID-update registration failure and local EOF before parse fires `on_close(ECONNRESET)` with zero stream creation. T9 asserts live `runtime_stream_close_notify` returns `XQC_OK`, destroys only that stream session/context, clears stream user data, leaves the CID registered, and allows a later stream to complete; it also asserts NULL, stale, and missing-ALPN read/write/close return values, NULL/stale/missing-ALPN closing no-ops, ALPN lookup before return, and no helper forwarding or cleanup side effects for those benign callback subcases. T16 asserts queued fds survive stop, create no streams while stopped, and convert FIFO after restart plus handshake; T17 asserts an already-live session and stream-user-data slot survive stop, then complete after restart; T18 asserts stale or closing handshake callbacks do not convert queued fds and that only the valid current handshake drains the queue; T19 asserts stopped destroy closes queued fds, clears connection ALPN user data, unregisters the CID, records no `CONN_CLOSE`, and destroys UDP before return without restart or post-destroy close-notify injection; T20 asserts stopped destroy clears live sessions and stream-user-data, clears connection ALPN user data, unregisters the CID, records no `CONN_CLOSE`, and destroys UDP before return without restart or post-destroy close-notify injection; T21 asserts already-connected and closing create notifications return `-1/EALREADY`, perform no second CID registration or ALPN user-data overwrite, use `odin_xqc_client_runtime_test_state` to prove `connect_errno == EALREADY` and unchanged `conn`, `current_cid`, `cid_registered`, `handshake_done`, and `closing`, and still let the original close/unregister cleanup run exactly once.
    T3, T9, T14, and T20 also assert that the recorded xqc stream-user-data clear precedes each non-OK `STREAM_CLOSE` on CID-update registration-failure abort, reset, running destroy, and stopped destroy paths.
    T2 also asserts `engine_config = NULL` records a null `last_udp_create.engine_config` and still succeeds through ALPN registration/start, while `engine_config->sendmmsg_on = 1` returns `-1/ENOTSUP` through `odin_xqc_udp_create`, preserves `*out`, records no ALPN register, no UDP destroy, no `xqc_connect`, and no xqc-UDP `engine_create` call, and frees copied runtime config under ASan.
    T1-T3, T5-T11, T13-T14, and T16-T21 install the complete §3.2.5 fake-xquic fixture before passing fake xquic objects through runtime wrappers. T15 clears optional client-runtime, server-runtime, and UDP fake ops, installs pass-through `odin_xqc_stream_transport_test_ops_t` callbacks that call `xqc_stream_recv`, `xqc_stream_send`, and `xqc_stream_set_user_data`, and exercises production `xqc_connect`, production UDP sockets, production `odin_xqc_server_runtime`, `ODIN_XQC_CLIENT_ALPN`, `ODIN_XQC_SERVER_ALPN`, real `xqc_transport_callbacks_t.cert_verify_cb` invocation, runtime no-op client cache callback defaults, production xqc stream I/O, and one real `XQC_STREAM_BIDI`. T3, T8, and T9 use the existing `ODIN_TRANSPORT_XQC_TESTING` stream-user-data hook. T2 uses the existing `ODIN_XQC_UDP_TESTING` engine-create failure hook, the existing host-macOS `ODIN_EVENT_LOOP_TESTING` kqueue-change failure hook, the new `ODIN_XQC_CLIENT_RUNTIME_TESTING` config-copy allocation hook, and client-runtime fake `xqc_connect`; T4 uses the existing `ODIN_CLIENT_SESSION_TESTING` per-session dial-failure hook to prove the factory branch avoids `start_dial`; T8 uses the existing `ODIN_CLIENT_SESSION_TESTING` client-session create-failure hook, the new `ODIN_XQC_CLIENT_RUNTIME_TESTING` pending-append and stream-context allocation hooks, and the new `ODIN_CONNECT_SESSION_TESTING` client connect-session create-failure hook; T21 uses the new `ODIN_XQC_CLIENT_RUNTIME_TESTING` state snapshot helper; T19 and T20 use the existing `ODIN_XQC_UDP_TESTING` engine-destroy hook to invoke xquic close notification inside `odin_xqc_udp_destroy`.
    Cross-compile-only enumeration: `out/xqc_client_mac_arm64/odin`, `out/xqc_client_linux_x64/odin`, `out/xqc_client_ios_sim/odin`, `out/xqc_client_ios_device/odin`, and their `odin_unittests` binaries are built but not executed; Linux UDP/socket and event-loop branches, iOS socket branches, XQUIC client TLS setup on non-host platforms, and alternate macOS architecture are verified by successful cross-compile plus code review only. `./tool/gn gen out/xqc_client_mac_asan --args='target_os="mac" is_asan=true'`, `./tool/ninja -C out/xqc_client_mac_asan odin_unittests`, and `out/xqc_client_mac_asan/odin_unittests --gtest_filter='OdinXqcClientRuntimeTest.*:OdinClientSessionUpstreamTransportTest.*'` exit without AddressSanitizer reports, backing T2 config-copy rollback, zero-ticket empty-byte ownership, and no-live-connection destroy cleanup, T3 CID-update registration-failure live-stream abort cleanup, T4 constructor downstream setup rollback, T8-T11 setup-failure, stream close/reset/stale callback, direct connection-close, T13-T20 destroy, stop-preservation, stopped-destroy engine-destroy callback, and stale-handshake lifecycle assertions, and T21 duplicate-create cleanup assertions. `./tool/ninja -C out/xqc_client_mac odin:odin_client_xqc_runtime_scope_check`, `./tool/ninja -C out/xqc_client_mac_arm64 odin:odin_client_xqc_runtime_scope_check`, `./tool/ninja -C out/xqc_client_linux_x64 odin:odin_client_xqc_runtime_scope_check`, `./tool/ninja -C out/xqc_client_ios_sim odin:odin_client_xqc_runtime_scope_check`, and `./tool/ninja -C out/xqc_client_ios_device odin:odin_client_xqc_runtime_scope_check` all pass. Production `out/xqc_client_mac/odin` and cross-compiled `out/xqc_client_linux_x64/odin` contain no `odin_xqc_client_runtime_test_*`, `odin_xqc_udp_test_*`, or `odin_xqc_stream_transport_test_*` symbols; `./tidy_odin.sh` exits clean over touched Odin files.
    The passing scope-check targets include T12's client-runtime local-resource token scan and `odin/client_session.c` factory-region scan on every listed output directory.
