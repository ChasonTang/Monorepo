# RFC-030: Async DNS Resolver

## 1. Summary

Add `odin/dns_resolver.c` and `odin/dns_resolver.h`, an owner-thread Odin resolver that uses c-ares `ares_getaddrinfo` through `odin_event_loop_t` and returns callback-scoped `sockaddr_storage` results without blocking or starting a resolver thread.

## 2. Goals

- **G1.** Resolve a hostname or numeric IP literal into one or more `sockaddr` results on the Odin event-loop owner thread, preserving the caller's port and requested address family.
- **G2.** Report validation, DNS, timeout, and c-ares setup failures as observable `ODIN_DNS_ERROR` completions or synchronous `-1`/`errno` returns, with no result addresses and no leaked query state.
- **G3.** Drive c-ares from Odin's existing event loop without c-ares event threads, without re-entrant user callbacks from `odin_dns_resolve_start`, and with query destruction legal before completion or from inside the completion callback.

## 3. Design

### 3.1 Overview

`odin/dns_resolver` is a new leaf module between callers that have a host slice and Odin's existing nonblocking dialer. Callers create one resolver on a live `odin_event_loop_t`, then start one or more query handles. Each query owns its own c-ares channel so `odin_dns_query_destroy` can abort exactly that query without canceling sibling lookups. The resolver copies c-ares `ares_addrinfo` nodes into Odin-owned `sockaddr_storage` entries, frees the c-ares result, and delivers the copied slice to the user callback.

This RFC does not wire DNS into `odin_server_session`; that follow-up can replace the current IPv4-literal path after this importable resolver is proven. Until then, production proxy behavior remains unchanged.

```text
caller host slice + port
    |
    v
odin_dns_resolve_start
    |
    v
per-query c-ares channel with ARES_OPT_SOCK_STATE_CB
    |
    +--> recorded c-ares socket state -> post-c-ares I/O reconcile
    +--> post-c-ares ares_timeout     -> Odin timer start/reset/stop
    |
    v
ares_getaddrinfo callback records result -> outer Odin finalizer copies results
```

### 3.2 Detailed Design

#### 3.2.1 Public API and Validation

```c
/* odin/dns_resolver.h */
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "odin/event_loop.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ODIN_DNS_NAME_MAX 255

typedef struct odin_dns_resolver_t odin_dns_resolver_t;
typedef struct odin_dns_query_t odin_dns_query_t;

typedef enum odin_dns_status_t {
  ODIN_DNS_OK = 0,
  ODIN_DNS_ERROR,
} odin_dns_status_t;

typedef struct odin_dns_resolver_config_t {
  const char *servers_csv;
  int timeout_ms;
  int tries;
} odin_dns_resolver_config_t;

typedef struct odin_dns_addr_t {
  struct sockaddr_storage addr;
  socklen_t addrlen;
  int ttl;
} odin_dns_addr_t;

typedef void (*odin_dns_cb)(odin_dns_query_t *query, odin_dns_status_t status,
                            int err, const odin_dns_addr_t *addrs,
                            size_t addr_count, void *user_data);

int odin_dns_resolver_create(odin_event_loop_t *loop,
                             const odin_dns_resolver_config_t *config,
                             odin_dns_resolver_t **out);
int odin_dns_resolve_start(odin_dns_resolver_t *resolver, const char *name,
                           size_t name_len, uint16_t port, int family,
                           odin_dns_cb on_done, void *user_data,
                           odin_dns_query_t **out);
void odin_dns_query_destroy(odin_dns_query_t *query);
void odin_dns_resolver_destroy(odin_dns_resolver_t *resolver);

#ifdef __cplusplus
}
#endif
```

**Unstated contract.** Public API calls that dereference a live loop, resolver, or query are owner-thread APIs under `odin/event_loop.h`'s owner assertion model: `odin_dns_resolver_create` with a non-NULL loop, `odin_dns_resolve_start`, `odin_dns_query_destroy(query != NULL)`, and `odin_dns_resolver_destroy(resolver != NULL)` must run on the corresponding Odin event-loop owner thread. The only affinity-free destroy calls are `odin_dns_query_destroy(NULL)` and `odin_dns_resolver_destroy(NULL)`, which are no-ops and perform no event-loop access. `config == NULL` uses system c-ares server configuration, c-ares' default timeout and tries, and the DNS-only name lookup policy from §3.2.2: no `HOSTALIASES` alias rewriting, no search-domain expansion, and no hosts-file lookup path. `config != NULL && config->servers_csv == NULL` has the same server-selection behavior while still honoring nonzero timeout and tries fields. When present, `config->servers_csv` is copied by `odin_dns_resolver_create` and later applied to each query with `ares_set_servers_ports_csv`; if c-ares rejects that CSV as malformed or unsupported while starting a query, `odin_dns_resolve_start` returns `-1/EINVAL`, leaves `*out` unchanged, and leaves no live query. Allocation or internal setup failures from the same call use the setup status mapping below. `timeout_ms == 0` and `tries == 0` keep c-ares defaults, while negative values return `-1/EINVAL` and leave `*out` unchanged.

The resolver initializes c-ares once per process with `ares_library_init(ARES_LIB_INIT_ALL)`, but the first `odin_dns_resolver_create` that can call `ares_library_init` is a process-start contract: it must run while the process is still single-threaded, before any resolver owner thread, DNS fixture thread, or other helper thread is started. A process that first creates an Odin DNS resolver after additional threads already exist is outside this RFC's supported contract; the process-global static `pthread_mutex_t` does not make that first c-ares call legal from already-running threads. The mutex protects only Odin's guard state and the cached success/failure path. After the guard reaches succeeded, two different owner threads may create resolvers concurrently and return from the cached-success path without calling `ares_library_init`. The guard state is tri-state: untried, succeeded, or failed with a cached `errno`. Success is cached permanently; failure is also cached permanently, so all later `odin_dns_resolver_create` calls return `-1` with the same cached `errno` without calling `ares_library_init` again. This RFC does not call `ares_library_cleanup`. If `ares_library_init` fails during the single-threaded first-create phase, `odin_dns_resolver_create` returns `-1` with `errno` mapped from the c-ares status and leaves `*out` unchanged. If `ares_init_options` or `ares_set_servers_ports_csv` fails while starting a query, `odin_dns_resolve_start` destroys any partially created channel outside c-ares callbacks, returns `-1` with mapped `errno`, leaves `*out` unchanged, and leaves no live query. `ARES_ENOMEM` maps to `ENOMEM`; `ARES_EFORMERR`, `ARES_EBADSTR`, and `ARES_ESERVICE` map to `EINVAL`; other setup statuses map to `EIO` unless §3.2.3 names a more specific mapping.

```text
ensure_cares_library_initialized():
  lock cares_init_mutex
  if cares_init_state == succeeded:
    unlock; return 0
  if cares_init_state == failed:
    errno = cached_errno; unlock; return -1
  status = ares_library_init(ARES_LIB_INIT_ALL)
  if status == ARES_SUCCESS:
    cares_init_state = succeeded
    unlock; return 0
  cached_errno = map status
  cares_init_state = failed
  errno = cached_errno
  unlock; return -1
```

`odin_dns_resolve_start` rejects `name == NULL`, `name_len == 0`, `name_len > ODIN_DNS_NAME_MAX`, embedded NUL in the supplied slice, unsupported `family` values outside `AF_UNSPEC`, `AF_INET`, and `AF_INET6`, `on_done == NULL`, or `out == NULL` with `-1` and `errno` set, before allocating a query or calling c-ares. It copies exactly `name_len` bytes plus one terminating NUL before passing the name to c-ares, so a peer-supplied embedded NUL cannot truncate the DNS question. `port` is formatted as a decimal service string and copied into every returned socket address. If c-ares records `ARES_ECANCELLED` or `ARES_EDESTRUCTION` before `odin_dns_resolve_start` can publish the query handle, the start call fails synchronously with `-1/ECANCELED`, leaves `*out` unchanged, and never invokes `on_done`. Successful `start` writes the query handle to `*out`; every user-visible completion callback receives that same query handle as its first argument and the original `user_data` pointer verbatim. Successful `start` never invokes `on_done` before returning, even when c-ares completes synchronously for numeric literals or local validation errors.

Satisfies: G1 via the resolver, query, address, name, port, and family surface; G2 via synchronous validation returns and exact no-result error contract; G3 via owner-thread API shape and the non-reentrant callback rule.

#### 3.2.2 c-ares Event-Loop Driver

Each query owns one c-ares channel initialized with:

```text
options.sock_state_cb = dns_sock_state_cb
options.sock_state_cb_data = query
options.flags = ARES_FLAG_NOALIASES | ARES_FLAG_NOSEARCH
options.lookups = "b"
optmask includes ARES_OPT_SOCK_STATE_CB, ARES_OPT_FLAGS, and ARES_OPT_LOOKUPS
if timeout_ms > 0: options.timeout = timeout_ms; optmask includes ARES_OPT_TIMEOUTMS
if tries > 0: options.tries = tries; optmask includes ARES_OPT_TRIES

hints.ai_family = requested family
hints.ai_socktype = SOCK_STREAM
hints.ai_protocol = IPPROTO_TCP
hints.ai_flags = ARES_AI_NUMERICSERV | ARES_AI_NOSORT
```

**Unstated contract.** `ARES_OPT_EVENT_THREAD` is never set. `ARES_OPT_FLAGS` is always set with `options.flags` including both `ARES_FLAG_NOALIASES` and `ARES_FLAG_NOSEARCH`, and `ARES_OPT_LOOKUPS` is always set with `options.lookups == "b"`; together those options make c-ares skip `HOSTALIASES` alias rewriting, local search-domain expansion, and the hosts-file lookup path before DNS resolution. For an accepted non-literal peer name, the DNS question name must equal the copied peer name exactly. c-ares callbacks run as locked c-ares frames, so Odin's c-ares callbacks do not call `ares_timeout`, `ares_destroy`, `ares_process_fds`, `odin_event_io_start`, `odin_event_io_update`, `odin_event_io_stop`, `odin_event_timer_start`, `odin_event_timer_reset`, or `odin_event_timer_stop`. The callbacks only record the desired fd state, result status, result pointer, and dirty/finalize flags on the query. The socket-state `readable` and `writable` booleans are independent: `readable == 0 && writable == 1` is a valid desired mask and starts or updates an `ODIN_EVENT_WRITE` watch rather than being treated as watch removal. Odin reconciles recorded flags only after `ares_getaddrinfo` or `ares_process_fds` has returned to the outer Odin frame. `ARES_AI_NOSORT` is required: c-ares must not perform RFC6724 address-sort connection probes to peer-selected result addresses before an upstream caller has applied its own dial policy. This RFC's runtime evidence executes on the macOS host backend only; Linux, alternate macOS arch, iOS simulator, and iOS device builds compile the same c-ares/Odin driver but are not executed in this environment.

