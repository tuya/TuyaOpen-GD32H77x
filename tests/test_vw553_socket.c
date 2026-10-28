#include "vw553_socket_backend.h"
#include "tkl_network.h"
#include "FreeRTOS.h"
#include "semphr.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct { unsigned count; bool mutex; } sem_t;
static sem_t semaphores[2];
static unsigned sem_index;
static unsigned now_ms;
SemaphoreHandle_t xSemaphoreCreateMutex(void) { sem_t *s = &semaphores[sem_index++]; s->mutex = true; return s; }
SemaphoreHandle_t xSemaphoreCreateCounting(unsigned int max, unsigned int initial)
{ (void)max; sem_t *s = &semaphores[sem_index++]; s->count = initial; return s; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t timeout)
{ sem_t *s = handle; if (s->mutex) return pdTRUE; if (s->count) { --s->count; return pdTRUE; } now_ms += timeout; return pdFALSE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t handle)
{ sem_t *s = handle; if (s->mutex) return pdTRUE; ++s->count; return pdTRUE; }

static vw553_device_event_handler_t event_handler;
static char last_command[320];
static uint8_t last_payload[2048];
static size_t last_payload_len;
static unsigned send_count;
static unsigned dns_count;
static unsigned reset_count;
static unsigned dns_failures_remaining;
static uint32_t cap_version = 3;
static bool remote_active = true;
static bool reject_next_cipstart;
static unsigned reject_cipstart_state;
static bool cipstart_timeout;
static bool fail_state_query;
static bool remote_retired;
static int remote_id, remote_fd;
static uint32_t remote_gen;
static bool inject_early_data;
static bool inject_early_udp_chunks;
static bool inject_early_accept;
static bool inject_early_udp_listener_dgram;
static bool inject_early_udp2_listener_dgram;
static bool tcp_server_active;
static bool udp_server_active;
static bool udp_server2_active;
static bool listener_stop_timeout;
static bool listener_stop_already_gone;

uint32_t tkl_system_get_millisecond(void) { return now_ms; }
void tkl_system_sleep(uint32_t ms) { now_ms += ms; }

int32_t vw553_device_subscribe(vw553_device_event_handler_t cb, void *ctx)
{ (void)ctx; event_handler = cb; return OPRT_OK; }
int32_t vw553_device_negotiate(uint32_t timeout) { (void)timeout; return OPRT_OK; }
int32_t vw553_device_get_capabilities(vw553_uart_capabilities_t *caps)
{ caps->version = (uint8_t)cap_version; caps->udp_send_id = 6; caps->max_send = 2048; caps->max_clients = 6; caps->max_servers = 4; return OPRT_OK; }
int32_t vw553_device_wait_events(uint32_t watermark, uint32_t timeout) { (void)watermark; (void)timeout; return OPRT_OK; }
void vw553_device_reset_and_recover(void) { ++reset_count; }

static void reply_set(const vw553_uart_request_t *request, const char *reply)
{
    if (request->reply && request->reply_capacity) {
        size_t n = strlen(reply); if (n >= request->reply_capacity) n = request->reply_capacity - 1U;
        memcpy(request->reply, reply, n); request->reply[n] = '\0';
        if (request->reply_length) *request->reply_length = n;
    }
}

