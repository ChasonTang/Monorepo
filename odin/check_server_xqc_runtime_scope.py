#!/usr/bin/env python3

import argparse
import os
import re
import sys


FORBIDDEN = ("xquic", "server_xqc_runtime", "transport_xqc")


def read_file(root, rel):
    with open(os.path.join(root, rel), "r", encoding="utf-8") as f:
        return f.read()


def strip_comments(text, hash_comments=False):
    out = []
    i = 0
    state = "normal"
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""
        if state == "normal":
            if hash_comments and c == "#":
                while i < len(text) and text[i] != "\n":
                    i += 1
                out.append("\n")
                continue
            if c == "/" and n == "/":
                state = "line_comment"
                i += 2
                continue
            if c == "/" and n == "*":
                state = "block_comment"
                i += 2
                continue
            if c == '"':
                state = "string"
                out.append(c)
                i += 1
                continue
            if c == "'":
                state = "char"
                out.append(c)
                i += 1
                continue
            out.append(c)
            i += 1
            continue
        if state == "line_comment":
            if c == "\n":
                out.append("\n")
                state = "normal"
            i += 1
            continue
        if state == "block_comment":
            if c == "*" and n == "/":
                state = "normal"
                i += 2
            else:
                out.append("\n" if c == "\n" else " ")
                i += 1
            continue
        if state == "string":
            if c == "\\":
                out.append(c)
                if i + 1 < len(text):
                    out.append(text[i + 1])
                i += 2
            elif c == '"':
                out.append(c)
                state = "normal"
                i += 1
            else:
                out.append(c)
                i += 1
            continue
        if state == "char":
            if c == "\\":
                out.append(c)
                if i + 1 < len(text):
                    out.append(text[i + 1])
                i += 2
            elif c == "'":
                out.append(c)
                state = "normal"
                i += 1
            else:
                out.append(c)
                i += 1
            continue
    return "".join(out)


def target_block(build_gn, target_name):
    pattern = r'source_set\("' + re.escape(target_name) + r'"\)\s*\{'
    m = re.search(pattern, build_gn)
    if not m:
        raise AssertionError(f'missing source_set("{target_name}")')
    depth = 1
    i = m.end()
    while i < len(build_gn) and depth:
        if build_gn[i] == "{":
            depth += 1
        elif build_gn[i] == "}":
            depth -= 1
        i += 1
    if depth != 0:
        raise AssertionError(f"unterminated {target_name} target")
    return build_gn[m.end(): i - 1]


def check_text(label, text, hash_comments=False):
    lowered = strip_comments(text, hash_comments=hash_comments).lower()
    for token in FORBIDDEN:
        if token in lowered:
            raise AssertionError(f"{label}: forbidden token {token!r}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--stamp", required=True)
    args = parser.parse_args()
    root = os.path.abspath(args.root)
    try:
        for rel in ("odin/server_runtime.c", "odin/server_runtime.h",
                    "odin/cli_server.c"):
            check_text(rel, read_file(root, rel))
        build_gn = read_file(root, "odin/BUILD.gn")
        for target in ("odin_server_runtime", "odin_cli_server"):
            check_text(f"odin/BUILD.gn:{target}",
                       target_block(build_gn, target),
                       hash_comments=True)
    except AssertionError as e:
        print(e, file=sys.stderr)
        return 1
    os.makedirs(os.path.dirname(args.stamp), exist_ok=True)
    with open(args.stamp, "w", encoding="utf-8") as f:
        f.write("ok\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
