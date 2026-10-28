#!/usr/bin/env python3
"""Read-only GD32H77x UART ROM-ISP handshake and GETID probe."""

from __future__ import annotations

import argparse
import importlib.util
import logging
import sys
from pathlib import Path

_ISP_PATH = Path(__file__).resolve().parents[1] / "download/gd32h77x_uart_isp.py"
_ISP_SPEC = importlib.util.spec_from_file_location("gd32h77x_uart_isp", _ISP_PATH)
if _ISP_SPEC is None or _ISP_SPEC.loader is None:
    raise ImportError(f"GD32H77X UART ISP module is unavailable: {_ISP_PATH}")
_ISP = importlib.util.module_from_spec(_ISP_SPEC)
sys.modules[_ISP_SPEC.name] = _ISP
_ISP_SPEC.loader.exec_module(_ISP)
AUTO_UART_BAUDS = _ISP.AUTO_UART_BAUDS
SerialTransport = _ISP.SerialTransport
UartIsp = _ISP.UartIsp
UartIspError = _ISP.UartIspError


def _parse_rates(values) -> tuple[int, ...]:
    if not values:
        return AUTO_UART_BAUDS
    return tuple(int(value) for value in values)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Probe a GD32H77x ROM UART without erasing or writing flash")
    parser.add_argument("-p", "--port", required=True,
                        help="Serial port, for example /dev/ttyUSB0")
    parser.add_argument("-b", "--baud", action="append", type=int,
                        help="Candidate baud rate; repeat for several values. "
                             f"Default: {','.join(str(rate) for rate in AUTO_UART_BAUDS)}")
    parser.add_argument("--timeout", type=float, default=30.0,
                        help="Seconds to wait for BOOT + reset (default: 30)")
    args = parser.parse_args(argv)

    logging.basicConfig(level=logging.INFO, format="[probe] %(message)s")
    logger = logging.getLogger("gd32h77x-uart-probe")
    rates = _parse_rates(args.baud)
    transport = None
    try:
        transport = SerialTransport(args.port, rates[0])
        session = UartIsp(transport, logger=logger)
        selected = session.select_baud(rates, timeout=args.timeout)
        identifier = session.get_id()
        print("RESULT port={} baud={} id={}".format(
            args.port, selected, identifier.hex().upper() or "(empty)"))
        return 0
    except (UartIspError, OSError, ValueError) as exc:
        print("FAILED port={}: {}".format(args.port, exc), file=sys.stderr)
        return 1
    finally:
        if transport is not None:
            transport.close()


if __name__ == "__main__":
    raise SystemExit(main())
