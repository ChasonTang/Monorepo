# RFC-012: Odin Nonblocking Socket Dial

## 1. Summary

Add `odin/dial.{c,h}`, a single-thread, event-loop-driven nonblocking socket dialer: given a live `odin_event_loop_t` and an already-resolved `struct sockaddr`, `odin_dial_start` creates one nonblocking `SOCK_STREAM` socket, issues `connect(2)`, and resolves the attempt entirely from the loop — watching the socket for writability through the RFC-010 `odin_event_io_*` API and reading `getsockopt(SO_ERROR)` to classify the outcome, or, when `connect(2)` fails immediately, carrying that error to the next loop turn through a 0-delay one-shot `odin_event_timer` — then invokes a caller completion callback exactly once with `ODIN_DIAL_OK` and the connected fd (ownership transferred to the caller) or `ODIN_DIAL_ERROR` with the failing `errno` and `fd == -1` (the socket already closed), performing no name resolution and no transport selection and depending on no Odin module other than `odin/event_loop.h`.

## 2. Goals

- **G1.** Given a live loop and an already-resolved socket address, establish a connection without blocking the caller and deliver the connected file descriptor to the caller through the completion callback exactly once as `ODIN_DIAL_OK` (with `err == 0`); the delivered fd is a connected stream socket the caller thereafter owns.
- **G2.** Report a dial that does not connect without leaking the descriptor the module created: a connection that is refused, unreachable, or otherwise fails — whether the kernel signals it immediately or only after the attempt is in flight — is delivered exactly once as `ODIN_DIAL_ERROR` with the failing `errno` and `fd == -1`, the module's socket already closed; and a *local setup* failure before the attempt is handed to the loop instead makes `odin_dial_start` return `-1` with `errno` set, write nothing to `*out`, and leave no socket open and no loop registration.
- **G3.** Drive the whole connect lifecycle using only the event-loop module — `odin_event_io_*` for writability and `odin_event_timer_*` for deferred delivery — starting no thread, performing no name resolution, and selecting no transport; the module's only Odin dependency is `odin/event_loop.h`. non-testable: the single-dependency / no-resolution / no-thread property is a build-graph, include-set, and API-shape constraint verified by the GN deps and the public header, not expressible as an executable §6 row.
- **G4.** Give the caller lifetime control through `odin_dial_destroy`: it aborts an in-flight dial — closing the still-owned socket and never invoking the callback — and reclaims a completed one without closing the descriptor already handed to the caller, and it is callable from within the completion callback.

## 3. Design

### 3.1 Overview

`odin/dial` is a new leaf module in the Odin library and the second in-tree consumer (after the RFC-011 relay) of the RFC-010 event loop, using both its I/O-watch API (`odin_event_io_start` / `_stop`) and its timer API (`odin_event_timer_start` / `_stop`). It depends on no other Odin module: it receives an address that is already resolved, so it runs no name resolution, and it never selects a transport.

The caller holds a live `odin_event_loop_t` and a resolved `struct sockaddr` (for the proxy, the upstream address the Server produced after DNS) and calls `odin_dial_start(loop, addr, addrlen, on_done, user_data, &dial)`. The dial creates exactly one nonblocking `SOCK_STREAM` socket whose family comes from `addr->sa_family`, issues `connect(2)` once, and from then on runs only from the loop's callbacks on the loop's owner thread. A `connect(2)` that is in flight (`EINPROGRESS` / `EINTR`) or already complete (`0`) is resolved by a one-shot WRITE watch: when the loop reports the socket writable, the dial reads `getsockopt(SO_ERROR)`, which is `0` on success or the connection's failing errno. A `connect(2)` that fails immediately is instead carried to the next loop turn by a 0-delay one-shot timer that delivers the captured errno. Either way the dial fires `on_done` exactly once as its final action: on success it hands the connected fd to the caller and the caller's ownership of that fd begins; on failure it closes the socket and reports the errno with `fd == -1`. The caller owns the `dial` object and frees it with `odin_dial_destroy`, which is callable from within `on_done` and which, on an in-flight dial, aborts the attempt and closes the still-owned socket. The dial starts no thread, opens no descriptor besides that one socket, and performs no I/O beyond that socket's `connect` / `getsockopt`.

