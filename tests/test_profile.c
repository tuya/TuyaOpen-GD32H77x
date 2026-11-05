#include "at_profile.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void default_command_gate_is_narrow(void)
{
    assert(at_uart_policy_command_supported("AT", 2U, false));
    assert(!at_uart_policy_command_supported("AT", 2U, true));
    assert(at_uart_policy_command_supported("AT+RST", 6U, false));
    assert(!at_uart_policy_command_supported("AT+RST", 6U, true));
    assert(at_uart_policy_command_supported("AT+TY553XCAP?", 13U, false));
    assert(!at_uart_policy_command_supported("AT+TY553XCAP?", 13U, true));
    assert(!at_uart_policy_command_supported("AT+GMR", 6U, false));
    assert(!at_uart_policy_command_supported("AT+CIPSTART", 11U, false));
    assert(!at_uart_policy_command_supported("AT+CWJAP", 8U, false));
    assert(!at_uart_policy_command_supported(NULL, 2U, false));
    assert(!at_uart_policy_command_supported("AT", 1U, false));
}

static void capability_negotiation_is_exact_and_dynamic(void)
{
    static const char good[] = "+TY553XCAP:3,ATOMIC_IPD,MULTI_SERVER,TCP_ACCEPT2,CLOSE_EVENT,CLOSE_ACK,GEN32,STATE_QUERY,UDP_SEND_ID=8,MAX_SEND=2048,MAX_CLIENTS=8,MAX_SERVERS=4\r\nOK\r\n";
    static const char missing[] = "+TY553XCAP:3,ATOMIC_IPD,MULTI_SERVER,TCP_ACCEPT2,CLOSE_EVENT,GEN32,STATE_QUERY,UDP_SEND_ID=8,MAX_SEND=2048,MAX_CLIENTS=8,MAX_SERVERS=4\r\nOK\r\n";
    static const char duplicate[] = "+TY553XCAP:3,ATOMIC_IPD,MULTI_SERVER,TCP_ACCEPT2,CLOSE_EVENT,CLOSE_ACK,GEN32,STATE_QUERY,STATE_QUERY,UDP_SEND_ID=8,MAX_SEND=2048,MAX_CLIENTS=8,MAX_SERVERS=4\r\nOK\r\n";
    static const char v1_probe_only[] = "+TY553XCAP:1,ATOMIC_IPD,DUAL_SERVER,TCP_ACCEPT,UDP_SEND_ID=8,MAX_SEND=2048,MAX_CLIENTS=8,MAX_SERVERS=4\r\nOK\r\n";
    static const char v2_good[] = "+TY553XCAP:2,ATOMIC_IPD,DUAL_SERVER,TCP_ACCEPT2,CLOSE_EVENT,CLOSE_ACK,GEN32,STATE_QUERY,UDP_SEND_ID=8,MAX_SEND=2048,MAX_CLIENTS=8\r\nOK\r\n";
    static const char v2_with_max_servers[] = "+TY553XCAP:2,ATOMIC_IPD,DUAL_SERVER,TCP_ACCEPT2,CLOSE_EVENT,CLOSE_ACK,GEN32,STATE_QUERY,UDP_SEND_ID=8,MAX_SEND=2048,MAX_CLIENTS=8,MAX_SERVERS=4\r\nOK\r\n";
    static const char wrong_order[] = "+TY553XCAP:3,ATOMIC_IPD,MULTI_SERVER,CLOSE_EVENT,TCP_ACCEPT2,CLOSE_ACK,GEN32,STATE_QUERY,UDP_SEND_ID=8,MAX_SEND=2048,MAX_CLIENTS=8,MAX_SERVERS=4\r\nOK\r\n";
    static const char bad_id[] = "+TY553XCAP:3,ATOMIC_IPD,MULTI_SERVER,TCP_ACCEPT2,CLOSE_EVENT,CLOSE_ACK,GEN32,STATE_QUERY,UDP_SEND_ID=7,MAX_SEND=2048,MAX_CLIENTS=8,MAX_SERVERS=4\r\nOK\r\n";
    static const char dynamic[] = "+TY553XCAP:3,ATOMIC_IPD,MULTI_SERVER,TCP_ACCEPT2,CLOSE_EVENT,CLOSE_ACK,GEN32,STATE_QUERY,UDP_SEND_ID=12,MAX_SEND=512,MAX_CLIENTS=12,MAX_SERVERS=4\r\nOK\r\n";
    at_capabilities_t capabilities;
    assert(at_uart_policy_network_capabilities_v2(good, sizeof(good) - 1U, &capabilities));
    assert(capabilities.version == 3U && capabilities.udp_send_id == 8U && capabilities.max_send == 2048U &&
           capabilities.max_clients == 8U && capabilities.max_servers == 4U);
    assert(at_uart_policy_network_capabilities_v2(dynamic, sizeof(dynamic) - 1U, &capabilities));
    assert(capabilities.udp_send_id == 12U && capabilities.max_send == 512U && capabilities.max_clients == 12U &&
           capabilities.max_servers == 4U);
    assert(at_uart_policy_network_capabilities_v2(v2_good, sizeof(v2_good) - 1U, &capabilities));
    assert(capabilities.version == 2U && capabilities.udp_send_id == 8U && capabilities.max_send == 2048U &&
           capabilities.max_clients == 8U && capabilities.max_servers == 2U);
    assert(!at_uart_policy_network_capabilities_v2(missing, sizeof(missing) - 1U, &capabilities));
    assert(!at_uart_policy_network_capabilities_v2(duplicate, sizeof(duplicate) - 1U, &capabilities));
    assert(!at_uart_policy_network_capabilities_v2(v1_probe_only, sizeof(v1_probe_only) - 1U, &capabilities));
    assert(!at_uart_policy_network_capabilities_v2(v2_with_max_servers, sizeof(v2_with_max_servers) - 1U,
                                                       &capabilities));
    assert(!at_uart_policy_network_capabilities_v2(wrong_order, sizeof(wrong_order) - 1U, &capabilities));
    assert(!at_uart_policy_network_capabilities_v2(bad_id, sizeof(bad_id) - 1U, &capabilities));
    assert(!at_uart_policy_network_capabilities_v2(NULL, 0U, &capabilities));
    assert(!at_uart_policy_network_capabilities_v2(good, sizeof(good) - 1U, NULL));
}

