#!/usr/bin/env bash
# Format odin and argcv C/C++ sources in place using
# ./tool/clang/bin/clang-format and the repository-root .clang-format.
#
# Usage:
#   ./format.sh                # format every odin and argcv C/C++ source
#   ./format.sh --diff         # format only files changed vs origin/main
#   ./format.sh --diff HEAD~1  # format only files changed vs HEAD~1

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
CLANG_FORMAT="$REPO_ROOT/tool/clang/bin/clang-format"
SOURCE_DIRS=("$REPO_ROOT/odin" "$REPO_ROOT/argcv")

DIFF_MODE=0
DIFF_BASE=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --diff)
            DIFF_MODE=1
            shift
            if [ "$#" -gt 0 ] && [[ "$1" != --* ]]; then
                DIFF_BASE="$1"
                shift
            fi
            ;;
        *)
            echo "error: unknown argument '$1'" >&2
            exit 1
            ;;
    esac
done

if [ "$DIFF_MODE" -eq 1 ] && [ -z "$DIFF_BASE" ]; then
    DIFF_BASE="origin/main"
fi

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
if [ "$DIFF_MODE" -eq 1 ]; then
    if ! git -C "$REPO_ROOT" rev-parse --verify --quiet "$DIFF_BASE" >/dev/null; then
        echo "error: diff base '$DIFF_BASE' not found in git" >&2
        exit 1
    fi
    echo "collecting odin and argcv changes vs $DIFF_BASE"
    while IFS= read -r -d '' rel; do
        case "$rel" in odin/*|argcv/*) ;; *) continue ;; esac
        case "$rel" in
            *.c|*.h|*.cc|*.cpp|*.hpp) ;;
            *) continue ;;
        esac
        abs="$REPO_ROOT/$rel"
        [ -f "$abs" ] || continue
        FILES+=("$abs")
    done < <(git -C "$REPO_ROOT" diff --name-only -z --diff-filter=ACMRTUB "$DIFF_BASE" -- odin argcv)
else
    while IFS= read -r -d '' f; do
        FILES+=("$f")
    done < <(find "${SOURCE_DIRS[@]}" \
        -type f \
        \( -name '*.c' -o -name '*.h' -o -name '*.cc' -o -name '*.cpp' -o -name '*.hpp' \) \
        -print0)
fi

COUNT="${#FILES[@]}"

if [ "$COUNT" -eq 0 ]; then
    if [ "$DIFF_MODE" -eq 1 ]; then
        echo "no odin or argcv C/C++ sources changed vs $DIFF_BASE"
    else
        echo "no C/C++ sources found under odin/ or argcv/"
    fi
    exit 0
fi

for f in "${FILES[@]}"; do
    echo "formatting ${f#$REPO_ROOT/}"
    "$CLANG_FORMAT" -i --style=file "$f"
done

echo "done. formatted $COUNT file(s)."
