# RFC-007: Odin CLI `--server` Host[:port] Parser

**Version:** 1.0  
**Author:** Chason Tang  
**Date:** 2026-05-25  
**Status:** Implemented

## 1. Summary

Add `odin/host_addr.{c,h}` exposing `odin_host_addr_parse(const char *s, odin_host_addr_t *out)` that parses the `--server` argument as one of `host`, `host:port`, `[v6]`, or `[v6]:port`. The `:port` arm is optional; when absent, `out->port` is set to `ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER` (4433) so the Client defaults to dialing where the Server defaults to listening. Add `odin/parse_util.{c,h}` with two reusable byte-buffer helpers — `odin_parse_util_port` (strict ASCII-decimal port validator) and `odin_parse_util_split_hostport` (structural `host[:port]` / `[v6][:port]` split) — that the new module and the refactored `odin/http_connect.c` (RFC-003) share so neither carries its own copy of the bracket-strip and digit-grammar code (`odin/cli.c`'s `parse_listen_port` keeps its own loop per §3 Non-Goals). `odin_cli_args_t.server_addr` is replaced by three flat fields: `server_host` / `server_host_len` / `server_port`. A new `ODIN_CLI_ERR_BAD_SERVER` status slots between `ERR_BAD_LISTEN_PORT` and `ERR_MISSING_REQUIRED`.

## 2. Motivation

RFC-006 swapped `--listen` to a typed `uint16_t listen_port` but left `--server` as an opaque `const char *` alias to `argv` (`odin/cli.h:75`). Two concrete pains follow: (1) the upcoming Client-side socket-connect step has no way to consume that opaque string — every caller (the connect step, future test fixtures, alternate front-ends) would re-derive the same `host[:port]` split; (2) RFC-003's HTTP CONNECT parser already contains nearly the same split + port-digit logic as private `static` helpers in `odin/http_connect.c:99-133`, and leaving the new `--server` parser and the HTTP CONNECT parser to each maintain their own `[0-9]{1,5}` + `≤65535` check, IPv6 bracket strip, and `host[:port]` colon split would let those copies drift over time. A third pain is UX: `--server example.com` (no port) is the shape a developer types most often, and there is no contract today for what port the Client uses — the natural default is the Server's listen default (`4433`), so the Client and Server pair on a single host with no flags on either side. Extracting `odin_parse_util_*` into a new shared module lets both parsers route through one validator while `odin_host_addr_parse` adds the optional-port arm. No data available at this time.

## 3. Goals and Non-Goals

- **G1.** Add `odin/parse_util.{c,h}` exporting `odin_parse_util_port(const uint8_t *bytes, size_t n)` and `odin_parse_util_split_hostport(const uint8_t *bytes, size_t n, odin_parse_util_hostport_t *out)` with the contracts pinned in §4.2.1 / §4.2.2; refactor `odin/http_connect.c` (RFC-003) so its private bracket-split block (`odin/http_connect.c:99-117`) and port-validation loop (`odin/http_connect.c:119-133`) call the new helpers, leaving the existing 10-row RFC-003 unit-test suite passing byte-identical to its pre-refactor results.
- **G2.** Add `odin/host_addr.{c,h}` exporting `odin_host_addr_parse(const char *s, odin_host_addr_t *out)` that returns `ODIN_HOST_ADDR_OK` on every input matching one of the four shapes `host` / `host:port` / `[v6]` / `[v6]:port`, filling `out->host` (aliasing `s`, brackets stripped), `out->host_len` (`∈ [1, ODIN_PROTO_HOST_MAX]`), and `out->port` (the parsed digits, or `ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER = 4433` when `:port` is absent); returns `ODIN_HOST_ADDR_ERR_EMPTY` / `ODIN_HOST_ADDR_ERR_BAD_TARGET` / `ODIN_HOST_ADDR_ERR_PORT_INVALID` / `ODIN_HOST_ADDR_ERR_HOST_LEN_INVALID` on rejection, with `*out` unmodified on every non-OK return.
- **G3.** Replace `odin_cli_args_t.server_addr` with three flat fields `const char *server_host` / `size_t server_host_len` / `uint16_t server_port`, and append `ODIN_CLI_ERR_BAD_SERVER` to `odin_cli_status_t` slotted between `ERR_BAD_LISTEN_PORT` and `ERR_MISSING_REQUIRED` in the precedence cascade; `odin_cli_parse` invokes `odin_host_addr_parse` on the `--server` value and writes the three fields only on the `OK` arm; `odin_cli_main`'s Client-mode banner becomes `server=<H>:<P>` (with `[...]` brackets re-added when `<H>` contains `:`), and a new `ERR_BAD_SERVER` banner row is added.

**Non-Goals:**

- TCP `connect(2)` / `getaddrinfo` resolution of the parsed host — the socket-side work is a separate RFC; the parser produces opaque host bytes (DNS / IP-literal syntax remains the Server's concern per `odin/protocol.h:32-34`).
- Multi-server / failover — one `host[:port]` per `--server` invocation; the field set carries no list.
- `--listen` host parsing — RFC-006 scoped `--listen` to bare port digits; that contract is unchanged here.
- Rewire `odin/cli.c`'s `parse_listen_port` digit loop (`odin/cli.c:47-66`) under `odin_parse_util_port` — that helper's 3-way `OK_PARSED` / `BAD_PORT_USE_DEFAULT` / `BAD_PORT` status (the `NULL` / empty arm signals default-port-use, absent from the 2-way `odin_parse_util_port` contract) needs a wrapper to compose; the rewire is a follow-on cleanup left to a later RFC so this one's diff stays scoped to the `--server` parser plus the in-place HTTP refactor.
- Punycode / IDN normalization — host bytes pass through unmodified, matching RFC-003's opaque `host-token` treatment.

## 4. Design

### 4.1 Overview

Two new modules join `source_set("odin")` at `odin/BUILD.gn:24-33`: `odin/parse_util.{c,h}` (shared byte helpers) and `odin/host_addr.{c,h}` (the `--server` wrapper). `odin/http_connect.c` is refactored in place — its `static` bracket-split (`odin/http_connect.c:99-117`) and port-validation loop (`odin/http_connect.c:119-133`) are replaced by calls to the new helpers; the `find_byte` / `find_crlf` / `find_double_crlf` statics all stay private (the surviving `find_byte` call at `odin/http_connect.c:81` scans the request-line SP separator, outside the helpers' surface). `odin/cli.h` swaps `server_addr` for the three flat fields and appends `ODIN_CLI_ERR_BAD_SERVER`. `odin/cli.c` invokes `odin_host_addr_parse` after `getopt_long` returns the `--server` value, raises the new status on parse failure, and updates the OK-arm banner. New test sources `odin/parse_util_unittests.cpp` and `odin/host_addr_unittests.cpp` join `executable("odin_unittests")` at `odin/BUILD.gn:68-89`; `odin/cli_unittests.cpp` changes only in row-level test data (per §5). No new GN target, no new symlink, no root `BUILD.gn` edit. Both new modules are pure: byte buffer or `const char *` in, struct + status out, no I/O, no allocation, no global state.

Dependency layering after the change:

```
  odin/cli.c  ->  odin/host_addr.c  --.
                                       v
  odin/http_connect.c  -----------> odin/parse_util.c
```

### 4.2 Detailed Design

#### 4.2.1 `odin_parse_util_port` Helper (`odin/parse_util.h`)

```c
/* odin/parse_util.h — port arm (split arm: §4.2.2) */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {  /* both arms */
#endif

typedef enum {
  ODIN_PARSE_UTIL_PORT_OK = 0,
  ODIN_PARSE_UTIL_PORT_BAD,
} odin_parse_util_port_status_t;

typedef struct {
  odin_parse_util_port_status_t status;
  uint16_t port;
} odin_parse_util_port_result_t;

odin_parse_util_port_result_t odin_parse_util_port(const uint8_t *bytes,
                                                   size_t n);
```

**Unstated contract.** Accepts `[0-9]{1,5}` ≤ 65535; the bound check fires in-loop before `uint16_t` narrowing, so `99999` rejects without wrapping below threshold. Leading zeros accepted; `n == 0` or any non-digit byte → `BAD` with `port == 0`. Pure; bounded scan over `[0, n)`.

**Mechanism.** Iterate `bytes[0..n)` into a `uint32_t acc`: reject non-digit bytes, fold `acc = acc * 10 + (bytes[i] - '0')`, reject when `acc > 65535` before the next multiply overflows `uint32_t`; narrow to `uint16_t` and return `{OK, port}`. Lifted from `odin/cli.c:47-66` / `odin/http_connect.c:119-133`.

Satisfies: G1 via the port-validator signature, subsuming RFC-006's `parse_listen_port` and RFC-003's CONNECT-port digit copies.

#### 4.2.2 `odin_parse_util_split_hostport` Helper (`odin/parse_util.h`)

```c
/* odin/parse_util.h — split arm (port arm: §4.2.1) */

typedef struct {
  size_t host_off;
  size_t host_len;
  size_t port_off;
  size_t port_len;
  uint8_t port_present;
} odin_parse_util_hostport_t;

typedef enum {
  ODIN_PARSE_UTIL_HOSTPORT_OK = 0,
  ODIN_PARSE_UTIL_HOSTPORT_BAD,
} odin_parse_util_hostport_status_t;

odin_parse_util_hostport_status_t odin_parse_util_split_hostport(
    const uint8_t *bytes, size_t n, odin_parse_util_hostport_t *out);

#ifdef __cplusplus
}  /* extern "C" */
#endif
```

**Unstated contract.** *Structural* split — caller validates port digits, so `"a:b:443"` splits at the first `:` and `"a:"` / `"[v6]:"` split `OK` with `port_present == 1` and `port_len == 0`; §4.2.3 rejects the empty-digit arm via `odin_parse_util_port`. `BAD` for `n == 0`, zero-length host, unbalanced bracket, or non-`:` byte after `]`. On `OK`, `host_off ∈ {0, 1}` (1 post-`[`); `port_off` / `port_len` apply only when `port_present != 0`. `*out` unmodified on `BAD`. Pure; bounded scan over `[0, n)`.

**Mechanism.**

```
split_hostport(bytes, n, *out):
  if n == 0: return BAD
  if bytes[0] == '[':
    rb = first ']' in bytes[1..n)
    if rb in {NOT_FOUND, 1}: return BAD               /* missing ']' or empty v6 */
    if rb+1 == n: *out = {1, rb-1, 0, 0, 0}; return OK
    if bytes[rb+1] != ':': return BAD
    *out = {1, rb-1, rb+2, n-rb-2, 1}; return OK
  cl = first ':' in bytes[0..n)
  if cl == NOT_FOUND: *out = {0, n, 0, 0, 0}; return OK
  if cl == 0: return BAD                              /* ":port" */
  *out = {0, cl, cl+1, n-cl-1, 1}; return OK
```

Body adapted from `odin/http_connect.c:99-117` with `port_present == 0` arms added. The HTTP refactor pairs this with §4.2.1 on the request-target slice; `port_present == 0` maps to `ODIN_HTTP_ERR_BAD_REQUEST_TARGET` (CONNECT requires the port).

Satisfies: G1 via the split signature, subsuming RFC-003's bracket / colon parse.

#### 4.2.3 `odin_host_addr_parse` Wrapper (`odin/host_addr.h`)

```c
/* odin/host_addr.h */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  ODIN_HOST_ADDR_OK = 0,
  ODIN_HOST_ADDR_ERR_EMPTY,
  ODIN_HOST_ADDR_ERR_BAD_TARGET,
  ODIN_HOST_ADDR_ERR_PORT_INVALID,
  ODIN_HOST_ADDR_ERR_HOST_LEN_INVALID,
} odin_host_addr_status_t;

typedef struct {
  const char *host;
  size_t host_len;
  uint16_t port;
} odin_host_addr_t;

odin_host_addr_status_t odin_host_addr_parse(const char *s,
                                             odin_host_addr_t *out);

#ifdef __cplusplus
}
#endif
```

**Unstated contract.** `s` is a NUL-terminated `argv` slot; the parser reads `[0, strlen(s))` and never mutates it. On `OK`, `out->host` aliases `s` (offset `0` reg-name / IPv4, `1` bracketed); `out->host_len ∈ [1, ODIN_PROTO_HOST_MAX]` (the `255` cap from `odin/protocol.h:56`); `out->port` is the parsed digits, or `ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER` (4433) when `:port` is absent — every accepted shape yields a `(host, host_len, port)` triple usable by `odin_proto_encode_connect_req` (`odin/protocol.h:74-79`). Status mapping: `s == NULL` or empty → `ERR_EMPTY`; structural malformation → `ERR_BAD_TARGET`; bad port digits → `ERR_PORT_INVALID`; `host_len > ODIN_PROTO_HOST_MAX` → `ERR_HOST_LEN_INVALID` (zero-length hosts are rejected upstream as `ERR_BAD_TARGET`). `*out` unmodified on every non-OK return. Pure: no I/O, no allocation, no global state.

**Mechanism.**

```
odin_host_addr_parse(s, *out):
  if s == NULL or s[0] == '\0':                   return ERR_EMPTY
  n = strlen(s)
  hp = {} ; r = split_hostport((const uint8_t *)s, n, &hp)
  if r != OK:                                     return ERR_BAD_TARGET
  if hp.host_len > ODIN_PROTO_HOST_MAX:           return ERR_HOST_LEN_INVALID
  if hp.port_present:
    pr = port((const uint8_t *)s + hp.port_off, hp.port_len)
    if pr.status != OK:                           return ERR_PORT_INVALID
    port_value = pr.port
  else:
    port_value = ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER
  *out = { s + hp.host_off, hp.host_len, port_value }
  return OK
```

Satisfies: G2 via the four-shape acceptance contract, the default-port fallback, and the error fan-out preserving `*out` on every non-OK return.

#### 4.2.4 `odin/cli.h` Surface Diff and Status Precedence

```c
/* odin/cli.h — diff against RFC-006 §4.2.1 */
typedef enum odin_cli_status_t {
  ODIN_CLI_OK = 0,
  ODIN_CLI_HELP,
  ODIN_CLI_ERR_UNKNOWN_MODE,
  ODIN_CLI_ERR_MISSING_REQUIRED,
  ODIN_CLI_ERR_UNKNOWN_FLAG,
  ODIN_CLI_ERR_BAD_LISTEN_PORT,
  ODIN_CLI_ERR_BAD_SERVER,         /* new */
} odin_cli_status_t;

typedef struct odin_cli_args_t {
  odin_cli_mode_t mode;
  uint16_t listen_port;
  const char *server_host;         /* replaces server_addr */
  size_t server_host_len;          /* new */
  uint16_t server_port;            /* new */
} odin_cli_args_t;
```

Status precedence (highest wins, extending RFC-006 §4.2.3's table):

| Rank | Status |
|------|--------|
| 1 | `ODIN_CLI_ERR_UNKNOWN_MODE` |
| 2 | `ODIN_CLI_HELP` |
| 3 | `ODIN_CLI_ERR_UNKNOWN_FLAG` |
| 4 | `ODIN_CLI_ERR_BAD_LISTEN_PORT` |
| 5 | `ODIN_CLI_ERR_BAD_SERVER` |
| 6 | `ODIN_CLI_ERR_MISSING_REQUIRED` |
| 7 | `ODIN_CLI_OK` |

**Unstated contract.** The three new fields are meaningful only on `ODIN_CLI_OK`; every other status leaves them at the entry-block zero-init (`NULL`, `0`, `0`) per RFC-002 §4.2.1. `ERR_BAD_SERVER` slots above `MISSING_REQUIRED` (caller-supplied-but-invalid is more specific than caller-omitted) and below `ERR_BAD_LISTEN_PORT`; the relative order between the two value-rejection statuses is pinned but arbitrary. `MISSING_REQUIRED` still covers Client invoked without `--server`. The new enumerator appends, preserving prior integer values. On `OK`, `server_host` aliases the `argv` slot at `&argv_value[0]` (bracket-less) or `&argv_value[1]` (bracketed). Server mode never invokes `odin_host_addr_parse` (`kServerLong[]` at `odin/cli.c:85-89` excludes `--server`), so a Server `OK` row leaves all three fields zero-init.

**Mechanism.** After `getopt_long`'s loop captures `listen_arg` / `server_arg`, a stack-local `sr_status = (mode == CLIENT && server_arg != NULL) ? odin_host_addr_parse(server_arg, &sr) : ODIN_HOST_ADDR_OK` runs after the existing `parse_listen_port` call (`odin/cli.c:200`). The precedence cascade gains one arm `else if (sr_status != ODIN_HOST_ADDR_OK) status = ODIN_CLI_ERR_BAD_SERVER;` between `BAD_PORT` and `MISSING_REQUIRED`. The Client `OK` field-write block appends `out->server_host = sr.host; out->server_host_len = sr.host_len; out->server_port = sr.port;`.

Satisfies: G3 via the field swap, the new enumerator's precedence slot, and the OK-arm write that fills the three new fields only on success.

#### 4.2.5 `odin_cli_main` Banner Update

```c
/* odin/cli.c — diff against RFC-006 §4.2.4 success arm */
case ODIN_CLI_OK:
  if (args.mode == ODIN_CLI_MODE_CLIENT) {
    const int host_has_colon =
        memchr(args.server_host, ':', args.server_host_len) != NULL;
    const char *fmt = host_has_colon
        ? "odin: mode=client listen=%u server=[%.*s]:%u\n"
        : "odin: mode=client listen=%u server=%.*s:%u\n";
    (void)fprintf(err, fmt,
                  (unsigned)args.listen_port,
                  (int)args.server_host_len, args.server_host,
                  (unsigned)args.server_port);
  } else { /* Server mode unchanged from RFC-006 §4.2.4 */
    (void)fprintf(err, "odin: mode=server listen=%u\n",
                  (unsigned)args.listen_port);
  }
  rc = 0;
  break;

/* New row appended to RFC-006 §4.2.4 status table: */
case ODIN_CLI_ERR_BAD_SERVER:
  (void)fputs("odin: invalid --server\n", err);
  (void)fputs(um, err);
  (void)fputc('\n', err);
  rc = 2;
  break;
```

**Unstated contract.** `%.*s` is required because the host slice in `argv` is not NUL-terminated after the parse (followed by `]` or `:`), so `%s` would over-read. Bracket re-add fires only on the Client `OK` arm — `memchr(args.server_host, ':', args.server_host_len)` distinguishes IPv6 (which contains `:`) from reg-name / IPv4 (which does not), avoiding a `was_bracketed` flag. Defaulted-port input `--server example.com` prints `server=example.com:4433` so the user sees the filled-in port. Server-mode `OK` is unchanged from RFC-006 §4.2.4. The `ERR_BAD_SERVER` first line is distinct from `unknown or invalid flag` and `invalid --listen port`, so a user typing `--server bad:99999` knows which token to fix; usage and `rc = 2` follow the RFC-002 §4.2.2 invalid-invocation pattern, with both streams `fflush`ed before return.

**Mechanism.** Client `OK`: the `memchr` result picks between `[%.*s]:%u` and `%.*s:%u`; one `fprintf` emits `mode=client listen=<P> server=<H>:<P>`; `rc = 0`. Server `OK`: unchanged from RFC-006 §4.2.4. `ERR_BAD_SERVER`: `fputs("odin: invalid --server\n", err)`, `fputs(um, err)`, `fputc('\n', err)`, `rc = 2`.

Satisfies: G3 via the parsed `server=<H>:<P>` banner with bracket re-add and the dedicated `ERR_BAD_SERVER` row.

### 4.3 Design Rationale

- **Chosen:** Two separate new modules — `odin/parse_util.{c,h}` (shared byte helpers) and `odin/host_addr.{c,h}` (the `--server` wrapper).
- **Reason:** `odin_parse_util_*` has two callers from day one (`odin/http_connect.c` after the refactor, `odin/host_addr.c` as a new caller); folding it into `host_addr` would force `http_connect.c` to either include a `--server`-specific header or duplicate the digit / split logic (undoing G1), and folding it into `http_connect` would leak the HTTP layer's identity into the CLI surface for two byte helpers.
- **Ruled out:** Single `odin/host_addr.{c,h}` with `static` helpers — saves one file at the cost of forcing `http_connect.c` to keep its private copies, so G1 fails on day one.

- **Chosen:** When `:port` is absent, default to `ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER` (4433) inside `odin_host_addr_parse` itself; no `port_was_supplied` flag.
- **Reason:** Output is always a usable `(host, host_len, port)` triple so every caller has one code path to `odin_proto_encode_connect_req`; pushing the default to the caller has one caller today and would force every future caller to re-implement it. The Server's listen default is the only port the Client could reasonably default to (Client + Server on a single dev box need to pair without flags); reusing the macro avoids a second number drifting from the first.
- **Ruled out:** A new `ODIN_CLI_DEFAULT_CLIENT_DIAL_PORT` macro to let Client-dial and Server-listen defaults diverge later — saves zero work today (value would be `4433` either way); a future RFC can introduce the asymmetry cheaply when needed.

## 5. Backward Compatibility & Migration

- **Breaks:** `odin_cli_args_t.server_addr` is removed in favor of the three flat fields `server_host` / `server_host_len` / `server_port` (§4.2.4); the Client-mode `OK` banner reshapes from `server=%s` (RFC-006 §4.2.4) to `server=%.*s:%u` with optional `[...]` brackets (§4.2.5); a `--server` token that does not match the four accepted shapes (e.g., `--server ""`, `--server "[::1"`, `--server "a:99999"`, `--server ":443"`) no longer parses, returning the new `ERR_BAD_SERVER` ranked above `ERR_MISSING_REQUIRED`.
- **Symptom on un-migrated caller:** code that read `args.server_addr` fails to compile with `error: 'odin_cli_args_t' {aka 'struct odin_cli_args_t'} has no member named 'server_addr'`; every `cli_unittests.cpp` row that asserts on `server_addr` — RFC-002 T1 (`cli_unittests.cpp:110` / `:119`), T2 (`:131` / `:139`), T3 (`:153` / `:162`), T4 (`:186`), T5 (`:229`), T6 (`:251`), RFC-006 T1 (`:410` / `:418`), RFC-006 T4 (`:470`) — fails compilation; RFC-002 T8 OK-CLIENT row at `cli_unittests.cpp:301-303` and RFC-006 T7 OK-CLIENT rows at `:521-527` (whose expected `err` strings end in `server=S\n`) fail `EXPECT_STREQ` because the new byte sequence becomes `server=S:4433\n`.
- **Migration:** in this RFC's diff, replace every `.server_addr` read in `odin/cli.c` and `odin/cli_unittests.cpp` with the three-field set. For OK-arm rows with bare-host `-s` (every existing row uses `"S"`), assert `out.server_host == &argv_value[0]` (alias check), `out.server_host_len == strlen(argv_value)`, and `out.server_port == ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER` — the triple holds under both the P1 stub (whole `s` becomes the host, port defaulted) and the P2 real parser. Shorten RFC-002 T1's `-s "quic.example.com:4433"` to `-s "quic.example.com"` so `server_port == 4433` holds in both phases (otherwise P1 treats the full token as a host while P2 splits it, values diverging). For non-OK rows replace `EXPECT_EQ(out.server_addr, nullptr)` with three `EXPECT_EQ`s on `server_host` / `server_host_len` / `server_port` against `nullptr` / `0` / `0` (zero-init persists on every non-OK arm). Update RFC-002 T8 OK-CLIENT expected `err` to `"odin: mode=client listen=8080 server=S:4433\n"`; append `:4433` to the `server=...` suffix of every RFC-006 T7 OK row. Leave RFC-002 T4's no-`-s` row alone — it returns `ERR_MISSING_REQUIRED` and its migrated `out.server_port == 0` assertion passes in both phases.

## 6. Security

Not applicable — `--server` bytes are local user-controlled `argv` (the user already owns the process they invoke), no network or filesystem input enters the parser, the `odin_parse_util_*` helpers preserve the `[0, n)` bounded-scan discipline RFC-003 §6 named (asserted by §7 T2/T3 directly and end-to-end by the existing RFC-003 T1–T10 suite running unchanged against the refactored `odin/http_connect.c`), and the port-digit accumulator's `> 65535` check fires inside the loop before any `uint32_t → uint16_t` narrowing so no integer overflow can wrap the parsed value below the rejection threshold.

## 7. Testing Strategy

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | `odin_parse_util_port` boundary + reject arms | Inputs `("0",1)`, `("65535",5)`, `("00080",5)`, `("65536",5)`, `("99999",5)`, `("123456",6)`, `("abc",3)`, `("",0)` | First three return `OK` with `port` `0`/`65535`/`80`; remaining five return `BAD` with `port == 0`; the in-loop `> 65535` check rejects `"65536"` / `"99999"` before `uint16_t` narrowing | G1 | unit |
| T2 | `odin_parse_util_split_hostport` accepted shapes (including empty-port boundary) | Inputs `"example.com"`, `"example.com:443"`, `"[::1]"`, `"[::1]:443"`, `"[2001:db8::1]:8080"`, `"127.0.0.1:9000"`, `"a:1"`, `"a:"`, `"[::1]:"` | All return `OK`; first and third have `port_present == 0`; remaining seven have `port_present == 1` with port slices `"443"`/`"443"`/`"8080"`/`"9000"`/`"1"`/`""`/`""` (last two with `port_len == 0`, pinning split-OK for trailing-colon-without-digits); host slices via `(bytes + host_off, host_len)` equal `"example.com"`/`"example.com"`/`"::1"`/`"::1"`/`"2001:db8::1"`/`"127.0.0.1"`/`"a"`/`"a"`/`"::1"` | G1 | unit |
| T3 | `odin_parse_util_split_hostport` malformed shapes | Inputs `""`, `":"`, `":443"`, `"[]"`, `"[]:443"`, `"[::1"`, `"[::1:443"`, `"[::1]443"`, `"[::1]X"` | All return `ODIN_PARSE_UTIL_HOSTPORT_BAD`; `*out` (pre-filled with a sentinel `odin_parse_util_hostport_t`) is byte-identical after the call | G1 | unit |
| T4 | `odin_host_addr_parse` OK arms (four shapes + default-port fallback + leading-zero port) | Inputs `"example.com"`, `"example.com:443"`, `"[::1]"`, `"[::1]:8080"`, `"127.0.0.1:9000"`, `"a:00001"` | All return `ODIN_HOST_ADDR_OK`; `out.host` aliases the input at offsets `0` / `0` / `1` / `1` / `0` / `0` with `out.host_len` `11` / `11` / `3` / `3` / `9` / `1`; `out.port` is `ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER` (4433) for the first and third inputs and `443` / `8080` / `9000` / `1` for the others | G2 | unit |
| T5 | `odin_host_addr_parse` error arms | Inputs `NULL` and `""` → `ERR_EMPTY`; `"[::1"`, `":443"`, `"[]"` → `ERR_BAD_TARGET`; `"a:99999"`, `"a:abc"`, `"a:b:443"`, `"a:"`, `"[::1]:"` → `ERR_PORT_INVALID` (last two compose split-OK-with-empty-port through the wrapper's `odin_parse_util_port` call returning `BAD` on `n == 0`, pinning the user-visible code at the port layer rather than the split layer); `("x" * 256) + ":443"` → `ERR_HOST_LEN_INVALID` | Every input returns its mapped status; `*out` (pre-filled with a sentinel `odin_host_addr_t`) is byte-identical after the call on every non-OK return | G2 | unit |
| T6 | `odin_cli_parse` `--server` integration (parse, struct fields, default port, alias) | `argv = {"odin-client", "-l", "8080", "-s", S}` for `S ∈ {"example.com", "example.com:443", "[::1]:8080", "[::1]"}` | All return `ODIN_CLI_OK`; `out.server_host == &argv_value[host_off]` for `host_off ∈ {0, 0, 1, 1}`; `out.server_host_len` is `11`/`11`/`3`/`3`; `out.server_port` is `4433`/`443`/`8080`/`4433` | G3 | unit |
| T7 | Status precedence (`BAD_LISTEN_PORT > BAD_SERVER > MISSING_REQUIRED`) | `{"odin-client", "-l", "abc", "-s", "bad:99999"}`, `{"odin-client", "-l", "8080", "-s", "bad:99999"}`, `{"odin-client", "-l", "8080", "-s", "[::1"}`, `{"odin-client", "-l", "8080"}` (no `-s` flag) | First returns `ERR_BAD_LISTEN_PORT` (bad listen port wins over bad server); second returns `ERR_BAD_SERVER` via the `ERR_PORT_INVALID` rejection of `"99999"`; third returns `ERR_BAD_SERVER` via the `ERR_BAD_TARGET` rejection of the unbalanced bracket; fourth returns `ERR_MISSING_REQUIRED`; every case leaves `out.server_host == NULL`, `out.server_host_len == 0`, `out.server_port == 0` | G3 | unit |
| T8 | `odin_cli_main` banner: `server=<H>:<P>` with bracket re-add for v6, plus the new `ERR_BAD_SERVER` row | Through `fmemopen` capture per RFC-002 T8: rows `{"odin-client", "-l", "8080", "-s", "example.com:443"}`, `{"odin-client", "-l", "8080", "-s", "example.com"}` (defaulted port), `{"odin-client", "-l", "8080", "-s", "[::1]:8080"}` (v6 bracket re-add), `{"odin-client", "-l", "8080", "-s", "bad:99999"}` (bad server) | First three write to `err` (in order) `"odin: mode=client listen=8080 server=example.com:443\n"`, `"odin: mode=client listen=8080 server=example.com:4433\n"`, `"odin: mode=client listen=8080 server=[::1]:8080\n"` and return `0`; the fourth writes `"odin: invalid --server\nusage: odin-client --listen ADDR --server ADDR\n"` and returns `2` | G3 | unit |

## 8. Implementation Plan

- **P1. Land the two new module surfaces, the `cli.h` struct + enum diff, the in-place test migration of RFC-002 / RFC-006 rows, and red `T1`–`T8`.**
  - **Scope:** add `odin/parse_util.h` with the §4.2.1 / §4.2.2 contract surfaces and `odin/parse_util.c` with both function bodies stubbed to return `BAD` so the binary links (no P1 caller invokes them); add `odin/host_addr.h` per §4.2.3 and `odin/host_addr.c` with `odin_host_addr_parse` stubbed to `if (s == NULL || s[0] == '\0') return ODIN_HOST_ADDR_ERR_EMPTY; *out = { s, strlen(s), ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER }; return ODIN_HOST_ADDR_OK;` — every supplied `--server` value is treated as a bare-host with the default port, matching the asserted P2 outputs for every bracket-less, colon-less `-s` input the migrated test data uses (per §5); update `odin/cli.h` per §4.2.4 with the precedence and grammar paragraphs added to the header doc-comment; update `odin/cli.c`'s `*out` zero-init to cover the three new fields, route `server_arg` through `odin_host_addr_parse`, add the `BAD_SERVER` precedence slot (dead under P1's stub), and update `odin_cli_main` per §4.2.5 (Client `OK` arm to the `%.*s:%u` format with bracket re-add, plus the `ERR_BAD_SERVER` switch arm); append the four new source/header names to `source_set("odin")` at `odin/BUILD.gn:24-33`; create `odin/parse_util_unittests.cpp` with rows `T1`–`T3` and `odin/host_addr_unittests.cpp` with rows `T4`–`T5`, append rows `T6`–`T8` to `odin/cli_unittests.cpp`, each row's first statement is `GTEST_SKIP() << "pending RFC-007 P2";`; append the two new `*_unittests.cpp` names to `executable("odin_unittests")` at `odin/BUILD.gn:68-89`; migrate the RFC-002 / RFC-006 rows §5 names. No edits to `odin/http_connect.{c,h}` in P1.
  - **Depends on:** None.
  - **Done when:** `gn gen out/` and `ninja -C out/ tests` build cleanly against the modified `:odin` and `:odin_unittests` targets; `odin/parse_util.h` and `odin/host_addr.h` export the §4.2.1 / §4.2.2 / §4.2.3 surfaces (G1, G2 staged); `odin/cli.h` exports the three new fields and the new enumerator (G3 staged); RFC-002 T1–T10, RFC-003 T1–T10, and RFC-006 T1–T8 all pass under the migrated test data; `T1`–`T8` are committed in `GTEST_SKIP`-wrapped (red) state and `out/odin_unittests --gtest_brief=1` reports all eight as `SKIPPED` while the run exits zero.
- **P2. Implement the helpers, the wrapper, the `http_connect` refactor, and turn `T1`–`T8` green.**
  - **Scope:** replace the `odin/parse_util.c` stubs with the bodies pinned in §4.2.1 / §4.2.2 (port-digit sweep lifted from `odin/cli.c:47-66` / `odin/http_connect.c:119-133`; bracket / colon split adapted from `odin/http_connect.c:99-117` and extended with the `port_present == 0` arms); replace the `odin/host_addr.c` stub with the §4.2.3 mechanism; refactor `odin/http_connect.c` per §4.1 — `#include "odin/parse_util.h"`, replace the bracket-split block (`odin/http_connect.c:99-117`) with a call to `odin_parse_util_split_hostport(buf + target_start, target_end - target_start, &hp)` followed by `host_off = target_start + hp.host_off; host_len = hp.host_len; port_start = target_start + hp.port_off;` returning `ODIN_HTTP_ERR_BAD_REQUEST_TARGET` when `r != OK || hp.port_present == 0`, and replace the port-validation loop (`odin/http_connect.c:119-133`) with a call to `odin_parse_util_port(buf + port_start, hp.port_len)` returning `ODIN_HTTP_ERR_PORT_INVALID` on `BAD`; the `find_byte` static (`odin/http_connect.c:43-51`) and its surviving SP-scan call site at `odin/http_connect.c:81` stay in place — they belong to HTTP request-line parsing, outside the host[:port] helper surface; remove the `GTEST_SKIP() << "pending RFC-007 P2";` first statement from every `T1`–`T8`; no further edits to `odin/cli.c` (the precedence cascade and OK-arm field writes land in P1 and become live once the stub starts returning real values).
  - **Depends on:** P1.
  - **Done when:** `T1` passes un-skipped against every boundary and reject port input (G1); `T2`–`T3` pass un-skipped against every accepted and malformed split shape (G1); `T4`–`T5` pass un-skipped against the four-shape `odin_host_addr_parse` OK arms and every error arm (G2); `T6` passes un-skipped against the four cli `--server` integration rows including v6 bracket-strip alias semantics (G3); `T7` passes un-skipped against every precedence pair (G3); `T8` passes un-skipped against byte-exact `err` for the three `OK` rows (defaulted port, supplied port, v6 bracket re-add) and the new `BAD_SERVER` row (G3); RFC-003 T1–T10 stay `PASSED` against the refactored `odin/http_connect.c` (G1 — the unchanged 10-row suite proves the refactor preserves the public contract); `tidy_odin.sh` exits clean on the new / modified files; after a clean `ninja -C out/ tests`, `out/odin_unittests --gtest_brief=1` reports `T1`–`T8` all `PASSED`, every migrated RFC-002 / RFC-006 row and the unchanged RFC-001 / RFC-003 rows remain `PASSED`, and zero rows are `SKIPPED`.
