# RFC-013: Pluggable Byte-Stream Transport Interface and fd Implementation

## 1. Summary

Add `odin/transport.{c,h}` and `odin/transport_fd.{c,h}`: an abstract byte-stream transport — an `odin_transport_t` carrying a const function-pointer vtable (`read`, `write`, `shutdown_write`, `set_interest`, `error`, `destroy`) plus thin dispatcher functions that forward each call to the installed implementation — together with one concrete implementation, `odin_fd_transport_create`, that backs the vtable with `read(2)` / `write(2)` / `shutdown(SHUT_WR)` / `getsockopt(SO_ERROR)` and the RFC-010 `odin_event_io_*` watch API over a caller-owned nonblocking connected stream socket, so the byte-forwarding, half-close, and error-classification logic the current fd↔fd relay (RFC-011) performs inline against raw fds can later be driven through one transport-agnostic interface (the planned xquic stream transport is the motivating future implementation and is out of scope here).

## 2. Goals

- **G1.** Provide a transport interface — an `odin_transport_t` value carrying a function-pointer vtable, plus public dispatcher functions `odin_transport_read` / `odin_transport_write` / `odin_transport_shutdown_write` / `odin_transport_set_interest` / `odin_transport_error` / `odin_transport_destroy` — that forwards every call to the installed implementation, so a single consumer drives byte forwarding, half-close, readiness, and error retrieval without naming a concrete transport.

- **G2.** Provide an fd implementation, `odin_fd_transport_create(loop, fd, on_ready, user_data, &t)`, whose vtable maps `read`→`read(2)`, `write`→`write(2)`, `shutdown_write`→`shutdown(fd, SHUT_WR)`, `error`→`getsockopt(SO_ERROR)`, and `set_interest`→the RFC-010 `odin_event_io_start`/`_update`/`_stop` watch lifecycle, delivering loop readiness to `on_ready`, preserving the byte-transfer / `EAGAIN` / orderly-EOF / genuine-error classification the RFC-011 relay relies on, and never closing `fd`.

- **G3.** Keep the interface open for extension: a new transport lands as a sibling `transport_<name>.{c,h}` implementing the same vtable with no edit to `transport.{c,h}` or `transport_fd.{c,h}`. non-testable: this extensibility outcome — a second implementation conforming to the vtable without touching the existing modules — cannot be exercised by a §5 row without shipping that second implementation, which is out of scope here.

## 3. Design

### 3.1 Overview

This RFC adds two new leaf modules to the Odin library and rewires no existing caller. `odin/transport` is a pure abstraction: it defines the `odin_transport_t` value, the vtable type, the readiness flags, the I/O-result enum, and the dispatcher functions, and depends on nothing (only `<stddef.h>` for `size_t`). `odin/transport_fd` is the first concrete implementation; it depends on `odin/transport` and on `odin/event_loop` (RFC-010), and on no other Odin module.

A consumer holds an `odin_transport_t *` and calls only the `odin_transport_*` dispatchers; each dispatcher reads the object's vtable pointer and forwards to the installed slot. The fd implementation embeds the `odin_transport_t` as the first member of its private struct, so the dispatcher's `odin_transport_t *` and the implementation's own pointer are the same address. Data flows in through `odin_transport_read` (which the fd impl serves from `read(2)` on the caller-owned socket) and out through `odin_transport_write` (`write(2)`); readiness flows the other way — the consumer expresses interest with `odin_transport_set_interest`, the fd impl translates that to an `odin_event_io_*` watch on the loop, and loop readiness arrives back at the consumer's `on_ready` callback. The transport owns only its own object and (for the fd impl) its one event-loop watch handle; it never owns or closes the underlying fd, mirroring the RFC-011 relay's caller-owns-the-fd contract.

