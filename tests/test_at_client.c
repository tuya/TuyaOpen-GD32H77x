/**
 * @file test_at_client.c
 * @brief Exercise the pure AT UART AT transaction state machine.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "at_client.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t  payload[32];
    size_t   length;
    unsigned lines;
    unsigned datagrams;
    unsigned ble_packets;
    unsigned accepts;
    unsigned accepts_v2, closes_v2;
} capture_t;

static void event_line(void *context, const char *line, size_t length)
{
    (void)line;
    (void)length;
    ++((capture_t *)context)->lines;
}

static void event_ble_data(void *context, uint8_t conn_id, const uint8_t *data, size_t length, bool final)
{
    capture_t *capture = context;
    assert(conn_id == 3U && final);
    assert(capture->length + length <= sizeof(capture->payload));
    memcpy(capture->payload + capture->length, data, length);
    capture->length += length;
    ++capture->ble_packets;
}

static void event_data(void *context, int remote_fd, const uint8_t *data, size_t length, bool final)
{
    capture_t *capture = context;
    assert(remote_fd == 3);
    assert(capture->length + length <= sizeof(capture->payload));
    if (length) {
        memcpy(capture->payload + capture->length, data, length);
        capture->length += length;
    }
    (void) final;
}

static void event_datagram(void *context, int listener_fd, const char *ip, uint16_t port,
                           const uint8_t *data, size_t length, bool final)
{
    capture_t *c = context;
    assert(listener_fd == -1);
    assert(!strcmp(ip, "192.168.1.2") && port == 53 && final);
    assert(length == 3 && !memcmp(data, "xyz", 3)); ++c->datagrams;
}

static void event_accept(void *context, int listener, int client, const char *ip, uint16_t port)
{
    capture_t *c = context;
    assert(listener == 2 && client == 7 && !strcmp(ip, "10.0.0.1") && port == 1234);
    ++c->accepts;
}

static void event_accept_v2(void *context, uint32_t con_id, uint32_t gen, int listener,
                            uint32_t listener_gen, int lwip, const char *ip, uint16_t port)
{
    capture_t *c = context;
    assert(con_id == 6U && gen == 11U && listener == 2 && listener_gen == 21U && lwip == 71);
    assert(!strcmp(ip, "10.0.0.1") && port == 4321U); ++c->accepts_v2;
}

static void event_close_v2(void *context, uint32_t con_id, int lwip, uint32_t gen, const char *reason)
{
    capture_t *c = context;
    assert(con_id == 6U && gen == 11U && lwip == 71 && !strcmp(reason, "REMOTE_EOF")); ++c->closes_v2;
}

static void take_result(at_client_t *client, at_result_t expected)
{
    at_result_t result;
    assert(!at_client_busy(client));
    assert(at_client_take_result(client, &result));
    assert(result == expected);
}

static void transaction_and_prompt(void)
{
    capture_t                      capture = {0};
    const at_client_events_t events  = {.line = event_line, .data = event_data};
    at_client_t              client;
    at_client_init(&client, &events, &capture);
    assert(!at_client_begin(&client, "AT+CWJAP=\"secret\"", 17, false, 100, 1000));
    assert(at_client_set_wire_mode(&client, AT_WIRE_ATOMIC_NO_IPD_TRAILER));
    assert(at_client_begin(&client, "AT+CIPSEND=3,4", 14, true, 100, 1000));
    assert(at_client_busy(&client));
    assert(!at_client_begin(&client, "AT", 2, false, 100, 1000));

    char   command[AT_COMMAND_SIZE + 2U];
    size_t command_length = 0;
    assert(at_client_take_command(&client, command, sizeof(command), &command_length));
    assert(command_length == 16 && memcmp(command, "AT+CIPSEND=3,4\r\n", 16) == 0);
    assert(!at_client_take_command(&client, command, sizeof(command), &command_length));

    at_client_line(&client, "OK", 2); /* informational OK before > is not final */
    assert(at_client_busy(&client));
    at_client_prompt(&client);
    assert(at_client_busy(&client));
    at_client_line(&client, "SEND OK", 7);
    assert(at_client_busy(&client));
    at_client_line(&client, "OK", 2);
    take_result(&client, AT_RESULT_OK);
    assert(capture.lines == 3);
}

