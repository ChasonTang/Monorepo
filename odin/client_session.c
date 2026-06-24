/* odin/client_session.c -- RFC-023 local-client orchestrator session. */

#include "odin/client_session.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "odin/connect_session.h"
#include "odin/dial.h"
#include "odin/http_connect.h"
#include "odin/protocol.h"
#include "odin/relay.h"
#include "odin/transport.h"
#include "odin/transport_fd.h"

#if defined(ODIN_CLIENT_SESSION_TESTING)
#include "odin/testing/client_session_internal_test.h"
#include "odin/testing/event_loop_internal_test.h"

static int g_client_session_fail_next_create_errno;
#endif

enum {
  ODIN_CLIENT_SESSION_S_PARSING = 1,
  ODIN_CLIENT_SESSION_S_DIALING = 2,
  ODIN_CLIENT_SESSION_S_HANDSHAKE = 3,
  ODIN_CLIENT_SESSION_S_WRITING_OK_HTTP = 4,
  ODIN_CLIENT_SESSION_S_WRITING_ERR_HTTP = 5,
  ODIN_CLIENT_SESSION_S_RELAY = 6,
  ODIN_CLIENT_SESSION_S_TERMINAL = 7,
};

struct odin_client_session_t {
  odin_event_loop_t *loop;
  int conn_fd;
  int dial_fd;
  int state;
  int pending_err;
  int active_depth;
  int destroy_pending;
  int on_close_fired;
  odin_client_session_close_cb on_close;
  void *user_data;
  odin_client_session_dial_filter_cb dial_filter;
  void *dial_filter_ud;
  struct sockaddr_in server_sa;
  odin_transport_t *downstream_t;
  odin_transport_t *upstream_t;
  odin_connect_session_t *s;
  odin_dial_t *dial;
  odin_relay_t *relay;
  uint8_t http_buf[ODIN_HTTP_REQUEST_MAX];
  size_t http_buf_used;
  size_t http_consumed;
  odin_http_connect_t http_view;
  odin_http_response_t http_resp;
  size_t http_resp_off;
#if defined(ODIN_CLIENT_SESSION_TESTING)
  int fail_next_dial_armed;
  int fail_next_dial_errno;
  int fail_next_upstream_transport_create_armed;
  int fail_next_upstream_transport_create_errno;
  int fail_next_connect_session_create_armed;
  int fail_next_connect_session_create_errno;
  int fail_next_http_parse_tail_write_armed;
  int fail_next_http_parse_tail_write_errno;
  int fail_next_client_tail_write_armed;
  int fail_next_client_tail_write_errno;
  int fail_next_relay_create_armed;
  int fail_next_relay_create_errno;
  int fail_next_relay_start_armed;
  int fail_next_relay_start_errno;
  int arm_next_kqueue_read_fault_at_relay_start_armed;
  int arm_next_kqueue_read_fault_at_relay_start_errno;
#endif
};

static void client_session_ready(odin_transport_t *t, unsigned int events,
                                 void *user_data);
static void dial_on_done(odin_dial_t *dial, odin_dial_status_t status, int fd,
                         int err, void *user_data);
static void session_on_done(odin_connect_session_t *s,
                            odin_connect_session_status_t status, int err,
                            void *user_data);
static void relay_on_done(odin_relay_t *relay, odin_relay_status_t status,
                          int err, void *user_data);
static void finish_destroy(odin_client_session_t *cs);
static void fire_terminal(odin_client_session_t *cs, int err);
static void drive_parse_http(odin_client_session_t *cs, unsigned int events);
static void start_dial(odin_client_session_t *cs);
static void handle_failure(odin_client_session_t *cs, odin_http_status_t status,
                           int err);
static void drive_write_http_resp(odin_client_session_t *cs,
                                  unsigned int events);

