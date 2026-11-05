/**
 * @file test_at_parser.c
 * @brief Exercise stream fragmentation, binary payloads, and malformed UART AT input.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "at_parser.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t  payload[64];
    size_t   received;
    unsigned lines;
    unsigned packets;
    unsigned ble_packets;
    unsigned prompts;
    unsigned faults;
    int      remote_fd;
    char     last_line[32];
    char     peer[16];
    uint16_t port;
    unsigned datagrams;
    unsigned accepts;
    int listener_fd, client_fd;
    unsigned accepts_v2, closes_v2;
    uint32_t con_id, generation, listener_generation;
    int lwip_fd;
    char reason[24];
} capture_t;

static void line(void *context, const char *text, size_t length)
{
    capture_t *capture = context;
    assert(length < sizeof(capture->last_line));
    memcpy(capture->last_line, text, length + 1);
    ++capture->lines;
}

static void data(void *context, int remote_fd, const uint8_t *bytes, size_t length, bool final)
{
    capture_t *capture = context;
    assert(capture->received + length <= sizeof(capture->payload));
    if (length) {
        memcpy(capture->payload + capture->received, bytes, length);
    }
    capture->received += length;
    capture->remote_fd = remote_fd;
    capture->packets += final;
}

static void ble_data(void *context, uint8_t conn_id, const uint8_t *bytes, size_t length, bool final)
{
    capture_t *capture = context;
    assert(capture->received + length <= sizeof(capture->payload));
    if (length) memcpy(capture->payload + capture->received, bytes, length);
    capture->received += length;
    capture->remote_fd = conn_id;
    capture->ble_packets += final;
}

static void prompt(void *context)
{
    ++((capture_t *)context)->prompts;
}

static void fault(void *context, at_fault_t reason)
{
    (void)reason;
    ++((capture_t *)context)->faults;
}

static void datagram(void *context, int listener_fd, const char *ip, uint16_t port,
                     const uint8_t *bytes, size_t length, bool final)
{
    capture_t *c = context;
    c->listener_fd = listener_fd;
    assert(strlen(ip) < sizeof(c->peer)); strcpy(c->peer, ip); c->port = port;
    assert(c->received + length <= sizeof(c->payload));
    if (length) memcpy(c->payload + c->received, bytes, length);
    c->received += length; c->datagrams += final;
}

static void accept_event(void *context, int listener, int client, const char *ip, uint16_t port)
{
    capture_t *c = context;
    c->listener_fd = listener; c->client_fd = client; c->port = port;
    strcpy(c->peer, ip); ++c->accepts;
}

static void accept_v2_event(void *context, uint32_t con_id, uint32_t generation, int listener,
                            uint32_t listener_generation, int lwip_fd, const char *ip, uint16_t port)
{
    capture_t *c = context; c->con_id = con_id; c->generation = generation; c->listener_generation = listener_generation;
    c->listener_fd = listener; c->lwip_fd = lwip_fd; c->port = port; strcpy(c->peer, ip); ++c->accepts_v2;
}

static void close_v2_event(void *context, uint32_t con_id, int lwip_fd, uint32_t generation, const char *reason)
{
    capture_t *c = context; c->con_id = con_id; c->generation = generation; c->lwip_fd = lwip_fd;
    strcpy(c->reason, reason); ++c->closes_v2;
}

static const at_callbacks_t callbacks = {.line = line, .data = data, .ble_data = ble_data, .prompt = prompt, .fault = fault,
                                               .datagram = datagram, .accept = accept_event,
                                               .accept_v2 = accept_v2_event, .close_v2 = close_v2_event};

static void fragmented_binary(void)
{
    static const uint8_t stream[] = "\r\n+IPD,7,8: A\0\r\nOK> \r\nOK\r\n+CLOSED:7\r\n";
    for (size_t split = 0; split <= sizeof(stream) - 1; ++split) {
        capture_t         capture = {0};
        at_parser_t parser;
        at_parser_init(&parser, &callbacks, &capture);
        at_parser_feed(&parser, stream, split);
        at_parser_feed(&parser, stream + split, sizeof(stream) - 1 - split);
        assert(capture.received == 8 && capture.packets == 1 && capture.remote_fd == 7);
        assert(memcmp(capture.payload, "A\0\r\nOK> ", 8) == 0);
        assert(capture.lines == 2 && strcmp(capture.last_line, "+CLOSED:7") == 0);
        assert(capture.prompts == 0 && capture.faults == 0);
    }
}

static void byte_at_a_time(void)
{
    static const uint8_t stream[] = "> \r\nSEND OK\r\n+IPD,9,3:  x \r\n+IPD,8,0: \r\nOK\r\n";
    capture_t            capture  = {0};
    at_parser_t    parser;
    at_parser_init(&parser, &callbacks, &capture);
    for (size_t i = 0; i < sizeof(stream) - 1; ++i) {
        at_parser_feed(&parser, stream + i, 1);
    }
    assert(capture.received == 3 && memcmp(capture.payload, " x ", 3) == 0);
    assert(capture.packets == 2 && capture.remote_fd == 8 && capture.prompts == 1);
    assert(capture.lines == 2 && capture.faults == 0);
}

static void ble_data_transfer(void)
{
    static const uint8_t wire[] = "+BLEDATA,3,6:A\0\r\nOK\r\nOK\r\n";
    for (size_t split = 0U; split < sizeof(wire); ++split) {
        capture_t capture = {0};
        at_parser_t parser;
        at_parser_init(&parser, &callbacks, &capture);
        at_parser_feed(&parser, wire, split);
        at_parser_feed(&parser, wire + split, sizeof(wire) - 1U - split);
        assert(capture.ble_packets == 1U && capture.remote_fd == 3 && capture.received == 6U);
        assert(memcmp(capture.payload, "A\0\r\nOK", 6U) == 0);
        assert(capture.lines == 1U && strcmp(capture.last_line, "OK") == 0 && capture.faults == 0U);
    }
}

static void malformed_input(void)
{
    static const char *bad[] = {"+IPD,2147483648,1: \r\n", "+IPD,1,65536: \r\n", "+IPD,-1,1: \r\n",
                                "+IPD,1,x: \r\n",          "+IPD,1,1:X\r\n",     "+IPD,999999999999999999999,1: \r\n"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        capture_t         capture = {0};
        at_parser_t parser;
        at_parser_init(&parser, &callbacks, &capture);
        at_parser_feed(&parser, (const uint8_t *)bad[i], strlen(bad[i]));
        at_parser_feed(&parser, (const uint8_t *)"OK\r\n", 4);
        assert(capture.faults == 1 && capture.received == 0 && capture.lines == 1);
        assert(strcmp(capture.last_line, "OK") == 0);
    }
    capture_t         capture = {0};
    at_parser_t parser;
    at_parser_init(&parser, &callbacks, &capture);
    uint8_t oversized[AT_LINE_SIZE + 5];
    memset(oversized, 'A', sizeof(oversized));
    at_parser_feed(&parser, oversized, sizeof(oversized));
    at_parser_feed(&parser, (const uint8_t *)"\r\nOK\r\n", 6);
    assert(capture.faults == 1 && capture.lines == 1);

    memset(&capture, 0, sizeof(capture));
    at_parser_init(&parser, &callbacks, &capture);
    static const uint8_t nul_header[] = "+IPD,1,1\0ignored: X\r\nOK\r\n";
    at_parser_feed(&parser, nul_header, sizeof(nul_header) - 1);
    /* A NUL inside a text line is vendor garbage, not a binary payload. Split
     * there and keep resynchronising instead of poisoning the transport. */
    assert(capture.faults == 0 && capture.received == 0 && capture.lines == 3);
    assert(strcmp(capture.last_line, "OK") == 0);
}

