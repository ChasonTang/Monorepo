# RFC-017: xquic UDP Event Driver

## 1. Summary

Add `odin/xqc_udp.{c,h}`, an owner-thread event driver that owns one xquic engine, one RFC-015 `odin_udp_t`, and one RFC-010 timer, maps UDP receive batches into `xqc_engine_packet_process` plus `xqc_engine_finish_recv`, maps xquic engine timers to `odin_event_timer_t`, maps xquic packet-send callbacks including stateless reset and pre-accept sends to `odin_udp_send`, uses WRITE-readiness recovery for connection sends with an `xqc_conn_continue_send` path, and defers teardown across driver-entered xquic callbacks.

## 2. Goals

- **G1.** Provide a public `odin_xqc_udp_create` / `start` / `stop` / `destroy` API that creates and owns exactly one `xqc_engine_t` and one `odin_udp_t`, exposes the owned engine pointer plus the adapter pointer that callers must use as xquic transport user data, exposes the `app_user_data` configured at create time, and exposes `register_conn` / `unregister_conn` entry points that callers use to enroll connection CIDs for send recovery.
- **G2.** On UDP READ readiness, process received datagrams in bounded batches: each `ODIN_UDP_OK` datagram is passed once to `xqc_engine_packet_process` with the datagram bytes, the endpoint's bound local address, the peer address, a monotonic microsecond receive timestamp, and the adapter user data; after a batch with at least one processed datagram, call `xqc_engine_finish_recv` exactly once unless adapter destruction was requested from inside the batch.
- **G3.** Map xquic's `set_event_timer(wake_after_us, engine_user_data)` callback to one Odin one-shot timer: each call replaces the previous pending engine timer, and timer expiry invokes `xqc_engine_main_logic` once on the owner thread.
- **G4.** Map xquic packet-send callbacks (`stateless_reset`, `write_socket`, `write_socket_ex`, and `conn_send_packet_before_accept`) to `odin_udp_send`: a full datagram send returns `size`; retryable UDP backpressure from `write_socket` or `write_socket_ex` returns `XQC_SOCKET_EAGAIN` only after UDP WRITE readiness is armed; backpressure from `stateless_reset` or `conn_send_packet_before_accept` returns `XQC_SOCKET_ERROR` because current xquic has no continuation path for those packets; non-retryable UDP failure or WRITE-interest reconciliation failure returns `XQC_SOCKET_ERROR`; and UDP WRITE readiness calls `xqc_conn_continue_send` for the registered CIDs only after WRITE interest is cleared or proven absent.
- **G5.** Close lifecycle gaps: creation rollback leaves no engine, UDP endpoint, timer, or loop watch alive; `destroy` has an explicit caller precondition that xquic connections are already destroyed or unavailable and unregistered; `destroy` outside xquic callbacks tears down immediately when no caller-entered xquic callback is on the stack; `destroy` requested from inside driver-entered xquic callbacks stops new Odin trigger surfaces and defers `xqc_engine_destroy` and wrapper free until the outermost driver-entered xquic call returns.

## 3. Design

### 3.1 Overview

`odin/xqc_udp` is the layer that RFC-016 intentionally left to the caller: it creates the xquic engine with Odin-owned environment callbacks, binds a nonblocking UDP endpoint, and drives xquic from Odin event-loop readiness and timers. The caller still owns application protocol registration, stream-level behavior, and any ALPN-specific state; this driver owns only the engine/UDP/timer event integration.

The driver copies the caller's `xqc_engine_callback_t` and `xqc_transport_callbacks_t`, then replaces the fields that must route through Odin: `engine_callback.set_event_timer`, `transport_callbacks.stateless_reset`, `transport_callbacks.write_socket`, `transport_callbacks.write_socket_ex`, and `transport_callbacks.conn_send_packet_before_accept`. When the caller did not supply `engine_callback.monotonic_ts`, the driver installs its own `odin_xqc_udp_default_monotonic_us` (CLOCK_MONOTONIC microseconds) into the copied table so xquic's global `xqc_monotonic_timestamp` (`xquic/src/common/xqc_time.c:62`, set by `xqc_engine_set_callback` at `xquic/src/transport/xqc_engine.c:412-414` during engine create) shares one time base with the driver's `recv_time` helper. The other callback fields are passed through unchanged except the mmsg send slots, which are cleared after creation rejects xquic sendmmsg mode. Because `write_socket`, `write_socket_ex`, and `conn_send_packet_before_accept` receive connection user data while `stateless_reset` receives the user data passed to `xqc_engine_packet_process` (`xquic/include/xquic/xquic.h:204-210`), both values must be `odin_xqc_udp_xqc_user_data(xu)` for sends that route through this driver. Application state should be reached through `odin_xqc_udp_app_user_data(xu)`, ALPN user data, or stream user data. Because `write_socket` and `write_socket_ex` do not carry a connection CID, callers register the CIDs returned by `xqc_connect` or server accept callbacks with this driver; after a retryable UDP send failure that successfully arms WRITE readiness, UDP WRITE readiness iterates that registered-CID set and calls `xqc_conn_continue_send`. Current xquic treats a non-full `conn_send_packet_before_accept` result as a socket error in both the normal pre-accept send path (`xquic/src/transport/xqc_conn.c:2515`) and retry-packet path (`xquic/src/transport/xqc_conn.c:3296`), and `xqc_engine_send_reset` only propagates a negative stateless-reset callback result (`xquic/src/transport/xqc_engine.c:671-674`), so pre-accept and stateless-reset packets are best-effort under UDP backpressure and are not part of WRITE-readiness recovery.

`odin/udp` gains one bound-local-address accessor so the receive path can pass a concrete `local_addr` to `xqc_engine_packet_process` without exposing the UDP fd. The accessor returns `getsockname(2)` output for the bound socket. This RFC does not add per-packet destination-address discovery for wildcard-bound sockets; callers that need the exact destination local IP for each packet must bind a concrete local address until a later `recvmsg(2)` plus `IP_PKTINFO` / `IPV6_PKTINFO` extension exists.

```
caller
  |
  | odin_xqc_udp_create(config, &xu)
  v
odin_xqc_udp_t
  | owns                         | owns
  v                              v
xqc_engine_t                 odin_udp_t
  ^                              |
  | set_event_timer              | READ readiness
  v                              v
odin_event_timer_t          xqc_engine_packet_process
                                  |
                                  v
                             xqc_engine_finish_recv

xquic packet-send callbacks  -> odin_udp_send
ODIN_UDP_WRITE after recoverable EAGAIN
                                -> xqc_conn_continue_send(registered CIDs)
timer expiry                  -> xqc_engine_main_logic
```

### 3.2 Detailed Design

#### 3.2.1 Public API and Ownership

```c
/* odin/udp.h addition */
int odin_udp_local_addr(odin_udp_t *u, struct sockaddr *addr,
                        socklen_t *addrlen);

/* odin/xqc_udp.h */
#include <sys/socket.h>

#include "odin/event_loop.h"
#include "odin/udp.h"
#include <xquic/xquic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ODIN_XQC_UDP_PACKET_CAP 65535u
#define ODIN_XQC_UDP_RECV_BATCH_MAX 64u

typedef struct odin_xqc_udp_t odin_xqc_udp_t;

typedef struct odin_xqc_udp_config_t {
  odin_event_loop_t *loop;
  const struct sockaddr *local_addr;
  socklen_t local_addrlen;
  xqc_engine_type_t engine_type;
  const xqc_config_t *engine_config;
  const xqc_engine_ssl_config_t *ssl_config;
  const xqc_engine_callback_t *engine_callbacks;
  const xqc_transport_callbacks_t *transport_callbacks;
  void *app_user_data;
} odin_xqc_udp_config_t;

int odin_xqc_udp_create(const odin_xqc_udp_config_t *config,
                        odin_xqc_udp_t **out);
int odin_xqc_udp_start(odin_xqc_udp_t *xu);
int odin_xqc_udp_stop(odin_xqc_udp_t *xu);
void odin_xqc_udp_destroy(odin_xqc_udp_t *xu);

xqc_engine_t *odin_xqc_udp_engine(odin_xqc_udp_t *xu);
void *odin_xqc_udp_xqc_user_data(odin_xqc_udp_t *xu);
void *odin_xqc_udp_app_user_data(odin_xqc_udp_t *xu);
int odin_xqc_udp_register_conn(odin_xqc_udp_t *xu, const xqc_cid_t *cid);
void odin_xqc_udp_unregister_conn(odin_xqc_udp_t *xu, const xqc_cid_t *cid);

#ifdef __cplusplus
}
#endif
```

