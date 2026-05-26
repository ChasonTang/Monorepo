# RFC-009: Odin Client TCP Listener and Single-Connection CONNECT Handshake

**Author:** Chason Tang  
**Date:** 2026-05-26  
**Status:** Implemented

## 1. Summary

Add `odin/client_listen.{c,h}` to the existing `:odin` source set, exposing two C functions. `int odin_client_listen_open(uint16_t port)` opens a blocking IPv4 `SOCK_STREAM` socket, enables `SO_REUSEADDR`, binds it to `127.0.0.1:port` via `INADDR_LOOPBACK`, calls `listen(fd, 1)`, and returns the listening fd — or `-1` with `errno` preserved on any of the four syscall failures, the partial fd closed. `odin_client_listen_status_t odin_client_listen_handshake(int conn_fd)` reads into a stack-resident `uint8_t buf[ODIN_HTTP_REQUEST_MAX]` in a loop, invokes RFC-003's `odin_http_parse_connect` on every accumulated prefix, writes the bytes returned by RFC-008's `odin_http_response_for_status` for whichever terminal status the parser reaches, and uniformly closes `conn_fd` via `shutdown(SHUT_WR)` → drain reads to EOF → `close`. It returns one of `OK` / `REJECTED` / `PEER_CLOSED` / `IO_ERROR`. Tests inject one end of `socketpair(AF_UNIX, SOCK_STREAM, 0)` as the `conn_fd` argument; `odin_cli_main`, `odin/main.c`, and the eventual `accept(2)` loop are not modified — a follow-up RFC wires them.

## 2. Motivation

After RFC-003 (CONNECT request parser) and RFC-008 (response status mapper), the Client has a pure-buffer request parser and a pure response writer but no code that puts bytes on or takes bytes off a socket. The RFC-006 CLI parser already populates `odin_cli_args_t.listen_port` from the `--listen` argument, yet every successful `odin_cli_main` invocation in Client mode prints its `mode=client listen=…` banner and exits with no TCP listener ever opened — a browser pointed at the documented proxy port gets `ECONNREFUSED` because nothing is bound. A second pain is the test boundary: the I/O surface (`bind` / `listen` / `accept` / `read` / `write` / `shutdown` / `close`) is the largest untested code path the Client will land, and folding it inline into `odin_cli_main` would force every test of the read/parse/respond/close cycle to spin up a real listener on a real loopback port — slow, racy with concurrent test workers, and tied to the host's port-allocation policy. Landing the listener and the handshake in a stand-alone module with the handshake exposed as `f(int fd)` gives the Client one importable surface, makes the per-connection control flow grep-able in one file, and lets every read/parse/respond/close scenario assert through `socketpair(AF_UNIX, SOCK_STREAM, 0)` without ever invoking `bind(2)`. No data available at this time.

## 3. Goals and Non-Goals