static void replies_payloads_and_tx_are_bounded(void)
{
    char reply[32];
    size_t used = 0U;
    assert(at_uart_policy_capture_line(reply, sizeof(reply), &used, "+CIPSTATE:1,2", 13U));
    size_t before_urc = used;
    assert(at_uart_policy_capture_line(reply, sizeof(reply), &used, "WIFI GOT IP", 11U));
    assert(used == before_urc);
    assert(at_uart_policy_capture_line(reply, sizeof(reply), &used, "CLOSED", 6U));
    assert(used == before_urc);
    assert(at_uart_policy_capture_line(reply, sizeof(reply), &used,
                                          "+TY553XDBG:CIPSEND_ENTER,argc=3", 31U));
    assert(used == before_urc);
    assert(at_uart_policy_capture_line(reply, sizeof(reply), &used, "+TY553XBLE:CONN,0", 17U));
    assert(used == before_urc);
    assert(at_uart_policy_capture_line(reply, sizeof(reply), &used,
                                          "connect success. conn idx:0, interval:0x18", 44U));
    assert(used == before_urc);
    assert(at_uart_policy_capture_line(reply, sizeof(reply), &used,
                                          "disconnected. conn idx: 0, reason 0x98", 39U));
    assert(used == before_urc);
    assert(at_uart_policy_capture_line(reply, sizeof(reply), &used, "OK", 2U));
    assert(strcmp(reply, "+CIPSTATE:1,2\r\nOK\r\n") == 0);
    assert(!at_uart_policy_reply_append(reply, used + 2U, &used, "too-long", 8U));
    assert(reply[used] == '\0');
    assert(at_uart_policy_reply_fits(21U, 20U));
    assert(!at_uart_policy_reply_fits(20U, 20U));
    assert(!at_uart_policy_reply_fits(0U, 0U));
    uint8_t payload[8] = {0};
    assert(at_uart_policy_payload_valid(false, NULL, 0U, sizeof(payload)));
    assert(at_uart_policy_payload_valid(true, payload, sizeof(payload), sizeof(payload)));
    assert(!at_uart_policy_payload_valid(true, NULL, 1U, sizeof(payload)));
    assert(!at_uart_policy_payload_valid(false, payload, 1U, sizeof(payload)));
    assert(!at_uart_policy_payload_valid(true, payload, sizeof(payload) + 1U, sizeof(payload)));
    assert(at_uart_policy_tx_complete(8U, 8U, true));
    assert(!at_uart_policy_tx_complete(8U, 7U, true));
    assert(!at_uart_policy_tx_complete(8U, 8U, false));
    assert(at_uart_policy_accept_prompt(true, true, 8U, false, false));
    assert(!at_uart_policy_accept_prompt(false, true, 8U, false, false));
    assert(!at_uart_policy_accept_prompt(true, false, 8U, false, false));
    assert(!at_uart_policy_accept_prompt(true, true, 8U, true, false));
    assert(!at_uart_policy_accept_prompt(true, true, 8U, false, true));
    assert(!at_uart_policy_request_frame_hold_expired(false, false, false, true, 1000U, 500U));
    assert(!at_uart_policy_request_frame_hold_expired(true, true, false, true, 1000U, 500U));
    assert(!at_uart_policy_request_frame_hold_expired(true, false, true, true, 1000U, 500U));
    assert(!at_uart_policy_request_frame_hold_expired(true, false, false, false, 1000U, 500U));
    assert(!at_uart_policy_request_frame_hold_expired(true, false, false, true, 499U, 500U));
    assert(at_uart_policy_request_frame_hold_expired(true, false, false, true, 500U, 500U));
    assert(at_uart_policy_payload_result(8U, true, true, true, false, true));
    assert(!at_uart_policy_payload_result(8U, true, false, true, false, true));
    assert(!at_uart_policy_payload_result(8U, true, true, false, false, true));
    assert(!at_uart_policy_payload_result(8U, true, true, true, true, true));
    assert(at_uart_policy_ble_payload_uses_plain_ok("AT+BLEDADATRANSSEND=0,165", 25U));
    assert(!at_uart_policy_ble_payload_uses_plain_ok("AT+CIPSEND=0,165", 16U));
    assert(!at_uart_policy_ble_payload_uses_plain_ok("AT+BLEDADATRANSSEND", 21U));
    assert(at_uart_policy_sequence_reached(12U, 12U));
    assert(at_uart_policy_sequence_reached(14U, 12U));
    assert(!at_uart_policy_sequence_reached(11U, 12U));
    assert(at_uart_policy_sequence_reached(1U, UINT32_MAX));
}

int main(void)
{
    default_command_gate_is_narrow();
    capability_negotiation_is_exact_and_dynamic();
    replies_payloads_and_tx_are_bounded();
    puts("AT UART transport policy checks passed");
    return 0;
}
