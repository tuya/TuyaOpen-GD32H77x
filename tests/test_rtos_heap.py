#!/usr/bin/env python3
"""Exercise the real allocator and production linker template with varying RAM use."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
OS = ROOT / 'gd32h77x_os'
HOST = ROOT / 'tests/rtos_heap_host'
ARM_GCC = os.environ.get('ARM_NONE_EABI_GCC') or shutil.which('arm-none-eabi-gcc')


def run(*args, **kwargs):
    return subprocess.run(args, text=True, capture_output=True, **kwargs)


class AllocatorTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'native C compiler required')
    def test_real_heap_under_sanitizers(self):
        for capacity in (16 * 1024, 128 * 1024):
            with self.subTest(capacity=capacity), tempfile.TemporaryDirectory() as tmp:
                folder = Path(tmp)
                script = folder / 'heap.ld'
                script.write_text('''SECTIONS { .test_heap (NOLOAD) : {
                    . = ALIGN(8); __heap_guard_before = .; . += 32;
                    __rtos_heap_start = .; ucHeap = .;
                    . += %d; __rtos_heap_end = .;
                    __heap_guard_after = .; . += 32;
                } } INSERT AFTER .bss;
                ''' % capacity)
                exe = folder / 'heap-test'
                result = run('cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                             '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                             '-I' + str(HOST), '-I' + str(OS / 'components/runtime'),
                             str(HOST / 'test_heap.c'),
                             str(OS / 'components/FreeRTOS/portable/MemMang/heap_4.c'),
                             '-Wl,-T,' + str(script), '-o', str(exe))
                self.assertEqual(result.returncode, 0, result.stderr)
                result = run(str(exe), env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('50000 allocation/free cycles passed', result.stdout)


@unittest.skipUnless(ARM_GCC, 'ARM_NONE_EABI_GCC or cross compiler in PATH required')
class LinkerTests(unittest.TestCase):
    def build(self, folder, static_bytes):
        script = (OS / 'components/startup/gd32h77x_linker.ld.in').read_text()
        script = script.replace('@CNVM_TEXT_INPUTS@', '*(.text*) *(.rodata*)')
        script = script.replace('@ECNVM_OUTPUT_SECTIONS@', '')
        (folder / 'firmware.ld').write_text(script)
        (folder / 'gd32h77x_memory.ldinc').write_text('''MEMORY {
            BOOT (rx) : ORIGIN = 0x08000000, LENGTH = 64K
            CNVM (rx) : ORIGIN = 0x08010000, LENGTH = 64K
            AXISRAM (xrw) : ORIGIN = 0x24000000, LENGTH = 128K
        }''')
        (folder / 'fixture.c').write_text('''#include <stdint.h>
            extern char _sp;
            void Reset_Handler(void) { for (;;) {} }
            __attribute__((section(".boot_table"), used))
            const uintptr_t boot[] = {(uintptr_t)&_sp, (uintptr_t)Reset_Handler};
            __attribute__((section(".vectors"), used))
            const uintptr_t __gVectors[] = {(uintptr_t)&_sp, (uintptr_t)Reset_Handler};
            __attribute__((section(".ramfunc"), used)) void ram_code(void) {}
            volatile unsigned char fixture_bss[%d];
            volatile unsigned fixture_data = 1;
        ''' % static_bytes)
        return run(ARM_GCC, '-mcpu=cortex-m7', '-mthumb', '-Os', '-nostdlib',
                   'fixture.c', '-Wl,-T,firmware.ld', '-o', 'fixture.elf', cwd=folder)

    def symbols(self, folder):
        nm = str(Path(ARM_GCC).with_name('arm-none-eabi-nm'))
        result = run(nm, str(folder / 'fixture.elf'))
        self.assertEqual(result.returncode, 0, result.stderr)
        return {parts[2]: int(parts[0], 16) for line in result.stdout.splitlines()
                if len(parts := line.split()) == 3}

    def test_static_growth_automatically_shrinks_heap(self):
        sizes = []
        for static_bytes in (1024, 5120, 33792):
            with tempfile.TemporaryDirectory() as tmp:
                folder = Path(tmp)
                result = self.build(folder, static_bytes)
                self.assertEqual(result.returncode, 0, result.stderr)
                symbols = self.symbols(folder)
                start, end = symbols['__rtos_heap_start'], symbols['__rtos_heap_end']
                self.assertEqual(start, symbols['ucHeap'])
                self.assertEqual(start, symbols['_heap_end'])
                self.assertEqual(end, symbols['_stack_bottom'])
                self.assertEqual(symbols['_heap_end'] - symbols['_end'], 16 * 1024)
                self.assertEqual(symbols['_sp'] - end, 8 * 1024)
                self.assertEqual(start % 8, 0)
                self.assertEqual(end % 8, 0)
                sizes.append(end - start)
        self.assertEqual(sizes[0] - sizes[1], 4096)
        self.assertEqual(sizes[0] - sizes[2], 32768)

    def test_static_data_cannot_overlap_stack(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = self.build(Path(tmp), 128 * 1024)
            self.assertNotEqual(result.returncode, 0, 'RAM overflow must fail at link time')

    def test_too_small_heap_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            # 128 KiB SRAM minus the separate 24 KiB libc/stack reservations.
            result = self.build(Path(tmp), 104 * 1024 - 512)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Insufficient AXI SRAM for FreeRTOS heap', result.stderr)


if __name__ == '__main__':
    unittest.main()
