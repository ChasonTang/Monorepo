/* odin/event_loop_internal_test.h */

#ifndef ODIN_EVENT_LOOP_INTERNAL_TEST_H_
#define ODIN_EVENT_LOOP_INTERNAL_TEST_H_

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
int odin_event_loop_test_fail_next_backend_wait(odin_event_loop_t *loop,
                                                int errnum);
int odin_event_loop_test_fail_next_timer_start(odin_event_loop_t *loop,
                                               int errnum);
int odin_event_loop_test_prepare_wait(odin_event_loop_t *loop,
                                      odin_event_loop_test_wait_record_t *out);
int odin_event_loop_test_backend_fds(odin_event_loop_t *loop,
                                     odin_event_loop_test_fd_record_t *out);
int odin_event_loop_test_fail_next_kqueue_change(odin_event_loop_t *loop,
                                                 int change, unsigned int event,
                                                 int errnum);
int odin_event_loop_test_kqueue_registered_mask(odin_event_loop_t *loop, int fd,
                                                unsigned int *out_events);
int odin_event_loop_test_dispatch_backend_events(
    odin_event_loop_t *loop, const odin_event_loop_test_ready_t *entries,
    size_t count);
int odin_event_loop_test_queue_backend_events(
    odin_event_loop_t *loop, const odin_event_loop_test_ready_t *entries,
    size_t count);

#ifdef __cplusplus
}
#endif

#endif /* defined(ODIN_EVENT_LOOP_TESTING) */

#endif /* ODIN_EVENT_LOOP_INTERNAL_TEST_H_ */
