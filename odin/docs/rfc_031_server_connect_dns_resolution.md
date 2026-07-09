# RFC-031: Server CONNECT DNS Resolution

## 1. Summary

Route the server-side CONNECT host through `odin_dns_resolve_start` before dialing, so both DNS names and numeric literals are converted to resolved socket addresses before the existing server-session dial, filter, response, and relay paths run.

## 2. Goals

- **G1.** For every decoded server CONNECT request whose host resolves to a usable stream address for the requested port, the server session dials a resolved address, emits a CONNECT response with `error_code = 0x0000`, and relays bytes exactly as the current resolved-address path does.
- **G2.** DNS validation, resolver setup, DNS failure, no-address, all-address-denied, and post-resolution dial setup failures produce the existing nonzero CONNECT response mapping and fire `on_close(ss, err)` with the original errno while leaving no live DNS query or owned resolver state.
- **G3.** When a dial filter is installed, every resolved address selected from a peer-supplied CONNECT host is offered to that filter before any outbound `connect(2)` is issued, and a nonzero filter result prevents that address from being dialed.
- **G4.** The QUIC server runtime supplies one shared DNS resolver to stream-backed server sessions, so accepted streams use DNS resolution without allocating a resolver per stream.

## 3. Design

### 3.1 Overview

`odin/server_session` stops interpreting the decoded CONNECT host as an IPv4 literal inside `session_on_req_decoded` and instead starts an Odin DNS query with the exact host slice and decoded port. The DNS completion callback chooses a resolved address, applies the existing `odin_server_session_set_dial_filter` hook to the resolved sockaddr, and then enters the current `odin_dial_start` path. Existing response mapping, tail injection, relay startup, and terminal teardown stay owned by the server-session phase machine.

The QUIC server runtime creates one resolver during runtime creation and passes that resolver to every stream session. Direct `odin_server_session_create*` callers keep a default private resolver path, while tests and advanced callers can use resolver-aware constructors to supply a preconfigured resolver.

```text
CONNECT_REQ host bytes + port
    |
    v
odin_connect_session SERVER decode
    |
    v
odin_dns_resolve_start(host, port)
    |
    v
for each resolved sockaddr: dial_filter(sockaddr)
    |
    v
first permitted address -> odin_dial_start -> existing RESP and relay paths
```

### 3.2 Detailed Design

#### 3.2.1 Server Session DNS API and State

Current code copies the decoded host and calls `inet_pton(AF_INET, ...)` at `odin/server_session.c:333-379`; this RFC replaces that branch with resolver-backed selection. The existing resolver API is declared at `odin/dns_resolver.h:42-50`, and `odin/BUILD.gn:129` already defines `:odin_dns_resolver`.

```c
/* odin/server_session.h additions */
#include "odin/dns_resolver.h"

int odin_server_session_create_with_resolver(
    odin_event_loop_t *loop, int conn_fd, odin_dns_resolver_t *resolver,
    odin_server_session_close_cb on_close, void *user_data,
    odin_server_session_t **out);

int odin_server_session_create_with_transport_and_resolver(
    odin_event_loop_t *loop,
    odin_server_session_transport_factory_cb create_downstream,
    void *factory_user_data, odin_dns_resolver_t *resolver,
    odin_server_session_close_cb on_close, void *user_data,
    odin_server_session_t **out);
```

```text
New internal state:
  S_RESOLVING
  odin_dns_resolver_t *resolver
  odin_dns_query_t *dns_query
  int owns_resolver
```

```text
Test-only DNS liveness additions under ODIN_DNS_RESOLVER_TESTING:
  odin_dns_resolver_test_liveness_t.resolver_create_calls
  odin_dns_resolver_test_liveness_t.resolver_destroy_calls
```

**Unstated contract.** Resolver-aware constructors require non-NULL `loop`, `resolver`, `on_close`, and `out`; the transport variant also requires non-NULL `create_downstream`. Invalid inputs return `-1/EINVAL`, leave `*out` untouched when `out != NULL`, do not close the caller-owned `conn_fd`, and do not call `create_downstream`. The supplied resolver is caller-owned, owner-thread only, and must outlive the session or be destroyed only after the session is destroyed. It must be the resolver created for the same `odin_event_loop_t *loop` passed to the constructor; this same-loop relationship is an unchecked precondition because `odin_dns_resolver_t` is opaque and exposes no loop accessor. Violating it is unsupported: DNS watches, timers, and callbacks run on the resolver's loop, so a mismatched resolver can invoke `dns_on_done` on a different loop than `ss->loop` and then issue `odin_dial_start(ss->loop, ...)` from the wrong owner thread. The existing `odin_server_session_create` and `odin_server_session_create_with_transport` signatures remain exported; they create a private default resolver with `odin_dns_resolver_create(loop, NULL, &resolver)` during construction, set `owns_resolver = 1`, and destroy it during terminal teardown or abort. If private resolver creation fails, the constructor returns before downstream factory, transport, connect-session, or READ-interest setup, preserves the resolver errno, leaves `*out` untouched, and leaves `conn_fd` caller-owned as before. If a later helper branch fails after private resolver creation, the constructor preserves that branch errno, rolls back the created transport/session state, destroys the private resolver, leaves `*out` untouched, and leaves `conn_fd` caller-owned as before. Under `ODIN_DNS_RESOLVER_TESTING`, successful `odin_dns_resolver_create` increments `resolver_create_calls`, each real `odin_dns_resolver_destroy` increments `resolver_destroy_calls`, and `odin_dns_resolver_test_reset_liveness` resets those cumulative counters only when all live DNS resources are already zero; final `resolvers == baseline.resolvers` alone is not rollback proof. All valid server-session DNS operations run on the event-loop owner thread; alternate event-loop and c-ares platform behavior is compile-verified on Linux, alternate macOS arch, and iOS, but only the macOS host binary runtime-verifies these rows in this environment.

