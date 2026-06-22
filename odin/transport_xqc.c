/* odin/transport_xqc.c -- RFC-016 xqc_stream_t transport implementation. */

#include "odin/transport_xqc.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>

#if defined(ODIN_TRANSPORT_XQC_TESTING)
#include "odin/testing/transport_xqc_internal_test.h"
#endif

typedef struct odin_xqc_stream_transport_t {
  odin_transport_t base;
  xqc_stream_t *stream;
  odin_transport_ready_cb on_ready;
  void *user_data;
  unsigned int interest;
  int err;
  int recv_eof_latched;
  int recv_eof_ready_pending;
  int read_ready_depth;
  int callback_depth;
  int destroy_pending;
  int fin_pending;
  int fin_sent;
} odin_xqc_stream_transport_t;

#if defined(ODIN_TRANSPORT_XQC_TESTING)
static odin_xqc_stream_transport_test_ops_t odin_xqc_test_ops;
static int odin_xqc_test_fail_next_create_armed;
static int odin_xqc_test_fail_next_create_errno;

void odin_xqc_stream_transport_test_set_ops(
    const odin_xqc_stream_transport_test_ops_t *ops) {
  if (ops == NULL) {
    odin_xqc_test_ops.recv = NULL;
    odin_xqc_test_ops.send = NULL;
    odin_xqc_test_ops.set_user_data = NULL;
    return;
  }
  odin_xqc_test_ops = *ops;
}

unsigned int odin_xqc_stream_transport_test_interest(odin_transport_t *t) {
  odin_xqc_stream_transport_t *s = (odin_xqc_stream_transport_t *)t;
  return s->interest;
}

int odin_xqc_stream_transport_test_fail_next_create(int errnum) {
  odin_xqc_test_fail_next_create_armed = 1;
  odin_xqc_test_fail_next_create_errno = errnum;
  return 0;
}
#endif

static ssize_t odin_xqc_stream_recv_call(xqc_stream_t *stream,
                                         unsigned char *recv_buf,
                                         size_t recv_buf_size, uint8_t *fin) {
#if defined(ODIN_TRANSPORT_XQC_TESTING)
  if (odin_xqc_test_ops.recv == NULL) {
    return -XQC_CLOSING;
  }
  return odin_xqc_test_ops.recv(stream, recv_buf, recv_buf_size, fin);
#else
  return xqc_stream_recv(stream, recv_buf, recv_buf_size, fin);
#endif
}

static ssize_t odin_xqc_stream_send_call(xqc_stream_t *stream,
                                         unsigned char *send_data,
                                         size_t send_data_size, uint8_t fin) {
#if defined(ODIN_TRANSPORT_XQC_TESTING)
  if (odin_xqc_test_ops.send == NULL) {
    return -XQC_CLOSING;
  }
  return odin_xqc_test_ops.send(stream, send_data, send_data_size, fin);
#else
  return xqc_stream_send(stream, send_data, send_data_size, fin);
#endif
}

static void odin_xqc_stream_set_user_data_call(xqc_stream_t *stream,
                                               void *user_data) {
#if defined(ODIN_TRANSPORT_XQC_TESTING)
  if (odin_xqc_test_ops.set_user_data != NULL) {
    odin_xqc_test_ops.set_user_data(stream, user_data);
  }
#else
  xqc_stream_set_user_data(stream, user_data);
#endif
}

static int odin_xqc_map_error(ssize_t ret) {
  if (ret == -XQC_ESTREAM_RESET || ret == -XQC_CLOSING) {
    return EPIPE;
  }
  return EIO;
}

static int odin_xqc_end_callback(odin_xqc_stream_transport_t *s) {
  s->callback_depth -= 1;
  if (s->callback_depth == 0 && s->destroy_pending) {
    free(s);
    return 1;
  }
  return 0;
}

static int odin_xqc_deliver_ready(odin_xqc_stream_transport_t *s,
                                  unsigned int events) {
  if (s->destroy_pending) {
    return 0;
  }
  s->callback_depth += 1;
  s->on_ready(&s->base, events, s->user_data);
  return odin_xqc_end_callback(s);
}

static int
odin_xqc_deliver_pending_eof_ready_if_armed(odin_xqc_stream_transport_t *s) {
  if (s->destroy_pending || !s->recv_eof_ready_pending ||
      !(s->interest & ODIN_TRANSPORT_READ) || s->read_ready_depth != 0) {
    return 0;
  }
  s->recv_eof_ready_pending = 0;
  return odin_xqc_deliver_ready(s, ODIN_TRANSPORT_READ);
}

