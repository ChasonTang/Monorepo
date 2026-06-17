/* odin/accept_loop.c -- RFC-019 event-loop-driven TCP accept listener.
 *
 * Registers one level-triggered ODIN_EVENT_READ watch on a caller-owned
 * nonblocking listening fd through the RFC-010 odin_event_io_* API. On each
 * readiness, drains pending connections by calling accept4(SOCK_NONBLOCK) on
 * Linux or accept(2) + fcntl(F_SETFL, O_NONBLOCK) on macOS up to
 * ODIN_ACCEPT_LOOP_BATCH_MAX per readiness; fires on_accept once per accepted
 * nonblocking conn_fd with caller ownership beginning at the call. Soft-
 * degrades on EMFILE / ENFILE / ENOBUFS / ENOMEM by stopping the watch and
 * arming a 0-delay one-shot timer that re-arms the watch on the next loop
 * pass. Routes any other errno to a single on_error and leaves the module in
 * a terminal state without auto-destroy. odin_accept_loop_destroy stops the
 * watch and any pending timer, never closes the listening fd, and is safe
 * from inside on_accept or on_error via a deferred-destroy flag.
 */

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* accept4(2) */
#endif

#include "odin/accept_loop.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(ODIN_ACCEPT_LOOP_TESTING)
#include "odin/testing/accept_loop_internal_test.h"
#endif

struct odin_accept_loop_t {
  odin_event_loop_t *loop;
  int listen_fd;
  odin_accept_loop_accept_cb on_accept;
  odin_accept_loop_error_cb on_error;
  void *user_data;
  odin_event_io_t *io;
  odin_event_timer_t *timer;
  int in_callback;
  int destroy_requested;
  int terminal;
#if defined(ODIN_ACCEPT_LOOP_TESTING)
  int fail_next_accept_err;
#if !defined(__linux__)
  int fail_next_fcntl_which;
  int fail_next_fcntl_err;
#endif
#endif
};

static void on_readable(odin_event_loop_t *loop, odin_event_io_t *io, int fd,
                        unsigned int events, void *user_data);
static void on_resume_timer(odin_event_loop_t *loop, odin_event_timer_t *timer,
                            void *user_data);
static void enter_soft_degradation(odin_accept_loop_t *al);
static void enter_terminal_error(odin_accept_loop_t *al, int err);
static void finish_destroy(odin_accept_loop_t *al);

/* Returns 0 with *out_conn_fd set on success, -1 with *out_err set on failure.
 * The accepted fd is nonblocking on either platform. On macOS, a fcntl
 * failure after a successful accept(2) closes the half-configured fd and
 * surfaces the fcntl errno through the same classification arms as if
 * accept(2) itself had returned it (per the §3.2.2 atomic-nonblocking-accept
 * contract). */
static int accept_one(odin_accept_loop_t *al, int *out_conn_fd, int *out_err) {
#if defined(ODIN_ACCEPT_LOOP_TESTING)
  if (al->fail_next_accept_err != 0) {
    *out_err = al->fail_next_accept_err;
    al->fail_next_accept_err = 0;
    return -1;
  }
#endif
#if defined(__linux__)
  const int r = accept4(al->listen_fd, NULL, NULL, SOCK_NONBLOCK);
  if (r >= 0) {
    *out_conn_fd = r;
    return 0;
  }
  *out_err = errno;
  return -1;
#else
  const int r = accept(al->listen_fd, NULL, NULL);
  if (r < 0) {
    *out_err = errno;
    return -1;
  }
#if defined(ODIN_ACCEPT_LOOP_TESTING)
  if (al->fail_next_fcntl_which == F_GETFL) {
    const int saved = al->fail_next_fcntl_err;
    al->fail_next_fcntl_which = 0;
    al->fail_next_fcntl_err = 0;
    close(r);
    *out_err = saved;
    return -1;
  }
#endif
  const int flags = fcntl(r, F_GETFL, 0);
  if (flags == -1) {
    const int saved = errno;
    close(r);
    *out_err = saved;
    return -1;
  }
#if defined(ODIN_ACCEPT_LOOP_TESTING)
  if (al->fail_next_fcntl_which == F_SETFL) {
    const int saved = al->fail_next_fcntl_err;
    al->fail_next_fcntl_which = 0;
    al->fail_next_fcntl_err = 0;
    close(r);
    *out_err = saved;
    return -1;
  }
#endif
  if (fcntl(r, F_SETFL, flags | O_NONBLOCK) == -1) {
    const int saved = errno;
    close(r);
    *out_err = saved;
    return -1;
  }
  *out_conn_fd = r;
  return 0;
#endif
}

