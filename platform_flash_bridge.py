#!/usr/bin/env python3
"""Geometry-driven flash bridge for the GD32H77X platform.

Flash settings come from the platform-owned Kconfig. Target identity is derived from
``CHIP_CHOICE`` and the build-bound canonical device registry:

``dfu``
    Native DfuSe over libusb. Erase sectors and capacity come from the resolved
    platform memory registry, never from the ROM's DfuSe descriptor (the H77x
    ROM advertises 4 KiB pages although the NVMC has 64 KiB CNVM sectors).
``swd``
    The vendor scheme: the CMSIS-Pack flash algorithm for the selected device
    through ``pyocd``.
``uart``
    The ROM system-memory bootloader over the board console UART.  It writes
    the internal CNVM image and explicit OSPI0 NOR payloads without SWD.

External OSPI NOR payloads require ``swd`` or ``uart``.  ``dfu`` rejects a
package that carries external payloads before opening any device, so it never
touches the NOR.

All backends consume the same build-bound layout report and resolved CNVM
geometry. No backend knows a board-specific erase size or tool hash.
"""

from __future__ import annotations

import hashlib
import importlib.util
import importlib.metadata
import os
import re
import shutil
import subprocess
import sys
import tempfile
import json
import csv
import zipfile
from pathlib import Path
from typing import Dict, List, Tuple


sys.path.insert(0, str(Path(__file__).resolve().parent))
from platform_config import backend_from_config, resolve_config


_PLATFORM = "GD32H77X"
_BACKENDS = ("dfu", "swd", "uart")

# GD32 ROM DFU protocol constants. These are fixed by the ROM loader and are
# not board/application configuration.
_DFU_USB_VID = "28e9"
_DFU_USB_PID = "0189"
_DFU_ALT = "0"
_DFU_ALT_NAME = "InternalFlash"
_UART_ISP_ERASE_MODE = "extended"
_SWD_DEFAULT_FREQUENCY_HZ = 10_000_000
_SWD_MAX_FREQUENCY_HZ = 50_000_000
_DFUSE_MODULE = None
_FLASH_PACKAGE_MODULE = None
_OSPI_MODULE = None
_UART_ISP_MODULE = None
_SWD_PACK_NAME = "GigaDevice.GD32H77x_78x_DFP.0.6.1.pack"


def _resolve_swd_pack(config=None) -> str:
    """Resolve the approved DFP with environment override and local auto-discovery."""
    configured = ((config or {}).get("CONFIG_FLASH_SWD_PACK", "").strip()
                  or os.environ.get("GD32H77X_DFP_PACK", "").strip())
    if configured:
        return str(Path(configured).expanduser().resolve())

    platform_root = Path(__file__).resolve().parent
    open_root = platform_root.parents[1]
    home = Path.home()
    direct_candidates = (
        platform_root / "gd32h77x_os" / "tools" / "packs" / _SWD_PACK_NAME,
        platform_root / "tools" / "packs" / _SWD_PACK_NAME,
        open_root / "tools" / "packs" / _SWD_PACK_NAME,
    )
    search_roots = (
        open_root / ".tools",
        open_root / ".cache",
        home / ".cache" / "pyocd" / "packs",
        home / ".pyocd" / "packs",
    )
    for candidate in direct_candidates:
        if candidate.is_file():
            return str(candidate.resolve())
    for root in search_roots:
        if not root.is_dir():
            continue
        for candidate in sorted(root.rglob(_SWD_PACK_NAME)):
            if candidate.is_file():
                return str(candidate.resolve())

    # Development workspaces often keep the vendor SDK beside TuyaOpen.
    workspace = platform_root.parents[2]
    sibling_sdk = workspace / "H779_553"
    if sibling_sdk.is_dir():
        for candidate in sorted(sibling_sdk.rglob(_SWD_PACK_NAME)):
            if candidate.is_file():
                return str(candidate.resolve())
    return ""


# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

def _platform_config(using_data: dict, boards_root: str) -> Dict[str, str]:
    board = using_data.get("CONFIG_BOARD_CHOICE", "")
    chip = _chip_from_using(using_data)
    project_config = (Path(boards_root) / _PLATFORM / board / "platform_config"
                      / "gd32h77x.config")
    return resolve_config(
        Path(__file__).resolve().parent, chip,
        project_config if project_config.is_file() else None)


def _uart_expected_id(platform_root: Path, canonical_chip: str) -> str:
    path = (platform_root / "gd32h77x_os" / "chips" / canonical_chip
            / "uart_isp.json")
    if not path.is_file():
        return ""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"Invalid UART ISP identity file {path}: {exc}") from exc
    expected_id = data.get("expected_id", "")
    if not isinstance(expected_id, str) or not expected_id.strip():
        raise ValueError(f"Invalid expected_id in {path}")
    return expected_id.strip()


def _board_flash_config(using_data: dict, boards_root: str) -> Dict[str, str]:
    platform = using_data.get("CONFIG_PLATFORM_CHOICE", "")
    board = using_data.get("CONFIG_BOARD_CHOICE", "")
    if not platform or not board:
        raise ValueError("Platform and board must be selected before flashing")
    if platform != _PLATFORM:
        raise ValueError(
            f"GD32H77X flash bridge called for platform {platform}")

    platform_root = Path(__file__).resolve().parent
    platform_config = _platform_config(using_data, boards_root)
    backend = backend_from_config(platform_config)
    identity = _load_platform_device(_chip_from_using(using_data))
    canonical_chip = identity["canonical_chip"]
    board_dir = Path(boards_root) / platform / board / "platform_config"
    config: Dict[str, str] = {
        "CONFIG_FLASH_BACKEND": backend,
        "CONFIG_FLASH_VERIFY": "y",
        "CONFIG_FLASH_USB_VID": _DFU_USB_VID,
        "CONFIG_FLASH_USB_PID": _DFU_USB_PID,
        "CONFIG_FLASH_USB_SERIAL": "",
        "CONFIG_FLASH_DFU_ALT": _DFU_ALT,
        "CONFIG_FLASH_DFU_ALT_NAME": _DFU_ALT_NAME,
        "CONFIG_FLASH_DFU_LEAVE": platform_config.get(
            "CONFIG_ENABLE_GD32H77X_FLASH_DFU_AUTOSTART", "n"),
        "CONFIG_FLASH_UART_PORT": "",
        "CONFIG_FLASH_UART_BAUD": "auto",
        "CONFIG_FLASH_UART_EXPECTED_ID": _uart_expected_id(
            platform_root, canonical_chip),
        "CONFIG_FLASH_UART_OSPI_PROFILES": "",
        "CONFIG_FLASH_UART_VERIFY": "",
        "CONFIG_FLASH_UART_ERASE_MODE": _UART_ISP_ERASE_MODE,
        "CONFIG_FLASH_SWD_PACK": _resolve_swd_pack(),
        "CONFIG_FLASH_SWD_PROBE": "",
        "CONFIG_FLASH_SWD_FREQUENCY_HZ": _swd_frequency(platform_config),
        "CONFIG_FLASH_TARGET_CHIP": canonical_chip,
        "CONFIG_FLASH_SWD_TARGET": canonical_chip,
        "_CONFIG_DIR": str(board_dir if board_dir.is_dir()
                           else platform_root),
    }
    profiles = board_dir / "ospi_profiles.csv"
    if profiles.is_file():
        config["CONFIG_FLASH_UART_OSPI_PROFILES"] = profiles.name
    return config


def _required(config: Dict[str, str], key: str) -> str:
    value = config.get(key, "").strip()
    if not value:
        raise ValueError(f"Missing {key} in GD32H77X flash configuration")
    return value


def _boolean(config: Dict[str, str], key: str, default: bool) -> bool:
    value = config.get(key, "").strip().lower()
    if not value:
        return default
    if value in ("y", "yes", "true", "1"):
        return True
    if value in ("n", "no", "false", "0"):
        return False
    raise ValueError(f"{key} must be y or n, got {value!r}")


def _backend(config: Dict[str, str]) -> str:
    backend = (config.get("CONFIG_FLASH_BACKEND", "swd").strip() or "swd").lower()
    if backend == "dfuse":
        backend = "dfu"
    if backend in ("serial", "isp"):
        backend = "uart"
    if backend == "dfu-util":
        raise ValueError(
            "The dfu-util backend is disabled for GD32H77X: stock-tool erase behavior "
            "against the ROM's 4 KiB descriptor is not verified for 64 KiB CNVM sectors. "
            "Use native 'dfu', 'swd', or 'uart'.")
    if backend not in _BACKENDS:
        raise ValueError(
            "Unsupported flash backend '{}'; choose {}".format(
                backend, ", ".join(_BACKENDS)))
    return backend


def _swd_frequency(config: Dict[str, str]) -> str:
    value = os.environ.get("GD32H77X_SWD_FREQ_HZ", "").strip()
    if not value:
        value = config.get("CONFIG_FLASH_SWD_FREQUENCY_HZ", "").strip()
    if not value:
        value = str(_SWD_DEFAULT_FREQUENCY_HZ)
    try:
        frequency = int(value, 0)
    except ValueError as exc:
        raise ValueError(
            f"Invalid SWD frequency {value!r}; use an integer in Hz") from exc
    if frequency <= 0 or frequency > _SWD_MAX_FREQUENCY_HZ:
        raise ValueError(
            f"SWD frequency must be between 1 and {_SWD_MAX_FREQUENCY_HZ} Hz")
    return str(frequency)


def _flash_package_module():
    global _FLASH_PACKAGE_MODULE
    if _FLASH_PACKAGE_MODULE is None:
        path = Path(__file__).resolve().parent / "gd32h77x_os/tools/firmware/flash_package.py"
        spec = importlib.util.spec_from_file_location("gd32h77x_flash_package", path)
        if spec is None or spec.loader is None:
            raise ValueError(f"GD32H77X flash package validator is unavailable: {path}")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        _FLASH_PACKAGE_MODULE = module
    return _FLASH_PACKAGE_MODULE


