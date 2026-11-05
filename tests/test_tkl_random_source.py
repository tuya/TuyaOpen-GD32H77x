#!/usr/bin/env python3
"""Compile and exercise the actual random adapter functions from tkl_system.c."""
import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
SOURCE = ROOT / "platform/GD32H77X/tuyaos/tuyaos_adapter/src/system/tkl_system.c"


def extract_function(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for pos in range(opening, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start:pos + 1]
    raise AssertionError(f"unterminated function: {signature}")


HARNESS = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>
#define OPRT_OK 0
#define OPRT_INVALID_PARM -2
#define OPRT_NOT_SUPPORTED -3
#define OPRT_COM_ERROR -4
#define OPRT_TIMEOUT -5
#define OPRT_RESOURCE_NOT_READY -17
typedef int OPERATE_RET;
typedef enum {
    GD32H77X_STATUS_OK = 0,
    GD32H77X_STATUS_INVALID_PARAM,
    GD32H77X_STATUS_NOT_READY,
    GD32H77X_STATUS_UNSUPPORTED,
    GD32H77X_STATUS_IO_ERROR,
    GD32H77X_STATUS_TIMEOUT,
} gd32h77x_status_t;
#define GD32H77X_ENTROPY_STAGE_CONTINUOUS_TEST 5U
#define GD32H77X_ENTROPY_BUSY_RETRIES 16U
typedef struct { int32_t error; uint32_t stage, trng_stat, trng_ctl, trng_htcfg, rcu_addctl0, rcu_ctl, trng_clock_enabled, trng_registers_valid, last_ready_stat, snapshot_available; } gd32h77x_entropy_diagnostics_t;
typedef char gd32h77x_random_requires_32bit_int[(sizeof(int) == sizeof(uint32_t)) ? 1 : -1];
static uint32_t entropy_words[64];
static gd32h77x_status_t entropy_status[64];
static unsigned int entropy_count, entropy_pos, sleep_count, panic_count, ipsr;
static char panic_message[256];
static jmp_buf panic_jump;
gd32h77x_status_t gd32h77x_entropy_read(uint8_t *dst, size_t len) {
    unsigned int i = entropy_pos++;
    entropy_count++;
    if (entropy_status[i] != GD32H77X_STATUS_OK) return entropy_status[i];
    assert(len == sizeof(uint32_t));
    memcpy(dst, &entropy_words[i], len);
    return GD32H77X_STATUS_OK;
}
void tkl_system_sleep(uint32_t ms) { assert(ms == 1U); sleep_count++; }
uint32_t __get_IPSR(void) { return ipsr; }
void gd32h77x_entropy_get_diagnostics(gd32h77x_entropy_diagnostics_t *diagnostics) { memset(diagnostics, 0, sizeof(*diagnostics)); }
void gd32h77x_platform_panic(const char *why) { snprintf(panic_message, sizeof(panic_message), "%s", why); panic_count++; longjmp(panic_jump, 1); }
static int gd32h77x_random_as_int(uint32_t value) { int result; memcpy(&result, &value, sizeof(result)); return result; }
'''


def main() -> None:
    source = SOURCE.read_text()
    functions = "\n".join([
        extract_function(source, "static OPERATE_RET gd32h77x_native_status_to_oprt("),
        extract_function(source, "static OPERATE_RET gd32h77x_random_sample("),
        extract_function(source, "int tkl_system_get_random("),
    ])
    # The production int bit-copy helper is included verbatim too.
    helper = extract_function(source, "static int gd32h77x_random_as_int(")
    harness = HARNESS.replace(
        "static int gd32h77x_random_as_int(uint32_t value) { int result; memcpy(&result, &value, sizeof(result)); return result; }",
        helper,
    )
    tests = r'''
int main(void) {
    entropy_words[0] = UINT32_MAX; assert(tkl_system_get_random(0) == 255); assert(entropy_count == 1);
    entropy_pos = entropy_count = 0; entropy_words[0] = UINT32_MAX; assert(tkl_system_get_random(1) == 0); assert(entropy_count == 0);
    entropy_pos = entropy_count = 0; entropy_words[0] = UINT32_C(0x80000000);
    assert((uint32_t)tkl_system_get_random(UINT32_MAX) == UINT32_C(0x80000000));
    entropy_pos = entropy_count = 0; entropy_words[0] = 0; entropy_words[1] = 4;
    assert(tkl_system_get_random(3) == 1); assert(entropy_count == 2);
    entropy_pos = entropy_count = sleep_count = 0; entropy_status[0] = GD32H77X_STATUS_NOT_READY;
    entropy_words[1] = 7; assert(tkl_system_get_random(8) == 7); assert(sleep_count == 1);
    memset(entropy_status, 0, sizeof(entropy_status)); entropy_pos = entropy_count = sleep_count = panic_count = 0;
    for (unsigned int i = 0; i < 16; ++i) entropy_status[i] = GD32H77X_STATUS_NOT_READY;
    ipsr = 1; if (setjmp(panic_jump) == 0) { (void)tkl_system_get_random(8); assert(0); }
    assert(entropy_count == 16 && sleep_count == 0 && panic_count == 1);
    assert(strstr(panic_message, "TRNG failure class=busy") != NULL);
    assert(strstr(panic_message, "STAT=") != NULL && strstr(panic_message, "ready=") != NULL);
    memset(entropy_status, 0, sizeof(entropy_status)); entropy_pos = entropy_count = panic_count = 0; ipsr = 0;
    entropy_status[0] = GD32H77X_STATUS_UNSUPPORTED; if (setjmp(panic_jump) == 0) { (void)tkl_system_get_random(8); assert(0); }
    assert(entropy_count == 1 && panic_count == 1);
    assert(strstr(panic_message, "TRNG failure class=source-error") != NULL);
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        cfile = pathlib.Path(tmp) / "test.c"
        binary = pathlib.Path(tmp) / "test"
        cfile.write_text(harness + "\n" + functions + "\n" + tests)
        subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", str(cfile), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
