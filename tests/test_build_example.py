#!/usr/bin/env python3
"""Host tests for the GD32H77X package payload build switch."""

import importlib.util
from pathlib import Path
import tempfile
import unittest


PLATFORM = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "gd32h77x_build_firmware_under_test", PLATFORM / "build_firmware.py")
build_firmware = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(build_firmware)
SPEC = importlib.util.spec_from_file_location(
    "gd32h77x_build_example_under_test", PLATFORM / "build_example.py")
build_example = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(build_example)


class BuildExampleTests(unittest.TestCase):

    def test_tuyaopen_inputs_become_generic_project_context(self):
        with tempfile.TemporaryDirectory(prefix="gd32h77x-project-context-test-") as temporary:
            parameters = Path(temporary)
            project_config = parameters / "project/tuya/project.config"
            project_config.parent.mkdir(parents=True)
            project_config.write_text("CONFIG_GD32H77X_AT_BAUD=460800\n")
            build_example._write_project_context(parameters, {
                "OPEN_HEADER_DIR": "/headers/one /headers/two",
                "OPEN_LIBS_DIR": "/prebuilt",
                "PLATFORM_NEED_LIBS": "tuyaapp tuyaos",
            }, project_config)
            context = (parameters / "gd32h77x_project.cmake").read_text()
            self.assertIn('GD32H77X_PROJECT_DIR', context)
            self.assertIn('GD32H77X_PROJECT_CONFIG', context)
            self.assertIn(str(project_config), context)
            self.assertIn('/headers/one', context)
            self.assertIn('/headers/two', context)
            self.assertIn('/prebuilt/libtuyaapp.a', context)
            self.assertIn('/prebuilt/libtuyaos.a', context)

    def test_tuya_project_config_staging_is_build_local_and_stale_safe(self):
        with tempfile.TemporaryDirectory(prefix="gd32h77x-stage-test-") as temporary:
            root = Path(temporary)
            board = "TEST_BOARD"
            source = (root / "boards/GD32H77X" / board / "platform_config"
                      / "gd32h77x.config")
            source.parent.mkdir(parents=True)
            source.write_text("CONFIG_GD32H77X_AT_BAUD=115200\n")
            output = root / "build/project/tuya/project.config"
            self.assertEqual(
                build_example.stage_project_config(root, board, output), output)
            self.assertEqual(output.read_text(), source.read_text())

            source.write_text("CONFIG_GD32H77X_AT_BAUD=460800\n")
            build_example.stage_project_config(root, board, output)
            self.assertEqual(output.read_text(), source.read_text())

            source.unlink()
            self.assertIsNone(
                build_example.stage_project_config(root, board, output))
            self.assertFalse(output.exists())

    def test_application_overlay_is_board_specific_and_stale_safe(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            app = root / "app"
            override = app / "platform_config/BOARD.config"
            override.parent.mkdir(parents=True)
            override.write_text("CONFIG_ENABLE_GD32H77X_AT_TRANSPORT_SPI=y\n")
            output = root / "build/project.config"
            build_example.stage_project_config(root, "BOARD", output, app)
            self.assertEqual(output.read_text(), override.read_text())
            board = root / "boards/GD32H77X/BOARD/platform_config/gd32h77x.config"
            board.parent.mkdir(parents=True)
            board.write_text("CONFIG_GD32H77X_AT_BAUD=115200\n")
            build_example.stage_project_config(root, "BOARD", output, app)
            self.assertTrue(output.read_text().startswith(board.read_text()))
            self.assertTrue(output.read_text().endswith(override.read_text()))
            self.assertIsNone(build_example.stage_project_config(root, "OTHER", output, app))
            self.assertFalse(output.exists())
            override.unlink()
            build_example.stage_project_config(root, "BOARD", output, app)
            self.assertEqual(output.read_text(), board.read_text())

    def test_ecnvm_payload_mode_comes_from_resolved_layout(self):
        with tempfile.TemporaryDirectory(prefix="gd32h77x-build-example-test-") as temporary:
            layout = Path(temporary) / "resolved_layout.json"
            layout.write_text('{"layout":{"partitions":{}}}')
            self.assertFalse(build_firmware._layout_has_ecnvm_code(layout))
            layout.write_text(
                '{"layout":{"partitions":{"ecnvm_code":{"size_bytes":4096}}}}')
            self.assertTrue(build_firmware._layout_has_ecnvm_code(layout))
    def test_csv_presence_controls_external_payload_inclusion(self):
        with tempfile.TemporaryDirectory(prefix="gd32h77x-build-example-test-") as temporary:
            csv_path = Path(temporary) / "flash_payloads.csv"
            csv_path.write_text("partition,file\nstaging,payload.bin\n")
            args = ("python", "flash_package.py", "layout.json", "report.json",
                    "image.elf", "image.bin", "image_QIO.bin", csv_path)

            with_external = build_firmware.make_package_command(*args)
            self.assertIn("--external-csv", with_external)

            csv_path.unlink()
            internal_only = build_firmware.make_package_command(*args)
            self.assertNotIn("--external-csv", internal_only)
            for option in ("--layout", "--report", "--elf", "--internal-bin", "--output"):
                self.assertIn(option, internal_only)


if __name__ == "__main__":
    unittest.main()
