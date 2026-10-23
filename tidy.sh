#!/usr/bin/env bash
# Select project files; LLVM's runner reads the database and runs the analysis.
# Usage: ./tidy.sh [--staged]
# JOBS sets parallelism (default: CPU count); BUILD_DIR defaults to out.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd -P)"
cd "$REPO_ROOT"

fail() {
    printf 'tidy: %s\n' "$*" >&2
    exit 1
}

if [ "$#" -gt 1 ] || { [ "$#" -eq 1 ] && [ "$1" != --staged ]; }; then
    fail "usage: ./tidy.sh [--staged]"
fi
if [[ -n "${JOBS:-}" && ! "$JOBS" =~ ^0*[1-9][0-9]*$ ]]; then
    fail "JOBS must be a positive integer"
fi

escape_regex() {
    printf '%s' "$1" | sed 's/[][(){}.^$*+?|\\]/\\&/g'
}

# Runner file arguments are regexes, not literal paths. Normalize each candidate
# before checking scope, then anchor and escape it for an exact match.
source_pattern() {
    local file="$1" directory
    [ -f "$file" ] || return 1
    directory="$(cd "$(dirname "$file")" && pwd -P)" || return 1
    file="$directory/${file##*/}"
    case "$file" in
        "$REPO_ROOT"/argcv/*|"$REPO_ROOT"/odin/*|"$REPO_ROOT"/ipsw/*) ;;
        *) return 1 ;;
    esac
    case "$file" in
        *.c|*.cc|*.cpp) ;;
        *) return 1 ;;
    esac
    printf '|\\A%s\\Z' "$(escape_regex "$file")"
}

staged_patterns() {
    local file ext
    git diff --cached --name-only -z --diff-filter=ACMRTUB -- argcv odin ipsw |
    while IFS= read -r -d '' file; do
        case "$file" in
            *.c|*.cc|*.cpp) source_pattern "$file" || : ;;
            *.h|*.hpp)
                for ext in c cc cpp; do
                    source_pattern "${file%.*}.$ext" || :
                done
                ;;
        esac
    done
}

FILE_FILTER='.*'
if [ "${1:-}" = --staged ]; then
    # Capture only the regex, preserving NUL-delimited Git filenames and the
    # pipeline's exit status. No JSON parsing or analysis scheduling lives here.
    FILE_FILTER="$(staged_patterns)"
    if [ -z "$FILE_FILTER" ]; then
        echo "no staged project sources or same-basename header siblings found"
        exit 0
    fi
fi
FILE_FILTER="${FILE_FILTER#|}"

GN="$REPO_ROOT/tool/gn"
CLANG_TIDY="$REPO_ROOT/tool/clang/bin/clang-tidy"
RUNNER="$REPO_ROOT/tool/clang/bin/run-clang-tidy"
for tool in "$GN" "$CLANG_TIDY" "$RUNNER"; do
    [ -x "$tool" ] || fail "$tool is not executable; run ./sync_tools.sh first"
done

BUILD_DIR="${BUILD_DIR:-out}"
case "$BUILD_DIR" in
    /*) ;;
    *) BUILD_DIR="$REPO_ROOT/$BUILD_DIR" ;;
esac
echo "regenerating $BUILD_DIR/compile_commands.json"
"$GN" gen "$BUILD_DIR" --check \
    --add-export-compile-commands='//argcv/*' \
    --add-export-compile-commands='//odin/*' \
    --add-export-compile-commands='//ipsw/*'

# Scope the runner's database entries even when the database contains dependencies.
SOURCE_FILTER="(?s)\\A$(escape_regex "$REPO_ROOT")/(argcv|odin|ipsw)/.*\\.(c|cc|cpp)\\Z"
# Unbuffer Python output so each completed file is visible through pipes and CI.
exec python3 -u "$RUNNER" -p "$BUILD_DIR" \
    -clang-tidy-binary "$CLANG_TIDY" \
    -source-filter "$SOURCE_FILTER" \
    -j "${JOBS:-0}" -quiet "$FILE_FILTER"
