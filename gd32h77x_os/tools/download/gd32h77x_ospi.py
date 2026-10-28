"""Fail-closed GD25Q64E writer via a small SWD-loaded SRAM loader.

The external NOR uses the resolved OSPI0/OSPI1 board binding. The RAM loader
deliberately uses GPIO bit-banged single-line SPI, so it does not enable quad
mode or write any status register. The public API is split into a read-only
preflight and a write/readback operation for the bridge.
"""
from __future__ import annotations

import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from typing import Any

PLATFORM_ROOT = Path(__file__).resolve().parents[3]
LOADER_SOURCE = PLATFORM_ROOT / "gd32h77x_os/tools/download/ospi_loader"
GD25Q64E_JEDEC = 0xC84017
GD25Q64E_GEOMETRY = "GD25Q64ESIG"
GD25Q64E_CAPACITY = 8 * 1024 * 1024
GD25Q64E_ERASE = 4096
GD25Q64E_PAGE = 256
_PIN_RE = __import__("re").compile(r"^P([A-K])(0|[1-9]|1[0-5])$")
MAILBOX_MAGIC = 0x48574F53
REQUEST_MAGIC = 0x52514553
RESPONSE_RUNNING = 1
RESPONSE_DONE = 2
RESPONSE_ERROR = 3
CMD_READ_ID = 1
CMD_READ_STATUS = 2
CMD_READ = 3
CMD_ERASE_4K = 4
CMD_PROGRAM = 5
MAILBOX_ADDRESS = 0x24010000
BUFFER_ADDRESS = 0x24011000
LOADER_BASE = 0x24000000
LOADER_STACK_TOP = 0x24030000


class OspiFlashError(RuntimeError):
    """Layout, probe, loader, or GD25Q64E operation failed."""


class OspiPlan:
    def __init__(self, **values: Any):
        for key, value in values.items():
            setattr(self, key, value)

    def to_dict(self) -> dict[str, Any]:
        return dict(vars(self))

    def close(self) -> None:
        shutil.rmtree(Path(self.loader_bin).parent, ignore_errors=True)

    @classmethod
    def from_value(cls, value: "OspiPlan | dict[str, Any]") -> "OspiPlan":
        if isinstance(value, cls):
            return value
        return value if isinstance(value, cls) else cls(**dict(value))


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _validate_layout(manifest: dict[str, Any],
                     external_payloads: list[dict[str, Any]] | None = None) -> tuple[dict[str, Any], dict[str, Any]]:
    layout = manifest.get("layout")
    if not isinstance(layout, dict):
        raise OspiFlashError("resolved layout is missing")
    target = str(layout.get("target_chip", "")).upper()
    registry_path = PLATFORM_ROOT / "gd32h77x_os/chips/platform_chips.json"
    registry = json.loads(registry_path.read_text()).get("platform_chips", {})
    if target not in registry or registry[target].get("target_chip") != target:
        raise OspiFlashError(f"OSPI loader requires a canonical registered H77 target, got {target!r}")
    devices = layout.get("devices")
    if not isinstance(devices, list):
        raise OspiFlashError("resolved layout devices are missing")
    nor = [device for device in devices
           if str(device.get("part_number", "")).upper() == GD25Q64E_GEOMETRY]
    if not nor:
        raise OspiFlashError("resolved layout has no supported GD25Q64ESIG device")
    if external_payloads:
        partitions = layout.get("partitions", {})
        selected_ids = set()
        for payload in external_payloads:
            part = partitions.get(str(payload.get("partition", "")), {})
            for segment in part.get("segments", []) if isinstance(part, dict) else []:
                device_id = segment.get("device")
                if any(str(item.get("id")) == str(device_id) for item in nor):
                    selected_ids.add(str(device_id))
        if len(selected_ids) != 1:
            raise OspiFlashError("selected external payloads must use one supported GD25Q64ESIG device")
        nor = [item for item in nor if str(item.get("id")) in selected_ids]
    elif len(nor) > 1:
        raise OspiFlashError("layout has multiple GD25Q64ESIG devices; select payloads for one device")
    device = nor[0]
    expected = {
        "kind": "flash",
        "capacity_bytes": GD25Q64E_CAPACITY,
        "erase_bytes": GD25Q64E_ERASE,
        "page_bytes": GD25Q64E_PAGE,
        "cpu_base": None,
        "executable": False,
    }
    for key, value in expected.items():
        if device.get(key) != value:
            raise OspiFlashError(f"GD25Q64E {key} does not match the supported geometry")
    if device.get("access") != "indirect":
        raise OspiFlashError("GD25Q64E must be an indirect-access device")
    device_id = str(device.get("id", ""))
    binding = layout.get("board_binding", {}).get(device_id)
    _normalize_pin_binding(binding)
    return layout, device


