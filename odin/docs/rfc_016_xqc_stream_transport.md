# RFC-016: xqc_stream_t Transport Implementation

## 1. Summary

Add `odin/transport_xqc.c` and `odin/transport_xqc.h`, an `odin_transport_t` implementation over a caller-supplied `xqc_stream_t` that maps xquic stream receive/send/FIN, stream read/write/error callbacks, and a deterministic first-WRITE kick into Odin's byte-stream transport contract, including relay-safe latched EOF recovery, while leaving xquic engine, connection, packet I/O, timers, stream creation, and stream close ownership to the caller.

## 2. Goals

- **G1.** Provide a public factory, `odin_xqc_stream_transport_create(stream, on_ready, user_data, &t)`, that adapts an existing caller-owned `xqc_stream_t *` to `odin_transport_t *`, installs the returned transport as the xquic stream's `user_data`, and releases only the Odin wrapper on `odin_transport_destroy`.

- **G2.** Preserve RFC-013 byte-stream behavior over xquic stream APIs: `odin_transport_read` returns a previously latched EOF without another xquic call, otherwise performs one `xqc_stream_recv` and returns `OK`, `EOF`, `AGAIN`, or `IO_ERROR`; a read callback that returns final bytes with `fin == 1` produces a synthetic READ readiness for the latched EOF either immediately while READ interest remains active or later when READ interest is re-enabled after temporary backpressure; `odin_transport_write` treats zero-length writes as local no-op success, otherwise performs one data `xqc_stream_send` and returns `OK`, `AGAIN`, or `IO_ERROR`; `odin_transport_shutdown_write` sends or queues a FIN without treating xquic write backpressure as a relay-visible failure; `odin_transport_set_interest` gates xquic read/write callback delivery, synchronously delivers pending latched-EOF READ readiness when READ is re-enabled, and synchronously delivers one WRITE readiness when WRITE interest transitions from disabled to enabled so readiness-driven consumers perform the first `xqc_stream_send`; the RFC-014 relay remains safe when those synchronous deliveries re-enter `odin_relay_ready` during interest reconciliation and when `on_done` destroys the relay; and asynchronous xquic stream closing or queued-FIN retry failures surface through `ODIN_TRANSPORT_ERROR` readiness and `odin_transport_error`.

- **G3.** Keep the implementation scoped to xquic stream-level APIs only: the public Odin API does not accept `xqc_engine_t *`, `xqc_connection_t *`, sockets, timers, CIDs, ALPN registration, or packet addresses, and the implementation directly references no xquic engine or connection APIs, UDP I/O, packet I/O, timer APIs, stream creation APIs, stream reset APIs, or stream close APIs.

## 3. Design

### 3.1 Overview

`odin/transport_xqc` is a concrete sibling of `odin/transport_fd`: it embeds `odin_transport_t` as the first member of its private wrapper object and fills the RFC-013 vtable, but its endpoint is an already-created `xqc_stream_t *` instead of an fd. Its GN target re-exports `odin/transport` and the existing `//xquic` target through `public_deps`, because `odin/transport_xqc.h` includes both `odin/transport.h` and `<xquic/xquic.h>` and `//xquic` exports public include/config state through `build/secondary/xquic/BUILD.gn`.

The caller owns all xquic engine and connection integration. For an active stream, the caller creates the `xqc_stream_t` through xquic, then calls `odin_xqc_stream_transport_create`. For a passive server stream, the caller's xquic `stream_create_notify` creates the Odin wrapper. The factory sets the xquic stream's `user_data` to the returned `odin_transport_t *`, so later xquic stream callbacks can either use the callback-compatible helper functions from this RFC or forward into them from caller-owned callbacks. The caller still owns xquic `stream_create_notify` and `stream_close_notify`, because those decide when the wrapper is allocated and destroyed. The caller must destroy the Odin wrapper while the `xqc_stream_t` is still valid, typically from xquic `stream_close_notify` or before any caller-owned teardown step that can let xquic invalidate or free the stream.

Data movement is synchronous at the transport call boundary: `odin_transport_read` pulls already-buffered QUIC stream bytes with `xqc_stream_recv`, latches peer EOF when xquic returns final data and FIN together, and returns that latched EOF on the next read without another xquic receive; non-empty `odin_transport_write` pushes bytes with `xqc_stream_send(..., fin=0)`, while zero-length writes return before calling xquic. Ordinary readiness flows from xquic into Odin: xquic invokes stream read/write/closing callbacks; the callback helper checks the wrapper's current interest mask and invokes the `odin_transport_ready_cb` supplied to the factory. The first WRITE readiness is local and deterministic: when `odin_transport_set_interest` enables WRITE after it was disabled, the wrapper synchronously emits one WRITE callback so a readiness-driven consumer can perform the first `xqc_stream_send`; later xquic `stream_write_notify` callbacks remain the continuation path after that send reports backpressure or partial progress. When a READ callback's consumer read gets final data and FIN in one xquic result, the wrapper keeps `recv_eof_ready_pending` set until it has delivered one synthetic READ readiness for the latched EOF. That synthetic readiness is delivered immediately after the original callback if READ interest is still set; if a relay-style consumer filled its read buffer and cleared READ interest first, the pending bit survives and `odin_transport_set_interest(..., READ)` emits the synthetic READ synchronously when the consumer later drains capacity and re-enables READ. Because RFC-014's relay is such a consumer and `reconcile` calls `odin_transport_set_interest` while updating endpoint interest state, this RFC also updates the relay's interest bookkeeping and destroy deferral so that a synchronous WRITE kick or pending-EOF callback can re-enter `odin_relay_ready` and even complete/destroy the relay without resuming against stale or freed relay state. The wrapper owns no xquic stream and no connection resource. Destroy clears the xquic stream `user_data` slot it installed, then frees the wrapper, deferring the physical free until the current wrapper-delivered `on_ready` returns when destroy is called from inside `on_ready`.

```
caller-owned xquic engine / connection / callback registration
        |
        | creates or receives xqc_stream_t *
        v
odin_xqc_stream_transport_create(stream, on_ready, user_data, &t)
        |
        v
  odin_transport_t wrapper
        | read/write/shutdown_write
        v
  xqc_stream_recv / xqc_stream_send on the existing stream
        ^
        | xquic stream_read_notify / stream_write_notify / stream_closing_notify
        |
  callback helpers gate events through odin_transport_set_interest
        |
        v
  on_ready(t, ODIN_TRANSPORT_READ|WRITE|ERROR, user_data)
```

### 3.2 Detailed Design

#### 3.2.1 Public API, Ownership, and Callback Bridge

```c
/* odin/transport_xqc.h */
#include "odin/transport.h"
#include <xquic/xquic.h>

#ifdef __cplusplus
extern "C" {
#endif

int odin_xqc_stream_transport_create(xqc_stream_t *stream,
                                     odin_transport_ready_cb on_ready,
                                     void *user_data,
                                     odin_transport_t **out);

xqc_int_t odin_xqc_stream_transport_read_notify(xqc_stream_t *stream,
                                                void *strm_user_data);
xqc_int_t odin_xqc_stream_transport_write_notify(xqc_stream_t *stream,
                                                 void *strm_user_data);
void odin_xqc_stream_transport_closing_notify(xqc_stream_t *stream,
                                              xqc_int_t err_code,
                                              void *strm_user_data);

#ifdef __cplusplus
}
#endif
```

