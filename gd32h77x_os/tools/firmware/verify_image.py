#!/usr/bin/env python3
"""Verify the actual ELF and full BIN against the selected flash layout."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

def _number(value):
    return int(value, 0) if isinstance(value, str) else int(value)


def _partition(layout, name):
    try:
        return layout["layout"]["partitions"][name]
    except (KeyError, TypeError) as error:
        raise ValueError(f"Resolved layout has no {name} partition") from error


def _segment_number(segment, stem):
    for key in (f"{stem}_bytes", stem):
        if key in segment:
            return _number(segment[key])
    raise ValueError(f"Resolved layout segment has no {stem} value")


def _cpu_partition_base(resolved, name):
    layout = resolved.get("layout", {})
    partition = _partition(resolved, name)
    if len(partition.get("segments", [])) != 1:
        raise ValueError(f"{name} must have one directly mapped CPU segment")
    segment = partition["segments"][0]
    devices = {device["id"]: device for device in layout.get("devices", [])}
    try:
        device = devices[segment["device"]]
    except KeyError as error:
        raise ValueError(f"{name} references an unknown device") from error
    if device.get("access") != "mapped" or not device.get("executable") or device.get("cpu_base") is None:
        raise ValueError(f"{name} is not in executable CPU mapped memory")
    cpu_base = _number(device["cpu_base"]) + _segment_number(segment, "device_offset")
    logical_base = _number(partition["logical_base"]) + _segment_number(segment, "logical_offset")
    if cpu_base != logical_base:
        raise ValueError(f"{name} logical address differs from its CPU mapping")
    return cpu_base


def verify(elf_path, bin_path, resolved_layout_path, bin_name=None, elf_name=None,
           ecnvm_bin_path=None, ecnvm_enabled=False):
    resolved = json.loads(Path(resolved_layout_path).read_text())
    primary_partition = _partition(resolved, "primary")
    tuya_partition_id = resolved["layout"]["ota_policy"]["kv"]
    tuya_partition = _partition(resolved, tuya_partition_id)
    boot_base = _cpu_partition_base(resolved, "boot")
    vector_base = _cpu_partition_base(resolved, "primary")
    app_limit = vector_base + _number(primary_partition["size_bytes"])
    ecnvm_regions = [item for item in resolved["layout"].get("linker_memory", [])
                     if item.get("name") == "ECNVM"]
    if len(ecnvm_regions) > 1:
        raise ValueError("Resolved layout has multiple ECNVM regions")
    ecnvm_base = _number(ecnvm_regions[0]["origin"]) if ecnvm_regions else 0
    ecnvm_limit = ecnvm_base + _number(ecnvm_regions[0]["length"]) if ecnvm_regions else 0
    ecnvm_binary = Path(ecnvm_bin_path).read_bytes() if ecnvm_bin_path else None
    tuya_base = _number(tuya_partition["logical_base"])
    devices = {device["id"]: device for device in resolved.get("layout", {}).get("devices", [])}
    kv_segments = tuya_partition.get("segments", [])
    kv_on_ecnvm = False
    if len(kv_segments) == 1:
        kv_device = devices.get(kv_segments[0].get("device"))
        kv_on_ecnvm = bool(kv_device and kv_device.get("region") == "ECNVM")
    if ecnvm_regions:
        ecnvm_code = _partition(resolved, "ecnvm_code")
        ecnvm_code_end = (_number(ecnvm_code["logical_base"]) +
                          _number(ecnvm_code["size_bytes"]))
        if (ecnvm_base != _number(ecnvm_code["logical_base"]) or
                ecnvm_limit != ecnvm_code_end):
            raise ValueError("ECNVM linker region differs from the ecnvm_code partition")
        if ecnvm_binary is not None and len(ecnvm_binary) > ecnvm_limit - ecnvm_base:
            raise ValueError("ECNVM BIN extends beyond the ecnvm_code partition")
        if kv_on_ecnvm and (ecnvm_limit != tuya_base or
                            _number(tuya_partition["logical_base"]) +
                            _number(tuya_partition["size_bytes"]) !=
                            _number(kv_device["cpu_base"]) + _number(kv_device["capacity_bytes"])):
            raise ValueError("ECNVM code must end at the KV partition and KV must end at ECNVM")
    flash_image = resolved.get("layout", {}).get("flash_image", {})
    ram_regions = [entry for entry in resolved.get("layout", {}).get("linker_memory", [])
                   if entry.get("name") == "AXISRAM"]
    if len(ram_regions) != 1:
        raise ValueError("Resolved layout must define exactly one AXISRAM linker region")
    ram_base = _number(ram_regions[0]["origin"])
    ram_limit = ram_base + _number(ram_regions[0]["length"])
    if (_number(flash_image.get("start_address", -1)) != boot_base
            or _number(flash_image.get("end_address", -1)) != app_limit):
        raise ValueError("Resolved full-image window differs from the mapped boot/primary regions")
    if boot_base >= vector_base or app_limit <= vector_base:
        raise ValueError("Resolved boot/primary address range is invalid")
    elf = Path(elf_path).read_bytes()
    binary = Path(bin_path).read_bytes()
    if elf[:7] != b"\x7fELF\x01\x01\x01" or len(elf) < 52:
        raise ValueError("Expected ELF32 little-endian firmware")
    if struct.unpack_from("<H", elf, 18)[0] != 40:
        raise ValueError("Expected ARM ELF")
    entry = struct.unpack_from("<I", elf, 24)[0]
    section_offset = struct.unpack_from("<I", elf, 32)[0]
    section_size, section_count, name_index = struct.unpack_from("<HHH", elf, 46)
    if section_size != 40 or section_count <= name_index:
        raise ValueError("Invalid ELF section table")
    sections = [struct.unpack_from("<10I", elf, section_offset + i * section_size) for i in range(section_count)]
    names_section = sections[name_index]
    names = elf[names_section[4]:names_section[4] + names_section[5]]

    def string_at(table, offset):
        end = table.find(b"\0", offset)
        if offset >= len(table) or end < 0:
            raise ValueError("Invalid ELF string offset")
        return table[offset:end].decode()

    by_name = {string_at(names, item[0]): item for item in sections}
    symbols = {}
    for section in sections:
        if section[1] != 2:  # SHT_SYMTAB
            continue
        strings = sections[section[6]]
        table = elf[strings[4]:strings[4] + strings[5]]
        if section[9] != 16:
            raise ValueError("Invalid ELF symbol entry size")
        for offset in range(section[4], section[4] + section[5], 16):
            name, value, _, _, _, index = struct.unpack_from("<IIIBBH", elf, offset)
            if name and index:
                symbols[string_at(table, name)] = value

    boot = by_name[".boot_table"]
    vectors = by_name[".vectors"]
    if boot[3] != boot_base or boot[5] != 8 or vectors[3] != vector_base:
        raise ValueError("ELF boot/vector sections do not match the resolved layout")
    if len(binary) < vector_base - boot_base + vectors[5] or len(binary) > app_limit - boot_base:
        raise ValueError("BIN is truncated or extends beyond the primary partition")
    program_offset = struct.unpack_from("<I", elf, 28)[0]
    program_size, program_count = struct.unpack_from("<HH", elf, 42)
    if program_size != 32:
        raise ValueError("Invalid ELF program header size")
    segments = []
    for index in range(program_count):
        kind, offset, virtual, physical, size, memory_size, _, _ = struct.unpack_from(
            "<8I", elf, program_offset + index * program_size)
        if kind != 1 or not size:  # Only file-backed PT_LOAD bytes belong in a BIN.
            continue
        in_cnvm = boot_base <= physical and physical + size <= app_limit
        in_ecnvm = ecnvm_regions and ecnvm_base <= physical and physical + size <= ecnvm_limit
        if ((not in_cnvm and not in_ecnvm) or offset + size > len(elf) or size > memory_size):
            raise ValueError("Load segment outside declared CNVM/ECNVM executable regions")
        segments.append((offset, virtual, physical, size))
    # objcopy fills gaps BETWEEN allocated sections with 0xFF, even when an ELF
    # segment contains zero padding there. Compare actual section bytes, including
    # the flash load address of RAM-initialized .data, and verify every gap too.
    expected = bytearray(b"\xff" * len(binary))
    expected_ecnvm = bytearray(b"\xff" * len(ecnvm_binary)) if ecnvm_binary is not None else None
    image_end = boot_base
    ranges = []
    ecnvm_ranges = []
    for section in sections:
        _, kind, flags, virtual, offset, size, _, _, _, _ = section
        if not flags & 2 or kind == 8 or not size:  # SHF_ALLOC, excluding SHT_NOBITS
            continue
        matching = [(physical + virtual - segment_virtual, segment_offset)
                    for segment_offset, segment_virtual, physical, segment_size in segments
                    if segment_offset <= offset and offset + size <= segment_offset + segment_size
                    and virtual - segment_virtual == offset - segment_offset]
        if len(matching) != 1:
            raise ValueError("Allocated section does not map to one load segment")
        address = matching[0][0]
        if ecnvm_regions and ecnvm_base <= address and address + size <= ecnvm_limit:
            if expected_ecnvm is None:
                raise ValueError("ELF has ECNVM sections but no separate ECNVM BIN was supplied")
            begin = address - ecnvm_base
            if begin + size > len(expected_ecnvm):
                raise ValueError("Allocated ECNVM section extends beyond ECNVM BIN")
            expected_ecnvm[begin:begin + size] = elf[offset:offset + size]
            ecnvm_ranges.append((address, address + size))
        else:
            begin = address - boot_base
            if begin < 0 or begin + size > len(binary):
                raise ValueError("Allocated section extends beyond CNVM BIN")
            expected[begin:begin + size] = elf[offset:offset + size]
            ranges.append((address, address + size))
            image_end = max(image_end, address + size)
    ranges.sort()
    if any(left[1] > right[0] for left, right in zip(ranges, ranges[1:])):
        raise ValueError("Allocated sections overlap in flash")
    if len(binary) != image_end - boot_base:
        raise ValueError("BIN length differs from ELF allocated sections")
    if binary != expected:
        raise ValueError("BIN section bytes or erased gaps differ from ELF")
    ecnvm_ranges.sort()
    if any(left[1] > right[0] for left, right in zip(ecnvm_ranges, ecnvm_ranges[1:])):
        raise ValueError("Allocated ECNVM sections overlap")
    if ecnvm_ranges:
        if expected_ecnvm is None or len(ecnvm_binary) != ecnvm_ranges[-1][1] - ecnvm_base:
            raise ValueError("ECNVM BIN length differs from ELF allocated sections")
        if ecnvm_binary != expected_ecnvm:
            raise ValueError("ECNVM BIN section bytes or erased gaps differ from ELF")
    elif ecnvm_binary is not None:
        raise ValueError("ECNVM BIN was supplied but ELF has no ECNVM load sections")
    if bool(ecnvm_enabled) != bool(ecnvm_ranges):
        raise ValueError("ECNVM verification mode does not match ELF ECNVM sections")
    if binary[:8] != elf[boot[4]:boot[4] + 8]:
        raise ValueError("BIN boot table differs from ELF")
    vector_data = binary[vector_base - boot_base:vector_base - boot_base + vectors[5]]
    if vector_data != elf[vectors[4]:vectors[4] + vectors[5]]:
        raise ValueError("BIN vectors differ from ELF")
    stack, reset = struct.unpack_from("<II", binary)
    if stack != ram_limit or stack != symbols["_sp"] or stack % 8:
        raise ValueError("Invalid initial stack pointer")
    if reset != entry or reset != symbols["Reset_Handler"] or not reset & 1:
        raise ValueError("Invalid Thumb reset entry")
    if binary[:8] != vector_data[:8]:
        raise ValueError("Boot and vector entries disagree")
    if any(byte != 0xFF for byte in binary[8:vector_base - boot_base]):
        raise ValueError("Boot sector gap must be erased bytes")
    for index, name in ((11, "SVC_Handler"), (14, "PendSV_Handler"), (15, "SysTick_Handler")):
        handler = struct.unpack_from("<I", vector_data, index * 4)[0]
        if handler != symbols[name] or handler == symbols["Default_Handler"] or not handler & 1:
            raise ValueError(f"FreeRTOS handler not installed: {name}")
    if not ram_base <= symbols["_sbss"] <= symbols["_ebss"] <= symbols["_heap_end"] <= symbols["_stack_bottom"] < stack:
        raise ValueError("RAM sections overlap")
    if "__rtos_heap_start" in symbols or "__rtos_heap_end" in symbols:
        heap_start = symbols.get("__rtos_heap_start", 0)
        heap_end = symbols.get("__rtos_heap_end", 0)
        if (heap_start != symbols["_heap_end"] or
                heap_end != symbols["_stack_bottom"] or
                heap_end - heap_start < 1024 or
                heap_start % 8 or heap_end % 8 or
                symbols.get("ucHeap") != heap_start):
            raise ValueError("Invalid linker-managed FreeRTOS heap bounds")
    app_entry = symbols["app_entry"] & ~1
    in_cnvm_app = vector_base <= app_entry < app_limit
    in_ecnvm_app = bool(ecnvm_regions) and ecnvm_base <= app_entry < ecnvm_limit
    if not (in_cnvm_app or in_ecnvm_app):
        raise ValueError("Application entry outside the CNVM/ECNVM executable regions")
    elf_sha = hashlib.sha256(elf).hexdigest()
    bin_sha = hashlib.sha256(binary).hexdigest()
    allowwrite = []
    for partition_name in ("boot", "primary"):
        item = _partition(resolved, partition_name)
        for segment in item["segments"]:
            logical_base = _number(item["logical_base"]) + _segment_number(segment, "logical_offset")
            allowwrite.append({"partition": partition_name, "device": segment["device"].lower(),
                               "deviceOffset": _segment_number(segment, "device_offset"),
                               "size": _segment_number(segment, "size"),
                               "logicalBase": logical_base})
    spans = [{"address": hex(start), "size": end - start} for start, end in ranges]
    source = resolved.get("source", {})
    source_inputs = resolved.get("sourceInputs", source.get("sourceInputs", source.get("source_inputs", [])))
    source_kind = resolved.get("sourceKind", source.get("sourceKind"))
    source_path = resolved.get("sourcePath", source.get("sourcePath"))
    source_digest = resolved.get("sourceDigest", source.get("sourceDigest"))
    layout_digest = resolved.get("normalizedLayoutDigest", resolved.get("layoutDigest"))
    build = resolved.get("build", {})
    chip = build.get("platform_chip", resolved.get("platform_chip",
                  resolved.get("chip", resolved.get("layout", {}).get("platform_chip"))))
    return {"board": build.get("platform_board", resolved.get("board", "GD32H779V_START")), "chip": chip,
            "sourceKind": source_kind, "sourcePath": source_path,
            "sourceDigest": source_digest, "normalizedLayoutDigest": layout_digest,
            "sourceInputs": source_inputs,
            "layout": resolved["layout"], "elfSha256": elf_sha, "binSha256": bin_sha,
            "ecnvmCodeEnabled": bool(ecnvm_enabled),
            "mapped_payloads": ([{"partition": "ecnvm_code", "region": "ECNVM",
                "address": ecnvm_base, "size": len(ecnvm_binary),
                "file": Path(ecnvm_bin_path).name,
                "sha256": hashlib.sha256(ecnvm_binary).hexdigest(),
                "spans": [{"address": hex(start), "size": end - start}
                          for start, end in ecnvm_ranges]}] if ecnvm_ranges else []),
            "elfFile": elf_name or Path(elf_path).name, "binFile": bin_name or Path(bin_path).name,
            "addresses": {"flash": hex(boot_base), "vector": hex(vector_base),
                          "primaryEnd": hex(app_limit), "resetEntry": hex(reset),
                          "initialSP": hex(stack)},
            "allowwrite": allowwrite, "spans": spans,
            "flash_address": hex(boot_base), "vector_address": hex(vector_base),
            "reset_entry": hex(reset), "initial_sp": hex(stack),
            "reserved_tuya_start": hex(tuya_base), "size": len(binary), "sha256": bin_sha,
            "verification": "static layout only; hardware execution is not verified"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--resolved-layout", required=True, type=Path)
    parser.add_argument("--bin-name")
    parser.add_argument("--elf-name")
    parser.add_argument("--ecnvm-bin", type=Path)
    parser.add_argument("--ecnvm-enabled", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        result = verify(args.elf, args.binary, args.resolved_layout, args.bin_name, args.elf_name,
                        args.ecnvm_bin, args.ecnvm_enabled)
    except (ValueError, KeyError, IndexError, struct.error) as error:
        parser.error(str(error))
    report = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(report)
    print(report, end="")


if __name__ == "__main__":
    main()
