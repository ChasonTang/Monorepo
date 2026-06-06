# RFC-010: Odin Event Loop

## 1. Summary

Add `odin/event_loop.c` and `odin/event_loop.h`, a single-owner-thread C event-loop module that owns a macOS `kqueue` or Linux `epoll` backend, exposes create/run/stop/destroy lifecycle calls, level-triggered file-descriptor watches, monotonic one-shot/repeating timers, and a same-thread `post` queue so later Odin c-ares and xquic integration can drive DNS sockets, QUIC UDP sockets, xquic engine timers, and deferred callback work on one Odin thread.

## 2. Goals

- **G1.** Provide an `odin_event_loop_t` lifecycle API where `create` returns a loop with initialized platform backend descriptors and a recorded owner thread, `run` dispatches callbacks on that owner thread until `stop` is requested and the current materialized callback batch drains or a backend error occurs, and `destroy` closes loop-owned backend descriptors and releases handle, timer, and queued-task memory without closing caller-owned I/O fds.
- **G2.** Provide level-triggered I/O watches over caller-owned nonblocking fds where one active watch per fd can be started, updated between read/write masks, stopped, and dispatched with the exact watched fd, ready mask, and caller `user_data`.
- **G3.** Provide monotonic microsecond timers that can be started as one-shot or repeating timers, reset before firing or from their own callback, stopped, and dispatched on the owner thread without using wall-clock time.
- **G4.** Provide an owner-thread `odin_event_post` API that queues callback+`user_data` tasks, dispatches each uncanceled task at most once in FIFO order by queue insertion, lets the active task snapshot drain after `stop`, and cancels without callback any task still queued outside that snapshot when `stop` causes `run` to return or `destroy` begins.

## 3. Design

### 3.1 Overview

`odin/event_loop` is a new leaf module in the Odin library. No current caller is rewired by this RFC; later c-ares and xquic adapters call into the loop from the same Odin thread that owns it.

The loop is single-thread-affine. `create` records the owner thread, every public API except `odin_event_loop_destroy(NULL)` requires that owner thread, and debug builds assert on a wrong-thread call before mutating loop state or dispatching callbacks. This keeps the module free of cross-thread wake, mutex, and memory-ordering contracts in this RFC. Internally the loop owns four collaborators on that one thread: the platform backend (`kqueue` on macOS, `epoll` on Linux), an I/O-watch table over caller-owned fds, a timer set, and a posted-task queue. Each `run` pass interleaves them — it drains queued tasks, dispatches due timers, then blocks in the backend for fd readiness and dispatches the ready I/O callbacks — so pending timer deadlines and queued work bound how long that backend wait blocks. The backend descriptors and syscall flags, the wait-timeout derivation, and the sub-millisecond deadline rule are specified in §3.2.1 and §3.2.3, not here.

The module does not include or link c-ares or xquic. It exposes the primitives those libraries need according to the local headers: c-ares exposes timeout and fd-event processing through `ares_timeout` and `ares_process_fds` in `c-ares/include/ares.h:906-951`, and xquic requires an application timer callback that invokes `xqc_engine_main_logic` after a microsecond interval in `xquic/include/xquic/xquic.h:99-107` and `xquic/include/xquic/xquic.h:1742-1746`. Later adapters can translate c-ares `ARES_FD_EVENT_READ` / `ARES_FD_EVENT_WRITE` masks to Odin read/write masks, and can translate xquic `wake_after` microseconds to an Odin timer.

```
owner thread
    |
    v
 kqueue/epoll wait -> ready fd callbacks
        ^       |
        |       v
    timer heap  task FIFO
```

### 3.2 Detailed Design

#### 3.2.1 Lifecycle and Dispatch

```c
/* odin/event_loop.h */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct odin_event_loop_t odin_event_loop_t;

int odin_event_loop_create(odin_event_loop_t **out);
int odin_event_loop_run(odin_event_loop_t *loop);
void odin_event_loop_stop(odin_event_loop_t *loop);
void odin_event_loop_destroy(odin_event_loop_t *loop);
/* The extern "C" block remains open through the API fragments in §3.2.2-§3.2.4. */
```

**Unstated contract.** The public header uses one C++ linkage wrapper that covers every exported C declaration shown in §3.2.1 through §3.2.4; the closing `extern "C"` brace appears after `odin_event_post`. All pointer parameters are non-null preconditions except `odin_event_loop_destroy(NULL)`, which is a no-op to simplify cleanup paths after failed setup. Functions that return `int` return `0` on success and `-1` with `errno` set on failure. `create` records the calling thread as the loop owner and initializes the backend, task queue, timer heap, active I/O table, deferred-free list, and snapshot-depth counter before writing `*out`; on failure it releases partial resources and leaves `*out` unmodified. Every public API that receives a live loop or handle asserts in debug builds that the current thread is the recorded owner before mutating loop state or dispatching callbacks; wrong-thread use is undefined behavior in release builds. `run` is not recursive; a nested `run` on the owner thread returns `-1` with `errno = EALREADY`, but `run` may be called again after a prior `run` has returned. `stop` is an owner-thread API that affects only an active `run`: when no `run` is active it returns after the owner-thread check, and when `run` is active it sets a flag observed before the loop enters another backend wait or begins another dispatch pass. The dispatchers do not check that flag between callbacks already claimed by the current task snapshot, due-timer snapshot, production backend-ready batch, or synthetic backend batch: those still-live callbacks continue in batch order. `stop` cancels and frees tasks still queued outside the active task snapshot without callback before `run` returns. The loop tracks one `snapshot_depth` across posted-task, timer, production backend, and test synthetic backend dispatch snapshots. Stopped I/O handles are freed immediately only when that depth is zero, otherwise they move to the deferred-free list and are reclaimed by the snapshot exit that returns the depth to zero. Stopped timer handles use the same snapshot rule and also remain allocated until every timer heap entry that references them has been popped or discarded, so wait preparation and due-timer cleanup can safely inspect stale entries after `odin_event_timer_stop` on an unfired timer. `destroy` must not run while `run` is active and is a debug-asserted precondition violation if `snapshot_depth` is nonzero. It de-registers active I/O handles, releases loop-owned handles and timers, drops queued tasks without invoking their callbacks, closes only loop-owned backend descriptors (the macOS `kqueue` fd, or the Linux `epoll` fd plus timerfd), and never closes caller-owned watched fds. The lifecycle stays split into `create`/`run`/`stop`/`destroy`: callers need a constructed loop to register I/O watches, timers, and startup tasks before the first `run`, while `stop` must be callable from callbacks without freeing memory that the still-running dispatch stack may touch. The loop has an internal monotonic microsecond clock for timers; this RFC does not expose a public time-reading API.

**Mechanism.**

```
create(out):
  allocate loop; record owner thread; create platform backend
  initialize task FIFO, timer heap, io table, deferred-free list, snapshot_depth=0
  *out = loop; return 0

run(loop):
  assert current thread is loop owner
  if loop already running: errno = EALREADY; return -1
  loop.running = 1; loop.stop = 0
  while loop.stop == 0:
    drain one snapshot of posted tasks
    if loop.stop != 0: break
    dispatch one snapshot of timers due at pass start
    if loop.stop != 0: break
    next_due = first live timer deadline after stale heap entries are discarded
    events = backend_wait_nonblocking() if task FIFO is non-empty
             else backend_wait_until(next_due)
    if backend_wait failed with EINTR: continue
    if backend_wait failed: loop.running = 0; return -1 with errno preserved
    translate, aggregate, and dispatch live I/O events from the current backend batch
  drop queued task nodes that were not claimed by a drained snapshot
  reclaim deferred handles if snapshot_depth == 0
  loop.running = 0
  return 0

stop(loop):
  assert current thread is loop owner
  if loop.running:
    loop.stop = 1
```

```
destroy(loop):
  if loop == NULL: return
  assert current thread is loop owner
  assert loop.running == 0 and loop.snapshot_depth == 0
  for each active io in io table:
    request backend deletion for io.fd filters
    remove io.fd from table
    io.active = 0; io.generation += 1; free io handle
  discard all timer heap entries, releasing their heap pins
  free every active timer handle that remains owned by the loop
  cancel queued task nodes without invoking callbacks
  reclaim every handle on the deferred-free list
  close kqueue fd on macOS, or epoll fd and timerfd on Linux
  free backend state, task FIFO, timer heap, io table, and loop object

enter_dispatch_snapshot(loop):
  loop.snapshot_depth += 1

leave_dispatch_snapshot(loop):
  loop.snapshot_depth -= 1
  if loop.snapshot_depth == 0: reclaim eligible deferred handles

retire_io_handle(loop, io):
  if loop.snapshot_depth == 0: free io
  else append io to deferred-free list

retire_timer_handle(loop, timer):
  if timer.heap_refs > 0: return
  if loop.snapshot_depth == 0: free timer
  else append timer to deferred-free list if it is not already linked

release_timer_heap_ref(loop, timer):
  timer.heap_refs -= 1
  if timer.active == 0: retire_timer_handle(loop, timer)
```