static void ipd_trailer_does_not_complete_transaction(void)
{
    static const uint8_t           stream[] = "+IPD,3,4: OK\0!\r\nOK\r\n";
    const at_client_events_t events   = {.line = event_line, .data = event_data};
    capture_t                      capture  = {0};
    at_client_t              client;
    at_parser_t              parser;
    at_client_init(&client, &events, &capture);
    assert(at_client_set_wire_mode(&client, AT_WIRE_ATOMIC_IPD_TRAILER));
    at_callbacks_t callbacks = at_client_parser_callbacks();
    at_parser_init(&parser, &callbacks, &client);
    assert(at_client_begin(&client, "AT+PING", 7, false, 1, 500));
    at_parser_feed(&parser, stream, sizeof(stream) - 1);
    assert(at_client_busy(&client));
    assert(capture.length == 4 && memcmp(capture.payload, "OK\0!", 4) == 0);
    at_parser_feed(&parser, (const uint8_t *)"OK\r\n", 4);
    take_result(&client, AT_RESULT_OK);
}

static void ipd_trailer_overtaken_by_prompt_is_dropped(void)
{
    capture_t                      capture = {0};
    const at_client_events_t events  = {.line = event_line, .data = event_data};
    at_client_t              client;
    at_client_init(&client, &events, &capture);
    assert(at_client_set_wire_mode(&client, AT_WIRE_ATOMIC_IPD_TRAILER));
    assert(at_client_begin(&client, "AT+CIPSEND=0,133", 16U, true, 0U, 500U));

    char   command[AT_COMMAND_SIZE + 2U];
    size_t command_length = 0U;
    assert(at_client_take_command(&client, command, sizeof(command), &command_length));

    /* A LAN datagram's +IPD arms the trailer OK, but the CIPSEND prompt
     * overtakes that OK on the wire. */
    at_client_datagram(&client, 9, "192.168.88.213", 7000U, (const uint8_t *)"x", 1U, true);
    assert(at_client_busy(&client));
    at_client_prompt(&client);
    assert(at_client_busy(&client));

    /* The late trailer OK must not complete the payload transaction. */
    at_client_line(&client, "OK", 2U);
    assert(at_client_busy(&client));

    at_client_line(&client, "SEND OK", 7U);
    at_client_line(&client, "OK", 2U);
    take_result(&client, AT_RESULT_OK);
}

static void stale_trailer_does_not_break_plain_ok_payload(void)
{
    static const char command[] = "AT+BLEDADATRANSSEND=0,1";
    at_client_t client;
    at_client_init(&client, NULL, NULL);
    assert(at_client_set_wire_mode(&client, AT_WIRE_ATOMIC_IPD_TRAILER));
    assert(at_client_begin(&client, command, sizeof(command) - 1U, true, 0U, 100U));
    at_client_datagram(&client, 9, "192.168.88.213", 7000U, (const uint8_t *)"x", 1U, true);
    at_client_prompt(&client);
    assert(at_client_busy(&client));
    at_client_line(&client, "OK", 2U);
    take_result(&client, AT_RESULT_OK);
}

