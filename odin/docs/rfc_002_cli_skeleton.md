# RFC-002: Odin Single-Binary CLI Skeleton

**Version:** 1.0  
**Author:** Chason Tang  
**Date:** 2026-05-18  
**Status:** Proposed

## 1. Summary

Add the single `odin` binary that dispatches on `argv[0]`'s basename (`odin-client` → Client mode, `odin-server` → Server mode, anything else → usage + exit 2) and accepts the minimum address flags per mode (Client: `-l/--listen ADDR` plus `-s/--server ADDR`; Server: `-l/--listen ADDR`). A new `odin/cli.{c,h}` exports the pure parser `odin_cli_parse(argc, argv, *out)` plus the stream-injectable entry `odin_cli_main(argc, argv, FILE *out, FILE *err)` that maps parser results to the byte-exact banner/usage/exit-code table in §4.2.2. `odin/main.c` shrinks to `int main(int argc, char **argv) { return odin_cli_main(argc, argv, stdout, stderr); }`. The `odin_unittests` googletest binary (`odin/cli_unittests.cpp`) drives every branch through `odin_cli_main` with `fmemopen` streams, landing all byte-exact assertions at unit level; one integration row spawns `out/odin-client` via `fork`/`execve` to prove symlink dispatch end-to-end. The build also ships `out/odin-client`/`out/odin-server` as relative symlinks targeting basename `odin`, invocable after `ninja -C out/ default`.

## 2. Motivation

`odin/` currently exposes only the per-stream control-frame codec from RFC-001 (`odin/protocol.{c,h}` plus `protocol_unittests.cpp`); no binary exists, no `main`, no place for the Client- and Server-side transport handlers landing in follow-up RFCs to hang off. The end state of `odin` is a single binary that branches on `argv[0]` and runs either an `HTTPS_PROXY` listener (Client) or a QUIC listener (Server). Until the binary, the basename-dispatch contract, and the minimum address flags are pinned in code, every follow-up RFC would have to re-litigate the entry-point shape, re-parse `argv`, and re-decide whether the two modes are one binary or two. Shipping the skeleton first gives each follow-up one place to inject mode-specific logic and lets the GN `tests` group exercise the dispatch + parse contract on every supported target without any networking code linked in. No data available at this time.

## 3. Goals and Non-Goals

- **G1.** Expose `odin_cli_parse(int argc, char *const *argv, odin_cli_args_t *out)` from `odin/cli.{c,h}` that maps `argv[0]`'s basename to `out.mode` (`odin-client` → `CLIENT`, `odin-server` → `SERVER`, anything else → `ODIN_CLI_ERR_UNKNOWN_MODE`), parses the mode-specific address flags (`-l/--listen ADDR` always required; `-s/--server ADDR` required only for `CLIENT`), captures each `ADDR` as an opaque `const char *` aliasing the `argv` slot, recognizes `--help/-h`, and returns one of `ODIN_CLI_OK` / `ODIN_CLI_HELP` / `ODIN_CLI_ERR_UNKNOWN_MODE` / `ODIN_CLI_ERR_MISSING_REQUIRED` / `ODIN_CLI_ERR_UNKNOWN_FLAG` without I/O or heap allocation.
- **G2.** `odin_cli_main(int argc, char *const *argv, FILE *out, FILE *err)` exported from `odin/cli.{c,h}` translates `odin_cli_parse` results into the side-effects pinned in §4.2.2: success writes `odin: mode=client listen=<L> server=<S>\n` (or `odin: mode=server listen=<L>\n`) to `err` and returns `0`; `HELP` writes the mode-specific `usage: …\n` line pinned in §4.2.2 to `out` and returns `0`; every `ERR_*` writes the corresponding pinned `odin: <reason>\nusage: …\n` byte sequence (also from §4.2.2) to `err` and returns `2`. `odin/main.c` is the one-line wrapper `int main(int argc, char **argv) { return odin_cli_main(argc, argv, stdout, stderr); }` so production callers get `stdout`/`stderr` while unit tests can inject `fmemopen` byte buffers.

**Non-Goals:**

