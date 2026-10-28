#!/usr/bin/env python3
"""Create and validate build-bound GD32H77X multi-region flash packages.

The package is a ZIP container with a ``.tufw`` suffix. Partition addresses are
never accepted from the package request CSV; they are derived from the resolved
layout embedded in the build report and checked against the current layout.
This module validates artifacts and describes explicit payload writes. It does
not perform flash I/O.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import struct
import tempfile
from typing import Any, Dict, Iterable, Optional, Tuple
import zipfile


_FORMAT = "tuyaopen-gd32h77x-flash-package"
_SCHEMA = 1
_MAX_ARCHIVE_BYTES = 128 * 1024 * 1024
_MAX_MEMBER_BYTES = 64 * 1024 * 1024
_FIXED_MEMBERS = {
    "artifacts/report.json", "artifacts/firmware.elf", "artifacts/internal.bin",
}
_PROTECTED_EXPLICIT_ROLES = {"kv", "ota_metadata", "ota_staging", "ota_scratch", "ota_backup"}


class PackageError(ValueError):
    """Invalid or unsupported flash package."""


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _unique_object(pairs: Iterable[Tuple[str, Any]]) -> Dict[str, Any]:
    result = {}
    for key, value in pairs:
        if key in result:
            raise PackageError("duplicate JSON key: {}".format(key))
        result[key] = value
    return result


def _read_json(path: Path) -> Tuple[dict, bytes]:
    raw = Path(path).read_bytes()
    try:
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=_unique_object)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise PackageError("invalid JSON {}: {}".format(path, exc)) from exc
    if not isinstance(value, dict):
        raise PackageError("JSON root must be an object: {}".format(path))
    return value, raw


def _layout_value(resolved_layout):
    if isinstance(resolved_layout, dict):
        return resolved_layout
    value, _ = _read_json(Path(resolved_layout))
    return value


def _report_value(report_path: Path):
    report, raw = _read_json(report_path)
    return report, raw


def _load_verifier():
    path = Path(__file__).resolve().with_name("verify_image.py")
    spec = importlib.util.spec_from_file_location("gd32h77x_flash_package_verifier", path)
    if spec is None or spec.loader is None:
        raise PackageError("GD32H77X image verifier is unavailable")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _verify_artifacts(report: dict, elf: bytes, internal_bin: bytes,
                      resolved: dict, ecnvm_bin: Optional[bytes] = None) -> None:
    required = ("board", "chip", "sourceKind", "sourceDigest",
                "normalizedLayoutDigest", "elfFile", "elfSha256", "binFile",
                "binSha256", "allowwrite", "layout")
    missing = [key for key in required if key not in report]
    if missing:
        raise PackageError("build report lacks required fields: {}".format(", ".join(missing)))
    build = resolved.get("build", {})
    source = resolved.get("source", {})
    if ((report["board"], report["chip"]) !=
            (build.get("platform_board"), build.get("platform_chip"))):
        raise PackageError("build report board/chip does not match resolved layout")
    if (report["sourceKind"] != source.get("sourceKind")
            or report["sourceDigest"] != source.get("sourceDigest")
            or report["normalizedLayoutDigest"] != resolved.get("normalizedLayoutDigest")
            or report["layout"] != resolved.get("layout")):
        raise PackageError("build report layout identity does not match resolved layout")
    if _sha256(elf) != report["elfSha256"] or _sha256(internal_bin) != report["binSha256"]:
        raise PackageError("ELF/BIN digest does not match the build report")
    if (not isinstance(report["elfFile"], str) or Path(report["elfFile"]).name != report["elfFile"]
            or not isinstance(report["binFile"], str) or Path(report["binFile"]).name != report["binFile"]):
        raise PackageError("build report artifact names must be basenames")
    mapped = report.get("mapped_payloads", [])
    if not isinstance(mapped, list) or len(mapped) > 1:
        raise PackageError("build report mapped_payloads must be an array with at most one entry")
    mapped_filename = None
    if mapped:
        entry = mapped[0]
        if not isinstance(entry, dict):
            raise PackageError("build report ECNVM payload record must be an object")
        mapped_filename = entry.get("file")
        if (not isinstance(mapped_filename, str) or not mapped_filename
                or mapped_filename in (".", "..") or Path(mapped_filename).name != mapped_filename
                or "/" in mapped_filename or "\\" in mapped_filename
                or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", mapped_filename)
                or mapped_filename in {report["elfFile"], report["binFile"],
                                       "resolved_layout.json", "report.json"}):
            raise PackageError("build report ECNVM filename must be a unique basename")
    if ecnvm_bin is not None and not mapped:
        raise PackageError("ECNVM binary has no build report entry")

    verifier = _load_verifier()
    with tempfile.TemporaryDirectory(prefix="gd32h77x-flash-package-verify-") as temp:
        root = Path(temp)
        elf_path = root / report["elfFile"]
        bin_path = root / report["binFile"]
        layout_path = root / "resolved_layout.json"
        elf_path.write_bytes(elf)
        bin_path.write_bytes(internal_bin)
        ecnvm_path = None
        if ecnvm_bin is not None:
            if (len(mapped) != 1 or mapped[0].get("partition") != "ecnvm_code"
                    or mapped[0].get("region") != "ECNVM"):
                raise PackageError("report must declare exactly the approved ecnvm_code payload")
            ecnvm_path = root / mapped_filename
            ecnvm_path.write_bytes(ecnvm_bin)
        elif report.get("mapped_payloads"):
            raise PackageError("report declares ECNVM payload but its binary is missing")
        layout_path.write_text(json.dumps(resolved), encoding="utf-8")
        try:
            actual = verifier.verify(elf_path, bin_path, layout_path,
                                     bin_name=report["binFile"], elf_name=report["elfFile"],
                                     ecnvm_bin_path=ecnvm_path,
                                     ecnvm_enabled=report.get("ecnvmCodeEnabled", False))
        except (ValueError, KeyError, IndexError, struct.error) as exc:  # noqa: F821
            raise PackageError("embedded ELF/BIN failed image verification: {}".format(exc)) from exc
    for key in ("board", "chip", "sourceKind", "sourceDigest", "normalizedLayoutDigest",
                "layout", "elfSha256", "binSha256", "allowwrite"):
        if actual.get(key) != report.get(key):
            raise PackageError("build report {} does not match independent verification".format(key))
    for key in ("ecnvmCodeEnabled", "mapped_payloads"):
        if key in report and actual.get(key) != report.get(key):
            raise PackageError("build report {} does not match independent verification".format(key))
    if ecnvm_bin is not None and not report.get("ecnvmCodeEnabled"):
        raise PackageError("ECNVM binary is not enabled by the verified build report")


def _external_requests(csv_path: Optional[Path]):
    if csv_path is None:
        return [], None
    csv_path = Path(csv_path).resolve()
    try:
        raw = csv_path.read_bytes()
        text = raw.decode("utf-8-sig")
    except (OSError, UnicodeDecodeError) as exc:
        raise PackageError("cannot read external payload CSV: {}".format(exc)) from exc
    reader = csv.reader(io.StringIO(text, newline=""), strict=True)
    rows = []
    try:
        for row in reader:
            if not row or (len(row) == 1 and not row[0].strip()) or row[0].lstrip().startswith("#"):
                continue
            if not rows:
                if [cell.strip() for cell in row] != ["partition", "file"]:
                    raise PackageError("external payload CSV header must be partition,file")
                rows.append(None)
                continue
            if len(row) != 2:
                raise PackageError("external payload CSV rows must have exactly two columns")
            partition, filename = (cell.strip() for cell in row)
            if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", partition):
                raise PackageError("invalid external payload partition: {}".format(partition))
            if not filename:
                raise PackageError("external payload file path cannot be empty")
            relative = Path(filename)
            if relative.is_absolute() or ".." in relative.parts:
                raise PackageError("external payload path must stay under the CSV directory")
            source_root = csv_path.parent.resolve()
            source = (source_root / relative).resolve(strict=True)
            try:
                source.relative_to(source_root)
            except ValueError as exc:
                raise PackageError("external payload path escapes the CSV directory") from exc
            if not source.is_file():
                raise PackageError("external payload is not a regular file: {}".format(filename))
            rows.append((partition, source, source.read_bytes()))
    except csv.Error as exc:
        raise PackageError("invalid external payload CSV: {}".format(exc)) from exc
    if not rows or rows[0] is not None:
        raise PackageError("external payload CSV is missing its header")
    requests = rows[1:]
    names = [item[0] for item in requests]
    if len(names) != len(set(names)):
        raise PackageError("external payload CSV repeats a partition")
    return requests, _sha256(raw)


def _device_map(layout: dict) -> dict:
    devices = layout.get("devices")
    if isinstance(devices, list):
        result = {item.get("id"): item for item in devices if isinstance(item, dict)}
    elif isinstance(devices, dict):
        result = devices
    else:
        raise PackageError("resolved layout has no device table")
    if len(result) != len(devices):
        raise PackageError("resolved layout has duplicate/invalid device identities")
    return result


def _payload_plan(layout: dict, partition_name: str, raw_size: int):
    partitions = layout.get("partitions", {})
    partition = partitions.get(partition_name) if isinstance(partitions, dict) else None
    if not isinstance(partition, dict):
        raise PackageError("external payload names unknown partition {}".format(partition_name))
    if partition_name in ("boot", "primary") or partition.get("role") in ("boot", "primary"):
        raise PackageError("boot/primary are owned by the verified internal image")
    if raw_size <= 0:
        raise PackageError("external payload must not be empty")
    try:
        partition_size = int(partition["size_bytes"])
        erase_bytes = int(partition["erase_bytes"])
    except (KeyError, TypeError, ValueError) as exc:
        raise PackageError("partition {} has invalid size/erase geometry".format(partition_name)) from exc
    if erase_bytes <= 0 or partition_size <= 0:
        raise PackageError("partition {} has invalid size/erase geometry".format(partition_name))
    padded_size = ((raw_size + erase_bytes - 1) // erase_bytes) * erase_bytes
    if padded_size > partition_size:
        raise PackageError("payload for {} exceeds its partition after erase padding".format(partition_name))
    devices = _device_map(layout)
    segments = partition.get("segments")
    if not isinstance(segments, list) or not segments:
        raise PackageError("partition {} has no segments".format(partition_name))
    extents = []
    cursor = 0
    for segment in sorted(segments, key=lambda item: int(item.get("logical_offset_bytes", -1))):
        try:
            logical_offset = int(segment["logical_offset_bytes"])
            segment_size = int(segment["size_bytes"])
            device_offset = int(segment["device_offset_bytes"])
            device_id = segment["device"]
            device = devices[device_id]
        except (KeyError, TypeError, ValueError) as exc:
            raise PackageError("partition {} has invalid segment geometry".format(partition_name)) from exc
        seg_start, seg_end = logical_offset, logical_offset + segment_size
        if seg_start != cursor or segment_size <= 0:
            raise PackageError("partition {} has a logical gap/overlap".format(partition_name))
        cursor = seg_end
        overlap_start = max(0, seg_start)
        overlap_end = min(padded_size, seg_end)
        if overlap_start >= overlap_end:
            continue
        if (device.get("kind") != "flash" or device.get("access") != "indirect"
                or device.get("region") == "CNVM"):
            raise PackageError("external payload {} must map only to indirect non-CNVM storage".format(
                partition_name))
        try:
            dev_erase = int(device["erase_bytes"])
            dev_capacity = int(device["capacity_bytes"])
        except (KeyError, TypeError, ValueError) as exc:
            raise PackageError("external device {} has invalid geometry".format(device_id)) from exc
        part_offset = overlap_start
        physical_offset = device_offset + overlap_start - seg_start
        length = overlap_end - overlap_start
        if (dev_erase <= 0 or physical_offset < 0 or physical_offset % dev_erase or length % dev_erase
                or physical_offset + length > dev_capacity):
            raise PackageError("payload {} does not align to device erase geometry or exceeds device".format(partition_name))
        extents.append({
            "device": device_id,
            "region": device.get("region"),
            "backend_status": device.get("backend_status"),
            "partition_offset_bytes": part_offset,
            "device_offset_bytes": physical_offset,
            "size_bytes": length,
            "erase_bytes": dev_erase,
        })
    if cursor != partition_size or not extents or sum(item["size_bytes"] for item in extents) != padded_size:
        raise PackageError("payload {} is not fully covered by indirect partition segments".format(partition_name))
    return padded_size, erase_bytes, extents, partition.get("role", "data")


def _write_zip(output_path: Path, members: dict) -> None:
    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    temp_path = None
    try:
        with tempfile.NamedTemporaryFile(prefix=".tufw-", suffix=".tmp", dir=output_path.parent,
                                         delete=False) as temp:
            temp_path = Path(temp.name)
        with zipfile.ZipFile(temp_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for name in sorted(members):
                info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = (stat.S_IFREG | 0o644) << 16
                archive.writestr(info, members[name])
        os.replace(temp_path, output_path)
    finally:
        if temp_path is not None and temp_path.exists():
            temp_path.unlink()


def create_package(output_path, report_path, elf_path, internal_bin_path,
                   resolved_layout, external_csv_path=None, ecnvm_bin_path=None) -> dict:
    """Create deterministic `.tufw`; explicit external CSV rows add payloads."""
    resolved = _layout_value(resolved_layout)
    report, report_raw = _report_value(Path(report_path))
    elf = Path(elf_path).read_bytes()
    internal_bin = Path(internal_bin_path).read_bytes()
    ecnvm_bin = Path(ecnvm_bin_path).read_bytes() if ecnvm_bin_path else None
    if report.get("elfFile") != Path(elf_path).name or report.get("binFile") != Path(internal_bin_path).name:
        raise PackageError("artifact filenames do not match the build report")
    _verify_artifacts(report, elf, internal_bin, resolved, ecnvm_bin)
    requests, _request_digest = _external_requests(Path(external_csv_path) if external_csv_path else None)
    members = {
        "artifacts/report.json": report_raw,
        "artifacts/firmware.elf": elf,
        "artifacts/internal.bin": internal_bin,
    }
    mapped_payloads = []
    if ecnvm_bin is not None:
        mapped = report.get("mapped_payloads")
        if not isinstance(mapped, list) or len(mapped) != 1:
            raise PackageError("ECNVM binary requires exactly one report mapped_payloads entry")
        entry = mapped[0]
        member = "payloads/ecnvm_code.bin"
        if entry.get("file") != Path(ecnvm_bin_path).name or entry.get("sha256") != _sha256(ecnvm_bin):
            raise PackageError("ECNVM binary name/digest differs from verified report")
        members[member] = ecnvm_bin
        mapped_payloads.append({"partition": "ecnvm_code", "role": "data", "region": "ECNVM",
            "member": member, "address": entry.get("address"), "size_bytes": len(ecnvm_bin),
            "sha256": _sha256(ecnvm_bin), "segments": entry.get("spans", [])})
    payloads = []
    physical_spans = set()
    for partition_name, _source, payload in requests:
        padded_size, erase_bytes, extents, role = _payload_plan(
            resolved["layout"], partition_name, len(payload))
        member = "payloads/{}.bin".format(partition_name)
        if member in members:
            raise PackageError("duplicate package member for partition {}".format(partition_name))
        padded = payload + b"\xFF" * (padded_size - len(payload))
        members[member] = padded
        for extent in extents:
            span = (extent["device"], extent["device_offset_bytes"],
                    extent["device_offset_bytes"] + extent["size_bytes"])
            if any(dev == span[0] and span[1] < end and start < span[2]
                   for dev, start, end in physical_spans):
                raise PackageError("external payload physical ranges overlap")
            physical_spans.add(span)
        payloads.append({
            "partition": partition_name, "role": role,
            "member": member, "filename": member,
            "size_bytes": padded_size, "sha256": _sha256(padded),
            "source_size_bytes": len(payload),
            "source_sha256": _sha256(payload), "padded_size_bytes": padded_size,
            "erase_bytes": erase_bytes, "explicit_payload": True,
            "segments": extents,
        })
    file_index = {name: {"size_bytes": len(data), "sha256": _sha256(data)}
                  for name, data in members.items()}
    source = resolved.get("source", {})
    manifest = {
        "format": _FORMAT,
        "schema_version": 2 if mapped_payloads else _SCHEMA,
        "build": {
            "board": report["board"], "chip": report["chip"],
            "sourceKind": report["sourceKind"],
            "sourceDigest": report["sourceDigest"],
            "normalizedLayoutDigest": report["normalizedLayoutDigest"],
            "canonicalTarget": resolved.get("layout", {}).get("target_chip"),
        },
        "layout": {"sourceDigest": source.get("sourceDigest"),
                   "normalizedLayoutDigest": resolved.get("normalizedLayoutDigest")},
        "artifacts": {"report": "artifacts/report.json", "elf": "artifacts/firmware.elf",
                      "internal_bin": "artifacts/internal.bin",
                      **({"ecnvm_code": "payloads/ecnvm_code.bin"} if mapped_payloads else {})},
        "external_payloads": payloads,
        **({"mapped_payloads": mapped_payloads} if mapped_payloads else {}),
        "write_policy": {
            "internal": "verified full-image boot+primary only",
            "external": "explicit CSV partitions only; no implicit partition writes",
            "protected_roles_require_explicit_payload": sorted(_PROTECTED_EXPLICIT_ROLES),
        },
        "files": file_index,
    }
    members["manifest.json"] = (json.dumps(manifest, sort_keys=True, indent=2) + "\n").encode("utf-8")
    _write_zip(Path(output_path), members)
    return manifest


def _member_name(name: str) -> bool:
    if not isinstance(name, str) or not name or "\\" in name:
        return False
    path = PurePosixPath(name)
    return (str(path) == name and not name.endswith("/") and not path.is_absolute()
            and all(part not in ("", ".", "..") for part in path.parts))


def validate_package(package_path, resolved_layout, expected_board=None,
                     expected_chip=None, extract_dir=None) -> dict:
    """Validate package contents and binding against the current resolved layout."""
    resolved = _layout_value(resolved_layout)
    try:
        archive = zipfile.ZipFile(package_path, "r")
    except (OSError, zipfile.BadZipFile) as exc:
        raise PackageError("cannot open flash package: {}".format(exc)) from exc
    with archive:
        infos = archive.infolist()
        names = [info.filename for info in infos]
        if len(names) != len(set(names)):
            raise PackageError("package contains duplicate ZIP entries")
        if any(not _member_name(name) for name in names):
            raise PackageError("package contains an unsafe member path")
        if any(info.flag_bits & 0x1 for info in infos):
            raise PackageError("encrypted ZIP members are not supported")
        if sum(info.file_size for info in infos) > _MAX_ARCHIVE_BYTES:
            raise PackageError("package exceeds maximum uncompressed size")
        if any(info.file_size > _MAX_MEMBER_BYTES for info in infos):
            raise PackageError("package member exceeds maximum size")
        if any(stat.S_ISLNK(info.external_attr >> 16) for info in infos):
            raise PackageError("symbolic links are not allowed in package")
        if "manifest.json" not in names:
            raise PackageError("package has no manifest.json")
        manifest, _ = _parse_json_bytes(archive.read("manifest.json"), "manifest.json")
        schema = manifest.get("schema_version")
        if manifest.get("format") != _FORMAT or schema not in (1, 2):
            raise PackageError("unsupported flash package format/schema")
        expected_manifest_fields = {"format", "schema_version", "build", "layout", "artifacts",
                                    "external_payloads", "write_policy", "files"}
        if schema == 2:
            expected_manifest_fields.add("mapped_payloads")
        if set(manifest) != expected_manifest_fields:
            raise PackageError("package manifest has unknown or missing fields")
        files = manifest.get("files")
        if not isinstance(files, dict) or "manifest.json" in files:
            raise PackageError("package manifest has invalid file index")
        if set(names) != set(files) | {"manifest.json"}:
            raise PackageError("package contains missing or unreferenced members")
        content = {}
        for name, entry in files.items():
            if (not _member_name(name) or not isinstance(entry, dict)
                    or set(entry) != {"size_bytes", "sha256"}):
                raise PackageError("package manifest has invalid file entry")
            if (not isinstance(entry["size_bytes"], int) or isinstance(entry["size_bytes"], bool)
                    or entry["size_bytes"] < 0
                    or not isinstance(entry["sha256"], str)
                    or not re.fullmatch(r"[0-9a-f]{64}", entry["sha256"])):
                raise PackageError("package manifest has invalid file size/digest")
            data = archive.read(name)
            if entry.get("size_bytes") != len(data) or entry.get("sha256") != _sha256(data):
                raise PackageError("package member digest/size mismatch: {}".format(name))
            content[name] = data
        if not _FIXED_MEMBERS.issubset(content):
            raise PackageError("package is missing report, ELF, or internal BIN")

    artifact_names = manifest.get("artifacts", {})
    expected_artifacts = {"report": "artifacts/report.json", "elf": "artifacts/firmware.elf",
                          "internal_bin": "artifacts/internal.bin"}
    if manifest.get("schema_version") == 2:
        expected_artifacts["ecnvm_code"] = "payloads/ecnvm_code.bin"
    if artifact_names != expected_artifacts:
        raise PackageError("package artifact member mapping is invalid")
    report, _ = _parse_json_bytes(content[artifact_names["report"]], "embedded report")
    mapped_data = content.get(artifact_names.get("ecnvm_code")) if manifest.get("schema_version") == 2 else None
    _verify_artifacts(report, content[artifact_names["elf"]], content[artifact_names["internal_bin"]],
                      resolved, mapped_data)
    build = manifest.get("build", {})
    expected = {
        "board": report["board"], "chip": report["chip"],
        "sourceKind": report["sourceKind"], "sourceDigest": report["sourceDigest"],
        "normalizedLayoutDigest": report["normalizedLayoutDigest"],
        "canonicalTarget": resolved.get("layout", {}).get("target_chip"),
    }
    if build != expected:
        raise PackageError("package build identity does not match its verified report")
    if expected_board is not None and report["board"] != expected_board:
        raise PackageError("package board does not match selected board")
    if expected_chip is not None and report["chip"] != expected_chip:
        raise PackageError("package chip does not match selected chip")
    if manifest.get("layout") != {
            "sourceDigest": resolved.get("source", {}).get("sourceDigest"),
            "normalizedLayoutDigest": resolved.get("normalizedLayoutDigest")}:
        raise PackageError("package layout digest does not match current layout")
    expected_policy = {
        "internal": "verified full-image boot+primary only",
        "external": "explicit CSV partitions only; no implicit partition writes",
        "protected_roles_require_explicit_payload": sorted(_PROTECTED_EXPLICIT_ROLES),
    }
    if manifest.get("write_policy") != expected_policy:
        raise PackageError("package write policy is invalid")

    payloads = manifest.get("external_payloads")
    if not isinstance(payloads, list):
        raise PackageError("package external_payloads must be an array")
    seen_partitions = set()
    physical_spans = []
    for item in payloads:
        if not isinstance(item, dict):
            raise PackageError("invalid external payload record")
        partition = item.get("partition")
        if not isinstance(partition, str) or partition in seen_partitions:
            raise PackageError("package repeats or invalidly names an external partition")
        seen_partitions.add(partition)
        member = item.get("member")
        if member != "payloads/{}.bin".format(partition) or member not in content:
            raise PackageError("external payload member does not match partition")
        data = content[member]
        raw_size = item.get("source_size_bytes")
        if not isinstance(raw_size, int) or isinstance(raw_size, bool) or raw_size <= 0 or raw_size > len(data):
            raise PackageError("invalid external payload source size")
        if _sha256(data[:raw_size]) != item.get("source_sha256"):
            raise PackageError("external payload source digest mismatch")
        if any(byte != 0xFF for byte in data[raw_size:]):
            raise PackageError("external payload erase padding must be 0xFF")
        padded_size, erase_bytes, extents, role = _payload_plan(resolved["layout"], partition, raw_size)
        payload_fields = {"partition", "role", "member", "filename", "size_bytes", "sha256",
                          "source_size_bytes", "source_sha256", "padded_size_bytes", "erase_bytes",
                          "explicit_payload", "segments"}
        if set(item) != payload_fields:
            raise PackageError("external payload record has unknown or missing fields")
        if (len(data) != padded_size or item.get("size_bytes") != padded_size
                or item.get("sha256") != _sha256(data)
                or item.get("filename") != member
                or item.get("padded_size_bytes") != padded_size
                or item.get("erase_bytes") != erase_bytes or item.get("role") != role
                or item.get("segments") != extents or item.get("explicit_payload") is not True
                or (role in _PROTECTED_EXPLICIT_ROLES and item.get("explicit_payload") is not True)):
            raise PackageError("external payload routing differs from current resolved partition")
        for extent in extents:
            start = extent["device_offset_bytes"]
            end = start + extent["size_bytes"]
            key = (extent["device"], start, end)
            if any(dev == key[0] and key[1] < old_end and old_start < key[2]
                   for dev, old_start, old_end in physical_spans):
                raise PackageError("external payload physical ranges overlap")
            physical_spans.append(key)

    mapped_payloads = manifest.get("mapped_payloads", [])
    if not isinstance(mapped_payloads, list):
        raise PackageError("package mapped_payloads must be an array")
    if bool(mapped_payloads) != bool(report.get("ecnvmCodeEnabled")):
        raise PackageError("package ECNVM payload presence differs from build configuration")
    if mapped_payloads:
        if len(mapped_payloads) != 1:
            raise PackageError("package must contain exactly one mapped ECNVM payload")
        item = mapped_payloads[0]
        report_item = report.get("mapped_payloads", [{}])[0]
        partition = resolved.get("layout", {}).get("partitions", {}).get("ecnvm_code", {})
        if (set(item) != {"partition", "role", "region", "member", "address", "size_bytes", "sha256", "segments"}
                or item.get("partition") != "ecnvm_code" or item.get("role") != "data"
                or item.get("region") != "ECNVM" or item.get("member") != "payloads/ecnvm_code.bin"
                or item.get("address") != partition.get("logical_base")
                or item.get("size_bytes") != report_item.get("size")
                or item.get("sha256") != report_item.get("sha256")
                or item.get("segments") != report_item.get("spans")):
            raise PackageError("mapped ECNVM payload differs from verified report/layout")

    extracted = {}
    if extract_dir is not None:
        extract_root = Path(extract_dir).resolve()
        extract_root.mkdir(parents=True, exist_ok=True)
        if any(extract_root.iterdir()):
            raise PackageError("package extraction directory must be empty")
        for name, data in content.items():
            destination = extract_root.joinpath(*PurePosixPath(name).parts)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
            extracted[name] = str(destination)
    payload_result = []
    for item in payloads:
        payload = dict(item)
        if item["member"] in extracted:
            payload["path"] = extracted[item["member"]]
        payload_result.append(payload)
    mapped_result = []
    for item in mapped_payloads:
        payload = dict(item)
        if item["member"] in extracted:
            payload["path"] = extracted[item["member"]]
        mapped_result.append(payload)
    return {
        "valid": True, "board": report["board"], "chip": report["chip"],
        "source_digest": report["sourceDigest"],
        "normalized_layout_digest": report["normalizedLayoutDigest"],
        "internal_bin_sha256": report["binSha256"],
        "internal_bin_size_bytes": len(content["artifacts/internal.bin"]),
        "internal_range": report["layout"]["flash_image"],
        "report": report, "external_payloads": payload_result,
        "mapped_payloads": mapped_result,
        "artifacts": {
            "report": extracted.get("artifacts/report.json"),
            "elf": extracted.get("artifacts/firmware.elf"),
            "internal_bin": extracted.get("artifacts/internal.bin"),
        },
    }


def _parse_json_bytes(raw: bytes, label: str):
    try:
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=_unique_object)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise PackageError("invalid {} JSON: {}".format(label, exc)) from exc
    if not isinstance(value, dict):
        raise PackageError("{} JSON root must be an object".format(label))
    return value, raw


def main(argv=None):
    import sys
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    create = sub.add_parser("create", help="create a build-bound .tufw package")
    create.add_argument("--layout", required=True, type=Path)
    create.add_argument("--report", required=True, type=Path)
    create.add_argument("--elf", required=True, type=Path)
    create.add_argument("--internal-bin", required=True, type=Path)
    create.add_argument("--external-csv", type=Path)
    create.add_argument("--ecnvm-bin", type=Path)
    create.add_argument("--output", required=True, type=Path)
    validate = sub.add_parser("validate", help="validate package against current layout")
    validate.add_argument("--package", required=True, type=Path)
    validate.add_argument("--layout", required=True, type=Path)
    validate.add_argument("--board")
    validate.add_argument("--chip")
    validate.add_argument("--extract-dir", type=Path,
                          help="extract verified artifacts/payloads under an empty directory")
    args = parser.parse_args(argv)
    try:
        if args.command == "create":
            result = create_package(args.output, args.report, args.elf,
                                    args.internal_bin, args.layout, args.external_csv,
                                    args.ecnvm_bin)
        else:
            result = validate_package(args.package, args.layout, args.board, args.chip,
                                      args.extract_dir)
    except (PackageError, OSError, zipfile.BadZipFile) as exc:
        print("flash package error: {}".format(exc), file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
