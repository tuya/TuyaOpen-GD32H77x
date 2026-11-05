/**
 * @file client.h
 * @brief Allocation-free UART AT transaction state for the AT.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#ifndef AT_CLIENT_H
#define AT_CLIENT_H

#include "at_parser.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AT_COMMAND_SIZE 256U

typedef enum {
    AT_RESULT_NONE,
    AT_RESULT_OK,
    AT_RESULT_ERROR,
    AT_RESULT_TIMEOUT,
    AT_RESULT_CANCELLED,
    AT_RESULT_IO,
    AT_RESULT_RESET,
    AT_RESULT_OVERFLOW,
    AT_RESULT_PROTOCOL,
} at_result_t;

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
    /* A +IPD frame's trailing OK was overtaken by the next command prompt. */
    void (*ipd_trailer_dropped)(void *context);
} at_client_events_t;

typedef enum { AT_IDLE, AT_WAIT_PROMPT, AT_WAIT_FINAL } at_phase_t;
typedef enum {
    /* Default for unmodified 1.0.3g: safe only for idle AT liveness probe. */
    AT_WIRE_PROBE_ONLY,
    /* Trailer-aware IPD mode used by CAP2 and stock CIPMUX=1 firmware. Full-frame
     * atomicity for concurrent stock sockets requires the minimal 553 IPD patch. */
    AT_WIRE_ATOMIC_IPD_TRAILER,
    /* Patched firmware emits the complete IPD header/payload atomically, without a trailer. */
    AT_WIRE_ATOMIC_NO_IPD_TRAILER,
} at_wire_mode_t;

typedef struct {
    at_client_events_t events;
    void                    *context;
    char                     command[AT_COMMAND_SIZE + 2U];
    size_t                   command_length;
    uint32_t                 deadline;
    at_result_t        result;
    at_phase_t         phase;
    at_wire_mode_t     wire_mode;
    bool                     result_ready;
    uint8_t                  suppress_ipd_ok;
    uint8_t                  stale_ipd_ok_pending;
    bool                     saw_send_ok;
    bool                     plain_ok_payload;
    bool                     command_pending;
    bool                     tainted;
} at_client_t;

/* This state machine allocates no memory and contains no RTOS or UART calls.
 * The adapter must serialize calls briefly; it must never hold that guard while
 * waiting for a response or while transmitting bytes. Event callbacks must be
 * quick and nonblocking. `now` and timeout use the same wrapping uint32
 * millisecond clock; timeouts must be below 2^31 ms.
 */
void at_client_init(at_client_t *client, const at_client_events_t *events, void *context);
at_callbacks_t at_client_parser_callbacks(void);
bool at_client_begin(at_client_t *client, const char *command, size_t length, bool wait_prompt,
                           uint32_t now, uint32_t timeout_ms);
bool at_client_set_wire_mode(at_client_t *client, at_wire_mode_t mode);
/* Returns the complete command including CRLF once; no credentials are logged. */
bool at_client_take_command(at_client_t *client, char *destination, size_t capacity, size_t *length);
void at_client_line(at_client_t *client, const char *line, size_t length);
void at_client_prompt(at_client_t *client);
void at_client_data(at_client_t *client, int remote_fd, const uint8_t *data, size_t length, bool final);
void at_client_ble_data(at_client_t *client, uint8_t conn_id, const uint8_t *data, size_t length,
                              bool final);
void at_client_datagram(at_client_t *client, int listener_fd, const char *peer_ipv4,
                              uint16_t peer_port, const uint8_t *data, size_t length, bool final);
void at_client_accept(at_client_t *client, int listener_fd, int client_fd,
                            const char *peer_ipv4, uint16_t peer_port);
void at_client_accept_v2(at_client_t *client, uint32_t con_id, uint32_t generation,
                               int listener_fd, uint32_t listener_generation, int lwip_fd, const char *peer_ipv4,
                               uint16_t peer_port);
void at_client_close_v2(at_client_t *client, uint32_t con_id, int lwip_fd, uint32_t generation,
                              const char *reason);
void at_client_fault(at_client_t *client, at_fault_t reason);
void at_client_fail(at_client_t *client, at_result_t reason);
void at_client_cancel(at_client_t *client);
/* Call only after transport reset, RX drain, and parser reinitialization. */
bool at_client_resynchronize(at_client_t *client);
void at_client_poll(at_client_t *client, uint32_t now);
bool at_client_take_result(at_client_t *client, at_result_t *result);
bool at_client_busy(const at_client_t *client);

#endif
