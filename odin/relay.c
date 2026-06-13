/* odin/relay.c -- RFC-014 transport-agnostic bidirectional byte relay.
 *
 * Forwards bytes between two caller-owned odin_transport_t endpoints (RFC-013)
 * through the odin_transport_* dispatchers: every read, write, half-close,
 * readiness/interest change, and asynchronous-error probe goes through the
 * vtable, so the relay names no concrete transport and carries none of a
 * transport's dependencies. It provides fixed 64 KiB
 * per-direction backpressure buffering, end-of-stream-as-shutdown_write
 * propagation, single-error aggregation, and exactly-once completion. It owns
 * its object and its two buffers only: it destroys neither transport and closes
 * no fd.
 */

#include "odin/relay.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>

#include "odin/transport.h"

/* Fixed per-direction buffer capacity: 64 KiB (§3.2.2 CAP). */
#define ODIN_RELAY_CAP 65536u

/* Direction indices: A = a -> b, B = b -> a. */
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

/* One forwarding direction: read from src_t into a CAP-byte ring, write the
 * ring out to sink_t. head/len describe the ring; tail is derived. */
typedef struct {
  odin_transport_t *src_t;
  odin_transport_t *sink_t;
  unsigned char *buf;
  size_t head;
  size_t len;
  int read_eof;
  int write_shut;
} odin_relay_dir_t;

/* One watched endpoint: sources one direction, sinks the other. cur is the last
 * interest mask set on the endpoint. */
typedef struct {
  odin_transport_t *t;
  odin_relay_dir_t *src;
  odin_relay_dir_t *sink;
  unsigned int cur;
} odin_relay_end_t;

struct odin_relay_t {
  odin_relay_done_cb on_done;
  void *user_data;
  odin_relay_dir_t dir[2];
  odin_relay_end_t end[2];
  int outcome;
  int err;
  int torn_down;
};

/* Reads into d's contiguous free run at tail (min(CAP-len, CAP-tail), > 0
 * whenever READ is watched), so the direction never buffers more than CAP. */
static int do_read(odin_relay_t *r, odin_relay_dir_t *d) {
  const size_t tail = (d->head + d->len) % ODIN_RELAY_CAP;
  size_t run = ODIN_RELAY_CAP - d->len;
  if (run > ODIN_RELAY_CAP - tail) {
    run = ODIN_RELAY_CAP - tail;
  }
  size_t n = 0;
  switch (odin_transport_read(d->src_t, d->buf + tail, run, &n)) {
  case ODIN_TRANSPORT_OK:
    d->len += n;
    return ODIN_RELAY_READ_PROGRESS;
  case ODIN_TRANSPORT_EOF:
    d->read_eof = 1;
    return ODIN_RELAY_READ_EOF;
  case ODIN_TRANSPORT_AGAIN:
    return ODIN_RELAY_READ_AGAIN;
  case ODIN_TRANSPORT_IO_ERROR:
    break;
  }
  r->outcome = ODIN_RELAY_OUTCOME_ERROR;
  r->err = errno; /* errno set by transport (RFC-013) */
  return ODIN_RELAY_READ_FAILED;
}

/* Drains d's contiguous buffered run at head (min(len, CAP-head)). write never
 * returns EOF (RFC-013), so any non-OK/AGAIN result is a genuine fault. */
static void do_write(odin_relay_t *r, odin_relay_dir_t *d) {
  size_t run = d->len;
  if (run > ODIN_RELAY_CAP - d->head) {
    run = ODIN_RELAY_CAP - d->head;
  }
  size_t n = 0;
  switch (odin_transport_write(d->sink_t, d->buf + d->head, run, &n)) {
  case ODIN_TRANSPORT_OK:
    d->head = (d->head + n) % ODIN_RELAY_CAP;
    d->len -= n;
    return;
  case ODIN_TRANSPORT_AGAIN:
    return;
  case ODIN_TRANSPORT_EOF:
  case ODIN_TRANSPORT_IO_ERROR:
    break;
  }
  r->outcome = ODIN_RELAY_OUTCOME_ERROR;
  r->err = errno;
}

/* Recomputes one endpoint's interest: READ while its source can still fill
 * (read_eof clear and buffer below CAP), WRITE while its sink has bytes.
 * set_interest handles the lazy start/update/stop of the underlying watch. */
static void reconcile(odin_relay_t *r, odin_relay_end_t *e) {
  unsigned int m = 0;
  if (e->src->read_eof == 0 && e->src->len < ODIN_RELAY_CAP) {
    m |= ODIN_TRANSPORT_READ;
  }
  if (e->sink->len > 0) {
    m |= ODIN_TRANSPORT_WRITE;
  }
  if (m == e->cur) {
    return;
  }
  if (odin_transport_set_interest(e->t, m) != 0) {
    r->outcome = ODIN_RELAY_OUTCOME_ERROR;
    r->err = errno;
    return;
  }
  e->cur = m;
}

/* For each drained, EOF'd direction issue exactly one shutdown_write on the
 * peer; then reconcile both interests; then complete when both directions are
 * half-closed. */
static void drive(odin_relay_t *r) {
  for (int i = 0; i < 2; ++i) {
    odin_relay_dir_t *d = &r->dir[i];
    if (d->read_eof && d->len == 0 && !d->write_shut) {
      if (odin_transport_shutdown_write(d->sink_t) != 0) {
        r->outcome = ODIN_RELAY_OUTCOME_ERROR;
        r->err = errno;
        return;
      }
      d->write_shut = 1;
    }
  }
  reconcile(r, &r->end[0]);
  reconcile(r, &r->end[1]);
  if (r->outcome == ODIN_RELAY_OUTCOME_NONE &&
      r->dir[ODIN_RELAY_DIR_A].write_shut &&
      r->dir[ODIN_RELAY_DIR_B].write_shut) {
    r->outcome = ODIN_RELAY_OUTCOME_OK;
  }
}

