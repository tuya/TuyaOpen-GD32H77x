"""Ensure PSRAM vendor sources only contain documented platform adaptations."""
from pathlib import Path
import re
import unittest
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT.parents[1]
ARCHIVE = SDK / 'boards/GD32H77X/GD32H779I_DESK_BUDDY/docs/psram/GD32H779.zip'
DEST = ROOT / 'gd32h77x_os/components/drivers/vendor'


def function(path, signature):
    text = path.read_bytes().decode()
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def replace_function(text, signature, replacement):
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[:start] + replacement + text[end:]


class VendorSourcesTests(unittest.TestCase):
    def test_reference_semantics_are_preserved(self):
        with ZipFile(ARCHIVE) as archive:
            sources = {}
            for unit in ('ospi', 'ospim'):
                for ext, folder in (('h', 'Include'), ('c', 'Source')):
                    name = f'GD32H779/Firmware/GD32H77x_standard_peripheral/{folder}/gd32h77x_{unit}.{ext}'
                    sources[f'gd32h77x_vendor_{unit}.{ext}'] = archive.read(name).decode()
            for ext in ('h', 'c'):
                sources[f'aps6404l.{ext}'] = archive.read(
                    f'GD32H779/Utilities/bsp_drive/aps6404l.{ext}').decode()
        headers = '\n'.join(s for n, s in sources.items() if n.endswith('.h') and n != 'aps6404l.h')
        functions = re.findall(r'\b(?:void|FlagStatus|uint32_t|ErrStatus)\s+((?:ospi|ospim)_\w+)\s*\(', headers)
        types = re.findall(r'}\s*((?:ospi|ospim)_\w+)\s*;', headers)
        names = set(functions + types)
        pattern = r'\b(?:' + '|'.join(sorted(names, key=len, reverse=True)) + r')\b'
        for name, original in sources.items():
            with self.subTest(source=name):
                expected = re.sub(pattern, lambda m: 'psram_vendor_' + m[0], original)
                for unit in ('ospi', 'ospim'):
                    expected = expected.replace(f'"gd32h77x_{unit}.h"', f'"gd32h77x_vendor_{unit}.h"')
                    expected = expected.replace(f'GD32H77X_{unit.upper()}_H',
                                                f'GD32H77X_PSRAM_VENDOR_{unit.upper()}_H')
                if name == 'gd32h77x_vendor_ospi.c':
                    signature = 'void psram_vendor_ospi_transmission_abort('
                    actual_function = function(DEST / name, signature)
                    self.assertIn('OSPI_FLAG_TMOUT', actual_function)
                    self.assertIn('OSPI_STATC_TMOUTC', actual_function)
                    expected = replace_function(expected, signature, actual_function)
                elif name == 'aps6404l.c':
                    for signature, markers in (
                        ('ErrStatus aps6404_memory_map_enable(',
                         ('OSPI_INT_TMOUT', 'NVIC_EnableIRQ(OSPI0_IRQn)')),
                        ('void aps6404_memory_map_disable(',
                         ('psram_vendor_ospi_interrupt_disable',
                          'NVIC_DisableIRQ(OSPI0_IRQn)')),
                    ):
                        actual_function = function(DEST / name, signature)
                        for marker in markers:
                            self.assertIn(marker, actual_function)
                        expected = replace_function(expected, signature,
                                                    actual_function)
                self.assertEqual((DEST / name).read_bytes(), expected.encode())


if __name__ == '__main__':
    unittest.main()
