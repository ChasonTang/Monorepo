## Agent guidance

- Use [README.md](README.md) for setup, build, test, and formatting instructions; consult the sections relevant to the task.
- For top-level builds, use the repository-managed tools under `./tool/`: `./tool/gn`, `./tool/ninja`, and the Clang toolchain in `./tool/clang/` (`./tool/clang/bin/clang` for C and `./tool/clang/bin/clang++` for C++). Do not assume globally installed versions. Bootstrap missing tools with `./sync_tools.sh`.
- For targets requiring C++23, reuse `//build:cxx23` instead of adding `-std=c++23` inline.
- Do not manually edit or commit the contents of `tool/`, `build/sdk/`, or `out/`; these directories are populated by repository scripts and build tools.
