#!/usr/bin/env python3
"""Guard the H77X SPI/QSPI DMA short-buffer and DTCM fallback policy."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
DRIVERS = ROOT / "tuyaos" / "tuyaos_adapter" / "src" / "driver"


class DmaShortBufferPolicyTest(unittest.TestCase):
    def test_qspi_short_and_dtcm_buffers_use_cpu_path(self):
        source = (DRIVERS / "tkl_qspi.c").read_text(encoding="utf-8")
        self.assertIn("#define QSPI_DMA_MIN_SIZE 256U", source)
        self.assertIn("__qspi_dma_range_allowed(data + offset, chunk)", source)
        self.assertEqual(source.count("use_dma && chunk > QSPI_DMA_MIN_SIZE"), 2)

    def test_spi_short_and_dtcm_buffers_use_cpu_or_irq_path(self):
        source = (DRIVERS / "tkl_spi.c").read_text(encoding="utf-8")
        self.assertIn("#define GD32H77X_SPI_DMA_MIN_SIZE     256U", source)
        self.assertIn("size > GD32H77X_SPI_DMA_MIN_SIZE", source)
        self.assertIn("__spi_dma_range_allowed(send_buf, size)", source)
        self.assertIn("__spi_dma_range_allowed(receive_buf, size)", source)


if __name__ == "__main__":
    unittest.main()