- HTTPS_PROXY listener, QUIC dial/listen, DNS resolution, TCP open, byte forwarding — every networking primitive ships in follow-up RFCs that build on top of this skeleton.
- Address string parsing or validation (host vs IP literal, IPv6 bracket form, port numeric range, hostname syntax) — the skeleton captures opaque strings and the follow-up RFCs that open sockets are the parse/validate point that owns the error surface.
- System-wide install or `$PATH` placement of the symlinks — this RFC ships `out/odin-client` and `out/odin-server` next to `out/odin` in the build directory; copying them onto a user's `$PATH` is a packaging concern owned by whoever distributes the binary.
- Config files, environment-variable overrides, logging frameworks, signal handlers, daemonization — the skeleton parses, prints one line, and exits.
- A `client` / `server` positional-subcommand mode (`odin client -l …`) — the requirement pins `argv[0]`-based dispatch.

## 4. Design

### 4.1 Overview

The existing `source_set("odin")` gains `odin/cli.{c,h}` beside `protocol.{c,h}`. The existing `executable("protocol_unittests")` is renamed to `executable("odin_unittests")` and adds `odin/cli_unittests.cpp` — one library source_set and one test binary cover `odin/`. A new `executable("odin_main")` over `odin/main.c` carries `output_name = "odin"`; the target is named `odin_main` because `executable("odin")` would collide with `source_set("odin")` in one `BUILD.gn`. `.gn` gains `script_executable = "python3"` so GN actions run through the repo's supported Python runner. A new `action("odin_symlinks")` runs `build/symlink.py` (added in P1) to create `$root_out_dir/odin-client` and `$root_out_dir/odin-server` as relative symlinks to basename `odin`. A new `group("odin_cli_artifacts")` lets callers pull in both binary and symlinks behind one label. The root `BUILD.gn` adds the group to `default` and relabels its `tests` entry from `"//odin:protocol_unittests"` to `"//odin:odin_unittests"`. The build graph below pins the new edges:

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

Three edges are load-bearing. The missing `:odin_symlinks` → `:odin_main` edge is deliberate: the symlink contents depend only on the link target basename and link paths, not on the current `out/odin` binary timestamp, so an `out/odin` relink does not re-run the action. The `:odin_unittests` `--data_deps→` `:odin_cli_artifacts` edge guarantees `ninja -C out/ tests` (without prior `default`) produces both the test binary and the runtime artifacts the §7 integration row T9 spawns (`out/odin`, `odin-client`, `odin-server`); §7's T8 byte-exact unit assertions run in-process via `fmemopen` and do not need the runtime symlinks, but `data_deps` still covers T9's spawn path so `ninja tests` is a self-sufficient runnable target. The `:odin_unittests` `--deps→` `:odin` edge — preserved verbatim from the existing `:protocol_unittests` target the §5 rename folds into `:odin_unittests` — is what the linker walks to resolve `odin_cli_parse` and `odin_cli_main` (both compiled into `:odin` from `cli.c`) and the RFC-001 codec entry points (compiled into `:odin` from `protocol.c`) that `cli_unittests.cpp` and `protocol_unittests.cpp` directly invoke; without it the test binary fails to link with undefined-reference errors on those symbols, so depending on the source_set under test is the standard GN unit-test pattern here, not an accidentally-retained edge. The binary's only transitive dep is `:odin` (library code, no transport), so its link surface stays libc-only. The skeleton has no I/O beyond the success banner, usage, and `exit`.

### 4.2 Detailed Design

#### 4.2.1 Pure Parser API and Status Enum

```c
/* odin/cli.h */
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
#ifdef __cplusplus
}
#endif
```

**Unstated contract.** `odin/cli.c` implements this header (alongside `odin_cli_main` from §4.2.2). `odin/main.c` consumes only `odin_cli_main` through the same header to stay a one-line wrapper, and the C++ googletest source `odin/cli_unittests.cpp` consumes both `odin_cli_parse` and `odin_cli_main` through the `extern "C"` block above. The parser allocates nothing and performs no I/O. It zeroes `*out` on entry. Once `argv[0]`'s basename matches `odin-client` or `odin-server`, `out.mode` is assigned and remains unchanged across every subsequent return path (`OK`, `HELP`, `ERR_UNKNOWN_FLAG`, `ERR_MISSING_REQUIRED`) — only `OK` additionally writes `out.listen_addr` / `out.server_addr`; the per-mode usage selector in §4.2.2 relies on this persistence. Otherwise `ERR_UNKNOWN_MODE` returns with `*out` left fully zeroed before flag parsing. On `OK`, address pointers alias `argv` slots. `optind`/`opterr` (and BSD `optreset`) are saved and restored on every return path; `opterr = 0` suppresses libc stderr. `--help/-h` wins after a valid basename. Otherwise `ERR_UNKNOWN_FLAG` (unknown option, missing option argument, or any extra operand) wins before `ERR_MISSING_REQUIRED`.

