/**
 * @file gd32h77x_diag_cli.c
 * @brief Bounded GD32H77x diagnostics exposed through the Tuya serial CLI.
 */

#include "gd32h77x_diag_cli.h"

#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gd32h77x_platform.h"
#include "gd32h77x_cnvm.h"
#include "gd32h77x_entropy.h"
#include "gd32h77x_flash_route.h"
#include "gd32h77x_nor.h"
#include "gd32h77x_storage.h"
#include "vw553_device.h"
#include "tal_cli.h"
#include "tal_log.h"
#include "tal_memory.h"
#include "tal_mutex.h"
#include "tal_queue.h"
#include "tal_semaphore.h"
#include "tal_system.h"
#include "tal_thread.h"
#include "tkl_mutex.h"
#include "tkl_flash.h"
#include "tkl_thread.h"

#define DIAG_THREAD_STACK_BYTES 2048U
#define DIAG_THREAD_TIMEOUT_MS  1000U
#define DIAG_SYNC_TIMEOUT_MS    50U

typedef struct {
    uint32_t checks;
    uint32_t failures;
} diag_result_t;

typedef void (*diag_test_fn_t)(diag_result_t *result);

typedef struct {
    volatile uint32_t priority_ok;
    volatile uint32_t watermark_ok;
    volatile uint32_t done;
    SEM_HANDLE        done_sem;
} diag_thread_context_t;

static bool                  s_cli_initialized;
static bool                  s_commands_registered;
static bool                  s_thread_test_reserved;
static diag_thread_context_t s_thread_context;

static void diag_log_output(const char *text)
{
    if (text) {
        gd32h77x_platform_log_write(text, strlen(text));
    }
}

static void diag_cli_writef(const char *format, ...)
{
    char    buffer[128];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    tal_cli_echo(buffer);
}

static void diag_check(diag_result_t *result, bool passed)
{
    result->checks++;
    if (!passed) {
        result->failures++;
    }
}

static void diag_emit_result(const char *name, const diag_result_t *result)
{
    diag_cli_writef("DIAG_RESULT %s %s checks=%lu failed=%lu", name, result->failures == 0U ? "PASS" : "FAIL",
                    (unsigned long)result->checks, (unsigned long)result->failures);
}

static void diag_test_info(diag_result_t *result)
{
    int      free_heap = tal_system_get_free_heap_size();
    uint32_t tick_ms   = (uint32_t)tal_system_get_millisecond();
    diag_check(result, free_heap >= 0);
    diag_cli_writef("tick_ms=%lu free_heap=%ld", (unsigned long)tick_ms, (long)free_heap);
}

static void diag_test_tick(diag_result_t *result)
{
    SYS_TIME_T before = tal_system_get_millisecond();
    tal_system_sleep(20U);
    SYS_TIME_T after   = tal_system_get_millisecond();
    uint32_t   elapsed = (uint32_t)(after - before);

    diag_check(result, elapsed >= 10U && elapsed <= 1000U);
    diag_cli_writef("sleep_ms=20 measured_ms=%lu", (unsigned long)elapsed);
}

static void diag_test_heap(diag_result_t *result)
{
    int free_heap = tal_system_get_free_heap_size();
    diag_check(result, free_heap > 0);
    diag_cli_writef("free_heap=%ld", (long)free_heap);
}

static void diag_test_memory(diag_result_t *result)
{
    uint8_t *memory = (uint8_t *)tal_calloc(8U, sizeof(uint32_t));
    diag_check(result, memory != NULL);
    if (!memory) {
        return;
    }

    bool zeroed = true;
    for (size_t index = 0; index < 8U * sizeof(uint32_t); index++) {
        zeroed = zeroed && memory[index] == 0U;
    }
    diag_check(result, zeroed);

    for (size_t index = 0; index < 8U * sizeof(uint32_t); index++) {
        memory[index] = (uint8_t)(index ^ 0xA5U);
    }
    uint8_t *grown = (uint8_t *)tal_realloc(memory, 64U);
    diag_check(result, grown != NULL);
    if (grown) {
        bool preserved = true;
        for (size_t index = 0; index < 8U * sizeof(uint32_t); index++) {
            preserved = preserved && grown[index] == (uint8_t)(index ^ 0xA5U);
        }
        diag_check(result, preserved);
        tal_free(grown);
    } else {
        tal_free(memory);
    }

    void *overflow = tal_calloc(SIZE_MAX, 2U);
    diag_check(result, overflow == NULL);
    tal_free(overflow);
}