**Mechanism.**

```text
create_with_resolver(..., conn_fd, resolver, ...):
  validate loop, resolver, on_close, out
  require resolver was created for loop as an unchecked precondition
  allocate ss with conn_fd, dial_fd = -1, state = S_HANDSHAKE
  ss.resolver = resolver
  ss.owns_resolver = 0
  create fd downstream transport, connect-session create, READ-interest setup
  publish *out

create_with_transport_and_resolver(..., resolver, ...):
  validate loop, factory, resolver, on_close, out
  require resolver was created for loop as an unchecked precondition
  allocate ss with conn_fd = -1, dial_fd = -1, state = S_HANDSHAKE
  ss.resolver = resolver
  ss.owns_resolver = 0
  run the existing downstream factory, connect-session create, READ-interest setup
  publish *out

create(...):
  validate loop, on_close, out
  create private resolver using config NULL
  if resolver create fails: preserve errno and return -1
  call the shared fd construction helper
  if later helper setup fails: destroy private resolver before returning failure

create_with_transport(...):
  validate loop, factory, on_close, out
  create private resolver using config NULL
  if resolver create fails: preserve errno and return -1
  call the shared construction helper
  if later helper setup fails: destroy private resolver before returning failure

finish_destroy/fire_terminal:
  if dns_query != NULL: odin_dns_query_destroy(dns_query); dns_query = NULL
  destroy relay, dial, connect session, transports, and fds in current order
  if owns_resolver: odin_dns_resolver_destroy(resolver)
```

Satisfies: G1 via the new `S_RESOLVING` state between request decode and dial; G2 via constructor rollback and DNS query teardown; G4 via resolver-aware constructors the runtime can use.

#### 3.2.2 DNS Completion, Address Selection, and Error Mapping

```text
S_HANDSHAKE + on_req_decoded:
  host_ptr, host_len = odin_connect_session_server_host(s)
  port = odin_connect_session_server_port(s)
  if odin_dns_resolve_start(resolver, host_ptr, host_len, port, AF_UNSPEC,
                            dns_on_done, ss, &dns_query) != 0:
    handle_dial_result(ss, errno)
    return
  state = S_RESOLVING
  downstream interest remains zero through connect-session wants()

dns_on_done(query, status, err, addrs, addr_count, ss):
  ss_enter(ss)
  if on_close_fired or destroy_pending:
    ss_leave(ss)
    return
  dns_query = NULL
  odin_dns_query_destroy(query)
  if status != ODIN_DNS_OK:
    handle_dial_result(ss, err)
    ss_leave(ss)
    return
  select_and_dial(addrs, addr_count)
  ss_leave(ss)
```

**Unstated contract.** The exact decoded host slice is passed to `odin_dns_resolve_start`; the session does not make a temporary C string for DNS, so embedded NUL bytes cannot truncate the resolver question. `odin_dns_resolve_start` rejects embedded NUL, empty, and over-255-byte names in current code (`odin/dns_resolver.c:236`), while the CONNECT decoder already limits host length to 1..255. DNS is requested with `AF_UNSPEC` so the resolver may return IPv4 or IPv6 addresses. The peer-supplied CONNECT port is the resolver service argument and is preserved in each resolved sockaddr observed by `dial_filter` and `odin_dial_start`, so address and port policy are enforced on the same object before any outbound connect. The resolver already sets `ARES_FLAG_NOALIASES`, `ARES_FLAG_NOSEARCH`, `lookups == "b"`, and `ARES_AI_NOSORT` at `odin/dns_resolver.c:1128-1171`; this integration relies on that DNS-only policy and does not add local host rewriting. DNS completion inherits the existing server-session callback-safety contract: every callback entry that can invoke user code holds an `ss_enter` frame, `odin_server_session_destroy(ss)` inside `dial_filter` only sets `destroy_pending`, and the callback stops address selection before touching `ss` again. The callback receives callback-scoped `odin_dns_addr_t` entries, so this RFC dials at most one selected address directly from the callback and does not implement asynchronous retry across connection failures. A follow-up RFC can add owned address-copying and retry if required.

**Mechanism.**

```text
select_and_dial(addrs, addr_count):
  if addr_count == 0:
    handle_dial_result(ss, EHOSTUNREACH)
    return
  first_filter_err = 0
  first_start_err = 0
  for addr in addrs in resolver order:
    if destroy_pending or on_close_fired: return
    if dial_filter exists:
      filter_err = dial_filter(&addr.addr, addr.addrlen, dial_filter_ud)
      if destroy_pending or on_close_fired: return
      if filter_err != 0:
        if first_filter_err == 0: first_filter_err = filter_err
        continue
    if ODIN_SERVER_SESSION_TESTING fail_next_dial is armed:
      if first_start_err == 0: first_start_err = armed errno
      continue
    if odin_dial_start(loop, &addr.addr, addr.addrlen, dial_on_done, ss, &dial) == 0:
      state = S_DIALING
      return
    if first_start_err == 0: first_start_err = errno
  if destroy_pending or on_close_fired: return
  if first_start_err != 0:
    handle_dial_result(ss, first_start_err)
  else if first_filter_err != 0:
    handle_dial_result(ss, first_filter_err)
  else:
    handle_dial_result(ss, EHOSTUNREACH)
```

`handle_dial_result` keeps the RFC-020 mapping: `ECONNREFUSED -> 0x0001`, `EHOSTUNREACH -> 0x0002`, `ETIMEDOUT -> 0x0003`, and every other errno -> `0x0004`; `on_close` receives the original errno, not the wire code. Destroying a session in `S_RESOLVING` calls `odin_dns_query_destroy`, closes the downstream transport/fd, suppresses `on_close`, and leaves caller-owned resolvers alive. Under `ODIN_SERVER_SESSION_TESTING`, the existing `odin_server_session_test_fail_next_dial` hook is consumed immediately before the production `odin_dial_start` call in `select_and_dial`, so tests can force the post-resolution synchronous start-failure branch without bypassing resolver entry.