static void udp_and_accept(void)
{
    static const uint8_t wire[] = "+IPD,192.168.1.2:65535, 7: A\r\nOK\r\n\r\nOK\r\n";
    capture_t c = {0}; at_parser_t p;
    at_parser_init(&p, &callbacks, &c);
    for (size_t i = 0; i < sizeof(wire)-1; ++i) at_parser_feed(&p, wire+i, 1);
    assert(c.datagrams == 1 && c.received == 7 && !memcmp(c.payload, "A\r\nOK\r\n", 7));
    assert(c.listener_fd == -1);
    assert(!strcmp(c.peer, "192.168.1.2") && c.port == 65535 && c.lines == 1);
    static const char accept[] = "+TY553XCONN:2,7,10.0.0.1,0\r\n";
    at_parser_feed(&p, (const uint8_t *)accept, sizeof(accept)-1);
    assert(c.accepts == 1 && c.listener_fd == 2 && c.client_fd == 7 && c.port == 0);
    static const char zero[] = "+IPD,0.0.0.0:0, 0: \r\n";
    at_parser_feed(&p, (const uint8_t *)zero, sizeof(zero)-1);
    assert(c.datagrams == 2 && c.received == 7 && c.port == 0 && c.listener_fd == -1);
}

static void udp_server_with_listener_descriptor(void)
{
    static const uint8_t wire[] = "+IPD,17,192.0.2.9:5000, 3: xyz\r\nOK\r\n";
    capture_t c = {0};
    at_parser_t p;
    at_parser_init(&p, &callbacks, &c);
    at_parser_feed(&p, wire, sizeof(wire) - 1U);
    assert(c.datagrams == 1 && c.received == 3 && !memcmp(c.payload, "xyz", 3));
    assert(c.listener_fd == 17 && !strcmp(c.peer, "192.0.2.9") && c.port == 5000);
    assert(c.faults == 0);
}