```
        consumer (future RFC-011 relay v2 / future xquic adapter)
                         |
                         |  odin_transport_read / _write / _shutdown_write
                         |  odin_transport_set_interest / _error / _destroy
                         v
            +-------------------------------+
            |  odin_transport_t { *vt }     |   (odin/transport.{c,h})
            +-------------------------------+
                         |  vtable slots (function pointers)
            +------------+--------------------------+
            v                                       v
   +----------------------------+        +---------------------------+
   | fd impl  transport_fd.c    |        | xquic impl  (future, OOS) |
   +----------------------------+        +---------------------------+
            |                                       |
            |  read(2)/write(2)/shutdown(SHUT_WR)   |  xqc_stream_recv/_send/fin
            |  getsockopt(SO_ERROR)                 |  engine-driven notifications
            |  odin_event_io_start/_update/_stop    |
            v                                       v
     kernel socket + odin_event_loop          xquic engine + UDP socket
```

### 3.2 Detailed Design

#### 3.2.1 Transport interface (abstract value, vtable, and dispatch)

Contract surface — `odin/transport.h` (include guard, copyright, and per-field doc-comments omitted; the `extern "C"` linkage is load-bearing because the test translation units are C++):

```c
/* odin/transport.h */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct odin_transport_t odin_transport_t;

/* Readiness flags: output bits delivered to odin_transport_ready_cb, and the
 * input mask accepted by set_interest (READ|WRITE only; ERROR is output-only). */
#define ODIN_TRANSPORT_READ  0x01u
#define ODIN_TRANSPORT_WRITE 0x02u
#define ODIN_TRANSPORT_ERROR 0x04u

typedef enum odin_transport_io_t {
  ODIN_TRANSPORT_OK = 0, /* transferred *out_n bytes                    */
  ODIN_TRANSPORT_AGAIN,  /* would block; wait for the next readiness    */
  ODIN_TRANSPORT_EOF,    /* read only: peer half-closed, orderly        */
  ODIN_TRANSPORT_ERROR,  /* failed; errno is set                        */
} odin_transport_io_t;

typedef void (*odin_transport_ready_cb)(odin_transport_t *t,
                                        unsigned int events, void *user_data);

typedef struct odin_transport_vtable_t {
  odin_transport_io_t (*read)(odin_transport_t *t, void *buf, size_t len,
                              size_t *out_n);
  odin_transport_io_t (*write)(odin_transport_t *t, const void *buf, size_t len,
                               size_t *out_n);
  int  (*shutdown_write)(odin_transport_t *t);
  int  (*set_interest)(odin_transport_t *t, unsigned int events);
  int  (*error)(odin_transport_t *t);
  void (*destroy)(odin_transport_t *t);
} odin_transport_vtable_t;

struct odin_transport_t {
  const odin_transport_vtable_t *vt;
};

odin_transport_io_t odin_transport_read(odin_transport_t *t, void *buf,
                                        size_t len, size_t *out_n);
odin_transport_io_t odin_transport_write(odin_transport_t *t, const void *buf,
                                         size_t len, size_t *out_n);
int  odin_transport_shutdown_write(odin_transport_t *t);
int  odin_transport_set_interest(odin_transport_t *t, unsigned int events);
int  odin_transport_error(odin_transport_t *t);
void odin_transport_destroy(odin_transport_t *t);

#ifdef __cplusplus
}
#endif
```

**Unstated contract.**