**Unstated contract.**

- **Construction and ownership.** `odin_xqc_stream_transport_create` allocates one wrapper, stores `stream`, `on_ready`, and `user_data`, writes `*out`, sets `stream`'s xquic user data to `*out` through `xqc_stream_set_user_data(stream, *out)`, and returns `0`. Its only local failure is `ENOMEM`, in which case it returns `-1`, sets `errno == ENOMEM`, writes nothing to `*out`, and does not call `xqc_stream_set_user_data`. Preconditions: `stream`, `on_ready`, and `out` are non-null; the stream belongs to the current xquic owner thread; and the caller keeps `stream` valid until `odin_transport_destroy(t)` has returned or, for in-callback destroy, until the wrapper-delivered callback that requested destroy has unwound.
- **Stream lifetime ordering.** The wrapper stores a non-owned `xqc_stream_t *` and uses it again during destroy to clear the xquic user-data slot. Therefore the caller must call `odin_transport_destroy(t)` while the `xqc_stream_t` is still valid. The normal xquic integration point is caller-owned `stream_close_notify`, because xquic invokes that callback while the stream pointer and its user data are still available; destroying the Odin wrapper there clears the slot before xquic frees the stream. Destroying the wrapper after xquic has invalidated or freed the stream is a caller precondition violation.
- **Destroy.** Subject to the stream lifetime ordering above, `odin_transport_destroy(t)` clears the xquic stream user data with `xqc_stream_set_user_data(stream, NULL)`. If no wrapper-delivered `on_ready` is active, it frees the wrapper immediately; if destroy is called from inside an `on_ready` delivered by a notify helper or by `set_interest`'s pending-EOF path, it marks `destroy_pending` and the outermost delivery frees the wrapper before returning. No delivery path emits another callback after `destroy_pending` is set. Destroy never calls `xqc_stream_close`, never sends RESET_STREAM, never closes a connection, and never frees an xquic object. `odin_transport_destroy(NULL)` remains the RFC-013 dispatcher no-op.
- **Callback bridge.** The three exported notify helpers match xquic's stream callback signatures. They expect `strm_user_data` to be the `odin_transport_t *` returned by the factory, which is what the factory installs on the stream. If `strm_user_data == NULL`, each helper is a no-op (`read_notify` and `write_notify` return `XQC_OK`, `closing_notify` returns void); this makes a callback after destroy harmless once the user data slot has been cleared.
- **Readiness interest.** `odin_transport_set_interest(t, events)` accepts only a subset of `ODIN_TRANSPORT_READ | ODIN_TRANSPORT_WRITE`. Any `ODIN_TRANSPORT_ERROR` bit or unknown bit returns `-1` with `errno == EINVAL` and preserves the previous interest mask. A valid mask stores the new interest and returns `0`; there is no event-loop watch to start or stop because xquic owns ordinary readiness dispatch. `set_interest` has two deterministic synchronous side effects after accepting the new mask. First, if the new mask includes READ, `recv_eof_ready_pending` is set, no READ callback is currently unwinding, and destroy has not been requested, `set_interest` clears the pending bit and delivers one synthetic `on_ready(t, ODIN_TRANSPORT_READ, user_data)` before returning. This path is the relay backpressure recovery path: when final data filled the relay ring, READ interest may be off before `read_notify` regains control; after later WRITE progress drains the ring, the relay re-enables READ and receives the latched EOF without waiting for another xquic notification. Second, if WRITE transitions from disabled in the previous mask to enabled in the new mask, and destroy has not been requested, `set_interest` delivers one `on_ready(t, ODIN_TRANSPORT_WRITE, user_data)` before returning. This WRITE kick performs no xquic call itself; it exists so a readiness-driven consumer can call `odin_transport_write`, whose non-empty send invokes `xqc_stream_send` and lets xquic arm any later `stream_write_notify` continuation. Repeating `set_interest` while WRITE is already enabled does not emit another WRITE kick. If both synchronous side effects are armed by one call, pending EOF READ is delivered first, then the WRITE kick is delivered only if the wrapper is still alive and WRITE remains enabled. Because these callbacks run before `set_interest` returns, consumers that call `set_interest` from inside readiness handling must tolerate owner-thread re-entry; section 3.2.3 pins the relay changes required for the only in-tree consumer that has that shape.
- **Notify behavior.** `read_notify` invokes `on_ready(t, ODIN_TRANSPORT_READ, user_data)` only when READ interest is set. During that callback, a `ret > 0 && fin == 1` read latches EOF and sets `recv_eof_ready_pending`; after the callback returns, `read_notify` invokes `deliver_pending_eof_ready_if_armed`. That helper clears the pending bit and invokes one additional `on_ready(t, ODIN_TRANSPORT_READ, user_data)` only if READ interest is currently set and destroy was not requested. If READ interest was cleared before `read_notify` regains control, the pending bit remains set and no synthetic READ is emitted while interest is off; the later `set_interest(..., READ)` path above delivers it. Synthetic READ readiness performs no xquic call itself, and it exists only to let readiness-driven consumers call `odin_transport_read` again and observe the latched EOF. `write_notify` first retries any pending FIN from `shutdown_write`, then invokes `on_ready(t, ODIN_TRANSPORT_WRITE, user_data)` only when WRITE interest is set; it is a continuation after an attempted `xqc_stream_send`, not the source of the first data send. `closing_notify` maps the xquic stream failure to `EPIPE`, latches that value for `odin_transport_error`, and invokes `on_ready(t, ODIN_TRANSPORT_ERROR, user_data)` regardless of the READ/WRITE interest mask.
- **Threading.** All entry points and callback helpers run on the xquic stream owner thread. The wrapper adds no locks and does not make xquic calls from any other thread.

**Mechanism.**