static int client_resp_code_to_errno(uint16_t error_code) {
  switch (error_code) {
  case 0x0001:
    return ECONNREFUSED;
  case 0x0002:
    return EHOSTUNREACH;
  case 0x0003:
    return ETIMEDOUT;
  case 0x0004:
    return EIO;
  default:
    return EPROTO;
  }
}

static void cs_enter(odin_client_session_t *cs) { cs->active_depth += 1; }

static void cs_leave(odin_client_session_t *cs) {
  cs->active_depth -= 1;
  if (cs->active_depth == 0 && cs->destroy_pending) {
    finish_destroy(cs);
  }
}

int odin_client_session_create(odin_event_loop_t *loop, int conn_fd,
                               const char *server_host, size_t server_host_len,
                               uint16_t server_port,
                               odin_client_session_close_cb on_close,
                               void *user_data, odin_client_session_t **out) {
#if defined(ODIN_CLIENT_SESSION_TESTING)
  if (g_client_session_fail_next_create_errno != 0) {
    const int err = g_client_session_fail_next_create_errno;
    g_client_session_fail_next_create_errno = 0;
    errno = err;
    return -1;
  }
#endif
  if (server_host_len == 0 || server_host_len > ODIN_PROTO_HOST_MAX) {
    errno = EINVAL;
    return -1;
  }

  char server_host_cstr[ODIN_PROTO_HOST_MAX + 1];
  memcpy(server_host_cstr, server_host, server_host_len);
  server_host_cstr[server_host_len] = '\0';

  struct sockaddr_in server_sa;
  memset(&server_sa, 0, sizeof(server_sa));
  server_sa.sin_family = AF_INET;
  server_sa.sin_port = htons(server_port);
  if (inet_pton(AF_INET, server_host_cstr, &server_sa.sin_addr) != 1) {
    errno = EINVAL;
    return -1;
  }

  odin_client_session_t *cs = (odin_client_session_t *)calloc(1, sizeof(*cs));
  if (cs == NULL) {
    errno = ENOMEM;
    return -1;
  }
  cs->loop = loop;
  cs->conn_fd = conn_fd;
  cs->dial_fd = -1;
  cs->state = ODIN_CLIENT_SESSION_S_PARSING;
  cs->on_close = on_close;
  cs->user_data = user_data;
  cs->server_sa = server_sa;

  if (odin_fd_transport_create(loop, conn_fd, client_session_ready, cs,
                               &cs->downstream_t) != 0) {
    const int saved = errno;
    free(cs);
    errno = saved;
    return -1;
  }
  if (odin_transport_set_interest(cs->downstream_t, ODIN_TRANSPORT_READ) != 0) {
    const int saved = errno;
    odin_transport_destroy(cs->downstream_t);
    free(cs);
    errno = saved;
    return -1;
  }

  *out = cs;
  return 0;
}

void odin_client_session_set_dial_filter(odin_client_session_t *cs,
                                         odin_client_session_dial_filter_cb cb,
                                         void *user_data) {
  if (cs == NULL) {
    return;
  }
  cs->dial_filter = cb;
  cs->dial_filter_ud = user_data;
}

void odin_client_session_destroy(odin_client_session_t *cs) {
  if (cs == NULL) {
    return;
  }
  if (cs->active_depth != 0) {
    cs->destroy_pending = 1;
    return;
  }
  finish_destroy(cs);
}

static void finish_destroy(odin_client_session_t *cs) {
  if (cs->relay != NULL) {
    odin_relay_destroy(cs->relay);
    cs->relay = NULL;
  }
  if (cs->dial != NULL) {
    odin_dial_destroy(cs->dial);
    cs->dial = NULL;
  }
  if (cs->s != NULL) {
    odin_connect_session_destroy(cs->s);
    cs->s = NULL;
  }
  if (cs->upstream_t != NULL) {
    odin_transport_destroy(cs->upstream_t);
    cs->upstream_t = NULL;
  }
  if (cs->downstream_t != NULL) {
    odin_transport_destroy(cs->downstream_t);
    cs->downstream_t = NULL;
  }
  if (cs->dial_fd >= 0) {
    (void)close(cs->dial_fd);
    cs->dial_fd = -1;
  }
  if (cs->conn_fd >= 0) {
    (void)close(cs->conn_fd);
    cs->conn_fd = -1;
  }
  free(cs);
}

