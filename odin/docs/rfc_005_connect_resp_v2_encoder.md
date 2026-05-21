# RFC-005: Fixed-Size CONNECT_RESP Encoder v2

**Version:** 1.0  
**Author:** Chason Tang  
**Date:** 2026-05-22  
**Status:** Implemented

## 1. Summary

Add `odin_proto_encode_connect_resp_v2` to `odin/protocol.c` and `odin/protocol.h` for encoding the fixed 4-byte `CONNECT_RESP` frame without the v1 `(uint8_t *buf, size_t cap, size_t *out_n)` variable-length-buffer shape. The v2 API writes into a typed `odin_proto_connect_resp_frame_t` whose storage is exactly `ODIN_PROTO_CONNECT_RESP_SIZE` bytes, has no recoverable error return, and emits the same byte sequence as v1: version `0x01`, frame type `0x02`, and the big-endian `uint16_t error_code`. The wire format, decoder, and v1 encoder remain available and behavior-preserving.

## 2. Motivation

`CONNECT_RESP` is documented as fixed at 4 bytes in `odin/protocol.h:15-20`, and `ODIN_PROTO_CONNECT_RESP_SIZE` is already `4` at `odin/protocol.h:59`. The current v1 encoder signature at `odin/protocol.h:108-110` still requires callers to pass `buf`, `cap`, and `out_n`; its implementation at `odin/protocol.c:81-97` has one non-OK path, `cap < ODIN_PROTO_CONNECT_RESP_SIZE`, then writes exactly four bytes and sets `*out_n = 4`. For callers that allocate a response frame as a fixed 4-byte object, the variable-length-buffer contract adds a branch and an output length write that do not carry protocol information. A v2 fixed-size encoder makes the contract match the frame shape while preserving v1 for existing callers. No data available at this time.

## 3. Goals and Non-Goals

- **G1.** Expose `void odin_proto_encode_connect_resp_v2(uint16_t error_code, odin_proto_connect_resp_frame_t *out)` from `odin/protocol.c` and `odin/protocol.h` such that each call writes exactly the fixed 4-byte `CONNECT_RESP` frame `{0x01, 0x02, error_code >> 8, error_code & 0xFF}` into `out->bytes`; the API has no `cap`, no `out_n`, no `odin_proto_status_t` return, no allocation, no I/O, and no global state.

**Non-Goals:**

- Wire-format changes - v2 emits the same `CONNECT_RESP` bytes that `odin_proto_encode_connect_resp` emits today.
- `odin_proto_decode_connect_resp_v2` - the decoder remains a prefix parser over caller-supplied input length `n`, so its length argument is still part of the stream parsing contract.
- v1 removal - `odin_proto_encode_connect_resp` remains callable and behavior-preserving; removing it belongs to a later RFC after callers migrate.
- `error_code` assignment policy - this RFC preserves the existing 16-bit value space and does not define new failure-code meanings.

## 4. Design

### 4.1 Overview

This RFC modifies `odin/protocol.h` to add one fixed-size frame typedef and one v2 encoder declaration near the existing `odin_proto_encode_connect_resp` declaration, and to add a doc-comment on the v1 response encoder recommending v2 without adding a compiler deprecation attribute. It appends one implementation body to `odin/protocol.c` and appends new unit tests to `odin/protocol_unittests.cpp`. No `odin/BUILD.gn` edit is needed because the existing `:odin` source set already includes `protocol.c` and `protocol.h` at `odin/BUILD.gn:25-32`, and the existing `:odin_unittests` executable already includes `protocol_unittests.cpp` at `odin/BUILD.gn:71-75`. The codec remains pure: bytes are derived from typed inputs, with no I/O, allocation, shared state, or thread interaction.

N/A — textual description above is sufficient.

### 4.2 Detailed Design

#### 4.2.1 Fixed-Size CONNECT_RESP Encoder API

```c
#include <stdint.h>

#define ODIN_PROTO_CONNECT_RESP_SIZE 4

typedef struct {
  uint8_t bytes[ODIN_PROTO_CONNECT_RESP_SIZE];
} odin_proto_connect_resp_frame_t;

void odin_proto_encode_connect_resp_v2(uint16_t error_code,
                                       odin_proto_connect_resp_frame_t *out);
```

**Unstated contract.** `out` must be non-null; passing NULL is a caller precondition violation and is enforced with `assert(out != NULL)` in debug builds, matching the existing v1 null-pointer style at `odin/protocol.c:84-85`. The frame object owns exactly four output bytes; callers send `out->bytes` with length `sizeof(out->bytes)` or `ODIN_PROTO_CONNECT_RESP_SIZE`. There is no status return because every `uint16_t error_code` is encodable and the output storage size is part of the type. The function writes all four bytes on every valid call and does not read prior contents of `*out`. It does not allocate, perform I/O, access global state, or write any caller memory outside `out->bytes`.

**Mechanism.**

```
assert out non-null
out->bytes[0] = ODIN_PROTO_VERSION_V1
out->bytes[1] = ODIN_PROTO_FRAME_CONNECT_RESP
out->bytes[2] = (uint8_t)((error_code >> 8) & 0xFF)
out->bytes[3] = (uint8_t)(error_code & 0xFF)
return
```

Satisfies: G1 via the typed 4-byte output frame, the no-`cap`/no-`out_n` signature, and the byte-writing mechanism.