static int odin_xqc_deliver_write_kick_if_armed(odin_xqc_stream_transport_t *s,
                                                unsigned int old_interest) {
  if (s->destroy_pending || (old_interest & ODIN_TRANSPORT_WRITE) ||
      !(s->interest & ODIN_TRANSPORT_WRITE)) {
    return 0;
  }
  return odin_xqc_deliver_ready(s, ODIN_TRANSPORT_WRITE);
}

static void odin_xqc_retry_pending_fin(odin_xqc_stream_transport_t *s) {
  if (!s->fin_pending) {
    return;
  }
  const ssize_t ret = odin_xqc_stream_send_call(s->stream, NULL, 0, 1);
  if (ret >= 0) {
    s->fin_pending = 0;
    s->fin_sent = 1;
    return;
  }
  if (ret == -XQC_EAGAIN) {
    return;
  }
  s->fin_pending = 0;
  s->err = odin_xqc_map_error(ret);
}

static odin_transport_io_t odin_xqc_read(odin_transport_t *t, void *buf,
                                         size_t len, size_t *out_n) {
  odin_xqc_stream_transport_t *s = (odin_xqc_stream_transport_t *)t;
  if (s->recv_eof_latched) {
    *out_n = 0;
    return ODIN_TRANSPORT_EOF;
  }

  uint8_t fin = 0;
  const ssize_t ret =
      odin_xqc_stream_recv_call(s->stream, (unsigned char *)buf, len, &fin);
  if (ret > 0) {
    if ((size_t)ret > len) {
      errno = EIO;
      return ODIN_TRANSPORT_IO_ERROR;
    }
    if (fin == 1) {
      s->recv_eof_latched = 1;
      if (s->read_ready_depth != 0) {
        s->recv_eof_ready_pending = 1;
      }
    }
    *out_n = (size_t)ret;
    return ODIN_TRANSPORT_OK;
  }
  if (ret == 0 && fin == 1) {
    s->recv_eof_latched = 1;
    *out_n = 0;
    return ODIN_TRANSPORT_EOF;
  }
  if (ret == -XQC_EAGAIN || (ret == 0 && fin == 0)) {
    return ODIN_TRANSPORT_AGAIN;
  }
  errno = odin_xqc_map_error(ret);
  return ODIN_TRANSPORT_IO_ERROR;
}

static odin_transport_io_t odin_xqc_write(odin_transport_t *t, const void *buf,
                                          size_t len, size_t *out_n) {
  odin_xqc_stream_transport_t *s = (odin_xqc_stream_transport_t *)t;
  if (len == 0) {
    *out_n = 0;
    return ODIN_TRANSPORT_OK;
  }

  const ssize_t ret =
      odin_xqc_stream_send_call(s->stream, (unsigned char *)buf, len, 0);
  if (ret > 0) {
    if ((size_t)ret > len) {
      errno = EIO;
      return ODIN_TRANSPORT_IO_ERROR;
    }
    *out_n = (size_t)ret;
    return ODIN_TRANSPORT_OK;
  }
  if (ret == -XQC_EAGAIN || ret == 0) {
    return ODIN_TRANSPORT_AGAIN;
  }
  errno = odin_xqc_map_error(ret);
  return ODIN_TRANSPORT_IO_ERROR;
}

static int odin_xqc_shutdown_write(odin_transport_t *t) {
  odin_xqc_stream_transport_t *s = (odin_xqc_stream_transport_t *)t;
  if (s->fin_sent || s->fin_pending) {
    return 0;
  }
  const ssize_t ret = odin_xqc_stream_send_call(s->stream, NULL, 0, 1);
  if (ret >= 0) {
    s->fin_sent = 1;
    return 0;
  }
  if (ret == -XQC_EAGAIN) {
    s->fin_pending = 1;
    return 0;
  }
  errno = odin_xqc_map_error(ret);
  return -1;
}

