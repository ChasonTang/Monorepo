#include "odin/dns_resolver.h"

#if defined(ODIN_DNS_RESOLVER_TESTING)
#include "odin/testing/dns_resolver_internal_test.h"
#endif

#include <ares.h>

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

typedef struct odin_dns_watch_t odin_dns_watch_t;

struct odin_dns_watch_t {
  int fd;
  unsigned int desired_events;
  unsigned int current_events;
  odin_event_io_t *io;
  odin_dns_watch_t *next;
};

struct odin_dns_resolver_t {
  odin_event_loop_t *loop;
  char *servers_csv;
  int timeout_ms;
  int tries;
  odin_dns_query_t *queries;
};

struct odin_dns_query_t {
  odin_dns_resolver_t *resolver;
  odin_dns_query_t *prev;
  odin_dns_query_t *next;
  char *name;
  char service[6];
  uint16_t port;
  int family;
  odin_dns_cb on_done;
  void *user_data;
  ares_channel_t *channel;
  odin_dns_watch_t *watches;
  odin_event_timer_t *timeout_timer;
  odin_event_timer_t *finalizer_timer;
  struct ares_addrinfo *result;
  int result_status;
  int completion_pending;
  int fatal_pending;
  int fatal_errno;
  int sock_state_dirty;
  int timeout_dirty;
  int published;
  int completed;
  int destroying;
  int suppress_callbacks;
  int in_callback;
#if defined(ODIN_DNS_RESOLVER_TESTING)
  int test_result_allocated;
#endif
};

static pthread_mutex_t g_library_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_library_state;
static int g_library_errno;

#if defined(ODIN_DNS_RESOLVER_TESTING)
static pthread_mutex_t g_test_mu = PTHREAD_MUTEX_INITIALIZER;
static odin_dns_resolver_test_liveness_t g_live;
static odin_dns_resolver_test_cares_observation_t g_obs;
static odin_dns_resolver_test_cares_step_t *g_steps;
static size_t g_steps_len;
static size_t g_steps_cap;
static int g_fail_next_result_alloc;

static void test_live_resolvers_add(void) {
  pthread_mutex_lock(&g_test_mu);
  g_live.resolvers += 1;
  pthread_mutex_unlock(&g_test_mu);
}

static void test_live_resolvers_sub(void) {
  pthread_mutex_lock(&g_test_mu);
  g_live.resolvers -= 1;
  pthread_mutex_unlock(&g_test_mu);
}

static void test_live_queries_add(void) {
  pthread_mutex_lock(&g_test_mu);
  g_live.queries += 1;
  pthread_mutex_unlock(&g_test_mu);
}

static void test_live_queries_sub(void) {
  pthread_mutex_lock(&g_test_mu);
  g_live.queries -= 1;
  pthread_mutex_unlock(&g_test_mu);
}

static void test_live_watches_add(void) {
  pthread_mutex_lock(&g_test_mu);
  g_live.watches += 1;
  pthread_mutex_unlock(&g_test_mu);
}

static void test_live_watches_sub(void) {
  pthread_mutex_lock(&g_test_mu);
  g_live.watches -= 1;
  pthread_mutex_unlock(&g_test_mu);
}

static void test_live_timers_add(void) {
  pthread_mutex_lock(&g_test_mu);
  g_live.timers += 1;
  pthread_mutex_unlock(&g_test_mu);
}

static void test_live_timers_sub(void) {
  pthread_mutex_lock(&g_test_mu);
  g_live.timers -= 1;
  pthread_mutex_unlock(&g_test_mu);
}

static void test_live_channels_add(void) {
  pthread_mutex_lock(&g_test_mu);
  g_live.cares_channels += 1;
  pthread_mutex_unlock(&g_test_mu);
}

static void test_live_channels_sub(void) {
  pthread_mutex_lock(&g_test_mu);
  g_live.cares_channels -= 1;
  pthread_mutex_unlock(&g_test_mu);
}

static void test_live_results_add(void) {
  pthread_mutex_lock(&g_test_mu);
  g_live.cares_results += 1;
  pthread_mutex_unlock(&g_test_mu);
}

static void test_live_results_sub(void) {
  pthread_mutex_lock(&g_test_mu);
  g_live.cares_results -= 1;
  pthread_mutex_unlock(&g_test_mu);
}

static int test_pop_step(int op, odin_dns_resolver_test_cares_step_t *out) {
  int found = 0;
  pthread_mutex_lock(&g_test_mu);
  if (g_steps_len > 0 && g_steps[0].op == op) {
    *out = g_steps[0];
    memmove(&g_steps[0], &g_steps[1], (g_steps_len - 1) * sizeof(g_steps[0]));
    g_steps_len -= 1;
    found = 1;
  }
  pthread_mutex_unlock(&g_test_mu);
  return found;
}

