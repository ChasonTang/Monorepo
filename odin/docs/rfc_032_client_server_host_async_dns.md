# RFC-032: Client `--server` Async DNS Resolution

## 1. Summary

Resolve the Client-mode `--server` host through `odin_dns_resolve_start` on the Odin event loop before creating the QUIC client runtime, so DNS names, IPv4 literals, and IPv6 literals produce a resolved server sockaddr without blocking in `inet_pton` or `getaddrinfo`.

## 2. Goals

- **G1.** For every Client-mode `--server` host that resolves to at least one IPv4 or IPv6 address for the requested port, the client startup path creates and starts the QUIC runtime with a resolved `peer_addr`, preserves the original host string for TLS/SNI and the banner, and continues to accept local proxy connections after the success banner.
- **G2.** Host-slice validation failures, resolver creation failures, resolver-start failures, DNS result failures, DNS OK with zero usable addresses, DNS-phase event-loop failures, DNS-phase zero-return without completion, and post-DNS runtime startup failures return client exit code `1`, print one deterministic startup-failure line, print no success banner, and leave no live CLI listener, event loop, DNS query, resolver, or QUIC runtime state.
- **G3.** A resolved IPv4 server address uses an IPv4 wildcard UDP local bind and a resolved IPv6 server address uses an IPv6 wildcard UDP local bind, with the parsed `--server` port installed on the selected peer sockaddr in both cases.

## 3. Design

### 3.1 Overview

`odin/cli.c` continues to parse `--server` into the existing `odin_cli_client_config_t` fields. `odin/cli_client` stops treating `config->server_host` as an IPv4-only presentation literal at startup. It validates and copies the host slice, creates the listener and event loop as it does today, creates one temporary Odin DNS resolver on that loop, starts one asynchronous DNS query with `AF_UNSPEC`, runs the loop until that query completes, destroys the DNS objects, then creates the QUIC client runtime from the selected resolved address.

The QUIC runtime remains unchanged: it already accepts `AF_INET` and `AF_INET6` peer addresses in `odin/client_xqc_runtime.c:997-1018` and consumes `peer_addr`, `peer_addrlen`, and `server_host` through the default config in `odin/client_xqc_runtime.h:38-46`. The client runner supplies the resolved sockaddr as `peer_addr`, a family-matching wildcard UDP sockaddr as `local_addr`, and the original copied host string as `server_host`.

```text
parsed --server host slice + port
    |
    v
cli_client copy/validate host slice
    |
    v
odin_dns_resolve_start(..., AF_UNSPEC)
    |
    v
DNS callback copies first usable sockaddr and stops startup loop
    |
    v
family-matching local UDP sockaddr + resolved peer sockaddr
    |
    v
odin_xqc_client_runtime_create_default -> start -> banner -> accept loop
```

### 3.2 Detailed Design

#### 3.2.1 Client Runner DNS Startup Contract

Current client config is `odin_cli_client_config_t` at `odin/cli_client.h:19-26`; no public field is added. The current IPv4-only helper is `copy_ipv4_server_endpoint` at `odin/cli_client.c:106-118`, and the current runtime config is assembled from that IPv4 result at `odin/cli_client.c:664-685`. This RFC replaces only that endpoint-resolution part of the runner.

```text
Client startup failure lines added or retained by this RFC:
  odin: client startup failed at server_endpoint
  odin: client startup failed at server_dns
  odin: client startup failed at xqc_client_runtime_create
  odin: client startup failed at xqc_client_runtime_start

Client success banner:
  odin: mode=client transport=quic listen=<actual_port> server=<display_host>:<server_port>
```

**Unstated contract.** `config->server_host[0..server_host_len)` remains the exact host slice produced by the parser; for bracketed IPv6 input, the slice excludes brackets as documented in `odin/cli.h:22-27`. The runner copies that slice into one NUL-terminated `server_host_cstr` before DNS and runtime creation. It rejects `server_host == NULL`, `server_host_len == 0`, `server_host_len > ODIN_PROTO_HOST_MAX`, and any embedded NUL with `server_endpoint` before opening a socket, creating an event loop, or creating a DNS resolver. This replaces IPv4-literal validation; DNS names and IPv6 literals are no longer rejected by the client runner.

The listener setup order through `getsockname` remains the current order at `odin/cli_client.c:555-634`. DNS starts after the listener is bound and after `odin_event_loop_create`, but before `odin_accept_loop_create`, before QUIC runtime creation, before signal handlers are installed, and before the success banner is printed. That means no local connection is accepted and no QUIC connection is started until DNS has completed successfully. DNS uses `odin_dns_resolver_create(loop, NULL, &resolver)` and `odin_dns_resolve_start(resolver, server_host_cstr, strlen(server_host_cstr), server_port, AF_UNSPEC, ...)`, using the existing resolver API at `odin/dns_resolver.h:42-50`. `config == NULL` keeps the resolver's existing system c-ares configuration and DNS-only lookup policy. All DNS APIs are called on the same event-loop owner thread that owns the later accept loop and QUIC runtime.

The DNS phase calls `odin_event_loop_run` before the runtime phase. `odin_event_loop_run` is already callable again after a prior run returns under RFC-010, so the same loop is reused for the later long-running client runtime. The DNS callback stops the startup loop with `odin_event_loop_stop`; `run_rc != 0` or `run_rc == 0` without a DNS completion is reported as `server_dns`. This integration has no new platform-specific backend branch, but its resolver and event-loop behavior ride the existing macOS kqueue, Linux epoll, and iOS cross-compiled code; this environment runtime-verifies only the macOS host backend and compile-verifies the alternate-platform paths.