static void client_session_ready(odin_transport_t *t, unsigned int events,
                                 void *user_data) {
  odin_client_session_t *cs = (odin_client_session_t *)user_data;
  cs_enter(cs);
  if (cs->on_close_fired) {
    cs_leave(cs);
    return;
  }
  if (cs->state == ODIN_CLIENT_SESSION_S_PARSING) {
    drive_parse_http(cs, events);
    cs_leave(cs);
    return;
  }
  if (cs->state == ODIN_CLIENT_SESSION_S_WRITING_OK_HTTP ||
      cs->state == ODIN_CLIENT_SESSION_S_WRITING_ERR_HTTP) {
    drive_write_http_resp(cs, events);
    cs_leave(cs);
    return;
  }
  if (cs->s != NULL) {
    const odin_connect_session_drive_t d =
        odin_connect_session_drive(cs->s, t, events);
    if (cs->s != NULL && d == ODIN_CONNECT_SESSION_DRIVE_CONTINUE) {
      (void)odin_transport_set_interest(t, odin_connect_session_wants(cs->s));
    }
    cs_leave(cs);
    return;
  }
  if (cs->relay != NULL) {
    odin_relay_ready(t, events, cs->relay);
    cs_leave(cs);
    return;
  }
  cs_leave(cs);
}

static void drive_parse_http(odin_client_session_t *cs, unsigned int events) {
  if ((events & ODIN_TRANSPORT_ERROR) != 0) {
    const int err = odin_transport_error(cs->downstream_t);
    if (err != 0) {
      fire_terminal(cs, err);
      return;
    }
  }
  if ((events & ODIN_TRANSPORT_READ) == 0) {
    return;
  }

  for (;;) {
    size_t n = 0;
    const odin_transport_io_t io =
        odin_transport_read(cs->downstream_t, cs->http_buf + cs->http_buf_used,
                            ODIN_HTTP_REQUEST_MAX - cs->http_buf_used, &n);
    switch (io) {
    case ODIN_TRANSPORT_OK:
      cs->http_buf_used += n;
      break;
    case ODIN_TRANSPORT_AGAIN:
      return;
    case ODIN_TRANSPORT_EOF:
      fire_terminal(cs, ECONNRESET);
      return;
    case ODIN_TRANSPORT_IO_ERROR: {
      const int saved = errno;
      fire_terminal(cs, saved);
      return;
    }
    }

    odin_http_connect_t view;
    size_t consumed = 0;
    const odin_http_status_t st = odin_http_parse_connect(
        cs->http_buf, cs->http_buf_used, &consumed, &view);
    if (st == ODIN_HTTP_OK) {
      cs->http_consumed = consumed;
      cs->http_view = view;
      start_dial(cs);
      return;
    }
    if (st == ODIN_HTTP_NEED_MORE) {
      if (cs->http_buf_used == ODIN_HTTP_REQUEST_MAX) {
        handle_failure(cs, ODIN_HTTP_ERR_REQUEST_TOO_LARGE, EPROTO);
        return;
      }
      continue;
    }
    handle_failure(cs, st, EPROTO);
    return;
  }
}

