/**
 * @file at_parser.c
 * @brief Parse UART AT lines and length-delimited binary IPD payloads.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "at_parser.h"
#include "at_commands.h"
#include <limits.h>
#include <string.h>

enum { LINE, SEPARATOR, PAYLOAD, DISCARD_LINE };

static void fault(at_parser_t *parser, at_fault_t reason)
{
    parser->used  = 0;
    parser->state = DISCARD_LINE;
    if (parser->callbacks.fault) {
        parser->callbacks.fault(parser->context, reason);
    }
}

static bool decimal(const char **cursor, unsigned limit, unsigned *value)
{
    const char *p      = *cursor;
    unsigned    result = 0;
    if (*p < '0' || *p > '9') {
        return false;
    }
    do {
        unsigned digit = (unsigned)(*p++ - '0');
        if (result > (limit - digit) / 10U) {
            return false;
        }
        result = result * 10U + digit;
    } while (*p >= '0' && *p <= '9');
    *cursor = p;
    *value  = result;
    return true;
}

static bool ipv4(const char **cursor, char out[16])
{
    const char *p = *cursor;
    size_t used = 0;
    for (unsigned part = 0; part < 4; ++part) {
        unsigned value;
        const char *start = p;
        if (!decimal(&p, 255U, &value) || (p - start > 1 && *start == '0')) return false;
        if (part) out[used++] = '.';
        if (value >= 100U) out[used++] = (char)('0' + value / 100U);
        if (value >= 10U) out[used++] = (char)('0' + (value / 10U) % 10U);
        out[used++] = (char)('0' + value % 10U);
        if (part < 3 && *p++ != '.') return false;
    }
    if (*p && *p != ':' && *p != ',') return false;
    out[used] = '\0';
    *cursor = p;
    return true;
}

static bool ipd_header(at_parser_t *parser)
{
    const char *cursor = parser->line + 5;
    unsigned    descriptor = 0;
    unsigned    length;
    parser->datagram = false;
    parser->ble_data = false;
    parser->separator_space = true;
    parser->listener_fd = -1;
    parser->peer_ipv4[0] = '\0';

    /* Legacy UDP server header: +IPD,<peer-ip>:<peer-port>, <length>: */
    if (ipv4(&cursor, parser->peer_ipv4) && *cursor++ == ':') {
        unsigned port;
        if (!decimal(&cursor, 65535U, &port) || *cursor++ != ',' || *cursor++ != ' ' ||
            !decimal(&cursor, AT_MAX_PAYLOAD, &length) || *cursor != '\0') return false;
        parser->datagram = true;
        parser->peer_port = (uint16_t)port;
    } else {
        cursor = parser->line + 5;
        if (!decimal(&cursor, INT_MAX, &descriptor) || *cursor++ != ',') return false;

        const char *after_descriptor = cursor;
        char peer[16];
        unsigned port;
        if (ipv4(&cursor, peer) && *cursor++ == ':') {
            if (!decimal(&cursor, 65535U, &port) || *cursor++ != ',' || *cursor++ != ' ' ||
                !decimal(&cursor, AT_MAX_PAYLOAD, &length) || *cursor != '\0') return false;
            memcpy(parser->peer_ipv4, peer, sizeof(parser->peer_ipv4));
            parser->peer_port = (uint16_t)port;
            parser->listener_fd = (int)descriptor;
            parser->datagram = true;
        } else {
            cursor = after_descriptor;
            if (!decimal(&cursor, AT_MAX_PAYLOAD, &length) || *cursor != '\0') return false;
        }
    }
    parser->remote_fd = (int)descriptor;
    parser->remaining = length;
    parser->frame_length = length;
    parser->used      = 0;
    parser->state     = SEPARATOR;
    return true;
}

/* Vendor BLE AT event: +BLEDATA,<connection-index>,<length>:<binary>. */
static bool ble_data_header(at_parser_t *parser)
{
    const char *cursor = parser->line + sizeof("+BLEDATA,") - 1U;
    unsigned conn_id;
    unsigned length;
    if (!decimal(&cursor, UINT8_MAX, &conn_id) || *cursor++ != ',' ||
        !decimal(&cursor, AT_MAX_PAYLOAD, &length) || *cursor != '\0') return false;
    parser->remote_fd = (int)conn_id;
    parser->remaining = length;
    parser->frame_length = length;
    parser->used = 0U;
    parser->datagram = false;
    parser->ble_data = true;
    parser->separator_space = false;
    parser->state = SEPARATOR;
    return true;
}

static bool accept_line(at_parser_t *parser)
{
    const char *p = parser->line + sizeof(AT_WIRE_CONNECTION_PREFIX) - 1U;
    unsigned listener, client, port;
    char peer[16];
    if (strncmp(parser->line, AT_WIRE_CONNECTION_PREFIX,
                sizeof(AT_WIRE_CONNECTION_PREFIX) - 1U) != 0 ||
        !decimal(&p, INT_MAX, &listener) || *p++ != ',' ||
        !decimal(&p, INT_MAX, &client) || *p++ != ',' || !ipv4(&p, peer) || *p++ != ',' ||
        !decimal(&p, 65535U, &port) || *p != '\0') return false;
    if (parser->callbacks.accept) parser->callbacks.accept(parser->context, (int)listener, (int)client, peer, (uint16_t)port);
    return true;
}