- *Object layout is the extension point.* `struct odin_transport_t` is intentionally non-opaque and holds exactly one member, a pointer to a `const odin_transport_vtable_t`. An implementation embeds `odin_transport_t base;` as the **first** member of its private struct; because the base sits at offset 0, the dispatcher's `odin_transport_t *` is bit-identical to the implementation's own struct pointer, so each slot may cast `t` back to its concrete type. Consumers never read `vt` directly; they call the dispatchers.
- *Dependency-minimal header.* `transport.h` pulls in only `<stddef.h>` (for `size_t`) — no event-loop, socket, or fd header — so the abstraction carries none of any concrete transport's dependencies; the fd implementation's event-loop and socket includes live in `transport_fd.{c,h}` alone.
- *Dispatch is the only behavior here.* Each dispatcher forwards to the matching vtable slot and returns its result unchanged. `odin_transport_destroy(NULL)` is a no-op (the dispatcher null-checks before forwarding); every other dispatcher treats `t` as a non-null precondition and does not null-check.
- *Result and error model.* `read`/`write` return `odin_transport_io_t`; on `ODIN_TRANSPORT_OK` they set `*out_n` to the byte count (for `read`, `> 0`; for `write` with `len > 0`, `> 0`). `read` may additionally return `ODIN_TRANSPORT_EOF` (orderly peer half-close, `*out_n == 0`); `write` never returns `EOF`. Both may return `ODIN_TRANSPORT_AGAIN` (retry on the next readiness) or `ODIN_TRANSPORT_ERROR` (with `errno` set). `shutdown_write` and `set_interest` follow the Odin house rule (`0` on success, `-1` with `errno` set). `error` returns the latched **asynchronous** transport error (the `getsockopt(SO_ERROR)` analogue) — `0` when none — and is the channel a consumer uses to classify an `ODIN_TRANSPORT_ERROR` readiness that produced no synchronous `read`/`write` failure.
- *Readiness model.* `set_interest(events)` takes a (possibly empty) subset of `ODIN_TRANSPORT_READ | ODIN_TRANSPORT_WRITE`; `ODIN_TRANSPORT_ERROR` is output-only and must not be set. An empty mask means "no readiness wanted." After interest is registered, the implementation invokes the `odin_transport_ready_cb` supplied at construction with an `events` mask drawn from `READ | WRITE | ERROR`. Watches are level-triggered (inherited from the RFC-010 loop).
- *Threading & lifetime.* All dispatchers and the readiness callback run on the implementation's owner thread; the interface adds no locks. `odin_transport_destroy` is callable from within the readiness callback (an implementation must not touch its own state after invoking a consumer callback that may destroy it).

**Mechanism.** Each dispatcher is a one-line forward, e.g.

```
odin_transport_read(t, buf, len, out_n):
    return t->vt->read(t, buf, len, out_n)

odin_transport_destroy(t):
    if t != NULL:
        t->vt->destroy(t)
```

Satisfies: G1 via the `odin_transport_t` value, the `odin_transport_vtable_t` function-pointer table, and the six dispatcher functions that forward to the installed slots; G3 via the function-pointer vtable indirection and the first-member-`base` embedding convention that together let a new implementation land as a sibling without editing this module.

#### 3.2.2 fd implementation (vtable over a caller-owned socket + event loop)

Contract surface — `odin/transport_fd.h`:

```c
/* odin/transport_fd.h */
#include "odin/event_loop.h"
#include "odin/transport.h"

int odin_fd_transport_create(odin_event_loop_t *loop, int fd,
                             odin_transport_ready_cb on_ready, void *user_data,
                             odin_transport_t **out);
```

**Unstated contract.**