static int test_fail_result_alloc(void) {
  int fail = 0;
  pthread_mutex_lock(&g_test_mu);
  if (g_fail_next_result_alloc) {
    g_fail_next_result_alloc = 0;
    fail = 1;
  }
  pthread_mutex_unlock(&g_test_mu);
  return fail;
}
#else
#define test_live_resolvers_add() ((void)0)
#define test_live_resolvers_sub() ((void)0)
#define test_live_queries_add() ((void)0)
#define test_live_queries_sub() ((void)0)
#define test_live_watches_add() ((void)0)
#define test_live_watches_sub() ((void)0)
#define test_live_timers_add() ((void)0)
#define test_live_timers_sub() ((void)0)
#define test_live_channels_add() ((void)0)
#define test_live_channels_sub() ((void)0)
#define test_live_results_add() ((void)0)
#define test_live_results_sub() ((void)0)
static int test_fail_result_alloc(void) { return 0; }
#endif

static int map_setup_status(int status) {
  if (status == ARES_ENOMEM) {
    return ENOMEM;
  }
  if (status == ARES_EFORMERR || status == ARES_EBADSTR ||
      status == ARES_ESERVICE) {
    return EINVAL;
  }
  return EIO;
}

static int map_result_errno(int status) {
  switch (status) {
  case ARES_SUCCESS:
    return 0;
  case ARES_ECONNREFUSED:
  case ARES_EREFUSED:
  case ARES_ENOSERVER:
    return ECONNREFUSED;
  case ARES_ENOMEM:
    return ENOMEM;
  case ARES_ENOTIMP:
    return EAFNOSUPPORT;
  case ARES_ESERVICE:
  case ARES_EFORMERR:
  case ARES_EBADSTR:
    return EINVAL;
  case ARES_ECANCELLED:
  case ARES_EDESTRUCTION:
    return ECANCELED;
  case ARES_ETIMEOUT:
    return ETIMEDOUT;
  case ARES_ENOTFOUND:
  case ARES_ENODATA:
  case ARES_ENONAME:
    return EHOSTUNREACH;
  case ARES_EBADRESP:
  default:
    return EIO;
  }
}

static int validate_name(const char *name, size_t name_len) {
  if (name == NULL || name_len == 0 || name_len > ODIN_DNS_NAME_MAX) {
    errno = EINVAL;
    return -1;
  }
  if (memchr(name, '\0', name_len) != NULL) {
    errno = EINVAL;
    return -1;
  }
  return 0;
}

static int validate_family(int family) {
  if (family == AF_UNSPEC || family == AF_INET || family == AF_INET6) {
    return 0;
  }
  errno = EAFNOSUPPORT;
  return -1;
}

#if defined(ODIN_DNS_RESOLVER_TESTING)
static int dns_ares_library_init(int flags) {
  odin_dns_resolver_test_cares_step_t step;
  int has_step;
  pthread_mutex_lock(&g_test_mu);
  g_obs.ares_library_init_calls += 1;
  pthread_mutex_unlock(&g_test_mu);
  has_step = test_pop_step(ODIN_DNS_TEST_CARES_LIBRARY_INIT, &step);
  if (has_step) {
    return step.status;
  }
  return ares_library_init(flags);
}

static int dns_ares_init_options(ares_channel_t **channelptr,
                                 const struct ares_options *options,
                                 int optmask) {
  odin_dns_resolver_test_cares_step_t step;
  int has_step;
  pthread_mutex_lock(&g_test_mu);
  g_obs.last_init_options_optmask = optmask;
  g_obs.last_init_options_flags = options != NULL ? options->flags : 0;
  memset(g_obs.last_init_options_lookups, 0,
         sizeof(g_obs.last_init_options_lookups));
  if (options != NULL && options->lookups != NULL) {
    strncpy(g_obs.last_init_options_lookups, options->lookups,
            sizeof(g_obs.last_init_options_lookups) - 1);
  }
  g_obs.last_init_options_timeout_ms = options != NULL ? options->timeout : 0;
  g_obs.last_init_options_tries = options != NULL ? options->tries : 0;
  pthread_mutex_unlock(&g_test_mu);

  has_step = test_pop_step(ODIN_DNS_TEST_CARES_INIT_OPTIONS, &step);
  if (has_step && step.status != ARES_SUCCESS) {
    return step.status;
  }
  const int rc = ares_init_options(channelptr, options, optmask);
  if (rc == ARES_SUCCESS) {
    test_live_channels_add();
  }
  return rc;
}

static int dns_ares_set_servers_ports_csv(ares_channel_t *channel,
                                          const char *servers) {
  odin_dns_resolver_test_cares_step_t step;
  if (test_pop_step(ODIN_DNS_TEST_CARES_SET_SERVERS, &step)) {
    return step.status;
  }
  return ares_set_servers_ports_csv(channel, servers);
}

static struct ares_addrinfo *make_test_addrinfo(void) {
  struct ares_addrinfo *ai = (struct ares_addrinfo *)calloc(1, sizeof(*ai));
  struct ares_addrinfo_node *node =
      (struct ares_addrinfo_node *)calloc(1, sizeof(*node));
  struct sockaddr_in *addr = (struct sockaddr_in *)calloc(1, sizeof(*addr));
  if (ai == NULL || node == NULL || addr == NULL) {
    free(addr);
    free(node);
    free(ai);
    return NULL;
  }
  addr->sin_family = AF_INET;
  addr->sin_port = htons(80);
  addr->sin_addr.s_addr = htonl(0x7f000001u);
  node->ai_ttl = 60;
  node->ai_family = AF_INET;
  node->ai_socktype = SOCK_STREAM;
  node->ai_protocol = IPPROTO_TCP;
  node->ai_addrlen = (ares_socklen_t)sizeof(*addr);
  node->ai_addr = (struct sockaddr *)addr;
  ai->nodes = node;
  return ai;
}

