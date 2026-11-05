"""GD32H77x UART ROM-ISP transport for CNVM and external OSPI NOR.

This module talks to the GigaDevice system-memory bootloader over a serial
port.  It is intentionally independent of SWD/pyOCD: the host only needs the
BOOT pin/reset sequence and the ROM UART protocol.

The protocol is derived from the vendor ``GD32_ISP_CLI`` V5.1.0 implementation:

* SPI-like command/ACK framing (``0x79`` ACK, ``0x1F`` NACK).
* ``0x06 0xF9`` GETID, ``0x31 0xCE`` WRITE, ``0x11 0xEE`` READ.
* ``0x08 0xF7`` OSPI configuration, ``0x81 0x7E`` OSPI pin configuration.
* ``0x84 0x7B`` erase one 4 KiB OSPI sector.
* ``0x44 0xBB`` extended page erase (16-bit page indices).

External NOR addresses use the ROM windows selected by each board binding:
OSPI0 at ``0x90000000`` and OSPI1 at ``0x70000000``.

Nothing in this module is board-verified yet. OSPI configuration bytes and
pin records must be supplied explicitly by the caller.
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence


PLATFORM_ROOT = Path(__file__).resolve().parents[3]

ISP_ACK = 0x79
ISP_NACK = 0x1F
ISP_SYNC = 0x7F

CMD_GET_ID = b"\x06\xf9"
CMD_GO = b"\x21\xde"
CMD_WRITE = b"\x31\xce"
CMD_READ = b"\x11\xee"
CMD_OSPI_CONFIG = b"\x08\xf7"
CMD_OSPI_PINS = b"\x81\x7e"
CMD_OSPI_ERASE_4K = b"\x84\x7b"
CMD_EXTENDED_ERASE = b"\x44\xbb"
CMD_BASE_ERASE = b"\x43\xbc"

CNVM_BASE = 0x08000000
OSPI0_WINDOW = 0x90000000
OSPI1_WINDOW = 0x70000000
OSPI_WINDOW_SIZE = 0x10000000
LOADER_BASE = 0x24010000
LOADER_JEDEC = 0xC84017

CNVM_PAGE = 0x10000
NOR_SECTOR = 0x1000
MAX_TRANSFER = 256
MAX_EXTENDED_ERASE_PAGES = 0xFFF9
ERASE_BATCH_PAGES = 64

DEFAULT_UART_BAUD = 115200
AUTO_UART_BAUDS = (921600, 460800, 230400, 115200, 57600, 9600)
PROGRESS_INTERVAL = 64 * 1024



class UartIspError(RuntimeError):
    """Protocol, layout, or verification failure in the UART ISP path."""


def _xor(data: bytes) -> int:
    value = 0
    for byte in data:
        value ^= byte
    return value & 0xFF


def _address_frame(address: int) -> bytes:
    if address < 0 or address > 0xFFFFFFFF:
        raise UartIspError("ROM ISP address is outside the 32-bit range")
    raw = int(address).to_bytes(4, "big")
    return raw + bytes([_xor(raw)])


def _be32_frame(value: int) -> bytes:
    if value < 0 or value > 0xFFFFFFFF:
        raise UartIspError("ROM ISP index is outside the 32-bit range")
    raw = int(value).to_bytes(4, "big")
    return raw + bytes([_xor(raw)])


def ospi_window(controller: str) -> int:
    """Return the ROM physical memory window for a declared OSPI controller."""
    value = str(controller).upper()
    if value == "OSPI0":
        return OSPI0_WINDOW
    if value == "OSPI1":
        return OSPI1_WINDOW
    raise UartIspError(f"unsupported UART NOR controller {controller!r}")


def _hex_bytes(value: str | bytes | bytearray | Sequence[int], length: int,
               label: str) -> bytes:
    if isinstance(value, (bytes, bytearray)):
        raw = bytes(value)
    elif isinstance(value, str):
        text = "".join(ch for ch in value.lower()
                       if ch not in " \t\r\n:-_,")
        if len(text) % 2:
            raise UartIspError(f"{label} must contain an even number of hex digits")
        try:
            raw = bytes.fromhex(text)
        except ValueError as exc:
            raise UartIspError(f"{label} is not valid hexadecimal") from exc
    else:
        raw = bytes(int(byte) & 0xFF for byte in value)
    if len(raw) != length:
        raise UartIspError(f"{label} must be exactly {length} raw bytes, got {len(raw)}")
    return raw


class SerialTransport:
    """Small pyserial wrapper used by :class:`UartIsp`.

    ``pyserial`` is imported lazily so unit tests and non-UART backends do not
    need the dependency loaded.
    """

    def __init__(self, port: str, baud: int = DEFAULT_UART_BAUD,
                 read_timeout: float = 0.05,
                 write_timeout: float = 2.0,
                 parity: int | None = None,
                 stopbits: int | None = None):
        if not str(port).strip():
            raise UartIspError("UART ROM ISP requires a serial port")
        try:
            import serial  # type: ignore
        except ImportError as exc:  # pragma: no cover - environment dependent
            raise UartIspError(
                "UART ROM ISP needs pyserial; install the SDK Python dependencies") from exc
        self._serial = serial.Serial(
            port=None,
            baudrate=int(baud),
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_EVEN if parity is None else parity,
            stopbits=serial.STOPBITS_ONE if stopbits is None else stopbits,
            timeout=read_timeout,
            write_timeout=write_timeout,
            dsrdtr=False,
            rtscts=False,
        )
        # CH340 and similar USB bridges may wire modem lines to reset.  Opening
        # the port must not reset a target that the user has already put into
        # ROM boot mode with BOOT + reset.
        self._serial.dtr = False
        self._serial.rts = False
        self._serial.port = str(port)
        self._serial.open()
        self.port = str(port)
        self.baud = int(baud)

    def write(self, data: bytes) -> None:
        self._serial.write(data)
        self._serial.flush()

    def read(self, size: int) -> bytes:
        return self._serial.read(size)

    def reset_input_buffer(self) -> None:
        self._serial.reset_input_buffer()

    def set_baud(self, baud: int) -> None:
        self._serial.baudrate = int(baud)
        self._serial.bytesize = 8
        self._serial.parity = "E"
        self._serial.stopbits = 1
        self._serial.reset_input_buffer()
        self.baud = int(baud)

    def set_format(self, baud: int, parity: int, stopbits: int) -> None:
        """Reconfigure an open port for the SRAM loader protocol."""
        self._serial.baudrate = int(baud)
        self._serial.bytesize = 8
        self._serial.parity = parity
        self._serial.stopbits = stopbits
        self.baud = int(baud)

    def close(self) -> None:
        self._serial.close()


class UartIsp:
    """Minimal GigaDevice ROM ISP client."""

    def __init__(self, transport: Any, timeout: float = 5.0, logger=None):
        self.transport = transport
        self.timeout = float(timeout)
        self.logger = logger

    # -- low level ---------------------------------------------------------
    def _log(self, message: str) -> None:
        if self.logger:
            self.logger.info(message)

    def _read_exact(self, size: int, context: str, timeout: float | None = None) -> bytes:
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        chunks = bytearray()
        while len(chunks) < size:
            remaining = size - len(chunks)
            data = self.transport.read(remaining)
            if data:
                chunks.extend(data)
                continue
            if time.monotonic() >= deadline:
                raise UartIspError(
                    f"timeout waiting for {size} byte(s) during {context}; "
                    f"received {bytes(chunks).hex() or '(none)'}")
            time.sleep(0.002)
        return bytes(chunks)

    def _expect_ack(self, context: str, timeout: float | None = None) -> None:
        value = self._read_exact(1, context, timeout)[0]
        if value == ISP_NACK:
            raise UartIspError(f"target returned NACK during {context}")
        if value != ISP_ACK:
            raise UartIspError(
                f"target returned 0x{value:02X} instead of ACK during {context}")

    # -- lifecycle ---------------------------------------------------------
    def handshake(self, retries: int = 200, delay: float = 0.05) -> None:
        # Keep polling for roughly 30 seconds so the operator can hold BOOT
        # and tap reset after starting the flash command.  The ROM only listens
        # for a short period after reset, so polling must already be running.
        self._log("Waiting for ROM ISP handshake; hold BOOT and reset the board")
        last = ""
        for attempt in range(retries):
            reset = getattr(self.transport, "reset_input_buffer", None)
            if callable(reset):
                reset()
            self.transport.write(bytes([ISP_SYNC]))
            try:
                value = self._read_exact(1, "handshake", timeout=0.25)[0]
                if value == 0x00:
                    value = self._read_exact(1, "handshake", timeout=0.25)[0]
                if value in (ISP_ACK, ISP_NACK):
                    self._log("ROM ISP handshake completed")
                    return
                last = f"unexpected byte 0x{value:02X}"
            except UartIspError as exc:
                last = str(exc)
            time.sleep(delay)
        raise UartIspError(f"ROM ISP handshake failed after {retries} attempts: {last}")

    def get_id(self, timeout: float | None = None) -> bytes:
        self.transport.write(CMD_GET_ID)
        # The H77x ROM needs a short settle time before it starts returning the
        # GETID response; the vendor CLI waits 50 ms before reading the ACK.
        time.sleep(0.05)
        self._expect_ack("GETID command", timeout)
        length = self._read_exact(1, "GETID length", timeout)[0]
        identifier = self._read_exact(length, "GETID payload", timeout) if length else b""
        self._expect_ack("GETID completion", timeout)
        return identifier

    def select_baud(self, rates: Iterable[int] = AUTO_UART_BAUDS,
                    timeout: float = 60.0, delay: float = 0.02) -> int:
        """Capture the ROM handshake at the first responding candidate rate."""

        candidates = tuple(int(rate) for rate in rates)
        if not candidates or any(rate <= 0 for rate in candidates):
            raise UartIspError("UART baud candidates must be positive integers")
        candidates = tuple(dict.fromkeys(candidates))
        set_baud = getattr(self.transport, "set_baud", None)
        if not callable(set_baud):
            raise UartIspError(
                "UART baud auto-negotiation requires a transport with set_baud()")

        def probe(rate: int):
            set_baud(rate)
            reset = getattr(self.transport, "reset_input_buffer", None)
            if callable(reset):
                reset()
            self.transport.write(bytes([ISP_SYNC]))
            value = self._read_exact(1, "handshake", timeout=0.1)[0]
            if value == 0x00:
                value = self._read_exact(1, "handshake", timeout=0.1)[0]
            if value not in (ISP_ACK, ISP_NACK):
                raise UartIspError(
                    "unexpected byte 0x{:02X} at {}".format(value, rate))
            return None

        preferred = min(candidates, key=lambda rate: abs(rate - DEFAULT_UART_BAUD))
        # Honor the candidate list's declared fallback order after the safe
        # preferred rate. The default list keeps 9600 as its final low-rate
        # fallback instead of promoting it merely because it is numerically
        # close to 115200.
        wait_order = [preferred] + [rate for rate in candidates if rate != preferred]
        self._log(
            "Waiting for ROM ISP handshake; hold BOOT and reset the board "
            "(safe {}, then up to {})".format(preferred, max(candidates)))
        deadline = time.monotonic() + float(timeout)
        last_error = "no response"
        captured_rate = None
        captured_id = b""
        while captured_rate is None and time.monotonic() < deadline:
            for rate in wait_order:
                if time.monotonic() >= deadline:
                    break
                try:
                    probe(rate)
                    # Capture only the synchronisation ACK. Keep this rate;
                    # changing baud after ROM capture can lose the session.
                    captured_id = b""
                    captured_rate = rate
                    break
                except UartIspError as exc:
                    last_error = "{} at {}".format(exc, rate)
                time.sleep(delay)
        if captured_rate is None:
            raise UartIspError(
                "ROM ISP baud negotiation failed after {:.0f}s: {}".format(
                    timeout, last_error))

        self._log(
            "ROM ISP handshake captured at {} 8E1".format(captured_rate))
        return captured_rate

    # -- memory ------------------------------------------------------------
    def write(self, address: int, data: bytes, timeout: float | None = None) -> None:
        payload = bytes(data)
        if not payload or len(payload) > MAX_TRANSFER:
            raise UartIspError(f"WRITE chunk must be 1..{MAX_TRANSFER} bytes")
        length_minus_one = len(payload) - 1
        checksum = length_minus_one ^ _xor(payload)
        self.transport.write(CMD_WRITE)
        self._expect_ack("WRITE command", timeout)
        self.transport.write(_address_frame(address))
        self._expect_ack("WRITE address", timeout)
        self.transport.write(bytes([length_minus_one]) + payload + bytes([checksum]))
        self._expect_ack("WRITE completion", timeout)

    def read(self, address: int, length: int, timeout: float | None = None) -> bytes:
        if length <= 0 or length > MAX_TRANSFER:
            raise UartIspError(f"READ length must be 1..{MAX_TRANSFER} bytes")
        self.transport.write(CMD_READ)
        self._expect_ack("READ command", timeout)
        self.transport.write(_address_frame(address))
        self._expect_ack("READ address", timeout)
        self.transport.write(bytes([length - 1, (-length) & 0xFF]))
        response = self._read_exact(length + 1, "READ payload", timeout)
        if response[0] != ISP_ACK:
            raise UartIspError(
                f"READ payload did not start with ACK: 0x{response[0]:02X}")
        return response[1:]

    def go(self, address: int, timeout: float | None = None) -> None:
        """Enter the ROM GO command and jump to a vector table at *address*."""
        raw = int(address).to_bytes(4, "big")
        frame = raw + bytes([_xor(raw)])
        self.transport.write(CMD_GO)
        self._expect_ack("GO command", timeout)
        self.transport.write(frame)
        self._expect_ack("GO address", timeout)
        self.transport.write(frame)
        self._expect_ack("GO confirmation", timeout)

    def load_ram_image(self, address: int, data: bytes,
                       timeout: float | None = None) -> None:
        """Write an image into a ROM-writable RAM window with 4-byte padding."""
        payload = bytes(data)
        if not payload:
            raise UartIspError("RAM loader image is empty")
        for offset in range(0, len(payload), MAX_TRANSFER):
            chunk = payload[offset:offset + MAX_TRANSFER]
            padded = chunk + b"\xFF" * ((-len(chunk)) % 4)
            self.write(int(address) + offset, padded, timeout=timeout)

    # -- erase -------------------------------------------------------------
    def erase_pages(self, pages: Iterable[int], mode: str = "extended",
                    timeout: float | None = None) -> None:
        page_list = [int(page) for page in pages]
        if not page_list:
            return
        if mode not in ("extended", "base"):
            raise UartIspError("UART erase mode must be 'extended' or 'base'")
        if len(page_list) > MAX_EXTENDED_ERASE_PAGES:
            raise UartIspError("too many pages for one ROM erase command")
        if mode == "base" and any(page > 0xFF for page in page_list):
            raise UartIspError("base page erase cannot address pages above 255")
        count_minus_one = len(page_list) - 1
        if mode == "extended":
            body = bytearray(count_minus_one.to_bytes(2, "big"))
            for page in page_list:
                if page < 0 or page > 0xFFFF:
                    raise UartIspError(f"page index {page} is outside 16-bit range")
                body.extend(int(page).to_bytes(2, "big"))
            command = CMD_EXTENDED_ERASE
        else:
            body = bytearray([count_minus_one])
            # BL_Base_ERASE emits the page list in reverse order; its checksum
            # still covers every page byte plus the leading count-1.
            body.extend(page & 0xFF for page in reversed(page_list))
            command = CMD_BASE_ERASE
        body.append(_xor(bytes(body)))
        self.transport.write(command)
        self._expect_ack("page erase command", timeout)
        self.transport.write(bytes(body))
        self._expect_ack("page erase completion", timeout)

    def erase_ospi_sector(self, address: int, sector_index: int,
                          timeout: float | None = None) -> None:
        self.transport.write(CMD_OSPI_ERASE_4K)
        self._expect_ack("OSPI erase command", timeout)
        self.transport.write(_address_frame(address))
        self._expect_ack("OSPI erase address", timeout)
        self.transport.write(_be32_frame(sector_index))
        self._expect_ack("OSPI erase index", timeout)

    # -- OSPI configuration ------------------------------------------------
    def config_ospi(self, config: bytes,
                    timeout: float | None = None) -> None:
        raw = _hex_bytes(config, 3, "OSPI configuration")
        self.transport.write(CMD_OSPI_CONFIG)
        self._expect_ack("OSPI configuration command", timeout)
        self.transport.write(raw + bytes([_xor(raw)]))
        self._expect_ack("OSPI configuration completion", timeout)

    def config_ospi_pins(self, pins: bytes,
                         timeout: float | None = None) -> None:
        raw = _hex_bytes(pins, 5, "OSPI pin configuration")
        self.transport.write(CMD_OSPI_PINS)
        self._expect_ack("OSPI pin command", timeout)
        self.transport.write(raw + bytes([_xor(raw)]))
        self._expect_ack("OSPI pin completion", timeout)

    def close(self) -> None:
        close = getattr(self.transport, "close", None)
        if callable(close):
            close()


# ---------------------------------------------------------------------------
# AXI SRAM UART loader protocol
# ---------------------------------------------------------------------------

LOADER_BAUD = 115200
LOADER_FRAME_SYNC = 0x5A
LOADER_STATUS_OK = 0x06
LOADER_STATUS_ERROR = 0x15

LOADER_CMD_PING = 0x01
LOADER_CMD_READ = 0x02
LOADER_CMD_ERASE = 0x03
LOADER_CMD_PROGRAM = 0x04
LOADER_CMD_RESET = 0x05
LOADER_CMD_INFO = 0x06


class UartLoader:
    """Client for the AXI SRAM loader entered through the ROM GO command."""

    def __init__(self, transport: Any, timeout: float = 5.0, logger=None):
        self.transport = transport
        self.timeout = float(timeout)
        self.logger = logger

    def _log(self, message: str) -> None:
        if self.logger:
            self.logger.info(message)

    def _read_exact(self, size: int, context: str,
                    timeout: float | None = None) -> bytes:
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        chunks = bytearray()
        while len(chunks) < size:
            data = self.transport.read(size - len(chunks))
            if data:
                chunks.extend(data)
                continue
            if time.monotonic() >= deadline:
                raise UartIspError(
                    f"timeout waiting for {size} byte(s) during {context}; "
                    f"received {bytes(chunks).hex() or '(none)'}")
            time.sleep(0.002)
        return bytes(chunks)

    @staticmethod
    def _frame(command: int, offset: int = 0, length: int = 0) -> bytes:
        body = (bytes([command & 0xFF]) + int(offset).to_bytes(4, "big") +
                int(length).to_bytes(2, "big"))
        return bytes([LOADER_FRAME_SYNC]) + body + bytes([_xor(body)])

    def _command(self, command: int, offset: int = 0, length: int = 0,
                 timeout: float | None = None) -> None:
        self.transport.write(self._frame(command, offset, length))
        status = self._read_exact(1, "loader status", timeout)[0]
        if status != LOADER_STATUS_OK:
            raise UartIspError(
                f"SRAM loader rejected command 0x{command:02X} "
                f"(status 0x{status:02X})")

    def handshake(self, retries: int = 40, timeout: float = 0.3,
                  delay: float = 0.05) -> int:
        """Ping until the loader answers; the first frames may race its init."""
        last_error = "no attempt"
        for _ in range(int(retries)):
            try:
                self.transport.write(self._frame(LOADER_CMD_PING))
                status = self._read_exact(1, "loader status", timeout)[0]
                if status != LOADER_STATUS_OK:
                    raise UartIspError(
                        f"SRAM loader PING status 0x{status:02X}")
                data = self._read_exact(4, "loader JEDEC ID", timeout)
                self._log("SRAM UART loader answered PING")
                return int.from_bytes(data, "big")
            except UartIspError as exc:
                last_error = str(exc)
                time.sleep(delay)
        raise UartIspError(
            f"SRAM UART loader did not respond to PING: {last_error}")

    def ping(self, timeout: float | None = None) -> int:
        return self.handshake(retries=1, timeout=self.timeout if timeout is None
                              else timeout, delay=0.0)

    def info(self, timeout: float | None = None) -> dict[str, int]:
        self._command(LOADER_CMD_INFO, timeout=timeout)
        data = self._read_exact(6, "loader NOR status", timeout)
        return {
            "jedec_id": int.from_bytes(data[0:4], "big"),
            "status1": data[4],
            "status2": data[5],
        }

    def read(self, offset: int, length: int,
             timeout: float | None = None) -> bytes:
        if length <= 0 or length > MAX_TRANSFER:
            raise UartIspError(f"loader READ length must be 1..{MAX_TRANSFER}")
        self._command(LOADER_CMD_READ, offset, length, timeout=timeout)
        return self._read_exact(length, "loader READ payload", timeout)

    def erase(self, offset: int, timeout: float | None = None) -> None:
        self._command(LOADER_CMD_ERASE, offset, NOR_SECTOR, timeout=timeout)

    def program(self, offset: int, data: bytes,
                timeout: float | None = None) -> None:
        payload = bytes(data)
        if not payload or len(payload) > MAX_TRANSFER:
            raise UartIspError(f"loader PROGRAM length must be 1..{MAX_TRANSFER}")
        self._command(LOADER_CMD_PROGRAM, offset, len(payload), timeout=timeout)
        self.transport.write(payload)
        status = self._read_exact(1, "loader PROGRAM result", timeout)[0]
        if status != LOADER_STATUS_OK:
            raise UartIspError(
                f"SRAM loader failed to program 0x{offset:06X} "
                f"(status 0x{status:02X})")

    def reset(self, timeout: float | None = None) -> None:
        self._command(LOADER_CMD_RESET, timeout=timeout)


# ---------------------------------------------------------------------------
# Payload planning / programming
# ---------------------------------------------------------------------------

def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _read_external_payload_data(plan: list[dict[str, Any]]) -> list[bytes]:
    payload_data = []
    for payload in plan:
        path = Path(payload["path"])
        data = path.read_bytes()
        if len(data) != int(payload["size_bytes"]) or _sha256(path) != payload["sha256"]:
            raise UartIspError(
                f"external payload {payload['partition']!r} changed after validation")
        payload_data.append(data)
    return payload_data


def validate_ospi_payloads(manifest: dict[str, Any],
                           external_payloads: list[dict[str, Any]]):
    """Validate generic resolved NOR payloads without board/model assumptions."""
    layout = manifest.get("layout", manifest)
    if not isinstance(layout, dict):
        raise UartIspError("resolved layout is missing")
    devices_value = layout.get("devices")
    if isinstance(devices_value, list):
        devices = {str(item.get("id")): item for item in devices_value
                   if isinstance(item, dict) and item.get("id")}
    elif isinstance(devices_value, dict):
        devices = devices_value
    else:
        raise UartIspError("resolved layout has no device table")
    partitions = layout.get("partitions")
    bindings = layout.get("board_binding")
    if not isinstance(partitions, dict) or not isinstance(bindings, dict):
        raise UartIspError("resolved layout partitions or board bindings are missing")

    package = _package_module()
    plans = []
    physical_ranges: dict[str, list[tuple[int, int, str]]] = {}
    for payload in external_payloads:
        if not isinstance(payload, dict):
            raise UartIspError("external payload entry must be an object")
        part_name = str(payload.get("partition", ""))
        path_value = payload.get("path")
        if not path_value:
            raise UartIspError(f"external payload {part_name!r} has no file")
        path = Path(str(path_value)).resolve(strict=True)
        if not path.is_file():
            raise UartIspError(f"external payload is not a regular file: {path}")
        size = path.stat().st_size
        expected_hash = str(payload.get("sha256", "")).lower()
        if (size <= 0 or int(payload.get("size_bytes", -1)) != size
                or len(expected_hash) != 64 or _sha256(path) != expected_hash):
            raise UartIspError(f"external payload {part_name!r} size/hash mismatch")
        try:
            padded_size, _partition_erase, extents, _role = package._payload_plan(
                layout, part_name, size)
        except (package.PackageError, KeyError, TypeError, ValueError) as exc:
            raise UartIspError(str(exc)) from exc
        if padded_size != size:
            raise UartIspError(f"external payload {part_name!r} is not erase padded")
        spans = []
        for extent in extents:
            device_id = str(extent["device"])
            device = devices.get(device_id)
            binding = bindings.get(device_id)
            if (not isinstance(device, dict) or not isinstance(binding, dict)
                    or str(device.get("region", "")).upper() != "NOR"
                    or device.get("kind") != "flash" or device.get("access") != "indirect"):
                raise UartIspError(f"partition {part_name!r} references a non-indirect NOR device")
            controller = str(binding.get("controller", "")).upper()
            window = ospi_window(controller)
            dev_off = int(extent["device_offset_bytes"])
            part_off = int(extent["partition_offset_bytes"])
            seg_size = int(extent["size_bytes"])
            capacity = int(device.get("capacity_bytes", 0))
            erase_bytes = int(device.get("erase_bytes", 0))
            page_bytes = int(device.get("page_bytes", 0))
            if erase_bytes != NOR_SECTOR or page_bytes != MAX_TRANSFER:
                raise UartIspError(
                    f"UART NOR device {device_id!r} must declare 4 KiB erase and 256-byte pages")
            if (capacity > OSPI_WINDOW_SIZE or dev_off < 0 or seg_size <= 0
                    or dev_off + seg_size > capacity
                    or dev_off + seg_size > OSPI_WINDOW_SIZE):
                raise UartIspError(f"partition {part_name!r} has a gap or out-of-range NOR segment")
            end = dev_off + seg_size
            ranges = physical_ranges.setdefault(device_id, [])
            if any(dev_off < old_end and old_start < end for old_start, old_end, _ in ranges):
                raise UartIspError(f"external payload {part_name!r} overlaps another payload on {device_id!r}")
            ranges.append((dev_off, end, part_name))
            spans.append({"device": device_id, "controller": controller,
                          "window": window, "device_offset_bytes": dev_off,
                          "partition_offset_bytes": part_off,
                          "program_size_bytes": seg_size,
                          "erase_start_bytes": dev_off,
                          "erase_end_bytes": end,
                          "page_bytes": page_bytes})
        if sum(span["program_size_bytes"] for span in spans) != size:
            raise UartIspError(f"external payload {part_name!r} is not fully mapped")
        plans.append({"partition": part_name, "path": str(path), "size_bytes": size,
                      "sha256": expected_hash, "spans": spans})
    return layout, devices, plans


def _package_module():
    path = Path(__file__).resolve().parents[1] / "firmware" / "flash_package.py"
    spec = importlib.util.spec_from_file_location("gd32h77x_flash_package_for_uart", path)
    if spec is None or spec.loader is None:
        raise UartIspError(f"flash package validator is unavailable: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _format_duration(seconds: float) -> str:
    seconds = max(0.0, float(seconds))
    if seconds < 60:
        return "{:.1f}s".format(seconds)
    minutes, remainder = divmod(int(seconds), 60)
    return "{}m{:02d}s".format(minutes, remainder)


class _TransferProgress:
    """Throttled byte progress with throughput and ETA logging."""

    def __init__(self, logger, action: str, total: int,
                 interval: int = PROGRESS_INTERVAL):
        self.logger = logger
        self.action = action
        self.total = int(total)
        self.interval = max(1, int(interval))
        self.started = time.monotonic()
        self.next_report = self.interval
        self.last_report = -1

    def update(self, done: int, force: bool = False) -> None:
        if not self.logger or not self.total:
            return
        done = min(int(done), self.total)
        if done == self.last_report:
            return
        if not force and done < self.next_report and done != self.total:
            return
        elapsed = max(1e-6, time.monotonic() - self.started)
        rate = done / elapsed
        remaining = max(0, self.total - done)
        eta = remaining / rate if rate > 0 else 0.0
        self.logger.info(
            "{}: {}/{} bytes ({:.1f}%), {:.1f} KiB/s, elapsed {}, ETA {}".format(
                self.action, done, self.total, done * 100.0 / self.total,
                rate / 1024.0, _format_duration(elapsed), _format_duration(eta)))
        self.last_report = done
        while self.next_report <= done:
            self.next_report += self.interval

    def finish(self) -> None:
        self.update(self.total, force=True)


def _write_chunk(session, address: int, data: bytes) -> None:
    session.write(address, data)


def _verify_range(session, address: int, expected: bytes, label: str, logger=None) -> None:
    progress = _TransferProgress(logger, "Verifying {}".format(label), len(expected))
    for offset in range(0, len(expected), MAX_TRANSFER):
        count = min(MAX_TRANSFER, len(expected) - offset)
        actual = session.read(address + offset, count)
        wanted = expected[offset:offset + count]
        if actual != wanted:
            for index, (left, right) in enumerate(zip(wanted, actual)):
                if left != right:
                    raise UartIspError(
                        f"readback mismatch in {label} at "
                        f"0x{address + offset + index:08X}: "
                        f"wrote 0x{left:02X}, read 0x{right:02X}")
            raise UartIspError(f"readback length mismatch in {label}")
        progress.update(offset + count)
    progress.finish()


def program_cnvm(session, image_path: str | Path, start: int, capacity: int,
                 erase_bytes: int, mode: str = "extended", verify: bool = True,
                 logger=None, image_bytes: bytes | None = None) -> dict[str, Any]:
    """Erase/program only the CNVM pages covered by the firmware image."""

    image = Path(image_path)
    data = image.read_bytes() if image_bytes is None else bytes(image_bytes)
    if not data:
        raise UartIspError("internal CNVM image is empty")
    if int(start) != CNVM_BASE:
        raise UartIspError(
            f"UART CNVM programming expects start 0x{CNVM_BASE:08X}, got 0x{int(start):08X}")
    if int(erase_bytes) != CNVM_PAGE:
        raise UartIspError(
            f"UART CNVM programming requires {CNVM_PAGE}-byte sectors, got {erase_bytes}")
    if len(data) > int(capacity):
        raise UartIspError("internal CNVM image exceeds the resolved application capacity")
    page_count = (len(data) + CNVM_PAGE - 1) // CNVM_PAGE
    if page_count > int(capacity) // CNVM_PAGE:
        raise UartIspError("internal CNVM image covers more pages than the application range")
    pages = list(range(page_count))
    for offset in range(0, len(pages), ERASE_BATCH_PAGES):
        batch = pages[offset:offset + ERASE_BATCH_PAGES]
        if logger:
            logger.info("Erasing CNVM pages {}..{}".format(batch[0], batch[-1]))
        session.erase_pages(batch, mode=mode)

    progress = _TransferProgress(logger, "Writing CNVM", len(data))
    written = 0
    while written < len(data):
        chunk = data[written:written + MAX_TRANSFER]
        # The ROM's aligned WRITE path pads to a 4-byte boundary.  Pad here so
        # the final short write uses the same checksum rules as every other
        # chunk.  Never let that padding cross the build-bound application
        # range, which may end immediately before OTA/metadata/KV partitions.
        padded = chunk + b"\xFF" * ((-len(chunk)) % 4)
        if written + len(padded) > int(capacity):
            raise UartIspError(
                "CNVM final WRITE alignment padding would exceed the application range")
        _write_chunk(session, CNVM_BASE + written, padded)
        written += len(chunk)
        progress.update(written)
    progress.finish()
    if verify:
        _verify_range(session, CNVM_BASE, data, "CNVM image", logger=logger)
    return {"pages": page_count, "size_bytes": len(data), "verified": bool(verify)}


def program_ospi(session, manifest: dict[str, Any],
                 external_payloads: list[dict[str, Any]],
                 ospi_profiles: Mapping[str, Any] | None = None,
                 verify: bool = True, logger=None) -> list[dict[str, Any]]:
    """Erase only payload-covered 4 KiB sectors, write, and optionally verify."""

    layout, devices, plan = validate_ospi_payloads(manifest, external_payloads)
    profiles = _validate_ospi_profiles(layout, plan, ospi_profiles)
    payload_data = []
    for payload in plan:
        path = Path(payload["path"])
        data = path.read_bytes()
        if len(data) != int(payload["size_bytes"]) or _sha256(path) != payload["sha256"]:
            raise UartIspError(
                f"external payload {payload['partition']!r} changed after validation")
        payload_data.append(data)
    results: list[dict[str, Any]] = []
    active_device = None
    for payload, data in zip(plan, payload_data):
        if logger:
            logger.info("Programming OSPI partition {} ({} bytes)".format(
                payload["partition"], len(data)))
        progress = _TransferProgress(
            logger, "Writing OSPI partition {}".format(payload["partition"]),
            len(data))
        erase_sectors = sum(
            (int(span["erase_end_bytes"]) - int(span["erase_start_bytes"])) // NOR_SECTOR
            for span in payload["spans"])
        erase_progress = _TransferProgress(
            logger, "Erasing OSPI partition {}".format(payload["partition"]),
            erase_sectors * NOR_SECTOR, interval=1024 * 1024)
        erased_sectors = 0
        consumed = 0
        programmed_total = 0
        for span in payload["spans"]:
            device_id = span["device"]
            if device_id != active_device:
                profile = profiles[device_id]
                session.config_ospi(profile["config"])
                for pin_record in profile["pins"]:
                    session.config_ospi_pins(pin_record)
                active_device = device_id
            offset = int(span["device_offset_bytes"])
            count = int(span["program_size_bytes"])
            erase_start = int(span["erase_start_bytes"])
            erase_end = int(span["erase_end_bytes"])
            for sector in range(erase_start, erase_end, NOR_SECTOR):
                session.erase_ospi_sector(
                    int(span["window"]) + sector, sector // NOR_SECTOR)
                erased_sectors += 1
                erase_progress.update(erased_sectors * NOR_SECTOR)
            chunk = data[consumed:consumed + count]
            written = 0
            while written < len(chunk):
                page_remaining = int(span["page_bytes"]) - ((offset + written) % int(span["page_bytes"]))
                piece = chunk[written:written + min(MAX_TRANSFER, page_remaining)]
                padded = piece + b"\xFF" * ((-len(piece)) % 4)
                write_end = offset + written + len(padded)
                if write_end > erase_end:
                    raise UartIspError(
                        "OSPI WRITE alignment padding would exceed the erased "
                        f"span for partition {payload['partition']!r}")
                session.write(int(span["window"]) + offset + written, padded)
                written += len(piece)
                programmed_total += len(piece)
                progress.update(programmed_total)
            if verify:
                _verify_range(
                    session, int(span["window"]) + offset, chunk,
                    f"OSPI partition {payload['partition']!r}", logger=logger)
            consumed += count
        erase_progress.finish()
        progress.finish()
        if consumed != len(data):
            raise UartIspError(
                f"external payload {payload['partition']!r} was not fully mapped")
        results.append({"partition": payload["partition"],
                        "size_bytes": len(data), "verified": bool(verify)})
    return results


def _arm_none_eabi_gcc() -> str:
    """Locate the ARM compiler used to build the SRAM UART loader."""
    toolchain = os.environ.get("GD32H77X_TOOLCHAIN")
    candidates = (os.environ.get("ARM_NONE_EABI_GCC"),
                  shutil.which("arm-none-eabi-gcc"),
                  str(Path(toolchain) / "bin" / "arm-none-eabi-gcc") if toolchain else None,
                  "/opt/gcc-arm-none-eabi-10.3-2021.10/bin/arm-none-eabi-gcc")
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return str(candidate)
    raise UartIspError(
        "the UART OSPI loader needs arm-none-eabi-gcc; set ARM_NONE_EABI_GCC")


def build_uart_loader(layout: dict[str, Any], out_dir: str | Path):
    """Compile the UART-entered SRAM loader for one resolved layout."""
    builder = PLATFORM_ROOT / "gd32h77x_os/tools/download/ospi_loader/build_loader.py"
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    layout_path = out / "layout.json"
    layout_path.write_text(json.dumps({"layout": layout}, sort_keys=True))
    command = [sys.executable, str(builder), "--uart",
               "--layout", str(layout_path), "--out-dir", str(out),
               "--compiler", _arm_none_eabi_gcc()]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        raise UartIspError(f"failed to build the SRAM UART loader: {detail}")
    metadata = json.loads((out / "loader.json").read_text())
    return out / metadata["binary"], metadata


def prepare_ospi_loader(manifest: dict[str, Any],
                        external_payloads: list[dict[str, Any]]) -> dict[str, Any]:
    """Validate and build the one-shot SRAM loader before opening the UART."""
    layout, devices, plan = validate_ospi_payloads(manifest, external_payloads)
    used = {str(span["device"]) for payload in plan for span in payload["spans"]}
    if len(used) != 1:
        raise UartIspError(
            "the SRAM UART loader supports exactly one external NOR device")
    device_id = next(iter(used))
    device_items = (list(devices.values()) if isinstance(devices, dict)
                    else list(devices))
    if not any(str(device.get("id")) == device_id
               and str(device.get("part_number", "")).upper() == "GD25Q64ESIG"
               for device in device_items):
        raise UartIspError(
            f"the SRAM UART loader only supports GD25Q64ESIG, got {device_id!r}")
    payload_data = _read_external_payload_data(plan)
    with tempfile.TemporaryDirectory(prefix="h77-uart-loader-") as tmp:
        binary_path, metadata = build_uart_loader(layout, tmp)
        image = binary_path.read_bytes()
    return {
        "layout": layout,
        "plan": plan,
        "payload_data": payload_data,
        "image": image,
        "metadata": metadata,
    }


def program_ospi_loader(session, manifest: dict[str, Any] | None = None,
                        external_payloads: list[dict[str, Any]] | None = None,
                        verify: bool = True, logger=None,
                        prepared: dict[str, Any] | None = None) -> list[dict[str, Any]]:
    """Program external NOR through the ROM GO entry and the SRAM UART loader.

    The GD25Q64E JEDEC ID differs from the value hard-coded in the H77x ROM, so
    the ROM OSPI command cannot initialise this part.  Instead the ROM WRITE
    command uploads a small AXI SRAM loader and GO jumps to it; the loader
    re-initialises the console UART and bit-bangs the resolved OSPI pins.
    """
    if prepared is None:
        if manifest is None or external_payloads is None:
            raise UartIspError(
                "SRAM UART loader needs a manifest and external payloads")
        prepared = prepare_ospi_loader(manifest, external_payloads)
    plan = prepared["plan"]
    payload_data = prepared["payload_data"]
    image = prepared["image"]
    metadata = prepared["metadata"]

    transport = session.transport
    if logger:
        logger.info("Built SRAM UART loader {} ({} bytes)".format(
            metadata.get("binary"), len(image)))
    session.load_ram_image(LOADER_BASE, image, timeout=5.0)
    session.go(LOADER_BASE, timeout=5.0)
    transport.set_format(LOADER_BAUD, "N", 1)
    loader = UartLoader(transport, logger=logger)
    results: list[dict[str, Any]] = []
    try:
        jedec = loader.handshake()
        if jedec != LOADER_JEDEC:
            raise UartIspError(
                "SRAM UART loader reports JEDEC 0x{:06X}, expected 0x{:06X}".format(
                    jedec, LOADER_JEDEC))
        info = loader.info()
        if (info["status1"] & 0x7D) != 0 or (info["status2"] & 0x40) != 0:
            raise UartIspError(
                "external NOR reports active block protection: SR1=0x{:02X} "
                "SR2=0x{:02X}".format(info["status1"], info["status2"]))
        for payload, data in zip(plan, payload_data):
            if logger:
                logger.info("Programming OSPI partition {} ({} bytes)".format(
                    payload["partition"], len(data)))
            progress = _TransferProgress(
                logger, "Writing OSPI partition {}".format(payload["partition"]),
                len(data))
            erase_sectors = sum(
                (int(span["erase_end_bytes"]) - int(span["erase_start_bytes"]))
                // NOR_SECTOR for span in payload["spans"])
            erase_progress = _TransferProgress(
                logger, "Erasing OSPI partition {}".format(payload["partition"]),
                erase_sectors * NOR_SECTOR, interval=1024 * 1024)
            erased_sectors = 0
            consumed = 0
            programmed_total = 0
            for span in payload["spans"]:
                offset = int(span["device_offset_bytes"])
                count = int(span["program_size_bytes"])
                erase_start = int(span["erase_start_bytes"])
                erase_end = int(span["erase_end_bytes"])
                if erase_start % NOR_SECTOR or erase_end % NOR_SECTOR:
                    raise UartIspError(
                        "OSPI erase span is not 4 KiB aligned")
                for sector in range(erase_start, erase_end, NOR_SECTOR):
                    loader.erase(sector)
                    erased_sectors += 1
                    erase_progress.update(erased_sectors * NOR_SECTOR)
                chunk = data[consumed:consumed + count]
                written = 0
                while written < len(chunk):
                    page_remaining = (int(span["page_bytes"]) -
                                      ((offset + written) % int(span["page_bytes"])))
                    piece = chunk[written:written + min(MAX_TRANSFER, page_remaining)]
                    loader.program(offset + written, piece)
                    written += len(piece)
                    programmed_total += len(piece)
                    progress.update(programmed_total)
                if verify:
                    position = 0
                    while position < len(chunk):
                        piece = chunk[position:position + MAX_TRANSFER]
                        back = loader.read(offset + position, len(piece))
                        if back != piece:
                            raise UartIspError(
                                "OSPI readback mismatch in partition "
                                f"{payload['partition']!r} at 0x{offset + position:06X}")
                        position += len(piece)
                consumed += count
            erase_progress.finish()
            progress.finish()
            if consumed != len(data):
                raise UartIspError(
                    f"external payload {payload['partition']!r} was not fully mapped")
            results.append({"partition": payload["partition"],
                            "size_bytes": len(data), "verified": bool(verify)})
    finally:
        try:
            loader.reset()
        except Exception:
            pass
    return results


def prepare_external_flash(manifest: dict[str, Any],
                           external_payloads: list[dict[str, Any]],
                           ospi_profiles: Mapping[str, Any] | None):
    """Select and fully preflight the ROM or SRAM-loader OSPI path."""
    layout, devices, plan = validate_ospi_payloads(manifest, external_payloads)
    device_items = (list(devices.values()) if isinstance(devices, dict)
                    else list(devices))
    part_numbers = {
        str(device.get("id")): str(device.get("part_number", "")).upper()
        for device in device_items if isinstance(device, dict)
    }
    used = {str(span["device"]) for payload in plan for span in payload["spans"]}
    if any(part_numbers.get(device_id) == "GD25Q64ESIG" for device_id in used):
        if len(used) != 1 or part_numbers.get(next(iter(used))) != "GD25Q64ESIG":
            raise UartIspError(
                "GD25Q64ESIG requires the one-device SRAM UART loader and cannot "
                "be mixed with ROM-controlled NOR devices")
        return "loader", prepare_ospi_loader(manifest, external_payloads)
    _validate_ospi_profiles(layout, plan, ospi_profiles)
    _read_external_payload_data(plan)
    return "rom", None


def _validate_ospi_profiles(layout, plan, ospi_profiles):
    used = {span["device"] for payload in plan for span in payload["spans"]}
    if not isinstance(ospi_profiles, Mapping):
        raise UartIspError("external OSPI payloads require explicit per-device OSPI profiles")
    if set(ospi_profiles) != used:
        raise UartIspError("OSPI profile device keys must exactly match payload NOR devices")
    bindings = layout["board_binding"]
    profiles = {}
    for device_id in sorted(used):
        profile = ospi_profiles[device_id]
        if not isinstance(profile, Mapping):
            raise UartIspError(f"OSPI profile for {device_id!r} must be an object")
        if "config" not in profile or "pins" not in profile:
            raise UartIspError(f"OSPI profile for {device_id!r} needs explicit config and pins")
        config = _hex_bytes(profile["config"], 3, f"{device_id} OSPI configuration")
        binding = bindings[device_id]
        data_pins = binding.get("data")
        if (not isinstance(data_pins, list) or not data_pins
                or not binding.get("chip_select") or not binding.get("clock")):
            raise UartIspError(f"OSPI binding for {device_id!r} has no data signals")
        try:
            records = tuple(_hex_bytes(record, 5, f"{device_id} OSPI pin record")
                            for record in profile["pins"])
        except TypeError as exc:
            raise UartIspError(f"OSPI pins for {device_id!r} must be a record sequence") from exc
        default_mode = records == (b"\x0f\x00\x00\x00\x00",)
        expected_count = 2 + len(data_pins)
        if not default_mode and len(records) != expected_count:
            raise UartIspError(
                f"OSPI pins for {device_id!r} need {expected_count} records from its board binding")
        selectors = [record[0] for record in records]
        if len(set(selectors)) != len(selectors):
            raise UartIspError(f"OSPI pin record selectors repeat for {device_id!r}")
        profiles[device_id] = {"config": config, "pins": records}
    return profiles


def flash(manifest: dict[str, Any], external_payloads: list[dict[str, Any]],
          image_path: str | Path, start: int, limit: int, capacity: int,
          device_geometry: dict[str, Any], port: str | None = None,
          baud: int | str | None = DEFAULT_UART_BAUD,
          expected_id: bytes | None = None, expected_chip: str | None = None,
          ospi_profiles: Mapping[str, Any] | None = None,
          erase_mode: str = "extended",
          verify: bool = True, transport: Any | None = None, logger=None) -> dict[str, Any]:
    """Program external OSPI payloads and the internal CNVM image over UART."""

    if not isinstance(expected_id, bytes) or not expected_id:
        raise UartIspError("UART ROM ISP requires a non-empty expected_id bytes value")
    target = expected_chip or "unspecified chip"
    if (int(start) != CNVM_BASE or int(device_geometry.get("erase_bytes", 0)) != CNVM_PAGE
            or int(capacity) <= 0 or int(limit) != int(start) + int(capacity)
            or int(capacity) % CNVM_PAGE or erase_mode not in ("extended", "base")):
        raise UartIspError("invalid static CNVM address or erase geometry")
    image_data = Path(image_path).read_bytes()
    if not image_data:
        raise UartIspError("internal CNVM image is empty")
    if len(image_data) > int(capacity):
        raise UartIspError("internal CNVM image exceeds application capacity")
    if (len(image_data) + CNVM_PAGE - 1) // CNVM_PAGE > int(capacity) // CNVM_PAGE:
        raise UartIspError("internal CNVM image covers more pages than application capacity")
    page_count = (len(image_data) + CNVM_PAGE - 1) // CNVM_PAGE
    max_pages = 256 if erase_mode == "base" else 0x10000
    if page_count > max_pages:
        raise UartIspError("internal CNVM image exceeds the selected ROM erase index range")
    if len(image_data) + ((-len(image_data)) % 4) > int(capacity):
        raise UartIspError("CNVM final WRITE alignment padding exceeds application capacity")
    external_mode = None
    prepared_loader = None
    if external_payloads:
        external_mode, prepared_loader = prepare_external_flash(
            manifest, external_payloads, ospi_profiles)
    elif ospi_profiles:
        raise UartIspError("OSPI profiles supplied without external payloads")
    auto_baud = baud is None or str(baud).strip().lower() in ("", "0", "auto")
    owns_transport = transport is None
    if transport is None:
        initial_baud = AUTO_UART_BAUDS[0] if auto_baud else int(baud)
        transport = SerialTransport(str(port or ""), initial_baud)
    session = UartIsp(transport, logger=logger)
    try:
        if auto_baud and callable(getattr(transport, "set_baud", None)):
            selected_baud = session.select_baud()
        else:
            selected_baud = int(getattr(transport, "baud", DEFAULT_UART_BAUD))
            session.handshake()
        actual_id = session.get_id()
        if actual_id != expected_id:
            raise UartIspError(
                "UART GETID mismatch: actual={} expected={} target={}".format(
                    actual_id.hex().upper() or "(empty)", expected_id.hex().upper(), target))
        external_result = []
        if external_mode == "rom":
            try:
                external_result = program_ospi(
                    session, manifest, external_payloads,
                    ospi_profiles=ospi_profiles, verify=verify, logger=logger)
            except Exception as exc:
                raise UartIspError(
                    "OSPI UART stage failed before CNVM was written: "
                    f"{exc}") from exc
        try:
            internal_result = program_cnvm(
                session, image_path, start, capacity,
                int(device_geometry.get("erase_bytes", 0)),
                mode=erase_mode, verify=verify, logger=logger,
                image_bytes=image_data)
        except Exception as exc:
            raise UartIspError(
                "CNVM UART stage failed; the internal image "
                f"may be incomplete: {exc}") from exc
        if external_mode == "loader":
            # The SRAM loader takes over the CPU and UART, so it is entered only
            # after the ROM-controlled CNVM stage has completed.
            try:
                external_result = program_ospi_loader(
                    session, verify=verify, logger=logger,
                    prepared=prepared_loader)
            except Exception as exc:
                raise UartIspError(
                    "OSPI SRAM-loader stage failed after CNVM was written; partial "
                    f"OSPI writes may have occurred: {exc}") from exc
    finally:
        if owns_transport:
            session.close()
    return {
        "success": True,
        "verified": bool(verify),
        "external": external_result,
        "internal": internal_result,
        "port": port,
        "baud": int(selected_baud),
    }
