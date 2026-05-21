# RFC-003: Odin HTTPS_PROXY CONNECT Request Parser

**Version:** 1.0  
**Author:** Chason Tang  
**Date:** 2026-05-20  
**Status:** Implemented

## 1. Summary

Add a pure byte-buffer parser for the HTTPS_PROXY CONNECT request under a new `odin/http_connect.{c,h}` module so the Client only does read socket → parse → call `odin_proto_encode_connect_req`. One exported function `odin_http_parse_connect(buf, n, *out_consumed, *out)` returns `OK` after a full `CONNECT host:port HTTP/1.x\r\n<headers>\r\n\r\n` arrives (writing the host offset/length and port into `*out` as a non-owning view of `buf`, and `*out_consumed` as the byte count to slide off the read buffer), `NEED_MORE` while the request-line or terminating `\r\n\r\n` has not yet arrived, and one of six `ERR_*` codes on protocol-level rejections. Header bytes are consumed but never inspected, so the leftover bytes after the parse are the post-handshake byte-forwarding payload alone. New rows extend `odin_unittests`.

## 2. Motivation

After RFC-001 (per-stream control-frame codec) and RFC-002 (single-binary CLI skeleton), the Client still has no way to interpret the bytes its local socket hands it. The local protocol the Client must speak is HTTPS_PROXY's request line `CONNECT host:port HTTP/1.1\r\n` followed by zero or more header fields and a terminating `\r\n\r\n` — defined in RFC 9110 §9.3.6 and RFC 9112 §3. Without a dedicated parser, the Client would either inline an ad-hoc state machine (which the follow-up tests and any second caller would have to re-derive) or treat the entire socket stream opaquely (which forwards the HTTP headers and any pipelined body bytes through QUIC as if they were the tunneled payload — every CONNECT_REQ followed by leaked plaintext header bytes is a corrupted tunnel). Pinning the parser as a stand-alone module hands the Client a three-step transport loop (read → parse → `odin_proto_encode_connect_req`), pins the request-line grammar and header-consumption contract in one file, and lets the parser be exhaustively tested below the socket layer. No data available at this time.

## 3. Goals and Non-Goals

- **G1.** Pin the accepted CONNECT request grammar verbatim in §4.2.1: request-line is `CONNECT` SP `host:port` SP (`HTTP/1.0` | `HTTP/1.1`) CRLF; separators are single ASCII space (`0x20`) and `\r\n` (no bare LF, no HTAB); `host:port` is either reg-name / IPv4 literal followed by `:` and a 1-to-5-digit port ≤ 65535, or `[`-bracketed IPv6 literal followed by `]:port` (brackets stripped from the reported host); the header section is opaque bytes terminated by `\r\n\r\n`; total request-byte budget is capped at `ODIN_HTTP_REQUEST_MAX = 8192` with `host_len ∈ [1, ODIN_HTTP_HOST_MAX = 255]` (the host cap equals `ODIN_PROTO_HOST_MAX` from `odin/protocol.h:57`).
- **G2.** Expose `odin_http_parse_connect(const uint8_t *buf, size_t n, size_t *out_consumed, odin_http_connect_t *out)` from `odin/http_connect.{c,h}` such that on a well-formed prefix it returns `ODIN_HTTP_OK` with `*out_consumed = byte_index_after_final_CRLFCRLF` (so `buf[*out_consumed .. n)` is the post-handshake byte-forwarding payload) and `*out = {host_off, host_len, port}` aliasing `buf` and usable as `((const char *)(buf + out->host_off), out->host_len, out->port)` arguments to `odin_proto_encode_connect_req` (the one cast bridges the parser's `const uint8_t *` view to the encoder's `const char *` host parameter at `odin/protocol.h:70`); returns `ODIN_HTTP_NEED_MORE` only while `n < ODIN_HTTP_REQUEST_MAX`, the partial buffer still matches the `"CONNECT "` prefix, and the request-line CRLF or headers' CRLFCRLF has not yet arrived; returns one of `ODIN_HTTP_ERR_BAD_METHOD` / `ODIN_HTTP_ERR_BAD_REQUEST_TARGET` / `ODIN_HTTP_ERR_BAD_VERSION` / `ODIN_HTTP_ERR_HOST_LEN_INVALID` / `ODIN_HTTP_ERR_PORT_INVALID` / `ODIN_HTTP_ERR_REQUEST_TOO_LARGE` on protocol rejection; no allocation, no I/O, no global state; the reported host slice has IPv6 brackets stripped while `buf` itself is unmodified.

