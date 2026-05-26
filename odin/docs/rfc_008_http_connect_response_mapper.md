# RFC-008: HTTP CONNECT Response Status Mapper

**Version:** 1.0  
**Author:** Chason Tang  
**Date:** 2026-05-26  
**Status:** Implemented

## 1. Summary

Replace the two exported response constants in `odin/http_connect.h` (`kOdinHttpConnectEstablished` for `200 Connection Established`, `kOdinHttpBadGateway` for `502 Bad Gateway`) with a single mapping function `odin_http_response_for_status(odin_http_status_t status)` returning `odin_http_response_t {const char *bytes; size_t len;}`. The function maps each terminal `odin_http_status_t` value to a fixed HTTP/1.1 response (status line, optional header lines, `CRLFCRLF`): `ODIN_HTTP_OK` → `200 Connection Established`; `ODIN_HTTP_ERR_BAD_METHOD` → `405 Method Not Allowed` with `Allow: CONNECT` (RFC 9110 §15.5.6); `ODIN_HTTP_ERR_BAD_REQUEST_TARGET` / `ODIN_HTTP_ERR_HOST_LEN_INVALID` / `ODIN_HTTP_ERR_PORT_INVALID` → `400 Bad Request`; `ODIN_HTTP_ERR_BAD_VERSION` → `505 HTTP Version Not Supported`; `ODIN_HTTP_ERR_REQUEST_TOO_LARGE` → `414 URI Too Long`. The 502 byte sequence is removed from the public surface. Passing `ODIN_HTTP_NEED_MORE` triggers a debug `assert` and falls back to the 400 response in release builds.

## 2. Motivation

`odin/http_connect.c:12-18` exposes exactly two response strings — `kOdinHttpConnectEstablished` (200) and `kOdinHttpBadGateway` (502) — paired with their lengths. The parser declared at `odin/http_connect.h:60-62` returns one of the eight `odin_http_status_t` enumerators defined at `odin/http_connect.h:43-52`: one `OK`, one `NEED_MORE`, and six terminal errors. With only two response constants, every caller has to either send 502 for every error or grow its own per-error mapping; both shapes lose precision and duplicate the dispatch. `502 Bad Gateway` is additionally misleading: RFC 9110 §15.6.3 defines it as the gateway receiving an invalid response from an upstream server, but `odin_http_parse_connect` only inspects local request bytes and has not contacted any upstream when it returns — `REQUEST_TOO_LARGE`, `BAD_VERSION`, `BAD_METHOD`, `BAD_REQUEST_TARGET`, `HOST_LEN_INVALID`, and `PORT_INVALID` are all client-side request defects. Centralizing the mapping in one function gives callers a single call site for "send the response for this status," matches HTTP semantics per error kind, and removes the inapplicable 502 from the public surface. No data available at this time.

## 3. Goals and Non-Goals

- **G1.** Expose `odin_http_response_t {const char *bytes; size_t len;}` and `odin_http_response_for_status(odin_http_status_t status)` from `odin/http_connect.h` such that for every `odin_http_status_t` enumerator — and any integer value cast to `odin_http_status_t` outside the defined enumerators — the function returns a non-NULL `bytes` pointer into program-lifetime static storage and a positive `len` whose first `len` bytes form a complete HTTP/1.1 response (status line plus zero or more header lines, terminated by `\r\n\r\n`); terminal statuses use their mapped response from G2, while `ODIN_HTTP_NEED_MORE` and out-of-enum integers fall back to the 400 response.
- **G2.** The mapping is `ODIN_HTTP_OK` → `"HTTP/1.1 200 Connection Established\r\n\r\n"`; `ODIN_HTTP_ERR_BAD_METHOD` → `"HTTP/1.1 405 Method Not Allowed\r\nAllow: CONNECT\r\n\r\n"`; `ODIN_HTTP_ERR_BAD_REQUEST_TARGET` / `ODIN_HTTP_ERR_HOST_LEN_INVALID` / `ODIN_HTTP_ERR_PORT_INVALID` → `"HTTP/1.1 400 Bad Request\r\n\r\n"`; `ODIN_HTTP_ERR_BAD_VERSION` → `"HTTP/1.1 505 HTTP Version Not Supported\r\n\r\n"`; `ODIN_HTTP_ERR_REQUEST_TOO_LARGE` → `"HTTP/1.1 414 URI Too Long\r\n\r\n"`.
- **G3.** Remove the `extern` exports `kOdinHttpConnectEstablished`, `kOdinHttpConnectEstablishedLen`, `kOdinHttpBadGateway`, and `kOdinHttpBadGatewayLen` from `odin/http_connect.h` and their definitions from `odin/http_connect.c`; no public symbol in `odin/http_connect.h` returns the 502 byte sequence after this RFC.

