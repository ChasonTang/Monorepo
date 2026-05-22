# RFC-006: Odin CLI `--listen` Port Parser

**Version:** 1.0  
**Author:** Chason Tang  
**Date:** 2026-05-22  
**Status:** Proposed

## 1. Summary

Replace `odin_cli_args_t.listen_addr` (opaque `const char *` aliasing the `argv` slot at `odin/cli.h:52`) with a typed `uint16_t listen_port` populated by a strict ASCII-decimal parser embedded in `odin_cli_parse`. The parser accepts only `[0-9]+` strings whose value is `≤ 65535`; any other non-empty value returns a new `ODIN_CLI_ERR_BAD_LISTEN_PORT` status without writing `listen_port`. When `--listen` is omitted from `argv` or supplied as the empty string, `listen_port` is filled from a per-mode default — `ODIN_CLI_DEFAULT_LISTEN_PORT_CLIENT = 8080`, `ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER = 4433` — both pinned as macros in `odin/cli.h` so a single host can run both modes concurrently for local testing without flag scaffolding. `odin_cli_main`'s success banner now prints the parsed port as decimal (`listen=8080`). Existing RFC-002 test data that supplied non-digit `--listen` values migrates in the same change.

## 2. Motivation

`odin_cli_parse` currently captures `--listen` as an opaque alias at `odin/cli.c:174` (`out->listen_addr = listen_arg;`), and §4.2.1 of RFC-002 deferred address parsing to "the follow-up RFCs that open sockets". The Server-side socket binder coming next has no way to consume a `const char *` alias as a `uint16_t` port without a parser, and encoding that parser inline in the socket-bind call would force every future caller (test fixture, embedded harness, alternate front-end) to redo the same digit walk. A second concrete pain comes from local testing: when a developer runs `out/odin-server` and `out/odin-client` on the same host, both listeners share `INADDR_ANY` by convention, so they need distinct default ports to avoid a `bind(2) EADDRINUSE`. Hard-coding a single port in the binder caller would force every developer to type `--listen <port>` for at least one of the two processes; per-mode defaults pinned in `odin/cli.h` let `out/odin-server` and `out/odin-client` both start with no flags. No data available at this time.

## 3. Goals and Non-Goals

- **G1.** Replace `odin_cli_args_t.listen_addr` with `uint16_t listen_port`; `odin_cli_parse` populates `listen_port` on `OK` by interpreting the supplied `--listen` value as an ASCII decimal port string in `[0, 65535]`, and returns a new `ODIN_CLI_ERR_BAD_LISTEN_PORT` status without writing `listen_port` when the value contains any non-digit byte or evaluates to `≥ 65536`.
- **G2.** When `--listen` is omitted from `argv` or supplied as the empty string, `listen_port` is set to the mode-specific default — `ODIN_CLI_DEFAULT_LISTEN_PORT_CLIENT = 8080` for `ODIN_CLI_MODE_CLIENT`, `ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER = 4433` for `ODIN_CLI_MODE_SERVER` — both exported as preprocessor macros from `odin/cli.h` so callers reference them by name; `--listen` ceases to be a required flag, so Server mode with no flags returns `OK`.

**Non-Goals:**

- `--server` value parsing — the requirement is scoped to `--listen`; `out.server_addr` keeps its opaque `const char *` aliasing contract from RFC-002 §4.2.1.
- `host:port` / `[v6]:port` / `:port` accepted shapes — `--listen` is bare port digits only; introducing host parsing now would commit to IPv6 bracket and reg-name semantics the listener does not yet need (the socket binder lands in a follow-up RFC and can revisit the shape if required).
- Privileged-port (`< 1024`) gating, port-in-use probing, or default-port collision detection — port `0` through `65535` are all accepted by the parser; what `bind(2)` does with the value is the binder's concern.
- Default-port runtime overrides via env vars or config files — the two macros in `odin/cli.h` are the only knob; recompiling is the migration path if a deployment needs different numbers.

## 4. Design

### 4.1 Overview

