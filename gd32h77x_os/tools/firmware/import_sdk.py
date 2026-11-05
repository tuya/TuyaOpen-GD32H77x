#!/usr/bin/env python3
"""Import the selected files directly from the pinned vendor archive (libarchive)."""

import argparse
import ctypes
import ctypes.util
import hashlib
from pathlib import Path, PurePosixPath
import stat

ARCHIVE_SHA256 = "0e15f0a2456ff8273c6cdd2008027a9a1152657fbc46754e41f1f04889783905"
ARCHIVE_ROOT = "GD32H77x_78x_Demo_Suites_V0.5.0"
LIBRARY = "GD32H77x_78x_Firmware_Library"
KERNEL = "GD32H77DI_EVAL_Demo_Suites/Projects/33_ENET/Third_party/FreeRTOS-Kernel-11.2.0"


def destination(source):
    if source.startswith(LIBRARY + "/CMSIS/"):
        return "gd32/" + source[len(LIBRARY) + 1:]
    if source.startswith(LIBRARY + "/GD32H77x_78x_standard_peripheral/"):
        return "gd32/" + source[len(LIBRARY) + 1:]
    if source.startswith(KERNEL + "/"):
        rel = source[len(KERNEL) + 1:]
        if (rel.startswith(("include/", "portable/GCC/ARM_CM7/r0p1/"))
                or rel == "portable/MemMang/heap_4.c"
                or ("/" not in rel and (rel.endswith(".c") or rel == "LICENSE.md"))):
            return "FreeRTOS/" + rel
    if source == "SOFTWARE LICENSE AGREEMENT SLA-GD0001-version1.1.pdf":
        return source
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    args = parser.parse_args()
    with args.archive.open("rb") as stream:
        actual = hashlib.file_digest(stream, "sha256").hexdigest()
    if actual != ARCHIVE_SHA256:
        parser.error(f"Unexpected SDK SHA256: {actual}")

    name = ctypes.util.find_library("archive")
    if not name:
        parser.error("libarchive is needed to import this 7z archive")
    lib = ctypes.CDLL(name)
    signatures = [
        ("archive_read_new", [], ctypes.c_void_p),
        ("archive_read_support_filter_all", [ctypes.c_void_p], ctypes.c_int),
        ("archive_read_support_format_all", [ctypes.c_void_p], ctypes.c_int),
        ("archive_read_open_filename", [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t], ctypes.c_int),
        ("archive_read_next_header", [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)], ctypes.c_int),
        ("archive_entry_pathname", [ctypes.c_void_p], ctypes.c_char_p),
        ("archive_entry_filetype", [ctypes.c_void_p], ctypes.c_uint),
        ("archive_read_data", [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t], ctypes.c_ssize_t),
        ("archive_read_free", [ctypes.c_void_p], ctypes.c_int),
        ("archive_error_string", [ctypes.c_void_p], ctypes.c_char_p),
    ]
    for function, arguments, result in signatures:
        getattr(lib, function).argtypes = arguments
        getattr(lib, function).restype = result

    output = Path(__file__).resolve().parents[2]
    reader = lib.archive_read_new()
    lib.archive_read_support_filter_all(reader)
    lib.archive_read_support_format_all(reader)
    entry = ctypes.c_void_p()
    buffer = ctypes.create_string_buffer(1024 * 1024)
    imported = 0
    try:
        if lib.archive_read_open_filename(reader, str(args.archive).encode(), 10240) != 0:
            raise RuntimeError(lib.archive_error_string(reader))
        while True:
            status = lib.archive_read_next_header(reader, ctypes.byref(entry))
            if status == 1:
                break
            if status < 0:
                raise RuntimeError(lib.archive_error_string(reader))
            path = PurePosixPath(lib.archive_entry_pathname(entry).decode())
            if path.is_absolute() or ".." in path.parts or path.parts[0] != ARCHIVE_ROOT:
                raise ValueError(f"Unexpected archive path: {path}")
            source = str(PurePosixPath(*path.parts[1:]))
            target = destination(source)
            if target is None or not stat.S_ISREG(lib.archive_entry_filetype(entry)):
                continue
            data = bytearray()
            while True:
                count = lib.archive_read_data(reader, buffer, len(buffer))
                if count < 0:
                    raise RuntimeError(lib.archive_error_string(reader))
                if not count:
                    break
                data.extend(buffer.raw[:count])
            dest = output / target
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
            imported += 1
    finally:
        lib.archive_read_free(reader)
    print(f"Imported {imported} vendor files from verified archive")


if __name__ == "__main__":
    main()