`backend_wait_until(next_due)` is backend-specific. On Linux, the loop arms the epoll-registered timerfd with `timerfd_settime(TFD_TIMER_ABSTIME)` for the absolute `CLOCK_MONOTONIC` deadline converted from microseconds to `struct timespec` by seconds/remainder arithmetic, disarms it when no timer exists, and calls `epoll_wait` with timeout `-1`; `epoll_wait`'s millisecond timeout is used only for the nonblocking `0` task case and never for timer deadlines. Timerfd readiness is an internal wake: `backend_wait` drains the timerfd counter and excludes that event from caller I/O aggregation, so due timers dispatch on the next loop iteration's timer pass. On macOS, the loop passes `NULL` to `kevent` when no timer exists, `{0, 0}` when the nearest timer is already due, or a positive nanosecond `timespec` computed from `next_due - monotonic_us()`; a positive sub-millisecond remainder stays positive instead of truncating to `{0, 0}`. The loop reads time through an internal monotonic-clock helper, and the timerfd/kevent conversion is isolated in an internal wait-preparation helper. A GN config named `:odin_event_loop_testing_config` defines `ODIN_EVENT_LOOP_TESTING` and is applied only to the test-only `:odin_event_loop_testing` source set and `:odin_unittests` executable, so `event_loop.c` compiles the hook definitions and `event_loop_unittests.cpp` sees the gated prototypes in the same build. Production `:odin_event_loop` and `:odin` compile the same implementation without that config, `odin/event_loop.h` never declares the hooks, and production objects define no `odin_event_loop_test_*` symbols. The non-public testing hook contract is pinned in §3.2.5.

Satisfies: G1 via the exported lifecycle calls, the backend ownership contract, the owner-thread assertion guard, the non-recursive run guard, the stop-driven dispatch loop, and the destroy teardown order.

#### 3.2.2 I/O Watch API

```c
#define ODIN_EVENT_READ  0x01u
#define ODIN_EVENT_WRITE 0x02u
#define ODIN_EVENT_ERROR 0x04u

typedef struct odin_event_io_t odin_event_io_t;

typedef void (*odin_event_io_cb)(odin_event_loop_t *loop,
                                 odin_event_io_t *io,
                                 int fd,
                                 unsigned int events,
                                 void *user_data);

int odin_event_io_start(odin_event_loop_t *loop, int fd,
                        unsigned int events, odin_event_io_cb cb,
                        void *user_data, odin_event_io_t **out);
int odin_event_io_update(odin_event_io_t *io, unsigned int events);
void odin_event_io_stop(odin_event_io_t *io);
```

**Unstated contract.** The accepted input event mask for `start` and `update` is exactly a non-empty subset of `ODIN_EVENT_READ | ODIN_EVENT_WRITE`. `ODIN_EVENT_ERROR` is output-only, so input masks such as `0`, `ODIN_EVENT_ERROR`, `ODIN_EVENT_READ | ODIN_EVENT_ERROR`, `ODIN_EVENT_WRITE | 0x80`, or any other unknown bit return `-1` with `errno = EINVAL` and do not create or modify a watch. `fd` must be a valid nonblocking descriptor owned by the caller and must remain open while the watch is active. The event loop never calls `read`, `write`, `send`, `recv`, or `close` on watched fds. At most one active I/O watch may exist for a given fd in one loop; a second `start` for the same fd returns `-1` with `errno = EEXIST`. `update` is valid only while the handle is active; after `stop`, the caller must not use the `odin_event_io_t *` again, and no public `EINVAL` result is promised for mutating a stopped handle. `start` returns `-1` with `errno = ENOMEM` on handle allocation failure. A backend add failure returns `-1` with the backend `errno` preserved and leaves no watch registered; a backend modify failure returns `-1` with the backend `errno` preserved and leaves the previous registration intact. Those failure guarantees apply to multi-filter macOS registrations: a failed read/write add leaves neither kqueue filter registered, and a failed read/write update leaves the old kqueue filter set and old `udata` bindings in place. `stop` is a void API: it preserves the caller's incoming `errno`, de-registers an active fd from the backend, removes the fd from the active table, increments the handle generation, marks the handle inactive, and defers handle memory reclamation until `loop.snapshot_depth == 0`, so no posted-task, timer, production backend, or synthetic backend snapshot can still hold the pointer. I/O mutators are owner-thread APIs and must be called before `run`, from a loop callback, or from a posted task; debug builds assert if another thread calls them. There is no cross-thread I/O mutation entry point in this RFC. Callbacks are level-triggered: if a fd remains readable or writable, the backend may report it again after the callback returns. A backend wait result is materialized before I/O callbacks begin; if one I/O callback calls `odin_event_loop_stop`, the remaining active and generation-matched entries in that same materialized backend batch still dispatch in registration-sequence order. Backend error/hangup conditions are ORed into `ODIN_EVENT_ERROR` and dispatched even when only read or write was requested.

**Mechanism.**

```
io_start(loop, fd, events, cb, user_data, out):
  assert current thread is loop owner
  reject any mask outside a non-empty READ/WRITE subset with errno = EINVAL
  reject duplicate fd with errno = EEXIST
  allocate io handle with active=1, generation=1, and registration sequence
  if allocation fails: errno = ENOMEM; return -1
  backend_io_add(io, events)
  on backend add failure, free io and return -1 with backend errno preserved
  insert fd -> io in loop table
  *out = io; return 0

io_update(io, events):
  assert current thread is io loop owner
  if events is not a non-empty READ/WRITE subset:
    errno = EINVAL; return -1
  backend_io_update(io, events); on backend update failure return -1
  io.events = events
  io.generation += 1
  return 0

io_stop(io):
  save errno
  assert current thread is io loop owner
  if io.active:
    io.generation += 1
    remove io.fd from loop table
    request backend deletion for io.fd filters; no status is exposed by this API
    io.active = 0
    retire_io_handle(io.loop, io)
  restore errno
```

```
dispatch_backend_events(batch):
  enter_dispatch_snapshot(loop)
  translate platform flags to ODIN_EVENT_* masks
  aggregate masks by io handle and observed generation
  sort aggregated handles by registration sequence
  for each item:
    if item.io.active and item.generation == item.io.generation:
      call item.io.cb(loop, item.io, item.io.fd, item.mask, item.io.user_data)
  leave_dispatch_snapshot(loop)
```

On Linux, `backend_io_add` uses one `epoll_ctl(ADD)` call with `EPOLLIN` and/or `EPOLLOUT` plus error/hangup reporting, and `backend_io_update` uses one `epoll_ctl(MOD)` call. On macOS, both helpers use `kevent` changes with `EV_RECEIPT`, inspect one receipt per requested read/write filter change, and update the loop's active fd table and `io.events` only after every receipt in the operation succeeds. For add, the helper submits `EV_ADD | EV_RECEIPT` for the requested `EVFILT_READ` and/or `EVFILT_WRITE` filters with `udata = io`; if any receipt reports a non-zero error, it submits `EV_DELETE | EV_RECEIPT` rollback changes for every filter whose add receipt succeeded before freeing `io`. For update, the helper first adds filters in `new_events & ~old_events`; if any add receipt fails, it deletes only the newly added successful filters and leaves the old filters untouched. It then deletes filters in `old_events & ~new_events`; if that delete receipt fails after a new filter was added, it deletes the new filter before returning failure, so the old registration remains the only live registration. The first failing receipt's error becomes public `errno`; the helper verifies successful rollback receipts before freeing a failed-start handle or returning from a failed update.

`dispatch_backend_events` is one internal helper shared by the production backend wait path and the direct unit-test hook pinned in §3.2.5. The direct hook accepts synthetic ready entries containing the loop-owned I/O handle and an `ODIN_EVENT_*` ready mask, snapshots every entry's current generation before dispatching the first callback, and calls this helper without waiting on kqueue or epoll; the helper enters the same backend dispatch snapshot guard for both callers. It exists only so T8 can deterministically feed a same-batch A-then-B stale entry where callback A stops B before B's already-materialized entry is dispatched. The queued synthetic backend hook in §3.2.5 uses the same helper through the `run` backend-wait path, so T19 proves stop-during-active-run behavior rather than a direct dispatch outside `run`.

Satisfies: G2 via the fd-owned watch handle, owner-thread mutation contract, read/write mask update contract, kqueue add/update rollback guarantees, level-triggered dispatch, duplicate-fd rejection, and caller-owned-fd boundary.

#### 3.2.3 Timer API

```c
typedef struct odin_event_timer_t odin_event_timer_t;

typedef void (*odin_event_timer_cb)(odin_event_loop_t *loop,
                                    odin_event_timer_t *timer,
                                    void *user_data);

int odin_event_timer_start(odin_event_loop_t *loop, uint64_t delay_us,
                           uint64_t repeat_us, odin_event_timer_cb cb,
                           void *user_data, odin_event_timer_t **out);
int odin_event_timer_reset(odin_event_timer_t *timer, uint64_t delay_us,
                           uint64_t repeat_us);
void odin_event_timer_stop(odin_event_timer_t *timer);
```