This RFC modifies `odin/cli.h` and `odin/cli.c` in place — no new module, no new GN target, no `odin/BUILD.gn` edit — because the existing `source_set("odin")` at `odin/BUILD.gn:24-33` already lists both files. `odin/cli.h` gains `<stdint.h>`, two `ODIN_CLI_DEFAULT_LISTEN_PORT_*` macros, one `ODIN_CLI_ERR_BAD_LISTEN_PORT` enumerator, and the `listen_addr` → `listen_port` field swap. `odin/cli.c` gains a static `parse_listen_port` helper, threads it into `odin_cli_parse`'s flag-capture branch, drops the missing-`--listen` arm of the existing `MISSING_REQUIRED` check, and changes one `fputs` call in `odin_cli_main` to an `fprintf("%u", …)` that prints the parsed port. `odin/cli_unittests.cpp` extends `T1`–`T8` from §7 below, and migrates the RFC-002 test cases whose `--listen` values were arbitrary placeholders (per §5). The parser remains pure: no allocation, no I/O, no global state — `parse_listen_port` runs over the existing `argv`-aliasing pointer with a single byte-by-byte sweep.

N/A — textual description above is sufficient.

### 4.2 Detailed Design

#### 4.2.1 `odin/cli.h` Surface Diff

```c
/* odin/cli.h — diff against RFC-002 §4.2.1 */
#include <stdint.h>

#define ODIN_CLI_DEFAULT_LISTEN_PORT_CLIENT 8080
#define ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER 4433

typedef enum odin_cli_status_t {
  ODIN_CLI_OK = 0,
  ODIN_CLI_HELP,
  ODIN_CLI_ERR_UNKNOWN_MODE,
  ODIN_CLI_ERR_MISSING_REQUIRED,
  ODIN_CLI_ERR_UNKNOWN_FLAG,
  ODIN_CLI_ERR_BAD_LISTEN_PORT, /* new */
} odin_cli_status_t;

typedef struct odin_cli_args_t {
  odin_cli_mode_t mode;
  uint16_t listen_port;       /* replaces listen_addr; populated on OK */
  const char *server_addr;    /* unchanged: alias to argv slot */
} odin_cli_args_t;
```

**Unstated contract.** `listen_port` is meaningful only on `ODIN_CLI_OK`; on every other status it is zero, matching the `*out` zeroing rule already pinned in RFC-002 §4.2.1. The two `ODIN_CLI_DEFAULT_LISTEN_PORT_*` macros are the only place those numbers appear in code, so a future RFC that changes them touches one file. The `ODIN_CLI_ERR_BAD_LISTEN_PORT` value is appended to the enum tail, preserving the integer value of every prior enumerator so any switch over `odin_cli_status_t` that did not explicitly handle the new value still compiles. `MISSING_REQUIRED` no longer fires for a missing `--listen` (G2); it remains the status for a Client-mode invocation that omits `--server`.

Satisfies: G1 via the new `listen_port` field and `ODIN_CLI_ERR_BAD_LISTEN_PORT` enumerator; G2 via the two default-port macros pinned at the header surface.

#### 4.2.2 Parser Function Contract

**Contract surface.**

```c
/* odin/cli.c */
typedef enum {
  OK_PARSED,
  BAD_PORT_USE_DEFAULT,
  BAD_PORT,
} parse_listen_port_status_t;
typedef struct {
  parse_listen_port_status_t status;
  uint16_t port; /* meaningful only when status == OK_PARSED */
} parse_listen_port_result_t;
static parse_listen_port_result_t parse_listen_port(const char *s);
```

| Input | Returned status | `port` |
|-------|-----------------|--------|
| `NULL` or `""` | `BAD_PORT_USE_DEFAULT` | `0` (caller picks the per-mode default) |
| `[0-9]+` with value `≤ 65535` | `OK_PARSED` | the parsed value |
| any non-digit byte, or value `≥ 65536` | `BAD_PORT` | `0` |