def _ospi_module():
    global _OSPI_MODULE
    if _OSPI_MODULE is None:
        path = Path(__file__).resolve().parent / "gd32h77x_os/tools/download/gd32h77x_ospi.py"
        spec = importlib.util.spec_from_file_location("gd32h77x_ospi_writer", path)
        if spec is None or spec.loader is None:
            raise ValueError(f"GD32H77X OSPI writer is unavailable: {path}")
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        try:
            spec.loader.exec_module(module)
        except Exception:
            if sys.modules.get(spec.name) is module:
                sys.modules.pop(spec.name, None)
            raise
        _OSPI_MODULE = module
    return _OSPI_MODULE


def _uart_isp_module():
    global _UART_ISP_MODULE
    if _UART_ISP_MODULE is None:
        path = Path(__file__).resolve().parent / "gd32h77x_os/tools/download/gd32h77x_uart_isp.py"
        spec = importlib.util.spec_from_file_location("gd32h77x_uart_isp", path)
        if spec is None or spec.loader is None:
            raise ValueError(f"GD32H77X UART ISP transport is unavailable: {path}")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        _UART_ISP_MODULE = module
    return _UART_ISP_MODULE


def _hex_config_bytes(config: Dict[str, str], key: str, default: str,
                      length: int | None) -> bytes:
    text = (config.get(key, "").strip() or default).lower()
    text = "".join(ch for ch in text if ch not in " \t\r\n:-_,")
    if len(text) % 2:
        raise ValueError(f"{key} must contain an even number of hex digits")
    try:
        raw = bytes.fromhex(text)
    except ValueError as exc:
        raise ValueError(f"{key} is not valid hexadecimal") from exc
    if length is not None and len(raw) != length:
        raise ValueError(f"{key} must be exactly {length} raw bytes, got {len(raw)}")
    if not raw:
        raise ValueError(f"{key} must contain the complete raw GETID bytes")
    return raw


def _load_platform_device(chip: str) -> dict:
    """Use the same canonical identity resolver as the build and layout paths."""
    resolver_path = Path(__file__).resolve().parent / "gd32h77x_os/tools/firmware/resolve_memory_layout.py"
    spec = importlib.util.spec_from_file_location("gd32h77x_resolver_identity", resolver_path)
    if spec is None or spec.loader is None:
        raise ValueError("GD32H77X canonical device resolver is unavailable")
    resolver = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(resolver)
    try:
        return resolver.resolve_chip_identity(chip, Path(__file__).resolve().parent)
    except Exception as exc:
        raise ValueError(f"Cannot resolve canonical device identity for {chip}: {exc}") from exc


# ---------------------------------------------------------------------------
# Build-bound layout validation
# ---------------------------------------------------------------------------

def _report_path(image: Path, using_data: dict) -> Path:
    name = using_data.get("CONFIG_PROJECT_NAME", "")
    version = using_data.get("CONFIG_PROJECT_VERSION", "")
    if not name or not version:
        raise ValueError("Project name and version are required to locate the build layout report")
    return image.parent / f"{name}_{version}.layout.json"


def _chip_from_using(using_data: dict) -> str:
    chip = using_data.get("CONFIG_CHIP_CHOICE", "")
    if not chip:
        raise ValueError("Chip must be selected before flashing")
    return chip


def _resolve_current_layout(boards_root: str, board: str, chip: str) -> dict:
    """Resolve exactly the current platform default or selected board override."""
    platform_root = Path(__file__).resolve().parent
    resolver = platform_root / "gd32h77x_os/tools/firmware/resolve_memory_layout.py"
    if not resolver.is_file():
        raise ValueError(f"GD32H77X layout resolver is missing: {resolver}")
    with tempfile.TemporaryDirectory(prefix="gd32h77x-layout-") as output_dir:
        params_path = Path(output_dir) / "build_param.json"
        open_root = Path(boards_root).resolve().parent
        params_path.write_text(json.dumps({
            "OPEN_ROOT": str(open_root),
            "PLATFORM_CHIP": chip,
            "PLATFORM_BOARD": board,
            "CONFIG_PLATFORM_CHOICE": _PLATFORM,
            "CONFIG_BOARD_CHOICE": board,
            "CONFIG_CHIP_CHOICE": chip,
        }), encoding="utf-8")
        command = [
            sys.executable, str(resolver),
            "--build-params", str(params_path),
            "--platform-root", str(platform_root),
            "--boards-root", str(Path(boards_root).resolve()),
            "--out-dir", output_dir,
        ]
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        if result.returncode != 0:
            detail = "\n".join(part for part in (result.stdout, result.stderr) if part).strip()
            raise ValueError(f"Current GD32H77X flash layout is invalid: {detail}")
        resolved_path = Path(output_dir) / "resolved_layout.json"
        if not resolved_path.is_file():
            raise ValueError("Current layout resolver did not produce resolved_layout.json")
        return json.loads(resolved_path.read_text(encoding="utf-8"))


def _verify_build_artifact(image: Path, elf: Path, current: dict, report: dict) -> None:
    platform_root = Path(__file__).resolve().parent
    verifier_path = platform_root / "gd32h77x_os/tools/firmware/verify_image.py"
    spec = importlib.util.spec_from_file_location("gd32h77x_verify_image", verifier_path)
    if spec is None or spec.loader is None:
        raise ValueError("GD32H77X image verifier is unavailable")
    verifier = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verifier)
    with tempfile.TemporaryDirectory(prefix="gd32h77x-verify-") as temp_dir:
        resolved_path = Path(temp_dir) / "resolved_layout.json"
        resolved_path.write_text(json.dumps(current), encoding="utf-8")
        mapped = report.get("mapped_payloads", [])
        if not isinstance(mapped, list) or len(mapped) > 1:
            raise ValueError("Build report mapped_payloads must contain at most one payload")
        ecnvm_path = None
        if mapped:
            entry = mapped[0]
            filename = entry.get("file") if isinstance(entry, dict) else None
            if (not isinstance(filename, str) or not filename or filename in (".", "..")
                    or Path(filename).name != filename or "/" in filename or "\\" in filename
                    or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", filename)
                    or filename in {report.get("elfFile"), report.get("binFile"),
                                    "resolved_layout.json", "report.json"}):
                raise ValueError("Build report ECNVM filename must be a unique basename")
            ecnvm_path = image.parent / filename
        verified = verifier.verify(elf, image, resolved_path,
                                   bin_name=report["binFile"], elf_name=report["elfFile"],
                                   ecnvm_bin_path=ecnvm_path,
                                   ecnvm_enabled=report.get("ecnvmCodeEnabled", False))
    for field in ("board", "chip", "sourceKind", "sourceDigest", "normalizedLayoutDigest",
                  "layout", "binFile", "binSha256", "elfFile", "elfSha256", "allowwrite"):
        if verified.get(field) != report.get(field):
            raise ValueError(f"Build report {field} does not match independent ELF/BIN verification")
    for field in ("ecnvmCodeEnabled", "mapped_payloads"):
        if field in report and verified.get(field) != report.get(field):
            raise ValueError(f"Build report {field} does not match independent ELF/BIN verification")


def _find_cnvm_device(layout: dict) -> dict:
    devices = layout.get("devices")
    if isinstance(devices, dict):
        devices = list(devices.values())
    if not isinstance(devices, list):
        raise ValueError("Resolved layout has no device table")
    matches = [device for device in devices if device.get("region") == "CNVM"]
    if len(matches) != 1:
        raise ValueError("Resolved layout must declare exactly one CNVM device")
    return matches[0]