static void diag_test_queue(diag_result_t *result)
{
    QUEUE_HANDLE queue  = NULL;
    OPERATE_RET  status = tal_queue_create_init(&queue, (int)sizeof(uint32_t), 2);
    diag_check(result, status == OPRT_OK && queue != NULL);
    if (!queue) {
        return;
    }

    uint32_t first    = 0x13579BDFU;
    uint32_t second   = 0x2468ACE0U;
    uint32_t third    = 0x55AA33CCU;
    uint32_t received = 0U;
    diag_check(result, tal_queue_post(queue, &first, 0U) == OPRT_OK);
    diag_check(result, tal_queue_post(queue, &second, 0U) == OPRT_OK);
    diag_check(result, tal_queue_post(queue, &third, 0U) != OPRT_OK);
    diag_check(result, tal_queue_fetch(queue, &received, 0U) == OPRT_OK && received == first);
    diag_check(result, tal_queue_fetch(queue, &received, 0U) == OPRT_OK && received == second);
    diag_check(result, tal_queue_fetch(queue, &received, DIAG_SYNC_TIMEOUT_MS) != OPRT_OK);
    tal_queue_free(queue);
}

static void diag_test_semaphore(diag_result_t *result)
{
    SEM_HANDLE  semaphore = NULL;
    OPERATE_RET status    = tal_semaphore_create_init(&semaphore, 0U, 1U);
    diag_check(result, status == OPRT_OK && semaphore != NULL);
    if (!semaphore) {
        return;
    }

    diag_check(result, tal_semaphore_wait(semaphore, DIAG_SYNC_TIMEOUT_MS) != OPRT_OK);
    diag_check(result, tal_semaphore_post(semaphore) == OPRT_OK);
    diag_check(result, tal_semaphore_wait(semaphore, DIAG_SYNC_TIMEOUT_MS) == OPRT_OK);
    diag_check(result, tal_semaphore_wait(semaphore, DIAG_SYNC_TIMEOUT_MS) != OPRT_OK);
    tal_semaphore_release(semaphore);
}

static void diag_test_mutex(diag_result_t *result)
{
    TKL_MUTEX_HANDLE mutex  = NULL;
    OPERATE_RET      status = tkl_mutex_create_init(&mutex);
    diag_check(result, status == OPRT_OK && mutex != NULL);
    if (!mutex) {
        return;
    }

    bool first_lock = tkl_mutex_trylock(mutex) == OPRT_OK;
    diag_check(result, first_lock);
    bool recursive_lock = first_lock && tkl_mutex_trylock(mutex) == OPRT_OK;
    diag_check(result, recursive_lock);
    if (recursive_lock) {
        diag_check(result, tkl_mutex_unlock(mutex) == OPRT_OK);
    }
    if (first_lock) {
        diag_check(result, tkl_mutex_unlock(mutex) == OPRT_OK);
    }
    diag_check(result, tkl_mutex_release(mutex) == OPRT_OK);
}

static void diag_thread_worker(void *argument)
{
    diag_thread_context_t *context   = (diag_thread_context_t *)argument;
    TKL_THREAD_HANDLE      self      = NULL;
    uint32_t               watermark = 0U;
    int                    priority  = -1;

    bool have_id = tkl_thread_get_id(&self) == OPRT_OK && self != NULL;
    context->priority_ok =
        have_id && tkl_thread_get_priority(self, &priority) == OPRT_OK && priority == (int)THREAD_PRIO_2;
    context->watermark_ok = have_id && tkl_thread_get_watermark(self, &watermark) == OPRT_OK && watermark > 0U &&
                            watermark <= DIAG_THREAD_STACK_BYTES;
    context->done         = 1U;
    (void)tal_semaphore_post(context->done_sem);
}

