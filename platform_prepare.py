#!/usr/bin/env python3
"""Prepare build tools and offline SWD dependencies."""
import hashlib
import importlib.metadata
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import zipfile

from toolchain_prepare import ensure_compiler


PLATFORM_ROOT = Path(__file__).resolve().parent
SWD_PACK = (PLATFORM_ROOT / "gd32h77x_os" / "tools" / "packs"
            / "GigaDevice.GD32H77x_78x_DFP.0.6.1.pack")
SWD_PACK_SHA256 = "94ed659f9adb58c881114858e9a13dba4a0540765c4d9e4d305a24e13ca5dee6"
SWD_REQUIREMENTS = PLATFORM_ROOT / "requirements-swd.txt"


def _find_compiler():
    """Prepare the host GCC or reuse an existing matching toolchain."""
    return ensure_compiler()


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _ensure_swd_pack() -> Path:
    override = os.environ.get("GD32H77X_DFP_PACK", "").strip()
    pack = Path(override).expanduser().resolve() if override else SWD_PACK
    if not pack.is_file():
        sys.exit(
            "Missing approved GD32H77x CMSIS-Pack. Expected platform repository "
            f"asset: {SWD_PACK}")
    actual = _sha256(pack)
    if actual != SWD_PACK_SHA256:
        sys.exit(
            f"GD32H77x CMSIS-Pack SHA256 mismatch: expected "
            f"{SWD_PACK_SHA256}, got {actual}: {pack}")
    return pack


def _ensure_pyocd() -> str:
    try:
        version = importlib.metadata.version("pyocd")
    except importlib.metadata.PackageNotFoundError:
        version = ""
    if version == "0.46.0":
        return version
    uv = os.environ.get("OPEN_SDK_UV", "").strip() or shutil.which("uv")
    if uv:
        command = [uv, "pip", "install", "--python", sys.executable,
                   "-r", str(SWD_REQUIREMENTS)]
        if os.environ.get("OPEN_COUNTRY_CODE", "").strip() == "China":
            command.extend(["--default-index", "https://mirrors.aliyun.com/pypi/simple/"])
    else:
        pip = os.environ.get("OPEN_SDK_PIP", "").strip()
        command = ([pip] if pip and Path(pip).is_file()
                   else [sys.executable, "-m", "pip"])
        command.extend(["install", "--disable-pip-version-check",
                        "-r", str(SWD_REQUIREMENTS)])
    try:
        subprocess.run(command, check=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        sys.exit(f"Cannot install pinned SWD dependencies from {SWD_REQUIREMENTS}: {exc}")
    try:
        version = importlib.metadata.version("pyocd")
    except importlib.metadata.PackageNotFoundError:
        sys.exit("pyocd installation completed but pyocd==0.46.0 is still unavailable")
    if version != "0.46.0":
        sys.exit(f"Installed pyocd version {version}; expected 0.46.0")
    return version


def main():
    try:
        compiler = _find_compiler()
    except (OSError, ValueError, zipfile.BadZipFile, tarfile.TarError) as exc:
        sys.exit(f"GD32H77X toolchain preparation failed: {exc}")
    if not compiler or not Path(compiler).is_file():
        sys.exit("Set GD32H77X_TOOLCHAIN to a GNU Arm Embedded installation, or add its bin to PATH")
    version = subprocess.check_output([str(compiler), "-dumpfullversion"], text=True).strip()
    if version != "10.3.1":
        sys.exit(f"GCC {version} is not validated; use GNU Arm Embedded 10.3-2021.10 (10.3.1)")
    supported_chips = {"GD32H77D", "GD32H779VW", "GD32H779IW"}
    if len(sys.argv) > 1 and sys.argv[1] not in supported_chips:
        sys.exit(f"Unsupported chip: {sys.argv[1]}")
    for command in ("cmake", "ninja"):
        if not shutil.which(command):
            sys.exit(f"Missing build tool: {command}")
    subprocess.run([str(compiler), "--version"], check=True)
    pack = _ensure_swd_pack()
    pyocd_version = _ensure_pyocd()
    print(f"GD32H77X: prepared with GCC {version}, pyocd {pyocd_version}; "
          f"DFP {pack.name}; OPEN_COUNTRY_CODE={os.environ.get('OPEN_COUNTRY_CODE', '')}")


if __name__ == "__main__":
    main()
