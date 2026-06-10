/* odin/transport.c -- RFC-013 transport interface dispatch.
 *
 * Each dispatcher forwards to the matching vtable slot and returns its result
 * unchanged. odin_transport_destroy(NULL) is a no-op (the dispatcher
 * null-checks before forwarding); every other dispatcher treats t as a non-null
 * precondition and does not null-check.
 */

#include "odin/transport.h"

odin_transport_io_t odin_transport_read(odin_transport_t *t, void *buf,
                                        size_t len, size_t *out_n) {
  return t->vt->read(t, buf, len, out_n);
}

odin_transport_io_t odin_transport_write(odin_transport_t *t, const void *buf,
                                         size_t len, size_t *out_n) {
  return t->vt->write(t, buf, len, out_n);
}

int odin_transport_shutdown_write(odin_transport_t *t) {
  return t->vt->shutdown_write(t);
}

int odin_transport_set_interest(odin_transport_t *t, unsigned int events) {
  return t->vt->set_interest(t, events);
}

int odin_transport_error(odin_transport_t *t) { return t->vt->error(t); }

void odin_transport_destroy(odin_transport_t *t) {
  if (t != NULL) {
    t->vt->destroy(t);
  }
}
