/* odin/relay.c -- RFC-011 bidirectional byte relay.
 *
 * Single-thread, event-loop-driven: watches two caller-owned nonblocking
 * connected stream sockets via the RFC-010 odin_event_io_* API and forwards
 * bytes in each direction through a fixed 64 KiB per-direction ring buffer with
 * backpressure (stop reading a source when its destination buffer is full,
 * resume when it drains). End-of-stream propagates as shutdown(SHUT_WR) on the
 * peer fd after flushing; a genuine read/write/shutdown (or asynchronous
 * socket) error aggregates into one teardown that fires on_done exactly once.
 * The relay never closes the fds and frees only its own state.
 */

#include "odin/relay.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(ODIN_RELAY_TESTING)
#include "odin/relay_internal_test.h"
#endif

/* Fixed per-direction buffer capacity: 64 KiB (§3.2.2 CAP). */
#define ODIN_RELAY_CAP 65536u

/* Direction indices: A = fd_a -> fd_b, B = fd_b -> fd_a. */
#define ODIN_RELAY_DIR_A 0
#define ODIN_RELAY_DIR_B 1

/* Relay-wide outcome: set once by the first fault (or dual end-of-stream),
 * then consumed once by teardown. */
enum {
  ODIN_RELAY_OUTCOME_NONE = 0,
  ODIN_RELAY_OUTCOME_OK,
  ODIN_RELAY_OUTCOME_ERROR,
};

/* do_read result classification. */
enum {
  ODIN_RELAY_READ_AGAIN = 0,
  ODIN_RELAY_READ_PROGRESS,
  ODIN_RELAY_READ_EOF,
  ODIN_RELAY_READ_FAILED,
};

/* One forwarding direction: read from src_fd into a CAP-byte ring, write the
 * ring out to sink_fd. head/len describe the ring; tail is derived. */
typedef struct {
  int src_fd;
  int sink_fd;
  unsigned char *buf;
  size_t head;
  size_t len;
  int read_eof;
  int write_shut;
} odin_relay_dir_t;

/* One watched fd endpoint: sources one direction, sinks the other. */
typedef struct {
  int fd;
  odin_relay_dir_t *src;
  odin_relay_dir_t *sink;
  odin_event_io_t *io;
  unsigned int cur;
} odin_relay_end_t;

struct odin_relay_t {
  odin_event_loop_t *loop;
  odin_relay_done_cb on_done;
  void *user_data;
  odin_relay_dir_t dir[2];
  odin_relay_end_t end[2];
  int outcome;
  int err;
  int torn_down;
};

static void on_ready(odin_event_loop_t *loop, odin_event_io_t *io, int fd,
                     unsigned int events, void *user_data);

static odin_relay_dir_t *relay_src(odin_relay_t *r, int fd) {
  return (fd == r->end[0].fd) ? r->end[0].src : r->end[1].src;
}

static odin_relay_dir_t *relay_sink(odin_relay_t *r, int fd) {
  return (fd == r->end[0].fd) ? r->end[0].sink : r->end[1].sink;
}

/* getsockopt(SO_ERROR): the latched asynchronous socket error, or the errno
 * from a failing getsockopt. Nonzero means a genuine fault. */
static int so_error(int fd) {
  int err = 0;
  socklen_t len = sizeof(err);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
    return errno;
  }
  return err;
}

/* Reads into d's contiguous free run at tail (min(CAP-len, CAP-tail), > 0
 * whenever READ is watched), so the direction never buffers more than CAP. */
static int do_read(odin_relay_t *r, odin_relay_dir_t *d) {
  const size_t tail = (d->head + d->len) % ODIN_RELAY_CAP;
  size_t run = ODIN_RELAY_CAP - d->len;
  if (run > ODIN_RELAY_CAP - tail) {
    run = ODIN_RELAY_CAP - tail;
  }
  const ssize_t n = read(d->src_fd, d->buf + tail, run);
  if (n > 0) {
    d->len += (size_t)n;
    return ODIN_RELAY_READ_PROGRESS;
  }
  if (n == 0) {
    d->read_eof = 1;
    return ODIN_RELAY_READ_EOF;
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
    return ODIN_RELAY_READ_AGAIN;
  }
  r->outcome = ODIN_RELAY_OUTCOME_ERROR;
  r->err = errno;
  return ODIN_RELAY_READ_FAILED;
}

/* Drains d's contiguous buffered run at head (min(len, CAP-head)). */
static void do_write(odin_relay_t *r, odin_relay_dir_t *d) {
  size_t run = d->len;
  if (run > ODIN_RELAY_CAP - d->head) {
    run = ODIN_RELAY_CAP - d->head;
  }
  const ssize_t n = write(d->sink_fd, d->buf + d->head, run);
  if (n > 0) {
    d->head = (d->head + (size_t)n) % ODIN_RELAY_CAP;
    d->len -= (size_t)n;
    return;
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
    return;
  }
  r->outcome = ODIN_RELAY_OUTCOME_ERROR;
  r->err = errno;
}

