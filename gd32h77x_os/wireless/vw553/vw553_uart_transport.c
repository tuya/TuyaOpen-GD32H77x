/**
 * @file vw553_uart_transport.c
 * @brief FreeRTOS UART transport for the VW553 AT client.
 */

#include "vw553_uart_transport.h"
#include "vw553_log.h"
#include "vw553_uart_binding.h"
#include "gd32h77x_78x_dma.h"
#if defined(GD32H77X_VW553_AT_BRIDGE) && (GD32H77X_VW553_AT_BRIDGE == 1)
#include "gd32h77x_vw553_bridge.h"
#endif

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "stream_buffer.h"
#include "task.h"
#include "vw553_at_parser.h"
#include "vw553_event_queue.h"
#include "vw553_uart_policy.h"
#include "tuya_error_code.h"

#include <string.h>

#define VW553_RX_STREAM_SIZE       12288U
#define VW553_RX_READ_SIZE         2048U
#define VW553_COMMAND_TX_SIZE      (VW553_AT_COMMAND_SIZE + 2U)
#define VW553_RX_TASK_STACK        1536U
/* Subscriber callbacks can run TAL BLE connection handling and an AT RPC,
 * including newlib formatting, on this task. FreeRTOS sizes are in words. */
#define VW553_EVENT_TASK_STACK     2048U
#define VW553_TASK_PRIORITY        5U
#define VW553_IO_POLL_MS           5U

/* Receive with a circular DMA so byte loss no longer depends on interrupt
 * latency. The UART interrupt is priority 6 while FreeRTOS critical sections
 * mask everything >= 5, and the ISR cannot be raised because it calls FromISR
 * APIs; hardware DMA is the only lossless option. Set to 0 to fall back to the
 * per-byte interrupt path. */
#ifndef VW553_RX_DMA_ENABLE
#define VW553_RX_DMA_ENABLE 1
#endif

#if VW553_RX_DMA_ENABLE
#define VW553_RX_DMA_PERIPH DMA0
#define VW553_RX_DMA_CH     DMA_CH7
#define VW553_RX_DMA_IRQ    DMA0_Channel7_IRQn
/* The module emits one contiguous +IPD frame of up to PASSTH_START_TRANSFER_LEN
 * (2920) payload bytes plus header/trailer. The DMA ring must exceed a whole
 * frame: with the old 512-byte ring a 2.4 KB TLS flight wrapped it 4-5 times
 * and the unread bytes were overwritten before the IDLE/TC drain ran, so the
 * parser saw announced>consumed and dropped the record. */
#define VW553_RX_DMA_SIZE   4096U
_Static_assert(VW553_RX_DMA_SIZE >= 2944U,
               "RX DMA ring must hold one largest +IPD frame (2920-byte payload + header/trailer)");
static uint8_t s_rx_dma_buffer[VW553_RX_DMA_SIZE] __attribute__((aligned(4)));
static volatile uint32_t s_rx_dma_pos;
#endif

/* A frame header that promises more bytes than the module ever delivers would
 * otherwise leave the parser in PAYLOAD forever, blocking every later
 * transaction. Give up when the stream stops feeding the frame. */
#define VW553_FRAME_IDLE_TIMEOUT_MS 500U
/* The stock +IPD frame ends with a bare OK. If a caller queues an AT
 * command exactly between the payload and that trailer, let RX drain the
 * trailer before starting the command instead of failing a valid request. */
#define VW553_REQUEST_FRAME_HOLD_MS (VW553_FRAME_IDLE_TIMEOUT_MS + 100U)

typedef struct {
    char command[VW553_AT_COMMAND_SIZE + 1U];
    size_t command_length;
    uint8_t payload[VW553_UART_MAX_PAYLOAD];
    size_t payload_length;
    bool wait_prompt;
    uint32_t timeout_ms;
    uint32_t started_ms;
    char reply[VW553_UART_REPLY_CAPACITY];
    size_t reply_length;
    bool reply_overflow;
    bool payload_sent;
    bool prompt_seen;
    bool saw_send_ok;
    bool saw_send_fail;
    bool negotiate;
    vw553_uart_capabilities_t capabilities;
    uint32_t event_watermark;
} request_t;

typedef struct {
    volatile bool initialized;
    volatile bool poisoned;
    bool poison_processed;
    volatile bool stopping;
    volatile uint32_t isr_fault;
    vw553_uart_transport_config_t config;
    vw553_uart_transport_events_t events;
    void *event_context;
    StreamBufferHandle_t rx_stream;
    QueueHandle_t request_queue;
    vw553_event_queue_t *event_queue;
    SemaphoreHandle_t caller_mutex;
    SemaphoreHandle_t result_ready;
    SemaphoreHandle_t event_progress;
    SemaphoreHandle_t event_available;
    SemaphoreHandle_t tasks_exited;
    TaskHandle_t rx_task;
    TaskHandle_t event_task;
    vw553_at_client_t client;
    vw553_at_parser_t parser;
    request_t request;
    vw553_at_result_t result;
    bool request_active;
    bool request_started;
    volatile uint32_t event_enqueued;
    volatile uint32_t event_dispatched;
    volatile uint32_t rx_stream_high_water;
    volatile uint32_t rx_dma_burst_high_water;
    volatile uint32_t rx_stream_overflow;
    bool capability_negotiated;
    uint8_t rx_trace[64];
    size_t rx_trace_used;
    uint32_t rx_last_byte_ms;
    vw553_uart_capabilities_t capabilities;
} transport_t;

enum {
    ISR_FAULT_OVERRUN = 1U << 0,
    ISR_FAULT_FRAMING = 1U << 1,
    ISR_FAULT_NOISE = 1U << 2,
    ISR_FAULT_PARITY = 1U << 3,
    ISR_FAULT_RX_OVERFLOW = 1U << 4,
    ISR_FAULT_CALLER_TIMEOUT = 1U << 5,
};

static transport_t s_transport;
static bool s_init_failed_with_live_worker;

static bool transmit_bytes(const uint8_t *bytes, size_t length, uint32_t deadline_ms, size_t *sent);

