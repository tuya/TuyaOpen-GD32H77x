/**
 * @file vw553_device.c
 * @brief The single VW553 transport owner and subscriber fanout.
 */
#include "vw553_device.h"
#include "vw553_log.h"
#include "vw553_uart_binding.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "tuya_error_code.h"

#include <string.h>

#define VW553_DEVICE_MAX_SUBSCRIBERS 8U
#define VW553_RECOVERY_STACK_SIZE 3072U
#define VW553_RECOVERY_PRIORITY (tskIDLE_PRIORITY + 1U)
#define VW553_RESET_MIN_INTERVAL_MS 30000U
#define VW553_RESET_WINDOW_MS 600000U
#define VW553_RESET_MAX_IN_WINDOW 3U

typedef struct {
    vw553_device_event_handler_t handler;
    void *context;
} subscriber_t;

static subscriber_t s_subscribers[VW553_DEVICE_MAX_SUBSCRIBERS];
static bool s_transport_started;
static bool s_transport_start_failed;
static bool s_transport_starting;
static SemaphoreHandle_t s_recovery_signal;
static bool s_recovery_task_started;
static uint32_t s_last_reset_ms;
static uint32_t s_reset_window_start_ms;
static uint32_t s_reset_window_count;
static volatile bool s_isp_mode;

static uint32_t device_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void dispatch(const vw553_device_event_t *event);

static void request_recovery(void)
{
    if (s_recovery_signal) (void)xSemaphoreGive(s_recovery_signal);
}

void vw553_device_reset(void)
{
    VW553_LOG("hardware reset start");
    rcu_periph_clock_enable(VW553_RESET_CLOCK);
    gpio_mode_set(VW553_RESET_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, VW553_RESET_PIN);
    gpio_output_options_set(VW553_RESET_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_12MHZ, VW553_RESET_PIN);
    gpio_bit_set(VW553_RESET_PORT, VW553_RESET_PIN);
    vTaskDelay(pdMS_TO_TICKS(10U) ? pdMS_TO_TICKS(10U) : 1U);
    gpio_bit_reset(VW553_RESET_PORT, VW553_RESET_PIN);
    vTaskDelay(pdMS_TO_TICKS(20U) ? pdMS_TO_TICKS(20U) : 1U);
    gpio_bit_set(VW553_RESET_PORT, VW553_RESET_PIN);
    vTaskDelay(pdMS_TO_TICKS(500U));
    VW553_LOG("hardware reset complete");
}

void vw553_device_enter_isp_mode(void)
{
    s_isp_mode = true;
    VW553_LOG("ISP mode enabled; AT traffic blocked");
}

void vw553_device_exit_isp_mode(void)
{
    s_isp_mode = false;
    VW553_LOG("ISP mode disabled");
}

void vw553_device_reset_and_recover(void)
{
    vw553_device_exit_isp_mode();
    vw553_device_reset();
    const vw553_device_event_t event = {.kind = VW553_DEVICE_EVENT_RESET};
    dispatch(&event);
    request_recovery();
}

static void dispatch(const vw553_device_event_t *event)
{
    subscriber_t subscribers[VW553_DEVICE_MAX_SUBSCRIBERS];
    taskENTER_CRITICAL();
    memcpy(subscribers, s_subscribers, sizeof(subscribers));
    taskEXIT_CRITICAL();
    for (size_t i = 0; i < VW553_DEVICE_MAX_SUBSCRIBERS; ++i) {
        if (subscribers[i].handler) {
            subscribers[i].handler(subscribers[i].context, event);
        }
    }
}

static void event_line(void *context, const char *line, size_t length)
{
    (void)context;
    if (length == 5U && memcmp(line, "READY", 5U) == 0) request_recovery();
    const vw553_device_event_t event = {.kind = VW553_DEVICE_EVENT_LINE, .value.line = {line, length}};
    dispatch(&event);
}

static void event_data(void *context, int remote_fd, const uint8_t *data, size_t length, bool final)
{
    (void)context;
    const vw553_device_event_t event = {
        .kind = VW553_DEVICE_EVENT_DATA, .value.data = {remote_fd, data, length, final}};
    dispatch(&event);
}

static void event_ble_data(void *context, uint8_t conn_id, const uint8_t *data, size_t length, bool final)
{
    (void)context;
    const vw553_device_event_t event = {
        .kind = VW553_DEVICE_EVENT_BLE_DATA, .value.ble_data = {conn_id, data, length, final}};
    dispatch(&event);
}

static void event_prompt(void *context)
{
    (void)context;
    const vw553_device_event_t event = {.kind = VW553_DEVICE_EVENT_PROMPT};
    dispatch(&event);
}

static void event_fault(void *context, vw553_at_fault_t fault)
{
    (void)context;
    VW553_LOG("transport fault=%d", (int)fault);
    request_recovery();
    const vw553_device_event_t event = {.kind = VW553_DEVICE_EVENT_FAULT, .value.fault = fault};
    dispatch(&event);
}