**Unstated contract.** Grammar: `[0-9]+`; no sign, no `0x`/`0o` prefix, no whitespace, no separators. `"0"` → port `0`; `"00080"` → port `80`. The in-loop `v > 65535` check fires before any `uint32_t` overflow, so arbitrary-length inputs (e.g. `"18446744073709551616"`) reject cleanly without UB. The parser is pure: no allocation, no I/O, no global state, and writes nothing through caller pointers — its sole output is the returned struct, which the caller stashes in a stack-local. `port` is meaningful only on `OK_PARSED`; the other two variants leave it `0`.

**Mechanism.**

```
parse_listen_port(s):
  if s == NULL or s[0] == '\0':  return {BAD_PORT_USE_DEFAULT, 0}
  v = 0
  for i in [0, strlen(s)):
    if s[i] < '0' or s[i] > '9': return {BAD_PORT, 0}
    v = v * 10 + (s[i] - '0')
    if v > 65535:                return {BAD_PORT, 0}
  return {OK_PARSED, (uint16_t)v}
```

Satisfies: G1 via the byte-by-byte digit check and in-loop `> 65535` rejection; G2 via the `BAD_PORT_USE_DEFAULT` arm that signals "use the per-mode default" to the caller.

#### 4.2.3 Status Precedence and Caller Integration

**Contract surface.** Status precedence within `odin_cli_parse` (highest wins):

| Rank | Status |
|------|--------|
| 1 | `ODIN_CLI_ERR_UNKNOWN_MODE` |
| 2 | `ODIN_CLI_HELP` |
| 3 | `ODIN_CLI_ERR_UNKNOWN_FLAG` |
| 4 | `ODIN_CLI_ERR_BAD_LISTEN_PORT` |
| 5 | `ODIN_CLI_ERR_MISSING_REQUIRED` |
| 6 | `ODIN_CLI_OK` |

**Unstated contract.** The table extends RFC-002 §4.2.1's permutation-invariant ordering (help wins after a valid basename; unknown flags beat missing-required) by slotting `ERR_BAD_LISTEN_PORT` above `ERR_MISSING_REQUIRED`. `out->listen_port` is assigned only on the `ODIN_CLI_OK` arm, so RFC-002 §4.2.1's "fields filled only on `OK`" rule extends verbatim to the new field. On `ERR_BAD_LISTEN_PORT`, `ERR_MISSING_REQUIRED`, or any other non-OK status, `out->listen_port` keeps the `0` written by the entry-block zero of `*out` (and `out->server_addr` keeps its `NULL`), even if `parse_listen_port` returned `OK_PARSED` for the supplied `-l` value. Default-fill routes only after the unknown-flag/help checks, so `{"odin-server", "-l", "", "-x"}` returns `ERR_UNKNOWN_FLAG`, not `OK`.

**Mechanism.** `odin_cli_parse` invokes `parse_listen_port(listen_arg)` between the help/unknown-flag guards and the missing-required check, stashing the returned struct in a stack-local `pr` (no write through any `out` pointer at this point). `pr.status == BAD_PORT` raises the running status to `ERR_BAD_LISTEN_PORT`; otherwise the function falls through to the missing-required check. Only on the final `ODIN_CLI_OK` arm does the function then write `out->listen_port = (pr.status == OK_PARSED) ? pr.port : (mode == CLIENT ? DEFAULT_LISTEN_PORT_CLIENT : DEFAULT_LISTEN_PORT_SERVER)` and (for Client) `out->server_addr = server_arg`; every non-OK arm returns without touching either field, so the case where `pr.status == OK_PARSED` but `MISSING_REQUIRED` wins precedence (Client supplied `-l 8080` without `-s`) leaves `out->listen_port` at `0`, not `8080`.

Satisfies: G1 via the precedence ordering that places `BAD_LISTEN_PORT` above `MISSING_REQUIRED`; G2 via the per-mode macro selection on the `OK`-arm default-fill.

#### 4.2.4 `odin_cli_main` Banner Format Update

