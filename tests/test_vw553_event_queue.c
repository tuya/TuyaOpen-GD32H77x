/**
 * @file test_vw553_event_queue.c
 * @brief Stress the ordered VW553 event queue during concurrent IPD traffic.
 */
#include "vw553_at_client.h"
#include "vw553_event_queue.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST_MAX_PAYLOAD 4096U
#define TEST_MAX_STREAM  16384U

typedef struct {
    vw553_event_queue_t queue;
} capture_t;

static uint32_t crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_C(0xffffffff);
    for (size_t i = 0U; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}

static bool push_event(capture_t *capture, const vw553_queued_event_t *event)
{
    return vw553_event_queue_enqueue(&capture->queue, event);
}

static void line_event(void *context, const char *line, size_t length)
{
    capture_t *capture = context;
    assert(length <= VW553_EVENT_LINE_SLOT_SIZE);
    char *slot = vw553_event_queue_line_acquire(&capture->queue);
    assert(slot != NULL);
    const uint8_t line_slot = vw553_event_queue_slot_for_line(&capture->queue, slot);
    assert(line_slot != VW553_EVENT_LINE_SLOT_NONE);
    memcpy(slot, line, length);
    vw553_queued_event_t event = {.kind = VW553_EVENT_LINE, .length = length,
                                  .data_slot = VW553_EVENT_DATA_SLOT_NONE, .line_slot = line_slot};
    assert(push_event(capture, &event));
}

static void data_event(void *context, int remote_fd, const uint8_t *data, size_t length, bool final)
{
    capture_t *capture = context;
    size_t offset = 0U;
    do {
        vw553_queued_event_t event = {.kind = VW553_EVENT_DATA, .remote_fd = remote_fd,
                                      .data_slot = VW553_EVENT_DATA_SLOT_NONE,
                                      .line_slot = VW553_EVENT_LINE_SLOT_NONE};
        size_t chunk = length - offset;
        if (chunk > VW553_EVENT_DATA_SLOT_SIZE) {
            chunk = VW553_EVENT_DATA_SLOT_SIZE;
        }
        if (chunk) {
            uint8_t *slot = vw553_event_queue_data_acquire(&capture->queue);
            assert(slot != NULL);
            event.data_slot = vw553_event_queue_slot_for_data(&capture->queue, slot);
            assert(event.data_slot != VW553_EVENT_DATA_SLOT_NONE);
            memcpy(slot, data + offset, chunk);
        }
        event.length = chunk;
        event.final = final && offset + chunk == length;
        assert(push_event(capture, &event));
        offset += chunk;
    } while (offset < length);
}

static void prompt_event(void *context)
{
    capture_t *capture = context;
    const vw553_queued_event_t event = {.kind = VW553_EVENT_PROMPT,
                                        .data_slot = VW553_EVENT_DATA_SLOT_NONE};
    assert(push_event(capture, &event));
}

static void fault_event(void *context, vw553_at_fault_t fault)
{
    capture_t *capture = context;
    const vw553_queued_event_t event = {.kind = VW553_EVENT_FAULT, .fault = fault,
                                        .data_slot = VW553_EVENT_DATA_SLOT_NONE};
    assert(push_event(capture, &event));
}

static void fill_payload(uint8_t *payload, size_t length, uint8_t seed, const char *marker)
{
    uint32_t state = UINT32_C(0x9e3779b9) ^ seed;
    for (size_t i = 0U; i < length; ++i) {
        state = state * UINT32_C(1664525) + UINT32_C(1013904223);
        payload[i] = (uint8_t)(state >> 24);
    }
    static const char embedded[] = "\r\nOK\r\nERROR\r\nAT+CIPSEND=0,67\r\nSEND OK\r\n";
    size_t marker_length = strlen(marker);
    assert(marker_length <= length);
    memcpy(payload + length / 4U, marker, marker_length);
    if (length > sizeof(embedded) + 32U) {
        memcpy(payload + length - sizeof(embedded), embedded, sizeof(embedded) - 1U);
    }
}

static size_t append_frame(uint8_t *stream, size_t used, int fd, const uint8_t *payload, size_t length)
{
    int header = snprintf((char *)stream + used, TEST_MAX_STREAM - used, "+IPD,%d,%lu: ", fd,
                          (unsigned long)length);
    assert(header > 0);
    used += (size_t)header;
    assert(used + length < TEST_MAX_STREAM);
    memcpy(stream + used, payload, length);
    return used + length;
}

static bool event_is_line(vw553_event_queue_t *queue, const vw553_queued_event_t *event, const char *line)
{
    const size_t length = strlen(line);
    const char *text = vw553_event_queue_line(queue, event->line_slot);
    return event->kind == VW553_EVENT_LINE && text != NULL && event->length == length &&
           !memcmp(text, line, length);
}

