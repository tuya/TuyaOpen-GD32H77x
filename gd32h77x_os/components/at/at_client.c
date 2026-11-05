/**
 * @file at_client.c
 * @brief Allocation-free UART AT transaction state for the AT.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "at_client.h"
#include "at_profile.h"
#include "at_commands.h"
#include <string.h>

static void parser_line(void *context, const char *line, size_t length)
{
    at_client_line(context, line, length);
}

static void parser_data(void *context, int remote_fd, const uint8_t *data, size_t length, bool final)
{
    at_client_data(context, remote_fd, data, length, final);
}

static void parser_ble_data(void *context, uint8_t conn_id, const uint8_t *data, size_t length, bool final)
{
    at_client_ble_data(context, conn_id, data, length, final);
}

static void parser_prompt(void *context)
{
    at_client_prompt(context);
}

static void parser_fault(void *context, at_fault_t reason)
{
    at_client_fault(context, reason);
}

static void parser_datagram(void *context, int listener_fd, const char *ip, uint16_t port,
                            const uint8_t *data, size_t length, bool final)
{
    at_client_datagram(context, listener_fd, ip, port, data, length, final);
}

static void parser_accept(void *context, int listener, int client, const char *ip, uint16_t port)
{
    at_client_accept(context, listener, client, ip, port);
}

static void parser_accept_v2(void *context, uint32_t con_id, uint32_t generation, int listener,
                             uint32_t listener_generation, int lwip_fd, const char *ip, uint16_t port)
{
    at_client_accept_v2(context, con_id, generation, listener, listener_generation, lwip_fd, ip, port);
}

static void parser_close_v2(void *context, uint32_t con_id, int lwip_fd, uint32_t generation, const char *reason)
{
    at_client_close_v2(context, con_id, lwip_fd, generation, reason);
}

static bool line_is_prefixed_ok(const char *line, size_t length)
{
    if (!line || length < 3U || memcmp(line + length - 3U, ",OK", 3U) != 0) {
        return false;
    }
    for (size_t i = 0U; i < length - 3U; ++i) {
        if (line[i] < '0' || line[i] > '9') {
            return false;
        }
    }
    return true;
}

at_callbacks_t at_client_parser_callbacks(void)
{
    const at_callbacks_t callbacks = {.line = parser_line, .data = parser_data, .ble_data = parser_ble_data,
                                            .prompt = parser_prompt,
                                            .fault = parser_fault, .datagram = parser_datagram, .accept = parser_accept,
                                            .accept_v2 = parser_accept_v2, .close_v2 = parser_close_v2};
    return callbacks;
}

void at_client_ble_data(at_client_t *client, uint8_t conn_id, const uint8_t *data, size_t length,
                              bool final)
{
    if (!client) return;
    if (client->wire_mode == AT_WIRE_PROBE_ONLY || client->suppress_ipd_ok) {
        at_client_fail(client, AT_RESULT_PROTOCOL);
        return;
    }
    /* +BLEDATA is an asynchronous BLE bridge event, not an AT command.  It
     * never owns a trailing OK, including when the legacy +IPD transport uses
     * one.  Suppressing an OK here consumes the next BLEDADATRANSSEND result
     * and poisons the provisioning response transaction. */
    if (client->events.ble_data) {
        client->events.ble_data(client->context, conn_id, data, length, final);
    }
}

static void finish(at_client_t *client, at_result_t result)
{
    if (client->phase != AT_IDLE) {
        client->phase           = AT_IDLE;
        client->result          = result;
        client->result_ready    = true;
        client->command_pending = false;
        client->saw_send_ok     = false;
        client->stale_ipd_ok_pending = false;
        client->plain_ok_payload = false;
        memset(client->command, 0, sizeof(client->command));
        client->command_length = 0;
    }
}

void at_client_init(at_client_t *client, const at_client_events_t *events, void *context)
{
    if (!client) {
        return;
    }
    memset(client, 0, sizeof(*client));
    if (events) {
        client->events = *events;
    }
    client->context = context;
}

bool at_client_set_wire_mode(at_client_t *client, at_wire_mode_t mode)
{
    if (!client || (unsigned)mode > AT_WIRE_ATOMIC_NO_IPD_TRAILER || client->phase != AT_IDLE ||
        client->result_ready || client->tainted || client->suppress_ipd_ok) {
        return false;
    }
    client->wire_mode = mode;
    return true;
}

bool at_client_begin(at_client_t *client, const char *command, size_t length, bool wait_prompt,
                           uint32_t now, uint32_t timeout_ms)
{
    if (!client || !command || !length || length > AT_COMMAND_SIZE || timeout_ms == 0 ||
        timeout_ms >= UINT32_C(0x80000000) || client->phase != AT_IDLE || client->result_ready ||
        client->tainted || client->suppress_ipd_ok) {
        return false;
    }
    if (client->wire_mode == AT_WIRE_PROBE_ONLY &&
        !at_uart_policy_command_supported(command, length, wait_prompt)) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        unsigned char byte = (unsigned char)command[i];
        if (byte < 0x20 || byte > 0x7e) {
            return false;
        }
    }
    memcpy(client->command, command, length);
    client->command[length]     = '\r';
    client->command[length + 1] = '\n';
    client->command_length      = length + 2U;
    client->deadline            = now + timeout_ms;
    client->phase               = wait_prompt ? AT_WAIT_PROMPT : AT_WAIT_FINAL;
    client->command_pending     = true;
    client->result              = AT_RESULT_NONE;
    client->saw_send_ok         = false;
    client->stale_ipd_ok_pending = false;
    client->plain_ok_payload    = at_uart_policy_ble_payload_uses_plain_ok(command, length);
    return true;
}

