# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build System

This monorepo uses the GN + Ninja build system. The build tools are pre-installed in the `tool/` directory.

### Essential Build Commands

```bash
# Generate build files
./tool/gn gen out

# Build all targets
./tool/ninja -C out

# Run the hello_world executable
./out/hello_world

# Clean build
rm -rf out/

# Update build tools (if needed)
./sync_tools.sh
```

### Build Configuration

- Debug builds by default (`is_debug = true` in `build/BUILDCONFIG.gn`)
- Compiler flags: `-Wall -Wextra -Werror` (strict warnings as errors)
- Currently supports macOS only (x86_64 and ARM64)

## Architecture

This is a C monorepo structured for scalability:

### Core Build Files
- `.gn` - Defines the root build configuration and toolchain
- `build/BUILDCONFIG.gn` - Global build settings and compiler configurations
- `build/toolchain/BUILD.gn` - Toolchain definitions for Clang on macOS

### Adding New Components
1. Create a new directory for your component
2. Add a `BUILD.gn` file in that directory defining targets
3. Reference the new target from the root `BUILD.gn` using `deps`

Example target structure:
```gn
executable("my_program") {
  sources = [ "main.c" ]
  deps = [ "//some/other:target" ]
}
```

### GN Target Types
- `executable()` - Creates an executable binary
- `static_library()` - Creates a static library (.a)
- `shared_library()` - Creates a shared library (.dylib on macOS)
- `source_set()` - Lightweight alternative to static_library

## Current Project Structure

```
/
├── build/           # Build system configuration
├── hello/           # Hello world example component
├── tool/            # Pre-built GN and Ninja binaries
└── sync_tools.sh    # Script to download/update build tools
```

## Important Notes

- No test framework is currently configured
- No linting beyond compiler warnings
- No CI/CD pipeline set up
- Build outputs go to `out/` directory

## Communication Guidelines

- Always respond in Chinese-simplified

## Git Guidelines

- 生成 git commit message 需要使用 git diff 的信息（包括 Untracked files）而不是对话内容，并且使用美式英语，并且不要参考之前的 git log 格式，使用 Conventional Commits 规范