The query owns one Odin timer for c-ares timeouts. The post-c-ares reconciliation step calls `ares_timeout(channel, NULL, &tv)` only outside c-ares callbacks. A non-null timeout starts or resets a one-shot Odin timer for that interval; a null timeout stops any active timeout timer, clears the query's timer handle, and does not finalize the query. The timer callback calls `ares_process_fds(channel, NULL, 0, ARES_PROCESS_FLAG_NONE)` and then runs post-c-ares reconciliation if the query is still pending. An I/O callback converts Odin readiness into one `ares_fd_events_t`, mapping `ODIN_EVENT_READ` to `ARES_FD_EVENT_READ`, `ODIN_EVENT_WRITE` to `ARES_FD_EVENT_WRITE`, and `ODIN_EVENT_ERROR` to `ARES_FD_EVENT_READ` because c-ares defines read readiness as including disconnect/error. A pure Odin error event is therefore never passed as `ARES_FD_EVENT_NONE`.

All driver API failures use one fatal internal-error path. If `ares_process_fds` returns non-success, the driver maps that status through §3.2.3 and records a fatal completion. If `odin_event_io_start`, `odin_event_io_update`, `odin_event_timer_start`, or `odin_event_timer_reset` returns `-1`, the driver saves `errno` and records a fatal completion with that errno. Fatal completion stops existing watches and the c-ares timeout timer, suppresses later c-ares destruction callbacks, and finalizes outside c-ares callbacks with `ODIN_DNS_ERROR`, no addresses, and no live watches, timers, c-ares channels, or c-ares results; completed query-handle ownership follows §3.2.3.

**Mechanism.**

```text
dns_sock_state_cb(query, fd, readable, writable):
  mask = readable ? READ : 0
  mask |= writable ? WRITE : 0
  record desired mask for fd
  query.sock_state_dirty = true
  query.timeout_dirty = true

after_cares_entrypoint(query, came_from_start):
  if query has c-ares result: mark completion pending
  if completion pending:
    stop watches and timeout timer
    if came_from_start and status is ARES_ECANCELLED or ARES_EDESTRUCTION:
      destroy partial channel/query outside c-ares; fail start with ECANCELED
      return
    if came_from_start: arm 0-delay finalizer timer or fail start with saved errno
    else: run finalizer now from the outer Odin callback
    return
  if query.sock_state_dirty:
    for each recorded fd:
      watch = find_watch(query, fd)
      if desired mask == 0 and watch exists: stop and remove watch
      if desired mask != 0 and watch exists: update watch or enter fatal
      if desired mask != 0 and watch missing: start watch or enter fatal
  if fatal was entered during reconciliation:
    stop watches and timeout timer
    if came_from_start: arm 0-delay finalizer timer or fail start with saved errno
    else: run finalizer now from the outer Odin callback
    return
  if query still pending and query.timeout_dirty:
    refresh_timeout_or_enter_fatal(query)
  if fatal was entered during timeout refresh:
    stop watches and timeout timer
    if came_from_start: arm 0-delay finalizer timer or fail start with saved errno
    else: run finalizer now from the outer Odin callback

refresh_timeout_or_enter_fatal(query):
  tvp = ares_timeout(query.channel, NULL, &tv)
  if tvp == NULL:
    if query.timeout_timer active: stop and clear it
    return
  timeout_us = tvp converted to uint64_t microseconds or enter fatal EOVERFLOW
  if query.timeout_timer active: reset timer or enter fatal
  else: start timer or enter fatal

on_io(loop, io, fd, events, watch):
  ares_events.fd = fd
  ares_events.events = map READ/WRITE/ERROR from events
  status = ares_process_fds(query.channel, &ares_events, 1, ARES_PROCESS_FLAG_NONE)
  if status != ARES_SUCCESS: enter fatal internal error
  if query still pending: after_cares_entrypoint(query, false)

on_timeout(loop, timer, query):
  status = ares_process_fds(query.channel, NULL, 0, ARES_PROCESS_FLAG_NONE)
  if status != ARES_SUCCESS: enter fatal internal error
  if query still pending: after_cares_entrypoint(query, false)
```

Satisfies: G1 via the fd and timer path that lets c-ares complete real DNS queries; G2 via timeout processing; G3 via c-ares socket-state mirroring into Odin I/O and timer handles without c-ares threads.

#### 3.2.3 Result Copying, Error Mapping, and Lifecycle

```text
c-ares status mapping:
  ARES_SUCCESS                         -> ODIN_DNS_OK, err = 0
  ARES_ENOTFOUND                       -> ODIN_DNS_ERROR, err = EHOSTUNREACH
  ARES_ENODATA                         -> ODIN_DNS_ERROR, err = EHOSTUNREACH
  ARES_ETIMEOUT                        -> ODIN_DNS_ERROR, err = ETIMEDOUT
  ARES_ECONNREFUSED                    -> ODIN_DNS_ERROR, err = ECONNREFUSED
  ARES_EREFUSED                        -> ODIN_DNS_ERROR, err = ECONNREFUSED
  ARES_ENOSERVER                       -> ODIN_DNS_ERROR, err = ECONNREFUSED
  ARES_ENOMEM                          -> ODIN_DNS_ERROR, err = ENOMEM
  ARES_ENOTIMP                         -> ODIN_DNS_ERROR, err = EAFNOSUPPORT
  ARES_ESERVICE                        -> ODIN_DNS_ERROR, err = EINVAL
  ARES_EFORMERR                        -> ODIN_DNS_ERROR, err = EINVAL
  ARES_EBADSTR                         -> ODIN_DNS_ERROR, err = EINVAL
  ARES_ECANCELLED                      -> start-path -1/ECANCELED before publish;
                                          destroy-driven abort suppresses callback;
                                          otherwise ODIN_DNS_ERROR/ECANCELED
  ARES_EDESTRUCTION                    -> start-path -1/ECANCELED before publish;
                                          destroy-driven abort suppresses callback;
                                          otherwise ODIN_DNS_ERROR/ECANCELED
  ARES_EBADRESP                        -> ODIN_DNS_ERROR, err = EIO
```

**Unstated contract.** The c-ares result callback records `status` and the `ares_addrinfo *` result pointer on the query, ignores the c-ares `timeouts` callback parameter, sets `completion_pending`, and returns without invoking Odin callbacks, mutating Odin event-loop registrations, or calling channel-taking c-ares APIs. The status table above is the stable errno contract for this RFC; unlisted c-ares non-success statuses may be reported as `ODIN_DNS_ERROR/EIO`, but they are not compatibility-pinned until a follow-up RFC names the status and adds executable coverage. The outer finalizer, reached only after `ares_getaddrinfo` or `ares_process_fds` has returned, copies every `ares_addrinfo_node` from an `ARES_SUCCESS` `AF_INET` / `AF_INET6` result into a contiguous `odin_dns_addr_t` array with `ttl` and an exact family length: `addrlen == sizeof(struct sockaddr_in)` for IPv4 and `addrlen == sizeof(struct sockaddr_in6)` for IPv6. `addrlen` is never `sizeof(struct sockaddr_storage)` and never a placeholder positive value. If result copying fails, the final completion is `ODIN_DNS_ERROR/ENOMEM` with no addresses. The c-ares `ares_addrinfo` pointer is always released with `ares_freeaddrinfo` before the user callback runs, including the result-copy allocation failure path. The `addrs` pointer passed to `on_done` is valid only until `on_done` returns; callers that need to retain addresses copy them. User-visible error completions receive `addrs == NULL` and `addr_count == 0`.

If `ARES_ECANCELLED` or `ARES_EDESTRUCTION` is recorded before `odin_dns_resolve_start` publishes `*out`, the start path rolls back the partial query, returns `-1/ECANCELED`, leaves `*out` unchanged, and invokes no callback. If either status is recorded after a successful start while the query is not already inside caller-initiated query destroy or resolver destroy, the finalizer reports one ordinary `ODIN_DNS_ERROR/ECANCELED` callback and leaves the completed handle destroyable by the caller. A status recorded while an explicit destroy flag is already set remains an internal abort: the destroy path has already made the handle dead, so Odin suppresses the callback while stopping watches and the timeout timer, destroying the channel if still owned, unlinking the query, and freeing it. Odin never calls `ares_cancel`; `ARES_ECANCELLED` is retained as a defensive c-ares status and is reachable in tests only through the c-ares status wrapper.

Completion from the `odin_dns_resolve_start` call path is deferred through a 0-delay Odin finalizer timer, so `odin_dns_resolve_start` never re-enters user code. If that finalizer timer cannot be started, `odin_dns_resolve_start` destroys the channel outside c-ares callbacks, frees the query, returns `-1` with the timer `errno`, leaves `*out` unchanged, and never invokes `on_done`. Completion reached from an Odin I/O or timeout callback may run the finalizer directly after `ares_process_fds` returns because it is already outside c-ares and outside the public start frame.

