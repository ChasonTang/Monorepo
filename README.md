# Monorepo

A C/C++ monorepo built with GN + Ninja.

## Quick Start

Requires macOS, `python3`, and the full
[Xcode app](https://developer.apple.com/download/all/) placed or symlinked at
`./Xcode.app`. Run from the repository root:

```bash
git submodule update --init --recursive
./sync_tools.sh                  # gn, ninja, clang, cfssl → ./tool/
./extract_sdk.sh                 # macOS + iOS SDKs → ./build/sdk/
./tool/gn gen out --check
./tool/ninja -C out
```

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
./tool/gn gen out/mac  --check --args='target_cpu="arm64"'
./tool/gn gen out/mac_x64    --check --args='target_cpu="x64"'
./tool/gn gen out/linux  --check --args='target_os="linux" target_cpu="x64"'
./tool/gn gen out/ios --check --args='target_os="ios" target_environment="device" target_cpu="arm64"'
./tool/gn gen out/ios_sim    --check --args='target_os="ios" target_environment="simulator" target_cpu="arm64"'
./tool/gn gen out/ios_sim_x64    --check --args='target_os="ios" target_environment="simulator" target_cpu="x64"'
```

Build with `./tool/ninja -C {BUILD_DIR}`, substituting your output directory.
Minimum deployment versions: macOS 10.15 (x64), macOS 11.0 (arm64), iOS 15.0.

## Build Arguments

Pass `--args=` to `./tool/gn gen`; see [build/BUILDCONFIG.gn](build/BUILDCONFIG.gn).

| Argument             | Default | Effect                                            |
| -------------------- | ------- | ------------------------------------------------- |
| `is_debug`           | `true`  | `false`: ThinLTO and link-time size optimization. |
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
./tool/gn check out           # check header dependencies in an existing build
./format.sh                  # format project C/C++ files in place
./tidy.sh --staged            # analyze sources selected from staged paths
./tidy.sh                     # analyze all project sources in the selected build
JOBS=12 BUILD_DIR=out ./tidy.sh # override parallelism and build directory
```

`JOBS` defaults to the logical CPU count and `BUILD_DIR` to `out/` relative to
the repository root. Both Tidy modes analyze working-tree contents, using only
`.c`, `.cc`, and `.cpp` entries under the project directories above in the
selected build's `compile_commands.json`. A full run analyzes all such entries.

`--staged` selects existing `.c`, `.cc`, and `.cpp` paths from the staged diff.
For staged `.h` and `.hpp` paths, it also selects existing source files with
the same basename in the same directory. Deleted paths are excluded. It does
not discover other direct or indirect includers of headers or source files.

A full run, or a staged run with candidates, regenerates the compilation
database with `gn gen --check`, which also checks header dependencies. With no
candidates, `--staged` exits successfully before both GN and static analysis;
this can happen with only configuration changes, deletions, or headers without
same-basename sources. If candidates exist but none match the database, GN
runs and the analyzer reports `0 files`, also successfully. Confirm that the
reported file count and paths cover the intended sources.

Choose the mode by the affected sources:

| Situation | Action |
| --------- | ------ |
| Every affected source is selected by `--staged` and appears in the selected build's database. | Run `./tidy.sh --staged`. |
| Affected includers or test wrappers are outside that selection, or coverage cannot be reliably established. | Run `./tidy.sh`. |
| Analysis rules, compiler macros/options, or shared build configuration change. | Run `./tidy.sh` in the affected build configurations. |
| Affected sources or conditional branches are absent from the selected build. | Add the relevant GN targets/sources or configure an appropriate `BUILD_DIR`, then choose the mode using the rules above. |

A header being `public` does not by itself require a full run; cover its
affected direct and indirect includers. Source-file changes can also affect
other compilation units: `odin/testing/event_loop_testing.c`, for example,
includes `odin/event_loop.c` and is not automatically selected when only the
latter is staged. New headers must be included by an analyzed source or test
in the selected build, even during a full run.

A full run covers only the selected build configuration. For platform-specific
changes, use the corresponding configured build directories, for example
`BUILD_DIR=out/linux ./tidy.sh`; the default build does not cover every platform
or conditional compilation branch.

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
   libraries that should be `shared_library` in debug and `static_library` in
   release.
2. Declare `deps`; use `public_deps` when consumers need dependency headers.
   List exported headers in `public` and private headers in `sources`.
3. Add an export macro in a public `export.h` and configure it through
   `public_configs`, as shown below.
4. Use repository-relative includes, such as `"argcv/argcv.h"`.
5. Add `//your_component/*` to `check_targets` in `.gn` for header checks.

GN does not evaluate preprocessor guards. For false positives from conditional
test includes or wrappers including production `.c` files, add a per-include
`nogncheck` with an explanation; keep target header checking enabled.

Put settings required by public headers in `public_configs` and
implementation-only settings in `configs`. Use a component-specific macro
and config name, following [argcv/BUILD.gn](argcv/BUILD.gn):

```gn
config("your_component_public_config") {
  # Match the shared-library branch of //build/component.gni.
  if (is_debug) {
    defines = [ "YOUR_COMPONENT_SHARED_LIBRARY" ]
  }
}
```

Attach the config to each component target exposing these headers:

```gn
public_configs = [ ":your_component_public_config" ]
```

Include `export.h` from public headers and annotate API declarations with
`YOUR_COMPONENT_EXPORT`. Follow [argcv/export.h](argcv/export.h):

```c
#if defined(YOUR_COMPONENT_SHARED_LIBRARY)
#define YOUR_COMPONENT_EXPORT __attribute__((visibility("default")))
#else
#define YOUR_COMPONENT_EXPORT
#endif
```

Match the macro condition to the shared-library branch of
[component.gni](build/component.gni). Leave `YOUR_COMPONENT_SHARED_LIBRARY`
undefined in static builds; do not define it as `0`.
