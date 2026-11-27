#!/usr/bin/env python3
"""Guard the AT CIPSTART lifecycle and the AP-broadcast gate.

The Cip Rcv worker retires itself when its last socket/listener closes.  If
cip_task_started is not resynchronised, the next CIPSTART skips task creation,
waits 500 ms for an IPC pair that will never appear, and answers ERROR after
the TCP connection was already stored.  That was the first-TCP-connection
failure seen after provisioning.

The AP netcfg thread can also queue one final broadcast after the vif left
SoftAP. The neutral AT socket layer must drop it before spending an AT
transaction; the Tuya TKL adapter remains a thin mapping layer.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT.parent / "GD32" / "gd32_os" / "MSDK" / "vw553_at_gateway"
ATCMD_TCPIP = VENDOR / "atcmd" / "atcmd_tcpip.c"
ATCMD_TCPIP_H = VENDOR / "atcmd" / "atcmd_tcpip.h"
SOCKET_BACKEND = ROOT / "gd32h77x_os" / "components" / "at" / "at_socket.c"
NETWORK_TKL = ROOT / "tuyaos" / "tuyaos_adapter" / "src" / "system" / "tkl_network.c"
WIFI_TKL = ROOT / "tuyaos" / "tuyaos_adapter" / "src" / "driver" / "tkl_wifi.c"
AT_DEVICE = ROOT / "gd32h77x_os" / "components" / "at" / "at_device.c"
MODULE_RESET = ROOT / "gd32h77x_os" / "components" / "at" / "at_module_reset.c"

STALE_FLAG = 'if ((cip_task_started == 0) || (!sys_task_exist((const uint8_t *)"Cip Rcv")))'


def main() -> int:
    failures: list[str] = []
    for path in (ATCMD_TCPIP, ATCMD_TCPIP_H):
        if not path.is_file():
            print(f"missing {path}", file=sys.stderr)
            return 1

    atcmd_tcpip = ATCMD_TCPIP.read_text(errors="replace")
    atcmd_tcpip_h = ATCMD_TCPIP_H.read_text(errors="replace")

    resync_count = atcmd_tcpip.count(STALE_FLAG)
    if resync_count < 3:
        failures.append(
            f"expected the stale-task resync in client and both server paths, found {resync_count}")
    if atcmd_tcpip.count("cip_client_abort(con_id);") < 2:
        failures.append("CIPSTART must release the stored client on task/IPC failure")
    if "Publish the retirement only after the IPC resources are gone." not in atcmd_tcpip:
        failures.append("Cip Rcv Exit must clear cip_task_started")
    if not re.search(r"Publish the retirement[^\n]*\n(?:[^\n]*\n){0,4}\s*cip_task_started = 0;",
                     atcmd_tcpip):
        failures.append("cip_task_started is not cleared in the Cip Rcv Exit path")

    bare_flag = re.compile(r"^\s*if \(cip_task_started == 0\) \{\s*$", re.MULTILINE)
    if bare_flag.search(atcmd_tcpip):
        failures.append("bare cip_task_started check reintroduced")

    backend = SOCKET_BACKEND.read_text(errors="replace")
    if "at_device_reset_and_recover()" in backend:
        failures.append("H77 CIPSTART failure still hard-resets VW553")
    if "STREAM_TX_CHUNK_SIZE 2048U" not in backend:
        failures.append("stream CIPSEND chunks do not use the VW553 2048-byte limit")
    if "mark_stream_send_failed" not in backend:
        failures.append("terminal CIPSEND failure does not invalidate the logical socket")
    if "leaving AT running" not in backend:
        failures.append("missing explicit no-reset CIPSTART timeout decision")
    if "listener stop deferred" not in backend:
        failures.append("H77 leaks a listener slot when its stop AT times out")
    if "ap_listener_invalid" not in backend or "AP vif switch invalidated" not in backend:
        failures.append("H77 does not release AP listeners locally on the vif switch")

    if "s_ap_teardown" not in backend or "peer == UINT32_MAX" not in backend:
        failures.append("AT socket does not gate AP-teardown broadcasts")

    tkl = NETWORK_TKL.read_text(errors="replace")
    if "at_socket_ap_teardown_active()" in tkl:
        failures.append("TKL must not own neutral AP broadcast policy")

    wifi_tkl = WIFI_TKL.read_text(errors="replace")
    if "at_device_reset();" in wifi_tkl:
        failures.append("tkl_wifi_init still hard-resets the AT module")
    if re.search(r"for \(unsigned attempt = 0U; attempt < 20U; \+\+attempt\)", wifi_tkl):
        failures.append("tkl_wifi_init still masks startup communication failures with retries")
    if "at_socket_ap_listeners_invalidate(true)" not in wifi_tkl:
        failures.append("tkl_wifi does not invalidate AP listeners when leaving SoftAP")
    if "ap start: leaving station" not in wifi_tkl:
        failures.append("tkl_wifi_start_ap does not disconnect STA before CWSAP_CUR")
    if "at_socket_wifi_join_set(true)" not in wifi_tkl:
        failures.append("tkl_wifi does not gate AT traffic during CWJAP")
    if "!s_wifi_join_active" not in wifi_tkl:
        failures.append("Wi-Fi status poll is not gated during CWJAP")
    if backend.count("if (s_wifi_join_active)") < 4:
        failures.append("socket backend is missing join gates for DNS/connect/send/listener")

    device = AT_DEVICE.read_text(errors="replace")
    reset_sequence = MODULE_RESET.read_text(errors="replace")
    if 'at_session_transact("AT+RST"' not in device:
        failures.append("H77 does not request AT soft reset with AT+RST")
    if "AT_MODULE_RESET_STARTUP" not in device or "AT_MODULE_RESET_SOFT_FIRST" not in device:
        failures.append("H77 recovery does not prefer soft reset before hardware reset")
    if "request = !s_module_reset_active;" not in device:
        failures.append("READY/fault notifications can recursively trigger module reset")
    if "hardware_reset_path(ops, context, ready_timeout_ms, result)" not in reset_sequence:
        failures.append("module reset has no hardware fallback path")
    if "for (" in reset_sequence:
        failures.append("module reset orchestration reintroduced an implicit retry loop")

    if "at_cip_stop_all_listeners" not in atcmd_tcpip:
        failures.append("AT stack does not close AP listeners on the STA switch")
    if re.search(r"if \(\(cip_info\.cli\[index\]\.fd >= 0\).*CIP_ROLE_SERVER", atcmd_tcpip):
        failures.append("client TX errors are still hidden from the host close-event path")
    if 'cip_info_cli_free_reason(idx, "TX_ERROR")' not in atcmd_tcpip:
        failures.append("TCP TX_ERROR does not emit a close event")
    if not re.search(r"#define MAX_SERVER_NUM\s+4\b", atcmd_tcpip_h):
        failures.append("AT server slots were not widened for the AP->LAN overlap")
    if "struct timeval tv = {12, 0};" not in atcmd_tcpip:
        failures.append("AT CIPSTART connect timeout is not 12 s")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("AT CIP lifecycle and AP broadcast gate checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
