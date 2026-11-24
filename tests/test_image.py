#!/usr/bin/env python3
"""Check real firmware artifacts and reject corrupted flashable images."""
import argparse
import json
import struct
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gd32h77x_os/tools/firmware"))
from verify_image import verify


class ImageTests(unittest.TestCase):
    elf = None
    binary = None
    resolved_layout = None
    ecnvm_binary = None
    ecnvm_enabled = False

    @classmethod
    def verify_args(cls):
        kwargs = {}
        if cls.ecnvm_binary is not None:
            kwargs["ecnvm_bin_path"] = cls.ecnvm_binary
        if cls.ecnvm_enabled:
            kwargs["ecnvm_enabled"] = True
        return kwargs

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.copy = Path(self.temporary.name) / "firmware.bin"
        self.raw = self.binary.read_bytes()

    def test_actual_image(self):
        result = verify(self.elf, self.binary, self.resolved_layout, **self.verify_args())
        self.assertEqual(result["size"], len(self.raw))
        self.assertEqual(result["vector_address"], "0x8010000")
        self.assertEqual(result["binSha256"], result["sha256"])
        self.assertEqual(result["normalizedLayoutDigest"], json.loads(self.resolved_layout.read_text())["normalizedLayoutDigest"])
        self.assertTrue(result["sourceInputs"])
        self.assertTrue(all({"sourceKind", "sourcePath", "digest"} <= item.keys()
                            for item in result["sourceInputs"]))

    def test_reject_corrupt_linker_heap_bounds(self):
        original = self.elf.read_bytes()
        section_offset = struct.unpack_from("<I", original, 32)[0]
        section_size, section_count = struct.unpack_from("<HH", original, 46)
        sections = [struct.unpack_from("<10I", original, section_offset + i * section_size)
                    for i in range(section_count)]
        offsets = {}
        for section in sections:
            if section[1] != 2:
                continue
            strings = sections[section[6]]
            table = original[strings[4]:strings[4] + strings[5]]
            for offset in range(section[4], section[4] + section[5], 16):
                name, value = struct.unpack_from("<II", original, offset)
                end = table.find(b"\0", name)
                symbol = table[name:end].decode()
                if symbol in ("__rtos_heap_start", "__rtos_heap_end", "ucHeap"):
                    offsets[symbol] = (offset + 4, value)
        if "__rtos_heap_start" not in offsets and "__rtos_heap_end" not in offsets:
            self.skipTest("Legacy image predates linker-managed heap")
        self.assertEqual(len(offsets), 3)
        for symbol, (offset, value) in offsets.items():
            with self.subTest(symbol=symbol):
                corrupt = bytearray(original)
                struct.pack_into("<I", corrupt, offset, value + 8)
                bad_elf = Path(self.temporary.name) / "bad-heap.elf"
                bad_elf.write_bytes(corrupt)
                with self.assertRaisesRegex(ValueError, "FreeRTOS heap bounds"):
                    verify(bad_elf, self.binary, self.resolved_layout, **self.verify_args())

    def test_corrupt_bytes(self):
        for offset in (0, 4, 100, 0x10000, 0x10000 + 15 * 4, 0x103CC, 0x11000, len(self.raw) - 1):
            with self.subTest(offset=hex(offset)):
                corrupt = bytearray(self.raw)
                corrupt[offset] ^= 1
                self.copy.write_bytes(corrupt)
                with self.assertRaises(ValueError):
                    verify(self.elf, self.copy, self.resolved_layout, **self.verify_args())

    def test_truncation(self):
        for end in (0, 8, 0x10001, len(self.raw) - 1):
            with self.subTest(end=end):
                self.copy.write_bytes(self.raw[:end])
                with self.assertRaises(ValueError):
                    verify(self.elf, self.copy, self.resolved_layout, **self.verify_args())

    def test_unexpected_trailer(self):
        self.copy.write_bytes(self.raw + b"\xff")
        with self.assertRaises(ValueError):
            verify(self.elf, self.copy, self.resolved_layout, **self.verify_args())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("binary", type=Path)
    parser.add_argument("resolved_layout", type=Path)
    parser.add_argument("--ecnvm-bin", type=Path)
    parser.add_argument("--ecnvm-enabled", action="store_true")
    args = parser.parse_args()
    ImageTests.elf = args.elf
    ImageTests.binary = args.binary
    ImageTests.resolved_layout = args.resolved_layout
    ImageTests.ecnvm_binary = args.ecnvm_bin
    ImageTests.ecnvm_enabled = args.ecnvm_enabled
    unittest.main(argv=[sys.argv[0]])
