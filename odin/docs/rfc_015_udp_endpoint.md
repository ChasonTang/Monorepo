# RFC-015: Odin Nonblocking UDP Endpoint

## 1. Summary

Add `odin/udp.{c,h}`, a single-thread, event-loop-driven nonblocking UDP endpoint: `odin_udp_open` creates one nonblocking UDP `SOCK_DGRAM` socket for the caller's `AF_INET` or `AF_INET6` local address family, `bind(2)`s it to a caller-supplied local `struct sockaddr`, and owns it; the caller then drives unconnected per-datagram I/O through `odin_udp_recv` (one datagram -> payload plus the sender's `struct sockaddr`) and `odin_udp_send` (one datagram -> a caller-supplied destination `struct sockaddr`), each returning `ODIN_UDP_OK` / `ODIN_UDP_AGAIN` / `ODIN_UDP_IO_ERROR`, while readiness is delivered through one level-triggered RFC-010 `odin_event_io_*` watch reconciled by `odin_udp_set_interest`, depending on no Odin module but `odin/event_loop.h`.

## 2. Goals

- **G1.** Given a bound endpoint and positive receive capacity, deliver each received datagram's payload and the sender's address to the caller without blocking: `odin_udp_recv` returns `ODIN_UDP_OK` with the datagram bytes (`*out_n`, where `*out_n == 0` is a genuine zero-length datagram, **not** zero-capacity truncation or end-of-stream) and the source `struct sockaddr` written into the caller's buffer, `ODIN_UDP_AGAIN` when no datagram is queued, or `ODIN_UDP_IO_ERROR` with `errno` set on failure.
- **G2.** Transmit one datagram to a caller-supplied destination address without blocking: `odin_udp_send` returns `ODIN_UDP_OK` with `*out_n == len` on success, `ODIN_UDP_AGAIN` when the send buffer is momentarily full (a class distinct from error, so the caller can re-arm `ODIN_UDP_WRITE` interest and retry), or `ODIN_UDP_IO_ERROR` with `errno` set (e.g. a datagram larger than the datagram maximum).
- **G3.** Deliver event-loop readiness to the caller: `odin_udp_set_interest` accepts only a `READ|WRITE` interest subset, rejects `ODIN_UDP_ERROR` or unknown input bits with `-1` / `EINVAL` while preserving the active watch, reconciles valid masks into one level-triggered watch over the owned socket (starting, updating, or stopping it), and `on_ready` fires on the owner thread carrying the delivered `READ|WRITE|ERROR` mask.
- **G4.** `odin_udp_open` creates, binds, and owns exactly one nonblocking UDP `SOCK_DGRAM` socket whose family comes from the caller's `AF_INET` or `AF_INET6` local address; a *local setup* failure returns `-1` with `errno` set, writes nothing to `*out`, and leaves no socket open and no loop registration; and `odin_udp_close` stops any active watch, closes the owned socket, and frees the endpoint, and is callable from within `on_ready`.
- **G5.** Add the endpoint without starting any thread and without depending on any Odin module other than `odin/event_loop.h`. non-testable: the no-thread / single-dependency / loop-only property is a build-graph, include-set, and API-shape constraint verified by the GN deps and the public header, not expressible as an executable §5 row.

## 3. Design

### 3.1 Overview

`odin/udp` is a new leaf module in the Odin library and the next in-tree consumer of the RFC-010 event loop (after the RFC-011 relay, the RFC-012 dial, and the RFC-013 fd transport), using its I/O-watch API (`odin_event_io_start` / `_update` / `_stop`). It depends on no other Odin module: the caller supplies an already-built local `struct sockaddr`, so the endpoint runs no name resolution, and it selects no higher-level transport.