- *Construction.* `odin_fd_transport_create` allocates the implementation struct, stores `loop` / `fd` / `on_ready` / `user_data`, writes `*out`, and returns `0`. It registers **no** event-loop watch (readiness begins only when the consumer calls `odin_transport_set_interest`), so it touches the loop not at all and its only failure is `ENOMEM` (returns `-1`, `errno == ENOMEM`, `*out` untouched). Preconditions: `fd` is a caller-owned, nonblocking, connected stream socket; `loop` is a live loop owned by the calling thread; `on_ready` is non-null. Owner-thread API.
- *Ownership.* The implementation never owns `fd`: neither `create`, the vtable `destroy`, nor any op closes it — the caller opens and closes it, exactly as the RFC-011 relay leaves caller fds untouched (`odin/relay.h:11-17`). The `destroy` slot stops any active watch and frees the implementation struct only.
- *`set_interest` masks the watch.* A non-empty `READ|WRITE` mask starts the watch if none is active (`odin_event_io_start`) or updates it otherwise (`odin_event_io_update`); an empty mask stops the active watch (`odin_event_io_stop`) and is a no-op if none is active. This reproduces the lazy start/stop the relay's `reconcile` performs today (`odin/relay.c:149-177`). `set_interest` returns `0`, or `-1` with the `errno` from the underlying `odin_event_io_*` call.
- *Op → syscall mapping preserves RFC-011 classification.* `read` mirrors `do_read` (`odin/relay.c:104-125`): `n > 0` → `OK` with `*out_n = n`; `n == 0` → `EOF`; `EAGAIN`/`EWOULDBLOCK`/`EINTR` → `AGAIN`; any other → `ERROR` with `errno` set. `write` mirrors `do_write` (`odin/relay.c:128-144`): `n > 0` → `OK` with `*out_n = n`; `EAGAIN`/`EWOULDBLOCK`/`EINTR` → `AGAIN`; else `ERROR`. `shutdown_write` issues `shutdown(fd, SHUT_WR)` as the relay's half-close does (`odin/relay.c:205-216`). `error` is the `so_error` probe — `getsockopt(SO_ERROR)`, or the `getsockopt` `errno` on failure (`odin/relay.c:93-100`).
- *Readiness forwarding.* The watch is registered with an internal `odin_event_io_cb` trampoline; on each readiness it translates the loop's `ODIN_EVENT_READ|WRITE|ERROR` (`odin/event_loop.h:41-43`) to the equal-valued `ODIN_TRANSPORT_*` bits and invokes `on_ready(&self->base, events, user_data)`. The fd implementation owns this translation; `transport.h` does not depend on `event_loop.h`.

**Mechanism.** Private struct and the two non-trivial slots (`base` first so the cast in every slot is valid):

```
struct odin_fd_transport_t:
    odin_transport_t         base        # vt pointer; offset 0
    odin_event_loop_t       *loop
    int                      fd           # not owned
    odin_event_io_t         *io           # active watch, or NULL
    unsigned int             cur          # current ODIN_EVENT_* mask
    odin_transport_ready_cb  on_ready
    void                    *user_data

fd_set_interest(t, mask):                 # mask subset of TRANSPORT_READ|WRITE
    s  = (odin_fd_transport_t *)t
    ev = mask translated to ODIN_EVENT_* bits
    if ev == 0:
        if s.io != NULL: odin_event_io_stop(s.io); s.io = NULL
        s.cur = 0; return 0
    if s.io == NULL:
        if odin_event_io_start(s.loop, s.fd, ev, fd_on_io, s, &s.io) != 0:
            return -1
    else if ev != s.cur:
        if odin_event_io_update(s.io, ev) != 0: return -1
    s.cur = ev; return 0

fd_on_io(loop, io, fd, events, user_data):
    s = (odin_fd_transport_t *)user_data
    s.on_ready(&s.base, events translated to ODIN_TRANSPORT_* bits, s.user_data)
```

Satisfies: G2 via `odin_fd_transport_create`, the vtable whose slots map one-to-one onto `read(2)`/`write(2)`/`shutdown(SHUT_WR)`/`getsockopt(SO_ERROR)` and the `odin_event_io_*` watch lifecycle while preserving the RFC-011 byte/EOF/`EAGAIN`/error classification, and the never-close-the-fd ownership rule enforced in `create` and the `destroy` slot.

## 4. Security

Not applicable — the fd implementation is a thin wrapper over `read(2)` / `write(2)` / `shutdown(2)` / `getsockopt(SO_ERROR)` and the RFC-010 watch API on a caller-owned socket; it moves opaque bytes between a caller-owned, caller-sized buffer (every transfer is bounded by the consumer-supplied `len`, which `read(2)`/`write(2)` honor) and the socket, parses or interprets none of those bytes, sizes nothing from received data, allocates only its fixed-size implementation struct, holds no credentials, and persists nothing, so it introduces no trust boundary beyond the one the socket layer already presents.

## 5. Testing Strategy

