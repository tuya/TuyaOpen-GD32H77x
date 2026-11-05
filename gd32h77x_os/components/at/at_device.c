/**
 * @file at_device.c
 * @brief The single AT transport owner and subscriber fanout.
 */
#include "at_device.h"
#include "at_log.h"
#include "at_module_reset.h"
#include "gd32h77x_78x.h"
#include "gd32h77x_config.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "at_status.h"

#include <string.h>

#define AT_DEVICE_MAX_SUBSCRIBERS 8U
#define AT_RECOVERY_STACK_SIZE 3072U
#define AT_RECOVERY_PRIORITY (tskIDLE_PRIORITY + 1U)
#define AT_RESET_MIN_INTERVAL_MS 30000U
#define AT_RESET_WINDOW_MS 600000U
#define AT_RESET_MAX_IN_WINDOW 3U
#define AT_RESET_MUTEX_TIMEOUT_MS 30000U
#define AT_READY_TIMEOUT_MS 5000U
#define AT_SOFT_RESET_TIMEOUT_MS 3000U
#define AT_PROBE_TIMEOUT_MS 1000U

typedef struct {
    at_device_event_handler_t handler;
    void *context;
} subscriber_t;

static subscriber_t s_subscribers[AT_DEVICE_MAX_SUBSCRIBERS];
static bool s_transport_started;
static bool s_transport_start_failed;
static bool s_transport_starting;
static SemaphoreHandle_t s_recovery_signal;
static bool s_recovery_task_started;
static bool s_reset_attempted;
static uint32_t s_last_reset_ms;
static uint32_t s_reset_window_start_ms;
static uint32_t s_reset_window_count;
static volatile bool s_isp_mode;
static SemaphoreHandle_t s_module_reset_mutex;
static bool s_module_initialized;
static bool s_module_initialization_failed;
static volatile bool s_module_reset_active;
static volatile bool s_ready_observed;

static uint32_t device_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void dispatch(const at_device_event_t *event);

static void request_recovery(void)
{
    if (s_recovery_signal) (void)xSemaphoreGive(s_recovery_signal);
}

void at_device_reset(void)
{
    AT_LOG("hardware reset start");
    rcu_periph_clock_enable(AT_RESET_CLOCK);
    gpio_mode_set(AT_RESET_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, AT_RESET_PIN);
    gpio_output_options_set(AT_RESET_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_12MHZ, AT_RESET_PIN);
    gpio_bit_set(AT_RESET_PORT, AT_RESET_PIN);
    vTaskDelay(pdMS_TO_TICKS(10U) ? pdMS_TO_TICKS(10U) : 1U);
    gpio_bit_reset(AT_RESET_PORT, AT_RESET_PIN);
    vTaskDelay(pdMS_TO_TICKS(20U) ? pdMS_TO_TICKS(20U) : 1U);
    gpio_bit_set(AT_RESET_PORT, AT_RESET_PIN);
    vTaskDelay(pdMS_TO_TICKS(500U));
    AT_LOG("hardware reset complete");
}

void at_device_enter_isp_mode(void)
{
    s_isp_mode = true;
    AT_LOG("ISP mode enabled; AT traffic blocked");
}

void at_device_exit_isp_mode(void)
{
    s_isp_mode = false;
    AT_LOG("ISP mode disabled");
}

void at_device_reset_and_recover(void)
{
    at_device_exit_isp_mode();
    at_device_reset();
    const at_device_event_t event = {.kind = AT_DEVICE_EVENT_RESET};
    dispatch(&event);
    request_recovery();
}

static void dispatch(const at_device_event_t *event)
{
    subscriber_t subscribers[AT_DEVICE_MAX_SUBSCRIBERS];
    taskENTER_CRITICAL();
    memcpy(subscribers, s_subscribers, sizeof(subscribers));
    taskEXIT_CRITICAL();
    for (size_t i = 0; i < AT_DEVICE_MAX_SUBSCRIBERS; ++i) {
        if (subscribers[i].handler) {
            subscribers[i].handler(subscribers[i].context, event);
        }
    }
}

static void event_line(void *context, const char *line, size_t length)
{
    (void)context;
    if (length == 5U && memcmp(line, "READY", 5U) == 0) {
        bool reset_active = false;
        taskENTER_CRITICAL();
        s_ready_observed = true;
        reset_active = s_module_reset_active;
        taskEXIT_CRITICAL();
        AT_SUMMARY("module READY observed reset_active=%d", reset_active ? 1 : 0);
        if (!reset_active) request_recovery();
    }
    const at_device_event_t event = {.kind = AT_DEVICE_EVENT_LINE, .value.line = {line, length}};
    dispatch(&event);
}