static void on_readable(odin_event_loop_t *loop, odin_event_io_t *io, int fd,
                        unsigned int events, void *user_data) {
  (void)loop;
  (void)io;
  (void)fd;
  (void)events;
  odin_accept_loop_t *al = (odin_accept_loop_t *)user_data;
  if (al->terminal) {
    return; /* defensive: watch is already stopped here */
  }
  for (unsigned int i = 0; i < ODIN_ACCEPT_LOOP_BATCH_MAX; ++i) {
    int conn_fd = -1;
    int err = 0;
    if (accept_one(al, &conn_fd, &err) == 0) {
      const odin_accept_loop_accept_cb cb = al->on_accept;
      void *const ud = al->user_data;
      al->in_callback = 1;
      cb(al, conn_fd, ud);
      if (al->destroy_requested) {
        finish_destroy(al);
        return;
      }
      al->in_callback = 0;
      continue;
    }
    if (err == EAGAIN || err == EWOULDBLOCK) {
      return;
    }
    if (err == EINTR || err == ECONNABORTED) {
      continue;
    }
    if (err == EMFILE || err == ENFILE || err == ENOBUFS || err == ENOMEM) {
      enter_soft_degradation(al);
      return;
    }
    enter_terminal_error(al, err);
    return;
  }
}

static void enter_soft_degradation(odin_accept_loop_t *al) {
  if (al->io != NULL) {
    odin_event_io_stop(al->io);
    al->io = NULL;
  }
  if (odin_event_timer_start(al->loop, 0, 0, on_resume_timer, al, &al->timer) !=
      0) {
    const int err = errno;
    enter_terminal_error(al, err);
    return;
  }
}

static void on_resume_timer(odin_event_loop_t *loop, odin_event_timer_t *timer,
                            void *user_data) {
  (void)loop;
  (void)timer;
  odin_accept_loop_t *al = (odin_accept_loop_t *)user_data;
  if (al->timer != NULL) {
    odin_event_timer_stop(al->timer);
    al->timer = NULL;
  }
  if (odin_event_io_start(al->loop, al->listen_fd, ODIN_EVENT_READ, on_readable,
                          al, &al->io) != 0) {
    const int err = errno;
    enter_terminal_error(al, err);
    return;
  }
}

static void enter_terminal_error(odin_accept_loop_t *al, int err) {
  if (al->io != NULL) {
    odin_event_io_stop(al->io);
    al->io = NULL;
  }
  if (al->timer != NULL) {
    odin_event_timer_stop(al->timer);
    al->timer = NULL;
  }
  al->terminal = 1;
  const odin_accept_loop_error_cb cb = al->on_error;
  void *const ud = al->user_data;
  al->in_callback = 1;
  cb(al, err, ud);
  if (al->destroy_requested) {
    finish_destroy(al);
    return;
  }
  al->in_callback = 0;
}

static void finish_destroy(odin_accept_loop_t *al) {
  if (al->io != NULL) {
    odin_event_io_stop(al->io);
    al->io = NULL;
  }
  if (al->timer != NULL) {
    odin_event_timer_stop(al->timer);
    al->timer = NULL;
  }
  free(al);
}

int odin_accept_loop_create(odin_event_loop_t *loop, int listen_fd,
                            odin_accept_loop_accept_cb on_accept,
                            odin_accept_loop_error_cb on_error, void *user_data,
                            odin_accept_loop_t **out) {
  odin_accept_loop_t *al = (odin_accept_loop_t *)calloc(1, sizeof(*al));
  if (al == NULL) {
    errno = ENOMEM;
    return -1;
  }
  al->loop = loop;
  al->listen_fd = listen_fd;
  al->on_accept = on_accept;
  al->on_error = on_error;
  al->user_data = user_data;
  if (odin_event_io_start(loop, listen_fd, ODIN_EVENT_READ, on_readable, al,
                          &al->io) != 0) {
    const int saved = errno;
    free(al);
    errno = saved;
    return -1;
  }
  *out = al;
  return 0;
}

void odin_accept_loop_destroy(odin_accept_loop_t *al) {
  if (al == NULL) {
    return;
  }
  if (al->in_callback) {
    al->destroy_requested = 1;
    return;
  }
  finish_destroy(al);
}

#if defined(ODIN_ACCEPT_LOOP_TESTING)
int odin_accept_loop_test_fail_next_accept(odin_accept_loop_t *al, int errnum) {
  if (al == NULL || errnum <= 0) {
    errno = EINVAL;
    return -1;
  }
  al->fail_next_accept_err = errnum;
  return 0;
}

#if !defined(__linux__)
int odin_accept_loop_test_fail_next_fcntl(odin_accept_loop_t *al, int which,
                                          int errnum) {
  if (al == NULL || (which != F_GETFL && which != F_SETFL) || errnum <= 0) {
    errno = EINVAL;
    return -1;
  }
  al->fail_next_fcntl_which = which;
  al->fail_next_fcntl_err = errnum;
  return 0;
}
#endif

int odin_accept_loop_test_is_paused(const odin_accept_loop_t *al) {
  return (al->io == NULL && al->timer != NULL && !al->terminal) ? 1 : 0;
}

int odin_accept_loop_test_is_terminal(const odin_accept_loop_t *al) {
  return al->terminal != 0 ? 1 : 0;
}
#endif