int32_t vw553_device_execute(const vw553_uart_request_t *request, vw553_at_result_t *result)
{
    size_t n = request->command_length; if (n >= sizeof(last_command)) n = sizeof(last_command) - 1U;
    memcpy(last_command, request->command, n); last_command[n] = '\0';
    last_payload_len = request->payload_length;
    if (last_payload_len) memcpy(last_payload, request->payload, last_payload_len);
    *result = VW553_AT_RESULT_OK;
    if (!strcmp(last_command, "AT+CIPMODE=0")) return OPRT_OK;
    if (!strcmp(last_command, "AT+CIPMUX=1")) return OPRT_OK;
    if (!strncmp(last_command, "AT+CIPSTART=", 12)) {
        int con_id = 0;
        (void)sscanf(last_command, "AT+CIPSTART=%d,", &con_id);
        remote_id = con_id;
        remote_fd = 42 + con_id; remote_gen = 17U + (uint32_t)con_id;
        remote_active = true; remote_retired = false;
        if (cipstart_timeout) {
            cipstart_timeout = false;
            remote_active = false;
            if (request->reply_length) *request->reply_length = 0;
            return OPRT_COM_ERROR;
        }
        if (reject_next_cipstart) {
            reject_next_cipstart = false;
            remote_active = reject_cipstart_state == 2U;
            remote_retired = reject_cipstart_state == 3U;
            reply_set(request, "ERROR\r\n");
            return OPRT_COM_ERROR;
        }
        if (inject_early_data) {
            static const uint8_t early[] = "early";
            const vw553_device_event_t event = {.kind = VW553_DEVICE_EVENT_DATA,
                .value.data = {remote_fd, early, sizeof(early) - 1U, true}};
            inject_early_data = false; event_handler(NULL, &event);
        }
        if (inject_early_udp_chunks) {
            static const uint8_t first[] = {0, '\r'};
            const vw553_device_event_t part1 = {.kind = VW553_DEVICE_EVENT_DATA,
                .value.data = {remote_fd, first, sizeof(first), false}};
            const vw553_device_event_t part2 = {.kind = VW553_DEVICE_EVENT_DATA,
                .value.data = {remote_fd, (const uint8_t *)"\nOK", 3, true}};
            const vw553_device_event_t part3 = {.kind = VW553_DEVICE_EVENT_DATA,
                .value.data = {remote_fd, (const uint8_t *)"next", 4, true}};
            inject_early_udp_chunks = false;
            event_handler(NULL, &part1); event_handler(NULL, &part2); event_handler(NULL, &part3);
        }
        return OPRT_OK;
    }
    if (!strcmp(last_command, "AT+TY553XSTATE?")) {
        if (fail_state_query) return OPRT_COM_ERROR;
        char reply[256];
        if (remote_active || remote_retired)
            snprintf(reply, sizeof(reply), "+TY553XSTATE:%d,%u,%s,TCP,CLIENT,%d,198.51.100.2,443,0,%s\r\nOK\r\n",
                     remote_id, remote_gen, remote_retired ? "RETIRED" : "ACTIVE", remote_fd,
                     remote_retired ? "REMOTE_EOF" : "NONE");
        else strcpy(reply, "OK\r\n");
        reply_set(request, reply); return OPRT_OK;
    }
    if (!strcmp(last_command, "AT+CIPSTATE?")) {
        if (fail_state_query) return OPRT_COM_ERROR;
        char reply[256];
        if (remote_active)
            snprintf(reply, sizeof(reply), "+CIPSTATE:%d,TCP,198.51.100.2,443,0,%d,0\r\nOK\r\n",
                     remote_id, remote_fd);
        else strcpy(reply, "OK\r\n");
        reply_set(request, reply); return OPRT_OK;
    }
    if (!strncmp(last_command, "AT+CIPSEND=", 11)) {
        ++send_count;
        reply_set(request, "SEND OK\r\nOK\r\n"); return OPRT_OK;
    }
    if (!strncmp(last_command, "AT+CIPCLOSE=", 12)) {
        remote_active = false;
        remote_retired = true;
        reply_set(request, "CLOSED\r\nOK\r\n"); return OPRT_OK;
    }
    if (!strcmp(last_command, "AT+CIPCLOSE")) {
        remote_active = false;
        remote_retired = true;
        reply_set(request, "CLOSED\r\nOK\r\n"); return OPRT_OK;
    }
    if (!strncmp(last_command, "AT+TY553XCLOSE=", 15)) { remote_retired = false; return OPRT_OK; }
    if (!strncmp(last_command, "AT+TY553XSERVER=TCP,", 20)) {
        tcp_server_active = !strstr(last_command, ",0");
        if (tcp_server_active && inject_early_accept) {
            static const uint8_t early[] = "listener-early";
            remote_id = 1; remote_fd = 77; remote_gen = 33;
            const vw553_device_event_t accepted = {.kind = VW553_DEVICE_EVENT_ACCEPT_V2,
                .value.accept_v2 = {1, 33, 66, 7, 77, "192.0.2.44", 4567}};
            const vw553_device_event_t data = {.kind = VW553_DEVICE_EVENT_DATA,
                .value.data = {77, early, sizeof(early) - 1U, true}};
            inject_early_accept = false; event_handler(NULL, &accepted); event_handler(NULL, &data);
        }
        return OPRT_OK;
    }
    if (!strncmp(last_command, "AT+TY553XSERVER=UDP,", 20)) {
        unsigned port = 0;
        (void)sscanf(last_command, "AT+TY553XSERVER=UDP,%u", &port);
        bool stop = strstr(last_command, ",0") != NULL;
        if (stop && listener_stop_already_gone) {
            listener_stop_already_gone = false;
            udp_server2_active = false;
            reply_set(request, "ERROR\r\n");
            return OPRT_COM_ERROR;
        }
        if (stop && listener_stop_timeout) {
            listener_stop_timeout = false;
            if (request->reply_length) *request->reply_length = 0;
            return OPRT_COM_ERROR;
        }
        if (stop) {
            udp_server_active = false;
            udp_server2_active = false;
        } else if (port == 6667U) udp_server_active = true;
        else if (port == 6668U) udp_server2_active = !stop;
        if (port == 6667U && udp_server_active && inject_early_udp_listener_dgram) {
            static const uint8_t first[] = "before-";
            static const uint8_t second[] = "query";
            const vw553_device_event_t part1 = {.kind = VW553_DEVICE_EVENT_DATAGRAM,
                .value.datagram = {55, "192.0.2.17", 17000, first, sizeof(first) - 1U, false}};
            const vw553_device_event_t part2 = {.kind = VW553_DEVICE_EVENT_DATAGRAM,
                .value.datagram = {55, "192.0.2.17", 17000, second, sizeof(second) - 1U, true}};
            inject_early_udp_listener_dgram = false; event_handler(NULL, &part1); event_handler(NULL, &part2);
        }
        if (port == 6668U && udp_server2_active && inject_early_udp2_listener_dgram) {
            static const uint8_t early[] = "before-6668";
            const vw553_device_event_t event = {.kind = VW553_DEVICE_EVENT_DATAGRAM,
                .value.datagram = {56, "192.0.2.18", 18000, early, sizeof(early) - 1U, true}};
            inject_early_udp2_listener_dgram = false; event_handler(NULL, &event);
        }
        return OPRT_OK;
    }
    if (!strcmp(last_command, "AT+TY553XSERVER?")) {
        char reply[256] = {0};
        if (tcp_server_active) strcat(reply, "+TY553XSERVER2:TCP,7001,66,7\r\n");
        if (udp_server_active) strcat(reply, "+TY553XSERVER2:UDP,6667,55,4\r\n");
        if (udp_server2_active) strcat(reply, "+TY553XSERVER2:UDP,6668,56,5\r\n");
        strcat(reply, "OK\r\n"); reply_set(request, reply);
        return OPRT_OK;
    }
    if (!strncmp(last_command, "AT+CIPDOMAIN=", 13)) {
        ++dns_count;
        if (dns_failures_remaining) {
            --dns_failures_remaining;
            reply_set(request, "ERROR\r\n");
            return OPRT_COM_ERROR;
        }
        reply_set(request, "+CIPDOMAIN:<\"203.0.113.8\">\r\nOK\r\n"); return OPRT_OK;
    }
    return OPRT_COM_ERROR;
}

