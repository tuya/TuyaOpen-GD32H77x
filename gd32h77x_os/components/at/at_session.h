/**
 * @file session.h
 * @brief Transport-independent AT session and serialized transactions.
 *
 * This session is separate from the console transport. Current wire policy
 * starts in probe-only mode. A fixed read-only capability query is required
 * before any other command or prompt/payload transaction is accepted.
 */
#ifndef AT_UART_TRANSPORT_H
#define AT_UART_TRANSPORT_H

#include "at_client.h"
#include "at_profile.h"
#include "at_status.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AT_SESSION_MAX_PAYLOAD 2048U
#define AT_SESSION_REPLY_CAPACITY 2048U

typedef struct {
    /* Response lines are CRLF-delimited; reply_capacity includes trailing NUL,
     * while reply_length excludes it. Pass capacity >= expected bytes + 1. */
    const char *command;
    size_t command_length;
    const void *payload;
    size_t payload_length;
    bool wait_prompt;
    char *reply;
    size_t reply_capacity;
    size_t *reply_length;
    uint32_t *event_watermark;
    uint32_t timeout_ms;
} at_request_t;

typedef struct {
    uint32_t rx_stream_high_water;
    uint32_t rx_stream_capacity;
    uint32_t transport_rx_burst_high_water;
    uint32_t event_queue_high_water;
    uint32_t event_queue_capacity;
    uint32_t data_slot_high_water;
    uint32_t data_slot_capacity;
    uint32_t data_slot_exhausted;
    uint32_t line_slot_high_water;
    uint32_t line_slot_capacity;
    uint32_t line_slot_exhausted;
    uint32_t rx_stream_overflow;
} at_session_stats_t;

typedef struct {
    void (*line)(void *context, const char *line, size_t length);
    void (*data)(void *context, int remote_fd, const uint8_t *data, size_t length, bool final);
    void (*ble_data)(void *context, uint8_t conn_id, const uint8_t *data, size_t length, bool final);
    void (*prompt)(void *context);
    void (*fault)(void *context, at_fault_t reason);
    void (*datagram)(void *context, int listener_fd, const char *peer_ipv4, uint16_t peer_port,
                     const uint8_t *data, size_t length, bool final);
    void (*accept)(void *context, int listener_fd, int client_fd, const char *peer_ipv4, uint16_t peer_port);
    void (*accept_v2)(void *context, uint32_t con_id, uint32_t generation, int listener_fd,
                      uint32_t listener_generation, int lwip_fd, const char *peer_ipv4, uint16_t peer_port);
    void (*close_v2)(void *context, uint32_t con_id, int lwip_fd, uint32_t generation, const char *reason);
} at_session_events_t;

/* One transport instance is supported for the single attached AT module.
 * It has static runtime lifetime; there is deliberately no public deinit API.
 * Callbacks run asynchronously on the event task and may overlap the caller
 * after execute returns. They must not be used as synchronous query results.
 * Input command/payload bytes are copied before queueing; the worker never
 * retains caller pointers. A synchronous reply is copied to caller storage
 * only after completion. Use request.reply for synchronous query results.
 * The only transition out of probe-only mode is the fixed, validated
 * the profile capability query; callers cannot select a wire mode.
 */
at_status_t at_session_init(const at_session_events_t *events, void *context);
at_status_t at_session_execute(const at_request_t *request, at_result_t *result);
at_status_t at_session_recover(void);
at_status_t at_session_transact(const char *command, size_t length, bool wait_prompt,
                                      uint32_t timeout_ms, at_result_t *result);
at_status_t at_session_negotiate(uint32_t timeout_ms);
at_status_t at_session_get_capabilities(at_capabilities_t *capabilities);
at_status_t at_session_wait_events(uint32_t watermark, uint32_t timeout_ms);
at_status_t at_session_get_stats(at_session_stats_t *stats);
bool at_session_is_poisoned(void);
/* Dumps the most recent raw RX bytes; diagnostics only. */
void at_session_dump_rx_trace(const char *tag);

#endif
