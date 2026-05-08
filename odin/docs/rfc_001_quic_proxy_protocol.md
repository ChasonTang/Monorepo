# RFC-001: odin Client-Server QUIC Proxy Protocol

**Version:** 1.0  
**Author(s):** Chason Tang  
**Date:** 2026-05-08  
**Status:** Proposed

## 1. Summary

odin is a single CLI binary that runs as either an `HTTPS_PROXY` (CONNECT) front-end (`odin client`) or as the upstream-side forwarder (`odin server`), with the two halves linked by a QUIC tunnel built on xquic. This RFC defines the wire protocol carried on each xquic bidirectional stream: a fixed three-byte header (1-byte type, 2-byte big-endian length) wraps two handshake frames — `CONNECT_REQUEST` (target authority) and `CONNECT_RESPONSE` (status code) — after which the stream becomes an opaque byte tunnel passed verbatim to and from the upstream HTTPS server. The RFC ships the encoder/decoder pair and a googletest suite that exercises every documented `DecodeResult` value.

## 2. Motivation

The odin project is a fresh codebase — `odin/` currently contains only an empty `docs/` directory. Before any client- or server-side wiring (HTTPS_PROXY listener, c-ares resolver, upstream TCP/TLS) can be built, both halves need a stable, machine-checkable agreement on the bytes that flow on each QUIC stream. Without that agreement, the two halves cannot be developed or tested independently, and there is no way to detect protocol regressions short of an end-to-end run.

The value of pinning the protocol now: the encoder/decoder pair becomes the executable spec; mismatches between client and server become localized test failures rather than full-tunnel hangs; and the ALPN string `odin/1` gives a clean version handle so that future revisions (e.g., UDP CONNECT, multi-stream multiplexing) can be introduced under a new ALPN without ambiguity, with xquic returning `XQC_EALPN_NOT_SUPPORTED` (errno `639` in `xqc_errno.h`) on mismatch.

## 3. Goals and Non-Goals

**Goals:**

- **G1.** A wire-format specification covering both handshake frames (`CONNECT_REQUEST`, `CONNECT_RESPONSE`) and the subsequent opaque-tunnel phase on a single xquic bidirectional stream, expressed in §4.2 as named constants and enums in `odin/protocol.h` plus an ALPN string `odin/1`.
- **G2.** A `FrameEncoder` / `FrameDecoder` pair under `odin/` whose decoder distinguishes every malformed-input category documented in §4.2 (truncated header, truncated payload, oversized authority, unknown frame type, malformed `CONNECT_RESPONSE` length) by returning a distinct `DecodeResult` enum value.
- **G3.** A googletest binary `protocol_test` declared in `odin/test/BUILD.gn` and wired into the existing root-level `//:tests` group, whose cases exercise every §8 scenario; the binary's exit status drives the protocol-regression signal under `ninja -C out/Default tests`.

**Non-Goals:**

- HTTPS_PROXY listener implementation (parsing the inbound `CONNECT host:port HTTP/1.1` request and writing back the `200 Connection Established` line) — out of scope; covered by a follow-up RFC once this protocol lands.
- c-ares-based DNS resolution and upstream TCP/TLS connection on the server side — deferred to a follow-up RFC for the same reason.
- xquic engine bring-up, UDP socket I/O loop, ALPN registration glue, retry/back-off — this RFC defines the bytes on a stream, not how streams are established.
- Authentication or authorization between odin client and server — the v1 protocol carries no credentials.

## 4. Design

### 4.1 Overview

The same odin binary runs in one of two roles selected by the first argv after the binary name (`odin client` / `odin server`). Each tunneled HTTPS connection takes one xquic bidirectional stream:

```
HTTPS client          odin client                  odin server                upstream HTTPS server
                      (HTTPS_PROXY)
     │                     │                            │                              │
     │ CONNECT host:443    │                            │                              │
     │ ──────────────────► │ xqc_stream_create()        │                              │
     │                     │ send CONNECT_REQUEST       │                              │
     │                     │ ─────────────────────────► │ ares_getaddrinfo(host)       │
     │                     │                            │ TCP connect, TLS (boringssl) │
     │                     │                            │ ────────────────────────────►│
     │                     │ recv CONNECT_RESPONSE(OK)  │                              │
     │                     │ ◄───────────────────────── │                              │
     │ 200 Connection Est. │                            │                              │
     │ ◄───────────────────│                            │                              │
     │       opaque bytes  │ ◄────── opaque bytes ────► │ ◄──── opaque bytes ─────────►│
```