static void diag_test_thread(diag_result_t *result)
{
    if (s_thread_test_reserved) {
        diag_check(result, false);
        return;
    }
    s_thread_test_reserved = true;
    memset(&s_thread_context, 0, sizeof(s_thread_context));

    OPERATE_RET status = tal_semaphore_create_init(&s_thread_context.done_sem, 0U, 1U);
    diag_check(result, status == OPRT_OK && s_thread_context.done_sem != NULL);
    if (!s_thread_context.done_sem) {
        s_thread_test_reserved = false;
        return;
    }

    THREAD_HANDLE thread = NULL;
    THREAD_CFG_T  config = {0};
    config.stackDepth    = DIAG_THREAD_STACK_BYTES;
    config.priority      = THREAD_PRIO_2;
    config.thrdname      = "diag_worker";
    status = tal_thread_create_and_start(&thread, NULL, NULL, diag_thread_worker, &s_thread_context, &config);
    diag_check(result, status == OPRT_OK && thread != NULL);
    if (status != OPRT_OK || !thread) {
        tal_semaphore_release(s_thread_context.done_sem);
        s_thread_context.done_sem = NULL;
        s_thread_test_reserved    = false;
        return;
    }

    bool completed =
        tal_semaphore_wait(s_thread_context.done_sem, DIAG_THREAD_TIMEOUT_MS) == OPRT_OK && s_thread_context.done == 1U;
    diag_check(result, completed);
    if (!completed) {
        /* Keep static worker state and semaphore alive if a delayed worker may still use them. */
        return;
    }

    diag_check(result, s_thread_context.priority_ok != 0U);
    diag_check(result, s_thread_context.watermark_ok != 0U);
    tal_semaphore_release(s_thread_context.done_sem);
    s_thread_context.done_sem = NULL;
    s_thread_test_reserved    = false;
}

static void diag_run_named(const char *name, diag_test_fn_t test)
{
    diag_result_t result = {0};
    test(&result);
    diag_emit_result(name, &result);
}

#define DIAG_SIMPLE_HANDLER(_handler_name, _test_name, _test_fn)                                                       \
    static void _handler_name(int argc, char *argv[])                                                                  \
    {                                                                                                                  \
        (void)argv;                                                                                                    \
        if (argc != 1) {                                                                                               \
            diag_result_t invalid = {1U, 1U};                                                                          \
            diag_emit_result(_test_name, &invalid);                                                                    \
            return;                                                                                                    \
        }                                                                                                              \
        diag_run_named(_test_name, _test_fn);                                                                          \
    }

DIAG_SIMPLE_HANDLER(diag_info_command, "diag_info", diag_test_info)
DIAG_SIMPLE_HANDLER(diag_tick_command, "diag_tick", diag_test_tick)
DIAG_SIMPLE_HANDLER(diag_heap_command, "diag_heap", diag_test_heap)
DIAG_SIMPLE_HANDLER(diag_memory_command, "diag_memory", diag_test_memory)
DIAG_SIMPLE_HANDLER(diag_queue_command, "diag_queue", diag_test_queue)
DIAG_SIMPLE_HANDLER(diag_semaphore_command, "diag_semaphore", diag_test_semaphore)
DIAG_SIMPLE_HANDLER(diag_mutex_command, "diag_mutex", diag_test_mutex)
DIAG_SIMPLE_HANDLER(diag_thread_command, "diag_thread", diag_test_thread)

