#!/usr/bin/env python3
"""Generate isolated host-test tables from the repository's selected H77 layout."""
import argparse
import importlib.util
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--platform-root", type=Path, required=True)
    parser.add_argument("--boards-root", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--chip", default="GD32H779VW")
    parser.add_argument("--board", default="GD32H779V_START")
    args = parser.parse_args()
    resolver_path = args.platform_root / "gd32h77x_os/tools/firmware/resolve_memory_layout.py"
    spec = importlib.util.spec_from_file_location("gd32h77x_layout_resolver", resolver_path)
    resolver = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(resolver)
    repo_root = args.boards_root.resolve().parent
    params = {
        "OPEN_ROOT": str(repo_root),
        "PLATFORM_CHIP": args.chip,
        "PLATFORM_BOARD": args.board,
        "CONFIG_PLATFORM_CHOICE": "GD32H77X",
        "CONFIG_BOARD_CHOICE": args.board,
        "CONFIG_CHIP_CHOICE": args.chip,
    }
    manifest = resolver.resolve_build_layout(
        params, platform_root=args.platform_root.resolve(), boards_root=args.boards_root.resolve())
    resolver.write_outputs(manifest, args.out_dir)


if __name__ == "__main__":
    main()