`T1` lives in `transport_unittests.cpp` and exercises the abstract dispatchers against a test-local fake vtable (no fd, no loop). `T2`–`T14` live in `transport_fd_unittests.cpp` and exercise the public `odin_transport_*` API against an `odin_fd_transport_create` instance, using a nonblocking `AF_UNIX` `socketpair` plus a live `odin_event_loop`, the `ODIN_EVENT_LOOP_TESTING` readiness-injection hook `odin_event_loop_test_queue_backend_events` (`odin/event_loop_internal_test.h:82-85`) fed the transport's live watch handle via a new `odin_fd_transport_test_io` accessor, and — for the genuine read-fault row (`T9`) and the latched-async-error row (`T14`) — a loopback TCP pair aborted with `SO_LINGER{1,0} + close` to force an RST (the pattern at `odin/relay_unittests.cpp:179-185`); the write-`AGAIN` row (`T12`) pins both socket buffers small and fills the send buffer (the `PinSocketBuf` pattern at `odin/relay_unittests.cpp:189-192`), and the write-`ERROR` row (`T13`) closes the peer with `SIGPIPE` ignored to force `EPIPE` (as `odin/relay_unittests.cpp:505-542` does). Every `transport_fd` row (`T2`–`T14`) runs under the same `fork` + `waitpid` 2 s deadline fixture the sibling relay suite uses (`odin/relay_unittests.cpp:5-7`); the loop-running rows (`T2`, `T8`, `T9`, `T10`, `T11`, `T14`) additionally arm a per-row watchdog timer that stops the loop, so a row whose expected readiness never arrives — or whose `odin_event_loop_run` would otherwise block (e.g. `T10`, which asserts that *no* callback fires) — fails on the deadline instead of hanging the suite.

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | Dispatchers forward to the installed vtable | A fake `odin_transport_vtable_t` whose six slots record their call and arguments and return sentinels; invoke each `odin_transport_*` dispatcher; also call `odin_transport_destroy(NULL)` | Each slot is invoked exactly once with the forwarded `buf`/`len`/`events`; each dispatcher returns the slot's sentinel; `odin_transport_destroy(NULL)` invokes no slot and does not crash | G1 | unit |
| T2 | Read delivers buffered bytes | socketpair; peer writes `"hello"`; `set_interest(READ)`; run loop until `on_ready` reports `READ`; then `odin_transport_read(t, buf, 64, &n)` | Returns `ODIN_TRANSPORT_OK`, `n == 5`, `buf[0..5) == "hello"` | G2 | integration |
| T3 | Read reports orderly EOF | socketpair; peer `shutdown(SHUT_WR)`; `odin_transport_read(t, buf, 64, &n)` | Returns `ODIN_TRANSPORT_EOF`, `n == 0` | G2 | integration |
| T4 | Read with no data yields AGAIN | socketpair, no peer write; `odin_transport_read(t, buf, 64, &n)` | Returns `ODIN_TRANSPORT_AGAIN` | G2 | integration |
| T5 | Write emits bytes and reports count | socketpair; `odin_transport_write(t, "hi", 2, &n)`; peer reads | Returns `ODIN_TRANSPORT_OK`, `n == 2`; peer `read` yields `"hi"` | G2 | integration |
| T6 | shutdown_write half-closes, reverse stays open | socketpair; `odin_transport_shutdown_write(t)`; peer `read`; then peer writes and `odin_transport_read` | `shutdown_write` returns `0`; peer `read` returns `0` (EOF); a subsequent `odin_transport_read` still returns the peer's later bytes as `OK` | G2 | integration |
| T7 | set_interest starts, updates, and stops the watch | `set_interest(READ)`, then `set_interest(READ\|WRITE)`, then `set_interest(0)`; query `odin_fd_transport_test_io` after each | First call makes the watch active (handle non-null); second keeps it active; third stops it (`odin_fd_transport_test_io` reports inactive via `errno == ENOENT`); each call returns `0` | G2 | integration |
| T8 | Readiness forwards ERROR; benign ERROR latches no error | `ODIN_EVENT_LOOP_TESTING` loop; `set_interest(READ)`; inject `{io, ODIN_TRANSPORT_READ\|ODIN_TRANSPORT_ERROR}`; run loop; then `odin_transport_error(t)` on the healthy socketpair | `on_ready` receives `events` with both `READ` and `ERROR` set; `odin_transport_error(t)` returns `0` | G2 | integration |
| T9 | Genuine read fault surfaces as ERROR | loopback TCP pair; abort peer with `SO_LINGER{1,0} + close` (RST); `set_interest(READ)`; run loop until `on_ready` fires; then `odin_transport_read(t, buf, 64, &n)` | Returns `ODIN_TRANSPORT_ERROR`; `errno == ECONNRESET` when read immediately after the call, before any other syscall (the read-derived errno, never a post-read `error()` probe) | G2 | integration |
| T10 | destroy stops the watch without closing the fd | socketpair; peer writes a byte so the read side is ready; `set_interest(READ)` (watch active); `odin_transport_destroy(t)`; then run the loop and inspect the fd | The underlying `fd` is still open (`fcntl(fd, F_GETFD) != -1`); a subsequent `odin_event_loop_run` dispatches no callback for the destroyed transport (the watchdog stops the loop) | G2 | integration |
| T11 | set_interest update applies WRITE interest | socketpair (relay end writable, no peer write); `set_interest(READ)`, then `set_interest(READ\|WRITE)`; run loop | `on_ready` fires with `ODIN_TRANSPORT_WRITE` set in its `events` mask | G2 | integration |
| T12 | Write on a full send buffer yields AGAIN | socketpair; pin both socket buffers small (`PinSocketBuf`, `odin/relay_unittests.cpp:189-192`); peer never reads; `odin_transport_write` until the send buffer fills | A subsequent `odin_transport_write(t, buf, len, &n)` returns `ODIN_TRANSPORT_AGAIN` | G2 | integration |
| T13 | Write to a closed peer yields ERROR | socketpair; `SIGPIPE` ignored; close the peer end; `odin_transport_write(t, "hi", 2, &n)` | Returns `ODIN_TRANSPORT_ERROR`; `errno == EPIPE` when read immediately after the call | G2 | integration |
| T14 | Latched async error surfaces through `error()` before any read | loopback TCP pair; abort peer with `SO_LINGER{1,0} + close` (RST); `set_interest(READ)`; run loop until `on_ready` reports `ERROR`; then call `odin_transport_error(t)` before any `read`/`write` or other syscall | Returns `ECONNRESET` — the latched asynchronous error `getsockopt(SO_ERROR)` reads and clears, distinct from `T9`'s `read`-derived `ECONNRESET` | G2 | integration |

