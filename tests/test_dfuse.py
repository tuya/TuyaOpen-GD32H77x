# Copyright (c) 2026 Tuya Inc. All rights reserved.
"""Host tests for DfuSe using a protocol- and flash-aware ROM model."""

import importlib.util
import ctypes
import struct
import types
from pathlib import Path
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parents[1] / "gd32h77x_os/tools/download/dfuse.py"
SPEC = importlib.util.spec_from_file_location("gd32h77x_dfuse_test", MODULE_PATH)
dfuse = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(dfuse)


def _interface(number, alt, protocol, string_index=0, interface_class=0xFE,
               subclass=0x01, transfer_size=1024, attributes=0x07, version=0x011A):
    functional = struct.pack("<BBBHHH", 9, 0x21, attributes, 0xFF,
                             transfer_size, version)
    descriptor = struct.pack("<BBBBBBBBB", 9, 0x04, number, alt, 0,
                             interface_class, subclass, protocol, string_index)
    return descriptor + functional


def _config_descriptor(interfaces):
    body = b"".join(interfaces)
    return struct.pack("<BBHBBBBB", 9, 0x02, 9 + len(body), len(interfaces), 1,
                       0, 0x80, 50) + body


class DfuSeFlashModel:
    """Small ROM model implementing AN3156's address/block semantics."""

    def __init__(self, base=0x08000000, capacity=0x40000, transfer_size=1024):
        self.base = base
        self.capacity = capacity
        self.transfer_size = transfer_size
        self.memory = bytearray(b"\xFF" * capacity)
        self.pointer = base
        self.state = dfuse.STATE_DFU_IDLE
        self.status = 0
        self.poll_ms = 7
        self.pending = None
        self.pending_stage = 0
        self.calls = []
        self.leave_requested = False
        self.fail_short_out = False
        self.bad_upload = False
        self.bad_next_status = False
        self.upload_uses_request_length = False

    def _flash_offset(self, address, length=0):
        if address < self.base or address + length > self.base + self.capacity:
            self.status = 0x08  # err-address
            self.state = dfuse.STATE_DFU_ERROR
            raise AssertionError("ROM rejected out-of-range flash access")
        return address - self.base

    def _execute_pending(self):
        kind, value, data = self.pending
        self.pending = None
        if kind == "command":
            command, address = struct.unpack("<BI", data)
            if command == dfuse.DFUSE_CMD_SET_ADDRESS:
                self._flash_offset(address)
                self.pointer = address
            elif command == dfuse.DFUSE_CMD_ERASE:
                if address % 0x10000:
                    self.status = 0x08
                    self.state = dfuse.STATE_DFU_ERROR
                    return
                offset = self._flash_offset(address, 0x10000)
                self.memory[offset:offset + 0x10000] = b"\xFF" * 0x10000
            else:
                self.status = 0x0F
                self.state = dfuse.STATE_DFU_ERROR
                return
        elif kind == "data":
            address = self.pointer + (value - dfuse.DFUSE_DATA_TRANSACTION) * self.transfer_size
            offset = self._flash_offset(address, len(data))
            self.memory[offset:offset + len(data)] = data
        elif kind == "leave":
            self.leave_requested = True
            self.state = dfuse.STATE_DFU_MANIFEST_SYNC
            return
        self.state = dfuse.STATE_DFU_DOWNLOAD_BUSY
        self.pending_stage = 1

    def control_out(self, bm, request, value, index, data, timeout):
        data = bytes(data)
        self.calls.append(("out", bm, request, value, index, data))
        if self.fail_short_out:
            self.fail_short_out = False
            return max(0, len(data) - 1)
        if request == dfuse.DFU_CLRSTATUS:
            self.status = 0
            self.state = dfuse.STATE_DFU_IDLE
        elif request == dfuse.DFU_ABORT:
            self.pending = None
            self.state = dfuse.STATE_DFU_IDLE
        elif request == dfuse.DFU_DNLOAD:
            if value == 0 and not data:
                self.pending = ("leave", value, data)
                self.state = dfuse.STATE_DFU_MANIFEST_SYNC
            elif value == 0:
                self.pending = ("command", value, data)
                self.state = dfuse.STATE_DFU_DOWNLOAD_SYNC
                self.pending_stage = 0
            else:
                if self.state not in (dfuse.STATE_DFU_IDLE,
                                      dfuse.STATE_DFU_DOWNLOAD_IDLE):
                    self.status = 0x0F
                    self.state = dfuse.STATE_DFU_ERROR
                    raise AssertionError("ROM received DNLOAD in invalid state")
                self.pending = ("data", value, data)
                self.state = dfuse.STATE_DFU_DOWNLOAD_SYNC
                self.pending_stage = 0
        return len(data)

    def control_in(self, bm, request, value, index, length, timeout):
        self.calls.append(("in", bm, request, value, index, length))
        if request == dfuse.DFU_GETSTATUS:
            if self.bad_next_status and self.pending is not None:
                self.bad_next_status = False
                self.pending = None
                return bytes([1, 0, 0, 0, dfuse.STATE_DFU_ERROR, 0])
            if self.pending is not None:
                self._execute_pending()
            elif self.state == dfuse.STATE_DFU_DOWNLOAD_BUSY:
                self.state = dfuse.STATE_DFU_DOWNLOAD_IDLE
            elif self.state == dfuse.STATE_DFU_MANIFEST_SYNC:
                self.state = dfuse.STATE_DFU_MANIFEST_WAIT_RESET
            return bytes([self.status, self.poll_ms & 0xFF,
                          (self.poll_ms >> 8) & 0xFF, (self.poll_ms >> 16) & 0xFF,
                          self.state, 0])
        if request == dfuse.DFU_UPLOAD:
            if self.state not in (dfuse.STATE_DFU_IDLE, dfuse.STATE_DFU_UPLOAD_IDLE):
                self.status = 0x0F
                self.state = dfuse.STATE_DFU_ERROR
                raise AssertionError("ROM received UPLOAD before returning to dfuIDLE")
            block_bytes = length if self.upload_uses_request_length else self.transfer_size
            address = self.pointer + (value - dfuse.DFUSE_DATA_TRANSACTION) * block_bytes
            offset = self._flash_offset(address, length)
            self.state = dfuse.STATE_DFU_UPLOAD_IDLE
            if self.bad_upload:
                self.bad_upload = False
                return bytes(self.memory[offset:offset + length - 1])
            return bytes(self.memory[offset:offset + length])
        raise AssertionError("unexpected control-in request {}".format(request))


