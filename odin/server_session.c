/* odin/server_session.c -- RFC-020 per-connection orchestrator session.
 *
 * Wires together the RFC-013 fd transport, the RFC-018 SERVER-mode connect
 * session, the RFC-012 dial, and the RFC-014 relay so a caller-supplied
 * accepted nonblocking conn_fd runs end-to-end. A single trampoline
 * (server_session_ready) is installed as the downstream and (later) upstream
 * transports' on_ready and dispatches into odin_connect_session_drive while
 * the session is alive and into odin_relay_ready afterwards.
 */

#include "odin/server_session.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "odin/connect_session.h"
#include "odin/dial.h"
#include "odin/event_loop.h"
#include "odin/protocol.h"
#include "odin/relay.h"
#include "odin/transport.h"
#include "odin/transport_fd.h"

#if defined(ODIN_SERVER_SESSION_TESTING)
#include "odin/testing/server_session_internal_test.h"
#endif

enum {
  ODIN_SERVER_SESSION_S_HANDSHAKE = 1,
  ODIN_SERVER_SESSION_S_DIALING = 2,
  ODIN_SERVER_SESSION_S_WRITING_OK_RESP = 3,
  ODIN_SERVER_SESSION_S_WRITING_ERR_RESP = 4,
  ODIN_SERVER_SESSION_S_RELAY = 5,
  ODIN_SERVER_SESSION_S_TERMINAL = 6,
};

struct odin_server_session_t {
  odin_event_loop_t *loop;
  int conn_fd;
  int dial_fd;
  int state;
  int pending_dial_err;
  int active_depth;
  int destroy_pending;
  int on_close_fired;
  unsigned int connect_drive_depth;
  unsigned int pending_downstream_interest;
  int pending_downstream_interest_armed;
  odin_server_session_close_cb on_close;
  void *user_data;
  odin_server_session_dial_filter_cb dial_filter;
  void *dial_filter_ud;
  odin_transport_t *downstream_t;
  odin_transport_t *upstream_t;
  odin_connect_session_t *s;
  odin_dial_t *dial;
  odin_relay_t *relay;
#if defined(ODIN_SERVER_SESSION_TESTING)
  int fail_next_dial_armed;
  int fail_next_dial_errno;
  int fail_next_upstream_xport_armed;
  int fail_next_upstream_xport_errno;
  int fail_next_tail_write_armed;
  int fail_next_tail_write_errno;
  int fail_next_relay_create_armed;
  int fail_next_relay_create_errno;
  int fail_next_relay_start_armed;
  int fail_next_relay_start_errno;
  int inject_session_error_armed;
  int inject_session_error_errno;
  int pending_inject_err_armed;
  int pending_inject_err;
#endif
};

static void server_session_ready(odin_transport_t *t, unsigned int events,
                                 void *user_data);
static void session_on_req_decoded(odin_connect_session_t *s, void *user_data);
static void session_on_done(odin_connect_session_t *s,
                            odin_connect_session_status_t status, int err,
                            void *user_data);
static void dial_on_done(odin_dial_t *dial, odin_dial_status_t status, int fd,
                         int err, void *user_data);
static void relay_on_done(odin_relay_t *relay, odin_relay_status_t status,
                          int err, void *user_data);
static void handle_dial_result(odin_server_session_t *ss, int err);
static void fire_terminal(odin_server_session_t *ss, int err);
static void finish_destroy(odin_server_session_t *ss);

#if defined(ODIN_SERVER_SESSION_TESTING)
static unsigned int g_server_session_live_count;
#endif

static uint16_t map_dial_errno_to_resp_code(int err) {
  switch (err) {
  case ECONNREFUSED:
    return ODIN_SERVER_SESSION_RESP_CODE_ECONNREFUSED;
  case EHOSTUNREACH:
    return ODIN_SERVER_SESSION_RESP_CODE_EHOSTUNREACH;
  case ETIMEDOUT:
    return ODIN_SERVER_SESSION_RESP_CODE_ETIMEDOUT;
  default:
    return ODIN_SERVER_SESSION_RESP_CODE_OTHER;
  }
}

static void ss_enter(odin_server_session_t *ss) { ss->active_depth += 1; }

static void ss_leave(odin_server_session_t *ss) {
  ss->active_depth -= 1;
  if (ss->active_depth == 0 && ss->destroy_pending) {
    finish_destroy(ss);
  }
}

