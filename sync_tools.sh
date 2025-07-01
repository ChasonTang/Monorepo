#!/bin/bash

# sync_tools.sh - Script to download build tools
# Downloads specific versions of gn and ninja to the tool directory

set -e  # Exit on any error

# Version and platform configuration
GN_GIT_REVISION="97b68a0bb62b7528bc3491c7949d6804223c2b82"  # Fixed gn git revision
NINJA_VERSION="1.13.0"  # Ninja version
PLATFORM="mac"  # Target platform

# Detect current machine architecture to download corresponding gn version
ARCH=$(uname -m)
if [ "$ARCH" = "arm64" ]; then
    GN_PLATFORM="mac-arm64"
    echo "Detected Apple Silicon (ARM64) architecture"
elif [ "$ARCH" = "x86_64" ]; then
    GN_PLATFORM="mac-amd64"
    echo "Detected Intel (x86_64) architecture"
else
    echo "Error: Unsupported architecture $ARCH"
    exit 1
fi

# Create tool directory
TOOL_DIR="$(dirname "$0")/tool"
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

# Verify tools
echo "Verifying tools..."
if [ -f "$TOOL_DIR/gn" ] && [ -f "$TOOL_DIR/ninja" ]; then
    echo "gn version: $($TOOL_DIR/gn --version)"
    echo "ninja version: $($TOOL_DIR/ninja --version)"
    echo "Tools synced successfully!"
else
    echo "Error: Tool download failed"
    exit 1
fi

echo "Tool paths:"
echo "  gn: $TOOL_DIR/gn"
echo "  ninja: $TOOL_DIR/ninja"
echo ""
echo "Usage:"
echo "  1. Run ./tool/gn gen out to generate build files"
echo "  2. Run ./tool/ninja -C out to build"
echo "  3. Run ./out/hello_world to execute the program" 