**Non-Goals:**

- `ODIN_HTTP_NEED_MORE` response mapping — this status means "buffer more input," not "send a response"; passing it to the mapping function violates the precondition documented in §4.2.1.
- Upstream-failure response codes (502, 504, etc.) — the parser only sees client request bytes; upstream-transport errors belong to a future I/O layer and are out of scope for this mapping.
- Dynamic Reason-Phrase customization, i18n, or response-header injection — only fixed compile-time bytes ship; per-deployment Reason-Phrase tuning is not a use case raised by any current caller.

## 4. Design

### 4.1 Overview

This RFC modifies `odin/http_connect.h` and `odin/http_connect.c` in place — no new file, no new GN target, no `odin/BUILD.gn` edit because `source_set("odin")` at `odin/BUILD.gn:24-37` already lists both files. `odin/http_connect.h` removes the four `extern` declarations at `odin/http_connect.h:67-71` together with the explanatory doc-comment block at `odin/http_connect.h:64-66`, and adds one struct typedef plus one function declaration in the same `extern "C"` block. `odin/http_connect.c` removes the four constant definitions at `odin/http_connect.c:12-18` and adds five file-static response strings plus the mapping function body. `odin/http_connect_unittests.cpp` appends one test case per §7 row; no other test file changes. The mapping is pure: input is a single `odin_http_status_t` by value, output is a struct of pointer-plus-length into static storage, with no allocation, I/O, or global mutable state. The four removed symbols have no callers anywhere in the Monorepo (grep over the repo at the time of this RFC yields only the definitions in `odin/http_connect.c:12-18` and the declarations in `odin/http_connect.h:67-71`).

N/A — textual description above is sufficient.

### 4.2 Detailed Design

#### 4.2.1 Status-to-Response Mapping API and Behavior

**Contract surface.**

```c
/* odin/http_connect.h — additions */
typedef struct odin_http_response_t {
  const char *bytes;
  size_t len;
} odin_http_response_t;

odin_http_response_t odin_http_response_for_status(odin_http_status_t status);
```

**Unstated contract.** `bytes` points to static storage with program-lifetime extent; callers must not free or write through it, and must use `len` (not `strlen`) to size the write since the slice is not NUL-terminated within `[0, len)`. Every returned slice is a complete HTTP/1.1 response ending in `\r\n\r\n`, so message framing for a CONNECT response (no body) is complete on a single write; the 405 mapping additionally carries an `Allow: CONNECT` header line — required by RFC 9110 §15.5.6 because the parser at `odin/http_connect.c:64-66` accepts exactly one method — while every other mapping is a bare status line plus the terminator. For every value in `odin_http_status_t` (`odin/http_connect.h:43-52`) and every integer cast to `odin_http_status_t` outside the defined enumerators, `bytes` is non-NULL and `len > 0` (the default arm guarantees this for `ODIN_HTTP_NEED_MORE` too). Both `ODIN_HTTP_NEED_MORE` (a non-terminal parser status meaning "buffer more input") and out-of-enum integers violate the precondition. Debug builds fire `assert(0 && "odin_http_response_for_status: non-terminal or unknown status")` for either input; release builds (where `assert` is a no-op) fall back to the 400 response for either input, so the caller never writes a NULL pointer to a socket. Pure function: no allocation, no I/O, no global state; thread-safe (read-only static storage, input passed by value).

**Mechanism.**

```
switch status:
  case ODIN_HTTP_OK:                   return {kRespOk,  sizeof(kRespOk)-1}
  case ODIN_HTTP_ERR_BAD_METHOD:       return {kResp405, sizeof(kResp405)-1}
  case ODIN_HTTP_ERR_BAD_REQUEST_TARGET,
       ODIN_HTTP_ERR_HOST_LEN_INVALID,
       ODIN_HTTP_ERR_PORT_INVALID:     return {kResp400, sizeof(kResp400)-1}
  case ODIN_HTTP_ERR_BAD_VERSION:      return {kResp505, sizeof(kResp505)-1}
  case ODIN_HTTP_ERR_REQUEST_TOO_LARGE: return {kResp414, sizeof(kResp414)-1}
  case ODIN_HTTP_NEED_MORE:
  default:
    assert(0 && "odin_http_response_for_status: non-terminal or unknown status")
    return {kResp400, sizeof(kResp400)-1}
```

