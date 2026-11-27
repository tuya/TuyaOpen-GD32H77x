#!/usr/bin/env python3
"""Resolve GD32H77X-owned configuration independently from TuyaOpen.

Inputs are the chip identity and an already-selected effective project config
fragment. The platform applies Kconfig defaults, chip defaults and that project
fragment. It does not discover boards or integration-specific config paths.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import kconfiglib

sys.path.insert(0, str(Path(__file__).resolve().parent))
from at_routes import UART_ROUTES, SPI_ROUTES, pin_binding, signal_binding, validate_package
from ospi_routes import ospi_pin_binding, ospi_signal_binding, validate_ospi_package


PLATFORM = "GD32H77X"
CHIPS_DIR = Path("gd32h77x_os") / "chips"
CHIP_REGISTRY = CHIPS_DIR / "platform_chips.json"

UART_OPTIONS = {
    "USART0": ("USART0", "RCU_USART0", "USART0_IRQn", "USART0_IRQHandler",
               "DMA_REQUEST_USART0_RX"),
    "USART1": ("USART1", "RCU_USART1", "USART1_IRQn", "USART1_IRQHandler",
               "DMA_REQUEST_USART1_RX"),
    "USART2": ("USART2", "RCU_USART2", "USART2_IRQn", "USART2_IRQHandler",
               "DMA_REQUEST_USART2_RX"),
    "UART3": ("UART3", "RCU_UART3", "UART3_IRQn", "UART3_IRQHandler",
              "DMA_REQUEST_UART3_RX"),
    "UART4": ("UART4", "RCU_UART4", "UART4_IRQn", "UART4_IRQHandler",
              "DMA_REQUEST_UART4_RX"),
    "USART5": ("USART5", "RCU_USART5", "USART5_IRQn", "USART5_IRQHandler",
               "DMA_REQUEST_USART5_RX"),
    "UART6": ("UART6", "RCU_UART6", "UART6_IRQn", "UART6_IRQHandler",
              "DMA_REQUEST_UART6_RX"),
    "UART7": ("UART7", "RCU_UART7", "UART7_IRQn", "UART7_IRQHandler",
              "DMA_REQUEST_UART7_RX"),
}

GPIO_PORT_OPTIONS = {
    "A": ("GPIOA", "RCU_GPIOA"),
    "B": ("GPIOB", "RCU_GPIOB"),
    "C": ("GPIOC", "RCU_GPIOC"),
    "D": ("GPIOD", "RCU_GPIOD"),
    "E": ("GPIOE", "RCU_GPIOE"),
    "F": ("GPIOF", "RCU_GPIOF"),
    "G": ("GPIOG", "RCU_GPIOG"),
    "H": ("GPIOH", "RCU_GPIOH"),
    "J": ("GPIOJ", "RCU_GPIOJ"),
    "K": ("GPIOK", "RCU_GPIOK"),
}


def _chip_config(platform_root: Path, chip: str) -> tuple[str, Path]:
    registry_path = platform_root / CHIP_REGISTRY
    try:
        registry = json.loads(registry_path.read_text(encoding="utf-8"))
        entry = registry["platform_chips"][chip]
    except (OSError, KeyError, json.JSONDecodeError) as exc:
        raise ValueError(
            f"Unknown GD32H77X CHIP_CHOICE={chip} in {registry_path}") from exc

    canonical_chip = entry.get("target_chip")
    if not canonical_chip:
        raise ValueError(
            f"CHIP_CHOICE={chip} has no canonical target_chip")
    chip_dir = platform_root / CHIPS_DIR / canonical_chip
    chip_config = chip_dir / f"{canonical_chip}.config"
    if not chip_config.is_file():
        raise ValueError(
            f"Missing variant platform config: {chip_config}")
    return canonical_chip, chip_config


def config_sources(platform_root: Path, chip: str,
                   project_config: Path | None) -> tuple[Path, Path, Path | None, str]:
    """Return Kconfig, chip default, optional project override and canonical chip."""
    kconfig = platform_root / "Kconfig"
    canonical_chip, chip_config = _chip_config(platform_root, chip)
    if not kconfig.is_file():
        raise ValueError(f"Missing GD32H77X platform Kconfig: {kconfig}")
    if project_config is not None:
        project_config = project_config.resolve()
        if not project_config.is_file():
            raise ValueError(f"Missing GD32H77X project config: {project_config}")
    return kconfig, chip_config, project_config, canonical_chip


def resolve_config(platform_root: Path, chip: str,
                   project_config: Path | None = None) -> dict[str, str]:
    """Resolve Kconfig defaults, chip defaults and an optional project override."""
    kconfig, chip_config, project_config, _canonical_chip = config_sources(
        platform_root, chip, project_config)
    kconf = kconfiglib.Kconfig(
        str(kconfig), warn_to_stderr=False, suppress_traceback=True)
    kconf.load_config(str(chip_config), replace=True)
    if project_config is not None:
        if "CONFIG_ENABLE_GD32H77X_AT_SPI_START_DESK_SIM=y" in project_config.read_text():
            raise ValueError("Removed AT SPI board preset: configure GD32H77X_AT_SPI_NUMBER, "
                             "SCK, MISO, MOSI and CS routes explicitly")
        kconf.load_config(str(project_config), replace=False)

    values = {
        f"CONFIG_{sym.name}": sym.str_value
        for sym in kconf.unique_defined_syms
        if sym.config_string
    }
    values["CONFIG_GD32H77X_CANONICAL_CHIP"] = _canonical_chip
    return values


def backend_from_config(values: dict[str, str]) -> str:
    """Derive the selected streaming backend from the Kconfig choice."""
    selected = [
        backend for backend, symbol in (
            ("swd", "CONFIG_ENABLE_GD32H77X_FLASH_BACKEND_SWD"),
            ("dfu", "CONFIG_ENABLE_GD32H77X_FLASH_BACKEND_DFU"),
            ("uart", "CONFIG_ENABLE_GD32H77X_FLASH_BACKEND_UART"),
        ) if values.get(symbol) == "y"
    ]
    if len(selected) != 1:
        raise ValueError(
            "Exactly one ENABLE_GD32H77X_FLASH_BACKEND_* choice must be selected")
    return selected[0]


def pmu_define_from_config(values: dict[str, str]) -> str:
    """Map the board VCORE topology to the vendor system-init define."""
    mode = values.get("CONFIG_GD32H77X_PMU_SMPS_MODE", "").strip().lower()
    defines = {
        "": "",
        "bypass": "SEL_PMU_SMPS_MODE=PMU_BYPASS",
        "direct": "SEL_PMU_SMPS_MODE=PMU_DIRECT_SMPS_SUPPLY",
    }
    if mode not in defines:
        raise ValueError(
            "GD32H77X_PMU_SMPS_MODE must be empty, 'bypass', or 'direct', "
            f"got {mode!r}")
    return defines[mode]


def _selected_option(values: dict[str, str], prefix: str,
                     options: dict[str, tuple[str, ...]],
                     description: str) -> str:
    selected = [
        name for name in options
        if values.get(f"CONFIG_{prefix}_{name}") == "y"
    ]
    if len(selected) != 1:
        raise ValueError(
            f"Exactly one {description} choice must be selected")
    return selected[0]


def _physical_pin_key(pin: str) -> str:
    """Collapse paired analog-switch pads onto one physical pin key."""
    return pin[:-2] if pin.endswith("_C") else pin


def _config_int(values: dict[str, str], symbol: str, minimum: int,
                maximum: int) -> int:
    raw = values.get(f"CONFIG_{symbol}")
    try:
        value = int(raw) if raw is not None else None
    except ValueError as exc:
        raise ValueError(f"{symbol} must be an integer, got {raw!r}") from exc
    if value is None or not minimum <= value <= maximum:
        raise ValueError(
            f"{symbol} must be in range {minimum}..{maximum}, got {raw!r}")
    return value


def _psram_bindings(values: dict[str, str], pins: dict[str, str],
                    extra: dict[str, str]) -> None:
    """Resolve and validate GD32H77X PSRAM pin bindings."""
    if values.get("CONFIG_ENABLE_GD32H77X_PSRAM") != "y":
        return

    port_values = [
        port for port in (0, 1)
        if values.get(f"CONFIG_GD32H77X_PSRAM_OSPIM_PORT{port}") == "y"
    ]
    if len(port_values) != 1:
        raise ValueError("Exactly one PSRAM OSPIM port must be selected")
    port = port_values[0]

    profile = values.get("CONFIG_GD32H77X_PSRAM_PROFILE", "")
    if not profile:
        raise ValueError("GD32H77X_PSRAM_PROFILE must not be empty")
    octal = profile.startswith("APS6408L")

    access_modes = [
        mode for mode, symbol in (
            ("indirect", "CONFIG_GD32H77X_PSRAM_ACCESS_INDIRECT"),
            ("mapped", "CONFIG_GD32H77X_PSRAM_ACCESS_MAPPED"),
        ) if values.get(symbol) == "y"
    ]
    if len(access_modes) != 1:
        raise ValueError("Exactly one PSRAM access mode must be selected")

    size_mb = _config_int(values, "GD32H77X_PSRAM_SIZE_MB", 1, 8)
    heap_offset_kb = _config_int(
        values, "GD32H77X_PSRAM_HEAP_OFFSET_KB", 0, 8192)
    heap_size_kb = _config_int(
        values, "GD32H77X_PSRAM_HEAP_SIZE_KB", 1, 8192)
    _config_int(values, "GD32H77X_PSRAM_CLOCK_HZ", 1000000, 84000000)
    if heap_offset_kb + heap_size_kb > size_mb * 1024:
        raise ValueError(
            "PSRAM heap offset + size exceeds configured PSRAM capacity")

    signals = ["CS", "SCK", "IO0", "IO1", "IO2", "IO3"]
    if octal:
        signals.extend(("IO4", "IO5", "IO6", "IO7", "DQS"))

    psram_pins: list[str] = []
    for signal in signals:
        pin = values.get(f"CONFIG_GD32H77X_PSRAM_{signal}_PIN", "")
        if not pin:
            raise ValueError(f"PSRAM {signal} pin must not be empty")
        binding = ospi_signal_binding(port, signal, pin)
        for suffix, token in binding.items():
            extra[f"GD32H77X_PSRAM_{signal}_GPIO_{suffix}"] = token
        pins[f"PSRAM {signal}"] = pin
        psram_pins.append(pin)

    reset_pin = values.get("CONFIG_GD32H77X_PSRAM_RESET_PIN", "")
    if reset_pin:
        for suffix, token in ospi_pin_binding(reset_pin).items():
            extra[f"GD32H77X_PSRAM_RESET_GPIO_{suffix}"] = token
        psram_pins.append(reset_pin)

    validate_ospi_package(
        values.get("CONFIG_GD32H77X_PSRAM_CANONICAL_CHIP", ""), psram_pins)

    selected = set(psram_pins)
    extra["GD32H77X_PSRAM_OSPIM_PORT"] = str(port)
    extra["GD32H77X_PSRAM_OCTAL"] = "1" if octal else "0"
    extra["GD32H77X_PSRAM_RESET_ENABLED"] = "1" if reset_pin else "0"

    # Keep every generated C token defined so the controller can use one
    # unconditional initializer for both four-line and eight-line devices.
    for signal in ("IO4", "IO5", "IO6", "IO7", "DQS"):
        for suffix in ("PORT", "CLOCK", "PIN", "AF"):
            extra.setdefault(f"GD32H77X_PSRAM_{signal}_GPIO_{suffix}", "0U")
    for suffix in ("PORT", "CLOCK", "PIN"):
        extra.setdefault(f"GD32H77X_PSRAM_RESET_GPIO_{suffix}", "0U")
    extra["GD32H77X_PSRAM_MAPPED_BASE"] = "0x90000000U"
    extra["GD32H77X_PSRAM_SIZE_BYTES"] = f"{size_mb * 1024 * 1024}U"
    extra["GD32H77X_PSRAM_HEAP_OFFSET_BYTES"] = f"{heap_offset_kb * 1024}U"
    extra["GD32H77X_PSRAM_HEAP_SIZE_BYTES"] = f"{heap_size_kb * 1024}U"

    extra["GD32H77X_PSRAM_PA1_SWITCH_SET"] = "1" if "PA1" in selected else "0"
    extra["GD32H77X_PSRAM_PA1_SWITCH_OPEN"] = "1"
    extra["GD32H77X_PSRAM_PC2_SWITCH_SET"] = (
        "1" if selected.intersection({"PC2", "PC2_C"}) else "0")
    extra["GD32H77X_PSRAM_PC2_SWITCH_OPEN"] = (
        "0" if "PC2_C" in selected else "1")


def hardware_bindings_from_config(values: dict[str, str]) -> dict[str, str]:
    """Map Kconfig hardware choices to the C tokens consumed by the platform."""
    console_uart = _selected_option(
        values, "ENABLE_GD32H77X_CONSOLE_UART", UART_OPTIONS,
        "console UART")
    console_port = _selected_option(
        values, "ENABLE_GD32H77X_CONSOLE_GPIO_PORT", GPIO_PORT_OPTIONS,
        "console GPIO port")
    at_uart = _selected_option(
        values, "ENABLE_GD32H77X_AT_UART", UART_OPTIONS,
        "AT UART")
    at_port = _selected_option(
        values, "ENABLE_GD32H77X_AT_GPIO_PORT", GPIO_PORT_OPTIONS,
        "AT UART GPIO port")
    reset_port = _selected_option(
        values, "ENABLE_GD32H77X_AT_RESET_GPIO_PORT", GPIO_PORT_OPTIONS,
        "AT reset GPIO port")

    console_tx = _config_int(values, "GD32H77X_CONSOLE_TX_PIN_NUMBER", 0, 15)
    console_rx = _config_int(values, "GD32H77X_CONSOLE_RX_PIN_NUMBER", 0, 15)
    console_af = _config_int(values, "GD32H77X_CONSOLE_AF_NUMBER", 0, 31)
    at_tx = _config_int(values, "GD32H77X_AT_TX_PIN_NUMBER", 0, 15)
    at_rx = _config_int(values, "GD32H77X_AT_RX_PIN_NUMBER", 0, 15)
    at_af = _config_int(values, "GD32H77X_AT_AF_NUMBER", 0, 31)
    at_baud = _config_int(values, "GD32H77X_AT_BAUD", 1200, 4000000)
    at_priority = _config_int(
        values, "GD32H77X_AT_IRQ_PRIORITY", 5, 15)
    reset_pin = _config_int(values, "GD32H77X_AT_RESET_PIN_NUMBER", 0, 15)

    spi_active = values.get("CONFIG_ENABLE_GD32H77X_AT_TRANSPORT_SPI") == "y"
    for signal, pin in (("TX", console_tx), ("RX", console_rx)):
        binding = signal_binding(UART_ROUTES, console_uart, signal, f"P{console_port}{pin}")
        if binding["AF"] != f"GPIO_AF_{console_af}":
            raise ValueError(f"Console {signal}: invalid AF {console_af}; expected {binding['AF']}")
    if not spi_active and console_uart == at_uart:
        raise ValueError(f"Console and AT cannot share UART peripheral {console_uart}")
    tx_route = values.get("CONFIG_GD32H77X_AT_UART_TX_ROUTE") or f"P{at_port}{at_tx}"
    rx_route = values.get("CONFIG_GD32H77X_AT_UART_RX_ROUTE") or f"P{at_port}{at_rx}"
    uart_tx = ({**pin_binding(tx_route), "AF": f"GPIO_AF_{at_af}"} if spi_active else
               signal_binding(UART_ROUTES, at_uart, "TX", tx_route))
    uart_rx = ({**pin_binding(rx_route), "AF": f"GPIO_AF_{at_af}"} if spi_active else
               signal_binding(UART_ROUTES, at_uart, "RX", rx_route))
    if not spi_active:
        for signal, route, binding in (("TX", tx_route, uart_tx), ("RX", rx_route, uart_rx)):
            if not values.get(f"CONFIG_GD32H77X_AT_UART_{signal}_ROUTE") and binding["AF"] != f"GPIO_AF_{at_af}":
                raise ValueError(f"AT {signal} {route}: invalid legacy AF {at_af}; expected {binding['AF']}")

    pins = {"console TX": f"P{console_port}{console_tx}",
            "console RX": f"P{console_port}{console_rx}",
            "AT reset": f"P{reset_port}{reset_pin}"}
    extra = {}
    if spi_active:
        number = _config_int(values, "GD32H77X_AT_SPI_NUMBER", 0, 5)
        for signal in ("SCK", "MISO", "MOSI"):
            pin = values[f"CONFIG_GD32H77X_AT_SPI_{signal}"]
            pins[f"SPI {signal}"] = pin
            prefix = "CLOCK" if signal == "SCK" else signal
            binding = signal_binding(SPI_ROUTES, number, signal, pin)
            for suffix, token in binding.items():
                extra[f"AT_SPI_{prefix}_{suffix}"] = token
        for signal, prefix in (("CS", "REQUEST"), ("READY", "READY")):
            if signal == "READY" and values.get("CONFIG_ENABLE_GD32H77X_AT_SPI_CS_ONLY") == "y":
                continue
            pin = values[f"CONFIG_GD32H77X_AT_SPI_{signal}"]
            pins[f"SPI {signal}"] = pin
            for suffix, token in pin_binding(pin).items():
                extra[f"AT_SPI_{prefix}_{suffix}"] = token
        extra.update({"AT_SPI_PERIPH": f"SPI{number}", "AT_SPI_INDEX": f"IDX_SPI{number}",
                      "AT_SPI_CLOCK_SOURCE": "RCU_SPISRC_PLL0Q" if number < 3 else "RCU_SPISRC_APB2",
                      "AT_SPI_CLOCK_GATE": f"RCU_SPI{number}",
                      "AT_SPI_RESET_GATE": f"RCU_SPI{number}RST",
                      "AT_SPI_DMA_TX_REQUEST": f"DMA_REQUEST_SPI{number}_TX",
                      "AT_SPI_DMA_RX_REQUEST": f"DMA_REQUEST_SPI{number}_RX"})
    else:
        pins.update({"AT TX": tx_route, "AT RX": rx_route})
    if values.get("CONFIG_ENABLE_GD32H77X_POWER_HOLD") == "y":
        pin = values["CONFIG_GD32H77X_POWER_HOLD_GPIO"]
        pins["power hold"] = pin
        for suffix, token in pin_binding(pin).items():
            extra[f"GD32H77X_POWER_HOLD_{suffix}"] = token
        extra["GD32H77X_POWER_HOLD_LEVEL"] = (
            "1" if values.get("CONFIG_ENABLE_GD32H77X_POWER_HOLD_ACTIVE_HIGH") == "y" else "0")
    _psram_bindings(values, pins, extra)
    validate_package(values.get("CONFIG_GD32H77X_CANONICAL_CHIP", ""), pins.values())
    used_pins = {}
    for label, pin in pins.items():
        key = _physical_pin_key(pin)
        previous = used_pins.get(key)
        if previous is not None:
            raise ValueError(
                f"{label} and {previous} use the same GPIO pin "
                f"{key}")
        used_pins[key] = label

    console_usart, console_clock, console_irq, console_handler, _console_dma = \
        UART_OPTIONS[console_uart]
    at_usart, at_clock, at_irq, at_handler, at_dma_rx = \
        UART_OPTIONS[at_uart]
    console_gpio, console_gpio_clock = GPIO_PORT_OPTIONS[console_port]
    at_gpio, at_gpio_clock = GPIO_PORT_OPTIONS[at_port]
    reset_gpio, reset_clock = GPIO_PORT_OPTIONS[reset_port]

    bindings = {
        "GD32H77X_CONSOLE_USART": console_usart,
        "GD32H77X_CONSOLE_USART_CLOCK": console_clock,
        "GD32H77X_CONSOLE_GPIO_CLOCK": console_gpio_clock,
        "GD32H77X_CONSOLE_GPIO_PORT": console_gpio,
        "GD32H77X_CONSOLE_TX_PIN": f"GPIO_PIN_{console_tx}",
        "GD32H77X_CONSOLE_RX_PIN": f"GPIO_PIN_{console_rx}",
        "GD32H77X_CONSOLE_AF": f"GPIO_AF_{console_af}",
        "GD32H77X_CONSOLE_IRQn": console_irq,
        "GD32H77X_CONSOLE_IRQHandler": console_handler,
        "AT_UART_IRQ_HANDLER": at_handler,
        "AT_RESET_CLOCK": reset_clock,
        "AT_RESET_PORT": reset_gpio,
        "AT_RESET_PIN": f"GPIO_PIN_{reset_pin}",
        "AT_UART_USART": at_usart,
        "AT_UART_CLOCK": at_clock,
        "AT_UART_GPIO_CLOCK": at_gpio_clock,
        "AT_UART_GPIO_PORT": at_gpio,
        "AT_UART_TX_PIN": f"GPIO_PIN_{at_tx}",
        "AT_UART_RX_PIN": f"GPIO_PIN_{at_rx}",
        "AT_UART_AF": f"GPIO_AF_{at_af}",
        "AT_UART_IRQn": at_irq,
        "AT_UART_DMA_RX_REQUEST": at_dma_rx,
        "AT_UART_BAUD": str(at_baud),
        "AT_UART_IRQ_PRIORITY": str(at_priority),
    }
    for signal, route in (("TX", uart_tx), ("RX", uart_rx)):
        for suffix, token in route.items():
            bindings[f"AT_UART_{signal}_{suffix}"] = token
    bindings.update(extra)
    # Compatibility aliases describe TX; RX has its own independent binding.
    bindings["AT_UART_GPIO_PORT"] = uart_tx["PORT"]
    bindings["AT_UART_GPIO_CLOCK"] = uart_tx["CLOCK"]
    bindings["AT_UART_AF"] = uart_tx["AF"]
    return bindings


def _c_macro_value(sym) -> str:
    if sym.type in (kconfiglib.BOOL, kconfiglib.TRISTATE):
        return "1" if sym.str_value == "y" else "0"
    if sym.type == kconfiglib.STRING:
        return json.dumps(sym.str_value, ensure_ascii=False)
    return sym.str_value


def write_outputs(platform_root: Path, chip: str, board: str,
                  out_dir: Path, project_config: Path | None = None) -> dict[str, str]:
    """Write platform .config, C header, CMake fragment and JSON snapshot."""
    values = resolve_config(platform_root, chip, project_config)
    bindings = hardware_bindings_from_config(values)
    kconfig, chip_config, project_config, canonical_chip = config_sources(
        platform_root, chip, project_config)

    kconf = kconfiglib.Kconfig(
        str(kconfig), warn_to_stderr=False, suppress_traceback=True)
    kconf.load_config(str(chip_config), replace=True)
    if project_config is not None:
        kconf.load_config(str(project_config), replace=False)

    out_dir.mkdir(parents=True, exist_ok=True)
    include_dir = out_dir / "include"
    include_dir.mkdir(parents=True, exist_ok=True)

    kconf.write_config(str(out_dir / "gd32h77x_platform.config"),
                       save_old=False)
    (out_dir / "gd32h77x_config.json").write_text(
        json.dumps(values, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")

    cmake_lines = [
        "# AUTO-GENERATED GD32H77X platform configuration.",
        f'set(GD32H77X_CONFIG_CHIP "{chip}")',
        f'set(GD32H77X_CONFIG_CANONICAL_CHIP "{canonical_chip}")',
        f'set(GD32H77X_CONFIG_BOARD "{board}")',
    ]
    pmu_define = pmu_define_from_config(values)
    if pmu_define:
        cmake_lines.append(f'set(GD32H77X_PMU_DEFINE "{pmu_define}")')
    for sym in sorted(kconf.unique_defined_syms, key=lambda item: item.name):
        if not sym.config_string:
            continue
        if sym.type in (kconfiglib.BOOL, kconfiglib.TRISTATE):
            value = sym.str_value
        elif sym.type == kconfiglib.STRING:
            escaped = sym.str_value.replace("\\", "\\\\").replace('"', '\\"')
            value = f'"{escaped}"'
        else:
            value = sym.str_value
        cmake_lines.append(f"set(CONFIG_{sym.name} {value})")
    (out_dir / "gd32h77x_config.cmake").write_text(
        "\n".join(cmake_lines) + "\n", encoding="utf-8")

    header_lines = [
        "/* AUTO-GENERATED GD32H77X PLATFORM CONFIG. DO NOT EDIT. */",
        "#ifndef GD32H77X_CONFIG_H",
        "#define GD32H77X_CONFIG_H",
        "",
        f'#define GD32H77X_CONFIG_CHIP "{chip}"',
        f'#define GD32H77X_CONFIG_CANONICAL_CHIP "{canonical_chip}"',
        f'#define GD32H77X_CONFIG_BOARD "{board}"',
        "",
    ]
    for sym in sorted(kconf.unique_defined_syms, key=lambda item: item.name):
        if not sym.config_string:
            continue
        header_lines.append(
            f"#define {sym.name} {_c_macro_value(sym)}")

    header_lines.extend([
        "",
        "/* Hardware bindings derived from the platform Kconfig choices. */",
    ])
    for name in sorted(bindings):
        header_lines.append(f"#define {name} {bindings[name]}")

    header_lines.extend([
        "#define AT_UART_CONFIG_INITIALIZER \\",
        "    { \\",
        f"        .usart = {bindings['AT_UART_USART']}, \\",
        f"        .usart_clock = {bindings['AT_UART_CLOCK']}, \\",
        f"        .gpio_clock = {bindings['AT_UART_TX_CLOCK']}, \\",
        f"        .gpio_port = {bindings['AT_UART_TX_PORT']}, \\",
        f"        .rx_gpio_clock = {bindings['AT_UART_RX_CLOCK']}, \\",
        f"        .rx_gpio_port = {bindings['AT_UART_RX_PORT']}, \\",
        f"        .rx_alternate_function = {bindings['AT_UART_RX_AF']}, \\",
        f"        .tx_pin = {bindings['AT_UART_TX_PIN']}, \\",
        f"        .rx_pin = {bindings['AT_UART_RX_PIN']}, \\",
        f"        .alternate_function = {bindings['AT_UART_TX_AF']}, \\",
        f"        .irq = {bindings['AT_UART_IRQn']}, \\",
        f"        .baudrate = {bindings['AT_UART_BAUD']}U, \\",
        "        .word_length = USART_WL_8BIT, \\",
        "        .stop_bits = USART_STB_1BIT, \\",
        "        .parity = USART_PM_NONE, \\",
        f"        .irq_priority = {bindings['AT_UART_IRQ_PRIORITY']}U \\",
        "    }",
        "",
    ])
    header_lines.extend(["", "#endif /* GD32H77X_CONFIG_H */", ""])
    (include_dir / "gd32h77x_config.h").write_text(
        "\n".join(header_lines), encoding="utf-8")
    return values


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform-root", type=Path, required=True)
    parser.add_argument("--chip", required=True)
    parser.add_argument("--board", required=True)
    parser.add_argument("--project-config", type=Path)
    parser.add_argument("--out-dir", type=Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    try:
        if args.out_dir:
            values = write_outputs(
                args.platform_root.resolve(), args.chip, args.board,
                args.out_dir.resolve(), args.project_config)
        else:
            values = resolve_config(
                args.platform_root.resolve(), args.chip, args.project_config)
    except (OSError, ValueError, kconfiglib.KconfigError) as exc:
        print(f"GD32H77X platform config error: {exc}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(values, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