int odin_server_session_create(odin_event_loop_t *loop, int conn_fd,
                               odin_server_session_close_cb on_close,
                               void *user_data, odin_server_session_t **out) {
  odin_server_session_t *ss = (odin_server_session_t *)calloc(1, sizeof(*ss));
  if (ss == NULL) {
    errno = ENOMEM;
    return -1;
  }
  ss->loop = loop;
  ss->conn_fd = conn_fd;
  ss->dial_fd = -1;
  ss->state = ODIN_SERVER_SESSION_S_HANDSHAKE;
  ss->on_close = on_close;
  ss->user_data = user_data;

  if (odin_fd_transport_create(loop, conn_fd, server_session_ready, ss,
                               &ss->downstream_t) != 0) {
    const int saved = errno;
    free(ss);
    errno = saved;
    return -1;
  }
  if (odin_connect_session_create_server(session_on_req_decoded,
                                         session_on_done, ss, &ss->s) != 0) {
    const int saved = errno;
    odin_transport_destroy(ss->downstream_t);
    free(ss);
    errno = saved;
    return -1;
  }
  if (odin_transport_set_interest(ss->downstream_t, ODIN_TRANSPORT_READ) != 0) {
    const int saved = errno;
    odin_connect_session_destroy(ss->s);
    odin_transport_destroy(ss->downstream_t);
    free(ss);
    errno = saved;
    return -1;
  }
#if defined(ODIN_SERVER_SESSION_TESTING)
  g_server_session_live_count += 1;
#endif
  *out = ss;
  return 0;
}

int odin_server_session_create_with_transport(
    odin_event_loop_t *loop,
    odin_server_session_transport_factory_cb create_downstream,
    void *factory_user_data, odin_server_session_close_cb on_close,
    void *user_data, odin_server_session_t **out) {
  if (loop == NULL || create_downstream == NULL || on_close == NULL ||
      out == NULL) {
    errno = EINVAL;
    return -1;
  }
  odin_server_session_t *ss = (odin_server_session_t *)calloc(1, sizeof(*ss));
  if (ss == NULL) {
    errno = ENOMEM;
    return -1;
  }
  ss->loop = loop;
  ss->conn_fd = -1;
  ss->dial_fd = -1;
  ss->state = ODIN_SERVER_SESSION_S_HANDSHAKE;
  ss->on_close = on_close;
  ss->user_data = user_data;

  if (create_downstream(server_session_ready, ss, factory_user_data,
                        &ss->downstream_t) != 0) {
    const int saved = errno;
    free(ss);
    errno = saved;
    return -1;
  }
  if (odin_connect_session_create_server(session_on_req_decoded,
                                         session_on_done, ss, &ss->s) != 0) {
    const int saved = errno;
    odin_transport_destroy(ss->downstream_t);
    free(ss);
    errno = saved;
    return -1;
  }
  if (odin_transport_set_interest(ss->downstream_t, ODIN_TRANSPORT_READ) != 0) {
    const int saved = errno;
    odin_connect_session_destroy(ss->s);
    odin_transport_destroy(ss->downstream_t);
    free(ss);
    errno = saved;
    return -1;
  }
#if defined(ODIN_SERVER_SESSION_TESTING)
  g_server_session_live_count += 1;
#endif
  *out = ss;
  return 0;
}

void odin_server_session_set_dial_filter(odin_server_session_t *ss,
                                         odin_server_session_dial_filter_cb cb,
                                         void *user_data) {
  if (ss == NULL) {
    return;
  }
  ss->dial_filter = cb;
  ss->dial_filter_ud = user_data;
}

void odin_server_session_destroy(odin_server_session_t *ss) {
  if (ss == NULL) {
    return;
  }
  if (ss->active_depth != 0) {
    ss->destroy_pending = 1;
    return;
  }
  finish_destroy(ss);
}

static void finish_destroy(odin_server_session_t *ss) {
  if (ss->relay != NULL) {
    odin_relay_destroy(ss->relay);
    ss->relay = NULL;
  }
  if (ss->dial != NULL) {
    odin_dial_destroy(ss->dial);
    ss->dial = NULL;
  }
  if (ss->s != NULL) {
    odin_connect_session_destroy(ss->s);
    ss->s = NULL;
  }
  if (ss->upstream_t != NULL) {
    odin_transport_destroy(ss->upstream_t);
    ss->upstream_t = NULL;
  }
  if (ss->downstream_t != NULL) {
    odin_transport_destroy(ss->downstream_t);
    ss->downstream_t = NULL;
  }
  if (ss->dial_fd >= 0) {
    (void)close(ss->dial_fd);
    ss->dial_fd = -1;
  }
  if (ss->conn_fd >= 0) {
    (void)close(ss->conn_fd);
    ss->conn_fd = -1;
  }
#if defined(ODIN_SERVER_SESSION_TESTING)
  g_server_session_live_count -= 1;
#endif
  free(ss);
}