File-static `static const char` arrays in `odin/http_connect.c`:

```
kRespOk  = "HTTP/1.1 200 Connection Established\r\n\r\n"    (39 bytes)
kResp400 = "HTTP/1.1 400 Bad Request\r\n\r\n"               (28 bytes)
kResp405 = "HTTP/1.1 405 Method Not Allowed\r\nAllow: CONNECT\r\n\r\n" (51 bytes)
kResp414 = "HTTP/1.1 414 URI Too Long\r\n\r\n"              (29 bytes)
kResp505 = "HTTP/1.1 505 HTTP Version Not Supported\r\n\r\n" (43 bytes)
```

Satisfies: G1 via the typedef and declaration; G2 via the per-status switch arms whose slices match the documented status lines verbatim; G3 via the absence of any 502-mapped arm.

### 4.3 Design Rationale

- **Chosen:** Return `odin_http_response_t` by value.
- **Reason:** On the x86-64 SysV and aarch64 ABIs this Monorepo targets, a two-word `{const char *, size_t}` aggregate returns in registers, so by-value and out-pointer pairs cost the same at the machine level. By-value lets call sites write `odin_http_response_t r = odin_http_response_for_status(s); write(fd, r.bytes, r.len);` without a separately declared out-`size_t`, and because every status either maps or asserts there is no failure path to signal, so no `odin_http_status_t` (or similar) return value would carry information.
- **Ruled out:** Out-pointer pair `void odin_http_response_for_status(odin_http_status_t, const char **out_bytes, size_t *out_len)`. Two writes through caller pointers, one extra line at every call site, and the same register-level cost — no contract benefit since the function has nothing to signal beyond the bytes-plus-length pair.

- **Chosen:** `assert(0)` plus default `400 Bad Request` fallback for `ODIN_HTTP_NEED_MORE` and out-of-enum integers.
- **Reason:** `NEED_MORE` means "the parser wants more bytes," not "send the client a response"; treating it as a call-site bug surfaces the misuse in debug builds (where the GN local test runner runs) while still returning well-formed bytes in release so a hardened production caller never writes a NULL pointer to a socket. The same default arm absorbs integer casts (UB in principle, real in practice) without adding a separate failure return value to the contract surface.
- **Ruled out:** Return `{NULL, 0}` for non-terminal or unknown inputs. The caller would have to gate every `write()` on a NULL check, and a missed gate would write nothing on a path where the parser was supposed to send a 400 — silently dropping the client diagnostic. The assert-plus-fallback shape removes the gate without losing the debug-time signal.

- **Chosen:** `ODIN_HTTP_ERR_BAD_METHOD` → `405 Method Not Allowed` with `Allow: CONNECT`.
- **Reason:** 405 is the status RFC 9110 §15.5.6 reserves for method-rejection at a resource that supports other methods, and the same section makes the `Allow` header mandatory; the parser at `odin/http_connect.c:64-66` accepts exactly `CONNECT`, so the header value is unambiguous and the per-error precision §2 cites as the central motivation survives the bad-method case instead of collapsing into the generic 400 bucket.
- **Ruled out:** Map `ODIN_HTTP_ERR_BAD_METHOD` to `400 Bad Request` and drop `kResp405` entirely. Saves 51 bytes of static storage and one switch arm, but erases the only signal that distinguishes a wrong-verb request from a malformed request-target — the same precision loss that motivated retiring 502 in §2.

## 5. Backward Compatibility & Migration