class TestLibraryDiscovery(unittest.TestCase):
    def test_windows_uses_bundled_absolute_path(self):
        package = types.SimpleNamespace(get_library_path=lambda: Path("C:/SDK/libusb-1.0.dll"))
        with mock.patch.object(dfuse.sys, "platform", "win32"), mock.patch.dict(
                "sys.modules", {"libusb_package": package}):
            self.assertEqual(dfuse._libusb_library(), str(Path("C:/SDK/libusb-1.0.dll")))

    def test_windows_missing_package_is_actionable(self):
        with mock.patch.object(dfuse.sys, "platform", "win32"), mock.patch.dict(
                "sys.modules", {"libusb_package": None}):
            with self.assertRaisesRegex(dfuse.DfuError, "SDK Python"):
                dfuse._libusb_library()

    def test_windows_empty_package_is_rejected(self):
        package = types.SimpleNamespace(get_library_path=lambda: None)
        with mock.patch.object(dfuse.sys, "platform", "win32"), mock.patch.dict(
                "sys.modules", {"libusb_package": package}):
            with self.assertRaisesRegex(dfuse.DfuError, "binary wheel"):
                dfuse._libusb_library()

    def test_linux_preserves_system_library_discovery(self):
        with mock.patch.object(dfuse.sys, "platform", "linux"), mock.patch.object(
                dfuse.ctypes.util, "find_library", return_value="libusb-1.0.so.0"):
            self.assertEqual(dfuse._libusb_library(), "libusb-1.0.so.0")


