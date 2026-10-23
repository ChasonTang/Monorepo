# Monorepo

A C/C++ monorepo built with GN + Ninja.

## Quick Start

```bash
git submodule update --init --recursive
./sync_tools.sh                           # gn, ninja, clang, cfssl → ./tool/
./extract_sdk.sh                          # macOS + iOS SDKs from installed Xcode (see below)
./tool/gn gen out --check
./tool/ninja -C out
```

`sync_tools.sh` also installs `./tool/cfssljson` on Intel Macs only; it skips
that tool on Apple Silicon. Thor uses `cfssl` and `cfssljson` for private CA
and certificate issuance; see [thor/README.md](thor/README.md).

## SDK Setup

The build host must be macOS — `sync_tools.sh` ships a macOS clang and
`BUILDCONFIG.gn` asserts on `host_os`. Fetch only the SDK your `target_os`
needs:

- **macOS / iOS targets:** download `Xcode.app` from
  <https://developer.apple.com/download/all/>, drop it (or a symlink) at the
  repo root, then run `./extract_sdk.sh` →
  `build/sdk/{MacOSX,iPhoneOS,iPhoneSimulator}.sdk/`. The extraction is
  pinned to the bundled `Xcode.app` rather than `xcode-select -p` so every
  developer ends up with the same SDK version regardless of what's in
  `/Applications/`. The iOS SDKs only ship inside Xcode, so the macOS SDK
  is pulled from the same source to keep the extraction uniform.
- **Linux x86_64 targets:** `./sync_linux_sysroot.sh` → SHA256-pinned
  Debian bullseye sysroot at `build/sdk/debian_bullseye_amd64-sysroot/`.

## Cross-compilation

One bundled LLVM toolchain (`./tool/clang`, mac-amd64 or mac-arm64) drives
every supported target — mac↔mac and mac→linux/iOS. Switching arch/OS is just
`--target=` plus the right platform layer.

- **macOS (x64 / arm64):** the SDK's TBD stubs plus compiler-rt cover the
  platform layer; libc++ / libc++abi are built from the matching
  `llvm-project` checkout via `component()`.
- **iOS device / simulator:** `iPhoneOS.sdk` / `iPhoneSimulator.sdk` supply the
  platform stubs; libc++ / libc++abi come from the same source-built runtime
  components instead of the SDK `libc++.tbd`.
- **Linux x86_64:** `sync_linux_sysroot.sh` supplies the system layer (libc
  headers + stub `.so`s); libc++ / libc++abi are built from
  `llvm-project`, while clang still supplies CRT objects and compiler-rt
  builtins. The toolchain disables unwind libraries with `-unwindlib=none`.

```bash
./tool/gn gen out/mac_arm64  --check --args='target_cpu="arm64"'                  # Apple Silicon
./tool/gn gen out/mac_x64    --check --args='target_cpu="x64"'                    # Intel mac
./tool/gn gen out/linux_x64  --check --args='target_os="linux" target_cpu="x64"'  # Linux (sysroot first)
./tool/gn gen out/ios_device --check --args='target_os="ios" target_environment="device" target_cpu="arm64"'
./tool/gn gen out/ios_sim    --check --args='target_os="ios" target_environment="simulator" target_cpu="arm64"'
```

## Build Arguments

Set via `--args=` on `gn gen`. See `build/BUILDCONFIG.gn`.

| Arg                  | Default | Effect                                                          |
| -------------------- | ------- | --------------------------------------------------------------- |
| `is_debug`           | `true`  | Debug build; release enables ThinLTO.                           |
| `is_asan`            | `false` | Enable AddressSanitizer.                                        |
| `target_os`          | host    | `mac`, `linux`, or `ios` (`linux` requires `target_cpu="x64"`). |
| `target_cpu`         | host    | `x64` or `arm64` depending on target OS/environment.             |
| `target_environment` | `""`    | For `ios`: `device` or `simulator`.                             |

## Language Standards

Without an explicit `-std=`, the bundled Clang defaults to GNU C17 (`gnu17`)
for C and GNU C++17 (`gnu++17`) for C++. The top-level GN configuration does
not set a global language standard, so ordinary targets inherit these defaults.

The `//build:cxx23` config in [build/BUILD.gn](build/BUILD.gn) sets
`-std=c++23` for targets that require C++23. The libc++ and libc++abi targets
currently use this config.

## Standard Library Headers

The `//llvm-project/libcxx` and `//llvm-project/libcxxabi` runtime targets export
their public headers through separate `:headers` targets. Depend on the runtime
targets when linking C++ code; the header targets alone do not link a runtime.
The ABI implementation can use libc++ headers without depending on the libc++
binary, so the runtime dependency graph remains acyclic.