**Unstated contract.** All APIs are owner-thread APIs under the RFC-010 event-loop contract. `odin_xqc_udp_create` requires non-null `config`, `config->loop`, `config->local_addr`, `config->engine_callbacks`, `config->transport_callbacks`, and `out`; `engine_config` and `ssl_config` are passed through to xquic and may be null if the selected xquic engine mode accepts null. The function returns `0` and writes `*out` on success, or returns `-1` with `errno` set and leaves `*out` unchanged on local setup failure. If `config->engine_config != NULL && config->engine_config->sendmmsg_on != 0`, creation returns `-1` with `errno = ENOTSUP`; this RFC implements single-datagram send callbacks only, not xquic `write_mmsg` / `write_mmsg_ex`.

The driver pins one time source for both `recv_time` and xquic's notion of "now". When `config->engine_callbacks->monotonic_ts == NULL`, the copied table's `monotonic_ts` is replaced with `odin_xqc_udp_default_monotonic_us`, which returns `clock_gettime(CLOCK_MONOTONIC)` microseconds (or the `ODIN_XQC_UDP_TESTING` timestamp hook in tests). When the caller supplied `monotonic_ts`, the driver preserves it. The driver's `monotonic_us(xu)` helper used to fill the `recv_time` argument of `xqc_engine_packet_process` (§3.2.2) reads from `xu.engine_callbacks.monotonic_ts`, so the helper and xquic's internal time source (via `xqc_monotonic_timestamp` set by `xqc_engine_create -> xqc_engine_set_callback`, `xquic/src/transport/xqc_engine.c:412-414`) are guaranteed to be the same function. Drift between `pkt_recv_time` and `xqc_monotonic_timestamp()` cannot occur, so ACK-delay encoding (`xquic/src/transport/xqc_frame_parser.c:1026`), packet-receive delta accounting (`xquic/src/transport/xqc_frame.c:1536`), RTT estimation, idle-timeout, and first-receive stats see consistent deltas. `realtime_ts` is left untouched because `recv_time` is the only call site this RFC drives.

The driver owns the `xqc_engine_t *` returned by `xqc_engine_create` (`xquic/include/xquic/xquic.h:1654`) and calls `xqc_engine_destroy` exactly once, subject to the callback deferral rule in section 3.2.4, the caller preconditions below, and xquic's warning that engine destroy is only valid after connections are destroyed and must not run inside xquic callbacks (`xquic/include/xquic/xquic.h:1662-1668`). The caller must not call `xqc_engine_destroy` on the pointer returned by `odin_xqc_udp_engine`.

`odin_xqc_udp_start` arms UDP READ interest and also arms UDP WRITE interest when a previous retryable send failure left `xu->write_blocked` set. Calling `start` on an already-started driver is a no-op success. If `odin_udp_set_interest` fails, `start` returns `-1`, preserves `errno`, and leaves `xu->started == 0` with no READ watch armed. `odin_xqc_udp_stop` removes UDP interest, clears pending WRITE recovery, and cancels a pending xquic engine timer; calling `stop` on an already-stopped driver is a no-op success. `destroy` stops the driver first, closes the UDP endpoint, destroys the engine, and frees the wrapper, or marks destruction pending when called from inside a driver-entered xquic call. `odin_xqc_udp_destroy(NULL)` is a no-op.

`odin_udp_local_addr` writes the endpoint's current bound socket address into the caller buffer using `getsockname(2)`. `*addrlen` is the caller capacity on input and the actual length on success. It returns `0` on success or `-1` with `errno` set. The xquic driver snapshots this address after `odin_udp_open` succeeds, so a `port 0` bind is reported to xquic with the kernel-assigned port.

`odin_xqc_udp_xqc_user_data(xu)` returns `xu`, and `odin_xqc_udp_app_user_data(xu)` returns the `config->app_user_data` pointer captured at creation. Callers must pass `odin_xqc_udp_xqc_user_data(xu)` as xquic transport user data for client connections created with `xqc_connect`, and server code must ensure accepted connections that use this UDP driver have the same transport user data, for example by calling `xqc_conn_set_transport_user_data(conn, odin_xqc_udp_xqc_user_data(xu))` in the server accept/create path when xquic has not already set it from packet processing. Passing any other non-null pointer to the installed send callbacks is a caller precondition violation; the callbacks return `XQC_SOCKET_ERROR` when the callback user data is null.

`odin_xqc_udp_register_conn` copies one `xqc_cid_t` into the driver's registered-CID set. It is idempotent for an already-registered CID, returns `0` on success, and returns `-1` with `errno = EINVAL` for null inputs or with `errno = ENOMEM` if the set cannot grow. `odin_xqc_udp_unregister_conn` removes a matching CID and is a no-op for null inputs or absent CIDs. CID equality uses xquic's `xqc_cid_is_equal` helper (`xquic/include/xquic/xquic.h:2110`). Client callers register the copied CID returned by `xqc_connect`; server callers register the accept/create CID for accepted connections; callers that receive xquic CID-update callbacks register the new CID and unregister the retired CID; all callers unregister in their connection close notification after xquic has destroyed or made the connection unavailable. The registered-CID set is only for WRITE-readiness recovery and is not a connection owner.

`odin_xqc_udp_destroy` never closes or drains xquic connections. Every non-null destroy call, including a destroy requested from inside a driver-entered callback, requires that all xquic connections on the engine are already destroyed or unavailable and unregistered from the driver before physical engine destroy can run. Because the engine pointer is exposed for caller-owned xquic operations, destroy outside driver-entered callbacks has one additional precondition: no caller-owned direct xquic API call, and no xquic callback entered through such a direct call, may be on the stack. Code that wants to destroy from a caller-entered xquic callback must post or defer the destroy request to the owner loop after that callback returns; driver-entered packet, finish-receive, timer, and WRITE-recovery continue-send callbacks are covered by the section 3.2.4 `callback_depth` deferral.

**Mechanism.**

```
create(config, out):
  validate non-null config, loop, local_addr, engine_callbacks, transport_callbacks, out
  if engine_config enables sendmmsg: errno = ENOTSUP; return -1
  allocate xu with fd-independent fields zeroed
  copy config->engine_callbacks into xu.engine_callbacks
  xu.engine_callbacks.set_event_timer = odin_xqc_udp_set_event_timer
  if xu.engine_callbacks.monotonic_ts == NULL:
    xu.engine_callbacks.monotonic_ts = odin_xqc_udp_default_monotonic_us
  copy config->transport_callbacks into xu.transport_callbacks
  xu.transport_callbacks.stateless_reset = odin_xqc_udp_stateless_reset
  xu.transport_callbacks.write_socket = odin_xqc_udp_write_socket
  xu.transport_callbacks.write_socket_ex = odin_xqc_udp_write_socket_ex
  xu.transport_callbacks.conn_send_packet_before_accept =
      odin_xqc_udp_conn_send_packet_before_accept
  xu.transport_callbacks.write_mmsg = NULL
  xu.transport_callbacks.write_mmsg_ex = NULL
  open odin_udp_t bound to config->local_addr
  read odin_udp_local_addr into xu.local_addr
  engine = xqc_engine_create(config->engine_type, config->engine_config,
                             config->ssl_config, &xu.engine_callbacks,
                             &xu.transport_callbacks, xu)
  if engine == NULL: rollback timer, UDP, and wrapper; errno = saved_or_EIO; return -1
  xu.engine = engine
  xu.app_user_data = config->app_user_data
  *out = xu; return 0

start(xu):
  if xu.started: return 0
  mask = ODIN_UDP_READ
  if xu.write_blocked: mask |= ODIN_UDP_WRITE
  if odin_udp_set_interest(xu.udp, mask) != 0:
    return -1
  xu.started = 1; return 0

stop(xu):
  if !xu.started and xu.timer == NULL and !xu.write_blocked: return 0
  xu.write_blocked = 0
  odin_udp_set_interest(xu.udp, 0)
  xu.started = 0
  if xu.timer != NULL: odin_event_timer_stop(xu.timer); xu.timer = NULL
  return 0

register_conn(xu, cid):
  validate xu and cid
  if cid already exists in xu.registered_cids: return 0
  append copied cid to xu.registered_cids
  return 0

unregister_conn(xu, cid):
  if xu == NULL or cid == NULL: return
  remove matching cid from xu.registered_cids if present
```

