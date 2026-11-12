#!/usr/bin/env python3
"""Validate packaged wireless coprocessor firmware and its manifest."""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
FW_ROOT = ROOT / "wireless_fw" / "vw553"
UART_ROOT = FW_ROOT / "uart"
SPI_ROOT = FW_ROOT / "spi"
README = FW_ROOT / "README.md"
WIRING_DOC = ROOT / "docs" / "gd32h779v_start_vw553_wiring.md"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> None:
    manifest_path = UART_ROOT / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert manifest["transport"] == "uart"
    assert manifest["chip"] == "GD32VW553HMQ6"
    assert manifest["version"] == "0.5.2"

    image = manifest["image"]
    image_path = UART_ROOT / image["file"]
    assert image_path.is_file(), image_path
    assert image_path.stat().st_size == image["size"]
    assert sha256(image_path) == image["sha256"]
    assert f"v{manifest['version']}" in image_path.name

    sums = (UART_ROOT / "SHA256SUMS").read_text(encoding="utf-8").splitlines()
    assert len(sums) == 1
    sum_hash, sum_name = re.split(r"\s+", sums[0].strip(), maxsplit=1)
    assert sum_name == image_path.name
    assert sum_hash == image["sha256"]

    pins = manifest["pins"]
    assert pins["at_uart"] == {
        "peripheral": "UART2", "tx": "PA6", "rx": "PA7", "baudrate": 115200,
    }
    assert pins["log_uart"] == {
        "peripheral": "UART1", "tx": "PB15", "rx": "PA8", "baudrate": 115200,
    }
    assert pins["reset"] == "RESET/NRST"
    assert pins["boot_mode"] == "BOOT0"
    assert "h779_connection" not in manifest
    assert manifest["flashing"]["recommended_tool"] == "GigaDevice GD32 Mass Production Tool"
    assert manifest["flashing"]["tested_version"] == "1.0.5g"
    assert manifest["flashing"]["erase_mode"] == "page_erase"

    spi_manifest_path = SPI_ROOT / "manifest.json"
    spi_manifest = json.loads(spi_manifest_path.read_text(encoding="utf-8"))
    assert spi_manifest["transport"] == "SPI"
    assert spi_manifest["chip"] == "GD32VW553HMQ6"
    assert spi_manifest["version"] == "0.1.0"
    assert spi_manifest["status"] == "hardware-smoke-validated"
    assert "h779_connection" not in spi_manifest
    hardware = spi_manifest["hardware_validation"]
    assert hardware["status"] == "basic-at-gmr-smoke-passed"
    assert hardware["h779_board"] == "GD32H779V-START"
    assert hardware["vw553_board"] == "LCKFB_GD32VW553"
    assert hardware["gmr_version"] == "v1.0.3g-04f35186ae4a8741"
    assert hardware["checks"] == {
        "at_ok": True,
        "gmr_ok": True,
        "spi_timeouts": 0,
        "invalid_frames": 0,
        "valid_frames": 69,
    }
    assert hardware["cold_start_first_attempt"] == "failed; cause undetermined"
    assert "VW553 normal startup" in hardware["recovery_observed"]
    for not_verified in (
        "binary payload and +IPD transfer", "Socket business flows",
        "BLE business flows", "throughput", "long-duration stability",
        "cold-start stability",
    ):
        assert not_verified in hardware["not_verified"]

    spi_image = spi_manifest["image"]
    assert spi_image["flash_address"] == "0x08000000"
    spi_image_path = SPI_ROOT / spi_image["file"]
    assert spi_image_path.is_file(), spi_image_path
    assert spi_image_path.stat().st_size == spi_image["size"]
    assert sha256(spi_image_path) == spi_image["sha256"]
    assert f"v{spi_manifest['version']}" in spi_image_path.name
    spi_sums = (SPI_ROOT / "SHA256SUMS").read_text(encoding="utf-8").splitlines()
    assert len(spi_sums) == 1
    spi_sum_hash, spi_sum_name = re.split(r"\s+", spi_sums[0].strip(), maxsplit=1)
    assert spi_sum_name == spi_image_path.name
    assert spi_sum_hash == spi_image["sha256"]

    spi_config = spi_manifest["spi"]
    assert spi_config == {
        "mode": 0,
        "bit_order": "MSB-first",
        "word_bits": 8,
        "frame_bytes": 256,
        "header_bytes": 16,
        "payload_bytes_max": 240,
        "request": "VW553 PA3 input, active high",
        "ready": "VW553 PA4 output, active high",
    }

    readme = README.read_text(encoding="utf-8")
    for pin in ("PA6", "PA7", "PB15", "PA8", "RESET/NRST", "BOOT0"):
        assert pin in readme
    for h77_pin in ("PB12", "PB13", "PC5", "UART4"):
        assert h77_pin not in readme
    for h779_spi_pin in ("PE2", "PE3", "PE4", "PE5", "PE6"):
        assert h779_spi_pin not in readme

    spi_readme = (SPI_ROOT / "README.md").read_text(encoding="utf-8")
    for pin in ("PE2", "PE3", "PE4", "PE5", "PE6", "PA0", "PA1", "PA2", "PA3", "PA4"):
        assert pin in spi_readme
    for result in (
        "AT+GMR", "SPI_AT_PROBE_RESULT PASS at=1 version=1",
        "timeouts=0", "invalid=0", "valid=69",
        "首次冷启动尝试未通过", "原因尚未查明",
        "Socket 与 BLE 业务", "吞吐", "长时间稳定性",
    ):
        assert result in spi_readme

    wiring_doc = WIRING_DOC.read_text(encoding="utf-8")
    for result in (
        "AT+GMR", "SPI_AT_PROBE_RESULT PASS at=1 version=1",
        "timeouts=0", "invalid=0", "valid=69",
        "首次冷启动尝试失败且原因未定", "尚不能声称冷启动稳定",
        "Socket/BLE 业务", "吞吐和长稳均未验收",
    ):
        assert result in wiring_doc

    for markdown_path, content in ((README, readme),
                                   (SPI_ROOT / "README.md", spi_readme),
                                   (WIRING_DOC, wiring_doc)):
        for link in re.findall(r"\[[^\]]+\]\(([^)]+)\)", content):
            if link.startswith(("https://", "http://", "mailto:")):
                continue
            target = (markdown_path.parent / link.split("#", 1)[0]).resolve()
            assert target.is_file(), f"Broken local link in {markdown_path}: {link}"

    print("wireless firmware package checks passed")


if __name__ == "__main__":
    main()