static bool accept_v2_line(at_parser_t *parser)
{
    const char *p = parser->line + sizeof(AT_WIRE_CONNECTION2_PREFIX) - 1U;
    unsigned con_id, generation, listener, listener_generation, lwip_fd, port;
    char peer[16];
    if (!decimal(&p, INT_MAX, &con_id) || *p++ != ',' || !decimal(&p, UINT32_MAX, &generation) || *p++ != ',' ||
        !decimal(&p, INT_MAX, &listener) || *p++ != ',' || !decimal(&p, UINT32_MAX, &listener_generation) ||
        *p++ != ',' || !decimal(&p, INT_MAX, &lwip_fd) || *p++ != ',' || !ipv4(&p, peer) || *p++ != ',' ||
        !decimal(&p, 65535U, &port) || *p != '\0') return false;
    if (parser->callbacks.accept_v2)
        parser->callbacks.accept_v2(parser->context, con_id, generation, (int)listener, listener_generation,
                                    (int)lwip_fd, peer, (uint16_t)port);
    return true;
}

static bool close_v2_line(at_parser_t *parser)
{
    const char *p = parser->line + sizeof(AT_WIRE_CLOSE_PREFIX) - 1U;
    unsigned con_id, lwip_fd, generation;
    const char *reason;
    if (!decimal(&p, INT_MAX, &con_id) || *p++ != ',' || !decimal(&p, INT_MAX, &lwip_fd) || *p++ != ',' ||
        !decimal(&p, UINT32_MAX, &generation) || *p++ != ',') return false;
    reason = p;
    if (strcmp(reason, "REMOTE_EOF") && strcmp(reason, "REMOTE_ERROR") && strcmp(reason, "LOCAL_STOP") &&
        strcmp(reason, "SERVER_STOP") && strcmp(reason, "TX_ERROR") && strcmp(reason, "TASK_CLEANUP")) return false;
    if (parser->callbacks.close_v2)
        parser->callbacks.close_v2(parser->context, con_id, (int)lwip_fd, generation, reason);
    return true;
}

static bool complete_ipv4_prefix(const char *text, size_t length)
{
    char address[16];
    char candidate[AT_LINE_SIZE];
    const char *cursor;
    if (length < 6 || length >= sizeof(candidate)) return false;
    memcpy(candidate, text, length);
    candidate[length] = '\0';
    cursor = candidate + 5;
    return ipv4(&cursor, address) && *cursor == '\0';
}

static bool complete_listener_ipv4_prefix(const char *text, size_t length)
{
    char address[16];
    char candidate[AT_LINE_SIZE];
    const char *cursor;
    unsigned descriptor;
    if (length < 8 || length >= sizeof(candidate)) return false;
    memcpy(candidate, text, length);
    candidate[length] = '\0';
    cursor = candidate + 5;
    return decimal(&cursor, INT_MAX, &descriptor) && *cursor++ == ',' &&
           ipv4(&cursor, address) && *cursor == '\0';
}

void at_parser_init(at_parser_t *parser, const at_callbacks_t *callbacks, void *context)
{
    memset(parser, 0, sizeof(*parser));
    parser->callbacks = *callbacks;
    parser->context   = context;
}

static void parser_emit_line(at_parser_t *parser)
{
    if (parser->used && parser->line[parser->used - 1] == '\r') {
        --parser->used;
    }
    parser->line[parser->used] = '\0';
    if (parser->suppress_next_ok) {
        parser->suppress_next_ok = false;
        if (parser->used == 2U && memcmp(parser->line, "OK", 2U) == 0) {
            /* Connection/close events each carry their own
             * bare OK. It must never complete an in-flight CIPSEND. */
            parser->used = 0;
            return;
        }
    }
    if (parser->used && strncmp(parser->line, AT_WIRE_CONNECTION2_PREFIX,
                                 sizeof(AT_WIRE_CONNECTION2_PREFIX) - 1U) == 0) {
        if (!accept_v2_line(parser)) fault(parser, AT_BAD_HEADER);
        else parser->suppress_next_ok = true;
    } else if (parser->used && strncmp(parser->line, AT_WIRE_CLOSE_PREFIX,
                                       sizeof(AT_WIRE_CLOSE_PREFIX) - 1U) == 0) {
        if (!close_v2_line(parser)) fault(parser, AT_BAD_HEADER);
        else parser->suppress_next_ok = true;
    } else if (parser->used && strncmp(parser->line, AT_WIRE_CONNECTION_PREFIX,
                                       sizeof(AT_WIRE_CONNECTION_PREFIX) - 1U) == 0) {
        if (!accept_line(parser)) fault(parser, AT_BAD_HEADER);
        else parser->suppress_next_ok = true;
    } else if (parser->used && parser->callbacks.line) {
        parser->callbacks.line(parser->context, parser->line, parser->used);
    }
    parser->used = 0;
}

