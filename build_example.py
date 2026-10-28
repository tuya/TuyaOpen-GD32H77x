#!/usr/bin/env python3
"""Build the firmware using the parameter directory supplied by TuyaOpen."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys


def make_package_command(data, python, package_script, resolved_layout,
                         report, elf, internal_bin, output, external_csv,
                         ecnvm_bin=None):
    """Build the package CLI command from explicit build parameters."""
    internal_only = data.get("CONFIG_GD32H77X_PACKAGE_INTERNAL_ONLY", False)
    if not isinstance(internal_only, bool):
        raise ValueError("CONFIG_GD32H77X_PACKAGE_INTERNAL_ONLY must be boolean")
    command = [
        str(python), str(package_script), "create",
        "--layout", str(resolved_layout), "--report", str(report),
        "--elf", str(elf), "--internal-bin", str(internal_bin),
        "--output", str(output),
    ]
    if not internal_only and Path(external_csv).is_file():
        command.extend(["--external-csv", str(external_csv)])
    if ecnvm_bin is not None:
        command.extend(["--ecnvm-bin", str(ecnvm_bin)])
    return command, not internal_only


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("parameters", type=Path)
    parser.add_argument("command", choices=("build", "clean"))
    args = parser.parse_args()
    parameters = args.parameters.resolve()
    # Keep intermediate files per application, never in a shared platform build tree.
    build = parameters.parent / "gd32h77x"
    if args.command == "clean":
        shutil.rmtree(build, ignore_errors=True)
        return
    data = json.loads((parameters / "build_param.json").read_text())
    supported_targets = {
        ("GD32H77D_GINO", "GD32H77D"),
        ("GD32H779V_START", "GD32H779VW"),
    }
    if (data["PLATFORM_BOARD"], data["PLATFORM_CHIP"]) not in supported_targets:
        parser.error("Unsupported GD32H77X board/chip combination: {}/{}".format(
            data["PLATFORM_BOARD"], data["PLATFORM_CHIP"]))
    root = Path(__file__).resolve().parent
    output = Path(data["BIN_OUTPUT_DIR"])
    package_output = output / f"{data['CONFIG_PROJECT_NAME']}_QIO_{data['CONFIG_PROJECT_VERSION']}.tufw"
    dist_package = (parameters.parent.parent / "dist"
                    / f"{data['CONFIG_PROJECT_NAME']}_{data['CONFIG_PROJECT_VERSION']}"
                    / package_output.name)
    # Never leave a previous external-payload package selectable if this build
    # later fails while recompiling or validating the replacement.
    package_output.unlink(missing_ok=True)
    dist_package.unlink(missing_ok=True)
    subprocess.run(["cmake", "-S", str(root), "-B", str(build), "-G", "Ninja",
                    f"-DBUILD_PARAM_PATH={parameters}", f"-DTOS_PYTHON={sys.executable}",
                    f"-DCMAKE_TOOLCHAIN_FILE={root / 'toolchain_file.cmake'}"], check=True)
    subprocess.run(["cmake", "--build", str(build), "--parallel", "4"], check=True)
    report = build / "firmware.layout.json"
    resolved_layout = build / "generated/memory/resolved_layout.json"
    bin_name = f"{data['CONFIG_PROJECT_NAME']}_QIO_{data['CONFIG_PROJECT_VERSION']}.bin"
    elf_name = f"{data['CONFIG_PROJECT_NAME']}_{data['CONFIG_PROJECT_VERSION']}.elf"
    ecnvm_name = f"{data['CONFIG_PROJECT_NAME']}_{data['CONFIG_PROJECT_VERSION']}.ecnvm.bin"
    (output / ecnvm_name).unlink(missing_ok=True)
    verify_command = [sys.executable, str(root / "gd32h77x_os/tools/firmware/verify_image.py"),
                      str(build / "firmware.elf"), str(build / "firmware.bin"),
                      "--resolved-layout", str(resolved_layout),
                      "--bin-name", bin_name, "--elf-name", elf_name,
                      "--output", str(report)]
    ecnvm_enabled = data.get("CONFIG_GD32H77X_ECNVM_CODE", False)
    if not isinstance(ecnvm_enabled, bool):
        raise ValueError("CONFIG_GD32H77X_ECNVM_CODE must be boolean")
    ecnvm_bin = build / "ecnvm_code.bin"
    if ecnvm_enabled:
        if not ecnvm_bin.is_file():
            raise ValueError("ECNVM code is enabled but the separate ECNVM image is missing")
        verify_command.extend(["--ecnvm-bin", str(ecnvm_bin), "--ecnvm-enabled"])
    subprocess.run(verify_command, check=True)
    output.mkdir(parents=True, exist_ok=True)
    # Remove the temporary pre-QIO name used during initial bring-up.
    legacy = output / f"{data['CONFIG_PROJECT_NAME']}_{data['CONFIG_PROJECT_VERSION']}.bin"
    if legacy.exists():
        legacy.unlink()
    for extension in ("elf", "map"):
        shutil.copy2(build / f"firmware.{extension}", output / f"{data['CONFIG_PROJECT_NAME']}_{data['CONFIG_PROJECT_VERSION']}.{extension}")
    # QIO is the TuyaOpen artifact kind for a full image, not the H77 flash bus mode.
    shutil.copy2(build / "firmware.bin", output / bin_name)
    if ecnvm_enabled:
        shutil.copy2(ecnvm_bin, output / ecnvm_name)
    # Preserve the established report name consumed by TuyaOpen tooling.
    report_output = output / f"{data['CONFIG_PROJECT_NAME']}_{data['CONFIG_PROJECT_VERSION']}.layout.json"
    shutil.copy2(report, report_output)
    if ecnvm_enabled:
        report_data = json.loads(report_output.read_text())
        report_data["mapped_payloads"][0]["file"] = ecnvm_name
        report_output.write_text(json.dumps(report_data, indent=2) + "\n")

    # Emit a self-contained flash artifact beside the legacy BIN. External
    # payloads are opt-in through the app-root CSV; creating the package on
    # every build also replaces any prior package, so removing the CSV cannot
    # leave stale external payloads in the distributable.
    package_script = root / "gd32h77x_os/tools/firmware/flash_package.py"
    external_csv = parameters.parent.parent / "flash_payloads.csv"
    package_command, include_external = make_package_command(
        data, sys.executable, package_script, resolved_layout, report_output,
        output / elf_name, output / bin_name, package_output, external_csv,
        output / ecnvm_name if ecnvm_enabled else None)
    if external_csv.exists() and not include_external:
        print("External payload CSV is present but excluded by explicit build configuration")
    subprocess.run(package_command, check=True)


if __name__ == "__main__":
    main()