bool at_client_take_command(at_client_t *client, char *destination, size_t capacity, size_t *length)
{
    if (!client || !destination || !length || !client->command_pending || capacity < client->command_length) {
        return false;
    }
    memcpy(destination, client->command, client->command_length);
    *length                 = client->command_length;
    client->command_pending = false;
    return true;
}

void at_client_line(at_client_t *client, const char *line, size_t length)
{
    if (!client || !line) {
        return;
    }
    if (length == 5 && memcmp(line, "READY", 5) == 0) {
        at_client_fail(client, AT_RESULT_RESET);
        if (client->events.line) {
            client->events.line(client->context, line, length);
        }
        return;
    }
    if (length >= 4 && memcmp(line, "+BLE", 4) == 0) {
        /* BLE link-state notifications from the module are asynchronous. They
         * routinely land inside an unrelated transaction (a BLE connect or
         * disconnect during the CWJAP that follows provisioning), so they are
         * forwarded as notifications and the transaction keeps waiting for
         * its own OK or ERROR. */
        static const char status_prefix[] = AT_WIRE_BLE_EVENT_PREFIX;
        if (length >= sizeof(status_prefix) - 1U &&
            memcmp(line, status_prefix, sizeof(status_prefix) - 1U) == 0) {
            if (client->events.line) {
                client->events.line(client->context, line, length);
            }
            return;
        }
        at_client_fail(client, AT_RESULT_PROTOCOL);
        if (client->events.line) {
            client->events.line(client->context, line, length);
        }
        return;
    }
    if (length == sizeof("SEND OK") - 1U && memcmp(line, "SEND OK", length) == 0) {
        client->saw_send_ok = true;
    }
    if (length == sizeof("SEND FAIL") - 1U && memcmp(line, "SEND FAIL", length) == 0) {
        /* The module rejected the payload (local-socket queue full) and
         * follows up with ERROR. Treat SEND FAIL as that terminal result so
         * the transport can retry instead of parsing the late ERROR/garbage. */
        finish(client, AT_RESULT_ERROR);
        if (client->events.line) {
            client->events.line(client->context, line, length);
        }
        return;
    }
    if (client->suppress_ipd_ok) {
        if (length == 2 && memcmp(line, "OK", 2) == 0) {
            --client->suppress_ipd_ok;
            return;
        }
        /* A different line (SEND OK / SEND FAIL / URC) overtook the trailer
         * OK. Keep waiting for it instead of poisoning the in-flight command:
         * the module emits the trailer immediately after the +IPD frame, but
         * the AT command task can interleave its own result first. */
    }
    /* Some 553 commands emit an informational OK before asking for payload. */
    if (client->phase == AT_WAIT_PROMPT && length == 2 && memcmp(line, "OK", 2) == 0) {
        if (client->events.line) {
            client->events.line(client->context, line, length);
        }
        return;
    }
    if (client->stale_ipd_ok_pending && !client->saw_send_ok && length == 2U &&
        memcmp(line, "OK", 2U) == 0) {
        /* The bare OK that trails a +IPD frame was overtaken by this command's
         * prompt and arrived late. It belongs to the IPD frame, not to the
         * payload command, so it must not finish the transaction. */
        --client->stale_ipd_ok_pending;
        if (client->events.line) {
            client->events.line(client->context, line, length);
        }
        return;
    }
    if ((length == 2 && memcmp(line, "OK", 2) == 0) || line_is_prefixed_ok(line, length)) {
        finish(client, AT_RESULT_OK);
    } else if (length == 5 && memcmp(line, "ERROR", 5) == 0) {
        finish(client, AT_RESULT_ERROR);
    }
    if (client->events.line) {
        client->events.line(client->context, line, length);
    }
}

void at_client_prompt(at_client_t *client)
{
    if (!client) {
        return;
    }
    if (client->suppress_ipd_ok) {
        /* The module emits +IPD and its trailing OK from the socket task; the
         * AT task can slip the new command's prompt between those two writes.
         * The prompt proves the previous frame is over, so drop the stale
         * expectation instead of poisoning the new transaction. The late OK
         * is ignored in at_client_line() until SEND OK arrives. */
        if (!client->plain_ok_payload) {
            client->stale_ipd_ok_pending = client->suppress_ipd_ok;
        }
        client->suppress_ipd_ok = 0U;
        if (client->events.ipd_trailer_dropped) {
            client->events.ipd_trailer_dropped(client->context);
        }
    }
    if (client->phase == AT_WAIT_PROMPT) {
        client->phase = AT_WAIT_FINAL;
    }
    if (client->events.prompt) {
        client->events.prompt(client->context);
    }
}