static void server_session_ready(odin_transport_t *t, unsigned int events,
                                 void *user_data) {
  odin_server_session_t *ss = (odin_server_session_t *)user_data;
  ss_enter(ss);
  if (ss->on_close_fired) {
    ss_leave(ss);
    return;
  }
  if (ss->s != NULL) {
    ss->connect_drive_depth += 1;
    const odin_connect_session_drive_t d =
        odin_connect_session_drive(ss->s, t, events);
    ss->connect_drive_depth -= 1;
    if (ss->s != NULL && d == ODIN_CONNECT_SESSION_DRIVE_CONTINUE) {
      unsigned int mask = 0;
      if (ss->pending_downstream_interest_armed) {
        mask = ss->pending_downstream_interest;
        ss->pending_downstream_interest = 0;
        ss->pending_downstream_interest_armed = 0;
      } else {
        mask = odin_connect_session_wants(ss->s);
      }
      (void)odin_transport_set_interest(t, mask);
    }
    ss_leave(ss);
    return;
  }
  if (ss->relay != NULL) {
    odin_relay_ready(t, events, ss->relay);
    ss_leave(ss);
    return;
  }
  ss_leave(ss);
}

#if defined(ODIN_SERVER_SESSION_TESTING)
static void deferred_inject_session_error_task(odin_event_loop_t *loop,
                                               void *user_data) {
  (void)loop;
  odin_server_session_t *ss = (odin_server_session_t *)user_data;
  ss_enter(ss);
  if (ss->on_close_fired) {
    ss_leave(ss);
    return;
  }
  if (!ss->pending_inject_err_armed) {
    ss_leave(ss);
    return;
  }
  const int err = ss->pending_inject_err;
  ss->pending_inject_err_armed = 0;
  ss->pending_inject_err = 0;
  fire_terminal(ss, err);
  ss_leave(ss);
}
#endif

static void session_on_req_decoded(odin_connect_session_t *s, void *user_data) {
  odin_server_session_t *ss = (odin_server_session_t *)user_data;

#if defined(ODIN_SERVER_SESSION_TESTING)
  if (ss->fail_next_dial_armed) {
    const int errnum = ss->fail_next_dial_errno;
    ss->fail_next_dial_armed = 0;
    ss->fail_next_dial_errno = 0;
    handle_dial_result(ss, errnum);
    return;
  }
#endif

  const char *host_ptr = NULL;
  size_t host_len = 0;
  odin_connect_session_server_host(s, &host_ptr, &host_len);
  const uint16_t port = odin_connect_session_server_port(s);

  uint8_t host_cstr[ODIN_PROTO_HOST_MAX + 1];
  if (host_len > ODIN_PROTO_HOST_MAX) {
    /* Decoder enforces host_len <= 255; defensive cap keeps the buffer write
     * bounded should the contract ever loosen. */
    host_len = ODIN_PROTO_HOST_MAX;
  }
  memcpy(host_cstr, host_ptr, host_len);
  host_cstr[host_len] = '\0';

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  const int rc = inet_pton(AF_INET, (const char *)host_cstr, &sa.sin_addr);
  if (rc != 1) {
    handle_dial_result(ss, EHOSTUNREACH);
    return;
  }

  if (ss->dial_filter != NULL) {
    const int filter_err = ss->dial_filter((const struct sockaddr *)&sa,
                                           sizeof(sa), ss->dial_filter_ud);
    if (filter_err != 0) {
      handle_dial_result(ss, filter_err);
      return;
    }
  }

  if (odin_dial_start(ss->loop, (const struct sockaddr *)&sa, sizeof(sa),
                      dial_on_done, ss, &ss->dial) != 0) {
    const int saved = errno;
    handle_dial_result(ss, saved);
    return;
  }
  ss->state = ODIN_SERVER_SESSION_S_DIALING;

#if defined(ODIN_SERVER_SESSION_TESTING)
  if (ss->inject_session_error_armed) {
    const int errnum = ss->inject_session_error_errno;
    ss->inject_session_error_armed = 0;
    ss->inject_session_error_errno = 0;
    ss->pending_inject_err_armed = 1;
    ss->pending_inject_err = errnum;
    (void)odin_event_post(ss->loop, deferred_inject_session_error_task, ss);
  }
#endif
}