```
struct odin_xqc_stream_transport_t:
    odin_transport_t        base        # first member, RFC-013 convention
    xqc_stream_t           *stream      # not owned
    odin_transport_ready_cb on_ready
    void                   *user_data
    unsigned int            interest    # READ|WRITE subset
    int                     err         # errno-style async error, 0 when none
    int                     recv_eof_latched
    int                     recv_eof_ready_pending
    int                     read_ready_depth
    int                     callback_depth
    int                     destroy_pending
    int                     fin_pending
    int                     fin_sent

create(stream, on_ready, user_data, out):
    s = calloc(1, sizeof *s)
    if s == NULL: errno = ENOMEM; return -1
    s.base.vt = &xqc_stream_transport_vtable
    s.stream = stream; s.on_ready = on_ready; s.user_data = user_data
    xqc_stream_set_user_data(stream, &s.base)
    *out = &s.base
    return 0

destroy(t):  # precondition: s.stream still names a valid xqc_stream_t
    s = (odin_xqc_stream_transport_t *)t
    xqc_stream_set_user_data(s.stream, NULL)
    if s.callback_depth != 0:
        s.destroy_pending = 1
        return
    free(s)

end_callback(s):
    s.callback_depth -= 1
    if s.callback_depth == 0 and s.destroy_pending:
        free(s)
        return destroyed
    return alive

deliver_ready(s, events):
    s.callback_depth += 1
    s.on_ready(&s.base, events, s.user_data)
    return end_callback(s)

deliver_pending_eof_ready_if_armed(s):
    if s.destroy_pending:
        return alive
    if !s.recv_eof_ready_pending:
        return alive
    if !(s.interest has READ):
        return alive
    if s.read_ready_depth != 0:
        return alive
    s.recv_eof_ready_pending = 0
    return deliver_ready(s, ODIN_TRANSPORT_READ)

deliver_write_kick_if_armed(s, old_interest):
    if s.destroy_pending:
        return alive
    if old_interest has WRITE:
        return alive
    if !(s.interest has WRITE):
        return alive
    return deliver_ready(s, ODIN_TRANSPORT_WRITE)

set_interest(t, events):
    if events has any bit outside READ|WRITE:
        errno = EINVAL; return -1
    s = (odin_xqc_stream_transport_t *)t
    old = s.interest
    s.interest = events
    if events has READ:
        if deliver_pending_eof_ready_if_armed(s) == destroyed:
            return 0
    if events has WRITE:
        if deliver_write_kick_if_armed(s, old) == destroyed:
            return 0
    return 0

read_notify(stream, strm_user_data):
    if strm_user_data == NULL: return XQC_OK
    s = (odin_xqc_stream_transport_t *)strm_user_data
    if s.interest has READ:
        s.callback_depth += 1
        s.read_ready_depth += 1
        s.on_ready(&s.base, ODIN_TRANSPORT_READ, s.user_data)
        s.read_ready_depth -= 1
        if end_callback(s) == destroyed:
            return XQC_OK
        deliver_pending_eof_ready_if_armed(s)
    return XQC_OK

write_notify(stream, strm_user_data):
    if strm_user_data == NULL: return XQC_OK
    s = (odin_xqc_stream_transport_t *)strm_user_data
    if s.fin_pending:
        retry_pending_fin(s)
        if s.err != 0:
            deliver_ready(s, ODIN_TRANSPORT_ERROR)
            return XQC_OK
    if s.interest has WRITE:
        deliver_ready(s, ODIN_TRANSPORT_WRITE)
    return XQC_OK

closing_notify(stream, err_code, strm_user_data):
    if strm_user_data == NULL: return
    s = (odin_xqc_stream_transport_t *)strm_user_data
    s.err = EPIPE
    deliver_ready(s, ODIN_TRANSPORT_ERROR)
```

Satisfies: G1 via the factory, first-member `odin_transport_t` wrapper, xquic user-data install/clear, explicit stream-valid-until-destroy lifetime precondition, deferred in-callback free, and wrapper-only destroy behavior; G2 via the interest-gated callback helpers, the synchronous WRITE kick on WRITE-interest enablement, synthetic READ readiness for a latched final-data EOF both while READ remains active and after READ is later re-enabled, and `ODIN_TRANSPORT_ERROR` propagation; G3 via the destroy-without-`xqc_stream_close` ownership rule and a public surface that accepts only `xqc_stream_t *` plus stream callback parameters and no engine, connection, socket, timer, CID, ALPN, or packet I/O objects.

#### 3.2.2 Stream I/O and FIN Mapping

The contract surface for this aspect is the xqc transport vtable installed by `odin_xqc_stream_transport_create` in section 3.2.1. Its slots map onto the existing xquic public stream functions `xqc_stream_recv` and `xqc_stream_send`, whose current signatures and return contracts are verified at `xquic/include/xquic/xquic.h:2024` and `xquic/include/xquic/xquic.h:2033`.

```
odin_transport_read(t, buf, len, out_n)  -> latched EOF or xqc_stream_recv(stream, buf, len, &fin)
odin_transport_write(t, buf, len, out_n) -> len == 0 ? local OK : xqc_stream_send(stream, buf, len, 0)
odin_transport_shutdown_write(t)         -> xqc_stream_send(stream, NULL, 0, 1)
odin_transport_error(t)                  -> latched asynchronous stream error
```

**Unstated contract.**

- **Read.** `odin_transport_read(t, buf, len, &n)` first checks `recv_eof_latched`; when set, it makes no xquic call and returns `ODIN_TRANSPORT_EOF` with `*out_n = 0`. Otherwise it calls `xqc_stream_recv(stream, buf, len, &fin)` exactly once. `ret > 0 && fin == 0` returns `ODIN_TRANSPORT_OK` with `*out_n = ret`. `ret > 0 && fin == 1` also returns `ODIN_TRANSPORT_OK` with `*out_n = ret`, but first latches `recv_eof_latched = 1` so the next `odin_transport_read` returns EOF without another xquic receive; when this read occurs inside a `read_notify`-delivered READ callback, it also sets `recv_eof_ready_pending = 1`, causing `read_notify` or a later READ-enabling `set_interest` call to deliver the synthetic READ readiness described in section 3.2.1. The pending bit is cleared only when that synthetic READ is delivered or the wrapper is destroyed, so relay backpressure cannot lose EOF by temporarily clearing READ interest. This branch is required because xquic can set `fin` on a positive final-data return (`xquic/src/transport/xqc_stream.c:1444-1467`). A direct caller that is not running inside `read_notify` observes the latch by calling `odin_transport_read` again; the transport does not synthesize a callback for a caller that did not enter through xquic readiness. `ret == 0 && fin == 1` latches `recv_eof_latched = 1` and returns `ODIN_TRANSPORT_EOF` with `*out_n = 0`; this is the QUIC FIN-only read path. `ret == -XQC_EAGAIN` returns `ODIN_TRANSPORT_AGAIN`. Any other `ret < 0` returns `ODIN_TRANSPORT_IO_ERROR` and sets `errno` to the mapped value without updating `odin_transport_error(t)`, because RFC-013 defines `error` as an asynchronous-error probe. A defensive `ret > len` check treats the call as `ODIN_TRANSPORT_IO_ERROR` with `errno == EIO` before writing `*out_n`; xquic should not produce that result, but the wrapper must not report an impossible byte count to callers.
- **Write.** `odin_transport_write(t, buf, len, &n)` treats `len == 0` as a local no-op success: it writes `*out_n = 0`, returns `ODIN_TRANSPORT_OK`, and does not call `xqc_stream_send`, because current xquic does not define an executable zero-data non-FIN send path. For `len > 0`, it calls `xqc_stream_send(stream, (unsigned char *)buf, len, 0)` exactly once. `ret > 0` returns `ODIN_TRANSPORT_OK` with `*out_n = ret`; xquic may send fewer than `len` bytes, and RFC-014 relay backpressure already retries the remainder on later WRITE readiness. `ret == -XQC_EAGAIN` returns `ODIN_TRANSPORT_AGAIN`. Any other `ret < 0` returns `ODIN_TRANSPORT_IO_ERROR` and sets `errno` to the mapped value without updating `odin_transport_error(t)`. For `len > 0`, `ret == 0` is treated as `ODIN_TRANSPORT_AGAIN` because no byte progressed and no hard error was reported.
- **Error mapping.** `-XQC_ESTREAM_RESET` and `-XQC_CLOSING` map to `EPIPE`, because the stream is no longer usable for byte transfer. Other non-`-XQC_EAGAIN` negative xquic returns map to `EIO`. Synchronous read, write, and initial FIN-send failures report that mapped value only through the immediate return and `errno`; they do not change the asynchronous error latch. `odin_transport_error(t)` returns the latched asynchronous stream error and does not clear it; `0` means no asynchronous xquic stream closing or queued-FIN retry failure is latched.
- **Half-close.** `odin_transport_shutdown_write(t)` sends a FIN-only block with `xqc_stream_send(stream, NULL, 0, 1)`. If that call returns `>= 0`, FIN is marked sent and the function returns `0`. If it returns `-XQC_EAGAIN`, FIN is marked pending and the function still returns `0`, so RFC-014's relay does not convert transient xquic write backpressure into a failed half-close. If the initial FIN send returns any other negative value, `shutdown_write` returns `-1` with mapped `errno` and does not update `odin_transport_error(t)`. The next `write_notify` retries the pending FIN before notifying caller WRITE readiness. If the FIN retry returns `-XQC_EAGAIN`, it remains pending; if it returns `>= 0`, it becomes sent; any other negative return latches an asynchronous error and emits `ODIN_TRANSPORT_ERROR`. A second `shutdown_write` after `fin_pending` or `fin_sent` is idempotent and returns `0`.

