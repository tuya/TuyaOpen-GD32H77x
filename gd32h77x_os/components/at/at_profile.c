/**
 * @file at_uart_policy.c
 * @brief Fail-closed transaction policy for the currently installed AT image.
 */
#include "at_profile.h"
#include "at_commands.h"

#include <string.h>

static bool parse_u16(const char *text, size_t length, uint16_t *value)
{
    if (!text || !length || !value) {
        return false;
    }
    uint32_t parsed = 0U;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        uint32_t digit = (uint32_t)(text[i] - '0');
        if (parsed > (UINT16_MAX - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    *value = (uint16_t)parsed;
    return true;
}

bool at_uart_policy_command_supported(const char *command, size_t length, bool wait_prompt)
{
    if (!command || wait_prompt) {
        return false;
    }
    return (length == 2U && memcmp(command, "AT", 2U) == 0) ||
           (length == sizeof("AT+RST") - 1U && memcmp(command, "AT+RST", length) == 0) ||
           (length == sizeof(AT_WIRE_CAPABILITY_QUERY) - 1U &&
            memcmp(command, AT_WIRE_CAPABILITY_QUERY, length) == 0);
}

bool at_uart_policy_network_capabilities_v2(const char *reply, size_t length,
                                                at_capabilities_t *capabilities)
{
    static const char prefix[] = AT_WIRE_CAPABILITY_PREFIX;
    static const char udp_id_token[] = "UDP_SEND_ID=";
    static const char max_send_token[] = "MAX_SEND=";
    static const char max_clients_token[] = "MAX_CLIENTS=";
    static const char max_servers_token[] = "MAX_SERVERS=";
    static const char *const required_v2[] = {"2", "ATOMIC_IPD", "DUAL_SERVER", "TCP_ACCEPT2", "CLOSE_EVENT",
                                              "CLOSE_ACK", "GEN32", "STATE_QUERY"};
    static const char *const required_v3[] = {"3", "ATOMIC_IPD", "MULTI_SERVER", "TCP_ACCEPT2", "CLOSE_EVENT",
                                              "CLOSE_ACK", "GEN32", "STATE_QUERY"};
    if (!reply || !capabilities || !length) {
        return false;
    }
    static const char ok_line[] = "OK\r\n";
    size_t cap_length = length;
    if (length <= sizeof(ok_line) - 1U || memcmp(reply + length - (sizeof(ok_line) - 1U), ok_line,
                                                sizeof(ok_line) - 1U) != 0) {
        return false;
    }
    cap_length -= sizeof(ok_line) - 1U;
    if (cap_length < sizeof(prefix) - 1U + 2U || reply[cap_length - 2U] != '\r' ||
        reply[cap_length - 1U] != '\n' || memcmp(reply, prefix, sizeof(prefix) - 1U) != 0) {
        return false;
    }
    size_t line_end = cap_length - 2U;
    size_t cursor = sizeof(prefix) - 1U;
    at_capabilities_t parsed = {0};
    const char *const *required = required_v3;
    size_t required_count = sizeof(required_v3) / sizeof(required_v3[0]);
    if (line_end - cursor >= 2U && reply[cursor + 1U] == ',') {
        if (reply[cursor] == '2') {
            required = required_v2;
            required_count = sizeof(required_v2) / sizeof(required_v2[0]);
        } else if (reply[cursor] != '3') {
            return false;
        }
    } else {
        return false;
    }
    for (size_t i = 0; i < required_count; ++i) {
        size_t end = cursor;
        while (end < line_end && reply[end] != ',') {
            ++end;
        }
        size_t token_length = end - cursor;
        size_t expected_length = strlen(required[i]);
        if (token_length != expected_length || memcmp(reply + cursor, required[i], expected_length) != 0 ||
            end == line_end) {
            return false;
        }
        cursor = end + 1U;
    }
    size_t end = cursor;
    while (end < line_end && reply[end] != ',') ++end;
    if (end == line_end || end - cursor <= sizeof(udp_id_token) - 1U ||
        memcmp(reply + cursor, udp_id_token, sizeof(udp_id_token) - 1U) != 0 ||
        !parse_u16(reply + cursor + sizeof(udp_id_token) - 1U, end - cursor - (sizeof(udp_id_token) - 1U),
                   &parsed.udp_send_id)) {
        return false;
    }
    cursor = end + 1U;
    end = cursor;
    while (end < line_end && reply[end] != ',') ++end;
    if (end == line_end || end - cursor <= sizeof(max_send_token) - 1U ||
        memcmp(reply + cursor, max_send_token, sizeof(max_send_token) - 1U) != 0 ||
        !parse_u16(reply + cursor + sizeof(max_send_token) - 1U, end - cursor - (sizeof(max_send_token) - 1U),
                   &parsed.max_send)) {
        return false;
    }
    cursor = end + 1U;
    end = cursor;
    while (end < line_end && reply[end] != ',') ++end;
    bool supports_max_servers = required == required_v3;
    if ((supports_max_servers && end == line_end) || (!supports_max_servers && end != line_end) ||
        end - cursor <= sizeof(max_clients_token) - 1U ||
        memcmp(reply + cursor, max_clients_token, sizeof(max_clients_token) - 1U) != 0 ||
        !parse_u16(reply + cursor + sizeof(max_clients_token) - 1U, end - cursor - (sizeof(max_clients_token) - 1U),
                   &parsed.max_clients)) {
        return false;
    }
    if (supports_max_servers) {
        cursor = end + 1U;
        if (line_end - cursor <= sizeof(max_servers_token) - 1U ||
            memcmp(reply + cursor, max_servers_token, sizeof(max_servers_token) - 1U) != 0 ||
            !parse_u16(reply + cursor + sizeof(max_servers_token) - 1U,
                       line_end - cursor - (sizeof(max_servers_token) - 1U), &parsed.max_servers)) {
            return false;
        }
    } else {
        parsed.max_servers = 2U;
    }
    if (!parsed.max_send || !parsed.max_clients || !parsed.max_servers ||
        parsed.udp_send_id != parsed.max_clients) {
        return false;
    }
    parsed.version = supports_max_servers ? 3U : 2U;
    *capabilities = parsed;
    return true;
}

bool at_uart_policy_reply_fits(size_t capacity, size_t length)
{
    /* Include room for the trailing NUL even though reply_length excludes it. */
    return capacity > length;
}

bool at_uart_policy_reply_append(char *reply, size_t capacity, size_t *used, const char *line, size_t length)
{
    if (!reply || !capacity || !used || (!line && length) || *used >= capacity || length > capacity - *used - 1U ||
        capacity - *used - length - 1U < 2U) {
        return false;
    }
    if (length) {
        memcpy(reply + *used, line, length);
    }
    *used += length;
    reply[(*used)++] = '\r';
    reply[(*used)++] = '\n';
    reply[*used] = '\0';
    return true;
}

bool at_uart_policy_capture_line(char *reply, size_t capacity, size_t *used, const char *line, size_t length)
{
    if (!line && length) {
        return false;
    }
    static const char diagnostic_prefix[] = AT_WIRE_DEBUG_PREFIX;
    if (length >= sizeof(diagnostic_prefix) - 1U &&
        memcmp(line, diagnostic_prefix, sizeof(diagnostic_prefix) - 1U) == 0) {
        return true;
    }
    static const char *const urcs[] = {"WIFI CONNECTED", "WIFI GOT IP", "WIFI DISCONNECT", "CLOSED"};
    for (size_t i = 0; i < sizeof(urcs) / sizeof(urcs[0]); ++i) {
        size_t urc_length = strlen(urcs[i]);
        if (length == urc_length && memcmp(line, urcs[i], length) == 0) {
            return true;
        }
    }
    /* BLE link-state reports are unsolicited too (see atcmd_ble.c in the
     * module firmware). Keeping them out of the captured reply stops them
     * from corrupting responses such as +CIPSTA or +CWJAP_CUR. */
    static const char ble_prefix[] = AT_WIRE_BLE_EVENT_PREFIX;
    if (length >= sizeof(ble_prefix) - 1U && memcmp(line, ble_prefix, sizeof(ble_prefix) - 1U) == 0) {
        return true;
    }
    static const char *const ble_status[] = {"connect success. conn idx:", "disconnected. conn idx: "};
    for (size_t i = 0; i < sizeof(ble_status) / sizeof(ble_status[0]); ++i) {
        size_t prefix_length = strlen(ble_status[i]);
        if (length >= prefix_length && memcmp(line, ble_status[i], prefix_length) == 0) {
            return true;
        }
    }
    return at_uart_policy_reply_append(reply, capacity, used, line, length);
}

bool at_uart_policy_payload_valid(bool wait_prompt, const void *payload, size_t length, size_t maximum)
{
    if (length > maximum || (length && !payload)) {
        return false;
    }
    return (length == 0U) == !wait_prompt;
}

bool at_uart_policy_tx_complete(size_t requested, size_t sent, bool transmit_complete)
{
    return sent == requested && transmit_complete;
}

bool at_uart_policy_accept_prompt(bool request_active, bool wait_prompt, size_t payload_length,
                                     bool prompt_seen, bool payload_sent)
{
    return request_active && wait_prompt && payload_length && !prompt_seen && !payload_sent;
}

bool at_uart_policy_request_pending_expired(bool request_active, bool request_started, bool poisoned,
                                                uint32_t waited_ms, uint32_t timeout_ms)
{
    return request_active && !request_started && !poisoned && waited_ms >= timeout_ms;
}

bool at_uart_policy_payload_result(size_t payload_length, bool prompt_seen, bool payload_sent,
                                      bool saw_send_ok, bool saw_send_fail, bool final_ok)
{
    return !payload_length || !final_ok || (prompt_seen && payload_sent && saw_send_ok && !saw_send_fail);
}

bool at_uart_policy_ble_payload_uses_plain_ok(const char *command, size_t length)
{
    static const char prefix[] = AT_WIRE_BLE_SEND_PREFIX;
    return command && length > sizeof(prefix) - 1U && memcmp(command, prefix, sizeof(prefix) - 1U) == 0;
}

bool at_uart_policy_sequence_reached(uint32_t dispatched, uint32_t watermark)
{
    return (int32_t)(dispatched - watermark) >= 0;
}
