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

- **macOS (x64 / arm64):** use the target SDK's libc++ headers and TBD stubs
  to link system libc++. Minimum deployment versions are 10.15 for x64 and
  11.0 for arm64.
- **iOS device / simulator:** `iPhoneOS.sdk` / `iPhoneSimulator.sdk` supply the
  libc++ headers and platform stubs, including `libc++.tbd`, for system libc++.
  The minimum deployment version remains iOS 15.0.
- **Linux x86_64:** `sync_linux_sysroot.sh` supplies the Debian bullseye
  system layer, including GCC 10 libstdc++ headers and libraries. Clang
  discovers the generic, multiarch and backward headers in that sysroot
  with `-stdlib=libstdc++`; linking uses its libgcc unwinder via
  `-unwindlib=libgcc`.

All executable and shared-library links use `clang++ -nostdlib++` with LLD.
Targets explicitly select their C++ standard library through
`//build:cxx_stdlib`, while the C++ driver retains platform math library and
sanitizer runtime selection. C and assembly compilation still use `clang`;
C++ and Objective-C++ use `clang++`.

The pinned Fuchsia Clang defaults to compiler-rt builtins, so the build leaves
`-rtlib` unset. This is a default of this toolchain distribution, not a promise
about every Clang installation. Compiler builtins, the C++ standard library,
and the exception unwinder are separate runtime choices.

The bundled macOS ASan dylib declares macOS 11.0 as its minimum version, so
LLD warns when linking it for the x64 10.15 deployment target. This existing
toolchain runtime constraint does not change the deployment targets above.

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

Available standard-library features depend on the target SDK / sysroot version,
independently of which language-standard flags Clang accepts. In particular,
Linux's GCC 10 libstdc++ does not provide a complete C++23 standard library.

## C++ Standard Library

Every target containing C++ or Objective-C++ sources, including mixed C/C++
targets and test entry points, must explicitly add `//build:cxx_stdlib`:

```gn
source_set("example") {
  sources = [ "example.cpp" ]
  configs += [ "//build:cxx_stdlib" ]
}
```

The config selects `libs = [ "c++" ]` on Apple platforms and
`libs = [ "stdc++" ]` on Linux using the current toolchain's `current_os`.
GN propagates these libraries from source sets and static libraries to their
final link, including pure C consumers of C++ objects. Pure C targets do not
add this config, and it is not a global default.

Apple C++ / Objective-C++ compilation uses `-nostdinc++` and the selected SDK's
`usr/include/c++/v1`, with normal API availability checks. Linux uses the
sysroot's GCC headers without `-nostdinc++`. Neither platform builds its own
standard library. The existing no-RTTI / no-exceptions defaults still apply.

**Rebuild all C++ artifacts in a fresh output directory after this migration.**
The previous runtime used `std::__mn`, Apple SDK libc++ uses `std::__1`, and
Linux now uses libstdc++. Old and new C++ objects or libraries must not be
mixed. Use a new directory such as `out/system_stdlib/mac_x64_debug` with
`gn gen --check` and Ninja; do not reuse old C++ build artifacts.

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

If the target contains any C++ or Objective-C++ sources, add
`configs += [ "//build:cxx_stdlib" ]` as shown above, including for static
libraries and source sets. A pure C target needs only its normal dependencies.

Add the component's `//your_component/*` pattern to `check_targets` in `.gn`
so its sources and tests participate in the default header dependency check.