**Mechanism.**

```text
copy_server_host_slice(config, state):
  if config.server_host is NULL or len is 0 or len > ODIN_PROTO_HOST_MAX:
    errno = EINVAL; return -1
  if any byte in server_host[0..len) is NUL:
    errno = EINVAL; return -1
  copy bytes to state.server_host_cstr and append NUL
  state.server_display_needs_brackets = copied host contains ':'
  return 0

run_quic_client(config, err):
  initialize state handles to NULL or -1
  if copy_server_host_slice fails: startup_fail("server_endpoint")
  create listener through getsockname as today
  create event loop
  if resolve_server_endpoint(state) fails: startup_fail("server_dns")
  create accept loop
  create and start QUIC runtime using state.resolved_peer and state.local_udp
  install signals, arm signal timer, print banner, run event loop as today
```

Satisfies: G1 via accepting non-IPv4 host slices and deferring runtime creation until DNS supplies a sockaddr; G2 via the `server_endpoint` and `server_dns` validation and startup-failure contract.

#### 3.2.2 DNS Completion and Runtime Address Selection

The DNS completion consumes `odin_dns_cb` from `odin/dns_resolver.h:38-40` and the QUIC runtime default config from `odin/client_xqc_runtime.h:38-46`.

```text
Resolved endpoint state:
  struct sockaddr_storage peer_addr
  socklen_t peer_addrlen
  struct sockaddr_storage local_udp_addr
  socklen_t local_udp_addrlen
```

**Unstated contract.** DNS is requested with `AF_UNSPEC`. The selected result must be the first callback-scoped `odin_dns_addr_t` whose `addr.ss_family` is `AF_INET` with `addrlen == sizeof(struct sockaddr_in)` or `AF_INET6` with `addrlen == sizeof(struct sockaddr_in6)`. `ODIN_DNS_OK` with `addr_count == 0`, `ODIN_DNS_OK` with no supported address shapes, and every `ODIN_DNS_ERROR` completion fail startup at `server_dns`. The runner copies the selected sockaddr before the callback returns, then overwrites its port field with `config->server_port` for the selected family before passing it to the QUIC runtime. The overwrite is a defensive integration invariant: the resolver already receives the parsed port, but the runtime must observe the CLI-parsed port even when a test hook supplies a synthetic sockaddr with a placeholder port.

For `AF_INET`, the local UDP bind address is `0.0.0.0:0` with `local_addrlen == sizeof(struct sockaddr_in)`. For `AF_INET6`, the local UDP bind address is `[::]:0` with `local_addrlen == sizeof(struct sockaddr_in6)`. The `server_host` field passed to `odin_xqc_client_runtime_create_default` is always `state.server_host_cstr`, not a resolved numeric address. The success banner prints the original host with brackets re-added only when the copied host contains `:`, so `[::1]:4433` is printed as `server=[::1]:4433` while `odin.test:4433` and `127.0.0.1:4433` are unchanged. The banner host and runtime `server_host` are intentionally not DNS canonical names, aliases, or resolved addresses.

The DNS callback owns no permanent resolver result memory. It copies the chosen sockaddr, records `dns_done`, destroys the completed query from inside the callback, clears `state.dns_query` in the same branch, records the test-only callback-exit DNS liveness snapshot, and stops the startup loop. `odin_dns_query_destroy(query)` inside `on_done` is legal under RFC-030 and current resolver behavior. `resolve_server_endpoint` owns the resolver and query state fields: every non-NULL `odin_dns_query_destroy` or `odin_dns_resolver_destroy` performed by that helper is followed immediately by assigning the corresponding `state.dns_query` or `state.dns_resolver` field to `NULL`. That rule applies to resolver-start failure, DNS-loop failure, DNS-loop zero-return without completion, DNS result failure, and DNS success before returning to runtime setup. After the DNS loop returns, the runner destroys and clears the resolver before accept-loop or QUIC runtime creation; the runtime stores its own copies of the peer address and server host. Under `ODIN_CLI_CLIENT_TESTING`, the runner also records DNS liveness immediately before accept-loop creation and before QUIC runtime create and start calls so T9 can prove the pre-accept and pre-runtime cleanup cutpoints before `cleanup_all` runs.

**Mechanism.**

```text
dns_on_done(query, status, err, addrs, addr_count, state):
  state.dns_done = 1
  if status != ODIN_DNS_OK:
    odin_dns_query_destroy(query)
    state.dns_query = NULL
    record callback-exit DNS liveness for tests
    odin_event_loop_stop(state.loop)
    return
  for addr in addrs[0..addr_count):
    if addr is exact AF_INET sockaddr_in:
      copy addr to state.peer_addr; state.peer_addrlen = sizeof(sockaddr_in)
      set peer sin_port to htons(state.server_port)
      build state.local_udp_addr as AF_INET INADDR_ANY port 0
      state.dns_success = 1
      break
    if addr is exact AF_INET6 sockaddr_in6:
      copy addr to state.peer_addr; state.peer_addrlen = sizeof(sockaddr_in6)
      set peer sin6_port to htons(state.server_port)
      build state.local_udp_addr as AF_INET6 in6addr_any port 0
      state.dns_success = 1
      break
  odin_dns_query_destroy(query)
  state.dns_query = NULL
  record callback-exit DNS liveness for tests
  odin_event_loop_stop(state.loop)

resolve_server_endpoint(state):
  create resolver
  if resolver create failed: return -1
  start DNS query
  if start failed:
    if state.dns_query != NULL:
      destroy state.dns_query; state.dns_query = NULL
    destroy state.dns_resolver; state.dns_resolver = NULL
    return -1
  record DNS-pending pre-run timing
  if DNS-loop-stop test trigger is armed: post a same-loop task that stops state.loop
  if DNS-loop-fail test failpoint is armed: arm the next backend wait failure
  run_rc = run event loop until dns_on_done or a test-only stop task stops it
  record DNS post-run timing before DNS cleanup
  if state.dns_query != NULL:
    destroy state.dns_query; state.dns_query = NULL
  if state.dns_resolver != NULL:
    destroy state.dns_resolver; state.dns_resolver = NULL
  if run_rc != 0 or !state.dns_done or !state.dns_success: return -1
  return 0

runtime_config:
  loop = state.loop
  local_addr = &state.local_udp_addr
  local_addrlen = state.local_udp_addrlen
  peer_addr = &state.peer_addr
  peer_addrlen = state.peer_addrlen
  server_host = state.server_host_cstr
  ca_file = config.quic_ca_file
```

