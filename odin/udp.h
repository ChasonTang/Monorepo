/* odin/udp.h
 *
 * Single-thread, event-loop-driven nonblocking UDP endpoint.
 *
 * odin_udp_open creates and owns one nonblocking IP UDP SOCK_DGRAM socket for
 * the caller's AF_INET or AF_INET6 local sockaddr, binds it, writes *out on
 * success, and registers no watch until odin_udp_set_interest is called. Local
 * setup failures return -1 with errno set, write nothing to *out, leave no
 * socket open, and leave no event-loop registration. Unsupported non-IP
 * families are rejected with EAFNOSUPPORT.
 *
 * odin_udp_recv is a positive receive-capacity operation: buf/out_n/src/srclen
 * are non-null, len > 0, and *srclen is the source-address capacity. It
 * performs one recvfrom, copies at most len payload bytes and at most *srclen
 * address bytes, returns ODIN_UDP_OK with *out_n for one datagram, returns
 * ODIN_UDP_AGAIN for EAGAIN/EWOULDBLOCK/EINTR, and returns
 * ODIN_UDP_IO_ERROR with errno set otherwise. *out_n == 0 on OK is a genuine
 * zero-length UDP datagram, not zero-capacity truncation and not
 * end-of-stream. Oversized datagrams are truncated to len and the remainder is
 * discarded by the kernel.
 *
 * odin_udp_send performs one atomic sendto to the caller-supplied destination
 * sockaddr: ODIN_UDP_OK carries *out_n == len, ODIN_UDP_AGAIN is the
 * retryable EAGAIN/EWOULDBLOCK/EINTR class, and ODIN_UDP_IO_ERROR carries
 * errno for genuine errors such as EMSGSIZE.
 *
 * odin_udp_set_interest accepts only READ|WRITE input bits. ERROR is
 * output-only; ERROR or unknown input bits return -1/EINVAL while preserving
 * the active watch, cached mask, and backend registration. Valid masks
 * reconcile one level-triggered odin_event_io watch, and on_ready receives a
 * READ|WRITE|ERROR delivery mask on the owner thread. odin_udp_close stops any
 * active watch, closes the owned socket, frees the endpoint, never invokes
 * on_ready, is a no-op for NULL, and is legal from within on_ready.
 */

#ifndef ODIN_UDP_H_
#define ODIN_UDP_H_

#include <stddef.h>
#include <sys/socket.h>

#include "odin/event_loop.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct odin_udp_t odin_udp_t;

typedef enum odin_udp_io_t {
  ODIN_UDP_OK = 0,   /* transferred one datagram of *out_n bytes      */
  ODIN_UDP_AGAIN,    /* would block; wait for the next readiness       */
  ODIN_UDP_IO_ERROR, /* failed; errno is set                           */
} odin_udp_io_t;

/* Readiness flags (same values as ODIN_EVENT_*): output bits delivered to
 * odin_udp_ready_cb, and the input mask accepted by set_interest (READ|WRITE
 * only; ERROR is output-only). */
#define ODIN_UDP_READ 0x01u
#define ODIN_UDP_WRITE 0x02u
#define ODIN_UDP_ERROR 0x04u

typedef void (*odin_udp_ready_cb)(odin_udp_t *u, unsigned int events,
                                  void *user_data);

int odin_udp_open(odin_event_loop_t *loop, const struct sockaddr *addr,
                  socklen_t addrlen, odin_udp_ready_cb on_ready,
                  void *user_data, odin_udp_t **out);

odin_udp_io_t odin_udp_recv(odin_udp_t *u, void *buf, size_t len, size_t *out_n,
                            struct sockaddr *src, socklen_t *srclen);

odin_udp_io_t odin_udp_send(odin_udp_t *u, const void *buf, size_t len,
                            size_t *out_n, const struct sockaddr *dst,
                            socklen_t dstlen);

int odin_udp_set_interest(odin_udp_t *u, unsigned int events);

void odin_udp_close(odin_udp_t *u);

#ifdef __cplusplus
}
#endif

#endif /* ODIN_UDP_H_ */