static int odin_xqc_set_interest(odin_transport_t *t, unsigned int events) {
  if (events & ~(ODIN_TRANSPORT_READ | ODIN_TRANSPORT_WRITE)) {
    errno = EINVAL;
    return -1;
  }

  odin_xqc_stream_transport_t *s = (odin_xqc_stream_transport_t *)t;
  const unsigned int old_interest = s->interest;
  s->interest = events;
  if (events & ODIN_TRANSPORT_READ) {
    if (odin_xqc_deliver_pending_eof_ready_if_armed(s)) {
      return 0;
    }
  }
  if (events & ODIN_TRANSPORT_WRITE) {
    if (odin_xqc_deliver_write_kick_if_armed(s, old_interest)) {
      return 0;
    }
  }
  return 0;
}

static int odin_xqc_error(odin_transport_t *t) {
  odin_xqc_stream_transport_t *s = (odin_xqc_stream_transport_t *)t;
  return s->err;
}

static void odin_xqc_destroy(odin_transport_t *t) {
  odin_xqc_stream_transport_t *s = (odin_xqc_stream_transport_t *)t;
  odin_xqc_stream_set_user_data_call(s->stream, NULL);
  if (s->callback_depth != 0) {
    s->destroy_pending = 1;
    return;
  }
  free(s);
}

static const odin_transport_vtable_t odin_xqc_stream_transport_vtable = {
    odin_xqc_read,         odin_xqc_write, odin_xqc_shutdown_write,
    odin_xqc_set_interest, odin_xqc_error, odin_xqc_destroy,
};

int odin_xqc_stream_transport_create(xqc_stream_t *stream,
                                     odin_transport_ready_cb on_ready,
                                     void *user_data, odin_transport_t **out) {
#if defined(ODIN_TRANSPORT_XQC_TESTING)
  if (odin_xqc_test_fail_next_create_armed) {
    const int errnum = odin_xqc_test_fail_next_create_errno;
    odin_xqc_test_fail_next_create_armed = 0;
    odin_xqc_test_fail_next_create_errno = 0;
    errno = errnum;
    return -1;
  }
#endif
  odin_xqc_stream_transport_t *s =
      (odin_xqc_stream_transport_t *)calloc(1, sizeof(*s));
  if (s == NULL) {
    errno = ENOMEM;
    return -1;
  }
  s->base.vt = &odin_xqc_stream_transport_vtable;
  s->stream = stream;
  s->on_ready = on_ready;
  s->user_data = user_data;
  odin_xqc_stream_set_user_data_call(stream, &s->base);
  *out = &s->base;
  return 0;
}

xqc_int_t odin_xqc_stream_transport_read_notify(xqc_stream_t *stream,
                                                void *strm_user_data) {
  (void)stream;
  if (strm_user_data == NULL) {
    return XQC_OK;
  }
  odin_xqc_stream_transport_t *s =
      (odin_xqc_stream_transport_t *)strm_user_data;
  if (s->interest & ODIN_TRANSPORT_READ) {
    s->callback_depth += 1;
    s->read_ready_depth += 1;
    s->on_ready(&s->base, ODIN_TRANSPORT_READ, s->user_data);
    s->read_ready_depth -= 1;
    if (odin_xqc_end_callback(s)) {
      return XQC_OK;
    }
    (void)odin_xqc_deliver_pending_eof_ready_if_armed(s);
  }
  return XQC_OK;
}

xqc_int_t odin_xqc_stream_transport_write_notify(xqc_stream_t *stream,
                                                 void *strm_user_data) {
  (void)stream;
  if (strm_user_data == NULL) {
    return XQC_OK;
  }
  odin_xqc_stream_transport_t *s =
      (odin_xqc_stream_transport_t *)strm_user_data;
  if (s->fin_pending) {
    odin_xqc_retry_pending_fin(s);
    if (s->err != 0) {
      (void)odin_xqc_deliver_ready(s, ODIN_TRANSPORT_ERROR);
      return XQC_OK;
    }
  }
  if ((s->interest & ODIN_TRANSPORT_WRITE) && !s->destroy_pending) {
    (void)odin_xqc_deliver_ready(s, ODIN_TRANSPORT_WRITE);
  }
  return XQC_OK;
}

void odin_xqc_stream_transport_closing_notify(xqc_stream_t *stream,
                                              xqc_int_t err_code,
                                              void *strm_user_data) {
  (void)stream;
  (void)err_code;
  if (strm_user_data == NULL) {
    return;
  }
  odin_xqc_stream_transport_t *s =
      (odin_xqc_stream_transport_t *)strm_user_data;
  s->err = EPIPE;
  (void)odin_xqc_deliver_ready(s, ODIN_TRANSPORT_ERROR);
}