Satisfies: G1 via the callback-owned result copy and runtime config; G3 via family-specific peer and local UDP sockaddr construction.

#### 3.2.3 Cleanup, Build Wiring, and Test Hooks

```text
CLI-owned DNS cleanup state:
  odin_dns_resolver_t *dns_resolver
  odin_dns_query_t *dns_query
  int dns_done
  int dns_success

ODIN_CLI_CLIENT_TESTING observation surface added in odin/testing/cli_client_internal_test.h:
  odin_cli_client_test_dns_timing_t
  odin_cli_client_test_dns_timing(odin_cli_client_test_dns_timing_t *out)
  ODIN_CLI_CLIENT_TEST_FAIL_DNS_EVENT_LOOP_RUN
  ODIN_CLI_CLIENT_TEST_TRIGGER_DNS_EVENT_LOOP_STOP

ODIN_DNS_RESOLVER_TESTING additions in odin/testing/dns_resolver_internal_test.h:
  #define ODIN_DNS_TEST_CARES_RESULT_PENDING 10
  int odin_dns_resolver_test_push_addr_result(
      const odin_dns_addr_t *addrs, size_t addr_count)
```

**Unstated contract.** `cleanup_all` grows guarded DNS cleanup before event-loop destruction: if `dns_query != NULL`, destroy it and set `dns_query = NULL`; if `dns_resolver != NULL`, destroy it and set `dns_resolver = NULL`; then continue with accept loop, signal timer, QUIC runtime, event loop, wakeup fd, listener fd, and signal handlers. `resolve_server_endpoint` is still the primary owner for DNS-phase cleanup and returns with both DNS fields NULL on resolver-start failure, DNS event-loop failure, DNS zero-return without completion, DNS result failure, and DNS success. The guarded cleanup remains in the common path to cover future early exits before the helper can finish and to keep teardown idempotent for NULL fields. If QUIC runtime creation or start fails after DNS succeeds, DNS objects are already destroyed and cannot be force-destroyed through the QUIC cleanup path.

`odin/BUILD.gn:65-70` adds `:odin_dns_resolver` to `source_set("odin_cli_client")` public deps. `odin/check_client_xqc_runtime_scope.py` continues to ban xquic tokens from `cli_client`; adding DNS symbols does not weaken that scope check. `odin/testing/BUILD.gn:180-204` already links `:odin_dns_resolver_testing` and applies `:odin_dns_resolver_testing_config` target-wide to `odin_unittests`, so `cli_client_unittests.cpp` can push resolver c-ares steps and read resolver liveness without adding a second DNS testing source set.

Under `ODIN_CLI_CLIENT_TESTING`, the existing test snapshot gains DNS liveness and c-ares observation fields read from `odin/testing/dns_resolver_internal_test.h:17-50`. The new CLI timing observation surface reports `dns_on_done_calls`, `query_destroyed_in_callback_calls`, `live_queries_at_callback_exit`, `dns_query_pending_before_dns_event_loop_run`, `accept_loop_create_calls_before_dns_event_loop_run`, `live_accept_loops_before_dns_event_loop_run`, `quic_runtime_add_connection_calls_before_dns_event_loop_run`, `dns_event_loop_run_rc`, `dns_done_after_dns_event_loop_run`, `dns_success_after_dns_event_loop_run`, `live_resolvers_after_dns_event_loop_run`, `live_queries_after_dns_event_loop_run`, `live_resolvers_before_accept_loop_create`, `live_queries_before_accept_loop_create`, `live_resolvers_before_runtime_create`, `live_queries_before_runtime_create`, `live_resolvers_before_runtime_start`, and `live_queries_before_runtime_start`; the runner updates those fields only inside `dns_on_done`, after `odin_dns_resolve_start` has returned with a live query and immediately before the DNS-phase `odin_event_loop_run`, immediately after that DNS-phase run returns and before destroying any still-live DNS object, and immediately before `odin_accept_loop_create`, `quic_runtime_create_default_call`, or `quic_runtime_start_call`.

Two resolver testing hooks are added to `odin/testing/dns_resolver_internal_test.h` under `ODIN_DNS_RESOLVER_TESTING`. `ODIN_DNS_TEST_CARES_RESULT_PENDING` is consumed by `dns_ares_getaddrinfo` after recording `g_obs.getaddrinfo_calls` / `last_ai_family` and before the real c-ares call; it returns without setting `completion_pending`, without recording a result, and without installing c-ares watches, so T8 pairs it with a backend-wait failure and T12 pairs it with a same-loop stop task without depending on external DNS.