Satisfies: G1 via DNS OK address selection and the existing dial/OK-response path; G2 via DNS/start/filter/no-address error routing through `handle_dial_result`; G3 via filtering resolved sockaddrs before `odin_dial_start`.

#### 3.2.3 QUIC Runtime Resolver Ownership

`odin/server_xqc_runtime.c:1116-1125` currently creates a stream session and then installs the dial filter. This RFC adds a resolver field to the runtime and passes it into the resolver-aware stream-session constructor.

```text
odin_xqc_server_runtime_t additions:
  odin_dns_resolver_t *resolver

runtime_create(config, out):
  allocate rt
  odin_dns_resolver_create(config->loop, NULL, &rt.resolver)
  if resolver create fails: free rt, return -1 with errno
  create event-loop UDP runtime as today
  if later runtime setup fails: destroy rt.resolver before freeing rt

runtime_stream_create_notify(stream):
  odin_server_session_create_with_transport_and_resolver(
      rt->loop, xqc_stream_transport_factory, stream_ctx,
      rt->resolver, runtime_stream_session_on_close, stream_ctx,
      &stream_ctx->ss)
  odin_server_session_set_dial_filter(stream_ctx->ss, rt->dial_filter,
                                      rt->dial_filter_ud)

runtime_finish_destroy(rt):
  destroy live stream sessions before destroying rt.resolver
  odin_dns_resolver_destroy(rt.resolver)
```

**Unstated contract.** Runtime creation remains owner-thread only. The runtime-owned resolver uses default c-ares server configuration and the resolver's DNS-only policy; no new CLI flag or runtime config field is added by this RFC. The resolver is destroyed after every stream session, because sessions may still hold `dns_query` handles linked to it. The runtime has no direct platform-specific DNS branch; c-ares and the Odin event loop carry macOS, Linux, and iOS differences, and this environment runtime-verifies only the macOS host path.

Satisfies: G4 via one runtime-owned resolver passed to each stream session.

## 4. Security

- **S1.**
  - **Threat:** A peer supplies a DNS name whose answer contains private, loopback, link-local, metadata, or otherwise policy-denied addresses, or pairs an otherwise permitted address with a policy-denied CONNECT port, causing the server to bypass an installed SSRF filter by filtering the hostname or address without the peer-supplied port.
  - **Mitigation:** §3.2.2 applies `odin_server_session_set_dial_filter` to each resolved sockaddr carrying the original CONNECT port before `odin_dial_start`; if every address/port tuple is denied, no outbound `connect(2)` is issued and the original filter errno is reported through the existing error-response path.
  - **Enforcement:** T4 proves a denied first answer is skipped before dialing a later permitted answer, T5 proves the denied IPv4 sockaddr exposes the exact CONNECT port to the filter and produces a failure response with no upstream accept, and T11 proves destroy from inside the filter stops later-address selection before any dial.

- **S2.**
  - **Threat:** Peer-supplied host bytes contain an embedded NUL, causing the DNS query or policy observation to use a truncated name.
  - **Mitigation:** §3.2.2 passes the decoded `(host_ptr, host_len)` slice directly to `odin_dns_resolve_start`, whose current validation rejects embedded NUL before c-ares receives the name.
  - **Enforcement:** T6 sends `bad\0name` and asserts `EINVAL`, no DNS query, no dial, and the catch-all response code.

- **S3.**
  - **Threat:** Local `HOSTALIASES`, search-domain, or hosts-file policy rewrites the peer-supplied host before DNS, so outbound resource selection differs from the CONNECT host bytes the peer sent.
  - **Mitigation:** §3.2.2 relies on the resolver's DNS-only c-ares options at `odin/dns_resolver.c:1128-1171`: no aliases, no search expansion, hosts-file lookup disabled, and no c-ares address-sort probes.
  - **Enforcement:** T7 runs the integrated server-session path under a `HOSTALIASES` mapping, asserts the DNS fixture sees the original single-label host, and asserts the observed c-ares options use DNS-only lookup flags.

## 5. Testing Strategy

The new rows are unit tests split across `OdinServerDnsTest` in `odin/testing/server_session_unittests.cpp` and `OdinXqcServerRuntimeDnsTest` in `odin/testing/server_xqc_runtime_unittests.cpp`. They reuse the existing fork plus 2 s parent deadline and in-loop watchdog pattern from the current server-session and DNS resolver tests. Rows that read or write sockets install deadlines or use nonblocking I/O; rows that can write to a closed peer install `signal(SIGPIPE, SIG_IGN)` before the action. Test-side threads never call owner-thread-only Odin APIs; owner-thread inspection uses timers or posted callbacks on the loop thread. Timeout subcases in T3 and T7 create their injected resolver with `odin_dns_resolver_config_t{servers_csv = fixture_servers, timeout_ms = 50, tries = 1}`, so the expected `ETIMEDOUT` arrives from c-ares before the 300 ms loop watchdog and the 2 s parent deadline.