static void free_test_addrinfo(struct ares_addrinfo *ai) {
  if (ai == NULL) {
    return;
  }
  struct ares_addrinfo_node *node = ai->nodes;
  while (node != NULL) {
    struct ares_addrinfo_node *next = node->ai_next;
    free(node->ai_addr);
    free(node);
    node = next;
  }
  free(ai);
}

static void dns_result_cb(void *arg, int status, int timeouts,
                          struct ares_addrinfo *res);

static void complete_test_result(odin_dns_query_t *query, int status) {
  if (query->suppress_callbacks) {
    return;
  }
  query->result_status = status;
  query->completion_pending = 1;
  if (status != ARES_SUCCESS) {
    return;
  }

  query->result = make_test_addrinfo();
  if (query->result == NULL) {
    query->result_status = ARES_ENOMEM;
    return;
  }
  query->test_result_allocated = 1;
  test_live_results_add();
}

static void dns_ares_getaddrinfo(odin_dns_query_t *query, const char *node,
                                 const char *service,
                                 const struct ares_addrinfo_hints *hints) {
  odin_dns_resolver_test_cares_step_t step;
  pthread_mutex_lock(&g_test_mu);
  g_obs.getaddrinfo_calls += 1;
  g_obs.last_ai_flags = hints != NULL ? hints->ai_flags : 0;
  g_obs.last_ai_family = hints != NULL ? hints->ai_family : 0;
  pthread_mutex_unlock(&g_test_mu);

  if (test_pop_step(ODIN_DNS_TEST_CARES_RESULT_STATUS, &step)) {
    complete_test_result(query, step.status);
    return;
  }

  ares_getaddrinfo(query->channel, node, service, hints, dns_result_cb, query);
}

static int first_recorded_fd(odin_dns_query_t *query) {
  for (odin_dns_watch_t *watch = query->watches; watch != NULL;
       watch = watch->next) {
    if (watch->io != NULL || watch->desired_events != 0) {
      return watch->fd;
    }
  }
  return -1;
}

static int
verify_process_expectations(const odin_dns_resolver_test_cares_step_t *step,
                            const ares_fd_events_t *events, size_t nevents) {
  if (step->expect_process_events != 0) {
    if (events == NULL || nevents != 1 ||
        events[0].events != step->expect_process_events) {
      return 0;
    }
  }
  if (step->expect_process_null_events) {
    if (events != NULL || nevents != 0) {
      return 0;
    }
  }
  return 1;
}

static ares_status_t dns_ares_process_fds(odin_dns_query_t *query,
                                          const ares_fd_events_t *events,
                                          size_t nevents, unsigned int flags);

static struct timeval *dns_ares_timeout(odin_dns_query_t *query,
                                        struct timeval *maxtv,
                                        struct timeval *tv) {
  odin_dns_resolver_test_cares_step_t step;
  (void)query;
  if (test_pop_step(ODIN_DNS_TEST_CARES_TIMEOUT_TIMEVAL, &step)) {
    if (tv == NULL) {
      return NULL;
    }
    tv->tv_sec = (time_t)step.timeout_tv_sec;
    tv->tv_usec = (suseconds_t)step.timeout_tv_usec;
    return tv;
  }
  if (test_pop_step(ODIN_DNS_TEST_CARES_TIMEOUT_NULL, &step)) {
    (void)step;
    return NULL;
  }
  return ares_timeout(query->channel, maxtv, tv);
}

static void dns_ares_destroy(ares_channel_t *channel) {
  pthread_mutex_lock(&g_test_mu);
  g_obs.ares_destroy_calls += 1;
  pthread_mutex_unlock(&g_test_mu);
  test_live_channels_sub();
  ares_destroy(channel);
}

static void dns_ares_freeaddrinfo(odin_dns_query_t *query,
                                  struct ares_addrinfo *ai) {
  pthread_mutex_lock(&g_test_mu);
  g_obs.ares_freeaddrinfo_calls += 1;
  pthread_mutex_unlock(&g_test_mu);
  test_live_results_sub();
  if (query->test_result_allocated) {
    query->test_result_allocated = 0;
    free_test_addrinfo(ai);
    return;
  }
  ares_freeaddrinfo(ai);
}
#else
static int dns_ares_library_init(int flags) { return ares_library_init(flags); }

static int dns_ares_init_options(ares_channel_t **channelptr,
                                 const struct ares_options *options,
                                 int optmask) {
  return ares_init_options(channelptr, options, optmask);
}

static int dns_ares_set_servers_ports_csv(ares_channel_t *channel,
                                          const char *servers) {
  return ares_set_servers_ports_csv(channel, servers);
}

static void dns_result_cb(void *arg, int status, int timeouts,
                          struct ares_addrinfo *res);

static void dns_ares_getaddrinfo(odin_dns_query_t *query, const char *node,
                                 const char *service,
                                 const struct ares_addrinfo_hints *hints) {
  ares_getaddrinfo(query->channel, node, service, hints, dns_result_cb, query);
}