Per-stream state is tracked separately on each side. The client starts in `INIT`, transitions to `WAIT_RESP` after sending `CONNECT_REQUEST`, then to `TUNNEL` once it reads a `CONNECT_RESPONSE` with status `kOk`, otherwise to `CLOSED`. The server starts in `INIT`, parses the request to enter `RESOLVING`, on success enters `CONNECTING_UPSTREAM`, on success enters `TUNNEL` and emits `CONNECT_RESPONSE(kOk)`; any failure transitions to `CLOSED` after emitting the corresponding non-zero status. Once both sides reach `TUNNEL` no further framing applies — bytes are forwarded verbatim using `xqc_stream_send` / `xqc_stream_recv`.

### 4.2 Detailed Design

A frame is a 3-byte fixed header followed by `length` payload bytes:

```
 0                   1                   2
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     type      |          length (BE)          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                payload (length bytes)         ...
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

Field semantics:

| Field | Width | Notes |
|---|---|---|
| `type` | uint8 | `0x01` = `CONNECT_REQUEST`, `0x02` = `CONNECT_RESPONSE`; all other values reserved |
| `length` | uint16, big-endian | Payload length in bytes; capped at 260 by `kMaxAuthorityLen` for `CONNECT_REQUEST` and fixed at 1 for `CONNECT_RESPONSE` |

`CONNECT_REQUEST` payload is `length` UTF-8 bytes of the authority `host:port` (port mandatory, no scheme, no path). The 260-byte cap covers a 253-byte FQDN (RFC 1035 maximum) plus `:65535` (6 chars) plus a small slack byte. `CONNECT_RESPONSE` payload is exactly 1 byte holding a `ConnectStatus`:

| Code | Name | Meaning |
|---|---|---|
| `0x00` | `kOk` | Server is in `TUNNEL`; opaque bytes follow |
| `0x01` | `kBadRequest` | `CONNECT_REQUEST` payload was not parseable as `host:port` |
| `0x02` | `kDnsFailure` | `ares_getaddrinfo` returned a non-success status |
| `0x03` | `kUpstreamUnreachable` | TCP or TLS to upstream failed |
| `0x04` | `kInternalError` | Server-side fault not covered above |

Decode-time error categories surfaced as `DecodeResult`:

| Enum | Trigger |
|---|---|
| `kOk` | Frame fully consumed |
| `kNeedMoreData` | Input shorter than 3 bytes, or shorter than `3 + length` |
| `kUnknownFrameType` | `type` byte is not `0x01` or `0x02` |
| `kAuthorityTooLong` | `type == 0x01` and `length > kMaxAuthorityLen` (260) |
| `kInvalidFrame` | `type == 0x02` and `length != 1` |

The ALPN string negotiated at xquic handshake is the C string `"odin/1"` (no NUL on the wire — passed to `xqc_engine_register_alpn` with explicit length `6`). Future incompatible revisions register `"odin/2"` etc.; older servers reject the new ALPN at handshake with `XQC_EALPN_NOT_SUPPORTED`.

### 4.3 Design Rationale

- **Chosen:** Fixed 3-byte header (uint8 type + uint16 BE length), one tunneled HTTPS connection per xquic bidirectional stream.
- **Reason:** xquic streams already provide reliable, ordered byte delivery, so the framing layer only needs to delimit the two short handshake frames before the opaque-byte phase; uint16 length is sufficient for an authority string and avoids varint complexity. One stream per HTTPS connection lets the existing xquic flow control and `stream_close_notify` cover per-tunnel teardown without an odin-level keep-alive frame.
- **Ruled out:** Multiplexing many HTTPS connections onto one stream with an odin-level connection ID — duplicates xquic's own stream-ID multiplexing for no gain and complicates the decoder state machine.

## 5. Interface Changes

**Before:**

```
N/A — new module odin/ (no prior odin headers, no prior odin BUILD.gn).
```

**After:**

```cpp
// odin/protocol.h
#ifndef ODIN_PROTOCOL_H_
#define ODIN_PROTOCOL_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace odin::protocol {

inline constexpr char        kAlpn[]            = "odin/1";
inline constexpr std::size_t kAlpnLen           = sizeof(kAlpn) - 1;
inline constexpr std::size_t kHeaderSize        = 3;
inline constexpr std::size_t kMaxAuthorityLen   = 260;

enum class FrameType : std::uint8_t {
  kConnectRequest  = 0x01,
  kConnectResponse = 0x02,
};

enum class ConnectStatus : std::uint8_t {
  kOk                  = 0x00,
  kBadRequest          = 0x01,
  kDnsFailure          = 0x02,
  kUpstreamUnreachable = 0x03,
  kInternalError       = 0x04,
};

enum class DecodeResult {
  kOk,
  kNeedMoreData,
  kUnknownFrameType,
  kAuthorityTooLong,
  kInvalidFrame,
};

struct ConnectRequest  { std::string   authority; };
struct ConnectResponse { ConnectStatus status;    };

