# Monorepo

A C/C++ monorepo built with GN + Ninja, vendoring BoringSSL, c-ares, xquic, and
the macOS-only `ipsw` tool.

## Quick Start

```bash
./sync_tools.sh              # Fetch pinned gn, ninja, and clang
./extract_sdk.sh             # Extract macOS SDK (see below)
./tool/gn gen out
./tool/ninja -C out
```

## SDK Setup

**macOS (required for any build):** download
`Command_Line_Tools_for_Xcode_<version>.dmg` from
<https://developer.apple.com/download/all/>, place it at the repo root, then
run `./extract_sdk.sh`. The SDK lands in `build/sdk/MacOSX.sdk/`.

**Linux x86_64 (cross-compile only):** run `./sync_linux_sysroot.sh` to fetch a
SHA256-pinned Debian bullseye sysroot, then generate with:

```bash
./tool/gn gen out_linux --args='target_os="linux" target_cpu="x64"'
./tool/ninja -C out_linux
```

## Build Arguments

Set via `--args=` on `gn gen`. See `build/BUILDCONFIG.gn`.

| Arg         | Default | Effect                                                  |
| ----------- | ------- | ------------------------------------------------------- |
| `is_debug`  | `true`  | Debug build; release enables ThinLTO.                   |
| `is_asan`   | `false` | Enable AddressSanitizer.                                |
| `target_os` | host    | `mac` or `linux`.                                       |

## Layout

- `BUILD.gn`, `.gn`, `build/` — root build, toolchains, and configs.
- `build/secondary/{boringssl,c-ares,xquic}/BUILD.gn` — GN files for vendored
  third-party sources whose own trees stay untouched.
- `boringssl/`, `c-ares/`, `xquic/` — third-party sources (upstream layout).
- `ipsw/` — macOS-only tool that parses `dyld_shared_cache`.
- `tool/` — pinned `gn`, `ninja`, and `clang` (populated by `sync_tools.sh`).
- `out/` — build output (gitignored).

## Adding a Component

Create `your_component/BUILD.gn` with a target, then add it to the root
`BUILD.gn`'s `default` group. Use `component()` (from
`//build/component.gni`) for libraries that should be `shared_library` in
debug and `source_set` in release.