`odin_dns_resolver_test_push_addr_result(const odin_dns_addr_t *addrs, size_t addr_count)` is a resolver-side FIFO of synthetic success result sets. The hook returns `-1` with `errno = EINVAL` and does not mutate the FIFO for `addrs == NULL && addr_count > 0`; accepted queue entries return `0`, copy the caller's `odin_dns_addr_t` array under `g_test_mu`, and are reset by `odin_dns_resolver_test_reset_liveness` together with `g_steps_len` and `g_fail_next_result_alloc` after the existing live-DNS-object check passes. `dns_ares_getaddrinfo` consumes one queued address result after observation and before every c-ares step hook or real c-ares call, copies it into query-owned test storage, sets `ARES_SUCCESS` / `completion_pending`, and returns; `after_cares_entrypoint(query, 1)` then arms the normal finalizer timer. `run_finalizer` detects the query-owned test address storage before the c-ares `copy_results` branch, copies it into the same callback-scoped `odin_dns_addr_t` array lifetime used for c-ares results, frees the query-owned test storage during recorded-result cleanup, and invokes the caller callback with `ODIN_DNS_OK`. Precedence inside `dns_ares_getaddrinfo` is: record observation, consume queued address result if present, consume `ODIN_DNS_TEST_CARES_RESULT_PENDING` if it is the next c-ares step, consume existing `ODIN_DNS_TEST_CARES_RESULT_EMPTY_SUCCESS`, consume existing `ODIN_DNS_TEST_CARES_RESULT_STATUS`, otherwise call real `ares_getaddrinfo`. T5 uses this branch to prove the invalid pointer/count pair is rejected without poisoning the next valid queued result and to prove a non-empty DNS OK list with only unsupported or malformed address shapes fails as no usable address; T11 uses it to prove first-usable selection from caller-supplied `AF_UNIX`, IPv6, and IPv4 results. One new CLI failpoint, `ODIN_CLI_CLIENT_TEST_FAIL_DNS_EVENT_LOOP_RUN`, is consumed immediately before the DNS-phase `odin_event_loop_run` by arming `odin_event_loop_test_fail_next_backend_wait(state.loop, errnum)` after the DNS-pending pre-run timing fields are recorded. One new CLI trigger, `ODIN_CLI_CLIENT_TEST_TRIGGER_DNS_EVENT_LOOP_STOP`, is consumed at the same cutpoint by posting a same-loop `odin_event_post` task whose callback calls `odin_event_loop_stop(state.loop)` without setting `dns_done`; paired with `ODIN_DNS_TEST_CARES_RESULT_PENDING`, it exercises the defensive `run_rc == 0 && !dns_done` branch without external DNS. Existing QUIC runtime failpoints remain consumed only after DNS has succeeded.

**Mechanism.**

```text
cleanup_all(state):
  if state.dns_query != NULL:
    odin_dns_query_destroy(state.dns_query)
    state.dns_query = NULL
  if state.dns_resolver != NULL:
    odin_dns_resolver_destroy(state.dns_resolver)
    state.dns_resolver = NULL
  destroy accept loop, signal timer, QUIC runtime, event loop, wakeup fd,
  listener fd, and signal handlers using the current guarded order
```

Satisfies: G2 via common cleanup for every DNS and post-DNS startup-failure path.

## 4. Security

- **S1.**
  - **Threat:** A DNS answer selects a numeric address, and the client accidentally passes that resolved numeric address as the QUIC `server_host`, causing TLS identity verification and the banner to bind to the DNS answer instead of the operator-configured `--server` host.
  - **Mitigation:** §3.2.2 passes the resolved sockaddr only as `peer_addr` and keeps `state.server_host_cstr` as `runtime_config.server_host` and the banner host.
  - **Enforcement:** T1 and T3 assert that the runtime receives the resolved peer sockaddr while `server_host_value` remains the original configured host string.

- **S2.**
  - **Threat:** An internal caller supplies `server_host` with an embedded NUL and a longer `server_host_len`, causing DNS, TLS/SNI, or the banner to use a truncated name while other code observes the longer slice.
  - **Mitigation:** §3.2.1 rejects embedded NUL before listener, event-loop, resolver, or runtime creation.
  - **Enforcement:** T6 passes an embedded-NUL host slice and asserts `server_endpoint`, no banner, no DNS query, and no runtime creation.

No address-deny policy is added for DNS answers in this RFC. `--server` is local operator configuration for the Odin server endpoint; peer-supplied HTTP CONNECT destinations do not enter this client DNS path and remain server-side policy.

## 5. Testing Strategy