**Unstated contract.** Timers use the loop's internal monotonic clock; wall-clock adjustments do not move deadlines. `delay_us == 0` means the timer is due on the next timer-dispatch pass before the loop enters another blocking wait. A positive `delay_us`, including `1` through `999`, records `now + delay_us` as a future microsecond deadline, and backend wait preparation must pass that positive sub-millisecond interval or deadline to timerfd or kevent instead of normalizing it to zero or rounding it to a full millisecond. This is a deadline contract, not an ordering guarantee against later callbacks: if monotonic time has reached the deadline by the next timer-dispatch pass, the timer is due in that pass. `repeat_us == 0` creates a one-shot timer; a positive `repeat_us` makes the loop automatically reschedule the timer after each callback using `now_after_callback + repeat_us`, so callers do not implement repetition by resetting the timer themselves. Timer callbacks run only on the owner thread. A timer pass materializes the due set once, using `pass_now = monotonic_us()` before the first timer callback; if one timer callback calls `odin_event_loop_stop`, the remaining active and generation-matched timers in that same due snapshot still dispatch in `(due_us, sequence)` order. Timers posted or reset by a callback are not added to the already-materialized due snapshot; if `stop` remains set, those later heap entries do not dispatch before `run` returns. Timer mutators follow the same owner-thread rule as I/O mutators: call them before `run`, from a callback, or from a posted task; debug builds assert if another thread calls them. `start` returns `-1` with `errno = ENOMEM` on allocation failure or `errno = EOVERFLOW` if `now + delay_us` overflows. `reset` is valid only while the timer is active; it returns `0` on success and returns `-1` with `errno = EOVERFLOW` without changing the timer if the new deadline overflows. After a timer is stopped or auto-stopped, the caller must not use the `odin_event_timer_t *` again, and no public `EINVAL` result is promised for resetting a stopped timer. `reset` replaces both the next deadline and repeat interval, increments the timer generation, and pushes a new heap entry; old heap entries are left in place and skipped later when their generation no longer matches. Each pushed heap entry owns one internal heap pin on the timer handle, and that pin is released only when the entry is popped or discarded as stale. If a callback calls `reset` on the timer currently being dispatched, that explicit reset wins and the automatic `repeat_us` reschedule for the just-fired callback is skipped. If a callback calls `stop` on the timer currently being dispatched, no automatic reschedule occurs. If automatic repeat reschedule would overflow `monotonic_us() + repeat_us`, the loop auto-stops the timer using the same inactive/deferred-free path as a one-shot completion, does not push a new repeat heap entry, and does not invoke that timer callback again for the overflowed repeat; no `errno` is reported because no public API call is returning from the automatic path. After this auto-stop, wait preparation must observe no live deadline for that timer, so a clamped or saturated far-future repeat deadline is a contract violation. `stop` is a void API that preserves incoming `errno`, marks an active timer inactive, increments its generation, and retires the timer handle, but retired timer storage is freed only after both `loop.snapshot_depth == 0` and `timer.heap_refs == 0`. A stopped unfired timer can therefore leave a stale future heap entry behind without arming a backend deadline, dispatching a callback, or exposing freed storage to wait preparation. A one-shot timer whose callback returns without `reset` or `stop` is stopped by the loop at the end of that callback. Timer heap order is `(due_us, sequence)`, where `start`, `reset`, and automatic repeat reschedule each assign the next sequence, so timers made due for the same timestamp dispatch in that sequence order.

**Mechanism.**

```
timer_start(loop, delay_us, repeat_us, cb, user_data, out):
  assert current thread is loop owner
  now = monotonic_us()
  if now + delay_us overflows: errno = EOVERFLOW; return -1
  allocate timer with active=1, generation=1, and heap_refs=0
  if allocation fails: errno = ENOMEM; return -1
  timer.due_us = now + delay_us
  timer.repeat_us = repeat_us
  timer.sequence = next timer sequence
  heap_push_timer_entry(timer)
  *out = timer; return 0

timer_reset(timer, delay_us, repeat_us):
  assert current thread is timer loop owner
  now = monotonic_us()
  if now + delay_us overflows: errno = EOVERFLOW; return -1
  timer.generation += 1
  timer.due_us = now + delay_us
  timer.repeat_us = repeat_us
  timer.sequence = next timer sequence
  heap_push_timer_entry(timer)
  return 0

timer_stop(timer):
  save errno
  assert current thread is timer loop owner
  if timer.active:
    timer.active = 0
    timer.generation += 1
    retire_timer_handle(timer.loop, timer)
  restore errno
```

```
heap_push_timer_entry(timer):
  timer.heap_refs += 1
  heap_push({timer, timer.due_us, timer.generation, timer.sequence})

first_live_timer_deadline(loop):
  while heap is non-empty:
    entry = heap minimum
    timer = entry.timer
    if timer.active and entry.generation == timer.generation:
      return entry.due_us
    heap_pop()
    release_timer_heap_ref(loop, timer)
  return none

dispatch_due_timers(loop):
  enter_dispatch_snapshot(loop)
  pass_now = monotonic_us()
  snapshot = []
  while heap is non-empty:
    entry = heap minimum
    timer = entry.timer
    if timer.active == 0 or entry.generation != timer.generation:
      heap_pop()
      release_timer_heap_ref(loop, timer)
      continue
    if entry.due_us > pass_now: break
    snapshot append heap_pop()
    release_timer_heap_ref(loop, timer)
  for entry in snapshot order:
    timer = entry.timer
    if timer inactive or entry.generation != timer.generation: continue
    fired_generation = timer.generation
    timer.cb(loop, timer, timer.user_data)
    if timer.active and timer.generation == fired_generation and timer.repeat_us > 0:
      now = monotonic_us()
      if now + timer.repeat_us overflows:
        timer.active = 0
        timer.generation += 1
        retire_timer_handle(loop, timer)
        continue
      timer.due_us = now + timer.repeat_us
      timer.generation += 1
      timer.sequence = next timer sequence
      heap_push_timer_entry(timer)
    else if timer.active and timer.generation == fired_generation:
      timer.active = 0
      timer.generation += 1
      retire_timer_handle(loop, timer)
  leave_dispatch_snapshot(loop)
```

The test-only hooks pinned in §3.2.5 expose `odin_event_loop_test_live_timer_count(loop)` and the wait-preparation recorder. Both discard inactive and stale-generation heap entries with `first_live_timer_deadline`, releasing heap pins before observing the next live deadline. T4 uses those hooks after a one-shot callback returns without explicit stop/reset, T15 uses them after the first overflowing repeat callback and posted stop task, and T27 uses them after stopping an unfired future timer, proving each path left no active timer and no backend deadline armed while the liveness counters and ASan run cover leak and use-after-free regressions; production objects do not define these hooks.

Satisfies: G3 via the monotonic microsecond deadline contract, owner-thread timer mutation contract, one-shot/repeating behavior, reset/stop APIs, and heap-driven wait timeout.

#### 3.2.4 Posted Task API

```c
typedef void (*odin_event_task_cb)(odin_event_loop_t *loop, void *user_data);

int odin_event_post(odin_event_loop_t *loop, odin_event_task_cb cb,
                    void *user_data);

#ifdef __cplusplus
}
#endif
```

**Unstated contract.** `odin_event_post` is an owner-thread API valid after `create` succeeds and before `destroy` begins; debug builds assert if another thread calls it. It allocates one task node, appends it to the task FIFO, and returns `0`; on allocation failure it returns `-1` with `errno = ENOMEM` and no task is queued. A successful post is not an unconditional execution guarantee: each task node is either dispatched once on the owner thread or canceled without callback. FIFO order is the append order observed on the owner thread among task snapshots that are actually drained. A task callback may call `stop`, start or stop I/O watches, reset timers, or post another task. The loop drains one snapshot of the task FIFO per dispatch pass; callbacks already in that snapshot continue in FIFO order even if an earlier callback requests `stop`, while tasks posted by a running task wait until the next pass. If `stop` is requested before that next pass begins, those unsnapped tasks are canceled and freed without callback as `run` exits. `destroy` also cancels queued tasks without invoking callbacks. `run` must not enter a blocking backend wait while the task FIFO is non-empty: after the task snapshot and timer pass, queued tasks force the next backend wait timeout to `0`, causing the following loop pass to drain them even when no fd or timer is ready. Because this RFC has no cross-thread post, a post cannot wake a backend wait that already began with an empty FIFO; that wait is interrupted only by fd readiness, timer timeout, or `EINTR`.

**Mechanism.**

```
post(loop, cb, user_data):
  assert current thread is loop owner
  node = allocate task
  if allocation fails: errno = ENOMEM; return -1
  append node to tail
  return 0

drain_posted_tasks(loop):
  enter_dispatch_snapshot(loop)
  snapshot = current head..tail
  clear shared queue
  for node in snapshot order:
    node.cb(loop, node.user_data)
    free node
  leave_dispatch_snapshot(loop)
  tasks appended while draining remain queued for the next pass

cancel_queued_tasks(loop):
  free every task node still queued without invoking callbacks
```

Satisfies: G4 via the owner-thread FIFO insertion point, at-most-once task dispatch, explicit stop/destroy cancellation boundary, and snapshot drain rule.

#### 3.2.5 Test-only Internal Hooks

```c
/* odin/event_loop_internal_test.h */
#if defined(ODIN_EVENT_LOOP_TESTING)
#include <stddef.h>
#include <stdint.h>
#include "odin/event_loop.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ODIN_EVENT_LOOP_TEST_BACKEND_LINUX 1
#define ODIN_EVENT_LOOP_TEST_BACKEND_MACOS 2
#define ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_ADD 1
#define ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_DELETE 2

typedef struct {
  odin_event_io_t *io;
  unsigned int events;
} odin_event_loop_test_ready_t;

typedef struct {
  int armed;
  int epoll_timeout_ms;
  int64_t abs_sec;
  long abs_nsec;
} odin_event_loop_test_linux_wait_t;

typedef struct {
  int timeout_is_null;
  int64_t rel_sec;
  long rel_nsec;
} odin_event_loop_test_macos_wait_t;
```