**Non-Goals:**

- HTTP/1.1 response writer (`200 Connection Established`, `4xx`, `5xx`) — a follow-up RFC pins the response byte sequences; this RFC ships only the request-side parser, matching the requirement's read → parse → encode three-step flow.
- HTTP methods other than `CONNECT`, HTTP/2, HTTP/3, plaintext upgrade, the `Proxy-Authorization` header value — the local listener exposes one tunnel verb and the headers are opaque per the requirement.
- Socket / network I/O, the read loop that feeds bytes into `buf`, and the buffer-management strategy that slides off `*out_consumed` bytes — the parser is pure-buffer; the Client owns the loop (separate RFC).

## 4. Design

### 4.1 Overview

A new flat module is added to `odin/`: `odin/http_connect.h` (public C header, the only public surface) and `odin/http_connect.c` (implementation, depending only on `<stddef.h>`, `<stdint.h>`, `<string.h>`, and `<assert.h>`). The new test source `odin/http_connect_unittests.cpp` joins the existing `:odin_unittests` executable in `odin/BUILD.gn` beside `cli_unittests.cpp` and `protocol_unittests.cpp`. The two new C sources append to the existing `source_set("odin")` `sources` list. No new GN target, no new symlink, no root `BUILD.gn` edit, no new dependency. The parser is pure: byte buffer in, struct + status out, no I/O, no allocation, no global state, no thread interaction. The Client (separate RFC) and the test binary link `:odin`; the Server side does not depend on this module because the Server never sees HTTPS_PROXY bytes.

N/A — textual description above is sufficient.

### 4.2 Detailed Design

#### 4.2.1 Accepted Grammar and Hard Caps

```
request          = request-line headers-block
request-line     = "CONNECT" SP request-target SP HTTP-version CRLF
request-target   = host ":" port
host             = host-token / "[" v6-token "]"
host-token       = 1*<any byte other than ':' / SP / CR / LF>
                                                ; reg-name / IPv4 literal, opaque
v6-token         = 1*<any byte other than ']' / SP / CR / LF>
                                                ; IPv6 literal, opaque; DNS /
                                                ; IP-literal syntax is the Server's
                                                ; concern (odin/protocol.h:32-34)
port             = 1*5 DIGIT                 ; value 0..65535
HTTP-version     = "HTTP/1.0" / "HTTP/1.1"
headers-block    = opaque bytes terminated by CRLF CRLF;
                   the parser scans for that 4-byte sequence and does
                   not parse field-name / field-value
SP               = %x20                      ; single space, no HTAB
CRLF             = %x0D %x0A                 ; no bare LF, no bare CR
```

Hard caps:

- `ODIN_HTTP_REQUEST_MAX = 8192` — total bytes from `buf[0]` through the second byte of the terminating CRLFCRLF; on overflow the parser returns `ERR_REQUEST_TOO_LARGE` (see §4.2.3 for the incremental-feed and one-shot cap branches).
- `ODIN_HTTP_HOST_MAX = 255` — host byte length after IPv6 bracket strip; must equal `ODIN_PROTO_HOST_MAX` (`odin/protocol.h:57`) so every host this parser accepts is a valid input to `odin_proto_encode_connect_req`.

**Unstated contract.** ASCII-only — every accepted token is ASCII, and `SP` / `CRLF` forbid HTAB and bare LF. The parser scans the header section only for the `\r\n\r\n` terminator, so any header-section syntax error (missing colon, fold-continuations, repeated headers) is invisible to it; a header-less request (`request-line CRLF` followed immediately by a second `CRLF`) is accepted. The two caps are not a DoS defense (listener is localhost-only): `ODIN_HTTP_HOST_MAX` keeps output a valid encoder input, and `ODIN_HTTP_REQUEST_MAX` lets the Client's read buffer be one fixed allocation.