The new rows live in `OdinRFC032ClientDnsTest` in `odin/testing/cli_client_unittests.cpp`. They extend the existing RFC-028 child and direct-run helpers with DNS resolver reset, c-ares step injection, synthetic address-result injection, c-ares observation capture, DNS timing capture, and DNS liveness capture. The RFC-032 fake xquic ops are parameterized per row with expected `server_host`, peer family/address/port, local family/address/port, and connect count instead of reusing `Rfc028QuicConnect`'s hard-coded `"127.0.0.1"` / `sockaddr_in` expectations. Child-side fake assertion failures are copied into the child snapshot and cause a reserved nonzero child exit, so the parent fails the row even when `odin_cli_main` returns the expected CLI status. Rows that spawn a child keep the existing parent-side deadlines; rows that write to sockets install `signal(SIGPIPE, SIG_IGN)` before writing. Test-side threads do not call owner-thread-only Odin APIs. The existing `ODIN_DNS_TEST_CARES_RESULT_STATUS`, `ODIN_DNS_TEST_CARES_RESULT_EMPTY_SUCCESS`, `ODIN_DNS_TEST_CARES_LIBRARY_INIT`, and `ODIN_DNS_TEST_CARES_INIT_OPTIONS` steps come from `odin/testing/dns_resolver_internal_test.h:42-50`; this RFC adds `ODIN_DNS_TEST_CARES_RESULT_PENDING` and the `dns_ares_getaddrinfo`-consumed `odin_dns_resolver_test_push_addr_result` queue to that hook surface. T7's library-init branch is not a plain fork child: the parent row fork+execs the current `odin_unittests` binary as `--gtest_filter=OdinRFC032ClientDnsExecChild.T7LibraryInit` with `ODIN_RFC032_EXEC_CHILD=T7_LIBRARY_INIT`, and that child-only test validates the env mode plus active filter before any resolver create or helper-thread setup. An absent mode with direct child-only selection fails before resolver creation; an absent mode during an ordinary unfiltered full-suite run skips before resolver creation; a wrong non-empty mode fails before resolver creation. `ODIN_CLI_CLIENT_TEST_FAIL_DNS_EVENT_LOOP_RUN` is the only new CLI failpoint, and `ODIN_CLI_CLIENT_TEST_TRIGGER_DNS_EVENT_LOOP_STOP` is the only new CLI trigger.

### 5.0 Coverage Matrix