```c
/* odin/cli.c — diff against RFC-002 §4.2.2 success arm */
case ODIN_CLI_OK:
  if (args.mode == ODIN_CLI_MODE_CLIENT) {
    (void)fprintf(err, "odin: mode=client listen=%u server=%s\n",
                  (unsigned)args.listen_port, args.server_addr);
  } else {
    (void)fprintf(err, "odin: mode=server listen=%u\n",
                  (unsigned)args.listen_port);
  }
  rc = 0;
  break;

/* New row appended to RFC-002 §4.2.2 status table: */
case ODIN_CLI_ERR_BAD_LISTEN_PORT:
  (void)fputs("odin: invalid --listen port\n", err);
  (void)fputs(um, err);
  (void)fputc('\n', err);
  rc = 2;
  break;
```

**Unstated contract.** `<P>` is the decimal representation of `listen_port` with no zero-padding, matching `printf("%u", uint16_t)` semantics — `8080`, never `08080` or `0x1F90`. The substitution applies to both Client (`listen=<P> server=<S>`) and Server (`listen=<P>`) success rows; every other row in RFC-002 §4.2.2 keeps its byte sequence. The new `ERR_BAD_LISTEN_PORT` row writes a message distinct from `unknown or invalid flag` so the user can tell a malformed port apart from a typo'd flag at a glance; it picks the per-mode usage line via the same `um` selector RFC-002 §4.2.2 uses. Both streams are still `fflush`ed before return per RFC-002 §4.2.2's Unstated contract.

Satisfies: G1 via the parsed-port banner and the dedicated `ERR_BAD_LISTEN_PORT` row.

### 4.3 Design Rationale