Before invoking `on_done`, the finalizer stops every active Odin I/O watch, stops the c-ares timeout timer, frees any c-ares result pointer, destroys the c-ares channel with destruction callbacks suppressed, moves its result array into a local pointer, and marks the query completed while leaving the handle on the resolver's owned-query list. The finalizer passes the same query handle returned by `odin_dns_resolve_start` and the original `user_data` pointer to `on_done` without substitution. The user callback is the query's final action except for freeing that local result array after the callback returns; no query fields are read after `on_done` returns. `odin_dns_query_destroy(query)` is therefore legal from inside `on_done`; it unlinks and frees the completed query immediately, while the finalizer returns using only stack-local state. If the callback does not destroy the query, the completed handle remains caller-addressable and resolver-owned until `odin_dns_query_destroy(query)` or `odin_dns_resolver_destroy(resolver)`. Destroying a pending query stops its watches and timer, destroys the c-ares channel outside c-ares callbacks while suppressing `ARES_EDESTRUCTION` callbacks, frees copied or still-pending results, unlinks the query, and never invokes `on_done`. Destroying a resolver aborts pending queries, frees completed-but-undestroyed query handles, and makes every query pointer returned by that resolver dead afterward. All non-NULL destroy calls in this paragraph are owner-thread calls per §3.2.1 before they touch Odin event-loop watches or timers; `odin_dns_resolver_destroy(NULL)` and `odin_dns_query_destroy(NULL)` are affinity-free no-ops.

Under `ODIN_DNS_RESOLVER_TESTING`, `odin/testing/dns_resolver_internal_test.h` exposes concrete liveness hooks implemented by `dns_resolver.c` at the resolver/query/watch/timer allocation and free sites:

```c
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
} odin_dns_resolver_test_cares_observation_t;

#define ODIN_DNS_TEST_CARES_LIBRARY_INIT 1
#define ODIN_DNS_TEST_CARES_INIT_OPTIONS 2
#define ODIN_DNS_TEST_CARES_SET_SERVERS 3
#define ODIN_DNS_TEST_CARES_RESULT_STATUS 4
#define ODIN_DNS_TEST_CARES_PROCESS_FDS_STATUS 5
#define ODIN_DNS_TEST_CARES_TIMEOUT_TIMEVAL 6
#define ODIN_DNS_TEST_CARES_SOCK_STATE 7
#define ODIN_DNS_TEST_CARES_TIMEOUT_NULL 8

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
int odin_dns_resolver_test_liveness(
    odin_dns_resolver_test_liveness_t *out);
int odin_dns_resolver_test_cares_observation(
    odin_dns_resolver_test_cares_observation_t *out);
int odin_dns_resolver_test_push_cares_step(
    const odin_dns_resolver_test_cares_step_t *step);
int odin_dns_resolver_test_fail_next_result_alloc(void);
int odin_dns_resolver_test_first_watch(odin_dns_query_t *query,
                                       odin_event_io_t **out_io,
                                       int *out_fd);

#ifdef __cplusplus
}
#endif
#endif
```

Under `ODIN_DNS_RESOLVER_TESTING`, the liveness and observation counters are protected by a process-local `pthread_mutex_t`; every allocation/free-site increment or decrement, wrapper-call count, reset, and read path holds that mutex. `reset_liveness` requires all counts to be zero while holding the mutex before resetting process-local counters and clearing test observations; it does not reset the resolver's process-global c-ares init guard. Because reset is not a fresh-process substitute, no `OdinDnsResolverTest` parent frame calls `odin_dns_resolver_create` or `odin_dns_resolve_start`; every row or subcase that may create a resolver executes inside the row's fork/deadline child, except the explicitly named fork+exec T16 library-init failure and T18 cached-success concurrency cases. `liveness` and `cares_observation` return `-1/EINVAL` when `out == NULL`. `liveness.cares_channels` increments after a successful `ares_init_options` wrapper and decrements at the `ares_destroy` wrapper; `liveness.cares_results` increments when the result callback records a non-null `ares_addrinfo *` and decrements at the `ares_freeaddrinfo` wrapper. `cares_observation.ares_library_init_calls`, `cares_observation.ares_destroy_calls`, and `cares_observation.ares_freeaddrinfo_calls` count those wrapper calls even when the live counts return to zero.

The production `ares_library_init`, `ares_init_options`, and `ares_getaddrinfo` calls are wrapped only under `ODIN_DNS_RESOLVER_TESTING`; the library-init wrapper counts actual calls after the single-threaded first-create path admits the untried state, the init-options wrapper records the optmask, `options->flags`, a NUL-terminated copy of `options->lookups`, timeout, and tries passed to c-ares, and the getaddrinfo wrapper records one observation per actual call from the resolver start path, including `hints->ai_flags` and `hints->ai_family`, then calls real c-ares. `push_cares_step` is a FIFO fault-injection wrapper at the resolver's other c-ares call sites only: setup operations replace the next `ares_library_init`, `ares_init_options`, or `ares_set_servers_ports_csv` status; the `ODIN_DNS_TEST_CARES_LIBRARY_INIT` operation is executable only before the current process' c-ares init guard has succeeded, so tests that cover it run in the child-only `OdinDnsResolverExecChild.T16LibraryInit` test selected by `--gtest_filter=OdinDnsResolverExecChild.T16LibraryInit` with `ODIN_DNS_EXEC_CHILD=T16_LIBRARY_INIT` before any resolver create or helper-thread start in the exec'd process. That child-only test first validates the mode, pushes `ODIN_DNS_TEST_CARES_LIBRARY_INIT`, calls `odin_dns_resolver_create`, asserts `-1/ENOMEM` and zero liveness, calls `odin_dns_resolver_create` once more to assert the cached failure without a second `ares_library_init` wrapper call, and exits without launching another child or running any `OdinDnsResolverTest.*` row body. `OdinDnsResolverExecChild.T18CachedSuccessConcurrency`, selected only by `--gtest_filter=OdinDnsResolverExecChild.T18CachedSuccessConcurrency` with `ODIN_DNS_EXEC_CHILD=T18_CACHED_SUCCESS_CONCURRENCY`, owns the T18 pre-init and pthread assertions directly and also never launches a child. Each child-only test checks `ODIN_DNS_EXEC_CHILD` and the active gtest filter before resolver creation: the exact expected mode runs the assertions; an absent mode with direct child-only selection fails before resolver creation; an absent mode in an ordinary unfiltered full-suite run records `GTEST_SKIP()` before resolver creation; and a wrong non-empty mode fails before resolver creation. Direct child-only selection means the active filter is exactly `OdinDnsResolverExecChild.T16LibraryInit`, exactly `OdinDnsResolverExecChild.T18CachedSuccessConcurrency`, or `OdinDnsResolverExecChild.*`, so a fork+exec launcher that omits `ODIN_DNS_EXEC_CHILD` cannot produce a skipped zero exit for T16 or T18. Plain fork is not sufficient for these two modes because it inherits an already-succeeded guard from the parent.

Other setup-failure steps may run in the ordinary fork/deadline row child because they occur after that child creates its resolver and before per-query setup succeeds; they never run in the gtest parent. `RESULT_STATUS` verifies `expect_process_events` when it is nonzero and the result is injected from `ares_process_fds`, then invokes the normal result-recording callback with the requested status from the next `ares_getaddrinfo` or `ares_process_fds` wrapper entered for a query. If that status is `ARES_ECANCELLED` or `ARES_EDESTRUCTION` and the wrapper is consumed by start-path `ares_getaddrinfo`, the wrapper drives the synchronous `-1/ECANCELED` rollback instead of publishing a dead handle. `PROCESS_FDS_STATUS` verifies `expect_process_events` when it is nonzero, verifies `events == NULL && nevents == 0` when `expect_process_null_events` is nonzero, and then returns the scripted status from the next `ares_process_fds` wrapper after any scripted sock-state callback. `TIMEOUT_TIMEVAL` replaces the next `ares_timeout` result with a non-null `struct timeval` whose fields are copied from `timeout_tv_sec` and `timeout_tv_usec`; tests use it both for ordinary non-null timers and for T14's overflowing raw-timeval branch. `TIMEOUT_NULL` makes the next `ares_timeout` wrapper return null without writing the caller's `timeval`. `SOCK_STATE` invokes the production sock-state recorder for the query's active c-ares fd with the scripted readable/writable bits. `fail_next_result_alloc` makes the next result-copy allocation site fail before any `odin_dns_addr_t` array is published; it is consumed only by an `ARES_SUCCESS` finalizer. `first_watch` returns `-1/ENOENT` when the query has no active watch, and on success writes both the active Odin I/O handle and its watched fd. Tests use the handle with `odin_event_loop_test_dispatch_backend_events` and use the fd with `odin_event_loop_test_kqueue_registered_mask`.

`dns_resolver_unittests.cpp` also includes the sibling `odin/testing/event_loop_internal_test.h` hooks `odin_event_loop_test_dispatch_backend_events`, `odin_event_loop_test_fail_next_timer_start`, `odin_event_loop_test_set_now_us`, `odin_event_loop_test_prepare_wait`, `odin_event_loop_test_kqueue_registered_mask`, and, on Apple-only subcases, `odin_event_loop_test_fail_next_kqueue_change`; that header declares those symbols only under `ODIN_EVENT_LOOP_TESTING`, so §6 P1 applies `:odin_event_loop_testing_config` directly to the DNS resolver test source set. `odin_event_loop_test_fail_next_timer_start(loop, err)` is a one-shot hook consumed by the next `odin_event_timer_start` on that loop; T14 timer-start-failure subcases therefore arm the row watchdog first, then arm this hook, then immediately execute the resolver action whose next timer start is the DNS timeout timer or 0-delay finalizer timer. Production targets do not declare or define these symbols.

Satisfies: G1 via copied `sockaddr_storage` results; G2 via the c-ares status-to-errno mapping and no-address error rule; G3 via deferred completion, exact abort semantics, and callback-safe query destruction.

## 4. Security

- **S1.**
  - **Threat:** Peer-supplied host bytes contain an embedded NUL or exceed the resolver's DNS-name buffer, causing the c-ares query to use a truncated or out-of-bounds name.
  - **Mitigation:** §3.2.1 rejects empty, over-`ODIN_DNS_NAME_MAX`, and embedded-NUL name slices before allocation; accepted names are copied into a new NUL-terminated buffer whose terminator is outside the peer-supplied slice.
  - **Enforcement:** T7 fires NULL, empty, overlong, and embedded-NUL inputs and asserts synchronous rejection with no callback and no live query, while its 255-byte allow-side subcase proves the boundary does not reject the highest valid length.

