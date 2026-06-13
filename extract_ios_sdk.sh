#!/usr/bin/env bash
# Extract iPhoneOS and iPhoneSimulator SDKs from a full Xcode install.
# Output:
#   build/sdk/iPhoneOS.sdk/
#   build/sdk/iPhoneSimulator.sdk/
#
# Unlike the macOS SDK, iOS SDKs only ship inside Xcode.app — the standalone
# Command Line Tools .dmg does not contain Platforms/iPhoneOS.platform, so we
# can't reuse extract_sdk.sh's dmg/pkg flow.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
DEST_DIR="$REPO_ROOT/build/sdk"

DEVDIR="$(xcode-select -p 2>/dev/null || true)"
if [ -z "$DEVDIR" ] || [ ! -d "$DEVDIR/Platforms/iPhoneOS.platform" ]; then
    echo "error: full Xcode required (xcode-select -p points at '$DEVDIR')" >&2
    echo "       install Xcode.app and run: sudo xcode-select -s /Applications/Xcode.app/Contents/Developer" >&2
    exit 1
fi

extract_one() {
    local platform="$1"  # iPhoneOS | iPhoneSimulator
    local dest="$DEST_DIR/${platform}.sdk"

    if [ -e "$dest" ]; then
        echo "${platform} SDK already at $dest (delete to re-extract)"
        return 0
    fi

    # Inside the platform dir, ${platform}.sdk is a symlink to ${platform}<ver>.sdk;
    # pick the real directory (mirrors extract_sdk.sh's ! -type l filter) so the
    # copy captures concrete files instead of recreating the symlink.
    local sdks="$DEVDIR/Platforms/${platform}.platform/Developer/SDKs"
    local src
    src="$(find "$sdks" -maxdepth 1 -type d -name "${platform}*.sdk" ! -type l -print | sort -r | head -1)"
    [ -n "$src" ] || { echo "error: no ${platform}*.sdk under $sdks" >&2; exit 1; }

    echo "==> Copying $(basename "$src") -> $dest"
    mkdir -p "$DEST_DIR"
    cp -a "$src" "$dest"
}

extract_one iPhoneOS
extract_one iPhoneSimulator

echo ""
echo "Done. SDKs at:"
echo "  $DEST_DIR/iPhoneOS.sdk"
echo "  $DEST_DIR/iPhoneSimulator.sdk"