static void ipd_trailer_overtaken_by_send_ok_is_swallowed(void)
{
    capture_t                      capture = {0};
    const at_client_events_t events  = {.line = event_line, .data = event_data};
    at_client_t              client;
    at_client_init(&client, &events, &capture);
    assert(at_client_set_wire_mode(&client, AT_WIRE_ATOMIC_IPD_TRAILER));
    assert(at_client_begin(&client, "AT+CIPSEND=0,133", 16U, true, 0U, 500U));

    char   command[AT_COMMAND_SIZE + 2U];
    size_t command_length = 0U;
    assert(at_client_take_command(&client, command, sizeof(command), &command_length));
    at_client_prompt(&client);
    assert(at_client_busy(&client));

    /* The server's first TLS record arrives between our payload and SEND OK. */
    at_client_data(&client, 3, (const uint8_t *)"\x16\x03", 2U, true);
    assert(at_client_busy(&client));
    at_client_line(&client, "SEND OK", 7U);
    assert(at_client_busy(&client));

    /* The IPD trailer OK is swallowed, the command's own OK completes it. */
    at_client_line(&client, "OK", 2U);
    assert(at_client_busy(&client));
    at_client_line(&client, "OK", 2U);
    take_result(&client, AT_RESULT_OK);
}

static void back_to_back_ipd_frames_keep_data_and_trailers(void)
{
    capture_t                      capture = {0};
    const at_client_events_t events  = {.line = event_line, .data = event_data};
    at_client_t              client;
    at_client_init(&client, &events, &capture);
    assert(at_client_set_wire_mode(&client, AT_WIRE_ATOMIC_IPD_TRAILER));
    assert(at_client_begin(&client, "AT+CIPSEND=0,133", 16U, true, 0U, 500U));

    char   command[AT_COMMAND_SIZE + 2U];
    size_t command_length = 0U;
    assert(at_client_take_command(&client, command, sizeof(command), &command_length));
    at_client_prompt(&client);
    assert(at_client_busy(&client));

    /* Two TLS records arrive before either trailer OK is processed. Neither
     * record may be dropped, and both trailers must be swallowed. */
    at_client_data(&client, 3, (const uint8_t *)"ab", 2U, true);
    at_client_data(&client, 3, (const uint8_t *)"cd", 2U, true);
    assert(capture.length == 4U && !memcmp(capture.payload, "abcd", 4U));
    assert(at_client_busy(&client));

    at_client_line(&client, "OK", 2U);
    at_client_line(&client, "OK", 2U);
    assert(at_client_busy(&client));
    at_client_line(&client, "SEND OK", 7U);
    at_client_line(&client, "OK", 2U);
    take_result(&client, AT_RESULT_OK);
}

static void ble_data_is_forwarded(void)
{
    static const uint8_t stream[] = "+BLEDATA,3,5:ab\0cd\r\n";
    capture_t capture = {0};
    const at_client_events_t events = {.ble_data = event_ble_data};
    at_client_t client;
    at_parser_t parser;
    at_client_init(&client, &events, &capture);
    assert(at_client_set_wire_mode(&client, AT_WIRE_ATOMIC_IPD_TRAILER));
    at_callbacks_t callbacks = at_client_parser_callbacks();
    at_parser_init(&parser, &callbacks, &client);
    assert(at_client_begin(&client, "AT+PING", 7, false, 1, 500));
    at_parser_feed(&parser, stream, sizeof(stream) - 1U);
    assert(capture.ble_packets == 1U && capture.length == 5U);
    assert(!memcmp(capture.payload, "ab\0cd", 5U));
    assert(at_client_busy(&client));
    at_parser_feed(&parser, (const uint8_t *)"OK\r\n", 4);
    take_result(&client, AT_RESULT_OK);
}

static void ble_data_without_trailer_allows_next_send(void)
{
    static const char command[] = "AT+BLEDADATRANSSEND=0,1";
    at_client_t client;
    at_client_init(&client, NULL, NULL);
    assert(at_client_set_wire_mode(&client, AT_WIRE_ATOMIC_NO_IPD_TRAILER));
    at_client_ble_data(&client, 0U, (const uint8_t *)"x", 1U, true);
    assert(at_client_begin(&client, command, sizeof(command) - 1U, true, 0U, 100U));
    at_client_prompt(&client);
    at_client_line(&client, "OK", 2U);
    take_result(&client, AT_RESULT_OK);
}

