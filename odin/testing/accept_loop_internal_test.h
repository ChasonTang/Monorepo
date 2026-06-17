/* odin/testing/accept_loop_internal_test.h */

#ifndef ODIN_ACCEPT_LOOP_INTERNAL_TEST_H_
#define ODIN_ACCEPT_LOOP_INTERNAL_TEST_H_

#if defined(ODIN_ACCEPT_LOOP_TESTING)

#include "odin/accept_loop.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Arms a one-shot synthetic errno for the next accept_one's accept(2) step:
 * the next accept_one inside on_readable skips the real accept4 / accept
 * syscall, returns FAIL with err = errnum, and clears the fault. Used by the
 * §5 rows to deterministically exercise EINTR / ECONNABORTED / ENFILE /
 * ENOBUFS / ENOMEM and "other errno" classification arms without inflicting
 * the corresponding system-wide condition on the test process.
 */
int odin_accept_loop_test_fail_next_accept(odin_accept_loop_t *al, int errnum);

#if !defined(__linux__)
/* macOS-only: arms a one-shot synthetic errno for the post-accept fcntl step.
 * which == F_GETFL substitutes -1/errnum for the F_GETFL syscall (skipping
 * it); which == F_SETFL lets the F_GETFL run for real to produce a plausible
 * flags value, then substitutes -1/errnum for the F_SETFL syscall. Either way
 * the §3.2.2 mechanism's fcntl-failure branch executes against a real
 * accepted fd: close(r) is called and the synthesized errno is routed through
 * the same classification arms as if accept(2) itself returned errnum.
 */
int odin_accept_loop_test_fail_next_fcntl(odin_accept_loop_t *al, int which,
                                          int errnum);
#endif

/* Returns 1 iff the loop is in the PAUSED soft-degradation state
 * (al.io == NULL && al.timer != NULL && !al.terminal). Does not mutate al.
 */
int odin_accept_loop_test_is_paused(const odin_accept_loop_t *al);

/* Returns 1 iff the loop is in the TERMINAL state (al.terminal != 0). Does
 * not mutate al.
 */
int odin_accept_loop_test_is_terminal(const odin_accept_loop_t *al);

#ifdef __cplusplus
}
#endif

#endif /* defined(ODIN_ACCEPT_LOOP_TESTING) */

#endif /* ODIN_ACCEPT_LOOP_INTERNAL_TEST_H_ */
