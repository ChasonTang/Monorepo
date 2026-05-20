# RFC-002: Odin Single-Binary CLI Skeleton

**Version:** 1.0  
**Author:** Chason Tang  
**Date:** 2026-05-18  
**Status:** Implemented

## 1. Summary

Add the single `odin` binary that dispatches on `argv[0]`'s basename (`odin-client` → Client mode, `odin-server` → Server mode, anything else → usage + exit 2) and accepts the minimum address flags per mode (Client: `-l/--listen ADDR` plus `-s/--server ADDR`; Server: `-l/--listen ADDR`). A new `odin/cli.{c,h}` exports the pure parser `odin_cli_parse(argc, argv, *out)` plus the stream-injectable entry `odin_cli_main(argc, argv, FILE *out, FILE *err)` that maps parser results to the byte-exact banner/usage/exit-code table in §4.2.2. `odin/main.c` is a tiny wrapper that includes `cli.h`/`<stdio.h>` and calls `odin_cli_main(argc, argv, stdout, stderr)`. The `odin_unittests` googletest binary drives every branch through `odin_cli_main` with `fmemopen`; one integration row spawns `out/odin-client` via `fork`/`execve` to prove symlink dispatch. The build also ships `out/odin-client`/`out/odin-server` as relative symlinks targeting basename `odin`, invocable after `ninja -C out/ default`.

## 2. Motivation

`odin/` currently exposes only the per-stream control-frame codec from RFC-001 (`odin/protocol.{c,h}` plus `protocol_unittests.cpp`); no binary exists, no `main`, no place for the Client- and Server-side transport handlers landing in follow-up RFCs to hang off. The end state of `odin` is a single binary that branches on `argv[0]` and runs either an `HTTPS_PROXY` listener (Client) or a QUIC listener (Server). Until the binary, the basename-dispatch contract, and the minimum address flags are pinned in code, every follow-up RFC would have to re-litigate the entry-point shape, re-parse `argv`, and re-decide whether the two modes are one binary or two. Shipping the skeleton first gives each follow-up one place to inject mode-specific logic and lets the GN `tests` group exercise the dispatch + parse contract on every supported target without any networking code linked in. No data available at this time.

## 3. Goals and Non-Goals

- **G1.** Expose `odin_cli_parse(int argc, char *const *argv, odin_cli_args_t *out)` from `odin/cli.{c,h}` that maps `argv[0]`'s basename to `out.mode` (`odin-client` → `CLIENT`, `odin-server` → `SERVER`, anything else → `ODIN_CLI_ERR_UNKNOWN_MODE`), parses the mode-specific address flags (`-l/--listen ADDR` always required; `-s/--server ADDR` required only for `CLIENT`), captures each `ADDR` as an opaque `const char *` aliasing the `argv` slot, recognizes `--help/-h`, and returns one of `ODIN_CLI_OK` / `ODIN_CLI_HELP` / `ODIN_CLI_ERR_UNKNOWN_MODE` / `ODIN_CLI_ERR_MISSING_REQUIRED` / `ODIN_CLI_ERR_UNKNOWN_FLAG` without I/O or heap allocation.
- **G2.** `odin_cli_main(int argc, char *const *argv, FILE *out, FILE *err)` exported from `odin/cli.{c,h}` translates `odin_cli_parse` results into the side-effects pinned in §4.2.2: success writes `odin: mode=client listen=<L> server=<S>\n` (or `odin: mode=server listen=<L>\n`) to `err` and returns `0`; `HELP` writes the mode-specific `usage: …\n` line pinned in §4.2.2 to `out` and returns `0`; every `ERR_*` writes the corresponding pinned `odin: <reason>\nusage: …\n` byte sequence to `err` and returns `2`. `odin/main.c` includes `cli.h` and `<stdio.h>` before calling `odin_cli_main(argc, argv, stdout, stderr)` so production callers get process stdio while unit tests can inject `fmemopen` byte buffers.

**Non-Goals:**