| Axis | Value | Rows |
|------|-------|------|
| G# | G1 resolved server host reaches runtime and accept loop | T1, T2, T3, T10, T11 |
| G# | G2 failures emit deterministic startup failure and cleanup | T4, T5, T6, T7, T8, T9, T12 |
| G# | G3 resolved family selects matching UDP local bind | T1, T2, T3, T11 |
| S# | S1 DNS answer does not replace configured TLS/SNI host | T1, T3 |
| S# | S2 embedded-NUL host slice rejected before resource selection | T6 |
| State | `PRE_DNS_VALIDATE` rejects invalid host slice | T6 |
| State | `LISTENER_READY` starts DNS before accept loop and runtime | T1, T2, T3, T4, T5, T7, T8, T9, T10, T11, T12 |
| State | `S_RESOLVING_SERVER` DNS OK selects address | T1, T2, T3, T9, T10, T11 |
| State | `S_RESOLVING_SERVER` DNS error or no address fails startup | T4, T5 |
| State | `S_RESOLVING_SERVER` setup or loop failure fails startup | T7, T8, T12 |
| State | `POST_DNS_RUNTIME_STARTUP` runtime create or start fails | T9 |
| State | `RUNTIME_READY` accepts local proxy connection after DNS | T10 |
| Completion mode | DNS callback finalizer for hooked hostname result | T1, T4, T5, T9, T10, T11 |
| Completion mode | DNS numeric-literal finalizer | T2, T3 |
| Completion mode | Synchronous resolver setup failure before callback | T7 |
| Completion mode | Pending query cleanup after DNS loop failure | T8 |
| Completion mode | DNS loop returns zero before query completion | T12 |
| Benign-vs-fatal split | DNS OK with usable address | T1, T2, T3, T10, T11 |
| Benign-vs-fatal split | DNS OK with zero usable addresses, including empty and unsupported-only results | T5 |
| Benign-vs-fatal split | DNS error status | T4 |
| Benign-vs-fatal split | DNS setup or event-loop fatal path | T7, T8, T12 |
| Constructor / factory precondition | CLI host slice validation | T6 |
| Constructor / factory precondition | QUIC runtime create/start after DNS | T9 |
| Address selection ordering | unsupported first result is skipped and the first usable result wins | T11 |
| Callback-safe lifecycle hand-off | `dns_on_done` destroys completed query before returning | T1, T4, T5 |
| DNS-pending accept guard | no accept loop or runtime add-connection path exists while DNS is pending | T8 |
| DNS-pending no-completion guard | zero-return DNS loop without callback still fails and cleans pending query | T12 |
| Pre-accept/runtime DNS cleanup cutpoint | DNS liveness is zero before accept-loop and QUIC create/start failpoints | T9 |

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | Hostname resolves to IPv4 before QUIC startup | Child run with `--server odin.test:8443`, CA fixture, fake xquic ops, and `ODIN_DNS_TEST_CARES_RESULT_STATUS` success | Banner is `server=odin.test:8443`; DNS observation has `getaddrinfo_calls == 1` and `last_ai_family == AF_UNSPEC`; runtime config `peer_addr` is `AF_INET 127.0.0.1:8443`, `local_addr` is `AF_INET 0.0.0.0:0`, `server_host_value == "odin.test"`, runtime create/start counts are 1, DNS timing has `query_destroyed_in_callback_calls == 1` and `live_queries_at_callback_exit == 0`, and DNS liveness is zero after SIGTERM cleanup | G1, G3, S1 | integration |
| T2 | IPv4 literal still uses async resolver path | Child run with `--server 127.0.0.1:4433`, CA fixture, and fake xquic ops without a DNS result hook | Banner is `server=127.0.0.1:4433`; DNS observation has `getaddrinfo_calls == 1`; runtime config `peer_addr` is `AF_INET 127.0.0.1:4433`, `local_addr` is `AF_INET 0.0.0.0:0`, and DNS liveness is zero after cleanup | G1, G3 | integration |
| T3 | IPv6 literal resolves and uses IPv6 UDP bind | Child run with `--server [::1]:9443`, CA fixture, and fake xquic ops without a DNS result hook | Banner is `server=[::1]:9443`; runtime config `peer_addr` is `AF_INET6 [::1]:9443`, `local_addr` is `AF_INET6 [::]:0`, `server_host_value == "::1"`, `getaddrinfo_calls == 1`, and DNS liveness is zero after cleanup | G1, G3, S1 | integration |
| T4 | DNS result errors fail before banner | Subcases: child run with `--server missing.test:4433` and pushed `ARES_ENOTFOUND`; child run with `--server slow.test:4433` and pushed `ARES_ETIMEOUT` | Each run returns `1`, stderr is exactly `odin: client startup failed at server_dns\n`, no success banner appears, runtime create/start counts are 0, DNS timing has `query_destroyed_in_callback_calls == 1` and `live_queries_at_callback_exit == 0`, and listener, event-loop, DNS, and runtime liveness counters are zero | G2 | integration |
| T5 | DNS OK with no usable addresses fails before runtime | Subcases: child run with `--server empty.test:4433` and pushed `ODIN_DNS_TEST_CARES_RESULT_EMPTY_SUCCESS`; child run with `--server unsupported.test:4433` whose setup first asserts `odin_dns_resolver_test_push_addr_result(NULL, 1) == -1` with `errno == EINVAL`, then pushes one valid `odin_dns_resolver_test_push_addr_result` queue entry containing only unsupported or malformed addresses: `AF_UNIX`, `AF_INET` with `addrlen != sizeof(struct sockaddr_in)`, and `AF_INET6` with `addrlen != sizeof(struct sockaddr_in6)` | Each run returns `1`, stderr is exactly `odin: client startup failed at server_dns\n`, runtime create/start counts are 0, the invalid hook call leaves the FIFO unchanged so the later valid unsupported-only result is the consumed DNS OK result, DNS result storage is freed, DNS timing has `query_destroyed_in_callback_calls == 1` and `live_queries_at_callback_exit == 0`, and listener, event-loop, DNS, and runtime liveness counters are zero | G2 | integration |
| T6 | Invalid host slice is rejected before resources | Subcases: direct `odin_cli_run_client` with `server_host_len == 0`; direct run with `server_host_len == ODIN_PROTO_HOST_MAX + 1`; child direct-run with `server_host == NULL` and nonzero length; direct run with `server_host = "good\0bad"` length 8; direct run with `server_host = "127.0.0.1\0bad"` length 13 and `ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_CREATE` armed | Each subcase returns `1` through the parent deadline when it uses a child, stderr is exactly `odin: client startup failed at server_endpoint\n`, no DNS observation is recorded, runtime create/start counts are 0, the valid-prefix embedded-NUL subcase leaves the runtime-create failpoint pending, and CLI/event-loop/DNS liveness counters stay zero | G2, S2 | unit |
| T7 | Resolver create and start setup failures clean up | Subcases: fork+exec child-only library-init run of the current `odin_unittests` binary as `--gtest_filter=OdinRFC032ClientDnsExecChild.T7LibraryInit` with `ODIN_RFC032_EXEC_CHILD=T7_LIBRARY_INIT`, which validates the mode and filter, pushes `ODIN_DNS_TEST_CARES_LIBRARY_INIT` status `ARES_ENOMEM`, then calls `odin_cli_main` as `odin-client --listen 0 --server libinit.test:4433 --ca-file <fixture>`; ordinary child run with pushed `ODIN_DNS_TEST_CARES_INIT_OPTIONS` status `ARES_EFORMERR` | The exec child exits success only after the CLI run returns `1`, stderr is exactly `odin: client startup failed at server_dns\n`, no banner appears, runtime create/start counts are 0, `ares_library_init_calls == 1`, and listener, event-loop, resolver, query, watch, timer, channel, and result liveness counters are zero; direct child-only selection without `ODIN_RFC032_EXEC_CHILD` exits nonzero before resolver creation, while ordinary unfiltered full-suite selection skips before resolver creation. The init-options child has the same CLI failure, banner, runtime-count, and liveness expectations without depending on the parent process' c-ares library-init guard state | G2 | integration |
| T8 | DNS-phase event-loop failure cleans up without accepting while DNS is pending | Child run with `--listen <known_free_port>`, parent-side external probes using `127.0.0.1:<known_free_port>`, `--server pending.test:4433`, pushed `ODIN_DNS_TEST_CARES_RESULT_PENDING`, and `ODIN_CLI_CLIENT_TEST_FAIL_DNS_EVENT_LOOP_RUN` armed with `EIO` | DNS timing before the DNS-phase event-loop run has `dns_query_pending_before_dns_event_loop_run == 1`, `accept_loop_create_calls_before_dns_event_loop_run == 0`, `live_accept_loops_before_dns_event_loop_run == 0`, and `quic_runtime_add_connection_calls_before_dns_event_loop_run == 0`; DNS post-run timing has `dns_event_loop_run_rc == -1`, `dns_done_after_dns_event_loop_run == 0`, and live query/resolver counts still nonzero before `resolve_server_endpoint` destroys and clears them; then the run returns `1`, stderr is exactly `odin: client startup failed at server_dns\n`, runtime create/start counts are 0, and liveness counters are zero | G2 | integration |
| T9 | Post-DNS runtime startup failures keep DNS cleaned | Subcases: hostname success hook plus `ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_CREATE`; hostname success hook plus `ODIN_CLI_CLIENT_TEST_FAIL_XQC_CLIENT_RUNTIME_START` | Create-failure run prints `xqc_client_runtime_create`; start-failure run prints `xqc_client_runtime_start`; both print no success banner, DNS timing shows zero live resolvers and queries at the pre-accept-loop cutpoint and at the pre-runtime-create or pre-runtime-start cutpoint before `cleanup_all`, and existing runtime liveness expectations match the current RFC-028 failure rows | G2 | integration |
| T10 | Accepted local connection after DNS success reaches runtime | Child run with `--server odin.test:4433`, hostname success hook, fake xquic ops, then parent connects to the printed proxy port and writes one HTTP CONNECT request | `quic_runtime_add_connection_calls == 1`, recorded fd is nonblocking, no upstream TCP listener is accepted by the client process, the child exits 0 after SIGTERM, and CLI/DNS/runtime liveness is zero | G1 | integration |
| T11 | Multiple DNS results select first usable address | Child run with `--server ordered.test:9443`, fake xquic ops, and one `odin_dns_resolver_test_push_addr_result` queue entry consumed by the resolver's `dns_ares_getaddrinfo` branch, returning, in order, an unsupported `AF_UNIX` sockaddr, `AF_INET6 2001:db8::32` with placeholder port `1111`, and `AF_INET 127.0.0.77` with placeholder port `2222` | Resolver observation still shows one `AF_UNSPEC` `getaddrinfo` request; banner is `server=ordered.test:9443`; runtime config `peer_addr` is `AF_INET6 [2001:db8::32]:9443`, `local_addr` is `AF_INET6 [::]:0`, `server_host_value == "ordered.test"`, and DNS timing/liveness matches T1; the later IPv4 result and placeholder ports are not observed by the runtime | G1, G3 | integration |
| T12 | DNS loop stops cleanly without DNS completion | Child run with `--server pending-stop.test:4433`, pushed `ODIN_DNS_TEST_CARES_RESULT_PENDING`, and `ODIN_CLI_CLIENT_TEST_TRIGGER_DNS_EVENT_LOOP_STOP` armed so the runner posts a same-loop task that calls `odin_event_loop_stop` during the DNS-phase run | DNS timing before the DNS-phase event-loop run has `dns_query_pending_before_dns_event_loop_run == 1`, `accept_loop_create_calls_before_dns_event_loop_run == 0`, and `quic_runtime_add_connection_calls_before_dns_event_loop_run == 0`; DNS post-run timing has `dns_event_loop_run_rc == 0`, `dns_done_after_dns_event_loop_run == 0`, `dns_success_after_dns_event_loop_run == 0`, and live query/resolver counts still nonzero before `resolve_server_endpoint` destroys and clears them; the child returns `1`, stderr is exactly `odin: client startup failed at server_dns\n`, no banner appears, runtime create/start counts are 0, and DNS/event-loop/CLI liveness counters are zero | G2 | integration |

