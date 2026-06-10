/* odin/dial_internal_test.h */

#ifndef ODIN_DIAL_INTERNAL_TEST_H_
#define ODIN_DIAL_INTERNAL_TEST_H_

#if defined(ODIN_DIAL_TESTING)

#include "odin/dial.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the socket the dial currently owns, or -1 with errno == ENOENT once
 * ownership has left (after an ODIN_DIAL_OK handoff, an ODIN_DIAL_ERROR close,
 * or before any socket exists). Used by the §6 rows to confirm the dial closed
 * its own socket or relinquished ownership.
 */
int odin_dial_test_fd(odin_dial_t *dial);

#ifdef __cplusplus
}
#endif

#endif /* defined(ODIN_DIAL_TESTING) */

#endif /* ODIN_DIAL_INTERNAL_TEST_H_ */
