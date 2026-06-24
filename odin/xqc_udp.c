/* odin/xqc_udp.c — RFC-017 xquic UDP event driver. */

#include "odin/xqc_udp.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>

#if defined(ODIN_XQC_UDP_TESTING)
#include "odin/testing/xqc_udp_internal_test.h"
#endif

struct odin_xqc_udp_t {
  odin_event_loop_t *loop;
  odin_udp_t *udp;
  xqc_engine_t *engine;
  odin_event_timer_t *timer;
  xqc_engine_callback_t engine_callbacks;
  xqc_transport_callbacks_t transport_callbacks;
  void *app_user_data;
  struct sockaddr_storage local_addr;
  socklen_t local_addrlen;
  xqc_cid_t *registered_cids;
  size_t registered_count;
  size_t registered_cap;
  int started;
  int write_blocked;
  int destroy_requested;
  int callback_depth;
  int last_udp_errno;
  int last_timer_errno;
};

#if defined(ODIN_XQC_UDP_TESTING)
static odin_xqc_udp_test_ops_t g_xqc_udp_test_ops;

void odin_xqc_udp_test_set_ops(const odin_xqc_udp_test_ops_t *ops) {
  if (ops == NULL) {
    memset(&g_xqc_udp_test_ops, 0, sizeof(g_xqc_udp_test_ops));
    return;
  }
  g_xqc_udp_test_ops = *ops;
}
#endif

static xqc_engine_t *xqc_udp_engine_create_call(
    xqc_engine_type_t engine_type, const xqc_config_t *engine_config,
    const xqc_engine_ssl_config_t *ssl_config,
    const xqc_engine_callback_t *engine_callback,
    const xqc_transport_callbacks_t *transport_cbs, void *user_data) {
#if defined(ODIN_XQC_UDP_TESTING)
  if (g_xqc_udp_test_ops.engine_create != NULL) {
    return g_xqc_udp_test_ops.engine_create(engine_type, engine_config,
                                            ssl_config, engine_callback,
                                            transport_cbs, user_data);
  }
#endif
  return xqc_engine_create(engine_type, engine_config, ssl_config,
                           engine_callback, transport_cbs, user_data);
}

static void xqc_udp_engine_destroy_call(xqc_engine_t *engine) {
#if defined(ODIN_XQC_UDP_TESTING)
  if (g_xqc_udp_test_ops.engine_destroy != NULL) {
    g_xqc_udp_test_ops.engine_destroy(engine);
    return;
  }
#endif
  xqc_engine_destroy(engine);
}

static xqc_int_t xqc_udp_packet_process_call(
    xqc_engine_t *engine, const unsigned char *packet_in_buf,
    size_t packet_in_size, const struct sockaddr *local_addr,
    socklen_t local_addrlen, const struct sockaddr *peer_addr,
    socklen_t peer_addrlen, xqc_usec_t recv_time, void *user_data) {
#if defined(ODIN_XQC_UDP_TESTING)
  if (g_xqc_udp_test_ops.packet_process != NULL) {
    return g_xqc_udp_test_ops.packet_process(
        engine, packet_in_buf, packet_in_size, local_addr, local_addrlen,
        peer_addr, peer_addrlen, recv_time, user_data);
  }
#endif
  return xqc_engine_packet_process(engine, packet_in_buf, packet_in_size,
                                   local_addr, local_addrlen, peer_addr,
                                   peer_addrlen, recv_time, user_data);
}

static void xqc_udp_finish_recv_call(xqc_engine_t *engine) {
#if defined(ODIN_XQC_UDP_TESTING)
  if (g_xqc_udp_test_ops.finish_recv != NULL) {
    g_xqc_udp_test_ops.finish_recv(engine);
    return;
  }
#endif
  xqc_engine_finish_recv(engine);
}

static void xqc_udp_main_logic_call(xqc_engine_t *engine) {
#if defined(ODIN_XQC_UDP_TESTING)
  if (g_xqc_udp_test_ops.main_logic != NULL) {
    g_xqc_udp_test_ops.main_logic(engine);
    return;
  }
#endif
  xqc_engine_main_logic(engine);
}

static xqc_int_t xqc_udp_conn_continue_send_call(xqc_engine_t *engine,
                                                 const xqc_cid_t *cid) {
#if defined(ODIN_XQC_UDP_TESTING)
  if (g_xqc_udp_test_ops.conn_continue_send != NULL) {
    return g_xqc_udp_test_ops.conn_continue_send(engine, cid);
  }
#endif
  return xqc_conn_continue_send(engine, cid);
}

