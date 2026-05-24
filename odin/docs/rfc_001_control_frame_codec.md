# RFC-001: Odin Proxy Control Frame and Codec

**Version:** 1.0  
**Author:** Chason Tang  
**Date:** 2026-05-13  
**Status:** Implemented

## 1. Summary

Define the wire format for the two control frames the empty `odin` project's QUIC proxy will use to negotiate each tunnel — `CONNECT_REQ` (Client → Server: host string + port) and `CONNECT_RESP` (Server → Client: 16-bit `error_code`, `0` = OK) — and ship the encode/decode module (`odin/protocol.{c,h}`) plus a googletest binary (`odin/protocol_unittests.cpp`). A new `odin/BUILD.gn` declares the GN targets, and the root `BUILD.gn` `tests` group's deps gains `//odin:protocol_unittests`. Both frames carry a 1-byte `version` and a 1-byte `frame_type` prefix so future control frame kinds can land without re-cutting the format. The codec is pure byte buffers in / byte buffers out — no QUIC, no sockets, no allocation — so the Client/Server transport layers (xquic, follow-up RFCs) can adopt it independently and the test binary runs without xquic linked in.

## 2. Motivation

The `odin/` directory is currently empty apart from `.clang-format`, `.clang-tidy`, and `docs/`. The next milestone is a QUIC proxy: a Client (HTTPS_PROXY accepting `CONNECT host:port` from local callers) tunneling each request over a fresh QUIC stream to a Server (which does DNS, opens TCP to the upstream, splices bytes). Each stream's first frame in either direction is a control frame: Client tells Server which upstream to dial, Server tells Client whether it dialed. Until that handshake is pinned in bytes, the Client and Server cannot be implemented in parallel and the codec cannot be unit-tested below the QUIC layer. Landing the wire format and a pure-buffer codec first gives both ends a single source of truth and isolates the one place future control frame kinds will evolve, while keeping the codec independent of xquic so the test binary can run on every supported target without dragging in the QUIC stack. No data available at this time.

## 3. Goals and Non-Goals

- **G1.** Define the on-the-wire layout for `CONNECT_REQ` and `CONNECT_RESP` — both prefixed by a 1-byte `version` and a 1-byte `frame_type`; `CONNECT_REQ` payload is `host_len(1) + host_bytes + port(2 BE)` with `host_len ∈ [1, 255]`; `CONNECT_RESP` payload is `error_code(2 BE)` with `0x0000` denoting success.
- **G2.** Expose `odin_proto_encode_connect_req`, `odin_proto_decode_connect_req`, `odin_proto_encode_connect_resp`, `odin_proto_decode_connect_resp` from `odin/protocol.{c,h}` such that every (host, port) and every `error_code` round-trip through encode → decode unchanged; the decoders are prefix parsers — they return `NEED_MORE` on every partial-input prefix length, `OK` with `*out_consumed` (total frame size) on a full frame leaving any trailing bytes in the caller's buffer, and `ERR_BAD_VERSION` / `ERR_BAD_FRAME_TYPE` / `ERR_HOST_LEN_INVALID` / `ERR_BUF_TOO_SMALL` on protocol-level invalid bytes via the `odin_proto_status_t` enum.

**Non-Goals:**

- xquic integration / QUIC stream lifecycle / Client- and Server-side transport handlers — separate RFCs build on top of this codec.
- Server-side DNS resolution and upstream TCP dial — the failure surface that defines specific non-zero `error_code` values lives there, not here.
- Client-side `HTTPS_PROXY` listener and `CONNECT host:port` line parsing — separate RFC; this codec only consumes the host/port pair the listener has already extracted.
- Specific non-zero `error_code` value assignments — the wire format reserves the 16-bit slot; v1 pins only `0x0000 = OK`. Subsequent RFCs assign reasons (e.g., DNS failure, connection refused) as their failure paths land.
- Encryption, peer authentication, replay protection — provided by QUIC at the layer below; the codec sees plaintext bytes and is not the trust enforcement point.

