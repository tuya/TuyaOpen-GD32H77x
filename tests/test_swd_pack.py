"""Offline validation of the pinned official CMSIS-Pack when available."""

import importlib.util
import importlib.metadata
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PACK_PATH = ROOT / "gd32h77x_os/tools/download/swd_pack.py"
SPEC = importlib.util.spec_from_file_location("gd32h77x_swd_pack_test", PACK_PATH)
swd_pack = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(swd_pack)
RESOLVER_PATH = ROOT / "gd32h77x_os/tools/firmware/resolve_memory_layout.py"
RESOLVER_SPEC = importlib.util.spec_from_file_location("gd32h77x_resolver_test", RESOLVER_PATH)
resolver = importlib.util.module_from_spec(RESOLVER_SPEC)
RESOLVER_SPEC.loader.exec_module(resolver)


def _official_pack():
    configured = os.environ.get("GD32H77X_DFP_PACK", "")
    if configured:
        return Path(configured).expanduser()
    return (ROOT / "gd32h77x_os/tools/packs"
            / "GigaDevice.GD32H77x_78x_DFP.0.6.1.pack")


class TestGd32h77xSwdPack(unittest.TestCase):
    def setUp(self):
        self.pack = _official_pack()
        if self.pack is None or not self.pack.is_file():
            self.skipTest("approved in-repository GigaDevice DFP 0.6.1 is unavailable")

    def _validate(self, chip):
        identity = resolver.resolve_chip_identity(chip, ROOT)
        geometry = {
            "capacity_bytes": identity["cnvm_capacity_bytes"],
            "erase_bytes": identity["cnvm_erase_bytes"],
            "program_bytes": identity["cnvm_program_bytes"],
            "vendor_algorithm": identity["cnvm_vendor_algorithm"],
        }
        return identity, geometry

    def test_official_pack_maps_all_registry_canonical_targets(self):
        registry = json.loads((ROOT / "gd32h77x_os" / "chips" / "platform_chips.json").read_text())
        canonical = sorted(name for name, entry in registry["platform_chips"].items()
                           if entry.get("target_chip") == name)
        self.assertEqual(len(canonical), 16)
        for chip in canonical:
            identity, geometry = self._validate(chip)
            with self.subTest(chip=chip):
                info = swd_pack.validate_pack(self.pack, identity["canonical_chip"], geometry)
                self.assertEqual(info["flash_size"], geometry["capacity_bytes"])
                self.assertEqual(info["erase_size"], 64 * 1024)
                self.assertEqual(info["program_unit"], 32)
                self.assertEqual(info["algorithm"], geometry["vendor_algorithm"])

    def test_installed_pyocd_lists_all_pack_targets_without_probe_discovery(self):
        try:
            version = importlib.metadata.version("pyocd")
        except importlib.metadata.PackageNotFoundError:
            self.skipTest("pyocd is not installed in this Python environment")
        if version != "0.46.0":
            self.skipTest(f"expected pyocd 0.46.0, found {version}")
        registry = json.loads((ROOT / "gd32h77x_os" / "chips" / "platform_chips.json").read_text())
        canonical = {name for name, entry in registry["platform_chips"].items()
                     if entry.get("target_chip") == name}
        with tempfile.TemporaryDirectory(prefix="gd32h77x-pyocd-offline-") as tmp:
            temp = Path(tmp)
            env = os.environ.copy()
            env["PYOCD_USER_DIR"] = str(temp / "user")
            Path(env["PYOCD_USER_DIR"]).mkdir()
            result = subprocess.run(
                [sys.executable, "-m", "pyocd", "json", "--targets",
                 "--pack", str(self.pack.resolve()), "--no-config",
                 "--project", str(temp), "--script", os.devnull],
                cwd=temp, env=env, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        targets = json.loads(result.stdout).get("targets", [])
        from_pack = {entry.get("part_number", "").upper()
                     for entry in targets
                     if entry.get("source") == "pack"
                     and entry.get("vendor", "").lower() == "gigadevice"}
        self.assertTrue(canonical.issubset(from_pack), canonical - from_pack)

    def test_pack_rejects_unregistered_target(self):
        identity, geometry = self._validate("GD32H77DIW")
        with self.assertRaisesRegex(swd_pack.PackValidationError, "canonical target"):
            swd_pack.validate_pack(self.pack, "GD32H77Z_FAKE", geometry)

    def test_pack_rejects_registry_capacity_mismatch(self):
        identity, geometry = self._validate("GD32H77DII")
        geometry["capacity_bytes"] = 2 * 1024 * 1024
        with self.assertRaisesRegex(swd_pack.PackValidationError, "CNVM region"):
            swd_pack.validate_pack(self.pack, identity["canonical_chip"], geometry)

    def test_pack_rejects_hash_mismatch(self):
        with tempfile.TemporaryDirectory(prefix="gd32h77x-pack-test-") as tmp:
            copy = Path(tmp) / self.pack.name
            copy.write_bytes(self.pack.read_bytes() + b"tamper")
            _identity, geometry = self._validate("GD32H77DIW")
            with self.assertRaisesRegex(swd_pack.PackValidationError, "SHA256 mismatch"):
                swd_pack.validate_pack(copy, "GD32H77DIW", geometry)


if __name__ == "__main__":
    unittest.main()