static void timeout_requires_resynchronization(void)
{
    at_client_t client;
    at_client_init(&client, NULL, NULL);
    assert(at_client_begin(&client, "AT", 2, false, UINT32_MAX - 10U, 20));
    at_client_poll(&client, 9U);
    assert(!at_client_begin(&client, "AT", 2, false, 6, 20));
    assert(!at_client_resynchronize(&client));
    take_result(&client, AT_RESULT_TIMEOUT);
    assert(at_client_set_wire_mode(&client, AT_WIRE_ATOMIC_NO_IPD_TRAILER) == false);
    assert(at_client_resynchronize(&client));
    assert(at_client_begin(&client, "AT", 2, false, 7, 20));
    at_client_cancel(&client);
    take_result(&client, AT_RESULT_CANCELLED);
    assert(!at_client_begin(&client, "AT", 2, false, 8, 20));
    assert(at_client_resynchronize(&client));
}

static void prompt_timeout_is_terminal(void)
{
    at_client_t client;
    at_client_init(&client, NULL, NULL);
    assert(at_client_set_wire_mode(&client, AT_WIRE_ATOMIC_IPD_TRAILER));
    assert(at_client_begin(&client, "AT+CIPSEND=1,3", 14U, true, 20U, 10U));
    at_client_poll(&client, 30U);
    at_result_t result;
    assert(at_client_take_result(&client, &result));
    assert(result == AT_RESULT_TIMEOUT);
    assert(!at_client_begin(&client, "AT", 2U, false, 31U, 10U));
}

static void error_and_protocol_failure(void)
{
    at_client_t client;
    at_client_init(&client, NULL, NULL);
    assert(at_client_set_wire_mode(&client, AT_WIRE_ATOMIC_NO_IPD_TRAILER));
    assert(at_client_begin(&client, "AT+CWSTATE?", 11, false, 0, 50));
    at_client_line(&client, "ERROR", 5);
    take_result(&client, AT_RESULT_ERROR);

    assert(at_client_begin(&client, "AT+CWSTATE?", 11, false, 0, 50));
    at_client_fault(&client, AT_BAD_HEADER);
    take_result(&client, AT_RESULT_PROTOCOL);
    assert(!at_client_begin(&client, "AT+CWSTATE?", 11, false, 1, 50));
}

static void malformed_ipd_and_probe_data_fail_closed(void)
{
    at_client_t client;
    at_client_init(&client, NULL, NULL);
    assert(at_client_set_wire_mode(&client, AT_WIRE_ATOMIC_IPD_TRAILER));
    assert(at_client_begin(&client, "AT+PING", 7, false, 0, 100));
    at_client_data(&client, 3, (const uint8_t *)"x", 1, true);
    /* A non-OK line may overtake the +IPD trailer OK. It is the command's own
     * result and must be reported as such instead of poisoning the link. */
    at_client_line(&client, "ERROR", 5);
    take_result(&client, AT_RESULT_ERROR);
    assert(!at_client_begin(&client, "AT+PING", 7, false, 1, 100));
    assert(at_client_resynchronize(&client));

    at_client_init(&client, NULL, NULL);
    assert(at_client_begin(&client, "AT", 2, false, 0, 100));
    at_client_data(&client, 3, (const uint8_t *)"x", 1, true);
    take_result(&client, AT_RESULT_PROTOCOL);
    assert(!at_client_begin(&client, "AT", 2, false, 1, 100));
}

static void idle_ipd_tail_cannot_complete_next_command(void)
{
    at_client_t client;
    at_client_init(&client, NULL, NULL);
    assert(at_client_set_wire_mode(&client, AT_WIRE_ATOMIC_IPD_TRAILER));
    at_client_data(&client, 3, (const uint8_t *)"x", 1, true);
    assert(!at_client_begin(&client, "AT+PING", 7, false, 0, 100));
    assert(!at_client_set_wire_mode(&client, AT_WIRE_ATOMIC_NO_IPD_TRAILER));
    at_client_line(&client, "OK", 2); /* Drain the prior IPD frame trailer. */
    assert(at_client_begin(&client, "AT+PING", 7, false, 1, 100));
    at_client_line(&client, "OK", 2);
    take_result(&client, AT_RESULT_OK);
}