/* Driver-installed default for engine_callbacks.monotonic_ts when caller
 * leaves the slot null. xquic's xqc_monotonic_timestamp is set from this
 * during xqc_engine_create, so the helper that fills recv_time and xquic's
 * internal "now" share one source. */
static xqc_usec_t odin_xqc_udp_default_monotonic_us(void) {
#if defined(ODIN_XQC_UDP_TESTING)
  if (g_xqc_udp_test_ops.now_us != NULL) {
    return g_xqc_udp_test_ops.now_us();
  }
#endif
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return (xqc_usec_t)ts.tv_sec * 1000000u +
         (xqc_usec_t)((unsigned long)ts.tv_nsec / 1000u);
}

static xqc_usec_t odin_xqc_udp_monotonic_us(odin_xqc_udp_t *xu) {
  return xu->engine_callbacks.monotonic_ts();
}

static void odin_xqc_udp_enter_xqc(odin_xqc_udp_t *xu) {
  xu->callback_depth += 1;
}

static int odin_xqc_udp_finish_destroy(odin_xqc_udp_t *xu);

static int odin_xqc_udp_leave_xqc(odin_xqc_udp_t *xu) {
  xu->callback_depth -= 1;
  if (xu->callback_depth == 0 && xu->destroy_requested && xu->udp != NULL) {
    return odin_xqc_udp_finish_destroy(xu);
  }
  return 0;
}

static int odin_xqc_udp_update_udp_interest(odin_xqc_udp_t *xu) {
  unsigned int mask = 0;
  if (xu->started) {
    mask |= ODIN_UDP_READ;
  }
  if (xu->write_blocked && xu->started) {
    mask |= ODIN_UDP_WRITE;
  }
  if (odin_udp_set_interest(xu->udp, mask) != 0) {
    xu->last_udp_errno = errno;
    return -1;
  }
  return 0;
}

static void odin_xqc_udp_on_timer(odin_event_loop_t *loop,
                                  odin_event_timer_t *timer, void *user_data) {
  (void)loop;
  odin_xqc_udp_t *xu = (odin_xqc_udp_t *)user_data;
  if (xu->destroy_requested) {
    return;
  }
  if (xu->timer == timer) {
    odin_event_timer_stop(timer);
  }
  xu->timer = NULL;
  odin_xqc_udp_enter_xqc(xu);
  if (xu->engine != NULL) {
    xqc_udp_main_logic_call(xu->engine);
  }
  (void)odin_xqc_udp_leave_xqc(xu);
}

static void odin_xqc_udp_set_event_timer(xqc_usec_t wake_after,
                                         void *engine_user_data) {
  odin_xqc_udp_t *xu = (odin_xqc_udp_t *)engine_user_data;
  if (xu == NULL || xu->destroy_requested) {
    return;
  }
  if (xu->timer != NULL) {
    if (odin_event_timer_reset(xu->timer, wake_after, 0) == 0) {
      return;
    }
    odin_event_timer_stop(xu->timer);
    xu->timer = NULL;
  }
  if (odin_event_timer_start(xu->loop, wake_after, 0, odin_xqc_udp_on_timer, xu,
                             &xu->timer) != 0) {
    xu->last_timer_errno = errno;
    xu->timer = NULL;
  }
}

static ssize_t odin_xqc_udp_send_datagram(odin_xqc_udp_t *xu,
                                          const unsigned char *buf, size_t size,
                                          const struct sockaddr *peer_addr,
                                          socklen_t peer_addrlen,
                                          int recoverable) {
  if (xu == NULL || xu->destroy_requested || peer_addr == NULL) {
    return XQC_SOCKET_ERROR;
  }
  size_t sent = 0;
  const odin_udp_io_t rc =
      odin_udp_send(xu->udp, buf, size, &sent, peer_addr, peer_addrlen);
  if (rc == ODIN_UDP_OK && sent == size) {
    return (ssize_t)size;
  }
  if (rc == ODIN_UDP_AGAIN) {
    if (!recoverable) {
      return XQC_SOCKET_ERROR;
    }
    xu->write_blocked = 1;
    if (xu->started) {
      const int saved = errno;
      if (odin_xqc_udp_update_udp_interest(xu) != 0) {
        xu->write_blocked = 0;
        return XQC_SOCKET_ERROR;
      }
      errno = saved;
    }
    return XQC_SOCKET_EAGAIN;
  }
  return XQC_SOCKET_ERROR;
}