```c
typedef struct {
  int backend;
  odin_event_loop_test_linux_wait_t linux_timerfd;
  odin_event_loop_test_macos_wait_t macos_kevent;
} odin_event_loop_test_wait_record_t;

typedef struct {
  int backend;
  int backend_fd;
  int timer_fd;
} odin_event_loop_test_fd_record_t;

typedef struct {
  size_t loops;
  size_t io_handles;
  size_t timers;
  size_t task_nodes;
} odin_event_loop_test_liveness_t;

void odin_event_loop_test_set_now_us(odin_event_loop_t *loop, uint64_t now_us);
size_t odin_event_loop_test_live_timer_count(odin_event_loop_t *loop);
void odin_event_loop_test_reset_liveness(void);
int odin_event_loop_test_liveness(odin_event_loop_test_liveness_t *out);
int odin_event_loop_test_fail_next_backend_create(int errnum);
int odin_event_loop_test_fail_next_backend_wait(
    odin_event_loop_t *loop,
    int errnum);
int odin_event_loop_test_prepare_wait(
    odin_event_loop_t *loop,
    odin_event_loop_test_wait_record_t *out);
int odin_event_loop_test_backend_fds(
    odin_event_loop_t *loop,
    odin_event_loop_test_fd_record_t *out);
int odin_event_loop_test_fail_next_kqueue_change(
    odin_event_loop_t *loop,
    int change,
    unsigned int event,
    int errnum);
int odin_event_loop_test_kqueue_registered_mask(
    odin_event_loop_t *loop,
    int fd,
    unsigned int *out_events);
int odin_event_loop_test_dispatch_backend_events(
    odin_event_loop_t *loop,
    const odin_event_loop_test_ready_t *entries,
    size_t count);
int odin_event_loop_test_queue_backend_events(
    odin_event_loop_t *loop,
    const odin_event_loop_test_ready_t *entries,
    size_t count);

#ifdef __cplusplus
}
#endif
#endif /* ODIN_EVENT_LOOP_TESTING */
```

**Unstated contract.** This header is test-only: when `ODIN_EVENT_LOOP_TESTING` is absent it declares no hook API, and production `:odin_event_loop` objects define no `odin_event_loop_test_*` symbols. The hook declarations sit inside one `extern "C"` linkage guard, opened after the `#include "odin/event_loop.h"` whose own wrapper has already closed, so the C hook definitions that `:odin_event_loop_testing` compiles from `event_loop.c` link unmangled from the C++ `event_loop_unittests.cpp` call sites. All loop-taking hooks are owner-thread APIs with non-null `loop` preconditions. `odin_event_loop_test_set_now_us` switches that loop to a fake monotonic clock and sets the current microsecond value used by timer start/reset, due-timer dispatch, and wait preparation. `odin_event_loop_test_live_timer_count` returns the number of active timers after discarding stale heap entries. `odin_event_loop_test_reset_liveness` requires that no loop-owned object is live, then resets the process-local testing counters for loop objects, I/O handles, timer handles, and queued-task nodes to zero. `odin_event_loop_test_liveness` writes those counters to `out` and returns `0`, or returns `-1` with `errno = EINVAL` when `out == NULL`; it is the only hook intentionally callable after `odin_event_loop_destroy(loop)` because it does not dereference a loop pointer. Each counter increments immediately after the corresponding test build allocation succeeds and decrements immediately before that object's storage is freed, so a post-destroy nonzero value is executable evidence of leaked loop-owned memory. `odin_event_loop_test_fail_next_backend_create` stores one process-local backend-create failure for the next `odin_event_loop_create` call and returns `0`, or returns `-1` with `errno = EINVAL` when `errnum <= 0`; the injected failure occurs after the loop allocation path has begun but before `create` writes `*out`, so T25 can prove partial cleanup and the unmodified-out contract. `odin_event_loop_test_fail_next_backend_wait` stores one pending non-`EINTR` backend-wait failure for the given loop's next backend wait and returns `0`, or returns `-1` with `errno = EINVAL` when `errnum <= 0` or `errnum == EINTR`; when consumed, the production wait path returns `-1` with that `errno` while `loop.running == 1`, and `run` clears `loop.running` before returning the failure. `odin_event_loop_test_prepare_wait` writes one complete record without blocking, dispatching callbacks, or mutating public callback queues; it returns `0` on success and `-1` with `errno = EINVAL` when `out == NULL`. On Linux, `backend == ODIN_EVENT_LOOP_TEST_BACKEND_LINUX`; `linux_timerfd.armed` is `1` only when a live timer deadline exists, `linux_timerfd.epoll_timeout_ms` is the value that would be passed to `epoll_wait`, and `abs_sec`/`abs_nsec` are the absolute timerfd deadline. On macOS, `backend == ODIN_EVENT_LOOP_TEST_BACKEND_MACOS`; `macos_kevent.timeout_is_null == 1` means `kevent` would receive a `NULL` timeout, otherwise `rel_sec`/`rel_nsec` are the relative timeout. `odin_event_loop_test_backend_fds` snapshots loop-owned descriptor numbers without duplicating or closing them; on macOS `backend_fd` is the `kqueue` fd and `timer_fd == -1`, while on Linux `backend_fd` is the `epoll` fd and `timer_fd` is the timerfd. It returns `0` on success and `-1` with `errno = EINVAL` when `out == NULL`. Tests may use the snapshot after `destroy` only with process fd APIs such as `fcntl(F_GETFD)`; they must not call back into the destroyed loop. `odin_event_loop_test_fail_next_kqueue_change` is macOS-only fault injection for T23: it stores one pending receipt failure for the next matching kqueue `EV_ADD` or `EV_DELETE` subchange on `ODIN_EVENT_READ` or `ODIN_EVENT_WRITE`; it returns `-1` with `errno = EINVAL` for invalid `change`, `event`, or non-positive `errnum`, and returns `-1` with `errno = EOPNOTSUPP` on Linux. `odin_event_loop_test_kqueue_registered_mask` is macOS-only observation for T23: it writes the current test-wrapper kqueue read/write filter mask for `fd` after receipt handling and rollback, returns `0` on macOS success, returns `-1` with `errno = EINVAL` when `out_events == NULL`, and returns `-1` with `errno = EOPNOTSUPP` on Linux. `odin_event_loop_test_dispatch_backend_events` returns `0` after dispatching the synthetic batch, or `-1` with `errno = EINVAL` if `count > 0` and `entries == NULL` or any entry references a handle from a different loop. Its `events` field uses the public output mask bits, so tests may pass `ODIN_EVENT_READ`, `ODIN_EVENT_WRITE`, and `ODIN_EVENT_ERROR`; this hook bypasses backend waiting but not active/generation checks. `odin_event_loop_test_queue_backend_events` validates the same synthetic entries, stores one pending batch, and returns `0`, or returns `-1` with `errno = EEXIST` if a pending synthetic batch already exists. The next backend wait performed by `odin_event_loop_run` consumes that pending batch while `loop.running == 1`, materializes generations immediately before dispatch, and calls the same aggregation/dispatch helper used by real kqueue/epoll readiness; it is the T19 hook for stop-during-active-I/O-batch behavior.

**Mechanism.**

```
prepare_wait(loop, out):
  next_due = first live timer deadline after stale heap entries are discarded
  fill Linux timerfd/epoll or macOS kevent record exactly as backend wait would
  return 0

backend_fds(loop, out):
  fill backend enum, kqueue/epoll fd, and Linux timerfd or macOS -1
  return 0

reset_liveness():
  assert all process-local loop-owned allocation counters are zero
  set loop, io handle, timer, and task-node counters to zero

liveness(out):
  fill process-local loop, io handle, timer, and task-node allocation counters
  return 0

fail_next_backend_create(errnum):
  store one-shot process-local create failure
  next create returns -1 with errno = errnum before writing *out

fail_next_backend_wait(loop, errnum):
  store one-shot wait failure on loop
  next backend wait returns -1 with errno = errnum

fail_next_kqueue_change(loop, change, event, errnum):
  on macOS, store one-shot receipt failure for the next matching kqueue subchange
  on Linux, errno = EOPNOTSUPP; return -1

kqueue_registered_mask(loop, fd, out_events):
  on macOS, report the test-wrapper mask after successful receipts and rollbacks
  on Linux, errno = EOPNOTSUPP; return -1

dispatch_backend_events(loop, entries, count):
  snapshot each entry's io pointer, generation, ready mask, and sequence
  pass the snapshot to the same aggregation and dispatch helper used by backend wait
  return 0

queue_backend_events(loop, entries, count):
  copy entries into loop's single pending synthetic backend batch
  the next run backend wait snapshots and dispatches that batch while running is true
  return 0
```

Satisfies: G1 via production/test symbol separation, observable backend descriptor snapshots, deterministic loop-owned allocation counters, backend-create fault injection, and backend-wait fault injection; G2 via deterministic synthetic backend batches and kqueue rollback fault injection/registered-mask observation; G3 via fake monotonic time, live-timer inspection, and platform-specific wait-preparation records.

## 4. Backward Compatibility & Migration

Not applicable — this RFC keeps existing exported Odin signatures and public GN labels intact while adding the event-loop API, tests, and a test-only hook target without changing any existing documented behavior, wire format, CLI flag, or caller contract.

## 5. Security