static void event_data(void *context, int remote_fd, const uint8_t *data, size_t length, bool final)
{
    (void)context;
    const at_device_event_t event = {
        .kind = AT_DEVICE_EVENT_DATA, .value.data = {remote_fd, data, length, final}};
    dispatch(&event);
}

static void event_ble_data(void *context, uint8_t conn_id, const uint8_t *data, size_t length, bool final)
{
    (void)context;
    const at_device_event_t event = {
        .kind = AT_DEVICE_EVENT_BLE_DATA, .value.ble_data = {conn_id, data, length, final}};
    dispatch(&event);
}

static void event_prompt(void *context)
{
    (void)context;
    const at_device_event_t event = {.kind = AT_DEVICE_EVENT_PROMPT};
    dispatch(&event);
}

static void event_fault(void *context, at_fault_t fault)
{
    (void)context;
    AT_LOG("transport fault=%d", (int)fault);
    bool request = false;
    taskENTER_CRITICAL();
    request = !s_module_reset_active;
    taskEXIT_CRITICAL();
    if (request) request_recovery();
    const at_device_event_t event = {.kind = AT_DEVICE_EVENT_FAULT, .value.fault = fault};
    dispatch(&event);
}

static void event_datagram(void *context, int listener_fd, const char *peer_ipv4, uint16_t peer_port,
                           const uint8_t *data, size_t length, bool final)
{
    (void)context;
    const at_device_event_t event = {
        .kind = AT_DEVICE_EVENT_DATAGRAM,
        .value.datagram = {listener_fd, peer_ipv4, peer_port, data, length, final}};
    dispatch(&event);
}

static void event_accept(void *context, int listener_fd, int client_fd, const char *peer_ipv4, uint16_t peer_port)
{
    (void)context;
    const at_device_event_t event = {
        .kind = AT_DEVICE_EVENT_ACCEPT,
        .value.accept = {listener_fd, client_fd, peer_ipv4, peer_port}};
    dispatch(&event);
}

static void event_accept_v2(void *context, uint32_t con_id, uint32_t generation, int listener_fd,
                            uint32_t listener_generation, int lwip_fd, const char *peer_ipv4, uint16_t peer_port)
{
    (void)context;
    const at_device_event_t event = {
        .kind = AT_DEVICE_EVENT_ACCEPT_V2,
        .value.accept_v2 = {con_id, generation, listener_fd, listener_generation, lwip_fd, peer_ipv4, peer_port}};
    dispatch(&event);
}

static void event_close_v2(void *context, uint32_t con_id, int lwip_fd, uint32_t generation, const char *reason)
{
    (void)context;
    const at_device_event_t event = {
        .kind = AT_DEVICE_EVENT_CLOSE_V2,
        .value.close_v2 = {con_id, generation, lwip_fd, reason}};
    dispatch(&event);
}

static bool reset_allowed(uint32_t now)
{
    if (s_reset_window_count == 0U || (uint32_t)(now - s_reset_window_start_ms) >= AT_RESET_WINDOW_MS) {
        s_reset_window_start_ms = now;
        s_reset_window_count = 0U;
    }
    if (s_reset_attempted && (uint32_t)(now - s_last_reset_ms) < AT_RESET_MIN_INTERVAL_MS) return false;
    return s_reset_window_count < AT_RESET_MAX_IN_WINDOW;
}

static void record_reset_attempt(const at_module_reset_result_t *result, uint32_t now)
{
    if (!result->software_reset_requested && !result->hardware_reset_attempted) return;
    if (s_reset_window_count == 0U || (uint32_t)(now - s_reset_window_start_ms) >= AT_RESET_WINDOW_MS) {
        s_reset_window_start_ms = now;
        s_reset_window_count = 0U;
    }
    ++s_reset_window_count;
    s_last_reset_ms = now;
    s_reset_attempted = true;
}

static int32_t recover_transport_when_idle(void)
{
    at_status_t status = AT_STATUS_NOT_READY;
    for (unsigned attempt = 0U; attempt < 100U; ++attempt) {
        status = at_session_recover();
        if (status != AT_STATUS_NOT_READY) break;
        vTaskDelay(pdMS_TO_TICKS(10U) ? pdMS_TO_TICKS(10U) : 1U);
    }
    return status;
}

static bool probe_module_once(void)
{
    at_result_t result = AT_RESULT_NONE;
    at_status_t status = at_session_transact("AT", 2U, false, AT_PROBE_TIMEOUT_MS, &result);
    return status == AT_STATUS_OK && result == AT_RESULT_OK;
}

static bool probe_module_callback(void *context)
{
    (void)context;
    return probe_module_once();
}