static void start_dial(odin_client_session_t *cs) {
  (void)odin_transport_set_interest(cs->downstream_t, 0);
  if (cs->dial_filter != NULL) {
    const int filter_err =
        cs->dial_filter((const struct sockaddr *)&cs->server_sa,
                        sizeof(cs->server_sa), cs->dial_filter_ud);
    if (filter_err != 0) {
      handle_failure(cs, ODIN_HTTP_ERR_BAD_REQUEST_TARGET, filter_err);
      return;
    }
  }

#if defined(ODIN_CLIENT_SESSION_TESTING)
  if (cs->fail_next_dial_armed) {
    const int errnum = cs->fail_next_dial_errno;
    cs->fail_next_dial_armed = 0;
    cs->fail_next_dial_errno = 0;
    handle_failure(cs, ODIN_HTTP_ERR_BAD_REQUEST_TARGET, errnum);
    return;
  }
#endif

  if (odin_dial_start(cs->loop, (const struct sockaddr *)&cs->server_sa,
                      sizeof(cs->server_sa), dial_on_done, cs,
                      &cs->dial) != 0) {
    const int saved = errno;
    handle_failure(cs, ODIN_HTTP_ERR_BAD_REQUEST_TARGET, saved);
    return;
  }
  cs->state = ODIN_CLIENT_SESSION_S_DIALING;
}

static void dial_on_done(odin_dial_t *dial, odin_dial_status_t status, int fd,
                         int err, void *user_data) {
  (void)dial;
  odin_client_session_t *cs = (odin_client_session_t *)user_data;
  cs_enter(cs);
  if (cs->on_close_fired) {
    cs_leave(cs);
    return;
  }
  odin_dial_destroy(cs->dial);
  cs->dial = NULL;
  if (status == ODIN_DIAL_ERROR) {
    handle_failure(cs, ODIN_HTTP_ERR_BAD_REQUEST_TARGET, err);
    cs_leave(cs);
    return;
  }

  cs->dial_fd = fd;
#if defined(ODIN_CLIENT_SESSION_TESTING)
  if (cs->fail_next_upstream_transport_create_armed) {
    const int errnum = cs->fail_next_upstream_transport_create_errno;
    cs->fail_next_upstream_transport_create_armed = 0;
    cs->fail_next_upstream_transport_create_errno = 0;
    (void)close(cs->dial_fd);
    cs->dial_fd = -1;
    handle_failure(cs, ODIN_HTTP_ERR_BAD_REQUEST_TARGET, errnum);
    cs_leave(cs);
    return;
  }
#endif
  if (odin_fd_transport_create(cs->loop, cs->dial_fd, client_session_ready, cs,
                               &cs->upstream_t) != 0) {
    const int saved = errno;
    (void)close(cs->dial_fd);
    cs->dial_fd = -1;
    handle_failure(cs, ODIN_HTTP_ERR_BAD_REQUEST_TARGET, saved);
    cs_leave(cs);
    return;
  }

#if defined(ODIN_CLIENT_SESSION_TESTING)
  if (cs->fail_next_connect_session_create_armed) {
    const int errnum = cs->fail_next_connect_session_create_errno;
    cs->fail_next_connect_session_create_armed = 0;
    cs->fail_next_connect_session_create_errno = 0;
    odin_transport_destroy(cs->upstream_t);
    cs->upstream_t = NULL;
    (void)close(cs->dial_fd);
    cs->dial_fd = -1;
    handle_failure(cs, ODIN_HTTP_ERR_BAD_REQUEST_TARGET, errnum);
    cs_leave(cs);
    return;
  }
#endif

  if (odin_connect_session_create_client(
          (const char *)(cs->http_buf + cs->http_view.host_off),
          cs->http_view.host_len, cs->http_view.port, session_on_done, cs,
          &cs->s) != 0) {
    const int saved = errno;
    odin_transport_destroy(cs->upstream_t);
    cs->upstream_t = NULL;
    (void)close(cs->dial_fd);
    cs->dial_fd = -1;
    handle_failure(cs, ODIN_HTTP_ERR_BAD_REQUEST_TARGET, saved);
    cs_leave(cs);
    return;
  }
  cs->state = ODIN_CLIENT_SESSION_S_HANDSHAKE;
  (void)odin_transport_set_interest(cs->upstream_t,
                                    odin_connect_session_wants(cs->s));
  cs_leave(cs);
}

