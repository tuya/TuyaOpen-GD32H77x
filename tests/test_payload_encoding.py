#!/usr/bin/env python3
"""Guard the VW553 AT gateway against formatting binary payloads as text.

The vendor's co_snprintf() treats a "%c" NUL byte as end-of-string, so emitting
an IPD or BLE payload byte-by-byte silently drops every 0x00 and shifts the
frame. The current AT gateway must use byte-exact frame fragments or memcpy.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT.parent / "GD32" / "gd32_os" / "MSDK" / "vw553_at_gateway"
ATCMD_TCPIP = VENDOR / "atcmd" / "atcmd_tcpip.c"
ATCMD_BLE = VENDOR / "ble" / "atcmd_ble.c"

# Any active binary payload formatted through the vendor printf is unsafe.
FORBIDDEN = re.compile(r'AT_RSP\(\s*"%c"')
REQUIRED_TCP = [
    "cip_send_ipd_atomic(ipd_header, rx_buf, (uint32_t)recv_sz);",
    "at_hw_send_frame(fragments, (uint32_t)(sizeof(fragments) / sizeof(fragments[0])));",
    "sys_memcpy(rsp_buf + rsp_buf_idx, p_item->data, p_item->data_len);",
]
REQUIRED_BLE = [
    "ble_datatrans_srv_tx(conn_idx, tx_buf, cur_cnt)",
]


def remove_disabled_blocks(source: str) -> str:
    return re.sub(r'(?ms)^[ \t]*#if[ \t]+0\b.*?^[ \t]*#endif[^\n]*\n?', '', source)


def main() -> int:
    failures: list[str] = []
    for path in (ATCMD_TCPIP, ATCMD_BLE):
        if not path.is_file():
            print(f"missing {path}", file=sys.stderr)
            return 1

    tcp = remove_disabled_blocks(ATCMD_TCPIP.read_text(errors="replace"))
    ble = ATCMD_BLE.read_text(errors="replace")
    for line_number, line in enumerate(tcp.splitlines(), 1):
        if FORBIDDEN.search(line):
            failures.append(f"binary payload formatted with printf at {ATCMD_TCPIP}:{line_number}")
    for needle in REQUIRED_TCP:
        if needle not in tcp:
            failures.append(f"missing byte-exact TCP payload path: {needle}")
    for needle in REQUIRED_BLE:
        if needle not in ble:
            failures.append(f"missing BLE payload path: {needle}")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("VW553 payload encoding checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