static void dial_on_done(odin_dial_t *dial, odin_dial_status_t status, int fd,
                         int err, void *user_data) {
  (void)dial;
  odin_server_session_t *ss = (odin_server_session_t *)user_data;
  ss_enter(ss);
  if (ss->on_close_fired) {
    ss_leave(ss);
    return;
  }
  odin_dial_destroy(ss->dial);
  ss->dial = NULL;
  if (status == ODIN_DIAL_OK) {
    ss->dial_fd = fd;
#if defined(ODIN_SERVER_SESSION_TESTING)
    if (ss->fail_next_upstream_xport_armed) {
      const int errnum = ss->fail_next_upstream_xport_errno;
      ss->fail_next_upstream_xport_armed = 0;
      ss->fail_next_upstream_xport_errno = 0;
      (void)close(ss->dial_fd);
      ss->dial_fd = -1;
      handle_dial_result(ss, errnum);
      ss_leave(ss);
      return;
    }
#endif
    if (odin_fd_transport_create(ss->loop, ss->dial_fd, server_session_ready,
                                 ss, &ss->upstream_t) != 0) {
      const int saved = errno;
      (void)close(ss->dial_fd);
      ss->dial_fd = -1;
      handle_dial_result(ss, saved);
      ss_leave(ss);
      return;
    }
    handle_dial_result(ss, 0);
    ss_leave(ss);
    return;
  }
  handle_dial_result(ss, err);
  ss_leave(ss);
}

static void handle_dial_result(odin_server_session_t *ss, int err) {
  if (err == 0) {
    ss->state = ODIN_SERVER_SESSION_S_WRITING_OK_RESP;
    odin_connect_session_server_set_error_code(ss->s, 0);
  } else {
    ss->state = ODIN_SERVER_SESSION_S_WRITING_ERR_RESP;
    ss->pending_dial_err = err;
    odin_connect_session_server_set_error_code(
        ss->s, map_dial_errno_to_resp_code(err));
  }
  const unsigned int mask = odin_connect_session_wants(ss->s);
  if (ss->connect_drive_depth != 0) {
    ss->pending_downstream_interest = mask;
    ss->pending_downstream_interest_armed = 1;
    return;
  }
  (void)odin_transport_set_interest(ss->downstream_t, mask);
}

static void session_on_done(odin_connect_session_t *s,
                            odin_connect_session_status_t status, int err,
                            void *user_data) {
  (void)s;
  odin_server_session_t *ss = (odin_server_session_t *)user_data;
  if (status == ODIN_CONNECT_SESSION_ERROR) {
    fire_terminal(ss, err);
    return;
  }
  assert(ss->state == ODIN_SERVER_SESSION_S_WRITING_OK_RESP ||
         ss->state == ODIN_SERVER_SESSION_S_WRITING_ERR_RESP);
  if (ss->state == ODIN_SERVER_SESSION_S_WRITING_OK_RESP) {
    const uint8_t *tail_ptr = NULL;
    size_t tail_len = 0;
    odin_connect_session_server_tail(ss->s, &tail_ptr, &tail_len);
    if (tail_len > 0) {
#if defined(ODIN_SERVER_SESSION_TESTING)
      if (ss->fail_next_tail_write_armed) {
        const int errnum = ss->fail_next_tail_write_errno;
        ss->fail_next_tail_write_armed = 0;
        ss->fail_next_tail_write_errno = 0;
        fire_terminal(ss, errnum);
        return;
      }
#endif
      size_t n = 0;
      const odin_transport_io_t io =
          odin_transport_write(ss->upstream_t, tail_ptr, tail_len, &n);
      switch (io) {
      case ODIN_TRANSPORT_OK:
        if (n != tail_len) {
          fire_terminal(ss, EAGAIN);
          return;
        }
        break;
      case ODIN_TRANSPORT_AGAIN:
        fire_terminal(ss, EAGAIN);
        return;
      case ODIN_TRANSPORT_EOF:
        fire_terminal(ss, EPIPE);
        return;
      case ODIN_TRANSPORT_IO_ERROR: {
        const int saved = errno;
        fire_terminal(ss, saved);
        return;
      }
      }
    }
    odin_connect_session_destroy(ss->s);
    ss->s = NULL;
#if defined(ODIN_SERVER_SESSION_TESTING)
    if (ss->fail_next_relay_create_armed) {
      const int errnum = ss->fail_next_relay_create_errno;
      ss->fail_next_relay_create_armed = 0;
      ss->fail_next_relay_create_errno = 0;
      fire_terminal(ss, errnum);
      return;
    }
#endif
    if (odin_relay_create(relay_on_done, ss, &ss->relay) != 0) {
      const int saved = errno;
      fire_terminal(ss, saved);
      return;
    }
#if defined(ODIN_SERVER_SESSION_TESTING)
    if (ss->fail_next_relay_start_armed) {
      const int errnum = ss->fail_next_relay_start_errno;
      ss->fail_next_relay_start_armed = 0;
      ss->fail_next_relay_start_errno = 0;
      fire_terminal(ss, errnum);
      return;
    }
#endif
    if (odin_relay_start(ss->relay, ss->downstream_t, ss->upstream_t) != 0) {
      const int saved = errno;
      fire_terminal(ss, saved);
      return;
    }
    ss->state = ODIN_SERVER_SESSION_S_RELAY;
    return;
  }
  /* S_WRITING_ERR_RESP: error RESP flushed; close with the latched dial err. */
  fire_terminal(ss, ss->pending_dial_err);
}