## 4. Design

### 4.1 Overview

This RFC adds a flat source layout under `odin/`: `odin/protocol.h` (public C header, the only public surface), `odin/protocol.c` (implementation, depending only on `<stdint.h>`, `<stddef.h>`, `<string.h>`, and `<assert.h>`), and `odin/protocol_unittests.cpp` (googletest source). A new `odin/BUILD.gn` declares two GN targets — `source_set("odin")` over the `.c`/`.h` pair, and `executable("protocol_unittests")` over the `.cpp` with deps on `//odin` and `//googletest:gtest_main`. The root `BUILD.gn` `tests` group's `deps` array gains a single line — `//odin:protocol_unittests` — so `ninja -C out/ tests` builds the new test binary alongside the existing entries. The codec is pure: byte buffers in, byte buffers out, no I/O, no allocation, no global state, no thread interaction. Future Client and Server executables (separate RFCs) will depend on `//odin`.

N/A — textual description above is sufficient.

### 4.2 Detailed Design

#### 4.2.1 Wire Format

```
CONNECT_REQ (Client -> Server, frame_type = 0x01):
  +---------+------------+----------+----------------+----------+
  | version | frame_type | host_len |   host bytes   |   port   |
  | 1 byte  |   1 byte   |  1 byte  | host_len bytes |  2 bytes |
  +---------+------------+----------+----------------+----------+
  Total: 5 + host_len bytes; host_len in [1, 255]; max 260 bytes.

CONNECT_RESP (Server -> Client, frame_type = 0x02):
  +---------+------------+--------------+
  | version | frame_type |  error_code  |
  | 1 byte  |   1 byte   |    2 bytes   |
  +---------+------------+--------------+
  Total: 4 bytes (fixed).
```

Field semantics (v1):

- `version` = `0x01` (else `ERR_BAD_VERSION`).
- `frame_type` = `0x01` (CONNECT_REQ) or `0x02` (CONNECT_RESP); else `ERR_BAD_FRAME_TYPE`.
- `host_len` ∈ `[1, 255]`; zero → `ERR_HOST_LEN_INVALID`.
- `error_code`: `0x0000` = OK; non-zero is a Server-assigned failure reason (v1 reserves the 16-bit space; specific assignments belong to follow-up RFCs).

**Unstated contract.** Multi-byte integers (`port`, `error_code`) are big-endian; 1-byte fields are endianness-agnostic. `host` bytes are opaque — DNS hostname syntax and IP-literal grammar are the Server's concern, and the codec does not reject embedded `\0` (callers that want a NUL-safe view check `strlen(host_out) == *out_host_len`). Each frame is self-delimiting from its own bytes: `CONNECT_REQ`'s total size is `5 + host_len` (byte 2), `CONNECT_RESP`'s is fixed at 4. Per §4.3, QUIC streams are byte-oriented, so the decoders are prefix parsers (§4.2.3) that consume one frame and leave any trailing bytes for the next layer. No padding, no reserved bits in v1; the 1-byte `version` is the sole forward-compatibility lever (a future v2 bumps to `0x02`, v1 decoders see `ERR_BAD_VERSION`).

**Mechanism.** Encode lays the bytes out in the order shown above; decode parses them back as a prefix parser (line-by-line trace in §4.2.3).

Satisfies: G1 via the byte layout and field semantics above.

#### 4.2.2 Codec API & Status Enum

