#!/usr/bin/env python3
"""Host tests for the GD32H77X package payload build switch."""

import importlib.util
from pathlib import Path
import tempfile
import unittest


PLATFORM = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "gd32h77x_build_example_under_test", PLATFORM / "build_example.py")
build_example = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(build_example)


class BuildExampleTests(unittest.TestCase):
    def test_explicit_internal_only_keeps_validated_artifacts_and_omits_csv(self):
        with tempfile.TemporaryDirectory(prefix="gd32h77x-build-example-test-") as temporary:
            csv_path = Path(temporary) / "flash_payloads.csv"
            csv_path.write_text("partition,file\nstaging,payload.bin\n")
            args = ("python", "flash_package.py", "layout.json", "report.json",
                    "image.elf", "image.bin", "image.tufw", csv_path)

            default, enabled = build_example.make_package_command({}, *args)
            self.assertTrue(enabled)
            self.assertIn("--external-csv", default)

            internal, enabled = build_example.make_package_command(
                {"CONFIG_GD32H77X_PACKAGE_INTERNAL_ONLY": True}, *args)
            self.assertFalse(enabled)
            self.assertNotIn("--external-csv", internal)
            for option in ("--layout", "--report", "--elf", "--internal-bin", "--output"):
                self.assertIn(option, internal)

    def test_invalid_build_parameter_fails_instead_of_silently_ignoring_payloads(self):
        args = ("python", "flash_package.py", "layout.json", "report.json",
                "image.elf", "image.bin", "image.tufw", "missing.csv")
        with self.assertRaisesRegex(ValueError, "must be boolean"):
            build_example.make_package_command(
                {"CONFIG_GD32H77X_PACKAGE_INTERNAL_ONLY": "y"}, *args)


if __name__ == "__main__":
    unittest.main()
