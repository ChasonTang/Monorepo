#!/bin/bash

# Download pinned build tools. Skip an existing Ninja binary; compare reported
# versions for GN, Clang, CFSSL, and CFSSLJSON.

set -euo pipefail

# Version and platform configuration
GN_GIT_REVISION="81b24e01531ecf0eff12ec9359a555ec3944ec4e"  # Fixed gn git revision
NINJA_VERSION="version:2@1.11.1.chromium.4"  # Ninja CIPD tag
CLANG_GIT_REVISION="80743bd43fd5b38fedc503308e7a652e23d3ec93"  # Clang git revision
CFSSL_VERSION="1.7.1"  # Cloudflare CFSSL release version

# Detect current machine architecture to download corresponding tool versions
ARCH=$(uname -m)
if [ "$ARCH" = "arm64" ]; then
    GN_PLATFORM="mac-arm64"
    NINJA_PLATFORM="mac-arm64"
    CLANG_PLATFORM="mac-arm64"
    CFSSL_PLATFORM="darwin_arm64"
    CFSSLJSON_PLATFORM=""
    echo "Detected Apple Silicon (ARM64) architecture"
elif [ "$ARCH" = "x86_64" ]; then
    GN_PLATFORM="mac-amd64"
    NINJA_PLATFORM="mac-amd64"
    CLANG_PLATFORM="mac-amd64"
    CFSSL_PLATFORM="darwin_amd64"
    CFSSLJSON_PLATFORM="darwin_amd64"
    echo "Detected Intel (x86_64) architecture"
else
    echo "Error: Unsupported architecture $ARCH" >&2
    exit 1
fi

TOOL_DIR="$(cd "$(dirname "$0")" && pwd)/tool"
mkdir -p "$TOOL_DIR"
WORK=$(mktemp -d "$TOOL_DIR/.sync-tools.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

echo "Syncing build tools..."
echo "Target directory: $TOOL_DIR"

tool_is_current() {
    local tool_name="$1"
    local binary="$2"
    local version_arg="--version"
    local version_output
    local gn_version_pattern='^[0-9]+ \(([0-9a-f]+)\)$'

    [ -f "$binary" ] || return 1
    if [ "$tool_name" = "ninja" ]; then
        return 0
    fi

    [ -x "$binary" ] || return 1
    case "$tool_name" in
        cfssl) version_arg="version" ;;
        cfssljson) version_arg="-version" ;;
    esac
    version_output=$("$binary" "$version_arg" 2>/dev/null) || return 1

    case "$tool_name" in
        gn)
            # GN reports a short hash, so match the pinned revision's prefix.
            [[ "$version_output" =~ $gn_version_pattern ]] &&
                [[ "$GN_GIT_REVISION" == "${BASH_REMATCH[1]}"* ]]
            ;;
        clang)
            [[ "$version_output" == *" $CLANG_GIT_REVISION)"* ]]
            ;;
        cfssl|cfssljson)
            printf '%s\n' "$version_output" | grep -Fx "Version: $CFSSL_VERSION" >/dev/null
            ;;
        *) return 1 ;;
    esac
}

sync_cipd_tool() {
    local tool_name="$1"
    local package="$2"
    local version="$3"
    local binary="$4"
    local url="https://chrome-infra-packages.appspot.com/dl/${package}/+/${version}"
    local output_path="$TOOL_DIR/$tool_name"
    local installed_binary="$output_path"
    local archive="$WORK/$tool_name.zip"
    local extracted="$WORK/$tool_name"
    local install_path="$extracted/$binary"

    if [ "$tool_name" = "clang" ]; then
        installed_binary="$output_path/$binary"
        install_path="$extracted"
    fi

    if tool_is_current "$tool_name" "$installed_binary"; then
        echo "$tool_name already installed, skipping"
        return
    fi

    echo "==> Downloading $tool_name ($version) from $url"
    curl -fL --retry 3 -o "$archive" "$url"
    mkdir -p "$extracted"
    unzip -q "$archive" -d "$extracted"
    if [ ! -f "$extracted/$binary" ] ||
        ! file -L "$extracted/$binary" | grep 'Mach-O' >/dev/null; then
        echo "Error: $tool_name archive does not contain a valid macOS binary" >&2
        exit 1
    fi

    if [ "$tool_name" = "clang" ]; then
        chmod +x "$extracted/bin/"*
    else
        chmod +x "$extracted/$binary"
    fi
    "$extracted/$binary" --version

    # Replace the whole artifact so files removed by an upgrade cannot linger.
    rm -rf "$output_path"
    mv "$install_path" "$output_path"
    echo "$tool_name $version installed successfully"
}

download_cfssl_tool() {
    local tool_name="$1"
    local download_url="$2"
    local output_path="$TOOL_DIR/$tool_name"
    local temp_path="$WORK/$tool_name"
    local version_arg="version"

    if [ "$tool_name" = "cfssljson" ]; then
        version_arg="-version"
    fi

    if tool_is_current "$tool_name" "$output_path"; then
        echo "$tool_name $CFSSL_VERSION already installed, skipping"
        return
    fi

    echo "==> Downloading $tool_name $CFSSL_VERSION..."
    curl -fL --retry 3 -o "$temp_path" "$download_url"
    if ! file "$temp_path" | grep 'Mach-O' >/dev/null; then
        echo "Error: $tool_name download is not a valid macOS binary" >&2
        exit 1
    fi
    chmod +x "$temp_path"
    if ! "$temp_path" "$version_arg" | grep -Fx "Version: $CFSSL_VERSION" >/dev/null; then
        echo "Error: $tool_name download does not report version $CFSSL_VERSION" >&2
        exit 1
    fi

    rm -rf "$output_path"
    mv "$temp_path" "$output_path"
    echo "$tool_name $CFSSL_VERSION installed successfully"
}

sync_cipd_tool gn "gn/gn/$GN_PLATFORM" "git_revision:$GN_GIT_REVISION" gn
sync_cipd_tool ninja "infra/3pp/tools/ninja/$NINJA_PLATFORM" "$NINJA_VERSION" ninja
sync_cipd_tool clang "fuchsia/third_party/clang/$CLANG_PLATFORM" "git_revision:$CLANG_GIT_REVISION" bin/clang

CFSSL_BASE_URL="https://github.com/cloudflare/cfssl/releases/download/v${CFSSL_VERSION}"
download_cfssl_tool cfssl "${CFSSL_BASE_URL}/cfssl_${CFSSL_VERSION}_${CFSSL_PLATFORM}"

if [ -n "$CFSSLJSON_PLATFORM" ]; then
    download_cfssl_tool cfssljson "${CFSSL_BASE_URL}/cfssljson_${CFSSL_VERSION}_${CFSSLJSON_PLATFORM}"
else
    echo "Skipping cfssljson download on ARM64: no darwin_arm64 prebuilt binary is published for cfssljson $CFSSL_VERSION"
fi

echo "Verifying tools..."
"$TOOL_DIR/gn" --version
"$TOOL_DIR/ninja" --version
"$TOOL_DIR/clang/bin/clang" --version
"$TOOL_DIR/cfssl" version
if [ -n "$CFSSLJSON_PLATFORM" ]; then
    "$TOOL_DIR/cfssljson" -version
fi
echo "Tools synced successfully at $TOOL_DIR"
