#!/usr/bin/env bash
# Download a SHA256-pinned Debian bullseye amd64 sysroot for Linux x86_64
# cross-compilation when the destination is absent. Uses Chromium's sysroot
# archives without requiring the gclient/Python stack.
#
# Source: chrome-linux-sysroot GCS bucket. Object key is the sha256.
# To bump: copy a new SysrootDict from chromium/src/build/linux/sysroot_scripts/sysroots.json
# Output: build/sdk/debian_bullseye_amd64-sysroot/
# An existing destination directory is skipped; remove it to reinstall or upgrade.

set -euo pipefail

SYSROOT_SHA256="36a164623d03f525e3dfb783a5e9b8a00e98e1ddd2b5cff4e449bd016dd27e50"
SYSROOT_URL="https://commondatastorage.googleapis.com/chrome-linux-sysroot/${SYSROOT_SHA256}"
SYSROOT_NAME="debian_bullseye_amd64-sysroot"

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
DEST="$REPO_ROOT/build/sdk/$SYSROOT_NAME"

sysroot_is_complete() {
    [ -f "$1/usr/include/stdio.h" ] &&
        [ -f "$1/usr/lib/x86_64-linux-gnu/libc.so" ]
}

if [ -d "$DEST" ]; then
    echo "$SYSROOT_NAME already exists at $DEST, skipping"
    exit 0
fi

if command -v sha256sum >/dev/null 2>&1; then
    sha256() { sha256sum "$1" | awk '{print $1}'; }
else
    sha256() { shasum -a 256 "$1" | awk '{print $1}'; }
fi

mkdir -p "$REPO_ROOT/build/sdk"
WORK=$(mktemp -d "$REPO_ROOT/build/sdk/.sysroot.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

TARBALL="$WORK/sysroot.tar.xz"

echo "==> Downloading $SYSROOT_URL"
curl -fL --retry 3 -o "$TARBALL" "$SYSROOT_URL"

ACTUAL=$(sha256 "$TARBALL")
if [ "$ACTUAL" != "$SYSROOT_SHA256" ]; then
    echo "error: sha256 mismatch" >&2
    echo "       expected $SYSROOT_SHA256" >&2
    echo "       actual   $ACTUAL" >&2
    exit 1
fi

echo "==> Extracting -> $DEST"
EXTRACTED="$WORK/$SYSROOT_NAME"
mkdir -p "$EXTRACTED"
tar mxf "$TARBALL" -C "$EXTRACTED"
if ! sysroot_is_complete "$EXTRACTED"; then
    echo "error: extracted sysroot is missing required headers or libraries" >&2
    exit 1
fi

# Publish only after the archive has been verified and fully extracted.
rm -rf "$DEST"
mv "$EXTRACTED" "$DEST"

echo ""
echo "Done. Sysroot at: $DEST"