static bool events_configured(const vw553_uart_transport_events_t *events)
{
    return events && (events->line || events->data || events->prompt || events->fault || events->datagram ||
                      events->accept || events->accept_v2 || events->close_v2);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static bool config_valid(const vw553_uart_transport_config_t *config)
{
    if (!config || !config->usart || !config->baudrate || !config->tx_pin || !config->rx_pin ||
        config->tx_pin == config->rx_pin || config->irq_priority > configLIBRARY_LOWEST_INTERRUPT_PRIORITY ||
        config->irq_priority < configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY ||
        config->word_length != USART_WL_8BIT || config->stop_bits != USART_STB_1BIT ||
        config->parity != USART_PM_NONE) {
        return false;
    }
    return true;
}

static void set_poison(uint32_t fault)
{
    taskENTER_CRITICAL();
    s_transport.isr_fault |= fault;
    s_transport.poisoned = true;
    /* A later fault must be processed even if an earlier poison episode was
     * already handled. Otherwise request_active can stay set forever and the
     * caller mutex is never released. */
    s_transport.poison_processed = false;
    taskEXIT_CRITICAL();
}

static bool enqueue_event(const vw553_queued_event_t *event)
{
    if (!s_transport.events.line && !s_transport.events.data && !s_transport.events.ble_data && !s_transport.events.prompt &&
        !s_transport.events.fault && !s_transport.events.datagram && !s_transport.events.accept &&
        !s_transport.events.accept_v2 && !s_transport.events.close_v2) {
        return true;
    }
    vw553_queued_event_t queued = *event;
    if (!vw553_event_queue_enqueue(s_transport.event_queue, &queued)) {
        VW553_LOG("event queue full kind=%d depth=%u pending=%lu enqueued=%lu dispatched=%lu",
                  (int)event->kind, (unsigned)VW553_EVENT_QUEUE_DEPTH,
                  (unsigned long)vw553_event_queue_pending(s_transport.event_queue),
                  (unsigned long)s_transport.event_enqueued,
                  (unsigned long)s_transport.event_dispatched);
        set_poison(ISR_FAULT_RX_OVERFLOW);
        return false;
    }
    s_transport.event_enqueued = queued.sequence;
    xSemaphoreGive(s_transport.event_available);
    return true;
}

static void client_line(void *context, const char *line, size_t length)
{
    (void)context;
    VW553_LOG("AT LINE active=%d phase=%d len=%lu text=%.*s", s_transport.request_active ? 1 : 0,
              (int)s_transport.client.phase, (unsigned long)length, (int)length, line);
    if (s_transport.request_active && length == sizeof("SEND OK") - 1U &&
        memcmp(line, "SEND OK", length) == 0) {
        s_transport.request.saw_send_ok = true;
    }
    /* The VW553 BLE data-transport command terminates a successful payload
     * transfer with plain OK, unlike the socket CIPSEND family which emits
     * SEND OK followed by OK.  Keep that exception tied to this exact command
     * and only after the prompt payload has been sent. */
    if (s_transport.request_active && s_transport.request.prompt_seen && s_transport.request.payload_sent &&
        length == 2U && memcmp(line, "OK", 2U) == 0 &&
        vw553_uart_policy_ble_payload_uses_plain_ok(s_transport.request.command,
                                                    s_transport.request.command_length)) {
        s_transport.request.saw_send_ok = true;
    }
    if (s_transport.request_active && length == sizeof("SEND FAIL") - 1U &&
        memcmp(line, "SEND FAIL", length) == 0) {
        s_transport.request.saw_send_fail = true;
    }
    if (s_transport.request_active &&
        !vw553_uart_policy_capture_line(s_transport.request.reply, sizeof(s_transport.request.reply),
                                        &s_transport.request.reply_length, line, length)) {
        s_transport.request.reply_overflow = true;
        set_poison(ISR_FAULT_RX_OVERFLOW);
        return;
    }
    char *slot = vw553_event_queue_line_acquire(s_transport.event_queue);
    if (!slot) {
        VW553_SUMMARY("event line slots exhausted len=%lu pending=%lu", (unsigned long)length,
                      (unsigned long)vw553_event_queue_pending(s_transport.event_queue));
        set_poison(ISR_FAULT_RX_OVERFLOW);
        return;
    }
    const uint8_t line_slot = vw553_event_queue_slot_for_line(s_transport.event_queue, slot);
    vw553_queued_event_t event = {.kind = VW553_EVENT_LINE, .length = length, .line_slot = line_slot};
    if (length > VW553_EVENT_LINE_SLOT_SIZE || line_slot == VW553_EVENT_LINE_SLOT_NONE) {
        if (line_slot != VW553_EVENT_LINE_SLOT_NONE) {
            vw553_event_queue_line_release(s_transport.event_queue, line_slot);
        }
        set_poison(ISR_FAULT_RX_OVERFLOW);
        return;
    }
    memcpy(slot, line, length);
    if (!enqueue_event(&event)) {
        vw553_event_queue_line_release(s_transport.event_queue, line_slot);
    }
}

static void client_ipd_trailer_dropped(void *context)
{
    (void)context;
    VW553_SUMMARY("AT stale IPD trailer overtaken by prompt");
}

static bool enqueue_data_event(vw553_event_kind_t kind, int remote_fd, uint8_t conn_id, int listener_fd,
                               const char *peer_ipv4, uint16_t peer_port, const uint8_t *data, size_t length,
                               bool final)
{
    size_t offset = 0U;
    do {
        vw553_queued_event_t event = {.kind = kind,
                                      .remote_fd = remote_fd,
                                      .conn_id = conn_id,
                                      .listener_fd = listener_fd,
                                      .peer_port = peer_port,
                                      .final = false,
                                      .data_slot = VW553_EVENT_DATA_SLOT_NONE,
                                      .line_slot = VW553_EVENT_LINE_SLOT_NONE};
        size_t chunk = length - offset;
        if (chunk > VW553_EVENT_DATA_SLOT_SIZE) {
            chunk = VW553_EVENT_DATA_SLOT_SIZE;
        }
        event.length = chunk;
        if (chunk) {
            uint8_t *slot = vw553_event_queue_data_acquire(s_transport.event_queue);
            if (!slot) {
                VW553_SUMMARY("event data slots exhausted kind=%d len=%lu pending=%lu",
                              (int)kind, (unsigned long)length,
                              (unsigned long)vw553_event_queue_pending(s_transport.event_queue));
                set_poison(ISR_FAULT_RX_OVERFLOW);
                return false;
            }
            event.data_slot = vw553_event_queue_slot_for_data(s_transport.event_queue, slot);
            if (event.data_slot == VW553_EVENT_DATA_SLOT_NONE) {
                set_poison(ISR_FAULT_RX_OVERFLOW);
                return false;
            }
            memcpy(slot, data + offset, chunk);
        }
        offset += chunk;
        event.final = final && offset == length;
        if (peer_ipv4 && kind == VW553_EVENT_DATAGRAM) {
            memcpy(event.peer_ipv4, peer_ipv4, sizeof(event.peer_ipv4));
        }
        if (!enqueue_event(&event)) {
            if (event.data_slot != VW553_EVENT_DATA_SLOT_NONE) {
                vw553_event_queue_data_release(s_transport.event_queue, event.data_slot);
            }
            return false;
        }
    } while (offset < length);
    return true;
}

static void client_data(void *context, int remote_fd, const uint8_t *data, size_t length, bool final)
{
    (void)context;
    VW553_LOG("AT DATA fd=%d len=%lu final=%d first=0x%02x", remote_fd, (unsigned long)length,
              final ? 1 : 0, data && length ? data[0] : 0U);
    (void)enqueue_data_event(VW553_EVENT_DATA, remote_fd, 0U, 0, NULL, 0U, data, length, final);
}

static void client_ble_data(void *context, uint8_t conn_id, const uint8_t *data, size_t length, bool final)
{
    (void)context;
    (void)enqueue_data_event(VW553_EVENT_BLE_DATA, 0, conn_id, 0, NULL, 0U, data, length, final);
}

static void client_prompt(void *context)
{
    (void)context;
    VW553_LOG("AT PROMPT active=%d wait=%d payload=%lu phase=%d", s_transport.request_active ? 1 : 0,
              s_transport.request.wait_prompt ? 1 : 0, (unsigned long)s_transport.request.payload_length,
              (int)s_transport.client.phase);
    if (!s_transport.request_active) {
        /* A CIPSEND prompt can arrive after its transaction timed out. With no
         * active request there is no payload to send, so consume the stale
         * prompt instead of poisoning an otherwise synchronized transport. */
        VW553_SUMMARY("AT stale prompt ignored phase=%d", (int)s_transport.client.phase);
        return;
    }
    if (!vw553_uart_policy_accept_prompt(s_transport.request_active, s_transport.request.wait_prompt,
                                         s_transport.request.payload_length, s_transport.request.prompt_seen,
                                         s_transport.request.payload_sent)) {
        VW553_SUMMARY("AT prompt rejected active=%d wait=%d prompt_seen=%d payload_sent=%d phase=%d",
                      s_transport.request_active ? 1 : 0, s_transport.request.wait_prompt ? 1 : 0,
                      s_transport.request.prompt_seen ? 1 : 0, s_transport.request.payload_sent ? 1 : 0,
                      (int)s_transport.client.phase);
        vw553_at_client_fail(&s_transport.client, VW553_AT_RESULT_PROTOCOL);
        set_poison(ISR_FAULT_NOISE);
        return;
    }
    s_transport.request.prompt_seen = true;
    if (s_transport.request.payload_length) {
        size_t sent = 0U;
        uint32_t deadline = s_transport.request.started_ms + s_transport.request.timeout_ms;
        if (!vw553_uart_policy_payload_valid(true, s_transport.request.payload,
                                             s_transport.request.payload_length, VW553_UART_MAX_PAYLOAD) ||
            !transmit_bytes(s_transport.request.payload, s_transport.request.payload_length, deadline, &sent) ||
            !vw553_uart_policy_tx_complete(s_transport.request.payload_length, sent, true)) {
            VW553_LOG("AT PAYLOAD TX failed sent=%lu/%lu", (unsigned long)sent,
                      (unsigned long)s_transport.request.payload_length);
            vw553_at_client_fail(&s_transport.client, VW553_AT_RESULT_IO);
            set_poison(ISR_FAULT_NOISE);
            return;
        }
        s_transport.request.payload_sent = true;
        VW553_LOG("AT PAYLOAD TX complete sent=%lu", (unsigned long)sent);
        char preview[161];
        size_t take = s_transport.request.payload_length < sizeof(preview) - 1U
                          ? s_transport.request.payload_length
                          : sizeof(preview) - 1U;
        for (size_t i = 0U; i < take; ++i) {
            uint8_t byte = ((const uint8_t *)s_transport.request.payload)[i];
            preview[i] = byte >= 0x20U && byte <= 0x7eU ? (char)byte : '.';
        }
        preview[take] = '\0';
        VW553_LOG("AT PAYLOAD preview=%s", preview);
    }
    const vw553_queued_event_t event = {.kind = VW553_EVENT_PROMPT};
    (void)enqueue_event(&event);
}

static void rx_trace_dump(const char *tag, int fault);

static void client_fault(void *context, vw553_at_fault_t fault)
{
    (void)context;
    rx_trace_dump("RX FAULT", fault);
    const vw553_queued_event_t event = {.kind = VW553_EVENT_FAULT, .fault = fault};
    (void)enqueue_event(&event);
}

void vw553_uart_transport_dump_rx_trace(const char *tag);

static void rx_trace_append(const uint8_t *data, size_t length)
{
    if (!data || !length) return;
    if (length >= sizeof(s_transport.rx_trace)) {
        memcpy(s_transport.rx_trace, data + length - sizeof(s_transport.rx_trace), sizeof(s_transport.rx_trace));
        s_transport.rx_trace_used = sizeof(s_transport.rx_trace);
        return;
    }
    if (s_transport.rx_trace_used + length > sizeof(s_transport.rx_trace)) {
        size_t drop = s_transport.rx_trace_used + length - sizeof(s_transport.rx_trace);
        memmove(s_transport.rx_trace, s_transport.rx_trace + drop, s_transport.rx_trace_used - drop);
        s_transport.rx_trace_used -= drop;
    }
    memcpy(s_transport.rx_trace + s_transport.rx_trace_used, data, length);
    s_transport.rx_trace_used += length;
}

void vw553_uart_transport_dump_rx_trace(const char *tag) { rx_trace_dump(tag, -1); }

/* Keeps the bytes that made the parser fault, so a rejected frame can be
 * identified from the log instead of inferred. */
static void rx_trace_dump(const char *tag, int fault)
{
    char hex[sizeof(s_transport.rx_trace) * 3U + 1U];
    size_t used = 0U;
    for (size_t i = 0U; i < s_transport.rx_trace_used; ++i) {
        int written = snprintf(hex + used, sizeof(hex) - used, i ? " %02x" : "%02x",
                               (unsigned)s_transport.rx_trace[i]);
        if (written <= 0 || (size_t)written >= sizeof(hex) - used) break;
        used += (size_t)written;
    }
    hex[used] = 0;
    VW553_SUMMARY("%s fault=%d len=%u data=%s", tag, fault, (unsigned)s_transport.rx_trace_used, hex);
    s_transport.rx_trace_used = 0U;
}

static void client_datagram(void *context, int listener_fd, const char *peer_ipv4, uint16_t peer_port,
                            const uint8_t *data, size_t length, bool final)
{
    (void)context;
    (void)enqueue_data_event(VW553_EVENT_DATAGRAM, 0, 0U, listener_fd, peer_ipv4, peer_port, data, length, final);
}

static void client_accept(void *context, int listener_fd, int client_fd, const char *peer_ipv4, uint16_t peer_port)
{
    (void)context;
    vw553_queued_event_t event = {
        .kind = VW553_EVENT_ACCEPT, .listener_fd = listener_fd, .client_fd = client_fd, .peer_port = peer_port};
    memcpy(event.peer_ipv4, peer_ipv4, sizeof(event.peer_ipv4));
    (void)enqueue_event(&event);
}

static void client_accept_v2(void *context, uint32_t con_id, uint32_t generation, int listener_fd,
                             uint32_t listener_generation, int lwip_fd, const char *peer_ipv4, uint16_t peer_port)
{
    (void)context;
    VW553_LOG("AT ACCEPT con_id=%lu gen=%lu listener=%d lgen=%lu fd=%d peer=%s:%u",
              (unsigned long)con_id, (unsigned long)generation, listener_fd,
              (unsigned long)listener_generation, lwip_fd, peer_ipv4 ? peer_ipv4 : "", (unsigned)peer_port);
    vw553_queued_event_t event = {.kind = VW553_EVENT_ACCEPT_V2, .con_id = con_id, .generation = generation,
                            .listener_fd = listener_fd, .listener_generation = listener_generation,
                            .lwip_fd = lwip_fd, .peer_port = peer_port};
    memcpy(event.peer_ipv4, peer_ipv4, sizeof(event.peer_ipv4));
    (void)enqueue_event(&event);
}

static void client_close_v2(void *context, uint32_t con_id, int lwip_fd, uint32_t generation, const char *reason)
{
    (void)context;
    VW553_LOG("AT CLOSE con_id=%lu gen=%lu fd=%d reason=%s", (unsigned long)con_id,
              (unsigned long)generation, lwip_fd, reason ? reason : "");
    vw553_queued_event_t event = {.kind = VW553_EVENT_CLOSE_V2, .con_id = con_id, .generation = generation, .lwip_fd = lwip_fd};
    size_t length = reason ? strlen(reason) : 0U;
    if (!length || length >= sizeof(event.reason)) {
        set_poison(ISR_FAULT_NOISE);
        return;
    }
    memcpy(event.reason, reason, length + 1U);
    (void)enqueue_event(&event);
}

static void dispatch_event(const vw553_queued_event_t *event)
{
    switch (event->kind) {
        case VW553_EVENT_LINE:
            if (s_transport.events.line) {
                s_transport.events.line(s_transport.event_context,
                                        vw553_event_queue_line(s_transport.event_queue, event->line_slot),
                                        event->length);
            }
            break;
        case VW553_EVENT_DATA:
            if (s_transport.events.data) {
                s_transport.events.data(s_transport.event_context, event->remote_fd,
                                        vw553_event_queue_data(s_transport.event_queue, event->data_slot),
                                        event->length, event->final);
            }
            break;
        case VW553_EVENT_BLE_DATA:
            if (s_transport.events.ble_data) {
                s_transport.events.ble_data(s_transport.event_context, event->conn_id,
                                            vw553_event_queue_data(s_transport.event_queue, event->data_slot),
                                            event->length, event->final);
            }
            break;
        case VW553_EVENT_PROMPT:
            if (s_transport.events.prompt) {
                s_transport.events.prompt(s_transport.event_context);
            }
            break;
        case VW553_EVENT_FAULT:
            if (s_transport.events.fault) {
                s_transport.events.fault(s_transport.event_context, event->fault);
            }
            break;
        case VW553_EVENT_DATAGRAM:
            if (s_transport.events.datagram) {
                s_transport.events.datagram(s_transport.event_context, event->listener_fd, event->peer_ipv4,
                                            event->peer_port,
                                            vw553_event_queue_data(s_transport.event_queue, event->data_slot),
                                            event->length, event->final);
            }
            break;
        case VW553_EVENT_ACCEPT:
            if (s_transport.events.accept) {
                s_transport.events.accept(s_transport.event_context, event->listener_fd, event->client_fd,
                                          event->peer_ipv4, event->peer_port);
            }
            break;
        case VW553_EVENT_ACCEPT_V2:
            if (s_transport.events.accept_v2) {
                s_transport.events.accept_v2(s_transport.event_context, event->con_id, event->generation,
                                             event->listener_fd, event->listener_generation, event->lwip_fd,
                                             event->peer_ipv4, event->peer_port);
            }
            break;
        case VW553_EVENT_CLOSE_V2:
            if (s_transport.events.close_v2) {
                s_transport.events.close_v2(s_transport.event_context, event->con_id, event->lwip_fd,
                                            event->generation, event->reason);
            }
            break;
        default:
            break;
    }
}

static void event_task(void *argument)
{
    (void)argument;
    vw553_queued_event_t event;
    while (!s_transport.stopping) {
        (void)xSemaphoreTake(s_transport.event_available, pdMS_TO_TICKS(VW553_IO_POLL_MS));
        while (vw553_event_queue_dequeue(s_transport.event_queue, &event)) {
            dispatch_event(&event);
            if ((event.kind == VW553_EVENT_DATA || event.kind == VW553_EVENT_BLE_DATA ||
                 event.kind == VW553_EVENT_DATAGRAM) && event.data_slot != VW553_EVENT_DATA_SLOT_NONE) {
                vw553_event_queue_data_release(s_transport.event_queue, event.data_slot);
            }
            if (event.kind == VW553_EVENT_LINE && event.line_slot != VW553_EVENT_LINE_SLOT_NONE) {
                vw553_event_queue_line_release(s_transport.event_queue, event.line_slot);
            }
            s_transport.event_dispatched = event.sequence;
            xSemaphoreGive(s_transport.event_progress);
        }
    }
    xSemaphoreGive(s_transport.tasks_exited);
    vTaskDelete(NULL);
}

static void finish_request(vw553_at_result_t result)
{
    if (!s_transport.request_active) {
        return;
    }
    s_transport.result = result;
    s_transport.request.event_watermark = s_transport.event_enqueued;
    s_transport.request_active = false;
    s_transport.request_started = false;
    xSemaphoreGive(s_transport.result_ready);
}

static vw553_at_result_t poison_result(void)
{
    taskENTER_CRITICAL();
    const uint32_t faults = s_transport.isr_fault;
    taskEXIT_CRITICAL();
    return (faults & ISR_FAULT_CALLER_TIMEOUT) ? VW553_AT_RESULT_TIMEOUT :
           (faults & ISR_FAULT_RX_OVERFLOW) ? VW553_AT_RESULT_OVERFLOW : VW553_AT_RESULT_IO;
}

static bool transmit_bytes(const uint8_t *bytes, size_t length, uint32_t deadline_ms, size_t *sent)
{
    if (!bytes || !sent) {
        return false;
    }
    *sent = 0U;
    for (size_t i = 0; i < length; ++i) {
        if ((int32_t)(now_ms() - deadline_ms) >= 0 || s_transport.poisoned || s_transport.stopping) {
            return false;
        }
        while (usart_flag_get(s_transport.config.usart, USART_FLAG_TBE) == RESET) {
            if ((int32_t)(now_ms() - deadline_ms) >= 0 || s_transport.poisoned || s_transport.stopping) {
                return false;
            }
            vTaskDelay(1);
        }
        usart_data_transmit(s_transport.config.usart, (uint16_t)bytes[i]);
        ++*sent;
    }
    while (usart_flag_get(s_transport.config.usart, USART_FLAG_TC) == RESET) {
        if ((int32_t)(now_ms() - deadline_ms) >= 0 || s_transport.poisoned || s_transport.stopping) {
            return false;
        }
        vTaskDelay(1);
    }
    return vw553_uart_policy_tx_complete(length, *sent, true);
}

static void begin_request(const request_t *request)
{
    size_t command_length = 0;
    char command[VW553_COMMAND_TX_SIZE];
    s_transport.request.reply_length = 0U;
    s_transport.request.reply_overflow = false;
    s_transport.request.reply[0] = '\0';
    bool began = vw553_at_client_begin(&s_transport.client, request->command, request->command_length,
                                       request->wait_prompt, now_ms(), request->timeout_ms);
    if (!began && vw553_at_client_resynchronize(&s_transport.client)) {
        /* A transaction that ended in timeout or protocol error leaves the
         * client tainted until an explicit resync. Without this the transport
         * rejects every later command, which surfaces only as PROTOCOL. */
        VW553_SUMMARY("AT client resynchronized before retry");
        began = vw553_at_client_begin(&s_transport.client, request->command, request->command_length,
                                      request->wait_prompt, now_ms(), request->timeout_ms);
    }
    if (!began ||
        !vw553_at_client_take_command(&s_transport.client, command, sizeof(command), &command_length)) {
        /* The request never reached the UART. Report every guard so a stuck
         * client can be told apart from a poisoned link in the field. */
        VW553_SUMMARY("AT begin rejected cmd=%.*s len=%lu phase=%d tainted=%d result_ready=%d suppress_ipd=%d "
                      "pending=%d wire=%d poisoned=%d in_frame=%d",
                      (int)request->command_length, request->command,
                      (unsigned long)request->command_length, (int)s_transport.client.phase,
                      s_transport.client.tainted, s_transport.client.result_ready,
                      (int)s_transport.client.suppress_ipd_ok, s_transport.client.command_pending,
                      (int)s_transport.client.wire_mode, s_transport.poisoned,
                      vw553_at_parser_in_frame(&s_transport.parser));
        finish_request(VW553_AT_RESULT_PROTOCOL);
        return;
    }
    size_t sent = 0U;
    uint32_t deadline = request->started_ms + request->timeout_ms;
    if (!transmit_bytes((const uint8_t *)command, command_length, deadline, &sent) ||
        !vw553_uart_policy_tx_complete(command_length, sent, true)) {
        vw553_at_client_fail(&s_transport.client, VW553_AT_RESULT_IO);
        set_poison(ISR_FAULT_NOISE);
    }
}

static void start_pending_request(void)
{
    if (!s_transport.request_active || s_transport.request_started || s_transport.poisoned ||
        s_transport.client.suppress_ipd_ok || vw553_at_parser_in_frame(&s_transport.parser)) {
        return;
    }
    /* begin_request() can finish synchronously; mark started first so that a
     * synchronous failure does not leave this request eligible for restart. */
    s_transport.request_started = true;
    begin_request(&s_transport.request);
}

static void handle_client_result(void)
{
    vw553_at_result_t result;
    if (vw553_at_client_take_result(&s_transport.client, &result)) {
        if (result != VW553_AT_RESULT_OK && result != VW553_AT_RESULT_ERROR) {
            int cmd_len = (int)s_transport.request.command_length;
            if (cmd_len >= 2) cmd_len -= 2;
            VW553_SUMMARY("AT transaction failed result=%d cmd=%.*s payload=%lu prompt=%d sent=%d send_ok=%d",
                          (int)result, cmd_len, s_transport.request.command,
                          (unsigned long)s_transport.request.payload_length,
                          s_transport.request.prompt_seen ? 1 : 0,
                          s_transport.request.payload_sent ? 1 : 0,
                          s_transport.request.saw_send_ok ? 1 : 0);
            rx_trace_dump("RX FAIL", (int)result);
            set_poison(result == VW553_AT_RESULT_TIMEOUT ? ISR_FAULT_CALLER_TIMEOUT : ISR_FAULT_NOISE);
        }
        if (!vw553_uart_policy_payload_result(s_transport.request.payload_length, s_transport.request.prompt_seen,
                                              s_transport.request.payload_sent, s_transport.request.saw_send_ok,
                                              s_transport.request.saw_send_fail,
                                              result == VW553_AT_RESULT_OK)) {
            VW553_SUMMARY("payload result mismatch cmd=%.*s ok=%d prompt=%d sent=%d send_ok=%d send_fail=%d "
                          "reply=%.*s",
                          (int)s_transport.request.command_length, s_transport.request.command,
                          result == VW553_AT_RESULT_OK, s_transport.request.prompt_seen,
                          s_transport.request.payload_sent, s_transport.request.saw_send_ok,
                          s_transport.request.saw_send_fail, (int)s_transport.request.reply_length,
                          s_transport.request.reply);
            rx_trace_dump("RX TRACE", (int)result);
            result = VW553_AT_RESULT_PROTOCOL;
            set_poison(ISR_FAULT_NOISE);
        }
        if (s_transport.request.reply_overflow) {
            result = VW553_AT_RESULT_OVERFLOW;
            set_poison(ISR_FAULT_RX_OVERFLOW);
        }
        finish_request(result);
    }
}

static void log_transport_high_water(const char *tag)
{
    vw553_event_queue_stats_t queue_stats;
    vw553_event_queue_get_stats(s_transport.event_queue, &queue_stats);
    VW553_SUMMARY("%s rx_stream=%lu/%u rx_dma=%lu/%u events=%lu/%u data_slots=%lu/%u "
                  "slot_exhausted=%lu line_slots=%lu/%u line_exhausted=%lu rx_overflow=%lu",
                  tag, (unsigned long)s_transport.rx_stream_high_water, (unsigned)VW553_RX_STREAM_SIZE,
                  (unsigned long)s_transport.rx_dma_burst_high_water, (unsigned)VW553_RX_DMA_SIZE,
                  (unsigned long)queue_stats.event_queue_high_water, (unsigned)VW553_EVENT_QUEUE_DEPTH,
                  (unsigned long)queue_stats.data_slot_high_water, (unsigned)VW553_EVENT_DATA_SLOTS,
                  (unsigned long)queue_stats.data_slot_exhausted,
                  (unsigned long)queue_stats.line_slot_high_water, (unsigned)VW553_EVENT_LINE_SLOTS,
                  (unsigned long)queue_stats.line_slot_exhausted,
                  (unsigned long)s_transport.rx_stream_overflow);
}

static void process_poison(void)
{
    if (!s_transport.poisoned || s_transport.poison_processed) {
        return;
    }
    s_transport.poison_processed = true;
    vw553_at_result_t reason = poison_result();
    VW553_SUMMARY("transport poison reason=%d faults=0x%08lX", (int)reason,
                  (unsigned long)s_transport.isr_fault);
    log_transport_high_water("transport high-water");
    usart_interrupt_disable(s_transport.config.usart, USART_INT_RBNE);
    usart_interrupt_disable(s_transport.config.usart, USART_INT_ERR);
    nvic_irq_disable(s_transport.config.irq);
    (void)xStreamBufferReset(s_transport.rx_stream);
    if (s_transport.client.result_ready) {
        vw553_at_result_t ignored;
        (void)vw553_at_client_take_result(&s_transport.client, &ignored);
    }
    vw553_at_client_fail(&s_transport.client, reason);
    if (!s_transport.request_active) {
        request_t *queued_request = NULL;
        if (xQueueReceive(s_transport.request_queue, &queued_request, 0) == pdPASS) {
            s_transport.request_active = true;
        }
    }
    if (s_transport.request_active) {
        finish_request(reason);
    }
}

static void rx_task(void *argument)
{
    (void)argument;
    const vw553_at_client_events_t events = {
        .line = client_line,
        .data = client_data,
        .ble_data = client_ble_data,
        .prompt = client_prompt,
        .fault = client_fault,
        .datagram = client_datagram,
        .accept = client_accept,
        .accept_v2 = client_accept_v2,
        .close_v2 = client_close_v2,
        .ipd_trailer_dropped = client_ipd_trailer_dropped};
    vw553_at_client_init(&s_transport.client, &events, NULL);
    vw553_at_callbacks_t parser_callbacks = vw553_at_client_parser_callbacks();
    vw553_at_parser_init(&s_transport.parser, &parser_callbacks, &s_transport.client);

    uint8_t bytes[VW553_RX_READ_SIZE];
    request_t *request = NULL;
    while (!s_transport.stopping) {
        if (!s_transport.request_active && xQueueReceive(s_transport.request_queue, &request, 0) == pdPASS) {
            s_transport.request_active = true;
            s_transport.request_started = false;
            s_transport.request.started_ms = now_ms();
            if (s_transport.poisoned) {
                finish_request(poison_result());
            } else if (request->negotiate) {
                s_transport.capabilities = request->capabilities;
                s_transport.capability_negotiated = true;
                if (request->capabilities.version < 2U || request->capabilities.version > 3U ||
                    !vw553_at_client_set_wire_mode(&s_transport.client, VW553_AT_WIRE_ATOMIC_IPD_TRAILER)) {
                    s_transport.capability_negotiated = false;
                    finish_request(VW553_AT_RESULT_PROTOCOL);
                } else {
                    finish_request(VW553_AT_RESULT_OK);
                }
            }
        }
        start_pending_request();
        const size_t stream_used = xStreamBufferBytesAvailable(s_transport.rx_stream);
        if (stream_used > s_transport.rx_stream_high_water) {
            s_transport.rx_stream_high_water = (uint32_t)stream_used;
        }
        size_t count = xStreamBufferReceive(s_transport.rx_stream, bytes, sizeof(bytes), pdMS_TO_TICKS(VW553_IO_POLL_MS));
        if (count) {
            s_transport.rx_last_byte_ms = now_ms();
        }
        if (count && !s_transport.poisoned) {
            rx_trace_append(bytes, count);
            vw553_at_parser_feed(&s_transport.parser, bytes, count);
        }
        if (vw553_at_parser_in_frame(&s_transport.parser) &&
            (uint32_t)(now_ms() - s_transport.rx_last_byte_ms) >= VW553_FRAME_IDLE_TIMEOUT_MS) {
            VW553_SUMMARY("AT frame idle timeout fd=%d announced=%lu consumed=%lu remaining=%lu",
                          s_transport.parser.remote_fd,
                          (unsigned long)s_transport.parser.frame_length,
                          (unsigned long)(s_transport.parser.frame_length - s_transport.parser.remaining),
                          (unsigned long)s_transport.parser.remaining);
            rx_trace_dump("RX PARTIAL", -1);
            vw553_at_parser_drop_frame(&s_transport.parser);
        }
        start_pending_request();
        vw553_at_client_poll(&s_transport.client, now_ms());
        process_poison();
        handle_client_result();
        const uint32_t request_wait_ms = (uint32_t)(now_ms() - s_transport.request.started_ms);
        if (vw553_uart_policy_request_frame_hold_expired(
                s_transport.request_active, s_transport.request_started, s_transport.poisoned,
                vw553_at_parser_in_frame(&s_transport.parser), request_wait_ms,
                VW553_REQUEST_FRAME_HOLD_MS)) {
            VW553_SUMMARY("AT request frame hold expired wait=%lums limit=%lums",
                          (unsigned long)request_wait_ms,
                          (unsigned long)VW553_REQUEST_FRAME_HOLD_MS);
            set_poison(ISR_FAULT_NOISE);
            finish_request(VW553_AT_RESULT_PROTOCOL);
        }
    }
    if (s_transport.request_active) {
        vw553_at_client_fail(&s_transport.client, VW553_AT_RESULT_CANCELLED);
        handle_client_result();
        if (s_transport.request_active) {
            finish_request(VW553_AT_RESULT_CANCELLED);
        }
    }
    xSemaphoreGive(s_transport.tasks_exited);
    vTaskDelete(NULL);
}

#if VW553_RX_DMA_ENABLE
static uint32_t rx_dma_drain(void)
{
    BaseType_t wake = pdFALSE;
    uint32_t fault = 0U;
    if (!s_transport.rx_stream) {
        return 0U;
    }
    uint32_t written = VW553_RX_DMA_SIZE -
                       dma_transfer_number_get(VW553_RX_DMA_PERIPH, VW553_RX_DMA_CH);
    if (written > VW553_RX_DMA_SIZE) {
        written = VW553_RX_DMA_SIZE;
    }
    uint32_t burst_size = (written + VW553_RX_DMA_SIZE - s_rx_dma_pos) % VW553_RX_DMA_SIZE;
    if (written == VW553_RX_DMA_SIZE && s_rx_dma_pos == 0U) {
        burst_size = VW553_RX_DMA_SIZE;
    }
    if (burst_size > s_transport.rx_dma_burst_high_water) {
        s_transport.rx_dma_burst_high_water = burst_size;
    }
    /* A wrap makes 'written' jump back to 0 while 'pos' is still near the end
     * of the ring; the modulo walk below covers that case. The loop is bounded
     * by the ring size because 'written' is constant for this pass. */
    while (s_rx_dma_pos != written) {
        uint8_t byte = s_rx_dma_buffer[s_rx_dma_pos];
        s_rx_dma_pos = (s_rx_dma_pos + 1U) % VW553_RX_DMA_SIZE;
        if (xStreamBufferSendFromISR(s_transport.rx_stream, &byte, 1U, &wake) != 1U) {
            ++s_transport.rx_stream_overflow;
            fault |= ISR_FAULT_RX_OVERFLOW;
        }
    }
    portYIELD_FROM_ISR(wake);
    return fault;
}

static void rx_dma_configure(uint32_t usart, uint32_t irq_priority)
{
    dma_single_data_parameter_struct init;
    rcu_periph_clock_enable(RCU_DMA0);
    rcu_periph_clock_enable(RCU_DMAMUX);
    dma_deinit(VW553_RX_DMA_PERIPH);
    dma_single_data_para_struct_init(&init);
    init.periph_addr         = (uint32_t)&USART_RDATA(usart);
    init.periph_inc          = DMA_PERIPH_INCREASE_DISABLE;
    init.memory0_addr        = (uint32_t)s_rx_dma_buffer;
    init.memory_inc          = DMA_MEMORY_INCREASE_ENABLE;
    init.periph_memory_width = DMA_PERIPH_WIDTH_8BIT;
    init.circular_mode       = DMA_CIRCULAR_MODE_ENABLE;
    init.direction           = DMA_PERIPH_TO_MEMORY;
    init.number              = VW553_RX_DMA_SIZE;
    init.priority            = DMA_PRIORITY_HIGH;
    dma_single_data_mode_init(VW553_RX_DMA_PERIPH, VW553_RX_DMA_CH, &init);
    dma_circulation_enable(VW553_RX_DMA_PERIPH, VW553_RX_DMA_CH);
    /* DMAMUX route: UART4 RX request. */
    DMAMUX_RM_CH7CFG = DMA_REQUEST_UART4_RX;
    dma_channel_enable(VW553_RX_DMA_PERIPH, VW553_RX_DMA_CH);
    s_rx_dma_pos = 0U;
    usart_dma_receive_config(usart, USART_RECEIVE_DMA_ENABLE);
    dma_interrupt_enable(VW553_RX_DMA_PERIPH, VW553_RX_DMA_CH, DMA_INT_FTF);
    nvic_irq_enable(VW553_RX_DMA_IRQ, irq_priority, 0U);
}
#endif

static void configure_uart(const vw553_uart_transport_config_t *config)
{
    rcu_periph_clock_enable(config->gpio_clock);
    rcu_periph_clock_enable(config->usart_clock);
    gpio_af_set(config->gpio_port, config->alternate_function, config->tx_pin | config->rx_pin);
    gpio_mode_set(config->gpio_port, GPIO_MODE_AF, GPIO_PUPD_PULLUP, config->tx_pin | config->rx_pin);
    gpio_output_options_set(config->gpio_port, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, config->tx_pin | config->rx_pin);
    usart_deinit(config->usart);
    usart_baudrate_set(config->usart, config->baudrate);
    usart_word_length_set(config->usart, config->word_length);
    usart_stop_bit_set(config->usart, config->stop_bits);
    usart_parity_config(config->usart, config->parity);
    usart_transmit_config(config->usart, USART_TRANSMIT_ENABLE);
    usart_receive_config(config->usart, USART_RECEIVE_ENABLE);
    /* The hardware FIFO absorbs RX interrupt latency. Without it a single
     * byte-time of delay loses a payload byte, which silently shifts every
     * following AT frame. usart_fifo_enable() clears UEN, so re-enable after. */
    usart_fifo_enable(config->usart);
    usart_receive_fifo_threshold_config(config->usart, 0U);
#if VW553_RX_DMA_ENABLE
    /* Arm the DMA ring before UEN. Enabling RX first permits an IDLE/error
     * interrupt to run rx_dma_drain() while the transfer is still at reset
     * defaults, which can starve the caller before DMA configuration finishes. */
    rx_dma_configure(config->usart, config->irq_priority);
    usart_interrupt_enable(config->usart, USART_INT_IDLE);
#else
    usart_interrupt_enable(config->usart, USART_INT_RBNE);
#endif
    usart_interrupt_enable(config->usart, USART_INT_ERR);
#if VW553_RX_DMA_ENABLE
    nvic_irq_enable(config->irq, config->irq_priority, 0U);
#endif
    usart_enable(config->usart);
}

static void release_unstarted_primitives(void)
{
    if (s_transport.rx_stream) {
        vStreamBufferDelete(s_transport.rx_stream);
    }
    if (s_transport.request_queue) {
        vQueueDelete(s_transport.request_queue);
    }
    if (s_transport.caller_mutex) {
        vSemaphoreDelete(s_transport.caller_mutex);
    }
    if (s_transport.result_ready) {
        vSemaphoreDelete(s_transport.result_ready);
    }
    if (s_transport.event_progress) {
        vSemaphoreDelete(s_transport.event_progress);
    }
    if (s_transport.event_available) {
        vSemaphoreDelete(s_transport.event_available);
    }
    if (s_transport.event_queue) {
        vPortFree(s_transport.event_queue);
    }
    if (s_transport.tasks_exited) {
        vSemaphoreDelete(s_transport.tasks_exited);
    }
    memset(&s_transport, 0, sizeof(s_transport));
}

int32_t vw553_uart_transport_init(const vw553_uart_transport_config_t *config,
                                  const vw553_uart_transport_events_t *events, void *context)
{
    VW553_SUMMARY("UART transport init");
    if (!config_valid(config)) {
        VW553_LOG("UART config invalid");
        return OPRT_INVALID_PARM;
    }
    if (s_transport.initialized || s_init_failed_with_live_worker) {
        return OPRT_COM_ERROR;
    }
    memset(&s_transport, 0, sizeof(s_transport));
    s_transport.config = *config;
    if (events) {
        s_transport.events = *events;
    }
    s_transport.event_context = context;
    bool has_events = events_configured(events);
    s_transport.rx_stream = xStreamBufferCreate(VW553_RX_STREAM_SIZE, 1U);
    s_transport.request_queue = xQueueCreate(1U, sizeof(request_t *));
    if (has_events) {
        s_transport.event_queue = pvPortMalloc(sizeof(*s_transport.event_queue));
        if (s_transport.event_queue) {
            vw553_event_queue_init(s_transport.event_queue);
            VW553_SUMMARY("event queue pool bytes=%u free_heap=%u",
                          (unsigned)sizeof(*s_transport.event_queue), (unsigned)xPortGetFreeHeapSize());
        } else {
            VW553_SUMMARY("event queue pool allocation failed bytes=%u",
                          (unsigned)sizeof(*s_transport.event_queue));
        }
    }
    s_transport.caller_mutex = xSemaphoreCreateMutex();
    s_transport.result_ready = xSemaphoreCreateBinary();
    s_transport.event_progress = xSemaphoreCreateBinary();
    s_transport.event_available = xSemaphoreCreateBinary();
    s_transport.tasks_exited = xSemaphoreCreateCounting(2U, 0U);
    if (!s_transport.rx_stream || !s_transport.request_queue || (has_events && !s_transport.event_queue) ||
        !s_transport.caller_mutex ||
        !s_transport.result_ready || !s_transport.event_progress || !s_transport.event_available ||
        !s_transport.tasks_exited) {
        release_unstarted_primitives();
        return OPRT_COM_ERROR;
    }
    if (xTaskCreate(rx_task, "553_rx", VW553_RX_TASK_STACK, NULL, VW553_TASK_PRIORITY, &s_transport.rx_task) != pdPASS) {
        release_unstarted_primitives();
        return OPRT_COM_ERROR;
    }
    if (has_events && xTaskCreate(event_task, "553_evt", VW553_EVENT_TASK_STACK, NULL, VW553_TASK_PRIORITY,
                                  &s_transport.event_task) != pdPASS) {
        s_transport.stopping = true;
        if (xSemaphoreTake(s_transport.tasks_exited, pdMS_TO_TICKS(1000U)) == pdTRUE) {
            release_unstarted_primitives();
        } else {
            s_init_failed_with_live_worker = true;
        }
        return OPRT_COM_ERROR;
    }
    s_transport.initialized = true;
    configure_uart(config);
    VW553_SUMMARY("UART transport ready rx_stream=%u rx_dma=%u event_depth=%u data_slots=%u line_slots=%u",
                  (unsigned)VW553_RX_STREAM_SIZE, (unsigned)VW553_RX_DMA_SIZE,
                  (unsigned)VW553_EVENT_QUEUE_DEPTH, (unsigned)VW553_EVENT_DATA_SLOTS,
                  (unsigned)VW553_EVENT_LINE_SLOTS);
    return OPRT_OK;
}

static int32_t submit_locked(const char *command, size_t length, const void *payload, size_t payload_length,
                             bool wait_prompt, uint32_t timeout_ms, bool negotiate,
                             const vw553_uart_capabilities_t *capabilities, vw553_at_result_t *result)
{
    if (s_transport.poisoned) {
        return OPRT_COM_ERROR;
    }
    while (xSemaphoreTake(s_transport.result_ready, 0) == pdTRUE) {
    }
    memset(&s_transport.request, 0, sizeof(s_transport.request));
    s_transport.request.command_length = length;
    s_transport.request.wait_prompt = wait_prompt;
    s_transport.request.timeout_ms = timeout_ms;
    s_transport.request.payload_length = payload_length;
    s_transport.request.negotiate = negotiate;
    if (capabilities) {
        s_transport.request.capabilities = *capabilities;
    }
    memcpy(s_transport.request.command, command, length);
    s_transport.request.command[length] = '\0';
    if (payload_length) {
        memcpy(s_transport.request.payload, payload, payload_length);
    }
    request_t *request = &s_transport.request;
    if (xQueueSend(s_transport.request_queue, &request, 0) != pdPASS) {
        return OPRT_COM_ERROR;
    }
    if (xSemaphoreTake(s_transport.result_ready, pdMS_TO_TICKS(timeout_ms + VW553_IO_POLL_MS + 2U)) != pdTRUE) {
        VW553_SUMMARY("AT transaction timeout cmd=%.*s wait_prompt=%d payload=%lu timeout=%lums poisoned=%d",
                  (int)length, command, wait_prompt ? 1 : 0, (unsigned long)payload_length,
                  (unsigned long)timeout_ms, s_transport.poisoned ? 1 : 0);
        set_poison(ISR_FAULT_CALLER_TIMEOUT);
        *result = VW553_AT_RESULT_TIMEOUT;
        return OPRT_COM_ERROR;
    }
    *result = s_transport.result;
    if (*result != VW553_AT_RESULT_OK) {
        bool credential = request->command_length >= 13U &&
                          (memcmp(request->command, "AT+CWJAP_CUR", 13U) == 0 ||
                           memcmp(request->command, "AT+CWSAP_CUR", 13U) == 0);
        if (credential) {
            VW553_SUMMARY("AT failed credential cmd result=%d prompt=%d payload_sent=%d phase=%d",
                          (int)*result, s_transport.request.prompt_seen ? 1 : 0,
                          s_transport.request.payload_sent ? 1 : 0, (int)s_transport.client.phase);
        } else {
            VW553_SUMMARY("AT failed cmd=%.*s result=%d prompt=%d payload_sent=%d phase=%d reply=%.*s",
                          (int)request->command_length, request->command, (int)*result,
                          s_transport.request.prompt_seen ? 1 : 0, s_transport.request.payload_sent ? 1 : 0,
                          (int)s_transport.client.phase, (int)s_transport.request.reply_length,
                          s_transport.request.reply);
        }
    }
    return OPRT_OK;
}

static int32_t execute_locked(const vw553_uart_request_t *request, vw553_at_result_t *result)
{
    if (!request || !request->command || !request->command_length ||
        request->command_length > VW553_AT_COMMAND_SIZE || !request->timeout_ms || !result ||
        request->timeout_ms >= UINT32_C(0x80000000) - VW553_IO_POLL_MS ||
        !vw553_uart_policy_payload_valid(request->wait_prompt, request->payload,
                                         request->payload_length, VW553_UART_MAX_PAYLOAD)) {
        return OPRT_INVALID_PARM;
    }
    if (!s_transport.capability_negotiated &&
        !vw553_uart_policy_command_supported(request->command, request->command_length, request->wait_prompt)) {
        return OPRT_NOT_SUPPORTED;
    }
    if (!s_transport.capability_negotiated && request->payload_length) {
        return OPRT_NOT_SUPPORTED;
    }
    if (s_transport.capability_negotiated && request->payload_length > s_transport.capabilities.max_send) {
        return OPRT_NOT_SUPPORTED;
    }
    if (request->reply_capacity && !request->reply) {
        return OPRT_INVALID_PARM;
    }
    const char *command = request->command;
    if (request->command_length >= 13U &&
        (memcmp(command, "AT+CWJAP_CUR", 13U) == 0 || memcmp(command, "AT+CWSAP_CUR", 13U) == 0)) {
        VW553_LOG("AT TX credential command (arguments redacted)");
    } else {
        VW553_LOG("AT TX %.*s timeout=%lums", (int)request->command_length, command,
                  (unsigned long)request->timeout_ms);
    }
    int32_t status = submit_locked(request->command, request->command_length, request->payload,
                                   request->payload_length, request->wait_prompt, request->timeout_ms,
                                   false, NULL, result);
    VW553_LOG("AT RX status=%ld result=%d reply_len=%lu", (long)status, (int)*result,
              (unsigned long)s_transport.request.reply_length);
    if (status != OPRT_OK) {
        return status;
    }
    if (request->event_watermark) {
        *request->event_watermark = s_transport.request.event_watermark;
    }
    size_t needed = s_transport.request.reply_length;
    if (!request->reply && !request->reply_capacity && !request->reply_length) {
        return OPRT_OK;
    }
    if (request->reply_length) {
        *request->reply_length = needed;
    }
    if (!vw553_uart_policy_reply_fits(request->reply_capacity, needed)) {
        return OPRT_BUFFER_NOT_ENOUGH;
    }
    if (needed) {
        memcpy(request->reply, s_transport.request.reply, needed);
    }
    if (request->reply) {
        request->reply[needed] = '\0';
    }
    return OPRT_OK;
}

int32_t vw553_uart_transport_execute(const vw553_uart_request_t *request, vw553_at_result_t *result)
{
    if (!s_transport.initialized || !request || !result || !request->timeout_ms ||
        request->timeout_ms >= UINT32_C(0x80000000) - VW553_IO_POLL_MS) {
        return OPRT_INVALID_PARM;
    }
    if (xSemaphoreTake(s_transport.caller_mutex, pdMS_TO_TICKS(request->timeout_ms)) != pdTRUE) {
        return OPRT_COM_ERROR;
    }
    int32_t status = s_transport.initialized && !s_transport.stopping ? execute_locked(request, result) : OPRT_COM_ERROR;
    xSemaphoreGive(s_transport.caller_mutex);
    return status;
}

int32_t vw553_uart_transport_recover(void)
{
    if (!s_transport.initialized || !s_transport.rx_stream || !s_transport.request_queue) {
        return OPRT_RESOURCE_NOT_READY;
    }
    if (xSemaphoreTake(s_transport.caller_mutex, pdMS_TO_TICKS(VW553_IO_POLL_MS)) != pdTRUE) {
        return OPRT_RESOURCE_NOT_READY;
    }
    if (s_transport.request_active) {
        xSemaphoreGive(s_transport.caller_mutex);
        return OPRT_RESOURCE_NOT_READY;
    }

    usart_interrupt_disable(s_transport.config.usart, USART_INT_RBNE);
    usart_interrupt_disable(s_transport.config.usart, USART_INT_ERR);
    nvic_irq_disable(s_transport.config.irq);
    (void)xStreamBufferReset(s_transport.rx_stream);

    const vw553_at_client_events_t events = {
        .line = client_line,
        .data = client_data,
        .ble_data = client_ble_data,
        .prompt = client_prompt,
        .fault = client_fault,
        .datagram = client_datagram,
        .accept = client_accept,
        .accept_v2 = client_accept_v2,
        .close_v2 = client_close_v2};
    vw553_at_client_init(&s_transport.client, &events, NULL);
    vw553_at_callbacks_t parser_callbacks = vw553_at_client_parser_callbacks();
    vw553_at_parser_init(&s_transport.parser, &parser_callbacks, &s_transport.client);

    taskENTER_CRITICAL();
    s_transport.poisoned = false;
    s_transport.poison_processed = false;
    s_transport.isr_fault = 0U;
    s_transport.capability_negotiated = false;
    memset(&s_transport.capabilities, 0, sizeof(s_transport.capabilities));
    taskEXIT_CRITICAL();

    usart_interrupt_enable(s_transport.config.usart, USART_INT_RBNE);
    usart_interrupt_enable(s_transport.config.usart, USART_INT_ERR);
    nvic_irq_enable(s_transport.config.irq, s_transport.config.irq_priority, 0U);
    xSemaphoreGive(s_transport.caller_mutex);
    return OPRT_OK;
}

int32_t vw553_uart_transport_transact(const char *command, size_t length, bool wait_prompt,
                                      uint32_t timeout_ms, vw553_at_result_t *result)
{
    vw553_uart_request_t request = {
        .command = command, .command_length = length, .wait_prompt = wait_prompt, .timeout_ms = timeout_ms};
    return vw553_uart_transport_execute(&request, result);
}

int32_t vw553_uart_transport_negotiate(uint32_t timeout_ms)
{
    static const char command[] = "AT+TY553XCAP?";
    vw553_at_result_t result = VW553_AT_RESULT_NONE;
    vw553_uart_capabilities_t capabilities;
    if (!s_transport.initialized || !timeout_ms ||
        timeout_ms >= UINT32_C(0x80000000) - VW553_IO_POLL_MS) {
        return OPRT_INVALID_PARM;
    }
    if (xSemaphoreTake(s_transport.caller_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return OPRT_COM_ERROR;
    }
    if (s_transport.capability_negotiated) {
        xSemaphoreGive(s_transport.caller_mutex);
        return OPRT_OK;
    }
    int32_t status = submit_locked(command, sizeof(command) - 1U, NULL, 0U, false, timeout_ms,
                                   false, NULL, &result);
    VW553_LOG("CAP query status=%ld result=%d", (long)status, (int)result);
    if (status == OPRT_OK && result == VW553_AT_RESULT_OK &&
        vw553_uart_policy_network_capabilities_v2(s_transport.request.reply,
                                                  s_transport.request.reply_length, &capabilities)) {
        s_transport.request.reply_length = 0U;
        status = submit_locked(command, sizeof(command) - 1U, NULL, 0U, false, timeout_ms,
                               true, &capabilities, &result);
        if (status == OPRT_OK && result == VW553_AT_RESULT_OK) {
            s_transport.capability_negotiated = true;
            s_transport.capabilities = capabilities;
            VW553_LOG("CAP v%u max_clients=%u max_servers=%u max_send=%u udp_send_id=%u",
                      (unsigned)capabilities.version, (unsigned)capabilities.max_clients,
                      (unsigned)capabilities.max_servers, (unsigned)capabilities.max_send,
                      (unsigned)capabilities.udp_send_id);
        }
    } else if (status == OPRT_OK && result == VW553_AT_RESULT_ERROR) {
        /* Stock 1.0.3g rejects AT+TY553XCAP?. Its standard CIPMUX=1 client
         * sockets remain fully usable; the extended server/accept features
         * are simply unavailable. Use the trailer-aware parser mode so the
         * bare OK emitted after each stock +IPD frame is not mistaken for a
         * command result. */
        if (!vw553_at_client_set_wire_mode(&s_transport.client, VW553_AT_WIRE_ATOMIC_IPD_TRAILER)) {
            status = OPRT_COM_ERROR;
        } else {
            vw553_uart_capabilities_t stock = {.version = 0U, .max_clients = 8U,
                                               .max_send = 2048U, .udp_send_id = 8U};
            s_transport.capability_negotiated = true;
            s_transport.capabilities = stock;
            status = OPRT_OK;
            VW553_LOG("stock AT mode max_clients=%u max_send=%u", (unsigned)stock.max_clients,
                      (unsigned)stock.max_send);
        }
    } else if (status == OPRT_OK) {
        status = OPRT_NOT_SUPPORTED;
    }
    xSemaphoreGive(s_transport.caller_mutex);
    return status;
}

int32_t vw553_uart_transport_get_capabilities(vw553_uart_capabilities_t *capabilities)
{
    if (!s_transport.initialized || !capabilities) {
        return OPRT_INVALID_PARM;
    }
    if (xSemaphoreTake(s_transport.caller_mutex, pdMS_TO_TICKS(VW553_IO_POLL_MS)) != pdTRUE) {
        return OPRT_COM_ERROR;
    }
    if (!s_transport.capability_negotiated) {
        xSemaphoreGive(s_transport.caller_mutex);
        return OPRT_NOT_SUPPORTED;
    }
    *capabilities = s_transport.capabilities;
    xSemaphoreGive(s_transport.caller_mutex);
    return OPRT_OK;
}

int32_t vw553_uart_transport_wait_events(uint32_t watermark, uint32_t timeout_ms)
{
    if (!s_transport.initialized || !timeout_ms ||
        timeout_ms >= UINT32_C(0x80000000) - VW553_IO_POLL_MS) {
        return OPRT_INVALID_PARM;
    }
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (!timeout_ticks) {
        timeout_ticks = 1U;
    }
    while (!vw553_uart_policy_sequence_reached(s_transport.event_dispatched, watermark)) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= timeout_ticks) {
            return OPRT_TIMEOUT;
        }
        if (xSemaphoreTake(s_transport.event_progress, timeout_ticks - elapsed) != pdTRUE &&
            !vw553_uart_policy_sequence_reached(s_transport.event_dispatched, watermark)) {
            return OPRT_TIMEOUT;
        }
    }
    return OPRT_OK;
}