def _normalize_pin_binding(binding: Any) -> dict[str, str]:
    if not isinstance(binding, dict) or binding.get("controller") not in ("OSPI0", "OSPI1"):
        raise OspiFlashError("GD25Q64E requires a resolved OSPI0/OSPI1 board_binding")
    data = binding.get("data")
    if not isinstance(data, list) or len(data) < 2 or len(data) > 4:
        raise OspiFlashError("OSPI data binding must contain MOSI, MISO and optional WP/HOLD/D3 pins")
    names = ["CS", "CLK", "MOSI", "MISO", "WP", "HOLD"]
    values = [binding.get("chip_select"), binding.get("clock"), *data]
    if len(values) == 5:
        values.append(None)
    result: dict[str, str] = {}
    seen = set()
    for name, value in zip(names, values):
        if value is None:
            continue
        if not isinstance(value, str) or not _PIN_RE.fullmatch(value):
            raise OspiFlashError(f"invalid OSPI {name} pin {value!r}")
        if value in seen:
            raise OspiFlashError(f"conflicting OSPI pin assignment {value}")
        seen.add(value)
        result[name.lower()] = value
    if not all(key in result for key in ("cs", "clk", "mosi", "miso")):
        raise OspiFlashError("OSPI board_binding is missing CS/CLK/MOSI/MISO")
    return result


def _target_macro(target: str) -> str:
    import re
    if not re.fullmatch(r"GD32H77[A-Z0-9]{3}", target):
        raise OspiFlashError(f"unsupported GD32H77 canonical target {target!r}")
    macro = target[:-2] + "X" + target[-1]
    header = (PLATFORM_ROOT / "gd32h77x_os/gd32/CMSIS/GD/GD32H77x_78x/Include/gd32h77x_78x.h").read_text()
    if f"defined({macro})" not in header:
        raise OspiFlashError(f"vendor CMSIS does not register target macro {macro}")
    return macro


def _pin_macros(binding: dict[str, Any]) -> dict[str, str]:
    pins = _normalize_pin_binding(binding)
    macros = {}
    for key, pin in pins.items():
        match = _PIN_RE.fullmatch(pin)
        port, number = match.groups()
        macros[f"OSPI_{key.upper()}_GPIO"] = f"GPIO{port}"
        macros[f"OSPI_{key.upper()}_RCU"] = f"RCU_GPIO{port}"
        macros[f"OSPI_{key.upper()}_PIN"] = f"GPIO_PIN_{number}"
    return macros