## 6. Implementation Plan

- **P1. Land red-verifiable RFC-032 client DNS rows behind a default skip.**
  - **Scope:** extend `odin/testing/cli_client_unittests.cpp` with `OdinRFC032ClientDnsTest` rows T1-T12 plus child-only `OdinRFC032ClientDnsExecChild.T7LibraryInit`, DNS liveness, DNS timing including T8/T12's DNS-pending pre-run accept-loop/add-connection fields and post-run DNS completion fields, and c-ares observation capture in the existing child snapshot; add parameterized RFC-032 fake xquic ops for expected host, peer/local family and address, and connect count; surface child-side fake assertion failures to the parent through the child snapshot and a reserved nonzero child exit; add DNS c-ares step setup in child/direct helpers, the T6 child direct-run helper with parent-side wait/read deadlines, T7's fork+exec launcher with `ODIN_RFC032_EXEC_CHILD=T7_LIBRARY_INIT`, `ODIN_DNS_TEST_CARES_RESULT_PENDING` and `odin_dns_resolver_test_push_addr_result` in the DNS resolver testing hook surface, resolver-side FIFO storage/reset plus the `dns_ares_getaddrinfo` consumption/finalizer branch described in §3.2.3, `odin_cli_client_test_dns_timing_t` / `odin_cli_client_test_dns_timing` in the CLI client testing hook surface, and the `ODIN_CLI_CLIENT_TEST_FAIL_DNS_EVENT_LOOP_RUN` and `ODIN_CLI_CLIENT_TEST_TRIGGER_DNS_EVENT_LOOP_STOP` enum/declaration/storage needed for T8 and T12 to compile. The existing `:odin_dns_resolver_testing_config` remains applied target-wide to `odin_unittests`, so the resolver hook gate is active for the CLI rows without adding a per-test GN config. Each `OdinRFC032ClientDnsTest` row calls `GTEST_SKIP("pending RFC-032 P2")` unless `ODIN_RFC032_RED=1` is set; `ODIN_RFC032_RED=1 out/client_dns_mac/odin_unittests --gtest_filter=OdinRFC032ClientDnsTest.*` is the red-verification command. No client production DNS integration lands in this phase.
  - **Depends on:** None.
  - **Done when:** host-runnable `out/client_dns_mac/odin_unittests --gtest_filter=OdinRFC032ClientDnsTest.*` reports T1-T12 skipped by default, `out/client_dns_mac/odin_unittests --gtest_filter=OdinRFC032ClientDnsExecChild.T7LibraryInit` without `ODIN_RFC032_EXEC_CHILD` exits nonzero before resolver creation, the full host `out/client_dns_mac/odin_unittests` remains green with that child-only row skipped before resolver creation, and `ODIN_RFC032_RED=1 out/client_dns_mac/odin_unittests --gtest_filter=OdinRFC032ClientDnsTest.*` executes T1-T12 and fails against the current IPv4-only client runner. T1 and T3 red evidence uses non-default parsed ports, so a hard-coded `4433` runtime port cannot satisfy the success assertions. T5's red evidence includes both empty-success and unsupported-only DNS OK subcases failing to reach the new `server_dns` cleanup assertions, including listener/event-loop liveness checks; the unsupported-only subcase also asserts `odin_dns_resolver_test_push_addr_result(NULL, 1) == -1/EINVAL` before pushing the valid queue entry, so an invalid push that mutates the FIFO cannot pass. T6's red evidence specifically includes the `server_host == NULL` child subcase completing through the parent deadline with non-`1` status and the `127.0.0.1\0bad` subcase reaching the armed runtime-create failpoint instead of `server_endpoint`, so neither subcase can hang or falsely pass. T8's red evidence includes a DNS-pending snapshot with `accept_loop_create_calls_before_dns_event_loop_run == 0`, `live_accept_loops_before_dns_event_loop_run == 0`, and `quic_runtime_add_connection_calls_before_dns_event_loop_run == 0`; an implementation that creates the accept loop before the pending DNS query fails this row before cleanup can hide the bug. T12's red evidence includes `dns_event_loop_run_rc == 0` with `dns_done_after_dns_event_loop_run == 0`, so an implementation that treats zero loop return as DNS success without callback completion cannot pass. Cross-compile-only binaries built but not executed are `out/client_dns_linux_x64/odin_unittests`, `out/client_dns_mac_arm64/odin_unittests`, `out/client_dns_ios_sim_arm64/odin_unittests`, and `out/client_dns_ios_device_arm64/odin_unittests`; `//odin:odin_cli_artifacts` in those same output directories produces built-only `odin`, `odin-client`, and `odin-server` artifacts. Their alternate event-loop and resolver backend branches are compile-only evidence.