static void diag_all_command(int argc, char *argv[])
{
    static const struct {
        const char    *name;
        diag_test_fn_t test;
    } tests[] = {
        {"info", diag_test_info},     {"tick", diag_test_tick},     {"heap", diag_test_heap},
        {"memory", diag_test_memory}, {"queue", diag_test_queue},   {"semaphore", diag_test_semaphore},
        {"mutex", diag_test_mutex},   {"thread", diag_test_thread},
    };
    diag_result_t total = {0};
    (void)argv;

    if (argc != 1) {
        diag_result_t invalid = {1U, 1U};
        diag_emit_result("diag_all", &invalid);
        return;
    }

    for (size_t index = 0U; index < sizeof(tests) / sizeof(tests[0]); index++) {
        diag_result_t current = {0};
        tests[index].test(&current);
        total.checks += current.checks;
        total.failures += current.failures;
    }
    diag_emit_result("diag_all", &total);
}

static void diag_echo_command(int argc, char *argv[])
{
    if (argc != 2 || !argv[1]) {
        tal_cli_echo("Usage: diag_echo <single-token>");
        return;
    }
    /* tal_cli_echo writes bytes directly; user text is never a format string. */
    tal_cli_echo(argv[1]);
}

static bool diag_kv_raw_layout_valid(const gd32h77x_layout_partition_t *partition)
{
    if (partition == NULL || partition->size_bytes < partition->erase_bytes ||
        partition->logical_base != GD32H77X_LAYOUT_TUYA_LOGICAL_BASE ||
        partition->size_bytes != GD32H77X_LAYOUT_TUYA_SIZE_BYTES ||
        partition->erase_bytes != GD32H77X_LAYOUT_TUYA_ERASE_BYTES ||
        (GD32H77X_LAYOUT_TUYA_LOGICAL_BASE % GD32H77X_LAYOUT_TUYA_ERASE_BYTES) != 0U ||
        partition->segment_count == 0U ||
        partition->first_segment >= GD32H77X_LAYOUT_SEGMENT_COUNT) {
        return false;
    }

    const gd32h77x_layout_segment_t *segment =
        &gd32h77x_layout_segments[partition->first_segment];
    if (segment->logical_offset_bytes != 0U ||
        segment->size_bytes < GD32H77X_LAYOUT_TUYA_ERASE_BYTES ||
        segment->device_index >= GD32H77X_LAYOUT_DEVICE_COUNT) {
        return false;
    }

    const gd32h77x_layout_device_t *device =
        &gd32h77x_layout_devices[segment->device_index];
    const uint32_t program_unit =
        device->program_bytes != 0U ? device->program_bytes : device->page_bytes;
    return device->region != NULL &&
           (strcmp(device->region, "CNVM") == 0 || strcmp(device->region, "NOR") == 0) &&
           device->backend_status == GD32H77X_LAYOUT_BACKEND_IMPLEMENTED &&
           device->erase_bytes == GD32H77X_LAYOUT_TUYA_ERASE_BYTES &&
           program_unit != 0U &&
           partition->program_bytes == device->program_bytes &&
           (segment->device_offset_bytes % GD32H77X_LAYOUT_TUYA_ERASE_BYTES) == 0U &&
           (uint64_t)segment->device_offset_bytes + GD32H77X_LAYOUT_TUYA_ERASE_BYTES <=
               device->capacity_bytes;
}

