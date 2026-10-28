#!/usr/bin/env python3
"""Check local dependencies without downloads or region re-detection."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def _find_compiler():
    """Locate a GNU Arm Embedded compiler: env override, PATH, then /opt."""
    override = os.environ.get("GD32H77X_TOOLCHAIN", "").strip()
    if override:
        return Path(override) / "bin/arm-none-eabi-gcc"
    found = shutil.which("arm-none-eabi-gcc")
    if found:
        return Path(found)
    for base in sorted(Path("/opt").glob("gcc-arm-none-eabi-*")):
        candidate = base / "bin/arm-none-eabi-gcc"
        if candidate.is_file():
            return candidate
    return None


def main():
    root = Path(__file__).resolve().parent
    compiler = _find_compiler()
    if not compiler or not Path(compiler).is_file():
        sys.exit("Set GD32H77X_TOOLCHAIN to a GNU Arm Embedded installation, or add its bin to PATH")
    version = subprocess.check_output([str(compiler), "-dumpfullversion"], text=True).strip()
    if version != "10.3.1":
        sys.exit(f"GCC {version} is not validated; use GNU Arm Embedded 10.3-2021.10 (10.3.1)")
    supported_chips = {"GD32H77D", "GD32H779VW"}
    if len(sys.argv) > 1 and sys.argv[1] not in supported_chips:
        sys.exit(f"Unsupported chip: {sys.argv[1]}")
    for command in ("cmake", "ninja"):
        if not shutil.which(command):
            sys.exit(f"Missing build tool: {command}")
    manifest_path = root / "gd32h77x_os/manifest.json"
    if not manifest_path.exists():
        sys.exit("Import the specified SDK with gd32h77x_os/tools/firmware/import_sdk.py first; see README.md")
    manifest = json.loads(manifest_path.read_text())
    for relative, info in manifest["files"].items():
        path = root / "gd32h77x_os" / relative
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != info["sha256"]:
            sys.exit(f"Vendor file differs from pinned SDK: {relative}")
    subprocess.run([str(compiler), "--version"], check=True)
    print(f"GD32H77X: verified {len(manifest['files'])} vendor files; OPEN_COUNTRY_CODE={os.environ.get('OPEN_COUNTRY_CODE', '')}")


if __name__ == "__main__":
    main()