**Mechanism.** Encode is not provided — the parser is decode-only; the bytes laid out above arrive from the local socket. Decode flow is in §4.2.3's Mechanism block.

Satisfies: G1 via the grammar block, the hard caps, and the forbidden-token list pinned in the Unstated contract paragraph.

#### 4.2.2 Parser API and Output Contract

```c
/* odin/http_connect.h */
#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include "odin/protocol.h"  /* ODIN_PROTO_HOST_MAX */
#ifdef __cplusplus
extern "C" {
#endif
#define ODIN_HTTP_HOST_MAX 255
#define ODIN_HTTP_REQUEST_MAX 8192
static_assert(ODIN_HTTP_HOST_MAX == ODIN_PROTO_HOST_MAX, "host caps must match");
typedef enum {
  ODIN_HTTP_OK = 0,
  ODIN_HTTP_NEED_MORE,
  ODIN_HTTP_ERR_REQUEST_TOO_LARGE,
  ODIN_HTTP_ERR_BAD_METHOD,
  ODIN_HTTP_ERR_BAD_REQUEST_TARGET,
  ODIN_HTTP_ERR_BAD_VERSION,
  ODIN_HTTP_ERR_HOST_LEN_INVALID,
  ODIN_HTTP_ERR_PORT_INVALID,
} odin_http_status_t;
typedef struct odin_http_connect_t {
  size_t host_off;
  size_t host_len;
  uint16_t port;
} odin_http_connect_t;
odin_http_status_t odin_http_parse_connect(const uint8_t *buf, size_t n, size_t *out_consumed, odin_http_connect_t *out);
#ifdef __cplusplus
}
#endif
```