static ares_status_t dns_ares_process_fds(odin_dns_query_t *query,
                                          const ares_fd_events_t *events,
                                          size_t nevents, unsigned int flags) {
  return ares_process_fds(query->channel, events, nevents, flags);
}

static struct timeval *dns_ares_timeout(odin_dns_query_t *query,
                                        struct timeval *maxtv,
                                        struct timeval *tv) {
  return ares_timeout(query->channel, maxtv, tv);
}

static void dns_ares_destroy(ares_channel_t *channel) { ares_destroy(channel); }

static void dns_ares_freeaddrinfo(odin_dns_query_t *query,
                                  struct ares_addrinfo *ai) {
  (void)query;
  ares_freeaddrinfo(ai);
}
#endif

static int ensure_cares_library(void) {
  pthread_mutex_lock(&g_library_mu);
  if (g_library_state == 1) {
    pthread_mutex_unlock(&g_library_mu);
    return 0;
  }
  if (g_library_state == 2) {
    const int err = g_library_errno;
    pthread_mutex_unlock(&g_library_mu);
    errno = err;
    return -1;
  }

  const int status = dns_ares_library_init(ARES_LIB_INIT_ALL);
  if (status == ARES_SUCCESS) {
    g_library_state = 1;
    pthread_mutex_unlock(&g_library_mu);
    return 0;
  }

  g_library_errno = map_setup_status(status);
  g_library_state = 2;
  const int err = g_library_errno;
  pthread_mutex_unlock(&g_library_mu);
  errno = err;
  return -1;
}

static void link_query(odin_dns_resolver_t *resolver, odin_dns_query_t *query) {
  query->resolver = resolver;
  query->next = resolver->queries;
  if (resolver->queries != NULL) {
    resolver->queries->prev = query;
  }
  resolver->queries = query;
}

static void unlink_query(odin_dns_query_t *query) {
  odin_dns_resolver_t *resolver = query->resolver;
  if (resolver == NULL) {
    return;
  }
  if (query->prev != NULL) {
    query->prev->next = query->next;
  } else {
    resolver->queries = query->next;
  }
  if (query->next != NULL) {
    query->next->prev = query->prev;
  }
  query->resolver = NULL;
  query->prev = NULL;
  query->next = NULL;
}

static odin_dns_watch_t *find_watch(odin_dns_query_t *query, int fd) {
  for (odin_dns_watch_t *watch = query->watches; watch != NULL;
       watch = watch->next) {
    if (watch->fd == fd) {
      return watch;
    }
  }
  return NULL;
}

static void enter_fatal(odin_dns_query_t *query, int err);

static odin_dns_watch_t *get_watch(odin_dns_query_t *query, int fd) {
  odin_dns_watch_t *watch = find_watch(query, fd);
  if (watch != NULL) {
    return watch;
  }
  watch = (odin_dns_watch_t *)calloc(1, sizeof(*watch));
  if (watch == NULL) {
    enter_fatal(query, ENOMEM);
    return NULL;
  }
  watch->fd = fd;
  watch->next = query->watches;
  query->watches = watch;
  return watch;
}

static void remove_watch_node(odin_dns_query_t *query,
                              odin_dns_watch_t *watch) {
  odin_dns_watch_t **pp = &query->watches;
  while (*pp != NULL && *pp != watch) {
    pp = &(*pp)->next;
  }
  if (*pp == watch) {
    *pp = watch->next;
  }
  free(watch);
}

static void clear_timeout_timer(odin_dns_query_t *query) {
  if (query->timeout_timer != NULL) {
    odin_event_timer_stop(query->timeout_timer);
    query->timeout_timer = NULL;
    test_live_timers_sub();
  }
}

static void clear_finalizer_timer(odin_dns_query_t *query) {
  if (query->finalizer_timer != NULL) {
    odin_event_timer_stop(query->finalizer_timer);
    query->finalizer_timer = NULL;
    test_live_timers_sub();
  }
}

static void stop_all_watches(odin_dns_query_t *query) {
  odin_dns_watch_t *watch = query->watches;
  while (watch != NULL) {
    odin_dns_watch_t *next = watch->next;
    if (watch->io != NULL) {
      odin_event_io_stop(watch->io);
      watch->io = NULL;
      test_live_watches_sub();
    }
    free(watch);
    watch = next;
  }
  query->watches = NULL;
}

static void destroy_channel(odin_dns_query_t *query) {
  if (query->channel == NULL) {
    return;
  }
  query->suppress_callbacks = 1;
  dns_ares_destroy(query->channel);
  query->channel = NULL;
}

static void free_recorded_result(odin_dns_query_t *query) {
  if (query->result == NULL) {
    return;
  }
  dns_ares_freeaddrinfo(query, query->result);
  query->result = NULL;
}

static void free_query_storage(odin_dns_query_t *query) {
  free(query->name);
  free(query);
  test_live_queries_sub();
}

static void cleanup_query(odin_dns_query_t *query) {
  stop_all_watches(query);
  clear_timeout_timer(query);
  clear_finalizer_timer(query);
  free_recorded_result(query);
  destroy_channel(query);
  unlink_query(query);
  free_query_storage(query);
}

