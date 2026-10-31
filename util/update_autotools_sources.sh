#!/usr/bin/env bash
# Convert c-ares/ngtcp2 Autotools file lists to sources.gni.
# Usage: ./util/update_autotools_sources.sh {c-ares|ngtcp2|all} [--check]
set -euo pipefail

usage() {
    printf 'Usage: ./util/update_autotools_sources.sh {c-ares|ngtcp2|all} [--check]\n'
}

fail() {
    printf 'update_autotools_sources: %s\n' "$*" >&2
    exit 1
}

if [ "$#" -eq 1 ] && { [ "$1" = --help ] || [ "$1" = -h ]; }; then
    usage
    exit 0
fi
[ "$#" -ge 1 ] && [ "$#" -le 2 ] || fail "expected a project and optional --check; see --help"
REQUESTED_PROJECT="$1"
case "$REQUESTED_PROJECT" in
    c-ares|ngtcp2) PROJECTS=("$REQUESTED_PROJECT") ;;
    all) PROJECTS=(ngtcp2 c-ares) ;;
    *) fail "unknown project: $REQUESTED_PROJECT" ;;
esac
CHECK=0
if [ "$#" -eq 2 ]; then
    case "$2" in
        --check) CHECK=1 ;;
        --help|-h) usage; exit 0 ;;
        *) fail "unknown option: $2" ;;
    esac
fi

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
cd "$REPO_ROOT"
export LC_ALL=C
TEMP_FILE=
trap 'if [ -n "$TEMP_FILE" ]; then rm -f "$TEMP_FILE"; fi' EXIT
trap 'exit 1' HUP INT TERM