- **Chosen:** Bare port digits only as the accepted `--listen` shape.
- **Reason:** The requirement narrowly asks for a `Port uint16_t` parser and the listener needs no host today (Server binds `INADDR_ANY` by convention; Client's local HTTPS_PROXY listener is the same). A digit-only grammar is `[0-9]+` — one inline byte sweep — and ships with no IPv6 bracket / reg-name / `:port` ambiguity to test against. The follow-up RFC that wires the binder can widen the accepted shape if the binder grows a host argument; widening a parser is additive, narrowing it later is a user-visible break.
- **Ruled out:** RFC-003-style `host[:port]` with IPv6 bracket support. The HTTPS_PROXY parser already needs that surface for incoming `CONNECT` request lines, but reusing it here would commit `--listen` to a host token that the listener's `bind(2)` call would have to immediately collapse to `INADDR_ANY` (or do real DNS resolution, also out of scope). Until a caller has a use for the host slot, parsing it is dead work.

- **Chosen:** New `ODIN_CLI_ERR_BAD_LISTEN_PORT` status with a dedicated banner row.
- **Reason:** A user who typed `--listen 99999` reads `odin: invalid --listen port` and knows exactly which token to fix; folding into `ERR_UNKNOWN_FLAG` would print `odin: unknown or invalid flag` for a syntactically valid flag whose value is wrong, sending the user to re-read the flag list instead of the value. The new enumerator also gives a future `--server` port parser one obvious slot to share, rather than overloading a generic flag error.
- **Ruled out:** Reuse `ERR_UNKNOWN_FLAG`. Saves one enum value at the cost of misdirecting every malformed-port report at a flag-name diagnosis the user already passed.

## 5. Backward Compatibility & Migration

- **Breaks:** `odin_cli_args_t.listen_addr` is removed in favor of `listen_port` (§4.2.1); `--listen` values are now strictly bare digits or empty (§4.2.2); the success banner shows `listen=<P>` (decimal port) instead of `listen=<L>` (verbatim argv) (§4.2.4); a missing `--listen` no longer returns `ERR_MISSING_REQUIRED` (§4.2.3 — Server with zero flags now returns `OK` with the default, and because Server has no other required flag the entire `ERR_MISSING_REQUIRED` `SERVER` scenario becomes unreachable).
- **Symptom on un-migrated caller:** code that read `args.listen_addr` fails to compile with `error: ‘odin_cli_args_t’ {aka ‘struct odin_cli_args_t’} has no member named ‘listen_addr’`; every `cli_unittests.cpp` row whose `--listen` value contains a non-digit byte — RFC-002 T1 (`127.0.0.1:8443`), T2 (`0.0.0.0:4433`), T4 case 1 at `cli_unittests.cpp:171` (`127.0.0.1:8443`), T7 OK-CLIENT at `cli_unittests.cpp:263-265` and OK-SERVER at `cli_unittests.cpp:272` (`L`), T8 OK-CLIENT and OK-SERVER rows (`L`), T9 spawned `execve` (`L`) — now short-circuits through `parse_listen_port` to `ODIN_CLI_ERR_BAD_LISTEN_PORT`, so each row's status `EXPECT_EQ` fires the wrong value (`ODIN_CLI_OK` expected on T1/T2/T7/T8/T9 OK arms; `ODIN_CLI_ERR_MISSING_REQUIRED` expected on T4 case 1, because `ERR_BAD_LISTEN_PORT` now wins precedence over `ERR_MISSING_REQUIRED`); `EXPECT_STREQ` against T8 OK rows' `"odin: mode=client listen=L server=S\n"` / `"odin: mode=server listen=L\n"` fails because the actual byte sequence becomes the new `ERR_BAD_LISTEN_PORT` banner from §4.2.4 (and, after migration, the OK banner reshapes to `"odin: mode=client listen=<P> server=S\n"` / `"odin: mode=server listen=<P>\n"`); RFC-002 T4's `{"odin-client", "-s", "quic.example.com:4433"}` and `{"odin-server"}` cases, and RFC-002 T8's `{"odin-server"}` `ERR_MISSING_REQUIRED` `SERVER` row at `cli_unittests.cpp:317-321` (whose assertions are `EXPECT_EQ(rc, 2)` and `EXPECT_STREQ(err_buf, "odin: missing required flag\nusage: odin-server --listen ADDR\n")`), now return `OK` with the per-mode default `listen_port` and `rc = 0` (T8's row would now produce `"odin: mode=server listen=4433\n"`) instead of `ERR_MISSING_REQUIRED` / `rc = 2`, so every one of those assertions fails.
- **Migration:** within this RFC's diff, replace every `.listen_addr` read in `odin/cli.c` and `odin/cli_unittests.cpp` with `.listen_port`; replace every test `--listen <non-digit>` token with the per-mode default-port digits so the migrated row's `listen_port` assertion holds identically under both the P1 stub parser and the P2 real parser — on every OK arm both phases write the same digit value into `out->listen_port` (P1's stub returns `BAD_PORT_USE_DEFAULT` and the OK arm fills the per-mode default macro; P2's real parser returns `OK_PARSED` of the matching digit value), and on every non-OK arm both phases leave `out->listen_port` at the entry-block `0` because §4.2.2's parser writes nothing through caller pointers and §4.2.3 only assigns `out->listen_port` on the `odin_cli_parse` OK arm — Client-mode `--listen` tokens (RFC-002 T1 both sub-tests, T4 case 1, T5 every row whose `-l` value was `"L"`, T7 OK-CLIENT, T8 OK-CLIENT, T9 child argv) take `"8080"` (matching `ODIN_CLI_DEFAULT_LISTEN_PORT_CLIENT`), Server-mode `--listen` tokens (T2 both sub-tests, T5 server-mode `-l "L"` rows, T7 OK-SERVER, T8 OK-SERVER) take `"4433"` (matching `ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER`); every `EXPECT_EQ(out.listen_addr, …)` on an OK row (RFC-002 T1 both sub-tests' `cli_unittests.cpp:107` and `:117`, T2 both sub-tests' `:128` and `:136`) rewrites as `EXPECT_EQ(out.listen_port, <port>)` with `<port>` set to the migrated digit (`8080` / `4433`); every `EXPECT_EQ(out.listen_addr, nullptr)` on a non-OK row (RFC-002 T3 `cli_unittests.cpp:150` and `:159`, T4 case 1's `:181`, T5 `:224`, T6 `:246`) rewrites as `EXPECT_EQ(out.listen_port, 0)`; update RFC-002 T8's expected `out`/`err` strings to the new `listen=<P>` byte sequence (`"odin: mode=client listen=8080 server=S\n"` for OK-CLIENT, `"odin: mode=server listen=4433\n"` for OK-SERVER); move RFC-002 T4's `{"odin-client", "-s", …}` and `{"odin-server"}` cases into this RFC's T1 (defaulted-port `OK`); delete RFC-002 T8's `{"odin-server"}` `ERR_MISSING_REQUIRED` `SERVER` row at `cli_unittests.cpp:317-321` outright because no `MISSING_REQUIRED` `SERVER` scenario remains reachable under RFC-006 (Server has no required flags); leave T4's `{"odin-client", "-l", "8080"}` (no `-s`) row in place — it still returns `ERR_MISSING_REQUIRED` because the Client-mode `--server` requirement is unchanged, and its migrated assertion `EXPECT_EQ(out.listen_port, 0)` passes in both phases because the OK arm is not selected.

## 6. Security

Not applicable — `--listen` bytes are local user-controlled `argv` (the user already owns the process they invoke), no network or filesystem input enters the parser, the parser performs no allocation or string copy (it sweeps the existing `argv`-aliased pointer), and the `v > 65535` check fires inside the accumulator before any `uint32_t → uint16_t` narrowing so no integer overflow can wrap the parsed value below the rejection threshold.

## 7. Testing Strategy

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | Per-mode default fires when `--listen` is omitted | Call `odin_cli_parse` with `argv = {"odin-client", "-s", "S"}` and `argv = {"odin-server"}` | Both return `ODIN_CLI_OK`; first `out.listen_port == ODIN_CLI_DEFAULT_LISTEN_PORT_CLIENT` (`8080`) and `out.server_addr == argv[2]`; second `out.listen_port == ODIN_CLI_DEFAULT_LISTEN_PORT_SERVER` (`4433`) and `out.server_addr == NULL` | G2 | unit |
| T2 | Empty `--listen` value resolves to per-mode default | `argv = {"odin-client", "-l", "", "-s", "S"}` and `argv = {"odin-server", "-l", ""}` | Both return `ODIN_CLI_OK`; `out.listen_port` is `8080` and `4433` respectively | G2 | unit |
| T3 | Valid digit string parses to exact port (boundaries + typical) | `argv = {"odin-server", "-l", PORT}` for `PORT ∈ {"0", "1", "80", "8080", "8443", "65535", "00080"}` | All seven return `ODIN_CLI_OK`; `out.listen_port` is `0`, `1`, `80`, `8080`, `8443`, `65535`, `80` respectively | G1 | unit |
| T4 | Out-of-range or oversized digit string returns `ERR_BAD_LISTEN_PORT` | `argv = {"odin-server", "-l", PORT}` for `PORT ∈ {"65536", "99999", "123456", "4294967296", "18446744073709551616"}` | All five return `ODIN_CLI_ERR_BAD_LISTEN_PORT`; `out.mode == ODIN_CLI_MODE_SERVER`; `out.listen_port == 0`; `out.server_addr == NULL` (the in-loop `v > 65535` reject fires before integer overflow on the longest input) | G1 | unit |
| T5 | Non-digit content returns `ERR_BAD_LISTEN_PORT` | `argv = {"odin-server", "-l", PORT}` for `PORT ∈ {"abc", "8080abc", "abc8080", "-1", "+80", "0x50", "8 0", " 80", "80 ", "80.0", "8_080"}` | All eleven return `ODIN_CLI_ERR_BAD_LISTEN_PORT`; `out.listen_port == 0` | G1 | unit |
| T6 | Status precedence: `HELP` > `UNKNOWN_FLAG` > `BAD_LISTEN_PORT` > `MISSING_REQUIRED` | `{"odin-client", "-x", "-l", "abc"}` (unknown flag + bad port); `{"odin-client", "--help", "-l", "abc"}` (help + bad port); `{"odin-client", "-l", "abc"}` (bad port, no `-s`) | First returns `ODIN_CLI_ERR_UNKNOWN_FLAG` (unknown flag wins over bad port); second returns `ODIN_CLI_HELP` (help wins over bad port); third returns `ODIN_CLI_ERR_BAD_LISTEN_PORT` (bad port wins over missing-required, even though `-s` is also missing) | G1 | unit |
| T7 | `odin_cli_main` banner prints parsed port as decimal on every `OK` row, and the new error-row writes the dedicated `BAD_LISTEN_PORT` banner | Through `fmemopen` capture per RFC-002 T8: rows `{"odin-client", "-l", "8443", "-s", "S"}`, `{"odin-server", "-l", "4433"}`, `{"odin-client", "-s", "S"}` (defaulted), `{"odin-server"}` (defaulted), `{"odin-server", "-l", "abc"}` (bad port) | First four write to `err` (in order) `"odin: mode=client listen=8443 server=S\n"`, `"odin: mode=server listen=4433\n"`, `"odin: mode=client listen=8080 server=S\n"`, `"odin: mode=server listen=4433\n"` and return `0`; the fifth writes `"odin: invalid --listen port\nusage: odin-server --listen ADDR\n"` and returns `2` | G1, G2, §5 | unit |
| T8 | `optind`/`opterr` restored on the new `BAD_LISTEN_PORT` return path | Snapshot `optind` and `opterr`, then call `odin_cli_parse` with `{"odin-server", "-l", "99999"}` and `{"odin-client", "-l", "abc", "-s", "S"}` | Both return `ODIN_CLI_ERR_BAD_LISTEN_PORT`; after each call `optind` and `opterr` equal the pre-test snapshot — the new error path honors RFC-002 §4.2.1's getopt-globals restoration invariant | G1 | unit |

## 8. Implementation Plan

- **P1. Land the surface diff, the in-place test migration of RFC-002 rows, and red `T1`–`T8`.**
  - **Scope:** add `<stdint.h>`, the two `ODIN_CLI_DEFAULT_LISTEN_PORT_*` macros, the `ODIN_CLI_ERR_BAD_LISTEN_PORT` enumerator, and the `listen_addr` → `listen_port` field swap to `odin/cli.h` per §4.2.1, with the new precedence and accepted-grammar paragraphs added to the existing header doc-comment; in `odin/cli.c`, replace the `listen_addr` write with a `listen_port = 0` initializer in the `*out` zeroing block, leave the post-`getopt` flag-capture branches dispatching to `parse_listen_port` as a stubbed static returning `BAD_PORT_USE_DEFAULT` so every supplied `--listen` value is currently treated as default and the binary still links, change the existing `ODIN_CLI_OK` arm in `odin_cli_main` — both the Client `fputs(args.listen_addr, err)` at `cli.c:208` and the Server `fputs(args.listen_addr, err)` at `cli.c:214` — to `fprintf(err, "%u", (unsigned)args.listen_port)` per §4.2.4 so the file compiles after the field swap, and append a `case ODIN_CLI_ERR_BAD_LISTEN_PORT:` arm in `odin_cli_main` that writes the §4.2.4 banner via `fputs`; in `odin/cli_unittests.cpp`, append `T1`–`T8` from §7 each as a separate `TEST(OdinCliListenPortTest, …)` whose first statement is `GTEST_SKIP() << "pending RFC-006 P2";`, and migrate the RFC-002 rows §5 names — every Client-mode `--listen` token (RFC-002 T1 both sub-tests' `"127.0.0.1:8443"`, T4 case 1's `"127.0.0.1:8443"`, T7 OK-CLIENT's `"L"`, T8 OK-CLIENT's `"L"`, T9 child argv's `"L"`) moves to the Client-default `"8080"` and every Server-mode `--listen` token (RFC-002 T2 both sub-tests' `"0.0.0.0:4433"`, T7 OK-SERVER's `"L"`, T8 OK-SERVER's `"L"`) moves to the Server-default `"4433"`, so under the P1 stub the default-fill matches the migrated digits and under P2 the real parser returns the same digits; T8's expected `err` strings update to `"odin: mode=client listen=8080 server=S\n"` (OK-CLIENT) and `"odin: mode=server listen=4433\n"` (OK-SERVER); T8's `{"odin-server"}` `ERR_MISSING_REQUIRED` `SERVER` row at `cli_unittests.cpp:317-321` is deleted outright (no `MISSING_REQUIRED` `SERVER` scenario remains reachable per §5); T4's two now-`OK` cases (`{"odin-client", "-s", …}` and `{"odin-server"}`) move into RFC-006 T1; every `EXPECT_EQ(out.listen_addr, …)` on an OK row (RFC-002 T1 both sub-tests at `cli_unittests.cpp:107` and `:117`, T2 both sub-tests at `:128` and `:136`) becomes `EXPECT_EQ(out.listen_port, <port>)` with `<port>` set to the migrated digit value (`8080` for Client rows, `4433` for Server rows); every `EXPECT_EQ(out.listen_addr, nullptr)` on a non-OK row (RFC-002 T3 at `cli_unittests.cpp:150` and `:159`, T4 case 1 at `:181`, T5 at `:224`, T6 at `:246`) becomes `EXPECT_EQ(out.listen_port, 0)` because §4.2.3's contract leaves `out->listen_port` at the entry-block zero on every non-OK arm in both the P1 stub and the P2 real parser. No `odin/BUILD.gn` edit.
  - **Depends on:** None.
  - **Done when:** `gn gen out/` and `ninja -C out/ tests` build cleanly against the unchanged `:odin` and `:odin_unittests` targets; `odin/cli.h` exports `listen_port` (G1 staged) and the two `ODIN_CLI_DEFAULT_LISTEN_PORT_*` macros (G2 staged); RFC-002 T1–T9 still pass under the migrated test data and the §5 entry's listed callers compile against `out.listen_port`; `T1`–`T8` are committed in `GTEST_SKIP`-wrapped (red) state and `out/odin_unittests --gtest_brief=1` reports all eight as `SKIPPED` while the run exits zero.
- **P2. Implement the strict parser, default-fill, and banner update; turn `T1`–`T8` green.**
  - **Scope:** replace the `parse_listen_port` stub in `odin/cli.c` with the byte-sweep digit-validation loop pinned in §4.2.2's Mechanism block (NULL/empty → `BAD_PORT_USE_DEFAULT`; non-digit → `BAD_PORT`; in-loop `v > 65535` → `BAD_PORT`; otherwise `OK_PARSED`); thread its three-way result into `odin_cli_parse`'s tail per §4.2.3 (precedence `HELP > UNKNOWN_FLAG > BAD_LISTEN_PORT > MISSING_REQUIRED > OK`, default macro selection by `mode` for the omitted/empty branch, `out->server_addr` only on Client `OK`); remove the `GTEST_SKIP() << "pending RFC-006 P2";` first statement from `T1`–`T8`. The `odin_cli_main` `OK`-arm `fprintf("%u", …)` substitution and the `ERR_BAD_LISTEN_PORT` switch arm both land in P1; P2 adds no further `odin_cli_main` edits.
  - **Depends on:** P1.
  - **Done when:** `T1`–`T2` pass un-skipped for the per-mode default-fill arms (G2); `T3` passes un-skipped for the boundary digit strings and the `00080` leading-zero case (G1); `T4`–`T5` pass un-skipped for every out-of-range and non-digit input row (G1); `T6` passes un-skipped for all three precedence pairs (G1); `T7` passes un-skipped against byte-exact `out`/`err` mappings for the four `OK` rows (covering both supplied and defaulted ports for both modes) and the new `BAD_LISTEN_PORT` row (G1, G2, §5 banner-format clause); `T8` passes un-skipped after the new `BAD_LISTEN_PORT` return path through `optind`/`opterr` snapshot/restore (G1); `tidy_odin.sh` exits clean on `odin/cli.c`, `odin/cli.h`, and `odin/cli_unittests.cpp`; after a clean `ninja -C out/ tests`, `out/odin_unittests --gtest_brief=1` reports `T1`–`T8` all `PASSED`, every migrated RFC-002 row remains `PASSED`, and zero rows are `SKIPPED`.
