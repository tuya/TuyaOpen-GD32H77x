/**
 * @file vw553_event_queue.h
 * @brief Bounded, ordered VW553 transport event queue.
 */
#ifndef VW553_EVENT_QUEUE_H
#define VW553_EVENT_QUEUE_H

#include "vw553_at_parser.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VW553_EVENT_QUEUE_DEPTH       32U
#define VW553_EVENT_DATA_SLOTS        12U
#define VW553_EVENT_DATA_SLOT_SIZE    2944U
#define VW553_EVENT_DATA_SLOT_NONE    UINT8_MAX
#define VW553_EVENT_LINE_SLOTS        31U
#define VW553_EVENT_LINE_SLOT_SIZE    VW553_AT_LINE_SIZE
#define VW553_EVENT_LINE_SLOT_NONE    UINT8_MAX

_Static_assert((VW553_EVENT_QUEUE_DEPTH & (VW553_EVENT_QUEUE_DEPTH - 1U)) == 0U,
               "event queue depth must be a power of two");
_Static_assert(VW553_EVENT_DATA_SLOTS <= 31U, "data slot bitmap supports at most 31 slots");
_Static_assert(VW553_EVENT_LINE_SLOTS <= 31U, "line slot bitmap supports at most 31 slots");
_Static_assert(VW553_EVENT_DATA_SLOT_SIZE >= 2920U,
               "one data slot must hold the largest VW553 +IPD payload");

typedef enum {
    VW553_EVENT_LINE,
    VW553_EVENT_DATA,
    VW553_EVENT_BLE_DATA,
    VW553_EVENT_PROMPT,
    VW553_EVENT_FAULT,
    VW553_EVENT_DATAGRAM,
    VW553_EVENT_ACCEPT,
    VW553_EVENT_ACCEPT_V2,
    VW553_EVENT_CLOSE_V2,
} vw553_event_kind_t;

typedef struct {
    vw553_event_kind_t kind;
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
    vw553_at_fault_t fault;
} vw553_queued_event_t;

typedef struct {
    uint32_t event_queue_high_water;
    uint32_t data_slot_high_water;
    uint32_t data_slot_exhausted;
    uint32_t line_slot_high_water;
    uint32_t line_slot_exhausted;
} vw553_event_queue_stats_t;

typedef struct {
    vw553_queued_event_t events[VW553_EVENT_QUEUE_DEPTH];
    uint8_t data[VW553_EVENT_DATA_SLOTS][VW553_EVENT_DATA_SLOT_SIZE] __attribute__((aligned(4)));
    char lines[VW553_EVENT_LINE_SLOTS][VW553_EVENT_LINE_SLOT_SIZE];
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
} vw553_event_queue_t;

void vw553_event_queue_init(vw553_event_queue_t *queue);
bool vw553_event_queue_enqueue(vw553_event_queue_t *queue, const vw553_queued_event_t *event);
bool vw553_event_queue_dequeue(vw553_event_queue_t *queue, vw553_queued_event_t *event);
uint8_t *vw553_event_queue_data_acquire(vw553_event_queue_t *queue);
void vw553_event_queue_data_release(vw553_event_queue_t *queue, uint8_t slot);
uint8_t *vw553_event_queue_data(vw553_event_queue_t *queue, uint8_t slot);
uint8_t vw553_event_queue_slot_for_data(vw553_event_queue_t *queue, const uint8_t *data);
char *vw553_event_queue_line_acquire(vw553_event_queue_t *queue);
void vw553_event_queue_line_release(vw553_event_queue_t *queue, uint8_t slot);
char *vw553_event_queue_line(vw553_event_queue_t *queue, uint8_t slot);
uint8_t vw553_event_queue_slot_for_line(vw553_event_queue_t *queue, const char *line);
void vw553_event_queue_get_stats(const vw553_event_queue_t *queue, vw553_event_queue_stats_t *stats);
uint32_t vw553_event_queue_pending(const vw553_event_queue_t *queue);

#endif