static void event_datagram(void *context, int listener_fd, const char *peer_ipv4, uint16_t peer_port,
                           const uint8_t *data, size_t length, bool final)
{
    (void)context;
    const vw553_device_event_t event = {
        .kind = VW553_DEVICE_EVENT_DATAGRAM,
        .value.datagram = {listener_fd, peer_ipv4, peer_port, data, length, final}};
    dispatch(&event);
}

static void event_accept(void *context, int listener_fd, int client_fd, const char *peer_ipv4, uint16_t peer_port)
{
    (void)context;
    const vw553_device_event_t event = {
        .kind = VW553_DEVICE_EVENT_ACCEPT,
        .value.accept = {listener_fd, client_fd, peer_ipv4, peer_port}};
    dispatch(&event);
}

static void event_accept_v2(void *context, uint32_t con_id, uint32_t generation, int listener_fd,
                            uint32_t listener_generation, int lwip_fd, const char *peer_ipv4, uint16_t peer_port)
{
    (void)context;
    const vw553_device_event_t event = {
        .kind = VW553_DEVICE_EVENT_ACCEPT_V2,
        .value.accept_v2 = {con_id, generation, listener_fd, listener_generation, lwip_fd, peer_ipv4, peer_port}};
    dispatch(&event);
}

static void event_close_v2(void *context, uint32_t con_id, int lwip_fd, uint32_t generation, const char *reason)
{
    (void)context;
    const vw553_device_event_t event = {
        .kind = VW553_DEVICE_EVENT_CLOSE_V2,
        .value.close_v2 = {con_id, generation, lwip_fd, reason}};
    dispatch(&event);
}

static bool reset_allowed(uint32_t now)
{
    if (s_reset_window_count == 0U || (uint32_t)(now - s_reset_window_start_ms) >= VW553_RESET_WINDOW_MS) {
        s_reset_window_start_ms = now;
        s_reset_window_count = 0U;
    }
    if (s_last_reset_ms != 0U && (uint32_t)(now - s_last_reset_ms) < VW553_RESET_MIN_INTERVAL_MS) return false;
    return s_reset_window_count < VW553_RESET_MAX_IN_WINDOW;
}

static int32_t recover_transport_when_idle(void)
{
    int32_t status = OPRT_RESOURCE_NOT_READY;
    for (unsigned attempt = 0U; attempt < 100U; ++attempt) {
        status = vw553_uart_transport_recover();
        if (status != OPRT_RESOURCE_NOT_READY) break;
        vTaskDelay(pdMS_TO_TICKS(10U) ? pdMS_TO_TICKS(10U) : 1U);
    }
    return status;
}

static bool probe_module(void)
{
    for (unsigned attempt = 0U; attempt < 5U; ++attempt) {
        vw553_at_result_t result = VW553_AT_RESULT_NONE;
        if (vw553_uart_transport_transact("AT", 2U, false, 1000U, &result) == OPRT_OK &&
            result == VW553_AT_RESULT_OK) return true;
        vTaskDelay(pdMS_TO_TICKS(200U) ? pdMS_TO_TICKS(200U) : 1U);
    }
    return false;
}

static void recovery_task(void *argument)
{
    (void)argument;
    for (;;) {
        if (xSemaphoreTake(s_recovery_signal, portMAX_DELAY) != pdTRUE) continue;
        /* Coalesce repeated fault/timeout notifications from one transport loss. */
        while (xSemaphoreTake(s_recovery_signal, 0U) == pdTRUE) {
        }
        vTaskDelay(pdMS_TO_TICKS(50U) ? pdMS_TO_TICKS(50U) : 1U);
        if (s_isp_mode) continue;

        int32_t status = OPRT_COM_ERROR;
        for (unsigned attempt = 0U; attempt < 3U; ++attempt) {
            status = recover_transport_when_idle();
            if (status == OPRT_OK) status = vw553_device_negotiate(10000U);
            if (status == OPRT_OK) break;
            /* A poisoned link usually clears by itself within a few hundred
             * milliseconds. Resetting the module wipes the SoftAP/Wi-Fi state
             * that the application layer will not re-apply, so keep probing
             * before falling back to a destructive reset. */
            if (attempt + 1U < 3U) {
                vTaskDelay(pdMS_TO_TICKS(300U) ? pdMS_TO_TICKS(300U) : 1U);
            }
        }
        if (status == OPRT_OK) {
            VW553_LOG("transport recovered without module reset");
            continue;
        }

        uint32_t now = device_now_ms();
        if (!reset_allowed(now)) {
            VW553_LOG("module reset suppressed by recovery policy");
            continue;
        }
        ++s_reset_window_count;
        s_last_reset_ms = now;
        VW553_LOG("module reset recovery count=%lu", (unsigned long)s_reset_window_count);

        vw553_device_reset();
        status = vw553_uart_transport_recover();
        if (status == OPRT_OK && probe_module()) status = vw553_device_negotiate(10000U);
        else status = OPRT_COM_ERROR;

        const vw553_device_event_t event = {.kind = VW553_DEVICE_EVENT_RESET};
        dispatch(&event);
        if (status == OPRT_OK) VW553_LOG("module reset recovery complete");
        else VW553_LOG("module reset recovery failed status=%ld", (long)status);
    }
}

