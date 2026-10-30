#!/usr/bin/env bash
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

TIDY_WORK=$(mktemp -d "${TMPDIR:-/tmp}/monorepo-tidy.XXXXXX")
trap 'rm -rf "$TIDY_WORK"' EXIT
STAGED=false
CONFIG_CHANGED=false
if [ "${1:-}" = --staged ]; then
    STAGED=true
    git diff --cached --name-only --no-renames -z -- argcv odin ipsw > "$TIDY_WORK/paths"
    : > "$TIDY_WORK/changed"
    : > "$TIDY_WORK/existing"
    while IFS= read -r -d '' path; do
        case "$path" in
            *.c|*.cc|*.cpp|*.h|*.hpp)
                printf '%s\0' "$REPO_ROOT/$path" >> "$TIDY_WORK/changed"
                if [ -e "$path" ]; then
                    printf '%s\0' "$REPO_ROOT/$path" >> "$TIDY_WORK/existing"
                fi
                ;;
        esac
    done < "$TIDY_WORK/paths"
    if [ ! -s "$TIDY_WORK/changed" ]; then
        echo "tidy: no staged project source/header changes; skipping"
        exit 0
    fi
    git diff --cached --name-only -z -- .gn '*.gn' '*.gni' > "$TIDY_WORK/configs"
    git diff --name-only -z -- .gn '*.gn' '*.gni' >> "$TIDY_WORK/configs"
    [ ! -s "$TIDY_WORK/configs" ] || CONFIG_CHANGED=true
fi

GN="$REPO_ROOT/tool/gn"
NINJA="$REPO_ROOT/tool/ninja"
JQ="$REPO_ROOT/tool/jq"
CLANG_TIDY="$REPO_ROOT/tool/clang/bin/clang-tidy"
RUNNER="$REPO_ROOT/tool/clang/bin/run-clang-tidy"
REQUIRED=("$GN" "$CLANG_TIDY" "$RUNNER")
if "$STAGED"; then
    REQUIRED+=("$NINJA" "$JQ")
fi
for tool in "${REQUIRED[@]}"; do
    [ -x "$tool" ] || fail "$tool is not executable; run ./sync_tools.sh first"
done

BUILD_DIR="${BUILD_DIR:-out}"
echo "tidy: regenerating $BUILD_DIR/compile_commands.json"
"$GN" gen "$BUILD_DIR" --check \
    --add-export-compile-commands='//argcv/*' \
    --add-export-compile-commands='//odin/*' \
    --add-export-compile-commands='//ipsw/*'
BUILD_DIR="$(cd "$BUILD_DIR" && pwd -P)"

escape_regex() {
    printf '%s' "$1" | sed 's/[][(){}.^$*+?|\\]/\\&/g'
}

FILE_FILTER='.*'
if "$STAGED"; then
    # Keep jq helpers inline; the temporary module is removed on exit.
    cat > "$TIDY_WORK/tidy.jq" <<'JQ'
# Paths in Ninja's database and dependency log are relative to their build directory.
def tidy_path($base):
  (if startswith("/") then . else $base + "/" + . end)
  | split("/")
  | reduce .[] as $part ([];
      if $part == "" or $part == "." then .
      elif $part == ".." then .[:-1]
      else . + [$part] end)
  | "/" + join("/");

def tidy_units($root; $build):
  map(.directory as $directory
      | .file |= tidy_path($directory)
      | select(.file | startswith($root + "/"))
      | select(.file | ltrimstr($root + "/") | test("^(argcv|odin|ipsw)/.*\\.(c|cc|cpp)$"))
      | .output |= tidy_path($directory)
      | if (.output | startswith($build + "/")) then .
        else error("object output is outside BUILD_DIR: " + .output) end
      | if any([.file, .output][]; test("[\r\n]")) then
          error("Ninja dependency output cannot represent a newline in a path")
        else . end
      | .target = (.output | ltrimstr($build + "/")))
  | unique_by(.output)
  | if length == 0 then error("no project compilation units in the selected build") else . end;

