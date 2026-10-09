## Repository essentials

- This is a C/C++ monorepo built with GN and Ninja. See `README.md` for full setup, SDK, cross-compilation, and build-argument documentation.
- For top-level builds, use the repository-managed tools under `./tool/`: `./tool/gn`, `./tool/ninja`, and `./tool/clang`. Do not assume globally installed versions.
- Bootstrap missing tools with `./sync_tools.sh`.
- Default build commands are `./tool/gn gen out` and `./tool/ninja -C out`.
- Without an explicit `-std=`, the bundled Clang defaults to GNU C17 (`gnu17`) for C and GNU C++17 (`gnu++17`) for C++. The top-level GN configuration does not set a global language standard, so ordinary targets inherit these defaults. Targets requiring C++23 should reuse `//build:cxx23` instead of specifying `-std=c++23` inline; libc++ and libc++abi currently use this config.
- `tool/`, `build/sdk/`, and `out/` are generated or provisioned directories; do not edit or commit their contents.