static void diag_kv_raw_test_command(int argc, char *argv[])
{
    static const uint8_t pattern[256U] = {
        0xA5U, 0x5AU, 0xC3U, 0x3CU, 0xA5U, 0x5AU, 0xC3U, 0x3CU,
        0xA5U, 0x5AU, 0xC3U, 0x3CU, 0xA5U, 0x5AU, 0xC3U, 0x3CU,
        0xA5U, 0x5AU, 0xC3U, 0x3CU, 0xA5U, 0x5AU, 0xC3U, 0x3CU,
        0xA5U, 0x5AU, 0xC3U, 0x3CU, 0xA5U, 0x5AU, 0xC3U, 0x3CU,
        0xA5U, 0x5AU, 0xC3U, 0x3CU, 0xA5U, 0x5AU, 0xC3U, 0x3CU,
        0xA5U, 0x5AU, 0xC3U, 0x3CU, 0xA5U, 0x5AU, 0xC3U, 0x3CU,
        0xA5U, 0x5AU, 0xC3U, 0x3CU, 0xA5U, 0x5AU, 0xC3U, 0x3CU,
        0xA5U, 0x5AU, 0xC3U, 0x3CU, 0xA5U, 0x5AU, 0xC3U, 0x3CU,
        0xA5U, 0x5AU, 0xC3U, 0x3CU, 0xA5U, 0x5AU, 0xC3U, 0x3CU,
        0xA5U, 0x5AU, 0xC3U, 0x3CU, 0xA5U, 0x5AU, 0xC3U, 0x3CU,
        0xA5U, 0x5AU, 0xC3U, 0x3CU, 0xA5U, 0x5AU, 0xC3U, 0x3CU,
        0xA5U, 0x5AU, 0xC3U, 0x3CU, 0xA5U, 0x5AU, 0xC3U, 0x3CU,
    };
    uint8_t data[sizeof(pattern)];
    const gd32h77x_layout_partition_t *partition =
        &gd32h77x_layout_partitions[GD32H77X_LAYOUT_PART_TUYA];
    const uint32_t address = GD32H77X_LAYOUT_TUYA_LOGICAL_BASE;
    const uint32_t sector_size = GD32H77X_LAYOUT_TUYA_ERASE_BYTES;

    if (argc != 2 || argv == NULL || argv[1] == NULL ||
        strcmp(argv[1], "ERASE_KV_SECTOR0") != 0) {
        tal_cli_echo("Usage: diag_kv_raw_test ERASE_KV_SECTOR0 (erases one KV sector)");
        return;
    }

    if (!diag_kv_raw_layout_valid(partition) ||
        !gd32h77x_tuya_range_valid(address, sizeof(data)) ||
        !gd32h77x_tuya_program_range_valid(address, sizeof(pattern)) ||
        !gd32h77x_tuya_erase_range_valid(address, sector_size) ||
        gd32h77x_flash_preflight(address, sizeof(data), GD32H77X_FLASH_OP_READ) !=
            GD32H77X_FLASH_ROUTE_OK ||
        gd32h77x_flash_preflight(address, sizeof(pattern), GD32H77X_FLASH_OP_WRITE) !=
            GD32H77X_FLASH_ROUTE_OK ||
        gd32h77x_flash_preflight(address, sector_size, GD32H77X_FLASH_OP_ERASE) !=
            GD32H77X_FLASH_ROUTE_OK) {
        tal_cli_echo("KV raw test refused: generated KV layout or capability is unsupported");
        return;
    }

    OPERATE_RET result = tkl_flash_erase(address, sector_size);
    diag_cli_writef("KV_RAW erase address=0x%08lx size=%lu ret=%d",
                    (unsigned long)address, (unsigned long)sector_size, (int)result);
    if (result != OPRT_OK) {
        tal_cli_echo("KV may have changed; restore the ROM backup before further testing");
        return;
    }

    memset(data, 0, sizeof(data));
    result = tkl_flash_read(address, data, sizeof(data));
    bool erased = result == OPRT_OK;
    for (size_t index = 0U; erased && index < sizeof(data); ++index) {
        erased = data[index] == 0xFFU;
    }
    diag_cli_writef("KV_RAW erase_read ret=%d all_ff=%u", (int)result, erased ? 1U : 0U);
    if (!erased) {
        tal_cli_echo("KV sector was erased but read check failed; restore the ROM backup");
        return;
    }

    result = tkl_flash_write(address, pattern, sizeof(pattern));
    diag_cli_writef("KV_RAW write address=0x%08lx size=%lu ret=%d",
                    (unsigned long)address, (unsigned long)sizeof(pattern), (int)result);
    if (result != OPRT_OK) {
        tal_cli_echo("KV may have changed; restore the ROM backup before further testing");
        return;
    }

    memset(data, 0, sizeof(data));
    result = tkl_flash_read(address, data, sizeof(data));
    bool matches = result == OPRT_OK && memcmp(data, pattern, sizeof(pattern)) == 0;
    diag_cli_writef("KV_RAW readback ret=%d matches=%u", (int)result, matches ? 1U : 0U);
    if (!matches) {
        tal_cli_echo("KV write verification failed; restore the ROM backup");
        return;
    }

    tal_cli_echo("KV raw TKL test passed; restore the complete ROM backup now");
}

