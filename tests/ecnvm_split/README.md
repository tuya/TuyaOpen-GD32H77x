# GD32H779V_START CNVM / ECNVM split experiment

This standalone bare metal image checks code fetch from ECNVM while `main`,
the reset handler, and the vendor vector table remain in CNVM. It uses the
vendor `SystemInit()` and the existing board console binding (USART5 PC6/PC7,
AF7, 115200 baud). The test function records its current Thumb PC, reads a
runtime selected `const volatile` view of the ECNVM array, and prints the
addresses and checksum repeatedly.

Build from the repository root without an interactive SDK setup:

```sh
platform/GD32H77X/tests/ecnvm_split/build.sh
```

Artifacts go under `.cache/gd32h779v_start/ecnvm-split/`:

- `ecnvm_split.elf`: linked image for inspection/debugging.
- `cnvm.bin`: standalone image for `0x08000000`.
- `ecnvm.bin`: standalone image for `0x08200000`.
- `ecnvm_split.hex`: address-bearing Intel HEX for inspection.
- `readelf.txt`, `ecnvm_disassembly.txt`: static layout evidence.

The build script enforces a 64 KiB maximum for each binary. The linker also
asserts that the CNVM load image fits its first 64 KiB sector, that `main` and
`Reset_Handler` are in CNVM, and that both ECNVM sections fit in
`0x08200000..0x0820FFFF`. The separate BINs must be programmed at their stated
addresses; never concatenate them into an image spanning the intervening flash.

Hardware validation passed: three UART reports confirmed ECNVM execution and
the expected checksum. Both 64 KiB sectors were restored and their readbacks
matched the pre-test backups byte for byte. Details are in the adjacent
`.cache/gd32h779v_start/ecnvm-split/hardware/validation.json`; production image
reset validation is tracked separately.
