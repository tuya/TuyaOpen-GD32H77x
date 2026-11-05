#!/usr/bin/env python3
"""Validate deterministic alignment of GD32H77X layout CSV files."""

import importlib.util
from pathlib import Path
import unittest


PLATFORM = Path(__file__).resolve().parents[1]
ROOT = PLATFORM.parents[1]
FORMATTER_PATH = (
    PLATFORM / "gd32h77x_os/tools/firmware/format_layout_csv.py")
SPEC = importlib.util.spec_from_file_location(
    "gd32h77x_layout_csv_formatter", FORMATTER_PATH)
formatter = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(formatter)


class LayoutCsvFormatterTests(unittest.TestCase):

    def test_aligns_columns_to_the_widest_value(self):
        source = "name,device,size\nboot,flash,64K\nmetadata,nor,128K\n"
        self.assertEqual(
            formatter.format_csv_text(source),
            "name,     device, size\n"
            "boot,     flash,  64K\n"
            "metadata, nor,    128K\n")

    def test_preserves_blank_and_comment_lines(self):
        source = "# layout\n\na,b\n1,22\n"
        self.assertEqual(
            formatter.format_csv_text(source),
            "# layout\n\na, b\n1, 22\n")

    def test_does_not_leave_trailing_spaces_on_empty_columns(self):
        formatted = formatter.format_csv_text(
            "device,geometry,cs,clk\nflash,CNVM,,\n")
        self.assertFalse(any(line.endswith(" ") for line in formatted.splitlines()))
        self.assertEqual(formatted, "device, geometry, cs, clk\nflash,  CNVM,     ,\n")

    def test_rejects_rows_with_different_column_counts(self):
        with self.assertRaisesRegex(ValueError, "same number of columns"):
            formatter.format_csv_text("a,b\n1,2,3\n")

    def test_board_and_chip_partitions_are_formatted(self):
        paths = list(sorted(
            (ROOT / "boards/GD32H77X").glob("*/platform_config/*.csv")))
        paths.extend(sorted(
            (PLATFORM / "gd32h77x_os/chips").glob("*/partitions.csv")))
        paths.extend(sorted(
            (PLATFORM / "gd32h77x_os/chips").glob("*/flash_devices.csv")))
        for path in paths:
            with self.subTest(path=path):
                source = path.read_text(encoding="utf-8")
                self.assertEqual(formatter.format_csv_text(source), source)


if __name__ == "__main__":
    unittest.main()