static int arm_finalizer_timer(odin_dns_query_t *query);
static void run_finalizer(odin_dns_query_t *query);

static void enter_fatal(odin_dns_query_t *query, int err) {
  if (!query->completion_pending) {
    query->completion_pending = 1;
  }
  query->fatal_pending = 1;
  query->fatal_errno = err;
}

static void dns_sock_state_cb(void *data, ares_socket_t socket_fd, int readable,
                              int writable) {
  odin_dns_query_t *query = (odin_dns_query_t *)data;
  if (query->suppress_callbacks) {
    return;
  }
  unsigned int mask = 0;
  if (readable) {
    mask |= ODIN_EVENT_READ;
  }
  if (writable) {
    mask |= ODIN_EVENT_WRITE;
  }
  odin_dns_watch_t *watch = get_watch(query, (int)socket_fd);
  if (watch == NULL) {
    return;
  }
  watch->desired_events = mask;
  query->sock_state_dirty = 1;
  query->timeout_dirty = 1;
}

static void dns_result_cb(void *arg, int status, int timeouts,
                          struct ares_addrinfo *res) {
  (void)timeouts;
  odin_dns_query_t *query = (odin_dns_query_t *)arg;
  if (query->suppress_callbacks) {
    return;
  }
  query->result_status = status;
  query->result = res;
  query->completion_pending = 1;
  if (res != NULL) {
    test_live_results_add();
  }
}

static int timeval_to_us(const struct timeval *tv, uint64_t *out) {
  if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec > 999999) {
    errno = EOVERFLOW;
    return -1;
  }
  const uint64_t sec = (uint64_t)tv->tv_sec;
  const uint64_t usec = (uint64_t)tv->tv_usec;
  if (sec > (UINT64_MAX - usec) / 1000000u) {
    errno = EOVERFLOW;
    return -1;
  }
  *out = sec * 1000000u + usec;
  return 0;
}

static void on_io(odin_event_loop_t *loop, odin_event_io_t *io, int fd,
                  unsigned int events, void *user_data);
static void on_timeout(odin_event_loop_t *loop, odin_event_timer_t *timer,
                       void *user_data);
static void on_finalizer(odin_event_loop_t *loop, odin_event_timer_t *timer,
                         void *user_data);

static int reconcile_watches(odin_dns_query_t *query) {
  odin_dns_watch_t *watch = query->watches;
  while (watch != NULL) {
    odin_dns_watch_t *next = watch->next;
    if (watch->desired_events == 0) {
      if (watch->io != NULL) {
        odin_event_io_stop(watch->io);
        watch->io = NULL;
        watch->current_events = 0;
        test_live_watches_sub();
      }
      remove_watch_node(query, watch);
    } else if (watch->io == NULL) {
      if (odin_event_io_start(query->resolver->loop, watch->fd,
                              watch->desired_events, on_io, query,
                              &watch->io) != 0) {
        enter_fatal(query, errno);
        return -1;
      }
      watch->current_events = watch->desired_events;
      test_live_watches_add();
    } else if (watch->current_events != watch->desired_events) {
      if (odin_event_io_update(watch->io, watch->desired_events) != 0) {
        enter_fatal(query, errno);
        return -1;
      }
      watch->current_events = watch->desired_events;
    }
    watch = next;
  }
  query->sock_state_dirty = 0;
  return 0;
}

static int refresh_timeout(odin_dns_query_t *query) {
  struct timeval tv;
  struct timeval *tvp = dns_ares_timeout(query, NULL, &tv);
  if (tvp == NULL) {
    clear_timeout_timer(query);
    query->timeout_dirty = 0;
    return 0;
  }

  uint64_t timeout_us = 0;
  if (timeval_to_us(tvp, &timeout_us) != 0) {
    enter_fatal(query, errno);
    return -1;
  }

  if (query->timeout_timer != NULL) {
    if (odin_event_timer_reset(query->timeout_timer, timeout_us, 0) != 0) {
      enter_fatal(query, errno);
      return -1;
    }
  } else {
    if (odin_event_timer_start(query->resolver->loop, timeout_us, 0, on_timeout,
                               query, &query->timeout_timer) != 0) {
      enter_fatal(query, errno);
      return -1;
    }
    test_live_timers_add();
  }
  query->timeout_dirty = 0;
  return 0;
}

static int after_cares_entrypoint(odin_dns_query_t *query,
                                  int came_from_start) {
  if (query->completion_pending) {
    stop_all_watches(query);
    clear_timeout_timer(query);
    if (came_from_start &&
        (query->result_status == ARES_ECANCELLED ||
         query->result_status == ARES_EDESTRUCTION) &&
        !query->fatal_pending) {
      errno = ECANCELED;
      return -1;
    }
    if (came_from_start) {
      return arm_finalizer_timer(query);
    }
    run_finalizer(query);
    return 0;
  }

  if (query->sock_state_dirty && reconcile_watches(query) != 0) {
    stop_all_watches(query);
    clear_timeout_timer(query);
    if (came_from_start) {
      return arm_finalizer_timer(query);
    }
    run_finalizer(query);
    return 0;
  }

  if (query->completion_pending) {
    stop_all_watches(query);
    clear_timeout_timer(query);
    if (came_from_start) {
      return arm_finalizer_timer(query);
    }
    run_finalizer(query);
    return 0;
  }

  if (query->timeout_dirty && refresh_timeout(query) != 0) {
    stop_all_watches(query);
    clear_timeout_timer(query);
    if (came_from_start) {
      return arm_finalizer_timer(query);
    }
    run_finalizer(query);
    return 0;
  }
  return 0;
}

