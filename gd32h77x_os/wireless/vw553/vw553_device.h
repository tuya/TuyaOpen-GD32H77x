/**
 * @file vw553_device.h
 * @brief Single-owner VW553 transport and asynchronous event fanout.
 */
#ifndef VW553_DEVICE_H
#define VW553_DEVICE_H

#include "vw553_uart_transport.h"

typedef enum {
    VW553_DEVICE_EVENT_LINE,
    VW553_DEVICE_EVENT_DATA,
    VW553_DEVICE_EVENT_BLE_DATA,
    VW553_DEVICE_EVENT_PROMPT,
    VW553_DEVICE_EVENT_FAULT,
    VW553_DEVICE_EVENT_DATAGRAM,
    VW553_DEVICE_EVENT_ACCEPT,
    VW553_DEVICE_EVENT_ACCEPT_V2,
    VW553_DEVICE_EVENT_CLOSE_V2,
    VW553_DEVICE_EVENT_RESET,
} vw553_device_event_kind_t;

typedef struct {
    vw553_device_event_kind_t kind;
    union {
        struct { const char *text; size_t length; } line;
        struct { int remote_fd; const uint8_t *data; size_t length; bool final; } data;
        struct { uint8_t conn_id; const uint8_t *data; size_t length; bool final; } ble_data;
        vw553_at_fault_t fault;
        struct { int listener_fd; const char *peer_ipv4; uint16_t peer_port; const uint8_t *data; size_t length;
                 bool final; } datagram;
        struct { int listener_fd; int client_fd; const char *peer_ipv4; uint16_t peer_port; } accept;
        struct { uint32_t con_id; uint32_t generation; int listener_fd; uint32_t listener_generation; int lwip_fd;
                 const char *peer_ipv4; uint16_t peer_port; } accept_v2;
        struct { uint32_t con_id; uint32_t generation; int lwip_fd; const char *reason; } close_v2;
    } value;
} vw553_device_event_t;

typedef void (*vw553_device_event_handler_t)(void *context, const vw553_device_event_t *event);

/* Subscribers and contexts must have static lifetime. Registration is
 * permanent because callbacks can be in flight on the event task. Subscribe
 * before init where possible. Callbacks must be quick and must not wait on an
 * RPC or hold a socket/backend state lock across one.
 */
int32_t vw553_device_subscribe(vw553_device_event_handler_t handler, void *context);
/* The only owner of UART binding and transport initialization. Idempotent. */
int32_t vw553_device_init(void);
/* Pulse the board-defined VW553 NRST line. */
void vw553_device_reset(void);
void vw553_device_enter_isp_mode(void);
void vw553_device_exit_isp_mode(void);
/* Reset the module and asynchronously recover the H77 transport/parser. */
void vw553_device_reset_and_recover(void);
int32_t vw553_device_negotiate(uint32_t timeout_ms);
int32_t vw553_device_execute(const vw553_uart_request_t *request, vw553_at_result_t *result);
int32_t vw553_device_wait_events(uint32_t watermark, uint32_t timeout_ms);
int32_t vw553_device_get_capabilities(vw553_uart_capabilities_t *capabilities);

#endif
