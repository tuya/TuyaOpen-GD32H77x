"""Validate dependency preparation in a uv-managed SDK without pip."""

import importlib.util
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
SPEC = importlib.util.spec_from_file_location("gd32_platform_prepare_test", ROOT / "platform_prepare.py")
prepare = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(prepare)


class PlatformPrepareTests(unittest.TestCase):
    def test_uv_installs_into_sdk_python_without_pip(self):
        for region in ("China", "Other"):
            with self.subTest(region=region), patch.dict(os.environ, {
                    "OPEN_SDK_UV": "C:/SDK tools/uv.exe", "OPEN_COUNTRY_CODE": region}), patch.object(
                    prepare.importlib.metadata, "version", side_effect=["", "0.46.0"]), patch.object(
                    prepare.subprocess, "run") as run:
                self.assertEqual(prepare._ensure_pyocd(), "0.46.0")
                command = run.call_args.args[0]
                self.assertEqual(command[:5], ["C:/SDK tools/uv.exe", "pip", "install", "--python", sys.executable])
                self.assertEqual("--default-index" in command, region == "China")

    def test_ready_pyocd_does_not_reinstall(self):
        with patch.object(prepare.importlib.metadata, "version", return_value="0.46.0"), patch.object(
                prepare.subprocess, "run") as run:
            self.assertEqual(prepare._ensure_pyocd(), "0.46.0")
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