static void unsupported_ble_binary_frame_cannot_complete_probe(void)
{
    /* BLEDATA's length-delimited payload contains a plausible AT terminal line. */
    static const uint8_t stream[] = "+BLEDATA:1,6,\r\nOK\r\n";
    at_client_t    client;
    at_parser_t    parser;
    at_client_init(&client, NULL, NULL);
    at_callbacks_t callbacks = at_client_parser_callbacks();
    at_parser_init(&parser, &callbacks, &client);
    assert(at_client_begin(&client, "AT", 2, false, 0, 100));
    at_parser_feed(&parser, stream, sizeof(stream) - 1U);
    take_result(&client, AT_RESULT_PROTOCOL);
    assert(!at_client_begin(&client, "AT", 2, false, 1, 100));
}

static void udp_datagram_and_accept_are_out_of_band(void)
{
    static const uint8_t stream[] = "+IPD,192.168.1.2:53, 3: xyz\r\nOK\r\n+TY553XCONN:2,7,10.0.0.1,1234\r\n";
    capture_t c = {0};
    const at_client_events_t e = {.line = event_line, .data = event_data,
                                        .datagram = event_datagram, .accept = event_accept};
    at_client_t client; at_parser_t parser;
    at_client_init(&client, &e, &c);
    assert(at_client_set_wire_mode(&client, AT_WIRE_ATOMIC_IPD_TRAILER));
    at_callbacks_t callbacks = at_client_parser_callbacks();
    at_parser_init(&parser, &callbacks, &client);
    assert(at_client_begin(&client, "AT+PING", 7, false, 0, 100));
    at_parser_feed(&parser, stream, sizeof(stream)-1);
    assert(c.datagrams == 1 && c.accepts == 1 && c.lines == 0 && at_client_busy(&client));
    at_client_line(&client, "OK", 2);
    take_result(&client, AT_RESULT_OK);
}

static void accept_close_v2_are_out_of_band(void)
{
    static const char stream[] = "+TY553XCONN2:6,11,2,21,71,10.0.0.1,4321\r\nOK\r\n"
                                 "+TY553XCLOSE:6,71,11,REMOTE_EOF\r\nOK\r\n";
    capture_t c = {0};
    const at_client_events_t events = {.line = event_line, .accept_v2 = event_accept_v2,
                                              .close_v2 = event_close_v2};
    at_client_t client; at_parser_t parser;
    at_client_init(&client, &events, &c);
    assert(at_client_set_wire_mode(&client, AT_WIRE_ATOMIC_IPD_TRAILER));
    at_callbacks_t callbacks = at_client_parser_callbacks();
    at_parser_init(&parser, &callbacks, &client);
    assert(at_client_begin(&client, "AT+CIPSTATE?", 12, false, 0, 100));
    for (size_t i = 0; i < sizeof(stream) - 1U; ++i) at_parser_feed(&parser, (const uint8_t *)stream + i, 1U);
    assert(c.accepts_v2 == 1 && c.closes_v2 == 1 && c.lines == 0 && at_client_busy(&client));
    at_parser_feed(&parser, (const uint8_t *)"OK\r\n", 4U);
    take_result(&client, AT_RESULT_OK);
}

static void ble_lines_and_ready_fail_closed(void)
{
    at_client_t client;
    at_client_init(&client, NULL, NULL);
    assert(at_client_begin(&client, "AT", 2, false, 0, 100));
    at_client_line(&client, "+BLECONN:1,0", 12);
    take_result(&client, AT_RESULT_PROTOCOL);
    assert(!at_client_begin(&client, "AT", 2, false, 1, 100));

    assert(at_client_resynchronize(&client));
    assert(at_client_begin(&client, "AT", 2, false, 2, 100));
    at_client_line(&client, "READY", 5);
    take_result(&client, AT_RESULT_RESET);
    assert(!at_client_begin(&client, "AT", 2, false, 3, 100));
}

