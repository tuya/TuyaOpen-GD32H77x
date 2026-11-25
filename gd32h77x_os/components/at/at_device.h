/**
 * @file device.h
 * @brief Single-owner AT transport and asynchronous event fanout.
 */
#ifndef AT_DEVICE_H
#define AT_DEVICE_H

#include "at_session.h"

typedef enum {
    AT_DEVICE_EVENT_LINE,
    AT_DEVICE_EVENT_DATA,
    AT_DEVICE_EVENT_BLE_DATA,
    AT_DEVICE_EVENT_PROMPT,
    AT_DEVICE_EVENT_FAULT,
    AT_DEVICE_EVENT_DATAGRAM,
    AT_DEVICE_EVENT_ACCEPT,
    AT_DEVICE_EVENT_ACCEPT_V2,
    AT_DEVICE_EVENT_CLOSE_V2,
    AT_DEVICE_EVENT_RESET,
} at_device_event_kind_t;

typedef struct {
    at_device_event_kind_t kind;
    union {
        struct { const char *text; size_t length; } line;
        struct { int remote_fd; const uint8_t *data; size_t length; bool final; } data;
        struct { uint8_t conn_id; const uint8_t *data; size_t length; bool final; } ble_data;
        at_fault_t fault;
        struct { int listener_fd; const char *peer_ipv4; uint16_t peer_port; const uint8_t *data; size_t length;
                 bool final; } datagram;
        struct { int listener_fd; int client_fd; const char *peer_ipv4; uint16_t peer_port; } accept;
        struct { uint32_t con_id; uint32_t generation; int listener_fd; uint32_t listener_generation; int lwip_fd;
                 const char *peer_ipv4; uint16_t peer_port; } accept_v2;
        struct { uint32_t con_id; uint32_t generation; int lwip_fd; const char *reason; } close_v2;
    } value;
} at_device_event_t;

typedef void (*at_device_event_handler_t)(void *context, const at_device_event_t *event);

/* Subscribers and contexts must have static lifetime. Registration is
 * permanent because callbacks can be in flight on the event task. Subscribe
 * before init where possible. Callbacks must be quick and must not wait on an
 * RPC or hold a socket/backend state lock across one.
 */
at_status_t at_device_subscribe(at_device_event_handler_t handler, void *context);
/* The only owner of UART binding and transport initialization. Idempotent. */
at_status_t at_device_init(void);
/* Pulse the board-defined AT NRST line. */
void at_device_reset(void);
/* Board-specific override for an explicit ISP sequence. This bypasses the
 * shared-reset guard and must only be called after the BOOT strap is asserted. */
void at_device_force_reset(void);
void at_device_enter_isp_mode(void);
void at_device_exit_isp_mode(void);
/* Reset the module and asynchronously recover the H77 transport/parser. */
void at_device_reset_and_recover(void);
at_status_t at_device_negotiate(uint32_t timeout_ms);
at_status_t at_device_execute(const at_request_t *request, at_result_t *result);
at_status_t at_device_wait_events(uint32_t watermark, uint32_t timeout_ms);
at_status_t at_device_get_capabilities(at_capabilities_t *capabilities);

#endif