Satisfies: G1 via the public factory, start/stop/destroy surface, owned engine/UDP/timer contract, xquic user-data helper, app-user-data helper, registered-CID set, and `odin_udp_local_addr`; G4 via the registered-CID surface used for WRITE recovery; G5 via the creation rollback, destroy ownership rules, and explicit caller preconditions for active connections and caller-entered xquic callbacks.

#### 3.2.2 UDP Receive Batches

The receive path is driven by the existing `odin_udp_ready_cb` and `odin_udp_recv` surfaces (`odin/udp.h:63`, `odin/udp.h:70`) and the current xquic receive entry points (`xquic/include/xquic/xquic.h:1735`, `xquic/include/xquic/xquic.h:1797`). The adapter uses a fixed `ODIN_XQC_UDP_PACKET_CAP` receive buffer and processes at most `ODIN_XQC_UDP_RECV_BATCH_MAX` datagrams per readiness callback.

**Unstated contract.** A READ-ready callback drains up to 64 queued datagrams or until `odin_udp_recv` returns `ODIN_UDP_AGAIN`, whichever comes first. Each successful receive calls `xqc_engine_packet_process` with:

- `packet_in_buf` and `packet_in_size` equal to the bytes returned by `odin_udp_recv`;
- `local_addr` equal to the bound address snapshotted by `odin_udp_local_addr`;
- `peer_addr` equal to the source address returned by `odin_udp_recv`;
- `recv_time` from `monotonic_us(xu)`, which calls `xu.engine_callbacks.monotonic_ts()` — either the caller-supplied `monotonic_ts` (preserved during create) or the driver's default `odin_xqc_udp_default_monotonic_us` installed when the caller left the slot null (§3.2.1) — so `recv_time` and `xqc_monotonic_timestamp()` are always the same function;
- `user_data = xu`.

The adapter does not parse QUIC packet bytes, inspect CIDs, rewrite peer addresses, retry malformed packets, or convert xquic packet-process return codes into Odin callbacks. A negative xquic return drops that datagram at the driver boundary; xquic owns connection-level consequences.

If at least one packet was processed and `destroy` was not requested from inside an xquic callback, the adapter calls `xqc_engine_finish_recv(engine)` once after the receive loop exits. If `destroy` was requested while `xqc_engine_packet_process` was on the stack, the adapter does not call `xqc_engine_finish_recv`, does not process more datagrams in that callback, and runs deferred teardown after the outermost xquic call returns. If `destroy` is requested while `xqc_engine_finish_recv` is on the stack, `leave_xqc` runs deferred teardown and the readiness callback returns immediately without reading wrapper state after `leave_xqc`. A readiness callback whose first receive returns `ODIN_UDP_AGAIN` calls neither `xqc_engine_packet_process` nor `xqc_engine_finish_recv`.

`ODIN_UDP_IO_ERROR` stops the current receive loop and records `errno` in the test-only state; this RFC exposes no public UDP-driver error callback because the requirement covers engine driving rather than application error notification. A later RFC that wires application lifecycle may decide how socket I/O errors surface to users.

When the same UDP readiness callback delivers `ODIN_UDP_WRITE`, the WRITE recovery branch in section 3.2.3 runs before the READ receive loop. If WRITE recovery destroys the wrapper, the callback returns before reading any datagrams.

**Mechanism.**

```
on_udp_ready(u, events, user_data):         # user_data = xu
  if xu.destroy_requested: return
  if events has ODIN_UDP_ERROR:
    xu.last_udp_errno = EIO
  if events has ODIN_UDP_WRITE:
    if handle_udp_write_ready(xu) == destroyed: return
  processed = 0
  if events has ODIN_UDP_READ:
    while processed < ODIN_XQC_UDP_RECV_BATCH_MAX:
      peer_len = sizeof peer
      n = 0
      rc = odin_udp_recv(xu.udp, packet, sizeof packet, &n, &peer, &peer_len)
      if rc == ODIN_UDP_AGAIN: break
      if rc == ODIN_UDP_IO_ERROR:
        xu.last_udp_errno = errno
        break
      enter_xqc(xu)
      xqc_engine_packet_process(xu.engine, packet, n,
                                &xu.local_addr, xu.local_addrlen,
                                &peer, peer_len, monotonic_us(xu), xu)
      if leave_xqc(xu) == destroyed: return
      processed += 1
      if xu.destroy_requested: return
    if processed != 0 and !xu.destroy_requested:
      enter_xqc(xu)
      xqc_engine_finish_recv(xu.engine)
      if leave_xqc(xu) == destroyed: return
```

Satisfies: G2 via the READ-readiness receive loop, exact packet/peer/local/timestamp/user-data call into `xqc_engine_packet_process`, the once-per-nonempty-batch `xqc_engine_finish_recv`, and the 64-packet callback bound; G5 via the no-further-xquic-calls rule after in-callback destroy.

#### 3.2.3 Timer and Packet-Send Callbacks

The xquic timer callback type and engine callback field are current at `xquic/include/xquic/xquic.h:107` and `xquic/include/xquic/xquic.h:1203-1205`. The xquic send callback surfaces are current at `xquic/include/xquic/xquic.h:199-210`, `xquic/include/xquic/xquic.h:361-367`, `xquic/include/xquic/xquic.h:473-481`, `xquic/include/xquic/xquic.h:636-637`, and `xquic/include/xquic/xquic.h:725-728`. The xquic write-readiness recovery API is current at `xquic/include/xquic/xquic.h:2130-2133`. Odin's timer API is current at `odin/event_loop.h:76-87`, and Odin's UDP send and READ|WRITE interest APIs are current at `odin/udp.h:73-77`.

```c
/* Internal callbacks installed into copied xquic callback tables. */
static void odin_xqc_udp_set_event_timer(xqc_usec_t wake_after,
                                         void *engine_user_data);
static ssize_t odin_xqc_udp_stateless_reset(const unsigned char *buf,
                                            size_t size,
                                            const struct sockaddr *peer_addr,
                                            socklen_t peer_addrlen,
                                            const struct sockaddr *local_addr,
                                            socklen_t local_addrlen,
                                            void *user_data);
static ssize_t odin_xqc_udp_write_socket(const unsigned char *buf,
                                         size_t size,
                                         const struct sockaddr *peer_addr,
                                         socklen_t peer_addrlen,
                                         void *conn_user_data);
static ssize_t odin_xqc_udp_write_socket_ex(uint64_t path_id,
                                            const unsigned char *buf,
                                            size_t size,
                                            const struct sockaddr *peer_addr,
                                            socklen_t peer_addrlen,
                                            void *conn_user_data);
static ssize_t odin_xqc_udp_conn_send_packet_before_accept(
    const unsigned char *buf, size_t size,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen,
    void *conn_user_data);
static int odin_xqc_udp_handle_udp_write_ready(odin_xqc_udp_t *xu);
```

**Unstated contract.** `set_event_timer` treats `wake_after` as microseconds, matching `xqc_usec_t` (`xquic/include/xquic/xquic_typedef.h:99-105`). Each call replaces any pending engine timer: an existing `odin_event_timer_t` is reset to `delay_us = wake_after, repeat_us = 0`; if no timer is active, a new one-shot timer is started. `wake_after == 0` follows RFC-010 timer semantics and fires on the next timer pass. When the timer callback fires, the adapter stops the fired one-shot handle with `odin_event_timer_stop(timer)` and clears `xu->timer` before calling `xqc_engine_main_logic`. Therefore a nested `set_event_timer` call from `main_logic` creates the next timer only after the fired handle is inactive; the driver never has two active Odin timers at the same time.

If both `odin_event_timer_reset` and the subsequent `odin_event_timer_start` fail (typically ENOMEM), the driver records `errno` into `xu.last_timer_errno`, leaves `xu.timer == NULL`, and returns. This case is terminal for that scheduling intent: xquic has been told a timer is installed and will not call `set_event_timer` again until its own state changes, so `xqc_engine_main_logic` will not fire on the schedule xquic expected. The next `set_event_timer` call from xquic (e.g. on the next packet receipt or send) re-runs the start path and clears the stuck state on success. Callers diagnose persistent stalls through the `odin_xqc_udp_test_last_timer_errno` hook in §3.2.5. The driver does not retry internally with a posted task because the failure path is dominated by allocator pressure that a same-tick retry would not relieve, and a deferred retry would race xquic's next `set_event_timer` call.