### 4.3 Design Rationale

- **Chosen:** `odin_proto_connect_resp_frame_t` plus a `void` v2 encoder.
- **Reason:** A C parameter written as `uint8_t out[4]` still decays to `uint8_t *`, so it does not make fixed-size storage part of the call type. A frame struct lets ordinary callers pass a typed 4-byte object without `cap`/`out_n`, and the lack of recoverable failure leaves no useful `odin_proto_status_t` value to return.
- **Ruled out:** `odin_proto_status_t odin_proto_encode_connect_resp_v2(uint16_t error_code, uint8_t out[ODIN_PROTO_CONNECT_RESP_SIZE])`. That removes `cap` and `out_n`, but still accepts an arbitrary byte pointer in normal C calls and would return `ODIN_PROTO_OK` unconditionally for every valid caller.

## 5. Backward Compatibility & Migration

Not applicable — this RFC adds `odin_proto_connect_resp_frame_t` and `odin_proto_encode_connect_resp_v2` as new declarations in `odin/protocol.h`; v1 `odin_proto_encode_connect_resp` keeps its signature, return codes, and behavior, and the v1 doc-comment recommendation adds no compiler warning.

## 6. Security

Not applicable — the new encoder takes a typed `uint16_t` and a typed fixed-size output frame, performs no I/O or allocation, reads no external state, and introduces no new attacker-controlled input.

## 7. Testing Strategy

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | v2 emits exact boundary-code bytes | Call `odin_proto_encode_connect_resp_v2` with `error_code = 0x0000` and `error_code = 0xFFFF`, each into a zero-initialized `odin_proto_connect_resp_frame_t`. | The first frame bytes are `{0x01, 0x02, 0x00, 0x00}`; the second frame bytes are `{0x01, 0x02, 0xFF, 0xFF}`. | G1 | unit |
| T2 | v2 frame type is exactly four bytes and writes stay inside the frame field | Define a local wrapper struct `{ uint8_t before; odin_proto_connect_resp_frame_t frame; uint8_t after; }`, initialize `before` and `after` to sentinel bytes, assert `sizeof(odin_proto_connect_resp_frame_t) == ODIN_PROTO_CONNECT_RESP_SIZE`, then encode `error_code = 0x1234` into `wrapper.frame`. | The size assertion sees `4`; `wrapper.frame.bytes` equals `{0x01, 0x02, 0x12, 0x34}`; `before` and `after` still equal their sentinel bytes. | G1 | unit |
| T3 | v2 output interops with the existing decoder and preserves trailing bytes | Encode `error_code = 0xABCD` with v2 into a 4-byte prefix, append 100 trailing bytes with pattern `i ^ 0x55`, snapshot the trailing bytes, then call `odin_proto_decode_connect_resp(buffer, 104, &consumed, &out_code)`. | Decode returns `ODIN_PROTO_OK`, `consumed == 4`, `out_code == 0xABCD`, and the 100 trailing bytes are byte-identical to the snapshot. | G1 | unit |

## 8. Implementation Plan

- **P1. Land v2 declaration, linkable stub, and red T1-T3.**
  - **Scope:** add `odin_proto_connect_resp_frame_t` and the `odin_proto_encode_connect_resp_v2` declaration to `odin/protocol.h` per §4.2.1; add a doc-comment above `odin_proto_encode_connect_resp` that recommends v2 for new fixed-size response frames without adding `__attribute__((deprecated))`; append a stub `odin_proto_encode_connect_resp_v2` body to `odin/protocol.c` that asserts `out != NULL` and returns without writing, so the test binary links; append T1-T3 from §7 to `odin/protocol_unittests.cpp`, each as a separate `TEST(OdinProtoRespV2Test, ...)` whose first statement is `GTEST_SKIP() << "pending RFC-005 P2";`.
  - **Depends on:** None.
  - **Done when:** `gn gen out/` resolves the unchanged `:odin` and `:odin_unittests` targets and `ninja -C out/ tests` builds without error; the `odin_proto_connect_resp_frame_t` typedef and v2 signature from §4.2.1 export from `odin/protocol.h` (G1 staged); T1-T3 are committed in `GTEST_SKIP`-wrapped red state and `out/odin_unittests --gtest_brief=1` reports them as `SKIPPED` while the run exits zero.
- **P2. Implement v2 encoder and turn T1-T3 green.**
  - **Scope:** replace the stub body in `odin/protocol.c` with the four-byte write sequence pinned in §4.2.1's Mechanism block; remove the `GTEST_SKIP() << "pending RFC-005 P2";` first statement from T1-T3 so the assertions run on every local test run.
  - **Depends on:** P1.
  - **Done when:** T1 passes un-skipped for the boundary byte sequences (G1); T2 passes un-skipped for `sizeof(odin_proto_connect_resp_frame_t) == ODIN_PROTO_CONNECT_RESP_SIZE`, exact `0x1234` bytes, and unchanged canaries (G1); T3 passes un-skipped through `odin_proto_decode_connect_resp` with `consumed == 4`, `out_code == 0xABCD`, and unchanged trailing bytes (G1); `tidy_odin.sh` exits clean on the modified `odin/protocol.c` and `odin/protocol_unittests.cpp`; after a clean `ninja -C out/ tests`, `out/odin_unittests --gtest_brief=1` reports zero `SKIPPED` and zero `FAILED` for the new response-v2 rows.