**Mechanism.**

```
map_xqc_error(ret):
    if ret == -XQC_ESTREAM_RESET or ret == -XQC_CLOSING: return EPIPE
    return EIO

read(t, buf, len, out_n):
    if s.recv_eof_latched:
        *out_n = 0; return ODIN_TRANSPORT_EOF
    fin = 0
    ret = xqc_stream_recv(s.stream, buf, len, &fin)
    if ret > 0:
        if ret > len:
            errno = EIO; return ODIN_TRANSPORT_IO_ERROR
        if fin == 1:
            s.recv_eof_latched = 1
            if s.read_ready_depth != 0:
                s.recv_eof_ready_pending = 1
        *out_n = ret; return ODIN_TRANSPORT_OK
    if ret == 0 and fin == 1:
        s.recv_eof_latched = 1
        *out_n = 0; return ODIN_TRANSPORT_EOF
    if ret == -XQC_EAGAIN or (ret == 0 and fin == 0):
        return ODIN_TRANSPORT_AGAIN
    errno = map_xqc_error(ret); return ODIN_TRANSPORT_IO_ERROR

write(t, buf, len, out_n):
    if len == 0:
        *out_n = 0; return ODIN_TRANSPORT_OK
    ret = xqc_stream_send(s.stream, (unsigned char *)buf, len, 0)
    if ret > 0:
        if ret > len:
            errno = EIO; return ODIN_TRANSPORT_IO_ERROR
        *out_n = ret; return ODIN_TRANSPORT_OK
    if ret == -XQC_EAGAIN or ret == 0:
        return ODIN_TRANSPORT_AGAIN
    errno = map_xqc_error(ret); return ODIN_TRANSPORT_IO_ERROR

shutdown_write(t):
    if s.fin_sent or s.fin_pending: return 0
    ret = xqc_stream_send(s.stream, NULL, 0, 1)
    if ret >= 0: s.fin_sent = 1; return 0
    if ret == -XQC_EAGAIN: s.fin_pending = 1; return 0
    errno = map_xqc_error(ret); return -1

retry_pending_fin(s):
    if !s.fin_pending: return
    ret = xqc_stream_send(s.stream, NULL, 0, 1)
    if ret >= 0: s.fin_pending = 0; s.fin_sent = 1; return
    if ret == -XQC_EAGAIN: return
    s.fin_pending = 0
    s.err = map_xqc_error(ret)
```

Satisfies: G2 via the latched EOF or one-call read mapping, final-data-plus-FIN handling with a readiness-delivered EOF observation path, FIN-only EOF handling, local zero-length write success without a fake xquic send, one-call non-empty write mapping, transient xquic backpressure mapping to `AGAIN` or pending FIN rather than hard failure, synchronous error reporting through return values and `errno`, and asynchronous queued-FIN error exposure through `ODIN_TRANSPORT_ERROR` plus `odin_transport_error`.

#### 3.2.3 Relay Re-entry During Interest Reconciliation

The contract surface for this aspect is the existing relay callback and destroy API, plus the internal `reconcile` path that calls `odin_transport_set_interest`; current signatures are in `odin/relay.h:72`, `odin/relay.h:82`, and `odin/relay.h:91`, and the current post-call `cur` assignment that must change is in `odin/relay.c:144`.

```
void odin_relay_ready(odin_transport_t *t, unsigned int events,
                      void *user_data);
void odin_relay_destroy(odin_relay_t *relay);

reconcile(relay, endpoint):
    old = endpoint.cur
    next = recomputed READ|WRITE mask
    endpoint.cur = next before odin_transport_set_interest(endpoint.t, next)
```

**Unstated contract.**

- **Synchronous transport callbacks.** A transport implementation may invoke its registered `odin_transport_ready_cb` synchronously from inside a successful `odin_transport_set_interest` call. The xqc stream transport uses this allowance for the WRITE kick after accepting a new WRITE interest mask and for the latched EOF recovery path after accepting a new READ interest mask, as pinned in section 3.2.1.
- **Interest state visibility.** Before calling `odin_transport_set_interest(e->t, m)`, `reconcile` writes `e->cur = m`. A nested `odin_relay_ready` therefore observes the relay's interest state as matching the mask the transport just accepted. If `odin_transport_set_interest` fails without invoking the callback, `reconcile` restores the old `cur`, records `errno`, and marks the relay failed. The xqc transport's invalid-mask failure path never invokes a callback.
- **Nested completion.** `odin_relay_ready` increments a relay `active_depth` before reading or writing relay state and decrements it on exit. `odin_relay_destroy` frees immediately only when `active_depth == 0`; otherwise it sets `destroy_pending` and leaves the object alive until the outermost `odin_relay_ready` returns. `teardown` sets `torn_down`, stops both transport interests, writes both endpoint `cur` fields to `0`, calls `on_done` once, and performs no user-visible callback after that. If `on_done` calls `odin_relay_destroy`, the destroy request is deferred when any relay frame is still active.
- **Outer-frame unwind.** After any `odin_transport_set_interest` call returns, the caller checks `torn_down` or `destroy_pending` before touching direction buffers, endpoint fields, or outcome state. An outer relay frame that was interrupted by a nested pending-EOF callback may only unwind when the nested frame completed the relay.
- **Ownership remains unchanged.** The relay still never destroys either transport and never closes fd or xquic resources. The lifetime guard protects only the relay object and its two buffers.

**Mechanism.**