def _validate_manifest(image: Path, boards_root: str, using_data: dict) -> Tuple[dict, int, int, int, dict]:
    report_path = _report_path(image, using_data)
    if not report_path.is_file():
        raise ValueError(f"Build-bound layout report is missing: {report_path}")
    try:
        report = json.loads(report_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"Build-bound layout report is unreadable: {exc}") from exc

    required = ("board", "chip", "sourceKind", "sourceDigest",
                "normalizedLayoutDigest", "elfFile", "elfSha256",
                "binFile", "binSha256", "allowwrite", "layout")
    missing = [key for key in required if key not in report]
    if missing:
        raise ValueError(f"Build-bound layout report lacks required fields: {', '.join(missing)}")
    board = using_data.get("CONFIG_BOARD_CHOICE", "")
    chip = _chip_from_using(using_data)
    if (report["board"], report["chip"]) != (board, chip):
        raise ValueError("Build-bound layout report board/chip does not match selected target")
    current = _resolve_current_layout(boards_root, board, chip)
    layout_ecnvm = "ecnvm_code" in current.get("layout", {}).get("partitions", {})
    if bool(report.get("ecnvmCodeEnabled", False)) != layout_ecnvm:
        raise ValueError("Build report ECNVM mode differs from the current board layout; rebuild")
    source = current.get("source", {})
    if (report["sourceKind"] != source.get("sourceKind")
            or report["sourceDigest"] != source.get("sourceDigest")
            or report["normalizedLayoutDigest"] != current.get("normalizedLayoutDigest")):
        raise ValueError("Firmware was built from a different GD32H77X flash layout; rebuild before flashing")
    if report["layout"] != current.get("layout"):
        raise ValueError("Build report resolved layout differs from the current layout; rebuild before flashing")
    identity = _load_platform_device(chip)
    target_chip = current.get("layout", {}).get("target_chip")
    if (target_chip != identity["canonical_chip"]
            or current.get("build", {}).get("canonical_target") != identity["canonical_chip"]):
        raise ValueError("Resolved layout canonical target differs from the platform chip registry")
    if (Path(report["binFile"]).name != report["binFile"] or report["binFile"] != image.name
            or _sha256_file(image) != report["binSha256"]):
        raise ValueError("Firmware BIN digest does not match its build-bound layout report")
    elf = image.parent / report["elfFile"]
    if Path(report["elfFile"]).name != report["elfFile"]:
        raise ValueError("Build report ELF filename must be a basename")
    if not elf.is_file() or _sha256_file(elf) != report["elfSha256"]:
        raise ValueError("Firmware ELF is missing or its digest does not match the build-bound report")
    mapped = report.get("mapped_payloads", [])
    if layout_ecnvm:
        if not isinstance(mapped, list) or len(mapped) != 1:
            raise ValueError("Layout declares ecnvm_code but report has no unique ECNVM payload")
        item = mapped[0]
        filename = item.get("file") if isinstance(item, dict) else None
        if (not isinstance(filename, str) or not filename or filename in (".", "..")
                or Path(filename).name != filename or "/" in filename or "\\" in filename
                or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", filename)
                or filename in {report.get("elfFile"), report.get("binFile")}):
            raise ValueError("Build report ECNVM filename must be a unique basename")
        part = current.get("layout", {}).get("partitions", {}).get("ecnvm_code", {})
        device = next((entry for entry in current.get("layout", {}).get("devices", [])
                       if entry.get("id") == "internal_ecnvm"), {})
        expected_ecnvm_geometry = identity["canonical_chip"] + "_ECNVM"
        if (item.get("partition") != "ecnvm_code" or item.get("region") != "ECNVM"
                or item.get("address") != part.get("logical_base")
                or item.get("size", 0) <= 0 or item.get("size", 0) > part.get("size_bytes", 0)
                or device.get("geometry_id") != expected_ecnvm_geometry
                or device.get("backend_status") != "implemented"):
            raise ValueError("Reported ECNVM payload does not match current canonical board geometry")
        sidecar = image.parent / filename
        if (not sidecar.is_file() or _sha256_file(sidecar) != item.get("sha256")):
            raise ValueError("ECNVM sidecar is missing or differs from its build-bound report")
    elif mapped:
        raise ValueError("ECNVM payload is present while the current layout has no ecnvm_code partition")

    layout = current.get("layout", {})
    flash_image = layout.get("flash_image", {})
    device = _find_cnvm_device(layout)
    try:
        start = int(flash_image["start_address"])
        limit = int(flash_image["end_address"])
        capacity = int(flash_image["capacity_bytes"])
        erase_bytes = int(device["erase_bytes"])
        cpu_base = int(device["cpu_base"])
        device_capacity = int(device["capacity_bytes"])
    except (KeyError, TypeError, ValueError) as exc:
        raise ValueError("Resolved layout has no valid CNVM geometry") from exc
    if erase_bytes <= 0 or capacity <= 0 or limit <= start or limit - start != capacity:
        raise ValueError("Resolved CNVM image range is invalid")
    if start % erase_bytes or limit % erase_bytes:
        raise ValueError(
            "Resolved CNVM image range is not aligned to the {} byte erase unit".format(erase_bytes))
    if start < cpu_base or limit > cpu_base + device_capacity:
        raise ValueError("Resolved CNVM image range exceeds the device geometry")
    if (device.get("geometry_id") != identity["cnvm_geometry"]
            or device_capacity != identity["cnvm_capacity_bytes"]
            or erase_bytes != identity["cnvm_erase_bytes"]
            or device.get("vendor_algorithm") != identity["cnvm_vendor_algorithm"]):
        raise ValueError("Resolved CNVM geometry differs from the selected canonical device registry")
    board_config = _board_flash_config(using_data, boards_root)
    board_target = _required(board_config, "CONFIG_FLASH_TARGET_CHIP")
    if board_target.upper() != identity["canonical_chip"]:
        raise ValueError("Board flash target identity does not match the selected build canonical target")
    expected_allowwrite = _factory_write_ranges(layout)
    _validate_allowwrite(report["allowwrite"], start, limit, expected_allowwrite)
    _verify_build_artifact(image, elf, current, report)
    return report, start, limit, capacity, device


def _extract_validated_package(package: Path, boards_root: str, using_data: dict,
                               extract_root: Path):
    """Validate a self-contained package and stage names expected by the BIN verifier."""
    board = using_data.get("CONFIG_BOARD_CHOICE", "")
    chip = _chip_from_using(using_data)
    current = _resolve_current_layout(boards_root, board, chip)
    package_result = _flash_package_module().validate_package(
        package, current, expected_board=board, expected_chip=chip,
        extract_dir=extract_root)
    report = package_result["report"]
    project = using_data.get("CONFIG_PROJECT_NAME", "")
    version = using_data.get("CONFIG_PROJECT_VERSION", "")
    expected_bin = f"{project}_QIO_{version}.bin"
    expected_elf = f"{project}_{version}.elf"
    if report.get("binFile") != expected_bin or report.get("elfFile") != expected_elf:
        raise ValueError("Flash package artifact names do not match the selected project/version")
    if (package_result.get("internal_bin_size_bytes") != report.get("size")
            or package_result.get("internal_bin_sha256") != report.get("binSha256")):
        raise ValueError("Flash package internal BIN does not match its build report")

    staged_bin = extract_root / expected_bin
    staged_elf = extract_root / expected_elf
    staged_report = extract_root / f"{project}_{version}.layout.json"
    staged_bin.write_bytes(Path(package_result["artifacts"]["internal_bin"]).read_bytes())
    staged_elf.write_bytes(Path(package_result["artifacts"]["elf"]).read_bytes())
    staged_report.write_text(json.dumps(report), encoding="utf-8")
    mapped_payloads = [dict(item) for item in package_result.get("mapped_payloads", [])]
    for item in mapped_payloads:
        filename = report.get("mapped_payloads", [{}])[0].get("file")
        if (not isinstance(filename, str) or not filename or filename in (".", "..")
                or Path(filename).name != filename or "/" in filename or "\\" in filename
                or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", filename)
                or filename in {report.get("elfFile"), report.get("binFile")}):
            raise ValueError("Package report ECNVM filename must be a unique basename")
        destination = extract_root / filename
        shutil.copyfile(item["path"], destination)
        item["path"] = str(destination)
    return staged_bin, package_result.get("external_payloads", []), mapped_payloads


def _factory_write_ranges(layout: dict) -> list:
    partitions = layout.get("partitions", {})
    devices = layout.get("devices", {})
    if isinstance(partitions, list):
        partitions = {part.get("id", "").lower(): part for part in partitions}
    if isinstance(devices, list):
        devices = {device.get("id", "").lower(): device for device in devices}
    if not isinstance(partitions, dict) or not isinstance(devices, dict):
        raise ValueError("Resolved layout has invalid device or partition tables")
    ranges = []
    for partition_name in ("boot", "primary"):
        partition = partitions.get(partition_name)
        if not isinstance(partition, dict):
            raise ValueError(f"Resolved layout is missing the {partition_name} partition")
        base = int(partition.get("logical_base", -1))
        for segment in partition.get("segments", []):
            device_ref = str(segment.get("device", "")).lower()
            device = devices.get(device_ref, {})
            if (device.get("region") != "CNVM"
                    or device.get("cpu_base") != 0x08000000):
                raise ValueError("Factory boot/primary partitions must reside on internal CNVM")
            device_id = str(device.get("id", device_ref)).lower()
            ranges.append({
                "partition": partition_name,
                "device": device_id,
                "deviceOffset": int(segment.get("device_offset_bytes", segment.get("deviceOffset", -1))),
                "size": int(segment.get("size_bytes", segment.get("size", -1))),
                "logicalBase": base + int(segment.get("logical_offset_bytes", segment.get("logicalOffset", 0))),
            })
    return ranges


