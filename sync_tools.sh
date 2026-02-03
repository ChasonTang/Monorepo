#!/bin/bash

# sync_tools.sh - Script to download build tools
# Downloads specific versions of gn, ninja, and clang toolchain to the tool directory

set -e  # Exit on any error

# Version and platform configuration
GN_GIT_REVISION="97b68a0bb62b7528bc3491c7949d6804223c2b82"  # Fixed gn git revision
NINJA_VERSION="1.13.0"  # Ninja version
CLANG_GIT_REVISION="725656bdd885483c39f482a01ea25d67acf39c46"  # Clang git revision
PLATFORM="mac"  # Target platform

# Detect current machine architecture to download corresponding gn and clang versions
ARCH=$(uname -m)
if [ "$ARCH" = "arm64" ]; then
    GN_PLATFORM="mac-arm64"
    CLANG_PLATFORM="mac-arm64"
    echo "Detected Apple Silicon (ARM64) architecture"
elif [ "$ARCH" = "x86_64" ]; then
    GN_PLATFORM="mac-amd64"
    CLANG_PLATFORM="mac-amd64"
    echo "Detected Intel (x86_64) architecture"
else
    echo "Error: Unsupported architecture $ARCH"
    exit 1
fi

# Create tool directory
TOOL_DIR="$(cd "$(dirname "$0")" && pwd)/tool"
mkdir -p "$TOOL_DIR"

echo "Syncing build tools..."
echo "Target directory: $TOOL_DIR"

# Download gn
echo "Downloading gn..."
if [ ! -f "$TOOL_DIR/gn" ]; then
    echo "Downloading gn (git revision: $GN_GIT_REVISION) for $GN_PLATFORM..."
    cd "$TOOL_DIR"

    # Download from Chrome infra packages using fixed git revision, supporting different architectures
    GN_URL="https://chrome-infra-packages.appspot.com/dl/gn/gn/${GN_PLATFORM}/+/git_revision:${GN_GIT_REVISION}"
    GN_ZIP="gn.zip"
    echo "Downloading gn ($GN_PLATFORM) from Chrome infra packages..."

    if curl -L -o "$GN_ZIP" "$GN_URL" 2>/dev/null && [ -s "$GN_ZIP" ]; then
        echo "Extracting gn..."
        if unzip -o "$GN_ZIP" && [ -f "gn" ] && file gn | grep -q "Mach-O"; then
            chmod +x gn
            rm -f "$GN_ZIP"
            echo "gn (git revision: $GN_GIT_REVISION) downloaded and extracted successfully"
        else
            rm -f gn "$GN_ZIP"
            echo "Error: gn extraction failed or file format is incorrect"
            echo ""
            echo "Please check if the downloaded file is a valid zip archive"
            echo "Download URL: $GN_URL"
            echo "Architecture: $GN_PLATFORM"
            echo ""
            echo "Alternative download methods:"
            echo "1. Use homebrew: brew install gn, then copy to this directory"
            echo "2. Copy gn binary from existing Chromium development environment"
            cd - > /dev/null
            exit 1
        fi
    else
        rm -f "$GN_ZIP"
        echo "Error: gn download failed"
        echo ""
        echo "Please check network connection or manually download gn tool"
        echo "Download URL: $GN_URL"
        echo "Architecture: $GN_PLATFORM"
        echo ""
        echo "Alternative download methods:"
        echo "1. Use homebrew: brew install gn, then copy to this directory"
        echo "2. Copy gn binary from existing Chromium development environment"
        cd - > /dev/null
        exit 1
    fi

    cd - > /dev/null
    echo "gn download completed"
else
    echo "gn already exists, skipping download"
fi

# Download ninja
echo "Downloading ninja $NINJA_VERSION..."
NINJA_URL="https://github.com/ninja-build/ninja/releases/download/v${NINJA_VERSION}/ninja-mac.zip"
NINJA_ZIP="$TOOL_DIR/ninja.zip"