/* Recomputes and applies an endpoint's watch mask: READ while its source can
 * still fill (read_eof clear and buffer below CAP), WRITE while its sink has
 * bytes. An empty mask stops the watch (re-started when it next wants one). */
static void reconcile(odin_relay_t *r, odin_relay_end_t *e) {
  unsigned int m = 0;
  if (e->src->read_eof == 0 && e->src->len < ODIN_RELAY_CAP) {
    m |= ODIN_EVENT_READ;
  }
  if (e->sink->len > 0) {
    m |= ODIN_EVENT_WRITE;
  }
  if (m == e->cur) {
    return;
  }
  if (m == 0) {
    odin_event_io_stop(e->io);
    e->io = NULL;
  } else if (e->io == NULL) {
    if (odin_event_io_start(r->loop, e->fd, m, on_ready, r, &e->io) != 0) {
      r->outcome = ODIN_RELAY_OUTCOME_ERROR;
      r->err = errno;
      return;
    }
  } else {
    if (odin_event_io_update(e->io, m) != 0) {
      r->outcome = ODIN_RELAY_OUTCOME_ERROR;
      r->err = errno;
      return;
    }
  }
  e->cur = m;
}

/* Stops both watches, then fires on_done as the relay's final action. The
 * torn_down guard makes this idempotent; no relay state is read or written
 * after on_done returns, so odin_relay_destroy from inside on_done is safe. */
static void teardown(odin_relay_t *r) {
  if (r->torn_down) {
    return;
  }
  r->torn_down = 1;
  if (r->end[0].io != NULL) {
    odin_event_io_stop(r->end[0].io);
    r->end[0].io = NULL;
  }
  if (r->end[1].io != NULL) {
    odin_event_io_stop(r->end[1].io);
    r->end[1].io = NULL;
  }
  const odin_relay_done_cb cb = r->on_done;
  void *const ud = r->user_data;
  const odin_relay_status_t st =
      (r->outcome == ODIN_RELAY_OUTCOME_OK) ? ODIN_RELAY_OK : ODIN_RELAY_ERROR;
  const int e = (st == ODIN_RELAY_OK) ? 0 : r->err;
  cb(r, st, e, ud);
}

/* For each drained, EOF'd direction issue exactly one shutdown(SHUT_WR); then
 * reconcile both watches; then complete when both directions are half-closed. */
static void drive(odin_relay_t *r) {
  for (int i = 0; i < 2; ++i) {
    odin_relay_dir_t *d = &r->dir[i];
    if (d->read_eof && d->len == 0 && !d->write_shut) {
      if (shutdown(d->sink_fd, SHUT_WR) != 0) {
        r->outcome = ODIN_RELAY_OUTCOME_ERROR;
        r->err = errno;
        return;
      }
      d->write_shut = 1;
    }
  }
  reconcile(r, &r->end[0]);
  reconcile(r, &r->end[1]);
  if (r->outcome == ODIN_RELAY_OUTCOME_NONE && r->dir[ODIN_RELAY_DIR_A].write_shut &&
      r->dir[ODIN_RELAY_DIR_B].write_shut) {
    r->outcome = ODIN_RELAY_OUTCOME_OK;
  }
}

/* Readiness callback: flush the ready fd's sink, drain its source, classify an
 * ODIN_EVENT_ERROR via read()/SO_ERROR, then drive; teardown when an outcome
 * is set. Skips every sub-step once outcome != NONE (so a same-batch sibling
 * that still re-enters becomes a no-op). */
static void on_ready(odin_event_loop_t *loop, odin_event_io_t *io, int fd,
                     unsigned int events, void *user_data) {
  (void)loop;
  (void)io;
  odin_relay_t *r = (odin_relay_t *)user_data;
  odin_relay_dir_t *src = relay_src(r, fd);
  odin_relay_dir_t *sink = relay_sink(r, fd);
  const int err = (events & ODIN_EVENT_ERROR) != 0;

  if (r->outcome == ODIN_RELAY_OUTCOME_NONE && sink->len > 0 &&
      ((events & ODIN_EVENT_WRITE) || err)) {
    do_write(r, sink);
  }

  int rd = ODIN_RELAY_READ_AGAIN;
  if (r->outcome == ODIN_RELAY_OUTCOME_NONE && src->read_eof == 0 &&
      src->len < ODIN_RELAY_CAP && ((events & ODIN_EVENT_READ) || err)) {
    rd = do_read(r, src);
  }

  if (r->outcome == ODIN_RELAY_OUTCOME_NONE && err && src->read_eof == 0 &&
      rd != ODIN_RELAY_READ_PROGRESS) {
    const int e = so_error(fd);
    if (e != 0) {
      r->outcome = ODIN_RELAY_OUTCOME_ERROR;
      r->err = e;
    }
  }

  if (r->outcome == ODIN_RELAY_OUTCOME_NONE) {
    drive(r);
  }
  if (r->outcome != ODIN_RELAY_OUTCOME_NONE) {
    teardown(r);
  }
}