class TestWindowsDisconnect(unittest.TestCase):
    def test_io_error_only_becomes_disconnect_when_device_is_gone(self):
        for present, expected in ((True, dfuse.DfuTransferError), (False, dfuse.DfuDisconnected)):
            with self.subTest(present=present), mock.patch.object(dfuse.sys, "platform", "win32"):
                library = mock.Mock()
                library._lib.libusb_control_transfer.return_value = -1
                library.device_present.return_value = present
                device = dfuse.UsbDevice(library, ctypes.c_void_p(1), "1-3.2", "", 1)
                with self.assertRaises(expected):
                    device.control_in(0xA1, dfuse.DFU_GETSTATUS, 0, 0, 6, 1000)

    def test_linux_io_error_remains_transfer_error(self):
        library = mock.Mock()
        library._lib.libusb_control_transfer.return_value = -1
        device = dfuse.UsbDevice(library, ctypes.c_void_p(1), "1-3.2", "", 1)
        with mock.patch.object(dfuse.sys, "platform", "linux"):
            with self.assertRaises(dfuse.DfuTransferError):
                device.control_in(0xA1, dfuse.DFU_GETSTATUS, 0, 0, 6, 1000)
        library.device_present.assert_not_called()

    def test_presence_uses_device_identity_and_fails_conservatively(self):
        devices = (ctypes.c_void_p * 2)(11, 22)
        for target, count, expected in ((22, 2, True), (33, 2, False), (22, -1, True)):
            with self.subTest(target=target, count=count):
                def get_list(context, output):
                    address = ctypes.cast(output, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p)))
                    address[0] = ctypes.cast(devices, ctypes.POINTER(ctypes.c_void_p))
                    return count
                library = object.__new__(dfuse.Libusb)
                library._ctx = ctypes.c_void_p()
                library._lib = mock.Mock()
                library._lib.libusb_get_device.return_value = target
                library._lib.libusb_get_device_list.side_effect = get_list
                self.assertEqual(library.device_present(ctypes.c_void_p(1)), expected)
                if count >= 0:
                    library._lib.libusb_free_device_list.assert_called_once()


class TestErasePlan(unittest.TestCase):
    def test_sectors_are_registry_erase_units(self):
        self.assertEqual(dfuse.erase_plan(0x08000000, 0x08030000, 0x10000),
                         [0x08000000, 0x08010000, 0x08020000])

    def test_rejects_unaligned_range(self):
        with self.assertRaisesRegex(dfuse.DfuError, "not aligned"):
            dfuse.erase_plan(0x08000000, 0x08008000, 0x10000)

    def test_rejects_empty_range(self):
        with self.assertRaisesRegex(dfuse.DfuError, "not aligned"):
            dfuse.erase_plan(0x08000000, 0x08000000, 0x10000)


class TestDescriptors(unittest.TestCase):
    def test_reads_transfer_size_from_functional_descriptor(self):
        blob = _config_descriptor([_interface(0, 0, 2, transfer_size=1024)])
        entries = dfuse.parse_configuration_interfaces(blob)
        info = dfuse.parse_dfu_functional_descriptor(entries[0]["extra"])
        self.assertEqual(info["wTransferSize"], 1024)

    def test_dfu_interfaces_include_protocol_and_alternate_identity(self):
        blob = _config_descriptor([
            _interface(0, 0, 0, string_index=1),
            _interface(0, 1, 1, string_index=2),
            _interface(0, 2, 2, string_index=3, attributes=0x07, version=0x011A),
        ])
        entries = dfuse.parse_configuration_interfaces(blob)
        self.assertEqual([entry["bInterfaceProtocol"] for entry in entries], [0, 1, 2])
        raw = _config_descriptor([
            _interface(0, 0, 0, string_index=1),
            _interface(0, 1, 1, string_index=2),
            _interface(0, 2, 2, string_index=3),
        ])

        class Lib:
            class Handle:
                def libusb_set_configuration(self, *_args):
                    return 0

            _lib = Handle()

            def _serial(self, _handle, index):
                return {1: "runtime", 2: "other", 3: "CNVM"}[index]

        device = dfuse.UsbDevice(Lib(), None, "1-1", "", 1)
        device._raw_configuration_descriptor = lambda: raw
        interfaces = device.interfaces()
        self.assertEqual(len(interfaces), 1)
        self.assertEqual(interfaces[0]["protocol"], dfuse.DFU_INTERFACE_PROTOCOL_MODE)
        self.assertEqual(interfaces[0]["alt"], 2)
        self.assertEqual(interfaces[0]["name"], "CNVM")
        self.assertEqual(interfaces[0]["transfer_size"], 1024)
        self.assertEqual(interfaces[0]["bcd_dfu_version"], 0x011A)
        self.assertEqual(interfaces[0]["bm_attributes"], 0x07)

    def test_descriptor_parser_preserves_version_and_transfer_capabilities(self):
        blob = _config_descriptor([_interface(0, 0, 2, attributes=0x05, version=0x0100)])
        entry = dfuse.parse_configuration_interfaces(blob)[0]
        functional = dfuse.parse_dfu_functional_descriptor(entry["extra"])
        self.assertEqual(functional["bcdDFUVersion"], 0x0100)
        self.assertEqual(functional["bmAttributes"] & 0x01, 0x01)  # download
        self.assertEqual(functional["bmAttributes"] & 0x02, 0x00)  # upload disabled

    def test_rejects_dfu_mode_interface_without_functional_descriptor(self):
        descriptor = struct.pack("<BBBBBBBBB", 9, 0x04, 0, 0, 0, 0xFE, 0x01, 2, 0)
        raw = _config_descriptor([descriptor])

        class Lib:
            def _serial(self, _handle, _index):
                return "CNVM"

        device = dfuse.UsbDevice(Lib(), None, "1-1", "", 1)
        device._raw_configuration_descriptor = lambda: raw
        with self.assertRaisesRegex(dfuse.DfuError, "no valid functional descriptor"):
            device.interfaces()

    def test_ignores_non_dfu_interfaces(self):
        blob = _config_descriptor([_interface(0, 0, 2, interface_class=0x03,
                                               subclass=0x00)])
        self.assertEqual(dfuse.parse_configuration_interfaces(blob)[0]["bInterfaceClass"], 0x03)


