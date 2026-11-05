#!/usr/bin/env python3
"""Stage a board's GD32H77X config as the Tuya project config input."""

from __future__ import annotations

import argparse
import os
from pathlib import Path


PLATFORM = "GD32H77X"


def board_config_path(source_root: Path, board: str) -> Path:
    return (source_root / "boards" / PLATFORM / board / "platform_config"
            / "gd32h77x.config")


def stage_project_config(source_root: Path, board: str, output: Path) -> Path | None:
    """Copy the selected board config into a build-local project config path.

    A missing board config removes any stale staged file from a previous build so
    switching boards can never silently reuse old settings.
    """
    source = board_config_path(source_root.resolve(), board)
    output = output.resolve()
    if not source.is_file():
        output.unlink(missing_ok=True)
        return None

    data = source.read_bytes()
    if output.is_file() and output.read_bytes() == data:
        return output

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".tmp")
    temporary.write_bytes(data)
    os.replace(temporary, output)
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--board", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = stage_project_config(args.source_root, args.board, args.output)
    print(result if result is not None else "")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
