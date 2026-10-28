#!/usr/bin/env python3
"""Check real firmware artifacts and reject corrupted flashable images."""
import argparse
import json
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

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.copy = Path(self.temporary.name) / "firmware.bin"
        self.raw = self.binary.read_bytes()

    def test_actual_image(self):
        result = verify(self.elf, self.binary, self.resolved_layout)
        self.assertEqual(result["size"], len(self.raw))
        self.assertEqual(result["vector_address"], "0x8010000")
        self.assertEqual(result["binSha256"], result["sha256"])
        self.assertEqual(result["normalizedLayoutDigest"], json.loads(self.resolved_layout.read_text())["normalizedLayoutDigest"])
        self.assertTrue(result["sourceInputs"])
        self.assertTrue(all({"sourceKind", "sourcePath", "digest"} <= item.keys()
                            for item in result["sourceInputs"]))

    def test_corrupt_bytes(self):
        for offset in (0, 4, 100, 0x10000, 0x10000 + 15 * 4, 0x103CC, 0x11000, len(self.raw) - 1):
            with self.subTest(offset=hex(offset)):
                corrupt = bytearray(self.raw)
                corrupt[offset] ^= 1
                self.copy.write_bytes(corrupt)
                with self.assertRaises(ValueError):
                    verify(self.elf, self.copy, self.resolved_layout)

    def test_truncation(self):
        for end in (0, 8, 0x10001, len(self.raw) - 1):
            with self.subTest(end=end):
                self.copy.write_bytes(self.raw[:end])
                with self.assertRaises(ValueError):
                    verify(self.elf, self.copy, self.resolved_layout)

    def test_unexpected_trailer(self):
        self.copy.write_bytes(self.raw + b"\xff")
        with self.assertRaises(ValueError):
            verify(self.elf, self.copy, self.resolved_layout)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("binary", type=Path)
    parser.add_argument("resolved_layout", type=Path)
    args = parser.parse_args()
    ImageTests.elf = args.elf
    ImageTests.binary = args.binary
    ImageTests.resolved_layout = args.resolved_layout
    unittest.main(argv=[sys.argv[0]])