- **P2. Implement client async DNS resolution and turn T1-T12 green.**
  - **Scope:** replace `copy_ipv4_server_endpoint` in `odin/cli_client.c` with host-slice validation and DNS startup state; add DNS resolver/query cleanup to `cleanup_all`; call `odin_dns_resolver_create`, `odin_dns_resolve_start(..., AF_UNSPEC, ...)`, and a DNS-phase `odin_event_loop_run` before accept-loop and runtime creation; copy the first usable IPv4 or IPv6 result, force the parsed port onto the selected sockaddr, build a family-matching wildcard UDP local bind, keep `server_host_cstr` as runtime `server_host`, bracket IPv6 hosts only in the banner, populate the DNS timing cutpoints, consume `ODIN_CLI_CLIENT_TEST_FAIL_DNS_EVENT_LOOP_RUN` and `ODIN_CLI_CLIENT_TEST_TRIGGER_DNS_EVENT_LOOP_STOP`, add `:odin_dns_resolver` to `//odin:odin_cli_client`, remove the RFC-032 test skips, and update or retire `OdinRFC028ClientTransportTest.T4QuicRejectsParsedNonIpv4BeforeResources` so it no longer asserts `example.com:443` or `[::1]:443` fail at `server_endpoint`; any retained RFC-028 row covers only malformed host/parser cases outside RFC-032's accepted DNS-name and IPv6-literal contract. The existing `odin_client_xqc_runtime_scope_check` remains in the build so the CLI still cannot include xquic internals directly.
  - **Depends on:** P1.
  - **Done when:** host-runnable `out/client_dns_mac/odin_unittests --gtest_filter=OdinRFC032ClientDnsTest.*` passes T1-T12 un-gated, including T1/T3/T11's non-default parsed-port runtime assertions, T5's invalid `push_addr_result` boundary and empty-success/unsupported-only DNS OK cleanup assertions, T7's exec-isolated library-init branch plus ordinary init-options branch, T8's DNS-pending no-accept-loop assertion, and T12's `run_rc == 0 && !dns_done` cleanup assertion, the full host `out/client_dns_mac/odin_unittests` passes after the obsolete RFC-028 non-IPv4 rejection expectation is updated or removed, and `//odin:odin_client_xqc_runtime_scope_check` passes. Cross-compile-only binaries built but not executed are `out/client_dns_linux_x64/odin_unittests`, `out/client_dns_mac_arm64/odin_unittests`, `out/client_dns_ios_sim_arm64/odin_unittests`, `out/client_dns_ios_device_arm64/odin_unittests`, plus the corresponding built-only `out/client_dns_linux_x64/odin`, `out/client_dns_linux_x64/odin-client`, `out/client_dns_linux_x64/odin-server`, `out/client_dns_mac_arm64/odin`, `out/client_dns_mac_arm64/odin-client`, `out/client_dns_mac_arm64/odin-server`, `out/client_dns_ios_sim_arm64/odin`, `out/client_dns_ios_sim_arm64/odin-client`, `out/client_dns_ios_sim_arm64/odin-server`, `out/client_dns_ios_device_arm64/odin`, `out/client_dns_ios_device_arm64/odin-client`, and `out/client_dns_ios_device_arm64/odin-server` artifacts. T1-T12's macOS host runtime evidence covers the platform-agnostic client contract, while alternate kqueue/epoll/iOS resolver and event-loop branches are build-only plus code-review evidence.