def tidy_dependencies($build):
  split("\n\n") | map(select(length > 0)
    | split("\n") as $lines
    | (($lines[0] | capture("^(?<output>.+): #deps (?<count>[0-9]+), deps mtime [0-9]+ \\(VALID\\)$"))
       // error("unusable refreshed Ninja dependency record: " + $lines[0])) as $header
    | $lines[1:] as $paths
    | if ($paths | length) != ($header.count | tonumber)
         or any($paths[]; startswith("    ") | not) then
        error("incomplete refreshed Ninja dependency record: " + $header.output)
      else {key: ($header.output | tidy_path($build)),
            value: ($paths | map(.[4:] | tidy_path($build)))} end)
  | from_entries;

def tidy_selection($build; $units; $changed; $existing; $built; $explain; $config_changed):
  tidy_dependencies($build) as $deps
  | ($changed | split("\u0000") | map(select(length > 0))) as $changed
  | ($existing | split("\u0000") | map(select(length > 0))) as $existing
  | ($built | split("\n")) as $commands
  | ($explain | split("\n") | map(
      select(startswith("ninja explain: command line changed for "))
      | ltrimstr("ninja explain: command line changed for ") | tidy_path($build))) as $new_commands
  | [$units[] | . as $unit
     | ($deps[$unit.output] // error("no Ninja dependency record for " + $unit.output)) as $includes
     | if ($includes | index($unit.file)) == null then
         error("Ninja dependencies do not include source " + $unit.file)
       else . end
     | {file: .file,
        covered: ([$unit.file] + $includes | map(. as $path | select($changed | index($path))) | unique),
        # GN changes may affect sources that are not themselves staged.
        rebuilt: (($config_changed and (($commands | index($unit.command)) != null))
                  or ($new_commands | index($unit.output)) != null)}] as $matches
  | ($matches | map(.covered[]) | unique) as $covered
  | ($existing - $covered) as $uncovered
  | if ($uncovered | length) > 0 then
      error("no compilation unit covers staged path(s): " + ($uncovered | join(", ")))
    else {files: ([$matches[] | select((.covered | length) > 0 or .rebuilt) | .file] | unique),
          deleted: ($changed - $covered)} end;
JQ

    # Ninja supplies explicit object outputs, so no shell-command parsing is needed.
    "$NINJA" -C "$BUILD_DIR" -t compdb > "$TIDY_WORK/compdb.json"
    "$JQ" -L "$TIDY_WORK" --arg root "$REPO_ROOT" --arg build "$BUILD_DIR" \
        'include "tidy"; tidy_units($root; $build)' \
        "$TIDY_WORK/compdb.json" > "$TIDY_WORK/units.json"
    "$JQ" -j '.[] | .target, "\u0000"' "$TIDY_WORK/units.json" > "$TIDY_WORK/targets"
    TARGETS=()
    while IFS= read -r -d '' target; do
        TARGETS+=("$target")
    done < "$TIDY_WORK/targets"

    echo "tidy: updating ${#TARGETS[@]} project object(s), including test objects"
    NINJA_ARGS=(-C "$BUILD_DIR" -v -d explain)
    [ -z "${JOBS:-}" ] || NINJA_ARGS+=(-j "$JOBS")
    # Retain the commands actually rebuilt, and reasons for changed compiler flags.
    if NINJA_STATUS='' "$NINJA" "${NINJA_ARGS[@]}" "${TARGETS[@]}" \
        2> "$TIDY_WORK/explain" | tee "$TIDY_WORK/built"; then
        :
    else
        status=$?
        cat "$TIDY_WORK/explain" >&2
        exit "$status"
    fi
    "$NINJA" -C "$BUILD_DIR" -t deps "${TARGETS[@]}" > "$TIDY_WORK/deps"
    "$JQ" -Rs -L "$TIDY_WORK" --arg build "$BUILD_DIR" \
        --slurpfile units "$TIDY_WORK/units.json" \
        --rawfile changed "$TIDY_WORK/changed" --rawfile existing "$TIDY_WORK/existing" \
        --rawfile built "$TIDY_WORK/built" --rawfile explain "$TIDY_WORK/explain" \
        --argjson config_changed "$CONFIG_CHANGED" \
        'include "tidy";
         tidy_selection($build; $units[0]; $changed; $existing; $built; $explain; $config_changed)' \
        "$TIDY_WORK/deps" > "$TIDY_WORK/selected.json"
    "$JQ" -r '.deleted[] | "tidy: deleted path has no remaining compiled includer: " + .' \
        "$TIDY_WORK/selected.json"
    COUNT=$("$JQ" '.files | length' "$TIDY_WORK/selected.json")
    if [ "$COUNT" -eq 0 ]; then
        echo "tidy: no remaining compilation units affected by staged deletions; skipping"
        exit 0
    fi
    echo "tidy: selected $COUNT source file(s)"
    "$JQ" -j '.files[] | ., "\u0000"' "$TIDY_WORK/selected.json" > "$TIDY_WORK/files"
    FILE_FILTER=''
    while IFS= read -r -d '' source; do
        printf '  %s\n' "${source#"$REPO_ROOT/"}"
        FILE_FILTER="$FILE_FILTER|\A$(escape_regex "$source")\Z"
    done < "$TIDY_WORK/files"
    FILE_FILTER="${FILE_FILTER#|}"
fi

SOURCE_FILTER="(?s)\A$(escape_regex "$REPO_ROOT")/(argcv|odin|ipsw)/.*\.(c|cc|cpp)\Z"
python3 -u "$RUNNER" -p "$BUILD_DIR" \
    -clang-tidy-binary "$CLANG_TIDY" -source-filter "$SOURCE_FILTER" \
    -j "${JOBS:-0}" -quiet "$FILE_FILTER"
