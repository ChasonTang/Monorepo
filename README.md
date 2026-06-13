# Monorepo

A C/C++ monorepo built with GN + Ninja.

## Quick Start

```bash
git submodule update --init --recursive
./sync_tools.sh                           # gn, ninja, clang → ./tool/
./extract_sdk.sh                          # macOS + iOS SDKs from installed Xcode (see below)
./tool/gn gen out
./tool/ninja -C out
```

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
every supported target — mac↔mac and mac→linux. Switching arch/OS is just
`--target=` plus the right platform layer; `libc++` is statically linked, so
the C++ runtime travels with the binary regardless of target OS.

- **macOS (x64 / arm64):** the SDK's TBD stubs plus fat `libc++.a` /
  `libclang_rt.osx.a` cover both arches; `--target=` alone switches.
- **Linux x86_64:** `sync_linux_sysroot.sh` supplies the system layer (libc
  headers + stub `.so`s); `tool/clang/lib/x86_64-unknown-linux-gnu/` ships
  `libc++.a` / `libunwind.a` / compiler-rt, picked up via
  `-static-libstdc++ -unwindlib=libunwind`.

```bash
./tool/gn gen out/mac_arm64  --args='target_cpu="arm64"'                  # Apple Silicon
./tool/gn gen out/mac_x64    --args='target_cpu="x64"'                    # Intel mac
./tool/gn gen out/linux_x64  --args='target_os="linux" target_cpu="x64"'  # Linux (sysroot first)
```

## Build Arguments

Set via `--args=` on `gn gen`. See `build/BUILDCONFIG.gn`.

| Arg          | Default | Effect                                                  |
| ------------ | ------- | ------------------------------------------------------- |
| `is_debug`   | `true`  | Debug build; release enables ThinLTO.                   |
| `is_asan`    | `false` | Enable AddressSanitizer.                                |
| `target_os`  | host    | `mac` or `linux` (`linux` requires `target_cpu="x64"`). |
| `target_cpu` | host    | `x64` or `arm64` (arm64 only valid for `mac`).          |

## Layout

- `BUILD.gn`, `.gn`, `build/` — root build, toolchains, configs, `component.gni`.
- `build/secondary/{PROJECT}/BUILD.gn` — GN files for
  vendored third-party trees that don't ship a usable GN build.
- `{PROJECT}/` — third-party submodules (upstream layout) without their own
  GN build.
- `ipsw/` — macOS-only tool that parses `dyld_shared_cache`.
- `tool/`, `build/sdk/`, `out/` — gitignored; populated by the sync/extract scripts.

## Adding a Component

Create `your_component/BUILD.gn` with a target, then add it to the root
`BUILD.gn`'s `default` group. Use `component()` (from
`//build/component.gni`) for libraries that should be `shared_library` in
debug and `source_set` in release.