int32_t vw553_uart_transport_get_stats(vw553_uart_transport_stats_t *stats)
{
    if (!stats || !s_transport.initialized || !s_transport.event_queue) {
        return OPRT_INVALID_PARM;
    }
    vw553_event_queue_stats_t queue_stats;
    vw553_event_queue_get_stats(s_transport.event_queue, &queue_stats);
    *stats = (vw553_uart_transport_stats_t){
        .rx_stream_high_water = s_transport.rx_stream_high_water,
        .rx_stream_capacity = VW553_RX_STREAM_SIZE,
        .rx_dma_burst_high_water = s_transport.rx_dma_burst_high_water,
        .rx_dma_capacity = VW553_RX_DMA_SIZE,
        .event_queue_high_water = queue_stats.event_queue_high_water,
        .event_queue_capacity = VW553_EVENT_QUEUE_DEPTH,
        .data_slot_high_water = queue_stats.data_slot_high_water,
        .data_slot_capacity = VW553_EVENT_DATA_SLOTS,
        .data_slot_exhausted = queue_stats.data_slot_exhausted,
        .line_slot_high_water = queue_stats.line_slot_high_water,
        .line_slot_capacity = VW553_EVENT_LINE_SLOTS,
        .line_slot_exhausted = queue_stats.line_slot_exhausted,
        .rx_stream_overflow = s_transport.rx_stream_overflow,
    };
    return OPRT_OK;
}

