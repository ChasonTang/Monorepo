# RFC-018: Orchestrator-Driven CONNECT Handshake Session

## 1. Summary

Add `odin/connect_session.{c,h}` — a transport-agnostic polled state machine that performs the RFC-001 control-frame handshake on a caller-supplied `odin_transport_t` (RFC-013) by issuing every read and write through the `odin_transport_*` dispatchers, exposing `odin_connect_session_drive(session, t, events)` that an upper orchestrator invokes from its own `on_ready` body — installed once on the transport at construction (since RFC-013 binds `on_ready` at construction and exposes no setter, as RFC-014 §3.1 spells out) and used as a permanent trampoline that dispatches internally to `odin_connect_session_drive` while the session is alive and then to `odin_relay_ready` (RFC-014, called with the relay handle from the body's local state) after `odin_connect_session_destroy` returns — so the session itself binds no `odin_transport_ready_cb`; Client mode encodes one CONNECT_REQ from caller-supplied `(host, host_len, port)`, writes it through `odin_transport_write`, then reads through `odin_transport_read` until one CONNECT_RESP decodes, firing `on_done(OK, 0)` with the decoded `error_code` and the post-RESP byte slice exposed via accessors; Server mode reads through `odin_transport_read` until one CONNECT_REQ decodes, fires `on_req_decoded` exposing a host/port view plus the post-REQ byte slice via accessors, suspends with `wants() == 0` until the caller supplies the dial-result `error_code` via `odin_connect_session_server_set_error_code`, then writes one CONNECT_RESP through `odin_transport_write` and fires `on_done(OK, 0)`; faults from synchronous I/O, a peer half-close before the in-flight frame completes, an unexplained `ODIN_TRANSPORT_ERROR` readiness classified through `odin_transport_error`, and any codec `ODIN_PROTO_ERR_*` aggregate into a single `on_done(ERROR, err)`.

## 2. Goals

- **G1.** Provide a Client-mode session that, given caller-supplied `(host, host_len, port)`, drives one CONNECT_REQ encode-and-write through `odin_transport_write` and then one CONNECT_RESP read-and-decode through `odin_transport_read`, firing `on_done(ODIN_CONNECT_SESSION_OK, 0)` exactly once with the decoded `error_code` retrievable via `odin_connect_session_client_error_code(s)` and the trailing post-RESP byte slice (up to `ODIN_PROTO_CONNECT_REQ_MAX - ODIN_PROTO_CONNECT_RESP_SIZE = 256` bytes) retrievable as an aliased `(ptr, len)` view via `odin_connect_session_client_tail(s, ...)`.

- **G2.** Provide a Server-mode session that drives one CONNECT_REQ read-and-decode through `odin_transport_read`, fires `on_req_decoded` exactly once with the decoded host slice and port retrievable via `odin_connect_session_server_host(s, ...)` and `odin_connect_session_server_port(s)` and the trailing post-REQ byte slice (length bounded by `ODIN_PROTO_CONNECT_REQ_MAX - (5 + host_len) = 255 - host_len` bytes, so `≤ 254` for any decodable REQ where `host_len ≥ 1`) retrievable as an aliased `(ptr, len)` view via `odin_connect_session_server_tail(s, ...)`, suspends with `odin_connect_session_wants(s) == 0` until the caller invokes `odin_connect_session_server_set_error_code(s, error_code)` exactly once, then drives one CONNECT_RESP encode-and-write through `odin_transport_write` and fires `on_done(ODIN_CONNECT_SESSION_OK, 0)` exactly once when the four CONNECT_RESP bytes are fully transmitted.

- **G3.** Aggregate every fault into a single `on_done(ODIN_CONNECT_SESSION_ERROR, err)`: a synchronous `ODIN_TRANSPORT_IO_ERROR` from `odin_transport_read`/`odin_transport_write` propagates the transport's `errno` unchanged into `err`; an `ODIN_TRANSPORT_EOF` returned by `odin_transport_read` during `C_READING_RESP` or `S_READING_REQ` before the in-flight frame fully decodes maps to `err = ECONNRESET` (the peer half-closed before the handshake could complete); an `ODIN_TRANSPORT_ERROR` readiness that produced no synchronous fault is classified by calling `odin_transport_error(t)` (a `0` return is benign and re-arms the next readiness; a non-zero return becomes `err`); a decoder return of `ODIN_PROTO_ERR_BAD_VERSION` / `ODIN_PROTO_ERR_BAD_FRAME_TYPE` / `ODIN_PROTO_ERR_HOST_LEN_INVALID` maps to `err = EPROTO`.

- **G4.** The session never binds an `odin_transport_ready_cb`, never calls `odin_transport_set_interest`, and never registers any event-loop watch directly; the orchestrator owns the transport's `on_ready` binding (a single permanent body installed at construction, since RFC-013 binds `on_ready` at construction and exposes no setter), owns the watch through `odin_transport_set_interest`, and computes the next interest mask after every `odin_connect_session_drive` call from `odin_connect_session_wants(s)` — so the orchestrator's on_ready body transitions its internal dispatch target from `odin_connect_session_drive` to `odin_relay_ready` (RFC-014, called with the relay handle from the body's local state since `odin_relay_ready` takes `user_data` as a parameter) once the session has been destroyed, with no setter on the transport itself.

## 3. Design

### 3.1 Overview

`odin/connect_session` is a new leaf module under `odin/`. It depends only on `odin/transport` (RFC-013) for the `odin_transport_t` value and the `odin_transport_*` dispatcher functions, and on `odin/protocol` (RFC-001 wire format with the RFC-004 zero-copy encode + aliasing decode and the RFC-005 fixed-frame response encoder) for the codec entry points and the `ODIN_PROTO_*` constants and status enum. The module registers no event-loop watch, binds no `odin_transport_ready_cb`, never owns or destroys a transport, and never closes any fd; the caller (orchestrator) owns the transport, owns the watch through `odin_transport_set_interest`, and installs a single permanent `on_ready` body on the transport at construction (RFC-013 binds `on_ready` at construction and the interface exposes no setter — RFC-014 §3.1 spells this out for the relay's parallel constraint) that acts as a trampoline: while the session is alive the body dispatches each readiness to `odin_connect_session_drive(s, t, events)`, and once the session has been destroyed the same body dispatches to `odin_relay_ready(t, events, relay)` (passing the relay handle even though the transport's stored `user_data` is the orchestrator's own state, because `odin_relay_ready` takes `user_data` as a parameter).

The orchestrator holds the session handle and the caller-owned `odin_transport_t *`. Before creating the transport it installs its own `on_ready` body (the trampoline described above) — that binding is fixed for the transport's lifetime. After session construction, the orchestrator queries `odin_connect_session_wants(s)` to learn the initial interest mask — `ODIN_TRANSPORT_WRITE` for Client mode (to send the REQ), `ODIN_TRANSPORT_READ` for Server mode (to receive it) — and calls `odin_transport_set_interest(t, mask)` to arm the watch. Each readiness invokes the orchestrator's `on_ready`, which (while the session is alive) calls `odin_connect_session_drive(s, t, events)`; the session reads or writes through the dispatchers, advances its internal state, and returns `ODIN_CONNECT_SESSION_DRIVE_CONTINUE` if more drives are needed or `ODIN_CONNECT_SESSION_DRIVE_DONE` after `on_done` has fired. After every `drive`, the orchestrator reads `odin_connect_session_wants(s)` again and updates `odin_transport_set_interest` if the mask changed. For Server sessions, between `on_req_decoded` and `odin_connect_session_server_set_error_code`, `wants` returns `0` and the orchestrator clears the watch; the orchestrator runs its TCP dial outside the session and re-arms `WRITE` after `set_error_code`. On `on_done`, the orchestrator destroys the session and flips its trampoline's internal dispatch target to `odin_relay_ready`; the transport's `on_ready` binding never changes.

```
                  caller / orchestrator
    install ORCHESTRATOR's trampoline as t's on_ready at transport construction (NOT the session's)
    create_client(host,host_len,port,on_done,ud,&s)
    or create_server(on_req_decoded,on_done,ud,&s)
    loop:
        set_interest(t, wants(s));   await readiness
        in trampoline: if (s alive) drive(s, t, events) once;  if wants changed: re-set_interest
        on on_req_decoded (server): kick off TCP dial; on dial result:
            server_set_error_code(s, code);  re-set_interest
        on on_done: destroy(s);  trampoline's internal target flips to odin_relay_ready(t, events, relay)
                                 (t's on_ready binding is unchanged — RFC-013 has no setter)
                            |
                            v
        +-----------------------------------------------+
        |          odin_connect_session_t               |   (odin/connect_session.{c,h})
        |   uint8_t buf[260]  +  state  +  offsets      |   depends only on
        |   on_done / on_req_decoded callbacks          |   odin/transport.h and
        |   server: resp_frame[4] + error_code latch    |   odin/protocol.h
        +-----------------------------------------------+
            |  odin_transport_read / _write / _error  (NEVER set_interest, NEVER destroy)
            v
       +----------------------+
       |   odin_transport_t   |   (caller-owned; orchestrator owns the trampoline on_ready and set_interest)
       +----------------------+
            |  fd impl (RFC-013) or xqc impl (RFC-016) or any conforming sibling
            v
        peer
```

### 3.2 Detailed Design

#### 3.2.1 Public API, two-mode lifecycle, and orchestrator hand-off

Contract surface — `odin/connect_session.h` (include guard, copyright, and per-declaration doc-comments omitted; the `#include "odin/transport.h"` and `#include "odin/protocol.h"` lines are load-bearing because the API speaks `odin_transport_t *` and uses the `ODIN_PROTO_*` constants, and `extern "C"` is load-bearing because the test translation unit is C++):

```c
/* odin/connect_session.h */
#include <stddef.h>
#include <stdint.h>

#include "odin/protocol.h"
#include "odin/transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct odin_connect_session_t odin_connect_session_t;

typedef enum odin_connect_session_status_t {
  ODIN_CONNECT_SESSION_OK = 0,
  ODIN_CONNECT_SESSION_ERROR,
} odin_connect_session_status_t;

typedef enum odin_connect_session_drive_t {
  ODIN_CONNECT_SESSION_DRIVE_CONTINUE = 0,
  ODIN_CONNECT_SESSION_DRIVE_DONE,
} odin_connect_session_drive_t;

typedef void (*odin_connect_session_done_cb)(odin_connect_session_t *s,
                                             odin_connect_session_status_t status,
                                             int err, void *user_data);

typedef void (*odin_connect_session_req_decoded_cb)(odin_connect_session_t *s,
                                                    void *user_data);

int odin_connect_session_create_client(const char *host, size_t host_len,
                                        uint16_t port,
                                        odin_connect_session_done_cb on_done,
                                        void *user_data,
                                        odin_connect_session_t **out);

int odin_connect_session_create_server(
    odin_connect_session_req_decoded_cb on_req_decoded,
    odin_connect_session_done_cb on_done, void *user_data,
    odin_connect_session_t **out);

odin_connect_session_drive_t odin_connect_session_drive(
    odin_connect_session_t *s, odin_transport_t *t, unsigned int events);

unsigned int odin_connect_session_wants(const odin_connect_session_t *s);

void odin_connect_session_server_set_error_code(odin_connect_session_t *s,
                                                uint16_t error_code);

void odin_connect_session_server_host(const odin_connect_session_t *s,
                                      const char **out_host,
                                      size_t *out_host_len);
uint16_t odin_connect_session_server_port(const odin_connect_session_t *s);
void odin_connect_session_server_tail(const odin_connect_session_t *s,
                                      const uint8_t **out_ptr,
                                      size_t *out_len);

uint16_t odin_connect_session_client_error_code(
    const odin_connect_session_t *s);
void odin_connect_session_client_tail(const odin_connect_session_t *s,
                                      const uint8_t **out_ptr,
                                      size_t *out_len);

void odin_connect_session_destroy(odin_connect_session_t *s);

#ifdef __cplusplus
}
#endif
```

**Unstated contract.**

- *Construction and ownership.* `odin_connect_session_create_client` validates `host_len ∈ [1, ODIN_PROTO_HOST_MAX]` (otherwise returns `-1` with `errno == EINVAL` and `*out` untouched), copies the `host` bytes into the session's own `buf` along with the encoded CONNECT_REQ header and port, stores `on_done` / `user_data`, sets the initial state to `C_WRITING_REQ`, writes `*out`, and returns `0`; the only other failure is `ENOMEM` (returns `-1`, `errno == ENOMEM`, `*out` untouched). Once `create_client` returns `0`, the caller may free or reuse the `host` buffer immediately — the session does not alias it. `odin_connect_session_create_server` stores `on_req_decoded` / `on_done` / `user_data`, sets the initial state to `S_READING_REQ`, writes `*out`, and returns `0`; its only failure is `ENOMEM`. `on_done` must be non-null in both constructors; `on_req_decoded` must be non-null in `create_server`. The session never owns the transport: no entry point calls `odin_transport_destroy`, no entry point closes any fd, no entry point calls `odin_transport_set_interest`, no entry point registers any `odin_event_io_*` watch.
- *Drive semantics and the `wants()` contract.* The orchestrator installs its own `on_ready` body on the transport at construction time (i.e., when calling `odin_fd_transport_create` / a sibling constructor with its own `on_ready`); inside that body it calls `odin_connect_session_drive(s, t, events)`. `drive` is the only entry point that touches the transport — it issues `odin_transport_read` / `odin_transport_write` / `odin_transport_error` only, and only on the transport `t` the caller passes in (the session stores no `odin_transport_t *`). It is a precondition that `drive` is invoked only with the same transport that carries the peer of this handshake; the session does not enforce this (it does not store the transport to compare against). `drive` returns `ODIN_CONNECT_SESSION_DRIVE_DONE` once `on_done` has fired (the session is then terminal — subsequent `drive` calls return `DONE` and touch neither the session nor the transport); else `ODIN_CONNECT_SESSION_DRIVE_CONTINUE`. After every `drive`, the orchestrator calls `odin_connect_session_wants(s)` and re-invokes `odin_transport_set_interest(t, wants)` if the mask changed (orchestrator-side caching is optional; the session does not deduplicate). `wants` returns a subset of `ODIN_TRANSPORT_READ | ODIN_TRANSPORT_WRITE` per current state (§3.2.2 and §3.2.3), `0` for a Server session in `S_AWAIT_DIAL` or any terminal state (`DONE` / `ERROR`).
- *Server fill-back.* `odin_connect_session_server_set_error_code(s, error_code)` is a single-shot setter: precondition is that the session is in `S_AWAIT_DIAL` (i.e., `on_req_decoded` has already fired and `on_done` has not). Calling it from any other state (including a second time, or before `on_req_decoded` fires, or after `on_done` fires) is a caller misuse asserted via `<assert.h>` in debug builds and undefined in release; the session does not silently demote misuse to a status return. The setter encodes the four-byte `CONNECT_RESP` frame into the session's resp slot (via `odin_proto_encode_connect_resp`), advances the state to `S_WRITING_RESP`, and resets the write offset to `0`. It does no I/O; the next `drive` call with `events & WRITE` writes the bytes (§3.2.3).
- *Accessors and slice lifetimes.* `odin_connect_session_server_host(s, &out_host, &out_host_len)`, `odin_connect_session_server_port(s)`, and `odin_connect_session_server_tail(s, &out_ptr, &out_len)` are valid from the moment `on_req_decoded` fires until the session is destroyed; the `out_host` pointer aliases the session's internal `buf` at the host-bytes offset that `odin_proto_decode_connect_req` resolves (no NUL terminator appended), so the caller must finish using it before calling `odin_connect_session_destroy`. `server_tail`'s `out_ptr` slice aliases `s.buf` at the bytes following the decoded CONNECT_REQ (offset `5 + host_len`, i.e., bytes the client wrote in the same TCP segment after the REQ — e.g., pipelined upstream payload), with `out_len ∈ [0, ODIN_PROTO_CONNECT_REQ_MAX - (5 + host_len)] = [0, 255 - host_len]` (so `≤ 254` because the decoder requires `host_len ≥ 1`); the slice may be empty when the kernel delivered exactly the REQ in this read or when the client deferred its first post-REQ byte to a later segment. `odin_connect_session_client_error_code(s)` and `odin_connect_session_client_tail(s, &out_ptr, &out_len)` are valid from the moment `on_done` fires with `ODIN_CONNECT_SESSION_OK` until `destroy`; the `out_ptr` slice aliases the session's internal `buf` at the bytes following the decoded CONNECT_RESP (offset `ODIN_PROTO_CONNECT_RESP_SIZE`), with `out_len ∈ [0, ODIN_PROTO_CONNECT_REQ_MAX - ODIN_PROTO_CONNECT_RESP_SIZE] = [0, 256]`. Calling any accessor in any other state is caller misuse (debug `assert`, UB in release).
- *Completion and destroy.* `on_done` fires exactly once, on the owner thread, as the session's last action inside the `drive` call that produced the terminal transition; the session reads or writes no internal state after `on_done` returns, so `odin_connect_session_destroy(s)` from inside `on_done` is legal (and is the orchestrator's primary hand-off point to flip its trampoline to dispatch readiness into `odin_relay_ready`, per §3.1 — the transport's `on_ready` binding is permanent and has no setter, only the trampoline's internal dispatch target changes). `odin_connect_session_destroy(NULL)` is a no-op; `odin_connect_session_destroy(s)` on a session that never reached completion frees the session and never fires `on_done`. `destroy` performs no I/O — it does not touch the transport.
- *Threading.* All entry points and both callbacks run on the orchestrator's thread; the session adds no locks.

**Mechanism.**

```
create_client(host, host_len, port, on_done, ud, &out):
    if host_len < 1 or host_len > ODIN_PROTO_HOST_MAX:
        errno = EINVAL; return -1
    s = calloc(1, sizeof *s); if !s: errno = ENOMEM; return -1
    s.mode = CLIENT; s.state = C_WRITING_REQ
    s.on_done = on_done; s.user_data = ud
    odin_proto_iov_t iov[3]; uint8_t hdr[3]; uint8_t port_be[2]
    odin_proto_encode_connect_req(host, host_len, port, iov, hdr, port_be)  # OK by precondition above
    flatten iov[0..3) into s.buf[0..5 + host_len); s.write_total = 5 + host_len
    s.write_off = 0; s.buf_used = 0   # buf_used is the read-side cursor; the write phase reads from s.buf indexed by s.write_off
    *out = s; return 0

create_server(on_req_decoded, on_done, ud, &out):
    s = calloc(1, sizeof *s); if !s: errno = ENOMEM; return -1
    s.mode = SERVER; s.state = S_READING_REQ
    s.on_req_decoded = on_req_decoded; s.on_done = on_done; s.user_data = ud
    s.buf_used = 0; *out = s; return 0

wants(s):
    switch s.state:
        case C_WRITING_REQ:  return ODIN_TRANSPORT_WRITE
        case C_READING_RESP: return ODIN_TRANSPORT_READ
        case S_READING_REQ:  return ODIN_TRANSPORT_READ
        case S_AWAIT_DIAL:   return 0
        case S_WRITING_RESP: return ODIN_TRANSPORT_WRITE
        case DONE: case ERROR: return 0

destroy(s):
    if s == NULL: return
    free s   # never touches the transport
```

Satisfies: G1 via the `odin_connect_session_create_client` + `odin_connect_session_client_error_code` + `odin_connect_session_client_tail` surface bound to the `C_WRITING_REQ → C_READING_RESP → DONE` state flow detailed in §3.2.2; G2 via the `odin_connect_session_create_server` + `odin_connect_session_server_set_error_code` + `odin_connect_session_server_host` + `odin_connect_session_server_port` + `odin_connect_session_server_tail` surface bound to the `S_READING_REQ → S_AWAIT_DIAL → S_WRITING_RESP → DONE` state flow detailed in §3.2.3; G4 via the absence of any `odin_transport_set_interest` / `odin_transport_ready_cb` / event-loop-watch call in this header and in the `create_*` / `destroy` Mechanism blocks, the `wants()` accessor that returns the next interest mask to the orchestrator, and the orchestrator-installed `on_ready` model documented in the **Unstated contract**.

#### 3.2.2 Client phase machine: write CONNECT_REQ, read CONNECT_RESP, expose tail

Internal session state for Client mode:

```
s.buf[ODIN_PROTO_CONNECT_REQ_MAX]    # 260-byte accumulator; shared across write and read phases
s.write_total                         # bytes in s.buf to send (5 + host_len, computed at create)
s.write_off                           # bytes already sent; advances during C_WRITING_REQ
s.buf_used                            # bytes accumulated during C_READING_RESP; reset to 0 at phase swap
s.client_error_code                   # set when CONNECT_RESP decodes
s.client_tail_off                     # offset into s.buf where the post-RESP tail begins; always equals ODIN_PROTO_CONNECT_RESP_SIZE
```

State machine transitions (`drive(s, t, events)`):

```
drive_client(s, t, events):
    if s.state == DONE or s.state == ERROR: return DONE

    if s.state == C_WRITING_REQ and (events & ODIN_TRANSPORT_WRITE):
        do_write_req(s, t)
    elif s.state == C_READING_RESP and (events & ODIN_TRANSPORT_READ):
        do_read_resp(s, t)

    # ERROR readiness classification (also runs for events that did not match the current state)
    if s.state not in {DONE, ERROR} and (events & ODIN_TRANSPORT_ERROR):
        err = odin_transport_error(t)
        if err != 0:
            aggregate_error(s, err)

    if s.state in {DONE, ERROR}:
        fire_on_done_once(s)
        return DONE
    return CONTINUE

# C_WRITING_REQ: drain s.buf[write_off .. write_total) onto the transport.
do_write_req(s, t):
    while s.write_off < s.write_total:
        n = 0
        rc = odin_transport_write(t, s.buf + s.write_off, s.write_total - s.write_off, &n)
        if rc == OK:           s.write_off += n
        elif rc == AGAIN:      return
        elif rc == IO_ERROR:   aggregate_error(s, errno); return
        # write never returns EOF (RFC-013 §3.2.1 Unstated contract)
    # full REQ on the wire — flip to read phase
    s.state = C_READING_RESP
    s.buf_used = 0

# C_READING_RESP: read up to (cap - buf_used) bytes into s.buf, then try to decode.
do_read_resp(s, t):
    while s.state == C_READING_RESP:
        run = ODIN_PROTO_CONNECT_REQ_MAX - s.buf_used   # the §4 S1 buffer bound
        if run == 0: break                              # defensive: CONNECT_RESP is 4 bytes; a well-formed peer hits OK at buf_used == 4 ≪ 260, so this arm is unreachable under v1 — kept as a forward-compat guard
        n = 0
        rc = odin_transport_read(t, s.buf + s.buf_used, run, &n)
        if rc == OK:           s.buf_used += n
        elif rc == AGAIN:      return
        elif rc == EOF:        aggregate_error(s, ECONNRESET); return  # peer half-closed before RESP
        elif rc == IO_ERROR:   aggregate_error(s, errno); return

        consumed = 0; ec = 0
        pst = odin_proto_decode_connect_resp(s.buf, s.buf_used, &consumed, &ec)
        if pst == OK:
            s.client_error_code = ec
            # s.buf[consumed .. buf_used) is the post-RESP tail (0..256 bytes), exposed by client_tail()
            s.client_tail_off = consumed                # always equals ODIN_PROTO_CONNECT_RESP_SIZE
            s.state = DONE
            return
        elif pst == NEED_MORE:
            continue       # loop and try another read
        else:              # ERR_BAD_VERSION / ERR_BAD_FRAME_TYPE
            aggregate_error(s, EPROTO); return
```

**Unstated contract.**

- *Write-phase byte-exactness.* The `C_WRITING_REQ` phase writes exactly the bytes `odin_proto_encode_connect_req` flattens at `create_client` time — no headers added, no host bytes mutated, no trailing padding. The session does not poll for partial-iov ergonomics: it uses the iov encoder once at create and then walks a single contiguous `s.buf[0 .. write_total)` range, which is why the buffer cap fits the worst-case REQ (260 bytes) and not the iov sum (which is the same 5 + host_len bytes laid out across three slots).
- *Read-phase decode is the prefix-parser pattern.* The `do_read_resp` loop accumulates into `s.buf` and re-runs `odin_proto_decode_connect_resp` after every read until it returns `OK`, `NEED_MORE` short-circuits the loop only after a subsequent read returns `AGAIN`, and any `ERR_*` from the decoder is non-recoverable (no additional input would resolve it). Because `CONNECT_RESP` is fixed at 4 bytes and the buffer is 260 bytes, the loop is bounded by at most `ceil(260 / minimum-readable-bytes)` iterations — in practice one or two reads.
- *Post-RESP tail exposure.* `s.buf[client_tail_off .. buf_used)` is the post-RESP byte slice — bytes the server may have written immediately after `CONNECT_RESP` (e.g., the first bytes of the upstream stream) and that arrived in the same `odin_transport_read` call that completed the `RESP` frame. The slice length is bounded by `ODIN_PROTO_CONNECT_REQ_MAX - ODIN_PROTO_CONNECT_RESP_SIZE = 256` bytes. Any further bytes the server sent past byte 260 are not read by the session — they wait in the transport's receive buffer and are read by the relay on its first `READ` readiness after the orchestrator hands the on_ready slot over.
- *Peer half-close before RESP is a fault.* If `odin_transport_read` returns `EOF` while the session is still in `C_READING_RESP` with `buf_used < 4` (or with the accumulated bytes still pending `NEED_MORE`), the session has no way to complete the handshake — the peer is gone. The session classifies this as `ERROR` with `err == ECONNRESET`, matching the convention RFC-013's `T9` established for peer-driven half-close on the read side of a handshake stream.

Satisfies: G1 via the `C_WRITING_REQ → C_READING_RESP → DONE` state transitions that issue exactly one CONNECT_REQ encode-and-write and one CONNECT_RESP read-and-decode, the bounded 260-byte buffer that yields the `client_tail` slice of up to 256 bytes pinned by the buffer cap, and the `client_error_code` field set from `odin_proto_decode_connect_resp`'s `out_error_code`; G3 via the `IO_ERROR`-captures-`errno` paths in `do_write_req`/`do_read_resp`, the EOF-before-RESP classification, and the decoder `ERR_*` → `EPROTO` mapping; G4 via the absence of any `odin_transport_set_interest` / `odin_transport_ready_cb` call in either Mechanism block.

#### 3.2.3 Server phase machine: read CONNECT_REQ, suspend, write CONNECT_RESP

Internal session state for Server mode:

```
s.buf[ODIN_PROTO_CONNECT_REQ_MAX]    # 260-byte accumulator; carries the REQ host bytes and any same-segment post-REQ tail for the lifetime of the session
s.buf_used                            # bytes accumulated during S_READING_REQ; persists past on_req_decoded so server_tail can compute (buf_used - server_tail_off)
s.req_view                            # odin_proto_connect_req_view_t (host_off, host_len, port) — aliases s.buf
s.server_tail_off                     # offset into s.buf where the post-REQ tail begins; always equals 5 + req_view.host_len
s.resp_frame                          # odin_proto_connect_resp_frame_t (4 bytes) — written by set_error_code
s.resp_write_off                      # bytes already sent during S_WRITING_RESP
```

State machine transitions (`drive(s, t, events)`):

```
drive_server(s, t, events):
    if s.state == DONE or s.state == ERROR: return DONE

    if s.state == S_READING_REQ and (events & ODIN_TRANSPORT_READ):
        do_read_req(s, t)
    elif s.state == S_WRITING_RESP and (events & ODIN_TRANSPORT_WRITE):
        do_write_resp(s, t)
    # S_AWAIT_DIAL: drive() consumes the events but issues no read/write — the session has no work
    # until the caller invokes server_set_error_code; the orchestrator should have cleared the watch
    # already, but a stray readiness is tolerated as a no-op.

    if s.state not in {DONE, ERROR} and (events & ODIN_TRANSPORT_ERROR):
        err = odin_transport_error(t)
        if err != 0:
            aggregate_error(s, err)

    if s.state in {DONE, ERROR}:
        fire_on_done_once(s)
        return DONE
    return CONTINUE

# S_READING_REQ: read into s.buf, then try to decode the REQ. On OK, fire on_req_decoded
# and suspend in S_AWAIT_DIAL.
do_read_req(s, t):
    while s.state == S_READING_REQ:
        run = ODIN_PROTO_CONNECT_REQ_MAX - s.buf_used   # the §4 S1 buffer bound
        if run == 0:
            aggregate_error(s, EPROTO); return  # defensive: under v1 host_len ∈ [1, 255], so a well-formed REQ totals 5 + host_len ≤ 260 bytes and the decoder returns OK at buf_used == 5 + host_len before this arm can fire — kept as a forward-compat guard
        n = 0
        rc = odin_transport_read(t, s.buf + s.buf_used, run, &n)
        if rc == OK:           s.buf_used += n
        elif rc == AGAIN:      return
        elif rc == EOF:        aggregate_error(s, ECONNRESET); return
        elif rc == IO_ERROR:   aggregate_error(s, errno); return

        consumed = 0
        pst = odin_proto_decode_connect_req(s.buf, s.buf_used, &consumed, &s.req_view)
        if pst == OK:
            # s.buf[consumed .. buf_used) is the post-REQ tail (0..254 bytes), exposed by server_tail()
            s.server_tail_off = consumed                # always equals 5 + s.req_view.host_len
            s.state = S_AWAIT_DIAL
            s.on_req_decoded(s, s.user_data)    # may not destroy the session (precondition)
            return
        elif pst == NEED_MORE:
            continue
        else:              # ERR_BAD_VERSION / ERR_BAD_FRAME_TYPE / ERR_HOST_LEN_INVALID
            aggregate_error(s, EPROTO); return

# server_set_error_code: encodes the 4-byte RESP frame and flips to S_WRITING_RESP.
server_set_error_code(s, error_code):
    assert s.state == S_AWAIT_DIAL
    odin_proto_encode_connect_resp(error_code, &s.resp_frame)   # void return (RFC-005)
    s.resp_write_off = 0
    s.state = S_WRITING_RESP

# S_WRITING_RESP: drain the 4-byte resp_frame onto the transport.
do_write_resp(s, t):
    while s.resp_write_off < ODIN_PROTO_CONNECT_RESP_SIZE:
        n = 0
        rc = odin_transport_write(t,
                                  s.resp_frame.bytes + s.resp_write_off,
                                  ODIN_PROTO_CONNECT_RESP_SIZE - s.resp_write_off,
                                  &n)
        if rc == OK:           s.resp_write_off += n
        elif rc == AGAIN:      return
        elif rc == IO_ERROR:   aggregate_error(s, errno); return
    # all 4 bytes on the wire — handshake complete
    s.state = DONE
```

**Unstated contract.**

- *REQ host bytes persist for the session lifetime.* The decoder returns a `{host_off, host_len, port}` view that aliases `s.buf` (RFC-004 §4.2.2); the session keeps `s.buf` intact through `S_AWAIT_DIAL` and through `S_WRITING_RESP` (the RESP is written from a separate `s.resp_frame` slot, not by overlaying `s.buf`), so the host pointer the caller obtains from `odin_connect_session_server_host` stays valid from `on_req_decoded` through `on_done` until the caller calls `odin_connect_session_destroy`. No NUL terminator is appended; callers that want a NUL-terminated string copy the slice into their own buffer.
- *`on_req_decoded` is a notification, not a re-entry point.* The session is mid-`drive` when `on_req_decoded` fires (the `do_read_req` Mechanism block invokes the callback inline and returns immediately afterwards), so the callback must not call `drive` again on this session and must not destroy the session — the call stack would unwind into freed state. The expected pattern is for the callback to kick off the orchestrator's TCP dial asynchronously (capturing `s` plus a host-copy made by `odin_connect_session_server_host`) and return promptly; the dial's completion callback later invokes `odin_connect_session_server_set_error_code(s, code)` on the same thread and updates `set_interest` from the new `wants()` value.
- *Post-REQ tail exposure.* `s.buf[server_tail_off .. buf_used)` is the post-REQ byte slice — bytes the client wrote immediately after `CONNECT_REQ` (e.g., a client that pipelined upstream payload into the same TCP segment as the REQ instead of waiting for the RESP) and that arrived in the same `odin_transport_read` call that completed the REQ frame. The session reads with `len = ODIN_PROTO_CONNECT_REQ_MAX - buf_used` (the §4 S1 bound), so up to `260 - (5 + host_len) = 255 - host_len` of those same-segment bytes are absorbed into `s.buf` past the REQ, and the orchestrator retrieves them via `odin_connect_session_server_tail` instead of dropping them at destroy. The slice length is bounded by `255 - host_len` bytes (so `≤ 254` for any decodable REQ, since `host_len ≥ 1`). Any further bytes the client sent past byte 260 in this read, plus any bytes the client sent in subsequent segments before the orchestrator re-arms `READ` on the relay, sit in the transport's receive buffer and are picked up by `odin_relay_ready`'s first read after the orchestrator flips the trampoline's internal dispatch target.
- *Stray readiness in `S_AWAIT_DIAL` is benign.* If the orchestrator left a `READ` interest armed (or the loop delivers a level-triggered event before `set_interest(0)` reaches the kernel), `drive` enters `S_AWAIT_DIAL` with `events & ODIN_TRANSPORT_READ`; the Mechanism does nothing in this state and returns `CONTINUE` without consuming or buffering bytes. An `ODIN_TRANSPORT_ERROR` readiness in `S_AWAIT_DIAL` is still classified through `odin_transport_error` so a peer-side reset during the dial wait surfaces as `on_done(ERROR, err)` rather than dangling forever.

Satisfies: G2 via the `S_READING_REQ → S_AWAIT_DIAL → S_WRITING_RESP → DONE` state transitions that issue exactly one CONNECT_REQ read-and-decode, fire `on_req_decoded` with the `req_view` exposed via `server_host`/`server_port` and the same-segment post-REQ bytes exposed via `server_tail` (`s.buf[server_tail_off .. buf_used)` of up to 254 bytes), suspend with `wants() == 0`, and after `server_set_error_code` issue exactly one CONNECT_RESP encode-and-write before firing `on_done(OK)`; G3 via the `IO_ERROR`-captures-`errno` paths in `do_read_req`/`do_write_resp`, the EOF-before-REQ classification, the decoder `ERR_*` → `EPROTO` mapping, and the `S_AWAIT_DIAL` `ODIN_TRANSPORT_ERROR` classification; G4 via the absence of any `odin_transport_set_interest` / `odin_transport_ready_cb` call in either Mechanism block.

#### 3.2.4 Error aggregation, exactly-once `on_done`, and buffer-length bound

```
aggregate_error(s, err):
    if s.state == ERROR: return         # first fault wins; subsequent faults dropped
    s.state = ERROR
    s.err = err

fire_on_done_once(s):
    if s.on_done_fired: return
    s.on_done_fired = 1
    status = (s.state == DONE) ? ODIN_CONNECT_SESSION_OK : ODIN_CONNECT_SESSION_ERROR
    err    = (s.state == DONE) ? 0                       : s.err
    cb     = s.on_done; ud = s.user_data        # capture before the call: destroy from inside on_done frees s
    cb(s, status, err, ud)                      # nothing in drive() touches s after this returns
```

**Unstated contract.**

- *The first fault wins.* `aggregate_error` is gated on `s.state == ERROR`; once the session is in `ERROR`, every subsequent fault on the same `drive` call (e.g., a synchronous `IO_ERROR` followed by the same readiness's `ODIN_TRANSPORT_ERROR` bit) is dropped. This mirrors RFC-014 §3.2.4: a same-batch sibling fault does not get a second `on_done`.
- *`on_done` is the last action of `drive`.* `drive_client` and `drive_server` call `fire_on_done_once` only after they have set the terminal state and only at the very end of the call, after `do_read_*` / `do_write_*` and after the `ODIN_TRANSPORT_ERROR` classification. Because the callback captures `s.on_done` and `s.user_data` into local variables before invoking the function pointer, the session struct is never read after the callback returns, so a `destroy(s)` from inside `on_done` is safe. The `on_done_fired` latch makes a second drive that somehow reaches `fire_on_done_once` (e.g., a defensive call after a stray readiness) a no-op.
- *Buffer-length bound is the security invariant.* `do_read_resp` and `do_read_req` both compute `run = ODIN_PROTO_CONNECT_REQ_MAX - s.buf_used` before calling `odin_transport_read`, so the transport never receives a length larger than the free capacity of `s.buf`. `s.buf_used` is monotone non-decreasing within a phase, so `run` is non-negative; `run == 0` cannot occur in `C_READING_RESP` for a well-formed peer (the decoder returns `OK` long before the buffer fills) and is treated as a protocol fault in `S_READING_REQ` (a malformed REQ that fills the buffer without becoming decodable maps to `EPROTO`). No allocation happens after `create_*`. The §4 S1 mitigation is exactly this `run` computation; the §5 row that fires the trigger asserts `odin_transport_read` is invoked with `len ≤ 260 - already_buffered` on every call.

Satisfies: G3 via the `aggregate_error` latch that yields exactly one `ODIN_CONNECT_SESSION_ERROR` outcome and the `fire_on_done_once` latch that pairs each terminal state with exactly one `on_done` invocation; this subsection also pins the §4 S1 enforcement point.

## 4. Security

The session moves bytes between the orchestrator-owned `odin_transport_t` and a 260-byte session-internal accumulator. The peer's bytes (the server in Client mode, the connecting client in Server mode) cross a trust boundary the session itself enforces: the byte-length bound on `odin_transport_read` is computed inside this module rather than inside the transport (RFC-013 §4 explicitly notes the fd transport "moves opaque bytes between a caller-owned, caller-sized buffer ... and the socket" — the session is the caller-side enforcement point). The QUIC-layer TLS that the xqc transport (RFC-016) provides authenticates the peer but does not vouch for the content shape, so a malformed CONNECT_REQ / CONNECT_RESP from an authenticated peer still has to be rejected at the decoder boundary.

- **S1.**
  - **Threat:** Buffer overflow into the session's 260-byte `s.buf` accumulator from a flooding or malformed peer. The trigger is either direction's `odin_transport_read` — the server side of a Client session, or the client side of a Server session — whose peer sends more bytes than the next decode needs (e.g., a CONNECT_RESP arriving in a TCP packet that also carries the first 1024 bytes of the upstream stream; or a malformed CONNECT_REQ that claims `host_len = 255` while only 50 bytes have arrived but the kernel buffer holds 4 KiB of arbitrary junk past the frame). If `do_read_resp` / `do_read_req` passed a length larger than the free capacity of `s.buf`, the transport would write past the buffer end and corrupt the session struct or adjacent allocations.
  - **Mitigation:** §3.2.4 pins the invariant: every `odin_transport_read` call in `do_read_resp` (§3.2.2) and `do_read_req` (§3.2.3) bounds its `len` argument to `ODIN_PROTO_CONNECT_REQ_MAX - s.buf_used`, which is `≤ 260 - s.buf_used` at every loop iteration. `s.buf_used` is monotone non-decreasing within a phase, so the bound shrinks (never grows); the session never re-enters a read phase after `s.buf_used == 260`. The decoders short-circuit well before the bound binds: `odin_proto_decode_connect_resp` returns `OK` at `buf_used == 4`, and `odin_proto_decode_connect_req` returns `OK` at `buf_used == 5 + host_len ≤ 260` or `ERR_HOST_LEN_INVALID` at `buf_used == 3` for `host_len == 0`. Surplus peer bytes wait in the transport's receive buffer (kernel socket buffer for the fd transport, xqc stream buffer for the xqc transport).
  - **Enforcement:** §5 row T15 (Client) and T16 (Server) script the fake transport's `read` slot to deliver a valid CONNECT_RESP / CONNECT_REQ prefix one or two bytes at a time across multiple readiness deliveries — each chunk small enough that the decoder stays in `NEED_MORE` and the session loops back into another read with a non-zero `buf_used` — record every `(len, buf_used_at_call_time)` pair the session passes to `odin_transport_read`, and assert that on every call `len == ODIN_PROTO_CONNECT_REQ_MAX - buf_used = 260 - buf_used` (so a regression that passed an unbounded length, the raw kernel buffer size, or any value `> 260 - buf_used` would fail the row). The P2 AddressSanitizer command additionally runs the full suite (including T15 and T16) and reports no heap-buffer-overflow or out-of-bounds-write, backing up the per-call invariant assertion with an out-of-band overflow detector.

## 5. Testing Strategy

Rows `T1`–`T27` are unit tests in `connect_session_unittests.cpp`. `T1`–`T26` drive the session against a test-local fake transport — a struct embedding `odin_transport_t` as its first member, whose vtable slots serve scripted `read`/`write`/`error` results, record every `set_interest` call (asserted zero in every `T1`–`T26` row, the G4 check), and refuse `destroy` calls (asserted zero in every `T1`–`T26` row, the never-owns-transport check). The fake's `read` slot additionally records every call as a `(len, n_served_before_this_call)` pair, where `n_served_before_this_call` is the fake's running tally of bytes it has previously returned on `OK` returns for this session; this tally mirrors `s.buf_used` at call time exactly (the session's only writer of `s.buf_used` is `do_read_resp` / `do_read_req` performing `s.buf_used += n` after each `OK` read), so T15/T16 assert the §4 S1 invariant `len == ODIN_PROTO_CONNECT_REQ_MAX - n_served_before_this_call` per call without reaching into the session's private state. Readiness is injected by calling `odin_connect_session_drive(s, &fake->base, events)` directly; the orchestrator's role is played inline by the test, which queries `odin_connect_session_wants(s)` after every `drive` and records any change (without actually calling `set_interest` because the fake's vtable does not connect to a real event loop). These rows use no event loop, no fd, and no real socket, which is what lets them assert the session forwards purely through the dispatchers and is genuinely transport-agnostic. `T27` is a constructor-validation row that instantiates no fake transport at all — `odin_connect_session_create_client` takes no transport argument and does no I/O on its `-1` / `EINVAL` early-return path, so the row asserts the synchronous `errno` / return / `*out`-untouched contract directly.

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | Client OK round-trip, no tail; destroy-from-`on_done` | Fake `t`; define `on_done` to record `(status, err)` plus `client_error_code(s)` and the `client_tail(s, &p, &l)` slice into per-test slots **before** calling `odin_connect_session_destroy(s)` from inside the callback (the §3.2.1 *Completion and destroy* contract — the orchestrator's primary hand-off point that flips the trampoline's internal dispatch target to `odin_relay_ready`); `create_client("example.com", 11, 443, on_done, ud, &s)`; assert `wants(s) == ODIN_TRANSPORT_WRITE`; script `t.write` to absorb the full 16-byte REQ in one call; `drive(s, &t, WRITE)`; assert `wants(s) == ODIN_TRANSPORT_READ`; script `t.read` to deliver the 4 bytes `{0x01, 0x02, 0x00, 0x00}` then `AGAIN`; `drive(s, &t, READ)` | The second `drive` returns `ODIN_CONNECT_SESSION_DRIVE_DONE` and does not touch `s` after the callback returns (so the destroy-from-`on_done` did not corrupt the unwind); the in-callback record shows `on_done` fired exactly once with `(ODIN_CONNECT_SESSION_OK, 0)`, `client_error_code(s) == 0`, and `client_tail` `l == 0`; under the P2 AddressSanitizer command the row reports no heap-use-after-free (the §3.2.4 *`on_done` is the last action of `drive`* invariant — `cb`/`ud` captured into locals before the call); `t.write` recorded one call whose buffer byte-equals the bytes `odin_proto_encode_connect_req` flattens for `("example.com", 11, 443)`; the fake's `set_interest` and `destroy` slots were never invoked (the session-level `odin_connect_session_destroy(s)` does no transport I/O per the §3.2.1 *Completion and destroy* contract) | G1, G4 | unit |
| T2 | Client OK with tail bytes (server pipelined the first 7 bytes of upstream data into the same packet as CONNECT_RESP) | Fake `t`; `create_client("a", 1, 80, ...)`; drive WRITE to completion; script `t.read` to deliver `{0x01, 0x02, 0x00, 0x00, 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA}` (4 RESP bytes + 7 tail bytes) in one call; `drive(s, &t, READ)` | `on_done(OK, 0)`; `client_error_code(s) == 0`; `client_tail(s, &p, &l)` yields `l == 7` and `memcmp(p, "\xDE\xAD\xBE\xEF\xCA\xFE\xBA", 7) == 0`; `set_interest` and `destroy` slots never invoked | G1, G4 | unit |
| T3 | Client decoder rejects bad version | Fake `t`; `create_client("a", 1, 80, ...)`; drive WRITE; script `t.read` → `{0xFF, 0x02, 0x00, 0x00}` then `AGAIN`; `drive(s, &t, READ)` | `on_done(ERROR, EPROTO)` exactly once; `client_error_code(s)` is undefined and the test does not call it; `set_interest` / `destroy` never invoked | G3, G4 | unit |
| T4 | Client decoder rejects bad frame_type | Fake `t`; same as T3 but script `t.read` → `{0x01, 0x03, 0x00, 0x00}` | `on_done(ERROR, EPROTO)` exactly once | G3 | unit |
| T5 | Client RESP arrives across multiple READ readiness deliveries | Fake `t`; drive WRITE; script `t.read` to return `{0x01}` then `AGAIN`, then on the next call `{0x02}` then `AGAIN`, then `{0x12, 0x34, 0x99}` then `AGAIN`; `drive(s, &t, READ)` three times | After the first two drives `wants(s) == READ` and `on_done` has not fired; after the third drive `on_done(OK, 0)` fires; the accumulated buffer is `{0x01, 0x02, 0x12, 0x34, 0x99}` and the decoder consumes the first 4 bytes, so `client_error_code(s) == 0x1234` (big-endian read of `buf[2..3]`) and `client_tail` yields `l == 1` and `p[0] == 0x99` (the single trailing byte the third read delivered after the 4-byte RESP) | G1 | unit |
| T6 | Client write IO_ERROR (e.g., peer half-closed before reading) | Fake `t`; `create_client(...)`; script `t.write` → `IO_ERROR` with `errno = EPIPE`; `drive(s, &t, WRITE)` | `on_done(ERROR, EPIPE)` exactly once | G3 | unit |
| T7 | Client read IO_ERROR | Fake `t`; drive WRITE to completion; script `t.read` → `IO_ERROR` with `errno = ECONNRESET`; `drive(s, &t, READ)` | `on_done(ERROR, ECONNRESET)` exactly once | G3 | unit |
| T8 | Client ERROR readiness with no synchronous fault classified through `odin_transport_error` | Fake `t`; drive WRITE to completion; script `t.read` → `AGAIN` and `t.error` → `ECONNRESET`; `drive(s, &t, READ | ERROR)` | `on_done(ERROR, ECONNRESET)` fires exactly once; the `t.error` slot was called once; the `t.read` slot was called once (the session attempted the read before classifying ERROR) | G3 | unit |
| T9 | Client EOF before RESP completes | Fake `t`; drive WRITE; script `t.read` → `EOF` with `buf_used == 0`; `drive(s, &t, READ)` | `on_done(ERROR, ECONNRESET)` exactly once — the peer hung up before the RESP arrived | G3 | unit |
| T10 | Server OK round-trip with no pipelined tail; suspend/resume across set_error_code; destroy-from-`on_done` | Fake `t`; define `on_done` to record `(status, err)` into per-test slots **before** calling `odin_connect_session_destroy(s)` from inside the callback (the §3.2.1 *Completion and destroy* contract — the orchestrator's primary hand-off point that flips the trampoline's internal dispatch target to `odin_relay_ready`); `create_server(on_req_decoded, on_done, ud, &s)`; assert `wants(s) == READ`; script `t.read` to deliver the 16-byte REQ for `("example.com", 11, 443)` then `AGAIN`; `drive(s, &t, READ)`; assert `on_req_decoded` fired exactly once; `server_host(s, &h, &hl)` yields `hl == 11` and `memcmp(h, "example.com", 11) == 0`; `server_port(s) == 443`; assert `wants(s) == 0`; `server_set_error_code(s, 0)`; assert `wants(s) == WRITE`; script `t.write` to absorb 4 bytes; `drive(s, &t, WRITE)` | The second `drive` returns `ODIN_CONNECT_SESSION_DRIVE_DONE` and does not touch `s` after the callback returns (so the destroy-from-`on_done` did not corrupt the unwind); the in-callback record shows `on_done` fired exactly once with `(ODIN_CONNECT_SESSION_OK, 0)`; under the P2 AddressSanitizer command the row reports no heap-use-after-free (the §3.2.4 *`on_done` is the last action of `drive`* invariant — `cb`/`ud` captured into locals before the call); `t.write` recorded one call whose buffer byte-equals `{0x01, 0x02, 0x00, 0x00}` (the RFC-005 RESP for `error_code == 0`); the fake's `set_interest` and `destroy` slots were never invoked (the session-level `odin_connect_session_destroy(s)` does no transport I/O per the §3.2.1 *Completion and destroy* contract) | G2, G4 | unit |
| T11 | Server REQ arrives across multiple READ readiness deliveries; non-OK error_code | Fake `t`; `create_server(...)`; script `t.read` to return `{0x01, 0x01, 0x03, 'a'}` then `AGAIN`, then on the next call `{'b', 'c', 0x01, 0xBB}` then `AGAIN`; `drive(s, &t, READ)` twice | After the first drive `on_req_decoded` has not fired; after the second drive `on_req_decoded` fires; `server_host` yields `("abc", 3)`; `server_port(s) == 0x01BB == 443`; `server_set_error_code(s, 0x000A)` → next drive WRITE emits `{0x01, 0x02, 0x00, 0x0A}`; `on_done(OK, 0)` fires | G2 | unit |
| T12 | Server decoder rejects bad host_len (== 0) | Fake `t`; `create_server(...)`; script `t.read` → `{0x01, 0x01, 0x00, ...}` (host_len = 0); `drive(s, &t, READ)` | `on_done(ERROR, EPROTO)` exactly once; `on_req_decoded` never fires | G3 | unit |
| T13 | Server decoder rejects bad version | Fake `t`; script `t.read` → `{0xFF, 0x01, 0x03, 'a', 'b', 'c', 0x01, 0xBB}`; `drive(s, &t, READ)` | `on_done(ERROR, EPROTO)` exactly once; `on_req_decoded` never fires | G3 | unit |
| T14 | Server CONNECT_RESP write splits across two WRITE readiness deliveries | Fake `t`; complete REQ read as in T10; `server_set_error_code(s, 0xABCD)`; script `t.write` → first call writes 2 bytes then `AGAIN`, second call writes the remaining 2 bytes; `drive(s, &t, WRITE)` twice | After the first drive `wants(s) == WRITE` and `on_done` has not fired; after the second drive `on_done(OK, 0)` fires; concatenated `t.write` buffer byte-equals `{0x01, 0x02, 0xAB, 0xCD}` | G2 | unit |
| T15 | Client read length is bounded to `260 - buf_used` on every call (S1 enforcement) | Fake `t`; drive WRITE to completion; script `t.read` so the valid CONNECT_RESP `{0x01, 0x02, 0x00, 0x00}` arrives one or two bytes at a time, the first two chunks small enough that `odin_proto_decode_connect_resp` returns `NEED_MORE` after the read and the third chunk landing the 4th byte to decode `OK`: `{0x01}` then `AGAIN`, then on the next call `{0x02}` then `AGAIN`, then `{0x00, 0x00}` then `AGAIN` (the first two OK reads stay in `NEED_MORE` because they leave `buf_used ∈ {1, 2}`, both within the v1 RESP decoder's `NEED_MORE` regime `n ∈ {0, 1, 2, 3}`); the fake records every `(len, buf_used_at_call_time)` pair the session passes to `odin_transport_read`; `drive(s, &t, READ)` three times | Every recorded `(len, buf_used)` pair satisfies the §4 S1 invariant `len == ODIN_PROTO_CONNECT_REQ_MAX - buf_used = 260 - buf_used` (so a regression that passed an unbounded length, the raw kernel buffer size, or any value `> 260 - buf_used` would fail the row); concretely the three `OK`-returning reads recorded `(260, 0)`, `(259, 1)`, `(258, 2)`, and each `AGAIN`-returning read recorded `(260 - buf_used, buf_used)` for the `buf_used` value reached so far; after the third drive `on_done(OK, 0)` fires with `client_error_code(s) == 0x0000` and `client_tail` yields `l == 0`; under the P2 AddressSanitizer command the row reports no heap-buffer-overflow | S1 | unit |
| T16 | Server read length is bounded to `260 - buf_used` on every call (S1 enforcement) | Fake `t`; `create_server(...)`; script `t.read` so a valid CONNECT_REQ for host `"A"` / port `0x01BB` arrives one or two bytes at a time, each chunk small enough that `odin_proto_decode_connect_req` returns `NEED_MORE` after the read: `{0x01}` then `AGAIN`, then on the next call `{0x01}` then `AGAIN`, then `{0x01, 'A', 0x01, 0xBB}` then `AGAIN` (the first two OK reads stay in `NEED_MORE` because they leave `buf_used ∈ {1, 2}`, the v1 REQ decoder's `NEED_MORE` regime before host_len has been parsed); the fake records every `(len, buf_used_at_call_time)` pair passed to `odin_transport_read`; `drive(s, &t, READ)` three times | Every recorded `(len, buf_used)` pair satisfies the §4 S1 invariant `len == ODIN_PROTO_CONNECT_REQ_MAX - buf_used = 260 - buf_used`; concretely the three `OK`-returning reads recorded `(260, 0)`, `(259, 1)`, `(258, 2)`, and each `AGAIN`-returning read recorded `(260 - buf_used, buf_used)`; after the third drive `on_req_decoded` has fired exactly once with `server_host` yielding `("A", 1)` and `server_port(s) == 0x01BB`, and `wants(s) == 0` (session is suspended in `S_AWAIT_DIAL`); under the P2 AddressSanitizer command the row reports no heap-buffer-overflow or out-of-bounds-write | S1 | unit |
| T17 | Server OK with pipelined tail bytes (client wrote `"a"`/port `443` REQ then 7 upstream bytes in the same TCP segment) | Fake `t`; `create_server(on_req_decoded, on_done, ud, &s)`; assert `wants(s) == ODIN_TRANSPORT_READ`; script `t.read` to deliver `{0x01, 0x01, 0x01, 0x61, 0x01, 0xBB, 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA}` (6 REQ bytes for `("a", 1, 443)` + 7 pipelined tail bytes) in one call then `AGAIN`; `drive(s, &t, READ)` | `on_req_decoded` fires exactly once; `server_host(s, &h, &hl)` yields `hl == 1` and `h[0] == 'a'`; `server_port(s) == 0x01BB == 443`; `server_tail(s, &p, &l)` yields `l == 7` and `memcmp(p, "\xDE\xAD\xBE\xEF\xCA\xFE\xBA", 7) == 0` (so a regression that absorbed-and-dropped the pipelined bytes would surface `l == 0`); `wants(s) == 0` (suspended in `S_AWAIT_DIAL`); `on_done` has not fired; `set_interest` / `destroy` slots never invoked | G2, G4 | unit |
| T18 | Server read IO_ERROR | Fake `t`; `create_server(on_req_decoded, on_done, ud, &s)`; script `t.read` → `IO_ERROR` with `errno = ECONNRESET`; `drive(s, &t, READ)` | `on_done(ERROR, ECONNRESET)` exactly once; `on_req_decoded` never fires | G3 | unit |
| T19 | Server EOF before REQ completes | Fake `t`; `create_server(...)`; script `t.read` → `EOF` (`buf_used == 0`); `drive(s, &t, READ)` | `on_done(ERROR, ECONNRESET)` exactly once — the peer hung up before the REQ arrived (the explicit `S_READING_REQ` → `ECONNRESET` branch in §3.2.3 / G3); `on_req_decoded` never fires | G3 | unit |
| T20 | Server write IO_ERROR | Fake `t`; complete the REQ read as in T10 so `on_req_decoded` fires and the session enters `S_AWAIT_DIAL`; `server_set_error_code(s, 0)`; assert `wants(s) == WRITE`; script `t.write` → `IO_ERROR` with `errno = EPIPE`; `drive(s, &t, WRITE)` | `on_done(ERROR, EPIPE)` exactly once | G3 | unit |
| T21 | Server ERROR readiness in `S_AWAIT_DIAL` with no synchronous fault classified through `odin_transport_error` | Fake `t`; complete the REQ read as in T10 so `on_req_decoded` fires and the session enters `S_AWAIT_DIAL`; do **not** call `server_set_error_code` (stay in `S_AWAIT_DIAL`); assert `wants(s) == 0`; script `t.error` → `ECONNRESET`; `drive(s, &t, ERROR)` | `on_done(ERROR, ECONNRESET)` fires exactly once (the §3.2.3 Stray readiness paragraph's design point — a peer reset during the dial wait surfaces rather than dangles); the `t.error` slot was called once; the `t.read` / `t.write` slots were not called in this drive (no I/O in `S_AWAIT_DIAL`); `set_interest` / `destroy` slots never invoked | G3, G4 | unit |
| T22 | Server ERROR readiness in `S_READING_REQ` with no synchronous fault classified through `odin_transport_error` | Fake `t`; `create_server(on_req_decoded, on_done, ud, &s)`; assert `wants(s) == ODIN_TRANSPORT_READ`; script `t.read` → `AGAIN` and `t.error` → `ECONNRESET`; `drive(s, &t, READ | ERROR)` | `on_done(ERROR, ECONNRESET)` fires exactly once; the `t.error` slot was called once; the `t.read` slot was called once (the session attempted the read before classifying ERROR — the §3.2.3 `drive_server` post-AGAIN classification branch, mirroring T8's Client-side ordering); `on_req_decoded` never fires; `set_interest` / `destroy` slots never invoked | G3, G4 | unit |
| T23 | Client CONNECT_REQ write splits across two WRITE readiness deliveries | Fake `t`; `create_client("example.com", 11, 443, on_done, ud, &s)`; assert `wants(s) == ODIN_TRANSPORT_WRITE`; script `t.write` → first call writes 7 bytes then `AGAIN`, second call writes the remaining 9 bytes; `drive(s, &t, WRITE)` twice | After the first drive `wants(s) == WRITE` and `on_done` has not fired; after the second drive `wants(s) == READ` (phase has flipped to `C_READING_RESP`) and `on_done` has not fired; concatenated `t.write` buffer byte-equals the 16-byte REQ that `odin_proto_encode_connect_req` flattens for `("example.com", 11, 443)` (so a regression that reset `s.write_off` on `AGAIN` or rewrote from offset `0` on the resumed drive would surface as a non-matching prefix) | G1 | unit |
| T24 | Server decoder rejects bad frame_type | Fake `t`; `create_server(on_req_decoded, on_done, ud, &s)`; script `t.read` → `{0x01, 0x03, 0x03, 'a', 'b', 'c', 0x01, 0xBB}` (version `0x01` matches `ODIN_PROTO_VERSION_V1` but frame_type `0x03` ≠ `ODIN_PROTO_FRAME_CONNECT_REQ = 0x01`, so `odin_proto_decode_connect_req` returns `ODIN_PROTO_ERR_BAD_FRAME_TYPE` at the `odin/protocol.c:94-96` branch) then `AGAIN`; `drive(s, &t, READ)` | `on_done(ERROR, EPROTO)` fires exactly once (the third of the three §3.2.3 `do_read_req` decoder branches — `ERR_BAD_VERSION` / `ERR_BAD_FRAME_TYPE` / `ERR_HOST_LEN_INVALID` — that the G3 mapping routes to `EPROTO`, the symmetric Server-side counterpart to the Client-side T4, so a regression that mapped `ERR_BAD_FRAME_TYPE` to a non-EPROTO errno or skipped `on_done` would fail this row); `on_req_decoded` never fires | G3 | unit |
| T25 | Client ERROR readiness with `odin_transport_error` returning `0` (benign) re-arms the next readiness instead of firing `on_done` | Fake `t`; `create_client("a", 1, 80, on_done, ud, &s)`; drive WRITE to completion (so the session is in `C_READING_RESP` and `wants(s) == ODIN_TRANSPORT_READ`); script `t.read` → `AGAIN` and `t.error` → `0` (the benign-return case of the §3.2.2 / G3 contract "a `0` return is benign and re-arms the next readiness; a non-zero return becomes `err`"); record `wants(s)` immediately before the drive; `drive(s, &t, READ | ERROR)` | `drive` returns `ODIN_CONNECT_SESSION_DRIVE_CONTINUE`; `on_done` has not fired (no terminal transition occurred — the §3.2.4 `aggregate_error` latch was never tripped); `wants(s) == ODIN_TRANSPORT_READ` (unchanged from the pre-drive value, so the orchestrator's next `set_interest` call keeps the watch armed for the retry); the `t.error` slot was called exactly once (the §3.2.2 `do_read_resp` `AGAIN` return left state in `C_READING_RESP`, so the post-block `if events & ODIN_TRANSPORT_ERROR` branch fired `odin_transport_error(t)` once, and its `0` return short-circuited the `if err != 0: aggregate_error(s, err)` guard — a regression that dropped that guard would surface `on_done(ERROR, 0)` and fail this row's no-fire assertion); the `t.read` slot was called exactly once (the session attempted the read before classifying ERROR — same ordering as T8); `set_interest` / `destroy` slots never invoked | G3, G4 | unit |
| T26 | Server ERROR readiness in `S_READING_REQ` with `odin_transport_error` returning `0` (benign) re-arms the next readiness instead of firing `on_done` | Fake `t`; `create_server(on_req_decoded, on_done, ud, &s)`; assert `wants(s) == ODIN_TRANSPORT_READ`; script `t.read` → `AGAIN` and `t.error` → `0` (the benign-return case of the §3.2.3 / G3 contract — the Server-side mirror of T25); record `wants(s)` immediately before the drive; `drive(s, &t, READ | ERROR)` | `drive` returns `ODIN_CONNECT_SESSION_DRIVE_CONTINUE`; `on_done` has not fired; `on_req_decoded` has not fired; `wants(s) == ODIN_TRANSPORT_READ` (unchanged from the pre-drive value, so the orchestrator's next `set_interest` call keeps the watch armed for the retry); the `t.error` slot was called exactly once (the §3.2.3 `do_read_req` `AGAIN` return left state in `S_READING_REQ`, so the post-block `if events & ODIN_TRANSPORT_ERROR` branch fired `odin_transport_error(t)` once, and its `0` return short-circuited the `if err != 0: aggregate_error(s, err)` guard — a regression that dropped that guard would surface `on_done(ERROR, 0)` and fail this row's no-fire assertion); the `t.read` slot was called exactly once (the session attempted the read before classifying ERROR — same ordering as T22); `set_interest` / `destroy` slots never invoked | G3, G4 | unit |
| T27 | `create_client` rejects out-of-range `host_len` synchronously with `-1` / `errno == EINVAL` / `*out` untouched (constructor-side validation, independent of the wire-decoder rejection rows T12/T13) | No fake transport instantiated (the constructor takes no transport argument and does no I/O); define `on_done` as a recording stub whose per-test counter starts at `0`; initialize `odin_connect_session_t *out = (odin_connect_session_t *)0xDEADBEEFu` (sentinel chosen so any write to `*out` is detectable); call `odin_connect_session_create_client("a", 0, 80, on_done, ud, &out)` then `odin_connect_session_create_client("a", 256, 80, on_done, ud, &out)` — the two boundary failures of the §3.2.1 *Construction and ownership* contract `host_len ∈ [1, ODIN_PROTO_HOST_MAX]`, with `ODIN_PROTO_HOST_MAX == 255` per `odin/protocol.h:56` | Both calls return `-1` with `errno == EINVAL`; `out == 0xDEADBEEFu` after each call (the constructor neither allocated a session nor wrote `*out`, satisfying the §3.2.1 contract "otherwise returns `-1` with `errno == EINVAL` and `*out` untouched"); the recording `on_done` counter remains `0` (the constructor never invokes the callback before returning `-1`); a regression that dropped the §3.2.1 *Mechanism*'s `if host_len < 1 or host_len > ODIN_PROTO_HOST_MAX` guard would fall through to `odin_proto_encode_connect_req`, which itself returns `ODIN_PROTO_ERR_HOST_LEN_INVALID` at `odin/protocol.c:58-60` without populating `out_iov`, leaving the next *Mechanism* step's `flatten iov[0..3) into s.buf[..)` reading uninitialised `iov[k].base` / `iov[k].len` — undefined behavior that the entry-point guard is designed to prevent, and that the P2 AddressSanitizer command would trip on as a stacked use-of-uninitialised-bytes report | G1 | unit |

## 6. Implementation Plan

- **P1. Land the `connect_session` surface and a linkable stub, with `T1`–`T27` executable-red behind a skip gate.**
  - **Scope:** add `odin/connect_session.h` with the §3.2.1 types and the `create_client` / `create_server` / `drive` / `wants` / `server_set_error_code` / `server_host` / `server_port` / `server_tail` / `client_error_code` / `client_tail` / `destroy` declarations; add `odin/connect_session.c` with a stub `odin_connect_session_create_client` that allocates a session whose state is `C_WRITING_REQ` and returns `0` for *any* caller-supplied `host_len` (deliberately skipping the §3.2.1 *Construction and ownership* `host_len ∈ [1, ODIN_PROTO_HOST_MAX]` validation, so T27's `host_len ∈ {0, 256}` calls return `0` and write `*out` against the stub — tripping T27's `-1` / `errno == EINVAL` / `*out == 0xDEADBEEFu` assertions while leaving T1–T26's valid-`host_len` callers unaffected) but whose write/read paths are stubbed, a stub `odin_connect_session_create_server` that allocates a session whose state is `S_READING_REQ`, and stubs for the rest chosen so every remaining §5 row is red — `odin_connect_session_drive` empties the `events` mask and returns `CONTINUE` without invoking `read`/`write`/`error` on the transport (so `T1`–`T14` and `T17`–`T26` never reach `on_done` / `on_req_decoded`, the `T15` / `T16` `(len, buf_used)`-recording vectors stay empty, and the `T25` / `T26` `t.error called once` / `t.read called once` assertions fail against the never-invoked slots), `odin_connect_session_wants` returns a fixed sentinel `0` regardless of state (so the §5 rows' `wants(s) == WRITE` / `wants(s) == READ` assertions fail, including T25's and T26's pre-drive `wants(s) == READ` records and post-drive `wants(s) == READ` re-arm assertions), `odin_connect_session_server_set_error_code` is a no-op (no state transition), `odin_connect_session_server_host` / `_server_port` / `_server_tail` / `_client_error_code` / `_client_tail` return sentinel values (zero-length host slice, port `0`, server tail `(NULL, 0)`, error_code `0xFFFF`, client tail `(NULL, 0)` — chosen to differ from every §5 row's expected post-handshake values, so T17's `server_tail` length-7 assertion fails against the `(NULL, 0)` sentinel even in the unreachable case that on_req_decoded fired), and `odin_connect_session_destroy` frees the session. Add `odin/connect_session_testing.c` containing the single line `#include "connect_session.c" // NOLINT(bugprone-suspicious-include)` — matching the project's existing per-module test-wrapper convention (`transport_testing.c`, `relay_testing.c`, `dial_testing.c`, … each pull their C implementation into the C++ unit-test binary the same way), so the new module's symbols resolve in the `:odin_unittests` link without needing to add `:odin_connect_session` to its `deps`. Add `odin/connect_session_unittests.cpp` containing `T1`–`T27`, the test-local fake transport struct used by `T1`–`T26` (embeds `odin_transport_t base` as the first member, scripts `read`/`write`/`error` from per-test vectors, records every `set_interest` and `destroy` invocation into per-test counters, records every `read` call's `(len, buf_used_at_call_time)` pair into a per-test vector — `T27` instantiates no transport at all because the constructor under test takes none), and a per-row `if (!getenv("ODIN_CONNECT_SESSION_RED")) GTEST_SKIP() << "pending RFC-018 P2";` guard before the assertions; in `odin/BUILD.gn` add `source_set("odin_connect_session") { sources = [ "connect_session.c", "connect_session.h" ]; public_deps = [ ":odin_core", ":odin_transport" ] }` (the `:odin_core` dep covers `odin/protocol.h`), append `":odin_connect_session"` to the `:odin` `deps` array, and append `"connect_session.h"`, `"connect_session_testing.c"`, and `"connect_session_unittests.cpp"` to the `executable("odin_unittests")` `sources` array (no new config — the rows reuse no existing testing flags). Red-verification command: `ODIN_CONNECT_SESSION_RED=1 out/odin_unittests --gtest_filter='OdinConnectSession*'`.
  - **Depends on:** None.
  - **Done when:** `:odin_unittests` links with the new module (via the `connect_session_testing.c` wrapper) and the default suite (`out/odin_unittests`) reports `T1`–`T27` **skipped** and stays green; the red-verification command runs `T1`–`T27` and reports every one **failing** against the stubs — `T1`/`T2`/`T5`/`T10`/`T11`/`T14`/`T17`/`T20`/`T21`/`T23` (the empty `drive` never invokes `t.write` / `t.read`, so the assertion that `t.write`/`t.read` recorded a call against the expected byte sequence fails; the initial `wants(s) == WRITE` (Client) / `wants(s) == READ` (Server) assertion also fails because the stub returns `0`; for T1 and T10 specifically the second-drive `ODIN_CONNECT_SESSION_DRIVE_DONE` return assertion also fails because `on_done` never fires and the stub's `drive` returns `CONTINUE`, so the destroy-from-`on_done` branch is never reached; for T17 specifically `on_req_decoded` also never fires so the `server_tail` length-7 assertion never runs against real bytes; T20 and T21 fail at the same `wants(s) == READ` precondition because they share T10's REQ-read setup, so their `on_done(ERROR, …)` assertions never run; T23 fails at its own `wants(s) == WRITE` precondition (the fresh `create_client` setup, not shared with T1) before the staged-write sequence runs, so the second-drive `wants(s) == READ` phase-flip assertion and the concatenated-16-byte equality also never run), `T3`/`T4`/`T6`/`T7`/`T8`/`T9`/`T12`/`T13`/`T18`/`T19`/`T22`/`T24` (the empty `drive` never sets the error state, so `on_done` never fires and the `on_done == (ERROR, …)` assertion fails; T22 additionally fails at its own `wants(s) == READ` precondition (the fresh `create_server` setup, not shared with T10) before the `READ | ERROR` drive, so the per-slot recording assertions for `t.read` / `t.error` never run; T24 fails purely at the missing `on_done(ERROR, EPROTO)` because its setup has no `wants` precondition assertion before the READ drive), `T15`/`T16` (the empty `drive` never calls `t.read`, so the recorded `(len, buf_used)` vector is empty, the per-call invariant assertion has no entries to satisfy, and the terminal `on_done(OK, 0)` / `on_req_decoded` assertions never fire — failing the row), `T25` and `T26` (the empty `drive` never invokes `t.error` or `t.read`, so the rows' `t.error called exactly once` and `t.read called exactly once` assertions both fail against the never-invoked slots; T25 also fails its pre-drive `wants(s) == ODIN_TRANSPORT_READ` precondition because the stub `wants` returns `0`, so the post-drive `wants(s) == READ` unchanged-re-arm assertion is never reached; T26 fails its initial `wants(s) == ODIN_TRANSPORT_READ` assertion at the fresh `create_server` setup for the same reason, blocking the rest of the row), and `T27` (the stub `create_client` skips the §3.2.1 host_len validation and returns `0` after writing `*out` for both `host_len == 0` and `host_len == 256`, so the row's `return-value == -1`, `errno == EINVAL`, and `out == 0xDEADBEEFu` assertions all fail on the very first sub-call).

- **P2. Implement the state machines and turn `T1`–`T27` green.**
  - **Scope:** replace the `connect_session.c` stubs with the full implementation from §3.2.1–§3.2.4 — `create_client` adds the §3.2.1 *Mechanism* `if host_len < 1 or host_len > ODIN_PROTO_HOST_MAX: errno = EINVAL; return -1` guard the P1 stub deliberately omitted (turning T27 green), then flattens the iov encoder output into `s.buf[0 .. 5 + host_len)` and sets `write_total`; `create_server` initializes the empty read accumulator; `drive` dispatches to `do_write_req` / `do_read_resp` / `do_read_req` / `do_write_resp` per state and runs the §3.2.2 / §3.2.3 `ODIN_TRANSPORT_ERROR` classification (`err = odin_transport_error(t); if err != 0: aggregate_error(s, err)` — the explicit `0`-is-benign guard that T25 and T26 exercise); `wants` returns the per-state mask; `server_set_error_code` encodes the 4-byte RESP via `odin_proto_encode_connect_resp` and flips to `S_WRITING_RESP`; the accessors return the cached `error_code` / view / tail (including `server_tail` aliasing `s.buf[server_tail_off .. buf_used)` set by `do_read_req` on OK decode); and `aggregate_error` plus the `on_done_fired` latch enforce the exactly-once `on_done` from §3.2.4. Remove the `GTEST_SKIP` guard from `T1`–`T27`. This is the first phase where the §4 S1 trigger surface (an `odin_transport_read` call against the session's accumulator) exists in production, and the same phase lands the mitigation — §3.2.4's `run = ODIN_PROTO_CONNECT_REQ_MAX - s.buf_used` bound applied at every `do_read_resp` / `do_read_req` iteration.
  - **Depends on:** P1.
  - **Done when:** `T1`–`T27` pass un-gated on a clean `out/odin_unittests` run after the guards are removed — including T1's and T10's destroy-from-`on_done` exercising the §3.2.1 *Completion and destroy* hand-off path (the in-callback record captures the OK status and post-handshake accessors **before** `odin_connect_session_destroy(s)` runs, and the subsequent `drive` returns `ODIN_CONNECT_SESSION_DRIVE_DONE` without re-reading `s` — a regression that read any session field after the callback returned would corrupt the unwind and trip the AddressSanitizer command below), T2's 7-byte client tail captured from a single read that delivered both CONNECT_RESP and pipelined upstream bytes, T5's three-readiness staged client read landing on `client_error_code(s) == 0x1234` and a one-byte tail, T11's two-segment server REQ read (`{0x01, 0x01, 0x03, 'a'}` then `{'b', 'c', 0x01, 0xBB}`) landing host `"abc"` / port `0x01BB == 443`, then `server_set_error_code(s, 0x000A)` flipping to `S_WRITING_RESP` and a subsequent WRITE drive emitting `{0x01, 0x02, 0x00, 0x0A}` to fire `on_done(OK, 0)`, T14's two-readiness staged RESP write, T15/T16's per-call `len == 260 - buf_used` assertions over the recorded `(len, buf_used)` vector on the §4 S1 path (T15 finishing through `on_done(OK, 0)` with `client_error_code == 0x0000` and an empty tail; T16 finishing through `on_req_decoded` for host `"A"` / port `0x01BB`), T17's 7-byte server tail captured from a single read that delivered both CONNECT_REQ and pipelined client bytes — directly exercising the §3.2.3 *Post-REQ tail exposure* contract that replaces the silent absorb-and-drop behavior, T18/T19/T20/T22 — the four Server-side G3 branches that mirror the Client coverage T6–T9 already prove: T18 exercises `do_read_req` synchronous `IO_ERROR` propagating the transport's `errno` (`ECONNRESET`) unchanged into `on_done`, T19 exercises `do_read_req` `EOF` in `S_READING_REQ` mapping to `on_done(ERROR, ECONNRESET)` (the §3.2.3 / G3 peer-half-close clause), T20 exercises `do_write_resp` synchronous `IO_ERROR` propagating `errno = EPIPE` after `server_set_error_code` has flipped to `S_WRITING_RESP`, and T22 mirrors T8's Client-side post-AGAIN `ODIN_TRANSPORT_ERROR` classification on the Server side — `do_read_req` returns `AGAIN` and the §3.2.3 `drive_server` post-block ERROR classification calls `odin_transport_error(t)` to surface `ECONNRESET`, asserted via `t.read` being recorded once before `t.error` so a regression that re-ordered or skipped the post-AGAIN ERROR classification on the Server side fails the row; T24 closes the symmetric coverage gap by exercising the Server-side `do_read_req` `ERR_BAD_FRAME_TYPE` branch — the third of the three §3.2.3 decoder branches that map to `EPROTO` (the prior round flagged this as missing because Client had T4 but Server had no counterpart) — asserted via `on_done(ERROR, EPROTO)` firing exactly once against a REQ-shaped buffer whose `buf[1] = 0x03 ≠ ODIN_PROTO_FRAME_CONNECT_REQ`, so a regression that mapped `ERR_BAD_FRAME_TYPE` to a non-EPROTO errno (e.g., the catch-all `EIO`) or that fell through to a different branch in the decoder switch would fail the row; T21 covers a distinct §3.2.3 *Stray readiness* design point — an `ODIN_TRANSPORT_ERROR` readiness classified while the session is suspended in `S_AWAIT_DIAL` with no I/O attempted in that drive, so a peer reset during the dial wait surfaces rather than dangles (T21 is not a T8 mirror because it tests the no-I/O `S_AWAIT_DIAL` branch, where T22 tests the I/O-attempted-then-classify `S_READING_REQ` branch); T23 mirrors T14 on the Client side, exercising `do_write_req`'s `s.write_off` resume across two WRITE readiness deliveries where the second drive must continue from `s.write_off == 7` rather than rewriting from offset `0` — asserted via the concatenated `t.write` buffer byte-equaling the full 16-byte REQ that `odin_proto_encode_connect_req` flattens for `("example.com", 11, 443)` and the post-second-drive `wants(s) == READ` flip into `C_READING_RESP`; T25 and T26 close the §3.2.2 / §3.2.3 G3 benign-return arm that the prior round flagged as unexercised — T25 (Client, `C_READING_RESP`) and T26 (Server, `S_READING_REQ`) both script `t.read → AGAIN` plus `t.error → 0`, drive `READ | ERROR`, and assert that the post-AGAIN ERROR classification fires `odin_transport_error(t)` exactly once, the `if err != 0: aggregate_error(s, err)` guard short-circuits on the benign `0` return, `on_done` does *not* fire, and `wants(s) == ODIN_TRANSPORT_READ` stays unchanged so the orchestrator's next `set_interest` re-arms the same mask — a regression that dropped the `if err != 0` guard would surface `on_done(ERROR, 0)` and fail both rows' no-fire assertions; and T27 closes the §3.2.1 constructor-validation gap by exercising `odin_connect_session_create_client("a", 0, …)` and `("a", 256, …)` and asserting `-1` / `errno == EINVAL` / `out` still equal to the `0xDEADBEEFu` sentinel / `on_done` counter still `0` — turning green because the P2 implementation adds the §3.2.1 *Mechanism* `if host_len < 1 or host_len > ODIN_PROTO_HOST_MAX` guard the P1 stub deliberately omitted, so a regression that removed the guard again would let control fall through to `odin_proto_encode_connect_req`'s `odin/protocol.c:58-60` early return and then to the *Mechanism*'s `flatten iov[0..3) into s.buf` step against the unpopulated `iov` (caught both by T27's `out == 0xDEADBEEFu` assertion via the would-be earlier `*out = s` write and by the AddressSanitizer command below as a use-of-uninitialised-bytes report); and `./tool/gn gen out/odin_asan --args='target_os="mac" is_asan=true'`, `./tool/ninja -C out/odin_asan odin_unittests`, and `out/odin_asan/odin_unittests --gtest_filter='OdinConnectSession*'` exit with no AddressSanitizer report — giving T15 and T16 their no-heap-buffer-overflow back-up coverage on the S1 enforcement path, giving T1's and T10's destroy-from-`on_done` callbacks their no-heap-use-after-free back-up coverage on the §3.2.1 *Completion and destroy* hand-off path (a regression that read `s` after the callback returns trips ASan here), and giving T27 its no-use-of-uninitialised-bytes back-up coverage on the §3.2.1 constructor-validation path described above.
