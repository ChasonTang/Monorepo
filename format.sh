#!/usr/bin/env bash
# Format odin, argcv, and ipsw C/C++ sources in place using
# ./tool/clang/bin/clang-format and the repository-root .clang-format.
#
# Usage: ./format.sh

set -euo pipefail

if [ "$#" -ne 0 ]; then
    echo "usage: ./format.sh" >&2
    exit 1
fi

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
CLANG_FORMAT="$REPO_ROOT/tool/clang/bin/clang-format"
SOURCE_DIRS=("$REPO_ROOT/odin" "$REPO_ROOT/argcv" "$REPO_ROOT/ipsw")

if [ ! -x "$CLANG_FORMAT" ]; then
    echo "error: $CLANG_FORMAT not found or not executable" >&2
    echo "       run ./sync_tools.sh first" >&2
    exit 1
fi

for dir in "${SOURCE_DIRS[@]}"; do
    if [ ! -d "$dir" ]; then
        echo "error: $dir not found" >&2
        exit 1
    fi
done

FILES=()
while IFS= read -r -d '' f; do
    FILES+=("$f")
done < <(find "${SOURCE_DIRS[@]}" \
    -type f \
    \( -name '*.c' -o -name '*.h' -o -name '*.cc' -o -name '*.cpp' -o -name '*.hpp' \) \
    -print0)

COUNT="${#FILES[@]}"

if [ "$COUNT" -eq 0 ]; then
    echo "no C/C++ sources found under odin/, argcv/, or ipsw/"
    exit 0
fi

"$CLANG_FORMAT" -i --verbose "${FILES[@]}"

echo "done. formatted $COUNT file(s)."