**Mechanism.** If `argc < 1` or `argv[0] == NULL` (or the extracted basename is empty), the parser short-circuits to `ERR_UNKNOWN_MODE` with `*out` left zeroed before any `getopt_long` setup. Otherwise basename dispatch selects mode and the allowed `getopt_long` table: Client accepts `-l/--listen`, `-s/--server`, and `-h/--help`; Server accepts `-l/--listen` and `-h/--help`. The optstring is prefixed with `+`, so scanning stops at the first non-option; a post-loop `optind == argc` check rejects leading or trailing operands without permutation differences. After restoring getopt state, missing `listen` or missing Client `server` returns `ERR_MISSING_REQUIRED`; otherwise the parser stores aliases in `out` and returns `OK`.

Satisfies: G1 via the parser signature and deterministic status contract.

#### 4.2.2 `odin_cli_main` Wiring, Banner Format, and Exit Codes

**Contract surface.**

```c
/* odin/cli.h — additional declaration, inserted inside the §4.2.1 extern "C"
   block so the C++ unit-test TU sees an unmangled `odin_cli_main` symbol */
#include <stdio.h>

int odin_cli_main(int argc, char *const *argv, FILE *out, FILE *err);
```

```c
/* odin/main.c — entire file */
int main(int argc, char **argv) { return odin_cli_main(argc, argv, stdout, stderr); }
```

`odin_cli_main` calls `odin_cli_parse` and maps each status to the exact byte sequences below, writing to the caller-supplied `out`/`err` streams and returning the matching exit code. `<U_C>` is `usage: odin-client --listen ADDR --server ADDR`; `<U_S>` is `usage: odin-server --listen ADDR`; `<U_BOTH>` is `usage: 'odin-client --listen ADDR --server ADDR' or 'odin-server --listen ADDR'`. `M` selects the per-mode usage line.

| Status | mode | `out` | `err` | return |
|---|---|---|---|---|
| `OK` | `CLIENT` | — | `odin: mode=client listen=<L> server=<S>\n` | `0` |
| `OK` | `SERVER` | — | `odin: mode=server listen=<L>\n` | `0` |
| `HELP` | `M` | `<U_M>\n` | — | `0` |
| `ERR_UNKNOWN_MODE` | `UNKNOWN` | — | `odin: unrecognized invocation name\n<U_BOTH>\n` | `2` |
| `ERR_MISSING_REQUIRED` | `M` | — | `odin: missing required flag\n<U_M>\n` | `2` |
| `ERR_UNKNOWN_FLAG` | `M` | — | `odin: unknown or invalid flag\n<U_M>\n` | `2` |

**Unstated contract.** `odin_cli_main` takes its output streams as parameters and never touches `stdio` globals directly: production callers (`main`) inject `stdout`/`stderr`; unit-test callers inject `fmemopen` byte buffers so every row in the table above is asserted byte-exactly in-process. `odin_cli_main` `fflush`es both streams before returning so test buffers and process stdio land deterministically. Running `out/odin` directly returns `ERR_UNKNOWN_MODE` and `2`; only symlink basenames are valid. Success writes to `err` so future proxy data never shares `out`. `<L>`/`<S>` are verbatim `argv` bytes. Error and usage strings are compile-time literals: no `argv[0]`, offending flag, locale text, color, timestamps, or extra newlines. Exit `2` follows getopt usage-error convention and leaves future non-usage failures distinct. The table is pinned verbatim in `odin/cli.c`'s top-of-file doc-comment so the implementation file and §4.2.2 cannot drift.

**Mechanism.** `odin_cli_main` calls `odin_cli_parse(argc, argv, &args)`, switches on the status, writes the matching row via `fputs`/`fprintf` to the supplied streams, `fflush`es both, and returns the matching exit code. Per-mode usage lookup uses `args.mode`; unknown mode uses `<U_BOTH>`. `main` is the one-line wrapper above that hands `stdout`/`stderr` to `odin_cli_main`.