def _validate_allowwrite(allowwrite, start: int, limit: int, expected) -> None:
    if not isinstance(allowwrite, list) or not allowwrite:
        raise ValueError("Build report has no explicit internal boot/primary write ranges")
    spans = []
    parsed = []
    for item in allowwrite:
        if not isinstance(item, dict):
            raise ValueError("Invalid build report allowwrite entry")
        try:
            part = item["partition"].lower()
            device = item["device"].lower()
            address = int(item["logicalBase"])
            offset = int(item["deviceOffset"])
            size = int(item["size"])
        except (KeyError, AttributeError, TypeError, ValueError) as exc:
            raise ValueError("Invalid build report allowwrite entry") from exc
        if part not in ("boot", "primary") or not device:
            raise ValueError("Factory DFU report may only write internal boot and primary partitions")
        if size <= 0 or address < start or address + size > limit or offset < 0:
            raise ValueError("Build report write range exceeds its resolved internal boot/primary image")
        spans.append((address, address + size))
        parsed.append((part, device, offset, size, address))
    expected_tuples = sorted((item["partition"], item["device"], item["deviceOffset"],
                              item["size"], item["logicalBase"]) for item in expected)
    if sorted(parsed) != expected_tuples:
        raise ValueError("Build report write ranges differ from the current resolved boot/primary layout")
    spans.sort()
    cursor = start
    for begin, end in spans:
        if begin != cursor:
            raise ValueError("Build report write ranges must cover boot and primary continuously")
        cursor = end
    if cursor != limit:
        raise ValueError("Build report write ranges do not cover the resolved factory image")


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as tool_file:
        for chunk in iter(lambda: tool_file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _create_padded_image(image: Path, capacity: int, erase_size: int) -> Tuple[str, int]:
    """Create an FF-padded temporary image aligned to whole erase units."""
    image_size = image.stat().st_size
    padded_size = ((image_size + erase_size - 1) // erase_size) * erase_size
    if image_size <= 0 or padded_size > capacity:
        raise ValueError(
            f"Sector-padded image size {padded_size} exceeds application capacity "
            f"{capacity}; refusing to touch the reserved KV sectors")

    temp_path = None
    try:
        with tempfile.NamedTemporaryFile(
                prefix="gd32h77x-flash-", suffix=".bin", delete=False) as temp:
            temp_path = temp.name
            with image.open("rb") as source:
                shutil.copyfileobj(source, temp)
            temp.write(b"\xFF" * (padded_size - image_size))
        return temp_path, padded_size
    except Exception:
        if temp_path and os.path.exists(temp_path):
            os.unlink(temp_path)
        raise


def _run(command, logger):
    if logger:
        logger.debug("Running: {}".format(" ".join(str(part) for part in command)))
    try:
        result = subprocess.run(command, capture_output=True, text=True, check=False)
    except OSError as exc:
        return 127, str(exc)
    output = "\n".join(part for part in (result.stdout, result.stderr) if part).strip()
    return result.returncode, output


# ---------------------------------------------------------------------------
# Backend: native DfuSe over libusb
# ---------------------------------------------------------------------------

def _dfuse_module():
    global _DFUSE_MODULE
    if _DFUSE_MODULE is None:
        path = Path(__file__).resolve().parent / "gd32h77x_os/tools/download/dfuse.py"
        spec = importlib.util.spec_from_file_location("gd32h77x_dfuse", path)
        if spec is None or spec.loader is None:
            raise ValueError(f"GD32H77X DfuSe transport is unavailable: {path}")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        _DFUSE_MODULE = module
    return _DFUSE_MODULE


def _dfu_identity(config: Dict[str, str]) -> Tuple[int, int]:
    vid_text = _required(config, "CONFIG_FLASH_USB_VID").lower().removeprefix("0x")
    pid_text = _required(config, "CONFIG_FLASH_USB_PID").lower().removeprefix("0x")
    try:
        return int(vid_text, 16), int(pid_text, 16)
    except ValueError as exc:
        raise ValueError("CONFIG_FLASH_USB_VID/PID must be hexadecimal") from exc


def _select_dfu_interface(interfaces: List[dict], alt: int, name_marker: str,
                          require_upload: bool = True) -> dict:
    candidates = [entry for entry in interfaces
                  if entry.get("protocol") == 2 and entry.get("alt") == alt]
    if not candidates:
        raise RuntimeError(
            "DFU device has no DFU-mode protocol-2 alternate setting {} (available: {})".format(
                alt, ", ".join(str(entry.get("alt")) for entry in interfaces)))
    for entry in candidates:
        name = str(entry.get("name", ""))
        lower_name = name.lower()
        normalized_name = re.sub(r"[^a-z0-9]", "", lower_name)
        normalized_marker = re.sub(r"[^a-z0-9]", "", name_marker.lower())
        attributes = entry.get("bm_attributes")
        version = entry.get("bcd_dfu_version")
        if (version == 0x011A and isinstance(attributes, int)
                and (attributes & 0x01)
                and (not require_upload or (attributes & 0x02))
                and normalized_marker in normalized_name
                and "internal" in normalized_name and "flash" in normalized_name
                and re.search(r"0x0*8000000\b", lower_name)):
            return entry
    names = ", ".join(repr(entry.get("name", "")) for entry in candidates)
    raise RuntimeError(
        "Selected DFU alternate is not the configured internal CNVM interface at "
        f"0x08000000; detected names: {names or '(empty)'}")


def _choose_dfu_device(candidates, selector: str, expected_serial: str, logger):
    if selector:
        candidates = [entry for entry in candidates
                      if entry[0].path == selector or entry[0].serial == selector]
        if not candidates:
            raise RuntimeError(
                f"No DFU device matches configured USB path or serial {selector!r}")
    if expected_serial:
        candidates = [entry for entry in candidates if entry[0].serial == expected_serial]
        if not candidates:
            raise RuntimeError(f"No DFU device with configured serial {expected_serial!r}")
    if not candidates:
        raise RuntimeError("No GD32H77X DFU device found; set BOOT and reset the board")
    if len(candidates) == 1:
        return candidates[0]
    print("Multiple matching GD32H77X DFU devices found:")
    for index, (device, _interface) in enumerate(candidates, start=1):
        print("  {}. USB path {} serial {}".format(index, device.path, device.serial or "-"))
    if not sys.stdin.isatty():
        paths = ", ".join(device.path for device, _interface in candidates)
        raise RuntimeError(
            "Multiple DFU devices are connected in non-interactive mode; "
            f"rerun with -p/--port and one USB path ({paths})")
    for _ in range(3):
        try:
            answer = input("Select DFU device by number (q to cancel): ").strip()
        except (EOFError, KeyboardInterrupt):
            raise RuntimeError("DFU device selection cancelled; no download was started")
        if answer.lower() == "q":
            raise RuntimeError("DFU device selection cancelled; no download was started")
        try:
            index = int(answer)
        except ValueError:
            index = 0
        if 1 <= index <= len(candidates):
            return candidates[index - 1]
        print("Invalid selection. Enter a listed number or q to cancel.")
    raise RuntimeError("Too many invalid selections; no download was started")


def _dfu_ecnvm_plan(report, mapped_payloads):
    """Validate the one executable ECNVM payload before opening DFU."""
    mapped_payloads = mapped_payloads or []
    if not mapped_payloads:
        return None
    if len(mapped_payloads) != 1:
        raise ValueError("DFU requires exactly one ECNVM payload")

    layout = report.get("layout", {})
    devices = layout.get("devices", [])
    if isinstance(devices, dict):
        devices = list(devices.values())
    ecnvm_devices = [item for item in devices
                     if isinstance(item, dict) and item.get("region") == "ECNVM"]
    if len(ecnvm_devices) != 1:
        raise ValueError("Current layout must declare exactly one ECNVM device")
    device = ecnvm_devices[0]
    partition = layout.get("partitions", {}).get("ecnvm_code", {})
    try:
        start = int(device["cpu_base"])
        capacity = int(device["capacity_bytes"])
        erase_bytes = int(device["erase_bytes"])
        program_bytes = int(device["program_bytes"])
        page_bytes = int(device["page_bytes"])
        partition_base = int(partition["logical_base"])
        partition_size = int(partition["size_bytes"])
    except (KeyError, TypeError, ValueError) as exc:
        raise ValueError("Resolved layout has no valid ECNVM geometry") from exc
    expected_geometry = str(report.get("chip", "")) + "_ECNVM"
    if (device.get("geometry_id") != expected_geometry
            or device.get("backend_status") != "implemented"
            or start != 0x08200000
            or capacity != 0x780000
            or erase_bytes != 0x1000
            or program_bytes != 8
            or page_bytes != 0x1000
            or partition_base != start
            or partition_size <= 0
            or partition_size > capacity
            or start % erase_bytes):
        raise ValueError(
            "ECNVM device differs from the approved {} DFU geometry".format(
                report.get("chip", "?")))

    mapped = mapped_payloads[0]
    if (mapped.get("partition") != "ecnvm_code" or mapped.get("region") != "ECNVM"
            or int(mapped.get("address", -1)) != partition_base):
        raise ValueError("ECNVM payload does not map to ecnvm_code at 0x08200000")
    payload_path = Path(mapped.get("path", ""))
    if not payload_path.is_file():
        raise ValueError("ECNVM payload file is missing")
    payload = payload_path.read_bytes()
    reported_size = mapped.get("size_bytes", mapped.get("size", -1))
    if (not payload or len(payload) > partition_size
            or hashlib.sha256(payload).hexdigest() != mapped.get("sha256")
            or int(reported_size) != len(payload)):
        raise ValueError("ECNVM payload failed address/range/digest preflight")
    return payload_path, start, partition_size, erase_bytes


def _flash_dfu(config, report, image, start, limit, capacity, device_geometry,
               usb_path, logger, mapped_payloads=None):
    dfuse = _dfuse_module()
    vendor, product = _dfu_identity(config)
    alt = int(config.get("CONFIG_FLASH_DFU_ALT", "0") or "0", 0)
    name_marker = config.get("CONFIG_FLASH_DFU_ALT_NAME", "InternalFlash").strip()
    if not name_marker:
        raise ValueError("CONFIG_FLASH_DFU_ALT_NAME must identify the CNVM alternate setting")
    expected_serial = config.get("CONFIG_FLASH_USB_SERIAL", "").strip()
    erase_bytes = int(device_geometry["erase_bytes"])
    verify = _boolean(config, "CONFIG_FLASH_VERIFY", True)
    leave = _boolean(config, "CONFIG_FLASH_DFU_LEAVE", False)
    ecnvm_plan = _dfu_ecnvm_plan(report, mapped_payloads)

    temp_image, transfer_size = _create_padded_image(image, capacity, erase_bytes)
    lib = None
    selected = None
    opened_devices = []
    try:
        lib, devices = dfuse.open_dfu_devices(vendor, product)
        opened_devices = list(devices)
        candidates = []
        for opened in devices:
            try:
                interfaces = opened.interfaces()
            except Exception as exc:  # a non-DFU device must not abort the scan
                if logger:
                    logger.debug("Skipping USB path {}: {}".format(opened.path, exc))
                continue
            if interfaces:
                try:
                    interface = _select_dfu_interface(
                        interfaces, alt, name_marker,
                        require_upload=verify or ecnvm_plan is not None)
                except RuntimeError as exc:
                    if logger:
                        logger.debug("Skipping USB path {}: {}".format(opened.path, exc))
                else:
                    candidates.append((opened, interface))
        selected, interface = _choose_dfu_device(candidates, usb_path, expected_serial, logger)
        for opened, _entry in candidates:
            if opened is not selected:
                opened.close()
        selected.claim(interface["interface"], alt)
        protocol = dfuse.DfuSeProtocol(selected, interface["interface"],
                                       interface["transfer_size"],
                                       log=(logger.info if logger else None))
        protocol.settle()

        if ecnvm_plan is not None:
            ecnvm_path, ecnvm_start, ecnvm_capacity, ecnvm_erase_bytes = ecnvm_plan
            padded_ecnvm_path, ecnvm_transfer_size = _create_padded_image(
                ecnvm_path, ecnvm_capacity, ecnvm_erase_bytes)
            try:
                ecnvm_payload = Path(padded_ecnvm_path).read_bytes()
                if len(ecnvm_payload) % interface["transfer_size"]:
                    raise ValueError("ECNVM padded image is not aligned to the DFU transfer size")
                ecnvm_sectors = dfuse.erase_plan(
                    ecnvm_start, ecnvm_start + ecnvm_transfer_size, ecnvm_erase_bytes)
                if logger:
                    logger.info(
                        "Erasing {} ECNVM sectors of {} bytes from 0x{:08X} "
                        "(backend dfu, geometry {})".format(
                            len(ecnvm_sectors), ecnvm_erase_bytes, ecnvm_start,
                            device_geometry.get("geometry_id", "?").replace(
                                "_CNVM", "_ECNVM")))
                for address in ecnvm_sectors:
                    protocol.erase(address)

                def ecnvm_progress(done):
                    if logger and (done % (64 * 1024) == 0
                                   or done == len(ecnvm_payload)):
                        logger.info("Programmed ECNVM 0x{:08X}/0x{:08X}".format(
                            done, len(ecnvm_payload)))

                protocol.write(ecnvm_start, ecnvm_payload, progress=ecnvm_progress)
                if logger:
                    logger.info(
                        "Verifying {} sector-padded ECNVM bytes at 0x{:08X}".format(
                            ecnvm_transfer_size, ecnvm_start))
                ecnvm_readback = protocol.read(ecnvm_start, ecnvm_transfer_size)
                if ecnvm_readback != ecnvm_payload:
                    mismatch = next((index for index, (left, right)
                                     in enumerate(zip(ecnvm_readback, ecnvm_payload))
                                     if left != right),
                                    min(len(ecnvm_readback), len(ecnvm_payload)))
                    raise RuntimeError(
                        "ECNVM readback mismatch at 0x{:08X}; CNVM was not written".format(
                            ecnvm_start + mismatch))
            finally:
                if os.path.exists(padded_ecnvm_path):
                    os.unlink(padded_ecnvm_path)

        sectors = dfuse.erase_plan(start, start + transfer_size, erase_bytes)
        if logger:
            logger.info(
                "Erasing {} CNVM sectors of {} bytes from 0x{:08X} (backend dfu, "
                "geometry {} {})".format(len(sectors), erase_bytes, start,
                                         device_geometry.get("geometry_id", "?"),
                                         device_geometry.get("part_number", "?")))
        for address in sectors:
            protocol.erase(address)

        payload = Path(temp_image).read_bytes()

        def program_progress(done):
            if logger and (done % (64 * 1024) == 0 or done == len(payload)):
                logger.info("Programmed 0x{:08X}/0x{:08X}".format(done, len(payload)))

        protocol.write(start, payload, progress=program_progress)

        if verify:
            if logger:
                logger.info("Verifying {} sector-padded image bytes at 0x{:08X}".format(
                    transfer_size, start))
            readback = protocol.read(start, transfer_size)
            expected = payload
            if readback != expected:
                mismatch = next((index for index, (left, right)
                                 in enumerate(zip(readback, expected)) if left != right),
                                min(len(readback), len(expected)))
                raise RuntimeError(
                    "CNVM readback mismatch at 0x{:08X}".format(start + mismatch))

        if leave:
            if logger:
                logger.info("Verification complete; requesting application start via DfuSe leave")
            protocol.leave()
        if ecnvm_plan is not None:
            message = ("DfuSe CNVM+ECNVM download and verification complete"
                       if verify else
                       "DfuSe CNVM+ECNVM download complete; ECNVM readback verified, "
                       "CNVM verification disabled by internal flash policy")
        else:
            message = ("DfuSe download and verification complete" if verify else
                       "DfuSe download complete; readback verification disabled by internal flash policy")
        return {"success": True, "message": message}
    finally:
        for opened in opened_devices:
            opened.close()
        if lib is not None:
            lib.close()
        if os.path.exists(temp_image):
            os.unlink(temp_image)


# ---------------------------------------------------------------------------
# Backend: ROM system-memory bootloader over the console UART
# ---------------------------------------------------------------------------

def _flash_uart(config, report, current_layout, image, external_payloads,
                port, baud, start, limit, capacity, device_geometry, logger):
    uart = _uart_isp_module()
    try:
        expected_id = _hex_config_bytes(
            config, "CONFIG_FLASH_UART_EXPECTED_ID", "", None)
    except ValueError as exc:
        raise ValueError(
            "UART ISP expected ID is missing or invalid in "
            "chips/<canonical_chip>/uart_isp.json") from exc
    expected_chip = _required(config, "CONFIG_FLASH_TARGET_CHIP")
    ospi_profiles = None
    if external_payloads:
        profile_file = _required(config, "CONFIG_FLASH_UART_OSPI_PROFILES")
        configured_path = Path(profile_file)
        if configured_path.is_absolute():
            raise ValueError("UART OSPI profile path must be relative to the board/platform directory")
        config_dir = Path(config["_CONFIG_DIR"]).resolve()
        profile_path = (config_dir / configured_path).resolve()
        if not profile_path.is_relative_to(config_dir):
            raise ValueError(
                "UART OSPI profile path must stay within the board/platform directory")
        if not profile_path.is_file():
            raise ValueError(f"UART OSPI profile CSV does not exist: {profile_path}")
        ospi_profiles = {}
        try:
            with profile_path.open("r", encoding="utf-8", newline="") as stream:
                reader = csv.DictReader(stream, strict=True)
                if (reader.fieldnames != ["device", "config", "pins"]):
                    raise ValueError(
                        "UART OSPI profile CSV header must be exactly device,config,pins "
                        "with no duplicate or extra columns")
                for row in reader:
                    if None in row or any(row.get(key) is None for key in reader.fieldnames):
                        raise ValueError("UART OSPI profile CSV contains a malformed row")
                    device_id = (row.get("device") or "").strip()
                    if not device_id or device_id in ospi_profiles:
                        raise ValueError(f"UART OSPI profile CSV has empty or duplicate device id: {device_id!r}")
                    ospi_config = _hex_config_bytes(
                        {"OSPI profile config": row.get("config", "")},
                        "OSPI profile config", "", 3)
                    records = [item.strip() for item in row.get("pins", "").split(";")]
                    if not records or any(not item for item in records):
                        raise ValueError(f"UART OSPI profile for {device_id} requires pin records")
                    pins = tuple(_hex_config_bytes(
                        {"OSPI profile pins": item}, "OSPI profile pins", "", 5)
                        for item in records)
                    ospi_profiles[device_id] = {"config": ospi_config, "pins": pins}
        except (OSError, UnicodeError, csv.Error) as exc:
            raise ValueError(f"Cannot read UART OSPI profile CSV {profile_path}: {exc}") from exc
    port_value = str(port or "").strip()
    if not port_value:
        try:
            from serial.tools import list_ports
        except ImportError as exc:
            raise ValueError("UART port auto-selection requires pyserial; specify -p/--port") from exc
        ports = list(list_ports.comports())
        if not ports:
            raise ValueError("No serial ports found; connect the target or specify -p/--port")
        if len(ports) == 1:
            port_value = ports[0].device
        else:
            entries = ["  {}. {} ({})".format(
                index, item.device, item.description or "unknown")
                for index, item in enumerate(ports, 1)]
            if not sys.stdin.isatty():
                raise ValueError(
                    "Multiple serial ports found in non-interactive mode; specify -p/--port:\n" +
                    "\n".join(entries))
            print("Multiple serial ports found:")
            print("\n".join(entries))
            for _ in range(3):
                try:
                    answer = input("Select serial port by number (q to cancel): ").strip()
                except (EOFError, KeyboardInterrupt):
                    raise ValueError("Serial port selection cancelled; no flash command was started")
                if answer.lower() == "q":
                    raise ValueError("Serial port selection cancelled; no flash command was started")
                try:
                    selected = int(answer)
                except ValueError:
                    selected = 0
                if 1 <= selected <= len(ports):
                    port_value = ports[selected - 1].device
                    break
                print("Invalid selection. Enter a listed number or q to cancel.")
            if not port_value:
                raise ValueError("Too many invalid selections; no flash command was started")

    configured_baud = config.get("CONFIG_FLASH_UART_BAUD", "").strip()
    if baud:
        selected_baud = int(baud)
    elif configured_baud.lower() in ("auto", "0", ""):
        selected_baud = "auto"
    else:
        selected_baud = int(configured_baud, 0)
    if selected_baud != "auto" and selected_baud <= 0:
        raise ValueError("UART baud must be a positive integer or auto")

    start = int(start)
    limit = int(limit)
    capacity = int(capacity)
    if start != int(uart.CNVM_BASE):
        raise ValueError(
            f"UART CNVM programming requires start 0x{int(uart.CNVM_BASE):08X}, "
            f"got 0x{start:08X}")
    if limit - start != capacity:
        raise ValueError("Build report UART CNVM range does not match capacity")
    layout_flash = report.get("layout", {}).get("flash_image", {})
    if (int(layout_flash.get("start_address", -1)) != start
            or int(layout_flash.get("end_address", -1)) != limit
            or int(layout_flash.get("capacity_bytes", -1)) != capacity):
        raise ValueError("Build report UART range does not match the resolved layout")
    erase_bytes = int(device_geometry.get("erase_bytes", 0))
    if erase_bytes != int(uart.CNVM_PAGE):
        raise ValueError(
            f"UART CNVM programming requires {int(uart.CNVM_PAGE)}-byte sectors, "
            f"got {erase_bytes}")
    if external_payloads and current_layout is None:
        raise ValueError("UART external payloads require the current resolved layout")

    verify = _boolean(config, "CONFIG_FLASH_VERIFY", True)
    verify_key = "readback verification"
    erase_mode = _UART_ISP_ERASE_MODE

    if logger:
        baud_text = ("auto (ROM baud capture)" if selected_baud == "auto"
                     else str(selected_baud))
        logger.info("Opening UART ROM ISP on {} at {} 8E1 (no SWD/pyOCD)".format(
            port_value, baud_text))
    result = uart.flash(
        manifest=current_layout,
        external_payloads=external_payloads,
        image_path=image,
        start=start,
        limit=limit,
        capacity=capacity,
        device_geometry=device_geometry,
        port=port_value,
        baud=selected_baud,
        expected_id=expected_id,
        expected_chip=expected_chip,
        ospi_profiles=ospi_profiles,
        erase_mode=erase_mode,
        verify=verify,
        logger=logger,
    )
    if not isinstance(result, dict) or not result.get("success", False):
        raise RuntimeError(
            "UART ROM ISP did not report success; inspect OSPI/CNVM state: {}".format(result))
    reported_baud = result.get("baud")
    if reported_baud:
        actual_baud = int(reported_baud)
    elif selected_baud == "auto":
        actual_baud = int(getattr(uart, "DEFAULT_UART_BAUD", 115200))
    else:
        actual_baud = int(selected_baud)

    partitions = [item.get("partition", "?") for item in result.get("external", [])
                  if isinstance(item, dict)]
    if partitions:
        stage = "OSPI partitions {} and CNVM".format(", ".join(partitions))
    else:
        stage = "CNVM"
    verification = ("readback verified" if verify else
                    "readback verification disabled")
    return {
        "success": True,
        "message": f"UART ROM ISP complete on {port_value} at {actual_baud} 8E1; "
                   f"{stage} {verification}",
    }


# ---------------------------------------------------------------------------
# Backend: SWD through the vendor CMSIS-Pack algorithm
# ---------------------------------------------------------------------------

def _flash_swd(config, report, image, port, start, limit, capacity, logger,
               mapped_payloads=None):
    try:
        pyocd_version = importlib.metadata.version("pyocd")
    except importlib.metadata.PackageNotFoundError:
        raise RuntimeError(
            "SWD flashing needs pyocd; install the pinned optional "
            "dependencies in the SDK Python environment")
    if pyocd_version != "0.46.0":
        raise RuntimeError(
            f"GD32H77X SWD requires pyocd==0.46.0, found {pyocd_version}; "
            "install the pinned optional dependencies in the SDK Python environment")
    pyocd = [sys.executable, "-m", "pyocd"]
    platform_root = Path(__file__).resolve().parent
    resolver_path = platform_root / "gd32h77x_os/tools/firmware/resolve_memory_layout.py"
    spec = importlib.util.spec_from_file_location("gd32h77x_resolver_swd", resolver_path)
    if spec is None or spec.loader is None:
        raise RuntimeError("GD32H77X canonical device resolver is unavailable")
    resolver = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(resolver)
    chip = report["chip"]
    identity = resolver.resolve_chip_identity(chip, platform_root)
    target = report.get("layout", {}).get("target_chip", "")
    if target != identity["canonical_chip"]:
        raise ValueError("Build-bound canonical target does not match the selected device registry")
    pack = _resolve_swd_pack(config)
    if not pack:
        raise ValueError(
            "SWD requires the approved GigaDevice.GD32H77x_78x_DFP.0.6.1.pack; "
            "install it in a standard PyOCD/TuyaOpen location or set GD32H77X_DFP_PACK")
    pack = str(Path(pack).expanduser().resolve())
    geometry = {
        "capacity_bytes": identity["cnvm_capacity_bytes"],
        "erase_bytes": identity["cnvm_erase_bytes"],
        "program_bytes": identity["cnvm_program_bytes"],
        "vendor_algorithm": identity["cnvm_vendor_algorithm"],
    }
    pack_info = _validate_swd_pack(platform_root, pack, target, geometry)
    mapped_payloads = mapped_payloads or []
    ecnvm_plan = None
    if mapped_payloads:
        entries = report.get("layout", {}).get("devices", [])
        ecnvm_devices = [item for item in entries if item.get("region") == "ECNVM"]
        if len(ecnvm_devices) != 1:
            raise ValueError("Current layout must declare exactly one ECNVM device")
        device = ecnvm_devices[0]
        expected_ecnvm_geometry = identity["canonical_chip"] + "_ECNVM"
        if (device.get("geometry_id") != expected_ecnvm_geometry
                or device.get("backend_status") != "implemented"
                or device.get("cpu_base") != 0x08200000
                or device.get("capacity_bytes") != 0x780000
                or device.get("erase_bytes") != 0x1000
                or device.get("program_bytes") != 8
                or device.get("page_bytes") != 0x1000
                or device.get("swd_erase_bytes") != 0x10000
                or device.get("swd_program_bytes") != 32
                or device.get("vendor_algorithm") != "GD32H77x_78x_ECNVM_7M_512K.FLM"):
            raise ValueError(
                "ECNVM device differs from the approved {} registry geometry".format(
                    identity["canonical_chip"]))
        mapped = mapped_payloads[0]
        payload_path = Path(mapped.get("path", ""))
        payload_data = payload_path.read_bytes()
        partition = report["layout"]["partitions"]["ecnvm_code"]
        if (mapped.get("partition") != "ecnvm_code" or mapped.get("region") != "ECNVM"
                or int(mapped.get("address", -1)) != int(partition["logical_base"])
                or len(mapped_payloads) != 1 or not payload_data
                or hashlib.sha256(payload_data).hexdigest() != mapped.get("sha256")
                or len(payload_data) > int(partition["size_bytes"])):
            raise ValueError("ECNVM payload failed address/range/digest preflight")
        ecnvm_geometry = {"region": "ECNVM", "cpu_base": 0x08200000,
            "capacity_bytes": 0x780000, "erase_bytes": device["swd_erase_bytes"],
            "program_bytes": device["swd_program_bytes"],
            "vendor_algorithm": "GD32H77x_78x_ECNVM_7M_512K.FLM"}
        ecnvm_pack_info = _validate_swd_pack(platform_root, pack, target, ecnvm_geometry)
        ecnvm_plan = (payload_path, ecnvm_geometry, ecnvm_pack_info)
    start = int(start)
    limit = int(limit)
    capacity = int(capacity)
    erase_bytes = int(identity["cnvm_erase_bytes"])
    image_size = image.stat().st_size
    padded_size = ((image_size + erase_bytes - 1) // erase_bytes) * erase_bytes
    layout_flash = report.get("layout", {}).get("flash_image", {})
    if (int(layout_flash.get("start_address", -1)) != start
            or int(layout_flash.get("end_address", -1)) != limit
            or int(layout_flash.get("capacity_bytes", -1)) != capacity
            or limit - start != capacity
            or start < 0x08000000
            or limit > 0x08000000 + identity["cnvm_capacity_bytes"]):
        raise ValueError("Build report SWD range does not match the verified canonical CNVM geometry")
    if (image_size <= 0 or padded_size > capacity or start % erase_bytes):
        raise ValueError("Sector-padded SWD image exceeds or is misaligned for selected CNVM geometry")
    with tempfile.TemporaryDirectory(prefix="gd32h77x-pyocd-") as temp_dir:
        isolated = Path(temp_dir)
        env = os.environ.copy()
        env["PYOCD_USER_DIR"] = str(isolated / "user")
        Path(env["PYOCD_USER_DIR"]).mkdir()
        common = ["--no-config", "--project", str(isolated), "--script", os.devnull]
        list_result = _run_pyocd(
            [*pyocd, "json", "--targets", "--pack", pack, *common],
            logger, isolated, env)
        if list_result[0] != 0:
            raise RuntimeError(f"pyOCD cannot load the selected DFP: {list_result[1]}")
        try:
            targets = json.loads(list_result[1]).get("targets", [])
        except (json.JSONDecodeError, AttributeError) as exc:
            raise RuntimeError("pyOCD returned invalid target-list JSON for the selected DFP") from exc
        found = [entry for entry in targets
                 if entry.get("part_number", "").upper() == target.upper()
                 and entry.get("vendor", "").lower() == "gigadevice"
                 and entry.get("source") == "pack"]
        if len(found) != 1:
            raise RuntimeError(
                f"pyOCD did not expose exactly one {target} target from the selected DFP")

        probe_hint = str(port or "").strip()
        probe_result = _run_pyocd(
            [*pyocd, "json", "--probes", *common], logger, isolated, env)
        if probe_result[0] != 0:
            raise RuntimeError(f"pyOCD probe discovery failed: {probe_result[1]}")
        try:
            probe_doc = json.loads(probe_result[1])
            probes = probe_doc.get("boards", [])
        except (json.JSONDecodeError, AttributeError) as exc:
            raise RuntimeError("pyOCD returned invalid probe-list JSON") from exc
        if probe_doc.get("status") != 0:
            raise RuntimeError("pyOCD probe discovery failed: {}".format(probe_doc.get("error", "unknown error")))
        probe_id = _select_swd_probe(probes, probe_hint)

        if ecnvm_plan is not None:
            ecnvm_path, ecnvm_geometry, ecnvm_pack_info = ecnvm_plan
            ecnvm_capacity = int(report["layout"]["partitions"]["ecnvm_code"]["size_bytes"])
            ecnvm_start = 0x08200000
            padded_ecnvm_path, ecnvm_transfer_size = _create_padded_image(
                ecnvm_path, ecnvm_capacity, int(ecnvm_geometry["erase_bytes"]))
            try:
                ecnvm_payload = Path(padded_ecnvm_path).read_bytes()
                ecnvm_command = [*pyocd, "flash", *common,
                    "-f", config["CONFIG_FLASH_SWD_FREQUENCY_HZ"],
                    "-O", "auto_unlock=false", "-O", "dap_swj_use_dormant=true",
                    "-O", "keep_unwritten=false",
                    "-O", "smart_flash=false", "-O", "resume_on_disconnect=false",
                    "-e", "sector", "--format", "bin", "--base-address",
                    "0x{:08X}".format(ecnvm_start), "--pack", pack, "--target", target,
                    "--probe", probe_id, "--no-wait", "--no-reset", padded_ecnvm_path]
                if logger:
                    logger.info("SWD programming ECNVM {} bytes ({} sector-padded) using {} erase".format(
                        ecnvm_path.stat().st_size, ecnvm_transfer_size,
                        ecnvm_pack_info["erase_size"]))
                ret, output = _run_pyocd(ecnvm_command, logger, isolated, env)
                if ret != 0:
                    return {"success": False, "message": f"ECNVM pyOCD flash failed; CNVM was not started: {output or ret}"}
                ecnvm_readback = isolated / "ecnvm-readback.bin"
                command = [*pyocd, "commander", *common, "-O", "auto_unlock=false", "-O", "dap_swj_use_dormant=true",
                    "-f", config["CONFIG_FLASH_SWD_FREQUENCY_HZ"],
                    "-O", "resume_on_disconnect=false", "--pack", pack, "--target", target,
                    "--probe", probe_id, "--no-wait", "--command",
                    "savemem 0x{:08X} {} ecnvm-readback.bin".format(ecnvm_start, ecnvm_transfer_size)]
                ret, output = _run_pyocd(command, logger, isolated, env)
                if ret != 0 or not ecnvm_readback.is_file() or ecnvm_readback.read_bytes() != ecnvm_payload:
                    return {"success": False, "message": "ECNVM readback failed or mismatched; CNVM was not started: {}".format(output or ret)}
            finally:
                if os.path.exists(padded_ecnvm_path):
                    os.unlink(padded_ecnvm_path)

        temp_image, transfer_size = _create_padded_image(
            image, capacity, erase_bytes)
        try:
            payload = Path(temp_image).read_bytes()
            command = [*pyocd, "flash", *common,
                       "-f", config["CONFIG_FLASH_SWD_FREQUENCY_HZ"],
                       "-O", "auto_unlock=false", "-O", "dap_swj_use_dormant=true",
                       "-O", "keep_unwritten=false",
                       "-O", "smart_flash=false",
                       "-O", "resume_on_disconnect=false",
                       "-e", "sector", "--format", "bin",
                       "--base-address", "0x{:08X}".format(start),
                       "--pack", pack,
                       "--target", target, "--probe", probe_id, "--no-wait", "--no-reset", temp_image]
            if logger:
                logger.info(
                    "SWD programming {} bytes ({} padded) using {} sector erase; "
                    "pyOCD target {}, probe {}".format(
                        image_size, transfer_size, pack_info["erase_size"], target, probe_id))
            ret, output = _run_pyocd(command, logger, isolated, env)
            if ret != 0:
                return {"success": False, "message": f"pyOCD flash failed: {output or ret}"}
            if _boolean(config, "CONFIG_FLASH_VERIFY", True):
                readback_path = isolated / "readback.bin"
                readback_cmd = [*pyocd, "commander", *common,
                                "-f", config["CONFIG_FLASH_SWD_FREQUENCY_HZ"],
                                "-O", "auto_unlock=false", "-O", "dap_swj_use_dormant=true",
                                "-O", "resume_on_disconnect=false",
                                "--pack", pack,
                                "--target", target, "--probe", probe_id, "--no-wait",
                                "--command", "savemem 0x{:08X} {} readback.bin".format(
                                    start, transfer_size)]
                ret, output = _run_pyocd(readback_cmd, logger, isolated, env)
                if ret != 0:
                    return {"success": False, "message": f"pyOCD readback failed: {output or ret}"}
                if not readback_path.is_file() or readback_path.read_bytes() != payload:
                    return {"success": False,
                            "message": "CNVM readback mismatch across the sector-padded boot/primary image"}
            if _boolean(config, "CONFIG_FLASH_VERIFY", True):
                message = f"SWD download complete; {transfer_size} padded CNVM bytes read back and verified"
            else:
                message = ("SWD download complete; readback verification skipped by "
                           "GD32H77X_FLASH_VERIFY=0")
            return {"success": True, "message": message}
        finally:
            if os.path.exists(temp_image):
                os.unlink(temp_image)


def _preflight_swd_external(config, report, image, port, start, limit,
                            capacity, device_geometry, logger):
    """Validate all static CNVM/SWD prerequisites before an OSPI write starts."""
    try:
        pyocd_version = importlib.metadata.version("pyocd")
    except importlib.metadata.PackageNotFoundError as exc:
        raise RuntimeError("External OSPI flashing requires pyocd==0.46.0") from exc
    if pyocd_version != "0.46.0":
        raise RuntimeError(f"External OSPI flashing requires pyocd==0.46.0, found {pyocd_version}")

    platform_root = Path(__file__).resolve().parent
    resolver_path = platform_root / "gd32h77x_os/tools/firmware/resolve_memory_layout.py"
    spec = importlib.util.spec_from_file_location("gd32h77x_resolver_ospi_preflight", resolver_path)
    if spec is None or spec.loader is None:
        raise RuntimeError("GD32H77X canonical device resolver is unavailable")
    resolver = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(resolver)
    identity = resolver.resolve_chip_identity(report["chip"], platform_root)
    target = report.get("layout", {}).get("target_chip", "")
    if target != identity["canonical_chip"]:
        raise ValueError("Build-bound canonical target does not match the selected device registry")

    pack = _resolve_swd_pack(config)
    if not pack:
        raise ValueError(
            "External OSPI flashing requires the approved GD32H77x CMSIS-Pack")
    pack = str(Path(pack).expanduser().resolve())
    geometry = {
        "capacity_bytes": identity["cnvm_capacity_bytes"],
        "erase_bytes": identity["cnvm_erase_bytes"],
        "program_bytes": identity["cnvm_program_bytes"],
        "vendor_algorithm": identity["cnvm_vendor_algorithm"],
    }
    _validate_swd_pack(platform_root, pack, target, geometry)
    image_size = image.stat().st_size
    padded_size = ((image_size + geometry["erase_bytes"] - 1)
                   // geometry["erase_bytes"]) * geometry["erase_bytes"]
    layout_flash = report.get("layout", {}).get("flash_image", {})
    if (int(layout_flash.get("start_address", -1)) != start
            or int(layout_flash.get("end_address", -1)) != limit
            or int(layout_flash.get("capacity_bytes", -1)) != capacity
            or limit - start != capacity or start % geometry["erase_bytes"]
            or padded_size > capacity or image_size <= 0):
        raise ValueError("CNVM image range/size failed SWD preflight")

    pyocd = [sys.executable, "-m", "pyocd"]
    with tempfile.TemporaryDirectory(prefix="gd32h77x-pyocd-preflight-") as temp_dir:
        isolated = Path(temp_dir)
        env = os.environ.copy()
        env["PYOCD_USER_DIR"] = str(isolated / "user")
        Path(env["PYOCD_USER_DIR"]).mkdir()
        common = ["--no-config", "--project", str(isolated), "--script", os.devnull]
        result = _run_pyocd([*pyocd, "json", "--targets", "--pack", pack, *common],
                            logger, isolated, env)
    if result[0] != 0:
        raise RuntimeError(f"pyOCD cannot load the selected DFP: {result[1]}")
    try:
        targets = json.loads(result[1]).get("targets", [])
    except (json.JSONDecodeError, AttributeError) as exc:
        raise RuntimeError("pyOCD returned invalid target-list JSON for the selected DFP") from exc
    found = [entry for entry in targets
             if entry.get("part_number", "").upper() == target.upper()
             and entry.get("vendor", "").lower() == "gigadevice"
             and entry.get("source") == "pack"]
    if len(found) != 1:
        raise RuntimeError(f"pyOCD did not expose exactly one {target} target from the selected DFP")
    probe_hint = str(port or "").strip()
    with tempfile.TemporaryDirectory(prefix="gd32h77x-pyocd-probes-") as temp_dir:
        isolated = Path(temp_dir)
        env = os.environ.copy()
        env["PYOCD_USER_DIR"] = str(isolated / "user")
        Path(env["PYOCD_USER_DIR"]).mkdir()
        common = ["--no-config", "--project", str(isolated), "--script", os.devnull]
        result = _run_pyocd([*pyocd, "json", "--probes", *common],
                            logger, isolated, env)
    if result[0] != 0:
        raise RuntimeError(f"pyOCD probe discovery failed: {result[1]}")
    try:
        probe_doc = json.loads(result[1])
        probes = probe_doc.get("boards", [])
    except (json.JSONDecodeError, AttributeError) as exc:
        raise RuntimeError("pyOCD returned invalid probe-list JSON") from exc
    if probe_doc.get("status") != 0:
        raise RuntimeError("pyOCD probe discovery failed: {}".format(
            probe_doc.get("error", "unknown error")))
    return pack, _select_swd_probe(probes, probe_hint)


def _flash_swd_with_ospi(config, report, current_layout, image, external_payloads,
                         port, start, limit, capacity, logger):
    _pack, probe_hint = _preflight_swd_external(
        config, report, image, port, start, limit, capacity,
        _find_cnvm_device(current_layout.get("layout", {})), logger)
    ospi = _ospi_module()
    prepare = getattr(ospi, "preflight_ospi", None)
    flash = getattr(ospi, "flash_ospi", None)
    if not callable(prepare) or not callable(flash):
        raise RuntimeError("GD32H77X OSPI writer has no preflight/program API")
    pack_path = _pack or _resolve_swd_pack(config)
    try:
        plan = prepare(current_layout, external_payloads, pack_path, probe_hint or None)
    except Exception as exc:
        raise RuntimeError(
            "OSPI read-only preflight failed; no flash writes were started: {}".format(exc)) from exc
    try:
        return _execute_ospi_then_cnvm(
            config, report, image, external_payloads, port, start, limit,
            capacity, logger, ospi, flash, plan, pack_path, probe_hint)
    finally:
        cleanup = getattr(ospi, "cleanup_plan", None)
        if callable(cleanup):
            cleanup(plan)


def _execute_ospi_then_cnvm(config, report, image, external_payloads,
                            port, start, limit, capacity, logger,
                            ospi, flash, plan, pack_path, probe_hint):
    if isinstance(plan, dict):
        selected_probe = plan.get("probe_id", "")
    else:
        selected_probe = getattr(plan, "probe_id", "")
    if not selected_probe:
        raise RuntimeError("OSPI preflight did not select a stable SWD probe ID")
    if selected_probe != probe_hint:
        raise RuntimeError(
            "OSPI preflight selected a different SWD probe than the completed pyOCD selection; "
            "no external or CNVM writes were started")
    verify = _boolean(config, "CONFIG_FLASH_VERIFY", True)
    try:
        external_result = flash(plan, selected_probe, verify=verify)
    except Exception as exc:
        raise RuntimeError(
            "OSPI writer failed before CNVM was started; inspect reported OSPI progress/partition "
            f"state because partial OSPI writes may have occurred: {exc}") from exc
    if not isinstance(external_result, dict) or not external_result.get("success", False):
        raise RuntimeError(
            "OSPI writer did not complete all explicit payloads; CNVM was not started; "
            "inspect OSPI progress/partition state: {}".format(external_result))
    if external_result.get("probe_id", selected_probe) != selected_probe:
        raise RuntimeError("OSPI writer changed the selected SWD probe ID")
    if verify and not external_result.get("verified", False):
        raise RuntimeError(
            "OSPI writer did not verify readback; CNVM was not started; OSPI writes may have occurred")
    written = external_result.get("partitions")
    if not written:
        written = [external_result]
    partition_names = ", ".join(
        str(item.get("partition", "?")) for item in written if isinstance(item, dict))
    if logger:
        if verify:
            logger.info("OSPI explicit payload readback verified on probe {}".format(selected_probe))
        else:
            logger.info("OSPI explicit payload programmed; readback verification disabled by internal flash policy")
    try:
        internal_result = _flash_swd(
            config, report, image, selected_probe,
            start, limit, capacity, logger)
    except Exception as exc:
        return {"success": False,
                "message": "OSPI stage completed for {}; CNVM stage raised an error on the same "
                           "probe {}: {}".format(partition_names or "explicit partitions",
                                                selected_probe, exc)}
    if not internal_result.get("success"):
        return {"success": False,
                "message": "OSPI stage completed for {}; CNVM stage failed on the same probe {}: {}".format(
                    partition_names or "explicit partitions", selected_probe,
                    internal_result.get("message", "unknown error"))}
    verification = ("OSPI and CNVM payloads read back and verified" if verify else
                    "OSPI and CNVM programmed; readback verification disabled by internal flash policy")
    return {"success": True,
            "message": f"{verification}; OSPI partitions {partition_names or '(package payloads)'} completed before CNVM boot+primary"}


def _validate_swd_pack(platform_root, pack, target, geometry):
    pack_module_path = platform_root / "gd32h77x_os/tools/download/swd_pack.py"
    pack_spec = importlib.util.spec_from_file_location("gd32h77x_swd_pack", pack_module_path)
    if pack_spec is None or pack_spec.loader is None:
        raise RuntimeError("GD32H77X CMSIS-Pack validator is unavailable")
    pack_module = importlib.util.module_from_spec(pack_spec)
    pack_spec.loader.exec_module(pack_module)
    return pack_module.validate_pack(pack, target, geometry)


def _run_pyocd(command, logger, cwd, env):
    if logger:
        logger.debug("Running isolated pyOCD command: {}".format(" ".join(map(str, command))))
    try:
        result = subprocess.run(command, cwd=str(cwd), env=env,
                                 capture_output=True, text=True, check=False)
    except OSError as exc:
        return 127, str(exc)
    output = "\n".join(part for part in (result.stdout, result.stderr) if part).strip()
    return result.returncode, output


def _select_swd_probe(probes: List[dict], selector: str):
    if selector:
        matches = [entry for entry in probes
                   if entry.get("unique_id", "") == selector]
        if not matches:
            matches = [entry for entry in probes
                       if selector in entry.get("unique_id", "")]
        if not matches:
            raise RuntimeError(f"No SWD probe matches -p/--port {selector!r}")
    else:
        matches = list(probes)
    if not matches:
        raise RuntimeError("No SWD debug probe found; connect a supported CMSIS-DAP probe")
    if len(matches) == 1:
        return matches[0]["unique_id"]
    print("Multiple SWD probes found:")
    for index, entry in enumerate(matches, 1):
        print("  {}. {} ({})".format(index, entry.get("unique_id"), entry.get("info", "unknown")))
    if not sys.stdin.isatty():
        identifiers = ", ".join(entry.get("unique_id", "?") for entry in matches)
        raise RuntimeError(
            "Multiple SWD probes are connected in non-interactive mode; rerun with "
            f"-p/--port with one probe ID ({identifiers})")
    for _ in range(3):
        try:
            answer = input("Select SWD probe by number (q to cancel): ").strip()
        except (EOFError, KeyboardInterrupt):
            raise RuntimeError("SWD probe selection cancelled; no flash command was started")
        if answer.lower() == "q":
            raise RuntimeError("SWD probe selection cancelled; no flash command was started")
        try:
            selected = int(answer)
        except ValueError:
            selected = 0
        if 1 <= selected <= len(matches):
            return matches[selected - 1]["unique_id"]
        print("Invalid selection. Enter a listed number or q to cancel.")
    raise RuntimeError("Too many invalid selections; no SWD flash command was started")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def platform_flash(using_data, binfile, port, baud, boards_root, logger,
                   verify=True):
    """Called by tos.py flash after the standard build artifact check."""
    package_temp = None
    try:
        config = _board_flash_config(using_data, boards_root)
        config["CONFIG_FLASH_VERIFY"] = "y" if verify else "n"
        port = str(port or "").strip()
        backend = _backend(config)
        if baud and backend != "uart":
            raise ValueError(
                "GD32H77X -b/--baud is only valid with the uart backend; "
                f"selected backend is {backend}")
        artifact = Path(binfile)
        if not artifact.is_file():
            raise ValueError(f"Firmware artifact not found: {artifact}")
        external_payloads = []
        mapped_payloads = []
        if zipfile.is_zipfile(artifact) or artifact.suffix.lower() == ".tufw":
            package_temp = tempfile.TemporaryDirectory(prefix="gd32h77x-qio-package-")
            image, external_payloads, mapped_payloads = _extract_validated_package(
                artifact, boards_root, using_data, Path(package_temp.name))
        elif artifact.suffix.lower() == ".bin":
            image = artifact
        else:
            raise ValueError("GD32H77X accepts a complete QIO package or legacy raw BIN image")
        if external_payloads and mapped_payloads:
            raise ValueError("QIO packages mixing indirect NOR and mapped ECNVM payloads are unsupported; no device opened")
        report, start, limit, capacity, device_geometry = _validate_manifest(
            image, boards_root, using_data)
        if "ecnvm_code" in report.get("layout", {}).get("partitions", {}) and not mapped_payloads:
            filename = report.get("mapped_payloads", [{}])[0].get("file")
            if not isinstance(filename, str) or not filename:
                raise ValueError("Layout declares ecnvm_code but report has no ECNVM payload")
            sidecar = image.parent / filename
            if not sidecar.is_file():
                raise ValueError("Layout declares ecnvm_code but the QIO package is missing its ECNVM payload")
            mapped_payloads = [dict(report["mapped_payloads"][0], path=str(sidecar))]
        if mapped_payloads and backend not in ("swd", "dfu"):
            raise ValueError(
                "ECNVM executable payloads require the swd or dfu backend; no probe opened")
        image_size = image.stat().st_size
        if image_size <= 0 or image_size > capacity:
            raise ValueError(
                f"Image size {image_size} is outside the application range "
                f"0x{start:08X}-0x{limit - 1:08X} ({capacity} bytes); "
                "refusing to touch OTA, metadata, or KV partitions")

        platform_device = _load_platform_device(_chip_from_using(using_data))
        if logger:
            logger.info(
                "Flashing with backend {}: device {} (order code {}), "
                "CNVM {} bytes, erase {} bytes, vendor algorithm {}".format(
                    backend,
                    device_geometry.get("part_number", platform_device["target_chip"]),
                    platform_device.get("order_code", "-"),
                    device_geometry.get("capacity_bytes"),
                    device_geometry.get("erase_bytes"),
                    device_geometry.get("vendor_algorithm") or "-"))

        current_layout = None
        if external_payloads:
            current_layout = _resolve_current_layout(
                boards_root, using_data.get("CONFIG_BOARD_CHOICE", ""),
                _chip_from_using(using_data))
            layout = current_layout.get("layout", {})
            devices = layout.get("devices", [])
            if isinstance(devices, list):
                device_map = {entry.get("id"): entry for entry in devices
                              if isinstance(entry, dict)}
            elif isinstance(devices, dict):
                device_map = devices
            else:
                raise ValueError("Resolved layout has an invalid external device table")
            bindings = layout.get("board_binding", {})
            for payload in external_payloads:
                if not payload.get("explicit_payload") or not payload.get("path"):
                    raise ValueError("Flash package external payload is not explicitly selected/extracted")
                segments = payload.get("segments", [])
                if not segments:
                    raise ValueError(
                        "GD32H77X package external payload contains no resolved device segments")
                for segment in segments:
                    device_id = segment.get("device")
                    device = device_map.get(device_id, {})
                    binding = bindings.get(device_id, {}) if isinstance(bindings, dict) else {}
                    if (segment.get("region") != "NOR" or device.get("region") != "NOR"
                            or device.get("kind") != "flash" or device.get("access") != "indirect"
                            or segment.get("backend_status") != device.get("backend_status")
                            or not isinstance(binding, dict)
                            or not str(binding.get("controller", "")).upper().startswith("OSPI")):
                        raise ValueError(
                            "External payload segment does not resolve to a registered indirect OSPI NOR device")
            if backend not in ("swd", "uart"):
                raise ValueError(
                    "External OSPI payloads require the swd or uart backend; "
                    "DFU mode cannot write NOR. "
                    "No device was opened or flash range modified.")
            if backend == "swd":
                return _flash_swd_with_ospi(
                    config, report, current_layout=current_layout,
                    image=image, external_payloads=external_payloads,
                    port=port, start=start, limit=limit, capacity=capacity,
                    logger=logger)
            return _flash_uart(
                config, report, current_layout, image, external_payloads,
                port, baud, start, limit, capacity, device_geometry, logger)

        if backend == "dfu":
            return _flash_dfu(config, report, image, start, limit, capacity,
                              device_geometry, port, logger, mapped_payloads)
        if backend == "uart":
            return _flash_uart(
                config, report, None, image, [], port, baud,
                start, limit, capacity, device_geometry, logger)
        return _flash_swd(config, report, image, port,
                          start, limit, capacity, logger, mapped_payloads)
    except Exception as exc:  # bridge contract reports failures to tos.py
        return {"success": False, "message": str(exc)}
    finally:
        if package_temp is not None:
            package_temp.cleanup()
