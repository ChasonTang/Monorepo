#!/usr/bin/env python3
"""build/symlink.py — create relative symlinks for the odin CLI artifacts.

For each ``--link PATH``, ensures ``PATH`` is a symlink whose target is the
basename ``--target NAME``. Symlink resolution is therefore relative to the
directory that contains ``PATH`` (so ``$root_out_dir/odin-client`` -> basename
``odin`` resolves to its sibling ``$root_out_dir/odin``), and the build
directory stays relocatable. Any pre-existing entry at ``PATH`` (regular
file, dangling symlink, valid symlink) is unlinked before the new symlink is
created so re-runs are deterministic.

Used by the ``:odin_symlinks`` GN action — see odin/BUILD.gn.
"""

import argparse
import os
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--target",
        required=True,
        help="Symlink target (basename, e.g. 'odin')",
    )
    parser.add_argument(
        "--link",
        action="append",
        default=[],
        help="Path of a symlink to create; may be repeated",
    )
    args = parser.parse_args()

    try:
        for path in args.link:
            if os.path.lexists(path):
                os.unlink(path)
            os.symlink(args.target, path)
    except OSError as e:
        sys.stderr.write("symlink.py: %s\n" % e)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
