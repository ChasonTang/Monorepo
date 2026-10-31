#!/bin/bash
# Record the link interface of a shared library without tracking code layout.
# Consumers link against the library but depend on this TOC with Ninja restat.
# This is not a C/C++ ABI checker; headers remain compiler dependencies.
#
# Uses the pinned LLVM inspectors and the host's Bash 3.2 / POSIX awk utilities.
# Like Chromium's solink wrappers and Android Soong's scripts/toc.sh, retain
# library identity and symbols, and leave unchanged interfaces untouched.
set -euo pipefail
export LC_ALL=C

fail() {
    printf 'solink_toc.sh: %s\n' "$*" >&2
    exit 1
}

format= tool_dir= library= toc= depfile=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --format=*|--tool-dir=*|--library=*|--toc=*|--depfile=*)
            option=${1%%=*}
            value=${1#*=}
            shift
            ;;
        --format|--tool-dir|--library|--toc|--depfile)
            [ "$#" -ge 2 ] || fail "missing value for $1"
            option=$1
            value=$2
            shift 2
            ;;
        -h|--help)
            printf 'Usage: %s --format=macho|elf --tool-dir=DIR --library=FILE --toc=FILE --depfile=FILE\n' "$0"
            exit 0
            ;;
        *) fail "unknown argument: $1" ;;
    esac
    [ -n "$value" ] || fail "empty value for $option"
    case "$option" in
        --format) format=$value ;;
        --tool-dir) tool_dir=$value ;;
        --library) library=$value ;;
        --toc) toc=$value ;;
        --depfile) depfile=$value ;;
    esac
done
[ -n "$format" ] && [ -n "$tool_dir" ] && [ -n "$library" ] &&
    [ -n "$toc" ] && [ -n "$depfile" ] || fail "all five options are required"

case "$format" in
    macho)
        inspector=${tool_dir%/}/llvm-objdump
        flags=(--macho --private-headers --exports-trie)
        ;;
    elf)
        inspector=${tool_dir%/}/llvm-readelf
        flags=(--wide --dynamic-table --dyn-syms)
        ;;
    *) fail "unsupported format: $format" ;;
esac

