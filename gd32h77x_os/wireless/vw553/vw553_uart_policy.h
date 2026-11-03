/**
 * @file vw553_uart_policy.h
 * @brief Host-testable command gate for the current unmodified VW553 firmware.
 */
#ifndef VW553_UART_POLICY_H
#define VW553_UART_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Probe-only mode permits AT, the fixed read-only capability query, and AT+RST. */
typedef struct {
    uint8_t version;
    uint16_t udp_send_id;
    uint16_t max_send;
    uint16_t max_clients;
    uint16_t max_servers;
} vw553_uart_capabilities_t;

bool vw553_uart_policy_command_supported(const char *command, size_t length, bool wait_prompt);
bool vw553_uart_policy_network_capabilities_v2(const char *reply, size_t length,
                                                vw553_uart_capabilities_t *capabilities);
bool vw553_uart_policy_reply_fits(size_t capacity, size_t length);
bool vw553_uart_policy_reply_append(char *reply, size_t capacity, size_t *used, const char *line, size_t length);
bool vw553_uart_policy_capture_line(char *reply, size_t capacity, size_t *used, const char *line, size_t length);
bool vw553_uart_policy_payload_valid(bool wait_prompt, const void *payload, size_t length, size_t maximum);
bool vw553_uart_policy_tx_complete(size_t requested, size_t sent, bool transmit_complete);
bool vw553_uart_policy_accept_prompt(bool request_active, bool wait_prompt, size_t payload_length,
                                     bool prompt_seen, bool payload_sent);
bool vw553_uart_policy_request_frame_hold_expired(bool request_active, bool request_started, bool poisoned,
                                                   bool in_frame, uint32_t waited_ms, uint32_t limit_ms);
bool vw553_uart_policy_payload_result(size_t payload_length, bool prompt_seen, bool payload_sent,
                                      bool saw_send_ok, bool saw_send_fail, bool final_ok);
bool vw553_uart_policy_ble_payload_uses_plain_ok(const char *command, size_t length);
bool vw553_uart_policy_sequence_reached(uint32_t dispatched, uint32_t watermark);

#endif