static void bad_udp_and_accept(void)
{
    static const char *bad[] = {"+IPD,256.1.1.1:1, 1: x\r\n", "+IPD,1.2.3:1, 1: x\r\n",
        "+IPD,1.2.3.4:65536, 1: x\r\n", "+IPD,1.2.3.4:999999999999, 1: x\r\n",
        "+IPD,1.2.3.4:1, 65536: x\r\n", "+IPD,1.2.3.4:1, 1:x\r\n",
        "+IPD,1.2.3.4::1, 1: x\r\n", "+IPD,1.2.3.4:broken:1, 1: x\r\n",
        "+TY553XCONN:2,7,1.2.999.4,80\r\n", "+TY553XCONN:2147483648,7,1.2.3.4,80\r\n"};
    for (size_t i=0; i<sizeof(bad)/sizeof(bad[0]); ++i) {
        capture_t c={0}; at_parser_t p; at_parser_init(&p,&callbacks,&c);
        at_parser_feed(&p,(const uint8_t *)bad[i],strlen(bad[i]));
        assert(c.faults == 1 && c.datagrams == 0 && c.accepts == 0);
    }

    /* A plausible peer prefix followed by a huge broken port/header must fault before any write past line[]. */
    uint8_t long_header[AT_LINE_SIZE + 32U];
    size_t n = (size_t)snprintf((char *)long_header, sizeof(long_header), "+IPD,1.2.3.4:");
    memset(long_header + n, '9', sizeof(long_header) - n);
    long_header[sizeof(long_header) - 1U] = ':';
    capture_t c = {0}; at_parser_t p; at_parser_init(&p, &callbacks, &c);
    at_parser_feed(&p, long_header, sizeof(long_header));
    assert(c.faults == 1 && c.datagrams == 0 && c.accepts == 0);
}

static void accept_close_v2(void)
{
    static const char stream[] = "+TY553XCONN2:8,4294967295,3,123456,77,192.0.2.9,65535\r\n"
                                 "+TY553XCLOSE:8,77,4294967295,REMOTE_EOF\r\nOK\r\n";
    capture_t c = {0}; at_parser_t p; at_parser_init(&p, &callbacks, &c);
    for (size_t i = 0; i < sizeof(stream) - 1U; ++i) at_parser_feed(&p, (const uint8_t *)stream + i, 1U);
    /* The trailing OK belongs to the CLOSE event and is swallowed, not a line. */
    assert(c.accepts_v2 == 1 && c.closes_v2 == 1 && c.lines == 0 && c.faults == 0);
    assert(c.con_id == 8U && c.generation == UINT32_MAX && c.listener_generation == 123456U &&
           c.lwip_fd == 77 && c.port == 65535U);
    assert(strcmp(c.peer, "192.0.2.9") == 0 && strcmp(c.reason, "REMOTE_EOF") == 0);
    static const char *bad[] = {
        "+TY553XCONN2:8,4294967296,3,1,77,192.0.2.9,80\r\n",
        "+TY553XCONN2:8,1,3,1,77,192.0.2.999,80\r\n",
        "+TY553XCONN2:8,1,3,1,77,192.0.2.9,65536\r\n",
        "+TY553XCLOSE:8,77,4294967296,REMOTE_EOF\r\n",
        "+TY553XCLOSE:8,77,1,OTHER\r\n",
        "+TY553XCLOSE:8,77,1,REMOTE_EOF,extra\r\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        memset(&c, 0, sizeof(c)); at_parser_init(&p, &callbacks, &c);
        at_parser_feed(&p, (const uint8_t *)bad[i], strlen(bad[i]));
        assert(c.faults == 1 && c.accepts_v2 == 0 && c.closes_v2 == 0 && c.lines == 0);
    }
}

static void zero_length_frame_keeps_following_line(void)
{
    capture_t c = {0};
    at_parser_t p;
    at_parser_init(&p, &callbacks, &c);
    static const uint8_t frame[] = "+IPD,4,0: \r\nOK\r\n";
    at_parser_feed(&p, frame, sizeof(frame) - 1U);
    assert(c.faults == 0U);
    assert(c.received == 0U && c.packets == 1U);
    assert(c.lines == 1U && strcmp(c.last_line, "OK") == 0);
}

