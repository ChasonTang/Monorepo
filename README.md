# Monorepo

A C/C++ monorepo built with GN + Ninja.

## Quick Start

Requires macOS (Intel or Apple Silicon), `python3`, and the full
[Xcode app](https://developer.apple.com/download/all/) placed or symlinked at
`./Xcode.app`. Run from the repository root:

```bash
git submodule update --init --recursive
./sync_tools.sh                  # gn, ninja, clang, cfssl → ./tool/
./extract_sdk.sh                 # macOS + iOS SDKs → ./build/sdk/
./tool/gn gen out --check
./tool/ninja -C out
```

Use `./tool/` tools for top-level builds; GN automatically selects the compiler
from `./tool/clang/bin/`.

Rerun `sync_tools.sh` to update GN, Clang, CFSSL, and CFSSLJSON when their reported
versions differ from the pins. Ninja is skipped whenever `tool/ninja` exists;
remove it before rerunning to reinstall or upgrade it. Rerun `extract_sdk.sh` to
refresh Apple SDKs.

See [Dependency upgrades](docs/dependency_upgrades.md) for updating the tools
and Linux sysroot from a newer Flutter release.

## Cross-compilation

Apple targets use `build/sdk/{MacOSX,iPhoneOS,iPhoneSimulator}.sdk/` from Quick
Start. For Linux x64, run `./sync_linux_sysroot.sh` to install the Debian
bullseye sysroot at `build/sdk/debian_bullseye_amd64-sysroot/`.
An existing sysroot directory is skipped without a version check; remove it
before rerunning to reinstall or upgrade it. New downloads are SHA256-verified.

All targets build on macOS. Use a separate output directory per target:

```bash
./tool/gn gen out/mac_arm64  --check --args='target_cpu="arm64"'
./tool/gn gen out/mac_x64    --check --args='target_cpu="x64"'
./tool/gn gen out/linux_x64  --check --args='target_os="linux" target_cpu="x64"'
./tool/gn gen out/ios_device --check --args='target_os="ios" target_environment="device" target_cpu="arm64"'
./tool/gn gen out/ios_sim    --check --args='target_os="ios" target_environment="simulator" target_cpu="arm64"'
```

Build with `./tool/ninja -C out/linux_x64`, substituting your output directory.
Intel iOS simulators use `target_cpu="x64"`. Minimum deployment versions:
macOS 10.15 (x64), macOS 11.0 (arm64), iOS 15.0.

## Build Arguments

Pass `--args=` to `./tool/gn gen`; see [build/BUILDCONFIG.gn](build/BUILDCONFIG.gn).

| Argument             | Default | Effect                                            |
| -------------------- | ------- | ------------------------------------------------- |
| `is_debug`           | `true`  | `false` enables release builds with ThinLTO.       |
| `is_asan`            | `false` | Enable AddressSanitizer (macOS/Linux only).         |
| `target_os`          | host    | `mac`, `linux`, or `ios`; Linux is x64 only.       |
| `target_cpu`         | host    | `x64` or `arm64`; iOS devices are arm64 only.      |
| `target_environment` | `""`    | Required for iOS: `device` or `simulator`.         |

The bundled macOS ASan runtime requires macOS 11.0+, so x64 ASan builds may
warn about the 10.15 deployment target.

## Compiler Defaults

Defaults: GNU C17 (`gnu17`), GNU C++17 (`gnu++17`), warnings as errors, hidden
symbols, and disabled C++ RTTI/exceptions. Append per-target overrides as needed:

```gn
configs += [ "//build:rtti" ]                # Enable C++ RTTI.
configs += [ "//build:exceptions" ]          # Enable C++ exceptions.
configs += [ "//build:visibility_default" ]  # Restore default and inline visibility.
```

Overrides affect only the target's own sources. Use `configs +=`: configs can
override flags set directly on a target.

Objective-C and Objective-C++ use ARC by default. Targets can disable it with
`cflags_objc = [ "-fno-objc-arc" ]` for `.m` sources and
`cflags_objcc = [ "-fno-objc-arc" ]` for `.mm` sources.

C++ links automatically use system libc++ on macOS/iOS and sysroot libstdc++
on Linux, including mixed C/C++ targets; no per-target standard-library config
or `libs` entry is needed. Library features depend on the SDK/sysroot; Linux's
GCC 10 libstdc++ lacks complete C++23 support.

## Tests

For native tests, ensure `openssl` is on `PATH` for TLS fixture generation:

```bash
./tool/gn gen out --check
./tool/ninja -C out tests
./out/odin_unittests
./out/argcv_unittests
```

The `tests` target also builds Odin's CLI artifacts and generates TLS fixtures
under `out/gen/thor/odin_test_certs/`.

## Formatting and Static Analysis

These checks cover `argcv/`, `odin/`, and `ipsw/`, using bundled tools and the
root `.clang-format` / `.clang-tidy` rules:

```bash
./tool/gn check out          # check header dependencies in an existing build
./format.sh                  # format project C/C++ files in place
./tidy.sh --staged            # analyze staged paths
./tidy.sh                     # analyze all project sources in the selected build
JOBS=12 BUILD_DIR=out ./tidy.sh # override parallelism and build directory
```

`gn gen --check` and Tidy also check header dependencies. Tidy analyzes only
sources in the selected build; `JOBS` defaults to the logical CPU count and
`BUILD_DIR` to `out/` relative to the repository root. `--staged` selects staged
sources and same-basename source siblings of staged headers, but analyzes
working-tree contents. It does not discover other direct or indirect includers
of a header; if no matching paths are staged, it skips analysis.

Use `--staged` when its selection covers the affected sources. For header
changes, also cover affected direct and indirect includers. A header being
`public` does not by itself require a full run; use the full check when the
affected set is broad or cannot be reliably identified or covered. New headers
must be included by a source or test in the selected build to be analyzed,
even during a full run.

## Layout

- `BUILD.gn`, `.gn`, `build/` — GN configuration, toolchains, `component.gni`.
- [`thor/`](thor/README.md) — private CA, certificate issuance, and Odin TLS
  fixtures. `sync_tools.sh` installs `cfssl`; `cfssljson` is installed on Intel
  Macs only.
- `{PROJECT}/` — upstream third-party submodules; GN adapters live in
  `build/secondary/{PROJECT}/BUILD.gn` where needed.
- `tool/`, `build/sdk/`, `out/` — generated, gitignored tools, SDKs/sysroots,
  and build artifacts.

## Adding a Component

1. Create `your_component/BUILD.gn` and add its target to the root `BUILD.gn`'s
   `default` group. Import `//build/component.gni` and use `component()` for
   libraries that should be `shared_library` in debug and `source_set` in release.
2. Declare `deps`; use `public_deps` when consumers need dependency headers.
   List exported headers in `public` and private headers in `sources`.
3. Use repository-relative includes, such as `"argcv/argcv.h"`; the root is
   already on every native target's include path.
4. Add `//your_component/*` to `check_targets` in `.gn` for header checks.

GN does not evaluate preprocessor guards. For false positives from conditional
test includes or wrappers including production `.c` files, add a per-include
`nogncheck` with an explanation; keep target header checking enabled.