- **S2.**
  - **Threat:** c-ares address sorting probes connect to peer-selected result addresses before the upstream caller's SSRF dial filter can evaluate those addresses.
  - **Mitigation:** §3.2.2 always sets `ARES_AI_NOSORT`, and this RFC does not wire resolver output into any `connect(2)` caller. A follow-up `odin_server_session` integration must pass each returned address through the existing `odin_server_session_set_dial_filter` hook before `odin_dial_start`.
  - **Enforcement:** T2 resolves a DNS answer through the production start path and asserts the test-only `ares_getaddrinfo` observation saw `ARES_AI_NOSORT` in `hints->ai_flags`; omitting the flag fails before any caller-connect policy is involved.

- **S3.**
  - **Threat:** Local `HOSTALIASES`, search-domain, or hosts-file policy rewrites peer-supplied host bytes before DNS, causing outbound resource selection to differ from the accepted peer name.
  - **Mitigation:** §3.2.2 always sets `ARES_FLAG_NOALIASES`, `ARES_FLAG_NOSEARCH`, and `lookups == "b"` before `ares_getaddrinfo`, so the DNS question name is the copied peer name rather than a locally rewritten alias.
  - **Enforcement:** T6 asserts the production `ares_init_options` path uses those DNS-only options and that the fixture-observed question for `aliaspeer` remains exactly `aliaspeer` under a configured `HOSTALIASES` file.

## 5. Testing Strategy

The `OdinDnsResolverTest` parent frame never calls `odin_dns_resolver_create` or `odin_dns_resolve_start`; it only resets observations, forks/waits, and checks child exit status. Every T1-T20 row or subcase that may create a resolver or start a query runs its body in a forked gtest child with a 2 s parent deadline and an in-loop watchdog timer; a row fails by assertion rather than by hanging the suite. The T16 library-init failure subcase and T18 concurrent cached-init row are the only fresh-process cases: the row process launches a fork+exec child of the current `odin_unittests` binary and waits with a 2 s `waitpid` deadline, but the exec target is never the same `OdinDnsResolverTest` row. T16 execs `out/dns_mac/odin_unittests --gtest_filter=OdinDnsResolverExecChild.T16LibraryInit` with `ODIN_DNS_EXEC_CHILD=T16_LIBRARY_INIT`; T18 execs `out/dns_mac/odin_unittests --gtest_filter=OdinDnsResolverExecChild.T18CachedSuccessConcurrency` with `ODIN_DNS_EXEC_CHILD=T18_CACHED_SUCCESS_CONCURRENCY`. Those `OdinDnsResolverExecChild.*` tests perform their resolver-create assertions directly, contain no fork/exec launcher path, fail before resolver creation when selected directly without `ODIN_DNS_EXEC_CHILD`, and skip only ordinary unfiltered full-suite runs when `ODIN_DNS_EXEC_CHILD` is absent. This exec boundary is required because a plain fork inherits the parent's process-global c-ares init guard. T18 performs its required successful `ares_library_init` through a main-thread resolver create/destroy before spawning the two owner-thread pthreads inside the child-only test. Any row child that needs a DNS fixture or other helper thread first performs the same single-threaded resolver create/destroy on a temporary loop while `cares_observation.ares_library_init_calls == 1`, then starts the helper thread and creates its real configured resolver through the cached-success path. The local DNS fixture is a test helper thread that owns only POSIX UDP/TCP sockets, uses receive/send deadlines, and never calls Odin owner-thread APIs. DNS response rows configure the resolver with `servers_csv = "127.0.0.1:<fixture-port>"`, `timeout_ms = 250`, and `tries = 1`; the fixture emits minimal A, AAAA, NXDOMAIN, or no-response behavior for the row. No row performs a blocking read without a socket deadline. T14's timer-start-failure subcases arm their in-loop watchdog before `odin_event_loop_test_fail_next_timer_start(loop, ENOMEM)`, then immediately trigger the target resolver path so the hook is consumed by the DNS timeout timer or numeric finalizer timer rather than by the watchdog. T14's two kqueue-change failure subcases are guarded with `#if defined(__APPLE__)`; the non-Apple branch records those subcases as `GTEST_SKIP()`-equivalent skips without calling `odin_event_loop_test_fail_next_kqueue_change`, and T14's non-kqueue driver-failure subcases still run where the binary is executed.

Split-risk note: T1-T20 reach the template hard cap of 20 rows. T17 and T18 remain here because active timeout refresh and process-global c-ares init caching are already part of the §3.2 resolver contract and need executable acceptance before any production caller is changed. Cycle 14 tightens existing T6/T17 assertions and coverage-matrix text so already specified DNS-only and fatal-driver contracts are executable; Cycle 15 tightens only existing API/lifecycle affinity and P1 red-skeleton behavior; Cycle 16 tightens existing T6/T18 assertions for the same DNS-only and init-lifecycle contracts; Cycle 17 tightens existing T4/T6 assertions and adds S3 only to trace an already specified peer-name trust boundary; Cycle 18 tightens existing process-isolation and T14 socket-state assertions without adding a row; Cycle 19 tightens existing status assertions and the test-header ABI snippet without adding scope; Cycle 20 splits the existing per-status assertions across T19/T20 to satisfy the per-row subcase cap without adding behavioral scope; Cycle 21 adds only missing setup-status subcases inside bounded T16 and keeps the T-row count unchanged; Cycle 22 only replaces ambiguous fork+exec filter prose with explicit child-only gtest modes and keeps the row count unchanged; Cycle 23 only makes direct missing-mode child selection fail-safe and keeps the row count unchanged; Cycle 24 only adds the missing `ares_set_servers_ports_csv` allocation-failure assertion to the existing setup-failure row, leaving T16 at its 8-subcase cap and the T-row count unchanged. Any further c-ares status/fault expansion requires a follow-up RFC split or an explicit exception before adding another T-row.

### 5.0 Coverage Matrix