**Test-hook contracts.** T14 and T15 use `odin_dns_resolver_test_push_cares_step` from `odin/testing/dns_resolver_internal_test.h` with `ODIN_DNS_TEST_CARES_LIBRARY_INIT` to inject private/runtime resolver-create failure before helper setup; T13 uses the same helper with `ODIN_DNS_TEST_CARES_INIT_OPTIONS` to inject resolver-start c-ares setup failure. T3 adds `#define ODIN_DNS_TEST_CARES_RESULT_EMPTY_SUCCESS 9` to the same header and pushes it through `odin_dns_resolver_test_push_cares_step`; the testing-gated `dns_ares_getaddrinfo` wrapper in `odin/dns_resolver.c` consumes that op before the existing `ODIN_DNS_TEST_CARES_RESULT_STATUS` path and completes the query with `ARES_SUCCESS`, an allocated `struct ares_addrinfo`, and `nodes == NULL`, so existing `copy_results` returns `addr_count == 0` and `run_finalizer` invokes the production resolver callback with `ODIN_DNS_OK`. The gated production wrappers are `dns_ares_library_init`, `dns_ares_init_options`, and `dns_ares_getaddrinfo` in `odin/dns_resolver.c`, and P1 adds `:odin_dns_resolver_testing_config` to `odin_unittests` so `ODIN_DNS_RESOLVER_TESTING` is visible target-wide. T13 and T17 use `odin_server_session_test_fail_next_dial` from `odin/testing/server_session_internal_test.h`, consumed in §3.2.2 immediately before the production `odin_dial_start` call in `select_and_dial`, with `:odin_server_session_testing_config` enabling `ODIN_SERVER_SESSION_TESTING`. T14 uses `odin_connect_session_test_fail_next_create_server` from `odin/testing/connect_session_internal_test.h`, consumed by `odin_connect_session_create_server`, with `:odin_connect_session_testing_config` enabling `ODIN_CONNECT_SESSION_TESTING`; T14's factory-failure and READ-interest-failure branches use test-local fake `odin_transport_t` vtables, not production hooks. T15 uses `odin_xqc_udp_test_set_ops` with `odin_xqc_udp_test_ops_t.engine_create` from `odin/testing/xqc_udp_internal_test.h` to make `xqc_udp_engine_create_call` fail under `runtime_udp_create_call`, and uses `odin_xqc_server_runtime_test_set_ops` with `odin_xqc_server_runtime_test_ops_t.engine_register_alpn` from `odin/testing/server_xqc_runtime_internal_test.h` to make `runtime_engine_register_alpn_call` fail; `:odin_xqc_udp_testing_config` and `:odin_xqc_server_runtime_testing_config` enable those gates for `odin_unittests`. T16 uses a test-local scripted `odin_transport_t` vtable, not a production hook, to return a 2-byte OK write, then `ODIN_TRANSPORT_AGAIN`, then the remaining 2-byte OK write on WRITE readiness.

### 5.0 Coverage Matrix