static void tail_variants_are_accepted(void)
{
    capture_t c = {0};
    at_parser_t p;
    at_parser_init(&p, &callbacks, &c);
    /* No CRLF/OK between two frames. */
    static const uint8_t back_to_back[] = "+IPD,4,2: hi+IPD,4,2: yo";
    at_parser_feed(&p, back_to_back, sizeof(back_to_back) - 1U);
    assert(c.faults == 0U && c.packets == 2U && c.received == 4U);
    assert(memcmp(c.payload, "hiyo", 4U) == 0);
}

static void truncated_frame_can_be_dropped(void)
{
    capture_t c = {0};
    at_parser_t p;
    at_parser_init(&p, &callbacks, &c);
    static const uint8_t partial[] = "+IPD,4,5: abc";
    at_parser_feed(&p, partial, sizeof(partial) - 1U);
    assert(at_parser_in_frame(&p));
    assert(c.packets == 0U);
    /* The transport calls this when the stream goes idle. */
    at_parser_drop_frame(&p);
    assert(!at_parser_in_frame(&p));
    static const uint8_t next[] = "+IPD,4,2: ok\r\nOK\r\n";
    at_parser_feed(&p, next, sizeof(next) - 1U);
    assert(c.faults == 0U);
    /* Bytes already received before the stall were delivered; the drop only
     * unblocks the parser. */
    assert(c.packets == 1U && c.received == 5U && memcmp(c.payload, "abcok", 5U) == 0);
}

static void frame_state_tracks_partial_payload(void)
{
    capture_t c = {0};
    at_parser_t p;
    at_parser_init(&p, &callbacks, &c);
    assert(!at_parser_in_frame(&p));
    static const uint8_t first[] = "+IPD,4,5: ab";
    at_parser_feed(&p, first, sizeof(first) - 1U);
    assert(at_parser_in_frame(&p));
    at_parser_feed(&p, (const uint8_t *)"cde", 3U);
    assert(!at_parser_in_frame(&p));
    assert(c.received == 5U && c.packets == 1U && memcmp(c.payload, "abcde", 5U) == 0);
}

static void nul_heavy_payload_keeps_full_length(void)
{
    capture_t c = {0};
    at_parser_t p;
    at_parser_init(&p, &callbacks, &c);
    /* 6-byte payload made mostly of NULs: a %c style formatter in the module
     * would truncate this to a short frame. */
    static const uint8_t frame[] = "+IPD,4,6: \0\0A\0\0B\r\nOK\r\n";
    for (size_t i = 0; i < sizeof(frame) - 1U; ++i) {
        at_parser_feed(&p, frame + i, 1U);
    }
    assert(c.faults == 0U);
    assert(c.received == 6U && c.packets == 1U);
    static const uint8_t expected[] = {0x00, 0x00, 'A', 0x00, 0x00, 'B'};
    assert(memcmp(c.payload, expected, sizeof(expected)) == 0);
}

static void control_bytes_split_lines_instead_of_faulting(void)
{
    /* Vendor AT_RSP_START(10) truncates "SEND FAIL\r\n" and emits two bytes of
     * heap garbage instead of CRLF: SEND FAIL \0 \x06 ERROR \r\n. */
    static const uint8_t stream[] = "SEND FAIL\0" "\x06" "ERROR\r\n";
    capture_t c = {0};
    at_parser_t p;
    at_parser_init(&p, &callbacks, &c);
    at_parser_feed(&p, stream, sizeof(stream) - 1U);
    assert(c.faults == 0U && c.lines == 2U);
    assert(strcmp(c.last_line, "ERROR") == 0);
}

static void urc_trailer_ok_is_swallowed_once(void)
{
    static const char stream[] = "+TY553XCONN2:8,1,3,1,77,192.0.2.9,80\r\nOK\r\nOK\r\n";
    capture_t c = {0};
    at_parser_t p;
    at_parser_init(&p, &callbacks, &c);
    at_parser_feed(&p, (const uint8_t *)stream, sizeof(stream) - 1U);
    assert(c.faults == 0U && c.accepts_v2 == 1U);
    /* First OK belongs to the URC; the second is a real command result. */
    assert(c.lines == 1U && strcmp(c.last_line, "OK") == 0);
}

int main(void)
{
    fragmented_binary();
    byte_at_a_time();
    ble_data_transfer();
    malformed_input();
    udp_and_accept();
    udp_server_with_listener_descriptor();
    bad_udp_and_accept();
    accept_close_v2();
    frame_state_tracks_partial_payload();
    zero_length_frame_keeps_following_line();
    tail_variants_are_accepted();
    truncated_frame_can_be_dropped();
    nul_heavy_payload_keeps_full_length();
    control_bytes_split_lines_instead_of_faulting();
    urc_trailer_ok_is_swallowed_once();
    puts("UART AT parser checks passed");
    return 0;
}
