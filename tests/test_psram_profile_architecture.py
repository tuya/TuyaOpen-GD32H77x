"""Keep PSRAM profiles behind the common controller and TKL layers."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT.parents[1]
DRIVERS = ROOT / 'gd32h77x_os/components/drivers'
TKL_MEMORY = ROOT / 'tuyaos/tuyaos_adapter/src/system/tkl_memory.c'
PROFILE_FILES = (
    DRIVERS / 'gd32h77x_psram_aps6404l.c',
    DRIVERS / 'gd32h77x_psram_aps6408l.c',
)


class PsramProfileArchitectureTests(unittest.TestCase):
    def test_tkl_vendor_path_is_limited_to_non_octal_boards(self):
        source = TKL_MEMORY.read_text()
        self.assertIn('__tkl_vendor_psram_init', source)
        self.assertIn('!defined(GD32H77X_PSRAM_OCTAL)', source)
        self.assertIn('return __tkl_vendor_psram_init();', source)

    def test_profiles_use_controller_abstraction(self):
        forbidden = (
            'ospi_command_config(',
            'ospi_transmit(',
            'ospi_receive(',
            'OSPI_CTL(',
            'OSPI_STAT(',
            'gpio_bit_set(',
            'gpio_bit_reset(',
        )
        for path in PROFILE_FILES:
            source = path.read_text()
            with self.subTest(profile=path.name):
                for token in forbidden:
                    self.assertNotIn(token, source)

    def test_both_desk_buddy_profiles_are_registered(self):
        source = (DRIVERS / 'gd32h77x_psram.c').read_text()
        self.assertIn('gd32h77x_psram_aps6404l_ops', source)
        self.assertIn('gd32h77x_psram_aps6408l_ops', source)

    def test_minimal_probe_has_old_and_new_board_configs(self):
        config = SDK / 'examples/system/psram/config'
        self.assertTrue((config / 'GD32H779I_DESK_BUDDY.config').is_file())
        self.assertTrue((config / 'GD32H77EI_DESK_BUDDY.config').is_file())


if __name__ == '__main__':
    unittest.main()