static ssize_t odin_xqc_udp_stateless_reset(
    const unsigned char *buf, size_t size, const struct sockaddr *peer_addr,
    socklen_t peer_addrlen, const struct sockaddr *local_addr,
    socklen_t local_addrlen, void *user_data) {
  (void)local_addr;
  (void)local_addrlen;
  return odin_xqc_udp_send_datagram((odin_xqc_udp_t *)user_data, buf, size,
                                    peer_addr, peer_addrlen, 0);
}

static ssize_t odin_xqc_udp_write_socket(const unsigned char *buf, size_t size,
                                         const struct sockaddr *peer_addr,
                                         socklen_t peer_addrlen,
                                         void *conn_user_data) {
  return odin_xqc_udp_send_datagram((odin_xqc_udp_t *)conn_user_data, buf, size,
                                    peer_addr, peer_addrlen, 1);
}

static ssize_t
odin_xqc_udp_write_socket_ex(uint64_t path_id, const unsigned char *buf,
                             size_t size, const struct sockaddr *peer_addr,
                             socklen_t peer_addrlen, void *conn_user_data) {
  (void)path_id;
  return odin_xqc_udp_send_datagram((odin_xqc_udp_t *)conn_user_data, buf, size,
                                    peer_addr, peer_addrlen, 1);
}

static ssize_t odin_xqc_udp_conn_send_packet_before_accept(
    const unsigned char *buf, size_t size, const struct sockaddr *peer_addr,
    socklen_t peer_addrlen, void *conn_user_data) {
  return odin_xqc_udp_send_datagram((odin_xqc_udp_t *)conn_user_data, buf, size,
                                    peer_addr, peer_addrlen, 0);
}

static int odin_xqc_udp_cid_is_registered(odin_xqc_udp_t *xu,
                                          const xqc_cid_t *cid) {
  for (size_t i = 0; i < xu->registered_count; ++i) {
    if (xqc_cid_is_equal(&xu->registered_cids[i], cid) == 0) {
      return 1;
    }
  }
  return 0;
}

static int odin_xqc_udp_handle_udp_write_ready(odin_xqc_udp_t *xu) {
  if (!xu->write_blocked) {
    (void)odin_xqc_udp_update_udp_interest(xu);
    return 0;
  }
  xu->write_blocked = 0;
  if (odin_xqc_udp_update_udp_interest(xu) != 0) {
    xu->write_blocked = 1;
    return 0;
  }
  const size_t count = xu->registered_count;
  xqc_cid_t *snapshot = NULL;
  if (count > 0) {
    snapshot = (xqc_cid_t *)malloc(count * sizeof(*snapshot));
    if (snapshot == NULL) {
      return 0;
    }
    memcpy(snapshot, xu->registered_cids, count * sizeof(*snapshot));
  }
  for (size_t i = 0; i < count; ++i) {
    if (xu->destroy_requested) {
      break;
    }
    if (!odin_xqc_udp_cid_is_registered(xu, &snapshot[i])) {
      continue;
    }
    odin_xqc_udp_enter_xqc(xu);
    (void)xqc_udp_conn_continue_send_call(xu->engine, &snapshot[i]);
    if (odin_xqc_udp_leave_xqc(xu) != 0) {
      free(snapshot);
      return 1;
    }
  }
  free(snapshot);
  return 0;
}

static void odin_xqc_udp_on_udp_ready(odin_udp_t *u, unsigned int events,
                                      void *user_data) {
  (void)u;
  odin_xqc_udp_t *xu = (odin_xqc_udp_t *)user_data;
  if (xu->destroy_requested) {
    return;
  }
  if (events & ODIN_UDP_ERROR) {
    xu->last_udp_errno = EIO;
  }
  if (events & ODIN_UDP_WRITE) {
    if (odin_xqc_udp_handle_udp_write_ready(xu) != 0) {
      return;
    }
  }
  if (!(events & ODIN_UDP_READ)) {
    return;
  }
  unsigned char packet[ODIN_XQC_UDP_PACKET_CAP];
  unsigned int processed = 0;
  while (processed < ODIN_XQC_UDP_RECV_BATCH_MAX) {
    struct sockaddr_storage peer;
    socklen_t peer_len = sizeof(peer);
    size_t n = 0;
    const odin_udp_io_t rc = odin_udp_recv(xu->udp, packet, sizeof(packet), &n,
                                           (struct sockaddr *)&peer, &peer_len);
    if (rc == ODIN_UDP_AGAIN) {
      break;
    }
    if (rc == ODIN_UDP_IO_ERROR) {
      xu->last_udp_errno = errno;
      break;
    }
    odin_xqc_udp_enter_xqc(xu);
    (void)xqc_udp_packet_process_call(
        xu->engine, packet, n, (struct sockaddr *)&xu->local_addr,
        xu->local_addrlen, (struct sockaddr *)&peer, peer_len,
        odin_xqc_udp_monotonic_us(xu), xu);
    if (odin_xqc_udp_leave_xqc(xu) != 0) {
      return;
    }
    processed += 1;
    if (xu->destroy_requested) {
      return;
    }
  }
  if (processed != 0 && !xu->destroy_requested) {
    odin_xqc_udp_enter_xqc(xu);
    xqc_udp_finish_recv_call(xu->engine);
    if (odin_xqc_udp_leave_xqc(xu) != 0) {
      return;
    }
  }
}

