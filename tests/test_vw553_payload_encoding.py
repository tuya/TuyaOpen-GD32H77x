#!/usr/bin/env python3
"""Guard the VW553 AT layer against binary payloads formatted through printf.

The vendor's co_snprintf() treats a "%c" NUL byte as end-of-string, so emitting
an IPD/BLEDATA payload byte-by-byte silently drops every 0x00 and shifts the
whole frame. Payloads must be copied with sys_memcpy() instead.

The module firmware is vendor code, so this test checks the recorded delta
against the vendor tree (patches/vw553-ginopi-at-fixes.patch) rather than a
build.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

PATCH = Path(__file__).resolve().parents[1] / "patches" / "vw553-ginopi-at-fixes.patch"

# Emitting binary payloads through the vendor printf drops NUL bytes.
FORBIDDEN = [
    re.compile(r'AT_RSP\(\s*"%c"\s*,\s*rx_buf\s*\['),
    re.compile(r'AT_RSP\(\s*"%c"\s*,\s*(?:const\s+)?uint8_t\s*\*?\s*data\s*\['),
]
# The IPD and BLEDATA payloads must be copied byte-exact.
REQUIRED = [
    "sys_memcpy(rsp_buf + rsp_buf_idx, rx_buf, recv_sz);",
    "sys_memcpy(rsp_buf + rsp_buf_idx, p_item->data, p_item->data_len);",
]


def added_lines(patch_text: str):
    for line in patch_text.splitlines():
        if line.startswith("+") and not line.startswith("+++"):
            yield line[1:]


def main() -> int:
    if not PATCH.is_file():
        print(f"missing {PATCH}", file=sys.stderr)
        return 1
    added = list(added_lines(PATCH.read_text(errors="replace")))

    failures = []
    for line in added:
        for pattern in FORBIDDEN:
            if pattern.search(line):
                failures.append(f"binary payload formatted with printf: {line.strip()}")
    joined = "\n".join(added)
    for needle in REQUIRED:
        if needle not in joined:
            failures.append(f"missing byte-exact payload copy: {needle}")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("VW553 payload encoding checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
