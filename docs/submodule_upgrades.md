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

5. Stage the updated submodule:

   ```bash
   git add -- "$dependency"
   ```

   For `googletest`, also stage the header list:

   ```bash
   git add -- build/secondary/googletest/headers.gni
   ```

6. Commit:

   ```bash
   git commit -m "chore(deps): upgrade $dependency to $target_tag"
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

   The generators below recreate `c-ares/gn/sources.gni` and platform headers
   under `c-ares/gn/config/`. The archive's `c-ares/config/` contains only its
   Autotools helper scripts. The config generator also overwrites the archive's
   generic `include/ares_build.h` with configure output.

3. Regenerate source lists and configuration headers:

   ```bash
   ./util/update_autotools_sources.sh c-ares
   ./util/update_autotools_sources.sh c-ares --check
   ./util/update_autotools_config.sh c-ares
   ./util/update_autotools_config.sh c-ares --check
   ```

   The config generator verifies that all six targets produce identical
   `ares_build.h` contents before overwriting `c-ares/include/ares_build.h`.
   If they differ, reconcile the differences before updating the shared header.
   Private `ares_config.h` files are grouped by identical contents; review the
   diff and remove generated header directories no longer referenced by
   `c-ares/gn/config/targets.gni`.

   On failure, inspect the target's `configure.log` and `config.log` under
   the printed `out/c-ares-config/run.*/` path.

4. Validate the upgrade:

   - Run `gn gen --check` and build `cares` for each
     [supported target](../README.md#cross-compilation).
   - Confirm compiler dependencies use `c-ares/include/ares_build.h` and the
     selected platform's `ares_config.h` under `c-ares/gn/config/`.
   - Run Odin's DNS resolver tests on the host and follow the
     [static-analysis requirements](../README.md#formatting-and-static-analysis).

5. Update the release and SHA256 above and the version in
   `c-ares/BUILD.gn`.

6. Stage and commit:

   ```bash
   git add -A -- c-ares README.md docs/submodule_upgrades.md \
       util/update_autotools_config.sh util/update_autotools_sources.sh
   git commit -m "chore(deps): upgrade c-ares to v$cares_version"
   ```

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
   The generators below recreate `ngtcp2/gn/sources.gni` and `ngtcp2/gn/config/`.

3. Regenerate the source lists and configuration headers:

   ```bash
   ./util/update_autotools_sources.sh ngtcp2
   ./util/update_autotools_sources.sh ngtcp2 --check
   ./util/update_autotools_config.sh ngtcp2
   ./util/update_autotools_config.sh ngtcp2 --check
   ```

   On failure, inspect the target's `configure.log` and `config.log` under
   the printed `out/ngtcp2-config/run.*/` path. If Apple configurations
   differ, split `ngtcp2/gn/config/` into platform groups, update
   `ngtcp2/BUILD.gn` and `util/update_autotools_config.sh`, then rerun generation.

   Keep the generator's triples and deployment versions synchronized with
   `build/toolchain/BUILD.gn`.

4. Stage and commit:

   ```bash
   git add -A -- ngtcp2 README.md docs/submodule_upgrades.md \
       util/update_autotools_sources.sh \
       util/update_autotools_config.sh
   git commit -m "chore(deps): upgrade ngtcp2 to v$ngtcp2_version"
   ```