Satisfies: G2 via the status → side-effects mapping pinned in the table; the stream-injectable signature lets §7's T8 byte-exact assertions land at unit level via `fmemopen` instead of `fork`/`execve`.

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
- **Reason:** `getopt_long` is in every libc this Monorepo's supported targets pull in (Apple libc on macOS x64/arm64 per `build/BUILDCONFIG.gn:42-43`; the Debian-bullseye glibc the `clang_linux_x64` toolchain links against per `build/BUILDCONFIG.gn:44-45` and `build/toolchain/BUILD.gn:147`), so no third-party dependency lands in the binary; it supports both short and long forms (`-l` and `--listen=`) the §3 G1 contract requires in a few lines of `case` statements; making the parser re-entrant within one process reduces to saving and restoring two `extern int` globals (plus `optreset` on BSD-derived libcs).
- **Ruled out:** A hand-rolled `argv` scanner would re-implement long-form `=value` and merged-short-form parsing `getopt_long` already handles correctly, all to avoid two `extern int` globals. A third-party parser (e.g., `cli11`, `argparse`) drags a runtime in for a 2-flag surface and changes the binary's dependency story for a skeleton that today only needs `<stdio.h>`, `<string.h>`, `<unistd.h>`, and `<getopt.h>`.

- **Chosen:** Capture `listen_addr` / `server_addr` as opaque `const char *` aliasing the `argv` slots.
- **Reason:** The skeleton has no way to validate an address — port numeric range matters only when the listener calls `bind`/`connect`, IPv6 bracket form matters only when the resolver splits host from port. Pushing validation into this RFC would commit to a parser the transport-handler RFCs would then re-litigate. Aliasing `argv` (not `strdup`) keeps the parser allocation-free and matches the §4.2.1 purity contract; `argv` outlives the parsed struct in every realistic caller (process lifetime).
- **Ruled out:** Parsing into a `{ host: char[256], port: uint16_t }` struct here would force this RFC to take a stance on IPv6 brackets, percent-encoded zone IDs, and Unix-socket paths — all surfaces the next RFC (which actually opens sockets) is better placed to define.

