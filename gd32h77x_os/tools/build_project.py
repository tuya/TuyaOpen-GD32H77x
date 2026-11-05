#!/usr/bin/env python3
"""Build a standalone GD32H77X application."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


OPEN_ROOT = Path(__file__).resolve().parents[4]
PLATFORM_ROOT = OPEN_ROOT / "platform" / "GD32H77X"
BUILD_FIRMWARE = PLATFORM_ROOT / "build_firmware.py"


def _cmake_set(name, value):
    value = str(value).replace("\\", "\\\\").replace('"', '\\"')
    return f'set({name} "{value}")'


def _write_build_params(project, parameters, name, version, chip, board):
    build = project / ".build"
    values = {
        "OPEN_ROOT": OPEN_ROOT,
        "OPEN_HEADER_DIR": "",
        "OPEN_LIBS_DIR": "",
        "PLATFORM_NEED_LIBS": "",
        "BIN_OUTPUT_DIR": build / "bin",
        "PLATFORM_CHIP": chip,
        "PLATFORM_BOARD": board,
        "CONFIG_PROJECT_NAME": name,
        "CONFIG_PROJECT_VERSION": version,
        "CONFIG_PLATFORM_CHOICE": "GD32H77X",
        "CONFIG_CHIP_CHOICE": chip,
        "CONFIG_BOARD_CHOICE": board,
    }
    parameters.mkdir(parents=True, exist_ok=True)
    (parameters / "build_param.json").write_text(
        json.dumps({key: str(value) for key, value in values.items()}, indent=4) + "\n",
        encoding="utf-8")
    (parameters / "build_param.cmake").write_text(
        "\n".join(_cmake_set(key, value) for key, value in values.items()) + "\n",
        encoding="utf-8")
    context = [_cmake_set("GD32H77X_PROJECT_DIR", project)]
    project_config = project / "project.config"
    if project_config.is_file():
        context.append(_cmake_set("GD32H77X_PROJECT_CONFIG", project_config))
    context.extend([
        "set(GD32H77X_EXTERNAL_INCLUDE_DIRS)",
        "set(GD32H77X_EXTERNAL_LIBRARIES)",
    ])
    (parameters / "gd32h77x_project.cmake").write_text(
        "\n".join(context) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", nargs="?", type=Path, default=Path.cwd(),
                        help="native project directory (default: current directory)")
    parser.add_argument("command", nargs="?", choices=("build", "clean"), default="build")
    parser.add_argument("--chip", help="target platform chip, for example GD32H779VW")
    parser.add_argument("--board", help="target board, for example GD32H779V_START")
    parser.add_argument("--name", help="artifact project name (default: project directory name)")
    parser.add_argument("--version", default="1.0.0", help="artifact project version")
    args = parser.parse_args()
    project = args.project.resolve()
    build = project / ".build"
    if args.command == "clean":
        shutil.rmtree(build, ignore_errors=True)
        return

    if not args.chip or not args.board:
        parser.error("--chip and --board are required for build")
    parameters = build / "build"
    _write_build_params(project, parameters, args.name or project.name, args.version,
                        args.chip, args.board)
    # The platform Kconfig resolver is independent from TuyaOpen, but reuses
    # the repository's vendored kconfiglib package instead of installing a
    # second copy into the host Python environment.
    env = os.environ.copy()
    pythonpath = str(OPEN_ROOT / "tools" / "kconfiglib")
    if env.get("PYTHONPATH"):
        pythonpath += os.pathsep + env["PYTHONPATH"]
    env["PYTHONPATH"] = pythonpath
    subprocess.run([sys.executable, str(BUILD_FIRMWARE), str(parameters), "build"],
                   check=True, env=env)


if __name__ == "__main__":
    main()