static void stress_delayed_consumer(void)
{
    capture_t capture;
    vw553_event_queue_init(&capture.queue);

    const vw553_at_client_events_t client_events = {
        .line = line_event,
        .data = data_event,
        .prompt = prompt_event,
        .fault = fault_event,
    };
    vw553_at_client_t client;
    vw553_at_parser_t parser;
    vw553_at_client_init(&client, &client_events, &capture);
    assert(vw553_at_client_set_wire_mode(&client, VW553_AT_WIRE_ATOMIC_NO_IPD_TRAILER));
    const vw553_at_callbacks_t callbacks = vw553_at_client_parser_callbacks();
    vw553_at_parser_init(&parser, &callbacks, &client);

    assert(vw553_at_client_begin(&client, "AT+CIPSEND=0,67", 15U, true, 10U, 700U));
    char command[VW553_AT_COMMAND_SIZE + 2U];
    size_t command_length = 0U;
    assert(vw553_at_client_take_command(&client, command, sizeof(command), &command_length));

    uint8_t first[2437];
    uint8_t second[2920];
    fill_payload(first, sizeof(first), 1U, "AT+CIPSEND=first");
    fill_payload(second, sizeof(second), 2U, "ERROR/OK/second");

    uint8_t stream[TEST_MAX_STREAM];
    size_t used = 0U;
    memcpy(stream, "> ", 2U);
    used = 2U;
    for (unsigned round = 0U; round < 2U; ++round) {
        used = append_frame(stream, used, 1, first, sizeof(first));
        used = append_frame(stream, used, 2, second, sizeof(second));
    }
    const size_t ipd_stream_end = used;
    memcpy(stream + used, "SEND OK\r\nOK\r\n", sizeof("SEND OK\r\nOK\r\n") - 1U);
    used += sizeof("SEND OK\r\nOK\r\n") - 1U;

    /* Do not consume events until every frame has arrived. The command must
     * remain pending while the two socket bursts and the delayed consumer are
     * both present. */
    for (size_t offset = 0U; offset < ipd_stream_end; offset += 2048U) {
        size_t count = ipd_stream_end - offset;
        if (count > 2048U) {
            count = 2048U;
        }
        vw553_at_parser_feed(&parser, stream + offset, count);
    }
    assert(vw553_at_client_busy(&client));
    vw553_at_parser_feed(&parser, stream + ipd_stream_end, used - ipd_stream_end);
    assert(!vw553_at_client_busy(&client));
    vw553_at_result_t result = VW553_AT_RESULT_NONE;
    assert(vw553_at_client_take_result(&client, &result));
    assert(result == VW553_AT_RESULT_OK);

    uint8_t received_first[2U * sizeof(first)];
    uint8_t received_second[2U * sizeof(second)];
    size_t first_used = 0U;
    size_t second_used = 0U;
    unsigned data_events = 0U;
    unsigned prompt_events = 0U;
    unsigned send_ok_lines = 0U;
    unsigned ok_lines = 0U;
    uint32_t previous_sequence = 0U;

    vw553_queued_event_t event;
    while (vw553_event_queue_dequeue(&capture.queue, &event)) {
        assert(event.sequence > previous_sequence);
        previous_sequence = event.sequence;
        if (event.kind == VW553_EVENT_PROMPT) {
            ++prompt_events;
        } else if (event.kind == VW553_EVENT_LINE) {
            send_ok_lines += event_is_line(&capture.queue, &event, "SEND OK");
            ok_lines += event_is_line(&capture.queue, &event, "OK");
            vw553_event_queue_line_release(&capture.queue, event.line_slot);
        } else if (event.kind == VW553_EVENT_DATA) {
            const uint8_t *data = vw553_event_queue_data(&capture.queue, event.data_slot);
            assert(data != NULL);
            if (event.remote_fd == 1) {
                assert(first_used + event.length <= sizeof(received_first));
                memcpy(received_first + first_used, data, event.length);
                first_used += event.length;
            } else {
                assert(event.remote_fd == 2);
                assert(second_used + event.length <= sizeof(received_second));
                memcpy(received_second + second_used, data, event.length);
                second_used += event.length;
            }
            vw553_event_queue_data_release(&capture.queue, event.data_slot);
            ++data_events;
        }
    }

    assert(prompt_events == 1U);
    assert(send_ok_lines == 1U && ok_lines == 1U);
    assert(data_events >= 4U);
    assert(first_used == sizeof(received_first));
    assert(second_used == sizeof(received_second));
    assert(!memcmp(received_first, first, sizeof(first)));
    assert(!memcmp(received_first + sizeof(first), first, sizeof(first)));
    assert(!memcmp(received_second, second, sizeof(second)));
    assert(!memcmp(received_second + sizeof(second), second, sizeof(second)));
    uint8_t expected_first[2U * sizeof(first)];
    uint8_t expected_second[2U * sizeof(second)];
    memcpy(expected_first, first, sizeof(first));
    memcpy(expected_first + sizeof(first), first, sizeof(first));
    memcpy(expected_second, second, sizeof(second));
    memcpy(expected_second + sizeof(second), second, sizeof(second));
    assert(crc32(received_first, sizeof(received_first)) == crc32(expected_first, sizeof(expected_first)));
    assert(crc32(received_second, sizeof(received_second)) == crc32(expected_second, sizeof(expected_second)));

    vw553_event_queue_stats_t stats;
    vw553_event_queue_get_stats(&capture.queue, &stats);
    assert(stats.event_queue_high_water > 0U);
    assert(stats.data_slot_high_water > 0U);
    assert(stats.data_slot_exhausted == 0U);
    assert(stats.line_slot_high_water > 0U);
    assert(stats.line_slot_exhausted == 0U);
    printf("ordered queue high-water: events=%u/%u data=%u/%u lines=%u/%u\n",
           stats.event_queue_high_water, VW553_EVENT_QUEUE_DEPTH,
           stats.data_slot_high_water, VW553_EVENT_DATA_SLOTS,
           stats.line_slot_high_water, VW553_EVENT_LINE_SLOTS);
}

