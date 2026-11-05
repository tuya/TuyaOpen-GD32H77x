#!/usr/bin/env python3
"""Validate GD32H77X-owned configuration resolution and generated outputs."""

from pathlib import Path
import importlib.util
import re
import tempfile
import unittest


PLATFORM = Path(__file__).resolve().parents[1]
ROOT = PLATFORM.parents[1]
SPEC = importlib.util.spec_from_file_location(
    "gd32h77x_platform_config_under_test", PLATFORM / "platform_config.py")
platform_config = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(platform_config)


class PlatformConfigTests(unittest.TestCase):

    def test_h779_chip_default_selects_swd_backend(self):
        values = platform_config.resolve_config(PLATFORM, "GD32H779VW")
        self.assertEqual(
            values["CONFIG_ENABLE_GD32H77X_FLASH_BACKEND_SWD"], "y")
        self.assertEqual(platform_config.backend_from_config(values), "swd")

    def test_generated_header_and_cmake_match_resolved_values(self):
        with tempfile.TemporaryDirectory(prefix="gd32h77x-config-test-") as tmp:
            out = Path(tmp)
            values = platform_config.write_outputs(
                PLATFORM, "GD32H779VW", "GD32H779V_START", out)
            header = (out / "include/gd32h77x_config.h").read_text()
            cmake = (out / "gd32h77x_config.cmake").read_text()
            self.assertIn('#define GD32H77X_CONFIG_CHIP "GD32H779VW"', header)
            self.assertIn('#define GD32H77X_CONFIG_CANONICAL_CHIP "GD32H779VW"', header)
            self.assertIn("#define ENABLE_GD32H77X_FLASH_BACKEND_SWD 1", header)
            self.assertNotIn("#define GD32H77X_FLASH_BACKEND ", header)
            for removed in ("GD32H77X_FLASH_BACKEND", "GD32H77X_FLASH_DEVICE",
                            "GD32H77X_FLASH_SWD_PACK",
                            "GD32H77X_FLASH_UART_BAUD",
                            "GD32H77X_FLASH_VERIFY"):
                self.assertNotIn(f"#define {removed} ", header)
                self.assertNotIn(f"set(CONFIG_{removed} ", cmake)
            self.assertEqual(platform_config.backend_from_config(values), "swd")
            for expected in (
                    "#define GD32H77X_CONSOLE_USART USART5",
                    "#define GD32H77X_CONSOLE_USART_CLOCK RCU_USART5",
                    "#define GD32H77X_CONSOLE_GPIO_CLOCK RCU_GPIOC",
                    "#define GD32H77X_CONSOLE_GPIO_PORT GPIOC",
                    "#define GD32H77X_CONSOLE_TX_PIN GPIO_PIN_6",
                    "#define GD32H77X_CONSOLE_RX_PIN GPIO_PIN_7",
                    "#define GD32H77X_CONSOLE_AF GPIO_AF_7",
                    "#define GD32H77X_CONSOLE_IRQn USART5_IRQn",
                    "#define GD32H77X_CONSOLE_IRQHandler USART5_IRQHandler",
                    "#define AT_UART_IRQ_HANDLER UART4_IRQHandler",
                    "#define AT_RESET_CLOCK RCU_GPIOC",
                    "#define AT_RESET_PORT GPIOC",
                    "#define AT_RESET_PIN GPIO_PIN_5",
                    "#define AT_UART_USART UART4",
                    "#define AT_UART_CLOCK RCU_UART4",
                    "#define AT_UART_GPIO_CLOCK RCU_GPIOB",
                    "#define AT_UART_GPIO_PORT GPIOB",
                    "#define AT_UART_TX_PIN GPIO_PIN_13",
                    "#define AT_UART_RX_PIN GPIO_PIN_12",
                    "#define AT_UART_AF GPIO_AF_14",
                    "#define AT_UART_IRQn UART4_IRQn",
                    "#define AT_UART_DMA_RX_REQUEST DMA_REQUEST_UART4_RX",
                    "#define AT_UART_BAUD 115200",
                    "#define AT_UART_IRQ_PRIORITY 6"):
                self.assertIn(expected, header)
            self.assertIn("#define AT_UART_CONFIG_INITIALIZER", header)
            self.assertIn(".word_length = USART_WL_8BIT", header)
            self.assertIn(".parity = USART_PM_NONE", header)
            definitions = re.findall(r"^#define (\w+)", header, re.MULTILINE)
            self.assertEqual(len(definitions), len(set(definitions)))

    def test_missing_project_config_uses_platform_defaults(self):
        values = platform_config.resolve_config(PLATFORM, "GD32H779VW")
        self.assertEqual(
            values["CONFIG_ENABLE_GD32H77X_CONSOLE_UART_USART5"], "y")
        self.assertEqual(
            values["CONFIG_ENABLE_GD32H77X_AT_UART_UART4"], "y")
        self.assertEqual(
            values["CONFIG_ENABLE_GD32H77X_AT_RESET_GPIO_PORT_C"], "y")

    def test_console_and_at_cannot_share_uart(self):
        values = platform_config.resolve_config(PLATFORM, "GD32H779VW")
        values["CONFIG_ENABLE_GD32H77X_AT_UART_UART4"] = "n"
        values["CONFIG_ENABLE_GD32H77X_AT_UART_USART5"] = "y"
        with self.assertRaisesRegex(ValueError, "cannot share UART"):
            platform_config.hardware_bindings_from_config(values)

    def test_project_config_can_override_uart_and_derive_dma_request(self):
        with tempfile.TemporaryDirectory(prefix="gd32h77x-project-config-") as tmp:
            project_config = Path(tmp) / "project.config"
            project_config.write_text(
                "CONFIG_ENABLE_GD32H77X_AT_UART_UART6=y\n"
                "CONFIG_ENABLE_GD32H77X_AT_GPIO_PORT_K=y\n"
                "CONFIG_GD32H77X_AT_TX_PIN_NUMBER=1\n"
                "CONFIG_GD32H77X_AT_RX_PIN_NUMBER=2\n"
                "CONFIG_GD32H77X_AT_AF_NUMBER=3\n"
                "CONFIG_ENABLE_GD32H77X_AT_RESET_GPIO_PORT_A=y\n"
                "CONFIG_GD32H77X_AT_RESET_PIN_NUMBER=4\n")
            values = platform_config.resolve_config(
                PLATFORM, "GD32H779VW", project_config)
            bindings = platform_config.hardware_bindings_from_config(values)
            self.assertEqual(bindings["AT_UART_USART"], "UART6")
            self.assertEqual(bindings["AT_UART_CLOCK"], "RCU_UART6")
            self.assertEqual(
                bindings["AT_UART_DMA_RX_REQUEST"],
                "DMA_REQUEST_UART6_RX")
            self.assertEqual(bindings["AT_UART_GPIO_PORT"], "GPIOK")
            self.assertEqual(bindings["AT_UART_TX_PIN"], "GPIO_PIN_1")
            self.assertEqual(bindings["AT_UART_RX_PIN"], "GPIO_PIN_2")
            self.assertEqual(bindings["AT_RESET_PORT"], "GPIOA")
            self.assertEqual(bindings["AT_RESET_PIN"], "GPIO_PIN_4")

    def test_hardware_bindings_reject_duplicate_gpio_pin(self):
        values = platform_config.resolve_config(PLATFORM, "GD32H779VW")
        values["CONFIG_ENABLE_GD32H77X_AT_GPIO_PORT_B"] = "n"
        values["CONFIG_ENABLE_GD32H77X_AT_GPIO_PORT_C"] = "y"
        values["CONFIG_GD32H77X_AT_TX_PIN_NUMBER"] = "6"
        with self.assertRaisesRegex(ValueError, "same GPIO pin"):
            platform_config.hardware_bindings_from_config(values)

    def test_alias_uses_canonical_variant_directory(self):
        canonical, path = platform_config._chip_config(PLATFORM, "GD32H77D")
        self.assertEqual(canonical, "GD32H77DIW")
        self.assertEqual(path.parent.name, "GD32H77DIW")
        self.assertEqual(path.name, "GD32H77DIW.config")

    def test_every_registered_variant_has_exactly_one_config(self):
        import json
        registry = json.loads(
            (PLATFORM / "gd32h77x_os/chips/platform_chips.json").read_text())
        for chip, entry in registry["platform_chips"].items():
            with self.subTest(chip=chip):
                canonical, path = platform_config._chip_config(PLATFORM, chip)
                self.assertEqual(canonical, entry["target_chip"])
                self.assertEqual(path.parent.name, canonical)
                self.assertEqual(path.name, f"{canonical}.config")

    def test_project_override_has_highest_priority(self):
        with tempfile.TemporaryDirectory(prefix="gd32h77x-project-priority-") as tmp:
            project_config = Path(tmp) / "project.config"
            project_config.write_text(
                "CONFIG_GD32H77X_AT_BAUD=460800\n"
                "CONFIG_ENABLE_GD32H77X_FLASH_BACKEND_UART=y\n",
                encoding="utf-8")
            values = platform_config.resolve_config(
                PLATFORM, "GD32H779VW", project_config)
            self.assertEqual(values["CONFIG_GD32H77X_AT_BAUD"], "460800")
            self.assertEqual(
                values["CONFIG_ENABLE_GD32H77X_FLASH_BACKEND_UART"], "y")
            self.assertEqual(platform_config.backend_from_config(values), "uart")

    def test_missing_explicit_project_config_is_an_error(self):
        with self.assertRaisesRegex(ValueError, "Missing GD32H77X project config"):
            platform_config.resolve_config(
                PLATFORM, "GD32H779VW", PLATFORM / "missing-project.config")

    def test_unknown_chip_fails_without_implicit_fallback(self):
        with self.assertRaisesRegex(ValueError, "Unknown GD32H77X CHIP_CHOICE"):
            platform_config.resolve_config(PLATFORM, "GD32H77UNKNOWN")


if __name__ == "__main__":
    unittest.main()