bool vw553_uart_transport_is_poisoned(void)
{
    return s_transport.poisoned;
}

#ifndef VW553_UART_IRQ_HANDLER
#error "The selected board must define VW553_UART_IRQ_HANDLER"
#endif
#if VW553_RX_DMA_ENABLE
void DMA0_Channel7_IRQHandler(void)
{
    /* Channel 7 flags live in DMA_INTF1/INTC1 with the per-channel stride. */
    const uint32_t slot = (uint32_t)VW553_RX_DMA_CH - 4U;
    if (DMA_INTF1(VW553_RX_DMA_PERIPH) & DMA_FLAG_ADD(DMA_INTF_FTFIF, slot)) {
        DMA_INTC1(VW553_RX_DMA_PERIPH) = DMA_FLAG_ADD(DMA_INTC_FTFIFC, slot);
    }
    uint32_t fault = rx_dma_drain();
    if (fault) {
        UBaseType_t mask = taskENTER_CRITICAL_FROM_ISR();
        s_transport.isr_fault |= fault;
        s_transport.poisoned = true;
        taskEXIT_CRITICAL_FROM_ISR(mask);
    }
}
#endif

void VW553_UART_IRQ_HANDLER(void)
{
#if defined(GD32H77X_VW553_AT_BRIDGE) && (GD32H77X_VW553_AT_BRIDGE == 1)
    if (gd32h77x_vw553_bridge_uart4_rx_isr()) {
        return;
    }
#endif
    BaseType_t wake = pdFALSE;
    uint32_t fault = 0;
    const uint32_t usart = s_transport.config.usart;
    if (!s_transport.initialized || !usart) {
        return;
    }
    if (usart_flag_get(usart, USART_FLAG_ORERR) == SET) {
        usart_flag_clear(usart, USART_FLAG_ORERR);
        fault |= ISR_FAULT_OVERRUN;
    }
    if (usart_flag_get(usart, USART_FLAG_FERR) == SET) {
        usart_flag_clear(usart, USART_FLAG_FERR);
        fault |= ISR_FAULT_FRAMING;
    }
    if (usart_flag_get(usart, USART_FLAG_NERR) == SET) {
        usart_flag_clear(usart, USART_FLAG_NERR);
        fault |= ISR_FAULT_NOISE;
    }
    if (usart_flag_get(usart, USART_FLAG_PERR) == SET) {
        usart_flag_clear(usart, USART_FLAG_PERR);
        fault |= ISR_FAULT_PARITY;
    }
#if VW553_RX_DMA_ENABLE
    if (usart_flag_get(usart, USART_FLAG_IDLE) == SET) {
        /* USART_INTC.IDLEC clears the flag. Do NOT read RDATA here: that would
         * steal one byte from the DMA stream and desynchronise every frame. */
        usart_flag_clear(usart, USART_FLAG_IDLE);
        fault |= rx_dma_drain();
    }
#else
    while (usart_flag_get(usart, USART_FLAG_RBNE) == SET) {
        uint8_t byte = (uint8_t)usart_data_receive(usart);
        if (xStreamBufferSendFromISR(s_transport.rx_stream, &byte, 1U, &wake) != 1U) {
            ++s_transport.rx_stream_overflow;
            fault |= ISR_FAULT_RX_OVERFLOW;
        }
    }
#endif
    if (fault) {
        UBaseType_t mask = taskENTER_CRITICAL_FROM_ISR();
        s_transport.isr_fault |= fault;
        s_transport.poisoned = true;
        taskEXIT_CRITICAL_FROM_ISR(mask);
    }
    portYIELD_FROM_ISR(wake);
}
