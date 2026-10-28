#!/usr/bin/env python3
"""Resolve GD32H77X default/board memory layout and emit build artifacts."""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import math
import re
import sys
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


MAX_DEVICES = 8
MAX_PARTITIONS = 32
MAX_SEGMENTS = 128
_PLATFORM_NAME = "GD32H77X"
_ACCESS = {"mapped": 0, "indirect": 1}
_BACKEND = {"implemented": 0, "partial": 1, "not_implemented": 2}
_ROLE_FLAGS = {
    "boot": 1 << 0,
    "primary": 1 << 1,
    "ota_staging": 1 << 2,
    "ota_scratch": 1 << 3,
    "ota_metadata": 1 << 4,
    "kv": 1 << 5,
    "ota_backup": 1 << 6,
    "filesystem": 1 << 7,
    "data": 1 << 8,
}
_LAYOUT_FIELDS = {"schema_version", "layout_id", "platform", "platform_chip",
                  "target_chip", "devices", "partitions", "linker_memory",
                  "ota_policy", "board_binding"}
_DEVICE_FIELDS = {"geometry", "access", "backend_status"}
_PARTITION_FIELDS = {"role", "logical_base", "size_bytes", "erase_bytes",
                     "program_bytes", "segments"}
_SEGMENT_FIELDS = {"logical_offset_bytes", "device", "device_offset_bytes", "size_bytes"}
_LINKER_FIELDS = {"name", "partition", "geometry", "attributes"}
_OTA_FIELDS = {"status", "primary", "staging", "scratch", "metadata", "kv",
               "backup", "install_strategy", "rollback"}
_GEOMETRY_FIELDS = {"kind", "part_number", "region", "capacity_bytes", "erase_bytes",
                    "program_bytes", "page_bytes", "cpu_base", "executable",
                    "allowed_backend_status", "default_backend_status",
                    "vendor_algorithm", "evidence"}


class LayoutError(ValueError):
    """Invalid or unsupported memory-layout configuration."""


def _unique_object(pairs: Iterable[Tuple[str, Any]]) -> Dict[str, Any]:
    result: Dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise LayoutError("duplicate JSON key: {}".format(key))
        result[key] = value
    return result


def _load_json(path: Path) -> Tuple[dict, bytes]:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise LayoutError("cannot read {}: {}".format(path, exc)) from exc
    try:
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=_unique_object)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise LayoutError("invalid JSON in {}: {}".format(path, exc)) from exc
    if not isinstance(value, dict):
        raise LayoutError("JSON root must be an object: {}".format(path))
    return value, raw


def _integer(value: Any, field: str, allow_none: bool = False) -> Optional[int]:
    if value is None and allow_none:
        return None
    if isinstance(value, bool):
        raise LayoutError("{} must be an integer".format(field))
    try:
        if isinstance(value, str):
            parsed = int(value, 0)
        elif isinstance(value, int):
            parsed = value
        else:
            raise ValueError()
    except ValueError as exc:
        raise LayoutError("{} must be an integer".format(field)) from exc
    if parsed < 0:
        raise LayoutError("{} cannot be negative".format(field))
    return parsed


def _required_string(obj: dict, key: str, context: str) -> str:
    value = obj.get(key)
    if not isinstance(value, str) or not value.strip():
        raise LayoutError("{}.{} must be a non-empty string".format(context, key))
    return value


def _required_string_list(obj: dict, key: str, context: str) -> List[str]:
    value = obj.get(key)
    if not isinstance(value, list) or not value:
        raise LayoutError("{}.{} must be a non-empty string array".format(context, key))
    result: List[str] = []
    for item in value:
        if not isinstance(item, str) or not item.strip():
            raise LayoutError("{}.{} must be a non-empty string array".format(context, key))
        if item in result:
            raise LayoutError("{}.{} repeats {}".format(context, key, item))
        result.append(item)
    return result


def _reject_unknown(obj: dict, allowed: set, context: str) -> None:
    unknown = sorted(set(obj) - allowed)
    if unknown:
        raise LayoutError("unknown {} field(s): {}".format(context, ", ".join(unknown)))


