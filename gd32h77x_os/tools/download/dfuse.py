"""DfuSe transport for the GD32H77x ROM USB DFU bootloader.

Why this exists instead of calling ``dfu-util``:

* The GD32H77x ROM advertises ``@InternalFlash /0x08000000/960*004Kg``, i.e.
  960 x 4 KiB pages. The NVMC it actually programs has 64 KiB CNVM sectors
  (``gd32h77x_78x_nvmc.h``: ``SECTOR_SIZE = 0x00010000``). Erase planning must
  follow the silicon, not the descriptor, so this module never derives erase
  geometry from the device: callers pass the addresses and sizes resolved from
  the platform memory registry.
* ``dfu-util`` 0.11 has no command line switch to override the device-reported
  DfuSe layout, which is why earlier revisions of this platform shipped a
  board-specific patched binary. Backend geometry now comes from the registry
  instead, so nothing device specific is built or pinned.

Only the USB transfer size is read from the DFU functional descriptor; it is a
transport limit, not an erase/flash geometry.

The protocol layer (``DfuSeProtocol``) is transport agnostic so it can be unit
tested without hardware. ``libusb`` is accessed through ctypes; no pyusb, no
external ``dfu-util`` binary.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import struct
import time
from typing import Callable, List, Optional, Tuple

# --- DFU 1.1 requests -------------------------------------------------------
DFU_DETACH = 0x00
DFU_DNLOAD = 0x01
DFU_UPLOAD = 0x02
DFU_GETSTATUS = 0x03
DFU_CLRSTATUS = 0x04
DFU_GETSTATE = 0x05
DFU_ABORT = 0x06

# --- DFU 1.1 states ---------------------------------------------------------
STATE_APP_IDLE = 0x00
STATE_APP_DETACH = 0x01
STATE_DFU_IDLE = 0x02
STATE_DFU_DOWNLOAD_SYNC = 0x03
STATE_DFU_DOWNLOAD_BUSY = 0x04
STATE_DFU_DOWNLOAD_IDLE = 0x05
STATE_DFU_MANIFEST_SYNC = 0x06
STATE_DFU_MANIFEST = 0x07
STATE_DFU_MANIFEST_WAIT_RESET = 0x08
STATE_DFU_UPLOAD_IDLE = 0x09
STATE_DFU_ERROR = 0x0A

STATE_NAMES = {
    STATE_APP_IDLE: "app-idle",
    STATE_APP_DETACH: "app-detach",
    STATE_DFU_IDLE: "dfu-idle",
    STATE_DFU_DOWNLOAD_SYNC: "download-sync",
    STATE_DFU_DOWNLOAD_BUSY: "download-busy",
    STATE_DFU_DOWNLOAD_IDLE: "download-idle",
    STATE_DFU_MANIFEST_SYNC: "manifest-sync",
    STATE_DFU_MANIFEST: "manifest",
    STATE_DFU_MANIFEST_WAIT_RESET: "manifest-wait-reset",
    STATE_DFU_UPLOAD_IDLE: "upload-idle",
    STATE_DFU_ERROR: "error",
}

# --- DfuSe special commands (sent through DFU_DNLOAD with wValue 0) ---------
DFUSE_CMD_SET_ADDRESS = 0x21
DFUSE_CMD_ERASE = 0x41

# Transaction number used for data transfers (0 is reserved for specials).
DFUSE_DATA_TRANSACTION = 2
DFUSE_MAX_BLOCK_NUMBER = 0xFFFF
DFUSE_MAX_DATA_BLOCKS = DFUSE_MAX_BLOCK_NUMBER - DFUSE_DATA_TRANSACTION + 1

DFU_DESCRIPTOR_TYPE = 0x21
USB_DESCRIPTOR_CONFIGURATION = 0x02
DFU_INTERFACE_CLASS = 0xFE
DFU_INTERFACE_SUBCLASS = 0x01
DFU_INTERFACE_PROTOCOL_MODE = 0x02

DEFAULT_TIMEOUT_MS = 5000
DEFAULT_TRANSFER_SIZE = 2048


class DfuError(RuntimeError):
    """DfuSe transport or protocol failure."""


class DfuTransferError(DfuError):
    """USB I/O error; only retried during application handover."""


class DfuStalled(DfuTransferError):
    """USB endpoint stalled, including during ROM application handover."""


class DfuDisconnected(DfuError):
    """The USB device disappeared while completing a DFU operation."""


# ---------------------------------------------------------------------------
# Pure helpers (unit tested without hardware)
# ---------------------------------------------------------------------------

def erase_plan(start: int, limit: int, erase_bytes: int) -> List[int]:
    """Return the base addresses of the registry erase units in [start, limit)."""
    if erase_bytes <= 0:
        raise DfuError("erase unit must be positive")
    if start % erase_bytes or limit % erase_bytes or limit <= start:
        raise DfuError(
            "flash range 0x{:08X}-0x{:08X} is not aligned to the {} byte erase unit".format(
                start, limit, erase_bytes))
    return list(range(start, limit, erase_bytes))


def parse_dfu_functional_descriptor(data: bytes) -> Optional[dict]:
    """Parse the first DFU functional descriptor found in a raw descriptor blob."""
    offset = 0
    while offset + 2 <= len(data):
        length = data[offset]
        if length < 2:
            break
        if offset + length > len(data):
            break
        if data[offset + 1] == DFU_DESCRIPTOR_TYPE and length >= 9:
            attributes, detach_timeout, transfer_size, version = struct.unpack_from(
                "<BHHH", data, offset + 2)
            return {
                "bmAttributes": attributes,
                "wDetachTimeOut": detach_timeout,
                "wTransferSize": transfer_size,
                "bcdDFUVersion": version,
            }
        offset += length
    return None


def parse_configuration_interfaces(data: bytes) -> List[dict]:
    """Return interface descriptors with their trailing extra descriptors."""
    if len(data) < 9 or data[1] != USB_DESCRIPTOR_CONFIGURATION:
        raise DfuError("device returned an invalid USB configuration descriptor")
    total = struct.unpack_from("<H", data, 2)[0]
    if total > len(data):
        raise DfuError("truncated USB configuration descriptor")
    interfaces: List[dict] = []
    offset = data[0]
    while offset + 2 <= total:
        length = data[offset]
        if length < 2 or offset + length > total:
            break
        if data[offset + 1] == 0x04 and length >= 9:
            interfaces.append({
                "bInterfaceNumber": data[offset + 2],
                "bAlternateSetting": data[offset + 3],
                "bInterfaceClass": data[offset + 5],
                "bInterfaceSubClass": data[offset + 6],
                "bInterfaceProtocol": data[offset + 7],
                "iInterface": data[offset + 8],
                "extra": data[offset + length:_extra_end(data, offset, length, total)],
            })
        offset += length
    return interfaces


def _extra_end(data: bytes, offset: int, length: int, total: int) -> int:
    """End offset of the class-specific descriptors that follow an interface."""
    cursor = offset + length
    end = cursor
    while cursor + 2 <= total:
        size = data[cursor]
        if size < 2 or cursor + size > total:
            break
        # Standard descriptor types restart a new top-level descriptor.
        if data[cursor + 1] in (0x01, 0x02, 0x03, 0x04, 0x05, 0x21):
            if data[cursor + 1] != DFU_DESCRIPTOR_TYPE:
                break
        end = cursor + size
        cursor += size
    return end


# ---------------------------------------------------------------------------
# DfuSe protocol state machine
# ---------------------------------------------------------------------------

class DfuSeProtocol:
    """DfuSe operations on an abstract USB transport.

    ``transport`` must provide ``control_out(bm, req, value, index, data, timeout)``
    and ``control_in(bm, req, value, index, length, timeout)``.
    """

    def __init__(self, transport, interface: int = 0,
                 transfer_size: int = DEFAULT_TRANSFER_SIZE,
                 timeout_ms: int = DEFAULT_TIMEOUT_MS, log: Optional[Callable[[str], None]] = None):
        if transfer_size <= 0 or transfer_size > 0xFFFF:
            raise DfuError("DFU transfer size must be in the range 1..65535")
        self._transport = transport
        self.interface = interface
        self.transfer_size = transfer_size
        self.timeout_ms = timeout_ms
        self._log = log or (lambda message: None)

    # -- status -------------------------------------------------------------
    def get_status(self) -> Tuple[int, int, int]:
        raw = self._transport.control_in(0xA1, DFU_GETSTATUS, 0, self.interface, 6,
                                         self.timeout_ms)
        if len(raw) < 6:
            raise DfuError("short DFU GETSTATUS response")
        status = raw[0]
        poll_ms = raw[1] | (raw[2] << 8) | (raw[3] << 16)
        state = raw[4]
        return status, poll_ms, state

    def clear_status(self) -> None:
        self._control_out(DFU_CLRSTATUS, 0, b"")

    def abort(self) -> None:
        self._control_out(DFU_ABORT, 0, b"")

    def _control_out(self, request: int, value: int, data: bytes) -> None:
        transferred = self._transport.control_out(0x21, request, value, self.interface,
                                                 data, self.timeout_ms)
        if transferred != len(data):
            raise DfuError("short USB control transfer: sent {} of {} bytes".format(
                transferred, len(data)))

    def _wait_poll(self, poll_ms: int, deadline: float, action: str) -> None:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise DfuError("{}: timed out waiting for DFU".format(action))
        delay = max(poll_ms, 1) / 1000.0
        time.sleep(min(delay, remaining))

    def _wait_until_idle(self, action: str, timeout_seconds: float = 30.0) -> int:
        deadline = time.monotonic() + timeout_seconds
        while True:
            status, poll_ms, state = self.get_status()
            if status != 0:
                if state == STATE_DFU_ERROR:
                    raise DfuError("{}: DFU status {} (error state)".format(action, status))
                raise DfuError("{}: DFU status {}".format(action, status))
            if state == STATE_DFU_IDLE:
                return state
            if state in (STATE_DFU_DOWNLOAD_SYNC, STATE_DFU_DOWNLOAD_BUSY,
                         STATE_DFU_MANIFEST_SYNC, STATE_DFU_MANIFEST):
                self._wait_poll(poll_ms, deadline, action)
                continue
            if state in (STATE_DFU_DOWNLOAD_IDLE, STATE_DFU_UPLOAD_IDLE):
                self.abort()
                self._wait_poll(1, deadline, action)
                continue
            raise DfuError("{}: cannot reach dfuIDLE from state {}".format(
                action, STATE_NAMES.get(state, state)))

    def settle(self, retries: int = 4) -> None:
        """Bring the interface to DFU_IDLE, clearing stale transfer state."""
        del retries  # Retained for API compatibility; the timeout bounds recovery.
        deadline = time.monotonic() + max(self.timeout_ms / 1000.0, 30.0)
        while True:
            status, poll_ms, state = self.get_status()
            if state == STATE_DFU_ERROR or status != 0:
                self.clear_status()
                self._wait_poll(1, deadline, "clearing DFU error")
                continue
            if state == STATE_DFU_IDLE:
                return
            if state in (STATE_DFU_DOWNLOAD_SYNC, STATE_DFU_DOWNLOAD_BUSY,
                         STATE_DFU_MANIFEST_SYNC, STATE_DFU_MANIFEST):
                self._wait_poll(poll_ms, deadline, "settling DFU interface")
                continue
            if state in (STATE_DFU_DOWNLOAD_IDLE, STATE_DFU_UPLOAD_IDLE):
                self.abort()
                self._wait_poll(1, deadline, "aborting stale DFU transfer")
                continue
            raise DfuError("DFU interface did not reach idle (state {})".format(
                STATE_NAMES.get(state, state)))

    def _await_download(self, action: str) -> None:
        deadline = time.monotonic() + max(self.timeout_ms / 1000.0, 30.0)
        while True:
            status, poll_ms, state = self.get_status()
            if status != 0:
                raise DfuError("{}: DFU status {}".format(action, status))
            if state == STATE_DFU_DOWNLOAD_IDLE:
                return
            if state in (STATE_DFU_DOWNLOAD_SYNC, STATE_DFU_DOWNLOAD_BUSY):
                self._wait_poll(poll_ms, deadline, action)
                continue
            raise DfuError("{}: unexpected DFU state {} after command".format(
                action, STATE_NAMES.get(state, state)))

    # -- special commands ---------------------------------------------------
    def set_address(self, address: int) -> None:
        if not 0 <= address <= 0xFFFFFFFF:
            raise DfuError("address is outside the 32-bit DFU address space")
        self._prepare_download()
        self._control_out(DFU_DNLOAD, 0, struct.pack("<BI", DFUSE_CMD_SET_ADDRESS, address))
        self._await_download("set address 0x{:08X}".format(address))

    def _prepare_download(self) -> None:
        status, _, state = self.get_status()
        if state == STATE_DFU_ERROR or status != 0:
            if state != STATE_DFU_ERROR:
                raise DfuError("DFU status {} before download".format(status))
            self.clear_status()
            self._wait_until_idle("recovering DFU error")
        elif state in (STATE_DFU_UPLOAD_IDLE,):
            self.abort()
            self._wait_until_idle("leaving upload state")
        elif state in (STATE_DFU_DOWNLOAD_SYNC, STATE_DFU_DOWNLOAD_BUSY):
            self._wait_until_idle("finishing pending download")
        elif state not in (STATE_DFU_IDLE, STATE_DFU_DOWNLOAD_IDLE):
            raise DfuError("cannot issue download command in state {}".format(
                STATE_NAMES.get(state, state)))

    def erase(self, address: int) -> None:
        if not 0 <= address <= 0xFFFFFFFF:
            raise DfuError("erase address is outside the 32-bit DFU address space")
        self._prepare_download()
        self._control_out(DFU_DNLOAD, 0, struct.pack("<BI", DFUSE_CMD_ERASE, address))
        self._await_download("erase 0x{:08X}".format(address))

    # -- data ---------------------------------------------------------------
    def write(self, address: int, data: bytes, progress=None) -> None:
        if not data:
            return
        if address < 0 or address + len(data) > 0x100000000:
            raise DfuError("write range is outside the 32-bit DFU address space")
        written = 0
        while written < len(data):
            segment_bytes = min(len(data) - written,
                                self.transfer_size * DFUSE_MAX_DATA_BLOCKS)
            self.set_address(address + written)
            segment_written = 0
            block = DFUSE_DATA_TRANSACTION
            while segment_written < segment_bytes:
                chunk = data[written + segment_written:
                             written + min(segment_written + self.transfer_size, segment_bytes)]
                chunk_address = address + written + segment_written
                # A short final block is the full-transfer boundary for the
                # following block. Reset the DfuSe pointer to its exact address
                # and restart at block 2 so implementations that use the short
                # buffer length in their block formula cannot shift the tail.
                if len(chunk) < self.transfer_size:
                    self.set_address(chunk_address)
                    block = DFUSE_DATA_TRANSACTION
                self._control_out(DFU_DNLOAD, block, chunk)
                self._await_download("write 0x{:08X}".format(
                    chunk_address))
                segment_written += len(chunk)
                block += 1
                if progress:
                    progress(written + segment_written)
            written += segment_written

    def read(self, address: int, size: int, progress=None) -> bytes:
        if size <= 0:
            return b""
        if address < 0 or address + size > 0x100000000:
            raise DfuError("read range is outside the 32-bit DFU address space")
        result = bytearray()
        while len(result) < size:
            segment_bytes = min(size - len(result),
                                self.transfer_size * DFUSE_MAX_DATA_BLOCKS)
            self.set_address(address + len(result))
            self.abort()
            self._wait_until_idle("switching to upload mode")
            segment_read = 0
            block = DFUSE_DATA_TRANSACTION
            while segment_read < segment_bytes:
                request = min(self.transfer_size, segment_bytes - segment_read)
                chunk_address = address + len(result)
                # Some DfuSe ROMs calculate UPLOAD offsets using this request's
                # wLength. Rebase a short final request and restart at block 2.
                if request < self.transfer_size:
                    self.set_address(chunk_address)
                    self.abort()
                    self._wait_until_idle("switching to final upload block")
                    block = DFUSE_DATA_TRANSACTION
                chunk = self._transport.control_in(0xA1, DFU_UPLOAD, block, self.interface,
                                                   request, self.timeout_ms)
                if len(chunk) != request:
                    raise DfuError("short DFU upload at 0x{:08X}: received {} of {} bytes".format(
                        chunk_address, len(chunk), request))
                result.extend(chunk)
                segment_read += len(chunk)
                block += 1
                if progress:
                    progress(len(result))
        return bytes(result)

    def _leave_status(self):
        # The H779 ROM can stall GETSTATUS briefly before disconnecting.
        # A persistent stall remains an error; do not report a failed jump as success.
        deadline = time.monotonic() + 2.0
        while True:
            try:
                return self.get_status()
            except DfuTransferError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.05)

    def leave(self) -> None:
        """Issue the DfuSe leave request so the ROM starts the new image."""
        self.set_address(0x08000000)
        self._control_out(DFU_DNLOAD, 0, b"")
        try:
            status, poll_ms, state = self._leave_status()
        except DfuDisconnected:
            return
        if status != 0:
            raise DfuError("leave DFU mode: status {}".format(status))
        deadline = time.monotonic() + max(self.timeout_ms / 1000.0, 30.0)
        while state in (STATE_DFU_MANIFEST_SYNC, STATE_DFU_MANIFEST):
            self._wait_poll(poll_ms, deadline, "leaving DFU mode")
            try:
                status, poll_ms, state = self._leave_status()
            except DfuDisconnected:
                return
            if status != 0:
                raise DfuError("leave DFU mode: status {}".format(status))
        if state != STATE_DFU_MANIFEST_WAIT_RESET:
            raise DfuError("leave DFU mode: unexpected DFU state {}".format(
                STATE_NAMES.get(state, state)))


# ---------------------------------------------------------------------------
# libusb transport (ctypes, no pyusb)
# ---------------------------------------------------------------------------

class _UsbDeviceDescriptor(ctypes.Structure):
    _fields_ = [
        ("bLength", ctypes.c_uint8),
        ("bDescriptorType", ctypes.c_uint8),
        ("bcdUSB", ctypes.c_uint16),
        ("bDeviceClass", ctypes.c_uint8),
        ("bDeviceSubClass", ctypes.c_uint8),
        ("bDeviceProtocol", ctypes.c_uint8),
        ("bMaxPacketSize0", ctypes.c_uint8),
        ("idVendor", ctypes.c_uint16),
        ("idProduct", ctypes.c_uint16),
        ("bcdDevice", ctypes.c_uint16),
        ("iManufacturer", ctypes.c_uint8),
        ("iProduct", ctypes.c_uint8),
        ("iSerialNumber", ctypes.c_uint8),
        ("bNumConfigurations", ctypes.c_uint8),
    ]


class Libusb:
    """Minimal ctypes binding for libusb-1.0."""

    def __init__(self):
        library = ctypes.util.find_library("usb-1.0") or "libusb-1.0.so.0"
        try:
            self._lib = ctypes.CDLL(library)
        except OSError as exc:  # pragma: no cover - depends on host libraries
            raise DfuError(
                "libusb-1.0 is required for the GD32H77X DFU backend: {}".format(exc))
        self._declare()
        self._ctx = ctypes.c_void_p()
        if self._lib.libusb_init(ctypes.byref(self._ctx)) != 0:
            raise DfuError(
                "libusb_init failed; check that /dev/bus/usb exists and is accessible "
                "(udev rules or root are usually needed)")
        self._closed = False

    def _declare(self):
        lib = self._lib
        lib.libusb_init.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
        lib.libusb_init.restype = ctypes.c_int
        lib.libusb_exit.argtypes = [ctypes.c_void_p]
        lib.libusb_get_device_list.argtypes = [ctypes.c_void_p,
                                               ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))]
        lib.libusb_get_device_list.restype = ctypes.c_ssize_t
        lib.libusb_free_device_list.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_int]
        lib.libusb_get_device_descriptor.argtypes = [ctypes.c_void_p,
                                                     ctypes.POINTER(_UsbDeviceDescriptor)]
        lib.libusb_get_device_descriptor.restype = ctypes.c_int
        lib.libusb_open.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
        lib.libusb_open.restype = ctypes.c_int
        lib.libusb_close.argtypes = [ctypes.c_void_p]
        lib.libusb_get_bus_number.argtypes = [ctypes.c_void_p]
        lib.libusb_get_bus_number.restype = ctypes.c_uint8
        lib.libusb_get_port_numbers.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint8),
                                                ctypes.c_int]
        lib.libusb_get_port_numbers.restype = ctypes.c_int
        lib.libusb_claim_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.libusb_claim_interface.restype = ctypes.c_int
        lib.libusb_set_configuration.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.libusb_set_configuration.restype = ctypes.c_int
        lib.libusb_release_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.libusb_set_interface_alt_setting.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        lib.libusb_set_interface_alt_setting.restype = ctypes.c_int
        lib.libusb_control_transfer.argtypes = [
            ctypes.c_void_p, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint16, ctypes.c_uint16,
            ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint16, ctypes.c_uint]
        lib.libusb_control_transfer.restype = ctypes.c_int
        lib.libusb_get_string_descriptor_ascii.argtypes = [
            ctypes.c_void_p, ctypes.c_uint8, ctypes.POINTER(ctypes.c_uint8), ctypes.c_int]
        lib.libusb_get_string_descriptor_ascii.restype = ctypes.c_int
        lib.libusb_strerror.argtypes = [ctypes.c_int]
        lib.libusb_strerror.restype = ctypes.c_char_p

    def error(self, code: int) -> str:
        message = self._lib.libusb_strerror(code)
        return message.decode("utf-8", "replace") if message else "libusb error {}".format(code)

    def devices(self, vendor: int, product: int) -> List["UsbDevice"]:
        pointer = ctypes.POINTER(ctypes.c_void_p)()
        count = self._lib.libusb_get_device_list(self._ctx, ctypes.byref(pointer))
        if count < 0:
            raise DfuError(self.error(count))
        found: List[UsbDevice] = []
        try:
            for index in range(count):
                device = pointer[index]
                descriptor = _UsbDeviceDescriptor()
                if self._lib.libusb_get_device_descriptor(device, ctypes.byref(descriptor)) != 0:
                    continue
                if descriptor.idVendor != vendor or descriptor.idProduct != product:
                    continue
                found.append(self._open(device, descriptor))
        finally:
            self._lib.libusb_free_device_list(pointer, 1)
        return found

    def _open(self, device, descriptor: _UsbDeviceDescriptor) -> "UsbDevice":
        handle = ctypes.c_void_p()
        result = self._lib.libusb_open(device, ctypes.byref(handle))
        if result != 0:
            raise DfuError("cannot open DFU device: {}".format(self.error(result)))
        ports = (ctypes.c_uint8 * 8)()
        depth = self._lib.libusb_get_port_numbers(device, ports, 8)
        bus = self._lib.libusb_get_bus_number(device)
        path = str(bus)
        if depth > 0:
            path += "-" + ".".join(str(ports[i]) for i in range(depth))
        serial = self._serial(handle, descriptor.iSerialNumber)
        return UsbDevice(self, handle, path, serial, descriptor.bNumConfigurations)

    def _serial(self, handle, index: int) -> str:
        if not index:
            return ""
        buffer = (ctypes.c_uint8 * 256)()
        length = self._lib.libusb_get_string_descriptor_ascii(handle, index, buffer, 256)
        if length <= 0:
            return ""
        return bytes(bytearray(buffer[:length])).decode("utf-8", "replace")

    def close(self):
        if not self._closed:
            self._closed = True
            self._lib.libusb_exit(self._ctx)


class UsbDevice:
    """An opened USB device providing the DFU control-transfer transport."""

    def __init__(self, lib: Libusb, handle, path: str, serial: str, configurations: int):
        self._lib = lib
        self._handle = handle
        self.path = path
        self.serial = serial
        self._configurations = configurations
        self._claimed: List[int] = []
        self._closed = False
        self._configuration_value = 0

    def interfaces(self) -> List[dict]:
        """Return DFU interfaces (number, alt setting, transfer size)."""
        raw = self._raw_configuration_descriptor()
        self._configuration_value = raw[5] if len(raw) > 5 else 0
        interfaces = []
        for entry in parse_configuration_interfaces(raw):
            if (entry["bInterfaceClass"] != DFU_INTERFACE_CLASS
                    or entry["bInterfaceSubClass"] != DFU_INTERFACE_SUBCLASS
                    or entry["bInterfaceProtocol"] != DFU_INTERFACE_PROTOCOL_MODE):
                continue
            functional = parse_dfu_functional_descriptor(entry["extra"])
            if not functional or not functional.get("wTransferSize"):
                raise DfuError("DFU-mode interface {} alt {} has no valid functional descriptor".format(
                    entry["bInterfaceNumber"], entry["bAlternateSetting"]))
            string_index = entry["iInterface"]
            interfaces.append({
                "interface": entry["bInterfaceNumber"],
                "alt": entry["bAlternateSetting"],
                "protocol": entry["bInterfaceProtocol"],
                "string_index": string_index,
                "name": self._lib._serial(self._handle, string_index) if string_index else "",
                "transfer_size": functional["wTransferSize"],
                "bcd_dfu_version": functional["bcdDFUVersion"],
                "bm_attributes": functional["bmAttributes"],
            })
        return interfaces

    def _raw_configuration_descriptor(self) -> bytes:
        header = self._control_in(0x80, 0x06, USB_DESCRIPTOR_CONFIGURATION << 8, 0, 9,
                                  DEFAULT_TIMEOUT_MS)
        if len(header) < 9:
            raise DfuError("cannot read USB configuration descriptor")
        total = struct.unpack_from("<H", header, 2)[0]
        if total <= 9:
            return header
        return self._control_in(0x80, 0x06, USB_DESCRIPTOR_CONFIGURATION << 8, 0, total,
                                DEFAULT_TIMEOUT_MS)

    def claim(self, interface: int, alt: int = 0) -> None:
        if self._configuration_value:
            result = self._lib._lib.libusb_set_configuration(
                self._handle, self._configuration_value)
            if result != 0:
                raise DfuError("cannot select USB configuration {}: {}".format(
                    self._configuration_value, self._lib.error(result)))
        result = self._lib._lib.libusb_claim_interface(self._handle, interface)
        if result != 0:
            raise DfuError("cannot claim DFU interface {}: {}".format(
                interface, self._lib.error(result)))
        self._claimed.append(interface)
        result = self._lib._lib.libusb_set_interface_alt_setting(self._handle, interface, alt)
        if result != 0:
            raise DfuError("cannot select DFU alternate setting {}: {}".format(
                alt, self._lib.error(result)))

    def control_out(self, bm: int, request: int, value: int, index: int, data: bytes,
                    timeout_ms: int) -> int:
        buffer = (ctypes.c_uint8 * max(len(data), 1))(*data) if data else None
        result = self._lib._lib.libusb_control_transfer(
            self._handle, bm, request, value, index, buffer, len(data), timeout_ms)
        if result == -4:  # LIBUSB_ERROR_NO_DEVICE; useful when DFU leave re-enumerates.
            raise DfuDisconnected("USB device disconnected")
        if result < 0:
            raise DfuError("USB control out failed: {}".format(self._lib.error(result)))
        if result != len(data):
            raise DfuError("short USB control out: transferred {} of {} bytes".format(
                result, len(data)))
        return result

    def control_in(self, bm: int, request: int, value: int, index: int, length: int,
                   timeout_ms: int) -> bytes:
        return self._control_in(bm, request, value, index, length, timeout_ms)

    def _control_in(self, bm: int, request: int, value: int, index: int, length: int,
                    timeout_ms: int) -> bytes:
        buffer = (ctypes.c_uint8 * max(length, 1))()
        result = self._lib._lib.libusb_control_transfer(
            self._handle, bm, request, value, index, buffer, length, timeout_ms)
        if result == -4:
            raise DfuDisconnected("USB device disconnected")
        if result == -9:
            raise DfuStalled("USB control in failed: endpoint stalled")
        if result == -1:
            raise DfuTransferError("USB control in failed: Input/Output Error")
        if result < 0:
            raise DfuError("USB control in failed: {}".format(self._lib.error(result)))
        return bytes(bytearray(buffer[:result]))

    def close(self):
        if self._closed:
            return
        self._closed = True
        try:
            for interface in self._claimed:
                self._lib._lib.libusb_release_interface(self._handle, interface)
        finally:
            self._lib._lib.libusb_close(self._handle)


def open_dfu_devices(vendor: int, product: int) -> Tuple[Libusb, List[UsbDevice]]:
    """Open every USB device matching ``vendor``:``product``.

    The caller owns the returned handle and must call ``lib.close()`` and close
    every device once it has selected one.
    """
    lib = Libusb()
    try:
        devices = lib.devices(vendor, product)
    except Exception:
        lib.close()
        raise
    return lib, devices