static void start_recovery_task(void)
{
    if (s_recovery_task_started) return;
    if (!s_recovery_signal) s_recovery_signal = xSemaphoreCreateBinary();
    if (s_recovery_signal && xTaskCreate(recovery_task, "553_recovery", VW553_RECOVERY_STACK_SIZE, NULL,
                                        VW553_RECOVERY_PRIORITY, NULL) == pdPASS) {
        s_recovery_task_started = true;
    } else {
        VW553_LOG("recovery task start failed");
    }
}

int32_t vw553_device_subscribe(vw553_device_event_handler_t handler, void *context)
{
    if (!handler) {
        return OPRT_INVALID_PARM;
    }
    taskENTER_CRITICAL();
    for (size_t i = 0; i < VW553_DEVICE_MAX_SUBSCRIBERS; ++i) {
        if (s_subscribers[i].handler == handler && s_subscribers[i].context == context) {
            taskEXIT_CRITICAL();
            return OPRT_OK;
        }
    }
    for (size_t i = 0; i < VW553_DEVICE_MAX_SUBSCRIBERS; ++i) {
        if (!s_subscribers[i].handler) {
            s_subscribers[i] = (subscriber_t){.handler = handler, .context = context};
            taskEXIT_CRITICAL();
            return OPRT_OK;
        }
    }
    taskEXIT_CRITICAL();
    return OPRT_RESOURCE_NOT_READY;
}

int32_t vw553_device_init(void)
{
    bool own_initialization = false;
    for (unsigned attempt = 0U; attempt < 100U; ++attempt) {
        taskENTER_CRITICAL();
        if (s_transport_started) {
            taskEXIT_CRITICAL();
            return OPRT_OK;
        }
        if (s_transport_start_failed) {
            taskEXIT_CRITICAL();
            return OPRT_COM_ERROR;
        }
        if (!s_transport_starting) {
            s_transport_starting = true;
            own_initialization = true;
            taskEXIT_CRITICAL();
            break;
        }
        taskEXIT_CRITICAL();
        vTaskDelay(pdMS_TO_TICKS(1U) ? pdMS_TO_TICKS(1U) : 1U);
    }
    if (!own_initialization) {
        return OPRT_RESOURCE_NOT_READY;
    }
    static const vw553_uart_transport_config_t config = VW553_UART_CONFIG_INITIALIZER;
    static const vw553_uart_transport_events_t events = {
        .line = event_line,
        .data = event_data,
        .ble_data = event_ble_data,
        .prompt = event_prompt,
        .fault = event_fault,
        .datagram = event_datagram,
        .accept = event_accept,
        .accept_v2 = event_accept_v2,
        .close_v2 = event_close_v2,
    };
    int32_t status = vw553_uart_transport_init(&config, &events, NULL);
    if (status == OPRT_OK) start_recovery_task();
    taskENTER_CRITICAL();
    s_transport_starting = false;
    if (status == OPRT_OK) {
        s_transport_started = true;
    } else {
        s_transport_start_failed = true;
    }
    taskEXIT_CRITICAL();
    return status;
}

int32_t vw553_device_negotiate(uint32_t timeout_ms)
{
    if (s_isp_mode) return OPRT_RESOURCE_NOT_READY;
    int32_t status = vw553_device_init();
    if (status != OPRT_OK) return status;
    status = vw553_uart_transport_negotiate(timeout_ms);
    if (status != OPRT_OK && vw553_uart_transport_is_poisoned()) request_recovery();
    return status;
}

int32_t vw553_device_execute(const vw553_uart_request_t *request, vw553_at_result_t *result)
{
    if (s_isp_mode) return OPRT_RESOURCE_NOT_READY;
    int32_t status = vw553_device_init();
    if (status != OPRT_OK) return status;
    status = vw553_uart_transport_execute(request, result);
    if (status != OPRT_OK && vw553_uart_transport_is_poisoned()) request_recovery();
    return status;
}

int32_t vw553_device_wait_events(uint32_t watermark, uint32_t timeout_ms)
{
    return vw553_uart_transport_wait_events(watermark, timeout_ms);
}

int32_t vw553_device_get_capabilities(vw553_uart_capabilities_t *capabilities)
{
    return vw553_uart_transport_get_capabilities(capabilities);
}