| Axis | Value | Rows |
|------|-------|------|
| G# | G1 address results | T2, T3, T4 |
| G# | G2 failures and cleanup | T1, T4, T5, T6, T7, T8, T9, T11, T12, T13, T14, T15, T16, T17, T19, T20 |
| G# | G3 event-loop lifecycle | T2, T3, T4, T9, T10, T11, T12, T13, T14, T17, T18 |
| S# | S1 host-byte validation | T7 |
| S# | S2 no result-address probe before policy | T2 |
| S# | S3 local name-policy rewrite boundary | T6 |
| State | Resolver constructor default and invalid config | T1 |
| State | Pending query on c-ares socket-readiness path | T2, T3, T5 |
| State | Immediate c-ares callback path | T4 |
| State | Pending query destroyed before completion | T4, T9 |
| State | Query callback destroys query | T10 |
| State | Two sibling queries active | T9, T11 |
| State | Resolver destroyed with live or deferred-finalizer queries | T12 |
| State | Resolver destroyed with completed-but-undestroyed query handles | T12 |
| State | c-ares/Odin driver internal failure | T14, T17 |
| State | Concurrent resolver creation on two owner threads after single-threaded c-ares pre-init | T18 |
| Completion mode | Happy c-ares fd-driven completion | T2, T3 |
| Completion mode | Happy immediate completion deferred by Odin timer | T4 |
| Completion mode | Error fd-driven completion | T5, T13, T15, T19, T20 |
| Completion mode | Error timeout completion | T6, T17 |
| Completion mode | Active c-ares timeout refresh resets, fails, or stops while query remains pending | T17 |
| Decoder branch | A answer copied | T2, T10 |
| Decoder branch | AAAA answer copied | T3 |
| Decoder branch | A plus AAAA answers copied | T3 |
| Decoder branch | NXDOMAIN maps to `EHOSTUNREACH` | T5 |
| Decoder branch | `ARES_ENODATA` maps to `EHOSTUNREACH` | T5 |
| Decoder branch | Timeout maps to `ETIMEDOUT` | T6 |
| Config option application | DNS-only `ARES_OPT_FLAGS`/`ARES_FLAG_NOALIASES`/`ARES_FLAG_NOSEARCH` and `ARES_OPT_LOOKUPS`/`lookups == "b"`/no `ARES_OPT_EVENT_THREAD` apply for default, zero, and nonzero config; `ARES_OPT_TIMEOUTMS` and `ARES_OPT_TRIES` are omitted for default/zero and passed only when nonzero | T4, T6 |
| Config option application | `config != NULL && config->servers_csv == NULL` still applies nonzero timeout and tries while keeping DNS-only lookup policy | T4 |
| Config option application | `HOSTALIASES`, search-domain expansion, and hosts-file lookup cannot rewrite or bypass a single-label peer name before DNS lookup | T6 |
| Port boundary branch | Ports `0`, `1`, and `65535` are preserved as `htons(value)` in IPv4 and IPv6 results | T2, T3, T4 |
| Address family branch | Requested `AF_UNSPEC`, `AF_INET`, and `AF_INET6` are passed to c-ares and reflected in results | T3 |
| Decoder branch | Each of `ARES_ECONNREFUSED`, `ARES_EREFUSED`, and `ARES_ENOSERVER` maps to `ECONNREFUSED` | T19 |
| Decoder/setup branch | Result allocation status and both setup allocation branches, `ares_init_options` and `ares_set_servers_ports_csv`, map to `ENOMEM` | T16, T19 |
| Decoder branch | Unsupported family status maps to `EAFNOSUPPORT` | T19 |
| Decoder/setup branch | Each of `ARES_ESERVICE`, `ARES_EFORMERR`, and `ARES_EBADSTR` maps to `EINVAL` on the covered result and setup paths | T8, T16, T20 |
| Decoder/setup branch | Named default representative `ARES_EBADRESP` maps to `EIO` on the covered result and setup paths; unlisted c-ares statuses are outside this RFC's stable-errno contract | T16, T20 |
| Decoder branch | `ARES_ECANCELLED` and `ARES_EDESTRUCTION` start-path rollback and post-start error callback | T15 |
| Benign-vs-fatal split | `ARES_AI_NOSORT` asserted on the actual `ares_getaddrinfo` call | T2 |
| Benign-vs-fatal split | Fatal DNS or driver failure returns no addresses | T5, T6, T13, T14, T15, T17, T19, T20 |
| Constructor / factory precondition | Invalid resolver config | T1 |
| Constructor / factory precondition | Invalid query name | T7 |
| Constructor / factory precondition | Invalid query family, callback/output, or server CSV | T8 |
| Constructor / factory precondition | c-ares setup failure maps setup status, leaves no query, and invokes no callback | T16 |
| Process init branch | c-ares library init failure is cached and post-init concurrent success avoids additional c-ares init calls | T16, T18 |
| Public NULL contract | resolver/query destroy and liveness NULL handling | T1 |
| Public NULL contract | `name == NULL` and `out == NULL` handling | T7, T8 |
| Driver readiness branch | `ODIN_EVENT_ERROR` maps to c-ares read readiness and `ODIN_EVENT_WRITE` maps to c-ares write readiness, including a write-only c-ares socket-state watch | T13, T14 |
| Driver socket-state branch | benign desired-mask removal plus write-only and combined READ\|WRITE existing-watch updates | T14 |
| Driver timer branch | non-null timeout arms/resets and null timeout stops the active timer | T6, T14, T17 |
| Driver failure branch | I/O-readiness and timeout-callback `ares_process_fds` failures, Odin I/O/timer failures, active timer-reset failure, raw `ares_timeout` overflow, and start-path finalizer-timer failure clean up | T14, T17 |
| Result-copy branch | Odin result-copy allocation failure returns `ENOMEM` after freeing c-ares result | T15 |
| Cleanup observation | c-ares channel/result live counts and `ares_destroy`/`ares_freeaddrinfo` wrapper calls | T2, T9, T12, T14, T15, T16, T19, T20 |
| Callback argument contract | callback receives the start-returned query handle and original `user_data` pointer on success and representative error completions | T5, T6, T10 |
| Callback-safe lifecycle hand-off | destroy before deferred finalizer or inside callback | T4, T10, T12 |

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | Resolver constructor config validation and NULL destroy/liveness contracts | Subcases: create/destroy resolver with `config == NULL`; create/destroy with `config->servers_csv == NULL`, `timeout_ms = 0`, `tries = 0`; create with `timeout_ms = -1`; create with `tries = -1`; call `odin_dns_resolver_destroy(NULL)`; call `odin_dns_query_destroy(NULL)`; call `odin_dns_resolver_test_liveness(NULL)` and `odin_dns_resolver_test_cares_observation(NULL)` after reset | Default and null-server config creates return `0` and destroy returns all resolver/query/watch/timer/c-ares live counts to zero; negative timeout and tries each return `-1/EINVAL` and leave sentinel `*out` unchanged; resolver/query destroy NULL calls are no-ops with zero liveness; liveness NULL and c-ares-observation NULL each return `-1/EINVAL` without changing counts | G2 | unit |
| T2 | A lookup returns a stream address with no c-ares sort probe enabled | Local DNS fixture answers `probe.test` A `127.0.0.1` with TTL 60; create resolver from a mutable `servers_csv` buffer, then overwrite that buffer before starting the `AF_INET` query with port `0` | Callback fires once on owner thread with `ODIN_DNS_OK`, `err == 0`, `addr_count == 1`, `AF_INET`, address `127.0.0.1`, `sin_port == htons(0)`, `addrlen == sizeof(struct sockaddr_in)`, and `ttl == 60`, proving the resolver copied `servers_csv`, preserved the low port boundary, and copied the result TTL; `cares_observation` reports one actual `ares_getaddrinfo` call with `last_ai_family == AF_INET`, `ARES_AI_NUMERICSERV` and `ARES_AI_NOSORT` set, and one `ares_freeaddrinfo` call; post-destroy liveness is zero including `cares_channels` and `cares_results` | G1, G3, S2 | unit |
| T3 | Requested DNS family is passed to c-ares and filters dual-stack answers | Local DNS fixture answers `dual.test` with A `203.0.113.7` and AAAA `2001:db8::7`; subcases start the same name and port `65535` with `AF_UNSPEC`, `AF_INET`, and `AF_INET6` | Each subcase records exactly one `ares_getaddrinfo` observation with `last_ai_family` equal to the requested family and `last_ai_flags` containing `ARES_AI_NUMERICSERV` and `ARES_AI_NOSORT`; `AF_UNSPEC` returns both `203.0.113.7` with `sin_port == htons(65535)` and `addrlen == sizeof(struct sockaddr_in)` and `2001:db8::7` with `sin6_port == htons(65535)` and `addrlen == sizeof(struct sockaddr_in6)`, `AF_INET` returns only the exact-length A result, `AF_INET6` returns only the exact-length AAAA result, and liveness returns to zero | G1, G3 | unit |
| T4 | Numeric literals complete asynchronously and can be destroyed before the finalizer | Subcases: success for `"192.0.2.10"` with `AF_INET` and port `1`; success for `"2001:db8::10"` with `AF_INET6` and port `1`; success for `"192.0.2.14"` with `config == NULL`; success for `"192.0.2.15"` using config `{ servers_csv = NULL, timeout_ms = 0, tries = 0 }`; success for `"192.0.2.13"` using config `{ servers_csv = NULL, timeout_ms = 250, tries = 1 }`; destroy-before-finalizer for `"192.0.2.11"` with `AF_INET`; no DNS fixture packet is allowed; each start asserts a `start_returned` flag before any callback | Starts return `0` without inline callback; success callbacks later report the literal address, exact `addrlen`, IPv4/IPv6 port `htons(1)`, and no DNS packet. Default and zero-config subcases observe optmask containing `ARES_OPT_FLAGS` and `ARES_OPT_LOOKUPS`, omitting `ARES_OPT_EVENT_THREAD`, `ARES_OPT_TIMEOUTMS`, and `ARES_OPT_TRIES`, flags including `ARES_FLAG_NOALIASES` and `ARES_FLAG_NOSEARCH`, and `lookups == "b"`. Nonzero null-server config observes `ARES_OPT_TIMEOUTMS`/`ARES_OPT_TRIES` with timeout `250` and tries `1`. Destroy-before-finalizer writes a query, then immediate `odin_dns_query_destroy(query)` before the loop yields no callback, zero liveness, and ASan-clean teardown | G1, G2, G3 | unit |
| T5 | Name-not-found and no-data answers map to address-unreachable error | Subcases pass sentinel `user_data`, save the returned query handle, and use a local DNS fixture that responds to `missing.test` with RCODE 3 and no answers or to `nodata.test` with NOERROR and no A/AAAA answer for the requested family | Each subcase fires one callback with callback `query` equal to the saved start-returned handle, callback `user_data` equal to the sentinel pointer, `ODIN_DNS_ERROR`, `err == EHOSTUNREACH`, `addrs == NULL`, and `addr_count == 0`; query and resolver destroy leave liveness zero | G2 | unit |
| T6 | DNS timeout maps to `ETIMEDOUT` and peer names are not rewritten | Local UDP fixture records questions but sends no response; normal timeout starts `timeout.test`; alias case creates a temp aliases file mapping single-label `aliaspeer` to `rewritten.test`, sets `HOSTALIASES` in the forked row child, starts `aliaspeer`, and restores the environment; resolver config uses `timeout_ms = 250` and `tries = 1`; each start passes sentinel `user_data`, saves the returned query handle, and runs with a watchdog longer than c-ares timeout | `cares_observation` reports the `ares_init_options` optmask contained `ARES_OPT_FLAGS`, `ARES_OPT_LOOKUPS`, `ARES_OPT_TIMEOUTMS`, and `ARES_OPT_TRIES`, did not contain `ARES_OPT_EVENT_THREAD`, recorded both `last_init_options_flags & ARES_FLAG_NOALIASES` and `last_init_options_flags & ARES_FLAG_NOSEARCH`, copied `last_init_options_lookups == "b"`, and recorded timeout `250` and tries `1`; each callback fires once before the row watchdog with callback `query` equal to the saved handle, callback `user_data` equal to the sentinel pointer, `ODIN_DNS_ERROR`, `err == ETIMEDOUT`, `addrs == NULL`, and `addr_count == 0`; alias case records at least one DNS question and every recorded alias-case question name equals `aliaspeer`, proving neither `HOSTALIASES`, local search domains, nor hosts-file lookup rewrote or bypassed the accepted single-label peer name; no live watch or timer remains after cleanup | G2, S3 | unit |
| T7 | Query host-slice boundary validation rejects unsafe names and accepts the high valid length | Subcases: `name == NULL` with `name_len = 1`, empty name, 255-byte syntactically valid DNS name, 256-byte name, and byte sequence `a b NUL c` with `name_len = 4`; the 255-byte subcase starts against a no-response fixture and immediately destroys the returned query; after each rejected start, run a short watchdog-only loop | NULL, empty, 256-byte, and embedded-NUL calls return `-1/EINVAL`; sentinel `*out` is unchanged; the watchdog observes `on_done` was never called and liveness stays zero. The 255-byte allow-side call returns `0`, writes a query handle, can be destroyed before completion, and leaves liveness zero | G2, S1 | unit |
| T8 | Query API preconditions reject unsupported family, missing callback/output, and invalid DNS server config | Subcases: unsupported `family = 255`, `on_done == NULL`, `out == NULL`, and resolver created with `servers_csv = "not a server"` before a valid-looking query so real c-ares rejects the invalid-string server CSV path; after each failed start with an output slot, verify the sentinel | Unsupported family returns `-1/EAFNOSUPPORT`; null callback, null output, and invalid `servers_csv` return `-1/EINVAL`; provided sentinel `*out` is unchanged; no callback fires and liveness stays zero | G2 | unit |
| T9 | Destroy pending query aborts exactly that query while a sibling survives | Local UDP fixture records but initially does not answer `abort.test` or `sibling.test`; start both queries on the same resolver, assert liveness shows two live queries and two c-ares channels with at least one active watch or timeout timer, then an Odin timer calls `odin_dns_query_destroy(abort_query)` before c-ares timeout and releases the fixture to answer `sibling.test` with A `203.0.113.11`; a later watchdog stops the loop | Both starts return `0`; no callback fires for `abort_query`; exactly one sibling callback fires with `ODIN_DNS_OK`, address `203.0.113.11`, and `addrlen == sizeof(struct sockaddr_in)`; the pre-abort liveness assertion proves two real pending c-ares-driven queries existed; post-run cleanup shows zero queries, watches, timers, `cares_channels`, and `cares_results`; `cares_observation` shows `ares_destroy` ran for the aborted channel without canceling the sibling channel; ASan reports no use-after-free in the green phase | G1, G2, G3 | unit |
| T10 | Destroy query from inside success callback is safe | Local DNS fixture answers `callback-destroy.test` with A `203.0.113.9`; start passes a sentinel `user_data` pointer and saves the returned query handle; callback copies the single address into row state, calls `odin_dns_query_destroy(query)`, then stops the loop | Callback fires once with `ODIN_DNS_OK`, the expected address, callback `query` equal to the saved start-returned handle, and callback `user_data` equal to the sentinel pointer; destroying inside the callback does not invalidate the copied row state, double-free, or touch query fields after callback return; resolver destroy leaves liveness zero under ASan | G3 | unit |
| T11 | Concurrent queries complete independently | Same resolver starts `ok.test` and `no.test`; DNS fixture answers A `203.0.113.10` for `ok.test` and NXDOMAIN for `no.test`; callbacks stop the loop only after both complete | Exactly two callbacks fire, one `ODIN_DNS_OK` with the A result and one `ODIN_DNS_ERROR/EHOSTUNREACH` with no addresses; completion order is not asserted; destroying one completed query does not alter the sibling result; final liveness is zero | G1, G2, G3 | unit |
| T12 | Resolver destroy aborts live, deferred-finalizer, and completed-undestroyed queries | Subcases: same resolver starts two no-response queries then an Odin timer calls `odin_dns_resolver_destroy(resolver)` before c-ares timeout; numeric literal `"192.0.2.12"` is started and the resolver is destroyed before the 0-delay finalizer fires; completed A query fires its callback without calling `odin_dns_query_destroy`, then the row destroys the resolver | No user callback fires for the abort subcases; returned aborted handles are treated as dead and are not destroyed separately. The completed subcase fires one `ODIN_DNS_OK` callback, leaves the completed query handle undestroyed by the caller, then resolver destroy reclaims it. Post-run liveness reports zero resolvers, queries, watches, timers, `cares_channels`, and `cares_results`; `cares_observation` shows each created channel was destroyed; ASan reports no leak or use-after-free | G2, G3 | unit |
| T13 | Pure Odin error readiness is delivered to c-ares as read readiness | Start a no-response query until `odin_dns_resolver_test_first_watch` returns its active watch and fd; push `ODIN_DNS_TEST_CARES_RESULT_STATUS` with `status = ARES_ECONNREFUSED` and `expect_process_events = ARES_FD_EVENT_READ`; inject `{watch, ODIN_EVENT_ERROR}` with `odin_event_loop_test_dispatch_backend_events`; run with the row watchdog | The wrapper observes `ARES_FD_EVENT_READ`, not `ARES_FD_EVENT_NONE`; callback fires once with `ODIN_DNS_ERROR`, `err == ECONNREFUSED`, `addrs == NULL`, and `addr_count == 0`; all watches, timers, query, and resolver state are gone after cleanup | G2, G3 | unit |
| T14 | Driver socket-state reconciliation and fatal driver cleanup are observable | Subcases: (1) active READ watch changes to 0; (2) active READ watch changes to WRITE-only with `SOCK_STATE(readable=0,writable=1)`, saves watch/fd, scripts `ARES_ECONNREFUSED` with `expect_process_events = ARES_FD_EVENT_WRITE`, then dispatches `{watch, ODIN_EVENT_WRITE}`; the same subcase separately changes another active READ watch to READ\|WRITE for the combined-mask assertion; (3) next `ares_process_fds` wrapper returns `ARES_ENOMEM`; (4) Apple READ watch add fails `EIO`; (5) Apple READ-to-READ\|WRITE add fails `EIO`; (6) DNS timeout timer start fails after watchdog is armed then `odin_event_loop_test_fail_next_timer_start(loop, ENOMEM)` then query start; (7) raw `ares_timeout` refresh overflows with `tv_sec = INT64_MAX` plus `tv_usec = 999999`; (8) numeric finalizer-timer start fails after watchdog is armed then the timer-start fail hook then numeric start | Benign removal reports no active watch, no fatal callback, then explicit query destroy leaves zero liveness. The write-only update observes a WRITE-only registered mask with `odin_event_loop_test_kqueue_registered_mask(loop, saved_fd, &mask)` on Apple, dispatches `ODIN_EVENT_WRITE` on the macOS host run, and the c-ares wrapper observes `ARES_FD_EVENT_WRITE` before cleanup; the combined update separately observes READ\|WRITE registration on Apple. Guarded Apple failure subcases skip elsewhere. Fatal subcases finalize only after c-ares returns with `ODIN_DNS_ERROR`, no addresses, mapped status or saved `errno`, zero live watches/timers/c-ares channels/results, and observed `ares_destroy`; the two timer-start failure subcases additionally prove the pre-armed watchdog did not consume the one-shot hook, and numeric finalizer failure returns `-1/ENOMEM` with sentinel `*out` unchanged and no callback | G2, G3 | unit |
| T15 | c-ares cancellation/destruction and Odin result-copy allocation failure map to stable outcomes | Subcases: inject `ARES_ECANCELLED` from start-path `ares_getaddrinfo`; inject `ARES_EDESTRUCTION` from start-path `ares_getaddrinfo`; inject `ARES_ECANCELLED` from post-start `ares_process_fds`; inject `ARES_EDESTRUCTION` from post-start `ares_process_fds`; arm `odin_dns_resolver_test_fail_next_result_alloc()` before copying an `ARES_SUCCESS` A answer | Start-path `ARES_ECANCELLED` and `ARES_EDESTRUCTION` each return `-1/ECANCELED`, leave sentinel `*out` unchanged, invoke no callback, observe channel cleanup, and leave zero query/channel/result liveness. Post-start `ARES_ECANCELLED` and `ARES_EDESTRUCTION` each fire one `ODIN_DNS_ERROR/ECANCELED` callback with no addresses and leave the completed handle destroyable. The copy-failure subcase fires one `ODIN_DNS_ERROR/ENOMEM` callback with `addrs == NULL` and `addr_count == 0`, observes `ares_freeaddrinfo` for the successful c-ares result, allows query teardown, and leaves zero query/channel/result liveness | G2 | unit |
| T16 | c-ares setup failures are synchronous and leave no query | Push setup-failure steps: the row's library-init subcase fork+execs the current `odin_unittests` binary as `--gtest_filter=OdinDnsResolverExecChild.T16LibraryInit` with `ODIN_DNS_EXEC_CHILD=T16_LIBRARY_INIT`; that child-only test validates the mode and active filter, runs before any resolver create in the exec'd process, scripts `ODIN_DNS_TEST_CARES_LIBRARY_INIT` to return `ARES_ENOMEM` during resolver create, and then attempts a second resolver create without launching another child. The same exact child-only filter without `ODIN_DNS_EXEC_CHILD` is a pre-create assertion failure, not a skip. In the ordinary fork/deadline row child, never the gtest parent, `ODIN_DNS_TEST_CARES_INIT_OPTIONS` returns `ARES_ENOMEM` or named default `ARES_EBADRESP` during query start, and `ODIN_DNS_TEST_CARES_SET_SERVERS` returns `ARES_ENOMEM`, `ARES_EFORMERR`, `ARES_EBADSTR`, `ARES_ESERVICE`, or named default `ARES_EBADRESP` while applying a non-null copied `servers_csv`; preset sentinel output pointers | Library-init failure makes both creates return `-1/ENOMEM` in the exec'd child, leaves both sentinel `*out` slots unchanged, and `cares_observation.ares_library_init_calls == 1`, proving cached failure rather than retry or racing state; the parent accepts a successful child exit only from the exact filter plus exact `ODIN_DNS_EXEC_CHILD` mode, while an omitted-mode direct child run exits nonzero before resolver creation. Ordinary setup-failure subcases run after child-local resolver creation and never depend on a parent guard state. Init-options `ARES_ENOMEM` returns `-1/ENOMEM`; init-options `ARES_EBADRESP` returns `-1/EIO`; set-servers `ARES_ENOMEM` returns `-1/ENOMEM`; set-servers `ARES_EFORMERR`, `ARES_EBADSTR`, and `ARES_ESERVICE` return `-1/EINVAL`; set-servers `ARES_EBADRESP` returns `-1/EIO`. Each failure leaves the relevant sentinel `*out` unchanged, invokes no callback, destroys any partial channel outside c-ares callbacks with `ares_destroy` observed when a channel was created, and leaves zero resolver/query/watch/timer/channel/result liveness | G2 | unit |
| T17 | Active DNS timeout timer refresh resets, fails, stops, and reports timeout-callback driver failure | Subcases start a no-response query after `ODIN_DNS_TEST_CARES_TIMEOUT_TIMEVAL` creates an active 250 ms DNS timer. Reset-success sets fake owner-thread time to `1000000` before start, then pushes a 500 ms `TIMEOUT_TIMEVAL` plus `SOCK_STATE(readable=1,writable=0)`, dispatches the active READ watch, and reads `odin_event_loop_test_prepare_wait`. Reset-failure starts with fake time `1000000`, then sets fake time to `UINT64_MAX - 100`, pushes a 1 s `TIMEOUT_TIMEVAL` plus `SOCK_STATE`, and dispatches READ. Null-stop pushes `ODIN_DNS_TEST_CARES_TIMEOUT_NULL` plus `SOCK_STATE`, dispatches READ, then runs past the original 250 ms deadline using the row watchdog. Process-fds-failure pushes `ODIN_DNS_TEST_CARES_PROCESS_FDS_STATUS` with `status = ARES_ENOMEM` and `expect_process_null_events = 1`, then lets the active DNS timeout timer fire | Reset success keeps exactly one DNS timer, invokes no callback, and `prepare_wait` reports the 500 ms reset deadline rather than the original 250 ms deadline; explicit destroy leaves zero liveness. Reset failure fires one `ODIN_DNS_ERROR/EOVERFLOW` callback only after `ares_process_fds` returns and leaves zero query/watch/timer/channel/result liveness. Null-stop reports `timers == 0` while the query remains pending, fires no timeout callback after the original deadline, and explicit destroy leaves zero liveness. Process-fds failure fires one `ODIN_DNS_ERROR/ENOMEM` callback after the timeout callback's `ares_process_fds(channel, NULL, 0, ARES_PROCESS_FLAG_NONE)` wrapper returns non-success, with no addresses and zero query/watch/timer/channel/result liveness | G2, G3 | unit |
| T18 | Concurrent resolver creates after single-threaded c-ares pre-init reuse cached success | The row fork+execs the current `odin_unittests` binary as `--gtest_filter=OdinDnsResolverExecChild.T18CachedSuccessConcurrency` with `ODIN_DNS_EXEC_CHILD=T18_CACHED_SUCCESS_CONCURRENCY`; that child-only test validates the mode and active filter before any resolver create or helper-thread start in the exec'd process. The same exact child-only filter without `ODIN_DNS_EXEC_CHILD` is a pre-create assertion failure, not a skip. Inside the child-only test, the main thread creates an `odin_event_loop_t`, calls `odin_dns_resolver_create(loop, NULL, &preinit_resolver)`, asserts `cares_observation.ares_library_init_calls == 1`, destroys the resolver and loop, and only then starts two pthreads. Each thread creates its own `odin_event_loop_t`, waits at a barrier, calls `odin_dns_resolver_create(loop, NULL, &resolver)` on that loop's owner thread, waits at a second barrier, then destroys its resolver and loop on the same thread | The main-thread pre-init create returns `0`, records exactly one `ares_library_init` wrapper call, and leaves zero resolver/query/watch/timer/channel/result liveness after destroy. Both threaded creates return `0`; both resolver handles are non-null until their owner thread destroys them; `cares_observation.ares_library_init_calls` remains `1`, proving concurrent cached-success creates did not invoke c-ares initialization from already-running owner threads; the parent accepts a successful child exit only from the exact filter plus exact `ODIN_DNS_EXEC_CHILD` mode, while an omitted-mode direct child run exits nonzero before resolver creation. The ordinary host run ends with zero resolver/query/watch/timer/channel/result liveness. A guard that calls c-ares from either concurrent owner thread, calls c-ares twice, re-enters the `OdinDnsResolverTest.T18` launcher, publishes inconsistent cached state, or lets a direct child-only run without mode skip successfully fails the call-count, return-code, sentinel, child-exit, or liveness observation | G3 | unit |
| T19 | c-ares connection, allocation-status, and unsupported-status mappings are individually pinned | Subcases: push `ODIN_DNS_TEST_CARES_RESULT_STATUS` then start a valid query for `ARES_ECONNREFUSED -> ECONNREFUSED`; repeat for `ARES_EREFUSED -> ECONNREFUSED`; repeat for `ARES_ENOSERVER -> ECONNREFUSED`; repeat for `ARES_ENOMEM -> ENOMEM`; repeat for `ARES_ENOTIMP -> EAFNOSUPPORT` | Each subcase defers completion until after `odin_dns_resolve_start` returns, then fires one `ODIN_DNS_ERROR` callback with no addresses and the exact errno named for that individual c-ares status. A mapper that only tests one representative per errno group, maps `ARES_EREFUSED` differently from `ARES_ECONNREFUSED`, treats `ARES_ENOMEM` as `EIO`, or reports `ARES_ENOTIMP` as `EINVAL` fails this row. Query and resolver teardown leave zero query/channel/result liveness with the channel destruction observed | G2 | unit |
| T20 | c-ares EINVAL-class and named default-result mappings are individually pinned | Subcases: push `ODIN_DNS_TEST_CARES_RESULT_STATUS` then start a valid query for `ARES_ESERVICE -> EINVAL`; repeat for `ARES_EFORMERR -> EINVAL`; repeat for `ARES_EBADSTR -> EINVAL`; repeat for named default representative `ARES_EBADRESP -> EIO` | Each subcase defers completion until after `odin_dns_resolve_start` returns, then fires one `ODIN_DNS_ERROR` callback with no addresses and the exact errno named for that individual c-ares status. A mapper that tests only `ARES_EFORMERR`, forgets `ARES_EBADSTR`, or promotes the default `ARES_EBADRESP` case into an `EINVAL` status fails this row. Query and resolver teardown leave zero query/channel/result liveness with the channel destruction observed | G2 | unit |

