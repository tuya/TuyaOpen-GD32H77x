#!/usr/bin/env python3
"""End-to-end flash-tool checks for every registered GD32H77x device variant.

The test compiles a tiny transient ARM image, builds the normal layout report,
and exercises the production DfuSe protocol against a ROM model. USB discovery
is replaced; no libusb enumeration or hardware access is performed.
"""

import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock


PLATFORM = Path(__file__).resolve().parents[1]
ROOT = PLATFORM.parents[1]
RESOLVER_PATH = PLATFORM / "gd32h77x_os/tools/firmware/resolve_memory_layout.py"
VERIFIER_PATH = PLATFORM / "gd32h77x_os/tools/firmware/verify_image.py"
BRIDGE_PATH = PLATFORM / "platform_flash_bridge.py"
DFUSE_PATH = PLATFORM / "gd32h77x_os/tools/download/dfuse.py"
TEST_DFUSE_PATH = Path(__file__).with_name("test_dfuse.py")


def _load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


resolver = _load_module("gd32h77x_flash_variants_resolver", RESOLVER_PATH)
verifier = _load_module("gd32h77x_flash_variants_verifier", VERIFIER_PATH)
bridge = _load_module("gd32h77x_flash_variants_bridge", BRIDGE_PATH)
dfuse = _load_module("gd32h77x_flash_variants_dfuse", DFUSE_PATH)
rom_model_module = _load_module("gd32h77x_flash_variants_rom_model", TEST_DFUSE_PATH)


DEVICES = (
    "GD32H77DIW", "GD32H77DIP", "GD32H77DII",
    "GD32H77EIW", "GD32H77EIP", "GD32H77EII",
    "GD32H779IW", "GD32H779IP", "GD32H779II",
    "GD32H779VW", "GD32H779VP", "GD32H779VI",
    "GD32H779ZW", "GD32H779ZP", "GD32H779ZI",
    "GD32H77RAP",
)

FIXTURE_ASM = r"""
.syntax unified
.cpu cortex-m7
.thumb

.section .boot_table,"a",%progbits
.word _sp
.word Reset_Handler

.section .vectors,"a",%progbits
.global __gVectors
__gVectors:
.word _sp
.word Reset_Handler
.rept 9
.word Default_Handler
.endr
.word SVC_Handler
.word Default_Handler
.word Default_Handler
.word PendSV_Handler
.word SysTick_Handler

.section .text.Reset_Handler,"ax",%progbits
.global Reset_Handler
.type Reset_Handler,%function
.thumb_func
Reset_Handler:
    b .

.section .text.Default_Handler,"ax",%progbits
.global Default_Handler
.type Default_Handler,%function
.thumb_func
Default_Handler:
    bx lr

.section .text.SVC_Handler,"ax",%progbits
.global SVC_Handler
.type SVC_Handler,%function
.thumb_func
SVC_Handler:
    bx lr

.section .text.PendSV_Handler,"ax",%progbits
.global PendSV_Handler
.type PendSV_Handler,%function
.thumb_func
PendSV_Handler:
    bx lr

.section .text.SysTick_Handler,"ax",%progbits
.global SysTick_Handler
.type SysTick_Handler,%function
.thumb_func
SysTick_Handler:
    bx lr

.section .text.tuya_app_main,"ax",%progbits
.global tuya_app_main
.type tuya_app_main,%function
.thumb_func
tuya_app_main:
    bx lr

.section .ramfunc,"ax",%progbits
.global TestCnvmCommand
.type TestCnvmCommand,%function
.thumb_func
TestCnvmCommand:
    bx lr
"""


class ModelUsbDevice:
    def __init__(self, flash, protocol_module):
        self.flash = flash
        self.path = "3-3.2"
        self.serial = "DIWd"
        self.protocol_module = protocol_module
        self.closed = False
        self.claimed = None

    def interfaces(self):
        return [{
            "interface": 0, "alt": 0, "protocol": 2,
            "name": "@InternalFlash /0x08000000/960*004Kg",
            "transfer_size": self.flash.transfer_size,
            "bcd_dfu_version": 0x011A, "bm_attributes": 0x07,
        }]

    def claim(self, interface, alt=0):
        self.claimed = (interface, alt)

    def control_out(self, *args):
        return self.flash.control_out(*args)

    def control_in(self, *args):
        return self.flash.control_in(*args)

    def close(self):
        self.closed = True


class FlashVariantEndToEndTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gcc = shutil.which("arm-none-eabi-gcc") or "/opt/gcc-arm-none-eabi-10.3-2021.10/bin/arm-none-eabi-gcc"
        cls.objcopy = shutil.which("arm-none-eabi-objcopy") or "/opt/gcc-arm-none-eabi-10.3-2021.10/bin/arm-none-eabi-objcopy"
        if not Path(cls.gcc).is_file() or not Path(cls.objcopy).is_file():
            raise unittest.SkipTest("GNU Arm Embedded compiler/objcopy unavailable")
        cls.temp = tempfile.TemporaryDirectory(prefix="gd32h77x-flash-variants-")
        cls.workspace = Path(cls.temp.name)
        cls.boards_root = cls.workspace / "boards"
        cls.boards_root.mkdir()
        cls.source = cls.workspace / "fixture.S"
        cls.source.write_text(FIXTURE_ASM)

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "temp"):
            cls.temp.cleanup()

    def _build_artifact(self, chip):
        board = "FLASH_VARIANT_" + chip
        board_dir = self.boards_root / "GD32H77X" / board
        board_dir.mkdir(parents=True, exist_ok=True)
        (board_dir / "flash.cfg").write_text(
            "CONFIG_FLASH_BACKEND=dfu\n"
            "CONFIG_FLASH_TARGET_CHIP={}\n"
            "CONFIG_FLASH_USB_VID=28e9\n"
            "CONFIG_FLASH_USB_PID=0189\n".format(chip))
        build_params = {
            "OPEN_ROOT": str(self.workspace), "PLATFORM_CHIP": chip,
            "PLATFORM_BOARD": board, "CONFIG_PLATFORM_CHOICE": "GD32H77X",
            "CONFIG_BOARD_CHOICE": board, "CONFIG_CHIP_CHOICE": chip,
        }
        layout = resolver.resolve_build_layout(
            build_params, platform_root=PLATFORM, boards_root=self.boards_root)
        out = self.workspace / (chip + "-generated")
        resolver.write_outputs(layout, out)

        # Use the canonical GD32H77DIW linker template with resolved MEMORY regions.
        # This is a minimal resolver/bridge fixture, not a build-support claim
        # for chip variants rejected by the platform CMake guard.
        elf = self.workspace / (chip + ".elf")
        binary = self.workspace / (chip + ".bin")
        linker = PLATFORM / "gd32h77x_os" / "chips" / "GD32H77DIW" / "linker.ld"
        subprocess.run([
            self.gcc, "-mcpu=cortex-m7", "-mthumb", "-nostdlib",
            "-Wl,--build-id=none", "-Wl,--fatal-warnings",
            "-Wl,-T," + str(linker), "-L", str(out), "-o", str(elf), str(self.source),
        ], check=True, capture_output=True, text=True)
        subprocess.run([
            self.objcopy, "-O", "binary", "--gap-fill", "0xff", str(elf), str(binary),
        ], check=True, capture_output=True, text=True)

        report = verifier.verify(elf, binary, out / "resolved_layout.json",
                                 bin_name=binary.name, elf_name=elf.name)
        report_path = binary.parent / "firmware_1.0.0.layout.json"
        report_path.write_text(json.dumps(report))
        using_data = {
            "CONFIG_PLATFORM_CHOICE": "GD32H77X", "CONFIG_BOARD_CHOICE": board,
            "CONFIG_CHIP_CHOICE": chip, "CONFIG_PROJECT_NAME": "firmware",
            "CONFIG_PROJECT_VERSION": "1.0.0",
        }
        return layout, binary, using_data

    def test_all_registered_variants_resolve_verify_and_flash_via_real_dfuse(self):
        for chip in DEVICES:
            with self.subTest(chip=chip):
                layout, image, using = self._build_artifact(chip)
                report, start, limit, capacity, device = bridge._validate_manifest(
                    image, str(self.boards_root), using)
                self.assertEqual(report["chip"], chip)
                self.assertEqual(layout["layout"]["target_chip"], chip)
                self.assertEqual(device["geometry_id"], chip + "_CNVM")
                self.assertGreaterEqual(limit - start, image.stat().st_size)

                cnvm_size = device["capacity_bytes"]
                rom = rom_model_module.DfuSeFlashModel(
                    base=start, capacity=cnvm_size, transfer_size=1024)
                rom.memory[:] = b"\xA5" * cnvm_size
                usb_device = ModelUsbDevice(rom, dfuse)
                lib = type("Lib", (), {"close": lambda _self: None})()

                def fake_open(vendor, product):
                    self.assertEqual((vendor, product), (0x28E9, 0x0189))
                    return lib, [usb_device]

                with mock.patch.object(dfuse, "open_dfu_devices", side_effect=fake_open), \
                        mock.patch.object(bridge, "_dfuse_module", return_value=dfuse):
                    result = bridge.platform_flash(
                        using, str(image), None, None, str(self.boards_root), None)
                self.assertTrue(result["success"], result)
                padded = ((image.stat().st_size + 65535) // 65536) * 65536
                expected = image.read_bytes() + b"\xFF" * (padded - image.stat().st_size)
                self.assertEqual(bytes(rom.memory[:padded]), expected)
                self.assertEqual(bytes(rom.memory[padded:]), b"\xA5" * (cnvm_size - padded))
                self.assertTrue(usb_device.closed)

    def test_dii_artifact_cannot_be_reused_after_switching_to_iw_and_never_enumerates(self):
        _layout, image, using = self._build_artifact("GD32H77DII")
        using["CONFIG_CHIP_CHOICE"] = "GD32H77DIW"
        with mock.patch.object(dfuse, "open_dfu_devices") as open_devices, \
                mock.patch.object(bridge, "_dfuse_module", return_value=dfuse):
            result = bridge.platform_flash(
                using, str(image), None, None, str(self.boards_root), None)
        self.assertFalse(result["success"])
        self.assertIn("does not match selected target", result["message"])
        open_devices.assert_not_called()


if __name__ == "__main__":
    unittest.main()