- HTTPS_PROXY listener, QUIC dial/listen, DNS resolution, TCP open, byte forwarding — every networking primitive ships in follow-up RFCs that build on top of this skeleton.
- Address string parsing or validation (host vs IP literal, IPv6 bracket form, port numeric range, hostname syntax) — the skeleton captures opaque strings and the follow-up RFCs that open sockets are the parse/validate point that owns the error surface.
- System-wide install or `$PATH` placement of the symlinks — this RFC ships `out/odin-client` and `out/odin-server` next to `out/odin` in the build directory; copying them onto a user's `$PATH` is a packaging concern owned by whoever distributes the binary.
- Config files, environment-variable overrides, logging frameworks, signal handlers, daemonization — the skeleton parses, prints one line, and exits.
- A `client` / `server` positional-subcommand mode (`odin client -l …`) — the requirement pins `argv[0]`-based dispatch.

## 4. Design

### 4.1 Overview

The existing `source_set("odin")` gains `odin/cli.{c,h}` beside `protocol.{c,h}`. The existing `executable("protocol_unittests")` is renamed to `executable("odin_unittests")` and adds `odin/cli_unittests.cpp`. A new `executable("odin_main")` over `odin/main.c` carries `output_name = "odin"`; the target is named `odin_main` because `source_set("odin")` already owns the `:odin` label. `.gn` gains `script_executable = "python3"` for GN actions. A new `action("odin_symlinks")` runs `build/symlink.py` to create `$root_out_dir/odin-client` and `$root_out_dir/odin-server` as relative symlinks to basename `odin`. `group("odin_cli_artifacts")` pulls in the binary and links; root `BUILD.gn` adds that group to `default` and relabels `tests` from `"//odin:protocol_unittests"` to `"//odin:odin_unittests"`. The build graph below pins the new edges:

```
   //:default                                             //:tests
        |                                                    |
        | deps                                               | deps
        v                                                    v
   //odin:odin_cli_artifacts  <-- data_deps --  //odin:odin_unittests
        |                                                    |
        +-- deps --> //odin:odin_main                        | deps
        |                  |                                 |
        |                  | deps                            v
        |                  +----> //odin:odin (source_set) <-+
        |
        +-- deps --> //odin:odin_symlinks   (intentionally no deps edge to :odin_main)
```

Three edges are load-bearing. `:odin_symlinks` intentionally has no `deps` edge to `:odin_main`: link contents depend only on target basename and link paths, so relinking `out/odin` does not re-run the action. `:odin_unittests` uses `data_deps = [ ":odin_cli_artifacts" ]` so `ninja -C out/ tests` produces the runtime artifacts T9 spawns without requiring `default` first. `:odin_unittests` still depends on `:odin` so the linker resolves `odin_cli_parse`, `odin_cli_main`, and the existing RFC-001 codec symbols. `:odin_unittests` is also the only target that receives `if (target_os == "linux") { defines = [ "_GNU_SOURCE" ] }`, because T8 uses `fmemopen` and the bundled glibc sysroot hides its declaration without a feature macro. The binary's only transitive dep is `:odin` (library code, no transport), so the skeleton has no I/O beyond the success banner, usage, and exit.

### 4.2 Detailed Design

#### 4.2.1 CLI Header and Pure Parser API

```c
/* odin/cli.h */
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef enum odin_cli_mode_t {
  ODIN_CLI_MODE_UNKNOWN = 0,
  ODIN_CLI_MODE_CLIENT,
  ODIN_CLI_MODE_SERVER,
} odin_cli_mode_t;
typedef enum odin_cli_status_t {
  ODIN_CLI_OK = 0,
  ODIN_CLI_HELP,
  ODIN_CLI_ERR_UNKNOWN_MODE,
  ODIN_CLI_ERR_MISSING_REQUIRED,
  ODIN_CLI_ERR_UNKNOWN_FLAG,
} odin_cli_status_t;
typedef struct odin_cli_args_t {
  odin_cli_mode_t mode;
  const char *listen_addr;
  const char *server_addr;
} odin_cli_args_t;
odin_cli_status_t odin_cli_parse(int argc, char *const *argv, odin_cli_args_t *out);
int odin_cli_main(int argc, char *const *argv, FILE *out, FILE *err);
#ifdef __cplusplus
}
#endif
```