class FrameEncoder {
 public:
  static void EncodeConnectRequest(const ConnectRequest&  req,
                                   std::vector<std::uint8_t>* out);
  static void EncodeConnectResponse(const ConnectResponse& resp,
                                    std::vector<std::uint8_t>* out);
};

class FrameDecoder {
 public:
  // Decode one frame. On kOk, *consumed = kHeaderSize + length. On
  // kNeedMoreData, *consumed is left untouched; caller appends more bytes
  // and retries with the same FrameDecoder instance.
  DecodeResult DecodeConnectRequest(const std::uint8_t* in, std::size_t in_len,
                                    ConnectRequest* out, std::size_t* consumed);
  DecodeResult DecodeConnectResponse(const std::uint8_t* in, std::size_t in_len,
                                     ConnectResponse* out, std::size_t* consumed);
};

}  // namespace odin::protocol

#endif  // ODIN_PROTOCOL_H_
```

**New build files:**

```
# odin/BUILD.gn
static_library("odin_protocol") {
  sources  = [ "protocol.cc", "protocol.h" ]
  configs += [ "//build:cxx20" ]
}

# odin/test/BUILD.gn
executable("protocol_test") {
  testonly = true
  sources  = [ "protocol_test.cc" ]
  deps     = [
    "//odin:odin_protocol",
    "//googletest:gtest",
  ]
  configs += [ "//build:cxx20" ]
}
```

The root `BUILD.gn` `group("tests")` gains one entry: `"//odin/test:protocol_test"` (next to the existing `dl_render_unittests` dep).

## 6. Backward Compatibility & Migration

Not applicable — this is the first release of the odin protocol; no prior wire format exists.

## 7. Security

The wire format itself carries no credentials and exposes no parsers richer than a length-delimited byte buffer. Two concerns specific to the framing surface:

- **Oversized-payload denial of memory.** Without the `kMaxAuthorityLen` cap a peer could announce `length = 0xFFFF` and force a 64 KiB allocation per stream. `FrameDecoder::DecodeConnectRequest` rejects `length > 260` with `kAuthorityTooLong` before any payload allocation occurs, by reading the 3-byte header first and checking the cap against the declared length.
- **Authority parsing.** The authority string is passed to `ares_getaddrinfo` and to the upstream connect logic in a follow-up RFC; the decoder treats the authority as opaque bytes only and does not interpret characters. Validation (rejecting NULs, control characters, missing `:port`) is the responsibility of the server-side consumer in the follow-up RFC and is out of scope here.

## 8. Testing Strategy

A single googletest binary `protocol_test` (sources at `odin/test/protocol_test.cc`) drives `FrameEncoder` and `FrameDecoder` end-to-end. Tests use `EXPECT_EQ` over byte vectors for encoder output and over `DecodeResult` values for decoder negative paths. The binary is declared by the `executable("protocol_test")` target in `odin/test/BUILD.gn` shown in §5, depends on the in-tree `//googletest:gtest` target (the same one consumed by `display_list/testing/BUILD.gn`), and is added to the root `:tests` group so it builds and runs under `ninja -C out/Default tests`.

**Key Scenarios:**

| # | Covers | Scenario | Input | Expected Behavior |
|---|--------|----------|-------|-------------------|
| S1 | G1, G2 | Encode CONNECT_REQUEST | `ConnectRequest{authority="example.com:443"}` | Output bytes equal `01 00 0F 65 78 61 6D 70 6C 65 2E 63 6F 6D 3A 34 34 33` (18 bytes) |
| S2 | G1, G2 | Encode CONNECT_RESPONSE OK | `ConnectResponse{status=kOk}` | Output bytes equal `02 00 01 00` (4 bytes) |
| S3 | G2, G3 | Round-trip both frames | Encoder output of S1 then S2 fed in one buffer to decoder | Two `kOk` returns; decoded structs equal originals; sum of `*consumed` equals total input length (22) |
| S4 | G2, G3 | Truncated header | 2-byte input `01 00` | Returns `kNeedMoreData`; `*consumed` unchanged from caller-supplied initial value |
| S5 | G2, G3 | Truncated payload | Header `01 00 0F` plus 14 of the 15 declared payload bytes | Returns `kNeedMoreData`; `*consumed` unchanged |
| S6 | G2, G3 | Oversized authority | `01 01 05` (length = 261) followed by 261 arbitrary bytes | Returns `kAuthorityTooLong` |
| S7 | G2, G3 | Unknown frame type | Two inputs: `00 00 00` and `03 00 00` | Both return `kUnknownFrameType` |
| S8 | G2, G3 | Malformed CONNECT_RESPONSE length | `02 00 02 00 00` (length = 2 instead of 1) | Returns `kInvalidFrame` |

## 9. Implementation Plan

### Phase 1: Build scaffolding and unit tests (red)

