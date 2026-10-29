# Upgrade submodule dependencies

Run the following commands from the repository root.

1. Set the dependency to upgrade. Use `xquic`, `boringssl`, `c-ares`,
   or `googletest`:

   ```bash
   dependency=googletest
   ```

2. Initialize the submodule, fetch upstream tags, and list available tags:

   ```bash
   git submodule update --init --recursive -- "$dependency"
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