int odin_relay_start(odin_event_loop_t *loop, int fd_a, int fd_b,
                     odin_relay_done_cb on_done, void *user_data,
                     odin_relay_t **out) {
  odin_relay_t *r = (odin_relay_t *)calloc(1, sizeof(*r));
  if (r == NULL) {
    errno = ENOMEM;
    return -1;
  }
  r->dir[ODIN_RELAY_DIR_A].buf = (unsigned char *)malloc(ODIN_RELAY_CAP);
  r->dir[ODIN_RELAY_DIR_B].buf = (unsigned char *)malloc(ODIN_RELAY_CAP);
  if (r->dir[ODIN_RELAY_DIR_A].buf == NULL ||
      r->dir[ODIN_RELAY_DIR_B].buf == NULL) {
    free(r->dir[ODIN_RELAY_DIR_A].buf);
    free(r->dir[ODIN_RELAY_DIR_B].buf);
    free(r);
    errno = ENOMEM;
    return -1;
  }

  r->loop = loop;
  r->on_done = on_done;
  r->user_data = user_data;
  r->outcome = ODIN_RELAY_OUTCOME_NONE;

  /* dir A: fd_a -> fd_b; dir B: fd_b -> fd_a. */
  r->dir[ODIN_RELAY_DIR_A].src_fd = fd_a;
  r->dir[ODIN_RELAY_DIR_A].sink_fd = fd_b;
  r->dir[ODIN_RELAY_DIR_B].src_fd = fd_b;
  r->dir[ODIN_RELAY_DIR_B].sink_fd = fd_a;

  /* end[0] = fd_a sources A, sinks B; end[1] = fd_b sources B, sinks A. */
  r->end[0].fd = fd_a;
  r->end[0].src = &r->dir[ODIN_RELAY_DIR_A];
  r->end[0].sink = &r->dir[ODIN_RELAY_DIR_B];
  r->end[1].fd = fd_b;
  r->end[1].src = &r->dir[ODIN_RELAY_DIR_B];
  r->end[1].sink = &r->dir[ODIN_RELAY_DIR_A];

  /* Both buffers empty, both read sides open: one READ watch per fd, fd_a
   * first, then fd_b. Roll back fd_a's watch if fd_b's fails. */
  if (odin_event_io_start(loop, fd_a, ODIN_EVENT_READ, on_ready, r,
                          &r->end[0].io) != 0) {
    const int saved = errno;
    free(r->dir[ODIN_RELAY_DIR_A].buf);
    free(r->dir[ODIN_RELAY_DIR_B].buf);
    free(r);
    errno = saved;
    return -1;
  }
  r->end[0].cur = ODIN_EVENT_READ;
  if (odin_event_io_start(loop, fd_b, ODIN_EVENT_READ, on_ready, r,
                          &r->end[1].io) != 0) {
    const int saved = errno;
    odin_event_io_stop(r->end[0].io);
    free(r->dir[ODIN_RELAY_DIR_A].buf);
    free(r->dir[ODIN_RELAY_DIR_B].buf);
    free(r);
    errno = saved;
    return -1;
  }
  r->end[1].cur = ODIN_EVENT_READ;

  *out = r;
  return 0;
}

void odin_relay_destroy(odin_relay_t *relay) {
  if (relay == NULL) {
    return;
  }
  if (relay->end[0].io != NULL) {
    odin_event_io_stop(relay->end[0].io);
    relay->end[0].io = NULL;
  }
  if (relay->end[1].io != NULL) {
    odin_event_io_stop(relay->end[1].io);
    relay->end[1].io = NULL;
  }
  free(relay->dir[ODIN_RELAY_DIR_A].buf);
  free(relay->dir[ODIN_RELAY_DIR_B].buf);
  free(relay);
}

#if defined(ODIN_RELAY_TESTING)
int odin_relay_test_io_handles(odin_relay_t *relay, odin_event_io_t **out_a,
                               odin_event_io_t **out_b) {
  if (relay->end[0].io == NULL || relay->end[1].io == NULL) {
    errno = ENOENT;
    return -1;
  }
  *out_a = relay->end[0].io;
  *out_b = relay->end[1].io;
  return 0;
}
#endif
