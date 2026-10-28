"""SWD command policy tests; all probe/program/readback operations are mocked."""

import importlib.metadata
import importlib.util
import json
import shlex
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


BRIDGE_PATH = Path(__file__).resolve().parents[1] / "platform_flash_bridge.py"
SPEC = importlib.util.spec_from_file_location("gd32h77x_swd_backend_test", BRIDGE_PATH)
bridge = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bridge)


class TestGd32h77xSwdBackend(unittest.TestCase):
    def setUp(self):
        self.identity = bridge._load_platform_device("GD32H77D")
        self.tmp = tempfile.TemporaryDirectory(prefix="input with spaces ")
        self.root = Path(self.tmp.name)
        self.image = self.root / "firmware image.bin"
        self.image.write_bytes(b"firmware payload")
        self.report = {
            "chip": "GD32H77D",
            "layout": {
                "target_chip": "GD32H77DIW",
                "flash_image": {
                    "start_address": 0x08000000,
                    "end_address": 0x080E0000,
                    "capacity_bytes": 0xE0000,
                },
            },
            "addresses": {"flash": "0x08000000"},
        }
        self.config = {"CONFIG_FLASH_SWD_PACK": "/local/approved.pack"}
        self.calls = []
        self.payload = None
        self.readback_delta = 0
        self.probe_list = [{"unique_id": "cmsis-dap:probe-a", "info": "mock A"}]
        self.cwd_paths = []

    def tearDown(self):
        self.tmp.cleanup()

    def _run_pyocd(self, command, _logger, cwd, _env):
        self.calls.append(list(command))
        self.cwd_paths.append(str(cwd))
        if "json" in command and "--targets" in command:
            return 0, json.dumps({"targets": [{
                "part_number": "GD32H77DIW", "vendor": "GigaDevice", "source": "pack"
            }]})
        if "json" in command and "--probes" in command:
            return 0, json.dumps({"status": 0, "boards": self.probe_list})
        if "flash" in command:
            self.payload = Path(command[-1]).read_bytes()
            return 0, "mock flash command"
        if "commander" in command:
            argv = shlex.split(command[command.index("--command") + 1])
            self.assertEqual(argv[0], "savemem")
            self.assertEqual(argv[3], "readback.bin")
            data = bytearray(self.payload)
            if data and self.readback_delta:
                data[0] ^= self.readback_delta
            (Path(cwd) / argv[3]).write_bytes(data)
            return 0, "mock readback command"
        raise AssertionError(f"unexpected pyOCD command: {command}")

    def _flash(self, config=None, port="cmsis-dap:probe-a", capacity=0xE0000):
        with patch.object(bridge.importlib.metadata, "version", return_value="0.46.0"), \
             patch.object(bridge, "_validate_swd_pack", return_value={"erase_size": 65536}), \
             patch.object(bridge, "_run_pyocd", side_effect=self._run_pyocd):
            return bridge._flash_swd(
                config if config is not None else self.config,
                self.report, self.image, port,
                0x08000000, 0x08000000 + capacity, capacity, None)

    def test_writes_sector_padded_binary_with_protective_pyocd_options(self):
        self.config["CONFIG_FLASH_SWD_PACK"] = "relative pack.pack"
        real_tempdir = tempfile.TemporaryDirectory
        with patch.object(bridge.tempfile, "TemporaryDirectory",
                          side_effect=lambda prefix: real_tempdir(prefix=prefix, dir=self.root)):
            result = self._flash()
        self.assertTrue(result["success"], result)
        flash = next(command for command in self.calls if "flash" in command)
        for name, value in (("-e", "sector"), ("--format", "bin"),
                            ("--base-address", "0x08000000"),
                            ("--target", "GD32H77DIW"),
                            ("--probe", "cmsis-dap:probe-a")):
            self.assertEqual(flash[flash.index(name) + 1], value)
        for option in ("auto_unlock=false", "keep_unwritten=false", "smart_flash=false",
                       "resume_on_disconnect=false"):
            self.assertIn(option, flash)
        self.assertIn("smart_flash=false", flash)
        self.assertIn("--no-reset", flash)
        self.assertIn("--no-config", flash)
        self.assertEqual(len(self.payload), 65536)
        self.assertEqual(self.payload[:self.image.stat().st_size], self.image.read_bytes())
        self.assertEqual(self.payload[self.image.stat().st_size:],
                         b"\xFF" * (65536 - self.image.stat().st_size))
        readback = next(command for command in self.calls if "commander" in command)
        cmd_text = readback[readback.index("--command") + 1]
        self.assertIn("readback.bin", cmd_text)
        self.assertNotIn(str(self.root), cmd_text)
        self.assertTrue(any(" " in path for path in self.cwd_paths))
        self.assertIn("padded CNVM bytes read back and verified", result["message"])
        pack_paths = [command[command.index("--pack") + 1]
                      for command in self.calls if "--pack" in command]
        self.assertEqual(pack_paths, [str(Path("relative pack.pack").resolve())] * 3)

    def test_readback_mismatch_fails(self):
        self.readback_delta = 1
        result = self._flash()
        self.assertFalse(result["success"])
        self.assertIn("readback mismatch", result["message"])

    def test_verification_disabled_is_reported_and_skips_commander(self):
        result = self._flash({**self.config, "CONFIG_FLASH_VERIFY": "n"})
        self.assertTrue(result["success"], result)
        self.assertIn("verification skipped", result["message"])
        self.assertFalse(any("commander" in command for command in self.calls))

    def test_bad_pack_is_rejected_before_pyocd_or_probe_access(self):
        with patch.object(bridge.importlib.metadata, "version", return_value="0.46.0"), \
             patch.object(bridge, "_validate_swd_pack", side_effect=ValueError("bad pack")), \
             patch.object(bridge, "_run_pyocd", side_effect=self._run_pyocd):
            with self.assertRaisesRegex(ValueError, "bad pack"):
                bridge._flash_swd(self.config, self.report, self.image,
                                  "cmsis-dap:probe-a", 0x08000000,
                                  0x080E0000, 0xE0000, None)
        self.assertEqual(self.calls, [])

    def test_unknown_cli_version_fails_closed(self):
        with patch.object(bridge.importlib.metadata, "version", return_value="0.47.0"), \
             patch.object(bridge, "_run_pyocd", side_effect=self._run_pyocd):
            with self.assertRaisesRegex(RuntimeError, "requires pyocd==0.46.0"):
                bridge._flash_swd(self.config, self.report, self.image,
                                  "cmsis-dap:probe-a", 0x08000000,
                                  0x080E0000, 0xE0000, None)
        self.assertEqual(self.calls, [])

    def test_board_or_manifest_target_override_is_rejected_before_cli(self):
        bad_report = {**self.report, "layout": {**self.report["layout"],
                                                "target_chip": "GD32H779IW"}}
        with patch.object(bridge.importlib.metadata, "version", return_value="0.46.0"), \
             patch.object(bridge, "_run_pyocd", side_effect=self._run_pyocd):
            with self.assertRaisesRegex(ValueError, "canonical target"):
                bridge._flash_swd(self.config, bad_report, self.image,
                                  "cmsis-dap:probe-a", 0x08000000,
                                  0x080E0000, 0xE0000, None)
        self.assertEqual(self.calls, [])

    def test_multiple_probes_need_selector_without_tty(self):
        self.probe_list = [{"unique_id": "cmsis-dap:a"}, {"unique_id": "cmsis-dap:b"}]
        with patch.object(bridge.sys.stdin, "isatty", return_value=False):
            with self.assertRaisesRegex(RuntimeError, "non-interactive mode"):
                self._flash(port="")

    def test_multiple_probes_allow_explicit_tty_selection(self):
        probes = [{"unique_id": "cmsis-dap:a"}, {"unique_id": "cmsis-dap:b"}]
        with patch.object(bridge.sys.stdin, "isatty", return_value=True), \
             patch("builtins.input", return_value="2"):
            self.assertEqual(bridge._select_swd_probe(probes, ""), "cmsis-dap:b")

    def test_padded_range_overflow_fails_before_probe_enumeration(self):
        self.image.write_bytes(b"x" * (0xE0000 + 1))
        with self.assertRaisesRegex(ValueError, "Sector-padded SWD image exceeds"):
            self._flash()
        self.assertFalse(any("--probes" in command for command in self.calls))


if __name__ == "__main__":
    unittest.main()
