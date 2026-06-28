/* odin/testing/connect_session_internal_test.h */

#ifndef ODIN_CONNECT_SESSION_INTERNAL_TEST_H_
#define ODIN_CONNECT_SESSION_INTERNAL_TEST_H_

#if defined(ODIN_CONNECT_SESSION_TESTING)

#ifdef __cplusplus
extern "C" {
#endif

int odin_connect_session_test_fail_next_create_server(int errnum);
int odin_connect_session_test_fail_next_create_client(int errnum);
unsigned int odin_connect_session_test_live_count(void);

#ifdef __cplusplus
}
#endif

#endif /* defined(ODIN_CONNECT_SESSION_TESTING) */

#endif /* ODIN_CONNECT_SESSION_INTERNAL_TEST_H_ */