**Unstated contract.** `cli.h` includes `<stdio.h>` so `FILE *` resolves for C and C++ consumers; both exported functions are inside the `extern "C"` block. `odin_cli_parse` zeroes `*out`, allocates nothing, and performs no I/O. Unknown or empty `argv[0]` returns `ERR_UNKNOWN_MODE` with `*out` fully zeroed. After a valid `odin-client` or `odin-server` basename, `out.mode` persists on `OK`, `HELP`, `ERR_UNKNOWN_FLAG`, and `ERR_MISSING_REQUIRED`; only `OK` fills address fields, and those pointers alias `argv` slots. `--help/-h` wins after a valid basename. Otherwise unknown options, missing option arguments, and extra operands return `ERR_UNKNOWN_FLAG` before missing-required validation. `optind`/`opterr` (and BSD `optreset`) are saved and restored; `opterr = 0` suppresses libc stderr.

**Mechanism.** If `argc < 1`, `argv[0] == NULL`, or the basename is empty, return `ERR_UNKNOWN_MODE` before `getopt_long` setup. Otherwise basename dispatch selects mode and the allowed table: Client accepts `-l/--listen`, `-s/--server`, and `-h/--help`; Server accepts `-l/--listen` and `-h/--help`. The optstring is prefixed with `+`, so scanning stops at the first non-option. If help was seen, return `HELP`; if `getopt_long` returned `?` or post-loop `optind != argc`, return `ERR_UNKNOWN_FLAG`. Then missing `listen` or missing Client `server` returns `ERR_MISSING_REQUIRED`; otherwise store aliases in `out` and return `OK`.

Satisfies: G1 via the parser signature and deterministic status contract.

#### 4.2.2 `odin_cli_main` Wiring, Banner Format, and Exit Codes

```c
/* odin/main.c — entire file */
#include "cli.h"

#include <stdio.h>

int main(int argc, char **argv) {
  return odin_cli_main(argc, argv, stdout, stderr);
}
```

`odin_cli_main` maps parser status to the exact bytes and exit codes below. `<U_C>` is `usage: odin-client --listen ADDR --server ADDR`; `<U_S>` is `usage: odin-server --listen ADDR`; `<U_BOTH>` is `usage: 'odin-client --listen ADDR --server ADDR' or 'odin-server --listen ADDR'`; `M` selects client/server usage.

| Status | mode | `out` | `err` | return |
|---|---|---|---|---|
| `OK` | `CLIENT` | — | `odin: mode=client listen=<L> server=<S>\n` | `0` |
| `OK` | `SERVER` | — | `odin: mode=server listen=<L>\n` | `0` |
| `HELP` | `M` | `<U_M>\n` | — | `0` |
| `ERR_UNKNOWN_MODE` | `UNKNOWN` | — | `odin: unrecognized invocation name\n<U_BOTH>\n` | `2` |
| `ERR_MISSING_REQUIRED` | `M` | — | `odin: missing required flag\n<U_M>\n` | `2` |
| `ERR_UNKNOWN_FLAG` | `M` | — | `odin: unknown or invalid flag\n<U_M>\n` | `2` |

**Unstated contract.** `odin_cli_main` never touches `stdout`/`stderr` directly; callers inject streams, and tests use `fmemopen`. It `fflush`es both streams before returning. Running `out/odin` directly returns `ERR_UNKNOWN_MODE`/`2`; only symlink basenames are valid. Success writes to `err` so future proxy data never shares `out`. `<L>`/`<S>` are verbatim `argv` bytes. Literals contain no `argv[0]`, offending flag, locale text, color, timestamps, or extra newlines. The table is pinned in `odin/cli.c`.

