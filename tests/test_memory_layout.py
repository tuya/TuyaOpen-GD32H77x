"""Focused validation tests for the GD32H77X CSV memory-layout resolver."""

import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
PLATFORM = ROOT / "platform" / "GD32H77X"
SCRIPT = PLATFORM / "gd32h77x_os/tools/firmware/resolve_memory_layout.py"
DEFAULT_PARTITIONS = PLATFORM / "gd32h77x_os" / "chips" / "GD32H77DIW" / "partitions.csv"
DEFAULT_DEVICES = PLATFORM / "gd32h77x_os" / "chips" / "GD32H77DIW" / "flash_devices.csv"
BOARD_DIR = ROOT / "boards" / "GD32H77X" / "GD32H77D_GINO"

spec = importlib.util.spec_from_file_location("gd32_layout_resolver", SCRIPT)
resolver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(resolver)


def _params(open_root, chip="GD32H77D", board="GD32H77D_GINO"):
    return {"OPEN_ROOT": str(open_root), "PLATFORM_CHIP": chip,
            "PLATFORM_BOARD": board, "CONFIG_PLATFORM_CHOICE": "GD32H77X",
            "CONFIG_BOARD_CHOICE": board, "CONFIG_CHIP_CHOICE": chip}


def _write_board(root, partitions=None, devices=None):
    board = root / "boards" / "GD32H77X" / "GD32H77D_GINO"
    board.mkdir(parents=True, exist_ok=True)
    if partitions is not None:
        (board / "partitions.csv").write_bytes(partitions)
    if devices is not None:
        (board / "flash_devices.csv").write_bytes(devices)
    return board


def _write_h779_board(root, partitions):
    board = root / "boards" / "GD32H77X" / "GD32H779V_START"
    board.mkdir(parents=True, exist_ok=True)
    (board / "partitions.csv").write_bytes(partitions)
    (board / "flash_devices.csv").write_bytes(
        (ROOT / "boards/GD32H77X/GD32H779V_START/flash_devices.csv").read_bytes())
    return board


def _resolve(root, chip="GD32H77D", board="GD32H77D_GINO"):
    return resolver.resolve_build_layout(_params(root, chip, board), platform_root=PLATFORM)


def _platform_copy(temp):
    """Copy the platform-owned chip configuration tree for resolver fixtures."""
    root = Path(temp) / "platform" / "GD32H77X"
    shutil.copytree(PLATFORM / "gd32h77x_os" / "chips", root / "gd32h77x_os" / "chips")
    return root