`write_socket`, `write_socket_ex`, and `conn_send_packet_before_accept` require `conn_user_data == xu`; `write_socket_ex` ignores `path_id` because this RFC owns one UDP endpoint and does not implement per-path sockets. `stateless_reset` requires `user_data == xu`, where that value is the packet-processing user data passed into `xqc_engine_packet_process`; it ignores `local_addr` / `local_addrlen` because the driver has one bound UDP endpoint. For a valid live adapter and non-null `peer_addr`, every installed packet-send callback calls `odin_udp_send(xu->udp, buf, size, &sent, peer_addr, peer_addrlen)`. `ODIN_UDP_OK` with `sent == size` returns `size`. `ODIN_UDP_IO_ERROR`, a short send, null callback user data, `peer_addr == NULL`, or `xu->destroy_requested` returns `XQC_SOCKET_ERROR`.

`ODIN_UDP_AGAIN` from `write_socket` or `write_socket_ex` sets `xu->write_blocked = 1`, reconciles UDP interest to include `ODIN_UDP_WRITE` when the driver is started, and returns `XQC_SOCKET_EAGAIN` (`xquic/include/xquic/xquic.h:361-367`) only if that reconciliation succeeds or the driver is currently stopped and can arm pending WRITE in a later `start`. If `odin_udp_set_interest` fails while adding WRITE to a started driver, the send callback records `errno`, clears `write_blocked`, and returns `XQC_SOCKET_ERROR`; Odin's UDP layer preserves the previous watch on update failure (`odin/udp.c:145`), so callers observe the old READ watch rather than a falsely armed WRITE recovery. `ODIN_UDP_AGAIN` from `stateless_reset` or `conn_send_packet_before_accept` returns `XQC_SOCKET_ERROR`, leaves `write_blocked` unchanged, and does not arm WRITE readiness because current xquic has no `xqc_conn_continue_send` recovery path for those packets.

The recovery-capable connection send callbacks receive no CID, so the blocked state is socket-wide rather than per connection. On the next `ODIN_UDP_WRITE` readiness, the driver clears `write_blocked`, removes WRITE interest before entering xquic again, snapshots the registered-CID set from section 3.2.1, and calls `xqc_conn_continue_send(xu->engine, &cid)` once for each CID still registered at its turn. If clearing WRITE interest fails, the driver records `errno`, restores `write_blocked = 1`, skips `xqc_conn_continue_send` for that readiness callback, and leaves the existing READ|WRITE watch in place for a later retry. If a retry send inside `xqc_conn_continue_send` hits `ODIN_UDP_AGAIN` again, the send callback re-sets `write_blocked` and re-arms WRITE interest for another readiness pass. If no CIDs are registered, WRITE readiness only clears the socket-wide blocked state.

A successful UDP send does not call `xqc_engine_finish_send`; current xquic marks `xqc_engine_finish_send` as only useful for manually triggered send mode (`xquic/include/xquic/xquic.h:1799-1800`), and this RFC does not enable manually triggered send.

**Mechanism.**

```
set_event_timer(wake_after, engine_user_data):
  xu = engine_user_data
  if xu == NULL or xu.destroy_requested: return
  if xu.timer != NULL:
    if odin_event_timer_reset(xu.timer, wake_after, 0) == 0: return
    odin_event_timer_stop(xu.timer); xu.timer = NULL
  if odin_event_timer_start(xu.loop, wake_after, 0, on_timer, xu,
                            &xu.timer) != 0:
    xu.last_timer_errno = errno

on_timer(loop, timer, user_data):
  xu = user_data
  if xu.destroy_requested: return
  if xu.timer == timer:
    odin_event_timer_stop(timer)
  xu.timer = NULL
  enter_xqc(xu)
  xqc_engine_main_logic(xu.engine)
  leave_xqc(xu)

update_udp_interest(xu):
  mask = 0
  if xu.started: mask |= ODIN_UDP_READ
  if xu.write_blocked and xu.started: mask |= ODIN_UDP_WRITE
  if odin_udp_set_interest(xu.udp, mask) != 0:
    xu.last_udp_errno = errno
    return -1
  return 0

send_datagram(xu, buf, size, peer_addr, peer_addrlen, recoverable):
  if xu == NULL or xu.destroy_requested or peer_addr == NULL:
    return XQC_SOCKET_ERROR
  rc = odin_udp_send(xu.udp, buf, size, &sent, peer_addr, peer_addrlen)
  if rc == ODIN_UDP_OK and sent == size: return size
  if rc == ODIN_UDP_AGAIN:
    if !recoverable: return XQC_SOCKET_ERROR
    xu.write_blocked = 1
    if update_udp_interest(xu) != 0:
      xu.write_blocked = 0
      return XQC_SOCKET_ERROR
    return XQC_SOCKET_EAGAIN
  return XQC_SOCKET_ERROR

handle_udp_write_ready(xu):
  if !xu.write_blocked:
    update_udp_interest(xu)
    return alive
  xu.write_blocked = 0
  if update_udp_interest(xu) != 0:   # drops WRITE before xquic can retry sends
    xu.write_blocked = 1
    return alive
  snapshot = copy xu.registered_cids
  for cid in snapshot:
    if cid is no longer registered: continue
    enter_xqc(xu)
    xqc_conn_continue_send(xu.engine, &cid)
    if leave_xqc(xu) == destroyed:
      free snapshot
      return destroyed
  free snapshot
  return alive

write_socket(buf, size, peer_addr, peer_addrlen, conn_user_data):
  return send_datagram(conn_user_data, buf, size, peer_addr, peer_addrlen,
                       recoverable = true)

write_socket_ex(path_id, buf, size, peer_addr, peer_addrlen, conn_user_data):
  ignore path_id
  return write_socket(buf, size, peer_addr, peer_addrlen, conn_user_data)

conn_send_packet_before_accept(buf, size, peer_addr, peer_addrlen,
                               conn_user_data):
  return send_datagram(conn_user_data, buf, size, peer_addr, peer_addrlen,
                       recoverable = false)

stateless_reset(buf, size, peer_addr, peer_addrlen,
                local_addr, local_addrlen, user_data):
  ignore local_addr, local_addrlen
  return send_datagram(user_data, buf, size, peer_addr, peer_addrlen,
                       recoverable = false)
```

Satisfies: G3 via the copied engine callback replacement, one active one-shot Odin timer, fired-handle stop before nested rescheduling, reset/replacement rule, and timer expiry call to `xqc_engine_main_logic`; G4 via the copied transport callback replacements, exact `odin_udp_send` return mapping, pre-accept and stateless-reset best-effort failure on backpressure, WRITE re-arming after recoverable `ODIN_UDP_AGAIN` only when interest reconciliation succeeds, failure-closed WRITE-interest clearing, and `xqc_conn_continue_send` over registered CIDs on WRITE readiness; G5 via callback refusal after destruction starts and `callback_depth` around WRITE recovery's xquic entry point.

#### 3.2.4 Lifecycle State Machine

```text
State          Trigger                         Next State       Observable action
-------------  ------------------------------  ---------------  -----------------------------
ALLOCATING     local setup failure             DEAD             close/free partial resources
ALLOCATING     engine create success           STOPPED          owns UDP + engine, no READ watch
STOPPED        start fails                     STOPPED          preserve errno, no READ watch
STOPPED        start succeeds                  STARTED          UDP READ interest armed, plus WRITE if blocked
STARTED        stop succeeds                   STOPPED          UDP interest removed, timer stopped
STARTED        destroy outside xquic callback  DEAD             preconditions hold; stop, close UDP, destroy engine, free
STOPPED        destroy outside xquic callback  DEAD             preconditions hold; close UDP, destroy engine, free
STARTED        destroy inside xquic callback   DESTROY_PENDING  stop triggers, defer engine/free
STOPPED        destroy inside xquic callback   DESTROY_PENDING  defer engine/free
DESTROY_PENDING outermost xquic call returns   DEAD             close UDP, destroy engine, free
```

**Unstated contract.** `enter_xqc` / `leave_xqc` wrap every driver-initiated call into xquic that can invoke application callbacks: `xqc_engine_packet_process`, `xqc_engine_finish_recv`, `xqc_engine_main_logic`, and `xqc_conn_continue_send`. They maintain `callback_depth`. `destroy` called while `callback_depth > 0` sets `destroy_requested`, removes UDP interest, stops any pending timer, clears pending WRITE recovery, and returns without calling `xqc_engine_destroy` or freeing `xu`. `leave_xqc` performs the physical close/destroy/free only when it decrements the depth to zero and sees `destroy_requested`.

