/**
 * @file at_event_queue.c
 * @brief Bounded SPSC event queue with fixed binary-data slots.
 */
#include "at_event_queue.h"

#include <string.h>

static uint32_t load_acquire(const volatile uint32_t *value)
{
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static void store_release(volatile uint32_t *destination, uint32_t value)
{
    __atomic_store_n(destination, value, __ATOMIC_RELEASE);
}

void at_event_queue_init(at_event_queue_t *queue)
{
    if (!queue) {
        return;
    }
    memset(queue, 0, sizeof(*queue));
    queue->next_sequence = 1U;
}

uint32_t at_event_queue_pending(const at_event_queue_t *queue)
{
    if (!queue) {
        return 0U;
    }
    const uint32_t write = load_acquire(&queue->write_index);
    const uint32_t read = load_acquire(&queue->read_index);
    return write - read;
}

bool at_event_queue_enqueue(at_event_queue_t *queue, const at_queued_event_t *event)
{
    if (!queue || !event) {
        return false;
    }
    const uint32_t write = queue->write_index;
    const uint32_t read = load_acquire(&queue->read_index);
    if ((uint32_t)(write - read) >= AT_EVENT_QUEUE_DEPTH) {
        return false;
    }
    at_queued_event_t queued = *event;
    queued.sequence = queue->next_sequence++;
    queue->events[write & (AT_EVENT_QUEUE_DEPTH - 1U)] = queued;
    store_release(&queue->write_index, write + 1U);
    const uint32_t pending = write + 1U - read;
    if (pending > queue->event_queue_high_water) {
        queue->event_queue_high_water = pending;
    }
    return true;
}

bool at_event_queue_dequeue(at_event_queue_t *queue, at_queued_event_t *event)
{
    if (!queue || !event) {
        return false;
    }
    const uint32_t read = queue->read_index;
    const uint32_t write = load_acquire(&queue->write_index);
    if (read == write) {
        return false;
    }
    *event = queue->events[read & (AT_EVENT_QUEUE_DEPTH - 1U)];
    store_release(&queue->read_index, read + 1U);
    return true;
}

uint8_t *at_event_queue_data_acquire(at_event_queue_t *queue)
{
    if (!queue) {
        return NULL;
    }
    uint32_t expected = load_acquire(&queue->data_slot_used);
    for (;;) {
        const uint32_t available = (~expected) & ((UINT32_C(1) << AT_EVENT_DATA_SLOTS) - 1U);
        if (!available) {
            ++queue->data_slot_exhausted;
            return NULL;
        }
        const uint8_t slot = (uint8_t)__builtin_ctz(available);
        if (__atomic_compare_exchange_n(&queue->data_slot_used, &expected, expected | (UINT32_C(1) << slot),
                                        false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            const uint32_t used = (uint32_t)__builtin_popcount(expected | (UINT32_C(1) << slot));
            if (used > queue->data_slot_high_water) {
                queue->data_slot_high_water = used;
            }
            return queue->data[slot];
        }
    }
}

char *at_event_queue_line_acquire(at_event_queue_t *queue)
{
    if (!queue) {
        return NULL;
    }
    uint32_t expected = load_acquire(&queue->line_slot_used);
    for (;;) {
        const uint32_t available = (~expected) & ((UINT32_C(1) << AT_EVENT_LINE_SLOTS) - 1U);
        if (!available) {
            ++queue->line_slot_exhausted;
            return NULL;
        }
        const uint8_t slot = (uint8_t)__builtin_ctz(available);
        if (__atomic_compare_exchange_n(&queue->line_slot_used, &expected, expected | (UINT32_C(1) << slot),
                                        false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            const uint32_t used = (uint32_t)__builtin_popcount(expected | (UINT32_C(1) << slot));
            if (used > queue->line_slot_high_water) {
                queue->line_slot_high_water = used;
            }
            return queue->lines[slot];
        }
    }
}

void at_event_queue_line_release(at_event_queue_t *queue, uint8_t slot)
{
    if (!queue || slot >= AT_EVENT_LINE_SLOTS) {
        return;
    }
    (void)__atomic_fetch_and(&queue->line_slot_used, ~(UINT32_C(1) << slot), __ATOMIC_RELEASE);
}

char *at_event_queue_line(at_event_queue_t *queue, uint8_t slot)
{
    if (!queue || slot >= AT_EVENT_LINE_SLOTS) {
        return NULL;
    }
    return queue->lines[slot];
}

uint8_t at_event_queue_slot_for_line(at_event_queue_t *queue, const char *line)
{
    if (!queue || !line || line < queue->lines[0] || line >= queue->lines[AT_EVENT_LINE_SLOTS]) {
        return AT_EVENT_LINE_SLOT_NONE;
    }
    const size_t offset = (size_t)(line - queue->lines[0]);
    return (uint8_t)(offset / AT_EVENT_LINE_SLOT_SIZE);
}

void at_event_queue_data_release(at_event_queue_t *queue, uint8_t slot)
{
    if (!queue || slot >= AT_EVENT_DATA_SLOTS) {
        return;
    }
    (void)__atomic_fetch_and(&queue->data_slot_used, ~(UINT32_C(1) << slot), __ATOMIC_RELEASE);
}

uint8_t *at_event_queue_data(at_event_queue_t *queue, uint8_t slot)
{
    if (!queue || slot >= AT_EVENT_DATA_SLOTS) {
        return NULL;
    }
    return queue->data[slot];
}

uint8_t at_event_queue_slot_for_data(at_event_queue_t *queue, const uint8_t *data)
{
    if (!queue || !data || data < queue->data[0] || data >= queue->data[AT_EVENT_DATA_SLOTS]) {
        return AT_EVENT_DATA_SLOT_NONE;
    }
    const size_t offset = (size_t)(data - queue->data[0]);
    return (uint8_t)(offset / AT_EVENT_DATA_SLOT_SIZE);
}

void at_event_queue_get_stats(const at_event_queue_t *queue, at_event_queue_stats_t *stats)
{
    if (!stats) {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    if (!queue) {
        return;
    }
    stats->event_queue_high_water = queue->event_queue_high_water;
    stats->data_slot_high_water = queue->data_slot_high_water;
    stats->data_slot_exhausted = queue->data_slot_exhausted;
    stats->line_slot_high_water = queue->line_slot_high_water;
    stats->line_slot_exhausted = queue->line_slot_exhausted;
}