- **Breaks:** `kOdinHttpConnectEstablished`, `kOdinHttpConnectEstablishedLen`, `kOdinHttpBadGateway`, and `kOdinHttpBadGatewayLen` are removed from `odin/http_connect.h` (§4.2.1 replaces them with the typedef and function).
- **Symptom on un-migrated caller:** `error: 'kOdinHttpConnectEstablished' undeclared (first use in this function)` at any `#include "odin/http_connect.h"` site that references the symbol (and the matching error for the other three names); link-time `undefined reference to 'kOdinHttpConnectEstablished'` for translation units that forward-declared the symbol locally. No such call site exists in the Monorepo at the time of this RFC — `git grep -F kOdinHttpConnectEstablished` and the same for `kOdinHttpBadGateway` return only the definitions in `odin/http_connect.c:12-18` and the declarations in `odin/http_connect.h:67-71`, both deleted by this RFC's diff.
- **Migration:** within this RFC's diff, no migration is required because no caller exists. For any future caller pulled from outside the Monorepo: replace `write(fd, kOdinHttpConnectEstablished, kOdinHttpConnectEstablishedLen)` with `const odin_http_response_t r = odin_http_response_for_status(ODIN_HTTP_OK); write(fd, r.bytes, r.len);`. The 502 byte sequence has no migration substitute because G3 forbids any public symbol from emitting it; a caller that needs a generic failure response calls `odin_http_response_for_status` with one of the 400-mapped statuses (e.g., `ODIN_HTTP_ERR_BAD_REQUEST_TARGET`) instead.

## 6. Security

Not applicable — `odin_http_response_for_status` takes one `odin_http_status_t` by value, returns pointers and lengths into program-lifetime static storage with no caller-supplied bytes ever entering the output, performs no allocation or I/O, and the five fixed response strings contain only ASCII status-line, optional header-line, and `\r\n\r\n` bytes pinned at compile time, so the mapping introduces no attacker-controlled write path or injection surface beyond what already existed for the `kOdinHttpConnectEstablished` constant the RFC removes.

## 7. Testing Strategy

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | `ODIN_HTTP_OK` returns exact `200 Connection Established` bytes | Call `odin_http_response_for_status(ODIN_HTTP_OK)` once | Returned `r.bytes` is non-NULL; `r.len == 39`; `memcmp(r.bytes, "HTTP/1.1 200 Connection Established\r\n\r\n", 39) == 0` | G1, G2, §5 | unit |
| T2 | The three 400-mapped errors share one byte sequence | Call for `ODIN_HTTP_ERR_BAD_REQUEST_TARGET`, `ODIN_HTTP_ERR_HOST_LEN_INVALID`, `ODIN_HTTP_ERR_PORT_INVALID` | All three `r.len == 28`; all three `memcmp(r.bytes, "HTTP/1.1 400 Bad Request\r\n\r\n", 28) == 0` | G2 | unit |
| T3 | Non-400 error mappings return their exact response bytes | Call for `ODIN_HTTP_ERR_BAD_METHOD`, `ODIN_HTTP_ERR_BAD_VERSION`, `ODIN_HTTP_ERR_REQUEST_TOO_LARGE` | Returned slices equal `"HTTP/1.1 405 Method Not Allowed\r\nAllow: CONNECT\r\n\r\n"` (`r.len == 51`), `"HTTP/1.1 505 HTTP Version Not Supported\r\n\r\n"` (`r.len == 43`), `"HTTP/1.1 414 URI Too Long\r\n\r\n"` (`r.len == 29`) respectively | G2 | unit |
| T4 | Every mapped response ends with CRLFCRLF and contains no embedded NUL | Iterate over `{ODIN_HTTP_OK, ODIN_HTTP_ERR_BAD_METHOD, ODIN_HTTP_ERR_BAD_REQUEST_TARGET, ODIN_HTTP_ERR_BAD_VERSION, ODIN_HTTP_ERR_HOST_LEN_INVALID, ODIN_HTTP_ERR_PORT_INVALID, ODIN_HTTP_ERR_REQUEST_TOO_LARGE}` and inspect each returned slice | For every status, `r.len >= 4`; `memcmp(&r.bytes[r.len-4], "\r\n\r\n", 4) == 0`; `memchr(r.bytes, '\0', r.len) == NULL` | G1 | unit |
| T5 | The 502 byte sequence is no longer reachable through any defined status | For every defined `odin_http_status_t` value enumerated in T4, search the returned `r.bytes[0..len)` for the substring `"502"` | No mapped status's response slice contains the substring `"502"` | G3 | unit |
| T6 | `ODIN_HTTP_NEED_MORE` and an out-of-enum integer cast both abort in debug and return the 400 fallback in release | For each input in `{ODIN_HTTP_NEED_MORE, (odin_http_status_t)999}`: call `EXPECT_DEBUG_DEATH(odin_http_response_for_status(input), "non-terminal")`; additionally, under `#ifdef NDEBUG`, call `odin_http_response_for_status(input)` once outside the death wrapper and inspect the returned slice | Debug builds: each death-test process aborts and stderr matches the regex `"non-terminal"`. Release builds: `EXPECT_DEBUG_DEATH` runs each statement once with no death assertion; each outside-the-wrapper call returns `r.len == 28` and `memcmp(r.bytes, "HTTP/1.1 400 Bad Request\r\n\r\n", 28) == 0` | G1 | unit |

