#!/usr/bin/env bash
# Generate ngtcp2/c-ares headers for six targets with the bundled Clang and SDKs.
# Usage: ./util/update_autotools_config.sh {c-ares|ngtcp2|all} [--check]
set -euo pipefail

usage() {
    printf 'Usage: ./util/update_autotools_config.sh {c-ares|ngtcp2|all} [--check]\n'
}

fail() {
    printf 'update_autotools_config: %s\n' "$*" >&2
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
CLANG_BIN="$REPO_ROOT/tool/clang/bin"
# Keep triples and deployment versions in sync with build/toolchain/BUILD.gn.
TARGETS=(
    'mac_arm64|arm64-apple-macos11.0|MacOSX.sdk'
    'mac_x64|x86_64-apple-macos10.15|MacOSX.sdk'
    'ios_arm64|arm64-apple-ios15.0|iPhoneOS.sdk'
    'ios_sim_arm64|arm64-apple-ios15.0-simulator|iPhoneSimulator.sdk'
    'ios_sim_x64|x86_64-apple-ios15.0-simulator|iPhoneSimulator.sdk'
    'linux_x64|x86_64-unknown-linux-gnu|debian_bullseye_amd64-sysroot'
)
TARGET_NAMES=()
for target in "${TARGETS[@]}"; do
    TARGET_NAMES+=("${target%%|*}")
    sdk="${target##*|}"
    [ -d "build/sdk/$sdk" ] || fail "missing build/sdk/$sdk; see README.md"
done
for tool in clang clang++ ld64.lld ld.lld llvm-ar llvm-ranlib llvm-nm llvm-strip; do
    [ -x "$CLANG_BIN/$tool" ] || fail "missing tool/clang/bin/$tool; run ./sync_tools.sh"
done

# Do not inherit SDKROOT, flags, CONFIG_SITE or cached probes from the shell.
CLEAN_ENV=(env -i HOME="$HOME" LC_ALL=C
    "PATH=$CLANG_BIN:/usr/bin:/bin:/usr/sbin:/sbin"
    CONFIG_SITE=/dev/null PKG_CONFIG=/usr/bin/false)
GENERATED=()
OUTPUTS=()
TEMP_FILE=
trap 'if [ -n "$TEMP_FILE" ]; then rm -f "$TEMP_FILE"; fi' EXIT
trap 'exit 1' HUP INT TERM

configure_target() {
    local name="$1" triple="$2" sdk="$3"
    local work="$RUN_DIR/$name" sysroot="$REPO_ROOT/build/sdk/$sdk"
    local host platform_flags cxx_flags link_flags linker header macro
    case "$sdk" in
        *.sdk)
            host="${triple%%-*}-apple-darwin"
            platform_flags="--target=$triple -isysroot $sysroot"
            cxx_flags="-stdlib=libc++ -nostdinc++ -isystem $sysroot/usr/include/c++/v1"
            link_flags="-fuse-ld=lld"
            linker="$CLANG_BIN/ld64.lld"
            ;;
        *)
            host="$triple"
            platform_flags="--target=$triple --sysroot=$sysroot -fPIC"
            cxx_flags="-stdlib=libstdc++"
            link_flags="-fuse-ld=lld -unwindlib=libgcc"
            linker="$CLANG_BIN/ld.lld"
            ;;
    esac
    mkdir -p "$work"
    printf 'Configuring %s / %s...\n' "$PROJECT" "$name"
    (
        cd "$work" || exit 1
        set --
        # On case-insensitive filesystems, Endian.h would satisfy endian.h.
        case "$PROJECT/$sdk" in
            ngtcp2/iPhone*.sdk) set -- ac_cv_header_endian_h=no ;;
        esac
        "${CLEAN_ENV[@]}" \
            CC="$CLANG_BIN/clang $platform_flags" \
            CXX="$CLANG_BIN/clang++ $platform_flags $cxx_flags" \
            CFLAGS="-O2 -std=gnu17" CXXFLAGS=-O2 CPPFLAGS= LIBS= \
            LDFLAGS="$link_flags" LD="$linker" \
            AR="$CLANG_BIN/llvm-ar" RANLIB="$CLANG_BIN/llvm-ranlib" \
            NM="$CLANG_BIN/llvm-nm" STRIP="$CLANG_BIN/llvm-strip" \
            "$@" /bin/sh "$REPO_ROOT/$PROJECT/configure" \
                --no-create --cache-file=/dev/null --build="$BUILD_TRIPLE" --host="$host" \
                "${CONFIGURE_OPTIONS[@]}" >configure.log 2>&1 &&
        "${CLEAN_ENV[@]}" /bin/sh ./config.status "${CONFIG_HEADERS[@]}" >>configure.log 2>&1
    ) || fail "$PROJECT/$name failed; see $work/configure.log and config.log"
    for header in "${CONFIG_HEADERS[@]}"; do
        [ -s "$work/$header" ] || fail "configure did not generate $work/$header"
    done
    if [ "$PROJECT" = c-ares ]; then
        for macro in CARES_THREADS CARES_SYMBOL_HIDING; do
            awk -v macro="$macro" '
                $1 == "#define" && $2 == macro && $3 == "1" { found = 1 }
                END { exit !found }
            ' "$work/src/lib/ares_config.h" || fail "$name: requested feature missing: $macro"
        done
    fi
}