```
relay_enter(r):
    r.active_depth += 1

relay_leave(r):
    r.active_depth -= 1
    if r.active_depth == 0 and r.destroy_pending:
        free relay buffers and relay object
        return destroyed
    return alive

reconcile(r, e):
    if r.torn_down or r.destroy_pending:
        return
    m = 0
    if e.src.read_eof == 0 and e.src.len < CAP: m |= READ
    if e.sink.len > 0: m |= WRITE
    if m == e.cur:
        return
    old = e.cur
    e.cur = m
    if odin_transport_set_interest(e.t, m) != 0:
        saved = errno
        if r.torn_down or r.destroy_pending:
            return
        e.cur = old
        r.outcome = ERROR
        r.err = saved
        return
    if r.torn_down or r.destroy_pending:
        return

teardown(r):
    if r.torn_down:
        return
    r.torn_down = 1
    for each endpoint e:
        if e.cur != 0:
            old = e.cur
            e.cur = 0
            if odin_transport_set_interest(e.t, 0) != 0:
                e.cur = old
    cb = r.on_done
    ud = r.user_data
    cb(r, status, err, ud)

odin_relay_ready(t, events, user_data):
    r = user_data
    relay_enter(r)
    if !r.torn_down and !r.destroy_pending:
        process write/read/error, drive, and maybe teardown
    relay_leave(r)

odin_relay_destroy(r):
    if r == NULL: return
    if r.active_depth != 0:
        r.destroy_pending = 1
        return
    stop remaining interests, free relay buffers and relay object
```

Satisfies: G2 via relay interest state that is already consistent when an xqc WRITE kick or pending EOF synchronously re-enters `odin_relay_ready`, plus deferred relay destruction that lets nested write/EOF completion and destroy-in-`on_done` unwind without use-after-free.

## 4. Security

The wrapper moves QUIC stream bytes supplied by a remote peer into caller-provided buffers, is entered by xquic callbacks whose `strm_user_data` pointer is controlled by the stream lifecycle wiring, and can synchronously enter RFC-014 relay callbacks when used as a relay endpoint.

- **S1.**
  - **Threat:** A peer sends more stream data than the caller's receive buffer can hold. If the wrapper passed any capacity larger than the caller's `len` to xquic or reported more than `len` bytes as successfully read, downstream relay code could advance a ring buffer past the memory it actually owns.
  - **Mitigation:** `odin_transport_read` passes the caller's exact `len` to `xqc_stream_recv(stream, buf, len, &fin)`, uses no intermediate receive buffer, and treats any impossible `ret > len` result as `ODIN_TRANSPORT_IO_ERROR` with `errno == EIO`. This enforcement point is pinned in section 3.2.2's Read contract and mechanism.
  - **Enforcement:** Section 5 row T12 drives a fake xquic receive with a queued 64-byte peer payload and a 16-byte caller buffer, asserts the fake receives `recv_buf_size == 16`, asserts Odin reports exactly 16 bytes rather than the queued payload size, then drives a fake impossible `recv` return of `17` for the same 16-byte caller buffer and asserts `ODIN_TRANSPORT_IO_ERROR`, `errno == EIO`, and no oversized byte count is reported.

- **S2.**
  - **Threat:** xquic can invoke a stream callback after the Odin wrapper has been destroyed if the stream's user-data slot still points at freed memory, causing a use-after-free when the callback helper casts `strm_user_data`.
  - **Mitigation:** `odin_xqc_stream_transport_create` installs the wrapper pointer with `xqc_stream_set_user_data(stream, t)`, the caller destroys the wrapper while the `xqc_stream_t` is still valid, `destroy` clears that slot with `xqc_stream_set_user_data(stream, NULL)` before freeing, destroy during a wrapper-delivered `on_ready` defers the physical free until the outermost delivery returns, and all exported callback helpers no-op when `strm_user_data == NULL`. This enforcement point is pinned in section 3.2.1's Stream lifetime ordering, Destroy, and Callback bridge contracts.
  - **Enforcement:** Section 5 row T13 simulates caller-owned `stream_close_notify` destroying the wrapper while the fake stream is still valid, verifies the fake xquic user-data slot was cleared to `NULL`, calls each helper with `strm_user_data == NULL` and asserts no `on_ready` callback fires, then drives destroy from inside a READ callback and asserts the helper emits no later callback after destroy; the P2 ASan gate runs T13 so immediate-free implementations fail and deferred-free implementations pass under the sanitizer.

- **S3.**
  - **Threat:** A peer sends final stream data with FIN that exactly fills the relay's remaining read capacity. The relay clears READ interest for backpressure and enables sink WRITE; the xqc transport can synchronously deliver the first WRITE kick from `odin_transport_set_interest(..., WRITE)`, and after that write drains the ring, `reconcile` can re-enable READ and synchronously deliver the pending EOF. If the relay commits interest state after `set_interest` returns or lacks nested-destroy deferral, the nested `odin_relay_ready` can complete the relay and `on_done` can destroy it while an outer `reconcile` frame still plans to write `e->cur`, causing stale interest state or use-after-free.
  - **Mitigation:** Relay reconciliation writes the new `cur` mask before calling `odin_transport_set_interest`, restores it only on immediate no-callback failure, and wraps `odin_relay_ready` with `active_depth`/`destroy_pending` so destroy-in-`on_done` defers the physical free until the outermost relay frame unwinds. This enforcement point is pinned in section 3.2.3's Interest state visibility, Nested completion, and Outer-frame unwind contracts.
  - **Enforcement:** Section 5 row T18 drives the real relay with two xqc fake transports through final-data backpressure, synchronous first WRITE drain from `set_interest(WRITE)`, synchronous pending EOF delivery from `set_interest(READ)`, EOF observation, teardown interest cleanup, and destroy-in-`on_done` under ASan.

## 5. Testing Strategy

