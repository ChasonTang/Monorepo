/* odin/transport_fd.c -- RFC-013 fd transport implementation.
 *
 * Backs the odin_transport_t vtable with read(2)/write(2)/shutdown(SHUT_WR)/
 * getsockopt(SO_ERROR) and the RFC-010 odin_event_io_* watch lifecycle over a
 * caller-owned nonblocking connected stream socket, preserving the RFC-011
 * relay byte/EOF/EAGAIN/error classification. It never closes fd: neither
 * create, the vtable destroy, nor any op closes it.
 */

#include "odin/transport_fd.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(ODIN_TRANSPORT_FD_TESTING)
#include "odin/testing/transport_fd_internal_test.h"
#endif

/* Internal state (§3.2.2). base is first so the cast in every slot is valid; fd
 * is not owned; io is the active watch (or NULL); cur is the current
 * ODIN_EVENT_* mask. */
typedef struct odin_fd_transport_t {
  odin_transport_t base;
  odin_event_loop_t *loop;
  int fd;
  odin_event_io_t *io;
  unsigned int cur;
  odin_transport_ready_cb on_ready;
  void *user_data;
} odin_fd_transport_t;

static void fd_on_io(odin_event_loop_t *loop, odin_event_io_t *io, int fd,
                     unsigned int events, void *user_data);

/* Mirrors do_read (odin/relay.c:104-125): n > 0 -> OK; n == 0 -> EOF;
 * EAGAIN/EWOULDBLOCK/EINTR -> AGAIN; any other -> ERROR with errno set. */
static odin_transport_io_t fd_read(odin_transport_t *t, void *buf, size_t len,
                                   size_t *out_n) {
  odin_fd_transport_t *s = (odin_fd_transport_t *)t;
  const ssize_t n = read(s->fd, buf, len);
  if (n > 0) {
    *out_n = (size_t)n;
    return ODIN_TRANSPORT_OK;
  }
  if (n == 0) {
    *out_n = 0;
    return ODIN_TRANSPORT_EOF;
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
    return ODIN_TRANSPORT_AGAIN;
  }
  return ODIN_TRANSPORT_IO_ERROR;
}

/* Mirrors do_write (odin/relay.c:128-144): n > 0 -> OK;
 * EAGAIN/EWOULDBLOCK/EINTR -> AGAIN; else -> ERROR with errno set. */
static odin_transport_io_t fd_write(odin_transport_t *t, const void *buf,
                                    size_t len, size_t *out_n) {
  odin_fd_transport_t *s = (odin_fd_transport_t *)t;
  const ssize_t n = write(s->fd, buf, len);
  if (n > 0) {
    *out_n = (size_t)n;
    return ODIN_TRANSPORT_OK;
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
    return ODIN_TRANSPORT_AGAIN;
  }
  return ODIN_TRANSPORT_IO_ERROR;
}

/* Half-close: shutdown(fd, SHUT_WR), as the relay's half-close does
 * (odin/relay.c:205-216). 0 on success, -1 with errno set. */
static int fd_shutdown_write(odin_transport_t *t) {
  odin_fd_transport_t *s = (odin_fd_transport_t *)t;
  return shutdown(s->fd, SHUT_WR);
}

/* A non-empty READ|WRITE mask starts the watch if none is active or updates it
 * otherwise; an empty mask stops the active watch (no-op if none is active),
 * reproducing the relay's lazy reconcile (odin/relay.c:149-177). */
static int fd_set_interest(odin_transport_t *t, unsigned int events) {
  odin_fd_transport_t *s = (odin_fd_transport_t *)t;
  unsigned int ev = 0;
  if (events & ODIN_TRANSPORT_READ) {
    ev |= ODIN_EVENT_READ;
  }
  if (events & ODIN_TRANSPORT_WRITE) {
    ev |= ODIN_EVENT_WRITE;
  }
  if (ev == 0) {
    if (s->io != NULL) {
      odin_event_io_stop(s->io);
      s->io = NULL;
    }
    s->cur = 0;
    return 0;
  }
  if (s->io == NULL) {
    if (odin_event_io_start(s->loop, s->fd, ev, fd_on_io, s, &s->io) != 0) {
      return -1;
    }
  } else if (ev != s->cur) {
    if (odin_event_io_update(s->io, ev) != 0) {
      return -1;
    }
  }
  s->cur = ev;
  return 0;
}

/* getsockopt(SO_ERROR): the latched asynchronous socket error -- 0 when none,
 * or the getsockopt errno on failure (odin/relay.c:93-100). */
static int fd_error(odin_transport_t *t) {
  odin_fd_transport_t *s = (odin_fd_transport_t *)t;
  int err = 0;
  socklen_t len = sizeof(err);
  if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
    return errno;
  }
  return err;
}

/* Stops any active watch and frees the implementation struct only; never closes
 * fd. Callable from within the readiness callback. */
static void fd_destroy(odin_transport_t *t) {
  odin_fd_transport_t *s = (odin_fd_transport_t *)t;
  if (s->io != NULL) {
    odin_event_io_stop(s->io);
    s->io = NULL;
  }
  free(s);
}

/* Translates the loop's ODIN_EVENT_* readiness to the equal-valued
 * ODIN_TRANSPORT_* bits and invokes on_ready. */
static void fd_on_io(odin_event_loop_t *loop, odin_event_io_t *io, int fd,
                     unsigned int events, void *user_data) {
  (void)loop;
  (void)io;
  (void)fd;
  odin_fd_transport_t *s = (odin_fd_transport_t *)user_data;
  unsigned int ev = 0;
  if (events & ODIN_EVENT_READ) {
    ev |= ODIN_TRANSPORT_READ;
  }
  if (events & ODIN_EVENT_WRITE) {
    ev |= ODIN_TRANSPORT_WRITE;
  }
  if (events & ODIN_EVENT_ERROR) {
    ev |= ODIN_TRANSPORT_ERROR;
  }
  s->on_ready(&s->base, ev, s->user_data);
}

static const odin_transport_vtable_t fd_vtable = {
    fd_read, fd_write, fd_shutdown_write, fd_set_interest, fd_error, fd_destroy,
};

int odin_fd_transport_create(odin_event_loop_t *loop, int fd,
                             odin_transport_ready_cb on_ready, void *user_data,
                             odin_transport_t **out) {
  odin_fd_transport_t *s = (odin_fd_transport_t *)calloc(1, sizeof(*s));
  if (s == NULL) {
    errno = ENOMEM;
    return -1;
  }
  s->base.vt = &fd_vtable;
  s->loop = loop;
  s->fd = fd;
  s->io = NULL;
  s->cur = 0;
  s->on_ready = on_ready;
  s->user_data = user_data;
  *out = &s->base;
  return 0;
}

#if defined(ODIN_TRANSPORT_FD_TESTING)
int odin_fd_transport_test_io(odin_transport_t *t, odin_event_io_t **out) {
  odin_fd_transport_t *s = (odin_fd_transport_t *)t;
  if (s->io == NULL) {
    errno = ENOENT;
    return -1;
  }
  *out = s->io;
  return 0;
}
#endif
