"""Host checks for the GD32H77x UART ROM-ISP transport."""
from __future__ import annotations

import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[3]
PLATFORM = ROOT / "platform/GD32H77X"
SCRIPT = PLATFORM / "gd32h77x_os/tools/download/gd32h77x_uart_isp.py"
SPEC = importlib.util.spec_from_file_location("gd32h77x_uart_isp_test", SCRIPT)
uart = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(uart)

EXPECTED_ID = b"ABCD"
TEST_OSPI_CONFIG = b"\x01\x02\x03"
TEST_OSPI_PINS = tuple(bytes([index]) + bytes([index + 0x10]) * 4
                       for index in range(1, 7))
TEST_OSPI_PROFILES = {"nor": {"config": TEST_OSPI_CONFIG, "pins": TEST_OSPI_PINS}}


class ScriptedTransport:
    def __init__(self, incoming=b""):
        self.incoming = bytearray(incoming)
        self.writes = []
        self.closed = False
        self.baud = 0

    def write(self, data):
        self.writes.append(bytes(data))

    def read(self, size):
        data = bytes(self.incoming[:size])
        del self.incoming[:len(data)]
        return data

    def feed(self, data):
        self.incoming.extend(data)

    def reset_input_buffer(self):
        self.incoming.clear()

    def set_baud(self, baud):
        self.baud = int(baud)
        self.incoming.clear()

    def close(self):
        self.closed = True


