#!/usr/bin/env bash
# Translate ICU's source manifests and configured Make variables into GN and ASM.
set -euo pipefail
export LC_ALL=C

fail() {
    printf 'icu_gn: %s\n' "$*" >&2
    exit 1
}

usage() {
    printf 'Usage: %s sources | config --build-dir DIR --output FILE --data-output FILE\n' "$0"
}

CALLER_DIR="$(pwd -P)"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
cd "$REPO_ROOT"
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/icu-gn.XXXXXX")"
trap 'rm -rf "$WORK_DIR"' EXIT
trap 'exit 1' HUP INT TERM

gn_quote() {
    local value="$1"
    value="${value//\\/\\\\}"
    value="${value//\"/\\\"}"
    value="${value//\$/\\\$}"
    value="${value//$'\t'/\\t}"
    value="${value//$'\r'/\\r}"
    value="${value//$'\n'/\\n}"
    printf '"%s"' "$value"
}

gn_list() {
    local name="$1" prefix="${2:-}" value
    printf '%s = [\n' "$name"
    awk '!seen[$0]++' | while IFS= read -r value; do
        printf '  '
        gn_quote "$prefix$value"
        printf ',\n'
    done
    printf ']\n'
}

source_lists() {
    local module directory header_glob path
    local -a headers
    printf '# Sources and installed headers selected by upstream Makefile.in.\n\n'
    for module in common i18n io; do
        directory="icu/source/$module"
        header_glob="$(awk '
            /^SOURCES[[:space:]]*=[[:space:]]*\$\(shell cat \$\(srcdir\)\/sources\.txt\)[[:space:]]*$/ { sources = 1 }
            /^HEADERS[[:space:]]*=[[:space:]]*\$\(srcdir\)\/[^[:space:]$]+[[:space:]]*$/ {
                sub(/^HEADERS[[:space:]]*=[[:space:]]*\$\(srcdir\)\//, "")
                sub(/[[:space:]]*$/, "")
                headers = $0
            }
            END { if (!sources || !headers) exit 1; print headers }
        ' "$directory/Makefile.in")" || fail "$module: upstream source/header selection changed"
        # Expand the upstream HEADERS glob, with the source directory quoted.
        headers=("$directory"/$header_glob)
        for path in "${headers[@]}"; do
            [ -f "$path" ] || fail "$module: missing public header: $path"
        done
        printf '%s\n' "${headers[@]}" | sort > "$WORK_DIR/public"
        awk -v directory="$directory" '
            { for (i = 1; i <= NF; ++i) {
                if (seen[$i]++ || $i !~ /\.(c|cpp)$/) {
                    print "icu_gn: duplicate or unsupported source: " $i > "/dev/stderr"
                    exit 1
                }
                print directory "/" $i
            } }
        ' "$directory/sources.txt" > "$WORK_DIR/sources"
        while IFS= read -r path; do
            [ -f "$path" ] || fail "$module: missing source: $path"
        done < "$WORK_DIR/sources"
        find "$directory" -type f -name '*.h' | sort > "$WORK_DIR/headers"
        awk 'NR == FNR { public[$0] = 1; next } !($0 in public)' \
            "$WORK_DIR/public" "$WORK_DIR/headers" >> "$WORK_DIR/sources"
        gn_list "icu_${module}_public_headers" // < "$WORK_DIR/public"
        printf '\n'
        gn_list "icu_${module}_sources" // < "$WORK_DIR/sources"
        printf '\n'
    done
}

# Split quoted compiler flags without evaluating shell substitutions. Inside
# double quotes, preserve backslashes except before a quote or backslash.
flag_words() {
    awk -v prefix="${1:-}" '
        function fail(message) { print "icu_gn: " message > "/dev/stderr"; exit 1 }
        function emit() {
            if (!active) return
            if (prefix != "" && (index(word, prefix) != 1 || length(word) == length(prefix)))
                fail("unsupported flag: " word)
            print substr(word, length(prefix) + 1)
            word = ""; active = 0
        }
        { text = text $0 "\n" }
        END {
            for (i = 1; i <= length(text); ++i) {
                c = substr(text, i, 1)
                if (quote == "\047") {
                    if (c == quote) quote = ""
                    else word = word c
                } else if (c == "\\") {
                    if (++i >= length(text)) fail("unterminated flag escape")
                    c = substr(text, i, 1)
                    if (quote == "\"" && c != "\\" && c != "\"") word = word "\\"
                    word = word c; active = 1
                } else if (quote == "\"") {
                    if (c == quote) quote = ""
                    else word = word c
                } else if (c == "\047" || c == "\"") {
                    quote = c; active = 1
                } else if (c ~ /[[:space:]]/) emit()
                else { word = word c; active = 1 }
            }
            if (quote != "") fail("unterminated flag quote")
            emit()
        }
    '
}

make_value() {
    awk -v name="ICU_GN_$1=" '
        index($0, name) == 1 { ++count; print substr($0, length(name) + 1) }
        END { if (count != 1) exit 1 }
    ' "$WORK_DIR/make-values" || fail "missing or duplicate make variable: $1"
}

# Wrap the release .dat using ICU's own assembly header/footer and .incbin.
# The assembler must search the archive's directory via -I.
assemble_data() {
    local assembly="$1" entry="$2" data="$3"
    [[ "$entry" =~ ^[A-Za-z_][A-Za-z_0-9]*$ ]] || fail 'invalid data entry point'

    # Scan the C initializer, respecting comments and concatenated string literals.
    # Reject unknown escapes/formats so upstream changes cannot silently corrupt ASM.
    ICU_ASSEMBLY="$assembly" ICU_ENTRY_POINT="$entry" ICU_DATA_FILE="${data##*/}" awk '
        function fail(message) { print "icu_gn: " message > "/dev/stderr"; exit 1 }
        function string_literal(    result, c) {
            ++position
            while (position <= length(source)) {
                c = substr(source, position++, 1)
                if (c == "\"") return result
                if (c == "\n") fail("newline inside C string")
                if (c == "\\") {
                    c = substr(source, position++, 1)
                    if (c == "n") c = "\n"
                    else if (c == "t") c = "\t"
                    else if (c == "r") c = "\r"
                    else if (c != "\\" && c != "\"" && c != "\047" && c != "?")
                        fail("unsupported C string escape: " c)
                }
                result = result c
            }
            fail("unterminated C string")
        }
        function next_token(    rest, c, ending) {
            while (position <= length(source)) {
                rest = substr(source, position)
                c = substr(rest, 1, 1)
                if (c ~ /[[:space:]]/) ++position
                else if (substr(rest, 1, 2) == "/*") {
                    ending = index(substr(rest, 3), "*/")
                    if (!ending) fail("unterminated C comment")
                    position += ending + 3
                } else if (substr(rest, 1, 2) == "//") {
                    ending = index(rest, "\n")
                    position += ending ? ending : length(rest)
                } else if (c == "\"") { value = string_literal(); return "string" }
                else if (c ~ /[{},]/) { ++position; value = c; return c }
                else if (match(rest, /^[A-Za-z_][A-Za-z_0-9]*/)) {
                    value = substr(rest, 1, RLENGTH); position += RLENGTH; return "identifier"
                } else fail("unsupported initializer token: " c)
            }
            fail("incomplete assembly initializer")
        }
        function expand(text,    result, i, c) {
            for (i = 1; i <= length(text); ++i) {
                c = substr(text, i, 1)
                if (c == "%") {
                    c = substr(text, ++i, 1)
                    if (c == "s") c = ENVIRON["ICU_ENTRY_POINT"]
                    else if (c != "%") fail("unsupported assembly format placeholder")
                }
                result = result c
            }
            return result
        }
        function quoted_path(path,    result, i, c) {
            result = "\""
            for (i = 1; i <= length(path); ++i) {
                c = substr(path, i, 1)
                if (c == "\\" || c == "\"") c = "\\" c
                else if (c == "\n") c = "\\n"
                else if (c == "\r") c = "\\r"
                else if (c == "\t") c = "\\t"
                result = result c
            }
            return result "\""
        }
        { source = source $0 "\n" }
        END {
            if (!match(source, /assemblyHeader[[:space:]]*\[[[:space:]]*\][[:space:]]*=[[:space:]]*\{/))
                fail("upstream assembly table not found")
            position = RSTART + RLENGTH
            while (next_token() == "{") {
                for (field = 1; field <= 4; ++field) {
                    fields[field] = ""; count = 0
                    while ((token = next_token()) == "string") { fields[field] = fields[field] value; ++count }
                    if (!count || token != ",") fail("unsupported assembly string field")
                }
                if (next_token() != "identifier") fail("missing hex format")
                hex_format = value
                if (next_token() != "}") fail("unsupported assembly initializer")
                if (fields[1] == ENVIRON["ICU_ASSEMBLY"]) {
                    if (hex_format != "HEX_0X") fail("unsupported hex format: " hex_format)
                    print "/* Generated by util/update_autotools_config.sh icu. Do not edit. */"
                    printf "%s.incbin %s\n%s", expand(fields[2]), quoted_path(ENVIRON["ICU_DATA_FILE"]), expand(fields[4])
                    exit
                }
                if (next_token() != ",") break
            }
            fail("upstream assembly template not found: " ENVIRON["ICU_ASSEMBLY"])
        }
    ' icu/source/tools/toolutil/pkg_genc.cpp
}

configuration() {
    local build="$1" variable module flags assembly entry data
    build="$(cd "$build" && pwd -P)"
    # Let GNU make expand the upstream platform fragment and cross-build data.
    {
        printf 'top_srcdir := %s/icu/source\ntop_builddir := .\ninclude icudefs.mk\n' "$REPO_ROOT"
        for variable in DEFS CPPFLAGS LIBCPPFLAGS THREADSCFLAGS THREADSCXXFLAGS \
                DEFAULT_LIBS GENCCODE_ASSEMBLY ICUDATA_ENTRY_POINT ICUDATA_PLATFORM_NAME; do
            printf '$(info ICU_GN_%s=$(%s))\n' "$variable" "$variable"
        done
        printf '.PHONY: icu-gn-variables\nicu-gn-variables: ; @:\n'
    } > "$build/gn-variables.mk"
    (cd "$build" && /usr/bin/make -s --no-print-directory -f gn-variables.mk icu-gn-variables) \
        > "$WORK_DIR/make-values"

    printf '# Generated by util/update_autotools_config.sh icu. Do not edit.\n'
    printf '# Expanded from configure output and the selected upstream platform fragment.\n\n'
    flags="$(make_value DEFS)"
    flags+=" $(make_value CPPFLAGS)"
    flags+=" $(make_value LIBCPPFLAGS)"
    printf '%s\n' "$flags" | flag_words -D | gn_list icu_private_defines
    make_value THREADSCFLAGS | flag_words | gn_list icu_thread_cflags
    make_value THREADSCXXFLAGS | flag_words | gn_list icu_thread_cflags_cc
    make_value DEFAULT_LIBS | flag_words -l | gn_list icu_system_libs
    for module in common i18n io; do
        flags="$(awk '
            /^DEFS[[:space:]]*\+=[[:space:]]*/ {
                sub(/^DEFS[[:space:]]*\+=[[:space:]]*/, ""); print; ++count
            }
            END { if (!count) exit 1 }
        ' "icu/source/$module/Makefile.in")" || fail "$module: no implementation definitions"
        printf '%s\n' "$flags" | flag_words -D | gn_list "icu_${module}_defines"
    done
    assembly="$(make_value GENCCODE_ASSEMBLY | flag_words | awk '
        NR == 1 && $0 != "-a" { failed = 1 }
        NR == 2 { assembly = $0 }
        END { if (failed || NR != 2) exit 1; print assembly }
    ')" || fail 'unsupported ICU data assembler'
    entry="$(make_value ICUDATA_ENTRY_POINT | awk '{$1=$1; print}')_dat"
    data="icu/source/data/in/$(make_value ICUDATA_PLATFORM_NAME | awk '{$1=$1; print}').dat"
    [ -f "$data" ] || fail "release archive lacks data for this target: $data"
    printf 'icu_data_archive = '; gn_quote "//$data"; printf '\n'
    assemble_data "$assembly" "$entry" "$data" > "$WORK_DIR/icudata.S"
}

[ "$#" -ge 1 ] || { usage >&2; exit 1; }
COMMAND="$1"
shift
case "$COMMAND" in
    sources)
        [ "$#" -eq 0 ] || fail 'sources takes no arguments'
        source_lists
        ;;
    config)
        BUILD_DIR= OUTPUT_FILE= DATA_OUTPUT=
        while [ "$#" -gt 0 ]; do
            [ "$#" -ge 2 ] || fail "missing value for $1"
            case "$1" in
                --build-dir) BUILD_DIR="$2" ;;
                --output) OUTPUT_FILE="$2" ;;
                --data-output) DATA_OUTPUT="$2" ;;
                *) fail "unknown option: $1" ;;
            esac
            shift 2
        done
        [ -n "$BUILD_DIR" ] && [ -n "$OUTPUT_FILE" ] && [ -n "$DATA_OUTPUT" ] || \
            fail 'config needs --build-dir, --output and --data-output'
        case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$CALLER_DIR/$BUILD_DIR" ;; esac
        case "$OUTPUT_FILE" in /*) ;; *) OUTPUT_FILE="$CALLER_DIR/$OUTPUT_FILE" ;; esac
        case "$DATA_OUTPUT" in /*) ;; *) DATA_OUTPUT="$CALLER_DIR/$DATA_OUTPUT" ;; esac
        configuration "$BUILD_DIR" > "$WORK_DIR/config.gni"
        cp "$WORK_DIR/config.gni" "$OUTPUT_FILE"
        cp "$WORK_DIR/icudata.S" "$DATA_OUTPUT"
        ;;
    --help|-h) usage ;;
    *) fail "unknown command: $COMMAND" ;;
esac