The checked-in `build/secondary/llvm-project/{libcxx,libcxxabi}/headers.gni`
inventories register public API headers, internal headers, and runtime support
headers. libc++ uses the checked-out upstream `header_information.py` to classify
its API; local `__config_site` and `__assertion_handler` headers are included too.
Private header targets have restricted visibility and are not re-exported to
consumers. GN cannot list extensionless internal headers in a source set's
`sources`, so libc++ registers them in `public` on a restricted private
dependency. This does not make them accessible to consumers. libc++abi's private
configuration header uses explicit `friend` permissions for its implementation
targets.

After updating the LLVM submodule, regenerate and commit both inventories:

```bash
python3 build/secondary/llvm-project/update_headers.py
python3 build/secondary/llvm-project/update_headers.py --check
./tool/gn gen out --check
./tool/gn check out '//llvm-project/*'
```

`--check` reports stale inventories without writing files. Inventory generation
uses the repository's `./tool/gn` formatter; run `./sync_tools.sh` first if needed.
Normal GN generation reads the inventories without scanning the LLVM tree.

## Tests

For a native build, build and run both Odin and argcv test suites from the
repository root:

```bash
./tool/gn gen out --check
./tool/ninja -C out tests
./out/odin_unittests
./out/argcv_unittests
```

The root `tests` group also builds Odin's CLI artifacts and generates its TLS
test certificates through `//thor:odin_test_certs` under
`out/gen/thor/odin_test_certs/`.

## Formatting and Static Analysis

Check header dependencies when generating build files, or check an existing
build directory without regenerating it:

```bash
./tool/gn gen out --check
./tool/gn check out
```

The `.gn` file selects all targets under `argcv/`, `odin/`, and `ipsw/`,
including their tests, that exist in the selected build configuration. It
checks both quoted and angle-bracket includes against the GN dependency graph.
Third-party targets are not checked themselves, but includes from project code
into their declared headers are checked.

The repository root is already on every native target's default include path;
use paths such as `"argcv/argcv.h"` without a per-component include config.
Declare target dependencies with `deps`, use `public_deps` when consumers need
the dependency's headers, and list exported headers in `public`. List private
headers in `sources`: GN ignores included files that are not known to the build.
GN does not evaluate preprocessor guards; test-only conditional includes and
test wrappers that compile a production `.c` file use documented, per-include
`nogncheck` annotations rather than disabling checks for a target.

The scripts cover `argcv/`, `odin/`, and `ipsw/`, using the bundled tools and
repository-root `.clang-format` and `.clang-tidy` rules.

```bash
./format.sh                  # format all project C/C++ files in place
./tidy.sh --staged            # check staged changes
./tidy.sh                     # check all project sources in the selected build
JOBS=4 BUILD_DIR=out ./tidy.sh # set parallelism and build directory
```

`JOBS` defaults to the logical CPU count; `BUILD_DIR` defaults to `out/`
relative to the repository root.

`--staged` checks working-tree contents; run a full check after shared header
changes. Before analyzing selected files, Tidy runs GN's header check for all
configured project targets. Tidy skips files outside the selected build
configuration.

## Layout

- `BUILD.gn`, `.gn`, `build/` — root build, toolchains, configs, `component.gni`.
- `odin/` — QUIC proxy client/server; builds the `odin` binary and the
  `odin-client` / `odin-server` symlink entry points.
- `argcv/` — allocation-free C command-line argument parser and value parsing
  helpers.
- `thor/` — private CA, certificate issuance scripts, and Odin TLS test fixture
  generation; see [thor/README.md](thor/README.md).
- `build/secondary/{PROJECT}/BUILD.gn` — GN files for
  vendored third-party trees that don't ship a usable GN build.
- `{PROJECT}/` — third-party submodules (upstream layout) without their own
  GN build.
- `ipsw/` — macOS-only tool that parses `dyld_shared_cache`.
- `tool/`, `build/sdk/` — gitignored; populated by the sync/extract scripts.
- `out/` — gitignored; build files and artifacts generated by GN/Ninja.

## Adding a Component

Create `your_component/BUILD.gn` with a target, then add it to the root
`BUILD.gn`'s `default` group. Use `component()` (from
`//build/component.gni`) for libraries that should be `shared_library` in
debug and `source_set` in release.

Add the component's `//your_component/*` pattern to `check_targets` in `.gn`
so its sources and tests participate in the default header dependency check.
