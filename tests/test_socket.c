#include "at_socket.h"
#include "FreeRTOS.h"
#include "semphr.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

_Static_assert(_Generic(&at_socket_send,
    int (*)(int, const void *, uint32_t): 1, default: 0),
    "socket byte counts must use int, never an error enum");
_Static_assert(_Generic(&at_socket_sendto,
    int (*)(int, const void *, uint32_t, uint32_t, uint16_t): 1, default: 0),
    "socket byte counts must use int, never an error enum");
_Static_assert(_Generic(&at_socket_recv,
    int (*)(int, void *, uint32_t): 1, default: 0),
    "socket byte counts must use int, never an error enum");
_Static_assert(_Generic(&at_socket_recv_nd_size,
    int (*)(int, void *, uint32_t, uint32_t): 1, default: 0),
    "socket byte counts must use int, never an error enum");
_Static_assert(_Generic(&at_socket_recvfrom,
    int (*)(int, void *, uint32_t, uint32_t *, uint16_t *): 1, default: 0),
    "socket byte counts must use int, never an error enum");

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

static at_device_event_handler_t event_handler;
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
static bool tcp_server2_active;
static bool reject_tcp_options;
static at_socket_tcp_options_t tcp_options[16];
static bool udp_server_active;
static bool udp_server2_active;
static bool listener_stop_timeout;
static bool listener_stop_already_gone;
static bool cipmux_enabled = true;
static unsigned cipmux_query_count;
static unsigned cipmux_set_count;

TickType_t xTaskGetTickCount(void) { return now_ms; }
void vTaskDelay(TickType_t ticks) { now_ms += ticks; }

at_status_t at_device_subscribe(at_device_event_handler_t cb, void *ctx)
{ (void)ctx; event_handler = cb; return AT_STATUS_OK; }
at_status_t at_device_negotiate(uint32_t timeout) { (void)timeout; return AT_STATUS_OK; }
at_status_t at_device_get_capabilities(at_capabilities_t *caps)
{ caps->version = (uint8_t)cap_version; caps->udp_send_id = 6; caps->max_send = 2048; caps->max_clients = 6; caps->max_servers = 4; return AT_STATUS_OK; }
at_status_t at_device_wait_events(uint32_t watermark, uint32_t timeout) { (void)watermark; (void)timeout; return AT_STATUS_OK; }
void at_device_reset_and_recover(void) { ++reset_count; }

static void reply_set(const at_request_t *request, const char *reply)
{
    if (request->reply && request->reply_capacity) {
        size_t n = strlen(reply); if (n >= request->reply_capacity) n = request->reply_capacity - 1U;
        memcpy(request->reply, reply, n); request->reply[n] = '\0';
        if (request->reply_length) *request->reply_length = n;
    }
}

