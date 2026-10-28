/**
 * @file vw553_uart_transport.h
 * @brief FreeRTOS UART transport and serialized AT transactions for VW553.
 *
 * This transport is separate from the Tuya console UART. Current wire policy
 * starts in probe-only mode. A fixed read-only capability query is required
 * before any other command or prompt/payload transaction is accepted.
 */
#ifndef VW553_UART_TRANSPORT_H
#define VW553_UART_TRANSPORT_H

#include "gd32h77x_78x.h"
#include "vw553_at_client.h"
#include "vw553_uart_policy.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t usart;
    rcu_periph_enum usart_clock;
    rcu_periph_enum gpio_clock;
    uint32_t gpio_port;
    uint32_t tx_pin;
    uint32_t rx_pin;
    uint32_t alternate_function;
    IRQn_Type irq;
    uint32_t baudrate;
    uint32_t word_length;
    uint32_t stop_bits;
    uint32_t parity;
    uint32_t irq_priority;
} vw553_uart_transport_config_t;

#define VW553_UART_MAX_PAYLOAD 2048U
#define VW553_UART_REPLY_CAPACITY 2048U

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
} vw553_uart_request_t;

typedef struct {
    void (*line)(void *context, const char *line, size_t length);
    void (*data)(void *context, int remote_fd, const uint8_t *data, size_t length, bool final);
    void (*ble_data)(void *context, uint8_t conn_id, const uint8_t *data, size_t length, bool final);
    void (*prompt)(void *context);
    void (*fault)(void *context, vw553_at_fault_t reason);
    void (*datagram)(void *context, int listener_fd, const char *peer_ipv4, uint16_t peer_port,
                     const uint8_t *data, size_t length, bool final);
    void (*accept)(void *context, int listener_fd, int client_fd, const char *peer_ipv4, uint16_t peer_port);
    void (*accept_v2)(void *context, uint32_t con_id, uint32_t generation, int listener_fd,
                      uint32_t listener_generation, int lwip_fd, const char *peer_ipv4, uint16_t peer_port);
    void (*close_v2)(void *context, uint32_t con_id, int lwip_fd, uint32_t generation, const char *reason);
} vw553_uart_transport_events_t;

/* One transport instance is supported for the single attached VW553 module.
 * It has static runtime lifetime; there is deliberately no public deinit API.
 * Callbacks run asynchronously on the event task and may overlap the caller
 * after execute returns. They must not be used as synchronous query results.
 * Input command/payload bytes are copied before queueing; the worker never
 * retains caller pointers. A synchronous reply is copied to caller storage
 * only after completion. Use request.reply for synchronous query results.
 * The only transition out of probe-only mode is the fixed, validated
 * AT+TY553XCAP? negotiation; callers cannot select a wire mode.
 */
int32_t vw553_uart_transport_init(const vw553_uart_transport_config_t *config,
                                  const vw553_uart_transport_events_t *events, void *context);
int32_t vw553_uart_transport_execute(const vw553_uart_request_t *request, vw553_at_result_t *result);
int32_t vw553_uart_transport_recover(void);
int32_t vw553_uart_transport_transact(const char *command, size_t length, bool wait_prompt,
                                      uint32_t timeout_ms, vw553_at_result_t *result);
int32_t vw553_uart_transport_negotiate(uint32_t timeout_ms);
int32_t vw553_uart_transport_get_capabilities(vw553_uart_capabilities_t *capabilities);
int32_t vw553_uart_transport_wait_events(uint32_t watermark, uint32_t timeout_ms);
bool vw553_uart_transport_is_poisoned(void);
/* Dumps the most recent raw RX bytes; diagnostics only. */
void vw553_uart_transport_dump_rx_trace(const char *tag);

#endif
