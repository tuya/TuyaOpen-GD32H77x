"""Exercise the production mapped-mode setup with simulated OSPI registers."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / 'gd32h77x_os/components/drivers/vendor/aps6404l.c'
PERIPH = ROOT / 'gd32h77x_os/components/drivers/vendor/gd32h77x_vendor_ospi.c'


def function(path, signature):
    text = path.read_text()
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


@unittest.skipUnless(shutil.which('cc'), 'native C compiler required')
class MappedTimeoutTests(unittest.TestCase):
    def test_abort_accepts_timeout_terminal_state(self):
        source = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#define RESET 0
#define OSPI_CTL_MDMAEN (1U << 2)
#define OSPI_CTL_ABORT (1U << 1)
#define OSPI_FLAG_TERR (1U << 0)
#define OSPI_FLAG_TC (1U << 1)
#define OSPI_FLAG_TMOUT (1U << 4)
#define OSPI_FLAG_BUSY (1U << 5)
#define OSPI_STATC_TERRC (1U << 0)
#define OSPI_STATC_TCC (1U << 1)
#define OSPI_STATC_TMOUTC (1U << 4)
static uint32_t ctl, stat, statc;
static uint32_t *mock_ctl(void) { return &ctl; }
static uint32_t *mock_stat(void) {
    if (ctl & OSPI_CTL_ABORT) {
        ctl &= ~OSPI_CTL_ABORT;
        stat = (stat & ~OSPI_FLAG_BUSY) | OSPI_FLAG_TMOUT;
    }
    return &stat;
}
static uint32_t *mock_statc(void) { return &statc; }
#define OSPI_CTL(x) (*mock_ctl())
#define OSPI_STAT(x) (*mock_stat())
#define OSPI_STATC(x) (*mock_statc())
'''
        source += function(PERIPH, 'void psram_vendor_ospi_transmission_abort(')
        source += r'''
int main(void) {
    ctl = OSPI_CTL_MDMAEN;
    stat = OSPI_FLAG_BUSY;
    psram_vendor_ospi_transmission_abort(0U);
    assert((ctl & OSPI_CTL_MDMAEN) == 0U);
    assert((stat & OSPI_FLAG_BUSY) == 0U);
    assert(statc == (OSPI_STATC_TERRC | OSPI_STATC_TCC | OSPI_STATC_TMOUTC));
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path / 'abort.c').write_text(source)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-parameter', str(path / 'abort.c'),
                            '-o', str(path / 'abort')], check=True)
            subprocess.run([str(path / 'abort')], check=True)

    def test_vendor_mapped_setup_preserves_timeout_and_control(self):
        # Compile real function bodies; only register IO and unrelated command
        # setup are mocked. This catches ordering relative to the vendor helper.
        source = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#define ERROR 0
#define SUCCESS 1
#define RESET 0
#define OSPI_FLAG_BUSY (1U << 5)
#define OSPI_CTL_TMOUTEN (1U << 3)
#define OSPI_CTL_TMOUTIE (1U << 20)
#define OSPI_CTL_FMOD (3U << 28)
#define OSPI_MEMORY_MAPPED (3U << 28)
#define OSPI0 1U
#define OSPI0_IRQn 92U
#define OSPI_INT_TMOUT OSPI_CTL_TMOUTIE
#define OSPI_TIMEOUT_COUNT_ENABLE OSPI_CTL_TMOUTEN
#define OSPI_TIMEOUT_COUNT_DISABLE 0
#define OSPI_STATC_TMOUTC (1U << 4)
#define OSPI_OPTYPE_READ_CFG 1
#define OSPI_OPTYPE_WRITE_CFG 2
#define APS6404_CMD_FAST_READ_QUAD 0xEB
#define APS6404_CMD_QUAD_WRITE 0x38
#define OSPI_ADDRESS_4_LINES 3
#define OSPI_DUMYC_CYCLES_6 6
#define OSPI_DATA_4_LINES 3
static uint32_t regs[4], commands, interrupt_enabled, nvic_enabled;
#define OSPI_CTL(x) regs[0]
#define OSPI_LPOUT(x) regs[1]
#define OSPI_STATC(x) regs[2]
#define OSPI_STAT(x) regs[3]
typedef int ErrStatus;
typedef struct { uint32_t addr_mode, dummy_cycles, data_mode; } ospi_regular_cmd_struct;
typedef struct { uint32_t timeout_activation, timeout_period; } ospi_memorymapped_struct;
typedef struct { uint32_t ospi_periph, ospi; } aps6404_dev_struct;
static uint32_t aps6404_chunk_limit(const aps6404_dev_struct *d) { return d->ospi; }
static uint32_t aps6404_ins_mode(const aps6404_dev_struct *d) { (void)d; return 1; }
static void aps6404_cmd_base(ospi_regular_cmd_struct *c, uint32_t op, uint32_t mode, uint8_t instruction) {
    (void)mode; (void)c;
    assert((op == OSPI_OPTYPE_READ_CFG && instruction == 0xEB) ||
           (op == OSPI_OPTYPE_WRITE_CFG && instruction == 0x38));
}
static void ospi_command_config(uint32_t p, uint32_t *cfg, ospi_regular_cmd_struct *c) {
    (void)p; (void)cfg; assert(c->addr_mode == OSPI_ADDRESS_4_LINES);
    assert(c->data_mode == OSPI_DATA_4_LINES); ++commands;
}
static uint32_t ospi_flag_get(uint32_t p, uint32_t flag) { (void)p; return regs[3] & flag; }
static void NVIC_SetPriority(uint32_t irq, uint32_t priority) { (void)irq; (void)priority; }
static void NVIC_ClearPendingIRQ(uint32_t irq) { (void)irq; }
static void NVIC_EnableIRQ(uint32_t irq) { if (irq == OSPI0_IRQn) ++nvic_enabled; }
static void ospi_interrupt_enable(uint32_t p, uint32_t irq) { (void)p; regs[0] |= irq; ++interrupt_enabled; }
static void __DSB(void) {}
static void __ISB(void) {}
'''
        source += r'''
#define APS6404_MMAP_TIMEOUT 32
static void ospi_timeout_cycle_config(uint32_t p, uint16_t n) { (void)p; regs[1] = n; }
static void ospi_timeout_enable(uint32_t p) { (void)p; regs[0] |= OSPI_CTL_TMOUTEN; }
'''
        import re
        header = (PERIPH.parent / 'gd32h77x_vendor_ospi.h').read_text()
        names = set(re.findall(r'psram_vendor_(ospi_\w+)', header))
        for name in sorted(names, key=len, reverse=True):
            source = re.sub(r'\b' + name + r'\b', 'psram_vendor_' + name, source)
        source += function(PERIPH, 'void psram_vendor_ospi_memorymapped_mode(') + '\n'
        source += function(VENDOR, 'ErrStatus aps6404_memory_map_enable(') + '\n'
        source += r'''
int main(void) {
    aps6404_dev_struct dev = {OSPI0, 128};
    regs[0] = 0x301; /* enable and FIFO threshold must survive */
    assert(aps6404_memory_map_enable(NULL) == ERROR);
    assert(commands == 0);
    assert(aps6404_memory_map_enable(&dev) == SUCCESS);
    assert(commands == 2);
    assert(regs[0] == (0x301U | OSPI_MEMORY_MAPPED | OSPI_CTL_TMOUTEN |
                       OSPI_CTL_TMOUTIE));
    assert((regs[0] & OSPI_CTL_TMOUTIE) != 0);
    assert(interrupt_enabled == 1);
    assert(nvic_enabled == 1);
    assert(regs[1] == APS6404_MMAP_TIMEOUT);
    assert(aps6404_memory_map_enable(&dev) == SUCCESS);
    assert(regs[0] == (0x301U | OSPI_MEMORY_MAPPED | OSPI_CTL_TMOUTEN |
                       OSPI_CTL_TMOUTIE));
    dev.ospi = 0;
    assert(aps6404_memory_map_enable(&dev) == ERROR);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path / 'test.c').write_text(source)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-parameter', str(path / 'test.c'),
                            '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
