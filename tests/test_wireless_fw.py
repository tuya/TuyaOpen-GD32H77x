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
README = FW_ROOT / "README.md"


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

    readme = README.read_text(encoding="utf-8")
    for pin in ("PA6", "PA7", "PB15", "PA8", "RESET/NRST", "BOOT0"):
        assert pin in readme
    for h77_pin in ("PB12", "PB13", "PC5", "UART4"):
        assert h77_pin not in readme
    assert (FW_ROOT / "spi" / "README.md").is_file()

    print("wireless firmware package checks passed")


if __name__ == "__main__":
    main()
