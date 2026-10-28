#!/usr/bin/env python3
"""Validate a local GigaDevice GD32H77x CMSIS-Pack before any SWD access."""

from __future__ import annotations

import hashlib
import struct
from pathlib import Path
from typing import Dict, Tuple
from xml.etree import ElementTree
from zipfile import BadZipFile, ZipFile


PACK_VENDOR = "GigaDevice"
PACK_NAME = "GD32H77x_78x_DFP"
PACK_VERSION = "0.6.1"
PACK_SHA256 = "94ed659f9adb58c881114858e9a13dba4a0540765c4d9e4d305a24e13ca5dee6"
FLASH_BASE = 0x08000000


class PackValidationError(ValueError):
    pass


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _uint(text: str, field: str) -> int:
    try:
        return int(text, 0)
    except (TypeError, ValueError) as exc:
        raise PackValidationError(f"CMSIS-Pack has invalid {field}: {text!r}") from exc


def _read_flm_geometry(data: bytes, member: str) -> Tuple[int, int, int, int]:
    """Read the CMSIS FlashDevice descriptor from the FLM's ELF symbol table.

    Returns (flash_start, flash_size, algorithm_page_size, erase_sector_size).
    The page size is reported independently from the registry's minimum program
    unit; they describe different constraints.
    """
    if len(data) < 52 or data[:7] != b"\x7fELF\x01\x01\x01":
        raise PackValidationError(f"{member} is not an ELF32 little-endian FLM")
    elf_type, machine = struct.unpack_from("<HH", data, 16)
    if elf_type != 2 or machine != 40:
        raise PackValidationError(f"{member} is not an ARM ELF executable")
    section_offset = struct.unpack_from("<I", data, 32)[0]
    section_size, section_count = struct.unpack_from("<HH", data, 46)
    if section_size != 40 or not section_count or section_offset + section_size * section_count > len(data):
        raise PackValidationError(f"{member} has an invalid ELF section table")
    sections = [struct.unpack_from("<10I", data, section_offset + i * section_size)
                for i in range(section_count)]
    flash_device = None
    for section in sections:
        if section[1] != 2:  # SHT_SYMTAB
            continue
        if section[9] != 16 or section[6] >= len(sections):
            raise PackValidationError(f"{member} has an invalid ELF symbol table")
        strings_section = sections[section[6]]
        strings = data[strings_section[4]:strings_section[4] + strings_section[5]]
        for offset in range(section[4], section[4] + section[5], 16):
            if offset + 16 > len(data):
                raise PackValidationError(f"{member} has a truncated ELF symbol table")
            name, value, size, _info, _other, section_index = struct.unpack_from("<IIIBBH", data, offset)
            if not name or section_index >= len(sections):
                continue
            end = strings.find(b"\0", name)
            if name >= len(strings) or end < 0:
                continue
            if strings[name:end] == b"FlashDevice":
                symbol_section = sections[section_index]
                file_offset = symbol_section[4] + value - symbol_section[3]
                flash_device = data[file_offset:file_offset + size]
                break
        if flash_device is not None:
            break
    if flash_device is None or len(flash_device) < 168:
        raise PackValidationError(f"{member} has no complete CMSIS FlashDevice descriptor")
    flash_start, flash_size, page_size = struct.unpack_from("<III", flash_device, 132)
    if flash_size <= 0 or page_size <= 0:
        raise PackValidationError(f"{member} has invalid FlashDevice range/page size")
    sector_offset = 160
    entries = []
    while sector_offset + 8 <= len(flash_device):
        sector_size, address = struct.unpack_from("<II", flash_device, sector_offset)
        if sector_size == 0xFFFFFFFF:
            break
        if sector_size <= 0 or address == 0xFFFFFFFF:
            raise PackValidationError(f"{member} has invalid FlashDevice sector geometry")
        entries.append((sector_size, address))
        sector_offset += 8
    if not entries:
        raise PackValidationError(f"{member} does not describe an erasable sector geometry")
    erase_size = entries[0][0]
    if entries[0][1] != 0:
        raise PackValidationError(f"{member} CNVM sectors do not start at offset zero")
    for index, (size, address) in enumerate(entries):
        end = entries[index + 1][1] if index + 1 < len(entries) else flash_size
        if address >= end or end > flash_size or (end - address) % size:
            raise PackValidationError(f"{member} has discontinuous CNVM sector geometry")
        if size != erase_size:
            raise PackValidationError(f"{member} has nonuniform CNVM erase sectors")
    return flash_start, flash_size, page_size, erase_size