bool at_parser_in_frame(const at_parser_t *parser)
{
    return parser && parser->remaining != 0U;
}

void at_parser_drop_frame(at_parser_t *parser)
{
    if (!parser) return;
    parser->remaining = 0U;
    parser->frame_length = 0U;
    parser->used      = 0U;
    parser->ble_data  = false;
    parser->datagram  = false;
    parser->state     = LINE;
}

void at_parser_feed(at_parser_t *parser, const uint8_t *data, size_t length)
{
    while (length) {
        if (parser->state == PAYLOAD) {
            size_t count = length < parser->remaining ? length : parser->remaining;
            parser->remaining -= count;
            if (parser->ble_data && parser->callbacks.ble_data) {
                parser->callbacks.ble_data(parser->context, (uint8_t)parser->remote_fd, data, count,
                                           parser->remaining == 0U);
            } else if (parser->datagram && parser->callbacks.datagram) {
                parser->callbacks.datagram(parser->context, parser->listener_fd, parser->peer_ipv4,
                                           parser->peer_port, data, count, parser->remaining == 0);
            } else if (!parser->datagram && parser->callbacks.data) {
                parser->callbacks.data(parser->context, parser->remote_fd, data, count, parser->remaining == 0);
            }
            data += count;
            length -= count;
            if (!parser->remaining) {
                parser->state = LINE;
            }
            continue;
        }
        uint8_t byte = *data++;
        --length;
        if (parser->state == SEPARATOR) {
            /* IPD uses exactly one ': ' separator while the BLE data-transfer
             * event uses ':'. The payload itself may start with a space, so the
             * separator cannot be skipped greedily. */
            if (parser->separator_space && byte != ' ') {
                fault(parser, AT_BAD_SEPARATOR);
            } else if (parser->remaining) {
                parser->state = PAYLOAD;
                if (!parser->separator_space) {
                    --data;
                    ++length;
                }
            } else {
                if (parser->ble_data && parser->callbacks.ble_data) {
                    parser->callbacks.ble_data(parser->context, (uint8_t)parser->remote_fd, NULL, 0U, true);
                } else if (parser->datagram && parser->callbacks.datagram) {
                    parser->callbacks.datagram(parser->context, parser->listener_fd, parser->peer_ipv4,
                                               parser->peer_port, NULL, 0, true);
                } else if (!parser->datagram && parser->callbacks.data) {
                    parser->callbacks.data(parser->context, parser->remote_fd, NULL, 0, true);
                }
                parser->state = LINE;
            }
            continue;
        }
        if (parser->state == DISCARD_LINE) {
            if (byte == '\n') {
                parser->state = LINE;
            }
            continue;
        }
        if (parser->prompt_space) {
            parser->prompt_space = false;
            if (byte == ' ') {
                continue;
            }
        }
        if (byte == '\n') {
            parser_emit_line(parser);
        } else if (byte == '\r') {
            if (parser->used == sizeof(parser->line) - 1) {
                fault(parser, AT_LINE_OVERFLOW);
            } else {
                parser->line[parser->used++] = '\r';
            }
        } else if ((unsigned char)byte < 0x20U) {
            /* A control byte cannot appear in an AT text line. The vendor's
             * undersized AT_RSP buffer emits NUL + heap garbage instead of a
             * CRLF; terminate the current line and drop the byte so the next
             * real result still lines up. Binary data never reaches this state
             * because +IPD/+BLEDATA payloads are length-delimited. */
            parser_emit_line(parser);
        } else if (parser->used == 0 && byte == '>') {
            if (parser->callbacks.prompt) {
                parser->callbacks.prompt(parser->context);
            }
            parser->prompt_space = true;
        } else if (byte == ':' && parser->used >= sizeof("+BLEDATA,") - 1U &&
                   memcmp(parser->line, "+BLEDATA,", sizeof("+BLEDATA,") - 1U) == 0) {
            parser->line[parser->used] = '\0';
            if (!ble_data_header(parser)) fault(parser, AT_BAD_HEADER);
        } else if (byte == ':' && parser->used >= 5 && memcmp(parser->line, "+IPD,", 5) == 0) {
            if (parser->used >= sizeof(parser->line) - 1) {
                fault(parser, AT_LINE_OVERFLOW);
                continue;
            }
            parser->line[parser->used] = '\0';
            if (!ipd_header(parser)) {
                /* Only a complete IPv4 address may introduce the peer-port colon. */
                if ((!strchr(parser->line + 5, ':') && complete_ipv4_prefix(parser->line, parser->used)) ||
                    complete_listener_ipv4_prefix(parser->line, parser->used)) {
                    parser->line[parser->used] = ':';
                    ++parser->used;
                } else {
                    fault(parser, AT_BAD_HEADER);
                }
            }
        } else if (parser->used == sizeof(parser->line) - 1) {
            fault(parser, AT_LINE_OVERFLOW);
        } else {
            parser->line[parser->used++] = (char)byte;
        }
    }
}