- **S1.**
  - **Threat:** Stale callback dispatch after a handle is stopped during the same dispatch snapshot. A local callback may stop an I/O watch or timer and release the state referenced by its `user_data`; if the backend or timer heap already produced another ready entry for that handle, invoking the stale callback can dereference invalid caller state. The externally influenced I/O trigger is two watched sockets becoming ready before one backend wait returns, then the first callback stopping the second watch before its ready entry is dispatched. The timer trigger is two due timers in the same timer batch, then the first callback stopping the second timer before its heap entry is dispatched.
  - **Mitigation:** §3.2.1, §3.2.2, §3.2.3, and §3.2.5 make watch and timer handles loop-owned, mark them inactive on `stop`, increment their generation, check active state and generation before every callback, defer I/O handle reclamation until `snapshot_depth` returns to zero across posted-task, timer, production backend, and synthetic backend snapshots, and keep stopped timer handles alive until both `snapshot_depth == 0` and all stale heap-entry pins have been released. The backend event pointer and timer heap entry never own caller `user_data`; each only identifies the loop-owned handle whose active state and generation are checked at dispatch time.
  - **Enforcement:** §6 row T8 feeds a synthetic same-batch backend event list with ready entry A followed by ready entry B, has callback A stop B before B's already-materialized entry dispatches, and asserts that B's callback count remains zero. §6 row T12 creates two due timers, has the first callback stop the second before it dispatches, and asserts that the second timer callback count remains zero.

- **S2.**
  - **Threat:** Stale kqueue `udata` after partial I/O registration failure. A read/write watch on macOS maps to multiple kqueue filters; if one filter change succeeds and a later filter change fails, freeing or logically rolling back the handle while the successful filter remains installed can make a later fd readiness event dispatch through a stale pointer.
  - **Mitigation:** §3.2.2 requires macOS I/O add and update helpers to use `EV_RECEIPT`, inspect each filter receipt, and explicitly roll back successful subchanges before returning backend failure or freeing a failed start handle.
  - **Enforcement:** §6 row T23 injects kqueue receipt failures for read/write add and update paths and asserts that the failed add leaves no registered filter mask while the failed update leaves the previous mask intact.

## 6. Testing Strategy