The caller holds a live `odin_event_loop_t` and an IPv4 or IPv6 local `struct sockaddr` and calls `odin_udp_open(loop, addr, addrlen, on_ready, user_data, &u)`. The endpoint creates exactly one nonblocking UDP `SOCK_DGRAM` socket whose family is `addr->sa_family`, `bind`s it to `addr`, and owns it for its lifetime. `open` registers **no** watch; readiness begins only when the caller calls `odin_udp_set_interest(u, mask)`, which starts, updates, or stops a single level-triggered watch over the socket for a `READ|WRITE` subset (mirroring the RFC-013 fd transport's lazy reconcile). When the loop reports readiness, `on_ready(u, events, user_data)` fires on the owner thread with a `READ|WRITE|ERROR` mask. The caller then moves datagrams with two ops, each acting on the single owned socket and returning the same three-way status: `odin_udp_recv` performs one `recvfrom` — one datagram into the caller's positive-capacity buffer plus the sender's address — and `odin_udp_send` performs one `sendto` — one datagram from the caller's buffer to a caller-supplied destination. The socket is unconnected, so successive datagrams may carry different peer addresses; the endpoint interprets none of the datagram bytes. The caller owns the `u` object and releases it with `odin_udp_close`, which stops the watch, closes the owned socket, and is safe to call from within `on_ready`.

This RFC does not pin an xquic adapter contract. The checkout contains xquic GN metadata but no `xquic/include/xquic/xquic.h`, so the future xquic wiring must verify the current header before binding names, signatures, return codes, timers, or local-address requirements. The endpoint remains a generic UDP primitive: a later adapter can map datagram payloads and peer addresses onto whatever xquic receive/send callbacks are present then. A wildcard-local-address server path may still need a `recvmsg(2)` + `IP_PKTINFO` / `IPV6_PKTINFO` extension because `recvfrom` alone reports the peer address, not which local address received the datagram.

```
  caller (future UDP consumer)
    |  odin_udp_open(loop, &local_addr, addrlen, on_ready, ud, &u)
    v
  odin_udp_t  --owns-->  nonblocking UDP SOCK_DGRAM socket  (bind local_addr)
    |   ^                          |
    |   |  on_ready(u,             |  one level-triggered
    |   |   READ|WRITE|ERROR, ud)  |  odin_event_io_* watch
    |   +------ odin_event_loop ---+
    |
    +-- odin_udp_recv(u, buf, len, &n, &src, &srclen) -> OK / AGAIN / IO_ERROR
    +-- odin_udp_send(u, buf, len, &n, &dst, dstlen)  -> OK / AGAIN / IO_ERROR
    +-- odin_udp_set_interest(u, READ|WRITE)  -> start / update / stop the watch
    +-- odin_udp_close(u)  -> stop watch, close owned socket, free
```

### 3.2 Detailed Design

#### 3.2.1 Public API, Ownership, and Lifecycle

```c
/* odin/udp.h */
#include <stddef.h>
#include <sys/socket.h>

#include "odin/event_loop.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct odin_udp_t odin_udp_t;

typedef enum odin_udp_io_t {
  ODIN_UDP_OK = 0,      /* transferred one datagram of *out_n bytes      */
  ODIN_UDP_AGAIN,       /* would block; wait for the next readiness       */
  ODIN_UDP_IO_ERROR,    /* failed; errno is set                           */
} odin_udp_io_t;

/* Readiness flags (same values as ODIN_EVENT_*): output bits delivered to
 * odin_udp_ready_cb, and the input mask accepted by set_interest (READ|WRITE
 * only; ERROR is output-only). */
#define ODIN_UDP_READ  0x01u
#define ODIN_UDP_WRITE 0x02u
#define ODIN_UDP_ERROR 0x04u

typedef void (*odin_udp_ready_cb)(odin_udp_t *u, unsigned int events,
                                  void *user_data);

int odin_udp_open(odin_event_loop_t *loop, const struct sockaddr *addr,
                  socklen_t addrlen, odin_udp_ready_cb on_ready, void *user_data,
                  odin_udp_t **out);

odin_udp_io_t odin_udp_recv(odin_udp_t *u, void *buf, size_t len, size_t *out_n,
                            struct sockaddr *src, socklen_t *srclen);

odin_udp_io_t odin_udp_send(odin_udp_t *u, const void *buf, size_t len,
                            size_t *out_n, const struct sockaddr *dst,
                            socklen_t dstlen);

int odin_udp_set_interest(odin_udp_t *u, unsigned int events);

void odin_udp_close(odin_udp_t *u);

#ifdef __cplusplus
}
#endif
```

**Unstated contract.**

- **Ownership & preconditions.** `odin_udp_open` creates the socket and owns it for the endpoint's whole lifetime; the `u` object is caller-owned. `addr` must be non-null and point to a valid `AF_INET` or `AF_INET6` local address (wildcard or specific local address), `addrlen` must be the correct length for that family (`sizeof(struct sockaddr_in)` or `sizeof(struct sockaddr_in6)`), `loop` a live loop owned by the calling thread, and `on_ready` non-null; `AF_UNIX` and every other non-IP family are rejected even when the host supports datagram sockets for them. For `odin_udp_recv`, `buf` / `out_n` / `src` / `srclen` are non-null, `len > 0`, `buf` points to at least `len` writable bytes, and `*srclen` is the capacity of `src` (e.g. `sizeof(struct sockaddr_storage)`). For `odin_udp_send`, `buf` / `out_n` / `dst` are non-null and `dstlen` is the length for `dst->sa_family`. Every entry point is an owner-thread API under the RFC-010 contract.
- **Open success.** On success `odin_udp_open` writes `*out` and returns `0`; it registers **no** watch, so the endpoint produces no `on_ready` callbacks until the caller calls `odin_udp_set_interest` with a non-empty mask. The socket is created with `socket(addr->sa_family, SOCK_DGRAM, IPPROTO_UDP)`, made nonblocking, and bound to `addr`.
- **Open failure & rollback.** A *local setup* failure returns `-1` with `errno` set, writes nothing to `*out`, and leaves no socket open and no loop registration: `EAFNOSUPPORT` when `addr->sa_family` is neither `AF_INET` nor `AF_INET6`, `ENOMEM` when the `u` object cannot be allocated, or the `errno` from `socket(2)` for an IP UDP socket, from making the socket nonblocking, or from `bind(2)` (e.g. `EADDRNOTAVAIL` for a non-local address, `EADDRINUSE` for a taken port). A socket created before a later setup step fails is closed before the return, so no descriptor leaks.
- **Receive.** `odin_udp_recv` performs one positive-capacity `recvfrom` and returns one datagram: `ODIN_UDP_OK` with `0 <= *out_n <= len` and the sender's address written into `src` (`*srclen` updated to its real length). Because `len > 0` is a call precondition, a received **zero-length** datagram is the only successful receive path that returns `ODIN_UDP_OK` with `*out_n == 0`; `recvfrom`'s `0` is a real empty datagram, **not** zero-capacity truncation or end-of-stream, the decisive difference from a stream `read(2)` (and from the RFC-013 fd transport's `ODIN_TRANSPORT_EOF`). UDP has no orderly close. A datagram longer than `len` is truncated to `len` bytes and the remainder discarded. `ODIN_UDP_AGAIN` means no datagram was queued (`EAGAIN` / `EWOULDBLOCK` / `EINTR`); `ODIN_UDP_IO_ERROR` carries `errno`. Because the socket is unconnected, consecutive calls may report different source addresses; the caller makes all peer-trust and packet-validation decisions.
- **Send.** `odin_udp_send` performs one `sendto` of `buf[0, len)` as a single datagram to `dst`. `ODIN_UDP_OK` always carries `*out_n == len` — UDP transmits a datagram atomically and never partially sends. `ODIN_UDP_AGAIN` (`EAGAIN` / `EWOULDBLOCK` / `EINTR`) means the send buffer was momentarily full; it is a distinct class from `ODIN_UDP_IO_ERROR` precisely so the caller can add `ODIN_UDP_WRITE` interest and retry on the next writable readiness. `ODIN_UDP_IO_ERROR` carries `errno` (e.g. `EMSGSIZE` when the datagram exceeds the datagram maximum).
- **Readiness.** `odin_udp_set_interest` takes a (possibly empty) subset of `ODIN_UDP_READ | ODIN_UDP_WRITE`; `ODIN_UDP_ERROR` is output-only and must not be set. If `events` contains `ODIN_UDP_ERROR` or any unknown bit, it returns `-1` with `errno == EINVAL` and leaves the active watch, cached mask, and backend registration unchanged. A valid non-empty mask starts the watch if none is active or updates it otherwise; a valid empty mask stops the active watch (a no-op if none is active). Start and update failures propagate `-1` with `errno` from `odin_event_io_start` / `_update` and preserve the previous endpoint state (`io == NULL` remains unwatched after a failed start; an already-active watch keeps its old mask after a failed update). `on_ready` is delivered on the owner thread with a `READ|WRITE|ERROR` mask; watches are level-triggered, so on a `READ` readiness the caller drains by calling `odin_udp_recv` until it returns `ODIN_UDP_AGAIN`.
- **Close.** `odin_udp_close` stops any active watch (its handle memory reclaimed by the loop's deferred-free per RFC-010), closes the owned socket, and frees the `u` object; it never invokes `on_ready`. Because `on_ready` is the endpoint's final action within a readiness dispatch (see §3.2.2), `odin_udp_close(u)` is legal from inside `on_ready`. `odin_udp_close(NULL)` is a no-op, and the pointer is dead afterward.
- **Threading.** All entry points and `on_ready` are owner-thread; the endpoint adds no locks.

Satisfies: G1 via the positive-capacity `odin_udp_recv` surface that returns `ODIN_UDP_OK` with the datagram bytes and source `struct sockaddr` (and the `*out_n == 0` zero-length-vs-zero-capacity-vs-EOF clause); G2 via the `odin_udp_send` surface whose `*out_n == len` success and distinct `ODIN_UDP_AGAIN` / `ODIN_UDP_IO_ERROR` arms let callers retry only transient send-buffer pressure; G3 via the `odin_udp_set_interest` / `odin_udp_ready_cb` readiness surface (the valid `READ|WRITE` interest input, invalid-mask rejection with state preservation, start/update/stop reconciliation, and `READ|WRITE|ERROR` delivery mask); G4 via the owns-the-IP-UDP-socket `open`, the `-1`/`errno`-with-`*out`-untouched local-setup-failure return, and `odin_udp_close`'s stop-watch / close-socket / free; G5 via the `event_loop.h`-only include set and the thread-free, owner-thread-only contract.

#### 3.2.2 Datagram I/O and Readiness Reconciliation

```c
/* odin_udp_t internal state (Proposed). `fd` is the owned SOCK_DGRAM socket,    */
/* set to -1 the instant it is closed; `io` is the single active watch (or NULL);*/
/* `cur` is the current ODIN_EVENT_* input mask. */
struct odin_udp_t {
  odin_event_loop_t *loop;
  int                fd;          /* owned socket; -1 once closed              */
  odin_event_io_t   *io;          /* active watch while interested; else NULL  */
  unsigned int       cur;         /* current ODIN_EVENT_* mask                 */
  odin_udp_ready_cb  on_ready;
  void              *user_data;
};
```

**Unstated contract.**

- **`recvfrom` / `sendto` classification.** Both ops map the syscall return identically: `n >= 0` → `ODIN_UDP_OK` with `*out_n = n` (so `recvfrom`'s `0` is an empty datagram for valid `len > 0` receives, never zero-capacity truncation or EOF, and `sendto` only ever returns `n == len`); `n < 0` with `errno` in `{EAGAIN, EWOULDBLOCK, EINTR}` → `ODIN_UDP_AGAIN`; any other `n < 0` → `ODIN_UDP_IO_ERROR` with `errno` left as the syscall set it. This reuses the EAGAIN/error split of the RFC-013 fd transport (`odin/transport_fd.c:40-72`) but drops the `n == 0 → EOF` arm, which does not exist for datagrams.
- **Lazy watch reconcile.** `odin_udp_set_interest` rejects any input bit outside `ODIN_UDP_READ | ODIN_UDP_WRITE` before translating to `ODIN_EVENT_*`; this matches `odin_event_io_start` / `_update`, whose `valid_input_mask` rejects zero, output-only `ODIN_EVENT_ERROR`, and unknown bits (`odin/event_loop.c:139-140, 1148-1194`). For valid masks: an empty result stops and clears the watch; a non-empty result starts the watch (when `io == NULL`) or updates it (when the mask changed), caching the mask in `cur` only after the loop call succeeds. This is the same single-watch reconcile the fd transport performs (`odin/transport_fd.c:84-112`); the endpoint never holds more than one watch over its one socket.
- **Mask translation and final-action callback.** The loop's readiness callback translates the delivered `ODIN_EVENT_READ | ODIN_EVENT_WRITE | ODIN_EVENT_ERROR` bits to the equal-valued `ODIN_UDP_*` bits and invokes `on_ready` as its **last** statement, reading no endpoint state afterward — so an `odin_udp_close(u)` from inside `on_ready` (which stops the watch and frees `u`) cannot be followed by a use-after-free.
- **Close discipline.** The endpoint closes the socket exactly once, in `odin_udp_close`, and on no other path; `open`'s rollback closes only a socket that `open` itself created before a later setup step failed.

**Mechanism.**

```
set_nonblocking(fd):                       /* returns 0, or -1 with errno set */
  flags = fcntl(fd, F_GETFL, 0)
  if flags == -1: return -1
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK)

odin_udp_open(loop, addr, addrlen, on_ready, user_data, out):
  if addr->sa_family != AF_INET and addr->sa_family != AF_INET6:
    errno = EAFNOSUPPORT; return -1
  u = calloc(1, sizeof *u)
  if u == NULL: errno = ENOMEM; return -1
  u.fd = socket(addr->sa_family, SOCK_DGRAM, IPPROTO_UDP)
  if u.fd < 0:                   save = errno; free(u); errno = save; return -1
  if set_nonblocking(u.fd) != 0: save = errno; close(u.fd); free(u); errno = save; return -1
  if bind(u.fd, addr, addrlen) != 0: save = errno; close(u.fd); free(u); errno = save; return -1
  u.loop = loop; u.io = NULL; u.cur = 0
  u.on_ready = on_ready; u.user_data = user_data
  *out = u; return 0

odin_udp_recv(u, buf, len, out_n, src, srclen):
  /* precondition: len > 0, so n == 0 cannot be zero-capacity truncation */
  n = recvfrom(u.fd, buf, len, 0, src, srclen)
  if n >= 0: *out_n = n; return ODIN_UDP_OK         /* n == 0 is an empty datagram, not EOF */
  if errno in {EAGAIN, EWOULDBLOCK, EINTR}: return ODIN_UDP_AGAIN
  return ODIN_UDP_IO_ERROR

odin_udp_send(u, buf, len, out_n, dst, dstlen):
  n = sendto(u.fd, buf, len, 0, dst, dstlen)
  if n >= 0: *out_n = n; return ODIN_UDP_OK         /* UDP is atomic: n == len */
  if errno in {EAGAIN, EWOULDBLOCK, EINTR}: return ODIN_UDP_AGAIN
  return ODIN_UDP_IO_ERROR

odin_udp_set_interest(u, events):
  if events & ~(ODIN_UDP_READ | ODIN_UDP_WRITE):
    errno = EINVAL; return -1
  ev = 0
  if events & ODIN_UDP_READ:  ev |= ODIN_EVENT_READ
  if events & ODIN_UDP_WRITE: ev |= ODIN_EVENT_WRITE
  if ev == 0:
    if u.io != NULL: odin_event_io_stop(u.io); u.io = NULL
    u.cur = 0; return 0
  if u.io == NULL:
    new_io = NULL
    if odin_event_io_start(u.loop, u.fd, ev, on_io, u, &new_io) != 0: return -1
    u.io = new_io
  else if ev != u.cur:
    if odin_event_io_update(u.io, ev) != 0: return -1
  u.cur = ev; return 0

on_io(loop, io, fd, events, user_data):            /* user_data = u */
  ev = 0
  if events & ODIN_EVENT_READ:  ev |= ODIN_UDP_READ
  if events & ODIN_EVENT_WRITE: ev |= ODIN_UDP_WRITE
  if events & ODIN_EVENT_ERROR: ev |= ODIN_UDP_ERROR
  u.on_ready(u, ev, u.user_data)                   /* final action; caller may close(u) here */

odin_udp_close(u):
  if u == NULL: return
  if u.io != NULL: odin_event_io_stop(u.io); u.io = NULL
  if u.fd >= 0: close(u.fd); u.fd = -1
  free(u)
```

Satisfies: G1 via the `len > 0` receive precondition and `recvfrom` classification that returns `ODIN_UDP_OK` with the datagram bytes and `src` (including `n == 0` only for a real empty datagram) and `ODIN_UDP_AGAIN` on a drained socket; G2 via the `sendto` classification with `*out_n == len` on success and the distinct `ODIN_UDP_AGAIN` / `ODIN_UDP_IO_ERROR` arms; G3 via `set_interest`'s invalid-mask guard, single-watch `odin_event_io_start` / `_update` / `_stop` reconcile, failure-state preservation, and `on_io`'s `ODIN_EVENT_* -> ODIN_UDP_*` mask translation into `on_ready`; G4 via `open`'s `AF_INET` / `AF_INET6` guard, `socket(..., SOCK_DGRAM, IPPROTO_UDP)` / `set_nonblocking` / `bind` sequence with `saved = errno; close; free; return -1` rollback, and `odin_udp_close`'s stop-watch / close-owned-fd / free; G5 via the single-watch, no-thread mechanism that reaches the loop only through the public `odin_event_io_*` API.

## 4. Security

The endpoint creates an unconnected `SOCK_DGRAM` socket and `recvfrom`s datagrams from arbitrary peers, so each datagram's bytes and the kernel-reported source address are attacker-controlled input crossing into the caller's buffers.

- **S1.**
  - **Threat:** An arbitrary peer sends an oversized datagram to the bound socket (`recvfrom` accepts from any source). A receiver that ignored the caller's buffer length would over-read past `buf`, and one that silently buffered the overflow would mis-frame a future QUIC packet — the oversized-datagram path the endpoint must bound.
  - **Mitigation:** `odin_udp_recv` passes the caller-supplied `len` and `*srclen` straight to `recvfrom`, which copies at most `len` payload bytes into `buf` and at most `*srclen` bytes into `src`; a datagram longer than `len` is truncated to `len` and the kernel discards the remainder rather than holding it for the next call. The endpoint interprets none of the bytes, sizes nothing from received data, and allocates only its fixed-size struct. Pinned in §3.2.2's `recvfrom` classification (the bounded `n >= 0 → *out_n = n` copy) and the §3.2.1 Receive contract.
  - **Enforcement:** §5 row T10 fires the trigger — a peer sends a 64-byte datagram into a 16-byte recv buffer — and asserts `odin_udp_recv` returns `ODIN_UDP_OK` with `*out_n == 16` and the first 16 bytes intact, then that the next `odin_udp_recv` returns `ODIN_UDP_AGAIN`, proving the copy is bounded to `len` and the discarded remainder is not buffered.

Beyond that bounded copy the endpoint adds no trust boundary: the source address it reports is the kernel-filled origin, passed through unmodified and untrusted, and peer-address trust, rate limiting, and packet validation remain the UDP consumer's responsibility, exactly as RFC-012's dial leaves address-selection trust to whoever resolved the address.

## 5. Testing Strategy

**Shared fixture (fork deadline + watchdog).** Every row executes inside the `fork` / `waitpid` deadline fixture RFC-010 §6, RFC-011 §6, and RFC-012 §6 established: the child runs the row body and all `ASSERT_*` / `EXPECT_*`, then `_exit(::testing::Test::HasFailure() ? 1 : 0)`, and the parent fails the row unless the child exits `0` within a 2 s deadline. The readiness-driven rows (T1, T3, T8, T9, T10, T13, T15) call `odin_event_loop_run` and arm a one-shot watchdog `odin_event_timer` (100 ms) that sets `state.timed_out` and calls `odin_event_loop_stop`, so an endpoint whose `on_ready` never fires cannot hang the child; `on_ready` records the delivered `events` and `calls` (and, for the receiving rows T1/T3/T10/T15, the `odin_udp_recv` result) before calling `odin_event_loop_stop`, except T8 where `on_ready` closes `u` before stopping. The synchronous rows (T2, T4, T5, T6, T7, T11, T12, T14) need no loop turn - they call `odin_udp_recv` / `odin_udp_send` / `odin_udp_open` / `odin_udp_set_interest` directly and assert the return - but still run under the same forked-child deadline. The IPv4 datagram-injecting readiness rows (T1, T3, T8, T10) inject from a plain `socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)` *peer* (not an `odin_udp_t`) bound to `127.0.0.1:0`, so the endpoint under test observes a real on-wire source address; T15 does the same with a plain `AF_INET6` UDP peer bound to `::1:0`, skipping only when the host cannot create and bind an IPv6 loopback UDP socket. T9 injects no datagram because a UDP socket is immediately writable; T13 injects readiness with RFC-010's queued synthetic backend hook. All executable rows run on the macOS `kqueue` backend per the RFC-010 §6 macOS-only constraint, with the Linux `epoll` build verified by cross-compilation; the endpoint uses only `socket` / `fcntl` / `bind` / `recvfrom` / `sendto` / `getsockname` / `close` plus the public `odin_event_io_*` API, so it is backend-agnostic.

**Test-only hooks.** `udp_unittests.cpp` compiles with `ODIN_UDP_TESTING` and includes `odin/udp_internal_test.h`, which declares three test-only helpers. `int odin_udp_test_fd(odin_udp_t *u)` returns the socket the endpoint owns, or `-1` with `errno == ENOENT` when `u` has no live fd; T1/T3/T8/T10/T15 call it immediately after `odin_udp_open`, then call `getsockname(fd, ...)` to obtain the executable endpoint port `EP` used by the peer `sendto`. `int odin_udp_test_io(odin_udp_t *u, odin_event_io_t **out)` snapshots the active watch handle, returning `-1` with `errno == ENOENT` when no watch is active; T9, T12, T13, and T14 use it to prove start/update/stop state and to feed RFC-010's `odin_event_loop_test_queue_backend_events`, with T12/T14 also comparing the post-invalid or post-failure handle to the original handle when preservation is the claim. `int odin_udp_test_fail_next_sendto(odin_udp_t *u, int errnum)` stores one test-only `sendto` failure (`EAGAIN`, `EWOULDBLOCK`, or `EINTR` only; otherwise `-1` / `EINVAL`) consumed by the next `odin_udp_send`, giving T11 a deterministic send-`AGAIN` path without relying on real UDP buffer exhaustion. Production builds define none of these symbols.

**Backend-behavior basis.** The per-row triggers rest on documented POSIX `socket(2)` / `bind(2)` / `recvfrom(2)` / `sendto(2)` semantics plus RFC-010's existing test hooks (`odin/event_loop_internal_test.h:54-82`): `odin_udp_open` rejects every family except `AF_INET` and `AF_INET6` with `EAFNOSUPPORT` before calling `socket`; `socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)` and `socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP)` create IP UDP sockets; `bind` to a non-local address fails `EADDRNOTAVAIL`; `recvfrom` on a drained nonblocking socket fails `EAGAIN` / `EWOULDBLOCK`; `recvfrom` of a zero-length datagram with `len > 0` returns `0`; a `recvfrom` whose datagram exceeds the supplied positive buffer length copies the first `len` bytes and discards the remainder (`SOCK_DGRAM` truncation, no `MSG_TRUNC` requested), so the next `recvfrom` reports `EAGAIN`; a nonblocking UDP socket is immediately writable, so a `WRITE` interest yields an `on_ready` `WRITE` readiness with no peer traffic; `sendto` of a datagram larger than the IPv4/UDP maximum (65507 bytes) fails `EMSGSIZE`; `odin_event_loop_test_liveness` snapshots the live `io_handles` count, letting T6/T7 assert a failed `odin_udp_open` did not leave an event-loop I/O handle alive; `odin_event_loop_test_kqueue_registered_mask` returns `0` and writes the active kqueue registration mask to `*out_events`, using mask `0` when no registration exists for that fd; `odin_event_loop_test_fail_next_kqueue_change` injects one matching start/update backend failure; and `odin_event_loop_test_queue_backend_events` injects an `ODIN_EVENT_ERROR` readiness through the real run path. T15 first probes IPv6 loopback by creating and binding an `AF_INET6` UDP peer to `::1:0`; it skips only if that probe fails. P1's red run is where these are first executed on the macOS `kqueue` target, so any platform-specific divergence in the exact `errno` (e.g. for the oversized-datagram, non-local-bind, or IPv6-loopback cases) surfaces there before P2 relies on it.

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | Receive delivers a datagram's payload and source address | `odin_udp_open` an endpoint bound to `127.0.0.1:0` (`AF_INET` / UDP); `fd0 = odin_udp_test_fd(u)`; `getsockname(fd0, &EP, ...)`; create a plain UDP *peer* bound to `127.0.0.1:0`, read its port `PP`; `odin_udp_set_interest(u, ODIN_UDP_READ)`; peer `sendto(EP, "hi", 2)`; arm 100 ms watchdog; `run` - `on_ready` records `events`/`calls`, calls `odin_udp_recv(u, buf, sizeof buf, &n, &src, &srclen)` recording its result, then `stop`; `odin_udp_close(u)` | `open`, `getsockname`, and `run` return `0`; `fd0 >= 0`; `on_ready.calls == 1` with `events & ODIN_UDP_READ`; `odin_udp_recv` returns `ODIN_UDP_OK`, `n == 2`, `buf` holds `"hi"`, and `src` is `AF_INET` with `sin_port == PP` and `sin_addr` `127.0.0.1` (per-datagram source captured); `timed_out == false` | G1, G3 | unit |
| T2 | Receive on a drained socket reports AGAIN, not error | `odin_udp_open` an endpoint bound to `127.0.0.1:0`; with no datagram queued, call `odin_udp_recv(u, buf, sizeof buf, &n, &src, &srclen)` directly (no loop turn); `odin_udp_close(u)` | `odin_udp_recv` returns `ODIN_UDP_AGAIN` (the empty-socket `EAGAIN` / `EWOULDBLOCK` classification); no `on_ready` is required | G1 | unit |
| T3 | Zero-length datagram is OK, not EOF | As T1, but the peer `sendto(EP, "", 0)` sends a zero-length datagram and `on_ready` receives into the same positive-capacity `buf`; `run`; `on_ready` -> `odin_udp_recv`; `odin_udp_close(u)` | `odin_udp_recv` returns `ODIN_UDP_OK` with `n == 0` (a received empty datagram - `recvfrom`'s `0` is **not** zero-capacity truncation and **not** end-of-stream, the UDP-vs-stream distinction), and `src` is the peer address; `timed_out == false` | G1, G3 | unit |
| T4 | Send transmits one datagram to the destination | `odin_udp_open` an endpoint bound to `127.0.0.1:0`; create a plain UDP *peer* bound to `127.0.0.1:0` with address `DST`; `ASSERT_EQ(odin_udp_send(u, "yo", 2, &n, &DST, sizeof DST), ODIN_UDP_OK)`; then `recvfrom(peer, ...)`; `odin_udp_close(u)` | `odin_udp_send` returns `ODIN_UDP_OK` with `n == 2`; `recvfrom(peer)` yields the 2 bytes `"yo"` - the datagram reached `DST` | G2 | unit |
| T5 | Oversized datagram send fails with IO_ERROR | `odin_udp_open` an endpoint bound to `127.0.0.1:0`; build a 70000-byte buffer (> the 65507 IPv4/UDP datagram maximum); `DST = 127.0.0.1:9`; `odin_udp_send(u, big, 70000, &n, &DST, sizeof DST)`; `odin_udp_close(u)` | `odin_udp_send` returns `ODIN_UDP_IO_ERROR` with `errno == EMSGSIZE` (the genuine-error arm, distinct from `ODIN_UDP_AGAIN`) | G2 | unit |
| T6 | open rejects non-IP datagram families: `*out` untouched, nothing leaked | A `struct sockaddr_un` with `sun_family = AF_UNIX`; preset sentinel `odin_udp_t *u = (odin_udp_t *)-1`; snapshot `odin_event_loop_test_liveness(&live_before)`; call `odin_udp_open(loop, (struct sockaddr *)&bad, sizeof bad, on_ready, &st, &u)`; snapshot `odin_event_loop_test_liveness(&live_after)` | `odin_udp_open` returns `-1` with `errno == EAFNOSUPPORT` and leaves `u == (odin_udp_t *)-1` (wrote nothing to `*out`); `live_after.io_handles == live_before.io_handles`, proving the failed open did not create or leave a loop I/O handle; the family guard rejects `AF_UNIX` before `socket(2)`, so a host-supported datagram-capable non-IP family is still outside the UDP endpoint contract, nothing is leaked, and nothing is registered on the loop; the test closes nothing | G4 | unit |
| T7 | open rollback after socket creation (bind fails): socket closed, no fd leak and no stale loop watch | Record the lowest free fd by opening then closing a probe `socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)` -> `P`; build an `AF_INET` `sockaddr` for non-local `192.0.2.1:0` (RFC 5737 TEST-NET-1; `bind` -> `EADDRNOTAVAIL`); preset sentinel `u`; snapshot `odin_event_loop_test_liveness(&live_before)`; `odin_udp_open(...)`; snapshot `odin_event_loop_test_liveness(&live_after)`; then open another probe socket -> `P2` | `odin_udp_open` returns `-1` with `errno == EADDRNOTAVAIL` and leaves `u` at its sentinel; `P2 == P` - the failed open closed the socket it had created before `bind` failed (a leaked fd would occupy `P`, forcing `P2 > P`), exercising the post-`socket` `close` + `free` rollback; `live_after.io_handles == live_before.io_handles`, proving the failed open also left no `odin_event_io_t` alive after the rollback | G4 | unit |
| T8 | close from within on_ready closes the owned socket and removes the active watch | `odin_udp_open` an endpoint bound to `127.0.0.1:0`; `fd0 = odin_udp_test_fd(u)`; `getsockname(fd0, &EP, ...)`; `odin_udp_set_interest(u, ODIN_UDP_READ)`; `odin_udp_test_io(u, &io_before)` succeeds; `odin_event_loop_test_kqueue_registered_mask(loop, fd0, &mask_before)` returns `0` with `mask_before == ODIN_EVENT_READ`; peer `sendto(EP, "x", 1)`; arm 100 ms watchdog; `run` - `on_ready` calls `odin_udp_recv` once, then `odin_udp_close(u)`, records `state.closed = true`, then `odin_event_loop_stop`; if the callback did not run, cleanup closes `u` after `run` | `open`, `getsockname`, pre-run watch snapshot, and `run` return `0`; `fd0 >= 0`; `on_ready.calls == 1`; after `run`, `fcntl(fd0, F_GETFD) == -1` with `errno == EBADF` (`close` closed the owned socket), and `odin_event_loop_test_kqueue_registered_mask(loop, fd0, &mask_after)` returns `0` with `mask_after == 0`, proving `odin_udp_close` stopped the backend watch rather than only closing the fd; under the P2 AddressSanitizer command no use-after-free is reported (the watch is stopped and `on_ready` is `on_io`'s final action, so no later readiness re-enters the freed endpoint); `timed_out == false` | G4, G3 | unit |
| T9 | WRITE interest delivers writable readiness and stop removes the watch | `odin_udp_open` an endpoint bound to `127.0.0.1:0`; `fd0 = odin_udp_test_fd(u)`; `odin_udp_set_interest(u, ODIN_UDP_READ)`; `odin_udp_test_io(u, &io_read)` succeeds; `odin_udp_set_interest(u, ODIN_UDP_WRITE)` (update; no datagram sent); arm 100 ms watchdog; `run` - `on_ready` records `events`/`calls`, calls `odin_udp_set_interest(u, 0)` (stop), then `stop`; after `run`, call `odin_udp_test_io(u, &io_after)` and `odin_event_loop_test_kqueue_registered_mask(loop, fd0, &mask)`; `odin_udp_close(u)` | every valid `odin_udp_set_interest` returns `0`; `on_ready.calls == 1` with `events & ODIN_UDP_WRITE` set and `events & ODIN_UDP_READ` clear (a UDP socket is immediately writable and nothing was sent); after the callback's `set_interest(0)`, `odin_udp_test_io` returns `-1` / `ENOENT` and the registered mask is `0`, proving stop removed the watch rather than only updating local state; `timed_out == false` | G3 | unit |
| T10 | Oversized datagram is truncated to the recv buffer; remainder discarded | `odin_udp_open` an endpoint bound to `127.0.0.1:0`; `fd0 = odin_udp_test_fd(u)`; `getsockname(fd0, &EP, ...)`; a plain UDP *peer* bound to `127.0.0.1:0` `sendto`s a 64-byte datagram of distinct bytes to `EP`; `odin_udp_set_interest(u, ODIN_UDP_READ)`; arm 100 ms watchdog; `run` - `on_ready` calls `odin_udp_recv(u, buf16, 16, &n, &src, &srclen)` (16-byte buffer) recording the result, then a second `odin_udp_recv(u, buf16b, 16, &n2, ...)`, then `stop`; `odin_udp_close(u)` | `fd0 >= 0` and `getsockname` returns `0`; the first `odin_udp_recv` returns `ODIN_UDP_OK` with `n == 16` and `buf16` equal to the first 16 bytes of the 64-byte payload (datagram truncated to the caller's `len`); the second `odin_udp_recv` returns `ODIN_UDP_AGAIN` (the discarded remainder was **not** buffered for the next read); `timed_out == false` | G1, S1 | unit |
| T11 | Send buffer pressure reports AGAIN, not IO_ERROR | `odin_udp_open` an endpoint bound to `127.0.0.1:0`; `DST = 127.0.0.1:9`; call `odin_udp_test_fail_next_sendto(u, EAGAIN)`; preset `n = 123`; call `odin_udp_send(u, "retry", 5, &n, &DST, sizeof DST)`; `odin_udp_close(u)` | the test fault hook returns `0`; `odin_udp_send` returns `ODIN_UDP_AGAIN` with `errno == EAGAIN` (the retryable send-buffer-full arm, distinct from `ODIN_UDP_IO_ERROR`) | G2 | unit |
| T12 | Invalid interest masks are rejected and preserve the active watch | `odin_udp_open` an endpoint bound to `127.0.0.1:0`; `fd0 = odin_udp_test_fd(u)`; `ASSERT_EQ(odin_udp_set_interest(u, ODIN_UDP_READ), 0)`; `ASSERT_EQ(odin_udp_test_io(u, &io0), 0)`; `ASSERT_EQ(odin_event_loop_test_kqueue_registered_mask(loop, fd0, &mask), 0)` and `ASSERT_EQ(mask, ODIN_EVENT_READ)`; call `odin_udp_set_interest` with `ODIN_UDP_ERROR`, `ODIN_UDP_READ | ODIN_UDP_ERROR`, and `ODIN_UDP_WRITE | 0x80u`; after each invalid call, re-check `odin_udp_test_io(u, &io_after)`, compare `io_after == io0`, and re-check `registered_mask` returns `0` with `mask == ODIN_EVENT_READ`; then call `odin_udp_set_interest(u, ODIN_UDP_WRITE)` and re-check `registered_mask` returns `0` with `mask == ODIN_EVENT_WRITE`; `odin_udp_close(u)` | each invalid call returns `-1` with `errno == EINVAL`; the same watch handle remains active and the backend mask remains exactly `ODIN_EVENT_READ` after every invalid input; the later valid `ODIN_UDP_WRITE` update changes the backend mask to `ODIN_EVENT_WRITE`, proving the invalid-input path preserved both the active watch and the cached interest rather than corrupting `cur` into a no-op | G3 | unit |
| T13 | ERROR readiness is delivered as `ODIN_UDP_ERROR` | `odin_udp_open` an endpoint bound to `127.0.0.1:0`; `odin_udp_set_interest(u, ODIN_UDP_READ)`; `odin_udp_test_io(u, &io)` succeeds; queue `{io, ODIN_EVENT_ERROR}` with `odin_event_loop_test_queue_backend_events(loop, entries, 1)`; arm 100 ms watchdog; `run` - `on_ready` records `events`/`calls`, then `stop`; `odin_udp_close(u)` | `on_ready.calls == 1`; `events & ODIN_UDP_ERROR` is set, and no datagram receive is required; `timed_out == false` | G3 | unit |
| T14 | `set_interest` start/update failures propagate and preserve watch state | macOS-only row using RFC-010 kqueue hooks; `odin_udp_open` an endpoint bound to `127.0.0.1:0`; `fd0 = odin_udp_test_fd(u)`; inject `odin_event_loop_test_fail_next_kqueue_change(loop, ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_ADD, ODIN_EVENT_WRITE, ENOSPC)`; call `odin_udp_set_interest(u, ODIN_UDP_READ | ODIN_UDP_WRITE)`; inspect `odin_udp_test_io` and registered mask; then call `odin_udp_set_interest(u, ODIN_UDP_READ)` successfully and snapshot `io0`; inject `odin_event_loop_test_fail_next_kqueue_change(loop, ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_DELETE, ODIN_EVENT_READ, ENOSPC)`; call `odin_udp_set_interest(u, ODIN_UDP_WRITE)`; inspect `odin_udp_test_io(u, &io_after)` and registered mask; because the matching kqueue failure hook is one-shot and was consumed, call `odin_udp_set_interest(u, ODIN_UDP_WRITE)` again and inspect the registered mask; `odin_udp_close(u)` | the failed start returns `-1` / `ENOSPC`, leaves no active watch, and registered mask `0`; the valid read start registers `ODIN_EVENT_READ`; the failed update returns `-1` / `ENOSPC`, leaves the same active watch in place (`io_after == io0`), and the registered mask remains exactly `ODIN_EVENT_READ`; the later retry without fault injection returns `0` and changes the registered mask to `ODIN_EVENT_WRITE`, proving UDP preserved its cached mask across the failed update instead of corrupting it into a no-op | G3 | unit |
| T15 | IPv6 open uses the caller's family and receives an IPv6 datagram | Probe IPv6 loopback by creating a plain `socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP)` peer and binding it to `::1:0`; if the probe fails, `GTEST_SKIP()` with the failing `errno`; otherwise `odin_udp_open` an endpoint bound to `::1:0` (`AF_INET6`); `fd0 = odin_udp_test_fd(u)`; `getsockname(fd0, &EP6, ...)`; assert `EP6.ss_family == AF_INET6`; `odin_udp_set_interest(u, ODIN_UDP_READ)`; peer `sendto(EP6, "v6", 2)`; arm 100 ms watchdog; `run` - `on_ready` records `events`/`calls`, calls `odin_udp_recv(u, buf, sizeof buf, &n, &src, &srclen)` recording its result, then `stop`; `odin_udp_close(u)` | `open`, `getsockname`, `set_interest`, and `run` return `0`; `fd0 >= 0`; the endpoint socket's `getsockname` family is `AF_INET6` (proving `open` did not hardcode IPv4); `on_ready.calls == 1` with `events & ODIN_UDP_READ`; `odin_udp_recv` returns `ODIN_UDP_OK`, `n == 2`, `buf` holds `"v6"`, and `src` is `AF_INET6` with the peer's loopback address and port; `timed_out == false` | G1, G3, G4 | unit |

## 6. Implementation Plan

- **P1. Land the UDP endpoint surface and red-verifiable `T1`-`T15` in the test binary only.**
  - **Scope:** add `odin/udp.h` exactly as §3.2.1 specifies - the `odin_udp_t` opaque type, the `odin_udp_io_t` enum in the order `OK`, `AGAIN`, `IO_ERROR`, the `ODIN_UDP_READ` / `_WRITE` / `_ERROR` flags, the `odin_udp_ready_cb` typedef, and the `odin_udp_open` / `_recv` / `_send` / `_set_interest` / `_close` declarations inside one `extern "C"` block, with the owns-the-IP-UDP-socket, `AF_INET` / `AF_INET6` family guard, positive receive-capacity, zero-length-vs-zero-capacity-vs-EOF, atomic-send, `AGAIN`-vs-`IO_ERROR`, valid/invalid interest-mask, level-triggered-readiness, local-setup-failure-rollback, and `close`-in-callback clauses pinned in the header doc-comment; add `odin/udp_internal_test.h` whose `odin_udp_test_fd`, `odin_udp_test_io`, and `odin_udp_test_fail_next_sendto` declarations are visible only under `ODIN_UDP_TESTING`; add `odin/udp.c` with a **stub** whose `odin_udp_open` `calloc`s the `u` object, stores `loop` / `on_ready` / `user_data`, initializes `io = NULL` and `cur = 0`, and, for supported `AF_INET` / `AF_INET6` addresses whose local bind succeeds, creates a real nonblocking `socket(addr->sa_family, SOCK_DGRAM, IPPROTO_UDP)`, binds it, stores the fd, writes `*out`, and returns `0`. The same stub intentionally remains wrong for local setup failures: unsupported families and bind failures close any fd they created, still write `*out`, and return `0`; the bind-failure path also starts a test-only `ODIN_EVENT_READ` I/O watch on the created fd before closing it and leaves that watch alive, so T7's loop-liveness assertion is red in addition to the failed return/out assertions. The stub's `odin_udp_recv` and `odin_udp_send` return `ODIN_UDP_IO_ERROR` with `errno = ENOSYS`, `odin_udp_set_interest` returns `0` for every mask and registers no watch, `odin_udp_close` closes `fd >= 0` and frees the object, `odin_udp_test_fd` returns the bound fd or `-1` / `ENOENT`, `odin_udp_test_io` returns `-1` / `ENOENT`, and `odin_udp_test_fail_next_sendto` accepts the retryable errno values but is ignored by the send stub. Add `odin/udp_testing.c` containing `#include "udp.c" // NOLINT(bugprone-suspicious-include)` so the test binary compiles `udp.c` under `ODIN_UDP_TESTING`; add `odin/udp_unittests.cpp` containing `T1`-`T15` from §5 as separate `TEST(OdinUdpTest, T#)` cases plus the replicated `fork` + deadline fixture and per-row watchdog timer, each row guarded by a leading `if (!getenv("ODIN_UDP_RED")) GTEST_SKIP() << "pending RFC-015 P2";`. In `odin/BUILD.gn`, add a test-only `config("odin_udp_testing_config") { defines = [ "ODIN_UDP_TESTING" ] }`, append `"udp.h"`, `"udp_internal_test.h"`, `"udp_testing.c"`, and `"udp_unittests.cpp"` to the `executable("odin_unittests")` `sources` array, and add that config to `odin_unittests`'s `configs` - no production target and no `:odin` dependency this phase, so the endpoint is unavailable to production callers while `T1`-`T15` are intentionally red. The stub compiles into `odin_unittests` against the existing `:odin_event_loop_testing` dependency, which supplies the `odin_event_io_*` / `odin_event_timer_*` symbols, the kqueue test hooks, `odin_event_loop_test_liveness`, and `odin/event_loop.h`.
  - **Depends on:** None.
  - **Done when:** `./tool/gn gen out/udp_mac --args='target_os="mac"'` and `./tool/gn gen out/udp_linux_x64 --args='target_os="linux" target_cpu="x64"'` resolve, and `./tool/ninja -C out/udp_mac odin_main odin_unittests tests` and `./tool/ninja -C out/udp_linux_x64 odin_main odin_unittests tests` build without error; the red-verification command `ODIN_UDP_RED=1 out/udp_mac/odin_unittests --gtest_filter='OdinUdpTest.*'` reports `T1`-`T15` failing against the stub through the forked child's nonzero `_exit` and the parent's `WIFEXITED && WEXITSTATUS == 0` check, except T15 may `SKIP` only if its IPv6 loopback probe fails. T1/T3/T8/T10 can obtain `EP`, and T15 can obtain `EP6` when the IPv6 loopback probe succeeds, because the stub owns a real bound fd, but they fail because the stub registers no watch and `on_ready` never fires; T9/T12/T13 fail at the active-watch assertions or missing callback because `odin_udp_test_io` reports `ENOENT`; T14 fails because the stub returns success instead of propagating the injected kqueue failure; T2 fails because recv returns `IO_ERROR` instead of `AGAIN`; T4 fails because send returns `IO_ERROR` instead of `OK`; T5 fails because `errno` is `ENOSYS` not `EMSGSIZE`; T6/T7 fail because open returns `0` instead of `-1`, and T7 additionally fails the before/after `odin_event_loop_test_liveness(...).io_handles` equality assertion because the bind-failure stub leaves its intentionally created I/O watch alive; and T11 fails because send returns `IO_ERROR` instead of `AGAIN`. The default `out/udp_mac/odin_unittests --gtest_brief=1` reports `T1`-`T15` `SKIPPED` and exits zero with every pre-existing Odin suite green; the `out/udp_linux_x64/odin_unittests` binary is cross-compiled but not run; the public `:odin` label still does not link `udp.c`, non-test translation units see no UDP declarations, and production objects expose no `odin_udp_test_*` symbol.
- **P2. Implement the endpoint and turn `T1`-`T15` green.**
  - **Scope:** replace the `odin/udp.c` stub with the full implementation from §3.2.2 - `odin_udp_open`'s `AF_INET` / `AF_INET6` family guard, `socket(addr->sa_family, SOCK_DGRAM, IPPROTO_UDP)` / `set_nonblocking` / `bind` sequence with the `saved = errno; close; free; return -1` rollback; `odin_udp_recv` / `odin_udp_send` with the `n >= 0 -> OK` (including `recvfrom`'s `0` only for valid `len > 0` receives) and `EAGAIN`/`EWOULDBLOCK`/`EINTR -> AGAIN` else `IO_ERROR` classification; `odin_udp_set_interest`'s invalid-mask guard, single-watch `odin_event_io_start` / `_update` / `_stop` reconcile, and start/update failure preservation; `on_io`'s `ODIN_EVENT_* -> ODIN_UDP_*` translation invoking `on_ready` as its final action; and `odin_udp_close`'s stop-watch / close-owned-fd / free. Under `ODIN_UDP_TESTING`, implement `odin_udp_test_fd`, `odin_udp_test_io`, and `odin_udp_test_fail_next_sendto`; the send path checks the one-shot injected retryable errno before calling real `sendto`, and production builds omit all test-only symbols. In `odin/BUILD.gn`, add `source_set("odin_udp") { sources = [ "udp.c", "udp.h" ]; public_deps = [ ":odin_event_loop" ] }` and append `":odin_udp"` to `source_set("odin")`'s `deps`, so production callers can link the endpoint; `odin_unittests` keeps compiling `udp.c` through `udp_testing.c` against `:odin_event_loop_testing` (no `:odin_udp` dependency, so `event_loop.c` is never linked twice) with `:odin_udp_testing_config` still applied. Remove the `ODIN_UDP_RED` skip guard from `T1`-`T15`. No `odin/main.c` or `odin/cli.c` change - this RFC ships the importable surface only, matching the RFC-012 / RFC-013 pattern that a follow-up RFC wires the consumer.
  - **Depends on:** P1.
  - **Done when:** the P1 build commands still succeed; `out/udp_mac/odin_unittests --gtest_filter='OdinUdpTest.*'` passes `T1`-`T15` un-gated, except T15 may `SKIP` only when its IPv6 loopback probe fails, including T1's payload-and-source delivery, T2's drained-socket `AGAIN`, T3's zero-length `OK`, T4's send round-trip, T5's `EMSGSIZE` `IO_ERROR`, T6's `EAFNOSUPPORT` non-IP-family setup failure with `*out` untouched and event-loop `io_handles` unchanged, T7's `EADDRNOTAVAIL` post-`socket` rollback with `P2 == P` (no fd leak) and event-loop `io_handles` unchanged (no stale watch), T8's `close`-in-callback with `fd0 -> EBADF` and backend registered mask `0`, T9's WRITE-readiness delivery plus observed stop removal, T10's oversized-datagram truncation to 16 bytes with the discarded remainder yielding `AGAIN` on the next recv, T11's injected send `AGAIN`, T12's invalid-mask `EINVAL` with the same watch handle preserved and a later valid update changing the backend mask to `ODIN_EVENT_WRITE`, T13's `ODIN_EVENT_ERROR -> ODIN_UDP_ERROR` translation, T14's kqueue start/update failure propagation with same-handle preservation plus a post-failure retry changing the backend mask to `ODIN_EVENT_WRITE`, and T15's `AF_INET6` `getsockname` family plus IPv6 datagram receive when loopback IPv6 is available; the unfiltered `out/udp_mac/odin_unittests --gtest_brief=1` runs `T1`-`T15` and exits zero with every pre-existing Odin suite green; `./tool/gn gen out/udp_mac_asan --args='target_os="mac" is_asan=true'`, `./tool/ninja -C out/udp_mac_asan odin_unittests`, and `out/udp_mac_asan/odin_unittests --gtest_filter='OdinUdpTest.*'` exit with no AddressSanitizer report, giving T8 its no-use-after-free coverage; `out/udp_linux_x64/odin_unittests` still cross-compiles with the endpoint linked; production `out/udp_mac/odin` links the endpoint symbols through `:odin` and contains no test-only symbol; `./tidy_odin.sh` exits clean over the odin tree (now including `udp.c`, `udp_testing.c`, and `udp_unittests.cpp`, with `udp.h` and `udp_internal_test.h` checked via the header filter).