```c
/* odin/protocol.h */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ODIN_PROTO_VERSION_V1         0x01
#define ODIN_PROTO_FRAME_CONNECT_REQ  0x01
#define ODIN_PROTO_FRAME_CONNECT_RESP 0x02
#define ODIN_PROTO_HOST_MAX           255
#define ODIN_PROTO_CONNECT_REQ_MAX    (5 + ODIN_PROTO_HOST_MAX)  /* 260 */
#define ODIN_PROTO_CONNECT_RESP_SIZE  4

typedef enum {
  ODIN_PROTO_OK = 0,
  ODIN_PROTO_NEED_MORE,
  ODIN_PROTO_ERR_BUF_TOO_SMALL,
  ODIN_PROTO_ERR_HOST_LEN_INVALID,
  ODIN_PROTO_ERR_BAD_VERSION,
  ODIN_PROTO_ERR_BAD_FRAME_TYPE,
} odin_proto_status_t;
```

```c
odin_proto_status_t odin_proto_encode_connect_req(
    const char *host, size_t host_len, uint16_t port,
    uint8_t *buf, size_t cap, size_t *out_n);

odin_proto_status_t odin_proto_decode_connect_req(
    const uint8_t *buf, size_t n, size_t *out_consumed,
    char *host_out, size_t host_cap, size_t *out_host_len,
    uint16_t *out_port);

odin_proto_status_t odin_proto_encode_connect_resp(
    uint16_t error_code, uint8_t *buf, size_t cap, size_t *out_n);

odin_proto_status_t odin_proto_decode_connect_resp(
    const uint8_t *buf, size_t n, size_t *out_consumed,
    uint16_t *out_error_code);

#ifdef __cplusplus
}
#endif
```

**Unstated contract (shared).** All four functions are pure: no allocation, no I/O, no global state, thread-safe via arguments-only state. All pointer parameters must be non-null; passing NULL is undefined behaviour and the codec asserts via `<assert.h>` in debug builds (no `ERR_INVALID_ARG` status — caller correctness is a precondition).

**Encode contract.** `encode_connect_req` fails only with `ERR_HOST_LEN_INVALID` (host_len `0` or `> 255`) or `ERR_BUF_TOO_SMALL` (`cap < encoded_size`); `encode_connect_resp` takes a typed `uint16_t error_code` and a fixed 4-byte output, so its only failure mode is `ERR_BUF_TOO_SMALL` (`cap < 4`). On any non-`OK` return, neither `buf` nor `*out_n` is written — both encoders validate inputs (host_len, cap) before any byte is laid down. There is no `NEED_MORE` path — both encoders produce a full frame from typed inputs in a single call, and the produced byte sequence is the one pinned in §4.2.1.

Satisfies: G2 via the four exported functions and the encode → decode round-trip invariant pinned by field-by-field byte symmetry against §4.2.1.

#### 4.2.3 Decode Prefix Parsing

**Decode contract.** Decode is a prefix parser. It returns `NEED_MORE` when `n` is shorter than the next byte-level check needs (callers keep accumulating from `stream_recv`). It returns `OK` and writes `*out_consumed` (total frame size: 4 for `CONNECT_RESP`, `6..260` for `CONNECT_REQ`; always ≤ `n`) when one frame parses fully — any trailing `n - *out_consumed` bytes remain in the caller's buffer for the next layer (the byte-forwarding mode that follows the handshake). It returns `ERR_*` (`BAD_VERSION`, `BAD_FRAME_TYPE`, `HOST_LEN_INVALID`, `BUF_TOO_SMALL`) on protocol-level invalid bytes — these are irrecoverable; no additional input resolves them. `host_out` receives the host bytes plus a trailing `\0`, so `host_cap ≥ host_len + 1` is required; `*out_host_len` is the on-wire byte count. All `out_*` pointers (including `*out_consumed`) are written only on `OK`; on `NEED_MORE` or any error the caller's output buffers are unmodified. `version` and `frame_type` are validated as soon as their byte arrives — an unknown version returns `BAD_VERSION` at `n=1`, not `NEED_MORE`.

**Mechanism (decode_connect_req).**

