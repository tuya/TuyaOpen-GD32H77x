/**
 * @file session.c
 * @brief Transport-independent AT session for the AT client.
 */

#include "at_session.h"
#include "at_log.h"
#include "at_transport.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "stream_buffer.h"
#include "task.h"
#include "at_parser.h"
#include "at_event_queue.h"
#include "at_profile.h"
#include "at_commands.h"
#include "at_status.h"

#include <string.h>

#define AT_RX_STREAM_SIZE       12288U
#define AT_RX_READ_SIZE         2048U
#define AT_COMMAND_TX_SIZE      (AT_COMMAND_SIZE + 2U)
#define AT_RX_TASK_STACK        1536U
/* Subscriber callbacks can run TAL BLE connection handling and an AT RPC,
 * including newlib formatting, on this task. FreeRTOS sizes are in words. */
#define AT_EVENT_TASK_STACK     2048U
#define AT_TASK_PRIORITY        5U
#define AT_IO_POLL_MS           5U

/* A frame header that promises more bytes than the module ever delivers would
 * otherwise leave the parser in PAYLOAD forever, blocking every later
 * transaction. Give up when the stream stops feeding the frame. */
#define AT_FRAME_IDLE_TIMEOUT_MS 500U
/* The stock +IPD frame ends with a bare OK. If a caller queues an AT
 * command exactly between the payload and that trailer, let RX drain the
 * trailer before starting the command instead of failing a valid request. */
#define AT_REQUEST_FRAME_HOLD_MS (AT_FRAME_IDLE_TIMEOUT_MS + 100U)

typedef struct {
    char command[AT_COMMAND_SIZE + 1U];
    size_t command_length;
    uint8_t payload[AT_SESSION_MAX_PAYLOAD];
    size_t payload_length;
    bool wait_prompt;
    uint32_t timeout_ms;
    uint32_t started_ms;
    char reply[AT_SESSION_REPLY_CAPACITY];
    size_t reply_length;
    bool reply_overflow;
    bool payload_sent;
    bool prompt_seen;
    bool saw_send_ok;
    bool saw_send_fail;
    bool negotiate;
    at_capabilities_t capabilities;
    uint32_t event_watermark;
} request_t;

