#!/usr/bin/env bash
# Extract MacOSX SDK from Command_Line_Tools_for_Xcode_*.dmg in repo root.
# Output: build/sdk/MacOSX.sdk/

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
DEST="$REPO_ROOT/build/sdk/MacOSX.sdk"

DMG=$(ls -1 "$REPO_ROOT"/Command_Line_Tools_for_Xcode_*.dmg 2>/dev/null | head -1)
if [ -z "$DMG" ]; then
    echo "error: no Command_Line_Tools_for_Xcode_*.dmg in $REPO_ROOT" >&2
    echo "       download it from https://developer.apple.com/download/all/" >&2
    exit 1
fi

if [ -e "$DEST" ]; then
    echo "SDK already at $DEST (delete to re-extract)"
    exit 0
fi

MNT=$(mktemp -d /tmp/cltsdk-mnt.XXXXXX)
WORK=$(mktemp -d /tmp/cltsdk-work.XXXXXX)
cleanup() {
    hdiutil detach "$MNT" -quiet 2>/dev/null || true
    rm -rf "$MNT" "$WORK"
}
trap cleanup EXIT

echo "==> Mounting $(basename "$DMG")"
hdiutil attach "$DMG" -mountpoint "$MNT" -nobrowse -quiet

PKG=$(find "$MNT" -maxdepth 2 -name '*.pkg' -print -quit)
[ -n "$PKG" ] || { echo "error: no .pkg in dmg" >&2; exit 1; }

echo "==> Expanding $(basename "$PKG")"
pkgutil --expand-full "$PKG" "$WORK/expanded"

SDK=$(find "$WORK/expanded" -type d -name 'MacOSX*.sdk' ! -type l -print | sort -r | head -1)
[ -n "$SDK" ] || { echo "error: MacOSX*.sdk not found inside pkg" >&2; exit 1; }

echo "==> Copying $(basename "$SDK") -> $DEST"
mkdir -p "$(dirname "$DEST")"
cp -a "$SDK" "$DEST"

echo ""
echo "Done. SDK at: $DEST"