## 6. Implementation Plan

- **P1. Land resolver surfaces and red-verifiable DNS tests behind a default skip.**
  - **Scope:** add `odin/dns_resolver.h` with §3.2.1 declarations and comments; add `odin/dns_resolver.c` skeleton whose `resolver_create` allocates a resolver without validating negative config, whose `resolve_start` first returns `-1/EINVAL` when `out == NULL` without dereferencing or allocating, then otherwise allocates an unlinked query but starts no c-ares channel, no I/O watch, and no timer, whose `odin_dns_query_destroy` frees that standalone query, and whose `odin_dns_resolver_destroy` frees only the resolver. Under `ODIN_DNS_RESOLVER_TESTING`, the skeleton must define every §3.2.3 `odin_dns_resolver_test_*` hook so T1-T20 link: `reset_liveness` returns `0`; `liveness` and `cares_observation` return `0` and zero-fill only when `out != NULL`, so their NULL-contract assertions fail red without crashing; `push_cares_step` returns `0` while dropping the step; `fail_next_result_alloc` returns `0` without arming a failpoint; and `first_watch` returns `-1/ENOENT` when no active watch exists.
  - **Build/test wiring:** in `odin/testing/BUILD.gn`, add `config("odin_dns_resolver_testing_config") { defines = [ "ODIN_DNS_RESOLVER_TESTING" ] }` and `source_set("odin_dns_resolver_testing") { testonly = true; sources = [ "../dns_resolver.c", "../dns_resolver.h", "dns_resolver_internal_test.h", "dns_resolver_unittests.cpp" ]; deps = [ ":odin_event_loop_testing", "//c-ares:cares", "//googletest:gtest" ]; configs += [ ":odin_dns_resolver_testing_config", ":odin_event_loop_testing_config", "//build:cxx17" ] }`; add that source set to `odin_unittests` deps and do not list `dns_resolver_unittests.cpp` directly in the executable sources. Add `odin/testing/dns_resolver_internal_test.h` with the liveness, c-ares observation, c-ares-step, result-allocation, and first-watch handle/fd hooks from §3.2.3; add `dns_resolver_unittests.cpp` inside the source set containing T1-T20, the local DNS fixture, a fork/deadline runner that keeps all resolver create/start calls out of the gtest parent, fork+exec runners that target only `OdinDnsResolverExecChild.T16LibraryInit` with `ODIN_DNS_EXEC_CHILD=T16_LIBRARY_INIT` and `OdinDnsResolverExecChild.T18CachedSuccessConcurrency` with `ODIN_DNS_EXEC_CHILD=T18_CACHED_SUCCESS_CONCURRENCY`, child-only tests that perform those assertions directly, do not call the fork+exec runners, fail direct child-only selection without exact `ODIN_DNS_EXEC_CHILD`, and skip only ordinary unfiltered full-suite entry when `ODIN_DNS_EXEC_CHILD` is absent, a parent-process isolation assertion that `cares_observation.ares_library_init_calls == 0` before and after the gated DNS rows, and an `ODIN_DNS_RED=1` red-verification gate while the default run skips these rows. Do not add a production `//odin:odin_dns_resolver` source set until P2.
  - **Depends on:** None.
  - **Done when:** Host-runnable: `./tool/gn gen out/dns_mac --args='target_os="mac"'` and `./tool/ninja -C out/dns_mac odin_unittests tests` build `out/dns_mac/odin_unittests` and `out/dns_mac/odin`; `ODIN_DNS_RED=1 out/dns_mac/odin_unittests --gtest_filter='OdinDnsResolverTest.*'` executes T1-T20 in child processes while the gtest parent observes `cares_observation.ares_library_init_calls == 0`, with the T16 and T18 rows spawning only their named `OdinDnsResolverExecChild.*` tests using the matching `ODIN_DNS_EXEC_CHILD` modes, and reports the rows failing against the skeleton: T1 because invalid configs and NULL liveness/observation contracts are not handled, T2-T6/T10-T11 because no c-ares query completes or records exact `addrlen`, port-boundary values, `ares_init_options` flags/lookups/optmask, default/zero omission of timeout/tries options, `HOSTALIASES`/search/hosts policy suppression, `ares_getaddrinfo`, callback identity, or cleanup observations, T7-T8 because invalid query inputs other than the safe `out == NULL` short-circuit are accepted or not fully handled, T9 because no live sibling c-ares queries, pending-destroy isolation, or surviving sibling completion exists, T12 because completed handles are not tracked for resolver teardown, T13 because no active watch/error-readiness translation exists, T14 because no socket-state reconciliation, write-only socket-state registration/dispatch, host-run WRITE dispatch, or ordered fatal driver/finalizer failure path exists, T15 because no cancellation/destruction or result-copy failpoint path exists, T16 because no c-ares setup wrapper, cached init failure path, `ARES_ESERVICE` setup mapping, set-servers `ARES_ENOMEM` allocation mapping, or setup-default `ARES_EBADRESP` mapping exists even though the child-local setup-failure runner executes, T17 because the skeleton has no active c-ares timeout timer to reset, fail, stop, or enter fatal cleanup from timeout-callback `ares_process_fds(NULL, 0)`, T18 because the skeleton does not call or count process-wide c-ares init, and T19-T20 because no per-status result mapping wrapper path exists. T8's `out == NULL` subcase returns `-1/EINVAL` and does not dereference the pointer, so the row's red evidence comes from ordinary failing assertions in its other subcases rather than a test-process crash. The default `out/dns_mac/odin_unittests --gtest_brief=1` reports T1-T20 skipped, child-only exec tests skip only as ordinary unfiltered full-suite entries when `ODIN_DNS_EXEC_CHILD` is absent, and exits zero with pre-existing suites green; the direct commands `out/dns_mac/odin_unittests --gtest_filter=OdinDnsResolverExecChild.T16LibraryInit` and `out/dns_mac/odin_unittests --gtest_filter=OdinDnsResolverExecChild.T18CachedSuccessConcurrency` with no `ODIN_DNS_EXEC_CHILD` exit nonzero before resolver creation. Cross-compile-only: `./tool/gn gen out/dns_linux_x64 --args='target_os="linux" target_cpu="x64"'` plus `./tool/ninja -C out/dns_linux_x64 odin_unittests odin_cli_artifacts` builds but does not run `out/dns_linux_x64/odin_unittests` and `out/dns_linux_x64/odin`; if that binary is run manually, T14 omits only its `#if defined(__APPLE__)` kqueue subcases and still runs non-kqueue subcases. `./tool/gn gen out/dns_mac_arm64 --args='target_os="mac" target_cpu="arm64"'` plus `./tool/ninja -C out/dns_mac_arm64 odin_unittests odin_cli_artifacts` builds but does not run `out/dns_mac_arm64/odin_unittests` and `out/dns_mac_arm64/odin`; `./tool/gn gen out/dns_ios_sim_arm64 --args='target_os="ios" target_environment="simulator" target_cpu="arm64"'` plus `./tool/ninja -C out/dns_ios_sim_arm64 odin_unittests odin_cli_artifacts` builds but does not run `out/dns_ios_sim_arm64/odin_unittests` and `out/dns_ios_sim_arm64/odin`; `./tool/gn gen out/dns_ios_device_arm64 --args='target_os="ios" target_environment="device" target_cpu="arm64"'` plus `./tool/ninja -C out/dns_ios_device_arm64 odin_unittests odin_cli_artifacts` builds but does not run `out/dns_ios_device_arm64/odin_unittests` and `out/dns_ios_device_arm64/odin`. T1-T20 execute only in `out/dns_mac/odin_unittests`; alternate backends are compile-only in this RFC.