if [ ! -f "$TOOL_DIR/ninja" ]; then
    echo "Downloading ninja..."
    curl -L -o "$NINJA_ZIP" "$NINJA_URL"

    echo "Extracting ninja..."
    cd "$TOOL_DIR"
    unzip -o "ninja.zip"
    chmod +x ninja
    rm -f "ninja.zip"
    cd - > /dev/null
    echo "ninja download completed"
else
    echo "ninja already exists, skipping download"
fi

# Download clang toolchain
echo "Downloading clang toolchain..."
CLANG_DIR="$TOOL_DIR/clang"

if [ ! -d "$CLANG_DIR" ] || [ ! -f "$CLANG_DIR/bin/clang" ]; then
    echo "Downloading clang (git revision: $CLANG_GIT_REVISION) for $CLANG_PLATFORM..."

    # Build CIPD download URL
    CLANG_URL="https://chrome-infra-packages.appspot.com/dl/fuchsia/third_party/clang/${CLANG_PLATFORM}/+/git_revision:${CLANG_GIT_REVISION}"
    CLANG_ZIP="$TOOL_DIR/clang.zip"
    echo "Downloading clang toolchain from Chrome infra packages..."

    if curl -L -o "$CLANG_ZIP" "$CLANG_URL" 2>/dev/null && [ -s "$CLANG_ZIP" ]; then
        echo "Extracting clang toolchain..."
        cd "$TOOL_DIR"

        # Remove old clang directory if exists
        rm -rf "$CLANG_DIR"
        mkdir -p "$CLANG_DIR"

        if unzip -o "$CLANG_ZIP" -d "$CLANG_DIR" && [ -f "$CLANG_DIR/bin/clang" ]; then
            # Make all binaries executable
            chmod +x "$CLANG_DIR/bin/"*
            rm -f "$CLANG_ZIP"
            echo "clang toolchain (git revision: $CLANG_GIT_REVISION) downloaded and extracted successfully"
        else
            rm -rf "$CLANG_DIR" "$CLANG_ZIP"
            echo "Error: clang toolchain extraction failed or file format is incorrect"
            echo ""
            echo "Please check if the downloaded file is a valid zip archive"
            echo "Download URL: $CLANG_URL"
            echo "Architecture: $CLANG_PLATFORM"
            echo ""
            echo "Alternative download methods:"
            echo "1. Use homebrew: brew install llvm, then create symlink"
            echo "2. Download from LLVM official releases"
            cd - > /dev/null
            exit 1
        fi
    else
        rm -f "$CLANG_ZIP"
        echo "Error: clang toolchain download failed"
        echo ""
        echo "Please check network connection or manually download clang toolchain"
        echo "Download URL: $CLANG_URL"
        echo "Architecture: $CLANG_PLATFORM"
        echo ""
        echo "Alternative download methods:"
        echo "1. Use homebrew: brew install llvm, then create symlink"
        echo "2. Download from LLVM official releases"
        cd - > /dev/null
        exit 1
    fi

    cd - > /dev/null
    echo "clang toolchain download completed"
else
    echo "clang toolchain already exists, skipping download"
fi

# Verify tools
echo "Verifying tools..."
if [ -f "$TOOL_DIR/gn" ] && [ -f "$TOOL_DIR/ninja" ] && [ -f "$CLANG_DIR/bin/clang" ]; then
    echo "gn version: $($TOOL_DIR/gn --version)"
    echo "ninja version: $($TOOL_DIR/ninja --version)"
    echo "clang version: $($CLANG_DIR/bin/clang --version | head -n 1)"
    echo "Tools synced successfully!"
else
    echo "Error: Tool download failed"
    exit 1
fi

echo "Tool paths:"
echo "  gn: $TOOL_DIR/gn"
echo "  ninja: $TOOL_DIR/ninja"
echo "  clang: $CLANG_DIR/bin/clang"
echo ""
echo "Usage:"
echo "  1. Run ./tool/gn gen out to generate build files"
echo "  2. Run ./tool/ninja -C out to build"
echo "  3. Use ./tool/clang/bin/clang for C/C++ compilation" 