**Mechanism.** Call `odin_cli_parse(argc, argv, &args)`, switch on status, write the matching row via `fputs`/`fprintf`, flush both streams, and return the matching exit code. Per-mode usage lookup uses `args.mode`; unknown mode uses `<U_BOTH>`.

Satisfies: G2 via the status → side-effects table and stream-injectable signature.

#### 4.2.3 Build-Time Symlink Generation

```gn
# .gn
script_executable = "python3"

# odin/BUILD.gn
action("odin_symlinks") {
  script = "//build/symlink.py"
  outputs = [
    "$root_out_dir/odin-client",
    "$root_out_dir/odin-server",
  ]
  args = [
    "--target", "odin",
    "--link", rebase_path("$root_out_dir/odin-client", root_build_dir),
    "--link", rebase_path("$root_out_dir/odin-server", root_build_dir),
  ]
}

group("odin_cli_artifacts") {
  deps = [
    ":odin_main",
    ":odin_symlinks",
  ]
}
```

**Unstated contract.** GN runs action scripts through `python3`; the build must not require a `python` shim. Both symlinks land in `$root_out_dir` and target the basename `odin`, so `readlink out/odin-client` returns `odin` and the build directory stays relocatable. The action has no `deps` on `:odin_main`; its outputs depend only on the target basename, link paths, script, and command line. `:odin_cli_artifacts` orders both binary and links for consumers. During a parallel build the links may dangle until the group completes; afterward both basenames resolve to the same sibling `out/odin`.

**Mechanism.** `build/symlink.py` accepts one `--target NAME` and repeated `--link PATH`. For each path it checks `os.path.lexists`, unlinks stale entries, then calls `os.symlink(NAME, PATH)`. Because `NAME` is a basename, resolution is relative to `dirname(PATH)`. Any unexpected `OSError` exits `2`, surfacing as a ninja-build failure.

Satisfies: G2 via shipping `odin-client`/`odin-server` as `ninja -C out/ default` artifacts so the §4.2.2 table is reachable on a built tree without manual `ln -s`.

### 4.3 Design Rationale

- **Chosen:** `getopt_long` from the libc.
- **Reason:** It is already available through the supported libcs, handles the short and long flag forms required by G1, and keeps the binary dependency-free. Re-entrancy for tests is covered by saving/restoring `optind`, `opterr`, and `optreset` where present.
- **Ruled out:** A hand-rolled scanner would duplicate `--flag=value` and missing-argument behavior. A third-party parser would add a runtime dependency for a two-flag skeleton.

- **Chosen:** Capture `listen_addr` / `server_addr` as opaque `const char *` aliasing the `argv` slots.
- **Reason:** Address validation belongs to the later code that opens sockets. Aliasing keeps the parser allocation-free and matches the §4.2.1 contract.
- **Ruled out:** Parsing into host/port fields here would prematurely decide IPv6 bracket, zone-ID, and socket-path semantics.

- **Chosen:** Pass `FILE *out, FILE *err` into `odin_cli_main` rather than write to `stdout`/`stderr` directly.
- **Reason:** `fmemopen` lets unit tests assert every §4.2.2 byte sequence in process. T9 can focus on symlink dispatch and `execve`.
- **Ruled out:** Direct writes from `main` would push byte-exact assertions into child-process plumbing that adds no coverage beyond T9.

## 5. Backward Compatibility & Migration

- **Breaks:** `executable("protocol_unittests")` (`odin/BUILD.gn:17`) is renamed to `executable("odin_unittests")`; `out/protocol_unittests` becomes `out/odin_unittests`. New CLI files, binary target, symlink action, and runtime symlinks have no prior callers.
- **Symptom on un-migrated caller:** stale GN deps on `//odin:protocol_unittests` fail `gn gen` with an unresolved dependency; shell invocations of `out/protocol_unittests --gtest_brief=1` fail after rebuild because that binary path no longer exists.
- **Migration:** replace `//odin:protocol_unittests` with `//odin:odin_unittests` in GN and `out/protocol_unittests` with `out/odin_unittests` in scripts. Existing `protocol_unittests.cpp` cases continue unchanged under the renamed binary.

