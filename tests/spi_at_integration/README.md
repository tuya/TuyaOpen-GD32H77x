# H779 ↔ VW553 SPI AT host regression

Run the test from the TuyaOpen repository root:

```sh
platform/GD32H77X/tests/spi_at_integration/run.sh
```

When the sibling `platform/GD32` checkout provides
`MSDK/vw553_at_gateway/gateway_spi_protocol.c`, the GD32H77X host CMake project
also registers this runner as `gd32h77x_spi_at_integration`. Without that
optional checkout, the normal H779 host-test configuration remains independent
of VW553 sources.

The runner compiles both endpoints' production codec and stop-and-wait helpers
with ASan and UBSan. It compares their encoded bytes and cross-decodes complete
256-byte frames, checks CRC and malformed-frame rejection, and drives both
helpers through startup (including a lost HELLO), retransmission and duplicate
delivery, backpressure with reverse ACK progress, sequence wrap, peer restart,
and binary streams split across 240-byte payloads. A second host test compiles
the production VW553 transport source against small OS/peripheral fakes and
checks receive arm/wait behavior, bounded stale-semaphore draining, NUL bytes,
whole-frame capacity checks, timeout/re-arm, and session-reset cancellation.

The tests exercise the protocol and API state machines. They do not replace SPI
electrical timing or on-board interoperability checks.
