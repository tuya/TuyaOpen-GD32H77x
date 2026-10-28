/**
 * @file vw553_at_parser.h
 * @brief Incremental parser for the pinned VW553 UART AT wire format.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#ifndef VW553_AT_PARSER_H
#define VW553_AT_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VW553_AT_LINE_SIZE   512U
#define VW553_AT_MAX_PAYLOAD 65535U

typedef enum {
    VW553_AT_BAD_HEADER,
    VW553_AT_LINE_OVERFLOW,
    VW553_AT_BAD_SEPARATOR,
    VW553_AT_UNSUPPORTED_FRAME,
} vw553_at_fault_t;

typedef struct {
    void (*line)(void *context, const char *line, size_t length);
    /* Data can arrive in several chunks. Consume/copy it before this callback returns.
     * remote_fd is the descriptor reported by 553, not a TuyaOpen socket handle.
     */
    void (*data)(void *context, int remote_fd, const uint8_t *data, size_t length, bool final);
    /* Fixed GATT data-transfer service used by the VW553 BLE AT firmware.
     * The payload is length-delimited and must be copied before returning. */
    void (*ble_data)(void *context, uint8_t conn_id, const uint8_t *data, size_t length, bool final);
    void (*prompt)(void *context);
    void (*fault)(void *context, vw553_at_fault_t reason);
    void (*datagram)(void *context, int listener_fd, const char *peer_ipv4, uint16_t peer_port,
                     const uint8_t *data, size_t length, bool final);
    void (*accept)(void *context, int listener_fd, int client_fd, const char *peer_ipv4, uint16_t peer_port);
    /* V2 events carry both the AT connection slot generation and lwIP fd. */
    void (*accept_v2)(void *context, uint32_t con_id, uint32_t generation, int listener_fd,
                      uint32_t listener_generation, int lwip_fd, const char *peer_ipv4, uint16_t peer_port);
    void (*close_v2)(void *context, uint32_t con_id, int lwip_fd, uint32_t generation, const char *reason);
} vw553_at_callbacks_t;

typedef struct {
    vw553_at_callbacks_t callbacks;
    void                *context;
    char                 line[VW553_AT_LINE_SIZE];
    size_t               used;
    size_t               remaining;
    size_t               frame_length;
    int                  remote_fd;
    int                  listener_fd;
    char                 peer_ipv4[16];
    uint16_t             peer_port;
    bool                 datagram;
    bool                 ble_data;
    bool                 separator_space;
    bool                 suppress_next_ok;
    unsigned             state;
    bool                 prompt_space;
} vw553_at_parser_t;

void vw553_at_parser_init(vw553_at_parser_t *parser, const vw553_at_callbacks_t *callbacks, void *context);
void vw553_at_parser_feed(vw553_at_parser_t *parser, const uint8_t *data, size_t length);
/* True while a length-delimited +IPD/+BLEDATA payload is still arriving. */
bool vw553_at_parser_in_frame(const vw553_at_parser_t *parser);
/* Abandons a partially received frame and resynchronises on the next line. */
void vw553_at_parser_drop_frame(vw553_at_parser_t *parser);

#endif