int main(void)
{
    assert(vw553_socket_backend_init() == OPRT_OK);
    unsigned select_start = now_ms;
    assert(vw553_socket_select(1, NULL, NULL, NULL, 10000000U) == 0);
    assert(now_ms - select_start <= 11000U);

    int tcp = tkl_net_socket_create(PROTOCOL_TCP); assert(tcp == (int)VW553_SOCKET_FD_BASE);
    assert(tkl_net_bind(tcp, UINT32_C(0xC0A80128), 0) == UNW_SUCCESS);
    inject_early_data = true;
    assert(tkl_net_connect(tcp, UINT32_C(0xC6336402), 443) == UNW_SUCCESS);
    uint8_t received[32] = {0};
    assert(tkl_net_recv(tcp, received, sizeof(received)) == 5 && !memcmp(received, "early", 5));
    uint8_t large_ipd[2920]; memset(large_ipd, 0x5a, sizeof(large_ipd));
    const vw553_device_event_t large_event = {.kind = VW553_DEVICE_EVENT_DATA,
        .value.data = {42, large_ipd, sizeof(large_ipd), true}};
    event_handler(NULL, &large_event);
    uint8_t large_copy[2920];
    assert(tkl_net_recv(tcp, large_copy, sizeof(large_copy)) == sizeof(large_copy));
    assert(!memcmp(large_ipd, large_copy, sizeof(large_copy)));
    const char tx[] = "tls record";
    assert(tkl_net_send(tcp, tx, sizeof(tx) - 1U) == (TUYA_ERRNO)(sizeof(tx) - 1U));
    assert(last_payload_len == sizeof(tx) - 1U && !memcmp(last_payload, tx, last_payload_len));
    assert(strcmp(last_command, "AT+CIPSEND=0,10") == 0);
    uint8_t large_tx[3000]; memset(large_tx, 0xa5, sizeof(large_tx)); unsigned before_large_send = send_count;
    assert(tkl_net_send(tcp, large_tx, sizeof(large_tx)) == (TUYA_ERRNO)sizeof(large_tx));
    assert(send_count == before_large_send + 2U && last_payload_len == 952U);
    assert(tkl_net_getpeername(tcp, &(TUYA_IP_ADDR_T){0}, &(uint16_t){0}) == OPRT_OK);

    /* A second outbound socket must coexist with the first one. */
    int tcp2 = tkl_net_socket_create(PROTOCOL_TCP); assert(tcp2 == (int)VW553_SOCKET_FD_BASE + 1);
    assert(tkl_net_bind(tcp2, UINT32_C(0xC0A80128), 0) == UNW_SUCCESS);
    assert(tkl_net_connect(tcp2, UINT32_C(0xCB007108), 80) == UNW_SUCCESS);
    assert(tkl_net_send(tcp2, tx, sizeof(tx) - 1U) == (TUYA_ERRNO)(sizeof(tx) - 1U));
    assert(strcmp(last_command, "AT+CIPSEND=1,10") == 0);
    assert(tkl_net_close(tcp2) == UNW_SUCCESS);
    assert(strcmp(last_command, "AT+CIPCLOSE=1") == 0 || strcmp(last_command, "AT+CIPSTATE?") == 0);

    assert(tkl_net_close(tcp) == UNW_SUCCESS);
    assert(strcmp(last_command, "AT+CIPCLOSE=0") == 0 || strcmp(last_command, "AT+CIPSTATE?") == 0);

    int tcp_listener = tkl_net_socket_create(PROTOCOL_TCP); assert(tcp_listener == (int)VW553_SOCKET_FD_BASE);
    assert(tkl_net_bind(tcp_listener, 0, 7001) == UNW_SUCCESS);
    inject_early_accept = true;
    assert(tkl_net_listen(tcp_listener, 2) == UNW_SUCCESS);
    TUYA_IP_ADDR_T accepted_peer = 0; uint16_t accepted_port = 0;
    int accepted = tkl_net_accept(tcp_listener, &accepted_peer, &accepted_port);
    assert(accepted >= 0 && accepted_peer == UINT32_C(0xC000022C) && accepted_port == 4567);
    memset(received, 0, sizeof(received));
    assert(tkl_net_recv(accepted, received, sizeof(received)) == 14);
    assert(!memcmp(received, "listener-early", 14));
    assert(tkl_net_close(accepted) == UNW_SUCCESS);
    assert(tkl_net_close(tcp_listener) == UNW_SUCCESS);

    int udp = tkl_net_socket_create(PROTOCOL_UDP); assert(udp == (int)VW553_SOCKET_FD_BASE);
    inject_early_udp_listener_dgram = true;
    assert(tkl_net_bind(udp, 0, 6667) == UNW_SUCCESS);
    TUYA_IP_ADDR_T listener_peer = 0; uint16_t listener_peer_port = 0;
    memset(received, 0, sizeof(received));
    assert(tkl_net_recvfrom(udp, received, sizeof(received), &listener_peer, &listener_peer_port) == 12);
    assert(listener_peer == UINT32_C(0xC0000211) && listener_peer_port == 17000 && !memcmp(received, "before-query", 12));
    static const uint8_t packet[] = {0, '\r', '\n', 'O', 'K'};
    const vw553_device_event_t dg = {.kind = VW553_DEVICE_EVENT_DATAGRAM,
        .value.datagram = {-1, "192.0.2.1", 65535, packet, sizeof(packet), true}};
    event_handler(NULL, &dg);
    TUYA_IP_ADDR_T peer = 0; uint16_t peer_port = 0; memset(received, 0, sizeof(received));
    assert(tkl_net_recvfrom(udp, received, sizeof(received), &peer, &peer_port) == sizeof(packet));
    assert(peer == UINT32_C(0xC0000201) && peer_port == 65535 && !memcmp(received, packet, sizeof(packet)));
    assert(tkl_net_send_to(udp, packet, sizeof(packet), UINT32_C(0xC0000202), 7000) == sizeof(packet));
    assert(!strcmp(last_command, "AT+CIPSEND=6,5,192.0.2.2,7000") && last_payload_len == sizeof(packet));
    /* A broadcast queued while the AP vif is being torn down must be dropped
     * before it becomes AT traffic.  Unicast keeps flowing. */
    unsigned sends_before_teardown = send_count;
    vw553_socket_ap_teardown_set(true);
    assert(tkl_net_send_to(udp, packet, sizeof(packet), (TUYA_IP_ADDR_T)UINT32_C(0xFFFFFFFF), 6667) ==
           (TUYA_ERRNO)sizeof(packet));
    assert(send_count == sends_before_teardown);
    assert(tkl_net_send_to(udp, packet, sizeof(packet), UINT32_C(0xC0000202), 7000) == sizeof(packet));
    assert(send_count == sends_before_teardown + 1U);
    vw553_socket_ap_teardown_set(false);
    assert(tkl_net_send_to(udp, packet, sizeof(packet), (TUYA_IP_ADDR_T)UINT32_C(0xFFFFFFFF), 6667) ==
           (TUYA_ERRNO)sizeof(packet));
    assert(send_count == sends_before_teardown + 2U);
    vw553_socket_dns_cache_flush();
    dns_count = 0U;
    TUYA_IP_ADDR_T dns = 0; assert(tkl_net_gethostbyname("example.com", &dns) == OPRT_OK);
    assert(dns == UINT32_C(0xCB007108));
    assert(dns_count == 1U);
    /* The second lookup is served from the cache (no AT+CIPDOMAIN). */
    assert(tkl_net_gethostbyname("example.com", &dns) == OPRT_OK);
    assert(dns == UINT32_C(0xCB007108) && dns_count == 1U);
    /* TTL expiry forces a fresh query. */
    now_ms += 61000U;
    assert(tkl_net_gethostbyname("example.com", &dns) == OPRT_OK);
    assert(dns_count == 2U);
    /* Explicit release forces a fresh query. */
    vw553_socket_dns_cache_flush();
    assert(tkl_net_gethostbyname("example.com", &dns) == OPRT_OK);
    assert(dns_count == 3U);
    /* Bounded LRU: a fifth host evicts the least recently used one. */
    vw553_socket_dns_cache_flush();
    dns_count = 0U;
    assert(tkl_net_gethostbyname("a.com", &dns) == OPRT_OK);
    assert(tkl_net_gethostbyname("b.com", &dns) == OPRT_OK);
    assert(tkl_net_gethostbyname("c.com", &dns) == OPRT_OK);
    assert(tkl_net_gethostbyname("d.com", &dns) == OPRT_OK);
    assert(dns_count == 4U);
    assert(tkl_net_gethostbyname("e.com", &dns) == OPRT_OK);
    assert(dns_count == 5U);
    assert(tkl_net_gethostbyname("a.com", &dns) == OPRT_OK);
    assert(dns_count == 6U);
    /* Retry and failure paths need a cache miss. */
    vw553_socket_dns_cache_flush();
    dns_count = 0U; dns_failures_remaining = 1U; now_ms = 0U;
    assert(tkl_net_gethostbyname("example.com", &dns) == OPRT_OK);
    assert(dns == UINT32_C(0xCB007108) && dns_count == 2U && now_ms >= 150U);
    vw553_socket_dns_cache_flush();
    dns_count = 0U; dns_failures_remaining = 3U;
    assert(tkl_net_gethostbyname("example.com", &dns) == OPRT_COM_ERROR);
    assert(dns_count == 3U);
    assert(tkl_net_gethostbyname("bad\"\r\nAT+CIPMUX=0", &dns) == OPRT_INVALID_PARM);
    int udp2 = tkl_net_socket_create(PROTOCOL_UDP);
    assert(udp2 >= 0 && udp2 != udp);
    inject_early_udp2_listener_dgram = true;
    assert(tkl_net_bind(udp2, 0, 6668) == UNW_SUCCESS);
    assert(tkl_net_set_block(udp, FALSE) == OPRT_OK);
    assert(tkl_net_set_block(udp2, FALSE) == OPRT_OK);
    memset(received, 0, sizeof(received));
    assert(tkl_net_recvfrom(udp2, received, sizeof(received), &peer, &peer_port) == 11);
    assert(peer == UINT32_C(0xC0000212) && peer_port == 18000 && !memcmp(received, "before-6668", 11));
    assert(tkl_net_recvfrom(udp, received, sizeof(received), &peer, &peer_port) == UNW_EAGAIN);

    const uint8_t first_packet[] = "listener-55";
    const vw553_device_event_t first_dg = {.kind = VW553_DEVICE_EVENT_DATAGRAM,
        .value.datagram = {55, "192.0.2.21", 21000, first_packet, sizeof(first_packet) - 1U, true}};
    event_handler(NULL, &first_dg);
    memset(received, 0, sizeof(received));
    assert(tkl_net_recvfrom(udp, received, sizeof(received), &peer, &peer_port) == (TUYA_ERRNO)(sizeof(first_packet) - 1U));
    assert(peer_port == 21000 && !memcmp(received, first_packet, sizeof(first_packet) - 1U));
    assert(tkl_net_recvfrom(udp2, received, sizeof(received), &peer, &peer_port) == UNW_EAGAIN);

    const uint8_t second_packet[] = "listener-56";
    const vw553_device_event_t second_dg = {.kind = VW553_DEVICE_EVENT_DATAGRAM,
        .value.datagram = {56, "192.0.2.22", 22000, second_packet, sizeof(second_packet) - 1U, true}};
    event_handler(NULL, &second_dg);
    memset(received, 0, sizeof(received));
    assert(tkl_net_recvfrom(udp2, received, sizeof(received), &peer, &peer_port) == (TUYA_ERRNO)(sizeof(second_packet) - 1U));
    assert(peer_port == 22000 && !memcmp(received, second_packet, sizeof(second_packet) - 1U));
    assert(tkl_net_recvfrom(udp, received, sizeof(received), &peer, &peer_port) == UNW_EAGAIN);
    assert(tkl_net_send_to(udp2, packet, sizeof(packet), UINT32_C(0xC0000203), 7001) == sizeof(packet));
    assert(!strcmp(last_command, "AT+CIPSEND=6,5,192.0.2.3,7001"));
    assert(tkl_net_close(udp2) == UNW_SUCCESS);

    assert(tkl_net_set_broadcast(udp) == OPRT_OK);
    assert(tkl_net_close(udp) == UNW_SUCCESS);
    /* A listener stop that times out on the shared AT channel must release the
     * logical slot: VW553 closes AP listeners when the vif switches to STA,
     * and the daemon must not leak the slot into the following LAN start. */
    int deferred = tkl_net_socket_create(PROTOCOL_UDP);
    assert(deferred == (int)VW553_SOCKET_FD_BASE);
    assert(tkl_net_bind(deferred, 0, 6668) == UNW_SUCCESS);
    listener_stop_timeout = true;
    assert(tkl_net_close(deferred) == UNW_SUCCESS);
    assert(!listener_stop_timeout);
    int reused = tkl_net_socket_create(PROTOCOL_UDP);
    assert(reused == (int)VW553_SOCKET_FD_BASE);
    assert(tkl_net_close(reused) == UNW_SUCCESS);
    /* VW553 may already have closed the AP listener with the vif switch and
     * answer ERROR. The host must verify the module table and free the slot. */
    int gone = tkl_net_socket_create(PROTOCOL_UDP);
    assert(gone == (int)VW553_SOCKET_FD_BASE);
    assert(tkl_net_bind(gone, 0, 6668) == UNW_SUCCESS);
    listener_stop_already_gone = true;
    assert(tkl_net_close(gone) == UNW_SUCCESS);
    assert(!listener_stop_already_gone);
    int reused_after_gone = tkl_net_socket_create(PROTOCOL_UDP);
    assert(reused_after_gone == (int)VW553_SOCKET_FD_BASE);
    assert(tkl_net_close(reused_after_gone) == UNW_SUCCESS);
    /* Once the AP->STA switch closed the listeners, close must release the
     * logical slot locally and not emit another stop command. */
    int invalidated = tkl_net_socket_create(PROTOCOL_UDP);
    assert(invalidated == (int)VW553_SOCKET_FD_BASE);
    assert(tkl_net_bind(invalidated, 0, 6668) == UNW_SUCCESS);
    vw553_socket_ap_listeners_invalidate(true);
    assert(tkl_net_close(invalidated) == UNW_SUCCESS);
    assert(strcmp(last_command, "AT+TY553XSERVER=UDP,6668,0") != 0);
    int after_invalidate = tkl_net_socket_create(PROTOCOL_UDP);
    assert(after_invalidate == (int)VW553_SOCKET_FD_BASE);
    assert(tkl_net_close(after_invalidate) == UNW_SUCCESS);
    int connected_udp = tkl_net_socket_create(PROTOCOL_UDP); assert(connected_udp == (int)VW553_SOCKET_FD_BASE);
    inject_early_udp_chunks = true;
    assert(tkl_net_connect(connected_udp, UINT32_C(0xC0000209), 9000) == UNW_SUCCESS);
    memset(received, 0, sizeof(received)); peer = 0; peer_port = 0;
    assert(tkl_net_recvfrom(connected_udp, received, sizeof(received), &peer, &peer_port) == sizeof(packet));
    assert(peer == UINT32_C(0xC0000209) && peer_port == 9000 && !memcmp(received, packet, sizeof(packet)));
    memset(received, 0, sizeof(received));
    assert(tkl_net_recvfrom(connected_udp, received, sizeof(received), &peer, &peer_port) == 4);
    assert(!memcmp(received, "next", 4));
    assert(tkl_net_send(connected_udp, packet, sizeof(packet)) == sizeof(packet));
    assert(!strncmp(last_command, "AT+CIPSEND=", 11) && strstr(last_command, ",5") != NULL);
    /* While CWJAP owns the AT channel, DNS/connect/send must fail fast without
     * issuing an AT command that would time out and poison the transport. */
    vw553_socket_wifi_join_set(true);
    assert(tkl_net_send(connected_udp, packet, sizeof(packet)) == UNW_ENETDOWN);
    unsigned dns_before_join = dns_count;
    TUYA_IP_ADDR_T gated_dns = 0;
    assert(tkl_net_gethostbyname("example.com", &gated_dns) == OPRT_COM_ERROR);
    assert(dns_count == dns_before_join);
    int gated_tcp = tkl_net_socket_create(PROTOCOL_TCP);
    assert(gated_tcp >= 0);
    assert(tkl_net_connect(gated_tcp, UINT32_C(0xC0000209), 9000) == UNW_ENETDOWN);
    vw553_socket_wifi_join_set(false);
    assert(tkl_net_send(connected_udp, packet, sizeof(packet)) == sizeof(packet));
    assert(tkl_net_close(gated_tcp) == UNW_SUCCESS);
    const vw553_device_event_t udp_data = {.kind = VW553_DEVICE_EVENT_DATA,
        .value.data = {42, packet, sizeof(packet), true}};
    event_handler(NULL, &udp_data);
    memset(received, 0, sizeof(received)); peer = 0; peer_port = 0;
    assert(tkl_net_recvfrom(connected_udp, received, sizeof(received), &peer, &peer_port) == sizeof(packet));
    assert(peer == UINT32_C(0xC0000209) && peer_port == 9000 && !memcmp(received, packet, sizeof(packet)));
    assert(tkl_net_close(connected_udp) == UNW_SUCCESS);

    for (unsigned i = 0; i < 10U; ++i) {
        int failed = tkl_net_socket_create(PROTOCOL_TCP); assert(failed == (int)VW553_SOCKET_FD_BASE);
        reject_next_cipstart = true;
        reject_cipstart_state = 1U;
        assert(tkl_net_connect(failed, UINT32_C(0xC0000209), 9000) == UNW_ECONNREFUSED);
        assert(tkl_net_close(failed) == UNW_SUCCESS);
    }

    /* An unanswered CIPSTART must fail the socket, not reset VW553: a module
     * reset also drops the station link and forces a full Wi-Fi re-join. */
    int timed_out = tkl_net_socket_create(PROTOCOL_TCP); assert(timed_out == (int)VW553_SOCKET_FD_BASE);
    unsigned resets_before_timeout = reset_count;
    cipstart_timeout = true;
    assert(tkl_net_connect(timed_out, UINT32_C(0xC0000209), 9000) == UNW_ECONNRESET);
    assert(reset_count == resets_before_timeout);
    assert(tkl_net_close(timed_out) == UNW_SUCCESS);

    for (unsigned state = 2U; state <= 3U; ++state) {
        int failed = tkl_net_socket_create(PROTOCOL_TCP); assert(failed == (int)VW553_SOCKET_FD_BASE);
        reject_next_cipstart = true; reject_cipstart_state = state;
        assert(tkl_net_connect(failed, UINT32_C(0xC0000209), 9000) == UNW_ECONNREFUSED);
        /* An active orphan is closed during connect recovery; an absent one stays
         * owned by the caller until close. */
        assert(tkl_net_close(failed) == (state == 2U ? UNW_EBADF : UNW_SUCCESS));
        int available = tkl_net_socket_create(PROTOCOL_TCP); assert(available == (int)VW553_SOCKET_FD_BASE);
        assert(tkl_net_close(available) == UNW_SUCCESS);
    }

    int uncertain = tkl_net_socket_create(PROTOCOL_TCP); assert(uncertain == (int)VW553_SOCKET_FD_BASE);
    fail_state_query = true;
    assert(tkl_net_connect(uncertain, UINT32_C(0xC0000209), 9000) == UNW_ECONNRESET);
    assert(tkl_net_close(uncertain) == UNW_ECONNRESET);
    int blocked_reuse = tkl_net_socket_create(PROTOCOL_TCP); assert(blocked_reuse != uncertain);
    fail_state_query = false; remote_active = false;
    assert(tkl_net_close(uncertain) == UNW_SUCCESS);
    assert(tkl_net_close(blocked_reuse) == UNW_SUCCESS);
    puts("VW553 fake AT socket checks passed");
    return 0;
}