The deferred destroy path makes `xqc_engine_destroy` comply with xquic's public warning against destroying the engine inside callbacks (`xquic/include/xquic/xquic.h:1662-1668`) for callbacks entered by this driver. It also prevents use-after-free in Odin callbacks: after `destroy_requested` is set, UDP readiness and timer callbacks check the flag before making any new xquic call, and `on_udp_ready` reads no wrapper state after `leave_xqc` reports that the wrapper was freed. `odin_udp_close` remains safe from inside UDP readiness because the UDP callback invokes `xu` callbacks as its final action under the RFC-015 contract (`odin/udp.h:31-33`).

The driver cannot observe xquic callbacks reached through caller-owned direct xquic API calls on `odin_xqc_udp_engine(xu)`, so `callback_depth == 0` is not proof that xquic has no active callbacks. The caller preconditions in section 3.2.1 are part of the `destroy` contract: before physical destroy can run, every xquic connection must already be destroyed or unavailable and every CID must be unregistered; before calling `destroy` outside a driver-entered callback, the caller must also have returned from any direct xquic API call or callback entered by such a call. Violating those preconditions can still call `xqc_engine_destroy` while xquic owns stack frames or active connections; this RFC does not claim to detect or repair that caller error.

Rollback is all-or-nothing. If allocation, UDP open, local-address snapshot, callback copy, or `xqc_engine_create` fails, `odin_xqc_udp_create` closes any opened UDP endpoint, stops any timer that may have been installed by xquic before create returned, frees the wrapper, leaves `*out` unchanged, and does not leave an engine pointer owned by the caller.

**Mechanism.**

```
enter_xqc(xu):
  xu.callback_depth += 1

leave_xqc(xu):
  xu.callback_depth -= 1
  if xu.callback_depth == 0 and xu.destroy_requested:
    finish_destroy(xu)
    return destroyed
  return alive

destroy(xu):
  if xu == NULL: return
  if xu.destroy_requested: return
  require xu.registered_cids is empty
  require no active xquic connections remain
  require no caller-entered xquic callback is on the stack
  xu.destroy_requested = 1
  xu.write_blocked = 0
  odin_xqc_udp_stop(xu)
  if xu.callback_depth != 0:
    return
  finish_destroy(xu)

finish_destroy(xu):
  if xu.timer != NULL: odin_event_timer_stop(xu.timer); xu.timer = NULL
  if xu.udp != NULL: odin_udp_close(xu.udp); xu.udp = NULL
  if xu.engine != NULL: xqc_engine_destroy(xu.engine); xu.engine = NULL
  free xu.registered_cids
  free(xu)
```

Satisfies: G1 via explicit STOPPED/STARTED ownership states; G5 via rollback, idempotent destroy request, trigger stop, callback-depth deferral, and final teardown.

#### 3.2.5 Test Hooks

```c
/* odin/xqc_udp_internal_test.h, only when ODIN_XQC_UDP_TESTING is defined. */
typedef struct odin_xqc_udp_test_ops_t {
  xqc_engine_t *(*engine_create)(xqc_engine_type_t engine_type,
                                 const xqc_config_t *engine_config,
                                 const xqc_engine_ssl_config_t *ssl_config,
                                 const xqc_engine_callback_t *engine_callback,
                                 const xqc_transport_callbacks_t *transport_cbs,
                                 void *user_data);
  void (*engine_destroy)(xqc_engine_t *engine);
  xqc_int_t (*packet_process)(xqc_engine_t *engine,
                              const unsigned char *packet_in_buf,
                              size_t packet_in_size,
                              const struct sockaddr *local_addr,
                              socklen_t local_addrlen,
                              const struct sockaddr *peer_addr,
                              socklen_t peer_addrlen,
                              xqc_usec_t recv_time, void *user_data);
  void (*finish_recv)(xqc_engine_t *engine);
  void (*main_logic)(xqc_engine_t *engine);
  xqc_int_t (*conn_continue_send)(xqc_engine_t *engine,
                                  const xqc_cid_t *cid);
  xqc_usec_t (*now_us)(void);
} odin_xqc_udp_test_ops_t;

void odin_xqc_udp_test_set_ops(const odin_xqc_udp_test_ops_t *ops);
int odin_xqc_udp_test_udp(odin_xqc_udp_t *xu, odin_udp_t **out);
int odin_xqc_udp_test_timer_active(odin_xqc_udp_t *xu);
int odin_xqc_udp_test_destroy_requested(odin_xqc_udp_t *xu);
int odin_xqc_udp_test_write_blocked(odin_xqc_udp_t *xu);
int odin_xqc_udp_test_last_timer_errno(odin_xqc_udp_t *xu);
xqc_timestamp_pt odin_xqc_udp_test_engine_monotonic_ts(odin_xqc_udp_t *xu);
```

**Unstated contract.** Production builds call xquic directly and expose none of these symbols. Test builds route engine create/destroy/process/finish/main-logic/continue-send and the receive timestamp through the installed ops, mirroring the RFC-016 stream transport's fake xquic hook pattern. `odin_xqc_udp_test_udp` returns the owned UDP endpoint only while the wrapper is live; tests use RFC-015's `odin_udp_test_fd` to discover the bound address, inject UDP send failures, and inspect READ|WRITE interest. `odin_xqc_udp_test_write_blocked` exposes only the socket-wide pending recovery bit so tests can distinguish a real armed WRITE recovery from a failed reconciliation. `odin_xqc_udp_test_last_timer_errno` returns the `errno` captured by the §3.2.3 set-event-timer fall-through (both reset and start failed) and `0` when no failure was recorded. `odin_xqc_udp_test_engine_monotonic_ts` returns the `monotonic_ts` slot in the copied engine callback table — the same function pointer xquic uses through `xqc_monotonic_timestamp` — so a §5 row can assert the helper is non-null after create and that `recv_time` arguments equal the value returned by that pointer. The test hooks do not change the public ownership contract.

**Mechanism.**

```
if ODIN_XQC_UDP_TESTING and test op exists:
  call test op
else:
  call real xquic function or production monotonic_us()
```

Satisfies: G1 via test-only hooks that back §5's deterministic assertions against fake xquic ops without changing the production ownership surface — the behavioral hooks for G2-G5 live in §3.2.2-§3.2.4.

## 4. Security

The driver receives UDP datagrams from arbitrary peers and passes their bytes and kernel-reported addresses into xquic, so external input crosses a trust boundary.

- **S1.**
  - **Threat:** An arbitrary peer sends a maximum-size UDP datagram or malformed QUIC bytes to the bound endpoint; an Odin-side parser or undersized buffer could overread, truncate silently before xquic, or treat attacker bytes as trusted local control data.
  - **Mitigation:** Section 3.2.2 pins that Odin does not parse QUIC bytes, receives into a 65535-byte packet buffer, passes exactly the byte count and peer address returned by `odin_udp_recv`, and leaves protocol validation to `xqc_engine_packet_process`.
  - **Enforcement:** T2 sends attacker-controlled bytes through a real UDP socket and asserts the fake xquic packet-process hook receives the exact bytes, exact peer address family/port, and adapter user data; the P2 ASan run covers the maximum-size receive row T3.

- **S2.**
  - **Threat:** Application code calls `odin_xqc_udp_destroy` from inside an xquic callback reached through packet processing, finish-receive, timer main logic, or WRITE-recovery `xqc_conn_continue_send`, causing immediate `xqc_engine_destroy` inside xquic or a use-after-free when the Odin callback resumes.
  - **Mitigation:** Section 3.2.4 pins `callback_depth`, `destroy_requested`, stopped trigger surfaces, and deferred physical destroy/free until the outermost xquic call returns.
  - **Enforcement:** T10, T11, T15, and T16 request destroy from fake xquic packet, timer, finish-receive, and continue-send callbacks and assert `xqc_engine_destroy` occurs only after the fake xquic call unwinds; the P2 ASan run covers the no-use-after-free condition.

- **S3.**
  - **Threat:** A peer floods the UDP socket so one READ callback drains indefinitely and starves timer and posted-task dispatch on the owner thread.
  - **Mitigation:** Section 3.2.2 caps one readiness callback at `ODIN_XQC_UDP_RECV_BATCH_MAX == 64` datagrams, calls `xqc_engine_finish_recv` once for that nonempty batch, and returns to the event loop while level-triggered READ readiness remains available for the next pass.
  - **Enforcement:** T4 queues 65 datagrams and asserts the first callback processes exactly 64 and calls `finish_recv` once, leaving the final datagram for a later readiness pass.

