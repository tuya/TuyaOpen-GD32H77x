/* Copyright (c) 2026 Tuya Inc. SPDX-License-Identifier: Apache-2.0 */
ENTRY(_start)
MEMORY { AXIRAM (rwx) : ORIGIN = 0x24010000, LENGTH = 0x00070000 }
SECTIONS {
  .text : { KEEP(*(.text.startup)) *(.text*) *(.rodata*) } > AXIRAM
  .data : { *(.data*) } > AXIRAM
  .bss (NOLOAD) : { *(.bss*) *(COMMON) } > AXIRAM
  _loader_end = .;
  _stack_bottom = 0x2405F000;
  _stack_top = 0x24060000;
  ASSERT(_loader_end < _stack_bottom, "UART loader overlaps its stack")
}