`T1` through `T18` live in `transport_xqc_unittests.cpp`. They compile `transport_xqc_testing.c`, which includes `transport_xqc.c` with `ODIN_TRANSPORT_XQC_TESTING` enabled and replaces the production xquic calls through a test-only ops table: `recv`, `send`, and `set_user_data`. Tests use an opaque fake `xqc_stream_t *` value and do not create an xquic engine or connection, because this RFC's contract is the stream wrapper, not engine integration. `T18` additionally instantiates the real RFC-014 relay through its public API and connects it to two xqc fake transports, so the first WRITE drain and pending EOF path are exercised through `odin_relay_ready`, `reconcile`, and xqc transport `set_interest` callbacks instead of through a hand-written callback sequence. `T19` is a static/build check implemented by the `odin_transport_xqc_scope_check` GN action; it reads the public header, source, and `odin/BUILD.gn` target shape instead of running an engine or connection harness. The fake ops table observes only the xquic calls the wrapper is allowed to make; absence of forbidden stream ownership and direct engine/connection APIs is enforced by `T19` against the staged source/header, not by a separate fixture. The production target uses `public_deps = [ ":odin_transport", "//xquic" ]`, so downstream users that include `odin/transport_xqc.h` inherit both public dependency surfaces.

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | Factory installs and destroy clears xquic stream user data | Fake stream `S`; fake xquic ops record `set_user_data`, `send`, and `recv`; call `odin_xqc_stream_transport_create(S, OnReady, &state, &t)`, then `odin_transport_destroy(t)` | Create returns `0` and calls `set_user_data(S, t)` exactly once; destroy calls `set_user_data(S, NULL)` exactly once; no fake `send` or `recv` op is called | G1 | unit |
| T2 | Read returns buffered stream bytes | Fake `recv(S, buf, 64, &fin)` writes `"hello"`, sets `fin = 0`, and returns `5`; call `odin_transport_read(t, buf, 64, &n)` | Returns `ODIN_TRANSPORT_OK`, `n == 5`, and `buf[0..5) == "hello"` | G2 | unit |
| T3 | FIN-only read becomes EOF | Fake `recv(S, buf, 64, &fin)` sets `fin = 1` and returns `0`; call `odin_transport_read` | Returns `ODIN_TRANSPORT_EOF` and `n == 0` | G2 | unit |
| T4 | Empty xquic receive queue becomes AGAIN | Fake `recv` sets `fin = 0` and returns `-XQC_EAGAIN`; call `odin_transport_read` | Returns `ODIN_TRANSPORT_AGAIN`; `odin_transport_error(t) == 0` | G2 | unit |
| T5 | Read hard error maps to synchronous errno-style failure | Fake `recv` returns `-XQC_ESTREAM_RESET`; call `odin_transport_read` and then `odin_transport_error(t)` | `odin_transport_read` returns `ODIN_TRANSPORT_IO_ERROR`; `errno == EPIPE` immediately after the call; `odin_transport_error(t) == 0`, proving synchronous read failure did not populate the async-error latch | G2 | unit |
| T6 | Write reports partial byte progress | Fake `send(S, "abcdef", 6, fin=0)` returns `3`; call `odin_transport_write(t, "abcdef", 6, &n)` | Returns `ODIN_TRANSPORT_OK`, `n == 3`, and fake `send` captured `fin == 0` with `send_data_size == 6` | G2 | unit |
| T7 | Zero-length write is local, and non-empty backpressure and hard error are distinct | Case A: initialize `n = 99`, configure fake `send` to fail the test if called, then call `odin_transport_write(t, "", 0, &n)`. Case B: first fake non-empty `send` returns `-XQC_EAGAIN`; second fake non-empty `send` returns `-XQC_EPARAM`; call `odin_transport_write` for each non-empty case | Case A returns `ODIN_TRANSPORT_OK`, sets `n == 0`, and fake `send` call count remains `0`, proving no fake zero-data send occurs. Case B first call returns `ODIN_TRANSPORT_AGAIN` and leaves `odin_transport_error(t) == 0`; second call returns `ODIN_TRANSPORT_IO_ERROR`, sets `errno == EIO`, and still leaves `odin_transport_error(t) == 0`, proving synchronous write failure did not populate the async-error latch | G2 | unit |
| T8 | shutdown_write sends an immediate FIN-only block | Fake `send(S, NULL, 0, fin=1)` returns `0`; call `odin_transport_shutdown_write(t)` twice | Both calls return `0`; fake `send` sees exactly one FIN-only call because the second shutdown is idempotent | G2 | unit |
| T9 | shutdown_write queues FIN across xquic backpressure and reports async retry failure | Case A: first fake FIN `send(S, NULL, 0, fin=1)` returns `-XQC_EAGAIN`; call `odin_transport_shutdown_write(t)`; then fake write readiness calls `odin_xqc_stream_transport_write_notify(S, t)` with the second fake FIN send returning `0`. Case B: repeat on a fresh wrapper, but the write-notify FIN retry returns `-XQC_EPARAM` | Case A `shutdown_write` returns `0` and latches no error; write notify retries exactly one FIN-only send, clears the pending FIN, returns `XQC_OK`, and does not emit `ODIN_TRANSPORT_ERROR`. Case B write notify returns `XQC_OK`, emits one `ODIN_TRANSPORT_ERROR`, and `odin_transport_error(t) == EIO`, proving queued-FIN retry failure is asynchronous | G2 | unit |
| T10 | READ notify honors interest, and WRITE enablement kicks the first send | Create wrapper with an `OnReady` recorder that calls `odin_transport_write(t, "abc", 3, &n)` only on the first WRITE event; fake `send` records data and returns `3`. Call `set_interest(READ)`, `read_notify(S, t)`, `write_notify(S, t)`, then `set_interest(WRITE)` without calling `write_notify`, then repeat `set_interest(WRITE)`, then call `write_notify(S, t)`, then `set_interest(0)` and call both notify helpers | Recorder sees exactly one READ event in the READ-interest phase and no WRITE event while WRITE interest is off; the first `set_interest(WRITE)` synchronously emits one WRITE event before any xquic `write_notify`, and fake `send` captures exactly one non-empty data send of `"abc"`; the repeated `set_interest(WRITE)` emits no extra kick; the later `write_notify` emits one additional WRITE continuation event while interest is on; no callback fires after interest is cleared | G2 | unit |
| T11 | Invalid interest mask is rejected without clearing prior interest | Set interest to `READ`; then call `set_interest` with `ODIN_TRANSPORT_ERROR` plus unknown bit `0x80u`; call `read_notify(S, t)` | Invalid call returns `-1` with `errno == EINVAL`; previous READ interest remains active, proven by one later `ODIN_TRANSPORT_READ` callback | G2 | unit |
| T12 | Receive capacity and impossible oversized xquic returns are bounded to caller length | Case A: fake stream has a 64-byte queued payload but the test read buffer is 16 bytes; fake `recv` fails the test unless `recv_buf_size == 16`, writes the first 16 bytes, sets `fin = 0`, and returns `16`; call `odin_transport_read(t, buf, 16, &n)`. Case B: create a fresh wrapper, initialize `n = 0`, fake `recv` again observes `recv_buf_size == 16`, sets `fin = 0`, and returns impossible byte count `17` without writing more than the caller buffer; call `odin_transport_read(t, buf, 16, &n)` | Case A returns `ODIN_TRANSPORT_OK`, `n == 16`, fake `recv` observed capacity `16`, and no value larger than the caller buffer length is reported. Case B returns `ODIN_TRANSPORT_IO_ERROR`, sets `errno == EIO`, leaves `odin_transport_error(t) == 0`, and leaves `n == 0`, proving no oversized byte count is reported and the async-error latch remains async-only | G2, S1 | unit |
| T13 | Close-notify-style destroy clears user data, callback after destroy is a NULL-user-data no-op, and destroy inside a READ callback is safe | Case A: create wrapper, simulate caller-owned xquic `stream_close_notify(S, t)` by calling `odin_transport_destroy(t)` while fake stream `S` is still valid, verify fake `set_user_data(S, NULL)` was called, then call `read_notify(S, NULL)`, `write_notify(S, NULL)`, and `closing_notify(S, XQC_ESTREAM_RESET, NULL)`. Case B: create a fresh wrapper, set READ interest, and use an `OnReady` callback that calls `odin_transport_destroy(t)` without reading; call `read_notify(S, t)` | Case A destroy returns before the fake stream is invalidated, clears fake stream user data to `NULL` exactly once, notify helpers return `XQC_OK` where applicable, do not dereference a wrapper, and the `OnReady` recorder remains at zero calls. Case B sees exactly one READ callback, clears fake stream user data to `NULL`, emits no second callback after destroy, returns `XQC_OK`, and the ASan variant reports no use-after-free | G1, S2 | unit |
| T14 | xquic stream closing surfaces ODIN transport ERROR | Create wrapper with `OnReady` recorder; call `odin_xqc_stream_transport_closing_notify(S, XQC_ESTREAM_RESET, t)`; then call `odin_transport_error(t)` | Recorder sees one `ODIN_TRANSPORT_ERROR` event even without READ/WRITE interest; `odin_transport_error(t) == EPIPE` | G2 | unit |
| T15 | Final stream data with FIN returns bytes before EOF | Fake first `recv(S, buf, 64, &fin)` writes `"bye"`, sets `fin = 1`, and returns `3`; configure the fake to fail the test if `recv` is called again; call `odin_transport_read(t, buf, 64, &n)`, then call `odin_transport_read(t, buf, 64, &n)` again | First read returns `ODIN_TRANSPORT_OK`, `n == 3`, and `buf[0..3) == "bye"`; second read returns `ODIN_TRANSPORT_EOF` with `n == 0`; fake `recv` call count is exactly `1`, proving EOF came from the wrapper latch rather than a second xquic receive | G2 | unit |
| T16 | Final stream data with FIN produces a follow-up READ readiness | Create wrapper with READ interest and an `OnReady` callback that calls `odin_transport_read` once per callback. Fake first `recv(S, buf, 64, &fin)` writes `"bye"`, sets `fin = 1`, and returns `3`; configure the fake to fail the test if `recv` is called again; call `odin_xqc_stream_transport_read_notify(S, t)` once | The single xquic `read_notify` returns `XQC_OK` and produces two `ODIN_TRANSPORT_READ` callbacks while READ interest remains set: the first callback's read returns `ODIN_TRANSPORT_OK`, `n == 3`, and `"bye"`; the second callback's read returns `ODIN_TRANSPORT_EOF`, `n == 0`; fake `recv` call count is exactly `1`. This is the readiness path a one-read-per-readiness relay observes, not a direct double-read shortcut | G2 | unit |
| T17 | Latched EOF survives relay-style READ backpressure | Create wrapper with READ interest and an `OnReady` callback that reads once per READ event into the simulated remaining relay capacity of 8 bytes. Fake first `recv(S, buf, 8, &fin)` writes 8 final bytes, sets `fin = 1`, and returns `8`; inside the first callback, after the OK read, call `odin_transport_set_interest(t, 0)` to model the relay clearing READ because its 64 KiB ring is full. Configure the fake to fail the test if `recv` is called again; after `read_notify` returns, call `odin_transport_set_interest(t, ODIN_TRANSPORT_READ)` to model later WRITE progress draining capacity and re-enabling READ | The initial `read_notify` returns `XQC_OK` and emits exactly one READ callback while interest is cleared; the later `set_interest(READ)` synchronously emits one synthetic READ callback; that callback's read returns `ODIN_TRANSPORT_EOF` with `n == 0`; fake `recv` call count is exactly `1`, proving the EOF came from the latched pending readiness rather than a second xquic receive | G2 | unit |
| T18 | Real relay first-WRITE kick and pending EOF recovery are re-entrant-safe | Create a real `odin_relay_t` whose `on_done` records status and calls `odin_relay_destroy`; create two xqc fake transports A and B with `odin_relay_ready` as `on_ready`, then start the relay. B's fake `recv` first returns FIN-only EOF and B `read_notify` lets the relay half-close A. A's fake `recv` then returns exactly 65536 final bytes with `fin = 1`; call A `read_notify` once. During that real relay path, A's final data fills the A-to-B relay ring, clears A READ interest, and leaves the xqc EOF readiness pending; relay `reconcile(B)` enables WRITE, and B's xqc transport synchronously emits the WRITE kick before any manual B `write_notify`. Make B's fake data `send` accept all 65536 bytes. During that nested WRITE path, relay `reconcile(A)` calls `set_interest(READ)`, and the xqc transport synchronously emits the pending EOF READ callback before `set_interest` returns. Configure A's fake to fail if `recv` is called again | B captured exactly 65536 data bytes from the synchronous WRITE kick without the test manually invoking B `write_notify`; the nested EOF callback reads `ODIN_TRANSPORT_EOF` from A's latch without another xquic `recv`; `on_done` fires exactly once with `ODIN_RELAY_OK` and destroys the relay; A and B test accessors report final interest masks of `0`; B captured one shutdown FIN caused by A's EOF; A captured the shutdown FIN caused by B's earlier EOF; the outer A `read_notify` returns after the nested completion without touching freed relay state; the ASan variant reports no use-after-free | G2, S3 | integration |
| T19 | Public API and implementation stay stream scoped | Run `./tool/ninja -C out odin:odin_transport_xqc_scope_check`. The action tokenizes `odin/transport_xqc.h` and `odin/transport_xqc.c` after stripping comments and strings, and parses the `odin_transport_xqc` GN target's public dependency shape | The public header exposes only `xqc_stream_t *`, xquic stream callback return/status types, Odin transport types, and `void *` callback user data; the GN target exposes `public_deps = [ ":odin_transport", "//xquic" ]`; and the source/header token scan permits only `xqc_stream_t`, `xqc_int_t`, `XQC_OK`, `XQC_EAGAIN`, `XQC_ESTREAM_RESET`, `XQC_CLOSING`, `xqc_stream_recv`, `xqc_stream_send`, and `xqc_stream_set_user_data` from xquic, rejecting direct engine, connection, stream-create/close/reset, socket, timer, CID, ALPN, and packet-I/O symbols in the staged source/header. This proves the wrapper has no direct engine/connection API surface or lifecycle ownership; it does not claim that xquic stream APIs avoid their own internal engine work | G3 | static/build |

