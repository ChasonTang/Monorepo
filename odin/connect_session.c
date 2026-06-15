/* odin/connect_session.c -- RFC-018 transport-agnostic CONNECT handshake.
 *
 * One round of the RFC-001 control-frame handshake on a caller-supplied
 * odin_transport_t (RFC-013). Issues every byte through the odin_transport_*
 * dispatchers; binds no odin_transport_ready_cb, registers no event-loop
 * watch, never calls odin_transport_set_interest, and never owns or destroys
 * the transport. The orchestrator owns the transport, owns the watch, and
 * computes the next interest mask from odin_connect_session_wants(s) after
 * every drive.
 */

#include "odin/connect_session.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "odin/protocol.h"
#include "odin/transport.h"

enum odin_connect_session_mode_t {
  ODIN_CONNECT_SESSION_MODE_CLIENT = 0,
  ODIN_CONNECT_SESSION_MODE_SERVER,
};

enum odin_connect_session_state_t {
  ODIN_CONNECT_SESSION_C_WRITING_REQ = 0,
  ODIN_CONNECT_SESSION_C_READING_RESP,
  ODIN_CONNECT_SESSION_S_READING_REQ,
  ODIN_CONNECT_SESSION_S_AWAIT_DIAL,
  ODIN_CONNECT_SESSION_S_WRITING_RESP,
  ODIN_CONNECT_SESSION_DONE,
  ODIN_CONNECT_SESSION_ERROR_STATE,
};

struct odin_connect_session_t {
  int mode;
  int state;
  odin_connect_session_done_cb on_done;
  odin_connect_session_req_decoded_cb on_req_decoded;
  void *user_data;

  /* 260-byte accumulator; shared by write and read phases across both modes. */
  uint8_t buf[ODIN_PROTO_CONNECT_REQ_MAX];
  size_t buf_used;
  size_t write_total;
  size_t write_off;

  /* Server-mode fields. */
  odin_proto_connect_req_view_t req_view;
  size_t server_tail_off;
  odin_proto_connect_resp_frame_t resp_frame;
  size_t resp_write_off;

  /* Client-mode fields. */
  uint16_t client_error_code;
  size_t client_tail_off;

  /* Fault state. */
  int err;
  int on_done_fired;
};

static void aggregate_error(odin_connect_session_t *s, int err) {
  if (s->state == ODIN_CONNECT_SESSION_ERROR_STATE) {
    return;
  }
  s->state = ODIN_CONNECT_SESSION_ERROR_STATE;
  s->err = err;
}

static void fire_on_done_once(odin_connect_session_t *s) {
  if (s->on_done_fired) {
    return;
  }
  s->on_done_fired = 1;
  const odin_connect_session_status_t status =
      (s->state == ODIN_CONNECT_SESSION_DONE) ? ODIN_CONNECT_SESSION_OK
                                              : ODIN_CONNECT_SESSION_ERROR;
  const int err = (s->state == ODIN_CONNECT_SESSION_DONE) ? 0 : s->err;
  const odin_connect_session_done_cb cb = s->on_done;
  void *const ud = s->user_data;
  /* Capture cb/ud before invoking — destroy from inside on_done frees s, and
   * drive() must not touch s after the callback returns. */
  cb(s, status, err, ud);
}

int odin_connect_session_create_client(const char *host, size_t host_len,
                                       uint16_t port,
                                       odin_connect_session_done_cb on_done,
                                       void *user_data,
                                       odin_connect_session_t **out) {
  if (host_len < 1 || host_len > ODIN_PROTO_HOST_MAX) {
    errno = EINVAL;
    return -1;
  }
  odin_connect_session_t *s = (odin_connect_session_t *)calloc(1, sizeof(*s));
  if (s == NULL) {
    errno = ENOMEM;
    return -1;
  }
  s->mode = ODIN_CONNECT_SESSION_MODE_CLIENT;
  s->state = ODIN_CONNECT_SESSION_C_WRITING_REQ;
  s->on_done = on_done;
  s->user_data = user_data;

  odin_proto_iov_t iov[3];
  uint8_t hdr[3];
  uint8_t portbe[2];
  const odin_proto_status_t pst =
      odin_proto_encode_connect_req(host, host_len, port, iov, hdr, portbe);
  /* host_len is in [1, 255] per the guard above, so the encoder must succeed.
   */
  assert(pst == ODIN_PROTO_OK);
  (void)pst;

  size_t off = 0;
  for (int i = 0; i < 3; ++i) {
    memcpy(s->buf + off, iov[i].base, iov[i].len);
    off += iov[i].len;
  }
  s->write_total = off;
  s->write_off = 0;

  *out = s;
  return 0;
}

