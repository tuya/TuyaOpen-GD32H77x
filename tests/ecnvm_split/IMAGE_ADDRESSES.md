# GD32H779VW split image map

- CNVM: `cnvm.bin`, program at `0x08000000`.
- ECNVM: `ecnvm.bin`, program at `0x08200000`.
- `ecnvm_split.elf` and `ecnvm_split.hex` retain their linked addresses.

Program the two binary files individually at the addresses above. Do not
concatenate them: the gap between the CNVM and ECNVM address ranges must never
be emitted as a single large binary.