#define DIAG_NOR_PROBE_BYTES 32U

static uint32_t diag_nor_fnv1a32(const uint8_t *data, size_t size)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t index = 0U; index < size; ++index) {
        hash = (hash ^ data[index]) * UINT32_C(16777619);
    }
    return hash;
}

static void diag_nor_probe_command(int argc, char *argv[])
{
    uint32_t device_index;
    uint32_t jedec_id;
    uint8_t sample[DIAG_NOR_PROBE_BYTES];
    bool found = false;

    if (argc != 1 || argv == NULL) {
        tal_cli_echo("Usage: diag_nor_probe (read-only OSPI JEDEC + 32-byte sample)");
        return;
    }
    for (device_index = 0U; device_index < GD32H77X_LAYOUT_DEVICE_COUNT; ++device_index) {
        if (gd32h77x_nor_binding_valid(&gd32h77x_layout_devices[device_index],
                                       &gd32h77x_layout_bindings[device_index])) {
            found = true;
            break;
        }
    }
    if (!found || gd32h77x_nor_get_jedec_id(device_index, &jedec_id) != 0) {
        tal_cli_echo("NOR_PROBE FAIL: no supported NOR binding or JEDEC probe failed");
        return;
    }
    if (gd32h77x_nor_read(device_index, 0U, sample, sizeof(sample)) != 0) {
        tal_cli_echo("NOR_PROBE FAIL: read offset 0 failed");
        return;
    }
    diag_cli_writef("NOR_PROBE PASS device=%s JEDEC=%06lX offset=0 bytes=%lu FNV1A32=%08lX",
                    gd32h77x_layout_devices[device_index].id,
                    (unsigned long)jedec_id, (unsigned long)sizeof(sample),
                    (unsigned long)diag_nor_fnv1a32(sample, sizeof(sample)));
}

static void diag_trng_recover_command(int argc, char *argv[])
{
    if (argc != 1 || argv == NULL) {
        tal_cli_echo("Usage: diag_trng_recover (inject one source error and exercise recovery)");
        return;
    }

    uint8_t sample[GD32H77X_ENTROPY_SAMPLE_SIZE] = {0};
    gd32h77x_entropy_diag_inject_failure_once();
    int status = gd32h77x_entropy_read(sample, sizeof(sample));

    gd32h77x_entropy_diagnostics_t diagnostics = {0};
    gd32h77x_entropy_get_diagnostics(&diagnostics);
    if (status != OPRT_OK || diagnostics.error != OPRT_COM_ERROR ||
        diagnostics.stage != GD32H77X_ENTROPY_STAGE_WAIT_READY) {
        diag_cli_writef("TRNG_RECOVER FAIL status=%d diag=%d stage=%lu", status,
                        diagnostics.error, (unsigned long)diagnostics.stage);
        return;
    }
    diag_cli_writef("TRNG_RECOVER PASS injected=%d recovered=1 sample=%02X%02X%02X%02X diag_stage=%lu",
                    OPRT_COM_ERROR, sample[0], sample[1], sample[2], sample[3],
                    (unsigned long)diagnostics.stage);
}

static void diag_vw553_reset_command(int argc, char *argv[])
{
    if (argc > 2 || argv == NULL || (argc == 2 && strcmp(argv[1], "run") != 0 && strcmp(argv[1], "isp") != 0)) {
        tal_cli_echo("Usage: diag_vw553_reset [run|isp]");
        return;
    }
    bool isp_only = argc == 2 && strcmp(argv[1], "isp") == 0;
    tal_cli_echo(isp_only ? "diag_vw553_reset: ISP pulse (BOOT0 must be high)"
                          : "diag_vw553_reset: reset and recover H77 transport");
    if (isp_only) {
        vw553_device_enter_isp_mode();
        vw553_device_reset();
    } else {
        vw553_device_reset_and_recover();
    }
    tal_cli_echo("diag_vw553_reset: done");
}

