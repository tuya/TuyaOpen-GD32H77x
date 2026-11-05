#!/usr/bin/env python3
"""Format GD32H77X layout CSV files with aligned columns."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
import sys


def _parse_csv_line(line: str) -> list[str]:
    try:
        row = next(csv.reader([line], strict=True))
    except csv.Error as exc:
        raise ValueError(f"invalid CSV: {exc}") from exc
    return [cell.strip() for cell in row]


def format_csv_text(text: str) -> str:
    """Return a deterministic, column-aligned representation of CSV text."""
    records: list[tuple[str, str | list[str]]] = []
    data_rows: list[list[str]] = []
    for line in text.splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            records.append(("raw", line))
            continue
        row = _parse_csv_line(line)
        records.append(("row", row))
        data_rows.append(row)

    if not data_rows:
        return "\n".join(str(value) for _kind, value in records) + "\n"

    column_count = len(data_rows[0])
    if any(len(row) != column_count for row in data_rows):
        raise ValueError("all CSV rows must have the same number of columns")
    if any("," in cell or "\n" in cell or "\r" in cell
           for row in data_rows for cell in row):
        raise ValueError("layout CSV values cannot contain commas or newlines")

    widths = [
        max(len(row[index]) for row in data_rows)
        for index in range(column_count)
    ]
    formatted: list[str] = []
    for kind, value in records:
        if kind == "raw":
            formatted.append(str(value))
            continue
        row = value
        line = row[0]
        for index in range(1, column_count):
            line += "," + " " * (widths[index - 1] - len(row[index - 1]) + 1)
            line += row[index]
        formatted.append(line.rstrip())
    return "\n".join(formatted) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path,
                        help="layout CSV files to format")
    parser.add_argument("--check", action="store_true",
                        help="fail instead of writing when a file is not formatted")
    args = parser.parse_args()

    failures = 0
    for path in args.paths:
        try:
            original = path.read_text(encoding="utf-8")
            formatted = format_csv_text(original)
        except (OSError, UnicodeDecodeError, ValueError) as exc:
            print(f"{path}: {exc}", file=sys.stderr)
            failures += 1
            continue
        if original == formatted:
            continue
        if args.check:
            print(f"{path}: layout CSV columns are not aligned", file=sys.stderr)
            failures += 1
        else:
            path.write_text(formatted, encoding="utf-8")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
