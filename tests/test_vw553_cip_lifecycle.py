#!/usr/bin/env python3
"""Guard the VW553 CIPSTART lifecycle and the H77 AP-broadcast gate.

The Cip Rcv worker retires itself when its last socket/listener closes.  If
cip_task_started is not resynchronised, the next CIPSTART skips task creation,
waits 500 ms for an IPC pair that will never appear, and answers ERROR after
the TCP connection was already stored.  That was the first-TCP-connection
failure seen after provisioning.

The AP netcfg thread can also queue one final broadcast after the vif left
SoftAP.  H77 must drop it in TKL instead of spending an AT transaction.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "patches" / "vw553-ginopi-at-fixes.patch"
SOCKET_BACKEND = ROOT / "gd32h77x_os" / "wireless" / "vw553" / "vw553_socket_backend.c"
NETWORK_TKL = ROOT / "tuyaos" / "tuyaos_adapter" / "src" / "system" / "tkl_network.c"
WIFI_TKL = ROOT / "tuyaos" / "tuyaos_adapter" / "src" / "driver" / "tkl_wifi.c"

STALE_FLAG = 'if ((cip_task_started == 0) || (!sys_task_exist((const uint8_t *)"Cip Rcv")))'


def added_lines(patch_text: str) -> list[str]:
    return [line[1:] for line in patch_text.splitlines()
            if line.startswith("+") and not line.startswith("+++")]


def main() -> int:
    failures: list[str] = []
    if not PATCH.is_file():
        print(f"missing {PATCH}", file=sys.stderr)
        return 1

    added = added_lines(PATCH.read_text(errors="replace"))
    joined = "\n".join(added)

    resync_count = joined.count(STALE_FLAG)
    if resync_count < 3:
        failures.append(
            f"expected the stale-task resync in client and both server paths, found {resync_count}")
    if joined.count("cip_client_abort(con_id);") < 2:
        failures.append("CIPSTART must release the stored client on task/IPC failure")
    if "Publish the retirement only after the IPC resources are gone." not in joined:
        failures.append("Cip Rcv Exit must clear cip_task_started")
    if not re.search(r"Publish the retirement[^\n]*\n(?:[^\n]*\n){0,4}\s*cip_task_started = 0;", joined):
        failures.append("cip_task_started is not cleared in the Cip Rcv Exit path")

    bare_flag = re.compile(r"^\s*if \(cip_task_started == 0\) \{\s*$", re.MULTILINE)
    for line in added:
        if bare_flag.match(line):
            failures.append("bare cip_task_started check reintroduced")
            break

    backend = SOCKET_BACKEND.read_text(errors="replace")
    if "vw553_device_reset_and_recover()" in backend:
        failures.append("H77 CIPSTART failure still hard-resets VW553")
    if "leaving VW553 running" not in backend:
        failures.append("missing explicit no-reset CIPSTART timeout decision")
    if "listener stop deferred" not in backend:
        failures.append("H77 leaks a listener slot when its stop AT times out")
    if "ap_listener_invalid" not in backend or "AP vif switch invalidated" not in backend:
        failures.append("H77 does not release AP listeners locally on the vif switch")

    tkl = NETWORK_TKL.read_text(errors="replace")
    if "vw553_socket_ap_teardown_active()" not in tkl:
        failures.append("tkl_net_send_to does not gate AP-teardown broadcasts")
    elif tkl.index("vw553_socket_ap_teardown_active()") > tkl.index("vw553_socket_sendto("):
        failures.append("AP broadcast gate is after the backend call")

    wifi_tkl = WIFI_TKL.read_text(errors="replace")
    if "vw553_socket_ap_listeners_invalidate(true)" not in wifi_tkl:
        failures.append("tkl_wifi does not invalidate AP listeners when leaving SoftAP")
    if "ap start: leaving station" not in wifi_tkl:
        failures.append("tkl_wifi_start_ap does not disconnect STA before CWSAP_CUR")
    if "vw553_socket_wifi_join_set(true)" not in wifi_tkl:
        failures.append("tkl_wifi does not gate AT traffic during CWJAP")
    if "!s_wifi_join_active" not in wifi_tkl:
        failures.append("Wi-Fi status poll is not gated during CWJAP")
    if backend.count("if (s_wifi_join_active)") < 4:
        failures.append("socket backend is missing join gates for DNS/connect/send/listener")

    if "at_cip_stop_all_listeners" not in joined:
        failures.append("VW553 does not close AP listeners on the STA switch")
    if not re.search(r"#define MAX_SERVER_NUM\s+4\b", joined):
        failures.append("VW553 server slots were not widened for the AP->LAN overlap")
    if "struct timeval tv = {12, 0};" not in joined:
        failures.append("VW553 CIPSTART connect timeout is not 12 s")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("VW553 CIP lifecycle and AP broadcast gate checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