- **Chosen:** Pass `FILE *out, FILE *err` into `odin_cli_main` rather than write to `stdout`/`stderr` directly.
- **Reason:** Every row of §4.2.2's byte-exact mapping is asserted at unit level via `fmemopen` byte buffers — `EXPECT_STREQ` on an in-process buffer surfaces the diff at the assertion line, where `fork`+`pipe`+`waitpid` plumbing delays the same diff behind a screen of harness boilerplate. Integration coverage collapses to one row (§7's T9) whose only job is proving the `:odin_symlinks` artifact resolves and `execve` propagates `argv[0]`, matching the review feedback that narrowed the integration footprint to "symlink dispatch + exec end-to-end".
- **Ruled out:** Writing directly to `stdout`/`stderr` from `main` would have forced every byte-exact assertion through a child-process harness (the original draft's T8/T9/T10 each spawning the binary), inflating the integration row count past what symlink-dispatch coverage actually warrants and putting nine `fork`/`execve`/`waitpid` chains in the critical-path test loop for no signal the unit harness cannot already produce.

## 5. Backward Compatibility & Migration

- **Breaks:** the §4.1 consolidation renames `executable("protocol_unittests")` (`odin/BUILD.gn:17`) to `executable("odin_unittests")` and folds `cli_unittests.cpp` into the same target; the `out/protocol_unittests` runtime artifact is replaced by `out/odin_unittests`. Other §4 surfaces (`odin/main.c`, `odin/cli.{c,h}`, `odin/cli_unittests.cpp`, the `:odin_main` executable, the `:odin_symlinks` action, the `out/odin-client` / `out/odin-server` runtime symlinks) are net-new with no prior callers and are excluded from this entry.
- **Symptom on un-migrated caller:** if the in-tree consumer at `BUILD.gn:24` (the root `tests` group's `deps = [ "//odin:protocol_unittests" ]`) is not migrated, `./tool/gn gen out/` aborts with `ERROR Unresolved dependencies.\n//:tests(//build/toolchain:clang)\n  needs //odin:protocol_unittests(//build/toolchain:clang)` on the default mac build; an out-of-tree GN target with the same stale `deps` label observes the same `ERROR Unresolved dependencies` block with its own caller label and selected toolchain in place of `//:tests(//build/toolchain:clang)`. Separately, any shell invocation of `out/protocol_unittests --gtest_brief=1` fails because the binary at that path no longer exists after the rebuild.
- **Migration:** mechanical search-and-replace `//odin:protocol_unittests` → `//odin:odin_unittests` in any GN reference (the only in-tree caller — `BUILD.gn:24` — flips atomically inside P1's same-diff edit) and `out/protocol_unittests` → `out/odin_unittests` in any shell invocation; existing `protocol_unittests.cpp` test cases continue to pass without source-side changes because the binary just gained a new sibling source (`cli_unittests.cpp`) under a new target name, not any change to RFC-001 codec semantics.

## 6. Security

Not applicable — `argv` bytes are local user-controlled (the user already owns the process they invoke), no network or file-system input enters the parser, the parser performs no allocation or string copies (it aliases `argv` slots), and the only side effects are bounded writes of fixed-format banners to stdout/stderr followed by `exit`.

## 7. Testing Strategy

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | Client basename with both flags, short and long forms | Call `odin_cli_parse` with `argv = {"odin-client", "-l", "127.0.0.1:8443", "-s", "quic.example.com:4433"}`, then with `argv = {"./bin/odin-client", "--listen", "127.0.0.1:8443", "--server", "quic.example.com:4433"}` | Both return `ODIN_CLI_OK`; `out.mode == ODIN_CLI_MODE_CLIENT`; `out.listen_addr` and `out.server_addr` are pointer-equal to the corresponding `argv` slots in each call (validates aliasing, not `strdup`) | G1 | unit |
| T2 | Server basename with listen flag, short and long forms | Call `odin_cli_parse` with `argv = {"odin-server", "--listen", "0.0.0.0:4433"}` and `argv = {"/usr/local/bin/odin-server", "-l", "0.0.0.0:4433"}` | Both return `ODIN_CLI_OK`; `out.mode == ODIN_CLI_MODE_SERVER`; `out.listen_addr` pointer-equal to its argv slot; `out.server_addr == NULL` | G1 | unit |
| T3 | Unknown / missing basename | `argv = {"odin"}`, `{"./odin"}`, `{"odin-foo"}`, `{""}`, plus `argc=0` with a one-slot `argv` whose `argv[0] = NULL` | All five return `ODIN_CLI_ERR_UNKNOWN_MODE`; `*out` is fully zeroed (`mode == ODIN_CLI_MODE_UNKNOWN`, `listen_addr == NULL`, `server_addr == NULL`) | G1 | unit |
| T4 | Missing required flag carries mode for usage-line selection | `{"odin-client", "-l", "127.0.0.1:8443"}` (no `-s`); `{"odin-client", "-s", "quic.example.com:4433"}` (no `-l`); `{"odin-server"}` (no `-l`) | All three return `ODIN_CLI_ERR_MISSING_REQUIRED`; `out.mode` is `ODIN_CLI_MODE_CLIENT`, `ODIN_CLI_MODE_CLIENT`, `ODIN_CLI_MODE_SERVER` respectively (so `odin_cli_main` can pick the per-mode usage line in §4.2.2's table); `out.listen_addr == NULL` and `out.server_addr == NULL` on every call | G1 | unit |
| T5 | Unknown flag precedes missing-required, including server-mode `-s` rejection, the missing-argument case, and stray positional operands (leading or trailing) | `{"odin-client", "--bogus=x", "-l", "L", "-s", "S"}`; `{"odin-server", "-l", "L", "-s", "S"}` (`-s` unknown in server mode); `{"odin-client", "-x"}` (unknown `-x` *and* missing both `-l` and `-s` — must return UNKNOWN_FLAG, not MISSING_REQUIRED, per §4.2.1 precedence); `{"odin-server", "-s", "S"}` (unknown `-s` *and* missing `-l`); `{"odin-client", "-l"}` (`-l` with no argument — `getopt_long`'s missing-argument case surfaces as UNKNOWN_FLAG per §4.2.1); `{"odin-client", "-l", "L", "-s", "S", "extra"}` (trailing positional operand after every required flag — post-loop `optind != argc` check fires per §4.2.1, so extras are *not* silently accepted); `{"odin-client", "extra"}` (leading positional with no flags consumed; the POSIX `+`-mode loop short-circuits at `extra`, the same post-loop check fires, and UNKNOWN_FLAG overrides MISSING_REQUIRED per §4.2.1 precedence — pinning that the rejection is permutation-invariant) | All seven return `ODIN_CLI_ERR_UNKNOWN_FLAG`; `out.mode` is CLIENT, SERVER, CLIENT, SERVER, CLIENT, CLIENT, CLIENT respectively; `out.listen_addr == NULL` and `out.server_addr == NULL` on every call | G1 | unit |
| T6 | `--help` short-circuits before required-flag check *and* before any later unknown flag | `{"odin-client", "--help"}`; `{"odin-server", "-h"}` (neither supplies `-l`); `{"odin-client", "--help", "-x"}` (unknown flag *after* `--help` — must still return `HELP`, not `ERR_UNKNOWN_FLAG`) | All three return `ODIN_CLI_HELP`; `out.mode` is `ODIN_CLI_MODE_CLIENT`, `ODIN_CLI_MODE_SERVER`, `ODIN_CLI_MODE_CLIENT` respectively; `out.listen_addr == NULL` and `out.server_addr == NULL` on every call | G1 | unit |
| T7 | Repeated parse calls leave `optind`/`opterr` undisturbed across every return path | Snapshot `optind` and `opterr`. Then within one test call `odin_cli_parse` six times in sequence covering every return path: `{"odin-client", "-l", "L", "-s", "S"}` (OK-CLIENT), `{"odin-client", "--help"}` (HELP), `{"odin-client", "--bogus"}` (ERR_UNKNOWN_FLAG), `{"odin-client"}` (ERR_MISSING_REQUIRED), `{"odin"}` (ERR_UNKNOWN_MODE), `{"odin-server", "-l", "L"}` (OK-SERVER) | Each call returns the status and `*out` it would have produced in isolation; after *every* call (not only the last), `optind` and `opterr` equal the pre-test snapshot — the parser restores on every return path; `optarg`/`optopt` are scratch and out of scope per §4.2.1 | G1 | unit |
| T8 | `odin_cli_main` byte-exact `out`/`err`/return mapping for every §4.2.2 row | Open two `fmemopen` byte buffers as the `out`/`err` arguments. For each of nine `argv` vectors covering every row of §4.2.2's table — success CLIENT `{"odin-client","--listen=L","--server=S"}`, success SERVER `{"odin-server","--listen=L"}`, HELP CLIENT `{"odin-client","--help"}`, HELP SERVER `{"odin-server","-h"}`, `ERR_UNKNOWN_MODE` `{"odin"}`, `ERR_MISSING_REQUIRED` CLIENT `{"odin-client"}`, `ERR_MISSING_REQUIRED` SERVER `{"odin-server"}`, `ERR_UNKNOWN_FLAG` CLIENT `{"odin-client","--bogus"}`, `ERR_UNKNOWN_FLAG` SERVER `{"odin-server","--bogus"}` — call `odin_cli_main(argc, argv, out, err)` and snapshot both buffers after `odin_cli_main`'s own `fflush` returns (no extra harness `fflush` needed; §4.2.2 pins the flush in the callee) | For each of the nine invocations, the captured `out` buffer, the captured `err` buffer, and the return value are byte-equal to the corresponding §4.2.2 row (e.g., success CLIENT: `out` empty, `err == "odin: mode=client listen=L server=S\n"`, return `0`; HELP SERVER: `out == "usage: odin-server --listen ADDR\n"`, `err` empty, return `0`; `ERR_UNKNOWN_MODE`: `out` empty, `err == "odin: unrecognized invocation name\nusage: 'odin-client --listen ADDR --server ADDR' or 'odin-server --listen ADDR'\n"`, return `2`); the test binary's `argv[0]` is `out/odin_unittests` post-§5 rename, so the harness only reaches this row if the renamed GN target and on-disk artifact resolved end-to-end | G2, §5 | unit |
| T9 | `out/odin-client` symlink dispatch + exec end-to-end | Derive `<bindir>` from `dirname(g_test_argv0)` — the file-scope `static const char *` copy of `argv[0]` captured by `cli_unittests.cpp`'s custom `main` introduced in P1 Scope (P1 drops `:gtest_main`, whose default `main()` does not expose `argv` to test bodies, precisely so the custom `main` can snapshot `argv[0]` before `RUN_ALL_TESTS`) — so the build-shipped `:odin_symlinks` artifacts in the same directory resolve without hard-coded paths; spawn `<bindir>/odin-client --listen=L --server=S` via `fork`/`execve`; capture exit code via `waitpid(2)` | Exit code `0` — proves the §4.2.3 relative symlink resolves to `out/odin`, `execve` propagates the symlink basename into `argv[0]` so basename dispatch fires, and the success arm of `odin_cli_main` returns `0` (byte-exact stdout/stderr assertions for every §4.2.2 row are owned by T8 at unit level via `fmemopen`, so this row deliberately asserts only the exit code) | G2 | integration |

## 8. Implementation Plan

- **P1. Land the parser surface, the binary skeleton, and the symlink artifacts with red `T1`–`T9`.**
  - **Scope:** add `odin/cli.h` per §4.2.1 (the two enums, the `odin_cli_args_t` struct, the `odin_cli_parse` signature, the §4.2.1 unstated contract — including the mode-on-error and narrowed getopt-globals clauses — pinned in the header doc-comment) plus the `odin_cli_main(int argc, char *const *argv, FILE *out, FILE *err)` declaration from §4.2.2's contract surface (and the matching `#include <stdio.h>` so `FILE *` resolves at consumer sites without forward-decl hacks); add `odin/cli.c` whose `odin_cli_parse` body is `return ODIN_CLI_ERR_UNKNOWN_MODE;` unconditionally and whose `odin_cli_main` body is `return 2;` so the consolidated test binary links cleanly and any direct invocation of `out/odin` exits as the §4.2.2 `ERR_UNKNOWN_MODE` row directs (`return 2` matches the row's exit code even before P2 wires in the banner writes); pin the §4.2.2 byte-exact banner-and-exit table verbatim in `odin/cli.c`'s top-of-file doc-comment so the implementation file and §4.2.2 cannot drift; add `odin/main.c` as the one-line wrapper `int main(int argc, char **argv) { return odin_cli_main(argc, argv, stdout, stderr); }` per §4.2.2's contract surface — `main.c` ships its final form in P1 and is not edited again in P2; add `odin/cli_unittests.cpp` containing (a) a file-scope `static const char *g_test_argv0 = nullptr;` plus an `int main(int argc, char **argv)` that assigns `g_test_argv0 = argv[0];` *before* calling `::testing::InitGoogleTest(&argc, argv)` and returning `RUN_ALL_TESTS()` — this is the access mechanism T9 reads (the `:gtest_main` default `main()` does not expose `argv` to test bodies, so the binary supplies its own `main` and the executable's gtest dep is swapped from `:gtest_main` to `:gtest` below); and (b) every `T1`–`T9` test body written to assert the §7 expected result (including T8's `fmemopen`-backed byte-buffer capture harness covering all nine §4.2.2 invocations with byte-exact `out`/`err`/return assertions, and T9's narrower `dirname(g_test_argv0)`-based `fork`/`execve`/`waitpid` harness asserting only exit code `0` on the `out/odin-client` symlink spawn), with each `TEST(...)` body's first statement `GTEST_SKIP() << "pending RFC-002 P2";` so the binary builds, runs, and reports every row as `SKIPPED`; set `.gn` `script_executable = "python3"`; add `build/symlink.py` per §4.2.3's Mechanism paragraph (parses `--target NAME` plus repeated `--link PATH`, `os.unlink`s any pre-existing entry, then `os.symlink(NAME, PATH)`, exiting `2` on `OSError`); in `odin/BUILD.gn` extend the existing `source_set("odin")`'s `sources` list with `"cli.c"` and `"cli.h"`; rename the existing `executable("protocol_unittests")` to `executable("odin_unittests")` (replacing its `"//googletest:gtest_main"` dep with `"//googletest:gtest"` so the custom `main` defined above in `cli_unittests.cpp` provides the binary's entry point — gtest's `TEST(...)` auto-registration means that single `main` covers both `cli_unittests.cpp` and the unchanged `protocol_unittests.cpp` TUs; preserving the `":odin"` dep so the linker still resolves `odin_cli_parse`, `odin_cli_main`, and the RFC-001 codec symbols invoked from both TUs per §4.1), extend its `sources` list with `"cli_unittests.cpp"`, and add `data_deps = [ ":odin_cli_artifacts" ]` so `ninja -C out/ tests` produces the runtime symlink artifacts T9's spawn walks (T8 is in-process via `fmemopen` and does not depend on the runtime artifacts, but T9 does, so the edge is still load-bearing); declare `executable("odin_main") { output_name = "odin"; sources = [ "main.c" ]; deps = [ ":odin" ] }`, `action("odin_symlinks") { ... }` without a `deps` edge to `:odin_main`, and `group("odin_cli_artifacts") { deps = [ ":odin_main", ":odin_symlinks" ] }` per §4.2.3 in the same file; in the root `BUILD.gn` append `"//odin:odin_cli_artifacts"` to the `default` group's `deps` (this pulls in both `out/odin` and the symlinks) and update the existing `tests` group entry from `"//odin:protocol_unittests"` to `"//odin:odin_unittests"`.
  - **Depends on:** None.
  - **Done when:** `gn gen out/` resolves the new `//odin:odin_cli_artifacts` dep and the relabeled `//odin:odin_unittests` dep, and the generated command for `//odin:odin_symlinks` invokes `build/symlink.py` through `python3` rather than an unresolved `python`; the §5 migration (GN target rename `//odin:protocol_unittests` → `//odin:odin_unittests` plus the on-disk artifact rename `out/protocol_unittests` → `out/odin_unittests`) lands atomically in this same commit, with no transitional alias retained, so callers of the old label see the §5 symptom on the same build that introduces the new label; `gn desc out/ //odin:odin_main deps` lists `//odin:odin` as the only direct dep (so the binary's link surface stays libc-only — no xquic, c-ares, or boringssl is linked in on any target the root `default` group already builds), and `gn desc out/ //odin:odin_symlinks deps` is empty, proving symlink regeneration is not coupled to `out/odin` relinks; from a clean `out/`, `ninja -C out/ default` builds `out/odin` plus `out/odin-client` and `out/odin-server` as relative symlinks (`readlink out/odin-client` returns the literal `odin`, same for `odin-server`; both resolve to the same `out/odin` file via `realpath`); after touching a source that only relinks `out/odin`, a subsequent `ninja -C out/ default` must not re-run `build/symlink.py` unless a symlink output is missing or the symlink action's script/command line changed; on a separately rebuilt clean tree, `ninja -C out/ tests` (with no prior `ninja -C out/ default` invocation) builds `out/odin_unittests` *together with* `out/odin` and both symlinks via the new `data_deps` edge, so the symlink spawn path T9 walks is present before the test binary runs; the `odin_cli_parse` signature, status enum, and `odin_cli_args_t` struct from §4.2.1 export from `odin/cli.h` alongside the `odin_cli_main` declaration from §4.2.2, and the §4.2.2 banner-and-exit table appears verbatim in `odin/cli.c`'s top-of-file doc-comment (G1, G2 staged); `T1`–`T9` are committed in `GTEST_SKIP`-wrapped (red) state, with T8's `fmemopen` byte-exact harness and T9's `fork`/`execve` symlink-dispatch harness already present behind the skips, and `out/odin_unittests --gtest_brief=1` (run after the clean `ninja -C out/ tests`) reports `T1`–`T9` as `SKIPPED` while the RFC-001 codec tests in the same binary continue to pass (zero `FAILED`); the run exits zero.
- **P2. Implement the parser and `odin_cli_main`, turn `T1`–`T9` green.**
  - **Scope:** replace the stub body in `odin/cli.c` with the basename-extract + `getopt_long`-loop mechanism pinned in §4.2.1, including the `memset(out, 0, sizeof(*out))` on entry, the `optind`/`opterr` (and `optreset` where required) save-and-restore that T7 relies on, the `out.mode = mode` assignments on the `HELP` / `ERR_UNKNOWN_FLAG` / `ERR_MISSING_REQUIRED` return paths required by T4, T5, and §4.2.2's per-mode usage selector, and the status-precedence rules pinned in the §4.2.1 unstated contract; replace the stub `odin_cli_main` body (also in `odin/cli.c`) with the parser-call + per-status switch matching §4.2.2's table — write banner / usage / error bytes via `fputs`/`fprintf` to the caller-supplied `out`/`err` streams, the `ERR_MISSING_REQUIRED`/`ERR_UNKNOWN_FLAG` arms index a per-mode usage-line constant array by `args.mode`, the `HELP` arm picks the same per-mode usage line T8's HELP rows assert via `fmemopen`, and `odin_cli_main` `fflush`es both streams before returning so T8's snapshot lands deterministically; `odin/main.c` already ships in its final one-line form from P1 and is not touched in this phase; remove the `GTEST_SKIP() << ...;` first statement from each of `T1`–`T9`.
  - **Depends on:** P1.
  - **Done when:** `T1`–`T7` pass un-skipped against the `odin_cli_parse` semantics pinned in §4.2.1 (G1 green); `T8` passes un-skipped against the byte-exact `out`/`err`/return mapping pinned in §4.2.2 — one row covers every §4.2.2 row in nine in-process `fmemopen` invocations; `T9` passes un-skipped by spawning the build-shipped `out/odin-client` symlink and asserting exit code `0`, so the §4.2.3 symlink dispatch + `execve` path is exercised end-to-end (G2 green); `tidy_odin.sh` exits clean on the new files; after a clean `ninja -C out/ tests`, `out/odin_unittests --gtest_brief=1` reports zero `SKIPPED` and zero `FAILED` across both the new cli rows and the existing RFC-001 codec rows.
