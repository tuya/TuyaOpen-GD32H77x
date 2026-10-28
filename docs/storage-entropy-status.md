# Flash and entropy adapter status

The GD32H77X build resolves the platform's `partitions.csv` and `flash_devices.csv` inputs (or a board replacement pair) into linker fragments and C routing tables. Device geometries are referenced from the platform registry; logical partition rows name `internal_flash` or `ospi0_flash` explicitly. The generated router checks whole logical ranges before splitting them across segments. It allows reads through generated CPU-mapped CNVM geometry. Host code and tests now exercise CNVM KV read/program/erase through generated KV segments; target-board CNVM write/erase has not yet been validated on hardware. External NOR operations and OTA flows remain unavailable. The hardware TRNG helper is connected to `tkl_system_get_random()` and remains fail-closed.

## CNVM

Logical reads can cover a full generated CNVM-backed partition, including a range split across multiple CNVM segments. A logical window is never cast to a pointer: each chunk uses the generated device's actual CNVM CPU base and physical offset. The router rejects overflow, ranges crossing partition boundaries, and unmapped or unregistered devices. CNVM KV program and erase use the board-generated KV segments and hardware-declared 32-byte programming and 64 KiB erase geometry. Host tests cover these paths, but target-board write/erase and cache behavior remain unverified. Lock/unlock is unavailable; UF and KV_DATA report the generated KV partition when its read/write/erase capabilities are all supported.

The selected layout defines KV geometry independently of the application image. The main-repository LittleFS setup currently maps one reported `block_size` to read, program, erase, cache and block geometry, while CNVM has distinct program and erase units. The KV module therefore stays disabled until its on-device geometry and data semantics are implemented and verified.

## TRNG

`gd32h77x_entropy_read()` uses the vendor H77 TRNG setup: NIST mode, IRC48M-backed CK48M divided by 8 (6 MHz), high analog power, and clock-error detection disabled. Clock-error detection cannot be enabled at this frequency because its documented threshold is HCLK/16 and HCLK is 600 MHz. The optional conditioning block (`CONDEN`) is not enabled: on this silicon it leaves the generator in `GSSTA` without ever asserting `DRDY`.

The reset output width is 128 bits, so the adapter reads exactly four 32-bit words per generated sample. Reading eight words on the assumption of 256-bit output caused stale/duplicate FIFO words and eventual seed errors. The helper discards the initial sample, compares each later 128-bit sample with its predecessor, and returns an error on timeout or health failure. It clears the full requested output buffer when an error occurs after acquiring the generator. A concurrent call that receives `OPRT_RESOURCE_NOT_READY` leaves its output untouched and does not alter the active call's state.

On 2026-10-22 the isolated `examples/get-started/cli` image was run on the GinoPi board with no IoT, network-manager, Wi-Fi, Bluetooth or provisioning startup. Four consecutive 1024-byte batches completed without panic: all 256 byte values appeared, 16 adjacent pairs repeated, and the byte-frequency chi-square was 267.4 with 255 degrees of freedom. A further 128 samples using `INT_MAX` as the range were all distinct. This is basic hardware-operation evidence, not a statistical or cryptographic certification. The helper remains fail-closed and preserves a first-error register snapshot; no generated sample is retained in diagnostics.

## PMU supply mode review

The GinoPi schematic supports selecting `PMU_BYPASS` for the shown board power network. In the local BSP source `sdk-bsp-gd32h77d-realthread-ginopi-1.0.0/docs/GinoPi_schematic.pdf`, page 2, external regulator U2 (`GD30LD1002WETR-I`) is powered from `VDD_3V3`; its output passes through R7 (0 Ω) to `VCORE_0V9`. On page 13, MCU U3 is marked `GD32H77DIWK7`; its VCORE pins R13, A14 and A4 connect to `VCORE_0V9`. The SMPS pins show `VDDSMPS` G1 tied to `VDD_3V3`, `VLXSMPS` F1 marked no-connect, and `VSSSMPS` F2 grounded. This board topology supplies VCORE externally and does not route the SMPS switch node.

That matches the GD32H77x/78x user manual §9.3, printed pages 373–374: `PMU_BYPASS` disables the SMPS and powers the VCORE domain through the external VCORE supply; direct-SMPS mode instead uses the SMPS for VCORE. The SDK's `PMU_BYPASS` definition is `PMU_CTL3_BYPASS`; `PMU_DIRECT_SMPS_SUPPLY` is `PMU_CTL3_DVSEN` (`gd32h77x_os/gd32/GD32H77x_78x_standard_peripheral/Include/gd32h77x_78x_pmu.h`). Therefore the GinoPi power schematic is sufficient to choose `SEL_PMU_SMPS_MODE = PMU_BYPASS` for the documented board revision, regardless of whether its exact package pin-count classification is 100/144/176.

The schematic and available BSP identify the MCU as `GD32H77DIWK7` / `GD32H77DIW`, and the BSP selects `GD32H77DXW`. They do not explicitly state the package's 100/144/176 pin count in the materials checked. The SDK `system_gd32h77x_78x.c` comments say 100/144-pin parts support only BYPASS, while 176-pin parts support either mode. That missing pin-count mapping does not change the bypass decision because the schematic shows the external VCORE rail, but an authoritative package drawing/ordering-code table is still needed before making a package-count claim. This is a schematic-based configuration conclusion; it does not verify the physical board or regulator voltage by measurement.

## Reproduce checks

From the TuyaOpen repository root, generate the actual selected layout and run all platform host checks (no bring-up `.build` tree is needed):

```sh
cmake -S platform/GD32H77X/tests -B /tmp/gd32h77x-host-tests -G Ninja
cmake --build /tmp/gd32h77x-host-tests
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir /tmp/gd32h77x-host-tests --output-on-failure
python3 platform/GD32H77X/tests/test_flash_bridge.py
```

The CMake setup invokes the real layout resolver for the selected board CSV inputs (or platform internal fallback if no board partitions file exists), then compiles bounds checks against that generated table. A separate fixture tests cross-device segment splitting and verifies full-range preflight rejects a later unsupported backend before any callback runs. The bridge unittest uses the real resolver with isolated board CSVs and checks stale reports after source changes, fallback after a missing board partition table, and fail-closed handling of malformed or incomplete board inputs; the current run passed 17/17 bridge cases, 12/12 DfuSe transport cases, 19/19 resolver cases and 6/6 CTest cases. The CSV-stage sample project build and the four destructive ELF/BIN image checks also pass; see the platform README for artifact digests.

The target firmware explicitly includes `gd32h77x_os/main/gd32h77x_entropy.c` and the TKL flash adapter; the mock test is a separate host executable. Basic TRNG generation is verified on the GD32H77D GinoPi, but long-run health behavior and statistical randomness quality still require dedicated testing.