def _canonical_json(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")


def _csv_rows(path: Path, expected_header: List[str]) -> Tuple[List[dict], bytes]:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise LayoutError("cannot read {}: {}".format(path, exc)) from exc
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        raise LayoutError("CSV must be UTF-8: {}".format(path)) from exc
    reader = csv.reader(io.StringIO(text, newline=""), skipinitialspace=False, strict=True)
    rows = []
    header = None
    try:
        for line_number, row in enumerate(reader, 1):
            if not row or (len(row) == 1 and not row[0].strip()) or row[0].lstrip().startswith("#"):
                continue
            if header is None:
                header = [cell.strip() for cell in row]
                if header != expected_header:
                    raise LayoutError("invalid CSV header in {}: expected {}".format(path, ",".join(expected_header)))
                continue
            if len(row) != len(expected_header):
                raise LayoutError("CSV row {} in {} has {} columns; expected {}".format(
                    line_number, path, len(row), len(expected_header)))
            rows.append({key: value.strip() for key, value in zip(expected_header, row)})
    except csv.Error as exc:
        raise LayoutError("invalid CSV syntax in {}: {}".format(path, exc)) from exc
    if header is None:
        raise LayoutError("CSV is missing header: {}".format(path))
    return rows, raw


def _csv_number(value: str, field: str) -> int:
    text = value.strip()
    match = re.fullmatch(r"(-?(?:0[xX][0-9a-fA-F]+|[0-9]+))([kKmM]?)", text)
    if not match:
        raise LayoutError("{} must be decimal, 0x hex, or K/M bytes".format(field))
    number_text, suffix = match.groups()
    sign = -1 if number_text.startswith("-") else 1
    unsigned_text = number_text[1:] if sign < 0 else number_text
    number = sign * int(unsigned_text, 16 if unsigned_text.lower().startswith("0x") else 10)
    if suffix:
        number *= 1024 if suffix.lower() == "k" else 1024 * 1024
    if number < 0:
        raise LayoutError("{} cannot be negative".format(field))
    return number


_STANDARD_ROLE = {
    "boot": "boot", "primary": "primary", "staging": "ota_staging",
    "scratch": "ota_scratch", "metadata": "ota_metadata", "kv": "kv",
    "old_backup": "ota_backup", "filesystem": "filesystem", "tuya": "kv",
}


def _load_csv_layout(partition_path: Path, device_path: Path, *, expected: dict,
                     geometries: dict, source_kind: str,
                     device_source_kind: str) -> Tuple[dict, List[dict]]:
    partition_rows, partition_raw = _csv_rows(
        partition_path, ["name", "device", "offset", "size", "logical_addr"])
    device_rows, device_raw = _csv_rows(
        device_path, ["device", "geometry", "controller", "cs", "clk", "d0", "d1", "d2", "d3"])
    device_defs = {}
    bindings = {}
    for row in device_rows:
        device_id = row["device"]
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", device_id) or device_id in device_defs:
            raise LayoutError("invalid or duplicate device id in {}: {}".format(device_path, device_id))
        geometry_id = row["geometry"]
        if geometry_id not in geometries:
            raise LayoutError("unknown geometry {} for device {}".format(geometry_id, device_id))
        geometry = geometries[geometry_id]
        if geometry_id not in expected["approved_geometries"]:
            raise LayoutError("geometry {} is not approved for this layout".format(geometry_id))
        access = "mapped" if geometry.get("cpu_base") is not None else "indirect"
        status = geometry.get("default_backend_status")
        if status not in geometry.get("allowed_backend_status", []):
            raise LayoutError("geometry {} has no approved default backend status".format(geometry_id))
        device_defs[device_id] = {"geometry": geometry_id, "access": access, "backend_status": status}
        if access == "indirect":
            pins = [row[key] for key in ("d0", "d1", "d2", "d3") if row[key]]
            if not row["controller"] or not row["cs"] or not row["clk"] or len(pins) != 4:
                raise LayoutError("indirect device {} needs controller, cs, clk, and d0-d3".format(device_id))
            bindings[device_id] = {"controller": row["controller"], "chip_select": row["cs"],
                                   "clock": row["clk"], "data": pins}
        elif any(row[key] for key in ("controller", "cs", "clk", "d0", "d1", "d2", "d3")):
            raise LayoutError("mapped device {} must have empty hardware binding columns".format(device_id))
    if not device_defs:
        raise LayoutError("device CSV must declare at least one device")

    groups = {}
    for row in partition_rows:
        name, device_id = row["name"], row["device"]
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", name):
            raise LayoutError("partition name must be a C identifier: {}".format(name))
        if device_id not in device_defs:
            raise LayoutError("partition {} references unknown device {}".format(name, device_id))
        groups.setdefault(name, []).append({
            "device": device_id,
            "device_offset_bytes": _csv_number(row["offset"], name + ".offset"),
            "size_bytes": _csv_number(row["size"], name + ".size"),
            "logical_base": _csv_number(row["logical_addr"], name + ".logical_addr"),
        })
    if not groups:
        raise LayoutError("partition CSV must declare at least one partition")

    partition_defs = {}
    for name, segments in groups.items():
        segments.sort(key=lambda item: item["logical_base"])
        logical_base = segments[0]["logical_base"]
        cursor = logical_base
        normalized_segments = []
        erase_values = []
        program_values = []
        for segment in segments:
            if segment["size_bytes"] <= 0:
                raise LayoutError("partition {} size must be positive".format(name))
            if segment["logical_base"] != cursor:
                raise LayoutError("partition {} has a logical gap or overlap".format(name))
            geometry = geometries[device_defs[segment["device"]]["geometry"]]
            erase = _integer(geometry.get("erase_bytes"), name + ".erase_bytes")
            erase_values.append(erase)
            program_values.append(_integer(geometry.get("program_bytes"), name + ".program_bytes", True))
            normalized_segments.append({
                "logical_offset_bytes": cursor - logical_base,
                "device": segment["device"],
                "device_offset_bytes": segment["device_offset_bytes"],
                "size_bytes": segment["size_bytes"],
            })
            cursor += segment["size_bytes"]
        erase_bytes = _lcm(erase_values)
        program_bytes = _lcm([unit for unit in program_values if unit is not None]) if all(
            unit is not None for unit in program_values) else None
        partition_defs[name] = {
            "role": _STANDARD_ROLE.get(name, "data"),
            "logical_base": logical_base,
            "size_bytes": cursor - logical_base,
            "erase_bytes": erase_bytes,
            "program_bytes": program_bytes,
            "segments": normalized_segments,
        }

    layout_id = "gd32h77x_{}_{}".format(source_kind, expected["target_chip"].lower())
    role_names = {part["role"]: name for name, part in partition_defs.items()}
    ota_policy = {
        "status": "planned_not_implemented",
        "primary": "primary",
        "staging": "staging",
        "metadata": "metadata",
        "kv": role_names["kv"],
        "install_strategy": ("copy_with_backup" if "ota_backup" in role_names else
                             "sector_swap" if "ota_scratch" in role_names else "copy_in_place"),
        "rollback": ("requires_boot_trial_and_confirm" if "ota_backup" in role_names else
                     "not_guaranteed"),
    }
    for optional_role, policy_key in (("ota_scratch", "scratch"), ("ota_backup", "backup")):
        if optional_role in role_names:
            ota_policy[policy_key] = role_names[optional_role]
    linker_memory = [
        {"name": "BOOT", "partition": "boot", "attributes": "rx"},
        {"name": "CNVM", "partition": "primary", "attributes": "rx"},
        {"name": "AXISRAM", "geometry": expected["target_chip"] + "_AXISRAM",
         "attributes": "xrw"},
    ]
    if "ecnvm_code" in partition_defs:
        linker_memory.append({"name": "ECNVM", "partition": "ecnvm_code", "attributes": "rx"})
    layout = {
        "schema_version": 1, "layout_id": layout_id,
        "platform": _PLATFORM_NAME, "platform_chip": expected["platform_chip"],
        "target_chip": expected["target_chip"], "devices": device_defs,
        "partitions": partition_defs,
        "linker_memory": linker_memory,
        "ota_policy": ota_policy,
        "board_binding": bindings,
    }
    inputs = [
        {"kind": "partitions", "sourceKind": source_kind,
         "sourcePath": partition_path.as_posix(), "digest": _sha256(partition_raw)},
        {"kind": "flash_devices", "sourceKind": device_source_kind,
         "sourcePath": device_path.as_posix(), "digest": _sha256(device_raw)},
    ]
    return layout, inputs


def _lcm(values: List[int]) -> int:
    result = 1
    for value in values:
        result = result * value // math.gcd(result, value)
    return result


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _inside_interval(start: int, size: int, limit: int) -> bool:
    return size > 0 and start <= limit and size <= limit - start


def _load_build_params(build_params: dict) -> dict:
    required = ("OPEN_ROOT", "PLATFORM_CHIP", "PLATFORM_BOARD",
                "CONFIG_PLATFORM_CHOICE", "CONFIG_BOARD_CHOICE", "CONFIG_CHIP_CHOICE")
    missing = [key for key in required if key not in build_params]
    if missing:
        raise LayoutError("build_param.json missing: {}".format(", ".join(missing)))
    if build_params["CONFIG_PLATFORM_CHOICE"] != _PLATFORM_NAME:
        raise LayoutError("build is not configured for {}".format(_PLATFORM_NAME))
    if build_params["PLATFORM_BOARD"] != build_params["CONFIG_BOARD_CHOICE"]:
        raise LayoutError("PLATFORM_BOARD and CONFIG_BOARD_CHOICE do not match")
    if build_params["PLATFORM_CHIP"] != build_params["CONFIG_CHIP_CHOICE"]:
        raise LayoutError("PLATFORM_CHIP and CONFIG_CHIP_CHOICE do not match")
    open_root = Path(str(build_params["OPEN_ROOT"])).resolve()
    if not open_root.is_dir():
        raise LayoutError("OPEN_ROOT is not a directory: {}".format(open_root))
    return {
        "open_root": open_root,
        "platform": build_params["CONFIG_PLATFORM_CHOICE"],
        "board": build_params["PLATFORM_BOARD"],
        "platform_chip": build_params["PLATFORM_CHIP"],
        "config_chip": build_params["CONFIG_CHIP_CHOICE"],
    }


def resolve_chip_identity(chip_name: str, platform_root: Path) -> dict:
    """Return the exact DFP device identity and verified memory facts for a selector."""
    platform_root = Path(platform_root).resolve()
    chips_root = platform_root / "gd32h77x_os" / "chips"
    index, _ = _load_json(chips_root / "platform_chips.json")
    if set(index) != {"schema_version", "platform_chips"} or index.get("schema_version") != 1:
        raise LayoutError("invalid platform_chips.json schema")
    entries = index.get("platform_chips")
    if not isinstance(entries, dict) or chip_name not in entries:
        raise LayoutError("no device registered for chip {}".format(chip_name))
    selected = entries[chip_name]
    if not isinstance(selected, dict):
        raise LayoutError("device registry entry {} must be an object".format(chip_name))
    _reject_unknown(selected, {"target_chip", "order_code", "alias_of", "allowed_boards", "approved_geometries"},
                    "platform chip {}".format(chip_name))
    target_chip = _required_string(selected, "target_chip", "platform chip")
    canonical_chip = selected.get("alias_of", chip_name)
    if not isinstance(canonical_chip, str) or not canonical_chip:
        raise LayoutError("platform chip {} has invalid alias_of".format(chip_name))
    if canonical_chip not in entries or not isinstance(entries[canonical_chip], dict):
        raise LayoutError("platform chip {} aliases an unknown canonical device".format(chip_name))
    canonical = entries[canonical_chip]
    if canonical.get("target_chip") != canonical_chip or canonical.get("alias_of") is not None:
        raise LayoutError("canonical device {} is not self-canonical".format(canonical_chip))
    if target_chip != canonical_chip:
        raise LayoutError("platform chip {} target does not match its canonical identity".format(chip_name))
    approved = _required_string_list(selected, "approved_geometries", "platform chip")
    if approved != canonical.get("approved_geometries"):
        raise LayoutError("alias {} geometry allowlist differs from {}".format(chip_name, canonical_chip))
    order_code = selected.get("order_code")
    if order_code is not None and (not isinstance(order_code, str) or not order_code.strip()):
        raise LayoutError("platform chip {} has invalid order_code".format(chip_name))
    allowed_boards = selected.get("allowed_boards")
    if allowed_boards is not None:
        if (not isinstance(allowed_boards, list) or not allowed_boards
                or any(not isinstance(board, str) or not board.strip() for board in allowed_boards)
                or len(set(allowed_boards)) != len(allowed_boards)):
            raise LayoutError("platform chip {} has invalid allowed_boards".format(chip_name))
    geometries_data, _ = _load_json(chips_root / "geometries.json")
    geometries = geometries_data.get("geometries") if geometries_data.get("schema_version") == 1 else None
    if not isinstance(geometries, dict):
        raise LayoutError("invalid geometries.json schema")
    for geometry_id in approved:
        if geometry_id not in geometries:
            raise LayoutError("approved geometry {} is not registered".format(geometry_id))
    cnvm_geometry = canonical_chip + "_CNVM"
    ram_geometry = canonical_chip + "_AXISRAM"
    cnvm = geometries.get(cnvm_geometry)
    ram = geometries.get(ram_geometry)
    if cnvm_geometry not in approved or ram_geometry not in approved or not isinstance(cnvm, dict) or not isinstance(ram, dict):
        raise LayoutError("device {} is missing approved CNVM/AXISRAM geometry".format(canonical_chip))
    return {
        "requested_chip": chip_name,
        "canonical_chip": canonical_chip,
        "target_chip": target_chip,
        "order_code": order_code,
        "approved_geometries": approved,
        "cnvm_geometry": cnvm_geometry,
        "cnvm_capacity_bytes": _integer(cnvm.get("capacity_bytes"), cnvm_geometry + ".capacity_bytes"),
        "cnvm_erase_bytes": _integer(cnvm.get("erase_bytes"), cnvm_geometry + ".erase_bytes"),
        "cnvm_program_bytes": _integer(cnvm.get("program_bytes"), cnvm_geometry + ".program_bytes"),
        "cnvm_vendor_algorithm": _required_string(cnvm, "vendor_algorithm", cnvm_geometry),
        "ram_geometry": ram_geometry,
        "ram_capacity_bytes": _integer(ram.get("capacity_bytes"), ram_geometry + ".capacity_bytes"),
        "alias_of": canonical_chip if chip_name != canonical_chip else None,
        "allowed_boards": allowed_boards,
    }


def _normalize_layout(layout: dict, geometries: dict, expected: dict) -> dict:
    _reject_unknown(layout, _LAYOUT_FIELDS, "layout")
    if layout.get("schema_version") != 1:
        raise LayoutError("unsupported layout schema_version")
    for field, expected_value in (("platform", _PLATFORM_NAME),
                                  ("platform_chip", expected["platform_chip"]),
                                  ("target_chip", expected["target_chip"])):
        if layout.get(field) != expected_value:
            raise LayoutError("layout {} mismatch: expected {}".format(field, expected_value))
    layout_id = _required_string(layout, "layout_id", "layout")
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", layout_id):
        raise LayoutError("layout_id must be an identifier")

    device_defs = layout.get("devices")
    if not isinstance(device_defs, dict) or not device_defs:
        raise LayoutError("layout.devices must be a non-empty object")
    if len(device_defs) > MAX_DEVICES:
        raise LayoutError("layout has more than {} devices".format(MAX_DEVICES))

    devices: List[dict] = []
    device_index: Dict[str, int] = {}
    seen_geometry_ids = set()
    normalized_device_ids = set()
    for device_id in sorted(device_defs):
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", device_id):
            raise LayoutError("device id must be a C identifier: {}".format(device_id))
        normalized_device_id = _c_identifier(device_id)
        if normalized_device_id in normalized_device_ids:
            raise LayoutError("device ids collide after C identifier normalization")
        normalized_device_ids.add(normalized_device_id)
        definition = device_defs[device_id]
        if not isinstance(definition, dict):
            raise LayoutError("device {} must be an object".format(device_id))
        _reject_unknown(definition, _DEVICE_FIELDS, "device {}".format(device_id))
        geometry_id = _required_string(definition, "geometry", "device {}".format(device_id))
        if geometry_id not in geometries:
            raise LayoutError("unknown physical geometry {}".format(geometry_id))
        if geometry_id in seen_geometry_ids:
            raise LayoutError("physical geometry {} is declared more than once".format(geometry_id))
        seen_geometry_ids.add(geometry_id)
        geometry = geometries[geometry_id]
        _reject_unknown(geometry, _GEOMETRY_FIELDS, "geometry {}".format(geometry_id))
        if geometry_id not in expected["approved_geometries"]:
            raise LayoutError("geometry {} is not approved for this layout".format(geometry_id))
        access = _required_string(definition, "access", "device {}".format(device_id))
        backend_status = _required_string(definition, "backend_status", "device {}".format(device_id))
        if access not in _ACCESS or backend_status not in _BACKEND:
            raise LayoutError("unsupported access/backend status on device {}".format(device_id))
        if backend_status not in geometry.get("allowed_backend_status", []):
            raise LayoutError("backend status {} is not approved for geometry {}".format(backend_status, geometry_id))
        cpu_base = _integer(geometry.get("cpu_base"), "{}.cpu_base".format(geometry_id), True)
        vendor_algorithm = geometry.get("vendor_algorithm")
        if vendor_algorithm is not None and (not isinstance(vendor_algorithm, str)
                                             or not vendor_algorithm.strip()):
            raise LayoutError("{}.vendor_algorithm must be a non-empty string".format(geometry_id))
        executable = geometry.get("executable")
        if not isinstance(executable, bool):
            raise LayoutError("{}.executable must be boolean".format(geometry_id))
        if (access == "mapped") != (cpu_base is not None):
            raise LayoutError("device {} access does not match its CPU mapping".format(device_id))
        normalized = {
            "id": device_id,
            "geometry_id": geometry_id,
            "kind": geometry.get("kind"),
            "part_number": geometry["part_number"],
            "region": _required_string(geometry, "region", geometry_id),
            "capacity_bytes": _integer(geometry.get("capacity_bytes"), geometry_id + ".capacity_bytes"),
            "erase_bytes": _integer(geometry.get("erase_bytes"), geometry_id + ".erase_bytes", True),
            "program_bytes": _integer(geometry.get("program_bytes"), geometry_id + ".program_bytes", True),
            "page_bytes": _integer(geometry.get("page_bytes"), geometry_id + ".page_bytes", True),
            "cpu_base": cpu_base,
            "executable": executable,
            "access": access,
            "backend_status": backend_status,
            "vendor_algorithm": vendor_algorithm,
            "evidence": geometry.get("evidence", ""),
        }
        if normalized["kind"] != "flash" or normalized["capacity_bytes"] <= 0:
            raise LayoutError("unsupported storage geometry {}".format(geometry_id))
        if normalized["erase_bytes"] is None or normalized["erase_bytes"] <= 0:
            raise LayoutError("flash geometry {} needs erase_bytes".format(geometry_id))
        if normalized["part_number"] == expected["target_chip"] and not vendor_algorithm:
            raise LayoutError(
                "{} flash geometry {} needs the vendor flash algorithm".format(
                    expected["target_chip"], geometry_id))
        if access == "indirect" and executable:
            raise LayoutError("indirect device {} cannot be executable".format(device_id))
        device_index[device_id] = len(devices)
        devices.append(normalized)

    partition_defs = layout.get("partitions")
    if not isinstance(partition_defs, dict) or not partition_defs:
        raise LayoutError("layout.partitions must be a non-empty object")
    if len(partition_defs) > MAX_PARTITIONS:
        raise LayoutError("layout has more than {} partitions".format(MAX_PARTITIONS))

    partitions: List[dict] = []
    partition_map: Dict[str, dict] = {}
    all_segments = 0
    logical_ranges: List[Tuple[int, int, str]] = []
    physical_ranges: Dict[str, List[Tuple[int, int, str]]] = {}
    for partition_id, definition in partition_defs.items():
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", partition_id):
            raise LayoutError("partition id must be a C identifier: {}".format(partition_id))
        if not isinstance(definition, dict):
            raise LayoutError("partition {} must be an object".format(partition_id))
        _reject_unknown(definition, _PARTITION_FIELDS, "partition {}".format(partition_id))
        role = _required_string(definition, "role", "partition {}".format(partition_id))
        if role not in _ROLE_FLAGS:
            raise LayoutError("unsupported partition role {}".format(role))
        logical_base = _integer(definition.get("logical_base"), partition_id + ".logical_base")
        size_bytes = _integer(definition.get("size_bytes"), partition_id + ".size_bytes")
        erase_bytes = _integer(definition.get("erase_bytes"), partition_id + ".erase_bytes")
        program_bytes = _integer(definition.get("program_bytes"), partition_id + ".program_bytes", True)
        if not size_bytes or not erase_bytes or size_bytes % erase_bytes or logical_base % erase_bytes:
            raise LayoutError("partition {} has invalid size/erase alignment".format(partition_id))
        if program_bytes is not None:
            if program_bytes == 0 or erase_bytes % program_bytes:
                raise LayoutError("partition {} has invalid program alignment".format(partition_id))
        # END macros and the public TKL address type are 32-bit; leave the
        # exclusive end itself representable rather than emitting 0x100000000.
        if not _inside_interval(logical_base, size_bytes, 0xFFFFFFFF):
            raise LayoutError("partition {} exceeds the 32-bit logical address space".format(partition_id))
        segments_raw = definition.get("segments")
        if not isinstance(segments_raw, list) or not segments_raw:
            raise LayoutError("partition {} needs one or more segments".format(partition_id))
        if any(not isinstance(item, dict) for item in segments_raw):
            raise LayoutError("partition {} segments must be objects".format(partition_id))
        for segment in segments_raw:
            _reject_unknown(segment, _SEGMENT_FIELDS, "partition {} segment".format(partition_id))
        all_segments += len(segments_raw)
        if all_segments > MAX_SEGMENTS:
            raise LayoutError("layout has more than {} segments".format(MAX_SEGMENTS))
        segments = []
        cursor = 0
        for segment_raw in sorted(segments_raw, key=lambda item: _integer(item.get("logical_offset_bytes"), partition_id + ".logical_offset_bytes")):
            if not isinstance(segment_raw, dict):
                raise LayoutError("partition {} segment must be an object".format(partition_id))
            logical_offset = _integer(segment_raw.get("logical_offset_bytes"), partition_id + ".logical_offset_bytes")
            segment_size = _integer(segment_raw.get("size_bytes"), partition_id + ".segment.size_bytes")
            device_id = _required_string(segment_raw, "device", "partition {} segment".format(partition_id))
            device_offset = _integer(segment_raw.get("device_offset_bytes"), partition_id + ".device_offset_bytes")
            if device_id not in device_index:
                raise LayoutError("partition {} references unknown device {}".format(partition_id, device_id))
            device = devices[device_index[device_id]]
            if logical_offset != cursor or not segment_size:
                raise LayoutError("partition {} has a logical gap or overlap".format(partition_id))
            if not _inside_interval(logical_offset, segment_size, size_bytes):
                raise LayoutError("partition {} segment exceeds its logical range".format(partition_id))
            if not _inside_interval(device_offset, segment_size, device["capacity_bytes"]):
                raise LayoutError("partition {} segment exceeds device {}".format(partition_id, device_id))
            unit = device["erase_bytes"]
            if (device_offset % unit or segment_size % unit or erase_bytes % unit
                    or logical_offset % erase_bytes or segment_size % erase_bytes):
                raise LayoutError("partition {} erase geometry is incompatible with {}".format(partition_id, device_id))
            if program_bytes is not None and device["program_bytes"] is not None:
                if program_bytes < device["program_bytes"] or program_bytes % device["program_bytes"]:
                    raise LayoutError("partition {} program geometry is incompatible with {}".format(partition_id, device_id))
            if program_bytes is not None:
                device_program = device["program_bytes"]
                if ((device_program is not None and device_offset % device_program)
                        or logical_offset % program_bytes or segment_size % program_bytes):
                    raise LayoutError("partition {} segment has invalid program alignment".format(partition_id))
            begin = device_offset
            end = begin + segment_size
            physical_ranges.setdefault(device_id, []).append((begin, end, partition_id))
            segments.append({
                "logical_offset_bytes": logical_offset,
                "size_bytes": segment_size,
                "device": device_id,
                "device_index": device_index[device_id],
                "device_offset_bytes": device_offset,
            })
            cursor += segment_size
        if cursor != size_bytes:
            raise LayoutError("partition {} segments do not cover its full logical range".format(partition_id))
        logical_ranges.append((logical_base, logical_base + size_bytes, partition_id))
        partition = {
            "id": partition_id,
            "role": role,
            "logical_base": logical_base,
            "size_bytes": size_bytes,
            "erase_bytes": erase_bytes,
            "program_bytes": program_bytes,
            "flags": _ROLE_FLAGS[role],
            "segments": segments,
        }
        partition_map[partition_id] = partition
        partitions.append(partition)

    for ranges, label in ((logical_ranges, "logical address"),):
        ranges.sort()
        for left, right in zip(ranges, ranges[1:]):
            if left[1] > right[0]:
                raise LayoutError("partitions {} and {} overlap in {} space".format(left[2], right[2], label))
    for device_id, ranges in physical_ranges.items():
        ranges.sort()
        for left, right in zip(ranges, ranges[1:]):
            if left[1] > right[0]:
                raise LayoutError("partitions {} and {} overlap on device {}".format(left[2], right[2], device_id))

    role_map = {}
    for part in partitions:
        if part["role"] == "data":
            continue
        if part["role"] in role_map:
            raise LayoutError("duplicate partition role {}".format(part["role"]))
        role_map[part["role"]] = part
    normalized_ids = [_c_identifier(part["id"]) for part in partitions]
    if len(set(normalized_ids)) != len(normalized_ids):
        raise LayoutError("partition ids collide after C identifier normalization")
    generated_macros = {
        "GD32H77X_LAYOUT_DEVICE_COUNT",
        "GD32H77X_LAYOUT_PARTITION_COUNT",
        "GD32H77X_LAYOUT_SEGMENT_COUNT",
        "GD32H77X_LAYOUT_BOOT_BASE",
        "GD32H77X_LAYOUT_PRIMARY_BASE",
        "GD32H77X_LAYOUT_PRIMARY_SIZE",
        "GD32H77X_LAYOUT_PRIMARY_END",
        "GD32H77X_LAYOUT_VECTOR_BASE",
        "GD32H77X_LAYOUT_FULL_IMAGE_BASE",
        "GD32H77X_LAYOUT_FULL_IMAGE_LIMIT",
        "GD32H77X_LAYOUT_TUYA_LOGICAL_BASE",
        "GD32H77X_LAYOUT_TUYA_SIZE_BYTES",
        "GD32H77X_LAYOUT_TUYA_END",
        "GD32H77X_LAYOUT_TUYA_ERASE_BYTES",
        "GD32H77X_LAYOUT_TUYA_PROGRAM_BYTES",
    }
    for partition in partitions:
        symbol = _c_identifier(partition["id"])
        candidate_macros = {
            "GD32H77X_LAYOUT_PART_{}".format(symbol),
            "GD32H77X_{}_BASE".format(symbol),
            "GD32H77X_{}_SIZE".format(symbol),
            "GD32H77X_{}_END".format(symbol),
        }
        collisions = candidate_macros & generated_macros
        if collisions:
            raise LayoutError("partition id {} generates reserved C macro(s): {}".format(
                partition["id"], ", ".join(sorted(collisions))))
        generated_macros.update(candidate_macros)
    for required_role in ("boot", "primary", "ota_staging", "ota_metadata", "kv"):
        if required_role not in role_map:
            raise LayoutError("layout missing required partition role {}".format(required_role))
    for core_id, core_role in (("boot", "boot"), ("primary", "primary")):
        if core_id not in partition_map or partition_map[core_id]["role"] != core_role:
            raise LayoutError("partition id {} must have role {}".format(core_id, core_role))

    linker_memory_raw = layout.get("linker_memory")
    if not isinstance(linker_memory_raw, list) or not linker_memory_raw:
        raise LayoutError("layout.linker_memory must be a non-empty array")
    linker_memory = []
    names = set()
    for item in linker_memory_raw:
        if not isinstance(item, dict):
            raise LayoutError("linker_memory entries must be objects")
        _reject_unknown(item, _LINKER_FIELDS, "linker_memory entry")
        name = _required_string(item, "name", "linker_memory")
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            raise LayoutError("linker memory name must be a linker identifier")
        attributes = _required_string(item, "attributes", "linker_memory {}".format(name))
        if (not attributes or any(char not in "rwx" for char in attributes)
                or len(set(attributes)) != len(attributes)):
            raise LayoutError("invalid linker attributes for {}".format(name))
        if name in names:
            raise LayoutError("duplicate linker memory name {}".format(name))
        names.add(name)
        if ("partition" in item) == ("geometry" in item):
            raise LayoutError("linker_memory {} needs exactly one of partition/geometry".format(name))
        if "partition" in item:
            partition_id = _required_string(item, "partition", "linker_memory {}".format(name))
            if partition_id not in partition_map:
                raise LayoutError("linker_memory {} references unknown partition".format(name))
            part = partition_map[partition_id]
            if len(part["segments"]) != 1:
                raise LayoutError("linker partition {} must have one physical segment".format(partition_id))
            segment = part["segments"][0]
            device = devices[segment["device_index"]]
            if not device["executable"] or device["access"] != "mapped":
                raise LayoutError("linker partition {} is not CPU executable".format(partition_id))
            if "x" in attributes and not device["executable"]:
                raise LayoutError("non-executable device used for linker region {}".format(name))
            origin = device["cpu_base"] + segment["device_offset_bytes"]
            if part["logical_base"] != origin or segment["logical_offset_bytes"] != 0:
                raise LayoutError("executable partition {} logical/CPU addresses differ".format(partition_id))
            length = part["size_bytes"]
        else:
            geometry_id = _required_string(item, "geometry", "linker_memory {}".format(name))
            if geometry_id not in geometries or geometries[geometry_id].get("kind") != "ram":
                raise LayoutError("linker region {} references unknown RAM geometry".format(name))
            _reject_unknown(geometries[geometry_id], _GEOMETRY_FIELDS,
                            "geometry {}".format(geometry_id))
            if geometry_id not in expected["approved_geometries"]:
                raise LayoutError("geometry {} is not approved for this layout".format(geometry_id))
            if name == "AXISRAM" and geometry_id != expected["target_chip"] + "_AXISRAM":
                raise LayoutError("AXISRAM must use the {} AXISRAM geometry".format(
                    expected["target_chip"]))
            if name == "AXISRAM" and attributes != "xrw":
                raise LayoutError("AXISRAM must use linker attributes xrw")
            geometry = geometries[geometry_id]
            origin = _integer(geometry.get("cpu_base"), geometry_id + ".cpu_base")
            length = _integer(geometry.get("capacity_bytes"), geometry_id + ".capacity_bytes")
        if "x" in attributes:
            executable_source = ("partition" in item and devices[partition_map[item["partition"]]["segments"][0]["device_index"]]["executable"]) or ("geometry" in item and geometries[item["geometry"]].get("executable"))
            if not executable_source:
                raise LayoutError("linker region {} requests execute on non-executable memory".format(name))
        linker_memory.append({"name": name, "origin": origin, "length": length, "attributes": attributes})
    linker_memory.sort(key=lambda item: (item["origin"], item["name"]))
    memory_map = {region["name"]: region for region in linker_memory}
    expected_regions = {"BOOT", "CNVM", "AXISRAM"}
    if "ecnvm_code" in partition_map:
        expected_regions.add("ECNVM")
        part = partition_map["ecnvm_code"]
        if (part["role"] != "data" or len(part["segments"]) != 1
                or devices[part["segments"][0]["device_index"]]["geometry_id"] != "GD32H779VW_ECNVM"
                or part["logical_base"] != 0x08200000 or part["size_bytes"] != 0x10000):
            raise LayoutError("ecnvm_code must be the first 64 KiB of mapped GD32H779VW ECNVM")
    elif "ECNVM" in memory_map:
        raise LayoutError("ECNVM linker region requires the declared ecnvm_code partition")
    if set(memory_map) != expected_regions:
        raise LayoutError("linker_memory must define BOOT, CNVM, AXISRAM and optional ECNVM")
    if (memory_map["BOOT"]["origin"] != partition_map["boot"]["logical_base"]
            or memory_map["BOOT"]["length"] != partition_map["boot"]["size_bytes"]
            or memory_map["CNVM"]["origin"] != partition_map["primary"]["logical_base"]
            or memory_map["CNVM"]["length"] != partition_map["primary"]["size_bytes"]):
        raise LayoutError("BOOT/CNVM linker regions must map boot/primary partitions")
    for left, right in zip(linker_memory, linker_memory[1:]):
        if left["origin"] + left["length"] > right["origin"]:
            raise LayoutError("linker MEMORY regions {} and {} overlap".format(left["name"], right["name"]))

    boot = role_map["boot"]
    primary = role_map["primary"]
    if len(boot["segments"]) != 1 or len(primary["segments"]) != 1:
        raise LayoutError("boot and primary must each be one executable CNVM segment")
    boot_seg, primary_seg = boot["segments"][0], primary["segments"][0]
    if boot_seg["device"] != primary_seg["device"]:
        raise LayoutError("boot and primary must reside on the same CNVM device")
    flash_device = devices[boot_seg["device_index"]]
    if (flash_device["geometry_id"] != expected["target_chip"] + "_CNVM"
            or not flash_device["executable"] or flash_device["access"] != "mapped"):
        raise LayoutError("boot/primary device must be mapped executable CNVM")
    if boot_seg["device_offset_bytes"] + boot["size_bytes"] != primary_seg["device_offset_bytes"]:
        raise LayoutError("boot and primary must be physically contiguous for full-image programming")
    if boot_seg["device_offset_bytes"] != 0 or flash_device["cpu_base"] != 0x08000000:
        raise LayoutError("ROM boot ABI requires CNVM offset 0 at CPU address 0x08000000")
    if boot["logical_base"] != flash_device["cpu_base"] + boot_seg["device_offset_bytes"]:
        raise LayoutError("boot logical address differs from its CPU-mapped address")
    if primary["logical_base"] != flash_device["cpu_base"] + primary_seg["device_offset_bytes"]:
        raise LayoutError("primary logical address differs from its CPU-mapped address")
    if memory_map["BOOT"]["attributes"] != "rx" or memory_map["CNVM"]["attributes"] != "rx":
        raise LayoutError("BOOT and CNVM linker regions must use attributes rx")
    if "ECNVM" in memory_map:
        part = partition_map["ecnvm_code"]
        if (memory_map["ECNVM"]["attributes"] != "rx"
                or memory_map["ECNVM"]["origin"] != part["logical_base"]
                or memory_map["ECNVM"]["length"] != part["size_bytes"]):
            raise LayoutError("ECNVM linker region must map the declared ecnvm_code partition")

    ota_policy = layout.get("ota_policy")
    if not isinstance(ota_policy, dict) or ota_policy.get("status") != "planned_not_implemented":
        raise LayoutError("ota_policy must explicitly state planned_not_implemented")
    _reject_unknown(ota_policy, _OTA_FIELDS, "ota_policy")
    for field in ("install_strategy", "rollback"):
        _required_string(ota_policy, field, "ota_policy")
    for key, role in (("primary", "primary"), ("staging", "ota_staging"),
                      ("metadata", "ota_metadata"), ("kv", "kv")):
        partition_id = _required_string(ota_policy, key, "ota_policy")
        if partition_id not in partition_map or partition_map[partition_id]["role"] != role:
            raise LayoutError("ota_policy.{} must reference a {} partition".format(key, role))
    if "scratch" in ota_policy:
        scratch = ota_policy["scratch"]
        if scratch not in partition_map or partition_map[scratch]["role"] != "ota_scratch":
            raise LayoutError("ota_policy.scratch must reference an ota_scratch partition")
    if "backup" in ota_policy:
        backup = ota_policy["backup"]
        if backup not in partition_map or partition_map[backup]["role"] != "ota_backup":
            raise LayoutError("ota_policy.backup must reference an ota_backup partition")

    board_binding = layout.get("board_binding", {})
    if not isinstance(board_binding, dict):
        raise LayoutError("board_binding must be an object")
    for device_id in board_binding:
        if device_id not in device_index:
            raise LayoutError("board_binding references unknown device {}".format(device_id))
    for device in devices:
        if device["access"] == "indirect" and device["id"] not in board_binding:
            raise LayoutError("indirect device {} needs an explicit board binding".format(device["id"]))
        if device["access"] == "indirect":
            binding = board_binding[device["id"]]
            _reject_unknown(binding, {"controller", "chip_select", "clock", "data"},
                            "board_binding {}".format(device["id"]))
            required = ("controller", "chip_select", "clock", "data")
            if not isinstance(binding, dict) or any(key not in binding for key in required):
                raise LayoutError("indirect device {} has incomplete board binding".format(device["id"]))
            if any(not isinstance(binding[key], str) or not binding[key] for key in required[:3]):
                raise LayoutError("indirect device {} controller/pins must be non-empty strings".format(device["id"]))
            if (not isinstance(binding["data"], list) or not binding["data"]
                    or any(not isinstance(pin, str) or not pin for pin in binding["data"])):
                raise LayoutError("indirect device {} data pins must be a non-empty string array".format(device["id"]))

    partitions.sort(key=lambda item: (item["logical_base"], item["id"]))
    flash_start = flash_device["cpu_base"] + boot_seg["device_offset_bytes"]
    flash_end = flash_device["cpu_base"] + primary_seg["device_offset_bytes"] + primary["size_bytes"]
    return {
        "schema_version": 1,
        "layout_id": layout_id,
        "platform": _PLATFORM_NAME,
        "platform_chip": expected["platform_chip"],
        "target_chip": expected["target_chip"],
        "devices": devices,
        "partitions": {part["id"]: part for part in partitions},
        "linker_memory": linker_memory,
        "flash_image": {
            "start_address": flash_start,
            "end_address": flash_end,
            "capacity_bytes": flash_end - flash_start,
            "boot_partition": boot["id"],
            "primary_partition": primary["id"],
        },
        "ota_policy": ota_policy,
        "board_binding": board_binding,
    }


def resolve_build_layout(build_params: dict, *, platform_root: Path,
                          boards_root: Optional[Path] = None) -> dict:
    """Resolve a build_param object; safe to import from platform consumers."""
    build = _load_build_params(build_params)
    platform_root = Path(platform_root).resolve()
    chips_root = platform_root / "gd32h77x_os" / "chips"
    index, _ = _load_json(chips_root / "platform_chips.json")
    if set(index) != {"schema_version", "platform_chips"} or index.get("schema_version") != 1:
        raise LayoutError("invalid platform_chips.json schema")
    chip_identity = resolve_chip_identity(build["platform_chip"], platform_root)
    if (chip_identity["allowed_boards"] is not None
            and build["board"] not in chip_identity["allowed_boards"]):
        raise LayoutError("chip alias {} is only valid for board(s): {}".format(
            build["platform_chip"], ", ".join(chip_identity["allowed_boards"])))
    expected = {"platform_chip": build["platform_chip"],
                "target_chip": chip_identity["canonical_chip"],
                "approved_geometries": chip_identity["approved_geometries"]}
    default_root = chips_root / expected["target_chip"]
    default_partition_path = default_root / "partitions.csv"
    default_device_path = default_root / "flash_devices.csv"
    if boards_root is None:
        boards_root = build["open_root"] / "boards"
    boards_root = Path(boards_root).resolve()
    board_root = boards_root / _PLATFORM_NAME / build["board"]
    override_partition_path = board_root / "partitions.csv"
    override_present = override_partition_path.exists() or override_partition_path.is_symlink()
    if override_present:
        if not override_partition_path.is_file():
            raise LayoutError("board override exists but is not a regular file: {}".format(override_partition_path))
        partition_path = override_partition_path
        board_devices_path = board_root / "flash_devices.csv"
        if board_devices_path.exists() or board_devices_path.is_symlink():
            if not board_devices_path.is_file():
                raise LayoutError("board device table exists but is not a regular file: {}".format(board_devices_path))
            device_path = board_devices_path
        else:
            device_path = default_device_path
        source_kind = "board_override"
        device_source_kind = "board_override" if device_path == board_devices_path else "platform_default"
    else:
        partition_path = default_partition_path
        device_path = default_device_path
        source_kind = "platform_default"
        device_source_kind = "platform_default"
    geometry_data, _ = _load_json(chips_root / "geometries.json")
    if set(geometry_data) != {"schema_version", "geometries"} or geometry_data.get("schema_version") != 1:
        raise LayoutError("invalid geometries.json schema")
    geometries = geometry_data.get("geometries")
    if not isinstance(geometries, dict):
        raise LayoutError("geometries.json missing geometries object")
    for geometry_id in expected["approved_geometries"]:
        if geometry_id not in geometries:
            raise LayoutError(
                "approved geometry {} is not registered".format(geometry_id))
    layout, source_inputs = _load_csv_layout(partition_path, device_path, expected=expected,
                                             geometries=geometries, source_kind=source_kind,
                                             device_source_kind=device_source_kind)
    normalized = _normalize_layout(layout, geometries, expected)
    repo_root = platform_root.parent.parent
    for item, path in zip(source_inputs, (partition_path, device_path)):
        resolved = path.resolve()
        try:
            item["sourcePath"] = resolved.relative_to(repo_root).as_posix()
        except ValueError:
            item["sourcePath"] = resolved.as_posix()
    source_identity = [{"kind": item["kind"], "digest": item["digest"]}
                       for item in sorted(source_inputs, key=lambda entry: entry["kind"])]
    build_identity = {
        "platform_choice": build["platform"],
        "platform_board": build["board"],
        "platform_chip": build["platform_chip"],
        "config_chip": build["config_chip"],
        "canonical_target": chip_identity["canonical_chip"],
        "order_code": chip_identity["order_code"],
        "alias_of": chip_identity["alias_of"],
    }
    return {
        "schema_version": 1,
        "source": {
            "sourceKind": source_kind,
            "sourcePath": source_inputs[0]["sourcePath"],
            "sourceInputs": source_inputs,
            "sourceDigest": _sha256(_canonical_json(source_identity)),
        },
        "normalizedLayoutDigest": _sha256(_canonical_json(normalized)),
        "build": build_identity,
        "layout": normalized,
    }


def _c_identifier(value: str) -> str:
    result = re.sub(r"[^A-Za-z0-9_]", "_", value).upper()
    if not result or result[0].isdigit():
        result = "_" + result
    return result


def _u32(value: Optional[int]) -> str:
    return "0U" if value is None else "UINT32_C(0x{:08X})".format(value)


def _generate_linker(layout: dict) -> str:
    lines = ["MEMORY", "{"]
    for region in layout["linker_memory"]:
        lines.append("    {} ({}) : ORIGIN = 0x{:08X}, LENGTH = 0x{:08X}".format(
            region["name"], region["attributes"], region["origin"], region["length"]))
    lines.extend(["}", ""])
    return "\n".join(lines)


def _generate_header(layout: dict) -> str:
    devices = layout["devices"]
    partitions = list(layout["partitions"].values())
    device_ids = {item["id"]: index for index, item in enumerate(devices)}
    partition_ids = {item["id"]: index for index, item in enumerate(partitions)}
    all_segments = [segment for part in partitions for segment in part["segments"]]
    lines = [
        "/* Generated by resolve_memory_layout.py; do not edit. */",
        "#ifndef GD32H77X_MEMORY_LAYOUT_H",
        "#define GD32H77X_MEMORY_LAYOUT_H",
        "",
        "#include <stdint.h>",
        "",
        "#define GD32H77X_LAYOUT_DEVICE_COUNT {}U".format(len(devices)),
        "#define GD32H77X_LAYOUT_PARTITION_COUNT {}U".format(len(partitions)),
        "#define GD32H77X_LAYOUT_SEGMENT_COUNT {}U".format(len(all_segments)),
        "#define GD32H77X_LAYOUT_BOOT_BASE {}".format(_u32(layout["partitions"]["boot"]["logical_base"])),
        "#define GD32H77X_LAYOUT_PRIMARY_BASE {}".format(_u32(layout["partitions"]["primary"]["logical_base"])),
        "#define GD32H77X_LAYOUT_PRIMARY_SIZE {}".format(_u32(layout["partitions"]["primary"]["size_bytes"])),
        "#define GD32H77X_LAYOUT_PRIMARY_END {}".format(_u32(layout["partitions"]["primary"]["logical_base"] + layout["partitions"]["primary"]["size_bytes"])),
        "#define GD32H77X_LAYOUT_VECTOR_BASE GD32H77X_LAYOUT_PRIMARY_BASE",
        "#define GD32H77X_LAYOUT_FULL_IMAGE_BASE {}".format(_u32(layout["flash_image"]["start_address"])),
        "#define GD32H77X_LAYOUT_FULL_IMAGE_LIMIT {}".format(_u32(layout["flash_image"]["end_address"])),
        "#define GD32H77X_LAYOUT_TUYA_LOGICAL_BASE {}".format(_u32(layout["partitions"][layout["ota_policy"]["kv"]]["logical_base"])),
        "#define GD32H77X_LAYOUT_TUYA_SIZE_BYTES {}".format(_u32(layout["partitions"][layout["ota_policy"]["kv"]]["size_bytes"])),
        "#define GD32H77X_LAYOUT_TUYA_END {}".format(_u32(
            layout["partitions"][layout["ota_policy"]["kv"]]["logical_base"] + layout["partitions"][layout["ota_policy"]["kv"]]["size_bytes"])),
        "#define GD32H77X_LAYOUT_TUYA_ERASE_BYTES {}".format(_u32(layout["partitions"][layout["ota_policy"]["kv"]]["erase_bytes"])),
        "#define GD32H77X_LAYOUT_TUYA_PROGRAM_BYTES {}".format(_u32(layout["partitions"][layout["ota_policy"]["kv"]]["program_bytes"])),
    ]
    kv_partition_id = layout["ota_policy"]["kv"]
    if _c_identifier(kv_partition_id) != "TUYA":
        lines.append("#define GD32H77X_LAYOUT_PART_TUYA GD32H77X_LAYOUT_PART_{}".format(
            _c_identifier(kv_partition_id)))
    for partition in partitions:
        symbol = _c_identifier(partition["id"])
        lines.extend([
            "#define GD32H77X_LAYOUT_PART_{} {}U".format(symbol, partition_ids[partition["id"]]),
            "#define GD32H77X_{}_BASE {}".format(symbol, _u32(partition["logical_base"])),
            "#define GD32H77X_{}_SIZE {}".format(symbol, _u32(partition["size_bytes"])),
            "#define GD32H77X_{}_END {}".format(symbol, _u32(partition["logical_base"] + partition["size_bytes"])),
        ])
    lines.extend([
        "",
        "typedef enum {",
        "    GD32H77X_LAYOUT_ACCESS_MAPPED = 0,",
        "    GD32H77X_LAYOUT_ACCESS_INDIRECT = 1",
        "} gd32h77x_layout_access_t;",
        "",
        "typedef enum {",
        "    GD32H77X_LAYOUT_BACKEND_IMPLEMENTED = 0,",
        "    GD32H77X_LAYOUT_BACKEND_PARTIAL = 1,",
        "    GD32H77X_LAYOUT_BACKEND_NOT_IMPLEMENTED = 2",
        "} gd32h77x_layout_backend_status_t;",
        "",
        "typedef struct {",
        "    const char *id;",
        "    const char *geometry_id;",
        "    const char *region;",
        "    uint32_t capacity_bytes;",
        "    uint32_t erase_bytes;",
        "    uint32_t program_bytes; /* zero means unknown */",
        "    uint32_t page_bytes; /* zero means not applicable */",
        "    uint32_t cpu_base; /* zero means not CPU mapped */",
        "    uint32_t access;",
        "    uint32_t backend_status;",
        "    uint32_t flags; /* bit 0: executable */",
        "} gd32h77x_layout_device_t;",
        "",
        "typedef struct {",
        "    uint32_t logical_offset_bytes;",
        "    uint32_t size_bytes;",
        "    uint32_t device_offset_bytes;",
        "    uint16_t device_index;",
        "    uint16_t reserved;",
        "} gd32h77x_layout_segment_t;",
        "",
        "typedef struct {",
        "    const char *id;",
        "    uint32_t logical_base;",
        "    uint32_t size_bytes;",
        "    uint32_t erase_bytes;",
        "    uint32_t program_bytes; /* zero means unspecified */",
        "    uint32_t first_segment;",
        "    uint32_t segment_count;",
        "    uint32_t flags; /* partition role bit */",
        "} gd32h77x_layout_partition_t;",
        "",
        "typedef struct {",
        "    const char *controller;",
        "    const char *chip_select;",
        "    const char *clock;",
        "    const char *data[4];",
        "} gd32h77x_layout_binding_t;",
        "",
        "extern const gd32h77x_layout_device_t gd32h77x_layout_devices[GD32H77X_LAYOUT_DEVICE_COUNT];",
        "extern const gd32h77x_layout_binding_t gd32h77x_layout_bindings[GD32H77X_LAYOUT_DEVICE_COUNT];",
        "extern const gd32h77x_layout_segment_t gd32h77x_layout_segments[GD32H77X_LAYOUT_SEGMENT_COUNT];",
        "extern const gd32h77x_layout_partition_t gd32h77x_layout_partitions[GD32H77X_LAYOUT_PARTITION_COUNT];",
        "",
        "#endif",
        "",
    ])
    return "\n".join(lines)


def _c_string(value: str) -> str:
    return json.dumps(value)


def _generate_table(layout: dict) -> str:
    devices = layout["devices"]
    partitions = list(layout["partitions"].values())
    device_ids = {item["id"]: index for index, item in enumerate(devices)}
    lines = [
        "/* Generated by resolve_memory_layout.py; do not edit. */",
        "#include <stddef.h>",
        '#include "gd32h77x_memory_layout.h"',
        "",
        "const gd32h77x_layout_device_t gd32h77x_layout_devices[GD32H77X_LAYOUT_DEVICE_COUNT] = {",
    ]
    for device in devices:
        lines.append("    {{{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}}},".format(
            _c_string(device["id"]), _c_string(device["geometry_id"]),
            _c_string(device["region"]),
            _u32(device["capacity_bytes"]), _u32(device["erase_bytes"]),
            _u32(device["program_bytes"]), _u32(device["page_bytes"]),
            _u32(device["cpu_base"]), "GD32H77X_LAYOUT_ACCESS_{}".format(device["access"].upper()),
            "GD32H77X_LAYOUT_BACKEND_{}".format(device["backend_status"].upper()),
            "{}U".format(1 if device["executable"] else 0)))
    lines.extend(["};", "", "const gd32h77x_layout_binding_t gd32h77x_layout_bindings[GD32H77X_LAYOUT_DEVICE_COUNT] = {"])
    bindings = layout.get("board_binding", {})
    for device in devices:
        binding = bindings.get(device["id"], {})
        data = list(binding.get("data", []))
        data.extend([None] * (4 - len(data)))
        values = [binding.get("controller"), binding.get("chip_select"), binding.get("clock")]
        values.extend(data[:4])
        lines.append("    {{{}, {}, {}, {{{}}}}},".format(
            *["NULL" if value is None else _c_string(value) for value in values[:3]],
            ", ".join("NULL" if value is None else _c_string(value) for value in values[3:])))
    lines.extend(["};", "", "const gd32h77x_layout_segment_t gd32h77x_layout_segments[GD32H77X_LAYOUT_SEGMENT_COUNT] = {"])
    for part in partitions:
        for segment in part["segments"]:
            lines.append("    {{{}, {}, {}, {}U, 0U}},".format(
                _u32(segment["logical_offset_bytes"]), _u32(segment["size_bytes"]),
                _u32(segment["device_offset_bytes"]), device_ids[segment["device"]]))
    lines.extend(["};", "", "const gd32h77x_layout_partition_t gd32h77x_layout_partitions[GD32H77X_LAYOUT_PARTITION_COUNT] = {"])
    segment_cursor = 0
    for part in partitions:
        lines.append("    {{{}, {}, {}, {}, {}, {}U, {}U, {}U}},".format(
            _c_string(part["id"]), _u32(part["logical_base"]), _u32(part["size_bytes"]),
            _u32(part["erase_bytes"]), _u32(part["program_bytes"]), segment_cursor,
            len(part["segments"]), part["flags"]))
        segment_cursor += len(part["segments"])
    lines.extend(["};", ""])
    return "\n".join(lines)


def _write_if_changed(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_bytes() == content:
        return
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(content)
    temporary.replace(path)


def write_outputs(manifest: dict, out_dir: Path) -> None:
    out_dir = Path(out_dir)
    layout = manifest["layout"]
    manifest_content = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")
    _write_if_changed(out_dir / "resolved_layout.json", manifest_content)
    _write_if_changed(out_dir / "gd32h77x_memory.ldinc", _generate_linker(layout).encode("utf-8"))
    _write_if_changed(out_dir / "include" / "gd32h77x_memory_layout.h", _generate_header(layout).encode("utf-8"))
    _write_if_changed(out_dir / "gd32h77x_memory_layout.c", _generate_table(layout).encode("utf-8"))


def _read_build_params(path: Path) -> dict:
    path = Path(path)
    if path.is_dir():
        path = path / "build_param.json"
    data, _ = _load_json(path)
    return data


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-params", required=True, type=Path,
                        help="build_param.json path or parameter directory")
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--platform-root", type=Path,
                        default=Path(__file__).resolve().parents[3])
    parser.add_argument("--boards-root", type=Path,
                        help="defaults to OPEN_ROOT/boards")
    args = parser.parse_args(argv)
    try:
        build_params = _read_build_params(args.build_params)
        manifest = resolve_build_layout(build_params, platform_root=args.platform_root,
                                        boards_root=args.boards_root)
        write_outputs(manifest, args.out_dir)
    except (LayoutError, OSError) as exc:
        parser.error(str(exc))
    print(json.dumps({
        "sourceKind": manifest["source"]["sourceKind"],
        "sourcePath": manifest["source"]["sourcePath"],
        "normalizedLayoutDigest": manifest["normalizedLayoutDigest"],
        "outDir": str(args.out_dir),
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
