/* odin/event_loop.h
 *
 * Single-owner-thread event-loop API.
 *
 * All public APIs that receive a live loop or handle are owner-thread APIs in
 * debug builds. int-returning APIs return 0 on success and -1 with errno set on
 * failure. odin_event_loop_destroy(NULL) is a no-op; all other pointer
 * parameters are non-null preconditions.
 */

#ifndef ODIN_EVENT_LOOP_H_
#define ODIN_EVENT_LOOP_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct odin_event_loop_t odin_event_loop_t;

/* Creates a loop with initialized loop-owned backend descriptors and records
 * the calling thread as owner. On failure, *out is not modified.
 */
int odin_event_loop_create(odin_event_loop_t **out);

/* Runs callbacks on the owner thread until stop is requested after the current
 * materialized callback batch drains, or until a backend error occurs. Nested
 * run returns -1 with errno=EALREADY.
 */
int odin_event_loop_run(odin_event_loop_t *loop);

/* Requests that an active run exit after the current snapshot batch boundary.
 */
void odin_event_loop_stop(odin_event_loop_t *loop);

/* Releases loop-owned backend descriptors, I/O handles, timers, and queued
 * tasks without closing caller-owned watched file descriptors.
 */
void odin_event_loop_destroy(odin_event_loop_t *loop);

#define ODIN_EVENT_READ 0x01u
#define ODIN_EVENT_WRITE 0x02u
#define ODIN_EVENT_ERROR 0x04u

typedef struct odin_event_io_t odin_event_io_t;

typedef void (*odin_event_io_cb)(odin_event_loop_t *loop, odin_event_io_t *io,
                                 int fd, unsigned int events, void *user_data);

/* Starts one active level-triggered watch for caller-owned nonblocking fd.
 * Input masks are exactly a non-empty subset of READ|WRITE; ERROR is
 * output-only. Duplicate active fd watches fail with errno=EEXIST.
 */
int odin_event_io_start(odin_event_loop_t *loop, int fd, unsigned int events,
                        odin_event_io_cb cb, void *user_data,
                        odin_event_io_t **out);

/* Updates an active watch to a new non-empty READ|WRITE input mask. */
int odin_event_io_update(odin_event_io_t *io, unsigned int events);

/* Stops an active watch, preserves incoming errno, de-registers backend
 * filters, and defers handle storage release across active dispatch snapshots.
 */
void odin_event_io_stop(odin_event_io_t *io);

typedef struct odin_event_timer_t odin_event_timer_t;

typedef void (*odin_event_timer_cb)(odin_event_loop_t *loop,
                                    odin_event_timer_t *timer, void *user_data);

/* Starts a monotonic microsecond timer. delay_us==0 is due on the next timer
 * pass; repeat_us==0 is one-shot; positive repeat_us auto-reschedules from
 * monotonic time observed after each callback.
 */
int odin_event_timer_start(odin_event_loop_t *loop, uint64_t delay_us,
                           uint64_t repeat_us, odin_event_timer_cb cb,
                           void *user_data, odin_event_timer_t **out);

/* Replaces the next deadline and repeat interval for an active timer. */
int odin_event_timer_reset(odin_event_timer_t *timer, uint64_t delay_us,
                           uint64_t repeat_us);

/* Stops an active timer, preserves incoming errno, and keeps storage alive
 * until stale heap pins and dispatch snapshots can no longer reference it.
 */
void odin_event_timer_stop(odin_event_timer_t *timer);

typedef void (*odin_event_task_cb)(odin_event_loop_t *loop, void *user_data);

/* Appends a same-owner-thread task to the FIFO. A posted task is dispatched at
 * most once or canceled without callback on stop/destroy if it is still outside
 * the active task snapshot.
 */
int odin_event_post(odin_event_loop_t *loop, odin_event_task_cb cb,
                    void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* ODIN_EVENT_LOOP_H_ */
