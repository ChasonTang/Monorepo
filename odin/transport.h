/* odin/transport.h
 *
 * Pluggable byte-stream transport interface (RFC-013).
 *
 * An odin_transport_t carries a const function-pointer vtable (read, write,
 * shutdown_write, set_interest, error, destroy); the odin_transport_*
 * dispatcher functions forward each call to the installed implementation, so a
 * single consumer drives byte forwarding, half-close, readiness, and error
 * retrieval without naming a concrete transport. This module is a pure
 * abstraction: it depends on nothing but <stddef.h> and carries none of any
 * concrete transport's dependencies.
 *
 * Object layout is the extension point. struct odin_transport_t holds exactly
 * one member, a pointer to a const odin_transport_vtable_t. An implementation
 * embeds odin_transport_t as the FIRST member of its private struct; because
 * the base sits at offset 0, the dispatcher's odin_transport_t * is
 * bit-identical to the implementation's own struct pointer, so each slot may
 * cast t back to its concrete type. Consumers never read vt directly; they call
 * the dispatchers. A new transport lands as a sibling transport_<name>.{c,h}
 * implementing this vtable with no edit to this module.
 *
 * Threading & lifetime: all dispatchers and the readiness callback run on the
 * implementation's owner thread; the interface adds no locks.
 * odin_transport_destroy is callable from within the readiness callback.
 */

#ifndef ODIN_TRANSPORT_H_
#define ODIN_TRANSPORT_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct odin_transport_t odin_transport_t;

/* Readiness flags: output bits delivered to odin_transport_ready_cb, and the
 * input mask accepted by set_interest (READ|WRITE only; ERROR is output-only).
 */
#define ODIN_TRANSPORT_READ 0x01u
#define ODIN_TRANSPORT_WRITE 0x02u
#define ODIN_TRANSPORT_ERROR 0x04u

typedef enum odin_transport_io_t {
  ODIN_TRANSPORT_OK = 0,   /* transferred *out_n bytes                  */
  ODIN_TRANSPORT_AGAIN,    /* would block; wait for the next readiness  */
  ODIN_TRANSPORT_EOF,      /* read only: peer half-closed, orderly      */
  ODIN_TRANSPORT_IO_ERROR, /* failed; errno is set                      */
} odin_transport_io_t;

/* Readiness callback: invoked on the owner thread with an events mask drawn
 * from READ|WRITE|ERROR after interest is registered. Watches are
 * level-triggered. */
typedef void (*odin_transport_ready_cb)(odin_transport_t *t,
                                        unsigned int events, void *user_data);

typedef struct odin_transport_vtable_t {
  odin_transport_io_t (*read)(odin_transport_t *t, void *buf, size_t len,
                              size_t *out_n);
  odin_transport_io_t (*write)(odin_transport_t *t, const void *buf, size_t len,
                               size_t *out_n);
  int (*shutdown_write)(odin_transport_t *t);
  int (*set_interest)(odin_transport_t *t, unsigned int events);
  int (*error)(odin_transport_t *t);
  void (*destroy)(odin_transport_t *t);
} odin_transport_vtable_t;

struct odin_transport_t {
  const odin_transport_vtable_t *vt;
};

/* Each dispatcher forwards to the matching vtable slot and returns its result
 * unchanged. read/write set *out_n to the byte count on ODIN_TRANSPORT_OK; read
 * may also return ODIN_TRANSPORT_EOF (orderly peer half-close, *out_n == 0).
 * shutdown_write and set_interest follow the Odin house rule (0 on success, -1
 * with errno set). error returns the latched asynchronous transport error, 0
 * when none. set_interest takes a (possibly empty) subset of READ|WRITE; ERROR
 * must not be set. odin_transport_destroy(NULL) is a no-op; every other
 * dispatcher treats t as a non-null precondition. */
odin_transport_io_t odin_transport_read(odin_transport_t *t, void *buf,
                                        size_t len, size_t *out_n);
odin_transport_io_t odin_transport_write(odin_transport_t *t, const void *buf,
                                         size_t len, size_t *out_n);
int odin_transport_shutdown_write(odin_transport_t *t);
int odin_transport_set_interest(odin_transport_t *t, unsigned int events);
int odin_transport_error(odin_transport_t *t);
void odin_transport_destroy(odin_transport_t *t);

#ifdef __cplusplus
}
#endif

#endif /* ODIN_TRANSPORT_H_ */
