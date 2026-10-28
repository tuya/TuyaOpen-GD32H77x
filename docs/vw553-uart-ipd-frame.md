# VW553 UART IPD frame patch

`vw553-uart-ipd-frame.patch` is an unapplied patch for the pinned VW553 SDK 1.0.3g archive at
`.cache/gino-validation/vendor/GD32VW55x_RELEASE_V1.0.3g.zip` (SHA256
`49995f9c482136e7b9be02fa4b1f1a6ea7716a215a0b993c1538a6d2f0792cea`). It was generated against the
archive's `MSDK/app/atcmd.c`, `atcmd.h`, and `atcmd_tcpip.c`; the archive and platform vendor files
remain unchanged.

The patch keeps each UART `+IPD` header, binary payload, CRLF, and trailing `OK` under one transmit
semaphore acquisition. It addresses only the Socket IPD frame boundary. It does not make BLE safe:
the existing `+BLEDATA` binary event is emitted through separate sends and is not understood by the
current H77 parser. BLE requires a separately specified length-delimited event frame and matching
parser/firmware changes before enabling BLE transactions.

The patch passed `patch --dry-run -p1` against a fresh extraction of the pinned archive. The
cross-build candidate was prepared from that archive by applying the saved handoff's
`vw553-reference-config/` and `vw553-overlay/`, then this patch; all of this work stayed under
`/tmp/vw553-at-candidate/GD32VW55x_RELEASE_V1.0.3g`. The effective preprocessor macros confirm
`CONFIG_ATCMD`, START board,
UART1 at 115200, LOG_UART2, with RF test and ATCMD_SPI disabled. The linked ELF exports
`at_hw_send_ipd`, `atcmd_init`, `atcmd_task`, and `cip_recv_task`.

With the bundled Nuclei GCC 10.2.0 toolchain, a fresh CMake/Ninja build completed all 320 steps. The
SDK's automatic post-build invocation passes an empty source-root argument, so its image packaging
step cannot find config/scripts. Packaging was then run manually with the candidate root explicitly
provided. `image-all.bin` validation confirmed the MBL prefix, the MSDK binary at offset `0xA000`,
an all-`0xFF` gap between MBL and offset `0xA000`, and exact image length.

The reproducible commands, complete logs, normalized source/config diff, effective macros, candidate
ELF/BIN, and packaged images are archived at
`.cache/gino-validation/vw553-ipd-candidate-20261019/`. Key SHA256 values:

- `MSDK.elf`: `79b894d4f3328dee92fa2b44de25bfd39ece7a647b9dbe0e2eee93e54458c8c5`
- `MSDK.bin`: `5ec9d8d299cb0bfb2c0e46cd2dd9ade06ddd4e5256cbbe75654f9602381a1e9c`
- `image-all.bin` (1,096,732 bytes): `f284c1d423997ab061943f8853b38a7a923ed7f6689e8d8b10e47bab37c82dd4`

The candidate has not been flashed or validated on hardware. The SDK archive and platform vendor
tree remain unchanged.
