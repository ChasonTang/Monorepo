#include "odin/udp.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(ODIN_UDP_TESTING)
#include "odin/udp_internal_test.h"
#endif

struct odin_udp_t {
  odin_event_loop_t *loop;
  int fd;
  odin_event_io_t *io;
  unsigned int cur;
  odin_udp_ready_cb on_ready;
  void *user_data;
#if defined(ODIN_UDP_TESTING)
  int fail_sendto_errno;
#endif
};

static void udp_on_io(odin_event_loop_t *loop, odin_event_io_t *io, int fd,
                      unsigned int events, void *user_data);

static int set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int odin_udp_open(odin_event_loop_t *loop, const struct sockaddr *addr,
                  socklen_t addrlen, odin_udp_ready_cb on_ready,
                  void *user_data, odin_udp_t **out) {
  if (addr->sa_family != AF_INET && addr->sa_family != AF_INET6) {
    errno = EAFNOSUPPORT;
    return -1;
  }

  odin_udp_t *u = (odin_udp_t *)calloc(1, sizeof(*u));
  if (u == NULL) {
    errno = ENOMEM;
    return -1;
  }

  u->fd = -1;
  u->fd = socket(addr->sa_family, SOCK_DGRAM, IPPROTO_UDP);
  if (u->fd < 0) {
    const int saved = errno;
    free(u);
    errno = saved;
    return -1;
  }
  if (set_nonblocking(u->fd) != 0) {
    const int saved = errno;
    (void)close(u->fd);
    free(u);
    errno = saved;
    return -1;
  }
  if (bind(u->fd, addr, addrlen) != 0) {
    const int saved = errno;
    (void)close(u->fd);
    free(u);
    errno = saved;
    return -1;
  }

  u->loop = loop;
  u->io = NULL;
  u->cur = 0;
  u->on_ready = on_ready;
  u->user_data = user_data;
  *out = u;
  return 0;
}

odin_udp_io_t odin_udp_recv(odin_udp_t *u, void *buf, size_t len, size_t *out_n,
                            struct sockaddr *src, socklen_t *srclen) {
  const ssize_t n = recvfrom(u->fd, buf, len, 0, src, srclen);
  if (n >= 0) {
    *out_n = (size_t)n;
    return ODIN_UDP_OK;
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
    return ODIN_UDP_AGAIN;
  }
  return ODIN_UDP_IO_ERROR;
}

odin_udp_io_t odin_udp_send(odin_udp_t *u, const void *buf, size_t len,
                            size_t *out_n, const struct sockaddr *dst,
                            socklen_t dstlen) {
#if defined(ODIN_UDP_TESTING)
  if (u->fail_sendto_errno != 0) {
    const int err = u->fail_sendto_errno;
    u->fail_sendto_errno = 0;
    errno = err;
    return ODIN_UDP_AGAIN;
  }
#endif
  const ssize_t n = sendto(u->fd, buf, len, 0, dst, dstlen);
  if (n >= 0) {
    *out_n = (size_t)n;
    return ODIN_UDP_OK;
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
    return ODIN_UDP_AGAIN;
  }
  return ODIN_UDP_IO_ERROR;
}

int odin_udp_set_interest(odin_udp_t *u, unsigned int events) {
  if ((events & ~(ODIN_UDP_READ | ODIN_UDP_WRITE)) != 0) {
    errno = EINVAL;
    return -1;
  }

  unsigned int ev = 0;
  if (events & ODIN_UDP_READ) {
    ev |= ODIN_EVENT_READ;
  }
  if (events & ODIN_UDP_WRITE) {
    ev |= ODIN_EVENT_WRITE;
  }
  if (ev == 0) {
    if (u->io != NULL) {
      odin_event_io_stop(u->io);
      u->io = NULL;
    }
    u->cur = 0;
    return 0;
  }
  if (u->io == NULL) {
    odin_event_io_t *new_io = NULL;
    if (odin_event_io_start(u->loop, u->fd, ev, udp_on_io, u, &new_io) != 0) {
      return -1;
    }
    u->io = new_io;
  } else if (ev != u->cur) {
    if (odin_event_io_update(u->io, ev) != 0) {
      return -1;
    }
  }
  u->cur = ev;
  return 0;
}

static void udp_on_io(odin_event_loop_t *loop, odin_event_io_t *io, int fd,
                      unsigned int events, void *user_data) {
  (void)loop;
  (void)io;
  (void)fd;
  odin_udp_t *u = (odin_udp_t *)user_data;
  unsigned int ev = 0;
  if (events & ODIN_EVENT_READ) {
    ev |= ODIN_UDP_READ;
  }
  if (events & ODIN_EVENT_WRITE) {
    ev |= ODIN_UDP_WRITE;
  }
  if (events & ODIN_EVENT_ERROR) {
    ev |= ODIN_UDP_ERROR;
  }
  u->on_ready(u, ev, u->user_data);
}

void odin_udp_close(odin_udp_t *u) {
  if (u == NULL) {
    return;
  }
  if (u->io != NULL) {
    odin_event_io_stop(u->io);
    u->io = NULL;
  }
  if (u->fd >= 0) {
    (void)close(u->fd);
    u->fd = -1;
  }
  free(u);
}

#if defined(ODIN_UDP_TESTING)
int odin_udp_test_fd(odin_udp_t *u) {
  if (u == NULL || u->fd < 0) {
    errno = ENOENT;
    return -1;
  }
  return u->fd;
}

int odin_udp_test_io(odin_udp_t *u, odin_event_io_t **out) {
  if (u == NULL || u->io == NULL) {
    errno = ENOENT;
    return -1;
  }
  *out = u->io;
  return 0;
}

int odin_udp_test_fail_next_sendto(odin_udp_t *u, int errnum) {
  if (errnum != EAGAIN && errnum != EWOULDBLOCK && errnum != EINTR) {
    errno = EINVAL;
    return -1;
  }
  u->fail_sendto_errno = errnum;
  return 0;
}
#endif