```
if n < 1:                                    return NEED_MORE
if buf[0] != VERSION_V1:                     return ERR_BAD_VERSION
if n < 2:                                    return NEED_MORE
if buf[1] != FRAME_CONNECT_REQ:              return ERR_BAD_FRAME_TYPE
if n < 3:                                    return NEED_MORE
host_len = buf[2]
if host_len < 1:                             return ERR_HOST_LEN_INVALID
total = 5 + host_len
if n < total:                                return NEED_MORE
if host_cap < host_len + 1:                  return ERR_BUF_TOO_SMALL
copy buf[3 .. 3 + host_len) to host_out; host_out[host_len] = '\0'
*out_host_len = host_len
*out_port = (buf[3 + host_len] << 8) | buf[4 + host_len]
*out_consumed = total
return OK
```

`decode_connect_resp` is the same prefix-parser pattern with `total = 4` after the version + frame_type checks.

Satisfies: G2 via the prefix-parser semantics and the rejection enum that pins decode's failure surface.

### 4.3 Design Rationale

- **Chosen:** Hand-rolled big-endian byte layout pinned in §4.2.1.
- **Reason:** A `CONNECT_RESP` is 4 bytes and a `CONNECT_REQ` is `≤ 260` bytes; a Protobuf or CBOR runtime would dwarf the codec's own object footprint, and the only payload variability is the host bytes. Hand-rolled keeps the implementation a single `.c` file with no third-party dependency in the QUIC data path, where every byte parsed has to clear length checks anyway.
- **Ruled out:** Protobuf adds a code-gen step, a runtime, and tag/wire-type framing that QUIC stream framing already implies, all to encode 5 + `host_len` bytes; CBOR has the same shape at smaller cost.

- **Chosen:** 1-byte `host_len` (max 255).
- **Reason:** DNS hostnames cap at 253 bytes (RFC 1035 §2.3.4) and the bracketed IPv6 literal form caps at ~41 bytes, so 1 byte covers every real CONNECT host with margin and self-documents the upper bound to the decoder.
- **Ruled out:** 2-byte `host_len` (max 65535) wastes a byte and falsely advertises support for 64KB hostnames that neither DNS nor HTTP CONNECT semantics actually allow; revisit only if `odin` tunnels something other than HTTPS proxy CONNECT.