| Axis | Value | Rows |
|------|-------|------|
| G# | G1 resolved host reaches OK response and relay | T1, T2, T4, T12, T16, T17, T18 |
| G# | G2 DNS and cleanup failures map to response and cleanup | T3, T5, T6, T7, T8, T9, T11, T13, T14, T15, T18 |
| G# | G3 resolved-address filter gate | T4, T5, T11, T12 |
| G# | G4 runtime shared resolver | T10, T15 |
| S# | S1 resolved-address and port SSRF filter | T4, T5, T11 |
| S# | S2 embedded-NUL host truncation | T6 |
| S# | S3 local name rewrite suppression | T7 |
| State | `S_HANDSHAKE` request decoded starts resolving | T1, T2, T3, T4, T5, T6, T7, T8, T11, T12, T13, T16, T17, T18 |
| State | `S_RESOLVING` DNS OK selects address | T1, T2, T4, T5, T11, T12, T13, T16, T17, T18 |
| State | `S_RESOLVING` DNS OK with zero addresses writes no-address response | T3 |
| State | `S_RESOLVING` DNS or start failure writes error response | T3, T6, T7, T13 |
| State | `S_RESOLVING` destroyed before completion | T8 |
| State | `S_RESOLVING` destroyed during completion | T11 |
| State | `S_WRITING_OK_RESP` staged CONNECT_RESP drains after DNS OK | T16 |
| State | Runtime stream creation with shared resolver | T10, T15 |
| Completion mode | Happy DNS fixture completion | T1, T4, T11, T12, T13, T16, T17 |
| Completion mode | Happy numeric literal immediate resolver completion | T2, T18 |
| Completion mode | Happy DNS completion followed by staged CONNECT_RESP write/resume | T16 |
| Completion mode | Successful DNS completion with `addr_count == 0` | T3 |
| Completion mode | Error DNS completion | T3, T7 |
| Completion mode | Error synchronous resolver-start validation rejection | T6 |
| Completion mode | Error synchronous resolver-start c-ares setup rejection | T13 |
| Decoder branch | DNS A answer selected | T1, T4, T5, T11, T13, T16, T17 |
| Decoder branch | DNS AAAA answer selected | T12 |
| Decoder branch | Numeric literal result selected | T2, T18 |
| Decoder branch | DNS OK with zero usable addresses | T3 |
| Decoder branch | NXDOMAIN and timeout errors | T3, T7 |
| Benign-vs-fatal split | No filter or filter permits an address | T1, T4, T12, T16, T17, T18 |
| Benign-vs-fatal split | Filter denies one address then permits later address | T4 |
| Benign-vs-fatal split | Filter denies all addresses | T5 |
| Benign-vs-fatal split | Filter destroys the session during DNS completion | T11 |
| Dial setup | Terminal post-resolution `odin_dial_start` synchronous failure | T13 |
| Dial setup | First `odin_dial_start` synchronous failure continues to later address | T17 |
| Constructor / factory precondition | Resolver-aware and existing private constructors reject NULL required inputs; same-loop resolver is an unchecked precondition | T9 |
| Constructor / factory precondition | Private resolver create and destroy lifecycle | T9 |
| Constructor / factory integrated path | Existing private constructors drive CONNECT through private resolver | T18 |
| Constructor / factory rollback | Private resolver create failure returns before helper setup | T14 |
| Constructor / factory rollback | Private resolver created and destroyed after factory, READ-interest, or connect-session helper failure | T14 |
| Runtime rollback | Runtime resolver-create failure returns before UDP setup | T15 |
| Runtime rollback | Runtime resolver created and destroyed after UDP or ALPN setup failure | T15 |
| Callback-safe lifecycle hand-off | Destroy while DNS query is pending | T8 |
| Callback-safe lifecycle hand-off | Destroy from inside `dial_filter` during DNS completion | T11 |

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | DNS name resolves and relays | Local DNS fixture answers `target.test` with A `127.0.0.1`; injected resolver uses the fixture; upstream listener owns the CONNECT port; downstream sends CONNECT_REQ for `target.test` plus one post-response payload | Server reads OK RESP `{0x01, 0x02, 0x00, 0x00}`; upstream accepts exactly one connection from the resolved address and receives the downstream payload; upstream-to-downstream bytes relay back; `on_close(ss, 0)` fires once; resolver query liveness returns to zero | G1 | unit |
| T2 | Numeric literal still uses resolver path | Injected resolver with DNS fixture configured to fail the row if it receives a packet; c-ares observation starts at zero; upstream listener on `127.0.0.1`; downstream sends CONNECT_REQ host `127.0.0.1` and that port | Start is non-reentrant, later OK RESP bytes equal `{0x01, 0x02, 0x00, 0x00}`; `odin_dns_resolver_test_cares_observation` reports `getaddrinfo_calls == 1` and `last_ai_family == AF_UNSPEC`; the fixture records no DNS packet; relay reaches `on_close(ss, 0)`; this rejects the old `inet_pton(AF_INET, ...)` bypass because that path would have zero resolver observations | G1 | unit |
| T3 | DNS and post-resolution dial errors map to existing response codes | Cases use fresh sessions: DNS fixture returns NXDOMAIN; push `ODIN_DNS_TEST_CARES_RESULT_EMPTY_SUCCESS` before sending CONNECT_REQ so the integrated server-session path reaches `dns_on_done` with `ODIN_DNS_OK` and `addr_count == 0`; no-answer fixture uses resolver config `{timeout_ms = 50, tries = 1}`; fixture returns `127.0.0.1` for a closed local port | NXDOMAIN and OK-with-zero-addresses each write RESP code `0x0002` and `on_close` err `EHOSTUNREACH`; the zero-address case records one `getaddrinfo_calls` delta, calls no filter, starts no dial, and returns DNS result liveness to baseline; timeout writes `0x0003` and err `ETIMEDOUT` before the watchdog fires; closed port writes `0x0001` and err `ECONNREFUSED`; each case leaves DNS query/watch/timer/channel/result liveness at baseline and no live dial | G2 | unit |
| T4 | Filter skips denied first answer and dials later permitted answer | DNS fixture returns two A records in order: denied `192.0.2.7`, then permitted `127.0.0.1`; filter records every sockaddr and returns `EACCES` for the first only; upstream listener uses the CONNECT port | Filter is called first with `192.0.2.7` and second with `127.0.0.1`; only the permitted address is dialed; OK RESP and relay complete; an implementation that fails on the first denied answer or dials before filtering fails | G1, G3, S1 | unit |
| T5 | All resolved addresses denied before connect | DNS fixture returns A `127.0.0.1`; downstream CONNECT uses the upstream listener's port; filter records the sockaddr and returns `EACCES` only when it observes `AF_INET`, `127.0.0.1`, and the exact CONNECT port; upstream listener remains non-accepted | Filter observes `127.0.0.1:CONNECT_port` before any dial; RESP code is `0x0004`, `on_close` err is `EACCES`, and `accept(listener_fd, NULL, NULL)` remains `-1/EAGAIN`, proving the IPv4 address/port tuple was denied before any outbound connect | G2, G3, S1 | unit |
| T6 | Embedded NUL host is rejected before DNS | Downstream writes CONNECT_REQ host bytes `bad\0name`; injected resolver has c-ares observation enabled; upstream listener is available but should not be contacted | Session writes RESP code `0x0004`, fires `on_close(ss, EINVAL)`, records zero `ares_getaddrinfo` calls, and the upstream listener sees no accept; this rejects an implementation that C-string-copies the host and queries `bad` | G2, S2 | unit |
| T7 | Local name rewriting does not change peer host | Row child sets `HOSTALIASES` mapping `aliaspeer` to `rewritten.test`; DNS fixture records questions, does not answer, and is used through resolver config `{timeout_ms = 50, tries = 1}`; downstream sends CONNECT_REQ host `aliaspeer`; c-ares observation starts at zero | Fixture observes at least one DNS question and every recorded question name is exactly `aliaspeer`; `odin_dns_resolver_test_cares_observation` reports `last_init_options_optmask` includes `ARES_OPT_FLAGS` and `ARES_OPT_LOOKUPS`, `last_init_options_lookups == "b"`, `last_init_options_flags` includes both `ARES_FLAG_NOALIASES` and `ARES_FLAG_NOSEARCH`, and `last_ai_flags` includes `ARES_AI_NOSORT`; session writes timeout RESP code `0x0003` and `on_close(ss, ETIMEDOUT)` before the watchdog fires; no query for `rewritten.test` is observed. This rejects an integrated path that still permits hosts-file or alias/search rewriting even when the fixture question remains `aliaspeer` | G2, S3 | unit |
| T8 | Destroy while resolving aborts query without callback | DNS fixture records but does not answer `slow.test`; after the session reaches `S_RESOLVING`, an owner-thread timer calls `odin_server_session_destroy(ss)` and stops the loop; downstream peer has SIGPIPE suppressed for the close probe | `on_close` is never called; the downstream fd owned by the session is closed; before explicit resolver destroy, the external resolver remains live with zero queries, watches, timers, channels, and results; after resolver destroy the live DNS resource counts (`resolvers`, `queries`, `watches`, `timers`, `cares_channels`, `cares_results`) return to baseline; ASan reports no use-after-free | G2 | unit |
| T9 | Constructor validation and private resolver lifecycle | Record DNS resolver liveness baseline; create the positive resolver on the same loop as the session; parameterize invalid-slot cases for `odin_server_session_create_with_resolver` over NULL `loop`, `resolver`, `on_close`, and `out` with caller-owned fds; parameterize `odin_server_session_create_with_transport_and_resolver` over NULL `loop`, `create_downstream`, `resolver`, `on_close`, and `out` with a factory counter; parameterize existing `odin_server_session_create` over NULL `loop`, `on_close`, and `out` with caller-owned fds; parameterize existing `odin_server_session_create_with_transport` over NULL `loop`, `create_downstream`, `on_close`, and `out` with a factory counter; also run existing `odin_server_session_create` with a private resolver followed by immediate destroy | Each invalid-slot case returns `-1/EINVAL`; cases with non-NULL `out` leave the sentinel unchanged; fd-backed invalid cases leave the caller fd open; transport invalid cases never call the factory; existing-private invalid cases record no resolver create/destroy delta. The row does not create a mismatched-loop resolver because §3.2.1 declares same-loop as an unchecked precondition, not an enforced validation branch. The private constructor yields `resolver_create_calls == baseline.resolver_create_calls + 1` and live `resolvers == baseline.resolvers + 1` after create, then yields `resolver_destroy_calls == baseline.resolver_destroy_calls + 1` and returns live DNS resource counts to baseline after destroy, while `on_close` remains zero | G2 | unit |
| T10 | QUIC runtime reuses one resolver for stream sessions | Create QUIC server runtime, record DNS resolver liveness and server-session live count, synthesize one accepted bidirectional stream through the existing runtime test harness, then destroy the runtime | Runtime create adds exactly one live resolver and yields `resolver_create_calls == baseline.resolver_create_calls + 1`; stream session creation increases the server-session live count by one without increasing resolver count; runtime destroy releases sessions first and then the resolver, yields `resolver_destroy_calls == baseline.resolver_destroy_calls + 1`, and returns live DNS resource counts plus server-session live count to baseline | G4 | unit |
| T11 | Destroy from DNS-completion filter stops selection | DNS fixture returns two A records in order: `127.0.0.1`, then another reachable listener address; filter records call count, calls `odin_server_session_destroy(ss)` during the first filter callback, and returns `EACCES`; upstream listeners remain nonblocking; downstream peer has SIGPIPE suppressed | Filter is called exactly once; no second address is offered; no `odin_dial_start` runs, no upstream listener accepts, no CONNECT response is written, and `on_close` remains zero; DNS query liveness returns to zero and ASan reports no use-after-free, proving `dns_on_done` held the `ss_enter` frame and returned after `destroy_pending` | G2, G3, S1 | unit |
| T12 | AAAA result reaches filter and IPv6 dial path | Preflight creates and listens on an `AF_INET6` TCP socket bound to `::1:0`, skipping only if IPv6 loopback bind or listen is unsupported; DNS fixture answers `v6.test` with AAAA `::1`; filter records `sa_family`, address bytes, and port, then permits; downstream sends CONNECT_REQ for `v6.test` and the listener port | `odin_dns_resolver_test_cares_observation` reports `last_ai_family == AF_UNSPEC`; filter receives exactly one `AF_INET6` sockaddr for `::1` with the CONNECT port before any dial; the IPv6 listener accepts exactly one connection; OK RESP and relay complete with `on_close(ss, 0)` | G1, G3 | unit |
| T13 | Resolver-start setup and post-resolution dial-start failures map cleanly | Cases use fresh sessions: push `ODIN_DNS_TEST_CARES_INIT_OPTIONS` with `ARES_ENOMEM` before resolving `setup.test`; DNS fixture returns A `127.0.0.1` and `odin_server_session_test_fail_next_dial(ss, EMFILE)` is armed before sending the CONNECT request | c-ares setup failure writes RESP code `0x0004`, fires `on_close(ss, ENOMEM)`, leaves `*dns_query` unobservably absent through zero query/watch/timer/channel liveness, and contacts no upstream; post-resolution dial-start failure first records `getaddrinfo_calls == 1` and `last_ai_family == AF_UNSPEC`, then writes RESP code `0x0004`, fires `on_close(ss, EMFILE)`, leaves DNS liveness at baseline, and contacts no upstream | G2 | unit |
| T14 | Private resolver rolls back across constructor failure branches | Reset DNS and server-session liveness in each forked subcase and preset `odin_server_session_t *out = (odin_server_session_t *)0xDEADBEEF`; subcase A runs before any resolver create, creates a caller-owned socketpair, pushes `ODIN_DNS_TEST_CARES_LIBRARY_INIT` with `ARES_ENOMEM`, and calls existing `odin_server_session_create(loop, pb, on_close, ud, &out)`; subcase B calls existing `odin_server_session_create_with_transport` with a factory returning `-1/EIO`; subcase C uses a factory returning a test-local fake downstream transport whose READ-interest setup returns `-1/EEXIST`; subcase D creates a caller-owned socketpair, arms `odin_connect_session_test_fail_next_create_server(ENOMEM)`, and calls existing `odin_server_session_create(loop, pb, on_close, ud, &out)` | Subcase A returns `-1/ENOMEM`, leaves `out` at the sentinel, leaves caller fd `pb` open (`fcntl(pb, F_GETFD) >= 0`), records `ares_library_init_calls == 1`, records no resolver create/destroy delta, and records no factory/connect-session/live server-session side effects. Subcases B-D return the injected errno, leave `out` at the sentinel, do not fire `on_close`, leave server-session live count unchanged, and show `post.resolver_create_calls == baseline.resolver_create_calls + 1`, `post.resolver_destroy_calls == baseline.resolver_destroy_calls + 1`, and live `resolvers`, `queries`, `watches`, `timers`, `cares_channels`, and `cares_results` all back at baseline. B records one factory call with no transport to destroy; C records the fake downstream transport destroyed once; D leaves caller fd `pb` open (`fcntl(pb, F_GETFD) >= 0`). A no-resolver constructor fails B-D's create/destroy deltas even though final live counts are baseline | G2 | unit |
| T15 | Runtime resolver rolls back across create and setup failure branches | Each forked subcase resets DNS liveness, records `odin_dns_resolver_test_liveness(&baseline)`, and presets runtime `*out` sentinels before calling `odin_xqc_server_runtime_create` through the existing `OdinXqcServerRuntimeTest` harness: subcase A runs before any resolver create and pushes `ODIN_DNS_TEST_CARES_LIBRARY_INIT` with `ARES_ENOMEM`; subcase B configures fake UDP creation to fail during engine creation; subcase C configures fake `engine_register_alpn` to return failure after UDP create succeeds | A returns `-1/ENOMEM`, leaves `*out` untouched, records `ares_library_init_calls == 1`, records no resolver create/destroy delta, and records zero UDP create, ALPN register, and server-session creation calls. B and C each report `post.resolver_create_calls == baseline.resolver_create_calls + 1`, `post.resolver_destroy_calls == baseline.resolver_destroy_calls + 1`, and all live DNS resource counts back at baseline. B returns the original UDP errno, leaves `*out` untouched, and records zero ALPN register calls; C returns `-1/EIO`, leaves `*out` untouched, and records UDP destroy/engine destroy cleanup; no server session is created in any subcase. A runtime that creates UDP before the resolver fails A, and a runtime that never allocates a resolver fails B/C create/destroy deltas even if final live counts are baseline | G2, G4 | unit |
| T16 | Staged OK CONNECT_RESP write resumes after DNS | DNS fixture answers `staged.test` with A `127.0.0.1`; injected resolver uses the fixture; upstream listener owns the CONNECT port; session is created with `odin_server_session_create_with_transport_and_resolver` and a test-local scripted downstream `odin_transport_t` whose read returns the CONNECT_REQ, whose response-write script returns `ODIN_TRANSPORT_OK` with `*out_n = 2`, then `ODIN_TRANSPORT_AGAIN`, then on a test-fired WRITE readiness returns `ODIN_TRANSPORT_OK` with `*out_n = 2` | Before the test-fired WRITE readiness, the scripted transport has recorded only the first two RESP bytes, `odin_server_session_test_state(ss)` reports `ODIN_SERVER_SESSION_TEST_STATE_WRITING_OK_RESP`, and no relay payload is accepted by the upstream peer; after WRITE readiness, the recorded RESP bytes equal `{0x01, 0x02, 0x00, 0x00}`, `odin_server_session_test_state(ss)` reaches `ODIN_SERVER_SESSION_TEST_STATE_RELAY`, downstream-to-upstream payload relays, and `on_close(ss, 0)` fires once. This rejects an implementation that starts DNS and dials but never resumes `resp_write_off` after `ODIN_TRANSPORT_AGAIN` | G1 | unit |
| T17 | First dial-start setup failure continues to later address | DNS fixture returns two A records in order: `192.0.2.77`, then reachable `127.0.0.1`; no dial filter is installed; upstream listener owns the CONNECT port; after session create and before sending CONNECT_REQ, arm `odin_server_session_test_fail_next_dial(ss, EMFILE)` | The armed failure is consumed for the first address but is not terminal; the second address is dialed, upstream accepts exactly one connection, OK RESP and relay complete, and `on_close(ss, 0)` fires once. A buggy implementation that reports the first `EMFILE` immediately writes RESP code `0x0004`, never connects to the listener, and fails this row | G1 | unit |
| T18 | Existing private constructors resolve numeric CONNECT through DNS path | Cases use fresh sessions plus DNS liveness and c-ares-observation baselines: existing `odin_server_session_create(loop, pb, ...)` with a socketpair downstream; existing `odin_server_session_create_with_transport(loop, factory, ...)` where the test-local factory wraps the same kind of socketpair end with `odin_fd_transport_create`; each downstream sends CONNECT_REQ host `127.0.0.1` to a loopback listener | Each case shows `resolver_create_calls == live0.resolver_create_calls + 1`, `getaddrinfo_calls == obs0.getaddrinfo_calls + 1`, `last_ai_family == AF_UNSPEC`, OK RESP, upstream accept, bidirectional relay, and after destroy or terminal close `resolver_destroy_calls == live0.resolver_destroy_calls + 1` with live DNS resource counts back at `live0`. A constructor that allocates and destroys a private resolver but still uses the old `inet_pton` path fails the `getaddrinfo_calls` delta | G1, G2 | unit |