static void session_on_done(odin_connect_session_t *s,
                            odin_connect_session_status_t status, int err,
                            void *user_data) {
  (void)s;
  odin_client_session_t *cs = (odin_client_session_t *)user_data;
  if (status == ODIN_CONNECT_SESSION_ERROR) {
    handle_failure(cs, ODIN_HTTP_ERR_BAD_REQUEST_TARGET, err);
    return;
  }
  const uint16_t error_code = odin_connect_session_client_error_code(cs->s);
  if (error_code != 0) {
    handle_failure(cs, ODIN_HTTP_ERR_BAD_REQUEST_TARGET,
                   client_resp_code_to_errno(error_code));
    return;
  }
  cs->state = ODIN_CLIENT_SESSION_S_WRITING_OK_HTTP;
  cs->http_resp = odin_http_response_for_status(ODIN_HTTP_OK);
  cs->http_resp_off = 0;
  (void)odin_transport_set_interest(cs->upstream_t, 0);
  (void)odin_transport_set_interest(cs->downstream_t, ODIN_TRANSPORT_WRITE);
}

static void handle_failure(odin_client_session_t *cs, odin_http_status_t status,
                           int err) {
  cs->state = ODIN_CLIENT_SESSION_S_WRITING_ERR_HTTP;
  cs->pending_err = err;
  cs->http_resp = odin_http_response_for_status(status);
  cs->http_resp_off = 0;
  if (cs->upstream_t != NULL) {
    (void)odin_transport_set_interest(cs->upstream_t, 0);
  }
  (void)odin_transport_set_interest(cs->downstream_t, ODIN_TRANSPORT_WRITE);
}

static int write_one_shot_or_terminal(odin_client_session_t *cs,
                                      odin_transport_t *t, const void *buf,
                                      size_t len, int eof_errno) {
  size_t n = 0;
  const odin_transport_io_t io = odin_transport_write(t, buf, len, &n);
  switch (io) {
  case ODIN_TRANSPORT_OK:
    if (n != len) {
      fire_terminal(cs, EAGAIN);
      return -1;
    }
    return 0;
  case ODIN_TRANSPORT_AGAIN:
    fire_terminal(cs, EAGAIN);
    return -1;
  case ODIN_TRANSPORT_EOF:
    fire_terminal(cs, eof_errno);
    return -1;
  case ODIN_TRANSPORT_IO_ERROR: {
    const int saved = errno;
    fire_terminal(cs, saved);
    return -1;
  }
  }
  return 0;
}