## 8. Implementation Plan

- **P1. Land surface diff, file-static stub, removal of old constants, and red `T1`–`T6`.**
  - **Scope:** in `odin/http_connect.h`, delete the four `extern` declarations at `odin/http_connect.h:67-71` and the explanatory doc-comment block at `odin/http_connect.h:64-66`, and add the §4.2.1 contract surface — the `odin_http_response_t` typedef and the `odin_http_response_for_status` declaration — inside the existing `extern "C"` block immediately after the `odin_http_parse_connect` declaration; in `odin/http_connect.c`, delete the four constant definitions at `odin/http_connect.c:12-18` and add five file-static `static const char kRespOk[]`, `kResp400[]`, `kResp405[]`, `kResp414[]`, and `kResp505[]` arrays initialized to the literals listed in §4.2.1's Mechanism block, then append an `odin_http_response_for_status` stub whose body is `return (odin_http_response_t){NULL, 0};` so the test binary links; in `odin/http_connect_unittests.cpp`, append T1–T6 from §7 each as a separate `TEST(OdinHttpResponseTest, ...)` whose first statement is `GTEST_SKIP() << "pending RFC-008 P2";`. No `odin/BUILD.gn` edit.
  - **Depends on:** None.
  - **Done when:** `gn gen out/` resolves the unchanged `:odin` and `:odin_unittests` targets and `ninja -C out/ tests` builds clean; the `odin_http_response_t` typedef and `odin_http_response_for_status` declaration export from `odin/http_connect.h` (G1 staged); the four removed names — `kOdinHttpConnectEstablished`, `kOdinHttpConnectEstablishedLen`, `kOdinHttpBadGateway`, `kOdinHttpBadGatewayLen` — no longer appear in `odin/http_connect.h` or `odin/http_connect.c`, and `git grep -F kOdinHttpConnectEstablished -- 'odin/**.h' 'odin/**.c'` (and the same for the other three names) returns no hits (G3); T1–T6 are committed in `GTEST_SKIP`-wrapped (red) state and `out/odin_unittests --gtest_brief=1` reports all six as `SKIPPED` while the run exits zero.
- **P2. Implement the switch mapping and turn `T1`–`T6` green.**
  - **Scope:** replace the stub body in `odin/http_connect.c` with the switch pinned in §4.2.1's Mechanism block — the seven case arms (`ODIN_HTTP_OK`, `ODIN_HTTP_ERR_BAD_METHOD`, the three 400-mapped statuses falling through to a single return, `ODIN_HTTP_ERR_BAD_VERSION`, `ODIN_HTTP_ERR_REQUEST_TOO_LARGE`) plus the `ODIN_HTTP_NEED_MORE` / `default` arm that holds the `assert(0 && "odin_http_response_for_status: non-terminal or unknown status")` and the 400 fallback return; remove the `GTEST_SKIP() << "pending RFC-008 P2";` first statement from T1–T6 so the assertions run on every local test invocation.
  - **Depends on:** P1.
  - **Done when:** T1 passes un-skipped for the byte-exact `200 Connection Established` slice (G1, G2); T2 passes un-skipped for the shared byte-exact 400 slice across the three 400-mapped statuses (G2); T3 passes un-skipped for the 405 / 505 / 414 byte-exact mappings, including the `Allow: CONNECT` header in the 405 slice (G2); T4 passes un-skipped for the CRLFCRLF terminator and the absence of embedded NUL across all seven terminal statuses (G1); T5 passes un-skipped with zero matches of the `"502"` substring across every mapped status's slice (G3); T6's `EXPECT_DEBUG_DEATH` passes un-skipped for both `ODIN_HTTP_NEED_MORE` and `(odin_http_status_t)999` with the debug-build assertion message matching the `"non-terminal"` regex, and each release-build outside-wrapper call returns the 400 fallback bytes (G1); `tidy_odin.sh` exits clean on the modified `odin/http_connect.c` and `odin/http_connect_unittests.cpp`; after a clean `ninja -C out/ tests`, `out/odin_unittests --gtest_brief=1` reports zero `SKIPPED` and zero `FAILED` for the new `OdinHttpResponseTest` rows.