def _validated_payloads(layout: dict[str, Any], device: dict[str, Any],
                        external_payloads: list[dict[str, Any]]) -> tuple[dict[str, Any], ...]:
    partitions = layout.get("partitions", {})
    if not isinstance(partitions, dict):
        raise OspiFlashError("resolved layout partitions are missing")
    plan: list[dict[str, Any]] = []
    device_id = str(device["id"])
    for payload in external_payloads:
        if not isinstance(payload, dict):
            raise OspiFlashError("external payload entry must be an object")
        part_name = str(payload.get("partition", ""))
        part = partitions.get(part_name)
        if not isinstance(part, dict):
            raise OspiFlashError(f"unknown external payload partition {part_name!r}")
        path_value = payload.get("path")
        if not path_value:
            raise OspiFlashError(f"external payload {part_name!r} has no extracted file path")
        path = Path(str(path_value)).resolve(strict=True)
        if not path.is_file():
            raise OspiFlashError(f"external payload is not a regular file: {path}")
        size = path.stat().st_size
        if size <= 0:
            raise OspiFlashError(f"external payload {part_name!r} is empty")
        expected_size = int(payload.get("size_bytes", -1))
        expected_hash = str(payload.get("sha256", "")).lower()
        if expected_size != size or len(expected_hash) != 64 or _sha256(path) != expected_hash:
            raise OspiFlashError(f"external payload {part_name!r} size/hash mismatch")
        if size % GD25Q64E_ERASE:
            raise OspiFlashError(
                f"external payload {part_name!r} is not padded to a 4 KiB erase boundary")
        segments = part.get("segments", [])
        if not isinstance(segments, list) or not segments:
            raise OspiFlashError(f"partition {part_name!r} has no segments")
        external_segments = []
        for segment in segments:
            if segment.get("device") != device_id:
                raise OspiFlashError(
                    f"partition {part_name!r} has a segment on an unsupported device")
            offset = int(segment["device_offset_bytes"])
            logical_offset = int(segment["logical_offset_bytes"])
            segment_size = int(segment["size_bytes"])
            if offset < 0 or segment_size <= 0 or offset + segment_size > GD25Q64E_CAPACITY:
                raise OspiFlashError(f"partition {part_name!r} exceeds GD25Q64E bounds")
            external_segments.append({
                "device_offset_bytes": offset,
                "partition_offset_bytes": logical_offset,
                "size_bytes": segment_size,
            })
        external_segments.sort(key=lambda item: item["partition_offset_bytes"])
        cursor = 0
        for segment in external_segments:
            if segment["partition_offset_bytes"] != cursor:
                raise OspiFlashError(f"partition {part_name!r} has a logical gap/overlap")
            cursor += segment["size_bytes"]
        if size > cursor:
            raise OspiFlashError(f"payload {part_name!r} is larger than its partition")
        remaining = size
        spans = []
        for segment in external_segments:
            if remaining == 0:
                break
            span_size = min(remaining, segment["size_bytes"])
            start = segment["device_offset_bytes"]
            end = start + span_size
            erase_start = (start // GD25Q64E_ERASE) * GD25Q64E_ERASE
            erase_end = ((end + GD25Q64E_ERASE - 1) // GD25Q64E_ERASE) * GD25Q64E_ERASE
            segment_end = segment["device_offset_bytes"] + segment["size_bytes"]
            if erase_start < segment["device_offset_bytes"] or erase_end > segment_end:
                raise OspiFlashError(
                    f"erasing payload {part_name!r} would touch outside its declared segment")
            if erase_start % GD25Q64E_ERASE or erase_end > GD25Q64E_CAPACITY:
                raise OspiFlashError(f"invalid erase range for {part_name!r}")
            spans.append({
                **segment,
                "program_size_bytes": span_size,
                "erase_start_bytes": erase_start,
                "erase_end_bytes": erase_end,
            })
            remaining -= span_size
        if remaining != 0:
            raise OspiFlashError(f"payload {part_name!r} was not fully mapped")
        plan.append({
            "partition": part_name,
            "path": str(path),
            "size_bytes": size,
            "sha256": expected_hash,
            "spans": spans,
            "eraseRanges": [
                {"offset": span["erase_start_bytes"],
                 "size": span["erase_end_bytes"] - span["erase_start_bytes"]}
                for span in spans
            ],
        })
    if not plan:
        raise OspiFlashError("no external payloads were provided")
    return tuple(plan)


def _find_compiler() -> str | None:
    override = os.environ.get("ARM_NONE_EABI_GCC", "").strip()
    if override:
        found = shutil.which(override)
        if found:
            return found
        candidate = Path(override).expanduser()
        if candidate.is_file():
            return str(candidate.resolve())

    toolchain = os.environ.get("GD32H77X_TOOLCHAIN", "").strip()
    if toolchain:
        binary_dir = Path(toolchain).expanduser() / "bin"
        names = ("arm-none-eabi-gcc.exe", "arm-none-eabi-gcc")
        for name in names:
            candidate = binary_dir / name
            found = shutil.which(str(candidate))
            if found:
                return found
            if candidate.is_file():
                return str(candidate.resolve())

    found = shutil.which("arm-none-eabi-gcc")
    if found:
        return found
    for root in sorted(Path("/opt").glob("gcc-arm-none-eabi-*")):
        for name in ("arm-none-eabi-gcc", "arm-none-eabi-gcc.exe"):
            candidate = root / "bin" / name
            if candidate.is_file():
                return str(candidate)
    return None


def _build_loader(target_chip: str, pin_binding: dict[str, Any]) -> tuple[str, str, str, str]:
    compiler = _find_compiler()
    if not compiler:
        raise OspiFlashError("arm-none-eabi-gcc is required to build the SRAM OSPI loader")
    directory = Path(tempfile.mkdtemp(prefix="gd32h77x-ospi-loader-"))
    script = LOADER_SOURCE / "build_loader.py"
    config_path = directory / "loader-config.json"
    pin_config = _normalize_pin_binding(pin_binding)
    target_macro = _target_macro(target_chip)
    config = {"target": target_chip, "target_macro": target_macro, "pins": pin_config,
              "pin_macros": _pin_macros(pin_binding)}
    config["binding_sha256"] = hashlib.sha256(
        json.dumps(config, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    config_path.write_text(json.dumps(config, sort_keys=True) + "\n")
    try:
        subprocess.run([sys.executable, str(script), "--compiler", compiler,
                        "--out-dir", str(directory), "--config", str(config_path)], check=True, capture_output=True,
                        text=True)
    except subprocess.CalledProcessError as exc:
        shutil.rmtree(directory, ignore_errors=True)
        raise OspiFlashError(f"failed to build SRAM OSPI loader: {exc.stderr}") from exc
    metadata_path = directory / "loader.json"
    metadata = json.loads(metadata_path.read_text())
    binary = directory / metadata["binary"]
    digest = _sha256(binary)
    if digest != metadata["binary_sha256"]:
        shutil.rmtree(directory, ignore_errors=True)
        raise OspiFlashError("SRAM loader build metadata hash mismatch")
    return str(directory / "gd32h77x_ospi_loader.elf"), str(binary), digest, str(metadata_path)


def preflight_ospi(manifest: dict[str, Any], external_payloads: list[dict[str, Any]],
                   pack_path: str, probe_id: str | None = None) -> OspiPlan:
    """Validate package/layout, build loader, then read-only probe GD25Q64E."""
    layout, device = _validate_layout(manifest, external_payloads)
    payloads = _validated_payloads(layout, device, external_payloads)
    pack = Path(pack_path).expanduser().resolve(strict=True)
    if not pack.is_file():
        raise OspiFlashError("CMSIS-Pack path is not a file")
    target_chip = str(layout["target_chip"]).upper().removesuffix("K7")
    binding = layout["board_binding"][str(device["id"])]
    elf, binary, loader_hash, metadata = _build_loader(target_chip, binding)
    loader_meta = json.loads(Path(metadata).read_text())
    pins = _normalize_pin_binding(binding)
    if loader_meta.get("target") != target_chip or loader_meta.get("pins") != pins:
        shutil.rmtree(Path(binary).parent, ignore_errors=True)
        raise OspiFlashError("SRAM loader target/pin binding differs from resolved layout")
    try:
        context = _open_target(pack, target_chip, probe_id)
        with context as target:
            _launch_loader(target, Path(binary), json.loads(Path(metadata).read_text()))
            info = _initialize_loader(target)
            _require_nor_ready(info)
    except Exception:
        shutil.rmtree(Path(binary).parent, ignore_errors=True)
        raise
    return OspiPlan(
        schema_version=1,
        probe_id=context.probe_id,
        target_chip=target_chip,
        source_digest=str(manifest.get("source", {}).get("sourceDigest", "")),
        layout_digest=str(manifest.get("normalizedLayoutDigest", "")),
        loader_elf=elf,
        loader_bin=binary,
        loader_sha256=loader_hash,
        loader_metadata=metadata,
        payloads=payloads,
        pack_path=str(pack),
        device_id=str(device["id"]),
        geometry_id=str(device.get("geometry_id", "")),
        pin_binding=pins,
        binding_sha256=loader_meta["binding_sha256"],
    )


def flash_ospi(plan: OspiPlan | dict[str, Any], probe_id: str | None = None,
               verify: bool = True) -> dict[str, Any]:
    """Erase only payload-covered sectors, program pages, and optionally read back."""
    plan = OspiPlan.from_value(plan)
    selected_probe = probe_id or plan.probe_id
    if plan.probe_id and selected_probe != plan.probe_id:
        raise OspiFlashError("selected SWD probe differs from the preflighted probe")
    results = []
    completed: list[str] = []
    progress = {"partition": "initialization", "phase": "loader setup", "offset": None}
    try:
        metadata = json.loads(Path(plan.loader_metadata).read_text())
        binary = Path(plan.loader_bin)
        if (_sha256(binary) != plan.loader_sha256 or
                metadata.get("binary_sha256") != plan.loader_sha256 or
                metadata.get("target") != plan.target_chip or
                metadata.get("pins") != plan.pin_binding or
                metadata.get("binding_sha256") != plan.binding_sha256):
            raise OspiFlashError("SRAM loader changed after preflight")
        context = _open_target(Path(plan.pack_path), plan.target_chip, selected_probe)
        with context as target:
            _launch_loader(target, binary, metadata)
            _require_nor_ready(_initialize_loader(target))
            for payload in plan.payloads:
                progress = {"partition": payload["partition"],
                            "phase": "payload validation", "offset": None}
                path = Path(payload["path"])
                data = path.read_bytes()
                if len(data) != payload["size_bytes"] or hashlib.sha256(data).hexdigest() != payload["sha256"]:
                    raise OspiFlashError(f"payload {payload['partition']!r} changed after preflight")
                cursor = 0
                payload_readback = bytearray()
                for span in payload["spans"]:
                    span_data = data[cursor:cursor + span["program_size_bytes"]]
                    cursor += span["program_size_bytes"]
                    for sector in range(span["erase_start_bytes"],
                                        span["erase_end_bytes"], GD25Q64E_ERASE):
                        progress = {"partition": payload["partition"],
                                    "phase": "erase", "offset": sector}
                        _request(target, CMD_ERASE_4K, sector, GD25Q64E_ERASE)
                    for offset in range(0, len(span_data), GD25Q64E_PAGE):
                        chunk = span_data[offset:offset + GD25Q64E_PAGE]
                        progress = {"partition": payload["partition"],
                                    "phase": "program",
                                    "offset": span["device_offset_bytes"] + offset}
                        _request(target, CMD_PROGRAM,
                                 span["device_offset_bytes"] + offset, len(chunk), chunk)
                    if verify:
                        readback = bytearray()
                        for offset in range(0, len(span_data), GD25Q64E_PAGE):
                            count = min(GD25Q64E_PAGE, len(span_data) - offset)
                            progress = {"partition": payload["partition"],
                                        "phase": "readback",
                                        "offset": span["device_offset_bytes"] + offset}
                            readback.extend(_request(
                                target, CMD_READ,
                                span["device_offset_bytes"] + offset, count))
                        if bytes(readback) != span_data:
                            raise OspiFlashError(
                                f"GD25Q64E readback mismatch in partition {payload['partition']!r}")
                        payload_readback.extend(readback)
                    else:
                        pass
                results.append({
                    "partition": payload["partition"],
                    "device": plan.device_id,
                    "offset": payload["spans"][0]["device_offset_bytes"],
                    "size": payload["size_bytes"],
                    "eraseRanges": payload["eraseRanges"],
                    "payloadSha256": payload["sha256"],
                    "readbackSha256": hashlib.sha256(payload_readback).hexdigest()
                    if verify else None,
                    "verified": bool(verify),
                })
                completed.append(payload["partition"])
    except Exception as exc:
        offset = (f"0x{progress['offset']:x}" if progress["offset"] is not None
                  else "unknown")
        raise OspiFlashError(
            f"OSPI write failed; completed partitions={completed}; "
            f"current partition={progress['partition']} phase={progress['phase']} "
            f"device offset={offset}: {exc}") from exc
    finally:
        shutil.rmtree(Path(plan.loader_bin).parent, ignore_errors=True)
    return {
        "success": True,
        "device": GD25Q64E_GEOMETRY,
        "probe_id": selected_probe,
        "verified": bool(verify),
        "partitions": results,
    }


def cleanup_plan(plan: OspiPlan | dict[str, Any]) -> None:
    """Remove the temporary loader after a bridge preflight/program failure."""
    OspiPlan.from_value(plan).close()


def _open_target(pack_path: Path, target_chip: str, probe_id: str | None):
    try:
        from pyocd.core.helpers import ConnectHelper
        from pyocd.target.pack.cmsis_pack import CmsisPack
        from pyocd.target.pack.pack_target import PackTargets
    except ImportError as exc:
        raise OspiFlashError("pyocd==0.46.0 is required for GD32H77X SWD") from exc
    if importlib.metadata.version("pyocd") != "0.46.0":
        raise OspiFlashError("GD32H77X OSPI writer requires pyocd==0.46.0")
    pack = CmsisPack(str(pack_path))
    devs = [dev for dev in pack.devices if dev.part_number.upper() == target_chip.upper()]
    if len(devs) != 1:
        raise OspiFlashError(f"selected CMSIS-Pack does not uniquely define {target_chip}")
    PackTargets.populate_device(devs[0])
    project_dir = tempfile.mkdtemp(prefix="gd32h77x-ospi-pyocd-")
    options = {
        "no_config": True,
        "user_script": os.devnull,
        "project_dir": project_dir,
        "auto_unlock": False,
        "resume_on_disconnect": False,
        "connect_mode": "halt",
        "target_override": target_chip.lower(),
        "frequency": 1000000,
    }
    old_cwd = os.getcwd()
    probes = ConnectHelper.get_all_connected_probes(blocking=False)
    candidates = ([probe for probe in probes
                   if probe_id.lower() in probe.unique_id.lower()]
                  if probe_id else list(probes))
    if len(candidates) != 1:
        shutil.rmtree(project_dir, ignore_errors=True)
        raise OspiFlashError("select exactly one SWD probe with --port/-p")
    selected_probe_id = candidates[0].unique_id
    try:
        session = ConnectHelper.session_with_chosen_probe(
            blocking=False, unique_id=selected_probe_id,
            return_first=False, options=options)
    except Exception:
        os.chdir(old_cwd)
        shutil.rmtree(project_dir, ignore_errors=True)
        raise
    if session is None:
        os.chdir(old_cwd)
        shutil.rmtree(project_dir, ignore_errors=True)
        raise OspiFlashError("selected SWD probe is unavailable")
    return _TargetContext(session, selected_probe_id, old_cwd)


class _TargetContext:
    def __init__(self, session, probe_id: str, old_cwd: str):
        self.session = session
        self.target = None
        self.project_dir = session.project_dir
        self.probe_id = probe_id
        self.old_cwd = old_cwd

    def __enter__(self):
        try:
            self.session.open()
            self.target = self.session.target
            self.target.reset_and_halt()
            return self.target
        except Exception:
            try:
                self.session.close()
            finally:
                shutil.rmtree(self.project_dir, ignore_errors=True)
                os.chdir(self.old_cwd)
            raise

    def __exit__(self, exc_type, exc, tb):
        try:
            if self.target is not None:
                self.target.halt()
        finally:
            try:
                self.session.close()
            finally:
                shutil.rmtree(self.project_dir, ignore_errors=True)
                os.chdir(self.old_cwd)


def _launch_loader(target, binary: Path, metadata: dict[str, Any]) -> None:
    base = int(LOADER_BASE)
    mailbox = int(metadata.get("mailbox_address", "0"), 0)
    buffer = int(metadata.get("buffer_address", "0"), 0)
    stack_top = int(metadata.get("stack_top", "0"), 0)
    if (mailbox, buffer, stack_top) != (MAILBOX_ADDRESS, BUFFER_ADDRESS, LOADER_STACK_TOP):
        raise OspiFlashError("SRAM loader mailbox/buffer/stack metadata is unsupported")
    entry = int(metadata["entry_address"], 0) if "entry_address" in metadata else base | 1
    image = binary.read_bytes()
    padded = image + b"\x00" * ((-len(image)) & 3)
    target.write_memory_block32(base, list(struct.unpack("<" + "I" * (len(padded) // 4), padded)))
    # Initialize shared words over the debug AP before the core enters the loop.
    target.write_memory_block32(mailbox, [0] * 11)
    target.write_memory_block32(buffer, [0] * (GD25Q64E_PAGE // 4))
    target.write_core_register("sp", stack_top)
    target.write_core_register("xpsr", 0x01000000)
    target.write_core_register("pc", entry)
    target.resume()
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        if target.read_memory(MAILBOX_ADDRESS, 32) == MAILBOX_MAGIC:
            return
        time.sleep(0.01)
    target.halt()
    raise OspiFlashError("SRAM loader did not start or AXI SRAM mailbox is not visible")


def _mailbox_words(target) -> list[int]:
    return target.read_memory_block32(MAILBOX_ADDRESS, 11)


def _request(target, command: int, offset: int = 0, length: int = 0,
             data: bytes | None = None) -> bytes:
    if command == CMD_ERASE_4K:
        if (length != GD25Q64E_ERASE or data is not None or
                offset < 0 or offset % GD25Q64E_ERASE or
                offset + length > GD25Q64E_CAPACITY):
            raise OspiFlashError("GD25Q64E erase command requires exactly 4096 bytes")
    elif command in (CMD_READ, CMD_PROGRAM):
        if (length <= 0 or length > GD25Q64E_PAGE or offset < 0 or
                offset + length > GD25Q64E_CAPACITY):
            raise OspiFlashError("GD25Q64E read/program length is outside 1..256")
        if command == CMD_PROGRAM:
            if data is None or len(data) != length or (
                    offset // GD25Q64E_PAGE != (offset + length - 1) // GD25Q64E_PAGE):
                raise OspiFlashError("GD25Q64E page program must fit one page and match its data")
        elif data is not None:
            raise OspiFlashError("GD25Q64E read cannot carry payload data")
    elif length != 0 or data is not None or offset != 0:
        raise OspiFlashError("GD25Q64E ID/status command must have zero offset and empty payload")
    words = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
    current = _mailbox_words(target)
    sequence = (current[2] + 1) & 0xFFFFFFFF
    words[2] = sequence
    words[3] = command
    words[4] = offset
    words[5] = length
    target.halt()
    if data is not None:
        padded = data + b"\x00" * ((-len(data)) & 3)
        if padded:
            target.write_memory_block32(BUFFER_ADDRESS,
                                        list(memoryview(padded).cast("I")))
    target.write_memory_block32(MAILBOX_ADDRESS + 4, words[1:])
    target.write_memory(MAILBOX_ADDRESS + 4, REQUEST_MAGIC, 32)
    target.resume()
    deadline = time.monotonic() + 15.0
    while time.monotonic() < deadline:
        current = _mailbox_words(target)
        if current[6] in (RESPONSE_DONE, RESPONSE_ERROR) and current[1] == 0:
            target.halt()
            if current[6] != RESPONSE_DONE or current[7] != 0:
                raise OspiFlashError(
                    f"SRAM loader command {command} failed with status {current[7]}")
            if command == CMD_READ_ID:
                return current[8].to_bytes(4, "big")[-3:]
            if command == CMD_READ_STATUS:
                return bytes((current[9] & 0xFF, current[10] & 0xFF))
            if command == CMD_READ:
                return bytes(target.read_memory_block8(BUFFER_ADDRESS, length))
            return b""
        time.sleep(0.01)
    target.halt()
    raise OspiFlashError(f"SRAM loader command {command} timed out")


def _initialize_loader(target) -> dict[str, int]:
    _request(target, CMD_READ_ID)
    words = _mailbox_words(target)
    return {"jedec_id": words[8], "status1": words[9] & 0xFF,
            "status2": words[10] & 0xFF}


def _require_nor_ready(info: dict[str, int]) -> None:
    if info["jedec_id"] != GD25Q64E_JEDEC:
        raise OspiFlashError(
            f"GD25Q64E identity mismatch: expected 0x{GD25Q64E_JEDEC:06X}, "
            f"got 0x{info['jedec_id']:06X}")
    sr1 = info["status1"]
    sr2 = info["status2"]
    if sr1 & 0x01:
        raise OspiFlashError("GD25Q64E is busy/suspended; refusing to write")
    if sr1 & 0x7C:
        raise OspiFlashError("GD25Q64E main-array BP protection is active")
    if sr2 & 0xC0:
        raise OspiFlashError("GD25Q64E CMP protection or suspend state is active")

