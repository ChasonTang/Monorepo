#!/usr/bin/env python3

import argparse
import pathlib
import re
import sys


def strip_c_like(text):
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            i += 2
            while i < n and text[i] != "\n":
                i += 1
            out.append("\n")
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            i = min(i + 2, n)
            continue
        if c in ("'", '"'):
            quote = c
            out.append(" ")
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def tokens(text):
    return re.findall(r"[A-Za-z_][A-Za-z0-9_]*", strip_c_like(text))


def find_target_body(build_text, target_name):
    marker = f'"{target_name}"'
    start = build_text.find(marker)
    if start < 0:
        raise RuntimeError(f"missing GN target {target_name}")
    brace = build_text.find("{", start)
    if brace < 0:
        raise RuntimeError(f"missing GN body for {target_name}")
    depth = 0
    for i in range(brace, len(build_text)):
        if build_text[i] == "{":
            depth += 1
        elif build_text[i] == "}":
            depth -= 1
            if depth == 0:
                return build_text[brace + 1 : i]
    raise RuntimeError(f"unterminated GN body for {target_name}")


def find_c_function_body(text, name):
    m = re.search(
        r"\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{", text, re.S
    )
    if not m:
        raise RuntimeError(f"missing function {name}")
    brace = m.end() - 1
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[brace + 1 : i]
    raise RuntimeError(f"unterminated body for {name}")


def find_balanced_block_at(text, open_brace):
    if open_brace < 0 or open_brace >= len(text) or text[open_brace] != "{":
        raise RuntimeError("missing block open brace")
    depth = 0
    for i in range(open_brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace + 1 : i], i + 1
    raise RuntimeError("unterminated block")


def find_drive_parse_http_factory_branch(client_session_text):
    body = find_c_function_body(client_session_text, "drive_parse_http")
    marker = "cs->upstream_mode == ODIN_CLIENT_SESSION_UPSTREAM_TCP_DIAL"
    marker_at = body.find(marker)
    if marker_at < 0:
        raise RuntimeError("missing drive_parse_http upstream-mode branch")
    tcp_open = body.find("{", marker_at)
    _, tcp_close = find_balanced_block_at(body, tcp_open)
    else_match = re.search(r"\belse\b\s*\{", body[tcp_close:])
    if not else_match:
        raise RuntimeError("missing drive_parse_http FACTORY branch")
    factory_open = tcp_close + else_match.end() - 1
    factory_body, _ = find_balanced_block_at(body, factory_open)
    return factory_body


def check_no_tokens(label, token_list, forbidden):
    found = []
    for tok in token_list:
        if tok in forbidden or (tok.startswith("exec") and "exec*" in forbidden):
            found.append(tok)
    if found:
        unique = ", ".join(sorted(set(found)))
        raise RuntimeError(f"{label}: forbidden token(s): {unique}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--stamp", required=True)
    args = parser.parse_args()

    root = pathlib.Path(args.root)
    build_text = (root / "odin" / "BUILD.gn").read_text()

    cli_text = "\n".join(
        [
            (root / "odin" / "cli_client.c").read_text(),
            (root / "odin" / "cli_client.h").read_text(),
            find_target_body(build_text, "odin_cli_client"),
        ]
    )
    check_no_tokens(
        "CLI client scope",
        tokens(cli_text),
        {"xquic", "client_xqc_runtime", "transport_xqc"},
    )

    runtime_text = "\n".join(
        [
            (root / "odin" / "client_xqc_runtime.c").read_text(),
            (root / "odin" / "client_xqc_runtime.h").read_text(),
            find_target_body(build_text, "odin_client_xqc_runtime"),
        ]
    )
    runtime_forbidden = {
        "odin_dial_start",
        "connect",
        "open",
        "exec*",
        "dlopen",
    }
    check_no_tokens("client xqc runtime scope", tokens(runtime_text), runtime_forbidden)

    client_session_text = (root / "odin" / "client_session.c").read_text()
    factory_region = "\n".join(
        [
            find_c_function_body(
                client_session_text,
                "odin_client_session_create_with_upstream_transport",
            ),
            find_c_function_body(client_session_text, "start_factory_upstream"),
            find_drive_parse_http_factory_branch(client_session_text),
        ]
    )
    check_no_tokens(
        "client session factory scope",
        tokens(factory_region),
        {
            "start_dial",
            "odin_dial_start",
            "connect",
            "open",
            "exec*",
            "dlopen",
        },
    )

    pathlib.Path(args.stamp).write_text("ok\n")
    print("no forbidden CLI token in odin_cli_client")
    print("no forbidden local-resource token in odin_client_xqc_runtime")
    print("no forbidden local-resource token in client_session factory")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        sys.exit(1)