## 5. Testing Strategy

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | Create, start, stop, and destroy own exactly one engine and UDP endpoint | Install fake xquic ops; create an Odin loop; set `app_user_data` to a stable test pointer; call `odin_xqc_udp_create` with `AF_INET 127.0.0.1:0`, default xquic config, and callback tables whose timer/send slots contain sentinel function pointers; call `odin_xqc_udp_start`, `odin_xqc_udp_stop`, and `odin_xqc_udp_destroy` | Fake `engine_create` is called once with `user_data == odin_xqc_udp_xqc_user_data(xu)`; copied callbacks have adapter timer/send functions, including `stateless_reset`, instead of sentinels; `odin_xqc_udp_engine(xu)` returns the fake engine before destroy; `odin_xqc_udp_xqc_user_data(xu) == xu`; `odin_xqc_udp_app_user_data(xu)` returns the configured test pointer; `start` arms one UDP READ watch; `stop` removes it; destroy closes the UDP fd and calls fake `engine_destroy` once | G1, G5 | unit |
| T2 | One UDP packet is forwarded to xquic with peer and local addresses | Create/start driver; get bound endpoint address through `odin_xqc_udp_test_udp` plus `odin_udp_test_fd`; send UDP payload `"pkt"` from a plain peer socket; run the loop with a watchdog; fake `packet_process` records arguments and stops the loop after `finish_recv` | Fake `packet_process` is called once with bytes `"pkt"`, peer address equal to the sending socket, local address equal to the driver's bound `getsockname` address including assigned port, `recv_time == fake_now_us`, and `user_data == xu`; fake `finish_recv` is called once after packet processing | G2, S1 | integration |
| T3 | Maximum-size UDP payload is not truncated before xquic | Create/start driver with fake timestamp; send the largest UDP payload accepted by the loopback socket up to `ODIN_XQC_UDP_PACKET_CAP`; run until fake `finish_recv` stops the loop | Fake `packet_process` receives the same byte count and byte pattern sent by the peer; `finish_recv` is called once; under the P2 ASan command the row exits with no buffer overflow report | G2, S1 | integration |
| T4 | Receive batch cap returns control to the loop under flood | Create/start driver; queue 65 small datagrams to the endpoint before running; fake `packet_process` records each payload; fake `finish_recv` stops the loop on its first call; run the loop once, then run again to consume the remaining datagram | First run records exactly 64 packet-process calls and one finish call; second run records the 65th packet and a second finish call; no single readiness callback processes more than `ODIN_XQC_UDP_RECV_BATCH_MAX` datagrams | G2, S3 | integration |
| T5 | xquic timer callback fires main logic once | Fake `engine_create` records the installed `set_event_timer`; create driver; call recorded `set_event_timer(500, xu)`; fix fake event-loop time at `1000000`, inspect wait preparation, advance fake time to `1000500`, and run | The event-loop wait record shows a 500 us one-shot timer; fake `main_logic` is not called before time advances; after the timer fires, fake `main_logic` is called exactly once with the owned engine | G3 | unit |
| T6 | Later xquic timer replaces an earlier timer | Create driver; invoke recorded `set_event_timer(60000000, xu)` and then `set_event_timer(0, xu)` before running; run the loop once with fake main logic stopping the loop | Fake `main_logic` is called once on the zero-delay timer; wait preparation after the callback reports no 60-second stale timer; `odin_xqc_udp_test_timer_active(xu)` returns false | G3 | unit |
| T7 | `write_socket` maps UDP send outcomes and arms WRITE only after retryable backpressure | Create/start driver and a plain UDP peer socket; set `ud = odin_xqc_udp_xqc_user_data(xu)`; call the installed `write_socket("ok", 2, peer_addr, peer_len, ud)`; then inject `EAGAIN` through `odin_udp_test_fail_next_sendto`; inspect the active UDP watch after the blocked call; then call `write_socket` with a 70000-byte buffer | First call returns `2` and the peer receives `"ok"`; injected retryable failure returns `XQC_SOCKET_EAGAIN`, `odin_xqc_udp_test_write_blocked(xu)` is true, and UDP interest includes READ|WRITE; oversized send returns `XQC_SOCKET_ERROR` and does not report a partial byte count | G4 | integration |
| T8 | Non-`write_socket` packet-send callbacks map UDP send outcomes, with stateless-reset and pre-accept backpressure non-recoverable | Create driver and peer socket; set `ud = odin_xqc_udp_xqc_user_data(xu)`; call installed `write_socket_ex(123, "ex", 2, peer_addr, peer_len, ud)`, installed `stateless_reset("sr", 2, peer_addr, peer_len, local_addr, local_len, ud)`, and installed `conn_send_packet_before_accept("pa", 2, peer_addr, peer_len, ud)` for successful sends. In fresh started drivers, inject one `EAGAIN` before a `write_socket_ex` call, one before a `stateless_reset` call, and one before a `conn_send_packet_before_accept` call; also call each callback with a 70000-byte buffer | Success calls each return `2`, and the peer receives payloads `"ex"`, `"sr"`, and `"pa"`; `write_socket_ex` retryable failure returns `XQC_SOCKET_EAGAIN` and sets READ|WRITE interest; `stateless_reset` and `conn_send_packet_before_accept` retryable failures return `XQC_SOCKET_ERROR`, leave `odin_xqc_udp_test_write_blocked(xu)` false, and leave UDP interest without WRITE; every oversized send returns `XQC_SOCKET_ERROR`; `path_id` and `local_addr` do not change the destination because this RFC owns one UDP endpoint | G4 | integration |
| T9 | Engine-create rollback stops a creation-time xquic timer | Configure fake `engine_create` to record the callback tables, call the installed `set_event_timer(60000000, xu)`, and then return `NULL`; preset `odin_xqc_udp_t *xu = (odin_xqc_udp_t *)-1`; snapshot event-loop liveness before/after; call `odin_xqc_udp_create` | Create returns `-1` with `errno == EIO`, leaves `xu` at the sentinel, closes the UDP fd opened before fake engine failure, leaves no active UDP watch or timer in event-loop liveness, and calls no fake `engine_destroy` because no engine was returned | G5 | unit |
| T10 | Destroy from packet-process callback is deferred | Fake `packet_process` calls `odin_xqc_udp_destroy(xu)` and asserts fake `engine_destroy_calls == 0` before returning; send one packet and run | `odin_xqc_udp_test_destroy_requested(xu)` becomes true during fake packet processing; `finish_recv` is not called after destroy request; fake `engine_destroy` is called exactly once only after fake `packet_process` returns; ASan reports no use-after-free | G5, S2 | integration |
| T11 | Destroy from timer main-logic callback is deferred | Fake `main_logic` calls `odin_xqc_udp_destroy(xu)` and asserts fake `engine_destroy_calls == 0` before returning; schedule `set_event_timer(0, xu)` and run | Fake `main_logic` is called once; fake `engine_destroy` is called exactly once after `main_logic` returns; pending timer state is cleared; ASan reports no use-after-free | G3, G5, S2 | unit |
| T12 | Stop suppresses UDP receive and pending timer triggers | Create/start driver; schedule `set_event_timer(60000000, xu)`; call `odin_xqc_udp_stop`; send one UDP packet to the endpoint; advance fake time past the timer and run the loop with a watchdog task that stops it | No fake `packet_process`, `finish_recv`, or `main_logic` call occurs while stopped; `odin_xqc_udp_start` can be called again afterward and re-arms READ interest | G1, G2, G3 | integration |
| T13 | Start failure preserves stopped state | macOS-only row using RFC-010 kqueue hooks; create driver but do not start it; get the owned UDP endpoint through `odin_xqc_udp_test_udp`; inject one backend READ-watch registration failure through `odin_event_loop_test_fail_next_kqueue_change(loop, ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_ADD, ODIN_EVENT_READ, ENOSPC)`; call `odin_xqc_udp_start`, then clear the fault and call `odin_xqc_udp_start` again | First `start` returns `-1` with `errno == ENOSPC`, `odin_udp_test_io` reports no active watch after the failure, and no READ callback can fire; the second `start` returns `0` and arms exactly one UDP READ watch, proving the failed start did not set `started` | G1 | unit |
| T14 | Destroy while started tears down live watch and timer immediately | Create/start driver; schedule `set_event_timer(60000000, xu)`; record the UDP fd, bound address, and event-loop liveness; call `odin_xqc_udp_destroy(xu)` outside any xquic callback; send one UDP packet to the previously bound address, advance fake time past the old timer deadline, and run the loop with a watchdog task that stops it | Fake `engine_destroy` is called before `destroy` returns; the recorded UDP fd is closed; event-loop liveness shows no active UDP watch or timer after destroy; no fake `packet_process`, `finish_recv`, or `main_logic` call occurs after the later packet/time advance; ASan reports no use-after-free | G5 | integration |
| T15 | Destroy from finish-recv callback is deferred | Fake `finish_recv` calls `odin_xqc_udp_destroy(xu)` and asserts fake `engine_destroy_calls == 0` before returning; fake `packet_process` records one packet without requesting destroy; send one packet and run with ASan | Fake `finish_recv` is called once after packet processing; `odin_xqc_udp_test_destroy_requested(xu)` becomes true during fake finish-receive; fake `engine_destroy` is called exactly once only after fake `finish_recv` returns; the readiness callback performs no wrapper reads after `leave_xqc` reports destruction; ASan reports no use-after-free | G2, G5, S2 | integration |
| T16 | UDP WRITE readiness continues registered CIDs after armed send backpressure and defers destroy from continue-send | Create/start driver and a plain UDP peer socket; register one copied fake `xqc_cid_t` with `odin_xqc_udp_register_conn`; inject `EAGAIN` through `odin_udp_test_fail_next_sendto`; call installed `write_socket("blocked", 7, peer_addr, peer_len, odin_xqc_udp_xqc_user_data(xu))`; dispatch `ODIN_UDP_WRITE` readiness through the owned UDP watch without injecting event-loop update failure; fake `conn_continue_send` unregisters that CID to satisfy the destroy precondition, calls `odin_xqc_udp_destroy(xu)`, and asserts fake `engine_destroy_calls == 0` before returning. In a second fresh driver, repeat the EAGAIN/WRITE setup with two registered CIDs; fake `conn_continue_send` records each CID and, for the first CID, calls the installed `write_socket("retry", 5, peer_addr, peer_len, xu)` | Destroy pass: the blocked send returns `XQC_SOCKET_EAGAIN`; UDP interest includes READ|WRITE before dispatch; WRITE interest is cleared before fake `conn_continue_send` is entered; `odin_xqc_udp_test_destroy_requested(xu)` becomes true during fake continue-send; fake `engine_destroy` is called exactly once only after fake `conn_continue_send` returns; the readiness callback performs no wrapper reads after `leave_xqc` reports destruction; ASan reports no use-after-free. Recovery pass: fake `conn_continue_send` is called once for each still-registered CID using `xqc_conn_continue_send` semantics; the peer receives `"retry"` from the fake xquic retry; after the successful retry pass, UDP interest keeps READ and clears WRITE | G1, G4, G5, S2 | integration |
| T17 | Timer main-logic reschedule does not overlap the fired one-shot timer | Fake `engine_create` records `set_event_timer`; create driver; call recorded `set_event_timer(0, xu)`; fake `main_logic` asserts `odin_event_loop_test_live_timer_count(loop) == 0`, then calls recorded `set_event_timer(700, xu)` before returning; run the loop once and inspect wait preparation | Fake `main_logic` is called once; during `main_logic` the fired timer handle is already stopped; after the nested reschedule, `odin_event_loop_test_live_timer_count(loop) == 1`, `odin_xqc_udp_test_timer_active(xu)` is true, and wait preparation reports a single 700 us one-shot timer rather than both the fired timer and the replacement | G3 | unit |
| T18 | WRITE-interest reconciliation failure is not reported as recoverable send progress | macOS-only row using RFC-010 kqueue hooks. Arming failure pass: create/start driver, register one fake CID, inject one `ODIN_UDP_AGAIN` send result and one event-loop WRITE-add failure via `odin_event_loop_test_fail_next_kqueue_change(loop, ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_ADD, ODIN_EVENT_WRITE, ENOSPC)`, then call installed `write_socket`. Clearing failure pass in a fresh driver: arm READ|WRITE with a successful `write_socket` EAGAIN path, inject a WRITE-delete failure via `odin_event_loop_test_fail_next_kqueue_change(loop, ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_DELETE, ODIN_EVENT_WRITE, ENOSPC)`, dispatch `ODIN_UDP_WRITE`, then clear the fault and dispatch `ODIN_UDP_WRITE` again | Arming failure pass: `write_socket` returns `XQC_SOCKET_ERROR`, `errno`/test state records `ENOSPC`, the active UDP watch remains READ-only, `odin_xqc_udp_test_write_blocked(xu)` is false, and fake `conn_continue_send` is not called. Clearing failure pass: the failed WRITE-clear dispatch records `ENOSPC`, leaves READ|WRITE interest and `write_blocked` intact, and does not call fake `conn_continue_send`; the later successful dispatch clears WRITE before calling fake `conn_continue_send` once for the registered CID | G4 | unit |
| T19 | `recv_time` is sourced from the same function xquic uses as `xqc_monotonic_timestamp` | Default-`monotonic_ts` pass: install fake xquic ops whose `now_us` returns a recorded counter; create driver with `config->engine_callbacks->monotonic_ts == NULL`; record `pt_default = odin_xqc_udp_test_engine_monotonic_ts(xu)`; start; advance fake counter to `7000`; send one UDP datagram; let `packet_process` record `recv_time` and stop the loop after `finish_recv`. Caller-supplied pass: fresh driver where the config supplies `monotonic_ts = caller_clock` (returns a separate recorded counter); record `pt_caller = odin_xqc_udp_test_engine_monotonic_ts(xu)`; start; advance the caller counter to `9000`; send one UDP datagram; record `recv_time` | Default pass: `pt_default != NULL`, `pt_default == odin_xqc_udp_default_monotonic_us` (the driver-installed default, which the §3.2.5 hook also feeds with the test `now_us`), and the recorded `recv_time == pt_default() == 7000`. Caller pass: `pt_caller == caller_clock`, the recorded `recv_time == pt_caller() == 9000`, and the caller's `monotonic_ts` is the function `xqc_engine_create -> xqc_engine_set_callback` installs into `xqc_monotonic_timestamp` — proving `pkt_recv_time` and `xqc_monotonic_timestamp()` share one source in both modes | G2 | integration |
| T20 | `set_event_timer` records `errno` and clears when a later call succeeds | Fake `engine_create` records `set_event_timer`; create driver; with no pending timer, install an `odin_event_timer_start` fault that returns `-1` / `ENOMEM` (via the RFC-010 test hook); call recorded `set_event_timer(1000, xu)`; observe state; clear the fault; call recorded `set_event_timer(0, xu)`; run the loop with fake `main_logic` stopping the loop | First call leaves `xu.timer == NULL`, `odin_xqc_udp_test_timer_active(xu) == 0`, `odin_xqc_udp_test_last_timer_errno(xu) == ENOMEM`, and no fake `main_logic` call; the second call returns successfully, `odin_xqc_udp_test_timer_active(xu)` becomes true, and fake `main_logic` is called exactly once after time advances, proving the next `set_event_timer` clears the stuck state | G3 | unit |