static int node_has_supported_addr(const struct ares_addrinfo_node *node) {
  if (node->ai_addr == NULL) {
    return 0;
  }
  if (node->ai_family == AF_INET) {
    return node->ai_addrlen >= (ares_socklen_t)sizeof(struct sockaddr_in);
  }
  if (node->ai_family == AF_INET6) {
    return node->ai_addrlen >= (ares_socklen_t)sizeof(struct sockaddr_in6);
  }
  return 0;
}

static int copy_results(odin_dns_query_t *query, odin_dns_addr_t **out_addrs,
                        size_t *out_count) {
  size_t count = 0;
  for (struct ares_addrinfo_node *node = query->result->nodes; node != NULL;
       node = node->ai_next) {
    if (node_has_supported_addr(node)) {
      count += 1;
    }
  }
  if (count == 0) {
    *out_addrs = NULL;
    *out_count = 0;
    return 0;
  }
  if (test_fail_result_alloc()) {
    errno = ENOMEM;
    return -1;
  }
  odin_dns_addr_t *addrs = (odin_dns_addr_t *)calloc(count, sizeof(addrs[0]));
  if (addrs == NULL) {
    errno = ENOMEM;
    return -1;
  }
  size_t i = 0;
  for (struct ares_addrinfo_node *node = query->result->nodes; node != NULL;
       node = node->ai_next) {
    if (!node_has_supported_addr(node)) {
      continue;
    }
    if (node->ai_family == AF_INET) {
      memcpy(&addrs[i].addr, node->ai_addr, sizeof(struct sockaddr_in));
      addrs[i].addrlen = (socklen_t)sizeof(struct sockaddr_in);
      addrs[i].ttl = node->ai_ttl;
      ++i;
    } else if (node->ai_family == AF_INET6) {
      memcpy(&addrs[i].addr, node->ai_addr, sizeof(struct sockaddr_in6));
      addrs[i].addrlen = (socklen_t)sizeof(struct sockaddr_in6);
      addrs[i].ttl = node->ai_ttl;
      ++i;
    }
  }
  *out_addrs = addrs;
  *out_count = i;
  return 0;
}

static void run_finalizer(odin_dns_query_t *query) {
  if (query->destroying || query->completed) {
    return;
  }

  stop_all_watches(query);
  clear_timeout_timer(query);
  clear_finalizer_timer(query);

  odin_dns_status_t status = ODIN_DNS_ERROR;
  int err = query->fatal_pending ? query->fatal_errno
                                 : map_result_errno(query->result_status);
  odin_dns_addr_t *addrs = NULL;
  size_t addr_count = 0;

  if (!query->fatal_pending && query->result_status == ARES_SUCCESS) {
    if (query->result != NULL &&
        copy_results(query, &addrs, &addr_count) == 0) {
      status = ODIN_DNS_OK;
      err = 0;
    } else {
      status = ODIN_DNS_ERROR;
      err = errno == 0 ? ENOMEM : errno;
    }
  }

  free_recorded_result(query);
  destroy_channel(query);
  query->completed = 1;

  odin_dns_cb cb = query->on_done;
  void *user_data = query->user_data;
  query->in_callback = 1;
  cb(query, status, err, status == ODIN_DNS_OK ? addrs : NULL,
     status == ODIN_DNS_OK ? addr_count : 0, user_data);
  free(addrs);
}

static int arm_finalizer_timer(odin_dns_query_t *query) {
  if (odin_event_timer_start(query->resolver->loop, 0, 0, on_finalizer, query,
                             &query->finalizer_timer) != 0) {
    return -1;
  }
  test_live_timers_add();
  return 0;
}

static void on_io(odin_event_loop_t *loop, odin_event_io_t *io, int fd,
                  unsigned int events, void *user_data) {
  (void)loop;
  (void)io;
  odin_dns_query_t *query = (odin_dns_query_t *)user_data;
  ares_fd_events_t ares_events;
  ares_events.fd = (ares_socket_t)fd;
  ares_events.events = ARES_FD_EVENT_NONE;
  if (events & ODIN_EVENT_READ) {
    ares_events.events |= ARES_FD_EVENT_READ;
  }
  if (events & ODIN_EVENT_WRITE) {
    ares_events.events |= ARES_FD_EVENT_WRITE;
  }
  if (events & ODIN_EVENT_ERROR) {
    ares_events.events |= ARES_FD_EVENT_READ;
  }

  const ares_status_t status =
      dns_ares_process_fds(query, &ares_events, 1, ARES_PROCESS_FLAG_NONE);
  if (status != ARES_SUCCESS) {
    enter_fatal(query, map_result_errno(status));
  }
  if (!query->destroying && !query->completed) {
    (void)after_cares_entrypoint(query, 0);
  }
}

