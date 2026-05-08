#!/usr/bin/env bash
# Format odin C/C++ sources in place using ./tool/clang/bin/clang-format.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
CLANG_FORMAT="$REPO_ROOT/tool/clang/bin/clang-format"
ODIN_DIR="$REPO_ROOT/odin"

if [ ! -x "$CLANG_FORMAT" ]; then
    echo "error: $CLANG_FORMAT not found or not executable" >&2
    echo "       run ./sync_tools.sh first" >&2
    exit 1
fi

if [ ! -d "$ODIN_DIR" ]; then
    echo "error: $ODIN_DIR not found" >&2
    exit 1
fi

COUNT=0
while IFS= read -r -d '' f; do
    echo "formatting ${f#$REPO_ROOT/}"
    "$CLANG_FORMAT" -i --style=file "$f"
    COUNT=$((COUNT + 1))
done < <(find "$ODIN_DIR" \
    -type f \
    \( -name '*.c' -o -name '*.h' -o -name '*.cc' -o -name '*.cpp' -o -name '*.hpp' \) \
    -print0)

if [ "$COUNT" -eq 0 ]; then
    echo "no C/C++ sources found under $ODIN_DIR"
    exit 0
fi

echo "done. formatted $COUNT file(s)."
