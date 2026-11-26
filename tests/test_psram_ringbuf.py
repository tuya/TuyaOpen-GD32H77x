"""Host contract tests for the production GD32 ring (hardware IO is mocked)."""
import ctypes as C
from pathlib import Path
import random
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ADAPTER = ROOT / 'tuyaos/tuyaos_adapter'


@unittest.skipUnless(shutil.which('cc'), 'native C compiler required')
class RingTests(unittest.TestCase):
    def test_byte_operations(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            (folder / 'tuya_cloud_types.h').write_text('''
#include <stdint.h>
#include <stdbool.h>
typedef int OPERATE_RET;
#define OPRT_OK 0
#define OPRT_INVALID_PARM -1
#define OPRT_NOT_SUPPORTED -2
#define OPRT_MALLOC_FAILED -3
''')
            (folder / 'tkl_memory.h').write_text('''
#include <stdlib.h>
#define tkl_system_malloc malloc
#define tkl_system_free free
void *tkl_system_psram_malloc(size_t);
void tkl_system_psram_free(void *);
void *tkl_system_memcpy(void *, const void *, size_t);
void *tkl_system_memset(void *, int, size_t);
int tkl_system_psram_get_free_heap_size(void);
''')
            (folder / 'tkl_output.h').write_text('''
#define tkl_log_output(...) ((void)0)
''')
            (folder / 'mock.c').write_text('''
#include <stdlib.h>
#include <string.h>
int psram_allocs, psram_frees, fail_alloc;
void *tkl_system_psram_malloc(size_t n) {
    ++psram_allocs;
    if (fail_alloc) return NULL;
    size_t rounded = (n + 31U) & ~(size_t)31U;
    void *p = NULL;
    if (posix_memalign(&p, 32U, rounded != 0U ? rounded : 32U) != 0) return NULL;
    return p;
}
void tkl_system_psram_free(void *p) { ++psram_frees; free(p); }
int tkl_system_psram_get_free_heap_size(void) { return 0; }
void *tkl_system_memcpy(void *d, const void *s, size_t n) {
    return memcpy(d, s, n);
}
void *tkl_system_memset(void *d, int v, size_t n) {
    return memset(d, v, n);
}
''')
            # Copy the public ring header so quoted type includes use host stubs.
            (folder / 'tuya_ringbuf.h').write_text(
                (ADAPTER / 'include/utilities/include/tuya_ringbuf.h').read_text())
            for enabled in (0, 1):
                lib = folder / f'ring{enabled}.so'
                subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                                f'-DENABLE_PSRAM={enabled}', '-I' + str(folder),
                                '-I' + str(ADAPTER / 'include/utilities/include'),
                                '-I' + str(ADAPTER / 'include/system'),
                                str(ADAPTER / 'include/utilities/src/tuya_ringbuf.c'),
                                str(folder / 'mock.c'), '-o', str(lib)], check=True)
                dll = C.CDLL(str(lib))
                create = dll.tuya_ring_buff_create
                create.argtypes = [C.c_uint32, C.c_int, C.POINTER(C.c_void_p)]
                for name in ('read', 'write', 'peek'):
                    getattr(dll, 'tuya_ring_buff_' + name).argtypes = [C.c_void_p, C.c_void_p, C.c_uint32]
                for name in ('free', 'reset', 'used_size_get', 'free_size_get'):
                    getattr(dll, 'tuya_ring_buff_' + name).argtypes = [C.c_void_p]
                dll.tuya_ring_buff_discard.argtypes = [C.c_void_p, C.c_uint32]
                for kind in ((0, 2) if enabled else (0,)):
                    for capacity in (1, 2, 5, 31, 33, 139):
                        C.c_int.in_dll(dll, 'psram_allocs').value = 0
                        C.c_int.in_dll(dll, 'psram_frees').value = 0
                        ring = C.c_void_p()
                        self.assertEqual(create(capacity, kind, C.byref(ring)), 0)
                        model = bytearray()
                        rng = random.Random(779)
                        for _ in range(2000):
                            op = rng.choice(('write', 'read', 'peek', 'discard', 'reset'))
                            count = rng.randrange(145)
                            buf = C.create_string_buffer(146)
                            if op == 'write':
                                data = rng.randbytes(count)
                                C.memmove(C.addressof(buf) + 1, data, count)
                                expected = min(count, capacity - 1 - len(model))
                                self.assertEqual(dll.tuya_ring_buff_write(ring, C.addressof(buf) + 1, count), expected)
                                model.extend(data[:expected])
                            elif op == 'reset':
                                self.assertEqual(dll.tuya_ring_buff_reset(ring), 0)
                                model.clear()
                            else:
                                expected = min(count, len(model))
                                fn = getattr(dll, 'tuya_ring_buff_' + op)
                                actual = fn(ring, count) if op == 'discard' else fn(ring, C.addressof(buf) + 1, count)
                                self.assertEqual(actual, expected)
                                if op != 'discard':
                                    self.assertEqual(buf.raw[1:1 + expected], model[:expected])
                                if op != 'peek':
                                    del model[:expected]
                            self.assertEqual(dll.tuya_ring_buff_used_size_get(ring), len(model))
                            self.assertEqual(dll.tuya_ring_buff_free_size_get(ring), capacity - 1 - len(model))
                        self.assertEqual(dll.tuya_ring_buff_free(ring), 0)
                        if enabled and kind == 2:
                            self.assertEqual(C.c_int.in_dll(dll, 'psram_allocs').value, 1)
                            self.assertEqual(C.c_int.in_dll(dll, 'psram_frees').value, 1)
                if enabled:
                    C.c_int.in_dll(dll, 'fail_alloc').value = 1
                    ring = C.c_void_p()
                    self.assertEqual(create(9, 2, C.byref(ring)), -3)
                    C.c_int.in_dll(dll, 'fail_alloc').value = 0


if __name__ == '__main__':
    unittest.main()
