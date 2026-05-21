# RFC-004: Zero-Copy CONNECT_REQ Encode + Aliasing Decode (v2)

**Version:** 1.0  
**Author:** Chason Tang  
**Date:** 2026-05-21  
**Status:** Implemented

## 1. Summary

Add `odin_proto_encode_connect_req_v2` and `odin_proto_decode_connect_req_v2` to `odin/protocol.{c,h}` to eliminate the per-call `memcpy` of host bytes that the v1 codec performs on both the encode and decode paths. Encode fills a caller-supplied 3-slot iovec — `{scratch_header[3], host_alias, scratch_port[2]}` — that a QUIC scatter-gather sender emits as one stream frame without an intermediate flatten pass. Decode returns a non-owning `{host_off, host_len, port}` view into the caller's input buffer, mirroring the output shape `odin_http_parse_connect` already uses. The wire format from RFC-001 §4.2.1 is unchanged, so v1 and v2 codecs interop byte-for-byte. v1 `odin_proto_encode_connect_req` and `odin_proto_decode_connect_req` are marked deprecated via header doc-comments but remain callable; CONNECT_RESP has no variable payload and is untouched.

## 2. Motivation

RFC-001's v1 codec (`odin/protocol.h:70-77`) does two `memcpy`s of host bytes that future Client and Server transport handlers will hit on every tunnel: `odin_proto_encode_connect_req` (`odin/protocol.c:10-34`) flattens the host string into a contiguous output buffer at `odin/protocol.c:29` (`memcpy(&buf[3], host, host_len)`), and `odin_proto_decode_connect_req` (`odin/protocol.c:36-79`) copies the host bytes back out into a caller-supplied buffer at `odin/protocol.c:72` (`memcpy(host_out, &buf[3], host_len)`). A QUIC scatter-gather send API — one that accepts a vector of `(base, len)` chunks and emits them as one stream frame — would consume an iovec-shaped encode output directly, so the encode-side `memcpy` is dead work for any caller using such an API; the Client already holds the host bytes in a stable buffer (its socket read buffer, since RFC-003's `odin_http_parse_connect` returns a view aliasing that buffer per `odin/http_connect.h:54-58`). The decode-side `memcpy` is similarly avoidable: an aliasing view lets the Server's next step copy directly out of the QUIC receive buffer into whatever fixed-size buffer `getaddrinfo` needs, without first copying through the codec. No data available at this time.

## 3. Goals and Non-Goals