Every row below whose setup calls `odin_event_loop_run` (T1-T6, T9-T13, T15, T16, T18-T20, and T26) runs inside the `EventLoopRunDeadline` test fixture, which `fork`s before executing the row body. In the child, the fixture creates and runs the loop on its owner thread and executes every `ASSERT_*`/`EXPECT_*` in the row body, then calls `_exit(::testing::Test::HasFailure() ? 1 : 0)` so it never returns into the gtest runner; a failed child-side assertion (wrong callback count, out-of-order FIFO drain, missing `ODIN_EVENT_ERROR`, and so on) therefore becomes a nonzero child exit status. The parent `waitpid`s the child and applies two independent verdicts: (1) if the child has not exited within 2 seconds the parent kills it and fails the row with an `EventLoopRunDeadline` failure; (2) otherwise the parent fails the row unless `WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0`, so a child that died on a signal or exited nonzero from a failed assertion fails the row through the exit-status channel rather than the deadline. This extends the existing `fork`/`WIFEXITED`/`WEXITSTATUS` harness at `odin/cli_unittests.cpp:380-392` with one added step — the in-child `_exit(::testing::Test::HasFailure() ? 1 : 0)` that converts in-process gtest assertions to an exit code, since that prior test `execve`s a separate binary instead of asserting in-process. The deadline verdict is separate from any row-level watchdog timer and keeps a broken stop path, missed nested post, missed I/O callback, or long sentinel timer from hanging the suite. For a passing row the child exits `0` within 2 seconds, so each forked row's expected result is `WEXITSTATUS == 0` with no `EventLoopRunDeadline` failure. The remaining rows (T7, T8, T14, T17, T21-T25, and T27) never call `run`; their assertions execute directly in the parent process and fail the row in the ordinary gtest way.

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | Lifecycle run exits after posted stop | `odin_event_loop_create(&loop)`; `odin_event_post(loop, task, &state)` where `task` increments `state.calls` and calls `odin_event_loop_stop(loop)`; call `odin_event_loop_run(loop)`; assert the post callback count before calling `odin_event_loop_destroy(loop)` | `create` and `post` return `0`; `run` returns `0`; immediately after `run` returns and before `destroy`, `state.calls == 1`; `destroy` returns to the test without closing any caller fd because no watched fd was supplied | G1, G4 | unit |
| T2 | Read readiness is level-triggered and preserves fd, mask, and user data | `socketpair(AF_UNIX, SOCK_STREAM, 0, fds)`; set both fds `O_NONBLOCK` with `fcntl` and assert setup succeeds; start a read watch on `fds[1]` with `user_data = &state`; start a 100000 us watchdog timer that sets `state.timed_out = true` and stops the loop if it fires; write byte `"x"` to `fds[0]`; first read callback records fd/mask/user pointer and leaves the byte unread; second read callback records again, reads the byte, stops the watch, stops the watchdog timer, and stops the loop | `run` returns `0`; read callback count is exactly `2`; both observed fds are `fds[1]`; both observed masks include `ODIN_EVENT_READ`; both observed `user_data == &state`; `state.timed_out == false`; the second callback reads `"x"`; both socketpair fds remain caller-owned and close successfully in the test. An edge-triggered implementation that does not report the still-readable fd a second time reaches the watchdog failure path | G2 | unit |
| T3 | Write watch can update to read watch | `socketpair`; set both fds `O_NONBLOCK` with `fcntl` and assert setup succeeds; start a watch on `fds[1]` with `ODIN_EVENT_WRITE`; first callback records `WRITE`, calls `odin_event_io_update(io, ODIN_EVENT_READ)`, and writes byte `"r"` to `fds[0]`; second callback reads from `fds[1]`, stops the watch, and stops the loop | Callback sequence is exactly `WRITE` then `READ`; update returns `0`; second callback reads `"r"`; no third callback fires before `run` returns | G2 | unit |
| T4 | Zero-delay one-shot timer auto-stops after callback return | Create loop; `odin_event_timer_start(loop, 0, 0, timer_cb, &state, &timer)`; timer callback increments `state.timer_calls`, posts a stop task, and returns without calling `odin_event_timer_stop`, `odin_event_timer_reset`, or `odin_event_loop_stop`; the stop task increments `state.stop_task_calls` and stops the loop; after `run` returns, invoke `odin_event_loop_test_live_timer_count(loop)` and the wait-preparation recorder | `timer_start` and the stop-task post both return `0`; `run` returns `0`; `state.timer_calls == 1`; `state.stop_task_calls == 1`; live timer count is `0`; wait preparation records no armed timer deadline. An implementation that leaves the one-shot active after callback return fails the live-count or wait-preparation assertion | G3, G4 | unit |
| T5 | Timer reset replaces an unfired deadline | Create loop; start a one-shot timer with `delay_us = 60000000`; post a task that calls `odin_event_timer_reset(timer, 0, 0)`; timer callback increments `state.timer_calls` and stops the loop | Reset returns `0`; timer callback runs once in the same test run without waiting for the original 60-second deadline; `state.timer_calls == 1` | G3, G4 | unit |
| T6 | Same-thread posts keep FIFO order | Create loop; owner thread calls `odin_event_post(loop, task1, &state)` and then `odin_event_post(loop, task2, &state)` before `run`; `task1` appends `1`; `task2` appends `2` and stops the loop | Both posts return `0`; `run` returns `0`; recorded task order is exactly `[1, 2]`; both callbacks ran on the owner thread recorded by the test | G4 | unit |
| T7 | Duplicate active fd watch is rejected and destroy preserves the active watched fd | `socketpair`; set both fds `O_NONBLOCK` with `fcntl` and assert setup succeeds; start a read watch on `fds[1]`; attempt a second read watch on the same `fds[1]`; destroy the loop without stopping the first watch; after destroy, write byte `"d"` to `fds[0]`, read it from `fds[1]`, and close both fds | First `start` returns `0`; second `start` returns `-1` with `errno == EEXIST`; destroy de-registers the still-active watch without closing either caller-owned fd; post-destroy write/read transfers byte `"d"`; both socketpair fds close successfully | G1, G2 | unit |
| T8 | Stopped ready handle is not dispatched from a synthetic same I/O batch | Create two socketpairs; set all four fds `O_NONBLOCK` with `fcntl` and assert setup succeeds; register read watch A first and read watch B second; call `odin_event_loop_test_dispatch_backend_events` with synthetic read-ready entries for A followed by B, letting the hook snapshot both generations before callback A runs; callback A stops watch B; callback B only increments `state.b_calls` | The test hook returns after dispatching callback A once; callback B does not run from B's stale already-materialized entry; `state.b_calls == 0`; all four socketpair fds remain caller-owned and close successfully | G2, S1 | unit |
| T9 | Repeating timer auto-reschedules without caller reset | Create loop; start a timer with `delay_us = 0` and `repeat_us = 1000`; timer callback increments `state.timer_calls`; on the third callback it calls `odin_event_timer_stop(timer)` and `odin_event_loop_stop(loop)` | `run` returns `0`; `state.timer_calls == 3`; the callback never calls `odin_event_timer_reset`, so the second and third firings prove the loop's automatic repeat path | G3 | unit |
| T10 | Callback reset of current timer overrides automatic repeat | Create loop; start a timer with `delay_us = 0` and `repeat_us = 60000000`; first callback increments `state.timer_calls` and calls `odin_event_timer_reset(timer, 0, 0)`; second callback increments `state.timer_calls` and stops the loop | `run` returns `0`; `state.timer_calls == 2`; the second callback runs without waiting for the original 60-second repeat interval; no third callback fires before `run` returns | G3 | unit |
| T11 | Task posted by a running task executes before blocking | Create loop with no watched fd and no timer; post task A before `run`; task A appends `1` and posts task B; task B appends `2` and calls `odin_event_loop_stop(loop)` | Initial post and task A's nested post return `0`; `run` returns `0`; recorded task order is exactly `[1, 2]`; task B runs even though no fd or timer ever becomes ready | G4 | unit |
| T12 | Stopped due timer is not dispatched from same timer batch | Create loop; start zero-delay timer A first and zero-delay timer B second; timer A callback increments `state.a_calls`, calls `odin_event_timer_stop(timer_b)`, and posts a task that stops the loop; timer B callback only increments `state.b_calls` | Timer A runs once; timer B does not run; posted stop task runs; `state.a_calls == 1`; `state.b_calls == 0` | G3, G4, S1 | unit |
| T13 | Peer hangup reports `ODIN_EVENT_ERROR` | `socketpair`; set both fds `O_NONBLOCK` with `fcntl` and assert setup succeeds; start a read watch on `fds[1]` with `user_data = &state`; close peer `fds[0]` before `run`; callback records fd/mask/user pointer, stops the watch, and stops the loop | Callback runs once; observed fd is `fds[1]`; observed `user_data == &state`; observed mask includes `ODIN_EVENT_ERROR`; watched fd remains caller-owned and closes successfully in the test | G2 | unit |
| T14 | Positive sub-millisecond timer arm reaches the backend as sub-millisecond | Create loop with the `ODIN_EVENT_LOOP_TESTING` fake monotonic clock fixed at `1000000`; start a one-shot timer with `delay_us = 500`; invoke the internal backend wait-preparation recorder once without running callbacks | On Linux the recorded timerfd absolute deadline corresponds to fake now plus 500 us (`tv_sec == 1`, `tv_nsec == 500000`) with `epoll_wait` timeout `-1`; on macOS the recorded `kevent` timeout is `{0, 500000}`; in both cases the delta or relative timeout is exactly 500 us, not zero and not one millisecond, and no timer callback runs during wait preparation | G3 | unit |
| T15 | Repeating timer overflow auto-stops and leaves no repeat deadline armed | Create loop with the `ODIN_EVENT_LOOP_TESTING` fake monotonic clock fixed at `UINT64_MAX - 10`; start a timer with `delay_us = 0` and `repeat_us = 20`; on the first callback, increment `state.timer_calls`, record that no explicit stop/reset was called, and post a task that stops the loop; on any second callback, set `state.second_call = true`, call `odin_event_timer_stop(timer)`, and call `odin_event_loop_stop(loop)` to bound the failure; after `run` returns, invoke `odin_event_loop_test_live_timer_count(loop)` and the wait-preparation recorder | `run` returns `0`; `state.timer_calls == 1`; `state.second_call == false`; the posted stop task runs; the first callback made no explicit stop/reset call before the automatic repeat path; live timer count is `0`; wait preparation records no armed timer deadline (Linux timerfd disarmed, macOS no timer timeout); an implementation that wraps into an immediate redispatch reaches the bounded second-callback failure path, and an implementation that clamps or saturates to a far-future deadline fails the live-count or wait-preparation assertion | G3, G4 | unit |
| T16 | Stop cancels tasks posted after the current task snapshot | Create loop; post task A before the first `run`; task A appends `1`, posts task B, records that the nested post returned `0`, and calls `odin_event_loop_stop(loop)`; task B appends `2`; after the first `run` returns, post task C, which appends `3` and stops the loop, then call `run` again | First `run` returns `0`; after the first run the recorded order is exactly `[1]`; task B never ran despite its successful post because it was not in the snapshot active when stop was requested; second `run` returns `0`; final recorded order is exactly `[1, 3]`, proving task B was canceled before a later run could drain it | G4 | unit |
| T17 | Destroy cancels queued tasks without callback | Create loop; post a task that would increment `state.calls`; call `odin_event_loop_destroy(loop)` without calling `run` | The post returns `0`; destroy returns to the test; `state.calls == 0`, proving destroy dropped the queued task without invoking its callback | G1, G4 | unit |
| T18 | Stop does not interrupt an already-snapshotted posted task | Create loop; post task A and task B before `run`; task A appends `1` and calls `odin_event_loop_stop(loop)`; task B appends `2` | Both posts return `0`; `run` returns `0`; recorded task order is exactly `[1, 2]`, proving task B was already claimed by the active task snapshot and was not canceled by task A's stop request | G1, G4 | unit |
| T19 | Stop during active run does not interrupt a materialized I/O batch | Create two socketpairs; set all four fds `O_NONBLOCK` with `fcntl` and assert setup succeeds; register read watch A first and read watch B second; call `odin_event_loop_test_queue_backend_events` with synthetic read-ready entries for A followed by B; callback A records `A` and calls `odin_event_loop_stop(loop)`; callback B records `B`; call `odin_event_loop_run(loop)` so the queued synthetic batch is consumed by the backend-wait path while `loop.running == 1` | `run` returns `0`; recorded callback order is exactly `[A, B]`; B still runs from the same already-materialized backend batch even though A requested stop during an active run; all four socketpair fds remain caller-owned and close successfully | G1, G2 | unit |
| T20 | Stop does not interrupt an already-materialized due-timer snapshot | Create loop; start zero-delay timer A first and zero-delay timer B second; timer A callback records `A` and calls `odin_event_loop_stop(loop)`; timer B callback records `B` | `run` returns `0`; recorded timer order is exactly `[A, B]`; timer B still runs from the same due snapshot even though timer A requested stop; no later timer pass or backend wait occurs before `run` returns | G1, G3 | unit |
| T21 | I/O input masks reject zero, output-only, and unknown bits | `socketpair`; set both fds `O_NONBLOCK` with `fcntl` and assert setup succeeds; create a loop; call `odin_event_io_start` with `0`, with `ODIN_EVENT_ERROR`, with `ODIN_EVENT_READ | ODIN_EVENT_ERROR`, and with `ODIN_EVENT_WRITE | 0x80`; then start one valid read watch and call `odin_event_io_update` with `0`, with `ODIN_EVENT_ERROR`, and with `ODIN_EVENT_READ | 0x80` | Each invalid `start` returns `-1` with `errno == EINVAL` and leaves its `out` pointer unchanged; the valid read watch starts successfully; each invalid `update` returns `-1` with `errno == EINVAL` and leaves the valid watch active until the test stops it; both socketpair fds remain caller-owned and close successfully | G2 | unit |
| T22 | Destroy closes loop-owned backend descriptors | Create loop; call `odin_event_loop_test_backend_fds(loop, &fds)`; assert each non-negative descriptor in the record is valid with `fcntl(F_GETFD)`; call `odin_event_loop_destroy(loop)`; immediately call `fcntl(F_GETFD)` on the recorded backend fd and, on Linux, the recorded timerfd | The fd hook returns `0`; before destroy the macOS `kqueue` fd or Linux `epoll` fd is valid, and Linux also reports a valid timerfd; after destroy, every recorded non-negative loop-owned descriptor returns `-1` with `errno == EBADF`; no caller-owned fd is involved in this row | G1 | unit |
| T23 | kqueue partial I/O add/update failure rolls back successful subchanges | macOS-only; skip on Linux after asserting `odin_event_loop_test_fail_next_kqueue_change` returns `-1` with `errno == EOPNOTSUPP`. On macOS, create loop and nonblocking `socketpair`; arrange `odin_event_loop_test_fail_next_kqueue_change(loop, ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_ADD, ODIN_EVENT_WRITE, ENOSPC)`; call `odin_event_io_start(loop, fds[1], ODIN_EVENT_READ | ODIN_EVENT_WRITE, cb, &state, &io)`, then inspect `odin_event_loop_test_kqueue_registered_mask(loop, fds[1], &mask)`. Next start a valid read watch on `fds[1]`, arrange `odin_event_loop_test_fail_next_kqueue_change(loop, ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_DELETE, ODIN_EVENT_READ, ENOSPC)`, call `odin_event_io_update(io, ODIN_EVENT_WRITE)`, inspect the mask again, then stop the watch | The failed read/write start returns `-1` with `errno == ENOSPC`, leaves `io` unchanged, and the registered mask is `0`, proving the successful read add was rolled back before the handle was freed. The valid read start returns `0` and reports mask `ODIN_EVENT_READ`. The failed update to write returns `-1` with `errno == ENOSPC`, and the mask remains exactly `ODIN_EVENT_READ`, proving the successful write add was rolled back and the previous read registration stayed intact. After `odin_event_io_stop(io)`, the mask is `0`; both socketpair fds remain caller-owned and close successfully | G2, S2 | unit |
| T24 | Destroy releases loop-owned handle, timer, task, and loop allocations | Call `odin_event_loop_test_reset_liveness()`; call `odin_event_loop_test_liveness(&zero_before)`; call `odin_event_loop_destroy(NULL)`; call `odin_event_loop_test_liveness(&zero_after_null)`; create a loop; create a nonblocking `socketpair`; start one read watch on `fds[1]`; start one one-shot timer with `delay_us = 60000000`; post one task whose callback would increment `state.calls`; call `odin_event_loop_test_liveness(&before)`; destroy the loop without calling `run`; call `odin_event_loop_test_liveness(&after)`; close both socketpair fds | `zero_before` and `zero_after_null` are all-zero, proving `destroy(NULL)` returned without a crash or counter change; before destroy, `before.loops == 1`, `before.io_handles == 1`, `before.timers == 1`, and `before.task_nodes == 1`; after destroy, all four counters are `0`; `state.calls == 0`; both socketpair fds remain caller-owned and close successfully. An implementation that leaks an active I/O handle, timer handle, queued-task node, or loop object after destroy fails the matching counter assertion | G1, G2, G3, G4 | unit |
| T25 | Backend initialization failure preserves caller output pointer | Call `odin_event_loop_test_reset_liveness()`; set `odin_event_loop_t *out` to a non-null sentinel value; call `odin_event_loop_test_fail_next_backend_create(EMFILE)`; call `odin_event_loop_create(&out)`; then call `odin_event_loop_test_liveness(&after)` | The fault hook returns `0`; `create` returns `-1` with `errno == EMFILE`; `out` still equals the original sentinel value, proving `create` did not write `*out` on failure; `after.loops == 0`, `after.io_handles == 0`, `after.timers == 0`, and `after.task_nodes == 0`, proving partial loop-owned allocations were released | G1 | unit |
| T26 | Backend wait failure terminates active run with errno preserved | Create loop; call `odin_event_loop_test_fail_next_backend_wait(loop, EIO)`; call `odin_event_loop_run(loop)` with no posted tasks, due timers, or queued synthetic backend events; after `run` returns, post a task that stops the loop and call `run` a second time to prove `loop.running` was cleared | The fault hook returns `0`; first `run` returns `-1` with `errno == EIO`; no callback runs before that failure; the second post returns `0`; the second `run` returns `0` after dispatching the stop task once, proving the failed run did not leave the loop stuck in the running state; destroy returns normally | G1 | unit |
| T27 | Stopping an unfired future timer removes its stale heap deadline safely | Call `odin_event_loop_test_reset_liveness()`; create loop; fix fake monotonic time at `1000000`; start a one-shot timer with `delay_us = 60000000` whose callback increments `state.timer_calls`; call `odin_event_timer_stop(timer)` before `run`; advance fake time to `61000000`; invoke `odin_event_loop_test_prepare_wait(loop, &wait)` and `odin_event_loop_test_live_timer_count(loop)`; call `odin_event_loop_test_liveness(&after_prepare)`; destroy the loop and call `odin_event_loop_test_liveness(&after_destroy)` | `timer_start` returns `0`; `state.timer_calls == 0`; wait preparation returns `0` and records no armed timer deadline; live timer count is `0`; `after_prepare.loops == 1`, `after_prepare.timers == 0`, `after_prepare.io_handles == 0`, and `after_prepare.task_nodes == 0`, proving the stopped timer storage was released after the stale heap entry was discarded; after destroy all liveness counters are `0`. Under the P2 ASan command, this row also exits without an AddressSanitizer use-after-free report when wait preparation inspects and discards the stale heap entry | G3 | unit |