int odin_connect_session_create_server(
    odin_connect_session_req_decoded_cb on_req_decoded,
    odin_connect_session_done_cb on_done, void *user_data,
    odin_connect_session_t **out) {
  odin_connect_session_t *s = (odin_connect_session_t *)calloc(1, sizeof(*s));
  if (s == NULL) {
    errno = ENOMEM;
    return -1;
  }
  s->mode = ODIN_CONNECT_SESSION_MODE_SERVER;
  s->state = ODIN_CONNECT_SESSION_S_READING_REQ;
  s->on_req_decoded = on_req_decoded;
  s->on_done = on_done;
  s->user_data = user_data;
  *out = s;
  return 0;
}

/* C_WRITING_REQ: drain s.buf[write_off .. write_total) onto the transport. */
static void do_write_req(odin_connect_session_t *s, odin_transport_t *t) {
  while (s->write_off < s->write_total) {
    size_t n = 0;
    const odin_transport_io_t rc = odin_transport_write(
        t, s->buf + s->write_off, s->write_total - s->write_off, &n);
    if (rc == ODIN_TRANSPORT_OK) {
      s->write_off += n;
      continue;
    }
    if (rc == ODIN_TRANSPORT_AGAIN) {
      return;
    }
    if (rc == ODIN_TRANSPORT_IO_ERROR) {
      aggregate_error(s, errno);
      return;
    }
    /* write never returns EOF (RFC-013 §3.2.1). */
    return;
  }
  s->state = ODIN_CONNECT_SESSION_C_READING_RESP;
  s->buf_used = 0;
}

/* C_READING_RESP: read up to (cap - buf_used) bytes into s.buf, then try to
 * decode. */
static void do_read_resp(odin_connect_session_t *s, odin_transport_t *t) {
  while (s->state == ODIN_CONNECT_SESSION_C_READING_RESP) {
    const size_t run = ODIN_PROTO_CONNECT_REQ_MAX - s->buf_used;
    if (run == 0) {
      /* Defensive: CONNECT_RESP is 4 bytes; a well-formed peer hits OK long
       * before buf_used == 260. Kept as a forward-compat guard. */
      return;
    }
    size_t n = 0;
    const odin_transport_io_t rc =
        odin_transport_read(t, s->buf + s->buf_used, run, &n);
    if (rc == ODIN_TRANSPORT_OK) {
      s->buf_used += n;
    } else if (rc == ODIN_TRANSPORT_AGAIN) {
      return;
    } else if (rc == ODIN_TRANSPORT_EOF) {
      aggregate_error(s, ECONNRESET);
      return;
    } else { /* ODIN_TRANSPORT_IO_ERROR */
      aggregate_error(s, errno);
      return;
    }

    size_t consumed = 0;
    uint16_t ec = 0;
    const odin_proto_status_t pst =
        odin_proto_decode_connect_resp(s->buf, s->buf_used, &consumed, &ec);
    if (pst == ODIN_PROTO_OK) {
      s->client_error_code = ec;
      s->client_tail_off = consumed;
      s->state = ODIN_CONNECT_SESSION_DONE;
      return;
    }
    if (pst == ODIN_PROTO_NEED_MORE) {
      continue;
    }
    aggregate_error(s, EPROTO);
    return;
  }
}