# Keep temporary files beside the TOC so replacement is atomic, even when the
# build directory is on a different filesystem from the system temp directory.
work_dir=$(mktemp -d "${toc}.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT
trap 'exit 1' HUP INT TERM

# Run separately from parsing so an inspector failure cannot produce a TOC.
"$inspector" "${flags[@]}" "$library" > "$work_dir/raw"
: > "$work_dir/identity"
: > "$work_dir/symbols"
# Pass paths through the environment: awk -v interprets backslash escapes.
TOC_WORK_DIR="$work_dir" awk -v format="$format" '
    function fail(message) {
        print "solink_toc.sh: " message > "/dev/stderr"
        failed = 1
        exit 1
    }

    function macho() {
        if ($0 == "Exports trie:") {
            in_exports = 1
        } else if (in_exports) {
            if ($0 == "") return
            if ($0 !~ /^(0x[0-9A-Fa-f]+[[:space:]]|\[re-export\] )/)
                fail("unexpected Mach-O export: " $0)
            # Preserve weak/TLS/re-export flags and absolute symbol values.
            # Ordinary addresses and resolver locations are implementation.
            if ($0 !~ /\[([^]]*[^[:alnum:]_])?absolute([^[:alnum:]_]|$)/)
                sub(/^0x[0-9A-Fa-f]+[[:space:]]+/, "")
            gsub(/resolver=0x[0-9A-Fa-f]+/, "resolver")
            print > symbols
        } else if ($0 ~ /^Load command /) {
            command = ""
        } else if ($0 ~ /^cmd /) {
            command = $2
            if (command == "LC_ID_DYLIB") dylib_ids++
            # Whole-library re-exports are not expanded in the export trie.
            # Conservatively propagate every relink through such libraries.
            if (command == "LC_REEXPORT_DYLIB") {
                print command > identity
                print "force" > force_file
            }
        } else if (command == "LC_ID_DYLIB" || command == "LC_REEXPORT_DYLIB") {
            if ($0 ~ /^(name |current version |compatibility version )/) {
                sub(/ \(offset [0-9]+\)$/, "")
                print > identity
                if (command == "LC_ID_DYLIB" && !($1 in id_fields)) {
                    id_fields[$1] = 1
                    id_field_count++
                }
            }
        }
    }

    function elf(    line, value, size, kind, binding, visibility, section, name, i) {
        if (index($0, "(SONAME)")) {
            line = substr($0, index($0, "(SONAME)") + 8)
            sub(/^[[:space:]]+/, "", line)
            print line > identity
            sonames++
        } else if ($0 ~ /^Symbol table \047[.]dynsym\047/) {
            if (seen_table || $0 !~ /^Symbol table \047[.]dynsym\047 contains [0-9]+ entries:$/)
                fail("unexpected ELF dynamic symbol table: " $0)
            seen_table = 1
            expected_symbols = $5 + 0
        } else if (seen_table && $0 ~ /^[0-9]+:/) {
            seen_symbols++
            if (NF == 7 && $1 == "0:") return # The unnamed null symbol.
            if (NF < 8) fail("unexpected ELF dynamic symbol: " $0)
            value = $2
            size = $3
            kind = $4
            binding = $5
            visibility = $6
            section = $7
            if (binding == "LOCAL") return
            # Keep the entire name, including symbol version annotations.
            name = $0
            for (i = 0; i < 7; i++) sub(/^[^[:space:]]+[[:space:]]*/, "", name)
            # Keep imports, exports and data/TLS sizes (e.g. copy relocations).
            # Function sizes, ordinary addresses and section numbers may vary.
            if (kind == "FUNC" || kind == "IFUNC" || kind == "GNU_IFUNC") size = "-"
            if (section ~ /^[0-9]+$/) section = "DEFINED"
            if (section != "ABS") value = "-"
            print name, kind, binding, visibility, section, size, value > symbols
        }
    }

    BEGIN {
        identity = ENVIRON["TOC_WORK_DIR"] "/identity"
        symbols = ENVIRON["TOC_WORK_DIR"] "/symbols"
        force_file = ENVIRON["TOC_WORK_DIR"] "/force"
    }
    {
        sub(/^[[:space:]]+/, "")
        sub(/[[:space:]]+$/, "")
        if (format == "macho") macho()
        else elf()
    }
    END {
        if (failed) exit 1
        if (format == "macho") {
            # Solink emits thin MH_DYLIB files; require one complete identity.
            if (dylib_ids != 1 || id_field_count != 3 || !in_exports)
                fail("expected one complete Mach-O dylib identity and exports trie")
        } else if (sonames != 1 || !seen_table || expected_symbols != seen_symbols) {
            fail("expected one ELF SONAME and a complete dynamic symbol table")
        }
    }
' < "$work_dir/raw"

cat "$work_dir/identity" > "$work_dir/toc"
sort "$work_dir/symbols" >> "$work_dir/toc"

depfile_path() {
    local escaped=$1
    escaped=${escaped//\\/\\\\}
    escaped=${escaped//\$/\$\$}
    escaped=${escaped// /\\ }
    escaped=${escaped//#/\\#}
    printf '%s' "$escaped"
}

# Track the script and inspector too. Preserve symlink names as invoked so
# replacing the toolchain or editing this script regenerates the interface.
script_dir=$(cd "$(dirname "$0")" && pwd -L)
printf '%s: %s %s\n' "$(depfile_path "$library")" \
    "$(depfile_path "$script_dir/$(basename "$0")")" \
    "$(depfile_path "$inspector")" > "$depfile"

if [ -f "$work_dir/force" ] || ! cmp -s "$toc" "$work_dir/toc"; then
    mv -f "$work_dir/toc" "$toc"
fi
