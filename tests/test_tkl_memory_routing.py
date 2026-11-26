"""Compile the production TKL memory routing helpers against a fake PSRAM range."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'tuyaos/tuyaos_adapter/src/system/tkl_memory.c'


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
class TklMemoryRoutingTests(unittest.TestCase):
    def test_generic_copy_and_fill_route_psram(self):
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ENABLE_PSRAM 1
#define TKL_PSRAM_ALIGNMENT 32U
static uint8_t mock_psram[4096] __attribute__((aligned(32)));
#define GD32H77X_PSRAM_MAPPED_BASE ((uintptr_t)mock_psram)
#define GD32H77X_PSRAM_HEAP_OFFSET_BYTES 0U
#define GD32H77X_PSRAM_HEAP_SIZE_BYTES ((uint32_t)sizeof(mock_psram))

static unsigned psram_copy_calls;
static unsigned psram_fill_calls;

static bool __tkl_psram_overlaps(const void *ptr, size_t size);
static bool __tkl_psram_range(const void *ptr, size_t size);
static void *__tkl_psram_memcpy(void *dst, const void *src, size_t size)
{
    ++psram_copy_calls;
    return memcpy(dst, src, size);
}
static void *__tkl_psram_memset(void *dst, int value, size_t size)
{
    ++psram_fill_calls;
    return memset(dst, value, size);
}
'''
        source += function(SOURCE, 'static bool __tkl_psram_overlaps(const void *ptr, size_t size)\n{') + '\n'
        source += function(SOURCE, 'static bool __tkl_psram_range(const void *ptr, size_t size)\n{') + '\n'
        source += function(SOURCE, 'void *tkl_system_memset(') + '\n'
        source += function(SOURCE, 'void *tkl_system_memcpy(') + r'''

int main(void)
{
    uint8_t src[128], dst[128];
    memset(src, 0x5A, sizeof(src));

    assert(tkl_system_memcpy(dst, src, sizeof(src)) == dst);
    assert(memcmp(dst, src, sizeof(src)) == 0);
    assert(psram_copy_calls == 0U);

    assert(tkl_system_memset(dst, 0xA5, sizeof(dst)) == dst);
    assert(dst[0] == 0xA5U && dst[127] == 0xA5U);
    assert(psram_fill_calls == 0U);

    uint8_t *psram = mock_psram + 64U;
    assert(tkl_system_memcpy(psram, src, 96U) == psram);
    assert(psram_copy_calls == 1U);
    assert(memcmp(psram, src, 96U) == 0);

    assert(tkl_system_memcpy(dst, psram + 7U, 33U) == dst);
    assert(psram_copy_calls == 2U);
    assert(memcmp(dst, src + 7U, 33U) == 0);

    assert(tkl_system_memset(psram + 3U, 0x3C, 65U) == psram + 3U);
    assert(psram_fill_calls == 1U);
    for (size_t i = 0U; i < 65U; ++i) assert(psram[3U + i] == 0x3CU);

    unsigned copies = psram_copy_calls;
    unsigned fills = psram_fill_calls;
    assert(tkl_system_memcpy(mock_psram + sizeof(mock_psram) - 8U,
                             src, 16U) == NULL);
    assert(tkl_system_memset(mock_psram + sizeof(mock_psram) - 8U,
                             0, 16U) == NULL);
    assert(psram_copy_calls == copies);
    assert(psram_fill_calls == fills);

    assert(tkl_system_memcpy(NULL, src, 1U) == NULL);
    assert(tkl_system_memset(NULL, 0, 1U) == NULL);
    assert(tkl_system_memcpy(dst, src, 0U) == dst);
    assert(tkl_system_memset(dst, 0, 0U) == dst);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'test.c'
            path.write_text(source)
            exe = Path(tmp) / 'test'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(path), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