## 6. Implementation Plan

- **P1. Land DNS-resolution API skeleton and red-verifiable tests behind a default skip.**
  - **Scope:** add resolver-aware server-session constructor declarations and linkable stubs; add `S_RESOLVING` test-state exposure plus DNS liveness assertions under `ODIN_SERVER_SESSION_TESTING`; extend `odin_dns_resolver_test_liveness_t` with `resolver_create_calls` and `resolver_destroy_calls`, add `#define ODIN_DNS_TEST_CARES_RESULT_EMPTY_SUCCESS 9`, and wire the new op in `dns_ares_getaddrinfo` under `ODIN_DNS_RESOLVER_TESTING`; add `OdinServerDnsTest` rows T1-T9, T11-T14, and T16-T18 plus `OdinXqcServerRuntimeDnsTest` rows T10 and T15 gated by `ODIN_SERVER_DNS_RED=1`; add `:odin_dns_resolver_testing_config` to the `odin_unittests` target configs so these rows can include `odin/testing/dns_resolver_internal_test.h` and call `odin_dns_resolver_test_liveness`, `odin_dns_resolver_test_cares_observation`, and `odin_dns_resolver_test_push_cares_step` target-wide; wire `//odin:odin_server_session` to depend on `:odin_dns_resolver` for the new declarations without implementing DNS behavior. The red-verification mode executes assertions; the default suite skips these rows.
  - **Depends on:** None.
  - **Done when:** Host-runnable: `./tool/gn gen out/server_dns_mac --args='target_os="mac"'` and `./tool/ninja -C out/server_dns_mac odin_unittests tests` build `out/server_dns_mac/odin_unittests` and `out/server_dns_mac/odin`; `ODIN_SERVER_DNS_RED=1 out/server_dns_mac/odin_unittests --gtest_filter='OdinServerDnsTest.*:OdinXqcServerRuntimeDnsTest.*'` executes T1-T18 and reports them red against the skeleton, with T3's zero-address subcase proving the new `ODIN_DNS_TEST_CARES_RESULT_EMPTY_SUCCESS` op is consumed only when the server session actually enters resolver-backed CONNECT handling, T14/T15/T18 specifically failing the `resolver_create_calls` and `resolver_destroy_calls` +1 assertions if no private/runtime resolver is created, T16 failing because no staged DNS-completion response write can resume, and T17 failing because no second address is dialed after the armed first-start failure, while `out/server_dns_mac/odin_unittests --gtest_brief=1` reports T1-T18 skipped and exits zero with pre-existing rows green. Cross-compile-only: `./tool/gn gen out/server_dns_linux_x64 --args='target_os="linux" target_cpu="x64"'` plus `./tool/ninja -C out/server_dns_linux_x64 odin_unittests odin_cli_artifacts` builds but does not run the Linux binary; the same build-only rule applies to `out/server_dns_mac_arm64`, `out/server_dns_ios_sim_arm64`, and `out/server_dns_ios_device_arm64` with the corresponding target args. T1-T18 execute only in the macOS host `odin_unittests` binary in this RFC.