T25 and T26 cover the syscall-failure return paths from §3.2.1's Mechanism through §3.2.5 fault injection rather than relying on nondeterministic real `kqueue()`/`epoll_create1()` or backend-wait failures. T25 proves `create` returns `-1`, preserves the injected `errno`, leaves `*out` unmodified, and releases partial loop-owned allocations when backend initialization fails. T26 proves a non-`EINTR` backend wait failure occurs during an active `run`, returns `-1` with `errno` preserved, and clears `loop.running` so a later `run` can execute normally. T27 isolates the stopped-unfired-timer heap-pin path from §3.2.3 and proves stale heap cleanup does not leave an armed deadline, callback, leak, or ASan-detectable use-after-free. G1's tested surface is covered by T1, T7, T17-T20, T22, and T24-T26, with T24 providing deterministic memory-release evidence for loop-owned I/O handles, timers, queued-task nodes, and the loop object.

Separately, every row in this table is executed only on the macOS `kqueue` build. The Odin unit-test suite can currently run only on macOS; the Linux `epoll`+timerfd test binary is verified by cross-compilation rather than execution (see §7). The red→green transition each `T#` requires therefore runs against the `kqueue` backend, while the Linux-`epoll` behavior these rows describe — the Linux assertion branches of T14 and T22, and the `EOPNOTSUPP` skip path of T23 — is compiled into the cross-built binary but not run, a disclosed run-coverage gap on the `epoll` backend rather than a silent omission. The rows branch on the backend the §3.2.5 hooks report, so the identical suite exercises the `epoll` backend red→green with no new test code once an Odin Linux test runner exists.

## 7. Implementation Plan

- **P1. Land the test-only event-loop surface with red-verifiable `T1`-`T27`.**
  - **Scope:** add `odin/event_loop.h` with the §3.2.1 through §3.2.4 declarations and doc-comments that pin the lifecycle, I/O watch, timer, posted-task, and owner-thread contracts, but wrap those declarations in a temporary `ODIN_EVENT_LOOP_TESTING` staging guard for P1 so non-test translation units see no event-loop API; add `odin/event_loop_internal_test.h` with the §3.2.5 declarations gated by `ODIN_EVENT_LOOP_TESTING`; add `odin/event_loop.c` with a bounded in-memory red implementation that creates a loop object, opens loop-owned backend descriptor stand-ins observable through `odin_event_loop_test_backend_fds`, implements the backend create/wait fault hooks and queued synthetic backend batch hook, and allocates I/O handles, timers, and task nodes with `ODIN_EVENT_LOOP_TESTING` liveness counters so every test setup reaches its behavior assertions, but intentionally omits real kqueue/epoll registration and real blocking backend wait behavior. Its `run` is bounded and always returns rather than performing a real blocking backend wait, so every red mode terminates well within the fixture's 2-second deadline and fails through an assertion, never through `EventLoopRunDeadline`. The red implementation has a row-selected fault mode keyed by `ODIN_EVENT_LOOP_RED_CASE=T#`: outside the selected row it runs only enough setup-compatible behavior to avoid hiding the selected failure; inside the selected row it executes the row's trigger surface and omits exactly the rule that row proves, leaving every other row's rule intact. The per-row red modes are listed at the end of this Scope. In `odin/BUILD.gn`, split the current non-event-loop source list into `source_set("odin_core")`; keep the existing public `source_set("odin")` depending only on `:odin_core`; do not add or depend on a production `:odin_event_loop` target in this phase; add `config("odin_event_loop_testing_config") { defines = [ "ODIN_EVENT_LOOP_TESTING" ] }`; add `testonly = true` `source_set("odin_event_loop_testing")` compiling `event_loop.c` with `configs += [ ":odin_event_loop_testing_config" ]` so the red implementation and hook definitions are linkable only by tests; update `executable("odin_unittests")` to depend on `:odin_core` and `:odin_event_loop_testing` instead of `:odin`, keep its existing data deps and Linux `_GNU_SOURCE` define, add `configs += [ ":odin_event_loop_testing_config" ]` so `event_loop_unittests.cpp` sees the staged event-loop API and internal hook prototypes, and add `event_loop_unittests.cpp` containing T1-T27 as separate `TEST(OdinEventLoopTest, T#)` cases named exactly `T1` through `T27` plus the `EventLoopRunDeadline` fixture. No production target in P1 sees the declarations, links `event_loop.c`, or defines `odin_event_loop_*`, `odin_event_io_*`, `odin_event_timer_*`, or `odin_event_post`, so the S1 stale-dispatch trigger surface, the S2 real kqueue-registration failure surface, and the stopped-unfired-timer stale-heap trigger surface are unavailable to production callers while T8, T12, T23, and T27 are intentionally red. Each non-platform-skipped T row skips unless `ODIN_EVENT_LOOP_RED_CASE` names that row. The per-row red modes, each omitting exactly the rule its §6 row proves, are:
    - **T1** — runs the posted stop task but returns the wrong `run` status.
    - **T2** — dispatches only the first level-triggered read, not the second.
    - **T3** — dispatches `WRITE` and accepts `odin_event_io_update`, but never reports the new `READ`.
    - **T4** — runs the one-shot callback, then leaves the timer live or misses the posted stop task.
    - **T5** — drains the reset task and returns `0` from `odin_event_timer_reset`, but leaves the original deadline in effect.
    - **T6** — drains both posted tasks out of FIFO order.
    - **T7** — accepts the duplicate-fd watch instead of returning `EEXIST`.
    - **T8** — runs the synthetic I/O batch but dispatches the stale stopped-B entry.
    - **T9** — fires the repeating timer only once.
    - **T10** — runs the reset callback but lets the old repeat interval suppress the immediate second firing.
    - **T11** — runs task A and accepts nested task B, then fails to drain the nested post before a blocking wait.
    - **T12** — materializes the due timers but still dispatches stopped timer B.
    - **T13** — observes the peer hangup without setting `ODIN_EVENT_ERROR`.
    - **T14** — records a rounded or missing sub-millisecond deadline.
    - **T15** — wraps or clamps the overflowing repeat instead of auto-stopping.
    - **T16** — runs task A and accepts task B after `stop`, then leaves B queued for the later run instead of canceling it.
    - **T17** — invokes the queued task during `destroy` instead of dropping it without callback.
    - **T18** — runs task A from the active snapshot, observes `stop`, and incorrectly cancels already-snapshotted task B.
    - **T19** — consumes the queued synthetic backend batch during `run`, observes callback A's `stop`, and incorrectly exits before dispatching already-materialized callback B.
    - **T20** — materializes the due timers but applies the wrong dispatch order under `stop`.
    - **T21** — accepts invalid input masks, including `0`, output-only `ODIN_EVENT_ERROR`, and unknown bits.
    - **T22** — leaves the loop-owned descriptor stand-ins open after `destroy`.
    - **T23** — (macOS) records the failed kqueue add/update masks without rolling back the successful subchanges.
    - **T24** — leaves at least one loop-owned liveness counter nonzero after `destroy`.
    - **T25** — injects backend-create failure but still overwrites `*out`, loses the injected `errno`, or leaks a partially allocated loop object.
    - **T26** — injects backend-wait failure while `run` is active but returns `0`, loses the injected `errno`, or leaves the loop stuck in the running state.
    - **T27** — stops an unfired future timer, then wait preparation still arms the stale deadline, leaks the retired timer handle, or inspects storage that ASan reports as freed in P2.
  - **Depends on:** None.
  - **Done when:** `./tool/gn gen out/event_loop_mac --args='target_os="mac"'`, `./tool/gn gen out/event_loop_linux_x64 --args='target_os="linux" target_cpu="x64"'`, `./tool/ninja -C out/event_loop_mac odin_main odin_unittests tests`, and `./tool/ninja -C out/event_loop_linux_x64 odin_main odin_unittests tests` all succeed. Running the macOS red matrix, `for t in T1 T2 T3 T4 T5 T6 T7 T8 T9 T10 T11 T12 T13 T14 T15 T16 T17 T18 T19 T20 T21 T22 T23 T24 T25 T26 T27; do ODIN_EVENT_LOOP_RED_CASE=$t out/event_loop_mac/odin_unittests --gtest_filter="OdinEventLoopTest.$t"; done`, produces exactly one failing selected row per invocation. For a forked row (T1-T6, T9-T13, T15, T16, T18-T20, and T26) the failure surfaces as the parent's `WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0` assertion failing after the child's red-mode assertion fails and the child `_exit`s nonzero; for a non-forked row it surfaces as a direct parent-process assertion. The red signal never comes through the `EventLoopRunDeadline` deadline, because P1's bounded `run` always returns inside the 2-second window — that deadline exists to catch a hang in P2's real blocking loop, not to flag a red row. The `out/event_loop_linux_x64/odin_unittests` binary is produced by cross-compilation only and is not executed in this RFC — the Odin project can currently run unit tests solely on macOS, so per this RFC's scope the Linux `epoll`+timerfd backend is verified by successful cross-compilation, not by a red matrix. Every `T#`'s red→green evidence therefore comes from the macOS `kqueue` run above; the Linux-only assertion branches in T14 (timerfd/`epoll_wait` record) and T22 (`epoll` fd plus timerfd `EBADF`) and T23's Linux `EOPNOTSUPP` skip path are compiled into that binary but not run. The red output identifies these row-specific failures: T1 post-driven run return, T2 read callback count, T3 write-to-read sequence, T4 one-shot live count or stop-task dispatch, T5 reset task executed but timer count remains zero because the new deadline is ignored, T6 FIFO task order, T7 duplicate-fd `EEXIST`, T8 stopped B callback count, T9 repeat count, T10 current-callback reset executed but reset-over-repeat count remains one, T11 task A executed and nested task B was accepted but nested-post order remains `[1]`, T12 stopped timer B callback count, T13 hangup/error callback count, T14 platform wait record, T15 overflow live count or armed deadline, T16 task A executed and task B was accepted after `stop` but appears in the later run instead of being canceled, T17 destroy callback count, T18 task A executed and already-snapshotted task B was canceled instead of drained, T19 active-run I/O batch drained after stop, T20 due-timer batch order, T21 zero/output-only/unknown mask `EINVAL`, T22 backend fd `EBADF` after destroy, macOS T23 kqueue rollback mask after partial add/update failure, T24 post-destroy liveness counters, T25 backend-create failure cleanup/out-pointer preservation, T26 backend-wait failure return/errno/running-state preservation, and T27 stopped-unfired-timer stale deadline, liveness, or ASan failure. On macOS the default `out/event_loop_mac/odin_unittests --gtest_filter='OdinEventLoopTest.T*'` reports T1-T27 skipped and exits zero, and the unfiltered default macOS run reports T1-T27 skipped while every pre-existing Odin test suite still passes; the Linux binary built above is cross-compiled but not run, so its default-suite behavior is not asserted in this RFC. The public `:odin` GN label still has no dependency on `event_loop.c` or `:odin_event_loop_testing`, non-test translation units see no event-loop declarations from `odin/event_loop.h`, and only `odin_unittests` resolves the staged event-loop symbols and `odin_event_loop_test_*` hook symbols.

