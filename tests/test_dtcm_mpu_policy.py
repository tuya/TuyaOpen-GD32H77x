#!/usr/bin/env python3
"""Guard the DTCM MPU attributes needed for unaligned task-stack accesses."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DtcmMpuPolicyTest(unittest.TestCase):
    def test_dtcm_is_normal_non_cacheable_memory(self):
        source = (ROOT / "gd32h77x_os" / "components" / "runtime" /
                  "gd32h77x_platform.c").read_text(encoding="utf-8")
        self.assertIn("region.tex_type = MPU_TEX_TYPE1;", source)
        self.assertIn("Normal non-cacheable memory", source)


if __name__ == "__main__":
    unittest.main()