static void ble_status_urc_does_not_poison_transaction(void)
{
    static const char command[] = "AT+CWJAP_CUR=\"Pico\",\"Pico123456\"";
    at_client_t client;
    capture_t capture = {0};
    const at_client_events_t events = {.line = event_line};
    at_client_init(&client, &events, &capture);
    assert(at_client_set_wire_mode(&client, AT_WIRE_ATOMIC_NO_IPD_TRAILER));
    assert(at_client_begin(&client, command, sizeof(command) - 1U, false, 0, 100));
    /* The module reports BLE link changes while an unrelated AT command is
     * still waiting, which is exactly what broke CWJAP after provisioning. */
    static const char connected[] = "connect success. conn idx:0, interval:0x18, latancy:0x0, supv_tout:0x48";
    static const char connected_urc[] = "+TY553XBLE:CONN,0";
    at_client_line(&client, connected, sizeof(connected) - 1U);
    at_client_line(&client, connected_urc, sizeof(connected_urc) - 1U);
    assert(at_client_busy(&client));
    at_client_line(&client, "OK", 2);
    take_result(&client, AT_RESULT_OK);
    assert(capture.lines == 3U);

    assert(at_client_begin(&client, command, sizeof(command) - 1U, false, 1, 100));
    static const char disconnected_urc[] = "+TY553XBLE:DISC,0,152";
    at_client_line(&client, disconnected_urc, sizeof(disconnected_urc) - 1U);
    assert(at_client_busy(&client));
    at_client_line(&client, "ERROR", 5);
    take_result(&client, AT_RESULT_ERROR);
}

static void bounded_and_probe_only(void)
{
    at_client_t client;
    at_client_init(&client, NULL, NULL);
    assert(!at_client_begin(&client, "AT\r\n", 4, false, 0, 10));
    assert(!at_client_begin(&client, "AT", 2, false, 0, 0));
    assert(at_client_begin(&client, "AT", 2, false, 0, 10));
    at_client_line(&client, "OK", 2);
    take_result(&client, AT_RESULT_OK);
    assert(at_client_begin(&client, "AT+RST", 6, false, 1, 10));
    at_client_line(&client, "OK", 2);
    take_result(&client, AT_RESULT_OK);
    assert(!at_client_begin(&client, "AT+RST", 6, true, 2, 10));
}

static void prefixed_ok_completes_cipstart(void)
{
    static const char command[] = "AT+CIPSTART=0";
    at_client_t client;
    at_client_init(&client, NULL, NULL);
    assert(at_client_set_wire_mode(&client, AT_WIRE_ATOMIC_NO_IPD_TRAILER));
    assert(at_client_begin(&client, command, sizeof(command) - 1U, false, 0, 100));
    at_client_line(&client, "0,OK", 4);
    take_result(&client, AT_RESULT_OK);
}

int main(void)
{
    transaction_and_prompt();
    ipd_trailer_does_not_complete_transaction();
    ipd_trailer_overtaken_by_prompt_is_dropped();
    stale_trailer_does_not_break_plain_ok_payload();
    ipd_trailer_overtaken_by_send_ok_is_swallowed();
    back_to_back_ipd_frames_keep_data_and_trailers();
    ble_data_is_forwarded();
    ble_data_without_trailer_allows_next_send();
    timeout_requires_resynchronization();
    prompt_timeout_is_terminal();
    error_and_protocol_failure();
    malformed_ipd_and_probe_data_fail_closed();
    idle_ipd_tail_cannot_complete_next_command();
    unsupported_ble_binary_frame_cannot_complete_probe();
    udp_datagram_and_accept_are_out_of_band();
    accept_close_v2_are_out_of_band();
    ble_lines_and_ready_fail_closed();
    ble_status_urc_does_not_poison_transaction();
    bounded_and_probe_only();
    prefixed_ok_completes_cipstart();
    puts("UART AT client checks passed");
    return 0;
}