class MemoryLayoutResolverTests(unittest.TestCase):
    def test_h779_start_keeps_ecnvm_tail_for_runtime_kv(self):
        manifest = resolver.resolve_build_layout(
            _params(ROOT, "GD32H779VW", "GD32H779V_START"),
            platform_root=PLATFORM, boards_root=ROOT / "boards")
        layout = manifest["layout"]
        self.assertEqual(layout["partitions"]["ecnvm_code"]["logical_base"], 0x08200000)
        self.assertEqual(layout["partitions"]["ecnvm_code"]["size_bytes"], 0x700000)
        self.assertEqual(layout["partitions"]["ecnvm_code"]["role"], "data")
        self.assertEqual(layout["partitions"]["kv"]["logical_base"], 0x08900000)
        self.assertEqual(layout["partitions"]["kv"]["size_bytes"], 0x00080000)
        self.assertEqual(layout["partitions"]["kv"]["erase_bytes"], 0x1000)
        self.assertEqual(layout["partitions"]["kv"]["program_bytes"], 8)
        self.assertEqual(layout["partitions"]["cnvm_reserved"]["logical_base"], 0x08180000)
        self.assertEqual(layout["partitions"]["cnvm_reserved"]["size_bytes"], 0x00080000)
        self.assertIn({"name": "ECNVM", "origin": 0x08200000,
                       "length": 0x700000, "attributes": "rx"}, layout["linker_memory"])
        ecnvm = next(item for item in layout["devices"] if item["region"] == "ECNVM")
        self.assertEqual(ecnvm["erase_bytes"], 0x1000)
        self.assertEqual(ecnvm["page_bytes"], 0x1000)
        self.assertEqual(ecnvm["program_bytes"], 8)
        self.assertEqual(ecnvm["swd_erase_bytes"], 0x10000)
        self.assertEqual(ecnvm["swd_program_bytes"], 32)
        self.assertEqual(ecnvm["backend_status"], "implemented")
        self.assertEqual(layout["flash_image"]["end_address"], 0x08090000)
        subareas = {item["id"]: item for item in layout["tuya_subareas"]}
        self.assertEqual(set(subareas), {
            "tuya_uf", "tuya_kv_data", "tuya_kv_key",
            "tuya_kv_swap", "tuya_user0", "tuya_kv_protect",
        })
        self.assertEqual(subareas["tuya_uf"]["logical_base"], 0x08900000)
        self.assertEqual(subareas["tuya_uf"]["size_bytes"], 0x00040000)
        self.assertEqual(subareas["tuya_kv_data"]["logical_base"], 0x08940000)
        self.assertEqual(subareas["tuya_kv_data"]["size_bytes"], 0x00010000)
        self.assertEqual(subareas["tuya_user0"]["logical_base"], 0x08955000)
        self.assertEqual(subareas["tuya_user0"]["size_bytes"], 0x00020000)
        self.assertEqual(subareas["tuya_kv_protect"]["logical_base"], 0x08975000)
        self.assertEqual(subareas["tuya_kv_protect"]["size_bytes"], 0x00002000)

    def test_h779_ecnvm_code_and_kv_boundaries_are_fail_closed(self):
        source = (ROOT / "boards/GD32H77X/GD32H779V_START/partitions.csv").read_text()
        cases = (
            (source.replace("ecnvm_code,internal_ecnvm,0K,7168K,0x08200000",
                            "ecnvm_code,internal_ecnvm,0K,7164K,0x08200000"),
             "ecnvm_code must end immediately before the kv partition"),
            (source.replace("ecnvm_code,internal_ecnvm,0K,7168K,0x08200000",
                            "ecnvm_code,internal_ecnvm,0K,7424K,0x08200000"),
             "overlap in logical address space"),
            (source.replace("kv,internal_ecnvm,7168K,512K,0x08900000",
                            "kv,internal_ecnvm,7168K,256K,0x08900000"),
             "outside kv partition"),
            (source.replace("tuya_uf,internal_ecnvm,7168K,256K,0x08900000",
                            "tuya_uf,internal_ecnvm,7424K,256K,0x08940000"),
             "overlap"),
        )
        for partitions, error in cases:
            with self.subTest(error=error), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                _write_h779_board(root, partitions.encode())
                with self.assertRaisesRegex(resolver.LayoutError, error):
                    _resolve(root, "GD32H779VW", "GD32H779V_START")

    def test_all_pack_h77_devices_resolve_to_distinct_official_geometries(self):
        expected = {
            "GD32H77DIW": (2 * 1024 * 1024, 768 * 1024, "GD32H77x_78x_CNVM_2M.FLM"),
            "GD32H77DIP": (2 * 1024 * 1024, 768 * 1024, "GD32H77x_78x_CNVM_2M.FLM"),
            "GD32H77DII": (0x1A0000, 512 * 1024, "GD32H77x_78x_CNVM_1M_640K.FLM"),
            "GD32H77EIW": (2 * 1024 * 1024, 768 * 1024, "GD32H77x_78x_CNVM_2M.FLM"),
            "GD32H77EIP": (2 * 1024 * 1024, 768 * 1024, "GD32H77x_78x_CNVM_2M.FLM"),
            "GD32H77EII": (0x1A0000, 512 * 1024, "GD32H77x_78x_CNVM_1M_640K.FLM"),
            "GD32H779IW": (2 * 1024 * 1024, 768 * 1024, "GD32H77x_78x_CNVM_2M.FLM"),
            "GD32H779IP": (2 * 1024 * 1024, 768 * 1024, "GD32H77x_78x_CNVM_2M.FLM"),
            "GD32H779II": (0x1A0000, 512 * 1024, "GD32H77x_78x_CNVM_1M_640K.FLM"),
            "GD32H779VW": (2 * 1024 * 1024, 768 * 1024, "GD32H77x_78x_CNVM_2M.FLM"),
            "GD32H779VP": (2 * 1024 * 1024, 768 * 1024, "GD32H77x_78x_CNVM_2M.FLM"),
            "GD32H779VI": (0x1A0000, 512 * 1024, "GD32H77x_78x_CNVM_1M_640K.FLM"),
            "GD32H779ZW": (2 * 1024 * 1024, 768 * 1024, "GD32H77x_78x_CNVM_2M.FLM"),
            "GD32H779ZP": (2 * 1024 * 1024, 768 * 1024, "GD32H77x_78x_CNVM_2M.FLM"),
            "GD32H779ZI": (0x1A0000, 512 * 1024, "GD32H77x_78x_CNVM_1M_640K.FLM"),
            "GD32H77RAP": (2 * 1024 * 1024, 768 * 1024, "GD32H77x_78x_CNVM_2M.FLM"),
        }
        self.assertEqual(len(expected), 16)
        for chip, facts in expected.items():
            with self.subTest(chip=chip), tempfile.TemporaryDirectory() as temp:
                identity = resolver.resolve_chip_identity(chip, PLATFORM)
                self.assertEqual(identity["canonical_chip"], chip)
                self.assertEqual(identity["cnvm_capacity_bytes"], facts[0])
                self.assertEqual(identity["ram_capacity_bytes"], facts[1])
                self.assertEqual(identity["cnvm_vendor_algorithm"], facts[2])
                manifest = _resolve(Path(temp), chip, "GENERIC_BOARD")
                self.assertEqual(manifest["layout"]["target_chip"], chip)
                devices = {item["id"]: item for item in manifest["layout"]["devices"]}
                self.assertEqual(devices["internal_flash"]["capacity_bytes"], facts[0])
                self.assertEqual(manifest["layout"]["ota_policy"]["status"], "planned_not_implemented")
                internal_size = sum(
                    item["size_bytes"] for item in manifest["layout"]["partitions"].values()
                    if item["segments"] and all(segment["device"] == "internal_flash"
                                                for segment in item["segments"]))
                self.assertEqual(internal_size, facts[0])

    def test_family_alias_is_gino_only_and_exact_variant_works_for_new_board(self):
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(resolver.LayoutError, "only valid for board.*GD32H77D_GINO"):
                _resolve(Path(temp), "GD32H77D", "GENERIC_BOARD")
            manifest = _resolve(Path(temp), "GD32H77DIP", "GENERIC_BOARD")
        self.assertEqual(manifest["layout"]["target_chip"], "GD32H77DIP")
        self.assertIsNone(manifest["build"]["alias_of"])

    def test_platform_default_is_full_2m_ota_layout(self):
        with tempfile.TemporaryDirectory() as temp:
            manifest = _resolve(Path(temp))
        self.assertEqual(manifest["source"]["sourceKind"], "platform_default")
        parts = manifest["layout"]["partitions"]
        self.assertEqual(sum(item["size_bytes"] for item in parts.values()), 2 * 1024 * 1024)
        self.assertEqual(parts["primary"]["size_bytes"], 832 * 1024)
        self.assertEqual(parts["staging"]["size_bytes"], 832 * 1024)
        self.assertEqual(manifest["layout"]["flash_image"]["capacity_bytes"], 896 * 1024)
        self.assertEqual(len(manifest["source"]["sourceInputs"]), 2)
        devices = {item["id"]: item for item in manifest["layout"]["devices"]}
        self.assertEqual(devices["internal_flash"]["vendor_algorithm"],
                         "GD32H77x_78x_CNVM_2M.FLM")

    def test_geometry_must_be_approved_for_the_selected_chip(self):
        with tempfile.TemporaryDirectory() as temp:
            platform = _platform_copy(temp)
            index_path = platform / "gd32h77x_os" / "chips" / "platform_chips.json"
            index = json.loads(index_path.read_text())
            index["platform_chips"]["GD32H77D"]["approved_geometries"].remove("GD25Q64ESIG")
            index["platform_chips"]["GD32H77DIW"]["approved_geometries"].remove("GD25Q64ESIG")
            index_path.write_text(json.dumps(index))
            with self.assertRaisesRegex(resolver.LayoutError, "not approved"):
                resolver.resolve_build_layout(_params(ROOT), platform_root=platform)

    def test_approved_geometry_list_must_reference_registered_geometries(self):
        with tempfile.TemporaryDirectory() as temp:
            platform = _platform_copy(temp)
            index_path = platform / "gd32h77x_os" / "chips" / "platform_chips.json"
            index = json.loads(index_path.read_text())
            index["platform_chips"]["GD32H77D"]["approved_geometries"].append("GD32H77BOGUS_CNVM")
            index["platform_chips"]["GD32H77DIW"]["approved_geometries"].append("GD32H77BOGUS_CNVM")
            index_path.write_text(json.dumps(index))
            with self.assertRaisesRegex(resolver.LayoutError, "is not registered"):
                resolver.resolve_build_layout(_params(ROOT), platform_root=platform)

    def test_chip_flash_geometry_must_name_its_vendor_algorithm(self):
        with tempfile.TemporaryDirectory() as temp:
            platform = _platform_copy(temp)
            geometries_path = platform / "gd32h77x_os" / "chips" / "geometries.json"
            data = json.loads(geometries_path.read_text())
            del data["geometries"]["GD32H77DIW_CNVM"]["vendor_algorithm"]
            geometries_path.write_text(json.dumps(data))
            with self.assertRaisesRegex(resolver.LayoutError, "vendor_algorithm"):
                resolver.resolve_build_layout(_params(ROOT), platform_root=platform)

    def test_board_override_is_complete_and_reports_current_nor_capability(self):
        manifest = _resolve(ROOT)
        layout = manifest["layout"]
        devices = {item["id"]: item for item in layout["devices"]}
        self.assertEqual(manifest["source"]["sourceKind"], "board_override")
        self.assertEqual(set(devices), {"internal_flash", "ospi0_flash"})
        self.assertEqual(devices["ospi0_flash"]["backend_status"], "implemented")
        self.assertEqual(devices["ospi0_flash"]["program_bytes"], None)
        self.assertEqual(layout["partitions"]["primary"]["size_bytes"], 1984 * 1024)
        self.assertNotIn("backup", layout["ota_policy"])
        self.assertEqual(layout["ota_policy"]["install_strategy"], "copy_in_place")

    def test_generated_kv_macros_follow_nondefault_external_csv_partition(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            partitions = (BOARD_DIR / "partitions.csv").read_text()
            partitions = partitions.replace(
                "tuya,ospi0_flash,2176K,288K,0x70220000",
                "kv,ospi0_flash,4M,128K,0x70400000")
            partitions = partitions.replace(
                "filesystem,ospi0_flash,2464K,5728K,0x70268000",
                "filesystem,ospi0_flash,4224K,3968K,0x70420000")
            _write_board(root, partitions.encode("utf-8"),
                         (BOARD_DIR / "flash_devices.csv").read_bytes())
            manifest = _resolve(root)
            output = root / "generated"
            resolver.write_outputs(manifest, output)
            header = (output / "include" / "gd32h77x_memory_layout.h").read_text()

        self.assertIn("#define GD32H77X_LAYOUT_TUYA_LOGICAL_BASE UINT32_C(0x70400000)", header)
        self.assertIn("#define GD32H77X_LAYOUT_TUYA_SIZE_BYTES UINT32_C(0x00020000)", header)
        self.assertIn("#define GD32H77X_LAYOUT_TUYA_END UINT32_C(0x70420000)", header)
        self.assertIn("#define GD32H77X_LAYOUT_TUYA_ERASE_BYTES UINT32_C(0x00001000)", header)
        self.assertIn("#define GD32H77X_LAYOUT_TUYA_PROGRAM_BYTES 0U", header)

    def test_default_layout_uses_scratch_without_backup(self):
        with tempfile.TemporaryDirectory() as temp:
            layout = _resolve(Path(temp))["layout"]
        self.assertEqual(layout["ota_policy"]["scratch"], "scratch")
        self.assertNotIn("backup", layout["ota_policy"])
        self.assertEqual(layout["ota_policy"]["install_strategy"], "sector_swap")

    def test_board_hardware_table_changes_source_and_normalized_digest(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            partitions = (BOARD_DIR / "partitions.csv").read_bytes()
            devices = (BOARD_DIR / "flash_devices.csv").read_bytes()
            _write_board(root, partitions, devices)
            first = _resolve(root)
            changed = devices.replace(b"PC11", b"PC10")
            _write_board(root, partitions, changed)
            second = _resolve(root)
        self.assertNotEqual(first["source"]["sourceDigest"], second["source"]["sourceDigest"])
        self.assertNotEqual(first["normalizedLayoutDigest"], second["normalizedLayoutDigest"])

    def test_source_digest_excludes_absolute_paths(self):
        with tempfile.TemporaryDirectory() as one, tempfile.TemporaryDirectory() as two:
            _write_board(Path(one), (BOARD_DIR / "partitions.csv").read_bytes(),
                         (BOARD_DIR / "flash_devices.csv").read_bytes())
            _write_board(Path(two), (BOARD_DIR / "partitions.csv").read_bytes(),
                         (BOARD_DIR / "flash_devices.csv").read_bytes())
            a, b = _resolve(Path(one)), _resolve(Path(two))
        self.assertEqual(a["source"]["sourceDigest"], b["source"]["sourceDigest"])
        self.assertNotEqual(a["source"]["sourceInputs"][0]["sourcePath"],
                            b["source"]["sourceInputs"][0]["sourcePath"])

    def test_default_fallback_and_board_device_table_without_partitions_is_ignored(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            _write_board(root, None, (BOARD_DIR / "flash_devices.csv").read_bytes())
            manifest = _resolve(root)
        self.assertEqual(manifest["source"]["sourceKind"], "platform_default")
        self.assertEqual({dev["id"] for dev in manifest["layout"]["devices"]}, {"internal_flash"})

    def test_board_override_without_board_devices_cannot_reference_external_device(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            partitions = (BOARD_DIR / "partitions.csv").read_text().replace("ospi0_flash", "missing_device").encode()
            _write_board(root, partitions)
            with self.assertRaisesRegex(resolver.LayoutError, "unknown device"):
                _resolve(root)

    def test_board_override_uses_default_devices_with_explicit_provenance(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            _write_board(root, DEFAULT_PARTITIONS.read_bytes())
            manifest = _resolve(root)
        inputs = {item["kind"]: item for item in manifest["source"]["sourceInputs"]}
        self.assertEqual(inputs["partitions"]["sourceKind"], "board_override")
        self.assertEqual(inputs["flash_devices"]["sourceKind"], "platform_default")
        self.assertEqual(manifest["layout"]["ota_policy"]["install_strategy"], "sector_swap")
        self.assertEqual(manifest["layout"]["ota_policy"]["rollback"], "not_guaranteed")

    def test_csv_accepts_bom_crlf_comments_and_units(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            partitions = (BOARD_DIR / "partitions.csv").read_text()
            partitions = partitions.replace("boot,internal_flash,0,64K,0x08000000",
                                            "boot,internal_flash,0,65536,0x08000000")
            partitions = partitions.replace("primary,internal_flash,64K,1984K,0x08010000",
                                            "primary,internal_flash,65536,1984K,0x08010000")
            partitions = partitions.replace("\n", "\r\n")
            partitions = ("\ufeff# generated file\r\n\r\n" + partitions).encode("utf-8")
            _write_board(root, partitions, (BOARD_DIR / "flash_devices.csv").read_bytes())
            manifest = _resolve(root)
        self.assertEqual(manifest["layout"]["partitions"]["boot"]["size_bytes"], 65536)

    def test_csv_rejects_bad_header_extra_column_and_bad_number(self):
        good = (BOARD_DIR / "partitions.csv").read_bytes()
        devices = (BOARD_DIR / "flash_devices.csv").read_bytes()
        for bad, error in ((good.replace(b"logical_addr", b"logical_address"), "header"),
                           (good.replace(b"boot,internal_flash,0,64K,0x08000000", b"boot,internal_flash,0,64K,0x08000000,extra"), "columns"),
                           (good.replace(b"boot,internal_flash,0,64K,0x08000000", b'boot,internal_flash,0,64K,"0x08000000'), "CSV syntax"),
                           (good.replace(b"64K", b"-64K", 1), "cannot be negative"),
                           (good.replace(b"64K", b"12XB", 1), "decimal")):
            with self.subTest(error=error), tempfile.TemporaryDirectory() as temp:
                _write_board(Path(temp), bad, devices)
                with self.assertRaisesRegex(resolver.LayoutError, error):
                    _resolve(Path(temp))

    def test_duplicate_partition_rows_must_form_contiguous_logical_range(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            partitions = (BOARD_DIR / "partitions.csv").read_text()
            partitions += "filesystem,ospi0_flash,4M,2M,0x70400000\nfilesystem,ospi0_flash,6M,2M,0x70610000\n"
            _write_board(root, partitions.encode(), (BOARD_DIR / "flash_devices.csv").read_bytes())
            with self.assertRaisesRegex(resolver.LayoutError, "logical gap or overlap"):
                _resolve(root)

    def test_physical_overlap_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            partitions = (BOARD_DIR / "partitions.csv").read_text()
            partitions += "user_data,ospi0_flash,4M,4K,0x71000000\n"
            _write_board(root, partitions.encode(), (BOARD_DIR / "flash_devices.csv").read_bytes())
            with self.assertRaisesRegex(resolver.LayoutError, "overlap on device"):
                _resolve(root)

    def test_custom_data_roles_are_allowed_and_can_span_internal_and_nor(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            partitions = (BOARD_DIR / "partitions.csv").read_text()
            partitions = partitions.replace(
                "primary,internal_flash,64K,1984K,0x08010000",
                "primary,internal_flash,64K,1920K,0x08010000")
            partitions = partitions.replace(
                "filesystem,ospi0_flash,2464K,5728K,0x70268000\n", "")
            partitions += "user_data,internal_flash,1984K,64K,0x71000000\nuser_data,ospi0_flash,4M,4032K,0x71010000\n"
            _write_board(root, partitions.encode(), (BOARD_DIR / "flash_devices.csv").read_bytes())
            manifest = _resolve(root)
            user = manifest["layout"]["partitions"]["user_data"]
        self.assertEqual(user["role"], "data")
        self.assertEqual([segment["device"] for segment in user["segments"]], ["internal_flash", "ospi0_flash"])
        self.assertEqual(user["erase_bytes"], 65536)
        self.assertIsNone(user["program_bytes"])

    def test_linker_rom_abi_and_reserved_macro_guards(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            partitions = (BOARD_DIR / "partitions.csv").read_text()
            partitions = partitions.replace("boot,internal_flash,0,64K,0x08000000",
                                            "boot,internal_flash,64K,64K,0x08010000")
            partitions = partitions.replace("primary,internal_flash,64K,1984K,0x08010000",
                                            "primary,internal_flash,128K,1920K,0x08020000")
            _write_board(root, partitions.encode(), (BOARD_DIR / "flash_devices.csv").read_bytes())
            with self.assertRaisesRegex(resolver.LayoutError, "ROM boot ABI"):
                _resolve(root)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            partitions = (BOARD_DIR / "partitions.csv").read_text().replace("filesystem,ospi0_flash", "layout_primary,ospi0_flash")
            _write_board(root, partitions.encode(), (BOARD_DIR / "flash_devices.csv").read_bytes())
            with self.assertRaisesRegex(resolver.LayoutError, "reserved C macro"):
                _resolve(root)

    def test_invalid_or_dangling_override_fails_closed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            _write_board(root, b"wrong,header\n", (BOARD_DIR / "flash_devices.csv").read_bytes())
            with self.assertRaisesRegex(resolver.LayoutError, "header"):
                _resolve(root)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            board = root / "boards" / "GD32H77X" / "GD32H77D_GINO"
            board.mkdir(parents=True)
            try:
                (board / "partitions.csv").symlink_to(board / "missing.csv")
            except OSError as exc:
                self.skipTest("symlink creation unavailable: {}".format(exc))
            with self.assertRaisesRegex(resolver.LayoutError, "not a regular file"):
                _resolve(root)

    def test_generated_outputs_are_stable_and_csv_is_not_json(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            manifest = _resolve(root)
            out = root / "out"
            resolver.write_outputs(manifest, out)
            paths = [out / "resolved_layout.json", out / "gd32h77x_memory.ldinc",
                     out / "gd32h77x_memory_layout.c", out / "include/gd32h77x_memory_layout.h"]
            first = [(path.read_bytes(), path.stat().st_mtime_ns) for path in paths]
            resolver.write_outputs(manifest, out)
            second = [(path.read_bytes(), path.stat().st_mtime_ns) for path in paths]
        self.assertEqual(first, second)
        self.assertFalse(DEFAULT_PARTITIONS.with_suffix(".json").exists())
        self.assertFalse((BOARD_DIR / "flash_layout.json").exists())
        self.assertTrue((BOARD_DIR / "partitions.csv").is_file())
        self.assertTrue((BOARD_DIR / "flash_devices.csv").is_file())


if __name__ == "__main__":
    unittest.main()
