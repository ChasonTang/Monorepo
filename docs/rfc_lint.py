#!/usr/bin/env python3
"""Mechanical RFC hard-cap and consistency checks.

This script is intentionally narrow. It checks limits that should be enforced
before human RFC review starts, plus low-risk numeric consistency warnings.
Semantic correctness still belongs to the RFC review prompt.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


T_ROW_RE = re.compile(r"^\|\s*T\d+(?:[A-Za-z]|\.\d+)?\s*\|")
T_ROW_ID_RE = re.compile(r"^\|\s*T(?P<num>\d+)(?:[A-Za-z]|\.\d+)?\s*\|")
T_REF_RE = re.compile(r"\bT(?P<num>\d+)(?:[A-Za-z]|\.\d+)?\b")
T_RANGE_RE = re.compile(
    r"\bT(?P<start>\d+)(?:[A-Za-z]|\.\d+)?\s*[-–]\s*"
    r"T(?P<end>\d+)(?:[A-Za-z]|\.\d+)?\b"
)
FULL_RANGE_WORD_RE = re.compile(r"(?i)\b(all|rows?|runs?|passes|executes)\b")
DETAIL_RE = re.compile(r"^####\s+3\.2\.\d+\b")
SECTION_1_RE = re.compile(r"^##\s+1\.")


def count_unescaped_pipes(line: str) -> int:
    count = 0
    escaped = False
    for ch in line:
        if ch == "\\" and not escaped:
            escaped = True
            continue
        if ch == "|" and not escaped:
            count += 1
        escaped = False
    return count


def split_table_row(line: str) -> list[str]:
    cells: list[str] = []
    current: list[str] = []
    escaped = False
    for ch in line.rstrip("\n"):
        if ch == "\\" and not escaped:
            escaped = True
            current.append(ch)
            continue
        if ch == "|" and not escaped:
            cells.append("".join(current).strip())
            current = []
        else:
            current.append(ch)
        escaped = False
    cells.append("".join(current).strip())
    if cells and cells[0] == "":
        cells = cells[1:]
    if cells and cells[-1] == "":
        cells = cells[:-1]
    return cells


def body_lines(lines: list[str]) -> list[str]:
    """Return lines counted against the body cap.

    Revision Notes and the template's Writing Instructions are excluded because
    they are process scaffolding, not the RFC body.
    """
    result: list[str] = []
    skipping = False
    for line in lines:
        if line.startswith("## Revision Notes") or line.startswith(
            "## Writing Instructions"
        ):
            skipping = True
            continue
        if skipping and SECTION_1_RE.match(line):
            skipping = False
        if not skipping:
            result.append(line)
    return result


def body_line_pairs(lines: list[str]) -> list[tuple[int, str]]:
    """Return (1-based line number, line) pairs counted against the body cap."""
    result: list[tuple[int, str]] = []
    skipping = False
    for idx, line in enumerate(lines, start=1):
        if line.startswith("## Revision Notes") or line.startswith(
            "## Writing Instructions"
        ):
            skipping = True
            continue
        if skipping and SECTION_1_RE.match(line):
            skipping = False
        if not skipping:
            result.append((idx, line))
    return result


def has_exception_before_section_1(lines: list[str]) -> bool:
    for line in lines:
        if SECTION_1_RE.match(line):
            return False
        if line.startswith("Exception:") or line.startswith("## Exception"):
            return True
    return False


def subcase_count(line: str) -> int:
    cells = split_table_row(line)
    setup = cells[2] if len(cells) >= 6 else line

    explicit_mentions = len(re.findall(r"(?i)\bsubcase\b", setup))
    match = re.search(r"(?i)\bSubcases:\s*(.+)", setup)
    if not match:
        return explicit_mentions

    text = match.group(1)
    # Count comma-delimited entries after "Subcases:". This deliberately errs
    # on the side of failing giant aggregate rows; authors can split the row.
    comma_items = [item.strip() for item in text.split(",") if item.strip()]
    return max(explicit_mentions, len(comma_items))


def check_file(path: Path, args: argparse.Namespace) -> int:
    lines = path.read_text(encoding="utf-8").splitlines()
    body_pairs = body_line_pairs(lines)
    counted_body = [line for _, line in body_pairs]
    failures: list[str] = []
    warnings: list[str] = []

    body_count = len(counted_body)
    t_rows = [(idx + 1, line) for idx, line in enumerate(lines) if T_ROW_RE.match(line)]
    t_ids = {
        int(match.group("num"))
        for _, line in t_rows
        if (match := T_ROW_ID_RE.match(line)) is not None
    }
    mechanism_surfaces = [
        idx + 1 for idx, line in enumerate(counted_body) if DETAIL_RE.match(line)
    ]
    exception = has_exception_before_section_1(lines)

    if body_count > args.max_body_lines:
        failures.append(
            f"body lines {body_count} > {args.max_body_lines} "
            "(excluding Revision Notes / Writing Instructions)"
        )
    elif body_count > args.warn_body_lines:
        warnings.append(f"body lines {body_count} > warning {args.warn_body_lines}")

    if len(t_rows) > args.max_t_rows:
        failures.append(f"T rows {len(t_rows)} > {args.max_t_rows}")
    elif len(t_rows) > args.warn_t_rows:
        warnings.append(f"T rows {len(t_rows)} > warning {args.warn_t_rows}")

    if len(mechanism_surfaces) > args.max_mechanism_surfaces:
        failures.append(
            f"§3.2 mechanism surfaces {len(mechanism_surfaces)} > "
            f"{args.max_mechanism_surfaces}"
        )

    if t_ids:
        max_t_id = max(t_ids)
        missing_t_ids = sorted(set(range(1, max_t_id + 1)) - t_ids)
        if missing_t_ids:
            warnings.append(
                "T row numeric gaps: missing "
                + ", ".join(f"T{num}" for num in missing_t_ids[:8])
                + (" ..." if len(missing_t_ids) > 8 else "")
            )
        check_t_reference_consistency(body_pairs, t_ids, max_t_id, warnings)

    for lineno, line in t_rows:
        row_len = len(line)
        pipes = count_unescaped_pipes(line)
        subcases = subcase_count(line)
        if row_len > args.max_t_row_chars:
            failures.append(
                f"line {lineno}: T row length {row_len} > "
                f"{args.max_t_row_chars}"
            )
        elif row_len > args.warn_t_row_chars:
            warnings.append(
                f"line {lineno}: T row length {row_len} > "
                f"warning {args.warn_t_row_chars}"
            )
        if subcases > args.max_t_row_subcases:
            failures.append(
                f"line {lineno}: T row subcases {subcases} > "
                f"{args.max_t_row_subcases}"
            )
        if pipes != 7:
            failures.append(
                f"line {lineno}: T row has {pipes} unescaped pipes; expected 7"
            )

    status = "PASS" if not failures else "FAIL"
    print(f"{status}: {path}")
    print(
        "  stats: "
        f"body_lines={body_count}, "
        f"T_rows={len(t_rows)}, "
        f"mechanism_surfaces={len(mechanism_surfaces)}, "
        f"exception={'yes' if exception else 'no'}"
    )
    for warning in warnings:
        print(f"  warning: {warning}")
    for failure in failures:
        print(f"  error: {failure}")

    return 1 if failures else 0


def check_t_reference_consistency(
    body_pairs: list[tuple[int, str]],
    t_ids: set[int],
    max_t_id: int,
    warnings: list[str],
) -> None:
    """Warn on stale-looking T references without rejecting partial phase ranges."""
    missing_refs: list[tuple[int, int]] = []
    stale_full_ranges: list[tuple[int, int]] = []
    reversed_ranges: list[tuple[int, int, int]] = []

    for lineno, line in body_pairs:
        ranges = list(T_RANGE_RE.finditer(line))
        for match in ranges:
            start = int(match.group("start"))
            end = int(match.group("end"))
            if start > end:
                reversed_ranges.append((lineno, start, end))
            for num in (start, end):
                if num not in t_ids:
                    missing_refs.append((lineno, num))

        if len(ranges) == 1:
            start = int(ranges[0].group("start"))
            end = int(ranges[0].group("end"))
            line_lower = line.lower()
            looks_like_full_set = (
                start == 1
                and end != max_t_id
                and " only" not in line_lower
                and FULL_RANGE_WORD_RE.search(line) is not None
            )
            if looks_like_full_set:
                stale_full_ranges.append((lineno, end))

        for match in T_REF_RE.finditer(line):
            num = int(match.group("num"))
            if num not in t_ids:
                missing_refs.append((lineno, num))

    for lineno, start, end in reversed_ranges[:5]:
        warnings.append(f"line {lineno}: T range T{start}-T{end} is reversed")

    seen_missing: set[tuple[int, int]] = set()
    for lineno, num in missing_refs:
        key = (lineno, num)
        if key in seen_missing:
            continue
        seen_missing.add(key)
        warnings.append(f"line {lineno}: references T{num}, but no T{num} row exists")
        if len(seen_missing) >= 8:
            remaining = len(set(missing_refs)) - len(seen_missing)
            if remaining > 0:
                warnings.append(f"{remaining} additional missing T references omitted")
            break

    for lineno, end in stale_full_ranges[:5]:
        warnings.append(
            f"line {lineno}: T1-T{end} looks like a full-row range, "
            f"but table ends at T{max_t_id}"
        )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path)
    parser.add_argument("--warn-body-lines", type=int, default=500)
    parser.add_argument("--max-body-lines", type=int, default=750)
    parser.add_argument("--warn-t-rows", type=int, default=12)
    parser.add_argument("--max-t-rows", type=int, default=20)
    parser.add_argument("--warn-t-row-chars", type=int, default=1500)
    parser.add_argument("--max-t-row-chars", type=int, default=2500)
    parser.add_argument("--max-t-row-subcases", type=int, default=8)
    parser.add_argument("--max-mechanism-surfaces", type=int, default=5)
    args = parser.parse_args(argv)

    rc = 0
    for path in args.paths:
        if check_file(path, args) != 0:
            rc = 1
    return rc


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