class RomTransport(ScriptedTransport):
    """Small ROM protocol model used to exercise the complete host flow."""

    def __init__(self, *, corrupt_cnvm_read=False, fail_ospi_erase=False):
        super().__init__()
        self.cnvm = bytearray(b"\xFF" * 0x200000)
        self.ospi = bytearray(b"\xFF" * 0x800000)
        self.cnvm_erases = []
        self.ospi_erases = []
        self.cnvm_writes = []
        self.ospi_writes = []
        self.ospi_config = None
        self.ospi_pins = None
        self.ospi_pin_records = []
        self.ospi_active_config = None
        self.ospi_erase_profiles = []
        self.ospi_write_profiles = []
        self.corrupt_cnvm_read = corrupt_cnvm_read
        self.fail_ospi_erase = fail_ospi_erase
        self.state = None
        self.address = None

    @staticmethod
    def _ack(ok=True):
        return bytes([uart.ISP_ACK if ok else uart.ISP_NACK])

    def write(self, data):
        data = bytes(data)
        self.writes.append(data)
        if data == bytes([uart.ISP_SYNC]):
            self.feed(self._ack())
            return
        if self.state is None:
            self._command(data)
        else:
            self._continue(data)

    def _command(self, data):
        if data == uart.CMD_GET_ID:
            self.feed(b"\x79\x04ABCD\x79")
        elif data == uart.CMD_WRITE:
            self.feed(self._ack())
            self.state = "write-address"
        elif data == uart.CMD_READ:
            self.feed(self._ack())
            self.state = "read-address"
        elif data == uart.CMD_OSPI_CONFIG:
            self.feed(self._ack())
            self.state = "ospi-config"
        elif data == uart.CMD_OSPI_PINS:
            self.feed(self._ack())
            self.state = "ospi-pins"
        elif data == uart.CMD_OSPI_ERASE_4K:
            self.feed(self._ack())
            self.state = "ospi-erase-address"
        elif data == uart.CMD_EXTENDED_ERASE:
            self.feed(self._ack())
            self.state = "extended-erase"
        elif data == uart.CMD_BASE_ERASE:
            self.feed(self._ack())
            self.state = "base-erase"
        else:
            self.feed(self._ack(False))

    @staticmethod
    def _parse_checked(data):
        if len(data) < 2 or uart._xor(data[:-1]) != data[-1]:
            return None
        return data[:-1]

    def _continue(self, data):
        state = self.state
        if state == "write-address":
            frame = self._parse_checked(data)
            if frame is None:
                self.feed(self._ack(False))
                self.state = None
                return
            self.address = int.from_bytes(frame, "big")
            self.feed(self._ack())
            self.state = "write-data"
        elif state == "write-data":
            if not data:
                self.feed(self._ack(False))
                self.state = None
                return
            length = data[0] + 1
            payload = data[1:1 + length]
            checksum = data[1 + length] if len(data) > 1 + length else None
            if len(payload) != length or checksum != (data[0] ^ uart._xor(payload)):
                self.feed(self._ack(False))
                self.state = None
                return
            if uart.CNVM_BASE <= self.address < uart.CNVM_BASE + len(self.cnvm):
                offset = self.address - uart.CNVM_BASE
                self.cnvm[offset:offset + length] = payload
                self.cnvm_writes.append((self.address, payload))
            elif any(window <= self.address < window + len(self.ospi)
                     for window in (uart.OSPI0_WINDOW, uart.OSPI1_WINDOW)):
                window = uart.OSPI0_WINDOW if self.address >= uart.OSPI0_WINDOW else uart.OSPI1_WINDOW
                offset = self.address - window
                self.ospi[offset:offset + length] = payload
                self.ospi_writes.append((self.address, payload))
                self.ospi_write_profiles.append((self.address, self.ospi_active_config))
            else:
                self.feed(self._ack(False))
                self.state = None
                return
            self.feed(self._ack())
            self.state = None
        elif state == "read-address":
            frame = self._parse_checked(data)
            if frame is None:
                self.feed(self._ack(False))
                self.state = None
                return
            self.address = int.from_bytes(frame, "big")
            self.feed(self._ack())
            self.state = "read-length"
        elif state == "read-length":
            length = data[0] + 1 if data else 0
            if len(data) != 2 or data[1] != (-length) & 0xFF:
                self.feed(self._ack(False))
                self.state = None
                return
            if uart.CNVM_BASE <= self.address < uart.CNVM_BASE + len(self.cnvm):
                offset = self.address - uart.CNVM_BASE
                payload = bytes(self.cnvm[offset:offset + length])
                if self.corrupt_cnvm_read and payload:
                    payload = bytes([payload[0] ^ 0xFF]) + payload[1:]
            elif any(window <= self.address < window + len(self.ospi)
                     for window in (uart.OSPI0_WINDOW, uart.OSPI1_WINDOW)):
                window = uart.OSPI0_WINDOW if self.address >= uart.OSPI0_WINDOW else uart.OSPI1_WINDOW
                offset = self.address - window
                payload = bytes(self.ospi[offset:offset + length])
            else:
                self.feed(self._ack(False))
                self.state = None
                return
            self.feed(self._ack() + payload)
            self.state = None
        elif state in ("ospi-config", "ospi-pins"):
            frame = self._parse_checked(data)
            self.feed(self._ack(frame is not None))
            if frame is not None:
                if state == "ospi-config":
                    self.ospi_config = frame
                    self.ospi_active_config = frame
                else:
                    self.ospi_pins = frame
                    self.ospi_pin_records.append(frame)
            self.state = None
        elif state == "ospi-erase-address":
            frame = self._parse_checked(data)
            if frame is None:
                self.feed(self._ack(False))
                self.state = None
                return
            self.address = int.from_bytes(frame, "big")
            self.feed(self._ack())
            self.state = "ospi-erase-index"
        elif state == "ospi-erase-index":
            frame = self._parse_checked(data)
            if frame is None:
                self.feed(self._ack(False))
                self.state = None
                return
            index = int.from_bytes(frame, "big")
            offset = index * uart.NOR_SECTOR
            window = uart.OSPI0_WINDOW if self.address >= uart.OSPI0_WINDOW else uart.OSPI1_WINDOW
            physical_offset = self.address - window
            if self.fail_ospi_erase:
                self.feed(self._ack(False))
            elif offset + uart.NOR_SECTOR > len(self.ospi) or physical_offset != offset:
                self.feed(self._ack(False))
            else:
                self.ospi[offset:offset + uart.NOR_SECTOR] = b"\xFF" * uart.NOR_SECTOR
                self.ospi_erases.append(physical_offset)
                self.ospi_erase_profiles.append((self.address, self.ospi_active_config))
                self.feed(self._ack())
            self.state = None
        elif state == "extended-erase":
            frame = self._parse_checked(data)
            if frame is None or len(frame) < 2:
                self.feed(self._ack(False))
                self.state = None
                return
            count = int.from_bytes(frame[:2], "big") + 1
            pages = [int.from_bytes(frame[2 + index * 2:4 + index * 2], "big")
                     for index in range(count)]
            if len(frame) != 2 + count * 2:
                self.feed(self._ack(False))
                self.state = None
                return
            for page in pages:
                offset = page * uart.CNVM_PAGE
                if offset + uart.CNVM_PAGE > len(self.cnvm):
                    self.feed(self._ack(False))
                    self.state = None
                    return
                self.cnvm[offset:offset + uart.CNVM_PAGE] = b"\xFF" * uart.CNVM_PAGE
                self.cnvm_erases.append(page)
            self.feed(self._ack())
            self.state = None
        elif state == "base-erase":
            if len(data) < 2:
                self.feed(self._ack(False))
                self.state = None
                return
            body = data[:-1]
            if uart._xor(body) != data[-1]:
                self.feed(self._ack(False))
                self.state = None
                return
            count = body[0] + 1
            pages = list(reversed(body[1:1 + count]))
            if len(pages) != count:
                self.feed(self._ack(False))
                self.state = None
                return
            for page in pages:
                offset = page * uart.CNVM_PAGE
                if offset + uart.CNVM_PAGE > len(self.cnvm):
                    self.feed(self._ack(False))
                    self.state = None
                    return
                self.cnvm[offset:offset + uart.CNVM_PAGE] = b"\xFF" * uart.CNVM_PAGE
                self.cnvm_erases.append(page)
            self.feed(self._ack())
            self.state = None
        else:
            self.feed(self._ack(False))
            self.state = None


