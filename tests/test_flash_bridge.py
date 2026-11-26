import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import tempfile
from types import SimpleNamespace
import unittest
import zipfile
from unittest.mock import patch


BRIDGE_PATH = Path(__file__).resolve().parents[1] / "platform_flash_bridge.py"
SPEC = importlib.util.spec_from_file_location("gd32h77x_flash_bridge", BRIDGE_PATH)
bridge = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bridge)


# ---------------------------------------------------------------------------
# Fake DfuSe transport: models a CNVM whose erase unit is the registry one.
# ---------------------------------------------------------------------------

class FakeFlash:
    def __init__(self, base, erase_bytes, capacity, fill=0xFF, erase_regions=None):
        self.base = base
        self.erase_bytes = erase_bytes
        self.erase_regions = list(erase_regions or [])
        self.capacity = capacity
        self.memory = bytearray([fill]) * capacity
        self.erases = []
        self.writes = []
        self.claimed = None
        self.closed = False

    def erase(self, address):
        self.erases.append(address)
        erase_bytes = self.erase_bytes
        for start, limit, unit in self.erase_regions:
            if start <= address < limit:
                erase_bytes = unit
                break
        offset = address - self.base
        self.memory[offset:offset + erase_bytes] = b"\xFF" * erase_bytes

    def write(self, address, data):
        self.writes.append((address, bytes(data)))
        offset = address - self.base
        self.memory[offset:offset + len(data)] = data

    def read(self, address, size):
        offset = address - self.base
        return bytes(self.memory[offset:offset + size])


class FakeDfuSeProtocol:
    def __init__(self, device, interface, transfer_size, timeout_ms=None, log=None):
        self.device = device
        self.interface = interface
        self.transfer_size = transfer_size
        self.settled = False
        self.left = False
        self._log = log

    def settle(self):
        self.settled = True

    def erase(self, address):
        self.device.flash.erase(address)

    def write(self, address, data, progress=None):
        for offset in range(0, len(data), self.transfer_size):
            chunk = data[offset:offset + self.transfer_size]
            self.device.flash.write(address + offset, chunk)
            if progress:
                progress(offset + len(chunk))

    def read(self, address, size, progress=None):
        return self.device.flash.read(address, size)

    def leave(self):
        self.left = True


class FakeUsbDevice:
    def __init__(self, flash, path, serial, transfer_size=64):
        self.flash = flash
        self.path = path
        self.serial = serial
        self.transfer_size = transfer_size
        self.closed = False

    def interfaces(self):
        return [{"interface": 0, "alt": 0, "protocol": 2,
                 "string_index": 1,
                 "name": "@InternalFlash /0x08000000/960*004Kg",
                 "bcd_dfu_version": 0x011A, "bm_attributes": 0x07,
                 "transfer_size": self.transfer_size}]

    def claim(self, interface, alt=0):
        self.flash.claimed = (interface, alt)

    def close(self):
        self.closed = True


class FakeLibusb:
    def __init__(self):
        self.closed = False

    def close(self):
        self.closed = True


def _fake_dfuse(devices):
    lib = FakeLibusb()

    class _Module:
        DfuSeProtocol = FakeDfuSeProtocol
        erase_plan = staticmethod(lambda start, limit, unit: list(range(start, limit, unit)))

        @staticmethod
        def open_dfu_devices(vendor, product):
            return lib, list(devices)

    return _Module(), lib


def _fake_uart_module(calls, image):
    def flash(**kwargs):
        calls.append(kwargs)
        image_path = Path(kwargs.get("image_path"))
        return {
            "success": True,
            "verified": bool(kwargs.get("verify")),
            "external": [{"partition": item.get("partition")}
                         for item in kwargs.get("external_payloads", [])],
            "internal": {"size_bytes": image_path.stat().st_size if image_path.is_file() else 0},
        }

    return SimpleNamespace(CNVM_BASE=0x08000000, CNVM_PAGE=0x10000, flash=flash)


