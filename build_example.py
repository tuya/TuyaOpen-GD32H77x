#!/usr/bin/env python3
"""TuyaOpen build hook for the GD32H77X external-application project."""

import argparse
import json
from pathlib import Path
import shlex
import subprocess
import sys


PLATFORM_ROOT = Path(__file__).resolve().parent
OPEN_ROOT = PLATFORM_ROOT.parents[1]
PROJECT_DIR = PLATFORM_ROOT / "gd32h77x_os" / "project" / "tuya"
BUILD_FIRMWARE = PLATFORM_ROOT / "build_firmware.py"

if str(PLATFORM_ROOT) not in sys.path:
    sys.path.insert(0, str(PLATFORM_ROOT))
from tuya_project_config import stage_project_config


def _cmake_set(name, value):
    value = str(value).replace("\\", "\\\\").replace('"', '\\"')
    return f'set({name} "{value}")'


def _write_project_context(parameters, data, project_config=None):
    includes = shlex.split(data.get("OPEN_HEADER_DIR", ""))
    libraries = []
    library_dir = data.get("OPEN_LIBS_DIR", "")
    for library in shlex.split(data.get("PLATFORM_NEED_LIBS", "")):
        libraries.append(str(Path(library_dir) / f"lib{library}.a"))

    lines = [_cmake_set("GD32H77X_PROJECT_DIR", PROJECT_DIR)]
    if project_config is not None:
        lines.append(_cmake_set("GD32H77X_PROJECT_CONFIG", project_config))
    lines.append("set(GD32H77X_EXTERNAL_INCLUDE_DIRS")
    lines.extend(f'    "{path}"' for path in includes)
    lines.append(")")
    lines.append("set(GD32H77X_EXTERNAL_LIBRARIES")
    lines.extend(f'    "{path}"' for path in libraries)
    lines.append(")")
    (parameters / "gd32h77x_project.cmake").write_text("\n".join(lines) + "\n",
                                                        encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("parameters", type=Path)
    parser.add_argument("command", choices=("build", "clean"))
    args = parser.parse_args()
    parameters = args.parameters.resolve()
    if args.command == "build":
        data = json.loads((parameters / "build_param.json").read_text())
        project_config = stage_project_config(
            OPEN_ROOT, data["PLATFORM_BOARD"],
            parameters.parent / "gd32h77x" / "project" / "tuya" / "project.config")
        _write_project_context(parameters, data, project_config)
    subprocess.run([sys.executable, str(BUILD_FIRMWARE), str(parameters), args.command],
                   check=True)


if __name__ == "__main__":
    main()