## 6. Implementation Plan

- **P1. Land the xqc stream transport surface and executable-red `T1`-`T19` gates.**
  - **Scope:** add `odin/transport_xqc.h` with the section 3.2.1 factory and callback-helper declarations; add `odin/transport_xqc.c` with a linkable wrapper struct and RFC-013 vtable whose slots are stubs chosen to fail the section 5 runtime rows (`create` allocates but does not call `xqc_stream_set_user_data`, `destroy` frees without clearing user data or deferring in-callback free, read/write return fixed `ODIN_TRANSPORT_IO_ERROR` with `errno = EINVAL`, `shutdown_write` returns `-1` with `errno = EINVAL`, `set_interest` accepts every mask but stores nothing, `error` returns `0`, and the notify helpers never invoke `on_ready` or synthesize EOF readiness) and whose private wrapper struct includes a temporary red-only `xqc_engine_t *scope_check_red_engine` member so the staged source itself violates `T19` while still compiling; add `odin/transport_xqc_internal_test.h` and `odin/transport_xqc_testing.c` with a test-only ops table for fake `recv`, `send`, and `set_user_data`; add `odin/transport_xqc_unittests.cpp` containing `T1`-`T18`, each guarded so it skips by default and runs assertions only when `ODIN_XQC_STREAM_TRANSPORT_RED=1` is set, with `T18` using the real relay APIs already linked into `:odin_unittests`; update `odin/BUILD.gn` with `source_set("odin_transport_xqc")` using `public_deps = [ ":odin_transport", "//xquic" ]`, add it to `:odin`, add `transport_xqc.h`, `transport_xqc_internal_test.h`, `transport_xqc_testing.c`, and `transport_xqc_unittests.cpp` to `:odin_unittests`, add an `ODIN_TRANSPORT_XQC_TESTING` config to the test binary, and add `//xquic` to the test binary deps so `<xquic/xquic.h>` resolves while compiling the testing include; add `odin/check_xqc_stream_transport_scope.py` and a GN action `odin_transport_xqc_scope_check` for `T19` that tokenizes `odin/transport_xqc.h` and `odin/transport_xqc.c` after stripping comments and strings, verifies the public header exposes only stream-level xquic types, verifies the GN target exposes `public_deps = [ ":odin_transport", "//xquic" ]`, permits only the xquic stream wrapper tokens `xqc_stream_t`, `xqc_int_t`, `XQC_OK`, `XQC_EAGAIN`, `XQC_ESTREAM_RESET`, `XQC_CLOSING`, `xqc_stream_recv`, `xqc_stream_send`, and `xqc_stream_set_user_data`, and fails on xquic engine, connection, datagram, socket, timer, CID, ALPN, stream-create, stream-close, stream-reset, or packet-I/O symbols in the staged source/header. Red-verification commands: `ODIN_XQC_STREAM_TRANSPORT_RED=1 out/odin_unittests --gtest_filter='OdinXqcStreamTransport*'`; `./tool/ninja -C out odin:odin_transport_xqc_scope_check`.
  - **Depends on:** None.
  - **Done when:** `./tool/ninja -C out tests` builds the new production target and test sources; the default `out/odin_unittests --gtest_filter='OdinXqcStreamTransport*'` reports `T1`-`T18` skipped and exits green; the runtime red-verification command runs `T1`-`T18` and reports each row failing against the stubs: `T1` misses the set/clear user-data calls, `T2`-`T5` get the wrong read result or errno, `T6`-`T7` get the wrong write result, errno, or zero-length no-send proof, `T8`-`T9` get failed or missing FIN sends, `T10` gets no READ callback and no synchronous first-WRITE kick from `set_interest(WRITE)`, `T11` accepts the invalid mask and loses the preserved-interest proof, `T12` never calls fake `recv` with the caller capacity and returns the wrong errno for the impossible `ret > len` case, `T13` fails because destroy did not clear user data in the close-notify-style and in-callback destroy cases, `T14` gets no `ODIN_TRANSPORT_ERROR` callback, `T15` returns neither the final bytes nor the latched EOF sequence, `T16` gets no readiness-delivered EOF because the stub never emits either READ callback, `T17` loses the latched EOF across READ disable/re-enable because the stub has no pending-EOF readiness path in `set_interest`, and `T18` never reaches the real relay's nested write/EOF completion because the stub transport does not deliver the first-WRITE kick from `set_interest(WRITE)` or the pending EOF callback from `set_interest(READ)`; the T19 red-verification command exits nonzero against the staged P1 source/header and reports the temporary red-only forbidden `xqc_engine_t` token in `transport_xqc.c`.