- **P2. Implement and expose the kqueue/epoll loop and turn `T1`-`T27` green.**
  - **Scope:** remove the temporary P1 `ODIN_EVENT_LOOP_TESTING` staging guard around the §3.2.1 through §3.2.4 declarations in `odin/event_loop.h`; replace the red implementation in `odin/event_loop.c` with the Linux `epoll` plus timerfd backend, macOS `kqueue` backend, owner-thread capture/assertion helper, loop-owned I/O handle table, monotonic timer heap with per-entry timer heap pins, owner-thread posted-task FIFO, inactive-handle/deferred-free dispatch guard keyed by `snapshot_depth`, exact input-mask validation, `errno`-preserving cleanup paths, macOS `EV_RECEIPT` add/update rollback for read/write filter changes, and the `ODIN_EVENT_LOOP_TESTING`-gated fake clock, synthetic dispatch, queued synthetic backend batches, live-timer-count, allocation liveness counters, wait-preparation, backend-fd snapshot, backend create/wait fault injection, kqueue receipt-failure injection, and kqueue registered-mask hooks pinned in §3.2.5. In `odin/BUILD.gn`, add production `source_set("odin_event_loop")` compiling `event_loop.c` and `event_loop.h` without `ODIN_EVENT_LOOP_TESTING`, make the existing public `source_set("odin")` depend on `:odin_core` and `:odin_event_loop`, and keep `:odin_event_loop_testing` as the only target that compiles hook definitions. This is the first phase where production callers can include and link the event-loop API, and the same phase implements the S1 inactive/generation/deferred-free guard, the S2 kqueue `EV_RECEIPT` rollback, and the stopped-unfired-timer heap-pin lifetime rule before the tests are un-gated. Remove the `ODIN_EVENT_LOOP_RED_CASE` skip gate from T1-T27 so they assert in every default local (macOS) test run; T23 keeps a compile-time platform guard — kqueue-rollback assertions compiled for macOS, the `EOPNOTSUPP` hook contract compiled for Linux — so the cross-built Linux binary still compiles cleanly even though it is not executed.
  - **Depends on:** P1.
  - **Done when:** The macOS build commands from P1 still succeed and `out/event_loop_mac/odin_unittests --gtest_filter='OdinEventLoopTest.T*'` passes T1-T27 un-gated natively, including T8 and T12's S1 stale-dispatch assertions, T19's active-run stop-during-I/O-batch assertion, T23's S2 injected kqueue partial add/update rollback assertions, T24's post-destroy liveness-counter assertions, T25/T26's backend create/wait fault-injection assertions, and T27's stopped-unfired-timer stale-heap cleanup assertions; the Linux build commands from P1 still succeed, so `out/event_loop_linux_x64/odin_unittests` cross-compiles with the real `epoll`+timerfd backend linked in; that binary is not executed in this RFC (Odin unit tests currently run only on macOS), so the Linux `epoll` behavior — including T14's timerfd/`epoll_wait` record branch, T22's `epoll`-fd-plus-timerfd `EBADF` branch, and T23's `EOPNOTSUPP` skip path — is verified by successful cross-compilation rather than a green matrix run. The unfiltered default macOS run `out/event_loop_mac/odin_unittests --gtest_brief=1` executes T1-T27 un-gated and exits zero with every pre-existing Odin test suite still green. `./tool/gn gen out/event_loop_mac_asan --args='target_os="mac" is_asan=true'`, `./tool/ninja -C out/event_loop_mac_asan odin_unittests`, and `out/event_loop_mac_asan/odin_unittests --gtest_filter='OdinEventLoopTest.T*'` exit without AddressSanitizer reports, giving use-after-free and heap-overflow coverage of the deferred-free dispatch guard behind S1 and S2 plus T27's stale timer heap-entry inspection after `odin_event_timer_stop`. LeakSanitizer-based allocation-leak detection remains outside this RFC because LSan is unsupported on Darwin and the cross-compiled Linux ASan binary cannot be executed here, but T24, T25, and T27 supply deterministic executable memory-release coverage through `ODIN_EVENT_LOOP_TESTING` counters for loop-owned I/O handles, timer handles, queued-task nodes, and loop objects returning to zero after `destroy`, failed backend initialization, and stale stopped-timer heap cleanup. T22 separately proves loop-owned backend descriptor cleanup on macOS by recording the `kqueue` fd before destroy and observing `fcntl(F_GETFD) == -1` with `errno == EBADF` after destroy (its Linux `epoll`-fd-plus-timerfd branch is compiled into the cross-built binary but not run); T23 separately proves macOS partial-registration cleanup by injecting receipt failures and observing no stale registered mask after failed add and the previous mask after failed update. Production `out/event_loop_mac/odin` and `out/event_loop_linux_x64/odin` link the public event-loop symbols through `:odin` and contain no `odin_event_loop_test_*` symbols while both `odin_unittests` binaries link those hook symbols from `:odin_event_loop_testing`; `./tidy_odin.sh` exits clean on `odin/event_loop.c`, `odin/event_loop.h`, `odin/event_loop_internal_test.h`, and `odin/event_loop_unittests.cpp`.
