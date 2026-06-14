#!/usr/bin/env python3

import argparse
import os
import re
import sys


ALLOWED_XQC_TOKENS = {
    "xqc_stream_t",
    "xqc_int_t",
    "XQC_OK",
    "XQC_EAGAIN",
    "XQC_ESTREAM_RESET",
    "XQC_CLOSING",
    "xqc_stream_recv",
    "xqc_stream_send",
    "xqc_stream_set_user_data",
}

FORBIDDEN_PATTERNS = [
    r"\bxqc_engine_t\b",
    r"\bxqc_connection_t\b",
    r"\bxqc_engine_[A-Za-z0-9_]*\b",
    r"\bxqc_conn(?!ection_t\b)[A-Za-z0-9_]*\b",
    r"\bxqc_datagram_[A-Za-z0-9_]*\b",
    r"\bxqc_socket[A-Za-z0-9_]*\b",
    r"\bxqc_timer[A-Za-z0-9_]*\b",
    r"\bxqc_cid[A-Za-z0-9_]*\b",
    r"\bxqc_alpn[A-Za-z0-9_]*\b",
    r"\bxqc_stream_create\b",
    r"\bxqc_stream_close\b",
    r"\bxqc_stream_reset\b",
    r"\bxqc_packet[A-Za-z0-9_]*\b",
    r"\bXQC_SOCKET[A-Za-z0-9_]*\b",
    r"\bXQC_[A-Za-z0-9_]*DGRAM[A-Za-z0-9_]*\b",
]


def strip_comments_strings_and_includes(text):
    out = []
    i = 0
    state = "normal"
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""
        if state == "normal":
            if c == "#":
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
                out.append(" ")
                i += 1
                continue
            if c == "'":
                state = "char"
                out.append(" ")
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
                i += 2
            elif c == '"':
                state = "normal"
                i += 1
            else:
                i += 1
            continue
        if state == "char":
            if c == "\\":
                i += 2
            elif c == "'":
                state = "normal"
                i += 1
            else:
                i += 1
            continue
    return "".join(out)


def read_file(root, rel):
    with open(os.path.join(root, rel), "r", encoding="utf-8") as f:
        return f.read()


def target_block(build_gn):
    m = re.search(r'source_set\("odin_transport_xqc"\)\s*\{', build_gn)
    if not m:
        raise AssertionError("missing source_set(\"odin_transport_xqc\")")
    depth = 1
    i = m.end()
    while i < len(build_gn) and depth:
        if build_gn[i] == "{":
            depth += 1
        elif build_gn[i] == "}":
            depth -= 1
        i += 1
    if depth != 0:
        raise AssertionError("unterminated odin_transport_xqc target")
    return build_gn[m.end(): i - 1]


def check_public_deps(build_gn):
    block = target_block(build_gn)
    m = re.search(r"public_deps\s*=\s*\[(.*?)\]", block, re.S)
    if not m:
        raise AssertionError("odin_transport_xqc target has no public_deps")
    deps = re.findall(r'"([^"]+)"', m.group(1))
    if deps != [":odin_transport", "//xquic"]:
        raise AssertionError(
            "odin_transport_xqc public_deps must be "
            '[ ":odin_transport", "//xquic" ]'
        )


def check_tokens(path, text):
    stripped = strip_comments_strings_and_includes(text)
    for pattern in FORBIDDEN_PATTERNS:
        m = re.search(pattern, stripped)
        if m:
            raise AssertionError(
                f"{path}: forbidden xquic scope token {m.group(0)!r}"
            )

    tokens = set(re.findall(r"\b[A-Za-z_][A-Za-z0-9_]*\b", stripped))
    bad = []
    for token in tokens:
        if token.startswith("odin_xqc"):
            continue
        if token.startswith("xqc_") or token.startswith("XQC_"):
            if token not in ALLOWED_XQC_TOKENS:
                bad.append(token)
    if bad:
        raise AssertionError(
            f"{path}: unexpected xquic tokens: {', '.join(sorted(bad))}"
        )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--stamp", required=True)
    args = parser.parse_args()

    root = os.path.abspath(args.root)
    try:
        header = read_file(root, "odin/transport_xqc.h")
        source = read_file(root, "odin/transport_xqc.c")
        build_gn = read_file(root, "odin/BUILD.gn")
        check_tokens("odin/transport_xqc.h", header)
        check_tokens("odin/transport_xqc.c", source)
        check_public_deps(build_gn)
    except AssertionError as e:
        print(e, file=sys.stderr)
        return 1

    os.makedirs(os.path.dirname(args.stamp), exist_ok=True)
    with open(args.stamp, "w", encoding="utf-8") as f:
        f.write("ok\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
