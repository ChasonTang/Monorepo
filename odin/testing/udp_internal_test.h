/* odin/testing/udp_internal_test.h */

#ifndef ODIN_UDP_INTERNAL_TEST_H_
#define ODIN_UDP_INTERNAL_TEST_H_

#if defined(ODIN_UDP_TESTING)

#include "odin/event_loop.h"
#include "odin/udp.h"

#ifdef __cplusplus
extern "C" {
#endif

int odin_udp_test_fd(odin_udp_t *u);
int odin_udp_test_io(odin_udp_t *u, odin_event_io_t **out);
int odin_udp_test_fail_next_sendto(odin_udp_t *u, int errnum);

#ifdef __cplusplus
}
#endif

#endif /* defined(ODIN_UDP_TESTING) */

#endif /* ODIN_UDP_INTERNAL_TEST_H_ */
