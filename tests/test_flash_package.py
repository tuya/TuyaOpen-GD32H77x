#!/usr/bin/env python3
"""Tests for build-bound GD32H77X multi-region flash packages."""

import importlib.util
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zipfile

import linker_template


PLATFORM = Path(__file__).resolve().parents[1]
ROOT = PLATFORM.parents[1]
SCRIPT_DIR = PLATFORM / "gd32h77x_os/tools/firmware"
TEST_BOARD = "GD32H77D_TEST_BOARD"
FIXTURE_BOARDS = PLATFORM / "tests" / "fixtures" / "boards"


def _module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


package = _module("gd32h77x_flash_package_tests", SCRIPT_DIR / "flash_package.py")
resolver = _module("gd32h77x_flash_package_resolver", SCRIPT_DIR / "resolve_memory_layout.py")
verifier = _module("gd32h77x_flash_package_verifier_tests", SCRIPT_DIR / "verify_image.py")

# Reuse the compact Thumb fixture source from the variant integration test.
fixture_module = _module("gd32h77x_flash_package_fixture", Path(__file__).with_name("test_flash_variants.py"))


class FlashPackageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gcc = shutil.which("arm-none-eabi-gcc") or "/opt/gcc-arm-none-eabi-10.3-2021.10/bin/arm-none-eabi-gcc"
        cls.objcopy = shutil.which("arm-none-eabi-objcopy") or "/opt/gcc-arm-none-eabi-10.3-2021.10/bin/arm-none-eabi-objcopy"
        if not Path(cls.gcc).is_file() or not Path(cls.objcopy).is_file():
            raise unittest.SkipTest("GNU Arm Embedded compiler/objcopy unavailable")
        cls.temp = tempfile.TemporaryDirectory(prefix="gd32h77x-package-test-")
        cls.work = Path(cls.temp.name)
        cls.layout = resolver.resolve_build_layout({
            "OPEN_ROOT": str(ROOT), "PLATFORM_CHIP": "GD32H77D",
            "PLATFORM_BOARD": TEST_BOARD, "CONFIG_PLATFORM_CHOICE": "GD32H77X",
            "CONFIG_BOARD_CHOICE": TEST_BOARD, "CONFIG_CHIP_CHOICE": "GD32H77D",
        }, platform_root=PLATFORM, boards_root=FIXTURE_BOARDS)
        generated = cls.work / "generated"
        resolver.write_outputs(cls.layout, generated)
        source = cls.work / "fixture.S"
        source.write_text(fixture_module.FIXTURE_ASM)
        cls.elf = cls.work / "firmware.elf"
        cls.bin = cls.work / "firmware.bin"
        linker = linker_template.render(cls.layout, cls.work / "linker.ld")
        subprocess.run([
            cls.gcc, "-mcpu=cortex-m7", "-mthumb", "-nostdlib",
            "-Wl,--build-id=none", "-Wl,--fatal-warnings",
            "-Wl,-T," + str(linker),
            "-L", str(generated), "-o", str(cls.elf), str(source),
        ], check=True, capture_output=True, text=True)
        subprocess.run([cls.objcopy, "-O", "binary", "--gap-fill", "0xff",
                        str(cls.elf), str(cls.bin)], check=True, capture_output=True, text=True)
        cls.layout_path = generated / "resolved_layout.json"
        cls.report = verifier.verify(cls.elf, cls.bin, cls.layout_path,
                                      bin_name=cls.bin.name, elf_name=cls.elf.name)
        cls.report_path = cls.work / "firmware_1.0.0.layout.json"
        cls.report_path.write_text(json.dumps(cls.report))

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "temp"):
            cls.temp.cleanup()

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix="case-", dir=self.work))
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.output = self.dir / "firmware_QIO.bin"

    def _create(self, csv_path=None):
        return package.create_package(
            self.output, self.report_path, self.elf, self.bin,
            self.layout, csv_path)

    def test_mapped_payload_report_path_cannot_escape_verifier_temp_directory(self):
        sentinel = self.work / "sentinel.bin"
        sentinel.write_bytes(b"do-not-overwrite")
        report = dict(self.report)
        report["ecnvmCodeEnabled"] = True
        report["mapped_payloads"] = [{"partition": "ecnvm_code", "region": "ECNVM",
                                       "file": "../sentinel.bin"}]
        with self.assertRaisesRegex(package.PackageError, "unique basename"):
            package._verify_artifacts(report, self.elf.read_bytes(), self.bin.read_bytes(),
                                      self.layout, b"attacker-data")
        self.assertEqual(sentinel.read_bytes(), b"do-not-overwrite")

    def test_pure_internal_package_round_trips_and_extracts_only_verified_members(self):
        manifest = self._create()
        self.assertEqual(manifest["external_payloads"], [])
        extract = self.dir / "extract"
        result = package.validate_package(self.output, self.layout,
                                          TEST_BOARD, "GD32H77D", extract)
        self.assertTrue(result["valid"])
        self.assertEqual(result["internal_bin_sha256"], self.report["binSha256"])
        self.assertEqual(Path(result["artifacts"]["internal_bin"]).read_bytes(), self.bin.read_bytes())
        with zipfile.ZipFile(self.output) as archive:
            self.assertEqual(set(archive.namelist()), {
                "manifest.json", "artifacts/report.json", "artifacts/firmware.elf",
                "artifacts/internal.bin"})

    def test_explicit_external_payloads_are_partition_routed_and_erase_padded(self):
        (self.dir / "stage.bin").write_bytes(b"stage-data")
        (self.dir / "fs.bin").write_bytes(b"filesystem-data")
        csv_path = self.dir / "payloads.csv"
        csv_path.write_text("partition,file\nstaging,stage.bin\nfilesystem,fs.bin\n")
        manifest = self._create(csv_path)
        self.assertEqual({item["partition"] for item in manifest["external_payloads"]},
                         {"staging", "filesystem"})
        self.assertTrue(all(item["explicit_payload"] for item in manifest["external_payloads"]))
        self.assertTrue(all(item["explicit_payload"] for item in manifest["external_payloads"]
                            if item["role"] == "ota_staging"))
        extract = self.dir / "extract"
        result = package.validate_package(self.output, self.layout, extract_dir=extract)
        self.assertEqual(len(result["external_payloads"]), 2)
        for item in result["external_payloads"]:
            payload = Path(item["path"]).read_bytes()
            self.assertEqual(len(payload), item["padded_size_bytes"])
            self.assertEqual(payload[:item["source_size_bytes"]],
                             (self.dir / {"staging": "stage.bin",
                                          "filesystem": "fs.bin"}[item["partition"]]).read_bytes())
            self.assertEqual(set(byte for byte in payload[item["source_size_bytes"]:]), {0xFF})
            self.assertTrue(all(segment["region"] == "NOR" for segment in item["segments"]))

    def test_protected_ota_kv_metadata_are_never_added_implicitly(self):
        manifest = self._create()
        listed = {item["partition"] for item in manifest["external_payloads"]}
        self.assertTrue(listed.isdisjoint({"staging", "metadata", "tuya"}))

    def test_external_csv_rejects_duplicate_partition_traversal_and_bad_header(self):
        (self.dir / "payload.bin").write_bytes(b"data")
        cases = [
            ("partition,file\nfilesystem,payload.bin\nfilesystem,payload.bin\n", "repeats"),
            ("partition,file\nfilesystem,../payload.bin\n", "stay under"),
            ("name,file\nfilesystem,payload.bin\n", "header must be"),
        ]
        for body, message in cases:
            with self.subTest(message=message):
                csv_path = self.dir / "bad.csv"
                csv_path.write_text(body)
                with self.assertRaisesRegex(package.PackageError, message):
                    self._create(csv_path)

    def test_payload_boundaries_and_internal_partition_override_are_rejected(self):
        csv_path = self.dir / "payloads.csv"
        filesystem_size = self.layout["layout"]["partitions"]["filesystem"]["size_bytes"]
        (self.dir / "too-large.bin").write_bytes(b"x" * (filesystem_size + 4096))
        csv_path.write_text("partition,file\nfilesystem,too-large.bin\n")
        with self.assertRaisesRegex(package.PackageError, "after erase padding"):
            self._create(csv_path)
        (self.dir / "payload.bin").write_bytes(b"x")
        csv_path.write_text("partition,file\nprimary,payload.bin\n")
        with self.assertRaisesRegex(package.PackageError, "boot/primary"):
            self._create(csv_path)

    def test_one_partition_payload_can_span_multiple_segments_on_one_device(self):
        synthetic = json.loads(json.dumps(self.layout["layout"]))
        filesystem = synthetic["partitions"]["filesystem"]
        original = filesystem["segments"][0]
        half = original["size_bytes"] // 2
        filesystem["segments"] = [
            {"logical_offset_bytes": 0, "device": original["device"],
             "device_offset_bytes": original["device_offset_bytes"], "size_bytes": half},
            {"logical_offset_bytes": half, "device": original["device"],
             "device_offset_bytes": original["device_offset_bytes"] + half,
             "size_bytes": original["size_bytes"] - half},
        ]
        padded, _erase, extents, _role = package._payload_plan(
            synthetic, "filesystem", half + 17)
        self.assertEqual(padded, half + 4096)
        self.assertEqual(len(extents), 2)
        self.assertEqual(extents[0]["partition_offset_bytes"], 0)
        self.assertEqual(extents[1]["partition_offset_bytes"], half)
        self.assertEqual(sum(item["size_bytes"] for item in extents), padded)

    def test_one_partition_payload_spans_two_nor_devices_with_erase_padding(self):
        synthetic = json.loads(json.dumps(self.layout["layout"]))
        nor0 = next(device for device in synthetic["devices"] if device["id"] == "ospi0_flash")
        nor1 = dict(nor0, id="ospi1_flash")
        synthetic["devices"].append(nor1)
        filesystem = synthetic["partitions"]["filesystem"]
        half = filesystem["size_bytes"] // 2
        old = filesystem["segments"][0]
        filesystem["segments"] = [
            {"logical_offset_bytes": 0, "device": "ospi0_flash",
             "device_offset_bytes": old["device_offset_bytes"], "size_bytes": half},
            {"logical_offset_bytes": half, "device": "ospi1_flash",
             "device_offset_bytes": 0, "size_bytes": filesystem["size_bytes"] - half},
        ]
        padded, _erase, extents, _role = package._payload_plan(
            synthetic, "filesystem", half + 1)
        self.assertEqual(padded, half + 4096)
        self.assertEqual([extent["device"] for extent in extents], ["ospi0_flash", "ospi1_flash"])
        self.assertEqual([extent["size_bytes"] for extent in extents], [half, 4096])

        for bad_offset, error in ((half + 4096, "logical gap/overlap"),
                                  (half - 4096, "logical gap/overlap"),
                                  (nor1["capacity_bytes"], "erase geometry or exceeds device")):
            bad = json.loads(json.dumps(synthetic))
            bad["partitions"]["filesystem"]["segments"][1]["logical_offset_bytes"] = bad_offset if error == "logical gap/overlap" else half
            if error != "logical gap/overlap":
                bad["partitions"]["filesystem"]["segments"][1]["device_offset_bytes"] = bad_offset
            with self.subTest(error=error, offset=bad_offset):
                with self.assertRaisesRegex(package.PackageError, error):
                    package._payload_plan(bad, "filesystem", half + 1)

    def test_external_payload_cannot_route_through_cnvm_even_when_partition_is_explicit(self):
        synthetic = json.loads(json.dumps(self.layout["layout"]))
        filesystem = synthetic["partitions"]["filesystem"]
        old = filesystem["segments"][0]
        filesystem["erase_bytes"] = 64 * 1024
        filesystem["segments"] = [
            {"logical_offset_bytes": 0, "device": "internal_flash",
             "device_offset_bytes": 1984 * 1024, "size_bytes": 64 * 1024},
            {"logical_offset_bytes": 64 * 1024, "device": "ospi0_flash",
             "device_offset_bytes": old["device_offset_bytes"],
             "size_bytes": filesystem["size_bytes"] - 64 * 1024},
        ]
        with self.assertRaisesRegex(package.PackageError, "indirect non-CNVM"):
            package._payload_plan(synthetic, "filesystem", 64 * 1024 + 1)

    def test_layout_mismatch_duplicate_zip_path_and_digest_corruption_fail(self):
        self._create()
        for field in ("sourceDigest", "normalizedLayoutDigest"):
            changed_layout = json.loads(json.dumps(self.layout))
            if field == "sourceDigest":
                changed_layout["source"][field] = "0" * 64
            else:
                changed_layout[field] = "0" * 64
            with self.subTest(field=field):
                with self.assertRaisesRegex(package.PackageError, "layout identity"):
                    package.validate_package(self.output, changed_layout)

        with zipfile.ZipFile(self.output) as source:
            members = {name: source.read(name) for name in source.namelist()}
        bad_hash = self.dir / "bad-hash_QIO.bin"
        members["artifacts/internal.bin"] += b"\xFF"
        with zipfile.ZipFile(bad_hash, "w") as archive:
            for name, data in members.items():
                archive.writestr(name, data)
        with self.assertRaisesRegex(package.PackageError, "digest/size mismatch"):
            package.validate_package(bad_hash, self.layout)

        duplicate = self.dir / "duplicate_QIO.bin"
        with zipfile.ZipFile(duplicate, "w") as archive:
            for name, data in members.items():
                archive.writestr(name, data)
            archive.writestr("manifest.json", members["manifest.json"])
        with self.assertRaisesRegex(package.PackageError, "duplicate ZIP"):
            package.validate_package(duplicate, self.layout)

        unsafe = self.dir / "unsafe_QIO.bin"
        with zipfile.ZipFile(unsafe, "w") as archive:
            for name, data in members.items():
                archive.writestr(name, data)
            archive.writestr("../escape", b"bad")
        with self.assertRaisesRegex(package.PackageError, "unsafe member path"):
            package.validate_package(unsafe, self.layout)

    def test_rehashed_padding_and_retargeted_segment_are_rejected_by_layout_rules(self):
        (self.dir / "payload.bin").write_bytes(b"four bytes")
        request = self.dir / "payload.csv"
        request.write_text("partition,file\nfilesystem,payload.bin\n")
        self._create(request)

        def rewrite(name, update):
            with zipfile.ZipFile(self.output) as source:
                members = {member: source.read(member) for member in source.namelist()}
            manifest = json.loads(members["manifest.json"])
            update(members, manifest)
            members["manifest.json"] = (json.dumps(manifest, sort_keys=True, indent=2) + "\n").encode()
            path = self.dir / name
            with zipfile.ZipFile(path, "w") as archive:
                for member, data in members.items():
                    archive.writestr(member, data)
            return path

        def corrupt_padding(members, manifest):
            member = "payloads/filesystem.bin"
            data = bytearray(members[member])
            data[-1] = 0
            members[member] = bytes(data)
            manifest["files"][member]["sha256"] = hashlib.sha256(data).hexdigest()
            manifest["external_payloads"][0]["sha256"] = hashlib.sha256(data).hexdigest()

        bad_pad = rewrite("bad-pad_QIO.bin", corrupt_padding)
        with self.assertRaisesRegex(package.PackageError, "padding must be 0xFF"):
            package.validate_package(bad_pad, self.layout)

        def retarget_segment(_members, manifest):
            manifest["external_payloads"][0]["segments"][0]["device_offset_bytes"] += 4096

        bad_route = rewrite("bad-route_QIO.bin", retarget_segment)
        with self.assertRaisesRegex(package.PackageError, "routing differs"):
            package.validate_package(bad_route, self.layout)


if __name__ == "__main__":
    unittest.main()
