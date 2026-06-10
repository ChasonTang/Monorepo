/* odin/dial.c -- RFC-012 nonblocking socket dial.
 *
 * Single-thread, event-loop-driven: creates one nonblocking SOCK_STREAM socket
 * for an already-resolved address, issues connect(2), and resolves the attempt
 * from the loop -- a WRITE watch plus getsockopt(SO_ERROR) for an in-flight
 * connect, or a 0-delay one-shot timer carrying the captured errno for one that
 * fails immediately. Completion funnels through complete(), which stops the
 * single active registration and fires on_done exactly once as its final
 * action, transferring the connected fd to the caller on success or closing it
 * on failure. The dial performs no name resolution and selects no transport.
 */

#include "odin/dial.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(ODIN_DIAL_TESTING)
#include "odin/dial_internal_test.h"
#endif

/* Internal state (§3.2.2). Exactly one of {io, timer} is registered at a time;
 * fd is the owned socket, set to -1 the instant ownership leaves (handed to the
 * caller on OK, or closed on ERROR / abort). */
struct odin_dial_t {
  odin_dial_cb        on_done;
  void               *user_data;
  int                 fd;           /* owned socket; -1 once ownership leaves   */
  int                 pending_err;  /* errno to deliver on the deferred path    */
  odin_event_io_t    *io;           /* WRITE watch while connecting; else NULL  */
  odin_event_timer_t *timer;        /* deferred-error timer; else NULL          */
};

/* Makes fd nonblocking. Returns 0, or -1 with errno set. */
static int set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* getsockopt(SO_ERROR): the authoritative connect result -- 0 on success, the
 * connection's failing errno otherwise, or the errno from a failing getsockopt
 * (itself treated as a connection failure). */
static int so_error(int fd) {
  int err = 0;
  socklen_t len = sizeof(err);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
    return errno;
  }
  return err;
}

/* The single completion step: stop whichever registration is active, then
 * transfer the fd (err == 0) or close it (err != 0), then fire on_done as the
 * last statement -- no dial field is touched after the callback returns, so
 * odin_dial_destroy(d) from inside on_done is safe. */
static void complete(odin_dial_t *d, int err) {
  if (d->io != NULL) {
    odin_event_io_stop(d->io);
    d->io = NULL;
  }
  if (d->timer != NULL) {
    odin_event_timer_stop(d->timer);
    d->timer = NULL;
  }
  const odin_dial_cb cb = d->on_done;
  void *const ud = d->user_data;
  if (err == 0) {
    const int out_fd = d->fd;
    d->fd = -1; /* transfer ownership to caller */
    cb(d, ODIN_DIAL_OK, out_fd, 0, ud);
  } else {
    close(d->fd);
    d->fd = -1; /* dial owned it; close on failure */
    cb(d, ODIN_DIAL_ERROR, -1, err, ud);
  }
}

/* WRITE-watch readiness: the socket is writable only once connect(2) resolved,
 * so SO_ERROR is the authoritative outcome. */
static void on_writable(odin_event_loop_t *loop, odin_event_io_t *io, int fd,
                        unsigned int events, void *user_data) {
  (void)loop;
  (void)io;
  (void)events;
  odin_dial_t *d = (odin_dial_t *)user_data;
  complete(d, so_error(fd));
}

/* Deferred delivery of an immediate connect(2) failure on the next loop turn. */
static void on_deferred_error(odin_event_loop_t *loop, odin_event_timer_t *timer,
                              void *user_data) {
  (void)loop;
  (void)timer;
  odin_dial_t *d = (odin_dial_t *)user_data;
  complete(d, d->pending_err);
}

int odin_dial_start(odin_event_loop_t *loop, const struct sockaddr *addr,
                    socklen_t addrlen, odin_dial_cb on_done, void *user_data,
                    odin_dial_t **out) {
  odin_dial_t *d = (odin_dial_t *)calloc(1, sizeof(*d));
  if (d == NULL) {
    errno = ENOMEM;
    return -1;
  }
  d->on_done = on_done;
  d->user_data = user_data;
  d->pending_err = 0;
  d->fd = socket(addr->sa_family, SOCK_STREAM, 0);
  if (d->fd < 0) {
    const int saved = errno;
    free(d);
    errno = saved;
    return -1;
  }
  if (set_nonblocking(d->fd) != 0) {
    const int saved = errno;
    close(d->fd);
    free(d);
    errno = saved;
    return -1;
  }

  const int r = connect(d->fd, addr, addrlen);
  if (r == 0 || (r < 0 && (errno == EINPROGRESS || errno == EINTR))) {
    if (odin_event_io_start(loop, d->fd, ODIN_EVENT_WRITE, on_writable, d,
                            &d->io) != 0) {
      const int saved = errno;
      close(d->fd);
      free(d);
      errno = saved;
      return -1;
    }
  } else {
    d->pending_err = errno;
    if (odin_event_timer_start(loop, 0, 0, on_deferred_error, d, &d->timer) !=
        0) {
      const int saved = errno;
      close(d->fd);
      free(d);
      errno = saved;
      return -1;
    }
  }

  *out = d;
  return 0;
}

void odin_dial_destroy(odin_dial_t *dial) {
  if (dial == NULL) {
    return;
  }
  if (dial->io != NULL) {
    odin_event_io_stop(dial->io);
    dial->io = NULL;
  }
  if (dial->timer != NULL) {
    odin_event_timer_stop(dial->timer);
    dial->timer = NULL;
  }
  if (dial->fd >= 0) {
    close(dial->fd); /* still owned: in-flight or aborted */
    dial->fd = -1;
  }
  free(dial);
}

#if defined(ODIN_DIAL_TESTING)
int odin_dial_test_fd(odin_dial_t *dial) {
  if (dial->fd < 0) {
    errno = ENOENT;
    return -1;
  }
  return dial->fd;
}
#endif
