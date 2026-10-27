#!/usr/bin/env bash
# Extract MacOSX, iPhoneOS and iPhoneSimulator SDKs from the Xcode.app
# bundled at the repo root. Output:
#   build/sdk/MacOSX.sdk/
#   build/sdk/iPhoneOS.sdk/
#   build/sdk/iPhoneSimulator.sdk/
#
# Use the repo-local Xcode.app so extraction does not depend on the host's
# xcode-select setting. See docs/dependency_upgrades.md for the required Xcode
# version and build. Place that Xcode.app at $REPO_ROOT/Xcode.app (or symlink it)
# before running.
#
# The standalone Command Line Tools .dmg is not an option for any of the
# three SDKs — it ships MacOSX.sdk without the Platforms/ wrapper and skips
# the iOS platforms entirely.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
DEST_DIR="$REPO_ROOT/build/sdk"
DEVDIR="$REPO_ROOT/Xcode.app/Contents/Developer"

if [ ! -d "$DEVDIR/Platforms/MacOSX.platform" ] ||
   [ ! -d "$DEVDIR/Platforms/iPhoneOS.platform" ] ||
   [ ! -d "$DEVDIR/Platforms/iPhoneSimulator.platform" ]; then
    echo "error: bundled Xcode missing at $REPO_ROOT/Xcode.app" >&2
    echo "       download Xcode.app from https://developer.apple.com/download/all/" >&2
    echo "       and place (or symlink) it at the repo root." >&2
    exit 1
fi

mkdir -p "$DEST_DIR"
WORK=$(mktemp -d "$DEST_DIR/.extract-sdk.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

sdk_version() {
    local sdk="$1"
    local version build
    [ -d "$sdk/usr/include" ] && [ -d "$sdk/usr/lib" ] &&
        [ -d "$sdk/System/Library/Frameworks" ] || return 1
    version=$(/usr/libexec/PlistBuddy -c 'Print :Version' "$sdk/SDKSettings.plist") || return 1
    build=$(/usr/libexec/PlistBuddy -c 'Print :ProductBuildVersion' \
        "$sdk/System/Library/CoreServices/SystemVersion.plist") || return 1
    [ -n "$version" ] && [ -n "$build" ] || return 1
    printf '%s (%s)\n' "$version" "$build"
}

extract_one() {
    local platform="$1"  # MacOSX | iPhoneOS | iPhoneSimulator
    local dest="$DEST_DIR/${platform}.sdk"
    local src="$DEVDIR/Platforms/${platform}.platform/Developer/SDKs/${platform}.sdk"
    local version installed_version

    if [ ! -d "$src" ]; then
        echo "error: missing SDK at $src" >&2
        exit 1
    fi
    # Resolve symlinks so the SDK contents are copied.
    src="$(cd "$src" && pwd -P)"

    if ! version=$(sdk_version "$src"); then
        echo "error: incomplete SDK or missing version/build metadata at $src" >&2
        exit 1
    fi
    installed_version=$(sdk_version "$dest" 2>/dev/null) || installed_version=""
    if [ "$installed_version" = "$version" ]; then
        echo "${platform} SDK $version already at $dest, skipping"
        return 0
    fi

    echo "==> Copying ${platform} SDK $version -> $dest"
    # Stage the copy so an interrupted extraction cannot look up to date.
    cp -a "$src" "$WORK/${platform}.sdk"
    rm -rf "$dest"
    mv "$WORK/${platform}.sdk" "$dest"
}

extract_one MacOSX
extract_one iPhoneOS
extract_one iPhoneSimulator

echo ""
echo "Done. SDKs at:"
echo "  $DEST_DIR/MacOSX.sdk"
echo "  $DEST_DIR/iPhoneOS.sdk"
echo "  $DEST_DIR/iPhoneSimulator.sdk"
