/**
 * @file board_diag.c
 * @brief Native GD32H77X FreeRTOS, NOR and AT-link diagnostics.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "gd32h77x_78x.h"
#include "gd32h77x_config.h"
#include "gd32h77x_console.h"
#include "gd32h77x_memory_layout.h"
#include "gd32h77x_nor.h"
#include "at_commands.h"
#include "at_transport.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "stream_buffer.h"
#include "task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DIAG_SYNC_TIMEOUT_MS 50U
#define DIAG_THREAD_STACK_WORDS 512U
#define DIAG_THREAD_PRIORITY 2U
#define DIAG_AT_TIMEOUT_MS 3000U
#define DIAG_AT_REPLY_SIZE 1024U
#define DIAG_NOR_PROBE_BYTES 32U

typedef struct {
    uint32_t checks;
    uint32_t failures;
} diag_result_t;

typedef struct {
    SemaphoreHandle_t done;
    volatile bool priority_ok;
    volatile bool watermark_ok;
} diag_thread_context_t;

static at_transport_t *s_at_transport;
static StreamBufferHandle_t s_at_rx_stream;

static void diag_check(diag_result_t *result, bool passed)
{
    ++result->checks;
    if (!passed) {
        ++result->failures;
    }
}

static void diag_emit(const char *name, const diag_result_t *result)
{
    gd32h77x_console_printf("DIAG_RESULT %s %s checks=%lu failed=%lu\r\n", name,
                            result->failures == 0U ? "PASS" : "FAIL",
                            (unsigned long)result->checks,
                            (unsigned long)result->failures);
}

static void diag_test_info(diag_result_t *result)
{
    gd32h77x_console_printf("tick_ms=%lu free_heap=%lu min_free_heap=%lu\r\n",
                            (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS),
                            (unsigned long)xPortGetFreeHeapSize(),
                            (unsigned long)xPortGetMinimumEverFreeHeapSize());
    diag_check(result, xPortGetFreeHeapSize() > 0U);
}

static void diag_test_tick(diag_result_t *result)
{
    const TickType_t before = xTaskGetTickCount();
    vTaskDelay(pdMS_TO_TICKS(20U));
    const TickType_t elapsed = (xTaskGetTickCount() - before) * portTICK_PERIOD_MS;
    gd32h77x_console_printf("sleep_ms=20 measured_ms=%lu\r\n", (unsigned long)elapsed);
    diag_check(result, elapsed >= 10U && elapsed <= 1000U);
}

static void diag_test_heap(diag_result_t *result)
{
    gd32h77x_console_printf("free_heap=%lu\r\n", (unsigned long)xPortGetFreeHeapSize());
    diag_check(result, xPortGetFreeHeapSize() > 0U);
}

static void diag_test_memory(diag_result_t *result)
{
    uint8_t *memory = pvPortMalloc(64U);
    diag_check(result, memory != NULL);
    if (memory == NULL) {
        return;
    }
    memset(memory, 0xA5, 64U);
    bool preserved = true;
    for (size_t index = 0U; index < 64U; ++index) {
        preserved = preserved && memory[index] == 0xA5U;
    }
    diag_check(result, preserved);
    vPortFree(memory);
}

static void diag_test_queue(diag_result_t *result)
{
    QueueHandle_t queue = xQueueCreate(2U, sizeof(uint32_t));
    diag_check(result, queue != NULL);
    if (queue == NULL) {
        return;
    }

    const uint32_t first = UINT32_C(0x13579BDF);
    const uint32_t second = UINT32_C(0x2468ACE0);
    const uint32_t third = UINT32_C(0x55AA33CC);
    uint32_t received = 0U;
    diag_check(result, xQueueSend(queue, &first, 0U) == pdPASS);
    diag_check(result, xQueueSend(queue, &second, 0U) == pdPASS);
    diag_check(result, xQueueSend(queue, &third, 0U) != pdPASS);
    diag_check(result, xQueueReceive(queue, &received, 0U) == pdPASS && received == first);
    diag_check(result, xQueueReceive(queue, &received, 0U) == pdPASS && received == second);
    diag_check(result, xQueueReceive(queue, &received, pdMS_TO_TICKS(DIAG_SYNC_TIMEOUT_MS)) != pdPASS);
    vQueueDelete(queue);
}

static void diag_test_semaphore(diag_result_t *result)
{
    SemaphoreHandle_t semaphore = xSemaphoreCreateBinary();
    diag_check(result, semaphore != NULL);
    if (semaphore == NULL) {
        return;
    }
    diag_check(result, xSemaphoreTake(semaphore, pdMS_TO_TICKS(DIAG_SYNC_TIMEOUT_MS)) != pdTRUE);
    diag_check(result, xSemaphoreGive(semaphore) == pdTRUE);
    diag_check(result, xSemaphoreTake(semaphore, pdMS_TO_TICKS(DIAG_SYNC_TIMEOUT_MS)) == pdTRUE);
    vSemaphoreDelete(semaphore);
}

static void diag_test_mutex(diag_result_t *result)
{
    SemaphoreHandle_t mutex = xSemaphoreCreateRecursiveMutex();
    diag_check(result, mutex != NULL);
    if (mutex == NULL) {
        return;
    }
    diag_check(result, xSemaphoreTakeRecursive(mutex, pdMS_TO_TICKS(DIAG_SYNC_TIMEOUT_MS)) == pdTRUE);
    diag_check(result, xSemaphoreTakeRecursive(mutex, pdMS_TO_TICKS(DIAG_SYNC_TIMEOUT_MS)) == pdTRUE);
    diag_check(result, xSemaphoreGiveRecursive(mutex) == pdTRUE);
    diag_check(result, xSemaphoreGiveRecursive(mutex) == pdTRUE);
    vSemaphoreDelete(mutex);
}

static void diag_thread_worker(void *argument)
{
    diag_thread_context_t *context = argument;
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    context->priority_ok = uxTaskPriorityGet(self) == DIAG_THREAD_PRIORITY;
    context->watermark_ok = uxTaskGetStackHighWaterMark(self) <= DIAG_THREAD_STACK_WORDS;
    (void)xSemaphoreGive(context->done);
    vTaskDelete(NULL);
}

static void diag_test_thread(diag_result_t *result)
{
    diag_thread_context_t context = {0};
    context.done = xSemaphoreCreateBinary();
    diag_check(result, context.done != NULL);
    if (context.done == NULL) {
        return;
    }

    TaskHandle_t thread = NULL;
    diag_check(result, xTaskCreate(diag_thread_worker, "diag_worker", DIAG_THREAD_STACK_WORDS,
                                   &context, DIAG_THREAD_PRIORITY, &thread) == pdPASS);
    diag_check(result, thread != NULL);
    if (thread != NULL) {
        diag_check(result, xSemaphoreTake(context.done, pdMS_TO_TICKS(1000U)) == pdTRUE);
        diag_check(result, context.priority_ok);
        diag_check(result, context.watermark_ok);
    }
    vSemaphoreDelete(context.done);
}

static void diag_run_all(diag_result_t *result)
{
    diag_test_info(result);
    diag_test_tick(result);
    diag_test_heap(result);
    diag_test_memory(result);
    diag_test_queue(result);
    diag_test_semaphore(result);
    diag_test_mutex(result);
    diag_test_thread(result);
}

static void diag_info_command(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    diag_result_t result = {0};
    diag_test_info(&result);
    diag_emit("diag_info", &result);
}

static void diag_tick_command(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    diag_result_t result = {0};
    diag_test_tick(&result);
    diag_emit("diag_tick", &result);
}

static void diag_heap_command(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    diag_result_t result = {0};
    diag_test_heap(&result);
    diag_emit("diag_heap", &result);
}

static void diag_memory_command(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    diag_result_t result = {0};
    diag_test_memory(&result);
    diag_emit("diag_memory", &result);
}

static void diag_queue_command(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    diag_result_t result = {0};
    diag_test_queue(&result);
    diag_emit("diag_queue", &result);
}

static void diag_semaphore_command(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    diag_result_t result = {0};
    diag_test_semaphore(&result);
    diag_emit("diag_semaphore", &result);
}

static void diag_mutex_command(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    diag_result_t result = {0};
    diag_test_mutex(&result);
    diag_emit("diag_mutex", &result);
}

static void diag_thread_command(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    diag_result_t result = {0};
    diag_test_thread(&result);
    diag_emit("diag_thread", &result);
}

static void diag_all_command(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    diag_result_t result = {0};
    diag_run_all(&result);
    diag_emit("diag_all", &result);
}

static void diag_echo_command(int argc, char *argv[])
{
    if (argc != 2) {
        gd32h77x_console_printf("Usage: diag_echo <token>\r\n");
        return;
    }
    gd32h77x_console_printf("ECHO %s\r\n", argv[1]);
}

static void at_rx_callback(void *context, const uint8_t *data, size_t length)
{
    (void)context;
    if (!s_at_rx_stream || !data || !length) return;
    BaseType_t wake = pdFALSE;
    if (xPortIsInsideInterrupt()) {
        (void)xStreamBufferSendFromISR(s_at_rx_stream, data, length, &wake);
        portYIELD_FROM_ISR(wake);
    } else {
        (void)xStreamBufferSend(s_at_rx_stream, data, length, 0U);
    }
}

static bool at_transport_init(void)
{
    if (s_at_transport) return true;
    s_at_transport = at_transport_get();
    s_at_rx_stream = xStreamBufferCreate(4096U, 1U);
    if (!s_at_transport || !s_at_rx_stream) return false;
    s_at_transport->rx_callback = at_rx_callback;
    s_at_transport->rx_context = NULL;
    return at_transport_open(s_at_transport) == AT_TRANSPORT_OK;
}

static bool at_query(const char *command, char *reply, size_t capacity)
{
    if (!s_at_transport || command == NULL || reply == NULL || capacity < 2U) {
        return false;
    }
    (void)xStreamBufferReset(s_at_rx_stream);
    char frame[256];
    const int frame_length = snprintf(frame, sizeof(frame), "%s\r\n", command);
    if (frame_length <= 0 || (size_t)frame_length >= sizeof(frame)) return false;
    size_t sent = 0U;
    if (at_transport_write(s_at_transport, (const uint8_t *)frame, (size_t)frame_length,
                           DIAG_AT_TIMEOUT_MS, &sent) != AT_TRANSPORT_OK ||
        sent != (size_t)frame_length) return false;

    size_t length = 0U;
    reply[0] = '\0';
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(DIAG_AT_TIMEOUT_MS);
    while ((int32_t)(deadline - xTaskGetTickCount()) > 0) {
        char chunk[64];
        const size_t received = xStreamBufferReceive(s_at_rx_stream, chunk, sizeof(chunk), 1U);
        for (size_t index = 0U; index < received; ++index) {
            if (length + 1U < capacity) reply[length++] = chunk[index];
            reply[length] = '\0';
        }
        if (strstr(reply, "OK\r\n") != NULL || strstr(reply, "ERROR\r\n") != NULL) {
            return strstr(reply, "ERROR\r\n") == NULL;
        }
    }
    return false;
}

static void at_pulse_reset(void)
{
    rcu_periph_clock_enable(AT_RESET_CLOCK);
    gpio_mode_set(AT_RESET_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, AT_RESET_PIN);
    gpio_output_options_set(AT_RESET_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_12MHZ, AT_RESET_PIN);
    gpio_bit_set(AT_RESET_PORT, AT_RESET_PIN);
    vTaskDelay(pdMS_TO_TICKS(10U));
    gpio_bit_reset(AT_RESET_PORT, AT_RESET_PIN);
    vTaskDelay(pdMS_TO_TICKS(20U));
    gpio_bit_set(AT_RESET_PORT, AT_RESET_PIN);
    vTaskDelay(pdMS_TO_TICKS(800U));
}

static void diag_at_reset_command(int argc, char *argv[])
{
    const bool isp = argc == 2 && strcmp(argv[1], "isp") == 0;
    if (argc > 2 || (argc == 2 && !isp && strcmp(argv[1], "run") != 0)) {
        gd32h77x_console_printf("Usage: diag_at_reset [run|isp]\r\n");
        return;
    }
    at_pulse_reset();
    gd32h77x_console_printf("AT module reset complete mode=%s\r\n", isp ? "isp" : "run");
}

static void diag_at_probe_command(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    char reply[256];
    (void)at_transport_init();
    at_pulse_reset();
    const bool ok = at_query("AT", reply, sizeof(reply));
    gd32h77x_console_printf("AT PROBE %s\r\n%s\r\n", ok ? "PASS" : "FAIL", reply);
}

static void diag_at_net_command(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    static const char *const commands[] = {
        "AT+CWSTATUS", "AT+CWLIF", "AT+CIFSR", AT_WIRE_SERVER_QUERY,
    };
    char reply[DIAG_AT_REPLY_SIZE];
    (void)at_transport_init();
    for (size_t index = 0U; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        const bool ok = at_query(commands[index], reply, sizeof(reply));
        gd32h77x_console_printf("%s %s\r\n%s\r\n", commands[index], ok ? "PASS" : "FAIL", reply);
    }
}

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
    (void)argc;
    (void)argv;
    for (uint32_t index = 0U; index < GD32H77X_LAYOUT_DEVICE_COUNT; ++index) {
        const gd32h77x_layout_device_t *device = &gd32h77x_layout_devices[index];
        if (device->access != GD32H77X_LAYOUT_ACCESS_INDIRECT || device->region == NULL ||
            strcmp(device->region, "NOR") != 0 ||
            device->backend_status != GD32H77X_LAYOUT_BACKEND_IMPLEMENTED) {
            continue;
        }
        uint32_t jedec = 0U;
        uint8_t sample[DIAG_NOR_PROBE_BYTES] = {0};
        const bool ok = gd32h77x_nor_init(index) == 0 &&
                        gd32h77x_nor_get_jedec_id(index, &jedec) == 0 &&
                        gd32h77x_nor_read(index, 0U, sample, sizeof(sample)) == 0;
        gd32h77x_console_printf("NOR_PROBE %s device=%s jedec=0x%06lX hash=0x%08lX\r\n",
                                ok ? "PASS" : "FAIL", device->id, (unsigned long)jedec,
                                (unsigned long)diag_nor_fnv1a32(sample, sizeof(sample)));
        return;
    }
    gd32h77x_console_printf("NOR_PROBE FAIL no supported NOR binding\r\n");
}

static const gd32h77x_console_command_t s_commands[] = {
    {"diag_info", "Show tick and heap state", diag_info_command},
    {"diag_tick", "Measure a bounded 20 ms delay", diag_tick_command},
    {"diag_heap", "Show FreeRTOS heap", diag_heap_command},
    {"diag_memory", "Test native heap allocation", diag_memory_command},
    {"diag_queue", "Test FreeRTOS queue semantics", diag_queue_command},
    {"diag_semaphore", "Test FreeRTOS semaphore semantics", diag_semaphore_command},
    {"diag_mutex", "Test recursive mutex semantics", diag_mutex_command},
    {"diag_thread", "Test task creation, priority and watermark", diag_thread_command},
    {"diag_all", "Run all bounded native diagnostics", diag_all_command},
    {"diag_echo", "Echo one token literally", diag_echo_command},
    {"diag_nor_probe", "Read-only OSPI NOR JEDEC and sample", diag_nor_probe_command},
    {"diag_at_reset", "Reset AT module: [run|isp]", diag_at_reset_command},
    {"diag_at_probe", "Reset AT module and probe AT", diag_at_probe_command},
    {"diag_at_net", "Query AT module network status", diag_at_net_command},
};

void app_entry(void *argument)
{
    (void)argument;
    gd32h77x_console_printf("GD32H77X native diagnostics\r\n");
    gd32h77x_console_start(s_commands, sizeof(s_commands) / sizeof(s_commands[0]));
    vTaskDelete(NULL);
}
