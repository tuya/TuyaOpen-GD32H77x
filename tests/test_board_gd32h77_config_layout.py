#!/usr/bin/env python3
"""Validate the board GD32H77 config-directory boundary for GD32H77X."""

from pathlib import Path
import json
import re
import sys

ROOT = Path(__file__).resolve().parents[3]
BOARDS = ROOT / "boards" / "GD32H77X"
BOARD_NAMES = ("GD32H779V_START",)
BOARD_SELECTION_FILES = {"GD32H779V_START": "GD32H779V.config"}
MANDATORY = set()
OPTIONAL = {"flash_devices.csv", "ospi_profiles.csv", "gd32h77x.config"}


def main() -> int:
    failures: list[str] = []
    for board_name in BOARD_NAMES:
        board = BOARDS / board_name
        kconfig = board / "Kconfig"
        config_dir = board / "platform_config"
        if not kconfig.is_file():
            failures.append(f"{board_name}: missing framework-entry Kconfig")
        else:
            kconfig_text = kconfig.read_text(errors="replace")
            for symbol in ("CHIP_CHOICE", "BOARD_CHOICE"):
                if f"config {symbol}" not in kconfig_text:
                    failures.append(f"{board_name}: missing {symbol}")
        if config_dir.is_dir():
            files = {item.name for item in config_dir.iterdir() if item.is_file()}
            for name in sorted(MANDATORY - files):
                failures.append(f"{board_name}: missing platform_config/{name}")
            for name in sorted(files - MANDATORY - OPTIONAL):
                failures.append(f"{board_name}: unexpected platform_config/{name}")
            for name in sorted(name for name in files if name.endswith(".h")):
                failures.append(
                    f"{board_name}: hardware binding headers must be platform Kconfig, not {name}")
            if board_name == "GD32H779V_START" and "flash_devices.csv" in files:
                failures.append(
                    f"{board_name}: flash_devices.csv duplicates the GD32H779VW chip default and must not exist")
        elif board_name != "GD32H779V_START":
            failures.append(f"{board_name}: missing optional platform_config/ directory")
        if board_name == "GD32H779V_START" and config_dir.exists():
            failures.append(
                f"{board_name}: must use GD32H779VW chip defaults without platform_config/")
        for name in sorted(MANDATORY | OPTIONAL):
            if (board / name).exists():
                failures.append(f"{board_name}: {name} must not remain at board root")
    family_kconfig = BOARDS / "Kconfig"
    family_tkl_kconfig = BOARDS / "TKL_Kconfig"
    family_os_service_kconfig = BOARDS / "OS_SERVICE_Kconfig"
    if not family_tkl_kconfig.is_file():
        failures.append("missing boards/GD32H77X/TKL_Kconfig")
    else:
        tkl_text = family_tkl_kconfig.read_text(errors="replace")
        for symbol in ("ENABLE_WIFI", "ENABLE_BLUETOOTH", "ENABLE_FLASH",
                       "ENABLE_GPIO", "ENABLE_UART", "ENABLE_PLATFORM_MBEDTLS",
                       "LITTLE_END"):
            if f"config {symbol}\n" not in tkl_text:
                failures.append(f"GD32H77X TKL_Kconfig is missing {symbol}")
        if not re.search(
                r"config ENABLE_BLUETOOTH\n(?:[ \t].*\n)*?[ \t]+default y\n",
                tkl_text):
            failures.append("GD32H77X ENABLE_BLUETOOTH must default to y")
    if not family_os_service_kconfig.is_file():
        failures.append("missing boards/GD32H77X/OS_SERVICE_Kconfig")
    else:
        os_service_text = family_os_service_kconfig.read_text(errors="replace")
        if "config MBEDTLS_CONFIG_FILE\n" not in os_service_text:
            failures.append("GD32H77X OS_SERVICE_Kconfig is missing MBEDTLS_CONFIG_FILE")
        for required in ("config ENABLE_GD32H77X_OS_SERVICE\n",
                         "select ENABLE_BT_SERVICE\n",
                         "select ENABLE_BT_NETCFG\n",
                         "select ENABLE_BT_CTRL\n"):
            if required not in os_service_text:
                failures.append(f"GD32H77X OS_SERVICE_Kconfig is missing {required.strip()}")
        if "select ENABLE_NIMBLE\n" in os_service_text:
            failures.append("GD32H77X must not select NimBLE")
    if family_kconfig.is_file():
        family_text = family_kconfig.read_text(errors="replace")
        for source in ('rsource "./TKL_Kconfig"', 'rsource "./OS_SERVICE_Kconfig"'):
            if source not in family_text:
                failures.append(f"GD32H77X family Kconfig is missing {source}")
        for symbol in ("ENABLE_WIFI", "ENABLE_BLUETOOTH", "ENABLE_FLASH",
                       "ENABLE_GPIO", "ENABLE_UART", "MBEDTLS_CONFIG_FILE"):
            if f"config {symbol}\n" in family_text:
                failures.append(f"GD32H77X family Kconfig must source {symbol}, not inline it")
    selection_dir = BOARDS / "config"
    unexpected_selections = {
        item.name for item in selection_dir.glob("*.config")
    } - set(BOARD_SELECTION_FILES.values())
    for name in sorted(unexpected_selections):
        failures.append(f"unexpected board selection config: {name}")

    for board_name in BOARD_NAMES:
        selection = selection_dir / BOARD_SELECTION_FILES[board_name]
        if not selection.is_file():
            failures.append(f"missing boards/GD32H77X/config/{board_name}.config")
            continue
        expected = {
            "CONFIG_BOARD_CHOICE_GD32H77X=y",
            f"CONFIG_BOARD_CHOICE_{board_name}=y",
        }
        actual = {
            line.strip() for line in selection.read_text().splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        }
        if actual != expected:
            failures.append(
                f"boards/GD32H77X/config/{selection.name} must contain "
                "only the platform and board choices")

    platform = ROOT / "platform" / "GD32H77X"
    os_root = platform / "gd32h77x_os"
    project_root = os_root / "project"
    components_root = os_root / "components"
    root_cmake = platform / "CMakeLists.txt"
    root_cmake_text = root_cmake.read_text(errors="replace")
    for component in ("startup", "runtime", "console", "drivers", "at"):
        if not (components_root / component).is_dir():
            failures.append(f"missing gd32h77x_os/components/{component}")
    if (os_root / "manifest.json").exists():
        failures.append("vendor file manifest must not exist; vendor sources are patchable")
    for legacy in (os_root / "main", os_root / "examples"):
        if legacy.exists():
            failures.append(f"legacy source directory must not exist: {legacy}")
    for board_name in BOARD_NAMES:
        legacy_config_dir = BOARDS / board_name / "gd32h77_config"
        if legacy_config_dir.exists():
            failures.append(
                f"{board_name}: legacy gd32h77_config/ must be renamed to platform_config/")
    if not (components_root / "startup" / "gd32h77x_linker.ld.in").is_file():
        failures.append("missing components/startup/gd32h77x_linker.ld.in")
    if (components_root / "CMakeLists.txt").exists():
        failures.append("components root must not contain a CMakeLists.txt")
    for component in ("FreeRTOS", "at", "console", "drivers", "runtime", "startup"):
        if not (components_root / component / "CMakeLists.txt").is_file():
            failures.append(f"missing components/{component}/CMakeLists.txt")
    if not (os_root / "gd32" / "CMakeLists.txt").is_file():
        failures.append("missing gd32/CMakeLists.txt")
    if '"${GD32H77X_COMPONENTS_ROOT}/*/CMakeLists.txt"' not in root_cmake_text:
        failures.append("platform CMake must discover components/*/CMakeLists.txt")
    if 'add_subdirectory("${GD32H77X_GD_ROOT}"' not in root_cmake_text:
        failures.append("platform CMake must add gd32/CMakeLists.txt")
    for app in ("tuya", "at_bridge", "board_diag", "at_reset"):
        app_dir = project_root / app
        for name in ("project.cmake",):
            if not (app_dir / name).is_file():
                failures.append(f"missing project/{app}/{name}")
        for legacy in ("CMakeLists.txt", "app_default.config", "project.json"):
            if (app_dir / legacy).exists():
                failures.append(
                    f"project/{app}/{legacy} must use the standalone build")
        sources = "\n".join(
            path.read_text(errors="replace")
            for path in app_dir.rglob("*.[ch]"))
        if "void app_entry(" not in sources:
            failures.append(f"project/{app} must implement app_entry")
        if re.search(r"\nvoid\s+tuya_app_main\s*\([^;]*\)\s*\{", sources):
            failures.append(f"project/{app} must not define tuya_app_main")
        if app != "tuya":
            for token in ("tuya_", "tkl_", "tal_", "OPRT_", "OPERATE_RET"):
                if token in sources:
                    failures.append(f"project/{app} must stay independent of {token}")
    tuya_project = (project_root / "tuya" / "project.cmake").read_text()
    for declaration in ("GD32H77X_EXTERNAL_INCLUDE_DIRS",
                        "GD32H77X_EXTERNAL_LIBRARIES",
                        "GD32H77X_PROJECT_REQUIRED_SYMBOLS",
                        "tuya_app_main"):
        if declaration not in tuya_project:
            failures.append(f"project/tuya must declare {declaration}")

    backend = (platform / "CMakeLists.txt").read_text()
    for token in ("tuya", "tkl_", "tal_", "OPRT_", "switch_demo", "libtuyaapp"):
        if token in backend:
            failures.append(f"platform CMake backend must stay project-agnostic: {token}")
    adapter_diag = platform / "tuyaos" / "tests" / "adapter_diag"
    if not (adapter_diag / "CMakeLists.txt").is_file():
        failures.append("missing tuyaos/tests/adapter_diag/CMakeLists.txt")
    if (adapter_diag / "app_default.config").exists():
        failures.append(
            "tuyaos/tests/adapter_diag/app_default.config must not exist; use tos.py config set")
    adapter_sources = "\n".join(
        path.read_text(errors="replace")
        for path in adapter_diag.rglob("*.[ch]"))
    if "void tuya_app_main(" not in adapter_sources:
        failures.append("adapter_diag must use the standard TuyaOpen entry")
    build_project = os_root / "tools" / "build_project.py"
    if not build_project.is_file():
        failures.append("missing gd32h77x_os/tools/build_project.py")
    elif "TOS_PROJECT_ROOT" in build_project.read_text(errors="replace"):
        failures.append("native project build must not depend on TOS_PROJECT_ROOT")
    if (platform / "default.config").exists():
        failures.append("platform/GD32H77X/default.config must not exist")
    for tkl_kconfig in (platform / "TKL_Kconfig",
                        platform / "tuyaos" / "TKL_Kconfig"):
        if tkl_kconfig.exists():
            failures.append(
                f"TuyaOpen TKL capabilities must not live in {tkl_kconfig}")
    if (platform / "OS_SERVICE_Kconfig").exists():
        failures.append("platform services must be merged into the platform Kconfig")
    platform_kconfig = platform / "Kconfig"
    if platform_kconfig.is_file():
        platform_text = platform_kconfig.read_text()
        for symbol in ("ENABLE_GD32H77X_FLASH_BACKEND_SWD",
                       "ENABLE_GD32H77X_FLASH_BACKEND_DFU",
                       "ENABLE_GD32H77X_FLASH_BACKEND_UART"):
            if f"config {symbol}\n" not in platform_text:
                failures.append(f"missing flashing backend choice {symbol}")
        for symbol in ("ENABLE_GD32H77X_CONSOLE_UART_USART5",
                       "ENABLE_GD32H77X_CONSOLE_GPIO_PORT_C",
                       "GD32H77X_CONSOLE_TX_PIN_NUMBER", "GD32H77X_CONSOLE_RX_PIN_NUMBER",
                       "GD32H77X_CONSOLE_AF_NUMBER",
                       "ENABLE_GD32H77X_AT_UART_UART4",
                       "ENABLE_GD32H77X_AT_GPIO_PORT_B",
                       "GD32H77X_AT_TX_PIN_NUMBER", "GD32H77X_AT_RX_PIN_NUMBER",
                       "GD32H77X_AT_AF_NUMBER", "GD32H77X_AT_BAUD",
                       "GD32H77X_AT_IRQ_PRIORITY",
                       "ENABLE_GD32H77X_AT_RESET_GPIO_PORT_C",
                       "GD32H77X_AT_RESET_PIN_NUMBER"):
            if f"config {symbol}\n" not in platform_text:
                failures.append(f"missing hardware binding config {symbol}")
        for symbol in ("GD32H77X_FLASH_BACKEND", "GD32H77X_FLASH_DEVICE",
                       "GD32H77X_FLASH_SWD_PACK",
                       "GD32H77X_FLASH_UART_BAUD",
                       "GD32H77X_FLASH_VERIFY"):
            if f"config {symbol}\n" in platform_text:
                failures.append(f"obsolete flashing config {symbol} must not exist")
        for symbol in ("GD32H77X_AT_PROBE", "GD32H77X_AT_AT_BRIDGE",
                       "GD32H77X_DIAG_CLI"):
            if f"config {symbol}\n" in platform_text:
                failures.append(
                    f"bring-up-only option {symbol} must not be in platform Kconfig")
        for symbol in ("ENABLE_WIFI", "ENABLE_BLUETOOTH", "ENABLE_FLASH",
                       "ENABLE_GPIO", "ENABLE_UART", "PLATFORM_CHOICE"):
            if f"config {symbol}\n" in platform_text:
                failures.append(
                    f"TuyaOpen capability {symbol} must not live in platform Kconfig")
    platform_entry = BOARDS / "Kconfig"
    if platform_entry.is_file() and \
            'rsource "../../platform/GD32H77X/Kconfig"' in platform_entry.read_text():
        failures.append("TuyaOpen board Kconfig must not source platform Kconfig")
    platform_config_py = platform / "platform_config.py"
    if not platform_config_py.is_file():
        failures.append("missing platform-owned platform_config.py")
    else:
        platform_config_text = platform_config_py.read_text(errors="replace")
        if "app_default.config" in platform_config_text:
            failures.append("platform config resolver must not read TuyaOpen app_default.config")
        if "boards_root" in platform_config_text:
            failures.append("platform config resolver must not discover boards")
    if not (platform / "tuya_project_config.py").is_file():
        failures.append("missing TuyaOpen project config staging helper")
    if (BOARDS / "GD32H779V_START" / "platform_config" / "board.config").exists():
        failures.append("legacy board.config must be renamed to gd32h77x.config")
    for required in ("GD32H77X_PROJECT_CONFIG", "--project-config",
                     "${GD32H77X_PROJECT_DIR}/project.config"):
        if required not in root_cmake_text:
            failures.append(f"platform CMake is missing project config input: {required}")
    for path in platform.glob("*.config"):
        failures.append(f"variant config must live under chips/: {path.name}")
    chips_root = platform / "gd32h77x_os" / "chips"
    registry = json.loads((chips_root / "platform_chips.json").read_text())
    canonical_chips = {
        entry["target_chip"] for entry in registry["platform_chips"].values()
    }
    actual_chip_dirs = {
        path.name for path in chips_root.iterdir() if path.is_dir()
    }
    for name in sorted(actual_chip_dirs - canonical_chips):
        failures.append(f"unregistered chip directory: chips/{name}")
    for canonical in sorted(canonical_chips):
        chip_dir = chips_root / canonical
        expected_files = {
            f"{canonical}.config",
            "partitions.csv",
            "flash_devices.csv",
        }
        if canonical == "GD32H77DIW":
            expected_files.add("uart_isp.json")
        actual_files = {
            path.name for path in chip_dir.iterdir() if path.is_file()
        }
        for name in sorted(expected_files - actual_files):
            failures.append(f"{canonical}: missing chips/{canonical}/{name}")
        for name in sorted(actual_files - expected_files):
            failures.append(f"{canonical}: unexpected chips/{canonical}/{name}")
    forbidden_target_tokens = (
        "tuya_", "TUYA_", "tkl_", "tal_", "OPRT_", "OPERATE_RET")
    for source in sorted(components_root.rglob("*")):
        if not source.is_file() or source.suffix not in {".c", ".h", ".S", ".s"}:
            continue
        text = source.read_text(errors="replace")
        for token in forbidden_target_tokens:
            if token in text:
                failures.append(
                    f"component source must not depend on {token}: {source}")
    shared_linker = components_root / "startup" / "gd32h77x_linker.ld.in"
    if not shared_linker.is_file():
        failures.append("missing components/startup/gd32h77x_linker.ld.in")
    if (BOARDS / "GD32H77D_GINO").exists():
        failures.append("GD32H77D_GINO board must not remain in the tree")
    h779_chip_config = (
        platform / "gd32h77x_os/chips/GD32H779VW/GD32H779VW.config")
    h779_chip_config_text = h779_chip_config.read_text(errors="replace")
    for expected in (
            "CONFIG_ENABLE_GD32H77X_FLASH_BACKEND_SWD=y",
            "CONFIG_ENABLE_GD32H77X_CONSOLE_UART_USART5=y",
            "CONFIG_ENABLE_GD32H77X_AT_UART_UART4=y"):
        if expected not in h779_chip_config_text:
            failures.append(
                f"GD32H779VW chip defaults are missing {expected}")
    if "CONFIG_GD32H77X_ECNVM_CODE" in h779_chip_config_text:
        failures.append("ECNVM code mode must be derived from partitions.csv, not gd32h77x.config")
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("GD32H77X board GD32H77 config-layout checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
