#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CFSSL="$REPO_ROOT/tool/cfssl"
CFSSLJSON="$REPO_ROOT/tool/cfssljson"
CSR="$REPO_ROOT/thor/cfssl/root-ca-csr.json"
OUT_DIR="$REPO_ROOT/thor/out"
OUT_PREFIX="$OUT_DIR/root-ca"

if [ "${1:-}" = "--help" ]; then
    echo "usage: $0 [--force]"
    exit 0
fi

FORCE=0
if [ "${1:-}" = "--force" ]; then
    FORCE=1
elif [ "$#" -gt 0 ]; then
    echo "error: unknown argument '$1'" >&2
    echo "usage: $0 [--force]" >&2
    exit 1
fi

if [ ! -x "$CFSSL" ]; then
    echo "error: $CFSSL not found or not executable" >&2
    echo "       run ./sync_tools.sh first" >&2
    exit 1
fi

if [ ! -x "$CFSSLJSON" ]; then
    echo "error: $CFSSLJSON not found or not executable" >&2
    echo "       run ./sync_tools.sh first" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

if [ "$FORCE" -eq 0 ] && { [ -e "$OUT_PREFIX.pem" ] || [ -e "$OUT_PREFIX-key.pem" ]; }; then
    echo "root CA already exists under thor/out"
    echo "use $0 --force to replace it"
    exit 0
fi

if [ "$FORCE" -eq 1 ]; then
    rm -f "$OUT_PREFIX.pem" "$OUT_PREFIX-key.pem" "$OUT_PREFIX.csr"
fi

"$CFSSL" gencert -initca "$CSR" | "$CFSSLJSON" -bare "$OUT_PREFIX"
chmod 600 "$OUT_PREFIX-key.pem"

echo "wrote thor/out/root-ca.pem"
echo "wrote thor/out/root-ca-key.pem"