at_status_t at_device_execute(const at_request_t *request, at_result_t *result)
{
    size_t n = request->command_length; if (n >= sizeof(last_command)) n = sizeof(last_command) - 1U;
    memcpy(last_command, request->command, n); last_command[n] = '\0';
    last_payload_len = request->payload_length;
    if (last_payload_len) memcpy(last_payload, request->payload, last_payload_len);
    *result = AT_RESULT_OK;
    if (!strcmp(last_command, "AT+CIPMODE=0")) return AT_STATUS_OK;
    if (!strcmp(last_command, "AT+CIPMUX?")) {
        ++cipmux_query_count;
        reply_set(request, cipmux_enabled ? "+CIPMUX:1\r\nOK\r\n" : "+CIPMUX:0\r\nOK\r\n");
        return AT_STATUS_OK;
    }
    if (!strcmp(last_command, "AT+CIPMUX=1")) {
        ++cipmux_set_count;
        if (cipmux_enabled) {
            reply_set(request, "Please close all connections before setting multiple connections.\r\nERROR\r\n");
            return AT_STATUS_IO_ERROR;
        }
        cipmux_enabled = true;
        return AT_STATUS_OK;
    }
    if (!strncmp(last_command, "AT+CIPTCPOPT=", 13)) {
        int id; unsigned gen;
        at_socket_tcp_options_t options;
        int count = sscanf(last_command, "AT+CIPTCPOPT=%d,%u,%d,%d,%d,%d,%d", &id, &gen,
                           &options.nodelay, &options.alive, &options.idle, &options.interval, &options.count);
        assert(id >= 0 && id < 16);
        assert(gen != 0 && gen == remote_gen);
        if (reject_tcp_options) { reject_tcp_options = false; return AT_STATUS_IO_ERROR; }
        if (count == 7) tcp_options[id] = options;
        options = tcp_options[id];
        char reply[192];
        snprintf(reply, sizeof(reply), "+CIPTCPOPT:%d,%u,%d,%d,%d,%d,%d\r\nOK\r\n", id, gen,
                 options.nodelay, options.alive, options.idle, options.interval, options.count);
        reply_set(request, reply);
        return AT_STATUS_OK;
    }
    if (!strncmp(last_command, "AT+CIPSTART=", 12)) {
        int con_id = 0;
        (void)sscanf(last_command, "AT+CIPSTART=%d,", &con_id);
        remote_id = con_id;
        tcp_options[con_id] = (at_socket_tcp_options_t){1, 0, 60, 1, 3};
        remote_fd = 42 + con_id; remote_gen = 17U + (uint32_t)con_id;
        remote_active = true; remote_retired = false;
        if (cipstart_timeout) {
            cipstart_timeout = false;
            remote_active = false;
            if (request->reply_length) *request->reply_length = 0;
            return AT_STATUS_IO_ERROR;
        }
        if (reject_next_cipstart) {
            reject_next_cipstart = false;
            remote_active = reject_cipstart_state == 2U;
            remote_retired = reject_cipstart_state == 3U;
            reply_set(request, "ERROR\r\n");
            return AT_STATUS_IO_ERROR;
        }
        if (inject_early_data) {
            static const uint8_t early[] = "early";
            const at_device_event_t event = {.kind = AT_DEVICE_EVENT_DATA,
                .value.data = {remote_fd, early, sizeof(early) - 1U, true}};
            inject_early_data = false; event_handler(NULL, &event);
        }
        if (inject_early_udp_chunks) {
            static const uint8_t first[] = {0, '\r'};
            const at_device_event_t part1 = {.kind = AT_DEVICE_EVENT_DATA,
                .value.data = {remote_fd, first, sizeof(first), false}};
            const at_device_event_t part2 = {.kind = AT_DEVICE_EVENT_DATA,
                .value.data = {remote_fd, (const uint8_t *)"\nOK", 3, true}};
            const at_device_event_t part3 = {.kind = AT_DEVICE_EVENT_DATA,
                .value.data = {remote_fd, (const uint8_t *)"next", 4, true}};
            inject_early_udp_chunks = false;
            event_handler(NULL, &part1); event_handler(NULL, &part2); event_handler(NULL, &part3);
        }
        return AT_STATUS_OK;
    }
    if (!strcmp(last_command, "AT+TY553XSTATE?")) {
        if (fail_state_query) return AT_STATUS_IO_ERROR;
        char reply[256];
        if (remote_active || remote_retired)
            snprintf(reply, sizeof(reply), "+TY553XSTATE:%d,%u,%s,TCP,CLIENT,%d,198.51.100.2,443,0,%s\r\nOK\r\n",
                     remote_id, remote_gen, remote_retired ? "RETIRED" : "ACTIVE", remote_fd,
                     remote_retired ? "REMOTE_EOF" : "NONE");
        else strcpy(reply, "OK\r\n");
        reply_set(request, reply); return AT_STATUS_OK;
    }
    if (!strcmp(last_command, "AT+CIPSTATE?")) {
        if (fail_state_query) return AT_STATUS_IO_ERROR;
        char reply[256];
        if (remote_active)
            snprintf(reply, sizeof(reply), "+CIPSTATE:%d,TCP,198.51.100.2,443,0,%d,0,%u\r\nOK\r\n",
                     remote_id, remote_fd, remote_gen);
        else strcpy(reply, "OK\r\n");
        reply_set(request, reply); return AT_STATUS_OK;
    }
    if (!strncmp(last_command, "AT+CIPSEND=", 11)) {
        ++send_count;
        reply_set(request, "SEND OK\r\nOK\r\n"); return AT_STATUS_OK;
    }
    if (!strncmp(last_command, "AT+CIPCLOSE=", 12)) {
        remote_active = false;
        remote_retired = true;
        reply_set(request, "CLOSED\r\nOK\r\n"); return AT_STATUS_OK;
    }
    if (!strcmp(last_command, "AT+CIPCLOSE")) {
        remote_active = false;
        remote_retired = true;
        reply_set(request, "CLOSED\r\nOK\r\n"); return AT_STATUS_OK;
    }
    if (!strncmp(last_command, "AT+TY553XCLOSE=", 15)) { remote_retired = false; return AT_STATUS_OK; }
    if (!strncmp(last_command, "AT+TY553XSERVER=TCP,", 20)) {
        unsigned tcp_port = 0;
        (void)sscanf(last_command, "AT+TY553XSERVER=TCP,%u", &tcp_port);
        if (tcp_port == 5055U) {
            tcp_server2_active = !strstr(last_command, ",0");
            return AT_STATUS_OK;
        }
        tcp_server_active = !strstr(last_command, ",0");
        if (tcp_server_active && inject_early_accept) {
            static const uint8_t early[] = "listener-early";
            remote_id = 1; remote_fd = 77; remote_gen = 33;
            const at_device_event_t accepted = {.kind = AT_DEVICE_EVENT_ACCEPT_V2,
                .value.accept_v2 = {1, 33, 66, 7, 77, "192.0.2.44", 4567}};
            const at_device_event_t data = {.kind = AT_DEVICE_EVENT_DATA,
                .value.data = {77, early, sizeof(early) - 1U, true}};
            inject_early_accept = false; event_handler(NULL, &accepted); event_handler(NULL, &data);
        }
        return AT_STATUS_OK;
    }
    if (!strncmp(last_command, "AT+TY553XSERVER=UDP,", 20)) {
        unsigned port = 0;
        (void)sscanf(last_command, "AT+TY553XSERVER=UDP,%u", &port);
        bool stop = strstr(last_command, ",0") != NULL;
        if (stop && listener_stop_already_gone) {
            listener_stop_already_gone = false;
            udp_server2_active = false;
            reply_set(request, "ERROR\r\n");
            return AT_STATUS_IO_ERROR;
        }
        if (stop && listener_stop_timeout) {
            listener_stop_timeout = false;
            if (request->reply_length) *request->reply_length = 0;
            return AT_STATUS_IO_ERROR;
        }
        if (stop) {
            udp_server_active = false;
            udp_server2_active = false;
        } else if (port == 6667U) udp_server_active = true;
        else if (port == 6668U) udp_server2_active = !stop;
        if (port == 6667U && udp_server_active && inject_early_udp_listener_dgram) {
            static const uint8_t first[] = "before-";
            static const uint8_t second[] = "query";
            const at_device_event_t part1 = {.kind = AT_DEVICE_EVENT_DATAGRAM,
                .value.datagram = {55, "192.0.2.17", 17000, first, sizeof(first) - 1U, false}};
            const at_device_event_t part2 = {.kind = AT_DEVICE_EVENT_DATAGRAM,
                .value.datagram = {55, "192.0.2.17", 17000, second, sizeof(second) - 1U, true}};
            inject_early_udp_listener_dgram = false; event_handler(NULL, &part1); event_handler(NULL, &part2);
        }
        if (port == 6668U && udp_server2_active && inject_early_udp2_listener_dgram) {
            static const uint8_t early[] = "before-6668";
            const at_device_event_t event = {.kind = AT_DEVICE_EVENT_DATAGRAM,
                .value.datagram = {56, "192.0.2.18", 18000, early, sizeof(early) - 1U, true}};
            inject_early_udp2_listener_dgram = false; event_handler(NULL, &event);
        }
        return AT_STATUS_OK;
    }
    if (!strcmp(last_command, "AT+TY553XSERVER?")) {
        char reply[256] = {0};
        if (tcp_server_active) strcat(reply, "+TY553XSERVER2:TCP,7001,66,7\r\n");
        if (tcp_server2_active) strcat(reply, "+TY553XSERVER2:TCP,5055,67,8\r\n");
        if (udp_server_active) strcat(reply, "+TY553XSERVER2:UDP,6667,55,4\r\n");
        if (udp_server2_active) strcat(reply, "+TY553XSERVER2:UDP,6668,56,5\r\n");
        strcat(reply, "OK\r\n"); reply_set(request, reply);
        return AT_STATUS_OK;
    }
    if (!strncmp(last_command, "AT+CIPDOMAIN=", 13)) {
        ++dns_count;
        if (dns_failures_remaining) {
            --dns_failures_remaining;
            reply_set(request, "ERROR\r\n");
            return AT_STATUS_IO_ERROR;
        }
        reply_set(request, "+CIPDOMAIN:<\"203.0.113.8\">\r\nOK\r\n"); return AT_STATUS_OK;
    }
    return AT_STATUS_IO_ERROR;
}