- **G1.** `int odin_client_listen_open(uint16_t port)` returns a positive blocking IPv4 `SOCK_STREAM` fd that has `SO_REUSEADDR` enabled, is bound to `127.0.0.1:port` (`INADDR_LOOPBACK` — the fixed loopback address pinned by the requirement), and has had `listen(fd, 1)` called on it; on any `socket(2)` / `setsockopt(2)` / `bind(2)` / `listen(2)` failure the function returns `-1` with `errno` preserved from the failing syscall and any partially-opened fd closed before the return. `port == 0` is a legal input — the kernel picks an ephemeral port the caller can read back via `getsockname(2)`.
- **G2.** `odin_client_listen_status_t odin_client_listen_handshake(int conn_fd)` consumes `conn_fd` (always closes it before returning); reads into a stack-resident `uint8_t buf[ODIN_HTTP_REQUEST_MAX]` accumulator in a `read(2)` loop, calling `odin_http_parse_connect` after every successful read; on the parser's first terminal `odin_http_status_t` writes the slice from `odin_http_response_for_status(s)` via a retry-on-`EINTR`-and-short-write helper, then closes via `shutdown(conn_fd, SHUT_WR)` → `read(2)`-and-discard until `0` or non-`EINTR` `-1` → `close(conn_fd)` (the requirement's "写完 200 后让对端读完再关" clause, applied uniformly so error responses are also delivered before the FIN). Returns `ODIN_CLIENT_LISTEN_OK` when the parser returned `OK`; `REJECTED` for any of the six errors; `PEER_CLOSED` when `read(2)` returned `0` while still in `NEED_MORE`; `IO_ERROR` when `read(2)` / `write(2)` returned `-1` for a reason other than `EINTR`, or when `shutdown(2)` returned `-1` (treated as non-interruptible per §4.2.3's Unstated contract).
- **G3.** `odin_client_listen_handshake`'s only connection-related parameter is `int conn_fd`; no global state, no thread-local fd, no callback-injected helper. Except for the end-to-end integration row (T9) — the explicit carve-out that proves `open` + `accept` + `handshake` compose on real TCP — every §7 row that exercises the read/parse/respond/close cycle passes an `AF_UNIX` stream-socket fd (one end of `socketpair(AF_UNIX, SOCK_STREAM, 0)` for the rows that need a peer, a bare `socket(AF_UNIX, SOCK_STREAM, 0)` for the unconnected-`IO_ERROR` arm) as the `conn_fd` argument; among §7's handshake-exercising rows, only T9 invokes `accept(2)` or opens an `AF_INET` socket (T8 is a listener-only row that calls `odin_client_listen_open(0)` to verify §4.2.1's `bind(2)` / `listen(2)` contract without ever invoking the handshake).

**Non-Goals:**

- Wiring `odin_cli_main`'s Client `ODIN_CLI_OK` arm to actually call `odin_client_listen_open` + `accept(2)` + `odin_client_listen_handshake` — a follow-up RFC lands the wiring; this RFC ships only the importable surface, matching the pattern RFC-003 (parser landed without wiring) and RFC-008 (response mapper landed without wiring) established.
- Multi-connection accept loop, `fork(2)` / thread-per-connection dispatch, or any connection lifecycle longer than the one CONNECT handshake — single-connection by construction per the requirement.
- Forwarding the post-handshake pipelined bytes (`buf[*out_consumed .. n)` per RFC-003 §4.2.2) onto an upstream tunnel — they are discarded when the handshake's stack frame unwinds; the tunnel is a follow-up RFC.
- Non-blocking I/O, `poll(2)` / `epoll(2)` / `kqueue(2)` dispatch, `SO_RCVTIMEO` / `SO_SNDTIMEO` socket timeouts, or any other countermeasure against a slow-loris peer (slow request) or a never-closing peer (drain stall) — blocking sockets only; a follow-up RFC adds dispatch and timeouts if the operational profile demands them.
- Wrapping `accept(2)` in a third function — `accept(2)` is one syscall with no value-add wrapper; the eventual `odin_cli_main` Client arm calls `accept(2)` directly on the fd returned by `odin_client_listen_open`.

## 4. Design

### 4.1 Overview

A new module is added to `odin/`: `odin/client_listen.h` (public C header — the only public surface) and `odin/client_listen.c` (implementation, depending only on `<errno.h>`, `<stddef.h>`, `<stdint.h>`, `<string.h>`, `<sys/socket.h>`, `<netinet/in.h>`, `<arpa/inet.h>`, `<unistd.h>`, and `"odin/http_connect.h"` for `odin_http_parse_connect`, `odin_http_response_for_status`, `ODIN_HTTP_REQUEST_MAX`, and `odin_http_status_t`). The new test source `odin/client_listen_unittests.cpp` joins the existing `:odin_unittests` executable beside `cli_unittests.cpp` and `http_connect_unittests.cpp`; every handshake row uses `socketpair(AF_UNIX, SOCK_STREAM, 0)` and the one listener row uses `getsockname(2)` on the fd returned by `odin_client_listen_open(0)`. `odin/BUILD.gn:25-36` appends `"client_listen.c"` and `"client_listen.h"` to the existing `source_set("odin")` `sources` array; `odin/BUILD.gn:75-81` appends `"client_listen_unittests.cpp"` to the `executable("odin_unittests")` `sources` array. No new GN target, no root `BUILD.gn` edit, no new dependency. `odin/main.c` and `odin/cli.c` are not modified — `odin_cli_main`'s Client `ODIN_CLI_OK` arm continues to print only the banner (RFC-006 §4.2.4), and a follow-up RFC threads the call to the new functions.

N/A — textual description above is sufficient.

### 4.2 Detailed Design

#### 4.2.1 Listener Open: Socket, Bind, Listen

```c
/* odin/client_listen.h — listener surface */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int odin_client_listen_open(uint16_t port);

#ifdef __cplusplus
}
#endif
```

**Unstated contract.** Address family is `AF_INET`; type is `SOCK_STREAM` (blocking by default — no `SOCK_NONBLOCK`); protocol is `0` (kernel selects TCP). Bind address is always `INADDR_LOOPBACK` (`127.0.0.1`, `0x7F000001` in host byte order) — there is no flag, env var, or compile-time switch to bind any other address, matching the requirement's "绑定地址固定 127.0.0.1" clause. `SO_REUSEADDR` is enabled before `bind(2)` so a rapid `open(port)` → `close` → `open(port)` cycle within `TIME_WAIT` does not return `EADDRINUSE`; failure of the `setsockopt(2)` is treated as a fatal open failure (any failure on a freshly-created socket signals something deeply wrong, so falling through silently would surface confusing rebind errors later). Backlog passed to `listen(2)` is `1` — single-connection by G2's contract; raising the backlog would be dead headroom because nothing consumes a second connection in this RFC. Any of `socket(2)` / `setsockopt(2)` / `bind(2)` / `listen(2)` returning `-1` aborts: the partial fd is closed via `close(2)` (its return ignored — the open already failed), `errno` is preserved from the failing syscall (the `close(2)` does not overwrite it because the cleanup is wrapped in a `saved = errno; close(fd); errno = saved;` block), and the function returns `-1`. The successful return is a positive fd the caller owns and must close — the listener does not register cleanup with `atexit` or thread-local destructors.

**Mechanism.**

```
open(port):
  fd = socket(AF_INET, SOCK_STREAM, 0)
  if fd < 0: return -1
  int one = 1
  if setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0:
    saved = errno; close(fd); errno = saved; return -1
  struct sockaddr_in addr = {}
  addr.sin_family      = AF_INET
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK)
  addr.sin_port        = htons(port)
  if bind(fd, &addr, sizeof addr) != 0:
    saved = errno; close(fd); errno = saved; return -1
  if listen(fd, 1) != 0:
    saved = errno; close(fd); errno = saved; return -1
  return fd
```

Satisfies: G1 via the four pinned syscalls, the fixed loopback bind address, and the `errno`-preserving cleanup on every failure arm.

#### 4.2.2 Handshake API and Read Loop

```c
/* odin/client_listen.h — handshake surface */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum odin_client_listen_status_t {
  ODIN_CLIENT_LISTEN_OK = 0,
  ODIN_CLIENT_LISTEN_REJECTED,
  ODIN_CLIENT_LISTEN_PEER_CLOSED,
  ODIN_CLIENT_LISTEN_IO_ERROR,
} odin_client_listen_status_t;

odin_client_listen_status_t odin_client_listen_handshake(int conn_fd);

#ifdef __cplusplus
}
#endif
```

**Unstated contract.** `conn_fd` must be a connected stream socket fd; the function never inspects the socket family, so a `socketpair(AF_UNIX, SOCK_STREAM, 0)` fd works identically to an `AF_INET` TCP fd accepted from the §4.2.1 listener (G3's enabling property). The caller transfers ownership: `close(conn_fd)` runs on every return arm including `IO_ERROR`. `OK` and `REJECTED` both mean the response slice was fully written and the §4.2.3 graceful close completed; they differ only by whether the parser verdict was `ODIN_HTTP_OK` or one of the six error statuses. `PEER_CLOSED` means `read(2)` returned `0` while still in `NEED_MORE` — no response is written and `conn_fd` is closed without a `shutdown(2)`. `IO_ERROR` means `read(2)` / `write_all` / `shutdown(2)` returned `-1` for a reason other than `EINTR`; partial response bytes may have been transmitted, and `conn_fd` is closed without further drain attempts. The buffer is `uint8_t buf[ODIN_HTTP_REQUEST_MAX]` on the stack (8 KiB — within every glibc / musl / macOS default thread-stack budget, so no heap allocation is needed). Pure aside from the named syscalls: no allocation, no global state, no thread interaction.

**Mechanism.**

```
handshake(conn_fd):
  uint8_t buf[ODIN_HTTP_REQUEST_MAX]; size_t n = 0
  odin_http_status_t s; size_t consumed; odin_http_connect_t cout
  for (;;):
    ssize_t r = read(conn_fd, buf + n, ODIN_HTTP_REQUEST_MAX - n)
    if r < 0:
      if errno == EINTR: continue
      close(conn_fd); return ODIN_CLIENT_LISTEN_IO_ERROR
    if r == 0:
      close(conn_fd); return ODIN_CLIENT_LISTEN_PEER_CLOSED
    n += (size_t)r
    s = odin_http_parse_connect(buf, n, &consumed, &cout)
    if s != ODIN_HTTP_NEED_MORE: break
    /* RFC-003 §4.2.3 guarantees s != NEED_MORE once n == ODIN_HTTP_REQUEST_MAX,
       so the loop terminates within at most ODIN_HTTP_REQUEST_MAX iterations. */
  odin_http_response_t resp = odin_http_response_for_status(s)
  if write_all(conn_fd, resp.bytes, resp.len) != 0:
    close(conn_fd); return ODIN_CLIENT_LISTEN_IO_ERROR
  if graceful_close(conn_fd) != 0:                       /* closes conn_fd internally */
    return ODIN_CLIENT_LISTEN_IO_ERROR
  return (s == ODIN_HTTP_OK) ? ODIN_CLIENT_LISTEN_OK : ODIN_CLIENT_LISTEN_REJECTED
```

Satisfies: G2 via the bounded read loop, the four-arm status enum, and the `EINTR`-retry on every blocking syscall; G3 via the single `int conn_fd` parameter and the absence of any other I/O parameter or global state on the handshake's surface.

#### 4.2.3 Response Write Helper and Graceful Close

**Contract surface.**

```
write_all(fd, bytes, len):                    graceful_close(fd):
  size_t off = 0                                if shutdown(fd, SHUT_WR) != 0:
  while off < len:                                close(fd); return -1
    ssize_t r = write(fd, bytes+off, len-off)   uint8_t sink[256]
    if r < 0:                                   for (;;):
      if errno == EINTR: continue                 ssize_t r = read(fd, sink, sizeof sink)
      return -1                                   if r > 0: continue
    if r == 0: return -1                          if r == 0: break
    off += (size_t)r                              if errno == EINTR: continue
  return 0                                        break    /* drain done — close anyway */
                                                close(fd); return 0
```

**Unstated contract.** `write_all` is the standard "loop until all bytes written, retry on `EINTR`, treat short writes as normal" idiom — returning `0` on full delivery and `-1` (with `errno` from the failing `write(2)`) on any other terminal error. A partial write before failure leaves the peer with a truncated response slice; the `IO_ERROR` return signals this and the caller treats the connection as dead. `graceful_close` implements the requirement's "写完 200 后让对端读完再关" clause uniformly across every terminal status: `shutdown(2)` sends a TCP `FIN` (or `AF_UNIX` half-close in tests) so the peer's pending `read(2)` returns the response bytes followed by `0` cleanly rather than seeing a `RST` that may discard response bytes still in its receive buffer; the drain loop then reads-and-discards whatever the peer sends between the FIN and its own `close(2)`, terminating on `0` or non-`EINTR` `-1` (e.g. `ECONNRESET` from an abortive peer close — already "done waiting", so the loop falls through to `close`). `EINTR` retries inside the drain; every other read error terminates the drain but the function still calls `close(fd)` so the fd is not leaked. `shutdown(2)` is treated as non-interruptible — POSIX does not list `EINTR` among its documented errors and the Linux / macOS implementations this RFC targets do not surface it in practice, so any non-zero return is propagated to the caller as `IO_ERROR` with no retry (matching G2's wording, which excludes `shutdown(2)` from the `EINTR`-retry clause). The drain has no timeout — a malicious peer that never closes blocks the handshake indefinitely; the §3 Non-Goals row leaves the timeout to a follow-up RFC. Both helpers are file-static.

Satisfies: G2 via the uniform `shutdown(SHUT_WR)` → drain → `close` sequence on every terminal-status arm and the `write_all` helper that the `IO_ERROR` arm distinguishes from `EINTR`-recoverable short-write retry.

### 4.3 Design Rationale

- **Chosen:** Uniform graceful close (`shutdown(SHUT_WR)` → drain → `close`) for every terminal status — `OK` (200) and all six `REJECTED` paths (400 / 405 / 414 / 505).
- **Reason:** The requirement only pins the close pattern for 200, but the same TCP-level concern (a `close(2)` with unread bytes in the receive buffer can let the kernel send `RST` instead of `FIN`, discarding response bytes the peer has not yet read) applies identically to the `4xx`/`5xx` responses. A user whose browser sends `GET /` to the proxy port and gets a `RST` instead of the `405 Method Not Allowed` cannot read the `Allow: CONNECT` header RFC-008 §4.2.1 designed for exactly that diagnostic. Routing both arms through one helper keeps the function one straight-line read → write → close path with one close point.
- **Ruled out:** Differentiated close — `shutdown`+drain+`close` on `OK`, bare `close` on `REJECTED`. Saves a `shutdown(2)` and a drain loop on the error arms (microseconds at most), at the cost of unpredictable RST behavior on error responses and a branching control-flow the §4.2.3 helper would have to expose as two entry points.

- **Chosen:** Blocking I/O (`SOCK_STREAM` default; no `SOCK_NONBLOCK`; no `SO_RCVTIMEO`/`SO_SNDTIMEO`).
- **Reason:** Single-connection one-shot — the eventual `accept` loop runs `handshake` serially, so a slow-loris peer that hangs the one handshake has the same operational consequence (operator restart) as a non-blocking version whose event loop has nothing else to do. The non-blocking equivalent adds `EAGAIN` retry, multiplexer registration, and an event handler — strictly more code with no behavioral win until the listener accepts concurrent connections (follow-up RFC).
- **Ruled out:** Non-blocking sockets + `poll(2)` event loop. Justified only for concurrent connections or per-connection timeouts; neither applies here.

- **Chosen:** Tests inject `AF_UNIX` `SOCK_STREAM` fds into the handshake — `socketpair(AF_UNIX, SOCK_STREAM, 0)` for the read/parse/respond/close rows, an unconnected `socket(AF_UNIX, SOCK_STREAM, 0)` for the `IO_ERROR` row; only T9 connects to and accepts on a real `AF_INET` listener.
- **Reason:** `read(2)` / `write(2)` / `shutdown(2)` / `close(2)` behave identically on `AF_UNIX` and `AF_INET` `SOCK_STREAM` for the byte-delivery semantics the handshake depends on. `socketpair` runs synchronously in-process with no kernel routing, no port allocation, no `TIME_WAIT` interference — every alternative (per-test ephemeral listener + connect, static port + serialized tests) trades that determinism for negligible coverage gain. T9 confirms `open` + `accept` + `handshake` compose end-to-end on real TCP.
- **Ruled out:** Mock the syscall layer (preload-shimmed `read`/`write`). The mock has to re-implement enough socket semantics (short reads, `EINTR`, EOF, peer half-close) to be a faithful stand-in — at which point it has reimplemented `socketpair`. No mock-vs-real fidelity gap to worry about with the real syscall pair.

## 5. Backward Compatibility & Migration

Not applicable — this RFC introduces `odin/client_listen.{c,h}` and `odin/client_listen_unittests.cpp` from scratch, appends them to the existing `source_set("odin")` `sources` array at `odin/BUILD.gn:25-36` and the `executable("odin_unittests")` `sources` array at `odin/BUILD.gn:75-81`, modifies no existing source file, and adds no new GN target, so nothing that previously compiled or ran changes behavior.

## 6. Security

- **Threat:** Out-of-bounds write into the stack-resident `uint8_t buf[ODIN_HTTP_REQUEST_MAX]` accumulator — a local peer connects to the loopback listener and sends more bytes than the buffer can hold (e.g. a 100 KiB blob that arrives in one `read(2)`). Without a per-`read(2)` count bound, the second-or-later read would write past `buf[ODIN_HTTP_REQUEST_MAX - 1]` and corrupt adjacent stack frames. The trigger surface is the local TCP socket the listener binds (and the `socketpair` fds the tests inject); "local" is not "trusted" — any process on the host running as any user may `connect(2)` to `127.0.0.1:listen_port` and craft arbitrary byte sequences against this code path.
- **Mitigation:** §4.2.2's Mechanism block bounds every `read(2)` call's count argument to `ODIN_HTTP_REQUEST_MAX - n`, so total bytes written to `buf` across all loop iterations never exceed `sizeof(buf)`. Once `n == ODIN_HTTP_REQUEST_MAX`, RFC-003 §4.2.3's parser is guaranteed to return `ODIN_HTTP_ERR_REQUEST_TOO_LARGE` (not `NEED_MORE`), so the loop exits before another `read(2)` is attempted on a full buffer. Any bytes the peer sent beyond `ODIN_HTTP_REQUEST_MAX` sit in the kernel receive buffer and are read-and-discarded by §4.2.3's `graceful_close` drain loop after the 414 response is written.
- **Enforcement:** §7 row T6 (peer sends > `ODIN_HTTP_REQUEST_MAX` bytes without a terminating CRLFCRLF; handshake returns `REJECTED`, peer reads the byte-exact `414 URI Too Long` slice from RFC-008, then EOF) fires the trigger input and asserts the bounded-read outcome.

## 7. Testing Strategy

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | OK round-trip — well-formed CONNECT → 200 → EOF | `socketpair(AF_UNIX, SOCK_STREAM, 0, fds)`; `write(fds[0], "CONNECT example.com:443 HTTP/1.1\r\n\r\n", 36)`; `shutdown(fds[0], SHUT_WR)`; call `odin_client_listen_handshake(fds[1])` | Return is `ODIN_CLIENT_LISTEN_OK`; `read(fds[0], …)` returns `39` bytes byte-equal to `"HTTP/1.1 200 Connection Established\r\n\r\n"`; the next `read(fds[0], …)` returns `0` (EOF — `fds[1]` was closed after graceful shutdown); `close(fds[0])` succeeds | G2, G3 | unit |
| T2 | Five small-input parser errors map to byte-exact 4xx / 5xx responses | For each `(request, expected_response_slice)` in `{("GET / HTTP/1.1\r\n\r\n", kResp405), ("CONNECT bad HTTP/1.1\r\n\r\n", kResp400), ("CONNECT a:1 HTTP/2.0\r\n\r\n", kResp505), ("CONNECT a:99999 HTTP/1.1\r\n\r\n", kResp400), ("CONNECT "+"x"*256+":1 HTTP/1.1\r\n\r\n", kResp400)}`: build a fresh `socketpair`, write the request, half-close `fds[0]` write side, call the handshake on `fds[1]` | All five return `ODIN_CLIENT_LISTEN_REJECTED`; the bytes read from `fds[0]` match the expected `odin_http_response_for_status(...)` slice byte-for-byte (lengths `51, 28, 43, 28, 28` respectively); the subsequent `read(fds[0], …)` returns `0` | G2, G3 | unit |
| T3 | Incremental writes — request split across multiple peer writes | `socketpair`; spawn a `std::thread` that writes the 36-byte well-formed CONNECT from T1 one byte at a time with a 1 ms sleep between bytes, then `shutdown(SHUT_WR)`; main thread calls handshake on `fds[1]`; join the writer thread after the handshake returns | Handshake returns `ODIN_CLIENT_LISTEN_OK`; `fds[0]` yields the byte-exact 39-byte 200 response and then EOF; the read loop's `NEED_MORE` arm is the only way the byte-at-a-time write completes the parse, so this row exercises the `s == ODIN_HTTP_NEED_MORE: continue` branch of §4.2.2's Mechanism | G2, G3 | unit |
| T4 | Pipelined bytes after CONNECT request are discarded; response is unaffected | `socketpair`; `write(fds[0], "CONNECT example.com:443 HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\n", 52)` (one well-formed CONNECT plus 16 bytes of "next" request); `shutdown(fds[0], SHUT_WR)`; call handshake | Return is `ODIN_CLIENT_LISTEN_OK`; `fds[0]` yields exactly the 39-byte 200 response followed by EOF — the 16 pipelined bytes are consumed into `buf[36..52)` by the handshake's read loop, discarded when the stack frame unwinds, and never echoed back through `fds[1]` | G2, G3 | unit |
| T5 | Peer half-closes mid-request → `PEER_CLOSED`, no response written | `socketpair`; `write(fds[0], "CONNECT ", 8)` (a valid prefix of `"CONNECT "` per RFC-003); `shutdown(fds[0], SHUT_WR)`; call handshake | Return is `ODIN_CLIENT_LISTEN_PEER_CLOSED`; `read(fds[0], …)` returns `0` immediately (no response bytes were written because the handshake exited the read loop on `r == 0` before reaching the write arm) | G2, G3 | unit |
| T6 | Request exceeds `ODIN_HTTP_REQUEST_MAX` → `REJECTED` with byte-exact 414 response (security row) | `socketpair(AF_UNIX, SOCK_STREAM, 0, fds)`; spawn a `std::thread` that writes `"CONNECT example.com:443 HTTP/1.1\r\n" + "X-Pad: " + ("a" * 8188) + "\r\n\r\n"` (8233 bytes = 34 + 7 + 8188 + 4; CRLFCRLF starts at offset 8229 — past `ODIN_HTTP_REQUEST_MAX`) to `fds[0]` across as many `write(2)` calls as the kernel send buffer requires, then `shutdown(fds[0], SHUT_WR)`; the writer thread is mandatory because the macOS default `AF_UNIX` `SOCK_STREAM` send buffer is 8192 bytes per direction (verified on Darwin 21.6.0), so a single-thread "write-all-then-handshake" ordering deadlocks once the writer blocks at byte 8193 with no concurrent reader; main thread calls `odin_client_listen_handshake(fds[1])`; join the writer thread after the handshake returns | Return is `ODIN_CLIENT_LISTEN_REJECTED`; `fds[0]` yields the byte-exact `"HTTP/1.1 414 URI Too Long\r\n\r\n"` slice (29 bytes), then EOF | G2, G3, §6 | unit |
| T7 | Unconnected socket → `IO_ERROR` on first `read(2)` | `int fd = socket(AF_UNIX, SOCK_STREAM, 0)` (no `connect(2)`, no `bind(2)`, no peer — an unconnected `AF_UNIX` stream socket returns `-1`/`ENOTCONN` from `read(2)` on POSIX, matching the `AF_INET` equivalent and keeping G3's "handshake-exercising rows use `AF_UNIX` outside T9" rule intact); call `odin_client_listen_handshake(fd)` | Return is `ODIN_CLIENT_LISTEN_IO_ERROR` (the first `read(2)` returns `-1` with `errno == ENOTCONN`); the function calls `close(fd)` before returning, so a subsequent `close(fd)` from the test would return `-1` with `EBADF` — the test does not double-close, it only inspects the return value | G2 | unit |
| T8 | `odin_client_listen_open` binds to `127.0.0.1` with a kernel-assigned ephemeral port | `int fd = odin_client_listen_open(0)`; call `getsockname(fd, …)` and inspect the returned `sockaddr_in` | `fd >= 0`; `addr.sin_family == AF_INET`; `addr.sin_addr.s_addr == htonl(INADDR_LOOPBACK)` (proves the fixed-loopback bind, not `INADDR_ANY`); `addr.sin_port != 0` (proves the kernel assigned an ephemeral port for the `port == 0` input); a second `getsockopt(fd, SOL_SOCKET, SO_REUSEADDR, …)` returns a non-zero option value (proves `SO_REUSEADDR` is set); `close(fd)` succeeds | G1 | unit |
| T9 | End-to-end on real TCP — `open` + `accept` + `handshake` round-trip a well-formed CONNECT | `int lfd = odin_client_listen_open(0)`; read its bound port via `getsockname`; spawn a `std::thread` that `socket(AF_INET, SOCK_STREAM, 0)` + `connect(127.0.0.1:port)`, writes the T1 36-byte CONNECT, half-closes write side, reads until EOF; main thread calls `int cfd = accept(lfd, NULL, NULL)` then `odin_client_listen_handshake(cfd)`; join the client thread | Handshake returns `ODIN_CLIENT_LISTEN_OK`; the client thread reads exactly 39 bytes byte-equal to the 200 response followed by `0` (EOF); `close(lfd)` succeeds | G1, G2 | integration |

## 8. Implementation Plan

- **P1. Land the listener + handshake surface, the test rows, and the BUILD.gn wiring with red `T1`–`T9`.**
  - **Scope:** add `odin/client_listen.h` exactly as §4.2.1 and §4.2.2 specify — the `odin_client_listen_open` declaration, the `odin_client_listen_status_t` enum in the order `OK / REJECTED / PEER_CLOSED / IO_ERROR`, and the `odin_client_listen_handshake` declaration inside one `extern "C"` block, with the requirement's "绑定地址固定 127.0.0.1" and "写完 200 后让对端读完再关" clauses pinned verbatim in the header doc-comment; add `odin/client_listen.c` with `odin_client_listen_open` body returning `-1` with `errno = ENOSYS` and `odin_client_listen_handshake` body that calls `close(conn_fd)` and returns `IO_ERROR` unconditionally so the binary links; add `odin/client_listen_unittests.cpp` containing `T1`–`T9` from §7, each as a separate `TEST(OdinClientListenTest, …)` whose first statement is `GTEST_SKIP() << "pending RFC-009 P2";`; append `"client_listen.c"` and `"client_listen.h"` to the `source_set("odin")` `sources` array at `odin/BUILD.gn:25-36`, and append `"client_listen_unittests.cpp"` to the `executable("odin_unittests")` `sources` array at `odin/BUILD.gn:75-81`. No other GN edit; no `odin/main.c` or `odin/cli.c` change.
  - **Depends on:** None.
  - **Done when:** `gn gen out/` resolves the modified `:odin` and `:odin_unittests` and `ninja -C out/ tests` builds without error; `odin/client_listen.h` exports `odin_client_listen_open` (G1 staged), `odin_client_listen_status_t` plus `odin_client_listen_handshake` (G2 staged), with `conn_fd` as the sole conn-related parameter on the handshake (G3 staged); `T1`–`T9` are committed in `GTEST_SKIP`-wrapped (red) state and `out/odin_unittests --gtest_brief=1` reports all nine as `SKIPPED` while the run exits zero alongside the existing RFC-001 / RFC-002 / RFC-003 / RFC-006 / RFC-007 / RFC-008 rows.
- **P2. Implement the listener, the handshake read/parse/write/close loop, and turn `T1`–`T9` green.**
  - **Scope:** replace the `odin_client_listen_open` stub in `odin/client_listen.c` with the four-syscall sequence from §4.2.1's Mechanism block (socket → setsockopt(SO_REUSEADDR) → bind(127.0.0.1:port) → listen(fd, 1)) with `saved = errno; close(fd); errno = saved; return -1` on every failure arm; replace the `odin_client_listen_handshake` stub with the bounded read loop, the `odin_http_parse_connect` dispatch, the response write via file-static `write_all`, and the file-static `graceful_close` (`shutdown(SHUT_WR)` → drain → `close`) from §4.2.2 and §4.2.3's Mechanism blocks; keep the dependency surface limited to `<errno.h>`, `<stddef.h>`, `<stdint.h>`, `<string.h>`, `<sys/socket.h>`, `<netinet/in.h>`, `<arpa/inet.h>`, `<unistd.h>`, and `"odin/http_connect.h"`; remove the `GTEST_SKIP()` first statement from each of `T1`–`T9`.
  - **Depends on:** P1.
  - **Done when:** `T8` passes un-skipped with `AF_INET` / `INADDR_LOOPBACK` / non-zero ephemeral port / `SO_REUSEADDR` set on the fd returned by `odin_client_listen_open(0)` (G1); `T1` passes un-skipped against the byte-exact 200 response plus post-close EOF (G2, G3); `T2` passes un-skipped for all five parser-error rows against the byte-exact `odin_http_response_for_status` slices (G2, G3); `T3`–`T5` pass un-skipped against the read-loop `NEED_MORE` branch (T3), pipelined-byte discard (T4), and `PEER_CLOSED` arm (T5) (G2, G3); `T6` passes un-skipped with the byte-exact 414 slice and `REJECTED` return on the > 8192-byte input (G2, G3, §6 bounded-read enforcement); `T7` passes un-skipped with `IO_ERROR` on the unconnected `AF_UNIX` socket (G2); `T9` passes un-skipped end-to-end over real TCP via `open` + `accept` + `handshake` + peer-side read (G1, G2); `tidy_odin.sh` exits clean on the new files; after a clean `ninja -C out/ tests`, `out/odin_unittests --gtest_brief=1` reports `T1`–`T9` all `PASSED` with no `SKIPPED`, and the surrounding RFC-001 / RFC-002 / RFC-003 / RFC-006 / RFC-007 / RFC-008 suites remain green.