class TestProtocol(unittest.TestCase):
    def setUp(self):
        self.model = DfuSeFlashModel(capacity=0x40000, transfer_size=4)
        self.protocol = dfuse.DfuSeProtocol(self.model, transfer_size=4)

    def test_short_final_block_uses_increasing_block_numbers_and_round_trips(self):
        data = b"ABCDEFGHI"
        self.protocol.write(self.model.base, data)
        transfers = [c for c in self.model.calls if c[0] == "out" and c[2] == dfuse.DFU_DNLOAD
                     and c[3] != 0]
        self.assertEqual([(c[3], c[5]) for c in transfers],
                         [(2, b"ABCD"), (3, b"EFGH"), (2, b"I")])
        tail_pointers = [struct.unpack("<BI", c[5])[1] for c in self.model.calls
                         if c[0] == "out" and c[2] == dfuse.DFU_DNLOAD and c[3] == 0
                         and len(c[5]) == 5 and c[5][0] == dfuse.DFUSE_CMD_SET_ADDRESS]
        self.assertEqual(tail_pointers, [self.model.base, self.model.base + 8])
        self.assertEqual(self.protocol.read(self.model.base, len(data)), data)
        self.assertEqual(self.model.memory[:len(data)], data)

    def test_multi_transfer_image_erases_writes_and_reads_back(self):
        model = DfuSeFlashModel(capacity=0x40000, transfer_size=1024)
        protocol = dfuse.DfuSeProtocol(model, transfer_size=1024)
        data = bytes((index * 37 + 11) & 0xFF for index in range(0x24000))
        for address in dfuse.erase_plan(model.base, model.base + 0x30000, 0x10000):
            protocol.erase(address)
        protocol.write(model.base, data)
        self.assertEqual(protocol.read(model.base, len(data)), data)
        self.assertEqual(model.memory[:len(data)], data)
        self.assertTrue(all(value == 0xFF for value in model.memory[len(data):0x30000]))

    def test_block_number_rollover_resets_pointer(self):
        model = DfuSeFlashModel(capacity=0x100, transfer_size=2)
        protocol = dfuse.DfuSeProtocol(model, transfer_size=2)
        with mock.patch.object(dfuse, "DFUSE_MAX_BLOCK_NUMBER", 4):
            with mock.patch.object(dfuse, "DFUSE_MAX_DATA_BLOCKS", 3):
                protocol.write(model.base, b"abcdefgh")
        set_addresses = [struct.unpack("<BI", c[5])[1] for c in model.calls
                         if c[0] == "out" and c[2] == dfuse.DFU_DNLOAD
                         and c[3] == 0 and len(c[5]) == 5
                         and c[5][0] == dfuse.DFUSE_CMD_SET_ADDRESS]
        self.assertEqual(set_addresses, [model.base, model.base + 6])
        self.assertEqual(model.memory[:8], b"abcdefgh")

    def test_upload_switches_from_download_idle_to_dfu_idle(self):
        self.protocol.write(self.model.base, b"test")
        self.protocol.read(self.model.base, 4)
        abort_positions = [i for i, c in enumerate(self.model.calls)
                           if c[0] == "out" and c[2] == dfuse.DFU_ABORT]
        upload_positions = [i for i, c in enumerate(self.model.calls)
                            if c[0] == "in" and c[2] == dfuse.DFU_UPLOAD]
        self.assertTrue(abort_positions and upload_positions)
        self.assertLess(abort_positions[-1], upload_positions[0])

    def test_short_final_upload_rebases_address_when_length_drives_block_math(self):
        model = DfuSeFlashModel(capacity=0x100, transfer_size=4)
        model.upload_uses_request_length = True
        model.memory[:9] = b"123456789"
        protocol = dfuse.DfuSeProtocol(model, transfer_size=4)
        self.assertEqual(protocol.read(model.base, 9), b"123456789")
        upload_calls = [c for c in model.calls if c[0] == "in" and c[2] == dfuse.DFU_UPLOAD]
        self.assertEqual([call[3] for call in upload_calls], [2, 3, 2])
        set_addresses = [struct.unpack("<BI", c[5])[1] for c in model.calls
                         if c[0] == "out" and c[2] == dfuse.DFU_DNLOAD and c[3] == 0
                         and len(c[5]) == 5 and c[5][0] == dfuse.DFUSE_CMD_SET_ADDRESS]
        self.assertEqual(set_addresses, [model.base, model.base + 8])

    def test_uses_poll_timeout_and_reaches_download_idle(self):
        with mock.patch.object(dfuse.time, "sleep") as sleep:
            self.protocol.write(self.model.base, b"abcd")
        self.assertTrue(any(call.args[0] >= 0.007 for call in sleep.call_args_list))

    def test_short_control_out_is_fatal_before_flash_changes(self):
        self.model.fail_short_out = True
        with self.assertRaisesRegex(dfuse.DfuError, "short USB control transfer"):
            self.protocol.write(self.model.base, b"abcd")
        self.assertEqual(self.model.memory[:4], b"\xFF" * 4)

    def test_bad_status_is_fatal(self):
        self.model.bad_next_status = True
        with self.assertRaisesRegex(dfuse.DfuError, "DFU status 1"):
            self.protocol.erase(self.model.base)
        self.assertEqual(self.model.memory[:0x10000], b"\xFF" * 0x10000)

    def test_short_upload_is_fatal(self):
        self.protocol.write(self.model.base, b"abcd")
        self.model.bad_upload = True
        with self.assertRaisesRegex(dfuse.DfuError, "short DFU upload"):
            self.protocol.read(self.model.base, 4)

    def test_leave_errors_are_not_swallowed(self):
        self.model.bad_next_status = True
        with self.assertRaisesRegex(dfuse.DfuError, "DFU status 1"):
            self.protocol.leave()

    def test_leave_accepts_manifest_wait_reset(self):
        self.protocol.leave()
        self.assertTrue(self.model.leave_requested)
        self.assertEqual(self.model.state, dfuse.STATE_DFU_MANIFEST_WAIT_RESET)

    def test_leave_accepts_transient_stall_followed_by_disconnect(self):
        with mock.patch.object(self.protocol, "set_address"), \
             mock.patch.object(self.protocol, "get_status", side_effect=[
                 dfuse.DfuStalled("stall"), dfuse.DfuDisconnected("gone")]), \
             mock.patch.object(dfuse.time, "sleep"):
            self.protocol.leave()

    def test_leave_does_not_hide_persistent_stall(self):
        with mock.patch.object(self.protocol, "set_address"), \
             mock.patch.object(self.protocol, "get_status", side_effect=dfuse.DfuStalled("stall")), \
             mock.patch.object(dfuse.time, "monotonic", side_effect=[0.0, 3.0]):
            with self.assertRaises(dfuse.DfuStalled):
                self.protocol.leave()

    def test_leave_accepts_transient_io_error_followed_by_disconnect(self):
        with mock.patch.object(self.protocol, "set_address"), \
             mock.patch.object(self.protocol, "get_status", side_effect=[
                 dfuse.DfuTransferError("I/O error"), dfuse.DfuDisconnected("gone")]), \
             mock.patch.object(dfuse.time, "sleep"):
            self.protocol.leave()

    def test_leave_does_not_hide_persistent_io_error(self):
        with mock.patch.object(self.protocol, "set_address"), \
             mock.patch.object(self.protocol, "get_status", side_effect=dfuse.DfuTransferError("I/O error")), \
             mock.patch.object(dfuse.time, "monotonic", side_effect=[0.0, 3.0]):
            with self.assertRaises(dfuse.DfuTransferError):
                self.protocol.leave()


if __name__ == "__main__":
    unittest.main()
