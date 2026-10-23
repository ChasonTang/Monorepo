#!/usr/bin/env python3
"""Update GN header inventories from the checked-out LLVM revision."""

import argparse
from pathlib import Path
import runpy
import subprocess
import sys


BUILD_DIR = Path(__file__).resolve().parent
REPO_ROOT = BUILD_DIR.parents[2]
LLVM_ROOT = REPO_ROOT / "llvm-project"
HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".inc", ".ipp", ".tcc", ".tpp", ".def"}


def gn_path(path):
    if not path.is_file():
        raise FileNotFoundError(f"missing header: {path}")
    return "//" + path.relative_to(REPO_ROOT).as_posix()


def implementation_headers(directory):
    return [
        gn_path(path)
        for path in directory.rglob("*")
        if path.is_file() and path.suffix in HEADER_SUFFIXES
    ]


def inventories():
    metadata_path = LLVM_ROOT / "libcxx/utils/libcxx/header_information.py"
    # Execute the upstream metadata without importing it into the source tree
    # (and without creating __pycache__ files in the LLVM checkout).
    metadata = runpy.run_path(str(metadata_path))
    config_headers = {
        name: BUILD_DIR / "libcxx/config" / name
        for name in ("__config_site", "__assertion_handler")
    }
    public = []
    private = []
    for header in metadata["all_headers"]:
        name = str(header)
        path = config_headers.get(name, LLVM_ROOT / "libcxx/include" / name)
        destination = public if header.is_public() else private
        destination.append(gn_path(path))

    yield "libcxx", {
        "libcxx_public_headers": public,
        "libcxx_private_headers": private,
        "libcxx_runtime_headers": implementation_headers(LLVM_ROOT / "libcxx/src"),
    }

    abi_include = LLVM_ROOT / "libcxxabi/include"
    abi_public = {gn_path(abi_include / "cxxabi.h")}
    yield "libcxxabi", {
        "libcxxabi_public_headers": abi_public,
        "libcxxabi_private_headers": (
            set(implementation_headers(abi_include)) - abi_public
        ),
        "libcxxabi_runtime_headers": implementation_headers(LLVM_ROOT / "libcxxabi/src"),
    }


def render(groups):
    lines = [
        "# Generated from the checked-out llvm-project headers. Do not edit.",
        "# Update: python3 build/secondary/llvm-project/update_headers.py",
        "# Verify: python3 build/secondary/llvm-project/update_headers.py --check",
    ]
    for name, paths in groups.items():
        lines.extend(["", f"{name} = ["])
        lines.extend(f'  "{path}",' for path in sorted(set(paths)))
        lines.append("]")
    # Use the pinned formatter so --check compares the same text developers see.
    return subprocess.run(
        [str(REPO_ROOT / "tool/gn"), "format", "--stdin"],
        input="\n".join(lines) + "\n",
        text=True,
        capture_output=True,
        check=True,
    ).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check", action="store_true", help="fail if inventories are stale"
    )
    args = parser.parse_args()

    try:
        # Validate both inventories before writing either one.
        files = [
            (BUILD_DIR / name / "headers.gni", render(groups))
            for name, groups in inventories()
        ]
    except (FileNotFoundError, KeyError) as error:
        parser.error(f"cannot generate LLVM header inventories: {error}")
    except subprocess.CalledProcessError as error:
        parser.error(f"GN header inventory formatting failed: {error.stderr}")

    stale = False
    for path, content in files:
        current = path.read_text(encoding="utf-8") if path.exists() else None
        if current == content:
            continue
        relative = path.relative_to(REPO_ROOT)
        if args.check:
            print(f"out of date: {relative}", file=sys.stderr)
            stale = True
        else:
            path.write_text(content, encoding="utf-8")
            print(f"updated {relative}")

    if stale:
        print(
            "run: python3 build/secondary/llvm-project/update_headers.py",
            file=sys.stderr,
        )
        return 1
    if args.check:
        print("LLVM header inventories are up to date")
    return 0


if __name__ == "__main__":
    sys.exit(main())