static void relay_on_done(odin_relay_t *relay, odin_relay_status_t status,
                          int err, void *user_data) {
  (void)relay;
  odin_server_session_t *ss = (odin_server_session_t *)user_data;
  const int e = (status == ODIN_RELAY_OK) ? 0 : err;
  fire_terminal(ss, e);
}

static void fire_terminal(odin_server_session_t *ss, int err) {
  if (ss->on_close_fired) {
    return;
  }
  ss->on_close_fired = 1;
  ss->state = ODIN_SERVER_SESSION_S_TERMINAL;
  if (ss->relay != NULL) {
    odin_relay_destroy(ss->relay);
    ss->relay = NULL;
  }
  if (ss->dial != NULL) {
    odin_dial_destroy(ss->dial);
    ss->dial = NULL;
  }
  if (ss->s != NULL) {
    odin_connect_session_destroy(ss->s);
    ss->s = NULL;
  }
  if (ss->upstream_t != NULL) {
    odin_transport_destroy(ss->upstream_t);
    ss->upstream_t = NULL;
  }
  if (ss->downstream_t != NULL) {
    odin_transport_destroy(ss->downstream_t);
    ss->downstream_t = NULL;
  }
  if (ss->dial_fd >= 0) {
    (void)close(ss->dial_fd);
    ss->dial_fd = -1;
  }
  if (ss->conn_fd >= 0) {
    (void)close(ss->conn_fd);
    ss->conn_fd = -1;
  }
  const odin_server_session_close_cb cb = ss->on_close;
  void *const ud = ss->user_data;
  cb(ss, err, ud);
  /* Do NOT free(ss) here. The outermost ss_enter/ss_leave frame runs
   * finish_destroy if destroy_pending was set inside cb. */
}

#if defined(ODIN_SERVER_SESSION_TESTING)

int odin_server_session_test_fail_next_dial(odin_server_session_t *ss,
                                            int errnum) {
  if (ss == NULL) {
    errno = EINVAL;
    return -1;
  }
  ss->fail_next_dial_armed = 1;
  ss->fail_next_dial_errno = errnum;
  return 0;
}

int odin_server_session_test_fail_next_upstream_transport_create(
    odin_server_session_t *ss, int errnum) {
  if (ss == NULL) {
    errno = EINVAL;
    return -1;
  }
  ss->fail_next_upstream_xport_armed = 1;
  ss->fail_next_upstream_xport_errno = errnum;
  return 0;
}

int odin_server_session_test_fail_next_tail_write(odin_server_session_t *ss,
                                                  int errnum) {
  if (ss == NULL) {
    errno = EINVAL;
    return -1;
  }
  ss->fail_next_tail_write_armed = 1;
  ss->fail_next_tail_write_errno = errnum;
  return 0;
}

int odin_server_session_test_fail_next_relay_create(odin_server_session_t *ss,
                                                    int errnum) {
  if (ss == NULL) {
    errno = EINVAL;
    return -1;
  }
  ss->fail_next_relay_create_armed = 1;
  ss->fail_next_relay_create_errno = errnum;
  return 0;
}

int odin_server_session_test_fail_next_relay_start(odin_server_session_t *ss,
                                                   int errnum) {
  if (ss == NULL) {
    errno = EINVAL;
    return -1;
  }
  ss->fail_next_relay_start_armed = 1;
  ss->fail_next_relay_start_errno = errnum;
  return 0;
}

int odin_server_session_test_inject_session_error_on_dial(
    odin_server_session_t *ss, int errnum) {
  if (ss == NULL) {
    errno = EINVAL;
    return -1;
  }
  ss->inject_session_error_armed = 1;
  ss->inject_session_error_errno = errnum;
  return 0;
}

int odin_server_session_test_state(const odin_server_session_t *ss) {
  if (ss == NULL) {
    return 0;
  }
  return ss->state;
}

unsigned int odin_server_session_test_live_count(void) {
  return g_server_session_live_count;
}

#endif /* defined(ODIN_SERVER_SESSION_TESTING) */
