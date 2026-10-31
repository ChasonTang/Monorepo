# Upgrade dependencies

Complete [Quick Start](../README.md#quick-start), then run from the repository root.

## Git submodules

1. Set the dependency to upgrade. Use `xquic`, `boringssl`, or `googletest`:

   ```bash
   dependency=googletest
   ```

2. Fetch and list upstream tags:

   ```bash
   git -C "$dependency" fetch origin --tags
   git -C "$dependency" tag --list --sort=-version:refname
   ```

3. Check out the target release and update nested submodules:

   ```bash
   target_tag='<newer-release-tag>'
   git -C "$dependency" checkout --detach "refs/tags/$target_tag"
   git -C "$dependency" submodule update --init --recursive
   ```

4. For `googletest`, regenerate
   `build/secondary/googletest/headers.gni`:

   ```bash
   ./util/update_googletest_headers.sh
   ```

## c-ares release archive

Current release: [`c-ares-1.34.9.tar.gz`](https://github.com/c-ares/c-ares/releases/tag/v1.34.9).
SHA256:
`9364b70a12eb59a06830fb488d9357f90100dff0d98b23ea3593a183d43e6f2f`.

1. Download the `c-ares-<version>.tar.gz`
   [release asset](https://github.com/c-ares/c-ares/releases):

   ```bash
   cares_version='<newer-release-version>'
   mkdir -p out/dependency-upgrades
   cares_upgrade_dir="$(mktemp -d "$PWD/out/dependency-upgrades/c-ares.XXXXXX")"
   cares_archive="$cares_upgrade_dir/c-ares-$cares_version.tar.gz"
   curl -fL --retry 3 -o "$cares_archive" \
       "https://github.com/c-ares/c-ares/releases/download/v$cares_version/c-ares-$cares_version.tar.gz"
   shasum -a 256 "$cares_archive"
   ```

   Check the hash against the asset's SHA256 digest on GitHub before extracting.

2. Extract and replace `c-ares/`, preserving the repository's GN adapter:

   ```bash
   tar -xzf "$cares_archive" -C "$cares_upgrade_dir"
   mv c-ares "$cares_upgrade_dir/previous"
   mv "$cares_upgrade_dir/c-ares-$cares_version" c-ares
   cp "$cares_upgrade_dir/previous/BUILD.gn" c-ares/BUILD.gn
   ```

3. Regenerate source lists and configuration headers:

   ```bash
   ./util/update_autotools_sources.sh c-ares
   ./util/update_autotools_config.sh c-ares
   ```

   If generation fails because targets produce different `ares_build.h`
   contents, reconcile the differences before updating
   `c-ares/include/ares_build.h`.
   Remove generated header directories no longer referenced by
   `c-ares/gn/config/targets.gni`.

   On failure, inspect the target's `configure.log` and `config.log` under
   the printed `out/c-ares-config/run.*/` path.

4. Update the release and SHA256 above and the version in
   `c-ares/BUILD.gn`.

## ngtcp2 release archive

1. Download the `ngtcp2-<version>.tar.xz` asset from the target
   [upstream release](https://github.com/ngtcp2/ngtcp2/releases).
   Set the version without the `v` prefix and the archive path:

   ```bash
   ngtcp2_version='<newer-release-version>'
   ngtcp2_archive="$HOME/Downloads/ngtcp2-$ngtcp2_version.tar.xz"
   ```

2. Extract the archive into a new directory:

   ```bash
   mkdir -p out/dependency-upgrades
   ngtcp2_upgrade_dir="$(mktemp -d "$PWD/out/dependency-upgrades/ngtcp2.XXXXXX")"
   tar -xf "$ngtcp2_archive" -C "$ngtcp2_upgrade_dir"
   ```

   Replace `ngtcp2/`, preserving the repository's GN adapter:

   ```bash
   mv ngtcp2 "$ngtcp2_upgrade_dir/previous"
   mv "$ngtcp2_upgrade_dir/ngtcp2-$ngtcp2_version" ngtcp2
   cp "$ngtcp2_upgrade_dir/previous/BUILD.gn" ngtcp2/BUILD.gn
   ```

   Keep the release's `configure` and `lib/includes/ngtcp2/version.h`.

3. Regenerate the source lists and configuration headers:

   ```bash
   ./util/update_autotools_sources.sh ngtcp2
   ./util/update_autotools_config.sh ngtcp2
   ```

   On failure, inspect the target's `configure.log` and `config.log` under
   the printed `out/ngtcp2-config/run.*/` path. If Apple configurations
   differ, split `ngtcp2/gn/config/` into platform groups, update
   `ngtcp2/BUILD.gn` and `util/update_autotools_config.sh`, then rerun generation.

   Keep the generator's triples and deployment versions synchronized with
   `build/toolchain/BUILD.gn`.
