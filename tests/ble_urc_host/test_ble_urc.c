/* Cross-end regression: real VW553 BLE emitter -> real H779 AT parser/client. */
#include "at_client.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t wire[2048];
static size_t wire_length;
static unsigned writes;
static void at_hw_send(char *data, int length)
{
    assert(length >= 0 && wire_length + (size_t)length <= sizeof(wire));
    memcpy(wire + wire_length, data, (size_t)length);
    wire_length += (size_t)length;
    ++writes;
}
#define DEBUG_ASSERT assert
#define sys_zalloc(length) calloc(1, (length))
#define sys_mfree free
#define sys_memcpy memcpy
#define sys_memset memset
#define co_snprintf snprintf
/* Extracted verbatim from the gateway's current macros and emitter function. */
#include "gateway_ble_rx.inc"

static uint8_t received[512];
static size_t received_length;
static unsigned completed;
static void ble_event(void *context, uint8_t id, const uint8_t *data, size_t length, bool final)
{
    (void)context;
    assert(id == 0);
    assert(received_length + length <= sizeof(received));
    if (length) memcpy(received + received_length, data, length);
    received_length += length;
    completed += final ? 1U : 0U;
}

int main(void)
{
    uint8_t payload[129];
    for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = (uint8_t)(i * 17);
    memcpy(payload + 10, "\r\nOK\r\nSEND OK\r\n>\0", 18);
    for (size_t size_index = 0; size_index < 4; ++size_index) {
        const size_t sizes[] = {0, 52, 129, 512};
        uint8_t large[512];
        for (size_t i = 0; i < sizeof(large); ++i) large[i] = payload[i % sizeof(payload)];
        size_t length = sizes[size_index];
        wire_length = 0; writes = 0;
        at_ble_normal_trans_rx_callback(0, (uint16_t)length, large);
        assert(writes == 1); /* Header + binary + CRLF must remain one atomic write. */
        for (unsigned phase = 0; phase < 2; ++phase) {
            for (size_t split = 0; split <= wire_length; ++split) {
                at_client_t client;
                at_parser_t parser;
                const at_client_events_t events = {.ble_data = ble_event};
                received_length = 0; completed = 0;
                at_client_init(&client, &events, NULL);
                assert(at_client_set_wire_mode(&client, AT_WIRE_ATOMIC_NO_IPD_TRAILER));
                at_callbacks_t callbacks = at_client_parser_callbacks();
                at_parser_init(&parser, &callbacks, &client);
                const char *command = "AT+CIPSEND=1,182";
                assert(at_client_begin(&client, command, strlen(command), true, 0, 1000));
                char output[128]; size_t output_length;
                assert(at_client_take_command(&client, output, sizeof(output), &output_length));
                if (phase == 0) at_parser_feed(&parser, (const uint8_t *)">\r\n", 3);
                at_parser_feed(&parser, wire, split);
                at_parser_feed(&parser, wire + split, wire_length - split);
                assert(received_length == length && completed == 1);
                assert(memcmp(received, large, length) == 0);
                assert(at_client_busy(&client) && !client.tainted);
                if (phase == 1) at_parser_feed(&parser, (const uint8_t *)">\r\n", 3);
                at_parser_feed(&parser, (const uint8_t *)"SEND OK\r\n", 9);
                assert(at_client_busy(&client) && client.saw_send_ok);
                at_parser_feed(&parser, (const uint8_t *)"OK\r\n", 4);
                at_result_t result;
                assert(at_client_take_result(&client, &result) && result == AT_RESULT_OK);
            }
        }
    }
    puts("BLE URC/CIPSEND interleaving and all byte splits passed");
    return 0;
}