static void diag_vw553_query(const char *command)
{
    char reply[512] = {0};
    size_t reply_length = 0U;
    vw553_at_result_t result = VW553_AT_RESULT_NONE;
    vw553_uart_request_t request = {
        .command = command,
        .command_length = strlen(command),
        .reply = reply,
        .reply_capacity = sizeof(reply),
        .reply_length = &reply_length,
        .timeout_ms = 3000U,
    };
    int32_t status = vw553_device_execute(&request, &result);
    if (status != OPRT_OK || result != VW553_AT_RESULT_OK) {
        diag_cli_writef("VW553 QUERY FAIL cmd=%s status=%ld result=%d", command, (long)status, (int)result);
        return;
    }
    diag_cli_writef("VW553 QUERY %s (%lu bytes)", command, (unsigned long)reply_length);
    tal_cli_echo(reply);
}

static void diag_vw553_net_command(int argc, char *argv[])
{
    if (argc != 1 || argv == NULL) {
        tal_cli_echo("Usage: diag_vw553_net (query AP IP and TY553X listeners)");
        return;
    }
    diag_vw553_query("AT+CWSTATUS");
    diag_vw553_query("AT+CWLIF");
    diag_vw553_query("AT+CIFSR");
    diag_vw553_query("AT+TY553XSERVER?");
}

static cli_cmd_t s_diag_commands[] = {
    {.name = "diag_info", .help = "Show tick and free heap", .func = diag_info_command},
    {.name = "diag_tick", .help = "Measure a bounded 20 ms sleep", .func = diag_tick_command},
    {.name = "diag_heap", .help = "Check and show free heap", .func = diag_heap_command},
    {.name = "diag_memory", .help = "Test bounded calloc/realloc behavior", .func = diag_memory_command},
    {.name = "diag_queue", .help = "Test FIFO, full, empty, and timeout", .func = diag_queue_command},
    {.name = "diag_semaphore", .help = "Test semaphore post/wait/timeout", .func = diag_semaphore_command},
    {.name = "diag_mutex", .help = "Test recursive mutex operations", .func = diag_mutex_command},
    {.name = "diag_thread", .help = "Test finite worker, priority, and watermark", .func = diag_thread_command},
    {.name = "diag_all", .help = "Run all bounded diagnostics", .func = diag_all_command},
    {.name = "diag_echo", .help = "Echo one token literally (including %)", .func = diag_echo_command},
    {.name = "diag_kv_raw_test", .help = "Erase one KV sector and test TKL read/write", .func = diag_kv_raw_test_command},
    {.name = "diag_nor_probe", .help = "Read-only OSPI JEDEC and offset-zero sample", .func = diag_nor_probe_command},
    {.name = "diag_trng_recover", .help = "Inject one TRNG error and verify recovery", .func = diag_trng_recover_command},
    {.name = "diag_vw553_reset", .help = "Reset VW553: [run|isp]", .func = diag_vw553_reset_command},
    {.name = "diag_vw553_net", .help = "Query AP IP and TCP/UDP listener state", .func = diag_vw553_net_command},
};

OPERATE_RET gd32h77x_diag_log_init(void)
{
    return tal_log_init(TAL_LOG_LEVEL_INFO, 1024, diag_log_output);
}

OPERATE_RET gd32h77x_diag_cli_init(void)
{
    if (s_cli_initialized) {
        return OPRT_OK;
    }

    OPERATE_RET status = OPRT_OK;
    if (!s_commands_registered) {
        status = tal_cli_cmd_register(s_diag_commands, (uint8_t)(sizeof(s_diag_commands) / sizeof(s_diag_commands[0])));
        if (status != OPRT_OK) {
            return status;
        }
        s_commands_registered = true;
    }

    /* tal_cli_init() also registers the shared sys_* command group, so the
     * board validation image exposes both the platform diag_* commands and the
     * standard TuyaOpen CLI words. */
    status = tal_cli_init();
    if (status == OPRT_OK) {
        s_cli_initialized = true;
    }
    return status;
}
