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

    def test_desk_buddy_routes_and_early_power_hold(self):
        values = platform_config.resolve_config(
            PLATFORM, "GD32H779IW",
            ROOT / "boards/GD32H77X/GD32H779I_DESK_BUDDY/platform_config/gd32h77x.config")
        bindings = platform_config.hardware_bindings_from_config(values)
        expected = {
            "GD32H77X_CONSOLE_USART": "USART0",
            "GD32H77X_CONSOLE_GPIO_PORT": "GPIOF",
            "GD32H77X_CONSOLE_AF": "GPIO_AF_4",
            "AT_SPI_PERIPH": "SPI1", "AT_SPI_CLOCK_PORT": "GPIOC",
            "AT_SPI_CLOCK_PIN": "GPIO_PIN_3",
            "AT_SPI_MISO_PORT": "GPIOC", "AT_SPI_MISO_PIN": "GPIO_PIN_2",
            "AT_SPI_MOSI_PORT": "GPIOC", "AT_SPI_MOSI_PIN": "GPIO_PIN_1",
            "AT_RESET_PORT": "GPIOH", "AT_RESET_PIN": "GPIO_PIN_10",
            "GD32H77X_POWER_HOLD_PORT": "GPIOC", "GD32H77X_POWER_HOLD_PIN": "GPIO_PIN_13",
            "GD32H77X_POWER_HOLD_LEVEL": "1",
        }
        for key, expected_value in expected.items():
            self.assertEqual(bindings[key], expected_value, key)
        values["CONFIG_ENABLE_GD32H77X_POWER_HOLD_ACTIVE_HIGH"] = "n"
        self.assertEqual(platform_config.hardware_bindings_from_config(values)["GD32H77X_POWER_HOLD_LEVEL"], "0")

    def test_power_hold_cannot_overlap_spi(self):
        values = self.spi_values()
        values["CONFIG_ENABLE_GD32H77X_POWER_HOLD"] = "y"
        values["CONFIG_GD32H77X_POWER_HOLD_GPIO"] = "PB13"
        with self.assertRaisesRegex(ValueError, "same GPIO pin"):
            platform_config.hardware_bindings_from_config(values)

    def test_start_keeps_hardware_spi_default(self):
        values = self.spi_values()
        bindings = platform_config.hardware_bindings_from_config(values)
        self.assertEqual(bindings["AT_SPI_MOSI_AF"], "GPIO_AF_5")
        self.assertEqual(bindings["AT_SPI_MOSI_PORT"], "GPIOB")
        self.assertEqual(bindings["AT_SPI_MOSI_PIN"], "GPIO_PIN_15")

    def spi_values(self):
        return platform_config.resolve_config(
            PLATFORM, "GD32H779VW",
            ROOT / "boards/GD32H77X/GD32H779V_START/platform_config/gd32h77x.config")

    def test_spi_routes_derive_controller_clock_and_dma(self):
        bindings = platform_config.hardware_bindings_from_config(self.spi_values())
        self.assertEqual(bindings["AT_SPI_PERIPH"], "SPI1")
        self.assertEqual(bindings["AT_SPI_CLOCK_SOURCE"], "RCU_SPISRC_PLL0Q")
        self.assertEqual(bindings["AT_SPI_DMA_RX_REQUEST"], "DMA_REQUEST_SPI1_RX")
        self.assertEqual(bindings["AT_SPI_REQUEST_PORT"], "GPIOC")
        self.assertEqual(bindings["AT_SPI_REQUEST_PIN"], "GPIO_PIN_0")
        self.assertNotIn("AT_SPI_READY_PORT", bindings)

    def test_spi_illegal_signal_is_rejected(self):
        values = self.spi_values()
        values["CONFIG_GD32H77X_AT_SPI_MOSI"] = "PB14"
        with self.assertRaisesRegex(ValueError, "unsupported route"):
            platform_config.hardware_bindings_from_config(values)

    def test_spi_duplicate_cs_and_signal_is_rejected(self):
        values = self.spi_values()
        values["CONFIG_GD32H77X_AT_SPI_CS"] = "PB13"
        with self.assertRaisesRegex(ValueError, "same GPIO pin"):
            platform_config.hardware_bindings_from_config(values)

    def test_spi_package_rejects_unbonded_normal_pc3(self):
        values = self.spi_values()
        values["CONFIG_GD32H77X_AT_SPI_SCK"] = "PC3"
        with self.assertRaisesRegex(ValueError, "not an ordinary bonded pad"):
            platform_config.hardware_bindings_from_config(values)
        values["CONFIG_GD32H77X_CANONICAL_CHIP"] = "GD32H779IW"
        bindings = platform_config.hardware_bindings_from_config(values)
        self.assertEqual(bindings["AT_SPI_CLOCK_AF"], "GPIO_AF_6")
        self.assertEqual(bindings["AT_SPI_MISO_PORT"], "GPIOB")

    def test_legacy_ready_clock_and_conflict(self):
        values = self.spi_values()
        values["CONFIG_ENABLE_GD32H77X_AT_SPI_CS_ONLY"] = "n"
        self.assertEqual(platform_config.hardware_bindings_from_config(values)[
            "AT_SPI_READY_CLOCK"], "RCU_GPIOE")
        values["CONFIG_GD32H77X_AT_SPI_READY"] = "PC5"
        with self.assertRaisesRegex(ValueError, "same GPIO pin"):
            platform_config.hardware_bindings_from_config(values)

    def test_uart_cross_port_and_different_af(self):
        values = platform_config.resolve_config(PLATFORM, "GD32H779IW")
        values["CONFIG_ENABLE_GD32H77X_AT_UART_UART4"] = "n"
        values["CONFIG_ENABLE_GD32H77X_AT_UART_USART0"] = "y"
        values["CONFIG_GD32H77X_AT_UART_TX_ROUTE"] = "PF4"
        values["CONFIG_GD32H77X_AT_UART_RX_ROUTE"] = "PA10"
        bindings = platform_config.hardware_bindings_from_config(values)
        self.assertEqual(bindings["AT_UART_TX_PORT"], "GPIOF")
        self.assertEqual(bindings["AT_UART_RX_PORT"], "GPIOA")
        self.assertEqual(bindings["AT_UART_TX_AF"], "GPIO_AF_4")
        self.assertEqual(bindings["AT_UART_RX_AF"], "GPIO_AF_7")

    def test_invalid_legacy_uart_af_is_rejected(self):
        values = platform_config.resolve_config(PLATFORM, "GD32H779VW")
        values["CONFIG_GD32H77X_AT_AF_NUMBER"] = "3"
        with self.assertRaisesRegex(ValueError, "invalid legacy AF"):
            platform_config.hardware_bindings_from_config(values)

    def test_special_c_pad_requires_separate_support(self):
        values = self.spi_values()
        values["CONFIG_GD32H77X_AT_SPI_MOSI"] = "PC3_C"
        with self.assertRaisesRegex(ValueError, "unsupported route"):
            platform_config.hardware_bindings_from_config(values)

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

    def test_board_pmu_topology_generates_vendor_define(self):
        with tempfile.TemporaryDirectory(prefix="gd32h77x-pmu-config-") as tmp:
            project_config = Path(tmp) / "project.config"
            project_config.write_text(
                'CONFIG_GD32H77X_PMU_SMPS_MODE="bypass"\n',
                encoding="utf-8")
            out = Path(tmp) / "out"
            platform_config.write_outputs(
                PLATFORM, "GD32H779IW", "GD32H779I_DESK_BUDDY", out,
                project_config)
            cmake = (out / "gd32h77x_config.cmake").read_text()
            header = (out / "include/gd32h77x_config.h").read_text()
        self.assertIn(
            'set(GD32H77X_PMU_DEFINE "SEL_PMU_SMPS_MODE=PMU_BYPASS")',
            cmake)
        self.assertIn('#define GD32H77X_PMU_SMPS_MODE "bypass"', header)

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
                'CONFIG_GD32H77X_AT_UART_TX_ROUTE="PE8"\n'
                'CONFIG_GD32H77X_AT_UART_RX_ROUTE="PE7"\n'
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
            self.assertEqual(bindings["AT_UART_TX_PORT"], "GPIOE")
            self.assertEqual(bindings["AT_UART_TX_PIN"], "GPIO_PIN_8")
            self.assertEqual(bindings["AT_UART_RX_PIN"], "GPIO_PIN_7")
            self.assertEqual(bindings["AT_RESET_PORT"], "GPIOA")
            self.assertEqual(bindings["AT_RESET_PIN"], "GPIO_PIN_4")

    def test_hardware_bindings_reject_duplicate_gpio_pin(self):
        values = platform_config.resolve_config(PLATFORM, "GD32H779VW")
        values["CONFIG_GD32H77X_AT_RESET_PIN_NUMBER"] = "6"
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


    def test_desk_buddy_psram_bindings(self):
        values = platform_config.resolve_config(
            PLATFORM, "GD32H779IW",
            ROOT / "boards/GD32H77X/GD32H779I_DESK_BUDDY/platform_config/gd32h77x.config")
        self.assertEqual(values["CONFIG_GD32H77X_PSRAM_ACCESS_INDIRECT"], "n")
        self.assertEqual(values["CONFIG_GD32H77X_PSRAM_ACCESS_MAPPED"], "y")
        bindings = platform_config.hardware_bindings_from_config(values)
        expected = {
            "GD32H77X_PSRAM_CS_GPIO_PORT": "GPIOE",
            "GD32H77X_PSRAM_CS_GPIO_PIN": "GPIO_PIN_11",
            "GD32H77X_PSRAM_CS_GPIO_AF": "GPIO_AF_11",
            "GD32H77X_PSRAM_SCK_GPIO_PORT": "GPIOB",
            "GD32H77X_PSRAM_SCK_GPIO_PIN": "GPIO_PIN_2",
            "GD32H77X_PSRAM_SCK_GPIO_AF": "GPIO_AF_9",
            "GD32H77X_PSRAM_IO0_GPIO_PIN": "GPIO_PIN_2",
            "GD32H77X_PSRAM_IO0_GPIO_AF": "GPIO_AF_6",
            "GD32H77X_PSRAM_IO1_GPIO_PIN": "GPIO_PIN_0",
            "GD32H77X_PSRAM_IO1_GPIO_AF": "GPIO_AF_4",
            "GD32H77X_PSRAM_IO2_GPIO_PORT": "GPIOA",
            "GD32H77X_PSRAM_IO2_GPIO_PIN": "GPIO_PIN_3",
            "GD32H77X_PSRAM_IO2_GPIO_AF": "GPIO_AF_6",
            "GD32H77X_PSRAM_IO3_GPIO_PIN": "GPIO_PIN_1",
            "GD32H77X_PSRAM_IO3_GPIO_AF": "GPIO_AF_9",
            "GD32H77X_PSRAM_OSPIM_PORT": "0",
            "GD32H77X_PSRAM_PA1_SWITCH_OPEN": "1",
            "GD32H77X_PSRAM_PC2_SWITCH_OPEN": "1",
            "GD32H77X_PSRAM_SIZE_BYTES": "8388608U",
            "GD32H77X_PSRAM_HEAP_OFFSET_BYTES": "4194304U",
            "GD32H77X_PSRAM_HEAP_SIZE_BYTES": "4194304U",
        }
        for key, expected_value in expected.items():
            self.assertEqual(bindings[key], expected_value, key)

    def test_h77ei_desk_buddy_eight_line_psram_bindings(self):
        values = platform_config.resolve_config(
            PLATFORM, "GD32H77EIW",
            ROOT / "boards/GD32H77X/GD32H77EI_DESK_BUDDY/platform_config/gd32h77x.config")
        bindings = platform_config.hardware_bindings_from_config(values)
        expected = {
            "GD32H77X_PSRAM_OCTAL": "1",
            "GD32H77X_PSRAM_CS_GPIO_PORT": "GPIOG",
            "GD32H77X_PSRAM_CS_GPIO_PIN": "GPIO_PIN_6",
            "GD32H77X_PSRAM_CS_GPIO_AF": "GPIO_AF_10",
            "GD32H77X_PSRAM_SCK_GPIO_PORT": "GPIOF",
            "GD32H77X_PSRAM_SCK_GPIO_PIN": "GPIO_PIN_10",
            "GD32H77X_PSRAM_SCK_GPIO_AF": "GPIO_AF_9",
            "GD32H77X_PSRAM_DQS_GPIO_PORT": "GPIOC",
            "GD32H77X_PSRAM_DQS_GPIO_PIN": "GPIO_PIN_5",
            "GD32H77X_PSRAM_DQS_GPIO_AF": "GPIO_AF_10",
            "GD32H77X_PSRAM_IO0_GPIO_PIN": "GPIO_PIN_8",
            "GD32H77X_PSRAM_IO0_GPIO_AF": "GPIO_AF_10",
            "GD32H77X_PSRAM_IO1_GPIO_PIN": "GPIO_PIN_9",
            "GD32H77X_PSRAM_IO1_GPIO_AF": "GPIO_AF_10",
            "GD32H77X_PSRAM_IO2_GPIO_PIN": "GPIO_PIN_7",
            "GD32H77X_PSRAM_IO2_GPIO_AF": "GPIO_AF_10",
            "GD32H77X_PSRAM_IO3_GPIO_PIN": "GPIO_PIN_3",
            "GD32H77X_PSRAM_IO3_GPIO_AF": "GPIO_AF_8",
            "GD32H77X_PSRAM_IO4_GPIO_PIN": "GPIO_PIN_4",
            "GD32H77X_PSRAM_IO4_GPIO_AF": "GPIO_AF_10",
            "GD32H77X_PSRAM_IO5_GPIO_PIN": "GPIO_PIN_5",
            "GD32H77X_PSRAM_IO5_GPIO_AF": "GPIO_AF_10",
            "GD32H77X_PSRAM_IO6_GPIO_PIN": "GPIO_PIN_6",
            "GD32H77X_PSRAM_IO6_GPIO_AF": "GPIO_AF_10",
            "GD32H77X_PSRAM_IO7_GPIO_PIN": "GPIO_PIN_7",
            "GD32H77X_PSRAM_IO7_GPIO_AF": "GPIO_AF_10",
            "GD32H77X_PSRAM_RESET_ENABLED": "1",
            "GD32H77X_PSRAM_RESET_GPIO_PORT": "GPIOB",
            "GD32H77X_PSRAM_RESET_GPIO_PIN": "GPIO_PIN_1",
            "GD32H77X_PSRAM_OSPIM_PORT": "0",
            "GD32H77X_PSRAM_SIZE_BYTES": "8388608U",
        }
        for key, expected_value in expected.items():
            self.assertEqual(bindings[key], expected_value, key)

    def test_eight_line_psram_requires_dqs(self):
        values = platform_config.resolve_config(
            PLATFORM, "GD32H77EIW",
            ROOT / "boards/GD32H77X/GD32H77EI_DESK_BUDDY/platform_config/gd32h77x.config")
        values["CONFIG_GD32H77X_PSRAM_DQS_PIN"] = ""
        with self.assertRaisesRegex(ValueError, "PSRAM DQS pin must not be empty"):
            platform_config.hardware_bindings_from_config(values)

    def test_psram_rejects_unverified_route(self):
        values = platform_config.resolve_config(
            PLATFORM, "GD32H779IW",
            ROOT / "boards/GD32H77X/GD32H779I_DESK_BUDDY/platform_config/gd32h77x.config")
        values["CONFIG_GD32H77X_PSRAM_CS_PIN"] = "PE10"
        with self.assertRaisesRegex(ValueError, "unsupported route"):
            platform_config.hardware_bindings_from_config(values)

    def test_psram_rejects_heap_outside_capacity(self):
        values = platform_config.resolve_config(
            PLATFORM, "GD32H779IW",
            ROOT / "boards/GD32H77X/GD32H779I_DESK_BUDDY/platform_config/gd32h77x.config")
        values["CONFIG_GD32H77X_PSRAM_HEAP_SIZE_KB"] = "8192"
        with self.assertRaisesRegex(ValueError, "exceeds configured PSRAM capacity"):
            platform_config.hardware_bindings_from_config(values)

    def test_board_without_psram_does_not_emit_psram_bindings(self):
        values = platform_config.resolve_config(PLATFORM, "GD32H779VW")
        bindings = platform_config.hardware_bindings_from_config(values)
        self.assertFalse(any("PSRAM" in key for key in bindings))


    def test_psram_pc2_c_conflicts_with_at_pc2(self):
        values = platform_config.resolve_config(
            PLATFORM, "GD32H779IW",
            ROOT / "boards/GD32H77X/GD32H779I_DESK_BUDDY/platform_config/gd32h77x.config")
        values["CONFIG_GD32H77X_AT_SPI_MOSI"] = "PC1"
        values["CONFIG_GD32H77X_AT_SPI_MISO"] = "PC2"
        values["CONFIG_GD32H77X_PSRAM_IO2_PIN"] = "PC2_C"
        with self.assertRaisesRegex(ValueError, "same GPIO pin PC2"):
            platform_config.hardware_bindings_from_config(values)


if __name__ == "__main__":
    unittest.main()
