"""Host checks for the GD32H77X SRAM/GD25Q64E writer."""
from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
PLATFORM = ROOT / "platform/GD32H77X"
TEST_BOARD = "GD32H77D_TEST_BOARD"
FIXTURE_BOARDS = PLATFORM / "tests" / "fixtures" / "boards"
SCRIPT = PLATFORM / "gd32h77x_os/tools/download/gd32h77x_ospi.py"
SPEC = importlib.util.spec_from_file_location("gd32h77x_ospi_test_module", SCRIPT)
ospi = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = ospi
SPEC.loader.exec_module(ospi)

RESOLVER_SPEC = importlib.util.spec_from_file_location(
    "gd32h77x_resolver_ospi_test", PLATFORM / "gd32h77x_os/tools/firmware/resolve_memory_layout.py")
resolver = importlib.util.module_from_spec(RESOLVER_SPEC)
sys.modules[RESOLVER_SPEC.name] = resolver
RESOLVER_SPEC.loader.exec_module(resolver)


class _FakeContext:
    probe_id = "cmsis-dap:probe-1"

    def __enter__(self):
        return object()

    def __exit__(self, *_args):
        return False


class _FakeMailboxTarget:
    def __init__(self):
        self.words = [0] * 11
        self.buffer = bytearray(256)
        self.requests = []

    def read_memory_block32(self, _address, count):
        return self.words[:count]

    def write_memory_block32(self, address, values):
        if address == ospi.MAILBOX_ADDRESS + 4:
            self.words[1:1 + len(values)] = values
        elif address == ospi.BUFFER_ADDRESS:
            raw = b"".join(int(value).to_bytes(4, "little") for value in values)
            self.buffer[:len(raw)] = raw
        else:
            raise AssertionError(hex(address))

    def write_memory(self, address, value, _size):
        assert address == ospi.MAILBOX_ADDRESS + 4
        self.words[1] = value

    def read_memory(self, _address, _size):
        return ospi.MAILBOX_MAGIC

    def read_memory_block8(self, _address, count):
        return self.buffer[:count]

    def halt(self):
        pass

    def resume(self):
        self.requests.append((self.words[3], self.words[4], self.words[5]))
        self.words[1] = 0
        self.words[6] = ospi.RESPONSE_DONE
        self.words[7] = 0


class _NorMailboxTarget(_FakeMailboxTarget):
    """Protocol-level mailbox target with a byte-addressed NOR model."""

    def __init__(self, *, bad_readback=False, timeout_command=None):
        super().__init__()
        self.flash = bytearray(b"\xa5" * ospi.GD25Q64E_CAPACITY)
        self.bad_readback = bad_readback
        self.timeout_command = timeout_command
        self.erase_log = []
        self.program_log = []
        self.read_log = []

    def resume(self):
        command, offset, length = self.words[3:6]
        self.requests.append((command, offset, length))
        if command == self.timeout_command:
            return
        if command == ospi.CMD_READ_ID:
            self.words[8] = ospi.GD25Q64E_JEDEC
            self.words[9:11] = [0, 0]
        elif command == ospi.CMD_READ_STATUS:
            self.words[9:11] = [0, 0]
        elif command == ospi.CMD_ERASE_4K:
            if length != ospi.GD25Q64E_ERASE or offset % ospi.GD25Q64E_ERASE:
                self.words[7] = 1
            else:
                self.flash[offset:offset + length] = b"\xff" * length
                self.erase_log.append((offset, length))
        elif command == ospi.CMD_PROGRAM:
            raw = bytes(self.buffer[:length])
            self.flash[offset:offset + length] = bytes(
                old & new for old, new in zip(self.flash[offset:offset + length], raw))
            self.program_log.append((offset, raw))
        elif command == ospi.CMD_READ:
            self.buffer[:length] = self.flash[offset:offset + length]
            self.read_log.append((offset, length))
        else:
            self.words[7] = 2
        self.words[1] = 0
        self.words[6] = ospi.RESPONSE_ERROR if self.words[7] else ospi.RESPONSE_DONE

    def read_memory_block8(self, _address, count):
        result = bytes(self.buffer[:count])
        if self.bad_readback and result:
            return bytes((result[0] ^ 1,)) + result[1:]
        return result


class _NorContext:
    def __init__(self, target):
        self.probe_id = "cmsis-dap:probe-1"
        self.target = target

    def __enter__(self):
        return self.target

    def __exit__(self, *_args):
        return False