typedef struct {
    volatile bool initialized;
    volatile bool poisoned;
    bool poison_processed;
    volatile bool stopping;
    volatile uint32_t isr_fault;
    at_transport_t *transport;
    at_session_events_t events;
    void *event_context;
    StreamBufferHandle_t rx_stream;
    QueueHandle_t request_queue;
    at_event_queue_t *event_queue;
    SemaphoreHandle_t caller_mutex;
    SemaphoreHandle_t result_ready;
    SemaphoreHandle_t event_progress;
    SemaphoreHandle_t event_available;
    SemaphoreHandle_t tasks_exited;
    TaskHandle_t rx_task;
    TaskHandle_t event_task;
    at_client_t client;
    at_parser_t parser;
    request_t request;
    at_result_t result;
    bool request_active;
    bool request_started;
    volatile uint32_t event_enqueued;
    volatile uint32_t event_dispatched;
    volatile uint32_t rx_stream_high_water;
    volatile uint32_t rx_stream_overflow;
    bool capability_negotiated;
    uint8_t rx_trace[64];
    size_t rx_trace_used;
    uint32_t rx_last_byte_ms;
    at_capabilities_t capabilities;
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

static bool events_configured(const at_session_events_t *events)
{
    return events && (events->line || events->data || events->prompt || events->fault || events->datagram ||
                      events->accept || events->accept_v2 || events->close_v2);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static uint32_t map_transport_fault(uint32_t faults)
{
    uint32_t mapped = 0U;
    if (faults & AT_TRANSPORT_FAULT_RX_OVERFLOW) mapped |= ISR_FAULT_RX_OVERFLOW;
    if (faults & AT_TRANSPORT_FAULT_OVERRUN) mapped |= ISR_FAULT_OVERRUN;
    if (faults & AT_TRANSPORT_FAULT_FRAMING) mapped |= ISR_FAULT_FRAMING;
    if (faults & AT_TRANSPORT_FAULT_NOISE) mapped |= ISR_FAULT_NOISE;
    if (faults & AT_TRANSPORT_FAULT_PARITY) mapped |= ISR_FAULT_PARITY;
    return mapped;
}

static void transport_fault(void *context, uint32_t faults)
{
    (void)context;
    const uint32_t mapped = map_transport_fault(faults);
    if (!mapped) return;
    if (xPortIsInsideInterrupt()) {
        UBaseType_t mask = taskENTER_CRITICAL_FROM_ISR();
        s_transport.isr_fault |= mapped;
        s_transport.poisoned = true;
        s_transport.poison_processed = false;
        taskEXIT_CRITICAL_FROM_ISR(mask);
    } else {
        taskENTER_CRITICAL();
        s_transport.isr_fault |= mapped;
        s_transport.poisoned = true;
        s_transport.poison_processed = false;
        taskEXIT_CRITICAL();
    }
}

static void transport_rx(void *context, const uint8_t *data, size_t length)
{
    (void)context;
    if (!s_transport.rx_stream || !data || !length) return;
    BaseType_t wake = pdFALSE;
    size_t sent;
    if (xPortIsInsideInterrupt()) {
        sent = xStreamBufferSendFromISR(s_transport.rx_stream, data, length, &wake);
    } else {
        sent = xStreamBufferSend(s_transport.rx_stream, data, length, 0U);
    }
    if (sent != length) {
        ++s_transport.rx_stream_overflow;
        transport_fault(NULL, AT_TRANSPORT_FAULT_RX_OVERFLOW);
    }
    if (xPortIsInsideInterrupt()) portYIELD_FROM_ISR(wake);
}

static bool transport_rx_ready(void *context, size_t length)
{
    (void)context;
    return s_transport.rx_stream &&
           xStreamBufferSpacesAvailable(s_transport.rx_stream) >= length;
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

static bool enqueue_event(const at_queued_event_t *event)
{
    if (!s_transport.events.line && !s_transport.events.data && !s_transport.events.ble_data && !s_transport.events.prompt &&
        !s_transport.events.fault && !s_transport.events.datagram && !s_transport.events.accept &&
        !s_transport.events.accept_v2 && !s_transport.events.close_v2) {
        return true;
    }
    at_queued_event_t queued = *event;
    if (!at_event_queue_enqueue(s_transport.event_queue, &queued)) {
        AT_LOG("event queue full kind=%d depth=%u pending=%lu enqueued=%lu dispatched=%lu",
                  (int)event->kind, (unsigned)AT_EVENT_QUEUE_DEPTH,
                  (unsigned long)at_event_queue_pending(s_transport.event_queue),
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
    AT_LOG("AT LINE active=%d phase=%d len=%lu text=%.*s", s_transport.request_active ? 1 : 0,
              (int)s_transport.client.phase, (unsigned long)length, (int)length, line);
    if (s_transport.request_active && length == sizeof("SEND OK") - 1U &&
        memcmp(line, "SEND OK", length) == 0) {
        s_transport.request.saw_send_ok = true;
    }
    /* The AT BLE data-transport command terminates a successful payload
     * transfer with plain OK, unlike the socket CIPSEND family which emits
     * SEND OK followed by OK.  Keep that exception tied to this exact command
     * and only after the prompt payload has been sent. */
    if (s_transport.request_active && s_transport.request.prompt_seen && s_transport.request.payload_sent &&
        length == 2U && memcmp(line, "OK", 2U) == 0 &&
        at_uart_policy_ble_payload_uses_plain_ok(s_transport.request.command,
                                                    s_transport.request.command_length)) {
        s_transport.request.saw_send_ok = true;
    }
    if (s_transport.request_active && length == sizeof("SEND FAIL") - 1U &&
        memcmp(line, "SEND FAIL", length) == 0) {
        s_transport.request.saw_send_fail = true;
    }
    if (s_transport.request_active &&
        !at_uart_policy_capture_line(s_transport.request.reply, sizeof(s_transport.request.reply),
                                        &s_transport.request.reply_length, line, length)) {
        s_transport.request.reply_overflow = true;
        set_poison(ISR_FAULT_RX_OVERFLOW);
        return;
    }
    char *slot = at_event_queue_line_acquire(s_transport.event_queue);
    if (!slot) {
        AT_SUMMARY("event line slots exhausted len=%lu pending=%lu", (unsigned long)length,
                      (unsigned long)at_event_queue_pending(s_transport.event_queue));
        set_poison(ISR_FAULT_RX_OVERFLOW);
        return;
    }
    const uint8_t line_slot = at_event_queue_slot_for_line(s_transport.event_queue, slot);
    at_queued_event_t event = {.kind = AT_EVENT_LINE, .length = length, .line_slot = line_slot};
    if (length > AT_EVENT_LINE_SLOT_SIZE || line_slot == AT_EVENT_LINE_SLOT_NONE) {
        if (line_slot != AT_EVENT_LINE_SLOT_NONE) {
            at_event_queue_line_release(s_transport.event_queue, line_slot);
        }
        set_poison(ISR_FAULT_RX_OVERFLOW);
        return;
    }
    memcpy(slot, line, length);
    if (!enqueue_event(&event)) {
        at_event_queue_line_release(s_transport.event_queue, line_slot);
    }
}

static void client_ipd_trailer_dropped(void *context)
{
    (void)context;
    AT_SUMMARY("AT stale IPD trailer overtaken by prompt");
}

static bool enqueue_data_event(at_event_kind_t kind, int remote_fd, uint8_t conn_id, int listener_fd,
                               const char *peer_ipv4, uint16_t peer_port, const uint8_t *data, size_t length,
                               bool final)
{
    size_t offset = 0U;
    do {
        at_queued_event_t event = {.kind = kind,
                                      .remote_fd = remote_fd,
                                      .conn_id = conn_id,
                                      .listener_fd = listener_fd,
                                      .peer_port = peer_port,
                                      .final = false,
                                      .data_slot = AT_EVENT_DATA_SLOT_NONE,
                                      .line_slot = AT_EVENT_LINE_SLOT_NONE};
        size_t chunk = length - offset;
        if (chunk > AT_EVENT_DATA_SLOT_SIZE) {
            chunk = AT_EVENT_DATA_SLOT_SIZE;
        }
        event.length = chunk;
        if (chunk) {
            uint8_t *slot = at_event_queue_data_acquire(s_transport.event_queue);
            if (!slot) {
                AT_SUMMARY("event data slots exhausted kind=%d len=%lu pending=%lu",
                              (int)kind, (unsigned long)length,
                              (unsigned long)at_event_queue_pending(s_transport.event_queue));
                set_poison(ISR_FAULT_RX_OVERFLOW);
                return false;
            }
            event.data_slot = at_event_queue_slot_for_data(s_transport.event_queue, slot);
            if (event.data_slot == AT_EVENT_DATA_SLOT_NONE) {
                set_poison(ISR_FAULT_RX_OVERFLOW);
                return false;
            }
            memcpy(slot, data + offset, chunk);
        }
        offset += chunk;
        event.final = final && offset == length;
        if (peer_ipv4 && kind == AT_EVENT_DATAGRAM) {
            memcpy(event.peer_ipv4, peer_ipv4, sizeof(event.peer_ipv4));
        }
        if (!enqueue_event(&event)) {
            if (event.data_slot != AT_EVENT_DATA_SLOT_NONE) {
                at_event_queue_data_release(s_transport.event_queue, event.data_slot);
            }
            return false;
        }
    } while (offset < length);
    return true;
}

static void client_data(void *context, int remote_fd, const uint8_t *data, size_t length, bool final)
{
    (void)context;
    AT_LOG("AT DATA fd=%d len=%lu final=%d first=0x%02x", remote_fd, (unsigned long)length,
              final ? 1 : 0, data && length ? data[0] : 0U);
    (void)enqueue_data_event(AT_EVENT_DATA, remote_fd, 0U, 0, NULL, 0U, data, length, final);
}

static void client_ble_data(void *context, uint8_t conn_id, const uint8_t *data, size_t length, bool final)
{
    (void)context;
    (void)enqueue_data_event(AT_EVENT_BLE_DATA, 0, conn_id, 0, NULL, 0U, data, length, final);
}

static void client_prompt(void *context)
{
    (void)context;
    AT_LOG("AT PROMPT active=%d wait=%d payload=%lu phase=%d", s_transport.request_active ? 1 : 0,
              s_transport.request.wait_prompt ? 1 : 0, (unsigned long)s_transport.request.payload_length,
              (int)s_transport.client.phase);
    if (!s_transport.request_active) {
        /* A CIPSEND prompt can arrive after its transaction timed out. With no
         * active request there is no payload to send, so consume the stale
         * prompt instead of poisoning an otherwise synchronized transport. */
        AT_SUMMARY("AT stale prompt ignored phase=%d", (int)s_transport.client.phase);
        return;
    }
    if (!at_uart_policy_accept_prompt(s_transport.request_active, s_transport.request.wait_prompt,
                                         s_transport.request.payload_length, s_transport.request.prompt_seen,
                                         s_transport.request.payload_sent)) {
        AT_SUMMARY("AT prompt rejected active=%d wait=%d prompt_seen=%d payload_sent=%d phase=%d",
                      s_transport.request_active ? 1 : 0, s_transport.request.wait_prompt ? 1 : 0,
                      s_transport.request.prompt_seen ? 1 : 0, s_transport.request.payload_sent ? 1 : 0,
                      (int)s_transport.client.phase);
        at_client_fail(&s_transport.client, AT_RESULT_PROTOCOL);
        set_poison(ISR_FAULT_NOISE);
        return;
    }
    s_transport.request.prompt_seen = true;
    if (s_transport.request.payload_length) {
        size_t sent = 0U;
        uint32_t deadline = s_transport.request.started_ms + s_transport.request.timeout_ms;
        if (!at_uart_policy_payload_valid(true, s_transport.request.payload,
                                             s_transport.request.payload_length, AT_SESSION_MAX_PAYLOAD) ||
            !transmit_bytes(s_transport.request.payload, s_transport.request.payload_length, deadline, &sent) ||
            !at_uart_policy_tx_complete(s_transport.request.payload_length, sent, true)) {
            AT_LOG("AT PAYLOAD TX failed sent=%lu/%lu", (unsigned long)sent,
                      (unsigned long)s_transport.request.payload_length);
            at_client_fail(&s_transport.client, AT_RESULT_IO);
            set_poison(ISR_FAULT_NOISE);
            return;
        }
        s_transport.request.payload_sent = true;
        AT_LOG("AT PAYLOAD TX complete sent=%lu", (unsigned long)sent);
        char preview[161];
        size_t take = s_transport.request.payload_length < sizeof(preview) - 1U
                          ? s_transport.request.payload_length
                          : sizeof(preview) - 1U;
        for (size_t i = 0U; i < take; ++i) {
            uint8_t byte = ((const uint8_t *)s_transport.request.payload)[i];
            preview[i] = byte >= 0x20U && byte <= 0x7eU ? (char)byte : '.';
        }
        preview[take] = '\0';
        AT_LOG("AT PAYLOAD preview=%s", preview);
    }
    const at_queued_event_t event = {.kind = AT_EVENT_PROMPT};
    (void)enqueue_event(&event);
}

static void rx_trace_dump(const char *tag, int fault);

static void client_fault(void *context, at_fault_t fault)
{
    (void)context;
    rx_trace_dump("RX FAULT", fault);
    const at_queued_event_t event = {.kind = AT_EVENT_FAULT, .fault = fault};
    (void)enqueue_event(&event);
}

void at_session_dump_rx_trace(const char *tag);

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

void at_session_dump_rx_trace(const char *tag) { rx_trace_dump(tag, -1); }

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
    AT_SUMMARY("%s fault=%d len=%u data=%s", tag, fault, (unsigned)s_transport.rx_trace_used, hex);
    s_transport.rx_trace_used = 0U;
}

static void client_datagram(void *context, int listener_fd, const char *peer_ipv4, uint16_t peer_port,
                            const uint8_t *data, size_t length, bool final)
{
    (void)context;
    (void)enqueue_data_event(AT_EVENT_DATAGRAM, 0, 0U, listener_fd, peer_ipv4, peer_port, data, length, final);
}

static void client_accept(void *context, int listener_fd, int client_fd, const char *peer_ipv4, uint16_t peer_port)
{
    (void)context;
    at_queued_event_t event = {
        .kind = AT_EVENT_ACCEPT, .listener_fd = listener_fd, .client_fd = client_fd, .peer_port = peer_port};
    memcpy(event.peer_ipv4, peer_ipv4, sizeof(event.peer_ipv4));
    (void)enqueue_event(&event);
}

static void client_accept_v2(void *context, uint32_t con_id, uint32_t generation, int listener_fd,
                             uint32_t listener_generation, int lwip_fd, const char *peer_ipv4, uint16_t peer_port)
{
    (void)context;
    AT_LOG("AT ACCEPT con_id=%lu gen=%lu listener=%d lgen=%lu fd=%d peer=%s:%u",
              (unsigned long)con_id, (unsigned long)generation, listener_fd,
              (unsigned long)listener_generation, lwip_fd, peer_ipv4 ? peer_ipv4 : "", (unsigned)peer_port);
    at_queued_event_t event = {.kind = AT_EVENT_ACCEPT_V2, .con_id = con_id, .generation = generation,
                            .listener_fd = listener_fd, .listener_generation = listener_generation,
                            .lwip_fd = lwip_fd, .peer_port = peer_port};
    memcpy(event.peer_ipv4, peer_ipv4, sizeof(event.peer_ipv4));
    (void)enqueue_event(&event);
}

static void client_close_v2(void *context, uint32_t con_id, int lwip_fd, uint32_t generation, const char *reason)
{
    (void)context;
    AT_LOG("AT CLOSE con_id=%lu gen=%lu fd=%d reason=%s", (unsigned long)con_id,
              (unsigned long)generation, lwip_fd, reason ? reason : "");
    at_queued_event_t event = {.kind = AT_EVENT_CLOSE_V2, .con_id = con_id, .generation = generation, .lwip_fd = lwip_fd};
    size_t length = reason ? strlen(reason) : 0U;
    if (!length || length >= sizeof(event.reason)) {
        set_poison(ISR_FAULT_NOISE);
        return;
    }
    memcpy(event.reason, reason, length + 1U);
    (void)enqueue_event(&event);
}

static void dispatch_event(const at_queued_event_t *event)
{
    switch (event->kind) {
        case AT_EVENT_LINE:
            if (s_transport.events.line) {
                s_transport.events.line(s_transport.event_context,
                                        at_event_queue_line(s_transport.event_queue, event->line_slot),
                                        event->length);
            }
            break;
        case AT_EVENT_DATA:
            if (s_transport.events.data) {
                s_transport.events.data(s_transport.event_context, event->remote_fd,
                                        at_event_queue_data(s_transport.event_queue, event->data_slot),
                                        event->length, event->final);
            }
            break;
        case AT_EVENT_BLE_DATA:
            if (s_transport.events.ble_data) {
                s_transport.events.ble_data(s_transport.event_context, event->conn_id,
                                            at_event_queue_data(s_transport.event_queue, event->data_slot),
                                            event->length, event->final);
            }
            break;
        case AT_EVENT_PROMPT:
            if (s_transport.events.prompt) {
                s_transport.events.prompt(s_transport.event_context);
            }
            break;
        case AT_EVENT_FAULT:
            if (s_transport.events.fault) {
                s_transport.events.fault(s_transport.event_context, event->fault);
            }
            break;
        case AT_EVENT_DATAGRAM:
            if (s_transport.events.datagram) {
                s_transport.events.datagram(s_transport.event_context, event->listener_fd, event->peer_ipv4,
                                            event->peer_port,
                                            at_event_queue_data(s_transport.event_queue, event->data_slot),
                                            event->length, event->final);
            }
            break;
        case AT_EVENT_ACCEPT:
            if (s_transport.events.accept) {
                s_transport.events.accept(s_transport.event_context, event->listener_fd, event->client_fd,
                                          event->peer_ipv4, event->peer_port);
            }
            break;
        case AT_EVENT_ACCEPT_V2:
            if (s_transport.events.accept_v2) {
                s_transport.events.accept_v2(s_transport.event_context, event->con_id, event->generation,
                                             event->listener_fd, event->listener_generation, event->lwip_fd,
                                             event->peer_ipv4, event->peer_port);
            }
            break;
        case AT_EVENT_CLOSE_V2:
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
    at_queued_event_t event;
    while (!s_transport.stopping) {
        (void)xSemaphoreTake(s_transport.event_available, pdMS_TO_TICKS(AT_IO_POLL_MS));
        while (at_event_queue_dequeue(s_transport.event_queue, &event)) {
            dispatch_event(&event);
            if ((event.kind == AT_EVENT_DATA || event.kind == AT_EVENT_BLE_DATA ||
                 event.kind == AT_EVENT_DATAGRAM) && event.data_slot != AT_EVENT_DATA_SLOT_NONE) {
                at_event_queue_data_release(s_transport.event_queue, event.data_slot);
            }
            if (event.kind == AT_EVENT_LINE && event.line_slot != AT_EVENT_LINE_SLOT_NONE) {
                at_event_queue_line_release(s_transport.event_queue, event.line_slot);
            }
            s_transport.event_dispatched = event.sequence;
            xSemaphoreGive(s_transport.event_progress);
        }
    }
    xSemaphoreGive(s_transport.tasks_exited);
    vTaskDelete(NULL);
}

static void finish_request(at_result_t result)
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

static at_result_t poison_result(void)
{
    taskENTER_CRITICAL();
    const uint32_t faults = s_transport.isr_fault;
    taskEXIT_CRITICAL();
    return (faults & ISR_FAULT_CALLER_TIMEOUT) ? AT_RESULT_TIMEOUT :
           (faults & ISR_FAULT_RX_OVERFLOW) ? AT_RESULT_OVERFLOW : AT_RESULT_IO;
}

static bool transmit_bytes(const uint8_t *bytes, size_t length, uint32_t deadline_ms, size_t *sent)
{
    if (!bytes || !sent || !s_transport.transport || s_transport.poisoned || s_transport.stopping ||
        (int32_t)(now_ms() - deadline_ms) >= 0) return false;
    const uint32_t remaining_ms = deadline_ms - now_ms();
    const int status = at_transport_write(s_transport.transport, bytes, length, remaining_ms, sent);
    return status == AT_TRANSPORT_OK && at_uart_policy_tx_complete(length, *sent, true);
}

static void begin_request(const request_t *request)
{
    size_t command_length = 0;
    char command[AT_COMMAND_TX_SIZE];
    s_transport.request.reply_length = 0U;
    s_transport.request.reply_overflow = false;
    s_transport.request.reply[0] = '\0';
    bool began = at_client_begin(&s_transport.client, request->command, request->command_length,
                                       request->wait_prompt, now_ms(), request->timeout_ms);
    if (!began && at_client_resynchronize(&s_transport.client)) {
        /* A transaction that ended in timeout or protocol error leaves the
         * client tainted until an explicit resync. Without this the transport
         * rejects every later command, which surfaces only as PROTOCOL. */
        AT_SUMMARY("AT client resynchronized before retry");
        began = at_client_begin(&s_transport.client, request->command, request->command_length,
                                      request->wait_prompt, now_ms(), request->timeout_ms);
    }
    if (!began ||
        !at_client_take_command(&s_transport.client, command, sizeof(command), &command_length)) {
        /* The request never reached the UART. Report every guard so a stuck
         * client can be told apart from a poisoned link in the field. */
        AT_SUMMARY("AT begin rejected cmd=%.*s len=%lu phase=%d tainted=%d result_ready=%d suppress_ipd=%d "
                      "pending=%d wire=%d poisoned=%d in_frame=%d",
                      (int)request->command_length, request->command,
                      (unsigned long)request->command_length, (int)s_transport.client.phase,
                      s_transport.client.tainted, s_transport.client.result_ready,
                      (int)s_transport.client.suppress_ipd_ok, s_transport.client.command_pending,
                      (int)s_transport.client.wire_mode, s_transport.poisoned,
                      at_parser_in_frame(&s_transport.parser));
        finish_request(AT_RESULT_PROTOCOL);
        return;
    }
    size_t sent = 0U;
    uint32_t deadline = request->started_ms + request->timeout_ms;
    if (!transmit_bytes((const uint8_t *)command, command_length, deadline, &sent) ||
        !at_uart_policy_tx_complete(command_length, sent, true)) {
        at_client_fail(&s_transport.client, AT_RESULT_IO);
        set_poison(ISR_FAULT_NOISE);
    }
}

static void start_pending_request(void)
{
    if (!s_transport.request_active || s_transport.request_started || s_transport.poisoned ||
        s_transport.client.suppress_ipd_ok || at_parser_in_frame(&s_transport.parser)) {
        return;
    }
    /* begin_request() can finish synchronously; mark started first so that a
     * synchronous failure does not leave this request eligible for restart. */
    s_transport.request_started = true;
    begin_request(&s_transport.request);
}

static void handle_client_result(void)
{
    at_result_t result;
    if (at_client_take_result(&s_transport.client, &result)) {
        if (result != AT_RESULT_OK && result != AT_RESULT_ERROR) {
            int cmd_len = (int)s_transport.request.command_length;
            if (cmd_len >= 2) cmd_len -= 2;
            AT_SUMMARY("AT transaction failed result=%d cmd=%.*s payload=%lu prompt=%d sent=%d send_ok=%d",
                          (int)result, cmd_len, s_transport.request.command,
                          (unsigned long)s_transport.request.payload_length,
                          s_transport.request.prompt_seen ? 1 : 0,
                          s_transport.request.payload_sent ? 1 : 0,
                          s_transport.request.saw_send_ok ? 1 : 0);
            rx_trace_dump("RX FAIL", (int)result);
            set_poison(result == AT_RESULT_TIMEOUT ? ISR_FAULT_CALLER_TIMEOUT : ISR_FAULT_NOISE);
        }
        if (!at_uart_policy_payload_result(s_transport.request.payload_length, s_transport.request.prompt_seen,
                                              s_transport.request.payload_sent, s_transport.request.saw_send_ok,
                                              s_transport.request.saw_send_fail,
                                              result == AT_RESULT_OK)) {
            AT_SUMMARY("payload result mismatch cmd=%.*s ok=%d prompt=%d sent=%d send_ok=%d send_fail=%d "
                          "reply=%.*s",
                          (int)s_transport.request.command_length, s_transport.request.command,
                          result == AT_RESULT_OK, s_transport.request.prompt_seen,
                          s_transport.request.payload_sent, s_transport.request.saw_send_ok,
                          s_transport.request.saw_send_fail, (int)s_transport.request.reply_length,
                          s_transport.request.reply);
            rx_trace_dump("RX TRACE", (int)result);
            result = AT_RESULT_PROTOCOL;
            set_poison(ISR_FAULT_NOISE);
        }
        if (s_transport.request.reply_overflow) {
            result = AT_RESULT_OVERFLOW;
            set_poison(ISR_FAULT_RX_OVERFLOW);
        }
        finish_request(result);
    }
}

static void log_transport_high_water(const char *tag)
{
    at_event_queue_stats_t queue_stats;
    at_event_queue_get_stats(s_transport.event_queue, &queue_stats);
    AT_SUMMARY("%s rx_stream=%lu/%u transport_burst=%lu events=%lu/%u data_slots=%lu/%u "
                  "slot_exhausted=%lu line_slots=%lu/%u line_exhausted=%lu rx_overflow=%lu",
                  tag, (unsigned long)s_transport.rx_stream_high_water, (unsigned)AT_RX_STREAM_SIZE,
                  (unsigned long)at_transport_rx_burst_high_water(s_transport.transport),
                  (unsigned long)queue_stats.event_queue_high_water, (unsigned)AT_EVENT_QUEUE_DEPTH,
                  (unsigned long)queue_stats.data_slot_high_water, (unsigned)AT_EVENT_DATA_SLOTS,
                  (unsigned long)queue_stats.data_slot_exhausted,
                  (unsigned long)queue_stats.line_slot_high_water, (unsigned)AT_EVENT_LINE_SLOTS,
                  (unsigned long)queue_stats.line_slot_exhausted,
                  (unsigned long)s_transport.rx_stream_overflow);
}

static void process_poison(void)
{
    if (!s_transport.poisoned || s_transport.poison_processed) {
        return;
    }
    s_transport.poison_processed = true;
    at_result_t reason = poison_result();
    AT_SUMMARY("transport poison reason=%d faults=0x%08lX", (int)reason,
                  (unsigned long)s_transport.isr_fault);
    log_transport_high_water("transport high-water");
    at_transport_suspend(s_transport.transport);
    at_transport_flush(s_transport.transport);
    (void)xStreamBufferReset(s_transport.rx_stream);
    if (s_transport.client.result_ready) {
        at_result_t ignored;
        (void)at_client_take_result(&s_transport.client, &ignored);
    }
    at_client_fail(&s_transport.client, reason);
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
    const at_client_events_t events = {
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
    at_client_init(&s_transport.client, &events, NULL);
    at_callbacks_t parser_callbacks = at_client_parser_callbacks();
    at_parser_init(&s_transport.parser, &parser_callbacks, &s_transport.client);

    uint8_t bytes[AT_RX_READ_SIZE];
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
                    !at_client_set_wire_mode(&s_transport.client, AT_WIRE_ATOMIC_IPD_TRAILER)) {
                    s_transport.capability_negotiated = false;
                    finish_request(AT_RESULT_PROTOCOL);
                } else {
                    finish_request(AT_RESULT_OK);
                }
            }
        }
        start_pending_request();
        const size_t stream_used = xStreamBufferBytesAvailable(s_transport.rx_stream);
        if (stream_used > s_transport.rx_stream_high_water) {
            s_transport.rx_stream_high_water = (uint32_t)stream_used;
        }
        size_t count = xStreamBufferReceive(s_transport.rx_stream, bytes, sizeof(bytes), pdMS_TO_TICKS(AT_IO_POLL_MS));
        if (count) {
            s_transport.rx_last_byte_ms = now_ms();
        }
        if (count && !s_transport.poisoned) {
            rx_trace_append(bytes, count);
            at_parser_feed(&s_transport.parser, bytes, count);
        }
        if (at_parser_in_frame(&s_transport.parser) &&
            (uint32_t)(now_ms() - s_transport.rx_last_byte_ms) >= AT_FRAME_IDLE_TIMEOUT_MS) {
            AT_SUMMARY("AT frame idle timeout fd=%d announced=%lu consumed=%lu remaining=%lu",
                          s_transport.parser.remote_fd,
                          (unsigned long)s_transport.parser.frame_length,
                          (unsigned long)(s_transport.parser.frame_length - s_transport.parser.remaining),
                          (unsigned long)s_transport.parser.remaining);
            rx_trace_dump("RX PARTIAL", -1);
            at_parser_drop_frame(&s_transport.parser);
        }
        start_pending_request();
        at_client_poll(&s_transport.client, now_ms());
        process_poison();
        handle_client_result();
        const uint32_t request_wait_ms = (uint32_t)(now_ms() - s_transport.request.started_ms);
        if (at_uart_policy_request_frame_hold_expired(
                s_transport.request_active, s_transport.request_started, s_transport.poisoned,
                at_parser_in_frame(&s_transport.parser), request_wait_ms,
                AT_REQUEST_FRAME_HOLD_MS)) {
            AT_SUMMARY("AT request frame hold expired wait=%lums limit=%lums",
                          (unsigned long)request_wait_ms,
                          (unsigned long)AT_REQUEST_FRAME_HOLD_MS);
            set_poison(ISR_FAULT_NOISE);
            finish_request(AT_RESULT_PROTOCOL);
        }
    }
    if (s_transport.request_active) {
        at_client_fail(&s_transport.client, AT_RESULT_CANCELLED);
        handle_client_result();
        if (s_transport.request_active) {
            finish_request(AT_RESULT_CANCELLED);
        }
    }
    xSemaphoreGive(s_transport.tasks_exited);
    vTaskDelete(NULL);
}

static void release_unstarted_primitives(void)
{
    if (s_transport.transport) {
        at_transport_close(s_transport.transport);
    }
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

at_status_t at_session_init(const at_session_events_t *events, void *context)
{
    AT_SUMMARY("AT session init");
    if (s_transport.initialized || s_init_failed_with_live_worker) {
        return AT_STATUS_IO_ERROR;
    }
    memset(&s_transport, 0, sizeof(s_transport));
    s_transport.transport = at_transport_get();
    if (!s_transport.transport) {
        return AT_STATUS_IO_ERROR;
    }
    s_transport.transport->rx_callback = transport_rx;
    s_transport.transport->rx_context = NULL;
    s_transport.transport->rx_ready_callback = transport_rx_ready;
    s_transport.transport->rx_ready_context = NULL;
    s_transport.transport->rx_ready_callback = transport_rx_ready;
    s_transport.transport->rx_ready_context = NULL;
    s_transport.transport->fault_callback = transport_fault;
    s_transport.transport->fault_context = NULL;
    if (events) {
        s_transport.events = *events;
    }
    s_transport.event_context = context;
    bool has_events = events_configured(events);
    s_transport.rx_stream = xStreamBufferCreate(AT_RX_STREAM_SIZE, 1U);
    s_transport.request_queue = xQueueCreate(1U, sizeof(request_t *));
    if (has_events) {
        s_transport.event_queue = pvPortMalloc(sizeof(*s_transport.event_queue));
        if (s_transport.event_queue) {
            at_event_queue_init(s_transport.event_queue);
            AT_SUMMARY("event queue pool bytes=%u free_heap=%u",
                          (unsigned)sizeof(*s_transport.event_queue), (unsigned)xPortGetFreeHeapSize());
        } else {
            AT_SUMMARY("event queue pool allocation failed bytes=%u",
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
        return AT_STATUS_IO_ERROR;
    }
    if (at_transport_open(s_transport.transport) != AT_TRANSPORT_OK) {
        release_unstarted_primitives();
        return AT_STATUS_IO_ERROR;
    }
    if (xTaskCreate(rx_task, "at_rx", AT_RX_TASK_STACK, NULL, AT_TASK_PRIORITY, &s_transport.rx_task) != pdPASS) {
        release_unstarted_primitives();
        return AT_STATUS_IO_ERROR;
    }
    if (has_events && xTaskCreate(event_task, "at_evt", AT_EVENT_TASK_STACK, NULL, AT_TASK_PRIORITY,
                                  &s_transport.event_task) != pdPASS) {
        s_transport.stopping = true;
        if (xSemaphoreTake(s_transport.tasks_exited, pdMS_TO_TICKS(1000U)) == pdTRUE) {
            release_unstarted_primitives();
        } else {
            s_init_failed_with_live_worker = true;
        }
        return AT_STATUS_IO_ERROR;
    }
    s_transport.initialized = true;
    AT_SUMMARY("AT session ready transport=%s rx_stream=%u event_depth=%u data_slots=%u line_slots=%u",
                  s_transport.transport->ops->name, (unsigned)AT_RX_STREAM_SIZE,
                  (unsigned)AT_EVENT_QUEUE_DEPTH, (unsigned)AT_EVENT_DATA_SLOTS,
                  (unsigned)AT_EVENT_LINE_SLOTS);
    return AT_STATUS_OK;
}

static int32_t submit_locked(const char *command, size_t length, const void *payload, size_t payload_length,
                             bool wait_prompt, uint32_t timeout_ms, bool negotiate,
                             const at_capabilities_t *capabilities, at_result_t *result)
{
    if (s_transport.poisoned) {
        return AT_STATUS_IO_ERROR;
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
        return AT_STATUS_IO_ERROR;
    }
    if (xSemaphoreTake(s_transport.result_ready, pdMS_TO_TICKS(timeout_ms + AT_IO_POLL_MS + 2U)) != pdTRUE) {
        AT_SUMMARY("AT transaction timeout cmd=%.*s wait_prompt=%d payload=%lu timeout=%lums poisoned=%d",
                  (int)length, command, wait_prompt ? 1 : 0, (unsigned long)payload_length,
                  (unsigned long)timeout_ms, s_transport.poisoned ? 1 : 0);
        set_poison(ISR_FAULT_CALLER_TIMEOUT);
        *result = AT_RESULT_TIMEOUT;
        return AT_STATUS_IO_ERROR;
    }
    *result = s_transport.result;
    if (*result != AT_RESULT_OK) {
        bool credential = request->command_length >= 13U &&
                          (memcmp(request->command, "AT+CWJAP_CUR", 13U) == 0 ||
                           memcmp(request->command, "AT+CWSAP_CUR", 13U) == 0);
        if (credential) {
            AT_SUMMARY("AT failed credential cmd result=%d prompt=%d payload_sent=%d phase=%d",
                          (int)*result, s_transport.request.prompt_seen ? 1 : 0,
                          s_transport.request.payload_sent ? 1 : 0, (int)s_transport.client.phase);
        } else {
            AT_SUMMARY("AT failed cmd=%.*s result=%d prompt=%d payload_sent=%d phase=%d reply=%.*s",
                          (int)request->command_length, request->command, (int)*result,
                          s_transport.request.prompt_seen ? 1 : 0, s_transport.request.payload_sent ? 1 : 0,
                          (int)s_transport.client.phase, (int)s_transport.request.reply_length,
                          s_transport.request.reply);
        }
    }
    return AT_STATUS_OK;
}

static int32_t execute_locked(const at_request_t *request, at_result_t *result)
{
    if (!request || !request->command || !request->command_length ||
        request->command_length > AT_COMMAND_SIZE || !request->timeout_ms || !result ||
        request->timeout_ms >= UINT32_C(0x80000000) - AT_IO_POLL_MS ||
        !at_uart_policy_payload_valid(request->wait_prompt, request->payload,
                                         request->payload_length, AT_SESSION_MAX_PAYLOAD)) {
        return AT_STATUS_INVALID_PARAM;
    }
    if (!s_transport.capability_negotiated &&
        !at_uart_policy_command_supported(request->command, request->command_length, request->wait_prompt)) {
        return AT_STATUS_UNSUPPORTED;
    }
    if (!s_transport.capability_negotiated && request->payload_length) {
        return AT_STATUS_UNSUPPORTED;
    }
    if (s_transport.capability_negotiated && request->payload_length > s_transport.capabilities.max_send) {
        return AT_STATUS_UNSUPPORTED;
    }
    if (request->reply_capacity && !request->reply) {
        return AT_STATUS_INVALID_PARAM;
    }
    const char *command = request->command;
    if (request->command_length >= 13U &&
        (memcmp(command, "AT+CWJAP_CUR", 13U) == 0 || memcmp(command, "AT+CWSAP_CUR", 13U) == 0)) {
        AT_LOG("AT TX credential command (arguments redacted)");
    } else {
        AT_LOG("AT TX %.*s timeout=%lums", (int)request->command_length, command,
                  (unsigned long)request->timeout_ms);
    }
    at_status_t status = submit_locked(request->command, request->command_length, request->payload,
                                   request->payload_length, request->wait_prompt, request->timeout_ms,
                                   false, NULL, result);
    AT_LOG("AT RX status=%ld result=%d reply_len=%lu", (long)status, (int)*result,
              (unsigned long)s_transport.request.reply_length);
    if (status != AT_STATUS_OK) {
        return status;
    }
    if (request->event_watermark) {
        *request->event_watermark = s_transport.request.event_watermark;
    }
    size_t needed = s_transport.request.reply_length;
    if (!request->reply && !request->reply_capacity && !request->reply_length) {
        return AT_STATUS_OK;
    }
    if (request->reply_length) {
        *request->reply_length = needed;
    }
    if (!at_uart_policy_reply_fits(request->reply_capacity, needed)) {
        return AT_STATUS_NO_BUFFER;
    }
    if (needed) {
        memcpy(request->reply, s_transport.request.reply, needed);
    }
    if (request->reply) {
        request->reply[needed] = '\0';
    }
    return AT_STATUS_OK;
}

at_status_t at_session_execute(const at_request_t *request, at_result_t *result)
{
    if (!s_transport.initialized || !request || !result || !request->timeout_ms ||
        request->timeout_ms >= UINT32_C(0x80000000) - AT_IO_POLL_MS) {
        return AT_STATUS_INVALID_PARAM;
    }
    if (xSemaphoreTake(s_transport.caller_mutex, pdMS_TO_TICKS(request->timeout_ms)) != pdTRUE) {
        return AT_STATUS_IO_ERROR;
    }
    at_status_t status = s_transport.initialized && !s_transport.stopping ? execute_locked(request, result) : AT_STATUS_IO_ERROR;
    xSemaphoreGive(s_transport.caller_mutex);
    return status;
}

at_status_t at_session_recover(void)
{
    if (!s_transport.initialized || !s_transport.rx_stream || !s_transport.request_queue) {
        return AT_STATUS_NOT_READY;
    }
    if (xSemaphoreTake(s_transport.caller_mutex, pdMS_TO_TICKS(AT_IO_POLL_MS)) != pdTRUE) {
        return AT_STATUS_NOT_READY;
    }
    if (s_transport.request_active) {
        xSemaphoreGive(s_transport.caller_mutex);
        return AT_STATUS_NOT_READY;
    }

    at_transport_suspend(s_transport.transport);
    at_transport_flush(s_transport.transport);
    (void)xStreamBufferReset(s_transport.rx_stream);

    const at_client_events_t events = {
        .line = client_line,
        .data = client_data,
        .ble_data = client_ble_data,
        .prompt = client_prompt,
        .fault = client_fault,
        .datagram = client_datagram,
        .accept = client_accept,
        .accept_v2 = client_accept_v2,
        .close_v2 = client_close_v2};
    at_client_init(&s_transport.client, &events, NULL);
    at_callbacks_t parser_callbacks = at_client_parser_callbacks();
    at_parser_init(&s_transport.parser, &parser_callbacks, &s_transport.client);

    taskENTER_CRITICAL();
    s_transport.poisoned = false;
    s_transport.poison_processed = false;
    s_transport.isr_fault = 0U;
    s_transport.capability_negotiated = false;
    memset(&s_transport.capabilities, 0, sizeof(s_transport.capabilities));
    taskEXIT_CRITICAL();

    (void)at_transport_resume(s_transport.transport);
    xSemaphoreGive(s_transport.caller_mutex);
    return AT_STATUS_OK;
}

at_status_t at_session_transact(const char *command, size_t length, bool wait_prompt,
                                      uint32_t timeout_ms, at_result_t *result)
{
    at_request_t request = {
        .command = command, .command_length = length, .wait_prompt = wait_prompt, .timeout_ms = timeout_ms};
    return at_session_execute(&request, result);
}

at_status_t at_session_negotiate(uint32_t timeout_ms)
{
    static const char command[] = AT_WIRE_CAPABILITY_QUERY;
    at_result_t result = AT_RESULT_NONE;
    at_capabilities_t capabilities;
    if (!s_transport.initialized || !timeout_ms ||
        timeout_ms >= UINT32_C(0x80000000) - AT_IO_POLL_MS) {
        return AT_STATUS_INVALID_PARAM;
    }
    if (xSemaphoreTake(s_transport.caller_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return AT_STATUS_IO_ERROR;
    }
    if (s_transport.capability_negotiated) {
        xSemaphoreGive(s_transport.caller_mutex);
        return AT_STATUS_OK;
    }
    at_status_t status = submit_locked(command, sizeof(command) - 1U, NULL, 0U, false, timeout_ms,
                                   false, NULL, &result);
    AT_LOG("CAP query status=%ld result=%d", (long)status, (int)result);
    if (status == AT_STATUS_OK && result == AT_RESULT_OK &&
        at_uart_policy_network_capabilities_v2(s_transport.request.reply,
                                                  s_transport.request.reply_length, &capabilities)) {
        s_transport.request.reply_length = 0U;
        status = submit_locked(command, sizeof(command) - 1U, NULL, 0U, false, timeout_ms,
                               true, &capabilities, &result);
        if (status == AT_STATUS_OK && result == AT_RESULT_OK) {
            s_transport.capability_negotiated = true;
            s_transport.capabilities = capabilities;
            AT_LOG("CAP v%u max_clients=%u max_servers=%u max_send=%u udp_send_id=%u",
                      (unsigned)capabilities.version, (unsigned)capabilities.max_clients,
                      (unsigned)capabilities.max_servers, (unsigned)capabilities.max_send,
                      (unsigned)capabilities.udp_send_id);
        }
    } else if (status == AT_STATUS_OK && result == AT_RESULT_ERROR) {
        /* Some stock firmware rejects the capability query. Its standard CIPMUX=1 client
         * sockets remain fully usable; the extended server/accept features
         * are simply unavailable. Use the trailer-aware parser mode so the
         * bare OK emitted after each stock +IPD frame is not mistaken for a
         * command result. */
        if (!at_client_set_wire_mode(&s_transport.client, AT_WIRE_ATOMIC_IPD_TRAILER)) {
            status = AT_STATUS_IO_ERROR;
        } else {
            at_capabilities_t stock = {.version = 0U, .max_clients = 8U,
                                               .max_send = 2048U, .udp_send_id = 8U};
            s_transport.capability_negotiated = true;
            s_transport.capabilities = stock;
            status = AT_STATUS_OK;
            AT_LOG("stock AT mode max_clients=%u max_send=%u", (unsigned)stock.max_clients,
                      (unsigned)stock.max_send);
        }
    } else if (status == AT_STATUS_OK) {
        status = AT_STATUS_UNSUPPORTED;
    }
    xSemaphoreGive(s_transport.caller_mutex);
    return status;
}

at_status_t at_session_get_capabilities(at_capabilities_t *capabilities)
{
    if (!s_transport.initialized || !capabilities) {
        return AT_STATUS_INVALID_PARAM;
    }
    if (xSemaphoreTake(s_transport.caller_mutex, pdMS_TO_TICKS(AT_IO_POLL_MS)) != pdTRUE) {
        return AT_STATUS_IO_ERROR;
    }
    if (!s_transport.capability_negotiated) {
        xSemaphoreGive(s_transport.caller_mutex);
        return AT_STATUS_UNSUPPORTED;
    }
    *capabilities = s_transport.capabilities;
    xSemaphoreGive(s_transport.caller_mutex);
    return AT_STATUS_OK;
}

at_status_t at_session_wait_events(uint32_t watermark, uint32_t timeout_ms)
{
    if (!s_transport.initialized || !timeout_ms ||
        timeout_ms >= UINT32_C(0x80000000) - AT_IO_POLL_MS) {
        return AT_STATUS_INVALID_PARAM;
    }
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (!timeout_ticks) {
        timeout_ticks = 1U;
    }
    while (!at_uart_policy_sequence_reached(s_transport.event_dispatched, watermark)) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= timeout_ticks) {
            return AT_STATUS_TIMEOUT;
        }
        if (xSemaphoreTake(s_transport.event_progress, timeout_ticks - elapsed) != pdTRUE &&
            !at_uart_policy_sequence_reached(s_transport.event_dispatched, watermark)) {
            return AT_STATUS_TIMEOUT;
        }
    }
    return AT_STATUS_OK;
}

at_status_t at_session_get_stats(at_session_stats_t *stats)
{
    if (!stats || !s_transport.initialized || !s_transport.event_queue) {
        return AT_STATUS_INVALID_PARAM;
    }
    at_event_queue_stats_t queue_stats;
    at_event_queue_get_stats(s_transport.event_queue, &queue_stats);
    *stats = (at_session_stats_t){
        .rx_stream_high_water = s_transport.rx_stream_high_water,
        .rx_stream_capacity = AT_RX_STREAM_SIZE,
        .transport_rx_burst_high_water =
            at_transport_rx_burst_high_water(s_transport.transport),
        .event_queue_high_water = queue_stats.event_queue_high_water,
        .event_queue_capacity = AT_EVENT_QUEUE_DEPTH,
        .data_slot_high_water = queue_stats.data_slot_high_water,
        .data_slot_capacity = AT_EVENT_DATA_SLOTS,
        .data_slot_exhausted = queue_stats.data_slot_exhausted,
        .line_slot_high_water = queue_stats.line_slot_high_water,
        .line_slot_capacity = AT_EVENT_LINE_SLOTS,
        .line_slot_exhausted = queue_stats.line_slot_exhausted,
        .rx_stream_overflow = s_transport.rx_stream_overflow,
    };
    return AT_STATUS_OK;
}

bool at_session_is_poisoned(void)
{
    return s_transport.poisoned;
}