- [ ] Add `odin/BUILD.gn` with the `static_library("odin_protocol")` target from §5, and `odin/test/BUILD.gn` with the `executable("protocol_test")` target from §5; add `"//odin/test:protocol_test"` to the `group("tests")` deps in the root `BUILD.gn`.
- [ ] Add forward declarations of `FrameEncoder`, `FrameDecoder`, the enums, and the structs in a placeholder `odin/protocol.h` (no `.cc`) so the test compilation unit parses but the link step fails on the encoder/decoder symbols.
- [ ] Add `odin/test/protocol_test.cc` with one `TEST` per scenario S1–S8 from §8.
- [ ] Confirm `gn gen out/Default && ninja -C out/Default protocol_test` reaches the link step and reports unresolved symbols for `FrameEncoder::*` / `FrameDecoder::*`.

**Done when:** S1–S8 are present in `protocol_test.cc` and the binary either fails to link (missing symbols) or links and reports failures — the red state. The test-binary scaffold required by G3 is in place.

### Phase 2: Encoder/decoder implementation (green)

- [ ] Add `odin/protocol.cc` implementing `FrameEncoder::EncodeConnectRequest` (writes header + UTF-8 bytes), `FrameEncoder::EncodeConnectResponse` (writes header + 1 status byte), `FrameDecoder::DecodeConnectRequest` (header parse → `kMaxAuthorityLen` check → payload copy into `ConnectRequest::authority`), `FrameDecoder::DecodeConnectResponse` (header parse → length-must-equal-1 check → status byte copy).
- [ ] Build with `ninja -C out/Default protocol_test`, run `out/Default/protocol_test`, and confirm every S1–S8 case in the gtest output is marked `[       OK ]`.

**Done when:** All §8 scenarios pass green when `out/Default/protocol_test` is run after `ninja -C out/Default protocol_test`, satisfying G1 (the encoder output bytes in S1 and S2 match the spec), G2 (each `DecodeResult` enumerator is reached by S3–S8), and G3 (the `protocol_test` binary exits non-zero on any failure, gating the `ninja … tests` build).

## 10. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| `kMaxAuthorityLen = 260` rejects an authority a future caller wants to send (e.g., internationalized hostnames after Punycode expansion still exceed 253) | Low | Med | Cap is exposed as the single named constant `kMaxAuthorityLen` in `protocol.h`; raising it is a one-line change plus updating S6's expected length |
| Decoder consumes input incrementally but tests treat it as one-shot, missing partial-buffer bugs | Med | Med | S4 and S5 explicitly assert `*consumed` is left untouched on `kNeedMoreData`, exercising the partial-buffer contract documented in the `FrameDecoder` header comment |
| ALPN string drifts between client and server (typo on one side) and the tunnel silently negotiates a different protocol on the same UDP socket | Low | High | Both halves consume `kAlpn` from `protocol.h` rather than hard-coding the string at the call site; xquic's `xqc_engine_register_alpn` plus its `XQC_EALPN_NOT_SUPPORTED` error path enforce server-side rejection at handshake |

## 11. Future Work

- HTTPS_PROXY listener and `CONNECT` parser on the client side — deferred per §3 Non-Goals.
- c-ares-based DNS resolution and upstream TCP/TLS via boringssl on the server side — deferred per §3 Non-Goals.
- xquic engine bring-up, UDP socket I/O loop, and ALPN registration glue using `xqc_engine_register_alpn(engine, kAlpn, kAlpnLen, ...)` — deferred per §3 Non-Goals.
- Authentication (e.g., pre-shared key prefix in `CONNECT_REQUEST` payload) under a new `odin/2` ALPN — anticipated as the first compatibility-breaking revision and the reason the ALPN carries an explicit version suffix.

## 12. References

- xquic public API: `xquic/include/xquic/xquic.h` (`xqc_engine_create`, `xqc_engine_register_alpn`, `xqc_connect`, `xqc_stream_create`, `xqc_stream_send`, `xqc_stream_recv`, `xqc_stream_close`)
- xquic typedefs: `xquic/include/xquic/xquic_typedef.h`
- xquic error codes (`XQC_EALPN_NOT_SUPPORTED = 639`): `xquic/include/xquic/xqc_errno.h`
- xquic HQ demo (ALPN-registered application protocol example): `xquic/demo/xqc_hq.h`
- c-ares public API: `c-ares/include/ares.h` (`ares_library_init`, `ares_init_options`, `ares_getaddrinfo`, `ares_freeaddrinfo`, `ares_process_fds`, `ares_destroy`)
- googletest public API: `googletest/googletest/include/gtest/gtest.h`
- BoringSSL (used transitively by xquic's TLS layer): `boringssl/include/openssl/`
- Prior RFC template: `docs/rfc_000_design_doc_template.md`