static void slot_exhaustion_is_explicit(void)
{
    vw553_event_queue_t queue;
    vw553_event_queue_init(&queue);
    for (unsigned i = 0U; i < VW553_EVENT_DATA_SLOTS; ++i) {
        assert(vw553_event_queue_data_acquire(&queue) != NULL);
    }
    assert(vw553_event_queue_data_acquire(&queue) == NULL);
    vw553_event_queue_stats_t stats;
    vw553_event_queue_get_stats(&queue, &stats);
    assert(stats.data_slot_high_water == VW553_EVENT_DATA_SLOTS);
    assert(stats.data_slot_exhausted == 1U);
    for (unsigned i = 0U; i < VW553_EVENT_DATA_SLOTS; ++i) {
        vw553_event_queue_data_release(&queue, (uint8_t)i);
    }
    assert(vw553_event_queue_data_acquire(&queue) != NULL);
}

static void line_slot_exhaustion_is_explicit(void)
{
    vw553_event_queue_t queue;
    vw553_event_queue_init(&queue);
    for (unsigned i = 0U; i < VW553_EVENT_LINE_SLOTS; ++i) {
        assert(vw553_event_queue_line_acquire(&queue) != NULL);
    }
    assert(vw553_event_queue_line_acquire(&queue) == NULL);
    vw553_event_queue_stats_t stats;
    vw553_event_queue_get_stats(&queue, &stats);
    assert(stats.line_slot_high_water == VW553_EVENT_LINE_SLOTS);
    assert(stats.line_slot_exhausted == 1U);
    for (unsigned i = 0U; i < VW553_EVENT_LINE_SLOTS; ++i) {
        vw553_event_queue_line_release(&queue, (uint8_t)i);
    }
    assert(vw553_event_queue_line_acquire(&queue) != NULL);
}

static void queue_full_is_explicit(void)
{
    vw553_event_queue_t queue;
    vw553_event_queue_init(&queue);
    uint32_t previous_sequence = 0U;
    for (unsigned round = 0U; round < 3U; ++round) {
        for (unsigned i = 0U; i < VW553_EVENT_QUEUE_DEPTH; ++i) {
            const vw553_queued_event_t event = {.kind = VW553_EVENT_PROMPT,
                                                .data_slot = VW553_EVENT_DATA_SLOT_NONE,
                                                .line_slot = VW553_EVENT_LINE_SLOT_NONE};
            assert(vw553_event_queue_enqueue(&queue, &event));
        }
        const vw553_queued_event_t full = {.kind = VW553_EVENT_PROMPT,
                                           .data_slot = VW553_EVENT_DATA_SLOT_NONE,
                                           .line_slot = VW553_EVENT_LINE_SLOT_NONE};
        assert(!vw553_event_queue_enqueue(&queue, &full));
        assert(vw553_event_queue_pending(&queue) == VW553_EVENT_QUEUE_DEPTH);
        for (unsigned i = 0U; i < VW553_EVENT_QUEUE_DEPTH; ++i) {
            vw553_queued_event_t event;
            assert(vw553_event_queue_dequeue(&queue, &event));
            assert(event.sequence > previous_sequence);
            previous_sequence = event.sequence;
        }
        assert(vw553_event_queue_pending(&queue) == 0U);
    }
}

int main(void)
{
    stress_delayed_consumer();
    slot_exhaustion_is_explicit();
    line_slot_exhaustion_is_explicit();
    queue_full_is_explicit();
    puts("VW553 ordered event queue stress checks passed");
    return 0;
}
