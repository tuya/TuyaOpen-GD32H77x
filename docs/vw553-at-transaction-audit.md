# VW553 AT transaction protocol audit

This audit uses the fixed SDK 1.0.3g archive
`.cache/gino-validation/vendor/GD32VW55x_RELEASE_V1.0.3g.zip` (SHA256
`49995f9c482136e7b9be02fa4b1f1a6ea7716a215a0b993c1538a6d2f0792cea`). The archive and normal
firmware were not modified.

## UART response serialization

`MSDK/app/atcmd.h:54-95` defines response macros. `AT_RSP_IMMEDIATE` and `AT_RSP_DIRECT` call the
hardware sender separately; `AT_RSP_OK` appends `OK\r\n` to the current response buffer and sends
that buffer. In `MSDK/app/atcmd.c:1536-1547`, the UART sender acquires `at_hw_tx_sema` for one
individual send call. There is no lock around a sequence of calls.

The normal TCP/UDP receive path in `MSDK/app/atcmd_tcpip.c:1680-1685` sends the `+IPD` header,
binary data, CRLF, and trailing `OK` separately. The UDP server path at `1454-1459` does the same.
Thus an unrelated response can be inserted between frame pieces in the pinned firmware. The host
cannot safely discard the next `OK` without a firmware guarantee. The candidate
[`vw553-uart-ipd-frame.patch`](vw553-uart-ipd-frame.patch) holds the existing TX semaphore
across the full IPD frame. It was applied only to an isolated temporary SDK copy, which compiled and
packaged successfully with the saved Handoff AT configuration. The resulting ELF exports
`at_hw_send_ipd`, `atcmd_init`, `atcmd_task`, and `cip_recv_task`; the combined image layout was
verified. Build logs, source/config diff, and binary hashes are recorded under
`.cache/gino-validation/vw553-ipd-candidate-20261019/`. No firmware was flashed.

`CIPSEND` is prompt-driven. TCP send emits `>` in `atcmd_tcpip.c:418`; after payload transmission the
receive task emits `SEND OK\r\n` followed by the command handler's final `OK\r\n` (around
`1518-1526`). Failure similarly emits `SEND FAIL\r\n` followed by `ERROR\r\n`. The host transaction
client therefore treats only exact `OK` and `ERROR` lines as terminal. An informational `OK` before
the prompt does not finish a prompt transaction.

## Socket and BLE framing gaps

The TCP client IPD form is `+IPD,<fd>,<length>: <binary>`. The UDP server form is instead
`+IPD,<ip>:<port>, <length>: <binary>` (`atcmd_tcpip.c:1454-1458`). The H77 parser currently supports
only the descriptor form; UDP server payloads are rejected and taint the link. The parser must gain
an address-bearing datagram callback before AP configuration can use UDP server receive semantics.

`MSDK/ble/app/atcmd_ble.c:263-271` emits `+BLEDATA:<conn>,<length>,` and then sends the binary
payload, CRLF, and `OK` in separate calls. Connection events at `622-630` are asynchronous text
events. The existing H77 parser does not support the BLEDATA frame. It now recognizes the
`+BLEDATA:` prefix at the colon, before consuming any payload, and fails closed. A payload containing
CRLF or `OK` cannot complete an active AT transaction. The transaction client also rejects known
`+BLE...` lines and treats exact `READY` as a module reset. BLE requires a specified and atomic event
frame plus a length-aware parser callback before the BLE adapter can be enabled.

The current H77 parser/client is a transaction foundation only. Unmodified SDK 1.0.3g is gated to a
single idle `AT` probe. Socket commands require an explicitly selected IPD wire mode whose firmware
guarantees atomic frame boundaries. The existing BLE wire format remains unsupported in every mode.
The socket source reports `CLOSED` only as the response to an explicit close command; it does not
provide a formal asynchronous socket-close event suitable for detecting remote disconnects. The
source's `CIPSERVER` implementation also tracks one `local_srv_fd`, so TCP TLS service and UDP
discovery cannot be active as independent servers at the same time without additional 553 work.
