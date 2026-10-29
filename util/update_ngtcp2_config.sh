#!/usr/bin/env bash
# Generate ngtcp2 config.h for all six targets using the bundled Clang and SDKs.
# Usage: ./util/update_ngtcp2_config.sh [--check]
set -euo pipefail

fail() {
    printf 'update_ngtcp2_config: %s\n' "$*" >&2
    exit 1
}

if [ "$#" -eq 1 ] && { [ "$1" = --help ] || [ "$1" = -h ]; }; then
    printf 'Usage: ./util/update_ngtcp2_config.sh [--check]\n'
    exit 0
fi
if [ "$#" -gt 1 ] || { [ "$#" -eq 1 ] && [ "$1" != --check ]; }; then
    fail "usage: ./util/update_ngtcp2_config.sh [--check]"
fi

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
cd "$REPO_ROOT"
export LC_ALL=C
CLANG_BIN="$REPO_ROOT/tool/clang/bin"
CONFIG_DIR=build/secondary/ngtcp2/config

# Missing SDKs must not make configure fall back to host headers or libraries.
for sdk in MacOSX.sdk iPhoneOS.sdk iPhoneSimulator.sdk debian_bullseye_amd64-sysroot; do
    [ -d "build/sdk/$sdk" ] || fail "missing build/sdk/$sdk; see README.md for SDK setup"
done
for tool in clang clang++ ld64.lld ld.lld llvm-ar llvm-ranlib llvm-nm llvm-strip; do
    [ -x "$CLANG_BIN/$tool" ] || fail "missing tool/clang/bin/$tool; run ./sync_tools.sh"
done

BUILD_TRIPLE="$(/bin/sh ngtcp2/config.guess)"
mkdir -p out/ngtcp2-config
RUN_DIR="$(mktemp -d "$REPO_ROOT/out/ngtcp2-config/run.XXXXXX")"
printf 'Configure outputs and logs: %s\n' "${RUN_DIR#"$REPO_ROOT/"}"
TEMP_FILE=
trap 'if [ -n "$TEMP_FILE" ]; then rm -f "$TEMP_FILE"; fi' EXIT
trap 'exit 1' HUP INT TERM

configure_target() {
    local name="$1" triple="$2" sdk="$3"
    local work="$RUN_DIR/$name" sysroot="$REPO_ROOT/build/sdk/$sdk"
    local host platform_flags cxx_flags link_flags linker

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
    printf 'Configuring %s...\n' "$name"
    (
        cd "$work" || exit 1
        set --
        # On case-insensitive filesystems, Endian.h would satisfy endian.h.
        case "$sdk" in
            iPhone*.sdk) set -- ac_cv_header_endian_h=no ;;
        esac
        env -i HOME="$HOME" LC_ALL=C \
            PATH="$CLANG_BIN:/usr/bin:/bin:/usr/sbin:/sbin" \
            CONFIG_SITE=/dev/null PKG_CONFIG=/usr/bin/false \
            CC="$CLANG_BIN/clang $platform_flags" \
            CXX="$CLANG_BIN/clang++ $platform_flags $cxx_flags" \
            CFLAGS="-O2 -std=gnu17" CXXFLAGS=-O2 CPPFLAGS= LIBS= \
            LDFLAGS="$link_flags" LD="$linker" \
            AR="$CLANG_BIN/llvm-ar" RANLIB="$CLANG_BIN/llvm-ranlib" \
            NM="$CLANG_BIN/llvm-nm" STRIP="$CLANG_BIN/llvm-strip" \
            "$@" /bin/sh "$REPO_ROOT/ngtcp2/configure" \
                --no-create --cache-file=/dev/null --build="$BUILD_TRIPLE" --host="$host" \
                --enable-lib-only --disable-cryptotest --disable-dependency-tracking \
                --without-openssl --without-gnutls --without-boringssl \
                --without-picotls --without-wolfssl --without-jemalloc \
                --without-libnghttp3 --without-libev \
                --without-libbrotlienc --without-libbrotlidec \
                >configure.log 2>&1 &&
        env -i HOME="$HOME" LC_ALL=C \
            PATH="$CLANG_BIN:/usr/bin:/bin:/usr/sbin:/sbin" \
            /bin/sh ./config.status config.h >>configure.log 2>&1
    ) || fail "$name failed; see $work/configure.log and $work/config.log"
}

# Keep the triples and deployment versions in sync with build/toolchain/BUILD.gn.
configure_target mac_arm64 arm64-apple-macos11.0 MacOSX.sdk
configure_target mac_x64 x86_64-apple-macos10.15 MacOSX.sdk
configure_target ios_arm64 arm64-apple-ios15.0 iPhoneOS.sdk
configure_target ios_sim_arm64 arm64-apple-ios15.0-simulator iPhoneSimulator.sdk
configure_target ios_sim_x64 x86_64-apple-ios15.0-simulator iPhoneSimulator.sdk
configure_target linux_x64 x86_64-unknown-linux-gnu debian_bullseye_amd64-sysroot

# Validate the shared Apple header before publishing either output.
for name in mac_x64 ios_arm64 ios_sim_arm64 ios_sim_x64; do
    if ! cmp -s "$RUN_DIR/mac_arm64/config.h" "$RUN_DIR/$name/config.h"; then
        diff -u -L mac_arm64/config.h -L "$name/config.h" \
            "$RUN_DIR/mac_arm64/config.h" "$RUN_DIR/$name/config.h" || [ "$?" -eq 1 ]
        fail "Apple configs differ; split $CONFIG_DIR and update build/secondary/ngtcp2/BUILD.gn before regenerating"
    fi
done

STALE=0
for group in apple linux; do
    case "$group" in
        apple) generated="$RUN_DIR/mac_arm64/config.h" ;;
        linux) generated="$RUN_DIR/linux_x64/config.h" ;;
    esac
    output="$CONFIG_DIR/$group/config.h"
    if cmp -s "$generated" "$output"; then
        printf '%s is up to date.\n' "$output"
    elif [ "${1:-}" = --check ]; then
        actual="$output"
        if [ ! -e "$actual" ]; then
            actual=/dev/null
        fi
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

if [ "$STALE" -ne 0 ]; then
    fail "Run ./util/update_ngtcp2_config.sh to update the headers."
fi
