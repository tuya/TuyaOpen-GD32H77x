/* Copyright (c) 2026 Tuya Inc. SPDX-License-Identifier: Apache-2.0 */
ENTRY(_start)
MEMORY { AXIRAM (rwx) : ORIGIN = 0x24000000, LENGTH = 0x00080000 }
SECTIONS {
  .text : { KEEP(*(.text.startup)) *(.text*) *(.rodata*) } > AXIRAM
  .data : { *(.data*) } > AXIRAM
  .bss (NOLOAD) : { *(.bss*) *(COMMON) } > AXIRAM
  _loader_end = .;
  _stack_bottom = 0x24020000;
  _stack_top = 0x24030000;
  ASSERT(_loader_end < 0x24010000, "OSPI loader overlaps the mailbox region")
}