class OspiWriterTests(unittest.TestCase):
    @staticmethod
    def manifest():
        return resolver.resolve_build_layout({
            "OPEN_ROOT": str(ROOT),
            "PLATFORM_CHIP": "GD32H77D",
            "PLATFORM_BOARD": TEST_BOARD,
            "CONFIG_PLATFORM_CHOICE": "GD32H77X",
            "CONFIG_BOARD_CHOICE": TEST_BOARD,
            "CONFIG_CHIP_CHOICE": "GD32H77D",
        }, platform_root=PLATFORM, boards_root=FIXTURE_BOARDS)

    def test_real_resolver_preflight_binds_padded_payload_and_probe(self):
        manifest = self.manifest()
        padded_data = bytes((index * 17) & 0xFF for index in range(8192))
        with tempfile.TemporaryDirectory() as temp:
            temp_path = Path(temp)
            payload_file = temp_path / "payload.bin"
            payload_file.write_bytes(padded_data)
            pack = temp_path / "test.pack"
            pack.write_bytes(b"test pack path for patched preflight")
            built = temp_path / "loader"
            built.mkdir()
            elf = built / "loader.elf"
            binary = built / "loader.bin"
            metadata = built / "loader.json"
            elf.write_bytes(b"elf")
            binary.write_bytes(b"loader")
            loader_hash = hashlib.sha256(binary.read_bytes()).hexdigest()
            metadata.write_text(json.dumps({"binary_sha256": loader_hash}))
            payload = {
                "partition": "staging",
                "path": str(payload_file),
                "size_bytes": len(padded_data),
                "sha256": hashlib.sha256(padded_data).hexdigest(),
                "source_size_bytes": 7000,
                "source_sha256": "f" * 64,
            }
            with (mock.patch.object(ospi, "_build_loader", return_value=(
                    str(elf), str(binary), loader_hash, str(metadata))),
                  mock.patch.object(ospi, "_open_target", return_value=_FakeContext()),
                  mock.patch.object(ospi, "_launch_loader"),
                  mock.patch.object(ospi, "_initialize_loader", return_value={
                      "jedec_id": ospi.GD25Q64E_JEDEC, "status1": 0, "status2": 0})):
                metadata.write_text(json.dumps({"binary_sha256": loader_hash,
                    "target": "GD32H77DIW", "pins": ospi._normalize_pin_binding(
                        manifest["layout"]["board_binding"]["ospi0_flash"]),
                    "binding_sha256": "test-binding"}))
                plan = ospi.preflight_ospi(manifest, [payload], str(pack))
            self.assertEqual(plan.probe_id, "cmsis-dap:probe-1")
            self.assertEqual(plan.pack_path, str(pack.resolve()))
            self.assertEqual(plan.payloads[0]["eraseRanges"], [{"offset": 0, "size": 8192}])
            ospi.cleanup_plan(plan)
            self.assertFalse(binary.parent.exists())

    def test_payload_hash_or_layout_mismatch_fails_closed(self):
        manifest = self.manifest()
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "payload.bin"
            path.write_bytes(b"x" * 4096)
            payload = {"partition": "staging", "path": str(path),
                       "size_bytes": 4096, "sha256": "0" * 64}
            nor = next(item for item in manifest["layout"]["devices"]
                       if item["part_number"] == ospi.GD25Q64E_GEOMETRY)
            nor["capacity_bytes"] -= 4096
            with self.assertRaises(ospi.OspiFlashError):
                ospi._validate_layout(manifest)

    def test_layout_accepts_registered_canonical_variant_and_resolved_pins(self):
        manifest = self.manifest()
        manifest["layout"]["target_chip"] = "GD32H77DIP"
        binding = manifest["layout"]["board_binding"]["ospi0_flash"]
        binding["chip_select"] = "PA2"
        binding["clock"] = "PB3"
        binding["data"] = ["PC4", "PD5", "PE6", "PF7"]
        _, device = ospi._validate_layout(manifest)
        self.assertEqual(ospi._target_macro("GD32H77DIP"), "GD32H77DXP")
        self.assertEqual(ospi._normalize_pin_binding(binding)["cs"], "PA2")
        binding["data"][1] = "PA2"
        with self.assertRaises(ospi.OspiFlashError):
            ospi._validate_layout(manifest)

    def test_invalid_pin_binding_fails_before_loader_build_or_probe(self):
        manifest = self.manifest()
        binding = manifest["layout"]["board_binding"]["ospi0_flash"]
        binding["data"][1] = binding["chip_select"]
        with tempfile.TemporaryDirectory() as temp:
            pack = Path(temp) / "pack.pack"
            pack.write_bytes(b"pack")
            with mock.patch.object(ospi, "_build_loader",
                                   side_effect=AssertionError("loader must not build")), \
                 mock.patch.object(ospi, "_open_target",
                                   side_effect=AssertionError("probe must not open")):
                with self.assertRaisesRegex(ospi.OspiFlashError, "conflicting OSPI pin"):
                    ospi.preflight_ospi(manifest, [], str(pack))

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            compiler = root / "bin/arm-none-eabi-gcc.exe"
            compiler.parent.mkdir()
            compiler.write_bytes(b"fake compiler")
            with self.subTest(source="ARM_NONE_EABI_GCC bare command"):
                with (mock.patch.dict(os.environ, {"ARM_NONE_EABI_GCC": "arm-gcc-test"},
                                      clear=True),
                      mock.patch.object(ospi.shutil, "which",
                                        side_effect=lambda name: str(compiler)
                                        if name == "arm-gcc-test" else None)):
                    self.assertEqual(ospi._find_compiler(), str(compiler))
            with self.subTest(source="ARM_NONE_EABI_GCC explicit path"):
                with (mock.patch.dict(os.environ, {"ARM_NONE_EABI_GCC": str(compiler)},
                                      clear=True),
                      mock.patch.object(ospi.shutil, "which", return_value=None)):
                    self.assertEqual(ospi._find_compiler(), str(compiler.resolve()))
            with self.subTest(source="GD32H77X_TOOLCHAIN root with Windows suffix"):
                with (mock.patch.dict(os.environ, {"GD32H77X_TOOLCHAIN": str(root)},
                                      clear=True),
                      mock.patch.object(ospi.shutil, "which", return_value=None)):
                    self.assertEqual(ospi._find_compiler(), str(compiler.resolve()))
            with self.subTest(source="PATH"):
                with (mock.patch.dict(os.environ, {}, clear=True),
                      mock.patch.object(ospi.shutil, "which",
                                        side_effect=lambda name: str(compiler)
                                        if name == "arm-none-eabi-gcc" else None)):
                    self.assertEqual(ospi._find_compiler(), str(compiler))

    def test_request_uses_sector_length_and_rejects_cross_page(self):
        target = _NorMailboxTarget()
        with mock.patch.object(ospi.time, "sleep"):
            ospi._request(target, ospi.CMD_ERASE_4K, 0, ospi.GD25Q64E_ERASE)
        self.assertEqual(target.requests[-1], (ospi.CMD_ERASE_4K, 0, 4096))
        with self.assertRaises(ospi.OspiFlashError):
            ospi._request(target, ospi.CMD_PROGRAM, 255, 2, b"ab")
        self.assertEqual(ospi._request(target, ospi.CMD_READ_ID), b"\xc8\x40\x17")
        for args in ((ospi.CMD_READ_ID, 1, 0, None),
                     (ospi.CMD_READ_STATUS, 0, 0, b"x"),
                     (ospi.CMD_PROGRAM, 0, 1, None)):
            with self.assertRaises(ospi.OspiFlashError):
                ospi._request(target, *args)

    def test_flash_ospi_protocol_model_programs_multiple_segmented_partitions(self):
        manifest = self.manifest()
        layout = manifest["layout"]
        device = next(item for item in layout["devices"]
                      if item["part_number"] == ospi.GD25Q64E_GEOMETRY)
        # A contiguous logical partition spans two separated physical regions.
        layout["partitions"]["staging"]["segments"] = [
            {"logical_offset_bytes": 0, "size_bytes": 4096,
             "device": device["id"], "device_offset_bytes": 0x1000},
            {"logical_offset_bytes": 4096, "size_bytes": 4096,
             "device": device["id"], "device_offset_bytes": 0x3000},
        ]
        layout["partitions"]["metadata"]["segments"] = [
            {"logical_offset_bytes": 0, "size_bytes": 4096,
             "device": device["id"], "device_offset_bytes": 0x5000},
        ]
        with tempfile.TemporaryDirectory() as temp:
            temp_path = Path(temp)
            payload_entries = []
            expected = {}
            for name, raw in (("staging", bytes(range(256)) * 32),
                              ("metadata", bytes(reversed(range(256))) * 16)):
                path = temp_path / f"{name}.bin"
                path.write_bytes(raw)
                expected[name] = raw
                payload_entries.append({
                    "partition": name, "path": str(path), "size_bytes": len(raw),
                    "sha256": hashlib.sha256(raw).hexdigest(),
                })
            payloads = ospi._validated_payloads(layout, device, payload_entries)
            loader_dir = temp_path / "loader"
            loader_dir.mkdir()
            binary = loader_dir / "loader.bin"
            binary.write_bytes(b"loader code")
            digest = hashlib.sha256(binary.read_bytes()).hexdigest()
            metadata_path = loader_dir / "loader.json"
            pins = ospi._normalize_pin_binding(layout["board_binding"][device["id"]])
            metadata_path.write_text(json.dumps({"binary_sha256": digest,
                "target": "GD32H77DIW", "pins": pins, "binding_sha256": "test-binding"}))
            pack = temp_path / "pack.pack"
            pack.write_bytes(b"pack")
            plan = ospi.OspiPlan(
                probe_id="cmsis-dap:probe-1", target_chip="GD32H77DIW",
                pack_path=str(pack), device_id=device["id"], payloads=payloads,
                loader_bin=str(binary), loader_elf=str(loader_dir / "loader.elf"),
                loader_metadata=str(metadata_path), loader_sha256=digest,
                pin_binding=pins, binding_sha256="test-binding",
            )
            target = _NorMailboxTarget()
            with (mock.patch.object(ospi, "_open_target", return_value=_NorContext(target)),
                  mock.patch.object(ospi, "_launch_loader"),
                  mock.patch.object(ospi.time, "sleep")):
                result = ospi.flash_ospi(plan, verify=True)
            self.assertTrue(result["success"])
            self.assertTrue(result["verified"])
            self.assertEqual([item["partition"] for item in result["partitions"]],
                             ["staging", "metadata"])
            self.assertEqual(len(target.erase_log), 3)
            self.assertEqual(target.flash[0x1000:0x2000], expected["staging"][:4096])
            self.assertEqual(target.flash[0x3000:0x4000], expected["staging"][4096:])
            self.assertEqual(target.flash[0x5000:0x6000], expected["metadata"])
            written = set(range(0x1000, 0x2000)) | set(range(0x3000, 0x4000)) | set(range(0x5000, 0x6000))
            self.assertTrue(all(byte == 0xA5 for index, byte in enumerate(target.flash)
                                if index not in written))
            for row, raw in zip(result["partitions"], expected.values()):
                self.assertEqual(row["readbackSha256"], hashlib.sha256(raw).hexdigest())
            self.assertFalse(loader_dir.exists())

    def test_flash_ospi_verify_false_and_readback_failure_cleanup(self):
        manifest = self.manifest()
        layout = manifest["layout"]
        device = next(item for item in layout["devices"]
                      if item["part_number"] == ospi.GD25Q64E_GEOMETRY)
        with tempfile.TemporaryDirectory() as temp:
            temp_path = Path(temp)
            payload_path = temp_path / "payload.bin"
            payload_path.write_bytes(b"p" * 4096)
            payloads = ospi._validated_payloads(layout, device, [{
                "partition": "staging", "path": str(payload_path), "size_bytes": 4096,
                "sha256": hashlib.sha256(payload_path.read_bytes()).hexdigest(),
            }])
            for bad_readback, verify in ((False, False), (True, True)):
                loader_dir = temp_path / f"loader-{bad_readback}"
                loader_dir.mkdir()
                binary = loader_dir / "loader.bin"
                binary.write_bytes(b"loader")
                digest = hashlib.sha256(binary.read_bytes()).hexdigest()
                metadata = loader_dir / "loader.json"
                pins = ospi._normalize_pin_binding(layout["board_binding"][device["id"]])
                metadata.write_text(json.dumps({"binary_sha256": digest,
                    "target": "GD32H77DIW", "pins": pins, "binding_sha256": "test-binding"}))
                plan = ospi.OspiPlan(
                    probe_id="cmsis-dap:probe-1", target_chip="GD32H77DIW",
                    pack_path=str(temp_path / "pack.pack"), device_id=device["id"],
                    payloads=payloads, loader_bin=str(binary), loader_elf="unused",
                    loader_metadata=str(metadata), loader_sha256=digest,
                    pin_binding=pins, binding_sha256="test-binding",
                )
                target = _NorMailboxTarget(bad_readback=bad_readback)
                with (mock.patch.object(ospi, "_open_target", return_value=_NorContext(target)),
                      mock.patch.object(ospi, "_launch_loader"),
                      mock.patch.object(ospi.time, "sleep")):
                    if verify:
                        with self.assertRaisesRegex(ospi.OspiFlashError,
                                                    "phase=readback.*device offset=0xf00.*readback mismatch"):
                            ospi.flash_ospi(plan, verify=True)
                    else:
                        result = ospi.flash_ospi(plan, verify=False)
                        self.assertTrue(result["success"])
                        self.assertFalse(result["verified"])
                        self.assertFalse(target.read_log)
                self.assertFalse(loader_dir.exists())

    def test_flash_ospi_timeout_reports_partial_state_and_cleans_loader(self):
        manifest = self.manifest()
        layout = manifest["layout"]
        device = next(item for item in layout["devices"]
                      if item["part_number"] == ospi.GD25Q64E_GEOMETRY)
        with tempfile.TemporaryDirectory() as temp:
            temp_path = Path(temp)
            entries = []
            for name in ("staging", "metadata"):
                path = temp_path / f"{name}.bin"
                path.write_bytes((name.encode() * 4096)[:4096])
                entries.append({"partition": name, "path": str(path), "size_bytes": 4096,
                                "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
            payloads = ospi._validated_payloads(layout, device, entries)
            loader_dir = temp_path / "loader"
            loader_dir.mkdir()
            binary = loader_dir / "loader.bin"
            binary.write_bytes(b"loader")
            digest = hashlib.sha256(binary.read_bytes()).hexdigest()
            metadata = loader_dir / "loader.json"
            pins = ospi._normalize_pin_binding(layout["board_binding"][device["id"]])
            metadata.write_text(json.dumps({"binary_sha256": digest,
                "target": "GD32H77DIW", "pins": pins, "binding_sha256": "test-binding"}))
            plan = ospi.OspiPlan(
                probe_id="cmsis-dap:probe-1", target_chip="GD32H77DIW",
                pack_path=str(temp_path / "pack.pack"), device_id=device["id"],
                payloads=payloads, loader_bin=str(binary), loader_elf="unused",
                loader_metadata=str(metadata), loader_sha256=digest,
                pin_binding=pins, binding_sha256="test-binding",
            )
            target = _NorMailboxTarget(timeout_command=ospi.CMD_ERASE_4K)
            ticks = [0.0]
            real_monotonic = ospi.time.monotonic
            def advance_time():
                if target.words[3] != ospi.CMD_ERASE_4K:
                    return real_monotonic()
                ticks[0] += 20.0
                return ticks[0]
            with (mock.patch.object(ospi, "_open_target", return_value=_NorContext(target)),
                  mock.patch.object(ospi, "_launch_loader"),
                  mock.patch.object(ospi.time, "sleep"),
                  mock.patch.object(ospi.time, "monotonic", side_effect=advance_time)):
                with self.assertRaisesRegex(
                        ospi.OspiFlashError,
                        r"completed partitions=\[\]; current partition=staging "
                        r"phase=erase device offset=0x0.*timed out"):
                    ospi.flash_ospi(plan, verify=True)
            self.assertFalse(loader_dir.exists())

    def test_nor_identity_and_protection_fail_closed(self):
        ready = {"jedec_id": ospi.GD25Q64E_JEDEC, "status1": 0, "status2": 0}
        ospi._require_nor_ready(ready)
        for bad in (
                {**ready, "jedec_id": 0x123456},
                {**ready, "status1": 0x04},
                {**ready, "status1": 0x01},
                {**ready, "status2": 0x40},
                {**ready, "status2": 0x80}):
            with self.subTest(bad=bad), self.assertRaises(ospi.OspiFlashError):
                ospi._require_nor_ready(bad)

    def test_production_c_core_with_protocol_aware_flash_model(self):
        compiler = shutil.which("gcc")
        if compiler is None:
            self.skipTest("host gcc is unavailable")
        harness = PLATFORM / "tests/test_ospi_loader_core.c"
        with tempfile.TemporaryDirectory() as temp:
            executable = Path(temp) / "test_ospi_loader_core"
            subprocess.run([
                compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", str(harness), "-o", str(executable),
            ], check=True, capture_output=True, text=True)
            env = dict(os.environ)
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            result = subprocess.run([str(executable)], check=True, capture_output=True,
                                    text=True, env=env)
        self.assertIn("production OSPI loader core model: PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