- **P2. Implement server-session DNS selection and turn only server-session rows green.**
  - **Scope:** replace the `inet_pton` branch with `odin_dns_resolve_start`; add DNS query ownership, `S_RESOLVING`, callback cleanup with `ss_enter` / `ss_leave`, resolved-address filtering, selected-address dial start, DNS error routing through `handle_dial_result`, destroy-in-resolving cleanup, private resolver ownership, resolver-aware and existing private-constructor validation, and the post-resolution `odin_server_session_test_fail_next_dial` call site; remove the skip gate from T1-T9, T11-T14, and T16-T18 while leaving runtime rows T10 and T15 gated.
  - **Depends on:** P1.
  - **Done when:** Host-runnable: `out/server_dns_mac/odin_unittests --gtest_filter='OdinServerDnsTest.*'` passes T1-T9, T11-T14, and T16-T18 un-gated, including T1/T2 OK relay with numeric-literal resolver observation, T3 exact response-code mappings with bounded timeout config plus the `ODIN_DNS_OK`/`addr_count == 0` no-address case driven by `ODIN_DNS_TEST_CARES_RESULT_EMPTY_SUCCESS` through `dns_ares_getaddrinfo`, `copy_results`, `run_finalizer`, and the server-session `dns_on_done` callback, T4/T5 filter-before-connect assertions including T5's exact IPv4 CONNECT-port denial, T6 embedded-NUL rejection, T7 alias suppression plus DNS-only c-ares option assertions with bounded timeout config, T8 resolving destroy, T9 resolver-aware and existing private-constructor NULL input validation plus same-loop positive construction and private lifecycle checks, T11 destroy-from-filter lifecycle safety, T12 IPv6-loopback behavior or preflight skip, T13 setup-failure mappings, T14 private resolver create failure before helper setup plus private resolver rollback after factory, READ-interest, and connect-session failures with `post.resolver_create_calls == baseline.resolver_create_calls + 1`, `post.resolver_destroy_calls == baseline.resolver_destroy_calls + 1`, and final DNS live counts at baseline for the post-create failure branches, T16 staged CONNECT_RESP write/resume after DNS completion, T17 continue-after-first-start-failure to a second address, and T18 numeric CONNECT through both existing private constructors with `getaddrinfo_calls` deltas; `out/server_dns_mac/odin_unittests --gtest_brief=1` stays green with T10 and T15 still skipped; `./tool/gn gen out/server_dns_mac_asan --args='target_os="mac" is_asan=true'`, `./tool/ninja -C out/server_dns_mac_asan odin_unittests`, and `out/server_dns_mac_asan/odin_unittests --gtest_filter='OdinServerDnsTest.*'` pass without AddressSanitizer reports, including T8, T11, and T16. Cross-compile-only: the P1 Linux, alternate macOS arch, iOS simulator, and iOS device binaries still build but are not executed; their event-loop backend and c-ares sysconfig branches are compile-verified only.