**Unstated contract.** Pure: no allocation, no I/O, no global state; all pointer parameters non-null and the parser asserts in debug via `<assert.h>` (no `ERR_INVALID_ARG` status — caller correctness is a precondition). The `static_assert` above couples `ODIN_HTTP_HOST_MAX` to `ODIN_PROTO_HOST_MAX` (`odin/protocol.h:57`) at compile time so every accepted host is a valid input to `odin_proto_encode_connect_req` and a future bump to either macro fails the build instead of silently breaking the contract — the spelling `static_assert` is the macro `<assert.h>` exposes under C11+ and a built-in keyword under C++11+, so the same header source compiles cleanly when included from `odin/http_connect.c` (C) and `odin/http_connect_unittests.cpp` (C++17) without relying on a compiler extension; `ODIN_HTTP_REQUEST_MAX = 8192` is the §4.2.1 cap. On `OK`, `odin_http_connect_t` is a non-owning slice into `buf` with IPv6 brackets excluded: for the bracketed `[v6]:port` form `host_off` points one byte past `[` and `host_len` counts the bytes between `[` and `]`; for the reg-name / IPv4-literal form `host_off` is the first byte of the host and `host_len` runs through the byte before `:`; `host_len ∈ [1, ODIN_HTTP_HOST_MAX]` in both cases (host-syntax validation is out of scope per §4.2.1's opaque `host-token` / `v6-token`). §4.2.3's Mechanism uses these helpers, all returning half-open positions or `NOT_FOUND` (`SIZE_MAX`) when the byte/pattern is absent in `[start, end)`: `find_crlf` returns the matched CRLF's `\r` index; `find_double_crlf` returns one byte past the second CRLF's `\n` (so `*out_consumed = end` covers the full request through CRLFCRLF); `find_byte` returns the matched-byte index (first match — the §4.2.3 colon split relies on this so the §4.2.1 `host-token` `:` exclusion fires as `ERR_PORT_INVALID` on `host:embedded:port` shapes). Prefix-parser write rule: `NEED_MORE` and any `ERR_*` write neither `*out_consumed` nor `*out`; only `OK` writes both, after which the caller slides `buf[*out_consumed .. n)` (any speculative post-CRLFCRLF bytes) to the post-handshake byte-forwarding buffer.

Satisfies: G2 via the exported signature, the status enum, the output-struct slice semantics, and the prefix-parser write rule that makes `buf[*out_consumed .. n)` the exact byte-forwarding payload.

#### 4.2.3 Parsing Algorithm and Error Ordering

**Mechanism.**

```
parse_connect(buf, n, *out_consumed, *out):
  if n >= 8 and buf[0..8) != "CONNECT ":            return ERR_BAD_METHOD
  if n <  8: return memcmp(buf, "CONNECT ", n) == 0 ? NEED_MORE : ERR_BAD_METHOD
  rl_end = find_crlf(buf, start=8, end=n)
  if rl_end == NOT_FOUND: return n >= ODIN_HTTP_REQUEST_MAX ? ERR_REQUEST_TOO_LARGE : NEED_MORE
  sp = find_byte(buf, 8, rl_end, 0x20)
  if sp == NOT_FOUND:                               return ERR_BAD_VERSION
  if buf[sp+1 .. rl_end) not in {"HTTP/1.0","HTTP/1.1"}: return ERR_BAD_VERSION
  target_start = 8; target_end = sp
  if buf[target_start] == '[':
    rb = find_byte(buf, target_start+1, target_end, ']')
    if rb == NOT_FOUND or rb+1 >= target_end or buf[rb+1] != ':': return ERR_BAD_REQUEST_TARGET
    host_off, host_len, port_start = target_start+1, rb-(target_start+1), rb+2
  else:
    cl = find_byte(buf, target_start, target_end, ':')   ; first ':' — §4.2.1 host-token excludes ':'
    if cl == NOT_FOUND:                             return ERR_BAD_REQUEST_TARGET
    host_off, host_len, port_start = target_start, cl-target_start, cl+1
  port_len = target_end - port_start
  if port_len < 1 or port_len > 5:                  return ERR_PORT_INVALID
  port_value = 0
  for i in [port_start, target_end):
    if buf[i] < '0' or buf[i] > '9':                return ERR_PORT_INVALID
    port_value = port_value*10 + (buf[i] - '0')
  if port_value > 65535:                            return ERR_PORT_INVALID
  if host_len < 1 or host_len > ODIN_HTTP_HOST_MAX: return ERR_HOST_LEN_INVALID
  end = find_double_crlf(buf, start=rl_end, end=n)
  if end == NOT_FOUND: return n >= ODIN_HTTP_REQUEST_MAX ? ERR_REQUEST_TOO_LARGE : NEED_MORE
  if end > ODIN_HTTP_REQUEST_MAX:                   return ERR_REQUEST_TOO_LARGE
  *out_consumed = end; *out = {host_off, host_len, (uint16_t)port_value}; return OK
```

**Error ordering and irrecoverability.** Errors fire in two early classes plus one deferred class. `BAD_METHOD` fires at the first divergent method-byte prefix, so a non-HTTPS_PROXY caller closes immediately without buffering up to `ODIN_HTTP_REQUEST_MAX`. `REQUEST_TOO_LARGE` fires once `n ≥ ODIN_HTTP_REQUEST_MAX` regardless of where in parsing we are — including before either CRLF is located (incremental-feed branch) and after the headers' CRLFCRLF is located but `end > ODIN_HTTP_REQUEST_MAX` (one-shot cap branch); it is irrecoverable for this connection because additional reads cannot resolve it, so the caller must close. The remaining `BAD_REQUEST_TARGET` / `BAD_VERSION` / `PORT_INVALID` / `HOST_LEN_INVALID` classes wait for the full request-line CRLF before deciding.

Satisfies: G1 via the strict-CRLF + single-SP token split that pins the §4.2.1 grammar in code; G2 via the rejection ordering that maps the eight `odin_http_status_t` codes onto stream-position-determined arms.

### 4.3 Design Rationale

- **Chosen:** Non-owning view (`host_off + host_len + port`) in `odin_http_connect_t`, diverging from the RFC-001 decoder's copy-out (`char *host_out, size_t host_cap, size_t *out_host_len`) shape.
- **Reason:** The Client's three-step loop (read → parse → encode) keeps `buf` alive across all three steps, so the parser hands back offsets and `odin_proto_encode_connect_req` reads `(buf + host_off, host_len, port)` directly with no `memcpy`. An intermediate copy buys nothing because the parser never strips, normalizes, or NUL-terminates the host bytes (`odin_proto_encode_connect_req` takes a `(char *, size_t)` pair, not a C string).
- **Ruled out:** The RFC-001 copy-out shape pre-allocates a `char host_out[ODIN_PROTO_HOST_MAX + 1]` per call and `memcpy`s the host into it; that pattern makes sense for the QUIC-side decoder because the Server wants the host as a NUL-terminated string for `getaddrinfo`, but on the HTTPS_PROXY side the caller's next step is the encoder, so the copy is dead work.

- **Chosen:** Strict CRLF; bare LF and bare CR are not accepted line terminators.
- **Reason:** RFC 9112 §2.2 mandates CRLF; bare-LF acceptance is a documented request-smuggling vector when a lenient parser pairs with a strict downstream parser. Even though the listener is localhost-only, a future deployment that fronts it with another proxy must not inherit a smuggling-friendly parser; the cost of strictness today is zero because curl, browsers, and every other standard HTTPS_PROXY caller emit CRLF.
- **Ruled out:** Lenient `\r?\n` would let an off-by-one bug in a future cross-proxy chain split one request into two; the upside (accepting hand-rolled clients that emit bare LF) is hypothetical and has no caller in the Monorepo today.

- **Chosen:** Wait-and-parse the request-line (locate the request-line CRLF first, then validate every token in one pass), with `BAD_METHOD` as the one fast-fail exception that fires on the first divergent prefix byte.
- **Reason:** Each parser invocation runs after one `recv()`, so saving CPU per call is not the bottleneck; correctness of the prefix-parser contract is. Fast-failing `BAD_METHOD` matters because a non-HTTPS_PROXY caller (e.g., a browser pointed at the proxy port without HTTPS) sends `GET / HTTP/1.1\r\n` and the Client should close immediately rather than buffer up to `ODIN_HTTP_REQUEST_MAX` bytes first. Every other error class requires the full request-line to even locate, so deferring to a single parse pass after CRLF arrives keeps the implementation one straight-line function.
- **Ruled out:** Byte-by-byte state-machine validation of every token would let `BAD_VERSION` fire as soon as the version's first byte was wrong, but the wins are tiny (request-lines are tens of bytes) and the cost is a state enum, a per-byte switch, and a sharply larger test matrix — not worth it for a parser the Client invokes once per tunnel.

## 5. Backward Compatibility & Migration

Not applicable — this RFC introduces `odin/http_connect.{c,h}` and `odin/http_connect_unittests.cpp` from scratch, appends them to the existing `source_set("odin")` and `executable("odin_unittests")` source lists in `odin/BUILD.gn:24` / `odin/BUILD.gn:66`, and adds no new GN targets and no root `BUILD.gn` edits, so nothing that previously compiled or ran changes behavior.

## 6. Security

- **Threat:** Out-of-bounds read on a crafted partial input — for example a request-line that contains no CRLF within `n` bytes (would scan past the end of `buf` if `find_crlf` trusted a sentinel) or a target slice whose `port_len` is derived from an unvalidated end pointer (would read past `target_end`). The trigger surface is the local TCP socket the Client listens on; local processes can craft arbitrary byte sequences and "local" does not mean "trusted" — any process on the host may connect to loopback — and the offsets emerging from this parser are forwarded into a CONNECT_REQ frame that crosses the QUIC trust boundary to the Server.
- **Mitigation:** Every scan helper in §4.2.3's Mechanism block (`find_crlf`, `find_byte`, `find_double_crlf`) is bounded by an explicit `[start, end)` range derived from validated indices (`rl_end ≤ n`, `target_end < rl_end`, etc.); the port-validation loop's `buf[i]` reads run only over `i ∈ [port_start, target_end)` and only after the `port_len > 5` rejection in §4.2.3 fires, so the loop's read range is bounded by `target_end < rl_end ≤ n` even on a multi-`:` target whose `port_start` lands deep inside the request line; the parser writes no output until every offset has been validated against those bounds.
- **Enforcement:** §7 rows T4 (`NEED_MORE` on every partial prefix `n = 0 .. total - 1` of a well-formed request, `OK` at `n = total`, and `NEED_MORE` on the 22-byte bare-LF buffer that contains no `\r\n` anywhere within `n`; outputs untouched on `NEED_MORE` in every case), T6 (malformed request-targets including unbalanced bracket and missing colon), and T7 (port-string shapes including `:65536`/`:99999`/`:123456`/`:abc`/`:` and the multi-`:` `a:b:443` target that exercises the bounded port-validation loop within `[port_start, target_end)`) fire the trigger shapes and assert the safe outcomes.

- **Threat:** Port-string integer overflow into `uint16_t` — a crafted 6+-digit port string would, under a naive accumulator without digit-count or post-loop range check, silently wrap into a `uint16_t` unrelated to the input (`123456 mod 65536 = 57920`); the resulting `out->port` would be forwarded into `CONNECT_REQ.port` (a 16-bit big-endian field per RFC-001 §4.2.1) and the Server would dial the wrong upstream port silently.
- **Mitigation:** The port-validation loop in §4.2.3's Mechanism rejects any string with `port_len > 5` (the maximum decimal digits a `uint16_t` can hold) before the accumulation runs, and the post-loop check `port_value > 65535` catches the residual case where 5 digits exceed `0xFFFF` (`65536` through `99999`); both rejections return `ERR_PORT_INVALID` without writing `out`.
- **Enforcement:** §7 row T7 fires `:65536`, `:99999`, `:123456`, `:abc`, and `:` (empty port) and asserts `ERR_PORT_INVALID` with `*out_consumed` and `*out` unmodified.

## 7. Testing Strategy

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | Round-trip success — hostname, IPv4 literal, and minimal forms | `"CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n"` (59 B), `"CONNECT 127.0.0.1:8443 HTTP/1.0\r\n\r\n"` (35 B), `"CONNECT a:1 HTTP/1.1\r\n\r\n"` (24 B) | All return `ODIN_HTTP_OK`; `*out_consumed` equals input length; the `(buf + out->host_off, out->host_len)` slices match `"example.com"` / `"127.0.0.1"` / `"a"`; `out->port` is `443` / `8443` / `1` respectively | G1, G2 | unit |
| T2 | IPv6 bracketed authority strips brackets from reported host | `"CONNECT [::1]:443 HTTP/1.1\r\n\r\n"` and `"CONNECT [2001:db8::1]:8080 HTTP/1.1\r\n\r\n"` | Both return `ODIN_HTTP_OK`; the `(buf + out->host_off, out->host_len)` slices equal `"::1"` and `"2001:db8::1"` (brackets excluded); `out->port` is `443` / `8080`; the underlying `buf` bytes are unmodified | G1, G2 | unit |
| T3 | Headers are consumed; pipelined post-CRLFCRLF bytes remain for byte forwarding | `"CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\nProxy-Authorization: Bearer abc\r\n\r\n"` (92 B) followed by the 16-byte tail `"GET / HTTP/1.1\r\n"` (total `n = 108`) | Returns `ODIN_HTTP_OK`; `*out_consumed = 92` (covers full request through final CRLFCRLF); the 16 trailing bytes at `buf[92..108)` are not read or modified by the parser; `(buf + out->host_off, out->host_len, out->port)` is `("example.com", 11, 443)` | G1, G2 | unit |
| T4 | Prefix parser returns `NEED_MORE` on every partial input; `OK` at full request; bare-LF buffers never satisfy G1's strict-CRLF separator clause | Take the 59-byte buffer from T1's first case; call `odin_http_parse_connect` for every prefix length `n = 0, 1, …, 58`, then `n = 59`; separately, feed the 22-byte buffer `"CONNECT a:1 HTTP/1.1\n\n"` (bare-LF terminators instead of CRLFs) at `n = 22` | Every `n < 59` call on the well-formed prefix returns `ODIN_HTTP_NEED_MORE`; the `n = 59` call returns `OK` with `*out_consumed = 59`; the 22-byte bare-LF call returns `ODIN_HTTP_NEED_MORE` (no `\r\n` is ever located within the request-line scan, so the parser keeps waiting and would only escalate to `ERR_REQUEST_TOO_LARGE` once `n ≥ ODIN_HTTP_REQUEST_MAX` per T10's incremental-feed branch); `*out_consumed`/`*out` unmodified on every call | G1, G2, §6 | unit |
| T5 | Bad method fast-fails on the first divergent prefix byte; HTAB after method violates G1's single-SP separator clause | `"GET / HTTP/1.1\r\n\r\n"`, `"connect a:1 HTTP/1.1\r\n\r\n"` (lowercase), `"CONNECTx a:1 HTTP/1.1\r\n\r\n"` (no SP after method), `"CONNECT\ta:1\tHTTP/1.1\r\n\r\n"` (HTAB after method — byte 7 is `\t`, not `0x20`, so the `"CONNECT "` prefix check fails on the first divergent byte); plus partial-prefix inputs `"GE"` (2 B) and `"CONN"` (4 B) | All four full-input calls return `ODIN_HTTP_ERR_BAD_METHOD`; the 2-byte `"GE"` prefix also returns `ERR_BAD_METHOD` (first byte already divergent from `'C'`); the 4-byte `"CONN"` prefix returns `NEED_MORE` (still a valid prefix of `"CONNECT "`); `*out_consumed`/`*out` unmodified on every call | G1, G2 | unit |
| T6 | Malformed request-target shapes | `"CONNECT  HTTP/1.1\r\n\r\n"` (empty target between SPs), `"CONNECT example.com HTTP/1.1\r\n\r\n"` (no `:port`), `"CONNECT [::1:443 HTTP/1.1\r\n\r\n"` (unbalanced `[`), `"CONNECT [::1]443 HTTP/1.1\r\n\r\n"` (bracket without `:`) | All four return `ODIN_HTTP_ERR_BAD_REQUEST_TARGET`; `*out_consumed`/`*out` unmodified | G2, §6 | unit |
| T7 | Port string invalid (out-of-range value, oversized digit count, non-numeric, empty, embedded `:` in host-token) | `"CONNECT a:65536 HTTP/1.1\r\n\r\n"`, `"CONNECT a:99999 HTTP/1.1\r\n\r\n"`, `"CONNECT a:123456 HTTP/1.1\r\n\r\n"` (6-digit port — rejected before accumulation), `"CONNECT a:abc HTTP/1.1\r\n\r\n"`, `"CONNECT a: HTTP/1.1\r\n\r\n"` (empty port), `"CONNECT a:b:443 HTTP/1.1\r\n\r\n"` (host-token embedded `:`: §4.2.3's `find_byte` splits at the first `:` per §4.2.1's `host-token` exclusion, so the port string `"b:443"` fails the digit check) | All six return `ODIN_HTTP_ERR_PORT_INVALID`; `*out_consumed`/`*out` unmodified | G1, G2, §6 | unit |
| T8 | Bad HTTP-version token | `"CONNECT a:1 HTTP/2.0\r\n\r\n"`, `"CONNECT a:1 http/1.1\r\n\r\n"` (lowercase), `"CONNECT a:1 HTTP/1.\r\n\r\n"` (truncated to 7 bytes), `"CONNECT a:1\r\n\r\n"` (version absent — no second SP) | All four return `ODIN_HTTP_ERR_BAD_VERSION`; `*out_consumed`/`*out` unmodified | G2 | unit |
| T9 | Host length out of range | `"CONNECT " + ("x" * 256) + ":443 HTTP/1.1\r\n\r\n"` (256-byte host) | Returns `ODIN_HTTP_ERR_HOST_LEN_INVALID`; `*out_consumed`/`*out` unmodified | G2 | unit |
| T10 | Request exceeds `ODIN_HTTP_REQUEST_MAX` (both incremental-feed and one-shot cap branches) | Three shapes built around the 8233-byte buffer `B = "CONNECT a:1 HTTP/1.1\r\n" + "X-Pad: " + ("a" * 8200) + "\r\n\r\n"` (well-formed request-line followed by one over-long header whose CRLFCRLF terminator lands at byte index 8232, so `end = 8233`): (a) incremental feed of `B` — call the parser for every prefix `n = 0, …, 8232`; (b) one-shot feed of `B` with `n = 8233` — the headers' CRLFCRLF is located but `end = 8233 > 8192`, so the parser returns `ERR_REQUEST_TOO_LARGE` via the post-locate cap branch in §4.2.3's Mechanism; (c) `"CONNECT " + ("a" * 8192)` fed in one call with `n = 8200` — no CRLF anywhere, so the request-line CRLF is never located within the cap | Case (a): `NEED_MORE` for every `n < 8192` and `ERR_REQUEST_TOO_LARGE` on the first call with `n ≥ 8192` (CRLFCRLF still absent, incremental-feed branch); cases (b) and (c): `ERR_REQUEST_TOO_LARGE` on the single call (post-locate cap branch and incremental-feed branch respectively); `*out_consumed`/`*out` unmodified on every call across all three shapes | G1, G2 | unit |

## 8. Implementation Plan

- **P1. Land the parser surface, the test rows, and the build wiring with red `T1`–`T10`.**
  - **Scope:** add `odin/http_connect.h` exactly as §4.2.2 specifies — the `ODIN_HTTP_HOST_MAX` / `ODIN_HTTP_REQUEST_MAX` macros, the `static_assert(ODIN_HTTP_HOST_MAX == ODIN_PROTO_HOST_MAX, ...)` line that pulls `odin/protocol.h` for the comparand, the `odin_http_status_t` enum, the `odin_http_connect_t` struct, and the one exported function declaration — with the §4.2.1 grammar and hard-cap statement pinned in the header doc-comment; add `odin/http_connect.c` with `odin_http_parse_connect` body returning `ODIN_HTTP_NEED_MORE` unconditionally so the test binary links cleanly; add `odin/http_connect_unittests.cpp` containing rows `T1`–`T10` from §7, each as a separate `TEST(...)` whose first statement is `GTEST_SKIP() << "pending RFC-003 P2";` so the binary builds, runs, and reports the rows as `SKIPPED` — the local test suite stays green; append `"http_connect.c"` and `"http_connect.h"` to the `source_set("odin")` `sources` array at `odin/BUILD.gn:24` and append `"http_connect_unittests.cpp"` to the `executable("odin_unittests")` `sources` array at `odin/BUILD.gn:66`; no other GN edit (no new target, no root `BUILD.gn` change).
  - **Depends on:** None.
  - **Done when:** `gn gen out/` resolves the modified `:odin` and `:odin_unittests` targets and `ninja -C out/ tests` builds without error (the build failing on the new `static_assert` would itself signal the host-cap contract break, so a clean build proves `ODIN_HTTP_HOST_MAX == ODIN_PROTO_HOST_MAX` at compile time); the §4.2.1 grammar and hard-cap statement are pinned verbatim in `odin/http_connect.h`'s doc-comment and the function signature from §4.2.2 exports from the same header (G1, G2 staged); `T1`–`T10` are committed in `GTEST_SKIP`-wrapped (red) state and `out/odin_unittests --gtest_brief=1` reports all ten as `SKIPPED` while the run exits zero alongside the existing RFC-001 / RFC-002 rows.
- **P2. Implement the parser and turn `T1`–`T10` green.**
  - **Scope:** replace the stub body in `odin/http_connect.c` with the prefix-parser logic pinned in §4.2.3's Mechanism block (method fast-fail, request-line CRLF scan, single-SP token split with strict `HTTP/1.0` / `HTTP/1.1` match, IPv6 bracket strip with `]:` validation, port digit-count then value validation, host-length validation, double-CRLF scan), keeping the file's dependency surface limited to `<stddef.h>`, `<stdint.h>`, `<string.h>`, and `<assert.h>`; remove the `GTEST_SKIP() << ...;` first statement from each of `T1`–`T10` so the assertions fire for real on every run.
  - **Depends on:** P1.
  - **Done when:** `T1`–`T3` pass un-skipped against the §4.2.1 grammar (G1, G2 success arms); `T4`–`T10` pass un-skipped against the `odin_http_status_t` enum from §4.2.2 with the rejection semantics (`NEED_MORE`, `OK` with `*out_consumed`, `ERR_*`) pinned in §4.2.2's Unstated contract (G2 rejection arms, plus G1's strict-separator clause via T4's bare-LF input and T5's HTAB input); `tidy_odin.sh` exits clean on the new files; after a clean `ninja -C out/ tests`, `out/odin_unittests --gtest_brief=1` reports `T1`–`T10` all `PASSED` with no `SKIPPED` among them, and the surrounding RFC-001 / RFC-002 suites remain green.
