#!/usr/bin/env python3
"""Build, verify, and package one GD32H77X firmware project."""

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys


def _layout_has_ecnvm_code(layout_path):
    layout = json.loads(Path(layout_path).read_text(encoding="utf-8"))
    entry = layout.get("layout", {}).get("partitions", {}).get("ecnvm_code")
    return isinstance(entry, dict) and int(entry.get("size_bytes", 0)) > 0


def make_package_command(python, package_script, resolved_layout,
                         report, elf, internal_bin, output, external_csv,
                         ecnvm_bin=None):
    """Build the package CLI command; the CSV itself opts into external payloads."""
    command = [
        str(python), str(package_script), "create",
        "--layout", str(resolved_layout), "--report", str(report),
        "--elf", str(elf), "--internal-bin", str(internal_bin),
        "--output", str(output),
    ]
    if Path(external_csv).is_file():
        command.extend(["--external-csv", str(external_csv)])
    if ecnvm_bin is not None:
        command.extend(["--ecnvm-bin", str(ecnvm_bin)])
    return command


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("parameters", type=Path)
    parser.add_argument("command", choices=("build", "clean"))
    args = parser.parse_args()
    parameters = args.parameters.resolve()
    build = parameters.parent / "gd32h77x"
    if args.command == "clean":
        shutil.rmtree(build, ignore_errors=True)
        build_params = parameters / "build_param.json"
        if build_params.is_file():
            try:
                data = json.loads(build_params.read_text(encoding="utf-8"))
                output = Path(data.get("BIN_OUTPUT_DIR", "")).resolve()
                build_root = parameters.parent.resolve()
                if output != build_root and build_root in output.parents:
                    shutil.rmtree(output, ignore_errors=True)
            except (OSError, ValueError, TypeError, json.JSONDecodeError):
                pass
        return

    data = json.loads((parameters / "build_param.json").read_text())
    supported_targets = {
        ("GD32H779V_START", "GD32H779VW"),
    }
    if (data["PLATFORM_BOARD"], data["PLATFORM_CHIP"]) not in supported_targets:
        parser.error("Unsupported GD32H77X board/chip combination: {}/{}".format(
            data["PLATFORM_BOARD"], data["PLATFORM_CHIP"]))
    root = Path(__file__).resolve().parent
    output = Path(data["BIN_OUTPUT_DIR"])
    project_name = data["CONFIG_PROJECT_NAME"]
    project_version = data["CONFIG_PROJECT_VERSION"]
    stale_tufw = output / f"{project_name}_QIO_{project_version}.tufw"
    stale_dist_tufw = (parameters.parent.parent / "dist" / f"{project_name}_{project_version}"
                       / stale_tufw.name)
    stale_tufw.unlink(missing_ok=True)
    stale_dist_tufw.unlink(missing_ok=True)

    project_context = parameters / "gd32h77x_project.cmake"
    if not project_context.is_file():
        raise ValueError(f"Missing GD32H77X project context: {project_context}")
    subprocess.run(["cmake", "-S", str(root), "-B", str(build), "-G", "Ninja",
                    f"-DBUILD_PARAM_PATH={parameters}", f"-DTOS_PYTHON={sys.executable}",
                    f"-DGD32H77X_PROJECT_CONTEXT={project_context}",
                    f"-DCMAKE_TOOLCHAIN_FILE={root / 'toolchain_file.cmake'}"], check=True)
    subprocess.run(["cmake", "--build", str(build), "--parallel", "4"], check=True)

    report = build / "firmware.layout.json"
    resolved_layout = build / "generated/memory/resolved_layout.json"
    bin_name = f"{project_name}_QIO_{project_version}.bin"
    elf_name = f"{project_name}_{project_version}.elf"
    ecnvm_name = f"{project_name}_{project_version}.ecnvm.bin"
    (output / ecnvm_name).unlink(missing_ok=True)
    verify_command = [sys.executable,
                      str(root / "gd32h77x_os/tools/firmware/verify_image.py"),
                      str(build / "firmware.elf"), str(build / "firmware.bin"),
                      "--resolved-layout", str(resolved_layout),
                      "--bin-name", bin_name, "--elf-name", elf_name,
                      "--output", str(report)]
    ecnvm_enabled = _layout_has_ecnvm_code(resolved_layout)
    ecnvm_bin = build / "ecnvm_code.bin"
    if ecnvm_enabled:
        if not ecnvm_bin.is_file():
            raise ValueError("ECNVM code is enabled but the separate ECNVM image is missing")
        verify_command.extend(["--ecnvm-bin", str(ecnvm_bin), "--ecnvm-enabled"])
    subprocess.run(verify_command, check=True)

    output.mkdir(parents=True, exist_ok=True)
    debug_output = output / "debug"
    shutil.rmtree(debug_output, ignore_errors=True)
    debug_output.mkdir(parents=True)
    for stale in (
            output / f"{project_name}_{project_version}.bin",
            output / bin_name,
            output / elf_name,
            output / f"{project_name}_{project_version}.map",
            output / f"{project_name}_{project_version}.layout.json",
            output / ecnvm_name):
        stale.unlink(missing_ok=True)
    for extension in ("elf", "map"):
        shutil.copy2(build / f"firmware.{extension}",
                     debug_output / f"{project_name}_{project_version}.{extension}")
    report_output = debug_output / f"{project_name}_{project_version}.layout.json"
    shutil.copy2(report, report_output)
    if ecnvm_enabled:
        report_data = json.loads(report_output.read_text())
        report_data["mapped_payloads"][0]["file"] = ecnvm_name
        report_output.write_text(json.dumps(report_data, indent=2) + "\n")

    # QIO is the complete TuyaOpen delivery artifact for this platform. Keep a
    # correctly named temporary raw image because the package report binds the
    # internal container member to the public QIO filename.
    package_stage = build / "qio-package-input"
    shutil.rmtree(package_stage, ignore_errors=True)
    package_stage.mkdir(parents=True)
    internal_qio = package_stage / bin_name
    shutil.copy2(build / "firmware.bin", internal_qio)
    staged_ecnvm = None
    if ecnvm_enabled:
        staged_ecnvm = package_stage / ecnvm_name
        shutil.copy2(ecnvm_bin, staged_ecnvm)
    package_script = root / "gd32h77x_os/tools/firmware/flash_package.py"
    external_csv = parameters.parent.parent / "flash_payloads.csv"
    package_command = make_package_command(
        sys.executable, package_script, resolved_layout, report_output,
        debug_output / elf_name, internal_qio, output / bin_name, external_csv,
        staged_ecnvm)
    try:
        subprocess.run(package_command, check=True)
    finally:
        shutil.rmtree(package_stage, ignore_errors=True)


if __name__ == "__main__":
    main()