static void on_timeout(odin_event_loop_t *loop, odin_event_timer_t *timer,
                       void *user_data) {
  (void)loop;
  odin_dns_query_t *query = (odin_dns_query_t *)user_data;
  if (query->timeout_timer == timer) {
    query->timeout_timer = NULL;
    test_live_timers_sub();
  }
  const ares_status_t status =
      dns_ares_process_fds(query, NULL, 0, ARES_PROCESS_FLAG_NONE);
  if (status != ARES_SUCCESS) {
    enter_fatal(query, map_result_errno(status));
  }
  if (!query->destroying && !query->completed) {
    (void)after_cares_entrypoint(query, 0);
  }
}

static void on_finalizer(odin_event_loop_t *loop, odin_event_timer_t *timer,
                         void *user_data) {
  (void)loop;
  odin_dns_query_t *query = (odin_dns_query_t *)user_data;
  if (query->finalizer_timer == timer) {
    query->finalizer_timer = NULL;
    test_live_timers_sub();
  }
  run_finalizer(query);
}

#if defined(ODIN_DNS_RESOLVER_TESTING)
static ares_status_t dns_ares_process_fds(odin_dns_query_t *query,
                                          const ares_fd_events_t *events,
                                          size_t nevents, unsigned int flags) {
  odin_dns_resolver_test_cares_step_t step;
  while (test_pop_step(ODIN_DNS_TEST_CARES_SOCK_STATE, &step)) {
    const int fd = first_recorded_fd(query);
    if (fd < 0) {
      return ARES_EFORMERR;
    }
    dns_sock_state_cb(query, (ares_socket_t)fd, step.readable, step.writable);
  }

  if (test_pop_step(ODIN_DNS_TEST_CARES_PROCESS_FDS_STATUS, &step)) {
    if (!verify_process_expectations(&step, events, nevents)) {
      return ARES_EFORMERR;
    }
    return (ares_status_t)step.status;
  }

  if (test_pop_step(ODIN_DNS_TEST_CARES_RESULT_STATUS, &step)) {
    if (!verify_process_expectations(&step, events, nevents)) {
      return ARES_EFORMERR;
    }
    complete_test_result(query, step.status);
    return ARES_SUCCESS;
  }

  return ares_process_fds(query->channel, events, nevents, flags);
}
#endif