## 6. Implementation Plan

- **P1. Land the xqc UDP driver surface and red-verifiable T1-T20.**
  - **Scope:** add `odin/xqc_udp.h` with the section 3.2.1 public declarations and doc-comments; add `odin/xqc_udp_internal_test.h` with the section 3.2.5 hooks gated by `ODIN_XQC_UDP_TESTING`, including `odin_xqc_udp_test_last_timer_errno` and `odin_xqc_udp_test_engine_monotonic_ts`; add `odin/xqc_udp.c` with a bounded stub that allocates `odin_xqc_udp_t`, opens a real `odin_udp_t`, snapshots `odin_udp_local_addr`, copies callbacks, calls the fake or real `xqc_engine_create`, and rolls back on local setup failure, but intentionally leaves behavior wrong for the §5 assertions: `start` arms UDP READ enough for T2-T4/T10/T15 receive dispatch, but records `started` even when `odin_udp_set_interest` fails, leaves `stop` incomplete, and never adds pending WRITE from `start`; the stub does not install a default `monotonic_ts` and fills `recv_time` from a fixed constant rather than `xu.engine_callbacks.monotonic_ts()`; `set_event_timer` creates a zero-delay timer regardless of `wake_after`, enters fake `main_logic` without stopping the fired handle first, and never records `last_timer_errno` when timer install fails; engine-create rollback and outside-callback destroy intentionally do not stop that timer; send callbacks call `odin_udp_send` only enough to exercise success delivery and `ODIN_UDP_AGAIN` WRITE readiness, but still return `XQC_SOCKET_ERROR` on successful sends, treat stateless-reset and pre-accept backpressure as retryable, and mishandle nonretryable/short-send details; WRITE readiness reaches fake `conn_continue_send` for T16, but WRITE interest cleanup, event-loop update-failure handling, and CID filtering are incomplete; packet receive enters fake `packet_process` and fake `finish_recv`, but passes deliberately wrong local/timestamp data and has no batch-cap correctness; and in-callback `destroy` from packet, finish-receive, timer, or continue-send calls fake `engine_destroy` immediately. Add `odin/xqc_udp_testing.c` containing `#include "xqc_udp.c" // NOLINT(bugprone-suspicious-include)`. Extend `odin/udp.h` and `odin/udp.c` with `odin_udp_local_addr`. Add `odin/xqc_udp_unittests.cpp` implementing T1-T20, each skipped by default unless `ODIN_XQC_UDP_RED=1` is present. In `odin/BUILD.gn`, add `config("odin_xqc_udp_testing_config") { defines = [ "ODIN_XQC_UDP_TESTING" ] }`, add test-only `xqc_udp_testing.c`, `xqc_udp_internal_test.h`, and `xqc_udp_unittests.cpp` to `odin_unittests`, and keep production `:odin` without an `:odin_xqc_udp` dependency in this phase.
  - **Depends on:** None.
  - **Done when:** `./tool/gn gen out/xqc_udp_mac --args='target_os="mac"'`, `./tool/gn gen out/xqc_udp_linux_x64 --args='target_os="linux" target_cpu="x64"'`, `./tool/ninja -C out/xqc_udp_mac odin_main odin_unittests tests`, and `./tool/ninja -C out/xqc_udp_linux_x64 odin_main odin_unittests tests` succeed. The red-verification command `ODIN_XQC_UDP_RED=1 out/xqc_udp_mac/odin_unittests --gtest_filter='OdinXqcUdpTest.*'` executes T1-T20 and reports them failing against the stub: T1 at incomplete stop/watch cleanup, callback replacement, or helper assertions; T2 and T3 at wrong packet-process arguments; T4 at missing batch-cap behavior; T5 and T6 at wrong timer scheduling; T7 and T8 at send callbacks returning `XQC_SOCKET_ERROR` on successful sends, failing failure mapping, or treating stateless-reset/pre-accept backpressure as recoverable; T9 at the leaked creation-time timer after fake `engine_create` calls `set_event_timer(60000000, xu)` and returns `NULL`; T10, T11, and T15 because fake `engine_destroy` runs before fake xquic callbacks return; T12 because stop/start state is incomplete; T13 because failed READ-watch registration is not propagated and the failed start leaves `started` set; T14 because live outside-callback destroy leaks the pending timer; T16 because the red WRITE path reaches fake `conn_continue_send` and fake `engine_destroy` runs before that fake callback returns, with WRITE interest cleanup still wrong; T17 because the fired one-shot timer remains active while fake `main_logic` reschedules; T18 because the stub returns `XQC_SOCKET_EAGAIN` after failed WRITE arming and enters fake `conn_continue_send` even when clearing WRITE interest fails; T19 because the stub never installs a default `monotonic_ts` (so `odin_xqc_udp_test_engine_monotonic_ts` returns the caller-supplied null in the default pass and the helper-vs-`recv_time` equality fails) and the recorded `recv_time` does not match the fake `now_us` counter in either pass; and T20 because the stub records nothing in `last_timer_errno` after a timer-start fault and may not even propagate the fault to keep `xu.timer == NULL`. The default `out/xqc_udp_mac/odin_unittests --gtest_brief=1` reports T1-T20 skipped and exits zero with pre-existing suites green; the Linux binary cross-compiles but is not run.

