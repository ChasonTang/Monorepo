# Monorepo - C Hello World Project

This is a C language Hello World project using the gn + ninja build system, configured for macOS platform development.

## Project Structure

```
Monorepo/
├── .gn                     # gn project root configuration
├── BUILD.gn               # Root build file
├── build/                 # Build configuration directory
│   ├── BUILDCONFIG.gn     # Build configuration
│   ├── BUILD.gn          # Build rules
│   └── toolchain/        # Toolchain configuration
│       └── BUILD.gn      # Clang toolchain definition
├── hello/                # Hello World subproject
│   ├── hello_world.c     # C source code
│   └── BUILD.gn          # Build configuration
├── sync_tools.sh         # Tool synchronization script
├── README.md             # Project documentation
├── LICENSE               # MIT License
└── tool/                 # Build tools directory (created after running sync script)
    ├── gn                # gn build system generator
    └── ninja             # ninja build executor
```

## Usage Steps

### 1. Sync Build Tools

First, run the tool synchronization script to download the required gn and ninja tools:

```bash
chmod +x sync_tools.sh
./sync_tools.sh
```

This script will:
- Automatically detect machine architecture (Intel x86_64 or Apple Silicon ARM64)
- Download the appropriate gn tool for the current architecture (zip format) and extract to `tool/gn`
- Download the specific version of ninja tool (zip format) and extract to `tool/ninja`
- Verify that tools are downloaded and extracted successfully



### 2. Generate Build Files

Use gn to generate ninja build files:

```bash
./tool/gn gen out
```

This will generate ninja build files in the `out` directory.

### 3. Build Project

Use ninja to build the project:

```bash
./tool/ninja -C out
```

### 4. Run Program

After building, the executable will be generated in the `out/` directory:

```bash
./out/hello_world
```

Should output:
```
Hello, World!
```

## Build Configuration

- **Target Platform**: macOS (currently supported)
- **Compiler**: Clang (via Xcode toolchain)
- **Build Mode**: Debug (default), Release mode uses `-Os` optimization for code size and preserves debug symbols
- **Target Architecture**: Defaults to host architecture (x86_64 or ARM64)
- **Build Machine Support**: Intel Mac (x86_64) and Apple Silicon Mac (ARM64)
- **Cross Compilation**: Supports cross-compilation by setting target_cpu and target_os

## Custom Build Parameters

You can customize build parameters using the `gn args out` command:

```bash
./tool/gn args out
```

Available build parameters:
- `is_debug = false`  # Set to Release mode
- `target_cpu = "arm64"`  # Set target architecture to ARM64
- `target_os = "mac"`  # Set target operating system

Example configuration file content:
```
is_debug = false
target_cpu = "arm64"
target_os = "mac"
```

### Cross Compilation Support

The build system now supports cross-compilation with the following parameter descriptions:
- `current_cpu` and `current_os`: Platform where the build toolchain runs (auto-detected, fixed to macOS)
- `target_cpu` and `target_os`: Platform where the target code runs (configurable)

For example, to build for ARM64 architecture:
```bash
./tool/gn args out
# Add in editor: target_cpu = "arm64"
./tool/ninja -C out
```

### Release Build

Release mode uses `-Os` optimization to reduce code size while preserving debug symbols for crash analysis:
```bash
echo "is_debug = false" > out/args.gn
./tool/gn gen out
./tool/ninja -C out
```

Release mode compilation options:
- `-Os`: Optimize for code size
- `-g`: Preserve debug symbols for crash analysis
- `-DNDEBUG=1`: Disable debug assertions

## Tool Versions

- **gn**: git revision `97b68a0bb62b7528bc3491c7949d6804223c2b82` (fixed version)
  - Supports Intel Mac (mac-amd64) and Apple Silicon Mac (mac-arm64)
  - Automatically detects current machine architecture and downloads corresponding version
- **ninja**: 1.13.0 (latest version)

To update tool versions, modify the version numbers in the `sync_tools.sh` script and re-run. Using a fixed git revision ensures build environment consistency and reproducibility.



## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details. 