void at_client_data(at_client_t *client, int remote_fd, const uint8_t *data, size_t length, bool final)
{
    if (!client) {
        return;
    }
    if (client->wire_mode == AT_WIRE_PROBE_ONLY) {
        at_client_fail(client, AT_RESULT_PROTOCOL);
        return;
    }
    if (!client->tainted && final && client->wire_mode == AT_WIRE_ATOMIC_IPD_TRAILER) {
        /* SDK 1.0.3g UART emits a bare OK after each +IPD payload. */
        if (client->suppress_ipd_ok < UINT8_MAX) {
            ++client->suppress_ipd_ok;
        }
    }
    if (client->events.data) {
        client->events.data(client->context, remote_fd, data, length, final);
    }
}

void at_client_datagram(at_client_t *client, int listener_fd, const char *ip, uint16_t port,
                              const uint8_t *data, size_t length, bool final)
{
    if (!client || !ip) return;
    if (client->wire_mode == AT_WIRE_PROBE_ONLY) {
        at_client_fail(client, AT_RESULT_PROTOCOL);
        return;
    }
    if (!client->tainted && final && client->wire_mode == AT_WIRE_ATOMIC_IPD_TRAILER) {
        if (client->suppress_ipd_ok < UINT8_MAX) {
            ++client->suppress_ipd_ok;
        }
    }
    if (client->events.datagram)
        client->events.datagram(client->context, listener_fd, ip, port, data, length, final);
}

void at_client_accept(at_client_t *client, int listener, int remote, const char *ip, uint16_t port)
{
    if (!client || !ip) return;
    if (client->wire_mode == AT_WIRE_PROBE_ONLY) {
        at_client_fail(client, AT_RESULT_PROTOCOL);
        return;
    }
    if (client->events.accept) client->events.accept(client->context, listener, remote, ip, port);
}

void at_client_accept_v2(at_client_t *client, uint32_t con_id, uint32_t generation, int listener,
                               uint32_t listener_generation, int lwip_fd, const char *ip, uint16_t port)
{
    if (!client || !ip) return;
    if (client->wire_mode == AT_WIRE_PROBE_ONLY) {
        at_client_fail(client, AT_RESULT_PROTOCOL);
        return;
    }
    if (client->events.accept_v2)
        client->events.accept_v2(client->context, con_id, generation, listener, listener_generation, lwip_fd, ip,
                                 port);
}

void at_client_close_v2(at_client_t *client, uint32_t con_id, int lwip_fd, uint32_t generation,
                              const char *reason)
{
    if (!client || !reason) return;
    if (client->wire_mode == AT_WIRE_PROBE_ONLY) {
        at_client_fail(client, AT_RESULT_PROTOCOL);
        return;
    }
    if (client->events.close_v2)
        client->events.close_v2(client->context, con_id, lwip_fd, generation, reason);
}

void at_client_fault(at_client_t *client, at_fault_t reason)
{
    if (!client) {
        return;
    }
    at_client_fail(client, AT_RESULT_PROTOCOL);
    if (client->events.fault) {
        client->events.fault(client->context, reason);
    }
}

void at_client_fail(at_client_t *client, at_result_t reason)
{
    if (!client || (unsigned)reason > AT_RESULT_PROTOCOL || reason == AT_RESULT_NONE ||
        reason == AT_RESULT_OK || reason == AT_RESULT_ERROR) {
        return;
    }
    if (reason == AT_RESULT_TIMEOUT || reason == AT_RESULT_CANCELLED || reason == AT_RESULT_RESET ||
        reason == AT_RESULT_IO || reason == AT_RESULT_OVERFLOW || reason == AT_RESULT_PROTOCOL) {
        client->tainted = true;
    }
    finish(client, reason);
}

void at_client_cancel(at_client_t *client)
{
    at_client_fail(client, AT_RESULT_CANCELLED);
}

bool at_client_resynchronize(at_client_t *client)
{
    if (!client || client->phase != AT_IDLE || client->result_ready) {
        return false;
    }
    client->tainted         = false;
    client->suppress_ipd_ok = 0U;
    client->stale_ipd_ok_pending = 0U;
    client->saw_send_ok     = false;
    client->plain_ok_payload = false;
    return true;
}

void at_client_poll(at_client_t *client, uint32_t now)
{
    if (client && client->phase != AT_IDLE && (now - client->deadline) < UINT32_C(0x80000000)) {
        at_client_fail(client, AT_RESULT_TIMEOUT);
    }
}

bool at_client_take_result(at_client_t *client, at_result_t *result)
{
    if (!client || !result || !client->result_ready) {
        return false;
    }
    *result              = client->result;
    client->result       = AT_RESULT_NONE;
    client->result_ready = false;
    return true;
}

bool at_client_busy(const at_client_t *client)
{
    return client && client->phase != AT_IDLE;
}
