#!/usr/bin/env python3
"""Validate standalone GD32H77X projects and their build parameters."""

import importlib.util
import json
from pathlib import Path
import tempfile


PLATFORM = Path(__file__).resolve().parents[1]
PROJECT = PLATFORM / "gd32h77x_os" / "project" / "at_bridge"
SCRIPT = PLATFORM / "gd32h77x_os" / "tools" / "build_project.py"
DIAG = PLATFORM / "gd32h77x_os" / "project" / "board_diag"
RESET = PLATFORM / "gd32h77x_os" / "project" / "at_reset"
TUYA = PLATFORM / "gd32h77x_os" / "project" / "tuya"


def _load_builder():
    spec = importlib.util.spec_from_file_location("gd32h77x_build_project", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    builder = _load_builder()
    for project, name in ((PROJECT, "at_bridge"), (DIAG, "board_diag"),
                          (RESET, "at_reset"), (TUYA, "tuya")):
        assert project.is_dir()
        assert (project / "project.cmake").is_file()
        assert not (project / "project.json").exists()

    with tempfile.TemporaryDirectory(prefix="gd32h77x-native-build-") as temporary:
        parameters = Path(temporary) / "build"
        builder._write_build_params(PROJECT, parameters, "at_bridge", "1.0.0",
                                    "GD32H779VW", "GD32H779V_START")
        data = json.loads((parameters / "build_param.json").read_text())
        assert data["PLATFORM_CHIP"] == "GD32H779VW"
        assert data["PLATFORM_BOARD"] == "GD32H779V_START"
        assert data["CONFIG_PROJECT_NAME"] == "at_bridge"
        assert data["PLATFORM_NEED_LIBS"] == ""
        assert data["OPEN_HEADER_DIR"] == ""
        assert data["OPEN_LIBS_DIR"] == ""
        assert "TOS_PROJECT_ROOT" not in data
        context = (parameters / "gd32h77x_project.cmake").read_text()
        assert str(PROJECT) in context
        assert "GD32H77X_PROJECT_CONFIG" not in context
        assert "GD32H77X_EXTERNAL_INCLUDE_DIRS" in context
        assert "GD32H77X_EXTERNAL_LIBRARIES" in context

    with tempfile.TemporaryDirectory(prefix="gd32h77x-native-project-") as temporary:
        project = Path(temporary)
        (project / "project.config").write_text(
            "CONFIG_GD32H77X_AT_BAUD=460800\n", encoding="utf-8")
        parameters = project / ".build/build"
        builder._write_build_params(project, parameters, "custom", "1.0.0",
                                    "GD32H779VW", "GD32H779V_START")
        context = (parameters / "gd32h77x_project.cmake").read_text()
        assert "CONFIG_GD32H77X_PROJECT_CONFIG" not in context
        assert "GD32H77X_PROJECT_CONFIG" in context
        assert str(project / "project.config") in context

    project_cmake = (PROJECT / "project.cmake").read_text()
    assert "at_bridge_app.c" in project_cmake
    assert "gd32h77x_at_bridge.c" in project_cmake
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
