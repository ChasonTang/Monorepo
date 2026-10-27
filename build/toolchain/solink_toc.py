#!/usr/bin/env python3
"""Record a shared library's link interface without tracking its code layout.

Used after a successful solink. GN links consumers against the library but
tracks this TOC with Ninja restat, so an implementation-only change stops here.
This is a link interface, not a C/C++ ABI checker; headers remain compiler deps.

Design references (with local adaptations for the pinned LLVM toolchain):
  Chromium: build/toolchain/apple/linker_driver.py and gcc_solink_wrapper.py
  Android Soong: scripts/toc.sh
  Meson: mesonbuild/scripts/symbolextractor.py
See README.md for links and the reasons for retaining richer symbol metadata.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def macho_toc(output):
    identity = []
    exports = []
    command = ""
    in_exports = False
    reexports = False
    dylib_ids = 0
    id_fields = set()
    for line in output.splitlines():
        line = line.strip()
        if line == "Exports trie:":
            in_exports = True
        elif in_exports:
            if not line:
                continue
            if not re.match(r"^(0x[0-9A-Fa-f]+\s|\[re-export\] )", line):
                raise ValueError("unexpected Mach-O export: " + line)
            # Keep weak/TLS/re-export flags, and absolute symbol values. Normal
            # addresses and resolver locations change when code layout changes.
            if not re.search(r"\[[^\]]*\babsolute\b", line):
                line = re.sub(r"^0x[0-9A-Fa-f]+\s+", "", line)
            line = re.sub(r"resolver=0x[0-9A-Fa-f]+", "resolver", line)
            exports.append(line)
        elif line.startswith("Load command "):
            command = ""
        elif line.startswith("cmd "):
            command = line.split()[1]
            if command == "LC_ID_DYLIB":
                dylib_ids += 1
            # Whole-library re-exports are not expanded in the export trie.
            # Conservatively propagate every relink through such libraries.
            if command == "LC_REEXPORT_DYLIB":
                reexports = True
                identity.append(command)
        elif command in ("LC_ID_DYLIB", "LC_REEXPORT_DYLIB"):
            if line.startswith(
                ("name ", "current version ", "compatibility version ")
            ):
                identity.append(re.sub(r" \(offset \d+\)$", "", line))
                if command == "LC_ID_DYLIB":
                    id_fields.add(line.split()[0])
    # Like Chromium, reject duplicate IDs instead of accepting an ambiguous
    # interface. Our solink only emits thin MH_DYLIB files, so an ID is required.
    if dylib_ids != 1 or len(id_fields) != 3 or not in_exports:
        raise ValueError("expected one complete Mach-O dylib identity and exports trie")
    return "\n".join(identity + sorted(exports)) + "\n", reexports


def elf_toc(output):
    identity = []
    symbols = []
    expected_symbols = None
    seen_symbols = 0
    for line in output.splitlines():
        line = line.strip()
        if "(SONAME)" in line:
            identity.append(line.split("(SONAME)", 1)[1].strip())
        elif line.startswith("Symbol table '.dynsym'"):
            header = re.fullmatch(r"Symbol table '.dynsym' contains (\d+) entries:", line)
            if header is None or expected_symbols is not None:
                raise ValueError("unexpected ELF dynamic symbol table: " + line)
            expected_symbols = int(header[1])
        elif expected_symbols is not None and re.match(r"^\d+:", line):
            seen_symbols += 1
            fields = line.split(None, 7)
            if len(fields) == 7 and fields[0] == "0:":  # The unnamed null symbol.
                continue
            if len(fields) != 8:
                raise ValueError("unexpected ELF dynamic symbol: " + line)
            _, value, size, kind, binding, visibility, section, name = fields
            if binding == "LOCAL":
                continue
            # Symbol versions are part of the name. Keep imports as well as
            # exports, and data/TLS sizes (e.g. for copy relocations). Function
            # sizes, addresses and section numbers describe implementation.
            if kind in ("FUNC", "IFUNC", "GNU_IFUNC"):
                size = "-"
            if section.isdigit():
                section = "DEFINED"
            if section != "ABS":
                value = "-"
            symbols.append(
                " ".join((name, kind, binding, visibility, section, size, value))
            )
    if len(identity) != 1 or expected_symbols != seen_symbols:
        raise ValueError("expected one ELF SONAME and a complete dynamic symbol table")
    return "\n".join(identity + sorted(symbols)) + "\n", False


def update_toc(path, contents, force=False):
    contents = contents.encode("utf-8", errors="surrogateescape")
    try:
        if not force and path.read_bytes() == contents:
            return
    except FileNotFoundError:
        pass
    # Chromium's Apple linker also uses a unique temporary file and rename.
    # Keep it beside the TOC so replacement stays atomic across mount points.
    with tempfile.NamedTemporaryFile(
        prefix=path.name + ".", dir=path.parent, delete=False
    ) as stream:
        temporary = Path(stream.name)
        try:
            stream.write(contents)
            stream.close()
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)


def depfile_path(path):
    return (str(path).replace("\\", "\\\\").replace("$", "$$")
            .replace(" ", "\\ ").replace("#", "\\#"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--format", choices=("macho", "elf"), required=True)
    parser.add_argument("--tool-dir", type=Path, required=True)
    parser.add_argument("--library", required=True)
    parser.add_argument("--toc", type=Path, required=True)
    parser.add_argument("--depfile", type=Path, required=True)
    args = parser.parse_args()

    if args.format == "macho":
        tool = args.tool_dir / "llvm-objdump"
        flags = ["--macho", "--private-headers", "--exports-trie"]
        collect = macho_toc
    else:
        tool = args.tool_dir / "llvm-readelf"
        flags = ["--wide", "--dynamic-table", "--dyn-syms"]
        collect = elf_toc
    try:
        output = subprocess.check_output(
            [str(tool), *flags, args.library],
            text=True,
            encoding="utf-8",
            errors="surrogateescape",
            env={**os.environ, "LC_ALL": "C"},
        )
        contents, force = collect(output)
        # Track the script and inspector too; changing either must regenerate
        # the TOC even if no C/C++ source changed. Keep symlink paths as invoked.
        dependencies = " ".join(
            depfile_path(p) for p in (os.path.abspath(__file__), tool)
        )
        args.depfile.write_text(f"{depfile_path(args.library)}: {dependencies}\n")
        update_toc(args.toc, contents, force)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"solink_toc.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