int odin_xqc_udp_create(const odin_xqc_udp_config_t *config,
                        odin_xqc_udp_t **out) {
  if (config == NULL || config->loop == NULL || config->local_addr == NULL ||
      config->engine_callbacks == NULL || config->transport_callbacks == NULL ||
      out == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (config->engine_config != NULL &&
      config->engine_config->sendmmsg_on != 0) {
    errno = ENOTSUP;
    return -1;
  }

  odin_xqc_udp_t *xu = (odin_xqc_udp_t *)calloc(1, sizeof(*xu));
  if (xu == NULL) {
    errno = ENOMEM;
    return -1;
  }
  xu->loop = config->loop;
  xu->app_user_data = config->app_user_data;
  xu->engine_callbacks = *config->engine_callbacks;
  xu->engine_callbacks.set_event_timer = odin_xqc_udp_set_event_timer;
  if (xu->engine_callbacks.monotonic_ts == NULL) {
    xu->engine_callbacks.monotonic_ts = odin_xqc_udp_default_monotonic_us;
  }
  xu->transport_callbacks = *config->transport_callbacks;
  xu->transport_callbacks.stateless_reset = odin_xqc_udp_stateless_reset;
  xu->transport_callbacks.write_socket = odin_xqc_udp_write_socket;
  xu->transport_callbacks.write_socket_ex = odin_xqc_udp_write_socket_ex;
  xu->transport_callbacks.conn_send_packet_before_accept =
      odin_xqc_udp_conn_send_packet_before_accept;
  xu->transport_callbacks.write_mmsg = NULL;
  xu->transport_callbacks.write_mmsg_ex = NULL;

  if (odin_udp_open(config->loop, config->local_addr, config->local_addrlen,
                    odin_xqc_udp_on_udp_ready, xu, &xu->udp) != 0) {
    const int saved = errno;
    free(xu);
    errno = saved;
    return -1;
  }
  xu->local_addrlen = sizeof(xu->local_addr);
  if (odin_udp_local_addr(xu->udp, (struct sockaddr *)&xu->local_addr,
                          &xu->local_addrlen) != 0) {
    const int saved = errno;
    odin_udp_close(xu->udp);
    free(xu);
    errno = saved;
    return -1;
  }

  xqc_engine_t *engine = xqc_udp_engine_create_call(
      config->engine_type, config->engine_config, config->ssl_config,
      &xu->engine_callbacks, &xu->transport_callbacks, xu);
  if (engine == NULL) {
    const int saved = errno;
    if (xu->timer != NULL) {
      odin_event_timer_stop(xu->timer);
      xu->timer = NULL;
    }
    odin_udp_close(xu->udp);
    free(xu->registered_cids);
    free(xu);
    errno = saved != 0 ? saved : EIO;
    return -1;
  }
  xu->engine = engine;
  *out = xu;
  return 0;
}

int odin_xqc_udp_start(odin_xqc_udp_t *xu) {
  if (xu->started) {
    return 0;
  }
  unsigned int mask = ODIN_UDP_READ;
  if (xu->write_blocked) {
    mask |= ODIN_UDP_WRITE;
  }
  if (odin_udp_set_interest(xu->udp, mask) != 0) {
    return -1;
  }
  xu->started = 1;
  return 0;
}

int odin_xqc_udp_stop(odin_xqc_udp_t *xu) {
  if (!xu->started && xu->timer == NULL && !xu->write_blocked) {
    return 0;
  }
  xu->write_blocked = 0;
  (void)odin_udp_set_interest(xu->udp, 0);
  xu->started = 0;
  if (xu->timer != NULL) {
    odin_event_timer_stop(xu->timer);
    xu->timer = NULL;
  }
  return 0;
}

static int odin_xqc_udp_finish_destroy(odin_xqc_udp_t *xu) {
  if (xu->timer != NULL) {
    odin_event_timer_stop(xu->timer);
    xu->timer = NULL;
  }
  if (xu->udp != NULL) {
    odin_udp_close(xu->udp);
    xu->udp = NULL;
  }
  if (xu->engine != NULL) {
    xqc_udp_engine_destroy_call(xu->engine);
    xu->engine = NULL;
  }
  free(xu->registered_cids);
  free(xu);
  return 1;
}

void odin_xqc_udp_destroy(odin_xqc_udp_t *xu) {
  if (xu == NULL) {
    return;
  }
  if (xu->destroy_requested) {
    return;
  }
  xu->destroy_requested = 1;
  (void)odin_xqc_udp_stop(xu);
  if (xu->callback_depth != 0) {
    return;
  }
  (void)odin_xqc_udp_finish_destroy(xu);
}

xqc_engine_t *odin_xqc_udp_engine(odin_xqc_udp_t *xu) { return xu->engine; }

void *odin_xqc_udp_xqc_user_data(odin_xqc_udp_t *xu) { return xu; }

void *odin_xqc_udp_app_user_data(odin_xqc_udp_t *xu) {
  return xu->app_user_data;
}

int odin_xqc_udp_local_addr(odin_xqc_udp_t *xu, struct sockaddr *addr,
                            socklen_t *addrlen) {
  if (xu == NULL || addr == NULL || addrlen == NULL) {
    errno = EINVAL;
    return -1;
  }
  const socklen_t required = xu->local_addrlen;
  if (*addrlen < required) {
    *addrlen = required;
    errno = ENOBUFS;
    return -1;
  }
  memcpy(addr, &xu->local_addr, required);
  *addrlen = required;
  return 0;
}

int odin_xqc_udp_register_conn(odin_xqc_udp_t *xu, const xqc_cid_t *cid) {
  if (xu == NULL || cid == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (odin_xqc_udp_cid_is_registered(xu, cid)) {
    return 0;
  }
  if (xu->registered_count == xu->registered_cap) {
    const size_t new_cap = xu->registered_cap == 0 ? 4 : xu->registered_cap * 2;
    xqc_cid_t *grown = (xqc_cid_t *)realloc(
        xu->registered_cids, new_cap * sizeof(*xu->registered_cids));
    if (grown == NULL) {
      errno = ENOMEM;
      return -1;
    }
    xu->registered_cids = grown;
    xu->registered_cap = new_cap;
  }
  xu->registered_cids[xu->registered_count] = *cid;
  xu->registered_count += 1;
  return 0;
}

void odin_xqc_udp_unregister_conn(odin_xqc_udp_t *xu, const xqc_cid_t *cid) {
  if (xu == NULL || cid == NULL) {
    return;
  }
  for (size_t i = 0; i < xu->registered_count; ++i) {
    if (xqc_cid_is_equal(&xu->registered_cids[i], cid) == 0) {
      if (i + 1 < xu->registered_count) {
        xu->registered_cids[i] = xu->registered_cids[xu->registered_count - 1];
      }
      xu->registered_count -= 1;
      return;
    }
  }
}

#if defined(ODIN_XQC_UDP_TESTING)
int odin_xqc_udp_test_udp(odin_xqc_udp_t *xu, odin_udp_t **out) {
  if (xu == NULL || xu->udp == NULL || out == NULL) {
    errno = ENOENT;
    return -1;
  }
  *out = xu->udp;
  return 0;
}

int odin_xqc_udp_test_timer_active(odin_xqc_udp_t *xu) {
  return xu != NULL && xu->timer != NULL;
}

int odin_xqc_udp_test_destroy_requested(odin_xqc_udp_t *xu) {
  return xu != NULL && xu->destroy_requested;
}

int odin_xqc_udp_test_write_blocked(odin_xqc_udp_t *xu) {
  return xu != NULL && xu->write_blocked;
}

int odin_xqc_udp_test_last_timer_errno(odin_xqc_udp_t *xu) {
  return xu != NULL ? xu->last_timer_errno : 0;
}

xqc_timestamp_pt odin_xqc_udp_test_engine_monotonic_ts(odin_xqc_udp_t *xu) {
  return xu != NULL ? xu->engine_callbacks.monotonic_ts : NULL;
}
#endif