```
  caller                        dial (odin_dial_t)                          event loop
   resolved sockaddr --odin_dial_start--> socket(sa_family, SOCK_STREAM) + O_NONBLOCK
                                          connect(addr)
                   0 / EINPROGRESS / EINTR --> odin_event_io WRITE watch --> getsockopt(SO_ERROR)
                   immediate -1            --> odin_event_timer (0 us, one-shot) --> captured errno
                                          |
                   on_done(dial, ODIN_DIAL_OK,    connected_fd, 0,   user_data)  // fd -> caller
                   on_done(dial, ODIN_DIAL_ERROR, -1,           err, user_data)  // socket closed

   odin_dial_destroy(dial): abort in-flight (close owned socket) or reclaim completed; never calls on_done
```

### 3.2 Detailed Design

#### 3.2.1 Public API, Ownership, and Lifecycle

```c
/* odin/dial.h */
#include <sys/socket.h>

#include "odin/event_loop.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct odin_dial_t odin_dial_t;

typedef enum odin_dial_status_t {
  ODIN_DIAL_OK = 0,
  ODIN_DIAL_ERROR,
} odin_dial_status_t;

typedef void (*odin_dial_cb)(odin_dial_t *dial, odin_dial_status_t status,
                             int fd, int err, void *user_data);

int odin_dial_start(odin_event_loop_t *loop, const struct sockaddr *addr,
                    socklen_t addrlen, odin_dial_cb on_done, void *user_data,
                    odin_dial_t **out);
void odin_dial_destroy(odin_dial_t *dial);

#ifdef __cplusplus
}
#endif
```

**Unstated contract.**