class TestGd32h77xFlashBridge(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.board_dir = (self.root / "GD32H77X" / "GD32H77D_TEST_BOARD"
                          / "platform_config")
        self.board_dir.mkdir(parents=True)
        self.image = self.root / "firmware.bin"
        self.image.write_bytes(b"firmware")
        self.elf = self.root / "firmware.elf"
        self.elf.write_bytes(b"elf firmware")
        self.using = {
            "CONFIG_PLATFORM_CHOICE": "GD32H77X",
            "CONFIG_BOARD_CHOICE": "GD32H77D_TEST_BOARD",
            "CONFIG_CHIP_CHOICE": "GD32H77D",
            "CONFIG_PROJECT_NAME": "firmware",
            "CONFIG_PROJECT_VERSION": "1.0.0",
        }
        self._write_board_config("CONFIG_FLASH_BACKEND=dfu\n")
        self.flash = FakeFlash(0x08000000, 0x10000, 0x200000)
        self.current_layout = {
            "source": {"sourceKind": "platform_default", "sourceDigest": "source-digest"},
            "normalizedLayoutDigest": "layout-digest",
            "build": {"canonical_target": "GD32H77DIW"},
            "layout": {
                "target_chip": "GD32H77DIW",
                "flash_image": {"start_address": 0x08000000,
                                "end_address": 0x080E0000,
                                "capacity_bytes": 0x000E0000,
                                "boot_partition": "boot",
                                "primary_partition": "primary"},
                "devices": [{"id": "internal_flash", "region": "CNVM",
                             "geometry_id": "GD32H77DIW_CNVM",
                             "part_number": "GD32H77DIW",
                             "cpu_base": 0x08000000, "capacity_bytes": 0x200000,
                             "erase_bytes": 0x10000, "program_bytes": 32,
                             "vendor_algorithm": "GD32H77x_78x_CNVM_2M.FLM"},
                            {"id": "renamed_nor", "region": "NOR", "kind": "flash",
                             "access": "indirect", "backend_status": "available",
                             "geometry_id": "test_nor", "capacity_bytes": 0x800000,
                             "erase_bytes": 0x1000, "page_bytes": 256}],
                "board_binding": {"renamed_nor": {"controller": "OSPI0"}},
                "partitions": {
                    "boot": {"id": "boot", "logical_base": 0x08000000,
                             "segments": [{"device": "internal_flash",
                                           "logical_offset_bytes": 0,
                                           "device_offset_bytes": 0,
                                           "size_bytes": 0x10000}]},
                    "primary": {"id": "primary", "logical_base": 0x08010000,
                                "segments": [{"device": "internal_flash",
                                              "logical_offset_bytes": 0,
                                              "device_offset_bytes": 0x10000,
                                              "size_bytes": 0xD0000}]},
                },
            },
        }
        self._write_report()

    def tearDown(self):
        self.tmp.cleanup()

    # -- helpers ------------------------------------------------------------
    def _write_board_config(self, extra=""):
        backend_symbols = {
            "swd": "CONFIG_ENABLE_GD32H77X_FLASH_BACKEND_SWD",
            "dfu": "CONFIG_ENABLE_GD32H77X_FLASH_BACKEND_DFU",
            "uart": "CONFIG_ENABLE_GD32H77X_FLASH_BACKEND_UART",
        }
        direct_keys = {
        }
        string_keys = {
        }
        lines = []
        for line in extra.splitlines():
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            value = value.strip()
            if key == "CONFIG_FLASH_BACKEND":
                symbol = backend_symbols.get(value.lower())
                if symbol:
                    lines.append(f"{symbol}=y")
                continue
            mapped = direct_keys.get(key)
            if not mapped:
                # UART expected ID now lives in the chip registry; OSPI profile
                # files are discovered from the board directory.
                continue
            if mapped in string_keys:
                value = f'"{value}"'
            lines.append(f"{mapped}={value}")
        with open(self.board_dir / "gd32h77x.config", 'a', encoding='utf-8') as f:
            f.write("\n".join(lines) + "\n")

    def _write_report(self):
        report = {
            "board": "GD32H77D_TEST_BOARD",
            "chip": "GD32H77D",
            "sourceKind": "platform_default",
            "sourceDigest": "source-digest",
            "normalizedLayoutDigest": "layout-digest",
            "layout": self.current_layout["layout"],
            "binFile": self.image.name,
            "binSha256": hashlib.sha256(self.image.read_bytes()).hexdigest(),
            "elfFile": self.elf.name,
            "elfSha256": hashlib.sha256(self.elf.read_bytes()).hexdigest(),
            "size": self.image.stat().st_size,
            "addresses": {"flash": "0x08000000"},
            "allowwrite": [
                {"partition": "boot", "device": "internal_flash", "deviceOffset": 0,
                 "size": 0x10000, "logicalBase": 0x08000000},
                {"partition": "primary", "device": "internal_flash", "deviceOffset": 0x10000,
                 "size": 0xD0000, "logicalBase": 0x08010000},
            ],
        }
        self.report = report
        self.reported_bin_bytes = self.image.read_bytes()
        self.reported_elf_bytes = self.elf.read_bytes()
        (self.root / "firmware_1.0.0.layout.json").write_text(
            json.dumps(report), encoding="utf-8")

    def _run_flash(self, devices=None, usb_path="", tty=False, inputs=(),
                   verify=True):
        devices = list(devices) if devices is not None else [
            FakeUsbDevice(self.flash, "3-3.2", "DIWd")]
        module, lib = _fake_dfuse(devices)
        with patch.object(bridge, "_DFUSE_MODULE", module), \
             patch.object(bridge, "_resolve_current_layout",
                          return_value=copy.deepcopy(self.current_layout)), \
             patch.object(bridge, "_verify_build_artifact"), \
             patch.object(bridge.sys.stdin, "isatty", return_value=tty), \
             patch("builtins.input", side_effect=list(inputs)):
            result = bridge.platform_flash(
                self.using, str(self.image), usb_path, 0, str(self.root), None,
                verify=verify)
        return result, devices, lib

    def _mock_package_module(self, external_payloads=()):
        report = copy.deepcopy(self.report)
        report["binFile"] = "firmware_QIO_1.0.0.bin"
        report["elfFile"] = "firmware_1.0.0.elf"

        class _PackageModule:
            @staticmethod
            def validate_package(package, layout, expected_board=None,
                                 expected_chip=None, extract_dir=None):
                root = Path(extract_dir)
                files = root / "artifacts"
                files.mkdir(parents=True, exist_ok=True)
                report_path = files / "report.json"
                elf_path = files / "firmware.elf"
                bin_path = files / "internal.bin"
                report_path.write_text(json.dumps(report))
                elf_path.write_bytes(self.reported_elf_bytes)
                bin_path.write_bytes(self.reported_bin_bytes)
                payload_results = []
                for index, entry in enumerate(external_payloads):
                    payload_path = files / f"payload-{index}.bin"
                    payload_path.write_bytes(b"external payload")
                    payload_results.append({**entry, "path": str(payload_path)})
                return {
                    "report": report,
                    "internal_bin_sha256": report["binSha256"],
                    "internal_bin_size_bytes": report["size"],
                    "artifacts": {"report": str(report_path), "elf": str(elf_path),
                                  "internal_bin": str(bin_path)},
                    "external_payloads": payload_results,
                }
        return _PackageModule()

    def _run_uart_direct(self, config, port="", external_payloads=()):
        calls = []
        with patch.object(bridge, "_UART_ISP_MODULE",
                          _fake_uart_module(calls, self.image)):
            result = bridge._flash_uart(
                config, self.report, self.current_layout if external_payloads else None,
                self.image, external_payloads, port, 0, 0x08000000,
                0x080E0000, 0xE0000,
                {"erase_bytes": 0x10000}, None)
        return result, calls

    def _run_package_flash(self, external_payloads=()):
        package = self.root / "firmware_QIO_1.0.0.tufw"
        package.write_bytes(b"self contained package")
        self.image.unlink(missing_ok=True)
        self.elf.unlink(missing_ok=True)
        (self.root / "firmware_1.0.0.layout.json").unlink(missing_ok=True)
        module, lib = _fake_dfuse([FakeUsbDevice(self.flash, "3-3.2", "DIWd")])
        with patch.object(bridge, "_FLASH_PACKAGE_MODULE",
                          self._mock_package_module(external_payloads)), \
             patch.object(bridge, "_DFUSE_MODULE", module), \
             patch.object(bridge, "_resolve_current_layout",
                          return_value=copy.deepcopy(self.current_layout)), \
             patch.object(bridge, "_verify_build_artifact"):
            result = bridge.platform_flash(
                self.using, str(package), "", 0, str(self.root), None)
        return result, module, lib

    def test_dfu_autostart_follows_platform_setting(self):
        for setting in ("y", "n"):
            with self.subTest(setting=setting):
                (self.board_dir / "gd32h77x.config").write_text(
                    "CONFIG_ENABLE_GD32H77X_FLASH_BACKEND_DFU=y\n"
                    f"CONFIG_ENABLE_GD32H77X_FLASH_DFU_AUTOSTART={setting}\n",
                    encoding="utf-8")
                config = bridge._board_flash_config(self.using, str(self.root))
                self.assertEqual(config["CONFIG_FLASH_DFU_LEAVE"], setting)

    def test_board_flash_backend_and_fixed_runtime_defaults(self):
        (self.board_dir / "gd32h77x.config").write_text(
            "CONFIG_ENABLE_GD32H77X_FLASH_BACKEND_SWD=y\n",
            encoding="utf-8")
        with patch.object(bridge, "_resolve_swd_pack", return_value=""):
            config = bridge._board_flash_config(self.using, str(self.root))
        self.assertEqual(config["CONFIG_FLASH_BACKEND"], "swd")
        self.assertEqual(config["CONFIG_FLASH_SWD_PACK"], "")
        self.assertEqual(config["CONFIG_FLASH_UART_BAUD"], "auto")
        self.assertEqual(config["CONFIG_FLASH_VERIFY"], "y")
        self.assertEqual(config["CONFIG_FLASH_SWD_PROBE"], "")

        with patch.dict(os.environ, {
                "GD32H77X_DFP_PACK": "/env/override.pack"}, clear=False):
            config = bridge._board_flash_config(self.using, str(self.root))
        self.assertEqual(config["CONFIG_FLASH_SWD_PACK"], "/env/override.pack")

    def test_platform_board_config_selects_backend(self):
        config = bridge._board_flash_config(self.using, str(self.root))
        self.assertEqual(config["CONFIG_FLASH_BACKEND"], "dfu")

        self._write_board_config("CONFIG_FLASH_BACKEND=uart\n")
        config = bridge._board_flash_config(self.using, str(self.root))
        self.assertEqual(config["CONFIG_FLASH_BACKEND"], "uart")

    def test_missing_board_config_uses_kconfig_defaults_and_canonical_target(self):
        shutil.rmtree(self.board_dir)
        config = bridge._board_flash_config(self.using, str(self.root))
        self.assertEqual(config["CONFIG_FLASH_BACKEND"], "swd")
        self.assertEqual(config["CONFIG_FLASH_VERIFY"], "y")
        self.assertEqual(config["CONFIG_FLASH_TARGET_CHIP"], "GD32H77DIW")
        self.assertEqual(config["CONFIG_FLASH_SWD_TARGET"], "GD32H77DIW")
        self.assertEqual(Path(config["_CONFIG_DIR"]), BRIDGE_PATH.parent)

    def test_ecnvm_raw_bin_requires_its_verified_sidecar(self):
        report = {
            "layout": {
                "partitions": {"ecnvm_code": {"logical_base": 0x08200000,
                                               "size_bytes": 0x10000}},
                "devices": [],
            },
            "mapped_payloads": [{"file": "missing.ecnvm.bin"}],
        }
        with patch.object(bridge, "_validate_manifest",
                          return_value=(report, 0x08000000, 0x08090000,
                                        0x90000, {})), \
             patch.object(bridge, "_dfuse_module",
                          side_effect=AssertionError("no device may be opened")):
            result = bridge.platform_flash(
                self.using, str(self.image), "", 0,
                str(self.root), None)
        self.assertFalse(result["success"])
        self.assertIn("QIO package is missing its ECNVM payload", result["message"])

    def test_mixed_external_and_mapped_payloads_fail_before_device_access(self):
        package = self.root / "mixed.tufw"
        package.write_bytes(b"package")
        external = [{"partition": "filesystem", "path": "payload.bin"}]
        mapped = [{"partition": "ecnvm_code", "path": "ecnvm.bin"}]
        with patch.object(bridge, "_extract_validated_package",
                          return_value=(self.image, external, mapped)), \
             patch.object(bridge, "_validate_manifest",
                          side_effect=AssertionError("manifest/device path must not run")), \
             patch.object(bridge, "_dfuse_module",
                          side_effect=AssertionError("no device may be opened")):
            result = bridge.platform_flash(
                self.using, str(package), "", 0, str(self.root), None)
        self.assertFalse(result["success"])
        self.assertIn("mixing indirect NOR and mapped ECNVM", result["message"])

    def test_ecnvm_readback_mismatch_stops_before_cnvm_even_when_verify_is_disabled(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            cnvm = root / "cnvm.bin"
            cnvm.write_bytes(b"cnvm")
            ecnvm = root / "ecnvm.bin"
            ecnvm.write_bytes(b"ecnvm payload")
            report = {
                "chip": "GD32H779VW",
                "layout": {
                    "target_chip": "GD32H779VW",
                    "flash_image": {"start_address": 0x08000000,
                                    "end_address": 0x080E0000,
                                    "capacity_bytes": 0xE0000},
                    "devices": [{"region": "CNVM", "capacity_bytes": 0x200000,
                        "erase_bytes": 0x10000, "cpu_base": 0x08000000},
                        {"region": "ECNVM", "geometry_id": "GD32H779VW_ECNVM",
                        "backend_status": "implemented", "cpu_base": 0x08200000,
                        "capacity_bytes": 0x780000, "erase_bytes": 0x1000,
                        "program_bytes": 8, "page_bytes": 0x1000,
                        "swd_erase_bytes": 0x10000, "swd_program_bytes": 32,
                        "vendor_algorithm": "GD32H77x_78x_ECNVM_7M_512K.FLM"}],
                    "partitions": {"ecnvm_code": {"logical_base": 0x08200000,
                        "size_bytes": 0x10000}},
                },
            }
            mapped = [{"partition": "ecnvm_code", "region": "ECNVM",
                "address": 0x08200000, "path": str(ecnvm),
                "sha256": hashlib.sha256(ecnvm.read_bytes()).hexdigest()}]
            commands = []
            pack_geometries = []

            def fake_run(command, _logger, cwd, _env):
                commands.append(command)
                if "json" in command and "--targets" in command:
                    return 0, json.dumps({"targets": [{"part_number": "GD32H779VW",
                        "vendor": "GigaDevice", "source": "pack"}]})
                if "json" in command and "--probes" in command:
                    return 0, json.dumps({"status": 0, "boards": [{"unique_id": "probe-1"}]})
                if "commander" in command:
                    (Path(cwd) / "ecnvm-readback.bin").write_bytes(b"mismatch")
                return 0, ""

            def fake_validate_pack(_root, _pack, _target, geometry):
                pack_geometries.append(dict(geometry))
                return {"erase_size": 0x10000}

            with patch.object(bridge.importlib.metadata, "version", return_value="0.46.0"), \
                 patch.object(bridge, "_validate_swd_pack", side_effect=fake_validate_pack), \
                 patch.object(bridge, "_run_pyocd", side_effect=fake_run):
                result = bridge._flash_swd({"CONFIG_FLASH_SWD_TARGET": "GD32H779VW",
                    "CONFIG_FLASH_SWD_PACK": "/fake.pack",
                    "CONFIG_FLASH_SWD_FREQUENCY_HZ": "10000000",
                    "CONFIG_FLASH_VERIFY": "n"},
                    report, cnvm, "", 0x08000000, 0x080E0000, 0xE0000, None, mapped)
            self.assertFalse(result["success"])
            self.assertIn("ECNVM readback failed", result["message"])
            flash_commands = [item for item in commands if "flash" in item]
            self.assertEqual(len(flash_commands), 1)
            self.assertIn("0x08200000", flash_commands[0])
            self.assertTrue(any("commander" in item for item in commands))
            self.assertEqual([item["erase_bytes"] for item in pack_geometries], [0x10000, 0x10000])
            self.assertEqual([item["program_bytes"] for item in pack_geometries], [32, 32])

    # -- native DfuSe backend ----------------------------------------------
    def test_native_dfu_erases_registry_geometry_and_programs_padded_image(self):
        result, devices, lib = self._run_flash()
        self.assertTrue(result["success"], result)
        self.assertTrue(lib.closed)
        self.assertTrue(devices[0].closed)
        self.assertEqual(self.flash.claimed, (0, 0))
        self.assertEqual(self.flash.erases, [0x08000000])
        self.assertEqual(self.flash.read(0x08000000, 8), b"firmware")
        self.assertEqual(self.flash.read(0x08000008, 8), b"\xFF" * 8)
        self.assertEqual(sum(len(chunk) for _address, chunk in self.flash.writes), 0x10000)
        self.assertEqual(self.image.read_bytes(), b"firmware")

    def test_native_dfu_programs_ecnvm_before_cnvm_and_verifies_both(self):
        payload = b"ECNVM" * 0x801  # 0x2004 bytes, padded to three 4 KiB pages.
        ecnvm = self.root / "ecnvm_code.bin"
        ecnvm.write_bytes(payload)
        report = {
            "chip": "GD32H77DIW",
            "layout": {
                "devices": [{
                    "id": "internal_ecnvm", "region": "ECNVM",
                    "geometry_id": "GD32H77DIW_ECNVM",
                    "backend_status": "implemented",
                    "cpu_base": 0x08200000, "capacity_bytes": 0x780000,
                    "erase_bytes": 0x1000, "program_bytes": 8, "page_bytes": 0x1000,
                }],
                "partitions": {
                    "ecnvm_code": {"logical_base": 0x08200000,
                                   "size_bytes": 0x700000},
                },
            },
        }
        mapped = [{
            "partition": "ecnvm_code", "region": "ECNVM",
            "address": 0x08200000, "size_bytes": len(payload), "path": str(ecnvm),
            "sha256": hashlib.sha256(payload).hexdigest(),
        }]
        flash = FakeFlash(
            0x08000000, 0x10000, 0x980000,
            erase_regions=[(0x08200000, 0x08980000, 0x1000)])
        module, lib = _fake_dfuse([FakeUsbDevice(flash, "3-3.2", "DIWd")])
        config = {
            "CONFIG_FLASH_USB_VID": "28e9", "CONFIG_FLASH_USB_PID": "0189",
            "CONFIG_FLASH_DFU_ALT": "0",
            "CONFIG_FLASH_DFU_ALT_NAME": "InternalFlash",
            "CONFIG_FLASH_VERIFY": "y", "CONFIG_FLASH_DFU_LEAVE": "n",
        }
        with patch.object(bridge, "_DFUSE_MODULE", module):
            result = bridge._flash_dfu(
                config, report, self.image, 0x08000000, 0x08010000, 0x10000,
                {"erase_bytes": 0x10000, "geometry_id": "GD32H77DIW_CNVM",
                 "part_number": "GD32H77DIW"}, "", None, mapped)
        self.assertTrue(result["success"], result)
        self.assertIn("CNVM+ECNVM", result["message"])
        self.assertEqual(flash.erases, [
            0x08200000, 0x08201000, 0x08202000, 0x08000000,
        ])
        self.assertEqual(flash.read(0x08200000, 0x3000),
                         payload + b"\xFF" * (0x3000 - len(payload)))
        self.assertEqual(flash.read(0x08000000, 8), self.image.read_bytes())
        self.assertTrue(lib.closed)

    def test_native_dfu_ecnvm_readback_mismatch_does_not_touch_cnvm(self):
        payload = b"ECNVM" * 4
        ecnvm = self.root / "ecnvm_code.bin"
        ecnvm.write_bytes(payload)
        report = {
            "chip": "GD32H77DIW",
            "layout": {
                "devices": [{
                    "id": "internal_ecnvm", "region": "ECNVM",
                    "geometry_id": "GD32H77DIW_ECNVM",
                    "backend_status": "implemented",
                    "cpu_base": 0x08200000, "capacity_bytes": 0x780000,
                    "erase_bytes": 0x1000, "program_bytes": 8, "page_bytes": 0x1000,
                }],
                "partitions": {
                    "ecnvm_code": {"logical_base": 0x08200000,
                                   "size_bytes": 0x700000},
                },
            },
        }
        mapped = [{
            "partition": "ecnvm_code", "region": "ECNVM",
            "address": 0x08200000, "size": len(payload), "path": str(ecnvm),
            "sha256": hashlib.sha256(payload).hexdigest(),
        }]
        flash = FakeFlash(
            0x08000000, 0x10000, 0x980000,
            erase_regions=[(0x08200000, 0x08980000, 0x1000)])
        module, _lib = _fake_dfuse([FakeUsbDevice(flash, "3-3.2", "DIWd")])
        original_read = FakeDfuSeProtocol.read

        def corrupt_ecnvm(protocol, address, size, progress=None):
            data = bytearray(original_read(protocol, address, size, progress))
            if address == 0x08200000:
                data[0] ^= 0xFF
            return bytes(data)

        config = {
            "CONFIG_FLASH_USB_VID": "28e9", "CONFIG_FLASH_USB_PID": "0189",
            "CONFIG_FLASH_DFU_ALT": "0",
            "CONFIG_FLASH_DFU_ALT_NAME": "InternalFlash",
            "CONFIG_FLASH_VERIFY": "y", "CONFIG_FLASH_DFU_LEAVE": "n",
        }
        with patch.object(bridge, "_DFUSE_MODULE", module), \
             patch.object(FakeDfuSeProtocol, "read", corrupt_ecnvm):
            with self.assertRaisesRegex(RuntimeError, "ECNVM readback mismatch"):
                bridge._flash_dfu(
                    config, report, self.image, 0x08000000, 0x08010000, 0x10000,
                    {"erase_bytes": 0x10000, "geometry_id": "GD32H77DIW_CNVM",
                     "part_number": "GD32H77DIW"}, "", None, mapped)
        self.assertEqual(flash.erases, [0x08200000])

    def test_platform_flash_routes_mapped_ecnvm_to_dfu(self):
        package = self.root / "firmware_QIO_1.0.0.tufw"
        package.write_bytes(b"self contained package")
        ecnvm = self.root / "ecnvm_code.bin"
        ecnvm.write_bytes(b"ecnvm")
        mapped = [{
            "partition": "ecnvm_code", "region": "ECNVM",
            "address": 0x08200000, "size": 5, "path": str(ecnvm),
            "sha256": hashlib.sha256(b"ecnvm").hexdigest(),
        }]
        report = {
            "layout": {
                "partitions": {"ecnvm_code": {"logical_base": 0x08200000,
                                               "size_bytes": 0x700000}},
                "devices": [],
            },
        }
        with patch.object(bridge, "_extract_validated_package",
                          return_value=(self.image, [], mapped)), \
             patch.object(bridge, "_validate_manifest",
                          return_value=(report, 0x08000000, 0x08010000,
                                        0x10000, {"erase_bytes": 0x10000})), \
             patch.object(bridge, "_flash_dfu",
                          return_value={"success": True, "message": "dfu-ok"}) as writer, \
             patch.object(bridge, "_dfuse_module",
                          side_effect=AssertionError("the routed writer owns USB access")):
            result = bridge.platform_flash(
                self.using, str(package), "", 0, str(self.root), None)
        self.assertTrue(result["success"], result)
        self.assertIs(writer.call_args.args[-1], mapped)

    def test_self_contained_package_flashes_without_sibling_artifacts(self):
        result, _module, lib = self._run_package_flash()
        self.assertTrue(result["success"], result)
        self.assertTrue(lib.closed)
        self.assertEqual(self.flash.erases, [0x08000000])
        self.assertEqual(self.flash.read(0x08000000, 8), b"firmware")

    def test_built_package_with_external_payload_uses_swd_ospi_then_cnvm_model(self):
        """Exercise the saved package through real validation and mocked SWD boundaries."""
        repo = Path(__file__).resolve().parents[3]
        app = repo / "examples/system/gd32h77x_bringup"
        params_file = app / ".build/build/build_param.json"
        bin_dir = app / ".build/bin"
        package_source = bin_dir / "gd32h77x_bringup_QIO_1.0.0.tufw"
        if not all(path.is_file() for path in (params_file, package_source)):
            self.skipTest("saved automatic .tufw build artifact is unavailable")

        with zipfile.ZipFile(package_source) as package:
            package_manifest = json.loads(package.read("manifest.json"))
        if not package_manifest.get("external_payloads"):
            self.skipTest("saved .tufw is internal-only; external OSPI payload fixture is unavailable")

        params = json.loads(params_file.read_text(encoding="utf-8"))
        using = {
            "CONFIG_PLATFORM_CHOICE": "GD32H77X",
            "CONFIG_BOARD_CHOICE": params["PLATFORM_BOARD"],
            "CONFIG_CHIP_CHOICE": params["CONFIG_CHIP_CHOICE"],
            "CONFIG_PROJECT_NAME": params["CONFIG_PROJECT_NAME"],
            "CONFIG_PROJECT_VERSION": params["CONFIG_PROJECT_VERSION"],
        }
        package_dir = self.root / "package-only"
        package_dir.mkdir()
        package_only = package_dir / package_source.name
        shutil.copy2(package_source, package_only)
        # The temp directory contains only the package; BIN/ELF/report are not
        # available to the bridge and cannot accidentally satisfy validation.
        self.assertEqual([path.name for path in package_only.parent.iterdir()],
                         [package_only.name])

        resolver_path = Path(bridge.__file__).resolve().parent / "gd32h77x_os/tools/firmware/resolve_memory_layout.py"
        resolver_spec = importlib.util.spec_from_file_location(
            "gd32h77x_bridge_package_test_resolver", resolver_path)
        resolver = importlib.util.module_from_spec(resolver_spec)
        resolver_spec.loader.exec_module(resolver)
        current = resolver.resolve_build_layout(
            params, platform_root=Path(bridge.__file__).resolve().parent, boards_root=repo / "boards")
        board_config = bridge._board_flash_config(using, str(repo / "boards"))
        board_config["CONFIG_FLASH_BACKEND"] = "swd"
        calls = []

        class OspiModel:
            @staticmethod
            def preflight_ospi(layout, payloads, pack_path, probe_id):
                calls.append(("ospi_preflight", layout, payloads, pack_path, probe_id))
                return {"probe_id": "cmsis-dap:fixture"}

            @staticmethod
            def flash_ospi(plan, probe_id, verify=True):
                calls.append(("ospi_flash", plan, probe_id, verify))
                return {"success": True, "verified": True, "probe_id": probe_id,
                        "partitions": [{"partition": "staging"}]}

        def flash_cnvm(config, report, image, port, start, limit, capacity, logger):
            calls.append(("cnvm", config.get("CONFIG_FLASH_SWD_PROBE"), start, limit, capacity))
            return {"success": True, "message": "modeled CNVM readback complete"}

        with patch.object(bridge, "_OSPI_MODULE", OspiModel), \
             patch.object(bridge, "_preflight_swd_external",
                          return_value=("/fixture/validated.pack", "cmsis-dap:fixture")), \
             patch.object(bridge, "_flash_swd", side_effect=flash_cnvm), \
             patch.object(bridge, "_resolve_current_layout", return_value=current), \
             patch.object(bridge, "_board_flash_config", return_value=board_config):
            result = bridge.platform_flash(
                using, str(package_only), "", 0, str(repo / "boards"), None)

        self.assertTrue(result["success"], result)
        self.assertIn("OSPI and CNVM payloads read back and verified", result["message"])
        self.assertEqual([call[0] for call in calls],
                         ["ospi_preflight", "ospi_flash", "cnvm"])
        self.assertEqual(calls[1][2:], ("cmsis-dap:fixture", True))
        self.assertEqual(calls[2][1], "cmsis-dap:fixture")
        self.assertEqual(self.flash.erases, [])

    def test_external_package_real_validation_builds_preflight_plan_without_flash_io(self):
        """Validate the real package and OSPI route while mocking only SWD target access."""
        repo = Path(__file__).resolve().parents[3]
        platform_root = Path(bridge.__file__).resolve().parent
        app = repo / "examples/system/gd32h77x_bringup"
        params_file = app / ".build/build/build_param.json"
        artifact_dir = app / ".build/bin"
        report_file = artifact_dir / "gd32h77x_bringup_1.0.0.layout.json"
        elf_file = artifact_dir / "gd32h77x_bringup_1.0.0.elf"
        bin_file = artifact_dir / "gd32h77x_bringup_QIO_1.0.0.bin"
        if not all(path.is_file() for path in (params_file, report_file, elf_file, bin_file)):
            self.skipTest("saved GinoPi build artifacts are unavailable")

        resolver_path = platform_root / "gd32h77x_os/tools/firmware/resolve_memory_layout.py"
        resolver_spec = importlib.util.spec_from_file_location(
            "gd32h77x_bridge_ospi_test_resolver", resolver_path)
        resolver = importlib.util.module_from_spec(resolver_spec)
        resolver_spec.loader.exec_module(resolver)
        params = json.loads(params_file.read_text(encoding="utf-8"))
        current = resolver.resolve_build_layout(
            params, platform_root=platform_root, boards_root=repo / "boards")
        package_module = bridge._flash_package_module()
        ospi = bridge._ospi_module()
        with tempfile.TemporaryDirectory(prefix="gd32h77x-ospi-package-preflight-") as temp:
            root = Path(temp)
            payload_source = root / "staging.bin"
            payload_source.write_bytes(b"selected staged payload")
            payload_csv = root / "flash_payloads.csv"
            payload_csv.write_text("partition,file\nstaging,staging.bin\n", encoding="utf-8")
            package_path = root / "firmware.tufw"
            package_module.create_package(package_path, report_file, elf_file, bin_file,
                                          current, payload_csv)
            extraction = root / "extract"
            extracted = package_module.validate_package(
                package_path, current, expected_board=params["PLATFORM_BOARD"],
                expected_chip=params["CONFIG_CHIP_CHOICE"], extract_dir=extraction)

            class TargetContext:
                probe_id = "cmsis-dap:exact-probe-id"

                def __enter__(self):
                    return object()

                def __exit__(self, *_args):
                    return False

            approved_pack = root / "approved.pack"
            approved_pack.write_bytes(b"pack access is mocked at the target boundary")
            with patch.object(ospi, "_open_target", return_value=TargetContext()), \
                 patch.object(ospi, "_launch_loader"), \
                 patch.object(ospi, "_initialize_loader",
                              return_value={"jedec_id": ospi.GD25Q64E_JEDEC,
                                            "status1": 0, "status2": 0}):
                plan = ospi.preflight_ospi(
                    current, extracted["external_payloads"], str(approved_pack),
                    "cmsis-dap:exact")
            try:
                self.assertEqual(plan.probe_id, "cmsis-dap:exact-probe-id")
                self.assertEqual(plan.target_chip, current["layout"]["target_chip"])
                self.assertEqual(len(plan.payloads), 1)
                payload = plan.payloads[0]
                self.assertEqual(payload["partition"], "staging")
                self.assertEqual(payload["size_bytes"], 4096)
                self.assertEqual(payload["spans"][0]["device_offset_bytes"], 0)
                self.assertEqual(payload["spans"][0]["erase_start_bytes"], 0)
                self.assertEqual(payload["spans"][0]["erase_end_bytes"], 4096)
                self.assertEqual(payload["path"], extracted["external_payloads"][0]["path"])
            finally:
                ospi.cleanup_plan(plan)

    def test_package_with_external_payload_rejects_dfu_before_opening_usb(self):
        external = [{"partition": "staging", "explicit_payload": True,
                     "segments": [{"device": "renamed_nor", "region": "NOR",
                                   "backend_status": "available"}]}]
        package = self.root / "firmware_QIO_1.0.0.tufw"
        package.write_bytes(b"self contained package")
        self.image.unlink(missing_ok=True)
        self.elf.unlink(missing_ok=True)
        (self.root / "firmware_1.0.0.layout.json").unlink(missing_ok=True)
        with patch.object(bridge, "_FLASH_PACKAGE_MODULE",
                          self._mock_package_module(external)), \
             patch.object(bridge, "_dfuse_module",
                          side_effect=AssertionError("USB must not be opened")), \
             patch.object(bridge, "_resolve_current_layout",
                          return_value=copy.deepcopy(self.current_layout)), \
             patch.object(bridge, "_verify_build_artifact"):
            result = bridge.platform_flash(
                self.using, str(package), "", 0, str(self.root), None)
        self.assertFalse(result["success"])
        self.assertIn("require the swd or uart backend", result["message"])
        self.assertEqual(self.flash.erases, [])

    def test_ospi_failure_does_not_touch_cnvm(self):
        self._write_board_config("CONFIG_FLASH_BACKEND=swd\n")
        external = [{"partition": "staging", "explicit_payload": True,
                     "segments": [{"device": "renamed_nor", "region": "NOR",
                                   "backend_status": "available"}]}]
        package = self.root / "firmware_QIO_1.0.0.tufw"
        package.write_bytes(b"self contained package")
        self.image.unlink(missing_ok=True)
        self.elf.unlink(missing_ok=True)
        (self.root / "firmware_1.0.0.layout.json").unlink(missing_ok=True)

        class _Ospi:
            @staticmethod
            def preflight_ospi(manifest, payloads, pack_path, probe_id):
                return {"probe_id": "probe-1"}

            @staticmethod
            def flash_ospi(plan, probe_id, verify=True):
                raise RuntimeError("simulated NOR write/readback failure")

        with patch.object(bridge, "_FLASH_PACKAGE_MODULE",
                          self._mock_package_module(external)), \
             patch.object(bridge, "_OSPI_MODULE", _Ospi), \
             patch.object(bridge, "_preflight_swd_external",
                          return_value=("/trusted/pack.pack", "probe-1")), \
             patch.object(bridge, "_flash_swd",
                          side_effect=AssertionError("CNVM must not be touched")), \
             patch.object(bridge, "_resolve_current_layout",
                          return_value=copy.deepcopy(self.current_layout)), \
             patch.object(bridge, "_verify_build_artifact"):
            result = bridge.platform_flash(
                self.using, str(package), "", 0, str(self.root), None)
        self.assertFalse(result["success"])
        self.assertIn("simulated NOR write/readback failure", result["message"])
        self.assertEqual(self.flash.erases, [])

    def test_ospi_unverified_success_does_not_start_cnvm(self):
        self._write_board_config("CONFIG_FLASH_BACKEND=swd\nCONFIG_FLASH_VERIFY=y\n")
        external = [{"partition": "staging", "explicit_payload": True,
                     "segments": [{"device": "renamed_nor", "region": "NOR",
                                   "backend_status": "available"}]}]
        package = self.root / "firmware_QIO_1.0.0.tufw"
        package.write_bytes(b"self contained package")
        self.image.unlink(missing_ok=True)
        self.elf.unlink(missing_ok=True)
        (self.root / "firmware_1.0.0.layout.json").unlink(missing_ok=True)

        class _Ospi:
            @staticmethod
            def preflight_ospi(manifest, payloads, pack_path, probe_id):
                return {"probe_id": "probe-1"}

            @staticmethod
            def flash_ospi(plan, probe_id, verify=True):
                return {"success": True, "verified": False,
                        "probe_id": probe_id, "partitions": [{"partition": "staging"}]}

        with patch.object(bridge, "_FLASH_PACKAGE_MODULE",
                          self._mock_package_module(external)), \
             patch.object(bridge, "_OSPI_MODULE", _Ospi), \
             patch.object(bridge, "_preflight_swd_external",
                          return_value=("/trusted/pack.pack", "probe-1")), \
             patch.object(bridge, "_flash_swd",
                          side_effect=AssertionError("CNVM must not be touched")), \
             patch.object(bridge, "_resolve_current_layout",
                          return_value=copy.deepcopy(self.current_layout)), \
             patch.object(bridge, "_verify_build_artifact"):
            result = bridge.platform_flash(
                self.using, str(package), "", 0, str(self.root), None)
        self.assertFalse(result["success"])
        self.assertIn("did not verify readback", result["message"])
        self.assertEqual(self.flash.erases, [])

    def test_ospi_plan_probe_mismatch_stops_before_external_or_cnvm_write(self):
        class _Ospi:
            pass

        external_writer = unittest.mock.Mock()
        cnvm_writer = unittest.mock.Mock()
        with self.assertRaisesRegex(RuntimeError, "different SWD probe"):
            bridge._execute_ospi_then_cnvm(
                {"CONFIG_FLASH_VERIFY": "y"}, self.report, self.image, [], "",
                0x08000000, 0x080E0000, 0xE0000, None, _Ospi,
                external_writer, {"probe_id": "other-probe"}, "/pack/validated.pack",
                "selected-probe")
        external_writer.assert_not_called()
        cnvm_writer.assert_not_called()

    def test_ospi_success_then_cnvm_reuses_the_exact_probe_and_verify_setting(self):
        self._write_board_config(
            "CONFIG_FLASH_BACKEND=swd\nCONFIG_FLASH_VERIFY=n\n"
            )
        external = [{"partition": "staging", "explicit_payload": True,
                     "segments": [{"device": "renamed_nor", "region": "NOR",
                                   "backend_status": "available"}]}]
        package = self.root / "firmware_QIO_1.0.0.tufw"
        package.write_bytes(b"self contained package")
        self.image.unlink(missing_ok=True)
        self.elf.unlink(missing_ok=True)
        (self.root / "firmware_1.0.0.layout.json").unlink(missing_ok=True)
        calls = []

        class _Ospi:
            @staticmethod
            def preflight_ospi(manifest, payloads, pack_path, probe_id):
                calls.append(("preflight", probe_id))
                return {"probe_id": "cmsis-dap:abcd1234"}

            @staticmethod
            def flash_ospi(plan, probe_id, verify=True):
                calls.append(("ospi", probe_id, verify))
                return {"success": True, "verified": False, "probe_id": probe_id,
                        "partitions": [{"partition": "staging"}]}

        def fake_cnvm(config, report, image, port, start, limit, capacity, logger):
            calls.append(("cnvm", config["CONFIG_FLASH_SWD_PROBE"], port))
            return {"success": True, "message": "verification skipped"}

        with patch.object(bridge, "_FLASH_PACKAGE_MODULE",
                          self._mock_package_module(external)), \
             patch.object(bridge, "_OSPI_MODULE", _Ospi), \
             patch.object(bridge, "_preflight_swd_external",
                          return_value=("/trusted/pack.pack", "cmsis-dap:abcd1234")), \
             patch.object(bridge, "_flash_swd", side_effect=fake_cnvm), \
             patch.object(bridge, "_resolve_current_layout",
                          return_value=copy.deepcopy(self.current_layout)), \
             patch.object(bridge, "_verify_build_artifact"):
            result = bridge.platform_flash(
                self.using, str(package), "", 0, str(self.root), None,
                verify=False)
        self.assertTrue(result["success"], result)
        self.assertEqual(calls, [
            ("preflight", "cmsis-dap:abcd1234"),
            ("ospi", "cmsis-dap:abcd1234", False),
            ("cnvm", "", "cmsis-dap:abcd1234"),
        ])
        self.assertIn("verification disabled", result["message"])

    def test_native_dfu_erases_every_sector_covered_by_the_padded_image(self):
        self.image.write_bytes(b"A" * (0x10000 + 5))
        self._write_report()
        result, _devices, _lib = self._run_flash()
        self.assertTrue(result["success"], result)
        self.assertEqual(self.flash.erases, [0x08000000, 0x08010000])
        self.assertEqual(self.flash.read(0x08010000, 5), b"AAAAA")
        self.assertEqual(self.flash.read(0x08010005, 4), b"\xFF" * 4)

    def test_native_dfu_verifies_readback(self):
        devices = [FakeUsbDevice(self.flash, "3-3.2", "DIWd")]
        module, _lib = _fake_dfuse(devices)

        original_read = FakeDfuSeProtocol.read

        def corrupt(self, address, size, progress=None):
            data = bytearray(original_read(self, address, size, progress))
            if data:
                data[3] ^= 0xFF
            return bytes(data)

        with patch.object(bridge, "_DFUSE_MODULE", module), \
             patch.object(bridge, "_resolve_current_layout",
                          return_value=copy.deepcopy(self.current_layout)), \
             patch.object(bridge, "_verify_build_artifact"), \
             patch.object(FakeDfuSeProtocol, "read", corrupt):
            result = bridge.platform_flash(
                self.using, str(self.image), "", 0, str(self.root), None)
        self.assertFalse(result["success"])
        self.assertIn("readback mismatch at 0x08000003", result["message"])

    def test_native_dfu_reports_when_verification_is_disabled(self):
        with patch.object(FakeDfuSeProtocol, "read",
                          side_effect=AssertionError("unexpected read")):
            result, _devices, _lib = self._run_flash(verify=False)
        self.assertTrue(result["success"], result)
        self.assertIn("verification disabled", result["message"])

    def test_native_dfu_selects_device_by_usb_path(self):
        devices = [FakeUsbDevice(self.flash, "3-3.2", "DIWd"),
                   FakeUsbDevice(FakeFlash(0x08000000, 0x10000, 0x200000), "3-3.3", "DIWd")]
        result, _devices, _lib = self._run_flash(devices, usb_path="3-3.3")
        self.assertTrue(result["success"], result)
        self.assertEqual(devices[0].flash.erases, [])
        self.assertEqual(devices[1].flash.erases, [0x08000000])

    def test_native_dfu_non_interactive_multiple_devices_requires_path(self):
        devices = [FakeUsbDevice(self.flash, "3-3.2", "DIWd"),
                   FakeUsbDevice(self.flash, "3-3.3", "DIWd")]
        result, _devices, _lib = self._run_flash(devices, tty=False)
        self.assertFalse(result["success"])
        self.assertIn("non-interactive mode", result["message"])
        self.assertEqual(self.flash.erases, [])

    def test_native_dfu_rejects_runtime_interface(self):
        self._write_board_config("CONFIG_FLASH_BACKEND=dfu\n")
        devices = [FakeUsbDevice(self.flash, "3-3.2", "DIWd")]
        devices[0].interfaces = lambda: [{
            "interface": 0, "alt": 0, "protocol": 0, "name": "@InternalFlash /0x08000000/"
        }]
        result, _devices, _lib = self._run_flash(devices)
        self.assertFalse(result["success"])
        self.assertEqual(self.flash.erases, [])

    def test_native_dfu_rejects_non_cnvm_alternate(self):
        interfaces = [
            {"interface": 0, "alt": 0, "protocol": 2, "name": "@ExternalFlash /0x90000000/",
             "bcd_dfu_version": 0x011A, "bm_attributes": 0x07,
             "transfer_size": 64},
        ]
        with self.assertRaisesRegex(RuntimeError, "not the configured internal CNVM"):
            bridge._select_dfu_interface(interfaces, 0, "ExternalFlash")

    def test_native_dfu_accepts_normalized_internal_flash_label(self):
        interface = bridge._select_dfu_interface([{
            "interface": 0, "alt": 0, "protocol": 2,
            "name": "@Internal Flash /0x08000000/32*064Kg",
            "bcd_dfu_version": 0x011A, "bm_attributes": 0x07,
            "transfer_size": 64,
        }], 0, "InternalFlash")
        self.assertEqual(interface["alt"], 0)

    def test_native_dfu_rejects_missing_download_or_upload_capability(self):
        base = {"interface": 0, "alt": 0, "protocol": 2,
                "name": "@InternalFlash /0x08000000/32*064Kg",
                "bcd_dfu_version": 0x011A, "bm_attributes": 0x01,
                "transfer_size": 64}
        with self.assertRaisesRegex(RuntimeError, "not the configured internal CNVM"):
            bridge._select_dfu_interface([base], 0, "InternalFlash", require_upload=True)
        self.assertEqual(bridge._select_dfu_interface(
            [base], 0, "InternalFlash", require_upload=False), base)

    def test_native_dfu_rejects_unsupported_dfu_version(self):
        entry = {"interface": 0, "alt": 0, "protocol": 2,
                 "name": "@InternalFlash /0x08000000/32*064Kg",
                 "bcd_dfu_version": 0x0100, "bm_attributes": 0x07,
                 "transfer_size": 64}
        with self.assertRaisesRegex(RuntimeError, "not the configured internal CNVM"):
            bridge._select_dfu_interface([entry], 0, "InternalFlash")

    def test_native_dfu_selector_accepts_usb_path_or_serial(self):
        result, _devices, _lib = self._run_flash(usb_path="wrong")
        self.assertFalse(result["success"])
        self.assertIn("configured USB path or serial", result["message"])
        self.assertEqual(self.flash.erases, [])

    def test_native_dfu_prompts_for_multiple_devices(self):
        second = FakeFlash(0x08000000, 0x10000, 0x200000)
        devices = [FakeUsbDevice(self.flash, "3-3.2", "DIWd"),
                   FakeUsbDevice(second, "3-3.3", "DIWd")]
        result, _devices, _lib = self._run_flash(devices, tty=True, inputs=("2",))
        self.assertTrue(result["success"], result)
        self.assertEqual(second.erases, [0x08000000])
        self.assertEqual(self.flash.erases, [])

    def test_native_dfu_leave_is_fixed_off(self):
        left = {}

        class LeaveProtocol(FakeDfuSeProtocol):
            def leave(self):
                left["called"] = True

        module, _lib = _fake_dfuse([FakeUsbDevice(self.flash, "3-3.2", "DIWd")])
        module.DfuSeProtocol = LeaveProtocol
        with patch.object(bridge, "_DFUSE_MODULE", module), \
             patch.object(bridge, "_resolve_current_layout",
                          return_value=copy.deepcopy(self.current_layout)), \
             patch.object(bridge, "_verify_build_artifact"):
            result = bridge.platform_flash(
                self.using, str(self.image), "", 0, str(self.root), None)
        self.assertTrue(result["success"], result)
        self.assertFalse(left.get("called", False))

    # -- geometry validation ------------------------------------------------
    def test_manifest_rejects_unaligned_erase_geometry(self):
        self.current_layout["layout"]["devices"][0]["erase_bytes"] = 0x30000
        self._write_report()
        with patch.object(bridge, "_resolve_current_layout",
                          return_value=copy.deepcopy(self.current_layout)), \
             patch.object(bridge, "_verify_build_artifact"):
            with self.assertRaisesRegex(ValueError, "not aligned to the 196608 byte erase unit"):
                bridge._validate_manifest(self.image, str(self.root), self.using)

    def test_manifest_rejects_range_outside_device_geometry(self):
        self.current_layout["layout"]["devices"][0]["capacity_bytes"] = 0x10000
        self._write_report()
        with patch.object(bridge, "_resolve_current_layout",
                          return_value=copy.deepcopy(self.current_layout)), \
             patch.object(bridge, "_verify_build_artifact"):
            with self.assertRaisesRegex(ValueError, "exceeds the device geometry"):
                bridge._validate_manifest(self.image, str(self.root), self.using)

    def test_rejects_image_larger_than_application_range(self):
        self.image.write_bytes(b"x" * (0x000E0000 + 1))
        self._write_report()
        result, _devices, _lib = self._run_flash()
        self.assertFalse(result["success"])
        self.assertIn("application range", result["message"])

    def test_rejects_stale_layout_source(self):
        self.current_layout["source"]["sourceDigest"] = "changed"
        with patch.object(bridge, "_resolve_current_layout",
                          return_value=copy.deepcopy(self.current_layout)), \
             patch.object(bridge, "_verify_build_artifact"):
            with self.assertRaisesRegex(ValueError, "different GD32H77X flash layout"):
                bridge._validate_manifest(self.image, str(self.root), self.using)

    def test_rejects_tampered_binary_or_report_allowwrite(self):
        self.image.write_bytes(b"changed")
        with patch.object(bridge, "_resolve_current_layout",
                          return_value=copy.deepcopy(self.current_layout)), \
             patch.object(bridge, "_verify_build_artifact"):
            with self.assertRaisesRegex(ValueError, "BIN digest"):
                bridge._validate_manifest(self.image, str(self.root), self.using)
        self.image.write_bytes(b"firmware")
        self._write_report()
        report_path = self.root / "firmware_1.0.0.layout.json"
        report = json.loads(report_path.read_text())
        report["allowwrite"][1]["partition"] = "ota_staging"
        report_path.write_text(json.dumps(report))
        with patch.object(bridge, "_resolve_current_layout",
                          return_value=copy.deepcopy(self.current_layout)), \
             patch.object(bridge, "_verify_build_artifact"):
            with self.assertRaisesRegex(ValueError, "only write internal boot and primary"):
                bridge._validate_manifest(self.image, str(self.root), self.using)

    # -- backend selection --------------------------------------------------
    def test_rejects_unknown_backend(self):
        with self.assertRaisesRegex(ValueError, "Unsupported flash backend"):
            bridge._backend({"CONFIG_FLASH_BACKEND": "openocd"})

    def test_baud_is_rejected_for_non_uart_backends(self):
        for backend in ("dfu", "swd"):
            self._write_board_config(f"CONFIG_FLASH_BACKEND={backend}\n")
            result = bridge.platform_flash(self.using, str(self.image), "", 115200,
                                           str(self.root), None)
            self.assertFalse(result["success"], backend)
            self.assertIn("only valid with the uart backend", result["message"])
            self.assertIn(backend, result["message"])

    def test_uart_backend_accepts_baud_without_swd(self):
        self._write_board_config("CONFIG_FLASH_BACKEND=uart\n")
        calls = []
        with patch.object(bridge, "_UART_ISP_MODULE",
                          _fake_uart_module(calls, self.image)), \
             patch.object(bridge, "_resolve_current_layout",
                          return_value=copy.deepcopy(self.current_layout)), \
             patch.object(bridge, "_verify_build_artifact"), \
             patch.object(bridge, "_dfuse_module",
                          side_effect=AssertionError("USB must not be opened")), \
             patch.object(bridge.importlib.metadata, "version",
                          side_effect=AssertionError("pyOCD must not be queried")):
            result = bridge.platform_flash(
                self.using, str(self.image), "COM42", 921600, str(self.root), None)
        self.assertTrue(result["success"], result)
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]["port"], "COM42")
        self.assertEqual(calls[0]["baud"], 921600)
        self.assertEqual(calls[0]["start"], 0x08000000)
        self.assertEqual(calls[0]["capacity"], 0xE0000)
        self.assertEqual(calls[0]["device_geometry"]["erase_bytes"], 0x10000)
        self.assertEqual(calls[0]["external_payloads"], [])
        self.assertIn("921600 8E1", result["message"])
        self.assertIn("CNVM", result["message"])

    def test_uart_backend_reads_port_baud_ospi_and_verify_from_config(self):
        self._write_board_config("CONFIG_FLASH_BACKEND=uart\n")
        calls = []
        with patch.object(bridge, "_UART_ISP_MODULE",
                          _fake_uart_module(calls, self.image)), \
             patch.object(bridge, "_resolve_current_layout",
                          return_value=copy.deepcopy(self.current_layout)), \
             patch.object(bridge, "_verify_build_artifact"), \
             patch("serial.tools.list_ports.comports",
                   side_effect=AssertionError("configured port skips enumeration")):
            result = bridge.platform_flash(
                self.using, str(self.image), "/dev/ttyUSB7", 230400,
                str(self.root), None, verify=False)
        self.assertTrue(result["success"], result)
        self.assertEqual(calls[0]["port"], "/dev/ttyUSB7")
        self.assertEqual(calls[0]["baud"], 230400)
        self.assertIsNone(calls[0]["ospi_profiles"])
        self.assertEqual(calls[0]["expected_id"], bytes.fromhex("44495764FFFFFFFF"))
        self.assertEqual(calls[0]["expected_chip"], "GD32H77DIW")
        self.assertFalse(calls[0]["verify"])
        self.assertIn("verification disabled", result["message"])

    def test_uart_backend_defaults_to_auto_baud(self):
        self._write_board_config("CONFIG_FLASH_BACKEND=uart\n")
        calls = []
        with patch.object(bridge, "_UART_ISP_MODULE",
                          _fake_uart_module(calls, self.image)), \
             patch.object(bridge, "_resolve_current_layout",
                          return_value=copy.deepcopy(self.current_layout)), \
             patch.object(bridge, "_verify_build_artifact"):
            result = bridge.platform_flash(
                self.using, str(self.image), "/dev/ttyUSB8", 0,
                str(self.root), None)
        self.assertTrue(result["success"], result)
        self.assertEqual(calls[0]["baud"], "auto")
        self.assertIn("115200 8E1", result["message"])

    def test_uart_no_verify_option(self):
        self._write_board_config("CONFIG_FLASH_BACKEND=uart\n")
        calls = []
        with patch.object(bridge, "_UART_ISP_MODULE",
                          _fake_uart_module(calls, self.image)), \
             patch.object(bridge, "_resolve_current_layout",
                          return_value=copy.deepcopy(self.current_layout)), \
             patch.object(bridge, "_verify_build_artifact"):
            result = bridge.platform_flash(
                self.using, str(self.image), "/dev/ttyUSB8", 0,
                str(self.root), None, verify=False)
        self.assertTrue(result["success"], result)
        self.assertFalse(calls[0]["verify"])
        self.assertIn("readback verification disabled", result["message"])

    def test_uart_backend_requires_port(self):
        self._write_board_config("CONFIG_FLASH_BACKEND=uart\n")
        with patch.object(bridge, "_resolve_current_layout",
                          return_value=copy.deepcopy(self.current_layout)), \
             patch.object(bridge, "_verify_build_artifact"), \
             patch("serial.tools.list_ports.comports", return_value=[]):
            result = bridge.platform_flash(self.using, str(self.image), "", 0,
                                           str(self.root), None)
        self.assertFalse(result["success"])
        self.assertIn("No serial ports found", result["message"])

    def test_uart_missing_expected_id_fails_before_port_enumeration(self):
        self._write_board_config("CONFIG_FLASH_BACKEND=uart\n")
        with patch.object(bridge, "_uart_expected_id", return_value=""), \
             patch.object(bridge, "_resolve_current_layout",
                          return_value=copy.deepcopy(self.current_layout)), \
             patch.object(bridge, "_verify_build_artifact"), \
             patch("serial.tools.list_ports.comports",
                   side_effect=AssertionError("must validate identity first")):
            result = bridge.platform_flash(self.using, str(self.image), "/dev/ttyUSB8", 0,
                                           str(self.root), None)
        self.assertFalse(result["success"])
        self.assertIn("uart_isp.json", result["message"])

    def test_uart_multiple_ports_requires_explicit_port_without_tty(self):
        self._write_board_config("CONFIG_FLASH_BACKEND=uart\n")
        ports = [SimpleNamespace(device="/dev/ttyA", description="A"),
                 SimpleNamespace(device="/dev/ttyB", description="B")]
        with patch.object(bridge, "_resolve_current_layout",
                          return_value=copy.deepcopy(self.current_layout)), \
             patch.object(bridge, "_verify_build_artifact"), \
             patch.object(bridge.sys.stdin, "isatty", return_value=False), \
             patch("serial.tools.list_ports.comports", return_value=ports):
            result = bridge.platform_flash(self.using, str(self.image), "", 0,
                                           str(self.root), None)
        self.assertFalse(result["success"])
        self.assertIn("Multiple serial ports", result["message"])
        self.assertIn("/dev/ttyA (A)", result["message"])
        self.assertIn("/dev/ttyB (B)", result["message"])

    def test_uart_auto_selects_single_port(self):
        ports = [SimpleNamespace(device="/dev/ttyOnly", description="Target")]
        with patch("serial.tools.list_ports.comports", return_value=ports):
            result, calls = self._run_uart_direct({
                "_CONFIG_DIR": str(self.board_dir),
                "CONFIG_FLASH_UART_EXPECTED_ID": "123456",
                "CONFIG_FLASH_TARGET_CHIP": "GD32H77DIW",
            })
        self.assertTrue(result["success"], result)
        self.assertEqual(calls[0]["port"], "/dev/ttyOnly")

    def test_uart_multiple_ports_selects_number_in_tty(self):
        ports = [SimpleNamespace(device="/dev/ttyA", description="A"),
                 SimpleNamespace(device="/dev/ttyB", description="B")]
        with patch("serial.tools.list_ports.comports", return_value=ports), \
             patch.object(bridge.sys.stdin, "isatty", return_value=True), \
             patch("builtins.input", return_value="2"):
            result, calls = self._run_uart_direct({
                "_CONFIG_DIR": str(self.board_dir),
                "CONFIG_FLASH_UART_EXPECTED_ID": "123456",
                "CONFIG_FLASH_TARGET_CHIP": "GD32H77DIW",
            })
        self.assertTrue(result["success"], result)
        self.assertEqual(calls[0]["port"], "/dev/ttyB")

    def test_uart_multiple_ports_tty_cancelled(self):
        ports = [SimpleNamespace(device="/dev/ttyA", description="A"),
                 SimpleNamespace(device="/dev/ttyB", description="B")]
        with patch("serial.tools.list_ports.comports", return_value=ports), \
             patch.object(bridge.sys.stdin, "isatty", return_value=True), \
             patch("builtins.input", return_value="q"):
            with self.assertRaisesRegex(ValueError, "selection cancelled"):
                self._run_uart_direct({
                    "_CONFIG_DIR": str(self.board_dir),
                    "CONFIG_FLASH_UART_EXPECTED_ID": "123456",
                    "CONFIG_FLASH_TARGET_CHIP": "GD32H77DIW",
                })

    def test_uart_explicit_port_skips_enumeration(self):
        with patch("serial.tools.list_ports.comports",
                   side_effect=AssertionError("explicit port skips enumeration")):
            result, calls = self._run_uart_direct({
                "_CONFIG_DIR": str(self.board_dir),
                "CONFIG_FLASH_UART_EXPECTED_ID": "123456",
                "CONFIG_FLASH_TARGET_CHIP": "GD32H77DIW",
            }, port="COM7")
        self.assertTrue(result["success"], result)
        self.assertEqual(calls[0]["port"], "COM7")

    def test_uart_ospi_profiles_validate_csv_and_path_before_enumeration(self):
        cases = [
            ("extra header", "device,config,pins,extra\na,010203,0011223344,x\n",
             "header must be exactly"),
            ("duplicate header", "device,config,pins,pins\na,010203,0011223344,0011223344\n",
             "header must be exactly"),
            ("missing cell", "device,config,pins\na,010203\n", "malformed row"),
            ("escaped path", "", "stay within"),
            ("absolute path", "", "must be relative"),
        ]
        for label, csv_text, message in cases:
            with self.subTest(label=label):
                profile_name = "../outside.csv" if label == "escaped path" else (
                    "/tmp/outside.csv" if label == "absolute path" else "profiles.csv")
                if csv_text:
                    (self.board_dir / "profiles.csv").write_text(csv_text, encoding="utf-8")
                config = {
                    "_CONFIG_DIR": str(self.board_dir),
                    "CONFIG_FLASH_UART_EXPECTED_ID": "123456",
                    "CONFIG_FLASH_TARGET_CHIP": "GD32H77DIW",
                    "CONFIG_FLASH_UART_OSPI_PROFILES": profile_name,
                }
                with patch("serial.tools.list_ports.comports",
                           side_effect=AssertionError("profile validation precedes enumeration")):
                    with self.assertRaisesRegex(ValueError, message):
                        self._run_uart_direct(config, external_payloads=[{"partition": "p"}])

    def test_uart_external_package_does_not_touch_swd_or_usb(self):
        profiles = self.board_dir / "ospi_profiles.csv"
        profiles.write_text(
            "device,config,pins\n"
            "renamed_nor,112233,AABBCCDDEE;BBCCDDEEFF\n",
            encoding="utf-8")
        self._write_board_config("CONFIG_FLASH_BACKEND=uart\n")
        external = [{"partition": "staging", "explicit_payload": True,
                     "segments": [{"device": "renamed_nor", "region": "NOR",
                                   "backend_status": "available"}]}]
        package = self.root / "firmware_QIO_1.0.0.tufw"
        package.write_bytes(b"self contained package")
        self.image.unlink(missing_ok=True)
        self.elf.unlink(missing_ok=True)
        (self.root / "firmware_1.0.0.layout.json").unlink(missing_ok=True)
        calls = []
        with patch.object(bridge, "_FLASH_PACKAGE_MODULE",
                          self._mock_package_module(external)), \
             patch.object(bridge, "_UART_ISP_MODULE",
                          _fake_uart_module(calls, self.root / "internal.bin")), \
             patch.object(bridge, "_resolve_current_layout",
                          return_value=copy.deepcopy(self.current_layout)), \
             patch.object(bridge, "_verify_build_artifact"), \
             patch.object(bridge, "_preflight_swd_external",
                          side_effect=AssertionError("SWD preflight must not run")), \
             patch.object(bridge, "_dfuse_module",
                          side_effect=AssertionError("USB must not be opened")):
            result = bridge.platform_flash(
                self.using, str(package), "COM9", 115200, str(self.root), None)
        self.assertTrue(result["success"], result)
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]["port"], "COM9")
        self.assertEqual(calls[0]["baud"], 115200)
        self.assertEqual(len(calls[0]["external_payloads"]), 1)
        self.assertEqual(calls[0]["external_payloads"][0]["partition"], "staging")
        self.assertEqual(calls[0]["manifest"], self.current_layout)
        self.assertEqual(calls[0]["ospi_profiles"]["renamed_nor"]["config"],
                         bytes.fromhex("112233"))
        self.assertIn("OSPI partitions staging and CNVM", result["message"])

    def test_stock_dfu_util_backend_is_disabled(self):
        with self.assertRaisesRegex(ValueError, "erase behavior"):
            bridge._backend({"CONFIG_FLASH_BACKEND": "dfu-util"})

    def test_swd_backend_requires_pyocd(self):
        self._write_board_config("CONFIG_FLASH_BACKEND=swd\n")
        with patch.object(bridge, "_resolve_current_layout",
                          return_value=copy.deepcopy(self.current_layout)), \
             patch.object(bridge, "_verify_build_artifact"), \
             patch.object(bridge.importlib.metadata, "version",
                          side_effect=bridge.importlib.metadata.PackageNotFoundError):
            result = bridge.platform_flash(
                self.using, str(self.image), "", 0, str(self.root), None)
        self.assertFalse(result["success"])
        self.assertIn("needs pyocd", result["message"])

    def test_probe_selection_requires_unique_match_or_explicit_selector(self):
        probes = [{"unique_id": "cmsis-dap:abc", "info": "one"},
                  {"unique_id": "cmsis-dap:def", "info": "two"}]
        with patch.object(bridge.sys.stdin, "isatty", return_value=False):
            with self.assertRaisesRegex(RuntimeError, "non-interactive mode"):
                bridge._select_swd_probe(probes, "")
        self.assertEqual(bridge._select_swd_probe(probes, "cmsis-dap:def"), "cmsis-dap:def")

    def test_external_preflight_resolves_exact_probe_with_existing_tty_rules(self):
        identity = {"canonical_chip": "GD32H77DIW", "cnvm_capacity_bytes": 0x200000,
                    "cnvm_erase_bytes": 0x10000, "cnvm_program_bytes": 32,
                    "cnvm_vendor_algorithm": "GD32H77x_78x_CNVM_2M.FLM"}
        resolver = SimpleNamespace(resolve_chip_identity=lambda chip, root: identity)
        loader = SimpleNamespace(exec_module=lambda module: None)
        fake_spec = SimpleNamespace(loader=loader)
        outputs = [
            (0, json.dumps({"targets": [{"part_number": "GD32H77DIW",
                                          "vendor": "GigaDevice", "source": "pack"}]})),
            (0, json.dumps({"status": 0, "boards": [
                {"unique_id": "cmsis-dap:one", "info": "first"},
                {"unique_id": "cmsis-dap:two", "info": "second"}]})),
        ]
        with patch.object(bridge.importlib.metadata, "version", return_value="0.46.0"), \
             patch.object(bridge.importlib.util, "spec_from_file_location", return_value=fake_spec), \
             patch.object(bridge.importlib.util, "module_from_spec", return_value=resolver), \
             patch.object(bridge, "_validate_swd_pack", return_value={}), \
             patch.object(bridge, "_run_pyocd", side_effect=outputs) as run_pyocd, \
             patch.object(bridge.sys.stdin, "isatty", return_value=True), \
             patch("builtins.input", return_value="2"):
            pack, probe = bridge._preflight_swd_external(
                {"CONFIG_FLASH_SWD_PACK": "/tmp/approved.pack"},
                {"chip": "GD32H77D", "layout": {"target_chip": "GD32H77DIW",
                                                   "flash_image": {"start_address": 0x08000000,
                                                                   "end_address": 0x080E0000,
                                                                   "capacity_bytes": 0xE0000}}},
                self.image, "", 0x08000000, 0x080E0000, 0xE0000,
                {"capacity_bytes": 0x200000, "erase_bytes": 0x10000,
                 "program_bytes": 32, "vendor_algorithm": "GD32H77x_78x_CNVM_2M.FLM"},
                None)
        self.assertEqual(pack, "/tmp/approved.pack")
        self.assertEqual(probe, "cmsis-dap:two")
        self.assertIn("--probes", run_pyocd.call_args_list[1].args[0])

    def test_external_preflight_rejects_ambiguous_probes_without_tty(self):
        identity = {"canonical_chip": "GD32H77DIW", "cnvm_capacity_bytes": 0x200000,
                    "cnvm_erase_bytes": 0x10000, "cnvm_program_bytes": 32,
                    "cnvm_vendor_algorithm": "GD32H77x_78x_CNVM_2M.FLM"}
        resolver = SimpleNamespace(resolve_chip_identity=lambda chip, root: identity)
        fake_spec = SimpleNamespace(loader=SimpleNamespace(exec_module=lambda module: None))
        outputs = [
            (0, json.dumps({"targets": [{"part_number": "GD32H77DIW",
                                          "vendor": "GigaDevice", "source": "pack"}]})),
            (0, json.dumps({"status": 0, "boards": [
                {"unique_id": "cmsis-dap:one"}, {"unique_id": "cmsis-dap:two"}]})),
        ]
        with patch.object(bridge.importlib.metadata, "version", return_value="0.46.0"), \
             patch.object(bridge.importlib.util, "spec_from_file_location", return_value=fake_spec), \
             patch.object(bridge.importlib.util, "module_from_spec", return_value=resolver), \
             patch.object(bridge, "_validate_swd_pack", return_value={}), \
             patch.object(bridge, "_run_pyocd", side_effect=outputs), \
             patch.object(bridge.sys.stdin, "isatty", return_value=False):
            with self.assertRaisesRegex(RuntimeError, "non-interactive mode"):
                bridge._preflight_swd_external(
                    {"CONFIG_FLASH_SWD_PACK": "/tmp/approved.pack"},
                    {"chip": "GD32H77D", "layout": {"target_chip": "GD32H77DIW",
                                                       "flash_image": {"start_address": 0x08000000,
                                                                       "end_address": 0x080E0000,
                                                                       "capacity_bytes": 0xE0000}}},
                    self.image, "", 0x08000000, 0x080E0000, 0xE0000,
                    {"capacity_bytes": 0x200000, "erase_bytes": 0x10000,
                     "program_bytes": 32, "vendor_algorithm": "GD32H77x_78x_CNVM_2M.FLM"},
                    None)

    def test_platform_device_registry_maps_order_code(self):
        entry = bridge._load_platform_device("GD32H77D")
        self.assertEqual(entry["target_chip"], "GD32H77DIW")
        self.assertEqual(entry["order_code"], "GD32H77DIWK7")


if __name__ == "__main__":
    unittest.main()
