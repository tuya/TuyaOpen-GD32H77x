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
    text = text.replace("@CNVM_TEXT_INPUTS@", cnvm_text)
    text = text.replace("@ECNVM_OUTPUT_SECTIONS@", ecnvm_sections)
    output.write_text(text, encoding="utf-8")
    return output
