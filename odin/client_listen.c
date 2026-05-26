/* odin/client_listen.c — Odin Client TCP listener + single-connection
 * CONNECT handshake (RFC-009 §4.2). */

#include "odin/client_listen.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "odin/http_connect.h"

int odin_client_listen_open(uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  int one = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
    int saved = errno;
    (void)close(fd);
    errno = saved;
    return -1;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
    int saved = errno;
    (void)close(fd);
    errno = saved;
    return -1;
  }

  if (listen(fd, 1) != 0) {
    int saved = errno;
    (void)close(fd);
    errno = saved;
    return -1;
  }

  return fd;
}

static int write_all(int fd, const char *bytes, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t r = write(fd, bytes + off, len - off);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (r == 0) {
      return -1;
    }
    off += (size_t)r;
  }
  return 0;
}

static int graceful_close(int fd) {
  if (shutdown(fd, SHUT_WR) != 0) {
    (void)close(fd);
    return -1;
  }
  uint8_t sink[256];
  for (;;) {
    ssize_t r = read(fd, sink, sizeof(sink));
    if (r > 0) {
      continue;
    }
    if (r == 0) {
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    break;
  }
  (void)close(fd);
  return 0;
}

odin_client_listen_status_t odin_client_listen_handshake(int conn_fd) {
  uint8_t buf[ODIN_HTTP_REQUEST_MAX];
  size_t n = 0;
  odin_http_status_t s = ODIN_HTTP_NEED_MORE;
  size_t consumed = 0;
  odin_http_connect_t cout;

  for (;;) {
    ssize_t r = read(conn_fd, buf + n, ODIN_HTTP_REQUEST_MAX - n);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      (void)close(conn_fd);
      return ODIN_CLIENT_LISTEN_IO_ERROR;
    }
    if (r == 0) {
      (void)close(conn_fd);
      return ODIN_CLIENT_LISTEN_PEER_CLOSED;
    }
    n += (size_t)r;
    s = odin_http_parse_connect(buf, n, &consumed, &cout);
    if (s != ODIN_HTTP_NEED_MORE) {
      break;
    }
  }

  odin_http_response_t resp = odin_http_response_for_status(s);
  if (write_all(conn_fd, resp.bytes, resp.len) != 0) {
    (void)close(conn_fd);
    return ODIN_CLIENT_LISTEN_IO_ERROR;
  }
  if (graceful_close(conn_fd) != 0) {
    return ODIN_CLIENT_LISTEN_IO_ERROR;
  }
  return (s == ODIN_HTTP_OK) ? ODIN_CLIENT_LISTEN_OK
                             : ODIN_CLIENT_LISTEN_REJECTED;
}