read_list() {
    local makefile="$1" variable="$2"
    awk -v project="$PROJECT" -v variable="$variable" -v directory="${makefile%/*}" '
        function fail(message) {
            printf "update_autotools_sources: %s:%d: %s\n", FILENAME, FNR, message > "/dev/stderr"
            failed = 1
            exit 1
        }
        function trim(value) {
            sub(/^[[:space:]]+/, "", value)
            sub(/[[:space:]]+$/, "", value)
            return value
        }
        function expand(name, value, result, reference, tail) {
            if (name in expanding) fail("recursive variable: " name)
            if (!(name in variables)) fail("undefined variable: " name)
            expanding[name] = 1
            value = variables[name]
            while (match(value, /\$\([[:alnum:]_]+\)|\$\{[[:alnum:]_]+\}/)) {
                result = result substr(value, 1, RSTART - 1)
                reference = substr(value, RSTART + 2, RLENGTH - 3)
                tail = substr(value, RSTART + RLENGTH)
                result = result expand(reference)
                value = tail
            }
            result = result value
            delete expanding[name]
            if (index(result, "$")) fail("unsupported expression in " name ": " result)
            return result
        }
        # Resolve ../ textually; no source/header files are inspected.
        function normalize(path, parts, count, top, i, result) {
            count = split(path, parts, "/")
            top = 0
            for (i = 1; i <= count; ++i) {
                if (parts[i] == "" || parts[i] == ".") continue
                if (parts[i] == ".." && top > 0 && parts[top] != "..") --top
                else parts[++top] = parts[i]
            }
            for (i = 1; i <= top; ++i) result = result (i > 1 ? "/" : "") parts[i]
            return result
        }
        function quote(value, i, character, result) {
            result = "\""
            for (i = 1; i <= length(value); ++i) {
                character = substr(value, i, 1)
                if (character == "\\" || character == "\"") result = result "\\"
                result = result character
            }
            return result "\""
        }
        BEGIN {
            if (project == "ngtcp2") {
                conditions["HAVE_BORINGSSL"] = 1
                count = split("HAVE_QUICTLS HAVE_LIBRESSL HAVE_GNUTLS HAVE_PICOTLS HAVE_WOLFSSL HAVE_OSSL HAVE_CRYPTOTEST HAVE_BORINGSSL_STDCXXLIB", disabled, " ")
                for (i = 1; i <= count; ++i) conditions[disabled[i]] = 0
            }
            depth = 0
            active[0] = 1
        }
        {
            line = $0
            sub(/\r$/, "", line)
            if (line ~ /\\$/) {
                pending = pending substr(line, 1, length(line) - 1) " "
                next
            }
            line = pending line
            pending = ""
            sub(/#.*/, "", line)
            line = trim(line)
            if (line == "") next
            if (line ~ /^if[[:space:]]/) {
                condition = trim(substr(line, 3))
                negated = substr(condition, 1, 1) == "!"
                if (negated) condition = substr(condition, 2)
                if (!(condition in conditions)) fail("unsupported condition: " line)
                enabled = conditions[condition]
                active[depth + 1] = active[depth] && (negated ? !enabled : enabled)
                ++depth
            } else if (line == "else") {
                if (depth == 0) fail("else without if")
                active[depth] = active[depth - 1] && !active[depth]
            } else if (line == "endif") {
                if (depth == 0) fail("endif without if")
                --depth
            } else if (active[depth]) {
                if (line !~ /^[[:alnum:]_]+[[:space:]]*(\+=|=)/) fail("unsupported statement: " line)
                name = line
                sub(/[[:space:]]*(\+=|=).*$/, "", name)
                assignment = trim(substr(line, length(name) + 1))
                append = substr(assignment, 1, 2) == "+="
                value = trim(substr(assignment, append ? 3 : 2))
                if (append && name in variables) value = variables[name] " " value
                variables[name] = value
            }
        }
        END {
            if (failed) exit 1
            if (pending != "") fail("unterminated continuation")
            if (depth != 0) fail("unterminated if")
            count = split(trim(expand(variable)), paths, /[[:space:]]+/)
            for (i = 1; i <= count; ++i) {
                if (paths[i] != "") print "  " quote("//" normalize(directory "/" paths[i])) ","
            }
        }
    ' "$makefile"
}

for PROJECT in "${PROJECTS[@]}"; do
    OUTPUT="$PROJECT/gn/sources.gni"
    TEMP_FILE="$(mktemp "${TMPDIR:-/tmp}/autotools-sources.XXXXXX")"
    {
        printf '# Generated by util/update_autotools_sources.sh %s. Do not edit.\n' "$PROJECT"
        case "$PROJECT" in
            ngtcp2)
                printf '# Core and BoringSSL Makefile.am lists, preserving upstream expansion order.\n\n'
                printf 'ngtcp2_public_headers = [\n'
                read_list ngtcp2/lib/includes/Makefile.am nobase_include_HEADERS
                read_list ngtcp2/crypto/includes/Makefile.am nobase_include_HEADERS
                printf ']\n\nngtcp2_sources = [\n'
                read_list ngtcp2/lib/Makefile.am libngtcp2_la_SOURCES
                read_list ngtcp2/crypto/boringssl/Makefile.am libngtcp2_crypto_boringssl_a_SOURCES
                printf ']\n'
                ;;
            c-ares)
                printf '# Autotools file lists, preserving upstream expansion order.\n'
                printf '# Platform-generated ares_config.h is added separately by the GN adapter.\n\n'
                printf 'cares_public_headers = [\n'
                read_list c-ares/include/Makefile.am include_HEADERS
                printf ']\n\ncares_sources = [\n'
                read_list c-ares/src/lib/Makefile.inc CSOURCES
                read_list c-ares/src/lib/Makefile.inc HHEADERS
                printf ']\n'
                ;;
        esac
    } > "$TEMP_FILE"

    if cmp -s "$TEMP_FILE" "$OUTPUT"; then
        printf '%s is up to date.\n' "$OUTPUT"
    elif [ "$CHECK" -eq 1 ]; then
        ACTUAL_FILE="$OUTPUT"
        if [ ! -e "$ACTUAL_FILE" ]; then
            ACTUAL_FILE=/dev/null
        fi
        diff -u -L "$OUTPUT" -L "$OUTPUT (generated)" "$ACTUAL_FILE" "$TEMP_FILE" || [ "$?" -eq 1 ]
        fail "Run ./util/update_autotools_sources.sh $PROJECT to update the file."
    else
        mkdir -p "$(dirname "$OUTPUT")"
        chmod 644 "$TEMP_FILE"
        mv -f "$TEMP_FILE" "$OUTPUT"
        printf 'Updated %s.\n' "$OUTPUT"
    fi
    rm -f "$TEMP_FILE"
    TEMP_FILE=
done
