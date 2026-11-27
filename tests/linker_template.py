"""Render the shared GD32H77X linker template for host-side build tests."""

from __future__ import annotations

from pathlib import Path


PLATFORM = Path(__file__).resolve().parents[1]
TEMPLATE = PLATFORM / "gd32h77x_os/components/startup/gd32h77x_linker.ld.in"


def render(layout: dict, output: Path,
           ecnvm_enabled: bool | None = None) -> Path:
    if ecnvm_enabled is None:
        ecnvm_enabled = "ecnvm_code" in layout["layout"]["partitions"]
    text = TEMPLATE.read_text(encoding="utf-8")
    if ecnvm_enabled:
        cnvm_text = ""
        ecnvm_sections = (
            ".ecnvm_text : { *(.text*) *(.rodata*) } > ECNVM\n"
            "    ASSERT(SIZEOF(.ecnvm_text) > 0, "
            '"ECNVM linker region declared but ECNVM text is empty")')
    else:
        cnvm_text = "*(.text*) *(.rodata*)"
        ecnvm_sections = ""
    direct_vector = layout["layout"]["flash_image"].get("vector_partition") == "boot"
    if direct_vector:
        vector_base = "ORIGIN(BOOT)"
        boot_table = "/DISCARD/ : { *(.boot_table) }"
        vector_output = "> BOOT"
        boot_table_assert = ""
    else:
        vector_base = "ORIGIN(CNVM)"
        boot_table = ".boot_table : { KEEP(*(.boot_table)) } > BOOT"
        vector_output = "> CNVM"
        boot_table_assert = (
            'ASSERT(SIZEOF(.boot_table) == 8, '
            '"boot table must contain SP and reset entry")')
    text = text.replace("@CNVM_TEXT_INPUTS@", cnvm_text)
    text = text.replace("@ECNVM_OUTPUT_SECTIONS@", ecnvm_sections)
    text = text.replace("@GD32H77X_LINKER_VECTOR_BASE@", vector_base)
    text = text.replace("@GD32H77X_LINKER_BOOT_TABLE@", boot_table)
    text = text.replace("@GD32H77X_LINKER_VECTOR_OUTPUT@", vector_output)
    text = text.replace("@GD32H77X_LINKER_BOOT_TABLE_ASSERT@", boot_table_assert)
    output.write_text(text, encoding="utf-8")
    return output