static void drive_write_http_resp(odin_client_session_t *cs,
                                  unsigned int events) {
  if ((events & ODIN_TRANSPORT_ERROR) != 0) {
    const int err = odin_transport_error(cs->downstream_t);
    if (err != 0) {
      fire_terminal(cs, err);
      return;
    }
  }
  if ((events & ODIN_TRANSPORT_WRITE) == 0) {
    return;
  }

  while (cs->http_resp_off < cs->http_resp.len) {
    size_t n = 0;
    const odin_transport_io_t io = odin_transport_write(
        cs->downstream_t, cs->http_resp.bytes + cs->http_resp_off,
        cs->http_resp.len - cs->http_resp_off, &n);
    switch (io) {
    case ODIN_TRANSPORT_OK:
      cs->http_resp_off += n;
      break;
    case ODIN_TRANSPORT_AGAIN:
      return;
    case ODIN_TRANSPORT_EOF:
      fire_terminal(cs, EPIPE);
      return;
    case ODIN_TRANSPORT_IO_ERROR: {
      const int saved = errno;
      fire_terminal(cs, saved);
      return;
    }
    }
  }

  if (cs->state == ODIN_CLIENT_SESSION_S_WRITING_ERR_HTTP) {
    if (cs->s != NULL) {
      odin_connect_session_destroy(cs->s);
      cs->s = NULL;
    }
    fire_terminal(cs, cs->pending_err);
    return;
  }

  const uint8_t *client_tail = NULL;
  size_t client_tail_len = 0;
  odin_connect_session_client_tail(cs->s, &client_tail, &client_tail_len);
  if (client_tail_len > 0) {
#if defined(ODIN_CLIENT_SESSION_TESTING)
    if (cs->fail_next_client_tail_write_armed) {
      const int errnum = cs->fail_next_client_tail_write_errno;
      cs->fail_next_client_tail_write_armed = 0;
      cs->fail_next_client_tail_write_errno = 0;
      fire_terminal(cs, errnum);
      return;
    }
#endif
    if (write_one_shot_or_terminal(cs, cs->downstream_t, client_tail,
                                   client_tail_len, EPIPE) != 0) {
      return;
    }
  }

  if (cs->http_buf_used > cs->http_consumed) {
#if defined(ODIN_CLIENT_SESSION_TESTING)
    if (cs->fail_next_http_parse_tail_write_armed) {
      const int errnum = cs->fail_next_http_parse_tail_write_errno;
      cs->fail_next_http_parse_tail_write_armed = 0;
      cs->fail_next_http_parse_tail_write_errno = 0;
      fire_terminal(cs, errnum);
      return;
    }
#endif
    if (write_one_shot_or_terminal(
            cs, cs->upstream_t, cs->http_buf + cs->http_consumed,
            cs->http_buf_used - cs->http_consumed, EPIPE) != 0) {
      return;
    }
  }

  odin_connect_session_destroy(cs->s);
  cs->s = NULL;
#if defined(ODIN_CLIENT_SESSION_TESTING)
  if (cs->fail_next_relay_create_armed) {
    const int errnum = cs->fail_next_relay_create_errno;
    cs->fail_next_relay_create_armed = 0;
    cs->fail_next_relay_create_errno = 0;
    fire_terminal(cs, errnum);
    return;
  }
#endif
  if (odin_relay_create(relay_on_done, cs, &cs->relay) != 0) {
    const int saved = errno;
    fire_terminal(cs, saved);
    return;
  }
#if defined(ODIN_CLIENT_SESSION_TESTING)
  if (cs->fail_next_relay_start_armed) {
    const int errnum = cs->fail_next_relay_start_errno;
    cs->fail_next_relay_start_armed = 0;
    cs->fail_next_relay_start_errno = 0;
    fire_terminal(cs, errnum);
    return;
  }
  if (cs->arm_next_kqueue_read_fault_at_relay_start_armed) {
    const int errnum = cs->arm_next_kqueue_read_fault_at_relay_start_errno;
    cs->arm_next_kqueue_read_fault_at_relay_start_armed = 0;
    cs->arm_next_kqueue_read_fault_at_relay_start_errno = 0;
    (void)odin_event_loop_test_fail_next_kqueue_change(
        cs->loop, ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_ADD, ODIN_EVENT_READ,
        errnum);
  }
#endif
  if (odin_relay_start(cs->relay, cs->downstream_t, cs->upstream_t) != 0) {
    const int saved = errno;
    fire_terminal(cs, saved);
    return;
  }
  cs->state = ODIN_CLIENT_SESSION_S_RELAY;
}

static void relay_on_done(odin_relay_t *relay, odin_relay_status_t status,
                          int err, void *user_data) {
  (void)relay;
  odin_client_session_t *cs = (odin_client_session_t *)user_data;
  const int e = (status == ODIN_RELAY_OK) ? 0 : err;
  fire_terminal(cs, e);
}