/* S_READING_REQ: read into s.buf, then try to decode the REQ. */
static void do_read_req(odin_connect_session_t *s, odin_transport_t *t) {
  while (s->state == ODIN_CONNECT_SESSION_S_READING_REQ) {
    const size_t run = ODIN_PROTO_CONNECT_REQ_MAX - s->buf_used;
    if (run == 0) {
      /* Defensive: under v1 host_len ∈ [1, 255], so a well-formed REQ totals
       * 5 + host_len ≤ 260. A malformed REQ that fills the buffer without
       * becoming decodable maps to EPROTO. */
      aggregate_error(s, EPROTO);
      return;
    }
    size_t n = 0;
    const odin_transport_io_t rc =
        odin_transport_read(t, s->buf + s->buf_used, run, &n);
    if (rc == ODIN_TRANSPORT_OK) {
      s->buf_used += n;
    } else if (rc == ODIN_TRANSPORT_AGAIN) {
      return;
    } else if (rc == ODIN_TRANSPORT_EOF) {
      aggregate_error(s, ECONNRESET);
      return;
    } else { /* ODIN_TRANSPORT_IO_ERROR */
      aggregate_error(s, errno);
      return;
    }

    size_t consumed = 0;
    const odin_proto_status_t pst = odin_proto_decode_connect_req(
        s->buf, s->buf_used, &consumed, &s->req_view);
    if (pst == ODIN_PROTO_OK) {
      s->server_tail_off = consumed;
      s->state = ODIN_CONNECT_SESSION_S_AWAIT_DIAL;
      s->on_req_decoded(s, s->user_data);
      return;
    }
    if (pst == ODIN_PROTO_NEED_MORE) {
      continue;
    }
    aggregate_error(s, EPROTO);
    return;
  }
}

/* S_WRITING_RESP: drain the 4-byte resp_frame onto the transport. */
static void do_write_resp(odin_connect_session_t *s, odin_transport_t *t) {
  while (s->resp_write_off < ODIN_PROTO_CONNECT_RESP_SIZE) {
    size_t n = 0;
    const odin_transport_io_t rc = odin_transport_write(
        t, s->resp_frame.bytes + s->resp_write_off,
        ODIN_PROTO_CONNECT_RESP_SIZE - s->resp_write_off, &n);
    if (rc == ODIN_TRANSPORT_OK) {
      s->resp_write_off += n;
      continue;
    }
    if (rc == ODIN_TRANSPORT_AGAIN) {
      return;
    }
    if (rc == ODIN_TRANSPORT_IO_ERROR) {
      aggregate_error(s, errno);
      return;
    }
    return;
  }
  s->state = ODIN_CONNECT_SESSION_DONE;
}

static odin_connect_session_drive_t drive_client(odin_connect_session_t *s,
                                                 odin_transport_t *t,
                                                 unsigned int events) {
  if (s->state == ODIN_CONNECT_SESSION_DONE ||
      s->state == ODIN_CONNECT_SESSION_ERROR_STATE) {
    return ODIN_CONNECT_SESSION_DRIVE_DONE;
  }

  if (s->state == ODIN_CONNECT_SESSION_C_WRITING_REQ &&
      (events & ODIN_TRANSPORT_WRITE)) {
    do_write_req(s, t);
  } else if (s->state == ODIN_CONNECT_SESSION_C_READING_RESP &&
             (events & ODIN_TRANSPORT_READ)) {
    do_read_resp(s, t);
  }

  if (s->state != ODIN_CONNECT_SESSION_DONE &&
      s->state != ODIN_CONNECT_SESSION_ERROR_STATE &&
      (events & ODIN_TRANSPORT_ERROR)) {
    const int err = odin_transport_error(t);
    if (err != 0) {
      aggregate_error(s, err);
    }
  }

  if (s->state == ODIN_CONNECT_SESSION_DONE ||
      s->state == ODIN_CONNECT_SESSION_ERROR_STATE) {
    fire_on_done_once(s);
    return ODIN_CONNECT_SESSION_DRIVE_DONE;
  }
  return ODIN_CONNECT_SESSION_DRIVE_CONTINUE;
}

