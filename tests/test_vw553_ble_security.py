#!/usr/bin/env python3
"""Guard the BLE provisioning link from vendor bond defaults.

VW553 resets its security configuration on BLEENABLE and defaults to bonded
pairing. Its peripheral connection path then sends a Security Request, causing
an OS pairing prompt before Tuya's application-level provisioning handshake.
The H77 adapter must explicitly disable BLE-layer bonding before advertising.
"""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
TKL_BLUETOOTH = ROOT / "tuyaos" / "tuyaos_adapter" / "src" / "driver" / "tkl_bluetooth.c"


def main() -> int:
    source = TKL_BLUETOOTH.read_text(errors="replace")
    enable = source.find('AT+BLEENABLE')
    set_auth = source.find('AT+BLESETAUTH=0,0,0,0,0,16')
    data_trans = source.find('AT+BLEDADATRANS=1')
    if enable < 0:
        print("missing AT+BLEENABLE", file=sys.stderr)
        return 1
    if set_auth < 0:
        print("missing AT+BLESETAUTH bonding override", file=sys.stderr)
        return 1
    if data_trans < 0:
        print("missing AT+BLEDADATRANS=1", file=sys.stderr)
        return 1
    if not enable < set_auth < data_trans:
        print("BLE security override must run after enable and before data transfer setup", file=sys.stderr)
        return 1
    print("VW553 BLE provisioning security policy checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