- **P3. Share the runtime resolver and turn runtime rows green.**
  - **Scope:** add `odin_dns_resolver_t *resolver` to `odin_xqc_server_runtime_t`; create it during runtime construction before UDP setup; pass it to `odin_server_session_create_with_transport_and_resolver`; destroy it after stream sessions and on UDP/ALPN setup rollback; remove the T10 and T15 gates.
  - **Depends on:** P2.
  - **Done when:** Host-runnable: `out/server_dns_mac/odin_unittests --gtest_filter='OdinXqcServerRuntimeDnsTest.*'` passes T10 and T15 un-gated, including T15 resolver-create failure returning before UDP setup, and resolver rollback with `post.resolver_create_calls == baseline.resolver_create_calls + 1`, `post.resolver_destroy_calls == baseline.resolver_destroy_calls + 1`, and final DNS live counts at baseline for both UDP-create and ALPN-register failures; the full `out/server_dns_mac/odin_unittests --gtest_brief=1` passes with T1-T18 all active; production `out/server_dns_mac/odin` links server DNS resolution and exports no `odin_dns_resolver_test_*` or `odin_server_session_test_*` symbols. Cross-compile-only: the P1 Linux, alternate macOS arch, iOS simulator, and iOS device build commands still build `odin_unittests` and `odin` but are not executed.