queue_output() {
    GENERATED+=("$1")
    OUTPUTS+=("$2")
}

# Group ares_config.h by identical contents. Future platform differences get
# their own directories and mappings; only byte-identical files are shared.
group_cares_config() {
    local header=src/lib/ares_config.h name group count=0 directory index
    local -a representatives=() members=() indices=() directories=()
    for name in "${TARGET_NAMES[@]}"; do
        group=0
        while [ "$group" -lt "$count" ]; do
            if cmp -s "$RUN_DIR/$name/$header" "$RUN_DIR/${representatives[$group]}/$header"; then
                break
            fi
            group=$((group + 1))
        done
        if [ "$group" -eq "$count" ]; then
            representatives+=("$name")
            members+=("$name")
            count=$((count + 1))
        else
            members[$group]="${members[$group]} $name"
        fi
        indices+=("$group")
    done
    for ((group = 0; group < count; group++)); do
        case "${members[$group]}" in
            'mac_arm64 mac_x64 ios_arm64 ios_sim_arm64 ios_sim_x64 linux_x64') directory=common ;;
            'mac_arm64 mac_x64 ios_arm64 ios_sim_arm64 ios_sim_x64') directory=apple ;;
            'mac_arm64 mac_x64') directory=mac ;;
            'ios_arm64 ios_sim_arm64 ios_sim_x64') directory=ios ;;
            'ios_sim_arm64 ios_sim_x64') directory=ios_sim ;;
            linux_x64) directory=linux ;;
            *) directory="${members[$group]// /_and_}" ;;
        esac
        directories+=("$CONFIG_DIR/$directory")
        queue_output "$RUN_DIR/${representatives[$group]}/$header" "$CONFIG_DIR/$directory/${header##*/}"
        printf '  %s/%s: %s\n' "$directory" "${header##*/}" "${members[$group]}"
    done
    {
        printf 'cares_config_dirs = {\n'
        for index in "${!TARGET_NAMES[@]}"; do
            group="${indices[$index]}"
            printf '  %s = "//%s"\n' "${TARGET_NAMES[$index]}" "${directories[$group]}"
        done
        printf '}\n'
    } >> "$RUN_DIR/targets.gni"
}