int odin_dns_resolver_create(odin_event_loop_t *loop,
                             const odin_dns_resolver_config_t *config,
                             odin_dns_resolver_t **out) {
  if (out == NULL || loop == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (config != NULL && (config->timeout_ms < 0 || config->tries < 0)) {
    errno = EINVAL;
    return -1;
  }
  if (ensure_cares_library() != 0) {
    return -1;
  }

  odin_dns_resolver_t *resolver =
      (odin_dns_resolver_t *)calloc(1, sizeof(*resolver));
  if (resolver == NULL) {
    errno = ENOMEM;
    return -1;
  }
  if (config != NULL) {
    resolver->timeout_ms = config->timeout_ms;
    resolver->tries = config->tries;
    if (config->servers_csv != NULL) {
      resolver->servers_csv = strdup(config->servers_csv);
      if (resolver->servers_csv == NULL) {
        free(resolver);
        errno = ENOMEM;
        return -1;
      }
    }
  }
  resolver->loop = loop;
  *out = resolver;
  test_live_resolvers_add();
  return 0;
}

int odin_dns_resolve_start(odin_dns_resolver_t *resolver, const char *name,
                           size_t name_len, uint16_t port, int family,
                           odin_dns_cb on_done, void *user_data,
                           odin_dns_query_t **out) {
  if (out == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (resolver == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (on_done == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (validate_name(name, name_len) != 0 || validate_family(family) != 0) {
    return -1;
  }

  odin_dns_query_t *query = (odin_dns_query_t *)calloc(1, sizeof(*query));
  if (query == NULL) {
    errno = ENOMEM;
    return -1;
  }
  test_live_queries_add();
  query->name = (char *)malloc(name_len + 1);
  if (query->name == NULL) {
    free_query_storage(query);
    errno = ENOMEM;
    return -1;
  }
  memcpy(query->name, name, name_len);
  query->name[name_len] = '\0';
  query->port = port;
  query->family = family;
  query->on_done = on_done;
  query->user_data = user_data;
  if (snprintf(query->service, sizeof(query->service), "%u", (unsigned)port) <
      0) {
    free_query_storage(query);
    errno = EIO;
    return -1;
  }

  struct ares_options options;
  memset(&options, 0, sizeof(options));
  options.flags = ARES_FLAG_NOALIASES | ARES_FLAG_NOSEARCH;
  options.lookups = (char *)"b";
  options.sock_state_cb = dns_sock_state_cb;
  options.sock_state_cb_data = query;
  int optmask = ARES_OPT_FLAGS | ARES_OPT_LOOKUPS | ARES_OPT_SOCK_STATE_CB;
  if (resolver->timeout_ms > 0) {
    options.timeout = resolver->timeout_ms;
    optmask |= ARES_OPT_TIMEOUTMS;
  }
  if (resolver->tries > 0) {
    options.tries = resolver->tries;
    optmask |= ARES_OPT_TRIES;
  }

  int status = dns_ares_init_options(&query->channel, &options, optmask);
  if (status != ARES_SUCCESS) {
    const int err = map_setup_status(status);
    free_query_storage(query);
    errno = err;
    return -1;
  }

  if (resolver->servers_csv != NULL) {
    status =
        dns_ares_set_servers_ports_csv(query->channel, resolver->servers_csv);
    if (status != ARES_SUCCESS) {
      const int err = map_setup_status(status);
      destroy_channel(query);
      free_query_storage(query);
      errno = err;
      return -1;
    }
  }

  link_query(resolver, query);

  struct ares_addrinfo_hints hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = family;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = ARES_AI_NUMERICSERV | ARES_AI_NOSORT;

  dns_ares_getaddrinfo(query, query->name, query->service, &hints);
  if (after_cares_entrypoint(query, 1) != 0) {
    const int err = errno;
    cleanup_query(query);
    errno = err;
    return -1;
  }

  query->published = 1;
  *out = query;
  return 0;
}

void odin_dns_query_destroy(odin_dns_query_t *query) {
  if (query == NULL) {
    return;
  }
  query->destroying = 1;
  query->suppress_callbacks = 1;
  cleanup_query(query);
}

void odin_dns_resolver_destroy(odin_dns_resolver_t *resolver) {
  if (resolver == NULL) {
    return;
  }
  odin_dns_query_t *query = resolver->queries;
  resolver->queries = NULL;
  while (query != NULL) {
    odin_dns_query_t *next = query->next;
    query->resolver = NULL;
    query->prev = NULL;
    query->next = NULL;
    query->destroying = 1;
    query->suppress_callbacks = 1;
    cleanup_query(query);
    query = next;
  }
  free(resolver->servers_csv);
  free(resolver);
  test_live_resolvers_sub();
}

#if defined(ODIN_DNS_RESOLVER_TESTING)
int odin_dns_resolver_test_reset_liveness(void) {
  pthread_mutex_lock(&g_test_mu);
  if (g_live.resolvers != 0 || g_live.queries != 0 || g_live.watches != 0 ||
      g_live.timers != 0 || g_live.cares_channels != 0 ||
      g_live.cares_results != 0) {
    pthread_mutex_unlock(&g_test_mu);
    errno = EBUSY;
    return -1;
  }
  memset(&g_obs, 0, sizeof(g_obs));
  g_steps_len = 0;
  g_fail_next_result_alloc = 0;
  pthread_mutex_unlock(&g_test_mu);
  return 0;
}

int odin_dns_resolver_test_liveness(odin_dns_resolver_test_liveness_t *out) {
  if (out == NULL) {
    errno = EINVAL;
    return -1;
  }
  pthread_mutex_lock(&g_test_mu);
  *out = g_live;
  pthread_mutex_unlock(&g_test_mu);
  return 0;
}

int odin_dns_resolver_test_cares_observation(
    odin_dns_resolver_test_cares_observation_t *out) {
  if (out == NULL) {
    errno = EINVAL;
    return -1;
  }
  pthread_mutex_lock(&g_test_mu);
  *out = g_obs;
  pthread_mutex_unlock(&g_test_mu);
  return 0;
}

int odin_dns_resolver_test_push_cares_step(
    const odin_dns_resolver_test_cares_step_t *step) {
  if (step == NULL) {
    errno = EINVAL;
    return -1;
  }
  pthread_mutex_lock(&g_test_mu);
  if (g_steps_len == g_steps_cap) {
    const size_t new_cap = g_steps_cap == 0 ? 8 : g_steps_cap * 2;
    if (new_cap < g_steps_cap) {
      pthread_mutex_unlock(&g_test_mu);
      errno = ENOMEM;
      return -1;
    }
    odin_dns_resolver_test_cares_step_t *new_steps =
        (odin_dns_resolver_test_cares_step_t *)realloc(
            g_steps, new_cap * sizeof(new_steps[0]));
    if (new_steps == NULL) {
      pthread_mutex_unlock(&g_test_mu);
      errno = ENOMEM;
      return -1;
    }
    g_steps = new_steps;
    g_steps_cap = new_cap;
  }
  g_steps[g_steps_len++] = *step;
  pthread_mutex_unlock(&g_test_mu);
  return 0;
}

int odin_dns_resolver_test_fail_next_result_alloc(void) {
  pthread_mutex_lock(&g_test_mu);
  g_fail_next_result_alloc = 1;
  pthread_mutex_unlock(&g_test_mu);
  return 0;
}

int odin_dns_resolver_test_first_watch(odin_dns_query_t *query,
                                       odin_event_io_t **out_io, int *out_fd) {
  if (query == NULL || out_io == NULL || out_fd == NULL) {
    errno = EINVAL;
    return -1;
  }
  for (odin_dns_watch_t *watch = query->watches; watch != NULL;
       watch = watch->next) {
    if (watch->io != NULL) {
      *out_io = watch->io;
      *out_fd = watch->fd;
      return 0;
    }
  }
  *out_io = NULL;
  *out_fd = -1;
  errno = ENOENT;
  return -1;
}
#endif