## 6. Implementation Plan

- **P1. Land the interface, an fd stub, and `T1`–`T14` executable-red behind a skip gate.**
  - **Scope:** add `odin/transport.h` (§3.2.1 types + dispatcher declarations) and `odin/transport.c` with dispatcher bodies **stubbed** to return fixed sentinels without consulting `vt` (so `odin_transport_destroy(NULL)` is the only correct behavior); add `odin/transport_fd.h` (§3.2.2 `create` declaration) and `odin/transport_fd.c` with a real `odin_fd_transport_create` plus partial vtable stubs chosen so every `T2`–`T14` assertion is red: `read`/`write` return `ODIN_TRANSPORT_ERROR` with `errno` set to a fixed sentinel distinct from `ECONNRESET` and `EPIPE` (the result is wrong for `T2`/`T3`/`T4`/`T12`, and the `errno` is wrong for `T9`/`T13`), `shutdown_write` returns `-1`, `error` returns a fixed nonzero sentinel (≠ `0`, so `T8`'s expected `0` fails, and ≠ `ECONNRESET`, so `T14`'s expected `ECONNRESET` fails), `set_interest` only **starts** the watch (`odin_event_io_start` wiring the real `fd_on_io` trampoline) and performs no update or empty-mask stop (so `T7`'s stop and `T11`'s `WRITE` update both go unobserved), and `destroy` is a no-op that neither stops the watch nor frees; add `odin/transport_fd_internal_test.h` declaring `odin_fd_transport_test_io` and **define** that accessor in `transport_fd.c` behind `#if defined(ODIN_TRANSPORT_FD_TESTING)` (reads `s->io`, returns `0` with `*out = s->io` when a watch is active else `-1`/`errno == ENOENT`, exactly as `odin/relay.c:349-360` does) so the symbol resolves at link time; add `odin/transport_fd_testing.c` (`#include "transport_fd.c"`) to compile that accessor into the test binary; add `transport_unittests.cpp` (`T1`) and `transport_fd_unittests.cpp` (`T2`–`T14`), each test guarded by `if (!getenv("ODIN_TRANSPORT_RED")) GTEST_SKIP() << "pending RFC-013 P2";` before its assertions; in `odin/BUILD.gn` add `source_set("odin_transport")` and `source_set("odin_transport_fd")` (public_deps `:odin_transport`, `:odin_event_loop`), add both to the `:odin` deps, add `config("odin_transport_fd_testing_config")` defining `ODIN_TRANSPORT_FD_TESTING`, and add `transport.c`, `transport.h`, `transport_unittests.cpp`, `transport_fd.h`, `transport_fd_internal_test.h`, `transport_fd_testing.c`, `transport_fd_unittests.cpp` plus that config to `:odin_unittests`. Red-verification command: `ODIN_TRANSPORT_RED=1 out/odin_unittests --gtest_filter='OdinTransport*:OdinFdTransport*'`.
  - **Depends on:** None.
  - **Done when:** `:odin_unittests` **links** — the defined `odin_fd_transport_test_io` resolves the references in `T7`/`T8`, whose `GTEST_SKIP()` is a runtime early-return that does not exclude their bodies from the link — and the default suite (`out/odin_unittests`) reports `T1`–`T14` **skipped** and stays green with the new modules linked into `:odin` and `:odin_unittests`; the red-verification command runs `T1`–`T14` and reports every one **failing** against the stubs: `T1` (dispatchers never reach the fake vtable), `T2`–`T6` (the `read`/`write`/`shutdown_write` sentinels are the wrong result), `T7` (`set_interest(0)` never stops the started watch, so the accessor still reports active), `T8` (`error` returns the nonzero sentinel ≠ `0`), `T9` (`read` returns the `ERROR` sentinel but with the fixed `errno`, so the `errno == ECONNRESET` assertion fails), `T10` (the no-op `destroy` leaves the watch active, so the post-`destroy` loop run dispatches a callback before the watchdog fires), `T11` (the start-only `set_interest` never applies `WRITE`, so the post-`set_interest(READ|WRITE)` loop run delivers no `WRITE` readiness), `T12` (the `write` sentinel returns `ERROR`, not `AGAIN`), `T13` (the `write` sentinel returns `ERROR` but with the fixed `errno`, so the `errno == EPIPE` assertion fails), and `T14` (the `error` sentinel is nonzero but ≠ `ECONNRESET`, so the `odin_transport_error(t) == ECONNRESET` assertion fails).

- **P2. Implement dispatch and the fd vtable; turn `T1`–`T14` green.**
  - **Scope:** replace the `transport.c` dispatcher stubs with the one-line `return t->vt->op(...)` forwards (and the `odin_transport_destroy(NULL)` guard) from §3.2.1; finish the `transport_fd.c` vtable from §3.2.2 — replace the `read`/`write`/`shutdown_write`/`error` sentinels with the real syscalls, extend `set_interest` (whose start path and `fd_on_io` trampoline already landed in P1) with the `odin_event_io_update` path and the lazy empty-mask `odin_event_io_stop`, and replace the no-op `destroy` with one that stops the watch and frees the struct without closing `fd`; remove the `GTEST_SKIP` guard from `T1`–`T14`. The `odin_fd_transport_test_io` accessor already landed in P1 and is unchanged.
  - **Depends on:** P1.
  - **Done when:** `T1`–`T14` pass un-gated on a clean `out/odin_unittests` run after the guards are removed.
