# RFC-014: Transport-Agnostic Bidirectional Byte Relay (relay v2)

## 1. Summary

Add `odin/relay_v2.{c,h}`, a transport-agnostic rewrite of the RFC-011 byte relay that forwards bytes between two caller-provided, caller-owned `odin_transport_t` endpoints (RFC-013) — performing every read, write, half-close, readiness/interest change, and asynchronous-error probe through the `odin_transport_*` dispatchers instead of raw fds and `odin_event_io_*` — while preserving the relay's fixed 64 KiB per-direction backpressure buffering, end-of-stream-as-`shutdown_write` propagation, single-error aggregation, and exactly-once completion; the existing fd↔fd `relay.{c,h}` is left untouched.

## 2. Goals

- **G1.** Forward bytes bidirectionally and in order between two caller-supplied `odin_transport_t` endpoints with fixed 64 KiB per-direction buffering and backpressure (a source's reads stop once its destination buffer is full and resume once it drains), driving all byte movement through the `odin_transport_read` / `odin_transport_write` / `odin_transport_set_interest` dispatchers and naming no concrete transport, so the relay forwards over any conforming transport.

- **G2.** Propagate end-of-stream as a half-close: when one direction reaches `ODIN_TRANSPORT_EOF` and its buffered bytes have been written out, issue exactly one `odin_transport_shutdown_write` on the peer endpoint; the opposite direction keeps forwarding; the relay reports `ODIN_RELAY_V2_OK` exactly when both directions have half-closed.

- **G3.** Aggregate a genuine read/write fault, a failed half-close (`odin_transport_shutdown_write`), or a latched asynchronous transport error surfaced through `odin_transport_error`, into a single `ODIN_RELAY_V2_ERROR` completion carrying the failing `errno`.

- **G4.** Complete through an `on_done` callback that fires exactly once on the owner thread; the relay destroys neither transport and closes no descriptor (the caller owns both transports), and `odin_relay_v2_destroy` is safe to call from within `on_done` and aborts a still-running relay such that no later readiness re-enters freed state.

## 3. Design

### 3.1 Overview

`odin/relay_v2` is a new leaf module that consumes only `odin/transport` (RFC-013). Unlike the RFC-011 relay, it depends on neither `odin/event_loop` nor the socket layer: it issues no syscalls and registers no watches directly, so it carries none of a concrete transport's dependencies. The existing `odin/relay` (`relay.{c,h}`) is unchanged; `relay_v2` is an additive sibling with its own tests.

Because a transport's readiness callback is bound at construction (RFC-013) and the interface exposes no setter, the relay is wired in two phases. First the caller calls `odin_relay_v2_create`, which allocates the relay object and its two 64 KiB buffers but registers nothing. The caller then builds the two transports — today via `odin_fd_transport_create`, in future via any sibling implementation — passing the relay's exported readiness trampoline `odin_relay_v2_ready` as each transport's `on_ready` and the relay handle as each transport's `user_data`. Finally the caller calls `odin_relay_v2_start(relay, a, b)`, which binds the two endpoints and registers a READ interest on each. No readiness can arrive before `start`, because a freshly created transport holds no watch until its first `set_interest` (RFC-013).

During the run, data flows in through `odin_transport_read` on the ready endpoint, into that direction's 64 KiB ring, and out through `odin_transport_write` on the peer endpoint; the relay expresses what it wants to watch through `odin_transport_set_interest` and receives readiness back through `odin_relay_v2_ready`. End-of-stream becomes `odin_transport_shutdown_write` on the peer; an `ODIN_TRANSPORT_ERROR` readiness that produced no synchronous failure is classified through `odin_transport_error`. The relay owns only its object and its two buffers; it owns neither transport and no fd, mirroring the RFC-011 relay's caller-owns-the-endpoint contract. The caller destroys the two transports (and closes the fds behind them) after `on_done`.

```
                         caller
   create(&r);  build a,b via odin_fd_transport_create(loop, fd,
       odin_relay_v2_ready, /*user_data=*/r, &t);  start(r, a, b);
   on on_done:  destroy(r); transport_destroy(a); transport_destroy(b); close fds
                         |
                         v
        +-------------------------------------------+
        |              odin_relay_v2_t              |   (odin/relay_v2.{c,h})
        |   2 x 64 KiB ring  +  dir/end state       |   depends only on
        |   exactly-once on_done                    |   odin/transport.h
        +-------------------------------------------+
            |  odin_transport_read / _write              ^  odin_transport_ready_cb
            |  _shutdown_write / _set_interest / _error  |  (odin_relay_v2_ready)
            v                                            |
   +----------------------+                   +----------------------+
   |  odin_transport_t a  |                   |  odin_transport_t b  |  (caller-owned)
   +----------------------+                   +----------------------+
            |  fd impl today; any impl tomorrow          |
            v                                            v
        endpoint A                                   endpoint B
```

### 3.2 Detailed Design

#### 3.2.1 Public API, two-phase lifecycle, and transport ownership

Contract surface — `odin/relay_v2.h` (include guard, copyright, and per-declaration doc-comments omitted; the `#include "odin/transport.h"` is load-bearing because `odin_relay_v2_ready` has type `odin_transport_ready_cb` and the API speaks `odin_transport_t`, and the `extern "C"` linkage is load-bearing because the test translation unit is C++):

```c
/* odin/relay_v2.h */
#include "odin/transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct odin_relay_v2_t odin_relay_v2_t;

typedef enum odin_relay_v2_status_t {
  ODIN_RELAY_V2_OK = 0,
  ODIN_RELAY_V2_ERROR,
} odin_relay_v2_status_t;

typedef void (*odin_relay_v2_done_cb)(odin_relay_v2_t *relay,
                                      odin_relay_v2_status_t status, int err,
                                      void *user_data);

int  odin_relay_v2_create(odin_relay_v2_done_cb on_done, void *user_data,
                          odin_relay_v2_t **out);
void odin_relay_v2_ready(odin_transport_t *t, unsigned int events,
                         void *user_data);
int  odin_relay_v2_start(odin_relay_v2_t *relay, odin_transport_t *a,
                         odin_transport_t *b);
void odin_relay_v2_destroy(odin_relay_v2_t *relay);

#ifdef __cplusplus
}
#endif
```

**Unstated contract.**

- *Two-phase construction.* `odin_relay_v2_create` allocates the relay object and its two 64 KiB buffers, stores `on_done` / `user_data`, writes `*out`, and returns `0`; it binds no transport and registers nothing. Its only failure is `ENOMEM` (returns `-1`, `errno == ENOMEM`, `*out` untouched). `on_done` must be non-null.
- *Readiness wiring.* The caller installs `odin_relay_v2_ready` as the `on_ready` of **both** transports, with `user_data` set to the `odin_relay_v2_t *` from `create`. `odin_relay_v2_ready` is the only readiness entry point; it identifies which endpoint fired by comparing `t` to the two bound transports (the analogue of the RFC-011 relay's fd comparison, `odin/relay.c:83-89`). It is a precondition that `odin_relay_v2_ready` is invoked only with one of the two bound transports and the matching relay handle; the caller must not call `read`/`write`/`set_interest`/`shutdown_write` on either transport itself while the relay drives it.
- *Binding and direction.* `odin_relay_v2_start` binds `a` and `b` as direction A (`a → b`) and direction B (`b → a`), registers a READ interest on each via `odin_transport_set_interest`, and returns `0`. On the first `set_interest` failure it returns `-1` with `errno` preserved and no interest registered; if the second fails it rolls the first back to an empty interest before returning `-1` with the second call's `errno` (e.g. `EEXIST` when the fd behind a transport already has a watch). `start` allocates nothing and is the only step that can register interest. After either failure path the relay is left re-startable — both `end[].cur` are `0` and no interest is registered (the `end[].t` assignments at the top of `start` are harmless and simply overwritten by the next `start`) — so `start` may be retried on the same created relay with the same `a`/`b` once the cause is cleared (as T12 does after stopping the colliding external watch).
- *Ownership.* The relay owns its object and its two buffers only. It never calls `odin_transport_destroy` on either transport and never closes any fd; the caller owns both transports and their fds and destroys them after `on_done` (exactly as the RFC-011 relay leaves caller fds untouched, `odin/relay.h:11-17`). `odin_relay_v2_destroy(relay)` stops any interest the relay still holds (`odin_transport_set_interest(t, 0)` on each still-watched endpoint), frees the two buffers and the relay object, and never invokes `on_done`; `odin_relay_v2_destroy(NULL)` is a no-op. Because `destroy` may touch the transports to stop their watches when aborting a still-running relay, the caller must call `odin_relay_v2_destroy(relay)` **before** destroying the two transports.
- *Completion and re-entrancy.* `on_done` fires exactly once, on the owner thread, as the relay's final action during teardown; no relay state is read or written after `on_done` returns, so `odin_relay_v2_destroy(relay)` (and then the two `odin_transport_destroy` calls) from inside `on_done` is legal. `ODIN_RELAY_V2_OK` carries `err == 0` when both directions reached end-of-stream; `ODIN_RELAY_V2_ERROR` carries the failing `errno`; `status` is the authoritative signal. A relay that is created but never started never fires `on_done`.
- *Threading.* All entry points and `odin_relay_v2_ready` run on the owner thread; the relay adds no locks.

**Mechanism.** `create` allocates the object and the two `CAP` buffers and registers nothing; `start` binds the endpoints and registers READ on each, rolling the first back if the second fails (and leaving the relay re-startable on either failure); `destroy` stops any interest still held — gated on `cur` so it is a no-op for an already-torn-down relay — then frees. The forwarding mechanism is §3.2.2–§3.2.4.

```
start(r, a, b):                                 # end/dir src/sink wiring per §3.2.2
    r.end[0].t = a;  r.end[1].t = b
    if odin_transport_set_interest(a, ODIN_TRANSPORT_READ) != 0:
        return -1                               # errno preserved; nothing registered, re-startable
    r.end[0].cur = ODIN_TRANSPORT_READ
    if odin_transport_set_interest(b, ODIN_TRANSPORT_READ) != 0:
        saved = errno
        odin_transport_set_interest(a, 0);  r.end[0].cur = 0    # roll the first back
        errno = saved;  return -1               # both cur == 0, a/b rebindable -> re-startable
    r.end[1].cur = ODIN_TRANSPORT_READ
    return 0

destroy(r):
    if r == NULL: return
    if r.end[0].cur != 0: odin_transport_set_interest(r.end[0].t, 0); r.end[0].cur = 0
    if r.end[1].cur != 0: odin_transport_set_interest(r.end[1].t, 0); r.end[1].cur = 0
    free r.dir[A].buf;  free r.dir[B].buf;  free r
```

Satisfies: G1 via the `odin_transport_t`-typed `start`/`ready` surface that names no concrete transport; G4 via the two-phase `create`/`start`, the exactly-once `on_done` fired as teardown's final action, and the `destroy`/teardown rules that stop interest but destroy no transport and close no fd.

#### 3.2.2 Forwarding ring and backpressure

Each direction owns a `CAP`-byte ring; the relay never buffers more than `CAP` per direction and stops reading a source whose ring is full. The state and the readiness handler mirror the RFC-011 relay one-to-one, with the `odin_transport_*` dispatchers replacing the raw syscalls and `odin_event_io_*` calls.

```
CAP = 65536                       # fixed per-direction capacity (matches odin/relay.c:26)

struct odin_relay_v2_t:
    odin_relay_v2_done_cb on_done
    void                 *user_data
    dir  dir[2]    # A: a->b, B: b->a. each: src_t, sink_t, buf[CAP], head, len, read_eof, write_shut
    end  end[2]    # end[0]: t=a, src=&dir[A], sink=&dir[B], cur
                   # end[1]: t=b, src=&dir[B], sink=&dir[A], cur   (cur = last interest mask set)
    int outcome    # NONE | OK | ERROR
    int err
    int torn_down

# Readiness handler (mirrors on_ready, odin/relay.c:229-264). sink.sink_t == t and
# src.src_t == t by construction, so both ops act on the endpoint that fired.
odin_relay_v2_ready(t, events, user_data):
    r   = user_data
    e   = (t == r.end[0].t) ? &r.end[0] : &r.end[1]
    src = e.src;  sink = e.sink
    err = (events & ODIN_TRANSPORT_ERROR) != 0

    if r.outcome == NONE and sink.len > 0 and ((events & ODIN_TRANSPORT_WRITE) or err):
        do_write(r, sink)

    rd = AGAIN
    if r.outcome == NONE and src.read_eof == 0 and src.len < CAP and ((events & ODIN_TRANSPORT_READ) or err):
        rd = do_read(r, src)

    if r.outcome == NONE and err and src.read_eof == 0 and rd != PROGRESS:
        e2 = odin_transport_error(t)          # latched async error; 0 when benign
        if e2 != 0: r.outcome = ERROR; r.err = e2

    if r.outcome == NONE: drive(r)
    if r.outcome != NONE: teardown(r)

# Read into the contiguous free run at tail (mirrors do_read, odin/relay.c:104-125).
do_read(r, d):
    tail = (d.head + d.len) % CAP
    run  = min(CAP - d.len, CAP - tail)       # > 0 whenever READ is watched
    switch odin_transport_read(d.src_t, d.buf + tail, run, &n):
        OK:       d.len += n;       return PROGRESS
        EOF:      d.read_eof = 1;   return EOF
        AGAIN:    return AGAIN
        IO_ERROR: r.outcome = ERROR; r.err = errno; return FAILED   # errno set by transport (RFC-013)

# Drain the contiguous buffered run at head (mirrors do_write, odin/relay.c:128-144).
do_write(r, d):
    run = min(d.len, CAP - d.head)
    switch odin_transport_write(d.sink_t, d.buf + d.head, run, &n):
        OK:       d.head = (d.head + n) % CAP; d.len -= n
        AGAIN:    return
        IO_ERROR: r.outcome = ERROR; r.err = errno
        # write never returns EOF (RFC-013)

# Recompute one endpoint's interest: READ while its source can still fill, WRITE while
# its sink has bytes (mirrors reconcile, odin/relay.c:149-177). set_interest absorbs the
# lazy start/update/stop the RFC-011 reconcile did by hand.
reconcile(r, e):
    m = 0
    if e.src.read_eof == 0 and e.src.len < CAP: m |= ODIN_TRANSPORT_READ
    if e.sink.len > 0:                          m |= ODIN_TRANSPORT_WRITE
    if m == e.cur: return
    if odin_transport_set_interest(e.t, m) != 0:
        r.outcome = ERROR; r.err = errno; return
    e.cur = m
```

**Unstated contract.** The free-run computation guarantees `run > 0` whenever READ is watched, so a watched source always makes progress or returns `AGAIN`/`EOF`, never a zero-length read. Backpressure is structural: a direction whose ring reaches `CAP` clears READ from its source's interest in `reconcile`, and refills it once `do_write` drains the ring below `CAP` on the next readiness — bytes are delivered to each sink in arrival order because each ring is FIFO. The relay performs no allocation after `create`.

Satisfies: G1 via the per-direction `CAP` ring, the `do_read`/`do_write` dispatch to `odin_transport_read`/`odin_transport_write`, and the `reconcile` interest recomputation through `odin_transport_set_interest` that gates and resumes a source's reads on its destination's fill level.

#### 3.2.3 Half-close propagation and completion

```
# Mirrors drive (odin/relay.c:205-223): half-close each drained, EOF'd direction once,
# reconcile both interests, then complete on dual half-close.
drive(r):
    for d in {r.dir[A], r.dir[B]}:
        if d.read_eof and d.len == 0 and not d.write_shut:
            if odin_transport_shutdown_write(d.sink_t) != 0:
                r.outcome = ERROR; r.err = errno; return
            d.write_shut = 1
    reconcile(r, &r.end[0])
    reconcile(r, &r.end[1])
    if r.outcome == NONE and r.dir[A].write_shut and r.dir[B].write_shut:
        r.outcome = OK
```

**Unstated contract.** A direction is half-closed only after its source reported `ODIN_TRANSPORT_EOF` **and** its ring fully drained, so no buffered byte is lost before the peer's `shutdown_write`. Each direction issues `shutdown_write` at most once (`write_shut` latch). A failed `odin_transport_shutdown_write` is a genuine fault that aggregates into the single `ODIN_RELAY_V2_ERROR` (§3.2.4), not a half-close. A peer's graceful end-of-stream completes one direction; it does not tear the relay down. The relay reports `ODIN_RELAY_V2_OK` only when both directions have half-closed; until then the still-open direction keeps forwarding.

Satisfies: G2 via the per-direction `read_eof && len == 0 && !write_shut` guard that issues one `odin_transport_shutdown_write` on the peer, the independent per-direction state that lets the reverse direction keep flowing, and the dual-`write_shut` test that sets the `OK` outcome.

#### 3.2.4 Error classification and single-teardown aggregation

```
# Mirrors teardown (odin/relay.c:182-201). The torn_down latch makes it idempotent;
# on_done is the final action, so destroy from inside on_done is safe.
teardown(r):
    if r.torn_down: return
    r.torn_down = 1
    if r.end[0].cur != 0: odin_transport_set_interest(r.end[0].t, 0); r.end[0].cur = 0
    if r.end[1].cur != 0: odin_transport_set_interest(r.end[1].t, 0); r.end[1].cur = 0
    st = (r.outcome == OK) ? ODIN_RELAY_V2_OK : ODIN_RELAY_V2_ERROR
    e  = (st == ODIN_RELAY_V2_OK) ? 0 : r.err
    r.on_done(r, st, e, r.user_data)          # nothing in odin_relay_v2_ready runs after this
```

**Unstated contract.** The relay-wide `outcome` is set once by the first fault (or by dual end-of-stream) and consumed once by `teardown`; the `odin_relay_v2_ready` guards (`outcome == NONE` before each sub-step) make a same-batch sibling readiness that re-enters after the outcome is set a no-op, and the `torn_down` latch makes a second `teardown` return without a second `on_done`. A genuine fault is captured at three sites, all yielding one aggregated `ODIN_RELAY_V2_ERROR`: a synchronous `ODIN_TRANSPORT_IO_ERROR` from `do_read`/`do_write` captures `errno` directly (the read/write case), a failed `odin_transport_shutdown_write` in `drive` (§3.2.3) captures `errno` the same way (the half-close case), while an `ODIN_TRANSPORT_ERROR` readiness that produced no synchronous failure is probed with `odin_transport_error` (the latched-async case) — a benign `ERROR` readiness, for which `odin_transport_error` returns `0` and the read still made progress, latches no error and the relay keeps running. Because `teardown` clears both interests before firing `on_done`, no later readiness re-enters the relay even if the loop keeps running, which is what makes `odin_relay_v2_destroy` safe both from inside `on_done` and as an abort of a still-running relay.

Satisfies: G3 via the `IO_ERROR`-captures-`errno` path in `do_read`/`do_write`, the failed `odin_transport_shutdown_write` in §3.2.3 `drive`, and the `odin_transport_error` probe of an unexplained `ERROR` readiness, all routed into the single relay-wide `outcome`; G4 via the `torn_down` latch and the clear-both-interests step that together fire `on_done` exactly once and prevent post-teardown re-entry.

## 4. Security

The relay moves attacker-controlled bytes between two untrusted endpoints, and because the fd transport reads exactly the consumer-supplied length (`odin/transport_fd.c:43` issues `read(s->fd, buf, len)`), `relay_v2.c` itself owns both the buffer-bound arithmetic and the post-teardown re-entry safety. The two concerns RFC-011 §5 documented for the behaviorally identical v1 relay therefore recur here rather than being absorbed by the transport vtable.

- **S1.**
  - **Threat:** Heap buffer overflow from a flooding peer. The relay hands `odin_transport_read` a pointer into one direction's fixed 64 KiB ring plus a length, and the transport transfers up to that length (the fd implementation issues `read(s->fd, buf, len)` at `odin/transport_fd.c:43`); a peer that floods that direction faster than the opposite end drains it (e.g. a multi-hundred-KiB burst while the consumer stalls) would, if the relay passed a length exceeding the ring's contiguous free space, have its bytes written past the buffer end and corrupt adjacent heap allocations. The trigger surface is either endpoint the relay reads from — for the proxy a remote endpoint or a local browser, neither trusted — and computing a safe length is the relay's responsibility, not the transport's (the transport bounds nothing it is not told to).
  - **Mitigation:** §3.2.2 bounds every `do_read` length to the contiguous free run `min(CAP - d.len, CAP - tail)`, which is `≤ CAP - d.len` and never reaches past the physical buffer end on wrap, and the §3.2.2 `reconcile` rule clears `ODIN_TRANSPORT_READ` from the source endpoint's interest the moment `d.len == CAP` (the backpressure stop), so total bytes buffered per direction never exceed `CAP = 65536` and surplus bytes wait in the kernel socket buffer and the peer's send buffer.
  - **Enforcement:** §5 row T13 streams a payload far larger than `CAP` plus both socket buffers through two real `odin_fd_transport` endpoints with a consumer that defers reading until the writer stalls past `CAP`; it asserts `written_at_gate < payload` (the writer was still blocked by backpressure when the reader began draining — proof the ring saturated to `CAP` and `reconcile` cleared the source's `ODIN_TRANSPORT_READ`, the backpressure stop), then that the full payload arrives byte-exact and in order (proof the cleared READ interest re-armed and the ring reassembled across wrap), and runs under the P2 AddressSanitizer command with no heap-overflow report.

- **S2.**
  - **Threat:** Use-after-free / double-completion from a stale same-batch event after teardown. Both bound transports can be ready within one materialized backend batch; a genuine error on one tears the relay down — and the caller may `odin_relay_v2_destroy` it from inside `on_done` — after which the other endpoint's already-materialized readiness could re-enter freed or torn-down relay state through `odin_relay_v2_ready`, causing a use-after-free or a second `on_done`.
  - **Mitigation:** §3.2.4 `teardown` clears both endpoints' interest (`odin_transport_set_interest(t, 0)`) and latches `torn_down` before firing `on_done`, and the `odin_relay_v2_ready` `outcome == NONE` guards make a same-batch sibling that still re-enters a no-op, so no second `on_done` fires; §3.2.1 `odin_relay_v2_destroy` stops any interest the relay still holds before freeing, covering the abort path where `teardown` never runs. `teardown` reads no relay state after `on_done` returns, so an in-callback `destroy` is safe.
  - **Enforcement:** §5 rows T11 (same-batch double error with `destroy` from inside `on_done`) and T14 (the same same-batch double error with **no** `destroy` in `on_done`, the `destroy` deferred until after `run`) both assert `on_done` fires exactly once (`calls == 1`), `ODIN_RELAY_V2_ERROR`, `err == ECONNRESET`, with no second callback. T14's no-destroy `calls == 1` is the assertion that actually loads the `torn_down` latch: it catches the compound failure that clears only the faulting endpoint's interest **and** drops the latch — a logic double-completion that fires `on_done` twice on the no-destroy path, which T11's in-callback `destroy` masks (by stopping the sibling's still-held interest) and which ASan cannot see because it is not a memory error. T4 (`destroy` from inside `on_done` on a write-fault teardown), T11, T14, and T15 all additionally run under the P2 AddressSanitizer command with no use-after-free or double-free report. T15 is the row that actually exercises the abort-path re-entry the mitigation's `destroy`-stops-interest clause guards: it aborts a still-running relay over two live `odin_fd_transport` endpoints and then makes the relay's fds readable, so a `destroy` that failed to stop the still-active interests would dispatch that later readiness into the freed relay through `odin_relay_v2_ready` under AddressSanitizer. T7 remains a loop-less functional check that `destroy` records `odin_transport_set_interest(t, 0)` on each endpoint before the free; with no event loop it materializes no post-abort readiness, so it does not itself exercise the abort-path re-entry.

## 5. Testing Strategy

Rows `T1`–`T7` and `T16` are unit tests in `relay_v2_unittests.cpp` that drive the relay against a test-local fake transport — a struct embedding `odin_transport_t` as its first member, whose vtable slots serve scripted `read`/`write`/`shutdown_write`/`error` results, record each `set_interest` mask and each `destroy` call, and accumulate written bytes — with readiness injected by calling the exported `odin_relay_v2_ready(&fake->base, events, r)` directly; they use no event loop and no fd, which is what lets them assert the relay forwards purely through the dispatchers and is genuinely transport-agnostic (`T16` scripts the fake's `shutdown_write` slot to fail — the one half-close fault no live connected socket triggers deterministically). Rows `T8`–`T15` are integration tests that exercise the relay over two real `odin_fd_transport_create` endpoints plus a live `odin_event_loop`, reusing the RFC-011 fixtures (`odin/relay_unittests.cpp:50-192`): the `fork` + `waitpid` 2 s deadline harness, a per-row watchdog timer, nonblocking `AF_UNIX` `socketpair`s, loopback TCP pairs aborted with `SO_LINGER{1,0} + close` to force an RST, and `PinSocketBuf`. The large-payload row (`T13`) additionally drives a writer and a reader `std::thread` (the saturation pattern RFC-011 §6 T2 established, mandatory because a single-thread write-all-then-`run` deadlocks once the writer blocks past `CAP` plus the socket buffers); its reader **defers its first read** until the writer has stalled past `CAP`, records `written_at_gate`, then drains `pb` to EOF byte-exact. In P2 both threads finish naturally — the relay forwards the full payload, the writer's `shutdown(pa, SHUT_WR)` reaches the relay as EOF, and the relay's resulting `shutdown_write` on `fd_b` gives the reader its EOF — so the child `join`s and asserts; against the P1 stub nothing is forwarded, the writer blocks once its pinned socket buffers fill, and the deferred reader parks forever on the `written >= CAP` gate the unfilled ring never opens, so `T13` is the single **disclosed hang-as-red** row: it surfaces red through the replicated 2 s fork deadline rather than an executed in-child assertion (mirroring RFC-011 §6 T2), with `T8` (assertion-red end-to-end forwarding over fd transports) corroborating that the behavior the hang detects is forwarding. The abort row (`T15`) mirrors RFC-011 §6 T10: it forwards a first byte, then from a 30 ms timer aborts the still-running relay with `odin_relay_v2_destroy` and makes the fds readable again, asserting `on_done` never fires and that the post-abort readiness is not dispatched into the freed relay under AddressSanitizer. The same-batch double-error rows (`T11`, `T14`) feed the two transports' live watch handles — obtained through the RFC-013 `odin_fd_transport_test_io` accessor (`odin/transport_fd_internal_test.h:20`) — to the `ODIN_EVENT_LOOP_TESTING` injection hook `odin_event_loop_test_queue_backend_events` (`odin/event_loop_internal_test.h`); the two differ only in whether `on_done` destroys the relay (T11) or the test destroys it after `run` (T14); both `ODIN_TRANSPORT_FD_TESTING` and `ODIN_EVENT_LOOP_TESTING` are already configured on `:odin_unittests`.

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | Bidirectional in-order forwarding; relay destroys no transport | Fake `a`,`b`; `create(&r)`; `start(r,a,b)`; script `a.read`→`"hello"` then `AGAIN`, `b.read`→`"world"` then `AGAIN`; call `odin_relay_v2_ready(a,READ,r)`, `ready(b,READ,r)`, then `ready(b,WRITE,r)`, `ready(a,WRITE,r)` | `b`'s write buffer holds `"hello"`, `a`'s holds `"world"`, each in order; the fake `destroy` slot was never called on `a` or `b` | G1, G4 | unit |
| T2 | Backpressure gates then resumes a source's READ | Fake `a`,`b`; `start`; `a.read` always yields data, `b.write` (the sink of dir A) scripted `AGAIN`; call `ready(a,READ,r)` until dir-A ring reaches `CAP`; then script `b.write`→`OK` and call `ready(b,WRITE,r)` | While dir A's ring is full, the last `set_interest` recorded for `a` has READ clear; after `b` accepts bytes and dir A drains below `CAP`, a later `set_interest` for `a` re-sets READ | G1 | unit |
| T3 | Half-close propagation + dual-EOF completion | Fake `a`,`b`; `start`; `a.read`→`"abc"` then `EOF`; `b.read`→`"xyz"` then `EOF`; drive both directions to drained via `ready` calls | `b.shutdown_write` called once and `a.shutdown_write` called once; `b` received `"abc"`, `a` received `"xyz"`; `on_done` fires once with `ODIN_RELAY_V2_OK`, `err == 0` | G2 | unit |
| T4 | Write fault → one ERROR; destroy-in-`on_done` leaves transports | Fake `a`,`b`; `start`; dir with buffered bytes whose sink `write` returns `IO_ERROR` with `errno == EPIPE`; `on_done` calls `odin_relay_v2_destroy(r)` | `on_done` fires once with `ODIN_RELAY_V2_ERROR`, `err == EPIPE`; the relay called `destroy` on neither transport; under the P2 AddressSanitizer command no use-after-free or double-free is reported (destroy frees only relay state) | G3, G4, S2 | unit |
| T5 | Read fault → one ERROR with errno | Fake `a`,`b`; `start`; `a.read` returns `IO_ERROR` with `errno == ECONNRESET`; `ready(a,READ,r)` | `on_done` fires once with `ODIN_RELAY_V2_ERROR`, `err == ECONNRESET` | G3 | unit |
| T6 | ERROR readiness: latched error surfaces; benign ERROR keeps relaying | Fake `a`,`b`; `start`; (a) `ready(a,ERROR,r)` with `a.read`→`AGAIN` and `a.error`→`ECONNRESET`; (b) fresh relay, `ready(a,READ\|ERROR,r)` with `a.read`→`"d"` and `a.error`→`0` | (a) `on_done` once, `ODIN_RELAY_V2_ERROR`, `err == ECONNRESET`; (b) no `on_done`, `outcome` stays open, `"d"` forwarded to `b` on the next drain | G3 | unit |
| T7 | `destroy` on a still-running relay stops both watches | Fake `a`,`b`; `start` (both `cur == READ`); `odin_relay_v2_destroy(r)` without completion | `set_interest(0)` recorded once on each of `a` and `b` before the relay frees; the fake `destroy` slot was not called on `a` or `b`; under the P2 AddressSanitizer command no use-after-free or double-free is reported | G4, S2 | unit |
| T8 | End-to-end OK over two fd transports; caller fds stay open | `socketpair`s; build fd transports `a`,`b` wired to `odin_relay_v2_ready`/`r`; peers write then `shutdown(SHUT_WR)` both ways; run loop | `on_done` once, `ODIN_RELAY_V2_OK`; each peer drains the forwarded bytes; both relay fds still open after `on_done`; caller `transport_destroy(a)`,`(b)` then `close` succeed | G1, G4 | integration |
| T9 | End-to-end half-close over fd transports | `socketpair`s; peer A writes `"abc"` then `shutdown(SHUT_WR)`; peer B, after reading `"abc"`, writes `"xyz"` then `shutdown(SHUT_WR)`; run loop | `on_done` once, `ODIN_RELAY_V2_OK`; peer B received `"abc"`, peer A received `"xyz"` | G2 | integration |
| T10 | Genuine read fault (RST) over fd transport | Loopback TCP pair for endpoint A; abort its peer with `SO_LINGER{1,0} + close`; run loop | `on_done` once, `ODIN_RELAY_V2_ERROR`, `err == ECONNRESET` | G3 | integration |
| T11 | Same-batch double error + destroy-in-`on_done` → exactly one `on_done` | TCP pairs for both endpoints; `CloseWithRst` both peers; `start`; get watch handles via `odin_fd_transport_test_io`; `PollBothReady`; inject `{io_a,ERROR}` and `{io_b,ERROR}` in one batch; `on_done` destroys `r` | `on_done` fires exactly once, `ODIN_RELAY_V2_ERROR`, `err == ECONNRESET`; no second callback; under the P2 AddressSanitizer command no use-after-free or double-free is reported | G4, S2 | integration |
| T12 | `start` rollback when the second endpoint's interest fails | `socketpair`s; pre-register an external watch on `fd_b` so transport `b`'s `set_interest(READ)` fails `EEXIST`; `create`; build `a`,`b`; `start(r,a,b)` | `start` returns `-1`, `errno == EEXIST`; `on_done` never fires; after the external watch is stopped, a second `start(r,a,b)` returns `0` and the relay runs to `ODIN_RELAY_V2_OK` | G4 | integration |
| T13 | Large payload over two fd transports: ring saturates to `CAP`, clears then re-arms READ, byte-exact across wrap | Two `socketpair`s; `PinSocketBuf` all four ends well below the payload; build fd transports `a`,`b` wired to `odin_relay_v2_ready`/`r`; `shutdown(pb, SHUT_WR)` (dir B idle); `create`; `start(r,a,b)`; a writer `std::thread` writes a 256 KiB pattern (`byte i = i & 0xff`) to `pa` in fixed chunks, publishing cumulative accepted bytes via `std::atomic<size_t> written`, then `shutdown(pa, SHUT_WR)`; a reader `std::thread` **defers its first read** until `written` has reached ≥ `CAP` and held steady (the writer is blocked, so the whole pipeline including the relay's `CAP`-byte ring is full and `a`'s READ interest has been cleared), records `written_at_gate = written`, then drains `pb` to EOF in fixed chunks, tallying; `on_done` records status and stops the loop; arm 1 s watchdog; `run`; `join` both threads | `on_done` fires once, `ODIN_RELAY_V2_OK`; `written_at_gate < 262144` — the writer was still blocked by backpressure when the reader began draining, proving the ring saturated to `CAP` and the relay cleared `a`'s READ interest (the backpressure stop); the reader collected exactly 262144 bytes byte-equal to the written pattern, in order (proving the cleared READ interest re-armed and the ring reassembled across wrap with no loss or reorder); under the P2 AddressSanitizer command no heap-overflow is reported | G1, S1 | integration |
| T14 | Same-batch double error, **no** destroy-in-`on_done` → exactly one `on_done` (no-destroy path) | TCP pairs for both endpoints; `CloseWithRst` both peers; `start`; get watch handles via `odin_fd_transport_test_io`; `PollBothReady`; inject `{io_a,ERROR}` and `{io_b,ERROR}` in one batch; `on_done` records `status`/`err`/`calls` and stops the loop but does **not** destroy; `odin_relay_v2_destroy(r)` after `run` returns | `on_done` fires exactly once (`calls == 1`), `ODIN_RELAY_V2_ERROR`, `err == ECONNRESET`; no second callback — on the no-destroy path the `calls == 1` assertion catches the compound failure that clears only the faulting endpoint's interest **and** drops the `torn_down` latch (a logic double-completion T11's in-`on_done` `destroy` masks and ASan cannot see); under the P2 AddressSanitizer command no use-after-free or double-free is reported | G4, S2 | integration |
| T15 | `destroy` aborts a still-running relay over fd transports; no later readiness re-enters freed state | Two `AF_UNIX` `socketpair`s, all four ends `O_NONBLOCK`; build fd transports `a`,`b` wired to `odin_relay_v2_ready`/`r`; `create`; `start(r,a,b)`; `write(pa,"x",1)`; arm a one-shot 30 ms timer whose callback `read(pb)` (recording whether it got `"x"`), then `odin_relay_v2_destroy(r)` (aborting the still-running relay whose two READ interests are still active), then `write(pa,"y",1)`, `write(pb,"z",1)`; arm 100 ms watchdog; `run`; after `run`, `transport_destroy(a)`/`(b)` then `close` all four fds | the 30 ms callback's `read(pb)` yielded `"x"`, proving `start` registered the interests and the relay forwarded — fails against the P1 interest-free stub; `on_done.calls == 0` (`destroy` never invokes `on_done`); `timed_out == true` (only the watchdog stops the loop); under the P2 AddressSanitizer command no use-after-free or double-free is reported — `destroy` stopped both still-active interests, so the post-`destroy` `"y"`/`"z"` readiness is not dispatched into the freed relay (a `destroy` that left the interests active would re-enter the freed relay → use-after-free) | G4, S2 | integration |
| T16 | Failed half-close → one aggregated ERROR with errno | Fake `a`,`b`; `start`; script `a.read`→`EOF` with dir A's ring empty so `drive` reaches the half-close; `b.shutdown_write`→ failure with `errno == EPIPE`; `ready(a,READ,r)` | `on_done` fires once with `ODIN_RELAY_V2_ERROR`, `err == EPIPE` — the failed `odin_transport_shutdown_write` aggregates into the single error completion | G3 | unit |

## 6. Implementation Plan

- **P1. Land the `relay_v2` surface and a stub, with `T1`–`T16` executable-red behind a skip gate.**
  - **Scope:** add `odin/relay_v2.h` with the §3.2.1 types and the `create`/`ready`/`start`/`destroy` declarations; add `odin/relay_v2.c` with a real `odin_relay_v2_create` (allocates the object and the two 64 KiB buffers, stores `on_done`/`user_data`, zeroes `dir`/`end`) and a real `odin_relay_v2_destroy` that frees the buffers and object **but issues no `set_interest`**, plus stubs chosen so every row is red: `odin_relay_v2_start` returns `0` without binding `a`/`b` or registering any interest, and `odin_relay_v2_ready` is empty; add `odin/relay_v2_testing.c` (`#include "relay_v2.c"`) so the translation unit links into the test binary, matching the sibling shim `odin/transport_testing.c`; add `relay_v2_unittests.cpp` with `T1`–`T16`, each guarded by `if (!getenv("ODIN_RELAY_V2_RED")) GTEST_SKIP() << "pending RFC-014 P2";` before its assertions, the fake transport for `T1`–`T7` and `T16`, and the RFC-011 fork/watchdog/`socketpair`/TCP fixtures for `T8`–`T15`; in `odin/BUILD.gn` add `source_set("odin_relay_v2")` (`sources` = `relay_v2.c`, `relay_v2.h`; `public_deps = [ ":odin_transport" ]`), add `:odin_relay_v2` to the `:odin` deps, and add `relay_v2.h`, `relay_v2_testing.c`, `relay_v2_unittests.cpp` to `:odin_unittests` sources (no new config — the rows reuse the already-configured `ODIN_TRANSPORT_FD_TESTING` and `ODIN_EVENT_LOOP_TESTING`). Red-verification command: `ODIN_RELAY_V2_RED=1 out/odin_unittests --gtest_filter='OdinRelayV2*'`.
  - **Depends on:** None.
  - **Done when:** `:odin_unittests` links with the new module, and the default suite (`out/odin_unittests`) reports `T1`–`T16` **skipped** and stays green; the red-verification command runs `T1`–`T16` and reports every one **failing** against the stubs — `T1`/`T3` (the empty `ready` forwards nothing and issues no `shutdown_write`), `T2` (the empty `ready` records no `set_interest` mask change), `T4`/`T5`/`T6` (the empty `ready` never sets an outcome, so `on_done` never fires), `T7` (the `set_interest`-free `destroy` records no `set_interest(0)`), `T8`/`T9`/`T10` (the interest-free `start` leaves the loop with no watch, so no readiness arrives and the watchdog trips), `T11` (the interest-free `start` leaves the transports unwatched, so `odin_fd_transport_test_io` returns `ENOENT` and the setup assertion fails), `T12` (the always-`0` `start` returns `0` where `-1`/`EEXIST` is asserted), `T13` (the interest-free `start` forwards nothing, so the writer blocks once its pinned socket buffers fill and the deferred reader parks forever on the `written >= CAP` saturation gate the unfilled ring never satisfies; the child blocks in `join()` and surfaces red through the replicated 2 s fork deadline — the single disclosed hang-as-red row, with `T8` corroborating that the missing behavior is forwarding), `T14` (like `T11`, the interest-free `start` leaves the transports unwatched, so `odin_fd_transport_test_io` returns `ENOENT` and the setup assertion fails before any readiness is injected), `T15` (the interest-free `start` forwards nothing, so the 30 ms callback's `read(pb)` never sees `"x"` and the `read(pb) == "x"` assertion fails against the stub), and `T16` (the empty `ready` never sets an outcome, so `on_done` never fires and the `calls == 1`/`ODIN_RELAY_V2_ERROR`/`EPIPE` assertion fails).

- **P2. Implement the relay and turn `T1`–`T16` green.**
  - **Scope:** replace the `relay_v2.c` stubs with the full implementation from §3.2.2–§3.2.4 — `odin_relay_v2_ready` (write-then-read-then-error-probe ordering), `do_read`/`do_write` over the `CAP` ring via `odin_transport_read`/`odin_transport_write`, `reconcile` via `odin_transport_set_interest`, `drive` with `odin_transport_shutdown_write` and dual-`write_shut` completion, and `teardown` with the `torn_down` latch and clear-both-interests step — and extend `odin_relay_v2_start` to bind `a`/`b`, set READ interest on each, and roll the first back on the second's failure, and `odin_relay_v2_destroy` to stop any interest still held before freeing; remove the `GTEST_SKIP` guard from `T1`–`T16`. This is the first phase where the §4 S1 flood path and S2 same-batch teardown path exist in production, and the same phase lands their mitigations — §3.2.2's contiguous-run read bound and READ-clear backpressure (S1), and §3.2.4's clear-both-interests + `torn_down` latch together with §3.2.1's `destroy`-stops-interest (S2).
  - **Depends on:** P1.
  - **Done when:** `T1`–`T16` pass un-gated on a clean `out/odin_unittests` run after the guards are removed — including T13's `written_at_gate < 262144` saturation assertion (the S1 backpressure-stop proof) plus its byte-exact 262144-byte in-order delivery across wrap, T14's no-destroy same-batch `on_done.calls == 1` assertion (the no-destroy double-completion check T11 cannot make), T15's abort-path `on_done.calls == 0` with the post-abort `"y"`/`"z"` readiness left undispatched, and T16's failed-half-close single-`ODIN_RELAY_V2_ERROR` (`err == EPIPE`); and `./tool/gn gen out/odin_asan --args='target_os="mac" is_asan=true'`, `./tool/ninja -C out/odin_asan odin_unittests`, and `out/odin_asan/odin_unittests --gtest_filter='OdinRelayV2*'` exit with no AddressSanitizer report — giving the S1 no-heap-overflow coverage (T13) and the S2 no-use-after-free/double-free coverage (T4, T11, T14, T15).