static bool request_module_soft_reset(void *context)
{
    (void)context;
    taskENTER_CRITICAL();
    s_ready_observed = false;
    taskEXIT_CRITICAL();
    at_result_t result = AT_RESULT_NONE;
    at_status_t status = at_session_transact("AT+RST", sizeof("AT+RST") - 1U, false,
                                                   AT_SOFT_RESET_TIMEOUT_MS, &result);
    bool accepted = status == AT_STATUS_OK && result == AT_RESULT_OK;
    AT_SUMMARY("module AT+RST %s status=%ld result=%d", accepted ? "accepted" : "failed",
                  (long)status, (int)result);
    return accepted;
}

static bool wait_module_ready(void *context, uint32_t timeout_ms)
{
    (void)context;
    uint32_t start = device_now_ms();
    for (;;) {
        bool ready = false;
        taskENTER_CRITICAL();
        ready = s_ready_observed;
        taskEXIT_CRITICAL();
        if (ready) return true;
        if ((uint32_t)(device_now_ms() - start) >= timeout_ms) return false;
        vTaskDelay(pdMS_TO_TICKS(10U) ? pdMS_TO_TICKS(10U) : 1U);
    }
}

static bool recover_module_transport(void *context)
{
    (void)context;
    at_status_t status = recover_transport_when_idle();
    return status == AT_STATUS_OK;
}

static bool reset_module_hardware(void *context)
{
    (void)context;
    at_device_reset();
    return true;
}

static const at_module_reset_ops_t s_module_reset_ops = {
    .probe = probe_module_callback,
    .request_soft_reset = request_module_soft_reset,
    .wait_ready = wait_module_ready,
    .recover_transport = recover_module_transport,
    .hardware_reset = reset_module_hardware,
};

static int32_t run_module_reset_locked(at_module_reset_mode_t mode)
{
    taskENTER_CRITICAL();
    s_module_reset_active = true;
    s_ready_observed = false;
    taskEXIT_CRITICAL();

    at_module_reset_result_t result = {0};
    bool ok = at_module_reset_run(mode, &s_module_reset_ops, NULL, AT_READY_TIMEOUT_MS, &result);

    taskENTER_CRITICAL();
    s_module_reset_active = false;
    s_ready_observed = false;
    taskEXIT_CRITICAL();

    record_reset_attempt(&result, device_now_ms());
    if (result.software_reset_requested || result.hardware_reset_attempted) {
        const at_device_event_t event = {.kind = AT_DEVICE_EVENT_RESET};
        dispatch(&event);
    }
    if (result.software_reset_requested) AT_SUMMARY("module soft reset complete ok=%d", ok ? 1 : 0);
    if (result.hardware_reset_attempted) AT_SUMMARY("module hardware reset fallback complete ok=%d", ok ? 1 : 0);
    return ok ? AT_STATUS_OK : AT_STATUS_IO_ERROR;
}

static void mark_module_initialized(void)
{
    taskENTER_CRITICAL();
    s_module_initialized = true;
    s_module_initialization_failed = false;
    taskEXIT_CRITICAL();
}

