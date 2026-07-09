/* odin/testing/dns_resolver_internal_test.h */

#ifndef ODIN_DNS_RESOLVER_INTERNAL_TEST_H_
#define ODIN_DNS_RESOLVER_INTERNAL_TEST_H_

#include <stddef.h>
#include <stdint.h>

#include "odin/dns_resolver.h"
#include "odin/event_loop.h"

#if defined(ODIN_DNS_RESOLVER_TESTING)
#ifdef __cplusplus
extern "C" {
#endif

typedef struct odin_dns_resolver_test_liveness_t {
  size_t resolvers;
  size_t queries;
  size_t watches;
  size_t timers;
  size_t cares_channels;
  size_t cares_results;
  size_t resolver_create_calls;
  size_t resolver_destroy_calls;
} odin_dns_resolver_test_liveness_t;

typedef struct odin_dns_resolver_test_cares_observation_t {
  size_t ares_library_init_calls;
  size_t getaddrinfo_calls;
  size_t ares_destroy_calls;
  size_t ares_freeaddrinfo_calls;
  int last_init_options_optmask;
  int last_init_options_flags;
  char last_init_options_lookups[4];
  int last_init_options_timeout_ms;
  int last_init_options_tries;
  int last_ai_flags;
  int last_ai_family;
  size_t consumed_addr_results;
  size_t last_consumed_addr_count;
} odin_dns_resolver_test_cares_observation_t;

#define ODIN_DNS_TEST_CARES_LIBRARY_INIT 1
#define ODIN_DNS_TEST_CARES_INIT_OPTIONS 2
#define ODIN_DNS_TEST_CARES_SET_SERVERS 3
#define ODIN_DNS_TEST_CARES_RESULT_STATUS 4
#define ODIN_DNS_TEST_CARES_PROCESS_FDS_STATUS 5
#define ODIN_DNS_TEST_CARES_TIMEOUT_TIMEVAL 6
#define ODIN_DNS_TEST_CARES_SOCK_STATE 7
#define ODIN_DNS_TEST_CARES_TIMEOUT_NULL 8
#define ODIN_DNS_TEST_CARES_RESULT_EMPTY_SUCCESS 9
#define ODIN_DNS_TEST_CARES_RESULT_PENDING 10

typedef struct odin_dns_resolver_test_cares_step_t {
  int op;
  int status;
  unsigned int expect_process_events;
  int expect_process_null_events;
  int64_t timeout_tv_sec;
  int64_t timeout_tv_usec;
  int readable;
  int writable;
} odin_dns_resolver_test_cares_step_t;

int odin_dns_resolver_test_reset_liveness(void);
int odin_dns_resolver_test_liveness(odin_dns_resolver_test_liveness_t *out);
int odin_dns_resolver_test_cares_observation(
    odin_dns_resolver_test_cares_observation_t *out);
int odin_dns_resolver_test_push_cares_step(
    const odin_dns_resolver_test_cares_step_t *step);
int odin_dns_resolver_test_push_addr_result(const odin_dns_addr_t *addrs,
                                            size_t addr_count);
int odin_dns_resolver_test_fail_next_result_alloc(void);
int odin_dns_resolver_test_first_watch(odin_dns_query_t *query,
                                       odin_event_io_t **out_io, int *out_fd);

#ifdef __cplusplus
}
#endif
#endif

#endif /* ODIN_DNS_RESOLVER_INTERNAL_TEST_H_ */