for PROJECT in "${PROJECTS[@]}"; do
    CONFIG_DIR="$PROJECT/gn/config"
    case "$PROJECT" in
        ngtcp2)
            CONFIG_GUESS=ngtcp2/config.guess
            CONFIG_HEADERS=(config.h)
            CONFIGURE_OPTIONS=(--enable-lib-only --disable-cryptotest --disable-dependency-tracking
                --without-openssl --without-gnutls --without-boringssl --without-picotls
                --without-wolfssl --without-jemalloc --without-libnghttp3 --without-libev
                --without-libbrotlienc --without-libbrotlidec)
            ;;
        c-ares)
            CONFIG_GUESS=c-ares/config/config.guess
            CONFIG_HEADERS=(src/lib/ares_config.h include/ares_build.h)
            CONFIGURE_OPTIONS=(--disable-tests --disable-dependency-tracking --disable-maintainer-mode
                --disable-code-coverage --enable-shared --enable-static --enable-symbol-hiding
                --enable-cares-threads)
            ;;
    esac
    [ -f "$PROJECT/configure" ] && [ -f "$CONFIG_GUESS" ] || fail "missing $PROJECT configure inputs"
    BUILD_TRIPLE="$("${CLEAN_ENV[@]}" /bin/sh "$CONFIG_GUESS")"
    mkdir -p "out/$PROJECT-config"
    RUN_DIR="$(mktemp -d "$REPO_ROOT/out/$PROJECT-config/run.XXXXXX")"
    printf 'Configure outputs and logs: %s\n' "${RUN_DIR#"$REPO_ROOT/"}"
    for target in "${TARGETS[@]}"; do
        IFS='|' read -r name triple sdk <<< "$target"
        configure_target "$name" "$triple" "$sdk"
    done
    if [ "$PROJECT" = c-ares ]; then
        # ares.h includes this header from its own directory. Overwrite the
        # release archive's generic copy only when all supported targets agree.
        for name in "${TARGET_NAMES[@]}"; do
            if ! cmp -s "$RUN_DIR/mac_arm64/include/ares_build.h" "$RUN_DIR/$name/include/ares_build.h"; then
                diff -u "$RUN_DIR/mac_arm64/include/ares_build.h" "$RUN_DIR/$name/include/ares_build.h" || [ "$?" -eq 1 ]
                fail "c-ares public headers differ; reconcile target differences before overwriting include/ares_build.h"
            fi
        done
        queue_output "$RUN_DIR/mac_arm64/include/ares_build.h" "$PROJECT/include/ares_build.h"
        {
            printf '# Generated by util/update_autotools_config.sh c-ares. Do not edit.\n'
            printf '# ares_config.h is grouped by identical contents.\n'
        } > "$RUN_DIR/targets.gni"
        group_cares_config
        queue_output "$RUN_DIR/targets.gni" "$CONFIG_DIR/targets.gni"
    else
        # Preserve the directories already consumed by ngtcp2's GN adapter.
        for name in mac_x64 ios_arm64 ios_sim_arm64 ios_sim_x64; do
            if ! cmp -s "$RUN_DIR/mac_arm64/config.h" "$RUN_DIR/$name/config.h"; then
                diff -u "$RUN_DIR/mac_arm64/config.h" "$RUN_DIR/$name/config.h" || [ "$?" -eq 1 ]
                fail "ngtcp2 Apple configs differ; split the config groups and update the GN adapter"
            fi
        done
        queue_output "$RUN_DIR/mac_arm64/config.h" "$CONFIG_DIR/apple/config.h"
        queue_output "$RUN_DIR/linux_x64/config.h" "$CONFIG_DIR/linux/config.h"
    fi
done

# Complete all probes and grouping before replacing any tracked output.
STALE=0
for index in "${!OUTPUTS[@]}"; do
    generated="${GENERATED[$index]}"
    output="${OUTPUTS[$index]}"
    if cmp -s "$generated" "$output"; then
        printf '%s is up to date.\n' "$output"
    elif [ "$CHECK" -eq 1 ]; then
        actual="$output"
        [ -e "$actual" ] || actual=/dev/null
        diff -u -L "$output" -L "$output (generated)" "$actual" "$generated" || [ "$?" -eq 1 ]
        STALE=1
    else
        mkdir -p "$(dirname "$output")"
        TEMP_FILE="$(mktemp "$output.XXXXXX")"
        cp "$generated" "$TEMP_FILE"
        chmod 644 "$TEMP_FILE"
        mv -f "$TEMP_FILE" "$output"
        TEMP_FILE=
        printf 'Updated %s.\n' "$output"
    fi
done
[ "$STALE" -eq 0 ] || fail "Run ./util/update_autotools_config.sh $REQUESTED_PROJECT to update the headers."