def validate_pack(path: str | Path, target: str, geometry: dict) -> dict:
    """Verify pack identity, selected target, CNVM algorithm, and FLM geometry."""
    pack_path = Path(path).expanduser().resolve()
    if not pack_path.is_file():
        raise PackValidationError(f"CMSIS-Pack file not found: {pack_path}")
    actual_hash = _sha256(pack_path)
    if actual_hash != PACK_SHA256:
        raise PackValidationError(
            f"CMSIS-Pack SHA256 mismatch: expected approved {PACK_SHA256}, got {actual_hash}")
    try:
        archive = ZipFile(pack_path)
    except (OSError, BadZipFile) as exc:
        raise PackValidationError(f"Cannot open CMSIS-Pack {pack_path}: {exc}") from exc
    with archive:
        pdsc_names = [name for name in archive.namelist() if name.endswith(".pdsc")]
        if len(pdsc_names) != 1:
            raise PackValidationError("CMSIS-Pack must contain exactly one PDSC")
        try:
            root = ElementTree.fromstring(archive.read(pdsc_names[0]))
        except (ElementTree.ParseError, KeyError) as exc:
            raise PackValidationError(f"Cannot parse CMSIS-Pack PDSC: {exc}") from exc
        releases = root.find("releases")
        versions = {item.get("version") for item in (list(releases) if releases is not None else [])
                    if item.tag == "release"}
        vendor = root.findtext("vendor", "").strip()
        name = root.findtext("name", "").strip()
        if (vendor, name) != (PACK_VENDOR, PACK_NAME) or PACK_VERSION not in versions:
            raise PackValidationError("CMSIS-Pack is not the approved GigaDevice H77x DFP 0.6.1")
        matches = [entry for entry in root.iter("device") if entry.get("Dname") == target]
        if len(matches) != 1:
            raise PackValidationError(
                f"Pack must contain exactly one canonical target {target}; found {len(matches)}")
        target_node = matches[0]
        region = str(geometry.get("region", "CNVM")).upper()
        memory_id = "IROM2" if region == "ECNVM" else "IROM1"
        memory = [entry for entry in target_node.findall("memory")
                  if entry.get("id") == memory_id]
        if len(memory) != 1:
            raise PackValidationError(f"Pack target {target} must declare one {memory_id} {region} region")
        flash_start = _uint(memory[0].get("start"), "IROM1 start")
        flash_size = _uint(memory[0].get("size"), "IROM1 size")
        algorithm_name = geometry.get("vendor_algorithm")
        algorithms = [entry for entry in target_node.findall("algorithm")
                      if Path(entry.get("name", "")).name == algorithm_name]
        if len(algorithms) != 1:
            raise PackValidationError(
                f"Pack target {target} does not uniquely declare {algorithm_name}")
        algorithm = algorithms[0]
        algorithm_start = _uint(algorithm.get("start"), "algorithm start")
        algorithm_size = _uint(algorithm.get("size"), "algorithm size")
        flm_member = algorithm.get("name", "")
        try:
            flm_data = archive.read(flm_member)
        except KeyError as exc:
            raise PackValidationError(f"Pack is missing CNVM Flash algorithm {flm_member}") from exc
    expected_capacity = int(geometry["capacity_bytes"])
    expected_erase = int(geometry["erase_bytes"])
    expected_start = int(geometry.get("cpu_base", FLASH_BASE))
    if flash_start != expected_start or flash_size != expected_capacity:
        raise PackValidationError(f"Pack {region} region differs from the selected registry geometry")
    if algorithm_start != expected_start or algorithm_size != expected_capacity:
        raise PackValidationError(f"{region} FLM address range differs from the selected registry geometry")
    flm_start, flm_size, page_size, erase_size = _read_flm_geometry(flm_data, flm_member)
    if (flm_start, flm_size) != (expected_start, expected_capacity):
        raise PackValidationError(f"{region} FLM FlashDevice range differs from the selected registry geometry")
    if erase_size != expected_erase:
        raise PackValidationError(
            f"{region} FLM erase sector is {erase_size} bytes; registry requires {expected_erase}")
    program_unit = int(geometry["program_bytes"])
    if program_unit <= 0 or page_size % program_unit:
        raise PackValidationError(
            "CNVM FLM page size is incompatible with the registry minimum program unit")
    return {
        "pack": str(pack_path),
        "pack_sha256": actual_hash,
        "pack_version": PACK_VERSION,
        "target": target,
        "algorithm": Path(flm_member).name,
        "flash_start": flm_start,
        "flash_size": flm_size,
        "page_size": page_size,
        "erase_size": erase_size,
        "program_unit": program_unit,
    }