- **P2. Implement c-ares integration and turn T1-T20 green.**
  - **Scope:** implement §3.2.1 config validation, name copying, caller-contracted single-threaded c-ares one-time init with cached success/failure, `ares_init_options`, and synchronous setup rollback; apply `servers_csv`, `ARES_OPT_FLAGS` with `ARES_FLAG_NOALIASES | ARES_FLAG_NOSEARCH`, `ARES_OPT_LOOKUPS` with `lookups = "b"`, `ARES_OPT_SOCK_STATE_CB`, `ARES_OPT_TIMEOUTMS` only when `timeout_ms > 0`, and `ARES_OPT_TRIES` only when `tries > 0`, while omitting `ARES_OPT_EVENT_THREAD`; implement §3.2.2 record-only c-ares callbacks, post-c-ares fd-watch reconciliation for watch start, update, removal, and write-only desired masks, `ODIN_EVENT_ERROR` to `ARES_FD_EVENT_READ` and `ODIN_EVENT_WRITE` to `ARES_FD_EVENT_WRITE` translation, `ares_process_fds`, timeout rescheduling including active `odin_event_timer_reset` success/failure, raw `struct timeval` conversion overflow, and the null `ares_timeout` timer-stop branch, and the fatal internal-error path for c-ares/Odin driver failures from both I/O readiness and timeout callbacks; implement §3.2.3 result-pointer recording, exact IPv4/IPv6 `addrlen` publication, outer-frame result copying, result-copy allocation failpoint, status mapping including pre-publish `ARES_ECANCELLED`/`ARES_EDESTRUCTION` rollback and post-publish `ODIN_DNS_ERROR/ECANCELED`, deferred start-path finalizer timer, pending/completed query ownership, resolver destroy semantics, test-only `ares_library_init` / `ares_init_options` / `ares_getaddrinfo` / `ares_destroy` / `ares_freeaddrinfo` observations, first-watch handle/fd exposure, mutex-protected live c-ares channel/result counters, and production omission of test hooks. In `odin/BUILD.gn`, add `source_set("odin_dns_resolver") { sources = [ "dns_resolver.c", "dns_resolver.h" ]; public_deps = [ ":odin_event_loop", "//c-ares:cares" ] }` and append `":odin_dns_resolver"` to production `source_set("odin")`; keep `odin_unittests` linked through `//odin/testing:odin_dns_resolver_testing` so the test gate and c-ares wrapper hooks remain target-local. Remove `ODIN_DNS_RED` skips so T1-T20 assert in the default host test run.
  - **Depends on:** P1.
  - **Done when:** Host-runnable: the P1 macOS build commands still succeed; `out/dns_mac/odin_unittests --gtest_filter='OdinDnsResolverTest.*'` runs T1-T20 un-gated in child processes while the gtest parent observes `cares_observation.ares_library_init_calls == 0`, and passes, including T1's liveness/observation NULL contracts, T2's recorded `ARES_AI_NOSORT`/`AF_INET` `ares_getaddrinfo` hints plus exact IPv4 `addrlen`, `sin_port == htons(0)`, and `ares_freeaddrinfo`, T3's `AF_UNSPEC`/`AF_INET`/`AF_INET6` family filtering with exact IPv4/IPv6 `addrlen` and port `65535` preserved as `htons(65535)`, T4's immediate-result deferral with exact numeric-literal `addrlen`, port `1` preserved as `htons(1)`, default and zero config preserving `ARES_OPT_FLAGS` with `ARES_FLAG_NOALIASES | ARES_FLAG_NOSEARCH`, `lookups == "b"`, no `ARES_OPT_EVENT_THREAD`, and no `ARES_OPT_TIMEOUTMS` or `ARES_OPT_TRIES`, nonzero null-server config preserving timeout/tries init options, and query destroy before the 0-delay finalizer, T5's DNS-error callback query/user_data identity, T6's DNS-only `ARES_OPT_FLAGS` with `ARES_FLAG_NOALIASES | ARES_FLAG_NOSEARCH`, `ARES_OPT_LOOKUPS`/`lookups == "b"`, no `ARES_OPT_EVENT_THREAD`, timeout/tries init options, exact single-label DNS question `aliaspeer` under `HOSTALIASES`, local alias/search/hosts policy suppression, and timeout-error callback identity, T7's NULL/255/256/embedded-NUL boundary checks, T8's real invalid server CSV rejection, T9's pending-query destroy while an active sibling completes, T10's destroy-inside-callback path with callback query/user_data identity, T11's sibling-query isolation, T12's live/deferred/completed resolver-destroy cleanup, T13's pure-error readiness translation, T14's benign socket-state removal/update including WRITE-only and READ|WRITE existing-watch updates, Apple `kqueue_registered_mask` assertions with fds returned by `first_watch`, macOS-host `ODIN_EVENT_WRITE` dispatch observed as `ARES_FD_EVENT_WRITE`, raw-timeval `EOVERFLOW`, fatal post-call cleanup, guarded macOS kqueue subcases, and watchdog-first timer-start failure branches for DNS timeout and numeric finalizer timers, T15's start-path `ARES_ECANCELLED`/`ARES_EDESTRUCTION` synchronous `-1/ECANCELED` rollback, post-start `ARES_ECANCELLED`/`ARES_EDESTRUCTION` error callbacks, and result-copy allocation failure, T16's `OdinDnsResolverExecChild.T16LibraryInit` fork+exec cached library-init failure plus child-local synchronous setup rollback for init-options `ARES_ENOMEM`, set-servers `ARES_ENOMEM`, set-servers `ARES_EFORMERR`, set-servers `ARES_EBADSTR`, set-servers `ARES_ESERVICE`, and setup `ARES_EBADRESP` with partial-channel cleanup observed, T17's active timeout timer reset success, reset-failure cleanup, null-stop branch, and timeout-callback `ares_process_fds(channel, NULL, 0, ARES_PROCESS_FLAG_NONE)` fatal `ARES_ENOMEM` cleanup, T18's `OdinDnsResolverExecChild.T18CachedSuccessConcurrency` single-threaded pre-init followed by concurrent owner-thread creates with `ares_library_init` wrapper calls remaining at exactly one and mutex-protected test observations, T19's per-status c-ares mapping for `ARES_ECONNREFUSED`, `ARES_EREFUSED`, `ARES_ENOSERVER`, `ARES_ENOMEM`, and `ARES_ENOTIMP`, and T20's per-status c-ares mapping for `ARES_ESERVICE`, `ARES_EFORMERR`, `ARES_EBADSTR`, and `ARES_EBADRESP`; `out/dns_mac/odin_unittests --gtest_brief=1` passes the full local suite while child-only exec tests skip only as ordinary unfiltered full-suite entries without `ODIN_DNS_EXEC_CHILD`; the direct commands `out/dns_mac/odin_unittests --gtest_filter=OdinDnsResolverExecChild.T16LibraryInit` and `out/dns_mac/odin_unittests --gtest_filter=OdinDnsResolverExecChild.T18CachedSuccessConcurrency` with no `ODIN_DNS_EXEC_CHILD` exit nonzero before resolver creation; `./tool/gn gen out/dns_mac_asan --args='target_os="mac" is_asan=true'`, `./tool/ninja -C out/dns_mac_asan odin_unittests`, and `out/dns_mac_asan/odin_unittests --gtest_filter='OdinDnsResolverTest.*'` pass without AddressSanitizer reports; production `out/dns_mac/odin` links the resolver through `:odin` and exports no `odin_dns_resolver_test_*` symbols. Cross-compile-only: the P1 Linux, alternate macOS arch, iOS simulator, and iOS device commands still build the same `odin` and `odin_unittests` artifact paths but are not executed; their c-ares platform configuration, Linux socket/timer integration through Odin's epoll backend, and Darwin/iOS c-ares sysconfig branches are compile-verified only.