- **G1.** Expose `odin_proto_encode_connect_req_v2(const char *host, size_t host_len, uint16_t port, odin_proto_iov_t out_iov[3], uint8_t scratch_header[3], uint8_t scratch_port[2])` from `odin/protocol.{c,h}` such that on `ODIN_PROTO_OK` the three iovs concatenated byte-equal the buffer `odin_proto_encode_connect_req` produces for the same `(host, host_len, port)`, with `out_iov[1].base == (const void *)host` (the host slot aliases the caller's pointer — no host byte is read or copied by the function); the only non-OK status return is `ODIN_PROTO_ERR_HOST_LEN_INVALID` (host_len `0` or `> 255`); NULL pointer arguments are caller-side preconditions enforced by `assert()` in debug builds and UB in release, matching v1 at `odin/protocol.c:14-16`; no allocation, no I/O, no global state.
- **G2.** Expose `odin_proto_decode_connect_req_v2(const uint8_t *buf, size_t n, size_t *out_consumed, odin_proto_connect_req_view_t *out)` from `odin/protocol.{c,h}` such that on `ODIN_PROTO_OK` `*out = {host_off, host_len, port}` aliases `buf` (the host bytes live at `buf[out.host_off .. out.host_off + out.host_len)`, no host byte is copied), `*out_consumed = 5 + out.host_len`, and any trailing `n - *out_consumed` bytes stay in the caller's buffer; returns the same `odin_proto_status_t` codes as v1 decode (`NEED_MORE` on partial input, `ERR_BAD_VERSION` / `ERR_BAD_FRAME_TYPE` / `ERR_HOST_LEN_INVALID` on protocol-level invalid bytes), but `ERR_BUF_TOO_SMALL` is unreachable because the view carries no copy-out buffer; no allocation, no I/O, no global state.

**Non-Goals:**

- Wire-format changes — RFC-001 §4.2.1's byte layout is unchanged; v1 and v2 codecs interop because they read and write the same bytes.
- CONNECT_RESP v2 — `CONNECT_RESP` is a fixed 4 bytes with no variable payload, so no `memcpy` of caller data exists to eliminate; `odin_proto_encode_connect_resp` and `odin_proto_decode_connect_resp` are untouched.
- v1 removal — v1 `odin_proto_encode_connect_req` and `odin_proto_decode_connect_req` remain callable; both are marked deprecated symmetrically in their header doc-comments (both v1 paths are deprecated together because §2 establishes both perform an unnecessary host `memcpy` that v2 eliminates) so new callers prefer v2, but a follow-up RFC owns the removal once Client and Server transport handlers (future RFCs) have migrated.
- QUIC integration — neither this RFC nor RFC-001 calls a QUIC scatter-gather API; the codec hands back the iovs, and the Client/Server transport handlers (future RFCs) feed them into their QUIC binding.

## 4. Design

### 4.1 Overview

This RFC modifies `odin/protocol.h` to add one new typedef (`odin_proto_iov_t`), one new struct (`odin_proto_connect_req_view_t`), and two new function declarations (`odin_proto_encode_connect_req_v2`, `odin_proto_decode_connect_req_v2`); appends the two function bodies to `odin/protocol.c`; appends new test cases to the existing `odin/protocol_unittests.cpp`; edits the v1 `odin_proto_encode_connect_req` declaration at `odin/protocol.h:70-73` and the v1 `odin_proto_decode_connect_req` declaration at `odin/protocol.h:75-77` to add doc-comments marking them deprecated as of this RFC and pointing callers at v2 (both v1 functions remain callable; no `__attribute__((deprecated))` annotation, so v1's existing in-tree callers — the RFC-001 unit tests — do not generate `-Wdeprecated-declarations` warnings). No new `.c`/`.h`/`.cpp` files, no new GN target, no `odin/BUILD.gn` edit (the existing `:odin` source_set at `odin/BUILD.gn:25-32` and `:odin_unittests` executable sources at `odin/BUILD.gn:71-75` already cover the three modified files), no new dependency. As in RFC-001 the codec stays pure: byte buffers in, byte buffers out, no I/O, no allocation, no global state, no thread interaction. Future Client and Server transport handlers (separate RFCs) choose v1 or v2 depending on whether their QUIC binding has a scatter-gather send/recv API.

N/A — textual description above is sufficient.

### 4.2 Detailed Design

#### 4.2.1 Encoder iovec API and Aliasing Contract

```c
/* odin/protocol.h (new declarations, after the v1 decoder) */
typedef struct {
  const void *base;
  size_t len;
} odin_proto_iov_t;

odin_proto_status_t odin_proto_encode_connect_req_v2(
    const char *host, size_t host_len, uint16_t port,
    odin_proto_iov_t out_iov[3],
    uint8_t scratch_header[3], uint8_t scratch_port[2]);
```

**Unstated contract.** Pure: no allocation, no I/O, no global state, thread-safe via arguments-only state. The status enum is the existing `odin_proto_status_t` from `odin/protocol.h:61-68` (no new enumerator). All pointer parameters non-null; passing NULL is undefined behaviour and the function asserts via `<assert.h>` in debug builds (no `ERR_INVALID_ARG` status — caller correctness is a precondition, matching v1 at `odin/protocol.c:14-16`). `scratch_header` must be ≥ 3 bytes and `scratch_port` must be ≥ 2 bytes; the array-dimension hints in the signature document the requirement and are not runtime-enforced — passing a shorter buffer is undefined behaviour under the same caller-correctness contract as the NULL-pointer one. On `ODIN_PROTO_OK` the function writes the 3 header bytes (`0x01`, `0x01`, `(uint8_t)host_len`) into `scratch_header`, the 2 big-endian port bytes into `scratch_port`, and fills `out_iov` with `{base=scratch_header, len=3}`, `{base=host, len=host_len}` (the host slot literally is the caller's pointer — the function does not read any byte of `*host` and copies no host byte anywhere), `{base=scratch_port, len=2}`. The three iovs concatenated byte-equal the buffer `odin_proto_encode_connect_req` (`odin/protocol.c:10-34`) writes for the same `(host, host_len, port)`, so a QUIC scatter-gather sender that emits the 3 iovs as one stream frame produces a frame that a v1 or v2 decoder accepts. Caller must keep `host`, `scratch_header`, and `scratch_port` valid for the lifetime of `out_iov` (i.e., until the consumer — typically a QUIC scatter-gather send — finishes reading the bytes); the function stores their addresses in `out_iov`, not their bytes, so a caller that stack-allocates `scratch_header`/`scratch_port` in a helper and returns before the QUIC send completes will dereference freed stack memory. On `ODIN_PROTO_ERR_HOST_LEN_INVALID` (host_len `0` or `> 255`) — the only non-OK return — neither `out_iov`, `scratch_header`, nor `scratch_port` is written.

**Mechanism.**

```
assert host, out_iov, scratch_header, scratch_port non-null
if host_len < 1 or host_len > 255:    return ERR_HOST_LEN_INVALID
scratch_header[0] = 0x01
scratch_header[1] = 0x01
scratch_header[2] = (uint8_t)host_len
scratch_port[0]   = (uint8_t)((port >> 8) & 0xFF)
scratch_port[1]   = (uint8_t)(port       & 0xFF)
out_iov[0] = { base: scratch_header, len: 3 }
out_iov[1] = { base: host,           len: host_len }   ; aliasing, no copy
out_iov[2] = { base: scratch_port,   len: 2 }
return OK
```

Satisfies: G1 via the iovec-fill pseudocode that aliases the host pointer and writes no host byte, and the byte-equal-to-v1 invariant pinned in the **Unstated contract** paragraph.

#### 4.2.2 Decoder View Struct and Prefix-Parser Contract

```c
/* odin/protocol.h (new declarations) */
typedef struct {
  size_t host_off;
  size_t host_len;
  uint16_t port;
} odin_proto_connect_req_view_t;

odin_proto_status_t odin_proto_decode_connect_req_v2(
    const uint8_t *buf, size_t n, size_t *out_consumed,
    odin_proto_connect_req_view_t *out);
```

**Unstated contract.** Same purity, status enum, and assert preconditions as §4.2.1. Prefix parser; same state machine as v1 decode (`odin/protocol.c:36-79`) — the only difference is the output shape, not the byte-validation arms. On `ODIN_PROTO_OK` `*out = {host_off = 3, host_len = buf[2], port = ((uint16_t)buf[3 + host_len] << 8) | buf[4 + host_len]}` aliases `buf` (the same big-endian shift expression v1 decode uses at `odin/protocol.c:75-76`); the host bytes live at `buf[out.host_off .. out.host_off + out.host_len)` (no NUL terminator appended, no copy made); `*out_consumed = 5 + out.host_len`; any trailing `n - *out_consumed` bytes stay in the caller's buffer for the post-handshake byte-forwarding payload. Caller must keep `buf` valid for the lifetime of `*out` (i.e., until the host slice is no longer dereferenced — typically through the Server's DNS lookup and connect step); the view stores offsets into the caller's buffer, not host bytes, so freeing or repurposing `buf` before the slice is consumed dangles the view. Returns `ODIN_PROTO_NEED_MORE` when `n` is shorter than the next byte-level check needs (call again after more bytes arrive). Returns `ERR_BAD_VERSION` when `buf[0] != 0x01` (fires at `n == 1`, not `NEED_MORE`); `ERR_BAD_FRAME_TYPE` when `buf[1] != 0x01` (fires at `n == 2`); `ERR_HOST_LEN_INVALID` when `buf[2] == 0` (fires at `n >= 3`). `ERR_BUF_TOO_SMALL` is unreachable — the view carries no copy-out buffer to undersize. On any non-`OK` return neither `*out_consumed` nor `*out` is written.

**Mechanism (decode_connect_req_v2).**

```
assert buf, out_consumed, out non-null
if n < 1:                          return NEED_MORE
if buf[0] != 0x01:                 return ERR_BAD_VERSION
if n < 2:                          return NEED_MORE
if buf[1] != 0x01:                 return ERR_BAD_FRAME_TYPE
if n < 3:                          return NEED_MORE
host_len = buf[2]
if host_len < 1:                   return ERR_HOST_LEN_INVALID
total = 5 + host_len
if n < total:                      return NEED_MORE
*out = {
  host_off: 3,
  host_len: host_len,
  port:     ((uint16_t)buf[3 + host_len] << 8) | buf[4 + host_len],
}
*out_consumed = total
return OK
```

Satisfies: G2 via the prefix-parser pseudocode that yields offsets aliasing `buf` and never reads or writes a host byte.

### 4.3 Design Rationale

- **Chosen:** Caller-supplied scratch buffers (`uint8_t scratch_header[3]`, `uint8_t scratch_port[2]`) + iovec output array (`odin_proto_iov_t out_iov[3]`).
- **Reason:** A QUIC scatter-gather send API takes a vector of `(base, len)` chunks for one stream frame, so handing back exactly that shape lets the caller pass `out_iov` through to its binding's vec send call without an intermediate pack step. Caller-supplied scratch (versus static or thread-local backing inside the function) keeps the encoder reentrant and allocation-free across all caller threads; the 5 bytes total (3 + 2) is small enough that the caller stack-allocates and never frees.
- **Ruled out:** An output struct that embeds the header and port bytes inline (e.g., `{ uint8_t header[3]; const char *host; size_t host_len; uint8_t port_be[2]; }`) and lets the caller compose the iovec from struct fields. That shape works for the encode contract but doubles the surface area the caller's QUIC binding has to know about — the struct *and* the iovec layout — and forces a per-call struct-to-iovec adapter at every send site; iovec out-params skip that step.

- **Chosen:** `decode_connect_req_v2` returns `host_off` / `host_len` / `port` aliasing `buf` (offsets, not pointers) — same shape as `odin_http_connect_t` at `odin/http_connect.h:54-58`.
- **Reason:** Sharing one mental model with `odin_http_parse_connect`'s output keeps the codebase's two view-style outputs symmetric — a reviewer who has internalized one knows the other. Offsets also survive a buffer-base-pointer move (e.g., the caller re-allocates and re-points to a larger buffer keeping content), which a `const uint8_t *host_ptr` field would silently invalidate.
- **Ruled out:** A `const uint8_t *host_ptr` field that aliases `buf + 3` directly. Saves the caller one `+` operation per access but diverges from `odin_http_connect_t`'s shape and breaks under the re-alloc-with-copy pattern above.

## 5. Backward Compatibility & Migration

Not applicable — this RFC adds `odin_proto_iov_t`, `odin_proto_connect_req_view_t`, `odin_proto_encode_connect_req_v2`, and `odin_proto_decode_connect_req_v2` as new declarations in `odin/protocol.h`; v1 `odin_proto_encode_connect_req` and `odin_proto_decode_connect_req` keep their signatures, return codes, and behaviour, and the v1 deprecations are doc-comment edits with no `__attribute__((deprecated))` annotation so existing callers see no warning. Nothing that previously compiled or ran changes behaviour.

## 6. Security

- **Threat:** Out-of-bounds read on the host slice when a caller derefs the view returned by `odin_proto_decode_connect_req_v2` — for example a malformed `CONNECT_REQ` byte stream that claims `host_len = 200` while the buffer holds only 50 bytes (would induce a `host_off=3, host_len=200` view that, dereferenced as `buf[host_off..host_off+host_len)`, reads past the end of `buf`). The trigger surface is the QUIC stream's byte feed, whose bytes ultimately carry data influenced by the third party that issued the original `CONNECT host:port` line to the Client; the QUIC-layer TLS authenticates the peer but does not vouch for the content shape.
- **Mitigation:** §4.2.2's Mechanism block validates `n >= 5 + host_len` (the `if n < total: return NEED_MORE` arm) before writing `*out`, so an `OK` return guarantees the host slice `[host_off, host_off + host_len)` fits within `buf[0, n)`. The version, frame_type, and host_len bytes are validated in order before the total-length check fires; `host_len == 0` returns `ERR_HOST_LEN_INVALID` so the view never reports a zero-length slice. On `NEED_MORE` and `ERR_*` returns neither `*out_consumed` nor `*out` is written, so the caller's sentinel-initialized view is preserved and a misuse pattern that reads `out` after a non-`OK` return does not observe undefined host_off / host_len bytes from this call.
- **Enforcement:** §7 rows T4 (rejection on bad version / frame_type / host_len with `*out` unmodified) and T5 (`NEED_MORE` on every partial-input length `n = 0..total-1`, `OK` at `n = total` with `host_off + host_len <= n`, and the trailing bytes left untouched) fire the trigger inputs and assert the safe outcomes.

## 7. Testing Strategy

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | v2 encode emits byte-equivalent output to v1; host slot aliases the caller's pointer | Call `odin_proto_encode_connect_req_v2` for three cases: `("example.com", 11, 443)`, `("a", 1, 0)`, and `("x"*255, 255, 65535)`. For each: flatten `out_iov[0..3)` by concatenating `(base, len)` ranges into a contiguous buffer, then call `odin_proto_encode_connect_req` for the same `(host, host_len, port)` into a second contiguous buffer; compare the two byte-for-byte. Also assert `out_iov[1].base == (const void *)host` (pointer-equal, not just byte-equal) and `out_iov[0].len == 3 && out_iov[2].len == 2`. | All three calls return `ODIN_PROTO_OK`. Flattened v2 bytes equal the v1-encoded bytes byte-for-byte (so the 16-/6-/260-byte expected sequences RFC-001 §7 T1 pins emerge from v2 as well). The host iov base is pointer-equal to the caller's `host` pointer (no `memcpy` of host occurred). | G1 | unit |
| T2 | v2 encode rejects `host_len` out of `[1, 255]` and writes nothing on the error path | Sentinel-fill `out_iov`, `scratch_header`, and `scratch_port`. Call `odin_proto_encode_connect_req_v2` once with `host_len=0` (host buffer `""`) and once with `host_len=256` (host buffer is 256 bytes of `'x'` so no out-of-bounds host read could occur even if the encoder erroneously read, though the contract says it does not read host). | Both calls return `ODIN_PROTO_ERR_HOST_LEN_INVALID`. Every byte of `out_iov`, `scratch_header`, and `scratch_port` equals its sentinel value after the call (no write on the error path). | G1 | unit |
| T3 | v2 decode round-trip yields the aliasing view and the slice matches the original host bytes | Use `odin_proto_encode_connect_req` to produce three frames for `("example.com", 11, 443)`, `("a", 1, 0)`, `("x"*255, 255, 65535)` (lengths 16, 6, 260). For each frame call `odin_proto_decode_connect_req_v2(frame, total, &consumed, &view)` with `view` zero-initialized. | All three calls return `ODIN_PROTO_OK`. `consumed` equals the encoded frame length (16/6/260). `view.host_off == 3`. `view.host_len` equals the input host length. `view.port` equals the input port. The byte slice `frame[view.host_off .. view.host_off + view.host_len)` byte-equals the original host bytes (so the alias is correct, not just the length). | G2 | unit |
| T4 | v2 decode rejects bad version / frame_type / host_len; `*out`/`*consumed` unmodified | For each of: `buf[0]=0x00` and `buf[0]=0x02` (bad version, called at `n=1` and at `n=16` — the canonical 16-byte `example.com:443` frame from T1, with `buf[0]` overwritten); `buf[1]=0x00`, `buf[1]=0x02`, `buf[1]=0x03` (bad frame_type — including the wrong-known `CONNECT_RESP` type — called at `n=2` and at `n=16` — same canonical T1 frame, with `buf[1]` overwritten); `buf[2]=0x00` (bad host_len, called at `n=5` with a syntactically-otherwise-valid frame). Pass a sentinel-initialized `view` and `consumed` to each call. | The bad-version cases return `ODIN_PROTO_ERR_BAD_VERSION`; the bad-frame_type cases return `ODIN_PROTO_ERR_BAD_FRAME_TYPE`; the bad-host_len case returns `ODIN_PROTO_ERR_HOST_LEN_INVALID`. `*consumed` and every field of `*view` equal their sentinel values after every call. | G2, §6 | unit |
| T5 | v2 decode returns `NEED_MORE` on every partial prefix, `OK` at full frame, and leaves trailing bytes untouched | Encode `("example.com", 11, 443)` into a 16-byte frame, then append 100 trailing bytes (XOR-derived pattern `i ^ 0x55`) into a 116-byte buffer; snapshot the trailing 100 bytes. For each `n = 0..15` call `odin_proto_decode_connect_req_v2` with a sentinel-initialized `view`/`consumed`; then call once with `n = 116`. | Every `n < 16` call returns `ODIN_PROTO_NEED_MORE` with `*consumed` and `*view` left at sentinels. The `n = 116` call returns `ODIN_PROTO_OK` with `consumed == 16`, `view.host_off == 3`, `view.host_len == 11`, `view.port == 443`, and `view.host_off + view.host_len == 14 <= 16 <= 116` (slice fits inside the consumed prefix); the trailing 100 bytes in the buffer are byte-identical to the snapshot taken before the call. | G2, §6 | unit |

## 8. Implementation Plan

- **P1. Land v2 declarations, v1 deprecation marker, and red T1–T5.**
  - **Scope:** add to `odin/protocol.h`, after the v1 decoder declaration at `odin/protocol.h:75-77`, the `odin_proto_iov_t` typedef and the `odin_proto_encode_connect_req_v2` declaration per §4.2.1, plus the `odin_proto_connect_req_view_t` struct and the `odin_proto_decode_connect_req_v2` declaration per §4.2.2; edit the v1 `odin_proto_encode_connect_req` declaration at `odin/protocol.h:70-73` and the v1 `odin_proto_decode_connect_req` declaration at `odin/protocol.h:75-77` to prepend the deprecation doc-comments described in §4.1 (no `__attribute__((deprecated))` annotation on either). Append to `odin/protocol.c` two stub function bodies — `odin_proto_encode_connect_req_v2` returning `ODIN_PROTO_OK` without writing any output and `odin_proto_decode_connect_req_v2` returning `ODIN_PROTO_NEED_MORE` unconditionally — so the test binary links cleanly; the stubs assert non-null pointers up front so debug-build callers still observe the precondition. Append to `odin/protocol_unittests.cpp` the five test cases T1–T5 from §7, each as a separate `TEST(OdinProtoV2Test, ...)` whose first statement is `GTEST_SKIP() << "pending RFC-004 P2";` so the binary builds, runs, and reports the rows as `SKIPPED` — the local test suite stays green. No `odin/BUILD.gn` edit (the existing source_set at `odin/BUILD.gn:25-32` and executable sources at `odin/BUILD.gn:71-75` already cover the three modified files).
  - **Depends on:** None.
  - **Done when:** `gn gen out/` resolves the unchanged `:odin` and `:odin_unittests` targets and `ninja -C out/ tests` builds without error; the `odin_proto_iov_t` and `odin_proto_connect_req_view_t` definitions plus both v2 function signatures from §4.2.1 / §4.2.2 export from `odin/protocol.h` (G1, G2 staged); `T1`–`T5` are committed in `GTEST_SKIP`-wrapped (red) state and `out/odin_unittests --gtest_brief=1` reports them as `SKIPPED` while the run exits zero alongside the existing RFC-001 / RFC-002 / RFC-003 rows.
- **P2. Implement v2 encode + decode and turn T1–T5 green.**
  - **Scope:** replace the two stub bodies in `odin/protocol.c` with the implementations pinned in §4.2.1's Mechanism block (encode aliases the host pointer into `out_iov[1]`, writes 3 bytes to `scratch_header` and 2 bytes to `scratch_port`, copies no host byte) and §4.2.2's Mechanism block (decode is a prefix parser whose `OK` arm writes `*out = {host_off=3, host_len, port}` and `*out_consumed = 5 + host_len`, copies no host byte); the v1 `<string.h>` include stays in `odin/protocol.c` for the existing v1 `memcpy` call sites at `odin/protocol.c:29` and `odin/protocol.c:72` (v2 itself uses no string functions). Remove the `GTEST_SKIP() << ...;` first statement from each of T1–T5 so the assertions fire for real on every run.
  - **Depends on:** P1.
  - **Done when:** `T1` (encode byte-equivalence and host-pointer aliasing) passes un-skipped against the bytes v1 emits and the pointer-equality check (G1); `T2` (encode rejection) passes un-skipped against `ERR_HOST_LEN_INVALID` with `out_iov`/`scratch_*` untouched (G1); `T3` (decode round-trip and aliasing view) passes un-skipped with the slice matching the original host bytes (G2); `T4` (decode rejection) passes un-skipped against `ERR_BAD_VERSION` / `ERR_BAD_FRAME_TYPE` / `ERR_HOST_LEN_INVALID` with `*view`/`*consumed` untouched (G2, §6); `T5` (decode `NEED_MORE` on every partial prefix and trailing-bytes preservation) passes un-skipped against the prefix-parser pseudocode (G2, §6); `tidy_odin.sh` exits clean on the modified `odin/protocol.c` and `odin/protocol_unittests.cpp`; after a clean `ninja -C out/ tests`, `out/odin_unittests --gtest_brief=1` reports zero `SKIPPED` and zero `FAILED` across the protocol rows alongside the existing RFC-001 / RFC-002 / RFC-003 rows.
