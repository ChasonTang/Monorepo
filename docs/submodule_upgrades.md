# Upgrade dependencies

Start after completing Quick Start, with the submodules already initialized.
Run the following commands from the repository root.

## Git submodules

1. Set the dependency to upgrade. Use `xquic`, `boringssl`, `c-ares`,
   or `googletest`:

   ```bash
   dependency=googletest
   ```

2. Fetch upstream tags and list available tags:

   ```bash
   git -C "$dependency" fetch origin --tags
   git -C "$dependency" tag --list --sort=-version:refname
   ```

3. Replace `<newer-release-tag>` with the newer release tag you want to use,
   then check out that tag and update any nested submodules:

   ```bash
   target_tag='<newer-release-tag>'
   git -C "$dependency" checkout --detach "refs/tags/$target_tag"
   git -C "$dependency" submodule update --init --recursive
   ```

4. When upgrading `googletest`, regenerate
   `build/secondary/googletest/headers.gni`:

   ```bash
   ./util/update_googletest_headers.sh
   ```

5. Stage the updated submodule:

   ```bash
   git add -- "$dependency"
   ```

   For `googletest`, also stage the regenerated header list:

   ```bash
   git add -- build/secondary/googletest/headers.gni
   ```

6. Commit the upgrade in the parent repository using a Conventional Commits
   message with the `chore(deps)` prefix:

   ```bash
   git commit -m "chore(deps): upgrade $dependency to $target_tag"
   ```

## ngtcp2 release archive

1. Download the `ngtcp2-<version>.tar.xz` asset from the target
   [upstream release](https://github.com/ngtcp2/ngtcp2/releases). Set the
   version without the `v` prefix and the downloaded archive's path:

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

   After extraction succeeds, replace the entire `ngtcp2/` tree, retaining
   the previous tree in the upgrade directory:

   ```bash
   mv ngtcp2 "$ngtcp2_upgrade_dir/previous"
   mv "$ngtcp2_upgrade_dir/ngtcp2-$ngtcp2_version" ngtcp2
   ```

   Keep the release's `configure` and `lib/includes/ngtcp2/version.h`.

3. Regenerate the source lists and configuration headers:

   ```bash
   ./util/update_ngtcp2_sources.sh
   ./util/update_ngtcp2_config.sh
   ```

   If configuration fails, inspect `configure.log` and `config.log` in the
   target directory under the printed `out/ngtcp2-config/run.*/` path. If
   Apple configurations differ, split `build/secondary/ngtcp2/config/` into
   the required platform groups, update `build/secondary/ngtcp2/BUILD.gn`
   and `util/update_ngtcp2_config.sh`, then rerun generation.

4. Stage and commit the source, generated files and any generator or GN
   adjustments:

   ```bash
   git add -A -- ngtcp2 build/secondary/ngtcp2 \
       util/update_ngtcp2_sources.sh util/update_ngtcp2_config.sh
   git commit -m "chore(deps): upgrade ngtcp2 to v$ngtcp2_version"
   ```