static int32_t ensure_module_startup_reset(void)
{
    if (!s_module_reset_mutex) return AT_STATUS_NOT_READY;
    if (xSemaphoreTake(s_module_reset_mutex, pdMS_TO_TICKS(AT_RESET_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return AT_STATUS_NOT_READY;
    }

    at_status_t status = AT_STATUS_OK;
    bool initialized;
    bool failed;
    taskENTER_CRITICAL();
    initialized = s_module_initialized;
    failed = s_module_initialization_failed;
    taskEXIT_CRITICAL();
    if (!initialized) {
        if (failed) {
            status = AT_STATUS_IO_ERROR;
        } else {
            status = run_module_reset_locked(AT_MODULE_RESET_STARTUP);
            if (status == AT_STATUS_OK) {
                mark_module_initialized();
                AT_SUMMARY("module startup reset complete");
            } else {
                taskENTER_CRITICAL();
                s_module_initialization_failed = true;
                taskEXIT_CRITICAL();
                AT_SUMMARY("module startup reset failed status=%ld", (long)status);
                request_recovery();
            }
        }
    }
    xSemaphoreGive(s_module_reset_mutex);
    return status;
}

static int32_t recover_module_after_fault(void)
{
    if (!s_module_reset_mutex) return AT_STATUS_NOT_READY;
    if (xSemaphoreTake(s_module_reset_mutex, pdMS_TO_TICKS(AT_RESET_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return AT_STATUS_NOT_READY;
    }

    at_status_t status = recover_transport_when_idle();
    bool probed = status == AT_STATUS_OK && probe_module_once();
    if (!probed) {
        uint32_t now = device_now_ms();
        if (!reset_allowed(now)) {
            AT_SUMMARY("module reset suppressed by recovery policy");
            xSemaphoreGive(s_module_reset_mutex);
            return AT_STATUS_IO_ERROR;
        }
        status = run_module_reset_locked(AT_MODULE_RESET_SOFT_FIRST);
        probed = status == AT_STATUS_OK;
    }
    if (probed) {
        mark_module_initialized();
        status = at_session_negotiate(10000U);
    } else {
        status = AT_STATUS_IO_ERROR;
    }
    xSemaphoreGive(s_module_reset_mutex);
    return status;
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

        at_status_t status = recover_module_after_fault();
        if (status == AT_STATUS_OK) AT_SUMMARY("module transport recovery complete");
        else AT_SUMMARY("module transport recovery failed status=%ld", (long)status);
    }
}

static void start_recovery_task(void)
{
    if (!s_module_reset_mutex) s_module_reset_mutex = xSemaphoreCreateMutex();
    if (s_recovery_task_started) return;
    if (!s_recovery_signal) s_recovery_signal = xSemaphoreCreateBinary();
    if (s_module_reset_mutex && s_recovery_signal &&
        xTaskCreate(recovery_task, "553_recovery", AT_RECOVERY_STACK_SIZE, NULL,
                    AT_RECOVERY_PRIORITY, NULL) == pdPASS) {
        s_recovery_task_started = true;
    } else {
        AT_LOG("recovery task start failed");
    }
}

at_status_t at_device_subscribe(at_device_event_handler_t handler, void *context)
{
    if (!handler) {
        return AT_STATUS_INVALID_PARAM;
    }
    taskENTER_CRITICAL();
    for (size_t i = 0; i < AT_DEVICE_MAX_SUBSCRIBERS; ++i) {
        if (s_subscribers[i].handler == handler && s_subscribers[i].context == context) {
            taskEXIT_CRITICAL();
            return AT_STATUS_OK;
        }
    }
    for (size_t i = 0; i < AT_DEVICE_MAX_SUBSCRIBERS; ++i) {
        if (!s_subscribers[i].handler) {
            s_subscribers[i] = (subscriber_t){.handler = handler, .context = context};
            taskEXIT_CRITICAL();
            return AT_STATUS_OK;
        }
    }
    taskEXIT_CRITICAL();
    return AT_STATUS_NOT_READY;
}

at_status_t at_device_init(void)
{
    bool own_initialization = false;
    for (unsigned attempt = 0U; attempt < 100U; ++attempt) {
        taskENTER_CRITICAL();
        if (s_transport_started) {
            taskEXIT_CRITICAL();
            return AT_STATUS_OK;
        }
        if (s_transport_start_failed) {
            taskEXIT_CRITICAL();
            return AT_STATUS_IO_ERROR;
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
        return AT_STATUS_NOT_READY;
    }
    static const at_session_events_t events = {
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
    at_status_t status = at_session_init(&events, NULL);
    if (status == AT_STATUS_OK) start_recovery_task();
    taskENTER_CRITICAL();
    s_transport_starting = false;
    if (status == AT_STATUS_OK) {
        s_transport_started = true;
    } else {
        s_transport_start_failed = true;
    }
    taskEXIT_CRITICAL();
    return status;
}

at_status_t at_device_negotiate(uint32_t timeout_ms)
{
    if (s_isp_mode) return AT_STATUS_NOT_READY;
    at_status_t status = at_device_init();
    if (status != AT_STATUS_OK) return status;
    status = ensure_module_startup_reset();
    if (status != AT_STATUS_OK) return status;
    status = at_session_negotiate(timeout_ms);
    if (status != AT_STATUS_OK && at_session_is_poisoned()) request_recovery();
    return status;
}

at_status_t at_device_execute(const at_request_t *request, at_result_t *result)
{
    if (s_isp_mode) return AT_STATUS_NOT_READY;
    at_status_t status = at_device_init();
    if (status != AT_STATUS_OK) return status;
    status = ensure_module_startup_reset();
    if (status != AT_STATUS_OK) return status;
    status = at_session_execute(request, result);
    if (status != AT_STATUS_OK && at_session_is_poisoned()) request_recovery();
    return status;
}

at_status_t at_device_wait_events(uint32_t watermark, uint32_t timeout_ms)
{
    return at_session_wait_events(watermark, timeout_ms);
}

at_status_t at_device_get_capabilities(at_capabilities_t *capabilities)
{
    return at_session_get_capabilities(capabilities);
}