int main(void)
{
    assert(at_socket_init() == AT_STATUS_OK);
    assert(cipmux_query_count == 1U);
    assert(cipmux_set_count == 0U);
    assert(strcmp(last_command, "AT+CIPMUX?") == 0);
    unsigned select_start = now_ms;
    assert(at_socket_select(1, NULL, NULL, NULL, 10000000U) == 0);
    assert(now_ms - select_start <= 11000U);

    int tcp = at_socket_create(AT_SOCKET_TYPE_STREAM); assert(tcp == (int)AT_SOCKET_FD_BASE);
    assert(at_socket_disable_nagle(-1) == AT_STATUS_INVALID_PARAM);
    assert(at_socket_disable_nagle(AT_SOCKET_FD_LIMIT + 1) == AT_STATUS_INVALID_PARAM);
    assert(at_socket_disable_nagle(tcp) == AT_STATUS_OK);
    assert(at_socket_set_keepalive(tcp, true, 100, 5, 1) == AT_STATUS_OK);
    assert(at_socket_set_keepalive(tcp, true, 0, 5, 1) == AT_STATUS_INVALID_PARAM);
    assert(at_socket_set_keepalive(tcp, true, 100, 0, 1) == AT_STATUS_INVALID_PARAM);
    assert(at_socket_set_keepalive(tcp, true, 100, 5, 0) == AT_STATUS_INVALID_PARAM);
    assert(at_socket_bind(tcp, UINT32_C(0xC0A80128), 0) == AT_SOCKET_ERRNO_NONE);
    inject_early_data = true;
    assert(at_socket_connect(tcp, UINT32_C(0xC6336402), 443) == AT_SOCKET_ERRNO_NONE);
    assert(at_socket_disable_nagle(tcp) == AT_STATUS_OK);
    at_socket_tcp_options_t actual;
    assert(at_socket_get_tcp_options(tcp, &actual) == AT_STATUS_OK);
    assert(actual.nodelay == 1 && actual.alive == 1 && actual.idle == 100 && actual.interval == 5 && actual.count == 1);
    assert(at_socket_set_keepalive(tcp, false, 0, 0, 0) == AT_STATUS_OK);
    assert(at_socket_get_tcp_options(tcp, &actual) == AT_STATUS_OK && actual.alive == 0);
    reject_tcp_options = true;
    assert(at_socket_set_keepalive(tcp, true, 100, 5, 1) != AT_STATUS_OK);
    assert(at_socket_get_tcp_options(tcp, &actual) == AT_STATUS_OK && actual.alive == 0);
    assert(at_socket_set_keepalive(tcp, true, 101, 7, 2) == AT_STATUS_OK);
    assert(at_socket_get_tcp_options(tcp, &actual) == AT_STATUS_OK);
    assert(actual.alive == 1 && actual.idle == 101 && actual.interval == 7 && actual.count == 2);
    uint8_t received[32] = {0};
    assert(at_socket_recv(tcp, received, sizeof(received)) == 5 && !memcmp(received, "early", 5));
    assert(at_socket_recv(tcp, NULL, 0U) == 0);
    uint8_t large_ipd[2920]; memset(large_ipd, 0x5a, sizeof(large_ipd));
    const at_device_event_t large_event = {.kind = AT_DEVICE_EVENT_DATA,
        .value.data = {42, large_ipd, sizeof(large_ipd), true}};
    event_handler(NULL, &large_event);
    uint8_t large_copy[2920];
    assert(at_socket_recv(tcp, large_copy, sizeof(large_copy)) == sizeof(large_copy));
    assert(!memcmp(large_ipd, large_copy, sizeof(large_copy)));
    const char tx[] = "tls record";
    assert(at_socket_send(tcp, tx, sizeof(tx) - 1U) == (int)(sizeof(tx) - 1U));
    assert(last_payload_len == sizeof(tx) - 1U && !memcmp(last_payload, tx, last_payload_len));
    assert(strcmp(last_command, "AT+CIPSEND=0,10") == 0);
    /* ARM may pack this enum into one signed byte. All positive byte counts,
     * especially the TLS ClientHello boundary at 129, must remain positive. */
    uint8_t boundary_tx[129];
    memset(boundary_tx, 0x3c, sizeof(boundary_tx));
    for (size_t length = 127U; length <= 129U; ++length) {
        unsigned before_send = send_count;
        assert(at_socket_send(tcp, boundary_tx, (uint32_t)length) == (int)length);
        assert(send_count == before_send + 1U);
        assert(last_payload_len == length && !memcmp(last_payload, boundary_tx, length));
    }
    uint8_t large_tx[3000]; memset(large_tx, 0xa5, sizeof(large_tx)); unsigned before_large_send = send_count;
    assert(at_socket_send(tcp, large_tx, sizeof(large_tx)) == (int)sizeof(large_tx));
    assert(send_count == before_large_send + 2U && last_payload_len == 952U);
    assert(at_socket_getpeername(tcp, &(uint32_t){0}, &(uint16_t){0}) == AT_STATUS_OK);

    /* A second outbound socket must coexist with the first one. */
    int tcp2 = at_socket_create(AT_SOCKET_TYPE_STREAM); assert(tcp2 == (int)AT_SOCKET_FD_BASE + 1);
    assert(at_socket_bind(tcp2, UINT32_C(0xC0A80128), 0) == AT_SOCKET_ERRNO_NONE);
    assert(at_socket_connect(tcp2, UINT32_C(0xCB007108), 80) == AT_SOCKET_ERRNO_NONE);
    assert(at_socket_send(tcp2, tx, sizeof(tx) - 1U) == (int)(sizeof(tx) - 1U));
    assert(strcmp(last_command, "AT+CIPSEND=1,10") == 0);
    assert(at_socket_close(tcp2) == AT_SOCKET_ERRNO_NONE);
    assert(strcmp(last_command, "AT+CIPCLOSE=1") == 0 || strcmp(last_command, "AT+CIPSTATE?") == 0);

    assert(at_socket_close(tcp) == AT_SOCKET_ERRNO_NONE);
    assert(at_socket_disable_nagle(tcp) == AT_STATUS_INVALID_PARAM);
    assert(strcmp(last_command, "AT+CIPCLOSE=0") == 0 || strcmp(last_command, "AT+CIPSTATE?") == 0);

    int tcp_listener = at_socket_create(AT_SOCKET_TYPE_STREAM); assert(tcp_listener == (int)AT_SOCKET_FD_BASE);
    assert(at_socket_bind(tcp_listener, 0, 7001) == AT_SOCKET_ERRNO_NONE);
    inject_early_accept = true;
    assert(at_socket_listen(tcp_listener, 2) == AT_SOCKET_ERRNO_NONE);
    int monitor = at_socket_create(AT_SOCKET_TYPE_STREAM);
    assert(at_socket_bind(monitor, 0, 5055) == AT_SOCKET_ERRNO_NONE);
    assert(at_socket_listen(monitor, 5) == AT_SOCKET_ERRNO_NONE);
    assert(at_socket_close(monitor) == AT_SOCKET_ERRNO_NONE);
    assert(tcp_server_active && !tcp_server2_active);
    uint32_t accepted_peer = 0; uint16_t accepted_port = 0;
    int accepted = at_socket_accept(tcp_listener, &accepted_peer, &accepted_port);
    assert(accepted >= 0 && accepted_peer == UINT32_C(0xC000022C) && accepted_port == 4567);
    memset(received, 0, sizeof(received));
    assert(at_socket_recv(accepted, received, sizeof(received)) == 14);
    assert(!memcmp(received, "listener-early", 14));
    assert(at_socket_close(accepted) == AT_SOCKET_ERRNO_NONE);
    assert(at_socket_close(tcp_listener) == AT_SOCKET_ERRNO_NONE);

    int udp = at_socket_create(AT_SOCKET_TYPE_DATAGRAM); assert(udp == (int)AT_SOCKET_FD_BASE);
    assert(at_socket_disable_nagle(udp) == AT_STATUS_INVALID_PARAM);
    inject_early_udp_listener_dgram = true;
    assert(at_socket_bind(udp, 0, 6667) == AT_SOCKET_ERRNO_NONE);
    uint32_t listener_peer = 0; uint16_t listener_peer_port = 0;
    memset(received, 0, sizeof(received));
    assert(at_socket_recvfrom(udp, received, sizeof(received), &listener_peer, &listener_peer_port) == 12);
    assert(listener_peer == UINT32_C(0xC0000211) && listener_peer_port == 17000 && !memcmp(received, "before-query", 12));
    static const uint8_t packet[] = {0, '\r', '\n', 'O', 'K'};
    const at_device_event_t dg = {.kind = AT_DEVICE_EVENT_DATAGRAM,
        .value.datagram = {-1, "192.0.2.1", 65535, packet, sizeof(packet), true}};
    event_handler(NULL, &dg);
    uint32_t peer = 0; uint16_t peer_port = 0; memset(received, 0, sizeof(received));
    assert(at_socket_recvfrom(udp, received, sizeof(received), &peer, &peer_port) == sizeof(packet));
    assert(peer == UINT32_C(0xC0000201) && peer_port == 65535 && !memcmp(received, packet, sizeof(packet)));
    assert(at_socket_sendto(udp, packet, sizeof(packet), UINT32_C(0xC0000202), 7000) == sizeof(packet));
    assert(!strcmp(last_command, "AT+CIPSEND=6,5,192.0.2.2,7000") && last_payload_len == sizeof(packet));
    /* A broadcast queued while the AP vif is being torn down must be dropped
     * before it becomes AT traffic.  Unicast keeps flowing. */
    unsigned sends_before_teardown = send_count;
    at_socket_ap_teardown_set(true);
    assert(at_socket_sendto(udp, packet, sizeof(packet), (uint32_t)UINT32_C(0xFFFFFFFF), 6667) ==
           (int)sizeof(packet));
    assert(send_count == sends_before_teardown);
    assert(at_socket_sendto(udp, packet, sizeof(packet), UINT32_C(0xC0000202), 7000) == sizeof(packet));
    assert(send_count == sends_before_teardown + 1U);
    at_socket_ap_teardown_set(false);
    assert(at_socket_sendto(udp, packet, sizeof(packet), (uint32_t)UINT32_C(0xFFFFFFFF), 6667) ==
           (int)sizeof(packet));
    assert(send_count == sends_before_teardown + 2U);
    at_socket_dns_cache_flush();
    dns_count = 0U;
    uint32_t dns = 0; assert(at_socket_gethostbyname("example.com", &dns) == AT_STATUS_OK);
    assert(dns == UINT32_C(0xCB007108));
    assert(dns_count == 1U);
    /* The second lookup is served from the cache (no AT+CIPDOMAIN). */
    assert(at_socket_gethostbyname("example.com", &dns) == AT_STATUS_OK);
    assert(dns == UINT32_C(0xCB007108) && dns_count == 1U);
    /* TTL expiry forces a fresh query. */
    now_ms += 61000U;
    assert(at_socket_gethostbyname("example.com", &dns) == AT_STATUS_OK);
    assert(dns_count == 2U);
    /* Explicit release forces a fresh query. */
    at_socket_dns_cache_flush();
    assert(at_socket_gethostbyname("example.com", &dns) == AT_STATUS_OK);
    assert(dns_count == 3U);
    /* Bounded LRU: a fifth host evicts the least recently used one. */
    at_socket_dns_cache_flush();
    dns_count = 0U;
    assert(at_socket_gethostbyname("a.com", &dns) == AT_STATUS_OK);
    assert(at_socket_gethostbyname("b.com", &dns) == AT_STATUS_OK);
    assert(at_socket_gethostbyname("c.com", &dns) == AT_STATUS_OK);
    assert(at_socket_gethostbyname("d.com", &dns) == AT_STATUS_OK);
    assert(dns_count == 4U);
    assert(at_socket_gethostbyname("e.com", &dns) == AT_STATUS_OK);
    assert(dns_count == 5U);
    assert(at_socket_gethostbyname("a.com", &dns) == AT_STATUS_OK);
    assert(dns_count == 6U);
    /* Retry and failure paths need a cache miss. */
    at_socket_dns_cache_flush();
    dns_count = 0U; dns_failures_remaining = 1U; now_ms = 0U;
    assert(at_socket_gethostbyname("example.com", &dns) == AT_STATUS_OK);
    assert(dns == UINT32_C(0xCB007108) && dns_count == 2U && now_ms >= 150U);
    at_socket_dns_cache_flush();
    dns_count = 0U; dns_failures_remaining = 3U;
    assert(at_socket_gethostbyname("example.com", &dns) == AT_STATUS_IO_ERROR);
    assert(dns_count == 3U);
    assert(at_socket_gethostbyname("bad\"\r\nAT+CIPMUX=0", &dns) == AT_STATUS_INVALID_PARAM);
    int udp2 = at_socket_create(AT_SOCKET_TYPE_DATAGRAM);
    assert(udp2 >= 0 && udp2 != udp);
    inject_early_udp2_listener_dgram = true;
    assert(at_socket_bind(udp2, 0, 6668) == AT_SOCKET_ERRNO_NONE);
    assert(at_socket_set_block(udp, false) == AT_STATUS_OK);
    assert(at_socket_set_block(udp2, false) == AT_STATUS_OK);
    memset(received, 0, sizeof(received));
    assert(at_socket_recvfrom(udp2, received, sizeof(received), &peer, &peer_port) == 11);
    assert(peer == UINT32_C(0xC0000212) && peer_port == 18000 && !memcmp(received, "before-6668", 11));
    assert(at_socket_recvfrom(udp, received, sizeof(received), &peer, &peer_port) == AT_SOCKET_ERRNO_AGAIN);

    const uint8_t first_packet[] = "listener-55";
    const at_device_event_t first_dg = {.kind = AT_DEVICE_EVENT_DATAGRAM,
        .value.datagram = {55, "192.0.2.21", 21000, first_packet, sizeof(first_packet) - 1U, true}};
    event_handler(NULL, &first_dg);
    memset(received, 0, sizeof(received));
    assert(at_socket_recvfrom(udp, received, sizeof(received), &peer, &peer_port) == (int)(sizeof(first_packet) - 1U));
    assert(peer_port == 21000 && !memcmp(received, first_packet, sizeof(first_packet) - 1U));
    assert(at_socket_recvfrom(udp2, received, sizeof(received), &peer, &peer_port) == AT_SOCKET_ERRNO_AGAIN);

    const uint8_t second_packet[] = "listener-56";
    const at_device_event_t second_dg = {.kind = AT_DEVICE_EVENT_DATAGRAM,
        .value.datagram = {56, "192.0.2.22", 22000, second_packet, sizeof(second_packet) - 1U, true}};
    event_handler(NULL, &second_dg);
    memset(received, 0, sizeof(received));
    assert(at_socket_recvfrom(udp2, received, sizeof(received), &peer, &peer_port) == (int)(sizeof(second_packet) - 1U));
    assert(peer_port == 22000 && !memcmp(received, second_packet, sizeof(second_packet) - 1U));
    assert(at_socket_recvfrom(udp, received, sizeof(received), &peer, &peer_port) == AT_SOCKET_ERRNO_AGAIN);
    assert(at_socket_sendto(udp2, packet, sizeof(packet), UINT32_C(0xC0000203), 7001) == sizeof(packet));
    assert(!strcmp(last_command, "AT+CIPSEND=6,5,192.0.2.3,7001"));
    assert(at_socket_close(udp2) == AT_SOCKET_ERRNO_NONE);

    assert(at_socket_set_broadcast(udp) == AT_STATUS_OK);
    assert(at_socket_close(udp) == AT_SOCKET_ERRNO_NONE);
    /* A listener stop that times out on the shared AT channel must release the
     * logical slot: AT closes AP listeners when the vif switches to STA,
     * and the daemon must not leak the slot into the following LAN start. */
    int deferred = at_socket_create(AT_SOCKET_TYPE_DATAGRAM);
    assert(deferred == (int)AT_SOCKET_FD_BASE);
    assert(at_socket_bind(deferred, 0, 6668) == AT_SOCKET_ERRNO_NONE);
    listener_stop_timeout = true;
    assert(at_socket_close(deferred) == AT_SOCKET_ERRNO_NONE);
    assert(!listener_stop_timeout);
    int reused = at_socket_create(AT_SOCKET_TYPE_DATAGRAM);
    assert(reused == (int)AT_SOCKET_FD_BASE);
    assert(at_socket_close(reused) == AT_SOCKET_ERRNO_NONE);
    /* AT may already have closed the AP listener with the vif switch and
     * answer ERROR. The host must verify the module table and free the slot. */
    int gone = at_socket_create(AT_SOCKET_TYPE_DATAGRAM);
    assert(gone == (int)AT_SOCKET_FD_BASE);
    assert(at_socket_bind(gone, 0, 6668) == AT_SOCKET_ERRNO_NONE);
    listener_stop_already_gone = true;
    assert(at_socket_close(gone) == AT_SOCKET_ERRNO_NONE);
    assert(!listener_stop_already_gone);
    int reused_after_gone = at_socket_create(AT_SOCKET_TYPE_DATAGRAM);
    assert(reused_after_gone == (int)AT_SOCKET_FD_BASE);
    assert(at_socket_close(reused_after_gone) == AT_SOCKET_ERRNO_NONE);
    /* Once the AP->STA switch closed the listeners, close must release the
     * logical slot locally and not emit another stop command. */
    int invalidated = at_socket_create(AT_SOCKET_TYPE_DATAGRAM);
    assert(invalidated == (int)AT_SOCKET_FD_BASE);
    assert(at_socket_bind(invalidated, 0, 6668) == AT_SOCKET_ERRNO_NONE);
    at_socket_ap_listeners_invalidate(true);
    assert(at_socket_close(invalidated) == AT_SOCKET_ERRNO_NONE);
    assert(strcmp(last_command, "AT+TY553XSERVER=UDP,6668,0") != 0);
    int after_invalidate = at_socket_create(AT_SOCKET_TYPE_DATAGRAM);
    assert(after_invalidate == (int)AT_SOCKET_FD_BASE);
    assert(at_socket_close(after_invalidate) == AT_SOCKET_ERRNO_NONE);
    int connected_udp = at_socket_create(AT_SOCKET_TYPE_DATAGRAM); assert(connected_udp == (int)AT_SOCKET_FD_BASE);
    inject_early_udp_chunks = true;
    assert(at_socket_connect(connected_udp, UINT32_C(0xC0000209), 9000) == AT_SOCKET_ERRNO_NONE);
    memset(received, 0, sizeof(received)); peer = 0; peer_port = 0;
    assert(at_socket_recvfrom(connected_udp, received, sizeof(received), &peer, &peer_port) == sizeof(packet));
    assert(peer == UINT32_C(0xC0000209) && peer_port == 9000 && !memcmp(received, packet, sizeof(packet)));
    memset(received, 0, sizeof(received));
    assert(at_socket_recvfrom(connected_udp, received, sizeof(received), &peer, &peer_port) == 4);
    assert(!memcmp(received, "next", 4));
    assert(at_socket_send(connected_udp, packet, sizeof(packet)) == sizeof(packet));
    assert(!strncmp(last_command, "AT+CIPSEND=", 11) && strstr(last_command, ",5") != NULL);
    /* While CWJAP owns the AT channel, DNS/connect/send must fail fast without
     * issuing an AT command that would time out and poison the transport. */
    at_socket_wifi_join_set(true);
    assert(at_socket_send(connected_udp, packet, sizeof(packet)) == AT_SOCKET_ERRNO_NETWORK_DOWN);
    unsigned dns_before_join = dns_count;
    uint32_t gated_dns = 0;
    assert(at_socket_gethostbyname("example.com", &gated_dns) == AT_STATUS_IO_ERROR);
    assert(dns_count == dns_before_join);
    int gated_tcp = at_socket_create(AT_SOCKET_TYPE_STREAM);
    assert(gated_tcp >= 0);
    assert(at_socket_connect(gated_tcp, UINT32_C(0xC0000209), 9000) == AT_SOCKET_ERRNO_NETWORK_DOWN);
    at_socket_wifi_join_set(false);
    assert(at_socket_send(connected_udp, packet, sizeof(packet)) == sizeof(packet));
    assert(at_socket_close(gated_tcp) == AT_SOCKET_ERRNO_NONE);
    const at_device_event_t udp_data = {.kind = AT_DEVICE_EVENT_DATA,
        .value.data = {42, packet, sizeof(packet), true}};
    event_handler(NULL, &udp_data);
    memset(received, 0, sizeof(received)); peer = 0; peer_port = 0;
    assert(at_socket_recvfrom(connected_udp, received, sizeof(received), &peer, &peer_port) == sizeof(packet));
    assert(peer == UINT32_C(0xC0000209) && peer_port == 9000 && !memcmp(received, packet, sizeof(packet)));
    assert(at_socket_close(connected_udp) == AT_SOCKET_ERRNO_NONE);

    for (unsigned i = 0; i < 10U; ++i) {
        int failed = at_socket_create(AT_SOCKET_TYPE_STREAM); assert(failed == (int)AT_SOCKET_FD_BASE);
        reject_next_cipstart = true;
        reject_cipstart_state = 1U;
        assert(at_socket_connect(failed, UINT32_C(0xC0000209), 9000) == AT_SOCKET_ERRNO_CONNECTION_REFUSED);
        assert(at_socket_close(failed) == AT_SOCKET_ERRNO_NONE);
    }

    /* An unanswered CIPSTART must fail the socket, not reset AT: a module
     * reset also drops the station link and forces a full Wi-Fi re-join. */
    int timed_out = at_socket_create(AT_SOCKET_TYPE_STREAM); assert(timed_out == (int)AT_SOCKET_FD_BASE);
    unsigned resets_before_timeout = reset_count;
    cipstart_timeout = true;
    assert(at_socket_connect(timed_out, UINT32_C(0xC0000209), 9000) == AT_SOCKET_ERRNO_CONNECTION_RESET);
    assert(reset_count == resets_before_timeout);
    assert(at_socket_close(timed_out) == AT_SOCKET_ERRNO_NONE);

    for (unsigned state = 2U; state <= 3U; ++state) {
        int failed = at_socket_create(AT_SOCKET_TYPE_STREAM); assert(failed == (int)AT_SOCKET_FD_BASE);
        reject_next_cipstart = true; reject_cipstart_state = state;
        assert(at_socket_connect(failed, UINT32_C(0xC0000209), 9000) == AT_SOCKET_ERRNO_CONNECTION_REFUSED);
        /* An active orphan is closed during connect recovery; an absent one stays
         * owned by the caller until close. */
        assert(at_socket_close(failed) == (state == 2U ? AT_SOCKET_ERRNO_BAD_FD : AT_SOCKET_ERRNO_NONE));
        int available = at_socket_create(AT_SOCKET_TYPE_STREAM); assert(available == (int)AT_SOCKET_FD_BASE);
        assert(at_socket_close(available) == AT_SOCKET_ERRNO_NONE);
    }

    int uncertain = at_socket_create(AT_SOCKET_TYPE_STREAM); assert(uncertain == (int)AT_SOCKET_FD_BASE);
    fail_state_query = true;
    assert(at_socket_connect(uncertain, UINT32_C(0xC0000209), 9000) == AT_SOCKET_ERRNO_CONNECTION_RESET);
    assert(at_socket_close(uncertain) == AT_SOCKET_ERRNO_CONNECTION_RESET);
    int blocked_reuse = at_socket_create(AT_SOCKET_TYPE_STREAM); assert(blocked_reuse != uncertain);
    fail_state_query = false; remote_active = false;
    assert(at_socket_close(uncertain) == AT_SOCKET_ERRNO_NONE);
    assert(at_socket_close(blocked_reuse) == AT_SOCKET_ERRNO_NONE);
    puts("AT fake AT socket checks passed");
    return 0;
}