def _manifest(segments):
    return {
        "layout": {
            "target_chip": "GD32H77DIW",
            "devices": [{
                "id": "nor",
                "part_number": "GD25Q64ESIG",
                "kind": "flash",
                "region": "NOR",
                "access": "indirect",
                "capacity_bytes": 0x800000,
                "erase_bytes": 0x1000,
                "page_bytes": 0x100,
                "cpu_base": None,
                "executable": False,
            }],
            "board_binding": {
                "nor": {
                    "controller": "OSPI0",
                    "chip_select": "PC11",
                    "clock": "PF10",
                    "data": ["PF8", "PF9", "PF7", "PA1"],
                },
            },
            "partitions": {"staging": {
                "role": "data", "size_bytes": sum(s["size_bytes"] for s in segments),
                "erase_bytes": 0x1000, "segments": segments,
            }},
        },
    }


def _payload(path: Path, data: bytes):
    path.write_bytes(data)
    return {
        "partition": "staging",
        "path": str(path),
        "size_bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }


class UartProtocolTests(unittest.TestCase):
    def test_handshake_accepts_ack_and_retries(self):
        class AckTransport(ScriptedTransport):
            def write(self, data):
                super().write(data)
                if data == b"\x7f":
                    self.feed(b"\x79")

        transport = AckTransport()
        session = uart.UartIsp(transport, timeout=0.1)
        session.handshake()
        self.assertEqual(transport.writes, [b"\x7f"])

        class RetryTransport(ScriptedTransport):
            attempts = 0

            def write(self, data):
                super().write(data)
                if data == b"\x7f":
                    self.attempts += 1
                    if self.attempts == 2:
                        self.feed(b"\x79")

        retry = RetryTransport()
        uart.UartIsp(retry, timeout=0.01).handshake(retries=2, delay=0)
        self.assertEqual(retry.attempts, 2)

    def test_handshake_tolerates_one_leading_zero(self):
        class NoisyTransport(ScriptedTransport):
            def write(self, data):
                super().write(data)
                if data == b"\x7f":
                    self.feed(b"\x00\x79")

        transport = NoisyTransport()
        uart.UartIsp(transport, timeout=0.1).handshake()
        self.assertEqual(transport.writes, [b"\x7f"])

    def test_handshake_reports_nack(self):
        class NackTransport(ScriptedTransport):
            def write(self, data):
                super().write(data)
                if data == b"\x7f":
                    self.feed(b"\x1f")

        transport = NackTransport()
        uart.UartIsp(transport, timeout=0.1).handshake()

    def test_getid_waits_and_reads_length_payload_and_ack(self):
        transport = ScriptedTransport(b"\x79\x03\x12\x34\x56\x79")
        with patch.object(uart.time, "sleep") as sleep:
            identifier = uart.UartIsp(transport, timeout=0.1).get_id()
        self.assertEqual(identifier, b"\x12\x34\x56")
        self.assertEqual(transport.writes, [uart.CMD_GET_ID])
        sleep.assert_any_call(0.05)

    def test_select_baud_keeps_initial_capture_rate_without_host_side_speedup(self):
        class AckTransport(ScriptedTransport):
            def write(self, data):
                super().write(data)
                if data == bytes([uart.ISP_SYNC]):
                    self.feed(b"\x79")

        transport = AckTransport()
        session = uart.UartIsp(transport, timeout=0.01)

        selected = session.select_baud(
            (921600, 460800, 230400, 115200), timeout=1, delay=0)
        self.assertEqual(selected, 115200)
        self.assertEqual(transport.baud, 115200)

    def test_auto_baud_prefers_115200_then_falls_back_to_9600(self):
        class LowRateOnlyTransport(ScriptedTransport):
            def __init__(self):
                super().__init__()
                self.rates = []

            def set_baud(self, baud):
                super().set_baud(baud)
                self.rates.append(int(baud))

            def write(self, data):
                super().write(data)
                if data == bytes([uart.ISP_SYNC]) and self.baud == 9600:
                    self.feed(bytes([uart.ISP_ACK]))

        transport = LowRateOnlyTransport()
        selected = uart.UartIsp(transport, timeout=0.01).select_baud(
            uart.AUTO_UART_BAUDS, timeout=1, delay=0)

        self.assertEqual(selected, 9600)
        self.assertLess(transport.rates.index(115200), transport.rates.index(9600))
        self.assertEqual(transport.rates[-1], 9600)
        self.assertEqual(transport.baud, 9600)

    def test_nack_in_command_fails(self):
        transport = ScriptedTransport(b"\x1f")
        with self.assertRaisesRegex(uart.UartIspError, "NACK"):
            uart.UartIsp(transport, timeout=0.1).write(0x08000000, b"ABCD")

    def test_write_frame_uses_address_xor_and_checksum(self):
        transport = ScriptedTransport(b"\x79\x79\x79")
        payload = b"\x01\x02\x03\x04\x05\x06\x07\x08"
        uart.UartIsp(transport, timeout=0.1).write(0x08001234, payload)
        self.assertEqual(transport.writes[0], uart.CMD_WRITE)
        self.assertEqual(transport.writes[1], b"\x08\x00\x12\x34\x2e")
        self.assertEqual(transport.writes[2],
                         bytes([len(payload) - 1]) + payload +
                         bytes([(len(payload) - 1) ^ uart._xor(payload)]))

    def test_read_frame_uses_len_and_twos_complement(self):
        transport = ScriptedTransport(b"\x79\x79\x79" + b"\xaa\xbb\xcc")
        data = uart.UartIsp(transport, timeout=0.1).read(0x70000000, 3)
        self.assertEqual(data, b"\xaa\xbb\xcc")
        self.assertEqual(transport.writes[0], uart.CMD_READ)
        self.assertEqual(transport.writes[1], b"\x70\x00\x00\x00\x70")
        self.assertEqual(transport.writes[2], b"\x02\xfd")

    def test_ospi_config_and_pin_frames(self):
        transport = ScriptedTransport(b"\x79\x79\x79\x79")
        session = uart.UartIsp(transport, timeout=0.1)
        session.config_ospi(b"\x01\x02\x03")
        session.config_ospi_pins(b"\x04\x05\x06\x07\x08")
        self.assertEqual(transport.writes[0], uart.CMD_OSPI_CONFIG)
        self.assertEqual(transport.writes[1], b"\x01\x02\x03\x00")
        self.assertEqual(transport.writes[2], uart.CMD_OSPI_PINS)
        self.assertEqual(transport.writes[3], b"\x04\x05\x06\x07\x08\x08")

    def test_ospi_sector_erase_frame(self):
        transport = ScriptedTransport(b"\x79\x79\x79")
        uart.UartIsp(transport, timeout=0.1).erase_ospi_sector(0x90012000, 0x12)
        self.assertEqual(transport.writes[0], uart.CMD_OSPI_ERASE_4K)
        self.assertEqual(transport.writes[1], b"\x90\x01\x20\x00\xb1")
        self.assertEqual(transport.writes[2], b"\x00\x00\x00\x12\x12")

    def test_extended_erase_frame_is_big_endian_and_checksummed(self):
        transport = ScriptedTransport(b"\x79\x79")
        uart.UartIsp(transport, timeout=0.1).erase_pages([1, 2], mode="extended")
        body = b"\x00\x01\x00\x01\x00\x02"
        self.assertEqual(transport.writes[0], uart.CMD_EXTENDED_ERASE)
        self.assertEqual(transport.writes[1], body + bytes([uart._xor(body)]))

    def test_base_erase_frame_uses_reverse_page_order(self):
        transport = ScriptedTransport(b"\x79\x79")
        uart.UartIsp(transport, timeout=0.1).erase_pages([1, 2, 3], mode="base")
        body = b"\x02\x03\x02\x01"
        self.assertEqual(transport.writes[0], uart.CMD_BASE_ERASE)
        self.assertEqual(transport.writes[1], body + bytes([uart._xor(body)]))

    def test_ospi_controller_windows_are_preserved(self):
        self.assertEqual(uart.ospi_window("OSPI0"), 0x90000000)
        self.assertEqual(uart.ospi_window("OSPI1"), 0x70000000)
        with self.assertRaisesRegex(uart.UartIspError, "unsupported"):
            uart.ospi_window("OSPI2")

    def test_readback_mismatch_reports_exact_address(self):
        transport = ScriptedTransport(b"\x79\x79\x79" + b"\x00\x02")
        with self.assertRaisesRegex(uart.UartIspError, "0x08000001"):
            uart._verify_range(uart.UartIsp(transport, timeout=0.1),
                               0x08000000, b"\x00\x01", "test image")


class UartProgrammingTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="gd32h77x-uart-isp-")
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_flash_programs_ospi_then_cnvm_and_verifies(self):
        payload_data = bytes(range(256)) * 16
        payload = _payload(self.root / "staging.bin", payload_data)
        manifest = _manifest([{
            "device": "nor",
            "device_offset_bytes": 0x2000,
            "logical_offset_bytes": 0,
            "size_bytes": len(payload_data),
        }])
        image_data = b"CNVM" + b"\x5a" * (0x10000 - 4) + b"TAIL"
        image = self.root / "firmware.bin"
        image.write_bytes(image_data)
        rom = RomTransport()
        result = uart.flash(
            manifest, [payload], image, uart.CNVM_BASE, uart.CNVM_BASE + 0x20000,
            0x20000, {"erase_bytes": uart.CNVM_PAGE}, transport=rom, expected_id=EXPECTED_ID, expected_chip="test-chip",
            ospi_profiles=TEST_OSPI_PROFILES, verify=True)
        self.assertTrue(result["success"])
        self.assertTrue(result["verified"])
        self.assertEqual(rom.ospi_erases, [0x2000])
        self.assertEqual(rom.cnvm_erases, [0, 1])
        self.assertEqual(bytes(rom.ospi[0x2000:0x2000 + len(payload_data)]), payload_data)
        self.assertEqual(bytes(rom.cnvm[:len(image_data)]), image_data)
        self.assertEqual(rom.ospi_config, TEST_OSPI_CONFIG)
        self.assertEqual(rom.ospi_pin_records, list(TEST_OSPI_PINS))
        self.assertEqual(result["external"][0]["partition"], "staging")
        self.assertEqual(result["internal"]["size_bytes"], len(image_data))

    def test_auto_baud_keeps_initial_capture_rate(self):
        image = self.root / "firmware.bin"
        image.write_bytes(b"CNVM")
        rom = RomTransport()
        result = uart.flash(
            _manifest([]), [], image, uart.CNVM_BASE,
            uart.CNVM_BASE + uart.CNVM_PAGE, uart.CNVM_PAGE,
            {"erase_bytes": uart.CNVM_PAGE}, baud="auto",
            transport=rom, expected_id=EXPECTED_ID, expected_chip="test-chip", verify=False)
        self.assertEqual(result["baud"], 115200)
        self.assertEqual(rom.baud, 115200)

    def test_missing_or_wrong_expected_id_stops_before_flash_commands(self):
        image = self.root / "firmware.bin"
        image.write_bytes(b"CNVM")
        for expected in (None, b""):
            rom = RomTransport()
            with self.assertRaisesRegex(uart.UartIspError, "expected_id"):
                uart.flash(_manifest([]), [], image, uart.CNVM_BASE,
                           uart.CNVM_BASE + uart.CNVM_PAGE, uart.CNVM_PAGE,
                           {"erase_bytes": uart.CNVM_PAGE}, transport=rom,
                           expected_id=expected)
            self.assertEqual(rom.writes, [])
        for baud in (115200, "auto"):
            rom = RomTransport()
            with self.assertRaisesRegex(uart.UartIspError, "actual=.*expected=.*target=test-chip"):
                uart.flash(_manifest([]), [], image, uart.CNVM_BASE,
                           uart.CNVM_BASE + uart.CNVM_PAGE, uart.CNVM_PAGE,
                           {"erase_bytes": uart.CNVM_PAGE}, baud=baud,
                           transport=rom, expected_id=b"WRONG", expected_chip="test-chip")
            self.assertFalse(any(w in (uart.CMD_WRITE, uart.CMD_EXTENDED_ERASE,
                                       uart.CMD_BASE_ERASE, uart.CMD_OSPI_CONFIG)
                                 for w in rom.writes))

    def test_ospi_requires_explicit_configuration_before_transport_creation(self):
        payload = _payload(self.root / "staging.bin", b"X" * 0x1000)
        image = self.root / "firmware.bin"
        image.write_bytes(b"CNVM")
        with patch.object(uart, "SerialTransport", side_effect=AssertionError("opened")):
            with self.assertRaisesRegex(uart.UartIspError, "explicit per-device OSPI profiles"):
                uart.flash(_manifest([{
                    "device": "nor", "device_offset_bytes": 0,
                    "logical_offset_bytes": 0, "size_bytes": 0x1000,
                }]), [payload], image, uart.CNVM_BASE,
                    uart.CNVM_BASE + uart.CNVM_PAGE, uart.CNVM_PAGE,
                    {"erase_bytes": uart.CNVM_PAGE}, port="fake",
                    expected_id=EXPECTED_ID)

    def test_empty_cnvm_is_rejected_before_ospi_configuration_or_erase(self):
        payload = _payload(self.root / "staging.bin", b"X" * 0x1000)
        image = self.root / "firmware.bin"
        image.write_bytes(b"")
        rom = RomTransport()
        with self.assertRaisesRegex(uart.UartIspError, "CNVM image is empty"):
            uart.flash(_manifest([{
                "device": "nor", "device_offset_bytes": 0,
                "logical_offset_bytes": 0, "size_bytes": 0x1000,
            }]), [payload], image, uart.CNVM_BASE,
                uart.CNVM_BASE + uart.CNVM_PAGE, uart.CNVM_PAGE,
                {"erase_bytes": uart.CNVM_PAGE}, transport=rom,
                expected_id=EXPECTED_ID, ospi_profiles=TEST_OSPI_PROFILES)
        self.assertEqual(rom.writes, [])

    def test_profiles_switch_across_devices_and_controllers(self):
        data = b"A" * 0x1000 + b"B" * 0x1000 + b"C" * 0x1000
        payload = _payload(self.root / "multi.bin", data)
        image = self.root / "firmware.bin"
        image.write_bytes(b"CNVM")
        devices, bindings, segments, profiles = [], {}, [], {}
        controllers = ("OSPI0", "OSPI1", "OSPI0")
        configs = (b"\x01\x00\x00", b"\x02\x00\x00", b"\x03\x00\x00")
        for index, (controller, config) in enumerate(zip(controllers, configs)):
            device_id = f"nor{index}"
            devices.append({
                "id": device_id, "kind": "flash", "region": "NOR",
                "access": "indirect", "capacity_bytes": 0x800000,
                "erase_bytes": 0x1000, "page_bytes": 0x100,
            })
            bindings[device_id] = {
                "controller": controller, "chip_select": f"CS{index}",
                "clock": f"CLK{index}", "data": ["D0", "D1", "D2", "D3"],
            }
            segments.append({
                "device": device_id, "device_offset_bytes": 0,
                "logical_offset_bytes": index * 0x1000, "size_bytes": 0x1000,
            })
            profiles[device_id] = {
                "config": config,
                "pins": tuple(bytes([n]) + bytes([index]) * 4 for n in range(1, 7)),
            }
        manifest = {"layout": {
            "devices": devices, "board_binding": bindings,
            "partitions": {"staging": {
                "role": "data", "size_bytes": len(data), "erase_bytes": 0x1000,
                "segments": segments,
            }},
        }}
        rom = RomTransport()
        uart.flash(manifest, [payload], image, uart.CNVM_BASE,
                   uart.CNVM_BASE + uart.CNVM_PAGE, uart.CNVM_PAGE,
                   {"erase_bytes": uart.CNVM_PAGE}, transport=rom,
                   expected_id=EXPECTED_ID, ospi_profiles=profiles, verify=True)
        self.assertEqual(rom.ospi_erase_profiles, [
            (uart.OSPI0_WINDOW, configs[0]),
            (uart.OSPI1_WINDOW, configs[1]),
            (uart.OSPI0_WINDOW, configs[2]),
        ])
        active_configs = []
        for _address, config in rom.ospi_write_profiles:
            if not active_configs or active_configs[-1] != config:
                active_configs.append(config)
        self.assertEqual(active_configs, list(configs))

    def test_non_gino_target_and_two_data_signal_profile(self):
        payload = _payload(self.root / "generic.bin", b"N" * 0x1000)
        image = self.root / "firmware.bin"
        image.write_bytes(b"CNVM")
        manifest = _manifest([{
            "device": "generic_nor", "device_offset_bytes": 0,
            "logical_offset_bytes": 0, "size_bytes": 0x1000,
        }])
        layout = manifest["layout"]
        layout["target_chip"] = "OTHER_TARGET"
        device = layout["devices"][0]
        device["id"] = "generic_nor"
        device["part_number"] = "GENERIC_PART"
        binding = layout["board_binding"].pop("nor")
        binding["data"] = ["D0", "D1"]
        layout["board_binding"]["generic_nor"] = binding
        profile = {"generic_nor": {
            "config": b"\x09\x08\x07",
            "pins": (b"\x01\x10\x11\x12\x13", b"\x02\x20\x21\x22\x23",
                     b"\x03\x30\x31\x32\x33", b"\x04\x40\x41\x42\x43"),
        }}
        rom = RomTransport()
        result = uart.flash(
            manifest, [payload], image, uart.CNVM_BASE,
            uart.CNVM_BASE + uart.CNVM_PAGE, uart.CNVM_PAGE,
            {"erase_bytes": uart.CNVM_PAGE}, transport=rom,
            expected_id=EXPECTED_ID, expected_chip="OTHER_TARGET",
            ospi_profiles=profile, verify=True)
        self.assertTrue(result["success"])
        self.assertEqual(rom.ospi_config, b"\x09\x08\x07")
        self.assertEqual(len(rom.ospi_pin_records), 4)

    def test_only_payload_covered_ospi_sectors_are_erased(self):
        data = b"\x11" * 0x1000 + b"\x22" * 0x1000
        payload = _payload(self.root / "staging.bin", data)
        manifest = _manifest([
            {"device": "nor", "device_offset_bytes": 0x2000,
             "logical_offset_bytes": 0, "size_bytes": 0x1000},
            {"device": "nor", "device_offset_bytes": 0x5000,
             "logical_offset_bytes": 0x1000, "size_bytes": 0x1000},
        ])
        image = self.root / "firmware.bin"
        image.write_bytes(b"\xa5" * 0x10000)
        rom = RomTransport()
        uart.flash(manifest, [payload], image, uart.CNVM_BASE,
                   uart.CNVM_BASE + 0x10000, 0x10000,
                   {"erase_bytes": uart.CNVM_PAGE}, transport=rom, expected_id=EXPECTED_ID, expected_chip="test-chip",
            ospi_profiles=TEST_OSPI_PROFILES, verify=True)
        self.assertEqual(rom.ospi_erases, [0x2000, 0x5000])
        self.assertEqual(bytes(rom.ospi[0x2000:0x3000]), b"\x11" * 0x1000)
        self.assertEqual(bytes(rom.ospi[0x5000:0x6000]), b"\x22" * 0x1000)
        self.assertEqual(bytes(rom.ospi[0x1000:0x2000]), b"\xFF" * 0x1000)
        self.assertEqual(bytes(rom.ospi[0x4000:0x5000]), b"\xFF" * 0x1000)

    def test_cnvm_readback_mismatch_fails_after_ospi(self):
        payload = _payload(self.root / "staging.bin", b"\x33" * 0x1000)
        manifest = _manifest([{
            "device": "nor", "device_offset_bytes": 0,
            "logical_offset_bytes": 0, "size_bytes": 0x1000,
        }])
        image = self.root / "firmware.bin"
        image.write_bytes(b"\x44" * 0x1000)
        rom = RomTransport(corrupt_cnvm_read=True)
        with self.assertRaisesRegex(uart.UartIspError, "readback mismatch"):
            uart.flash(manifest, [payload], image, uart.CNVM_BASE,
                       uart.CNVM_BASE + 0x10000, 0x10000,
                       {"erase_bytes": uart.CNVM_PAGE}, transport=rom, expected_id=EXPECTED_ID, expected_chip="test-chip",
            ospi_profiles=TEST_OSPI_PROFILES, verify=True)
        self.assertEqual(rom.ospi_erases, [0])
        self.assertEqual(rom.cnvm_erases, [0])

    def test_ospi_failure_stops_before_cnvm(self):
        payload = _payload(self.root / "staging.bin", b"\x55" * 0x1000)
        manifest = _manifest([{
            "device": "nor", "device_offset_bytes": 0,
            "logical_offset_bytes": 0, "size_bytes": 0x1000,
        }])
        image = self.root / "firmware.bin"
        image.write_bytes(b"\x66" * 0x1000)
        rom = RomTransport(fail_ospi_erase=True)
        with self.assertRaisesRegex(uart.UartIspError, "OSPI UART stage failed"):
            uart.flash(manifest, [payload], image, uart.CNVM_BASE,
                       uart.CNVM_BASE + 0x10000, 0x10000,
                       {"erase_bytes": uart.CNVM_PAGE}, transport=rom, expected_id=EXPECTED_ID, expected_chip="test-chip",
            ospi_profiles=TEST_OSPI_PROFILES, verify=True)
        self.assertEqual(rom.cnvm_erases, [])
        self.assertEqual(rom.cnvm_writes, [])

    def test_cnvm_requires_rom_sector_geometry(self):
        image = self.root / "firmware.bin"
        image.write_bytes(b"\x77" * 0x1000)
        with self.assertRaisesRegex(uart.UartIspError, "requires 65536-byte sectors"):
            uart.program_cnvm(ScriptedTransport(), image, uart.CNVM_BASE,
                              0x10000, 0x1000)


if __name__ == "__main__":
    unittest.main()
