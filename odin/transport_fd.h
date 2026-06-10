/* odin/transport_fd.h
 *
 * fd implementation of the RFC-013 transport interface.
 *
 * odin_fd_transport_create backs the odin_transport_t vtable with read(2),
 * write(2), shutdown(fd, SHUT_WR), getsockopt(SO_ERROR), and the RFC-010
 * odin_event_io_* watch lifecycle over a caller-owned nonblocking connected
 * stream socket, preserving the byte-transfer / EAGAIN / orderly-EOF /
 * genuine-error classification the RFC-011 relay relies on.
 *
 * Ownership: the implementation never owns fd -- neither create, the vtable
 * destroy, nor any op closes it; the caller opens and closes it. create
 * allocates the implementation struct, stores loop/fd/on_ready/user_data,
 * writes *out, and returns 0; it registers NO watch (readiness begins only when
 * the consumer calls odin_transport_set_interest), so its only failure is
 * ENOMEM (returns -1, errno == ENOMEM, *out untouched). Preconditions: fd is a
 * caller-owned, nonblocking, connected stream socket; loop is a live loop owned
 * by the calling thread; on_ready is non-null. Owner-thread API.
 */

#ifndef ODIN_TRANSPORT_FD_H_
#define ODIN_TRANSPORT_FD_H_

#include "odin/event_loop.h"
#include "odin/transport.h"

#ifdef __cplusplus
extern "C" {
#endif

int odin_fd_transport_create(odin_event_loop_t *loop, int fd,
                             odin_transport_ready_cb on_ready, void *user_data,
                             odin_transport_t **out);

#ifdef __cplusplus
}
#endif

#endif /* ODIN_TRANSPORT_FD_H_ */