- **Ownership & preconditions.** `odin_dial_start` creates the socket and owns it for the duration of the attempt; the `dial` object is caller-owned. `addr` must be non-null and point to a valid resolved address whose family the host supports, `addrlen` must be the correct length for `addr->sa_family`, `loop` must be a live loop owned by the calling thread, and `on_done` must be non-null. `odin_dial_start` is an owner-thread API under the RFC-010 contract — callable before `run`, from a loop callback, or from a posted task. The socket is created with `socket(addr->sa_family, SOCK_STREAM, 0)` and made nonblocking; the dial performs no name resolution (the address is already resolved) and selects no transport.
- **Success.** On success `odin_dial_start` registers the in-flight attempt with the loop (a WRITE watch, or — for an immediate `connect(2)` error — a 0-delay one-shot timer), writes `*out`, and returns `0`. It never invokes `on_done` itself: even a `connect(2)` that completes synchronously (returns `0`) is reported on a later loop turn through the WRITE watch, so the callback never fires re-entrantly from within `start`.
- **Failure & rollback.** A *local setup* failure returns `-1` with `errno` set, writes nothing to `*out`, and leaves no socket open and no loop registration: `ENOMEM` when the `dial` object cannot be allocated, or the errno from `socket(2)` (e.g. `EAFNOSUPPORT` for an unsupported `sa_family`), from making the socket nonblocking, or from `odin_event_io_start` / `odin_event_timer_start`. Any socket created before a later setup step failed is closed before the return. A *connection* failure is never reported this way — it always flows through `on_done` (see Completion), so a caller handles connection outcomes in exactly one place regardless of whether the kernel refuses the connection synchronously or after the attempt is in flight.
- **Completion (`on_done`).** Fires exactly once on the owner thread, as the dial's **final action**. `ODIN_DIAL_OK` carries `err == 0` and `fd` set to the connected socket; the caller's ownership of `fd` begins at that moment and the dial will never close it. `ODIN_DIAL_ERROR` carries `fd == -1` and `err` set to the failing connection errno — the writable socket's `getsockopt(SO_ERROR)` value (e.g. `ECONNREFUSED`, `EHOSTUNREACH`, `ETIMEDOUT`) for an in-flight attempt, or the immediate `connect(2)` errno (e.g. `ENOENT` for an `AF_UNIX` path with no listener) for one that failed synchronously — and the dial has already closed the socket it created. `status` is the authoritative signal. The dial reads and writes no `dial` state after `on_done` returns, so `odin_dial_destroy(dial)` is legal from inside `on_done`.
- **Destroy.** `odin_dial_destroy` stops any still-active loop registration (the WRITE watch or the deferred-error timer — their handle memory is reclaimed by the loop's deferred-free per RFC-010), closes the socket only if the dial still owns it (an in-flight or never-completed attempt), frees the `dial` object, and never invokes `on_done`. After an `ODIN_DIAL_OK` completion the socket has already passed to the caller, so `destroy` does not close it; after an `ODIN_DIAL_ERROR` completion the socket is already closed. `odin_dial_destroy(NULL)` is a no-op, and the pointer is dead afterward. `destroy` is how a caller both reclaims a completed dial and aborts an in-flight one.
- **Threading.** All entry points and internal callbacks are owner-thread; the dial adds no locks.

Satisfies: G1 via the `odin_dial_cb` surface that delivers the connected `fd` with `ODIN_DIAL_OK` exactly once; G2 via the `-1`/`errno` local-setup-failure return that writes nothing to `*out`, and the `ODIN_DIAL_ERROR` / `fd == -1` completion whose socket is closed before the callback; G3 via the `struct sockaddr` input and the `event_loop.h`-only include set that fix the no-resolution, single-dependency surface; G4 via the caller-owned `dial` object, the owns-until-handoff fd rule, and the final-action callback that makes `destroy`-from-`on_done` safe.

#### 3.2.2 Connect Initiation and Readiness Resolution

```c
/* odin_dial_t internal state (Proposed). Exactly one of {io, timer} is        */
/* registered at a time; `fd` is the owned socket, set to -1 the instant        */
/* ownership leaves (handed to the caller on OK, or closed on ERROR / abort).   */
struct odin_dial_t {
  odin_dial_cb        on_done;
  void               *user_data;
  int                 fd;           /* owned socket; -1 once ownership leaves   */
  int                 pending_err;  /* errno to deliver on the deferred path    */
  odin_event_io_t    *io;           /* WRITE watch while connecting; else NULL  */
  odin_event_timer_t *timer;        /* deferred-error timer; else NULL          */
};
```

**Unstated contract.**

- **WRITE watch and `SO_ERROR`.** The watch is registered with a non-empty `ODIN_EVENT_WRITE` input mask (RFC-010 requires a non-empty READ/WRITE input mask; `ODIN_EVENT_ERROR` is output-only). When the loop reports the socket ready, the dial does not trust the mask bits — it calls `getsockopt(SO_ERROR)`, the authoritative connect result: `0` means connected, nonzero is the connection's failing errno (and a failing `getsockopt` itself yields its own errno, which is treated as a connection failure). This is why a WRITE-only watch suffices even though a failed connect also raises the output-only `ODIN_EVENT_ERROR` bit, and why no false "writable" wakeup is mishandled: a connecting socket only becomes writable once `connect(2)` has resolved.
- **`connect(2)` classification.** `0` (already connected) and `-1` with `errno` in `{EINPROGRESS, EINTR}` (attempt in flight) both take the WRITE-watch path — for a nonblocking socket, `EINTR` continues the attempt asynchronously exactly like `EINPROGRESS`, so it is not retried. Any other `-1` is an immediate failure whose `errno` is captured into `pending_err` and delivered on the next loop turn by a 0-delay one-shot timer. The timer (not `odin_event_post`) is used precisely because it can be individually canceled by `odin_dial_destroy`, so aborting before the deferred callback runs never leaves a dispatched task pointing at freed state.
- **Single completion.** Completion is funneled through one `complete(err)` step that stops whichever single registration is active, then either transfers the fd (on `err == 0`) or closes it (on `err != 0`), then calls `on_done` as its last statement — so no `dial` field is touched after the callback returns. A single watched fd plus a single active registration means `complete` runs at most once per dial, so no separate idempotency guard is needed; `odin_dial_destroy` stops the registration that `complete` would have consumed, so the two never race on one dial.
- **Close discipline.** The dial closes the socket on every failure path (immediate, in-flight, and abort) and on no success path; it never closes a descriptor it has handed to the caller.

**Mechanism.**

```
set_nonblocking(fd):                      /* returns 0, or -1 with errno set */
  flags = fcntl(fd, F_GETFL, 0)
  if flags == -1: return -1
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK)

so_error(fd):                             /* authoritative connect result */
  err = 0; len = sizeof err
  if getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0: return errno
  return err

odin_dial_start(loop, addr, addrlen, on_done, user_data, out):
  d = calloc(1, sizeof *d)
  if d == NULL: errno = ENOMEM; return -1
  d.on_done = on_done; d.user_data = user_data; d.pending_err = 0
  d.fd = socket(addr->sa_family, SOCK_STREAM, 0)
  if d.fd < 0:                   save = errno; free(d); errno = save; return -1
  if set_nonblocking(d.fd) != 0: save = errno; close(d.fd); free(d); errno = save; return -1
  r = connect(d.fd, addr, addrlen)
  if r == 0 or (r < 0 and errno in {EINPROGRESS, EINTR}):
    if odin_event_io_start(loop, d.fd, ODIN_EVENT_WRITE, on_writable, d, &d.io) != 0:
      save = errno; close(d.fd); free(d); errno = save; return -1
  else:
    d.pending_err = errno
    if odin_event_timer_start(loop, 0, 0, on_deferred_error, d, &d.timer) != 0:
      save = errno; close(d.fd); free(d); errno = save; return -1
  *out = d; return 0

on_writable(loop, io, fd, events, user_data):    /* user_data = d */
  complete(d, so_error(fd))

on_deferred_error(loop, timer, user_data):        /* user_data = d; d.pending_err != 0 */
  complete(d, d.pending_err)

complete(d, err):
  if d.io != NULL:    odin_event_io_stop(d.io);       d.io = NULL
  if d.timer != NULL: odin_event_timer_stop(d.timer); d.timer = NULL
  cb = d.on_done; ud = d.user_data
  if err == 0:
    out_fd = d.fd; d.fd = -1                    /* transfer ownership to caller */
    cb(d, ODIN_DIAL_OK, out_fd, 0, ud)          /* final action; caller may destroy(d) here */
  else:
    close(d.fd); d.fd = -1                      /* dial owned it; close on failure */
    cb(d, ODIN_DIAL_ERROR, -1, err, ud)         /* final action */

odin_dial_destroy(d):
  if d == NULL: return
  if d.io != NULL:    odin_event_io_stop(d.io);       d.io = NULL
  if d.timer != NULL: odin_event_timer_stop(d.timer); d.timer = NULL
  if d.fd >= 0: close(d.fd); d.fd = -1          /* still owned: in-flight or aborted */
  free(d)
```

Satisfies: G1 via the `connect` -> WRITE watch -> `so_error == 0` -> `ODIN_DIAL_OK` path that delivers the connected fd; G2 via the two failure paths (`so_error != 0` for in-flight failures and the captured-errno deferred timer for immediate failures), each closing the socket before an `fd == -1` callback, plus `start`'s close-and-free rollback on local setup failure; G3 via using only `odin_event_io_*` and `odin_event_timer_*` to drive the lifecycle with no thread and no resolver; G4 via `complete`'s owns-until-handoff fd transfer and the registration-stop that `odin_dial_destroy` reuses to abort an in-flight attempt and close the still-owned socket.

## 4. Backward Compatibility & Migration

Not applicable — this RFC adds `odin/dial.{c,h}`, test-only `odin/dial_internal_test.h` and `odin/dial_testing.c`, and `odin/dial_unittests.cpp` as brand-new files with no prior callers, and only appends them and new targets to `odin/BUILD.gn`, so nothing that compiled or ran before this RFC changes behavior.

## 5. Security

Not applicable — `odin_dial` receives an already-resolved `struct sockaddr`, passes it verbatim to `connect(2)`, and parses no attacker-controlled bytes, allocates nothing sized by input, holds no credentials, and persists nothing; the address-selection trust decision (refusing internal or link-local targets) is made upstream by whoever resolved the address, so this RFC introduces no new trust boundary.

## 6. Testing Strategy

**Shared fixture (fork deadline + watchdog).** Every row calls `odin_event_loop_run`, so each executes under the same `fork` / `waitpid` deadline fixture RFC-010 §6 and RFC-011 §6 established (the `WIFEXITED` / `WEXITSTATUS` harness): the child runs the loop and all `ASSERT_*` / `EXPECT_*`, then `_exit(::testing::Test::HasFailure() ? 1 : 0)`, and the parent fails the row unless the child exits `0` within a 2 s deadline. Each row also arms a one-shot watchdog `odin_event_timer` (100 ms) that sets `state.timed_out` and calls `odin_event_loop_stop`, so a dial that never completes cannot hang the child; `on_done` records `status` / `err` / `fd` / `calls` and calls `odin_event_loop_stop`. Because the dial watches a single fd and needs no peer thread to make `connect` resolve, every row is single-threaded and surfaces red through an executed in-child assertion — no thread-join or hang-as-red path is needed. All rows run on the macOS `kqueue` backend per the RFC-010 §6 constraint that Odin unit tests currently run only on macOS, with the Linux `epoll` build verified by cross-compilation; the dial uses only `socket` / `fcntl` / `connect` / `getsockopt` / `close` plus the public `odin_event_io_*` / `odin_event_timer_*` API, so it is backend-agnostic, and the uniform-via-callback contract makes each row's observable outcome identical even where Linux classifies a refusal synchronously while macOS resolves it in flight.

**Test-only fd-ownership hook.** `dial_unittests.cpp` compiles with `ODIN_DIAL_TESTING` and includes `odin/dial_internal_test.h`, which declares `int odin_dial_test_fd(odin_dial_t *dial)` — it returns the socket the dial currently owns, or `-1` with `errno == ENOENT` once ownership has left (after an `ODIN_DIAL_OK` handoff, an `ODIN_DIAL_ERROR` close, or before any socket exists). Rows that assert the module closed its own socket capture the fd with this helper immediately after `odin_dial_start` returns, then after `run` assert `fcntl(fd, F_GETFD) == -1` with `errno == EBADF`; the success rows use it to confirm the dial relinquished ownership (returns `-1` / `ENOENT`) after `ODIN_DIAL_OK`.

**Backend-behavior basis.** The per-row triggers below were confirmed on this RFC's macOS `kqueue` target: a loopback TCP `connect` to a live or a closed port returns `EINPROGRESS` and then reports writable with `SO_ERROR == 0` or `SO_ERROR == ECONNREFUSED` respectively; an `AF_UNIX` `connect` to a live listener returns `0` synchronously; an `AF_UNIX` `connect` to a nonexistent path returns `-1` / `ENOENT` synchronously; `socket(2)` with an unsupported family returns `-1` / `EAFNOSUPPORT`; and a `connect` to `192.0.2.1` (RFC 5737 TEST-NET-1) returns `EINPROGRESS` and stays in flight well past the 100 ms watchdog.

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | Loopback TCP dial succeeds; connected fd handed to caller, ownership transferred | `bind`+`listen` a `127.0.0.1:0` `AF_INET` / `SOCK_STREAM` listener (`SO_REUSEADDR`), read its ephemeral port via `getsockname`; arm 100 ms watchdog; `odin_dial_start(loop, &addr, sizeof addr, on_done, &st, &d)`; capture `fd0 = odin_dial_test_fd(d)`; `run` (`on_done` records `status`/`fd`/`calls` then `stop`); after `run`: `accept` the listener into `srv`, `write(fd, "hi", 2)`, `read(srv)`; then `odin_dial_test_fd(d)`; `odin_dial_destroy(d)` | `start` and `run` return `0`; `on_done.calls == 1`, `status == ODIN_DIAL_OK`, `err == 0`, `fd >= 0` and `fd == fd0` (the same socket the dial created); `timed_out == false`; `read(srv)` yields `"hi"`, proving a genuinely connected stream socket; `odin_dial_test_fd(d)` returns `-1` / `ENOENT` (ownership transferred); the test closes `fd`, `srv`, `listener` | G1 | unit |
| T2 | AF_UNIX dial completes synchronously (`connect()==0`) yet is still delivered via the loop as OK | Create+`bind` an `AF_UNIX` / `SOCK_STREAM` listener at a unique temp path, `listen(1)`; arm 100 ms watchdog; `odin_dial_start` to that `sockaddr_un`; capture `fd0`; `run` (`on_done` records + `stop`); after `run`: `accept` into `srv`, exchange a byte; `odin_dial_test_fd(d)`; `odin_dial_destroy(d)`; `unlink` the path | `start` and `run` return `0`; `on_done.calls == 1`, `status == ODIN_DIAL_OK`, `err == 0`, `fd == fd0 >= 0`; `timed_out == false`; the byte round-trips through the AF_UNIX pair; `odin_dial_test_fd(d)` returns `-1` / `ENOENT` — exercising the `connect()==0` classification arm (a dialer treating only `EINPROGRESS` as in-flight would misroute this immediate success to the error path); the test closes `fd`/`srv`/`listener` | G1 | unit |
| T3 | Loopback TCP dial to a closed port fails via the writable then `getsockopt(SO_ERROR)` path; socket closed, no leak | Obtain an unused loopback port (`bind` a `127.0.0.1:0` `SO_REUSEADDR` socket, `getsockname`, then `close` it); arm 100 ms watchdog; `odin_dial_start` to that address; capture `fd0`; `run` (`on_done` records + `stop`); after `run`: `fcntl(fd0, F_GETFD)`; `odin_dial_destroy(d)` | `start` and `run` return `0`; `on_done.calls == 1`, `status == ODIN_DIAL_ERROR`, `err == ECONNREFUSED`, `fd == -1`; `timed_out == false`; `fcntl(fd0, F_GETFD) == -1` with `errno == EBADF` (the dial closed the socket it created — no descriptor leak); on this kqueue backend the refusal is observed through the in-flight `connect` then writable then `getsockopt(SO_ERROR)` resolution | G2 | unit |
| T4 | AF_UNIX dial to a nonexistent path fails immediately and is delivered via the deferred-error timer; socket closed | Build an `AF_UNIX` `sockaddr_un` for a unique path guaranteed not to exist (never created); arm 100 ms watchdog; `odin_dial_start`; capture `fd0`; `run` (`on_done` records + `stop`); after `run`: `fcntl(fd0, F_GETFD)`; `odin_dial_destroy(d)` | `start` and `run` return `0`; `on_done.calls == 1`, `status == ODIN_DIAL_ERROR`, `err == ENOENT`, `fd == -1`; `timed_out == false`; `fcntl(fd0, F_GETFD) == -1` with `errno == EBADF` (socket closed, no leak) — exercising the immediate-`connect`-failure then 0-delay one-shot timer delivery path, distinct from T3's in-flight `SO_ERROR` path | G2 | unit |
| T5 | `odin_dial_start` local setup failure: unsupported family makes `socket(2)` fail; `*out` untouched, no callback, nothing leaked | A `struct sockaddr` with `sa_family` set to an unsupported value (`255`); preset sentinel `odin_dial_t *d = (odin_dial_t *)-1`; `odin_dial_start(loop, &bad, sizeof bad, on_done, &st, &d)`; then arm 100 ms watchdog and `run` to confirm no callback path exists | `odin_dial_start` returns `-1` with `errno == EAFNOSUPPORT` and leaves `d == (odin_dial_t *)-1` (wrote nothing to `*out`); the subsequent `run` reaches the watchdog (`timed_out == true`) with `on_done.calls == 0` (the failed `start` created no dial, registered nothing on the loop, and `socket(2)` failing before any fd exists leaks nothing); the test destroys nothing (no dial was created) | G2 | unit |
| T6 | `odin_dial_destroy` aborts an in-flight dial: still-owned socket closed, callback never fires | An `AF_INET` `sockaddr` for blackhole `192.0.2.1:80` (`connect` returns `EINPROGRESS` and stays in flight); arm a one-shot 30 ms timer whose callback calls `odin_dial_destroy(d)` (aborting the in-flight dial whose WRITE watch is still active); arm a one-shot 100 ms watchdog that sets `timed_out` and `stop`s; `odin_dial_start` to the blackhole; capture `fd0 = odin_dial_test_fd(d)`; `run` | `start` and `run` return `0`; `fd0 >= 0` (the dial registered an in-flight attempt and owns the socket — fails against the P1 stub, which registers nothing and whose `odin_dial_test_fd` returns `-1` / `ENOENT`, making the row red); `on_done.calls == 0` (`destroy` never invokes the callback); `timed_out == true` (only the watchdog stopped the loop — the aborted dial never completes); `fcntl(fd0, F_GETFD) == -1` with `errno == EBADF` (`destroy` closed the still-owned socket); under the P2 ASan command no use-after-free is reported (the WRITE watch was stopped before the free, so no later readiness re-enters the freed dial) | G4 | unit |
| T7 | `odin_dial_destroy` from within `on_done` is safe and does not close the handed-off fd | Loopback TCP listener as in T1; arm 100 ms watchdog; `odin_dial_start` to it; `run`; `on_done` records `fd`/`status`/`calls`, calls `odin_dial_destroy(dial)`, then `odin_event_loop_stop`; after `run`: `accept` into `srv` and exchange a byte over `fd`, and `fcntl(fd, F_GETFD)` | `start` and `run` return `0`; `on_done.calls == 1`, `status == ODIN_DIAL_OK`, `fd >= 0`; `timed_out == false`; under the P2 ASan command no use-after-free or double-free is reported (the dial reads no state after `on_done` returns, so the in-callback `destroy` is safe); after `run`, `fcntl(fd, F_GETFD)` succeeds and a byte round-trips via the accepted peer — `destroy` did not close the descriptor handed to the caller; the test closes `fd`/`srv`/`listener` | G4 | unit |

One `odin_dial_start` rollback branch has no dedicated row: the close-partial-socket-and-free path taken when `odin_event_io_start` or `odin_event_timer_start` fails *after* `socket(2)` and `set_nonblocking` succeed. It has no deterministic trigger under this module's contract — the dial always watches a freshly created fd, so `odin_event_io_start` cannot collide (`EEXIST` needs an already-watched fd) and `odin_event_timer_start` fails only on allocation failure, neither reproducible without fault injection the event loop does not expose. T5 exercises the sibling rollback branch (`socket(2)` itself failing, before any fd exists), and the post-`socket` rollback is the same `saved = errno; close(fd); free(d); errno = saved; return -1` shape validated by inspection of §3.2.2. T3's in-flight `SO_ERROR` path and T4's immediate-failure deferred-timer path jointly cover both connection-failure classification arms.

## 7. Implementation Plan

- **P1. Land the dial surface and red-verifiable `T1`–`T7` in the test binary only.**
  - **Scope:** add `odin/dial.h` exactly as §3.2.1 specifies — the `odin_dial_t` opaque type, the `odin_dial_status_t` enum in the order `OK`, `ERROR`, the `odin_dial_cb` typedef, and the `odin_dial_start` / `odin_dial_destroy` declarations inside one `extern "C"` block, with the caller-owned-fd, owns-until-handoff, exactly-once / final-action `on_done`, and `destroy`-in-callback clauses pinned in the header doc-comment; add `odin/dial_internal_test.h` whose `odin_dial_test_fd` declaration is visible only under `ODIN_DIAL_TESTING`; add `odin/dial.c` with a **stub** whose `odin_dial_start` allocates the `dial` object, stores `on_done` / `user_data`, sets `fd = -1`, registers **no** watch and **no** timer, and returns `0`, whose `odin_dial_destroy` frees the object, and whose `ODIN_DIAL_TESTING` helper returns `-1` / `ENOENT` (the stub owns no socket) — so against the stub no connection is attempted, `on_done` never fires, and no fd is owned; add `odin/dial_testing.c` containing `#include "dial.c" // NOLINT(bugprone-suspicious-include)` so the test binary compiles `dial.c` under `ODIN_DIAL_TESTING`; add `odin/dial_unittests.cpp` containing `T1`–`T7` from §6 as separate `TEST(OdinDialTest, T#)` cases plus the replicated `fork`+deadline fixture and per-row watchdog timer, each row guarded by a leading `if (!getenv("ODIN_DIAL_RED")) GTEST_SKIP() << "pending RFC-012 P2";`. In `odin/BUILD.gn`, add a test-only `config("odin_dial_testing_config") { defines = [ "ODIN_DIAL_TESTING" ] }`, append `"dial.h"`, `"dial_internal_test.h"`, `"dial_testing.c"`, and `"dial_unittests.cpp"` to the `executable("odin_unittests")` `sources` array, and add that config to `odin_unittests`'s `configs` — no production target and no `:odin` dependency this phase, so the dial is unavailable to production callers while `T1`–`T7` are intentionally red. The stub compiles into `odin_unittests` against the existing `:odin_event_loop_testing` dependency, which supplies the `odin_event_io_*` / `odin_event_timer_*` symbols and `odin/event_loop.h`.
  - **Depends on:** None.
  - **Done when:** `./tool/gn gen out/dial_mac --args='target_os="mac"'` and `./tool/gn gen out/dial_linux_x64 --args='target_os="linux" target_cpu="x64"'` resolve, and `./tool/ninja -C out/dial_mac odin_main odin_unittests tests` and `./tool/ninja -C out/dial_linux_x64 odin_main odin_unittests tests` build without error; the red-verification command `ODIN_DIAL_RED=1 out/dial_mac/odin_unittests --gtest_filter='OdinDialTest.*'` reports `T1`–`T7` each failing against the stub through the forked child's nonzero `_exit` and the parent's `WIFEXITED && WEXITSTATUS == 0` check — `T1`/`T2`/`T3`/`T4`/`T7` because `on_done` never fires so the `on_done.calls == 1` / `status` assertions fail after the watchdog stops the loop, `T5` because the stub's `odin_dial_start` returns `0` instead of `-1` / `EAFNOSUPPORT`, and `T6` because `odin_dial_test_fd` returns `-1` / `ENOENT` so the `fd0 >= 0` capture assertion fails; the default `out/dial_mac/odin_unittests --gtest_brief=1` reports `T1`–`T7` `SKIPPED` and exits zero with every pre-existing Odin suite green; the `out/dial_linux_x64/odin_unittests` binary is cross-compiled but not run; the public `:odin` label still does not link `dial.c`, non-test translation units see no dial declarations, and production objects expose no `odin_dial_test_*` symbol.
- **P2. Implement the dial and turn `T1`–`T7` green.**
  - **Scope:** replace the `odin/dial.c` stub with the full implementation from §3.2.2 — `odin_dial_start`'s `socket` / `set_nonblocking` / `connect` sequence with the `0` / `EINPROGRESS` / `EINTR` -> WRITE-watch versus immediate-error -> 0-delay one-shot timer classification and the `saved = errno; close; free; return -1` setup-failure rollback; `on_writable` reading `so_error` (`getsockopt(SO_ERROR)`); `on_deferred_error` delivering `pending_err`; `complete`'s stop-registration, ownership-transfer-on-success / close-on-failure, and final-action `on_done`; and `odin_dial_destroy`'s stop-registration, close-if-owned, and free (closing no handed-off fd). Under `ODIN_DIAL_TESTING`, implement `odin_dial_test_fd` to return the currently owned `fd` or `-1` / `ENOENT` once ownership has left; production builds omit that symbol. In `odin/BUILD.gn`, add `source_set("odin_dial") { sources = [ "dial.c", "dial.h" ]; public_deps = [ ":odin_event_loop" ] }` and append `":odin_dial"` to `source_set("odin")`'s `deps`, so production callers can link the dial; `odin_unittests` keeps compiling `dial.c` through `dial_testing.c` against `:odin_event_loop_testing` (no `:odin_dial` dependency, so `event_loop.c` is never linked twice) with `:odin_dial_testing_config` still applied. Remove the `ODIN_DIAL_RED` skip guard from `T1`–`T7`. No `odin/main.c` or `odin/cli.c` change — this RFC ships the importable surface only, matching the RFC-009 / RFC-011 pattern that a follow-up RFC wires the proxy.
  - **Depends on:** P1.
  - **Done when:** the P1 build commands still succeed; `out/dial_mac/odin_unittests --gtest_filter='OdinDialTest.*'` passes `T1`–`T7` un-gated, including T1/T2's `ODIN_DIAL_OK` connected-fd delivery and ownership transfer, T3's in-flight `ECONNREFUSED` teardown with the dial's socket closed (`fd0` -> `EBADF`), T4's immediate-`ENOENT` deferred-timer teardown with the socket closed, T5's `-1` / `EAFNOSUPPORT` setup-failure with `*out` untouched and `on_done.calls == 0`, T6's in-flight `destroy` abort (`on_done.calls == 0`, `fd0` -> `EBADF`), and T7's `destroy`-in-callback safety with the handed-off fd still usable; the unfiltered `out/dial_mac/odin_unittests --gtest_brief=1` runs `T1`–`T7` and exits zero with every pre-existing Odin suite green; `./tool/gn gen out/dial_mac_asan --args='target_os="mac" is_asan=true'`, `./tool/ninja -C out/dial_mac_asan odin_unittests`, and `out/dial_mac_asan/odin_unittests --gtest_filter='OdinDialTest.*'` exit with no AddressSanitizer report, giving T6 and T7 their no-use-after-free coverage; `out/dial_linux_x64/odin_unittests` still cross-compiles with the dial linked; production `out/dial_mac/odin` links the dial symbols through `:odin` and contains no test-only symbol; `./tidy_odin.sh` exits clean over the odin tree (now including `dial.c`, `dial_testing.c`, and `dial_unittests.cpp`, with `dial.h` and `dial_internal_test.h` checked via the header filter).
