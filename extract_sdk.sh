#!/usr/bin/env bash
# Extract MacOSX, iPhoneOS and iPhoneSimulator SDKs from the Xcode.app
# bundled at the repo root. Output:
#   build/sdk/MacOSX.sdk/
#   build/sdk/iPhoneOS.sdk/
#   build/sdk/iPhoneSimulator.sdk/
#
# We deliberately do NOT use xcode-select -p / /Applications/Xcode.app: the
# extraction must be hermetic so every developer pulls the same SDK version
# regardless of what the host machine happens to have installed. Drop the
# matching Xcode.app at $REPO_ROOT/Xcode.app (or symlink it) before running.
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

extract_one() {
    local platform="$1"  # MacOSX | iPhoneOS | iPhoneSimulator
    local dest="$DEST_DIR/${platform}.sdk"

    if [ -e "$dest" ]; then
        echo "${platform} SDK already at $dest (delete to re-extract)"
        return 0
    fi

    # Inside the platform dir, ${platform}.sdk is a symlink to ${platform}<ver>.sdk;
    # pick the real directory so the copy captures concrete files instead of
    # recreating the symlink.
    local sdks="$DEVDIR/Platforms/${platform}.platform/Developer/SDKs"
    local src
    src="$(find "$sdks" -maxdepth 1 -type d -name "${platform}*.sdk" ! -type l -print | sort -r | head -1)"
    [ -n "$src" ] || { echo "error: no ${platform}*.sdk under $sdks" >&2; exit 1; }

    echo "==> Copying $(basename "$src") -> $dest"
    mkdir -p "$DEST_DIR"
    cp -a "$src" "$dest"
}

extract_one MacOSX
extract_one iPhoneOS
extract_one iPhoneSimulator

echo ""
echo "Done. SDKs at:"
echo "  $DEST_DIR/MacOSX.sdk"
echo "  $DEST_DIR/iPhoneOS.sdk"
echo "  $DEST_DIR/iPhoneSimulator.sdk"
