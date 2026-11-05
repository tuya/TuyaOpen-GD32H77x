#!/usr/bin/env python3
"""Build the configured GD25Q64E SRAM loader with the platform ARM GCC."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

PLATFORM = Path(__file__).resolve().parents[4]
VENDOR = PLATFORM / "gd32h77x_os/gd32/GD32H77x_78x_standard_peripheral"
CMSIS = PLATFORM / "gd32h77x_os/gd32/CMSIS"
SOURCE = Path(__file__).resolve().parent


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-dir", type=Path, required=True)
    source_group = parser.add_mutually_exclusive_group(required=True)
    source_group.add_argument("--layout", type=Path,
                              help="generated resolved layout/manifest JSON")
    source_group.add_argument("--config", type=Path,
                              help=argparse.SUPPRESS)
    parser.add_argument("--compiler", default=os.environ.get(
        "ARM_NONE_EABI_GCC", "arm-none-eabi-gcc"))
    parser.add_argument("--uart", action="store_true",
                        help="build the UART-entered AXI SRAM loader instead of "
                             "the SWD mailbox loader")
    args = parser.parse_args()
    if args.layout:
        document = json.loads(args.layout.read_text())
        layout = document.get("layout", document)
        target_for_layout = layout.get("target_chip")
        candidates = [item for item in layout.get("devices", [])
                      if item.get("part_number", "").upper() == "GD25Q64ESIG"]
        if len(candidates) != 1:
            parser.error("resolved layout must identify exactly one GD25Q64ESIG device")
        device = candidates[0]
        binding = layout.get("board_binding", {}).get(str(device.get("id")))
        if not isinstance(binding, dict) or binding.get("controller") not in ("OSPI0", "OSPI1"):
            parser.error("resolved layout is missing the device OSPI0/OSPI1 board_binding")
        data = binding.get("data")
        if not isinstance(data, list) or len(data) < 2 or len(data) > 4:
            parser.error("resolved OSPI binding must contain MOSI/MISO and optional WP/HOLD pins")
        values = [binding.get("chip_select"), binding.get("clock"), *data]
        labels = ("cs", "clk", "mosi", "miso", "wp", "hold")
        pins = dict(zip(labels, values))
        config = {"target": target_for_layout,
                  "pins": {key: value for key, value in pins.items() if value is not None}}
    else:
        config = json.loads(args.config.read_text())
    target = config.get("target")
    target_macro = target[:-2] + "X" + target[-1] if isinstance(target, str) else None
    pins = config.get("pins")
    if not isinstance(target, str) or not isinstance(target_macro, str) or not isinstance(pins, dict):
        parser.error("config must provide target, target_macro and pins")
    if not target_macro.startswith("GD32H77") or not target_macro.endswith(("XI", "XP", "XW")):
        parser.error("invalid vendor target macro")
    registry = json.loads((PLATFORM / "gd32h77x_os/chips/platform_chips.json").read_text())["platform_chips"]
    header = (CMSIS / "GD/GD32H77x_78x/Include/gd32h77x_78x.h").read_text()
    if target not in registry or registry[target].get("target_chip") != target or f"defined({target_macro})" not in header:
        parser.error("target must be a canonical registered target supported by vendor CMSIS")
    pin_macros = {}
    used = set()
    for key in ("cs", "clk", "mosi", "miso", "wp", "hold"):
        pin = pins.get(key)
        if pin is None:
            if key in ("cs", "clk", "mosi", "miso"):
                parser.error("pins must include cs, clk, mosi and miso")
            continue
        match = re.fullmatch(r"P([A-K])(0|[1-9]|1[0-5])", pin) if isinstance(pin, str) else None
        if not match or pin in used:
            parser.error("pin assignments must be valid and unique GPIOs")
        used.add(pin)
        port, number = match.groups()
        pin_macros[f"OSPI_{key.upper()}_GPIO"] = f"GPIO{port}"
        pin_macros[f"OSPI_{key.upper()}_RCU"] = f"RCU_GPIO{port}"
        pin_macros[f"OSPI_{key.upper()}_PIN"] = f"GPIO_PIN_{number}"
    out = args.out_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if args.uart:
        elf = out / "gd32h77x_uart_loader.elf"
        binary = out / "gd32h77x_uart_loader.bin"
        sources = [SOURCE / "startup_uart.S", SOURCE / "gd32h77x_ospi_loader.c",
                   VENDOR / "Source/gd32h77x_78x_gpio.c",
                   VENDOR / "Source/gd32h77x_78x_rcu.c",
                   VENDOR / "Source/gd32h77x_78x_usart.c"]
        linker_script = SOURCE / "gd32h77x_uart_loader.ld"
        map_file = out / "uart_loader.map"
    else:
        elf = out / "gd32h77x_ospi_loader.elf"
        binary = out / "gd32h77x_ospi_loader.bin"
        sources = [SOURCE / "startup.S", SOURCE / "gd32h77x_ospi_loader.c",
                   VENDOR / "Source/gd32h77x_78x_gpio.c",
                   VENDOR / "Source/gd32h77x_78x_rcu.c"]
        linker_script = SOURCE / "gd32h77x_ospi_loader.ld"
        map_file = out / "loader.map"
    command = [args.compiler, "-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=soft",
               "-Os", "-ffreestanding", "-fno-builtin", "-fno-common",
               "-ffunction-sections", "-fdata-sections", "-fno-unwind-tables",
               "-fno-asynchronous-unwind-tables", "-Wall", "-Wextra", "-Werror",
               "-nostdlib", "-D" + target_macro,
               "-I" + str(SOURCE),
               "-I" + str(PLATFORM / "gd32h77x_os/components/runtime"),
               "-I" + str(PLATFORM / "gd32h77x_os/components/drivers"),
               "-I" + str(PLATFORM / "gd32h77x_os/gd32/include"),
               "-I" + str(VENDOR / "Include"),
               "-I" + str(CMSIS), "-I" + str(CMSIS / "Include"),
               "-I" + str(CMSIS / "GD/GD32H77x_78x/Include"),
               "-T" + str(linker_script),
               "-Wl,--gc-sections", "-Wl,--build-id=none", "-Wl,-Map," + str(map_file),
               "-o", str(elf), *map(str, sources), "-lgcc"]
    if args.uart:
        command.insert(command.index("-D" + target_macro), "-DOSPI_UART_LOADER")
    command[command.index("-o"):command.index("-o")] = ["-D{}={}".format(k, v)
                                                        for k, v in sorted(pin_macros.items())]
    binding_record = {"target": target, "target_macro": target_macro,
                      "pins": pins, "pin_macros": pin_macros}
    binding_sha256 = hashlib.sha256(json.dumps(
        binding_record, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    if config.get("binding_sha256") not in (None, binding_sha256):
        parser.error("binding_sha256 does not match configured target and pins")
    subprocess.run(command, check=True)
    objcopy = str(Path(args.compiler).with_name("arm-none-eabi-objcopy"))
    subprocess.run([objcopy, "-O", "binary", str(elf), str(binary)], check=True)
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    metadata = {
        "schema_version": 2,
        "target": target,
        "target_macro": target_macro,
        "pins": pins,
        "binding_sha256": binding_sha256,
        "method": ("UART ROM ISP to AXI SRAM; GPIO bit-banged single-line SPI on "
                   "resolved OSPI pins" if args.uart else
                   "AXI SRAM mailbox; GPIO bit-banged single-line SPI on resolved OSPI pins"),
        "flash": "GD25Q64E / GD25Q64ESIG",
        "capacity_bytes": 8388608,
        "erase_bytes": 4096,
        "page_bytes": 256,
        "entry": "_start",
        "entry_address": "0x24010001" if args.uart else "0x24000001",
        "load_address": "0x24010000" if args.uart else "0x24000000",
        "ram_capacity_min_bytes": 0x70000 if args.uart else 524288,
        "stack_top": "0x24060000" if args.uart else "0x24030000",
        "uart_baud": 115200 if args.uart else None,
        "binary": binary.name,
        "binary_size": binary.stat().st_size,
        "binary_sha256": digest,
    }
    if not args.uart:
        metadata["mailbox_address"] = "0x24010000"
        metadata["buffer_address"] = "0x24011000"
    (out / "loader.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(json.dumps(metadata, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