## 6. Security

Not applicable — `argv` bytes are local user-controlled (the user already owns the process they invoke), no network or file-system input enters the parser, the parser performs no allocation or string copies (it aliases `argv` slots), and the only side effects are bounded writes of fixed-format banners to caller-supplied streams followed by `exit`.

## 7. Testing Strategy

All C++ `argv = {...}` examples below are test data passed through a `MutableArgv` helper that owns `std::vector<std::string>` storage and exposes `std::vector<char *>` to the C API; no test assigns string literals to `char *`. T8's `fmemopen` calls compile on Linux only because `:odin_unittests` defines `_GNU_SOURCE` target-wide before any test source includes `<stdio.h>`.

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | Client basename with both flags, short and long forms | Call `odin_cli_parse` with `argv = {"odin-client", "-l", "127.0.0.1:8443", "-s", "quic.example.com:4433"}`, then with `argv = {"./bin/odin-client", "--listen", "127.0.0.1:8443", "--server", "quic.example.com:4433"}` | Both return `ODIN_CLI_OK`; `out.mode == ODIN_CLI_MODE_CLIENT`; `out.listen_addr` and `out.server_addr` are pointer-equal to the corresponding `argv` slots in each call (validates aliasing, not `strdup`) | G1 | unit |
| T2 | Server basename with listen flag, short and long forms | Call `odin_cli_parse` with `argv = {"odin-server", "--listen", "0.0.0.0:4433"}` and `argv = {"/usr/local/bin/odin-server", "-l", "0.0.0.0:4433"}` | Both return `ODIN_CLI_OK`; `out.mode == ODIN_CLI_MODE_SERVER`; `out.listen_addr` pointer-equal to its argv slot; `out.server_addr == NULL` | G1 | unit |
| T3 | Unknown / missing basename | `argv = {"odin"}`, `{"./odin"}`, `{"odin-foo"}`, `{""}`, plus `argc=0` with a one-slot `argv` whose `argv[0] = NULL` | All five return `ODIN_CLI_ERR_UNKNOWN_MODE`; `*out` is fully zeroed (`mode == ODIN_CLI_MODE_UNKNOWN`, `listen_addr == NULL`, `server_addr == NULL`) | G1 | unit |
| T4 | Missing required flag carries mode for usage-line selection | `{"odin-client", "-l", "127.0.0.1:8443"}` (no `-s`); `{"odin-client", "-s", "quic.example.com:4433"}` (no `-l`); `{"odin-server"}` (no `-l`) | All three return `ODIN_CLI_ERR_MISSING_REQUIRED`; `out.mode` is `ODIN_CLI_MODE_CLIENT`, `ODIN_CLI_MODE_CLIENT`, `ODIN_CLI_MODE_SERVER` respectively (so `odin_cli_main` can pick the per-mode usage line in §4.2.2's table); `out.listen_addr == NULL` and `out.server_addr == NULL` on every call | G1 | unit |
| T5 | Unknown flag precedes missing-required, including server-mode `-s` rejection, the missing-argument case, and stray positional operands (leading or trailing) | `{"odin-client", "--bogus=x", "-l", "L", "-s", "S"}`; `{"odin-server", "-l", "L", "-s", "S"}` (`-s` unknown in server mode); `{"odin-client", "-x"}` (unknown `-x` *and* missing both `-l` and `-s` — must return UNKNOWN_FLAG, not MISSING_REQUIRED, per §4.2.1 precedence); `{"odin-server", "-s", "S"}` (unknown `-s` *and* missing `-l`); `{"odin-client", "-l"}` (`-l` with no argument — `getopt_long`'s missing-argument case surfaces as UNKNOWN_FLAG per §4.2.1); `{"odin-client", "-l", "L", "-s", "S", "extra"}` (trailing positional operand after every required flag — post-loop `optind != argc` check fires per §4.2.1, so extras are *not* silently accepted); `{"odin-client", "extra"}` (leading positional with no flags consumed; the POSIX `+`-mode loop short-circuits at `extra`, the same post-loop check fires, and UNKNOWN_FLAG overrides MISSING_REQUIRED per §4.2.1 precedence — pinning that the rejection is permutation-invariant) | All seven return `ODIN_CLI_ERR_UNKNOWN_FLAG`; `out.mode` is CLIENT, SERVER, CLIENT, SERVER, CLIENT, CLIENT, CLIENT respectively; `out.listen_addr == NULL` and `out.server_addr == NULL` on every call | G1 | unit |
| T6 | `--help` short-circuits before required-flag check *and* before any later unknown flag | `{"odin-client", "--help"}`; `{"odin-server", "-h"}` (neither supplies `-l`); `{"odin-client", "--help", "-x"}` (unknown flag *after* `--help` — must still return `HELP`, not `ERR_UNKNOWN_FLAG`) | All three return `ODIN_CLI_HELP`; `out.mode` is `ODIN_CLI_MODE_CLIENT`, `ODIN_CLI_MODE_SERVER`, `ODIN_CLI_MODE_CLIENT` respectively; `out.listen_addr == NULL` and `out.server_addr == NULL` on every call | G1 | unit |
| T7 | Repeated parse calls leave `optind`/`opterr` undisturbed across every return path | Snapshot `optind` and `opterr`. Then within one test call `odin_cli_parse` six times in sequence covering every return path: `{"odin-client", "-l", "L", "-s", "S"}` (OK-CLIENT), `{"odin-client", "--help"}` (HELP), `{"odin-client", "--bogus"}` (ERR_UNKNOWN_FLAG), `{"odin-client"}` (ERR_MISSING_REQUIRED), `{"odin"}` (ERR_UNKNOWN_MODE), `{"odin-server", "-l", "L"}` (OK-SERVER) | Each call returns the status and `*out` it would have produced in isolation; after *every* call (not only the last), `optind` and `opterr` equal the pre-test snapshot — the parser restores on every return path; `optarg`/`optopt` are scratch and out of scope per §4.2.1 | G1 | unit |
| T8 | `odin_cli_main` byte-exact `out`/`err`/return mapping for every §4.2.2 row | Open two `fmemopen` buffers. For nine mutable argv vectors covering every table row — success CLIENT/SERVER, HELP CLIENT/SERVER, `ERR_UNKNOWN_MODE`, `ERR_MISSING_REQUIRED` CLIENT/SERVER, `ERR_UNKNOWN_FLAG` CLIENT/SERVER — call `odin_cli_main(argc, argv, out, err)` and snapshot buffers after the callee's `fflush`. | For each invocation, captured `out`, captured `err`, and return value are byte-equal to the corresponding §4.2.2 row; examples include success CLIENT `err == "odin: mode=client listen=L server=S\n"` and `ERR_UNKNOWN_MODE` returning `2` with `<U_BOTH>` on `err`. | G2 | unit |
| T9 | `out/odin-client` symlink dispatch + exec end-to-end | `cli_unittests.cpp` supplies a custom `main` that captures `argv[0]` in `g_test_argv0`. Derive `<bindir>` with a non-mutating C++ helper over `std::string(g_test_argv0)` (not POSIX `dirname` on a `const char *`), set `client_path = <bindir>/odin-client`, then fork and call `execve(client_path.c_str(), child_argv, environ)` where `child_argv` is a mutable vector with string contents `{client_path, "--listen", "L", "--server", "S"}` plus the final `NULL`. Capture exit code with `waitpid(2)`. | Exit code `0`, proving the relative symlink path resolves to `out/odin`, the caller-supplied `argv[0]` basename is `odin-client` for dispatch, and the success arm of `odin_cli_main` returns `0`. Byte-exact stdout/stderr assertions stay in T8. | G2 | integration |
| T10 | Renamed unit-test binary still carries the RFC-001 codec tests | Run `out/odin_unittests --gtest_brief=1` after a clean `ninja -C out/ tests`. The un-skipped T10 body in `cli_unittests.cpp` reads `g_test_argv0` and gtest's registry. | The process basename is `odin_unittests`; the registry contains the `OdinProtoTest` suite from `protocol_unittests.cpp`; the full run exits `0`, so the existing RFC-001 codec rows execute under the renamed artifact instead of `out/protocol_unittests`. | §5 | integration |

## 8. Implementation Plan

- **P1. Land the parser surface, the binary skeleton, and the symlink artifacts with red `T1`–`T10`.**
  - **Scope:** add `odin/cli.h` exactly as §4.2.1 specifies, including `<stdio.h>`, both exported signatures, and the header doc-comment for parser status semantics. Add `odin/cli.c` with linkable stubs (`odin_cli_parse` returns `ERR_UNKNOWN_MODE`; `odin_cli_main` returns `2`) plus the §4.2.2 table in a top-of-file doc-comment. Add `odin/main.c` exactly as §4.2.2 specifies, with `#include "cli.h"` and `<stdio.h>` before the wrapper body. Add `odin/cli_unittests.cpp` with `T1`–`T10` present but `GTEST_SKIP()`ed, a `MutableArgv` helper for all C API calls and `execve` child vectors, `fmemopen` capture for T8, a custom gtest `main` that snapshots `argv[0]`, a non-mutating bindir helper for T9, and gtest-registry lookup for T10. Add `.gn` `script_executable = "python3"`, `build/symlink.py`, the `:odin_main`, `:odin_symlinks`, `:odin_cli_artifacts`, and renamed `:odin_unittests` targets, swap `:gtest_main` to `:gtest`, preserve the `:odin` test dep, add `data_deps = [ ":odin_cli_artifacts" ]`, set `defines = [ "_GNU_SOURCE" ]` on `:odin_unittests` when `target_os == "linux"`, and update root `default`/`tests` deps.
  - **Depends on:** None.
  - **Done when:** `gn gen out/` resolves `//odin:odin_cli_artifacts` and `//odin:odin_unittests`; `gn desc out/ //odin:odin_main deps` lists only `//odin:odin`; `gn desc out/ //odin:odin_symlinks deps` is empty; clean `ninja -C out/ default` builds `out/odin` plus relative `odin-client`/`odin-server` links whose `readlink` value is `odin`; clean `ninja -C out/ tests` builds those runtime artifacts through `data_deps`; `T1`–`T10` are committed in `GTEST_SKIP` red state (G1, G2, §5 staged) while existing RFC-001 codec tests pass from `out/odin_unittests`; the §5 target and artifact rename lands atomically with no compatibility alias.
- **P2. Implement the parser and `odin_cli_main`, turn `T1`–`T10` green.**
  - **Scope:** replace `odin/cli.c` stubs with the §4.2.1 basename + `getopt_long` parser, including zeroing `*out`, status precedence, `out.mode` persistence on non-unknown-mode returns, address aliasing only on `OK`, and getopt global save/restore. Implement `odin_cli_main` as the §4.2.2 status table, using supplied streams and flushing both before return. Remove the `GTEST_SKIP()` statements from `T1`–`T10`; leave `odin/main.c` unchanged from P1.
  - **Depends on:** P1.
  - **Done when:** `T1`–`T7` pass un-skipped for G1; `T8` passes un-skipped for every byte-exact §4.2.2 row; `T9` passes un-skipped by spawning `out/odin-client` for G2 with `argv[0] = <bindir>/odin-client`; `T10` passes un-skipped for the §5 migration check; `tidy_odin.sh` exits clean; after clean `ninja -C out/ tests`, `out/odin_unittests --gtest_brief=1` reports zero `SKIPPED` and zero `FAILED` across CLI and RFC-001 codec rows.