static odin_connect_session_drive_t drive_server(odin_connect_session_t *s,
                                                 odin_transport_t *t,
                                                 unsigned int events) {
  if (s->state == ODIN_CONNECT_SESSION_DONE ||
      s->state == ODIN_CONNECT_SESSION_ERROR_STATE) {
    return ODIN_CONNECT_SESSION_DRIVE_DONE;
  }

  if (s->state == ODIN_CONNECT_SESSION_S_READING_REQ &&
      (events & ODIN_TRANSPORT_READ)) {
    do_read_req(s, t);
  } else if (s->state == ODIN_CONNECT_SESSION_S_WRITING_RESP &&
             (events & ODIN_TRANSPORT_WRITE)) {
    do_write_resp(s, t);
  }
  /* S_AWAIT_DIAL: drive() consumes the events but issues no read/write — the
   * session has no work until the caller invokes server_set_error_code. */

  if (s->state != ODIN_CONNECT_SESSION_DONE &&
      s->state != ODIN_CONNECT_SESSION_ERROR_STATE &&
      (events & ODIN_TRANSPORT_ERROR)) {
    const int err = odin_transport_error(t);
    if (err != 0) {
      aggregate_error(s, err);
    }
  }

  if (s->state == ODIN_CONNECT_SESSION_DONE ||
      s->state == ODIN_CONNECT_SESSION_ERROR_STATE) {
    fire_on_done_once(s);
    return ODIN_CONNECT_SESSION_DRIVE_DONE;
  }
  return ODIN_CONNECT_SESSION_DRIVE_CONTINUE;
}

odin_connect_session_drive_t
odin_connect_session_drive(odin_connect_session_t *s, odin_transport_t *t,
                           unsigned int events) {
  if (s->mode == ODIN_CONNECT_SESSION_MODE_CLIENT) {
    return drive_client(s, t, events);
  }
  return drive_server(s, t, events);
}

unsigned int odin_connect_session_wants(const odin_connect_session_t *s) {
  switch (s->state) {
  case ODIN_CONNECT_SESSION_C_WRITING_REQ:
  case ODIN_CONNECT_SESSION_S_WRITING_RESP:
    return ODIN_TRANSPORT_WRITE;
  case ODIN_CONNECT_SESSION_C_READING_RESP:
  case ODIN_CONNECT_SESSION_S_READING_REQ:
    return ODIN_TRANSPORT_READ;
  case ODIN_CONNECT_SESSION_S_AWAIT_DIAL:
  case ODIN_CONNECT_SESSION_DONE:
  case ODIN_CONNECT_SESSION_ERROR_STATE:
  default:
    return 0;
  }
}

void odin_connect_session_server_set_error_code(odin_connect_session_t *s,
                                                uint16_t error_code) {
  assert(s->state == ODIN_CONNECT_SESSION_S_AWAIT_DIAL);
  odin_proto_encode_connect_resp(error_code, &s->resp_frame);
  s->resp_write_off = 0;
  s->state = ODIN_CONNECT_SESSION_S_WRITING_RESP;
}

void odin_connect_session_server_host(const odin_connect_session_t *s,
                                      const char **out_host,
                                      size_t *out_host_len) {
  *out_host = (const char *)(s->buf + s->req_view.host_off);
  *out_host_len = s->req_view.host_len;
}

uint16_t odin_connect_session_server_port(const odin_connect_session_t *s) {
  return s->req_view.port;
}

void odin_connect_session_server_tail(const odin_connect_session_t *s,
                                      const uint8_t **out_ptr,
                                      size_t *out_len) {
  *out_ptr = s->buf + s->server_tail_off;
  *out_len = s->buf_used - s->server_tail_off;
}

uint16_t
odin_connect_session_client_error_code(const odin_connect_session_t *s) {
  return s->client_error_code;
}

void odin_connect_session_client_tail(const odin_connect_session_t *s,
                                      const uint8_t **out_ptr,
                                      size_t *out_len) {
  *out_ptr = s->buf + s->client_tail_off;
  *out_len = s->buf_used - s->client_tail_off;
}

void odin_connect_session_destroy(odin_connect_session_t *s) {
  if (s == NULL) {
    return;
  }
  free(s);
}