static void fire_terminal(odin_client_session_t *cs, int err) {
  if (cs->on_close_fired) {
    return;
  }
  cs->on_close_fired = 1;
  cs->state = ODIN_CLIENT_SESSION_S_TERMINAL;
  if (cs->relay != NULL) {
    odin_relay_destroy(cs->relay);
    cs->relay = NULL;
  }
  if (cs->dial != NULL) {
    odin_dial_destroy(cs->dial);
    cs->dial = NULL;
  }
  if (cs->s != NULL) {
    odin_connect_session_destroy(cs->s);
    cs->s = NULL;
  }
  if (cs->upstream_t != NULL) {
    odin_transport_destroy(cs->upstream_t);
    cs->upstream_t = NULL;
  }
  if (cs->downstream_t != NULL) {
    odin_transport_destroy(cs->downstream_t);
    cs->downstream_t = NULL;
  }
  if (cs->dial_fd >= 0) {
    (void)close(cs->dial_fd);
    cs->dial_fd = -1;
  }
  if (cs->conn_fd >= 0) {
    (void)close(cs->conn_fd);
    cs->conn_fd = -1;
  }
  const odin_client_session_close_cb cb = cs->on_close;
  void *const ud = cs->user_data;
  cb(cs, err, ud);
}

#if defined(ODIN_CLIENT_SESSION_TESTING)

int odin_client_session_test_fail_next_dial(odin_client_session_t *cs,
                                            int errnum) {
  if (cs == NULL) {
    errno = EINVAL;
    return -1;
  }
  cs->fail_next_dial_armed = 1;
  cs->fail_next_dial_errno = errnum;
  return 0;
}

int odin_client_session_test_fail_next_upstream_transport_create(
    odin_client_session_t *cs, int errnum) {
  if (cs == NULL) {
    errno = EINVAL;
    return -1;
  }
  cs->fail_next_upstream_transport_create_armed = 1;
  cs->fail_next_upstream_transport_create_errno = errnum;
  return 0;
}

int odin_client_session_test_fail_next_connect_session_create(
    odin_client_session_t *cs, int errnum) {
  if (cs == NULL) {
    errno = EINVAL;
    return -1;
  }
  cs->fail_next_connect_session_create_armed = 1;
  cs->fail_next_connect_session_create_errno = errnum;
  return 0;
}

int odin_client_session_test_fail_next_http_parse_tail_write(
    odin_client_session_t *cs, int errnum) {
  if (cs == NULL) {
    errno = EINVAL;
    return -1;
  }
  cs->fail_next_http_parse_tail_write_armed = 1;
  cs->fail_next_http_parse_tail_write_errno = errnum;
  return 0;
}

int odin_client_session_test_fail_next_client_tail_write(
    odin_client_session_t *cs, int errnum) {
  if (cs == NULL) {
    errno = EINVAL;
    return -1;
  }
  cs->fail_next_client_tail_write_armed = 1;
  cs->fail_next_client_tail_write_errno = errnum;
  return 0;
}

int odin_client_session_test_fail_next_relay_create(odin_client_session_t *cs,
                                                    int errnum) {
  if (cs == NULL) {
    errno = EINVAL;
    return -1;
  }
  cs->fail_next_relay_create_armed = 1;
  cs->fail_next_relay_create_errno = errnum;
  return 0;
}

int odin_client_session_test_fail_next_relay_start(odin_client_session_t *cs,
                                                   int errnum) {
  if (cs == NULL) {
    errno = EINVAL;
    return -1;
  }
  cs->fail_next_relay_start_armed = 1;
  cs->fail_next_relay_start_errno = errnum;
  return 0;
}

int odin_client_session_test_arm_next_kqueue_read_fault_at_relay_start(
    odin_client_session_t *cs, int errnum) {
  if (cs == NULL) {
    errno = EINVAL;
    return -1;
  }
  cs->arm_next_kqueue_read_fault_at_relay_start_armed = 1;
  cs->arm_next_kqueue_read_fault_at_relay_start_errno = errnum;
  return 0;
}

int odin_client_session_test_state(const odin_client_session_t *cs) {
  if (cs == NULL) {
    return 0;
  }
  return cs->state;
}

int odin_client_session_test_fail_next_create(int errnum) {
  if (errnum <= 0) {
    errno = EINVAL;
    return -1;
  }
  g_client_session_fail_next_create_errno = errnum;
  return 0;
}

#endif /* defined(ODIN_CLIENT_SESSION_TESTING) */