/* Stops both interests, then fires on_done as the relay's final action. The
 * torn_down guard makes this idempotent; no relay state is read or written
 * after on_done returns, so odin_relay_destroy from inside on_done is safe. */
static void teardown(odin_relay_t *r) {
  if (r->torn_down) {
    return;
  }
  r->torn_down = 1;
  if (r->end[0].cur != 0) {
    odin_transport_set_interest(r->end[0].t, 0);
    r->end[0].cur = 0;
  }
  if (r->end[1].cur != 0) {
    odin_transport_set_interest(r->end[1].t, 0);
    r->end[1].cur = 0;
  }
  const odin_relay_done_cb cb = r->on_done;
  void *const ud = r->user_data;
  const odin_relay_status_t st =
      (r->outcome == ODIN_RELAY_OUTCOME_OK) ? ODIN_RELAY_OK : ODIN_RELAY_ERROR;
  const int e = (st == ODIN_RELAY_OK) ? 0 : r->err;
  cb(r, st, e, ud);
}

int odin_relay_create(odin_relay_done_cb on_done, void *user_data,
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

  r->on_done = on_done;
  r->user_data = user_data;
  r->outcome = ODIN_RELAY_OUTCOME_NONE;

  *out = r;
  return 0;
}

/* Readiness handler: flush the ready endpoint's sink, drain its source,
 * classify an ODIN_TRANSPORT_ERROR readiness via do_read/odin_transport_error,
 * then drive; teardown when an outcome is set. Skips every sub-step once
 * outcome
 * != NONE (so a same-batch sibling that still re-enters becomes a no-op).
 * sink->sink_t == t and src->src_t == t by construction, so both ops act on the
 * endpoint that fired. */
void odin_relay_ready(odin_transport_t *t, unsigned int events,
                      void *user_data) {
  odin_relay_t *r = (odin_relay_t *)user_data;
  odin_relay_end_t *e = (t == r->end[0].t) ? &r->end[0] : &r->end[1];
  odin_relay_dir_t *src = e->src;
  odin_relay_dir_t *sink = e->sink;
  const int err = (events & ODIN_TRANSPORT_ERROR) != 0;

  if (r->outcome == ODIN_RELAY_OUTCOME_NONE && sink->len > 0 &&
      ((events & ODIN_TRANSPORT_WRITE) || err)) {
    do_write(r, sink);
  }

  int rd = ODIN_RELAY_READ_AGAIN;
  if (r->outcome == ODIN_RELAY_OUTCOME_NONE && src->read_eof == 0 &&
      src->len < ODIN_RELAY_CAP && ((events & ODIN_TRANSPORT_READ) || err)) {
    rd = do_read(r, src);
  }

  if (r->outcome == ODIN_RELAY_OUTCOME_NONE && err && src->read_eof == 0 &&
      rd != ODIN_RELAY_READ_PROGRESS) {
    const int e2 =
        odin_transport_error(t); /* latched async error; 0 when benign */
    if (e2 != 0) {
      r->outcome = ODIN_RELAY_OUTCOME_ERROR;
      r->err = e2;
    }
  }

  if (r->outcome == ODIN_RELAY_OUTCOME_NONE) {
    drive(r);
  }
  if (r->outcome != ODIN_RELAY_OUTCOME_NONE) {
    teardown(r);
  }
}

int odin_relay_start(odin_relay_t *relay, odin_transport_t *a,
                     odin_transport_t *b) {
  /* dir A: a -> b; dir B: b -> a. */
  relay->dir[ODIN_RELAY_DIR_A].src_t = a;
  relay->dir[ODIN_RELAY_DIR_A].sink_t = b;
  relay->dir[ODIN_RELAY_DIR_B].src_t = b;
  relay->dir[ODIN_RELAY_DIR_B].sink_t = a;

  /* end[0] = a sources A, sinks B; end[1] = b sources B, sinks A. */
  relay->end[0].t = a;
  relay->end[0].src = &relay->dir[ODIN_RELAY_DIR_A];
  relay->end[0].sink = &relay->dir[ODIN_RELAY_DIR_B];
  relay->end[1].t = b;
  relay->end[1].src = &relay->dir[ODIN_RELAY_DIR_B];
  relay->end[1].sink = &relay->dir[ODIN_RELAY_DIR_A];

  /* One READ watch per endpoint, a first then b. Roll a back if b's fails. */
  if (odin_transport_set_interest(a, ODIN_TRANSPORT_READ) != 0) {
    return -1; /* errno preserved; nothing registered, re-startable */
  }
  relay->end[0].cur = ODIN_TRANSPORT_READ;
  if (odin_transport_set_interest(b, ODIN_TRANSPORT_READ) != 0) {
    const int saved = errno;
    odin_transport_set_interest(a, 0);
    relay->end[0].cur = 0;
    errno = saved; /* both cur == 0, a/b rebindable -> re-startable */
    return -1;
  }
  relay->end[1].cur = ODIN_TRANSPORT_READ;
  return 0;
}

void odin_relay_destroy(odin_relay_t *relay) {
  if (relay == NULL) {
    return;
  }
  if (relay->end[0].cur != 0) {
    odin_transport_set_interest(relay->end[0].t, 0);
    relay->end[0].cur = 0;
  }
  if (relay->end[1].cur != 0) {
    odin_transport_set_interest(relay->end[1].t, 0);
    relay->end[1].cur = 0;
  }
  free(relay->dir[ODIN_RELAY_DIR_A].buf);
  free(relay->dir[ODIN_RELAY_DIR_B].buf);
  free(relay);
}
