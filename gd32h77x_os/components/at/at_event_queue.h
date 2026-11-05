/**
 * @file event_queue.h
 * @brief Bounded, ordered AT transport event queue.
 */
#ifndef AT_EVENT_QUEUE_H
#define AT_EVENT_QUEUE_H

#include "at_parser.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AT_EVENT_QUEUE_DEPTH       32U
#define AT_EVENT_DATA_SLOTS        12U
#define AT_EVENT_DATA_SLOT_SIZE    2944U
#define AT_EVENT_DATA_SLOT_NONE    UINT8_MAX
#define AT_EVENT_LINE_SLOTS        31U
#define AT_EVENT_LINE_SLOT_SIZE    AT_LINE_SIZE
#define AT_EVENT_LINE_SLOT_NONE    UINT8_MAX

_Static_assert((AT_EVENT_QUEUE_DEPTH & (AT_EVENT_QUEUE_DEPTH - 1U)) == 0U,
               "event queue depth must be a power of two");
_Static_assert(AT_EVENT_DATA_SLOTS <= 31U, "data slot bitmap supports at most 31 slots");
_Static_assert(AT_EVENT_LINE_SLOTS <= 31U, "line slot bitmap supports at most 31 slots");
_Static_assert(AT_EVENT_DATA_SLOT_SIZE >= 2920U,
               "one data slot must hold the largest AT +IPD payload");

typedef enum {
    AT_EVENT_LINE,
    AT_EVENT_DATA,
    AT_EVENT_BLE_DATA,
    AT_EVENT_PROMPT,
    AT_EVENT_FAULT,
    AT_EVENT_DATAGRAM,
    AT_EVENT_ACCEPT,
    AT_EVENT_ACCEPT_V2,
    AT_EVENT_CLOSE_V2,
} at_event_kind_t;

typedef struct {
    at_event_kind_t kind;
    uint32_t sequence;
    size_t length;
    int remote_fd;
    uint8_t conn_id;
    int listener_fd;
    int client_fd;
    int lwip_fd;
    uint32_t con_id;
    uint32_t generation;
    uint32_t listener_generation;
    char peer_ipv4[16];
    char reason[24];
    uint16_t peer_port;
    bool final;
    uint8_t data_slot;
    uint8_t line_slot;
    at_fault_t fault;
} at_queued_event_t;

typedef struct {
    uint32_t event_queue_high_water;
    uint32_t data_slot_high_water;
    uint32_t data_slot_exhausted;
    uint32_t line_slot_high_water;
    uint32_t line_slot_exhausted;
} at_event_queue_stats_t;

typedef struct {
    at_queued_event_t events[AT_EVENT_QUEUE_DEPTH];
    uint8_t data[AT_EVENT_DATA_SLOTS][AT_EVENT_DATA_SLOT_SIZE] __attribute__((aligned(4)));
    char lines[AT_EVENT_LINE_SLOTS][AT_EVENT_LINE_SLOT_SIZE];
    volatile uint32_t read_index;
    volatile uint32_t write_index;
    volatile uint32_t data_slot_used;
    volatile uint32_t line_slot_used;
    uint32_t next_sequence;
    uint32_t event_queue_high_water;
    uint32_t data_slot_high_water;
    uint32_t data_slot_exhausted;
    uint32_t line_slot_high_water;
    uint32_t line_slot_exhausted;
} at_event_queue_t;

void at_event_queue_init(at_event_queue_t *queue);
bool at_event_queue_enqueue(at_event_queue_t *queue, const at_queued_event_t *event);
bool at_event_queue_dequeue(at_event_queue_t *queue, at_queued_event_t *event);
uint8_t *at_event_queue_data_acquire(at_event_queue_t *queue);
void at_event_queue_data_release(at_event_queue_t *queue, uint8_t slot);
uint8_t *at_event_queue_data(at_event_queue_t *queue, uint8_t slot);
uint8_t at_event_queue_slot_for_data(at_event_queue_t *queue, const uint8_t *data);
char *at_event_queue_line_acquire(at_event_queue_t *queue);
void at_event_queue_line_release(at_event_queue_t *queue, uint8_t slot);
char *at_event_queue_line(at_event_queue_t *queue, uint8_t slot);
uint8_t at_event_queue_slot_for_line(at_event_queue_t *queue, const char *line);
void at_event_queue_get_stats(const at_event_queue_t *queue, at_event_queue_stats_t *stats);
uint32_t at_event_queue_pending(const at_event_queue_t *queue);

#endif
