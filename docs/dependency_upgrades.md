# Dependency upgrades

Use **Flutter 3.47.8** as the reference for GN, Ninja, Clang, the Linux x64
sysroot, and Apple SDKs. For an upgrade, replace `3.47.8` in the Flutter source
links below with the target release tag.

## Update the pins

Update [sync_tools.sh](../sync_tools.sh) from Flutter's
[DEPS](https://github.com/flutter/flutter/blob/3.47.8/DEPS):

| Monorepo variable | Flutter value |
| ----------------- | ------------- |
| `GN_GIT_REVISION` | `version` of the `gn/gn/${{platform}}` package, without `git_revision:`. |
| `NINJA_VERSION` | Full `version` of the `infra/3pp/tools/ninja/${{platform}}` package, including `version:`. |
| `CLANG_GIT_REVISION` | `clang_version` used by the macOS `fuchsia/third_party/clang` packages, without `git_revision:`. |

Check `mac-arm64` and `mac-amd64` support; adjust download URLs if package names
or version formats change.

Update [sync_linux_sysroot.sh](../sync_linux_sysroot.sh) from `bullseye_amd64` in
[sysroots.json](https://github.com/flutter/flutter/blob/3.47.8/engine/src/build/linux/sysroot_scripts/sysroots.json):
set `SYSROOT_SHA256` to `Sha256Sum`, `SYSROOT_NAME` to `SysrootDir`, and
`SYSROOT_URL` to `URL` + `/` + `Sha256Sum` (not `Tarball`). If the Debian release
changes, use the replacement amd64 entry and update `sysroot_path` in
[build/toolchain/BUILD.gn](../build/toolchain/BUILD.gn) and [README.md](../README.md).

## Enable Apple safe ICF when upgrading Clang

For each Clang upgrade, determine whether the bundled `ld64.lld` includes
[llvm-project#188400](https://github.com/llvm/llvm-project/pull/188400), commit
`788ea1105438bd866752d7758f4c305d4675e0cc`, or an equivalent backport. This
[fix](https://lists.llvm.org/pipermail/all-commits/Week-of-Mon-20260323/291955.html)
makes `--icf=safe` preserve distinct symbol addresses for objects without
address-significance metadata, including assembly inputs. Use the actual
source revision to determine whether the fix is included; a newer version
number or acceptance of the option is insufficient. The current Clang pin
`80743bd43fd5b38fedc503308e7a652e23d3ec93` lacks this fix.

If the fix is included, enable Apple safe ICF in the same upgrade:
update the `current_os == "mac" || current_os == "ios"` branch of
`config("release")` in [build/BUILD.gn](../build/BUILD.gn) to use:

```gn
cflags += [ "-faddrsig" ]
ldflags = [
  "-Wl,-dead_strip",
  "-Wl,--icf=safe",
]
```

Remove the obsolete comment that keeps Apple ICF disabled. If the fix is absent
or its inclusion is unknown, leave Apple ICF disabled.

## Find the matching Xcode version

1. Read [bin/internal/engine.version](https://github.com/flutter/flutter/blob/3.47.8/bin/internal/engine.version)
   at the release tag. Use that Engine revision in the configuration URLs below
   to match the published binaries.
2. In [engine/src/flutter/.ci.yaml](https://github.com/flutter/flutter/blob/3.47.8/engine/src/flutter/.ci.yaml),
   read `sdk_version` under `$flutter/osx_sdk` on the `Mac mac_host_engine` and
   `Mac mac_ios_engine` release tasks (`release_build: "true"`). Check overrides in
   [mac_host_engine.json](https://github.com/flutter/flutter/blob/3.47.8/engine/src/flutter/ci/builders/mac_host_engine.json)
   and [mac_ios_engine.json](https://github.com/flutter/flutter/blob/3.47.8/engine/src/flutter/ci/builders/mac_ios_engine.json)
   for macOS x64/arm64 and iOS device/simulator builds across Debug, Profile,
   and Release.
3. `sdk_version` is an **Xcode build number**; the CI `os` field describes the
   host OS. Match the build number at
   [Apple Developer Downloads](https://developer.apple.com/download/all/) and
   check the Xcode release notes for bundled SDK versions.

For Flutter 3.47.8, the release tasks and sub-builds pin `17c52`:
**Xcode 26.2 (17C52)** with **macOS 26.2 and iOS 26.2 SDKs**
([release notes](https://developer.apple.com/documentation/xcode-release-notes/xcode-26_2-release-notes)).

## Update CFSSL

CFSSL follows [Cloudflare releases](https://github.com/cloudflare/cfssl/releases/latest)
independently of Flutter. Set `CFSSL_VERSION` in `sync_tools.sh` to the release
tag without `v` (currently [1.7.1](https://github.com/cloudflare/cfssl/releases/tag/v1.7.1));
this pins both `cfssl` and `cfssljson`.

Check the release assets for
`cfssl_<version>_darwin_arm64`, `cfssl_<version>_darwin_amd64`, and
`cfssljson_<version>_darwin_amd64`. In v1.7.1, the script installs `cfssljson`
only on Intel Macs because no macOS arm64 asset is published. Recheck platform
support and adjust `CFSSLJSON_PLATFORM` and download URLs as needed.

## Refresh local dependencies

`sync_tools.sh` compares version output for GN (Git revision prefix), Clang
(full Git revision), and CFSSL/CFSSLJSON (release version).

Ninja and the Linux sysroot are skipped whenever their binary or directory
exists. Changing their pins alone does not reinstall them. Before upgrading
either, remove its existing installation from the repository root, running
only the applicable command:

```bash
rm -f tool/ninja
rm -rf build/sdk/debian_bullseye_amd64-sysroot
```

After updating the pins, run from the repository root:

```bash
./sync_tools.sh
./sync_linux_sysroot.sh
```

For a CFSSL-only upgrade, run `./sync_tools.sh`.

Download and unpack the matching full Xcode distribution from Apple, place or
symlink it at `./Xcode.app`, and verify its version and build against the Engine pin:

```bash
DEVELOPER_DIR="$PWD/Xcode.app/Contents/Developer" xcodebuild -version
```

After confirming the match, run [extract_sdk.sh](../extract_sdk.sh):

```bash
./extract_sdk.sh
```

Update this document's versions and links alongside the pins.