- **P2. Implement the driver and turn T1-T20 green.**
  - **Scope:** replace the P1 stub in `odin/xqc_udp.c` with the sections 3.2.1-3.2.4 implementation: real callback copying and override, default `monotonic_ts` install when the caller leaves it null and a `monotonic_us(xu)` helper that reads from `xu.engine_callbacks.monotonic_ts` so `recv_time` and `xqc_monotonic_timestamp()` share one source, sendmmsg rejection, UDP open plus `odin_udp_local_addr`, xquic engine creation with `user_data = xu`, registered-CID storage with register/unregister, `start` failure propagation and successful READ plus pending-WRITE interest arming, `stop` interest/timer/write-blocked handling, bounded receive loop with `xqc_engine_packet_process` and once-per-nonempty-batch `xqc_engine_finish_recv`, `set_event_timer` start/reset behavior including `last_timer_errno` capture and `xu.timer == NULL` on the terminal both-failed branch, timer callback that stops the fired one-shot before `xqc_engine_main_logic`, `stateless_reset` / `write_socket` / `write_socket_ex` / pre-accept send mapping through `odin_udp_send`, recoverable `ODIN_UDP_AGAIN` WRITE re-arming only for `write_socket` / `write_socket_ex` and only when `odin_udp_set_interest` succeeds, `stateless_reset` and `conn_send_packet_before_accept` backpressure returning `XQC_SOCKET_ERROR` without WRITE recovery, WRITE readiness recovery through `xqc_conn_continue_send` over registered CIDs only after WRITE interest is cleared, failure-closed handling for WRITE add/delete update failures, creation-failure rollback that stops a timer installed during `xqc_engine_create`, outside-callback destroy that immediately removes live UDP watch/timer surfaces before engine destroy/free subject to the documented caller preconditions, and callback-depth-based destroy deferral for packet, finish-receive, timer, and continue-send entry points. Add production `source_set("odin_xqc_udp") { sources = [ "xqc_udp.c", "xqc_udp.h" ]; public_deps = [ ":odin_event_loop", ":odin_udp", "//xquic" ] }` and append it to `source_set("odin")` deps; keep `odin_unittests` compiling `xqc_udp_testing.c` under `ODIN_XQC_UDP_TESTING` without depending on production `:odin_xqc_udp`. Remove the `ODIN_XQC_UDP_RED` skip wrappers so T1-T20 assert in the default macOS test run.
  - **Depends on:** P1.
  - **Done when:** the P1 build commands still succeed; `out/xqc_udp_mac/odin_unittests --gtest_filter='OdinXqcUdpTest.*'` passes T1-T20 un-gated, including T1's engine/UDP/timer ownership and helper observations, T2's exact packet/peer/local/timestamp/user-data forwarding, T3's maximum-size payload forwarding under ASan, T4's 64-packet batch cap, T5-T6's timer fire/reset behavior, T7-T8's UDP send callback success, recoverable-backpressure mapping for `write_socket` / `write_socket_ex`, stateless-reset and pre-accept backpressure returning `XQC_SOCKET_ERROR` without WRITE recovery, and non-retryable-failure mapping for all four packet-send callbacks, T9's creation-time timer rollback cleanup, T10-T11 and T15's deferred in-callback destroy, T12's stopped-driver suppression, T13's failed-start errno and stopped-state preservation, T14's immediate live destroy cleanup, T16's WRITE-readiness `xqc_conn_continue_send` recovery after armed EAGAIN and deferred destroy from fake continue-send, T17's nested timer reschedule with only one live timer, T18's WRITE-add failure returning `XQC_SOCKET_ERROR` plus WRITE-clear failure skipping `xqc_conn_continue_send` until a later successful clear, T19's `recv_time` / `xqc_monotonic_timestamp` shared-source equality in both default-`monotonic_ts` and caller-supplied passes, and T20's `last_timer_errno` capture on the both-failed branch plus the cleared state after the next successful `set_event_timer` call. The unfiltered default macOS run `out/xqc_udp_mac/odin_unittests --gtest_brief=1` exits zero with pre-existing suites green. `./tool/gn gen out/xqc_udp_mac_asan --args='target_os="mac" is_asan=true'`, `./tool/ninja -C out/xqc_udp_mac_asan odin_unittests`, and `out/xqc_udp_mac_asan/odin_unittests --gtest_filter='OdinXqcUdpTest.*'` exit with no AddressSanitizer report, covering S1's receive buffer, S2's deferred-destroy no-use-after-free assertions including WRITE-recovery continue-send, T14's live outside-callback destroy cleanup, and T15's no-read-after-`leave_xqc` assertion. Production `out/xqc_udp_mac/odin` and `out/xqc_udp_linux_x64/odin` link `:odin_xqc_udp` through `:odin` and contain no `odin_xqc_udp_test_*` symbols; `./tidy_odin.sh` exits clean for the new and modified Odin files.