- **Chosen:** Prefix-parser decode (returns `*out_consumed`, `NEED_MORE` on partial input, leaves trailing bytes in the caller's buffer).
- **Reason:** QUIC streams are byte-oriented; `stream_recv` boundaries do not align with control-frame boundaries, and the byte-forwarding payload that follows the handshake arrives on the same stream. A prefix parser lets the transport layer accumulate `stream_recv` chunks, call decode after each chunk, and naturally hand off any post-frame bytes to byte-forwarding mode without a separate slicing step.
- **Ruled out:** "Exact one-frame buffer" decode (treat short buffers as `ERR_TRUNCATED` and trailing bytes as `ERR_TRAILING_BYTES`) requires the transport layer to find the frame boundary itself before calling the codec — but the v1 wire format has no whole-frame length prefix, so the slicing logic would have to duplicate the per-frame-type size discovery the prefix parser already performs internally. Adding a 2-byte `frame_length` to the common header would let an exact decoder work generically but spends 2 bytes per frame on information the prefix parser derives for free from `host_len` (or from the fixed-4 `CONNECT_RESP` size).

## 5. Backward Compatibility & Migration

Not applicable — this RFC introduces `odin/protocol.{c,h}` and `odin/BUILD.gn` from scratch; the codec has no callers, and the root `BUILD.gn` edit is a single additive `deps` entry, so nothing that previously compiled or ran changes behaviour.

## 6. Security

- **Threat:** Memory corruption from a malformed `CONNECT_REQ` byte stream — for example a frame claiming `host_len = 200` while the accumulated buffer holds only 50 bytes (would read past the end of `buf` if the codec trusted `host_len`), or a caller-supplied `host_cap < host_len + 1` (would write past the end of `host_out` if the codec copied unconditionally). The trigger surface is the QUIC stream's byte feed, whose bytes ultimately carry data influenced by the third party that issued the original `CONNECT host:port` line to the Client; the QUIC-layer TLS authenticates the peer but does not vouch for the content shape.
- **Mitigation:** The prefix parser validates every offset before reading or writing — each successive byte-range check returns `NEED_MORE` (no read past `n - 1`) when `n` is short of the next field, and `ERR_BUF_TOO_SMALL` (no write to `host_out`) when `host_cap < host_len + 1`. The exact ordering is pinned in §4.2.3's Mechanism block; on `NEED_MORE` and on any `ERR_*` return, no `out_*` pointer is written.
- **Enforcement:** §7 rows T6 (`NEED_MORE` on every partial-input length `n=0..total-1`, `OK` at `n=total`) and T8 (`ERR_BUF_TOO_SMALL` on undersized output buffers) fire the trigger inputs and assert the safe outcomes.

## 7. Testing Strategy

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | CONNECT_REQ round-trip | `host="example.com"` (host_len=11), `port=443` (frame size = 16); also boundary cases `host="a"` (host_len=1, frame size = 6), `host="x"*255` (host_len=255, frame size = 260), `port=0`, `port=65535` | encode produces the byte sequence pinned in §4.2.1; decode of the exact frame buffer returns `OK`, sets `*out_consumed` to the frame size, and yields the original `host`, `host_len`, and `port` | G1, G2 | unit |
| T2 | CONNECT_RESP round-trip | `error_code=0x0000` and `error_code=0xFFFF` | encode produces `01 02 00 00` and `01 02 FF FF` respectively; decode of those 4-byte buffers returns `OK`, sets `*out_consumed = 4`, and yields the original `error_code` | G1, G2 | unit |
| T3 | Both decoders reject unknown `version` immediately at `n=1` | For each of `decode_connect_req` and `decode_connect_resp`: bytes with `buf[0] = 0x00` and `buf[0] = 0x02` (no v2 yet), each at the decoder's full-frame length and at `n=1` (single byte) | all eight calls return `ERR_BAD_VERSION`; the four `n=1` cases prove the §4.2.3 contract that a bad version byte fires immediately, not `NEED_MORE`; `out_*` pointers and `*out_consumed` unmodified | G2 | unit |
| T4 | Both decoders reject wrong/unknown `frame_type` immediately at `n=2` | `decode_connect_req` with `buf[1]` set to `0x00`, `0x03` (unknown), and `0x02` (wrong-known: the `CONNECT_RESP` type); `decode_connect_resp` with `buf[1]` set to `0x00`, `0x03` (unknown), and `0x01` (wrong-known: the `CONNECT_REQ` type); each at the decoder's full-frame length and at `n=2` (only valid `buf[0]=0x01` and the bad `frame_type` byte) | all twelve calls return `ERR_BAD_FRAME_TYPE`; the six `n=2` cases prove early rejection at the second byte, including the two wrong-known-type cases that catch a decoder mistakenly accepting the other direction's frame; `out_*` pointers and `*out_consumed` unmodified | G2 | unit |
| T5 | Decoder/encoder reject bad `host_len` | Decode: CONNECT_REQ bytes with `buf[2]=0x00`. Encode: `odin_proto_encode_connect_req` called with `host_len=0` and `host_len=256` | all three calls return `ERR_HOST_LEN_INVALID`; `out_*` pointers and `*out_consumed` unmodified | G2 | unit |
| T6 | Decoder returns `NEED_MORE` on every partial prefix; `OK` at full frame | Encoded CONNECT_REQ for `host="example.com", port=443` (16 bytes); call decode for every prefix length `n=0, 1, …, 15`, then `n=16`; also CONNECT_RESP bytes with `n=0, 1, 2, 3`, then `n=4` | every `n < total` call returns `NEED_MORE` with `*out_*` and `*out_consumed` unmodified; the `n=total` call returns `OK` with `*out_consumed=total` (16 and 4 respectively) and the round-trip outputs | G2, §6 | unit |
| T7 | Decoder consumes one frame and leaves trailing bytes | Buffer holding a well-formed CONNECT_REQ (16 bytes) followed by 100 arbitrary bytes (`n=116`); same for CONNECT_RESP (4 bytes + 100 trailing, `n=104`) | both decodes return `OK` with `*out_consumed=16` and `*out_consumed=4` respectively; the trailing 100 bytes in the caller's buffer are not read or modified | G2 | unit |
| T8 | Encoder/decoder reject undersized output buffer | Encode CONNECT_REQ with `cap=4` (under 5+host_len); decode CONNECT_REQ on a complete frame with `host_cap=host_len` (one short of `host_len+1`); encode CONNECT_RESP with `cap=3` | all three return `ERR_BUF_TOO_SMALL`; `out_*` pointers, `*out_consumed`, and output buffers unmodified | G2, §6 | unit |

## 8. Implementation Plan

- **P1. Land `odin/BUILD.gn` and the codec skeleton with red `T1`–`T8`.**
  - **Scope:** add `odin/protocol.h` per §4.2.2 (the constants, the `odin_proto_status_t` enum, and the four function declarations, with the §4.2.1 wire format pinned in the header doc-comment); add `odin/protocol.c` with each function body returning `ODIN_PROTO_NEED_MORE` unconditionally so the test binary links cleanly; add `odin/protocol_unittests.cpp` containing rows `T1`–`T8` from §7, each as a separate `TEST(...)` whose first statement is `GTEST_SKIP() << "pending RFC-001 P2";` so the binary builds, runs, and reports the rows as skipped — the local test suite stays green; add `odin/BUILD.gn` declaring `source_set("odin")` over the `.c`/`.h` pair and `executable("protocol_unittests")` with `testonly = true`, `sources = [ "protocol_unittests.cpp" ]`, `deps = [ ":odin", "//googletest:gtest_main" ]`, and `configs += [ "//build:cxx17" ]`; append `"//odin:protocol_unittests"` to the root `BUILD.gn` `tests` group's `deps` array.
  - **Depends on:** None.
  - **Done when:** `gn gen out/` resolves the new `//odin:protocol_unittests` dep added to the root `BUILD.gn` `tests` group and `ninja -C out/ tests` builds without error; the §4.2.1 wire format is pinned verbatim in `odin/protocol.h`'s doc-comment and the four function signatures from §4.2.2 export from the same header (G1, G2 staged); `T1`–`T8` are committed in `GTEST_SKIP`-wrapped (red) state and `out/protocol_unittests --gtest_brief=1` reports them all as `SKIPPED` while the run exits zero.
- **P2. Implement the codec and turn `T1`–`T8` green.**
  - **Scope:** replace the four stub function bodies in `odin/protocol.c` with the codec logic pinned in §4.2.1 (wire format the encoders write), §4.2.2's Encode contract, and §4.2.3's Mechanism block (the decoders parse the same bytes back), keeping the file dependency surface limited to `<stdint.h>`, `<stddef.h>`, `<string.h>`, and `<assert.h>`; remove the `GTEST_SKIP() << ...;` first statement from each of `T1`–`T8` so the assertions fire for real on every run.
  - **Depends on:** P1.
  - **Done when:** `T1` and `T2` (round-trip) pass un-skipped against the §4.2.1 byte layout (G1); `T3`–`T8` pass un-skipped against the `odin_proto_status_t` enum from §4.2.2 with the decode-side semantics (`NEED_MORE`, `OK` with `*out_consumed`, `ERR_*`) pinned in §4.2.3 (G2); `tidy_odin.sh` exits clean on the new files; `out/protocol_unittests --gtest_brief=1` reports zero skipped, zero failed.