- **P2. Implement xquic stream mapping, relay re-entry safety, and turn `T1`-`T19` green.**
  - **Scope:** replace the P1 stubs in `transport_xqc.c` with the section 3.2 implementation and remove the temporary red-only `xqc_engine_t *scope_check_red_engine` member: create/destroy install and clear xquic stream user data, including the valid-stream-until-destroy precondition and deferred free for destroy inside wrapper-delivered callbacks; read returns a latched EOF before calling xquic, otherwise calls `xqc_stream_recv` once and maps returns exactly as section 3.2.2 specifies, including latching EOF and setting `recv_eof_ready_pending` for `ret > 0 && fin == 1` during READ callback delivery and keeping synchronous read failures out of the async-error latch; write returns local `ODIN_TRANSPORT_OK` with `*out_n = 0` before calling xquic when `len == 0`, otherwise calls `xqc_stream_send(..., fin=0)` once and maps returns exactly as section 3.2.2 specifies, including keeping synchronous write failures out of the async-error latch; `shutdown_write` sends or queues a FIN-only `xqc_stream_send(..., fin=1)`, treats direct FIN hard failure as a synchronous `-1`/`errno` result, and retries pending FIN from `write_notify`; `set_interest` rejects invalid bits with `EINVAL` while preserving the previous mask, stores valid masks, delivers one pending synthetic READ synchronously when READ is re-enabled with `recv_eof_ready_pending` set, and delivers one synchronous WRITE kick when WRITE transitions from disabled to enabled; `read_notify` emits the initial interest-gated READ readiness and then calls the shared pending-EOF helper so the synthetic EOF READ readiness is delivered immediately when READ is still active or left pending when READ is temporarily off; `write_notify` and `closing_notify` implement their interest/error behavior and all helpers implement NULL-user-data no-op; `error` returns only the latched asynchronous stream error from `closing_notify` or a failed pending-FIN retry in `write_notify`. Update `odin/relay.c` and the relay lifecycle comments in `odin/relay.h` per section 3.2.3: pre-commit `e->cur` before `odin_transport_set_interest`, restore it on immediate no-callback failure, add `active_depth` and `destroy_pending`, defer `odin_relay_destroy` while a relay frame is active, and make outer frames unwind without touching relay state after nested teardown triggered by either a WRITE kick or pending EOF callback. Remove the `ODIN_XQC_STREAM_TRANSPORT_RED` skip gate from `T1`-`T18`; keep `odin_transport_xqc_scope_check` wired so `T19` runs as a normal static/build row.
  - **Depends on:** P1.
  - **Done when:** `T1`-`T18` pass un-gated in `out/odin_unittests --gtest_filter='OdinXqcStreamTransport*'`, including T5/T7's proof that synchronous read/write hard errors report through `errno` without populating `odin_transport_error`, T7's proof that zero-length writes return success without calling fake `send`, T9's proof that a failed queued-FIN retry does populate the async-error latch and emit `ODIN_TRANSPORT_ERROR`, T10's proof that enabling WRITE synchronously kicks the first non-empty send before any xquic `write_notify`, T12's impossible `ret > len` proof, T13's close-notify-style and in-wrapper-callback destroy proof, T16's proof that one xquic `read_notify` produces the first READ callback for final bytes and a second READ callback for the latched EOF without another `recv`, T17's transport-local proof that final bytes filling remaining read capacity leave EOF readiness pending while READ interest is off and deliver it through `set_interest(READ)` after later drain without another `recv`, and T18's real-relay proof that `set_interest(WRITE)` first drains buffered data without a manual sink `write_notify`, then `set_interest(READ)` re-enters `odin_relay_ready`, observes EOF, finishes with final interest masks at `0`, and survives destroy-in-`on_done`; `./tool/gn gen out/asan --args='is_asan=true'`, `./tool/ninja -C out/asan odin:odin_unittests`, and `out/asan/odin_unittests --gtest_filter='OdinXqcStreamTransport*.T13*:OdinXqcStreamTransport*.T18*'` report no AddressSanitizer use-after-free for both wrapper in-callback destroy and relay nested-completion destroy; `./tool/ninja -C out odin:odin_transport_xqc_scope_check` passes `T19` against the implemented source/header and proves the public API exposes only xquic stream objects and callbacks, the GN target uses `public_deps = [ ":odin_transport", "//xquic" ]`, and the implementation directly uses only the allowed xquic stream symbols without claiming anything about xquic stream APIs' internal engine work; and `./tool/ninja -C out tests` remains green with `:odin` depending on `:odin_transport_xqc`.
