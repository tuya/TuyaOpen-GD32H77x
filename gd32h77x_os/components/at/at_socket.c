/**
 * @file at_socket.c
 * @brief Bounded generic socket semantics over the AT client.
 *
 * The table maps logical descriptors, AT connection IDs, and wire descriptors
 * independently. No backend state lock is held while an AT RPC is running.
 */
#include "at_socket.h"
#include "at_log.h"
#include "at_commands.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define SOCKET_FD_BASE AT_SOCKET_FD_BASE
#define SOCKET_COUNT AT_SOCKET_FD_LIMIT
#define RX_RING_SIZE 8192U
#define RX_DYNAMIC_LIMIT 12288U
#define STREAM_RX_FETCH_SIZE 2048U
#define STREAM_TX_CHUNK_SIZE 1024U
#define STREAM_TX_GAP_MS 20U
#define SEND_FAIL_RETRY_COUNT 4U
#define UDP_QUEUE_DEPTH 2U
#define ACCEPT_QUEUE_DEPTH 4U
#define UDP_MAX_DATAGRAM 2920U
#define PENDING_RAW_COUNT 4U
#define PENDING_FINAL_COUNT 16U
#define PENDING_UDP_COUNT 4U

static uint32_t at_socket_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void at_socket_sleep(uint32_t milliseconds)
{
    vTaskDelay(pdMS_TO_TICKS(milliseconds));
}
#define RPC_TIMEOUT_MS 10000U
#define DNS_TIMEOUT_MS 20000U
#define DNS_RETRY_TIMEOUT_MS 5000U
#define DNS_RETRY_COUNT 3U
#define DNS_RETRY_DELAY_MS 150U
#define CIPMUX_SETTLE_MS 300U
#define DNS_CACHE_SIZE 4U
#define DNS_CACHE_HOST_SIZE 96U
#define DNS_CACHE_TTL_MS 60000U

typedef enum { SLOT_FREE, SLOT_CREATED, SLOT_CONNECTING, SLOT_CONNECTED, SLOT_CLOSING, SLOT_LISTENER,
               SLOT_STARTING_LISTENER, SLOT_NO_REMOTE, SLOT_QUARANTINE, SLOT_FAILED, SLOT_EOF } slot_state_t;

typedef struct rx_packet {
    struct rx_packet *next;
    uint32_t addr;
    uint16_t port;
    uint16_t length;
    uint16_t offset;
    bool final;
} rx_packet_t;

typedef struct {
    rx_packet_t *head;
    rx_packet_t *tail;
    size_t size;
    size_t build_length;
    rx_packet_t *build_head;
    rx_packet_t *build_prev;
    uint8_t completed_datagrams;
    bool build_dropped;
} socket_rx_queue_t;

typedef struct {
    slot_state_t state;
    at_socket_type_t type;
    uint32_t generation;
    int remote_id;
    uint32_t remote_generation;
    int lwip_fd;
    uint32_t listener_generation;
    uint16_t local_port;
    uint32_t peer_addr;
    uint16_t peer_port;
    bool nonblock;
    bool broadcast;
    bool ap_listener_invalid;
    bool tcp_options_configured;
    at_socket_tcp_options_t tcp_options;
    int recv_timeout;
    int send_timeout;
    uint32_t rx_events;
    uint32_t rx_bytes;
    uint32_t rx_ready_events;
    uint32_t rx_fetches;
    bool rx_ready;
    bool rx_fetching;
    socket_rx_queue_t rx;
    int accepted[ACCEPT_QUEUE_DEPTH];
    uint32_t accepted_addr[ACCEPT_QUEUE_DEPTH];
    uint16_t accepted_port[ACCEPT_QUEUE_DEPTH];
    uint8_t accepted_head;
    uint8_t accepted_count;
} socket_slot_t;

typedef char socket_slot_size_check[(sizeof(socket_slot_t) <= 1024U) ? 1 : -1];

typedef struct {
    slot_state_t state;
    at_socket_type_t type;
    uint32_t generation;
    int remote_id;
    uint32_t remote_generation;
    int lwip_fd;
    uint32_t listener_generation;
    uint16_t local_port;
    uint32_t peer_addr;
    uint16_t peer_port;
    bool nonblock;
    bool broadcast;
    bool ap_listener_invalid;
    bool tcp_options_configured;
    at_socket_tcp_options_t tcp_options;
    int recv_timeout;
    int send_timeout;
    size_t stream_size;
    uint32_t rx_events;
    uint32_t rx_bytes;
} socket_meta_t;

typedef struct {
    bool used;
    int raw_fd;
    uint8_t datagram_count;
    socket_rx_queue_t rx;
} pending_raw_t;
typedef struct {
    bool used;
    uint32_t con_id, generation, listener_generation;
    int listener_fd, lwip_fd;
    uint16_t peer_port;
    char peer_ipv4[16];
} pending_accept_t;
typedef struct {
    bool used;
    bool complete;
    int listener_fd;
    uint32_t peer_addr;
    uint16_t peer_port;
    socket_rx_queue_t rx;
} pending_datagram_t;

typedef struct {
    bool used;
    char host[DNS_CACHE_HOST_SIZE];
    uint32_t addr;
    uint32_t expires_ms;
    uint32_t last_used_ms;
} dns_cache_entry_t;

static socket_slot_t s_slots[SOCKET_COUNT];
static size_t s_rx_dynamic_bytes;
static pending_raw_t s_pending_raw[PENDING_RAW_COUNT];
static pending_accept_t s_pending_accepts[ACCEPT_QUEUE_DEPTH];
static pending_datagram_t s_pending_datagrams[PENDING_UDP_COUNT];
static SemaphoreHandle_t s_state_mutex;
static SemaphoreHandle_t s_ready;
static bool s_initialized;
static bool s_initializing;
static bool s_subscribed;
static volatile uint8_t s_sync_init_lock;
static at_socket_errno_t s_last_errno;
static uint16_t s_udp_send_id;
static uint16_t s_max_send;
static uint16_t s_max_clients;
static uint16_t s_max_servers;
static volatile bool s_ap_teardown;
static volatile bool s_wifi_join_active;
static dns_cache_entry_t s_dns_cache[DNS_CACHE_SIZE];

static bool state_lock(void);
static void state_unlock(void);
static at_status_t ensure_multi_connection_mode(uint16_t max_clients);

at_socket_errno_t at_socket_get_errno(void) { return s_last_errno; }

void at_socket_ap_teardown_set(bool active) { s_ap_teardown = active; }
bool at_socket_ap_teardown_active(void) { return s_ap_teardown; }
void at_socket_wifi_join_set(bool active) { s_wifi_join_active = active; }
bool at_socket_wifi_join_active(void) { return s_wifi_join_active; }

bool at_socket_has_active_socket(void)
{
    bool active = false;
    if (!state_lock()) return false;
    for (unsigned i = SOCKET_FD_BASE; i < SOCKET_COUNT; ++i) {
        if (s_slots[i].state == SLOT_CONNECTING || s_slots[i].state == SLOT_CONNECTED ||
            s_slots[i].state == SLOT_CLOSING) {
            active = true;
            break;
        }
    }
    state_unlock();
    return active;
}

static bool state_lock(void)
{
    return s_state_mutex && xSemaphoreTake(s_state_mutex, portMAX_DELAY) == pdTRUE;
}
static void state_unlock(void) { (void)xSemaphoreGive(s_state_mutex); }
static void wake_waiters(void) { if (s_ready) (void)xSemaphoreGive(s_ready); }

static void rx_packet_free_locked(rx_packet_t *packet)
{
    if (!packet) return;
    size_t allocation = sizeof(*packet) + packet->length;
    s_rx_dynamic_bytes = allocation <= s_rx_dynamic_bytes ? s_rx_dynamic_bytes - allocation : 0U;
    vPortFree(packet);
}

static rx_packet_t *rx_packet_alloc_locked(const uint8_t *data, size_t length,
                                            uint32_t addr, uint16_t port, bool final)
{
    if (length > UINT16_MAX || (!data && length)) return NULL;
    size_t allocation = sizeof(rx_packet_t) + length;
    if (s_rx_dynamic_bytes > RX_DYNAMIC_LIMIT) return NULL;
    if (allocation > RX_DYNAMIC_LIMIT - s_rx_dynamic_bytes) return NULL;
    rx_packet_t *packet = pvPortMalloc(allocation);
    if (!packet) return NULL;
    memset(packet, 0, sizeof(*packet));
    packet->addr = addr;
    packet->port = port;
    packet->length = (uint16_t)length;
    packet->final = final;
    if (length) memcpy(packet + 1, data, length);
    s_rx_dynamic_bytes += allocation;
    return packet;
}

static void rx_queue_clear_locked(socket_rx_queue_t *queue)
{
    rx_packet_t *packet = queue->head;
    while (packet) {
        rx_packet_t *next = packet->next;
        rx_packet_free_locked(packet);
        packet = next;
    }
    memset(queue, 0, sizeof(*queue));
}

static void rx_queue_append_packet_locked(socket_rx_queue_t *queue, rx_packet_t *packet)
{
    if (!packet) return;
    if (queue->tail) queue->tail->next = packet;
    else queue->head = packet;
    queue->tail = packet;
    queue->size += packet->length;
}

static bool rx_queue_append_locked(socket_rx_queue_t *queue, const uint8_t *data, size_t length,
                                   uint32_t addr, uint16_t port, bool final)
{
    rx_packet_t *packet = rx_packet_alloc_locked(data, length, addr, port, final);
    if (!packet) return false;
    rx_queue_append_packet_locked(queue, packet);
    return true;
}

static void rx_queue_splice_locked(socket_rx_queue_t *dst, socket_rx_queue_t *src)
{
    if (!src->head) return;
    if (dst->tail) dst->tail->next = src->head;
    else dst->head = src->head;
    dst->tail = src->tail;
    dst->size += src->size;
    src->head = NULL;
    src->tail = NULL;
    src->size = 0U;
}

static size_t rx_queue_read_locked(socket_rx_queue_t *queue, void *buffer, size_t length)
{
    size_t copied = 0U;
    uint8_t *dst = buffer;
    while (copied < length && queue->head) {
        rx_packet_t *packet = queue->head;
        size_t available = packet->length - packet->offset;
        size_t take = available < length - copied ? available : length - copied;
        if (take) {
            memcpy(dst + copied, (const uint8_t *)(packet + 1) + packet->offset, take);
            packet->offset += (uint16_t)take;
            queue->size -= take;
            copied += take;
        }
        if (packet->offset == packet->length) {
            queue->head = packet->next;
            if (!queue->head) queue->tail = NULL;
            rx_packet_free_locked(packet);
        }
    }
    return copied;
}

static void rx_queue_discard_locked(socket_rx_queue_t *queue, size_t length)
{
    while (length && queue->head) {
        rx_packet_t *packet = queue->head;
        size_t available = packet->length - packet->offset;
        size_t take = available < length ? available : length;
        packet->offset += (uint16_t)take;
        queue->size -= take;
        length -= take;
        if (packet->offset == packet->length) {
            queue->head = packet->next;
            if (!queue->head) queue->tail = NULL;
            rx_packet_free_locked(packet);
        }
    }
}

static void rx_datagram_drop_build_locked(socket_rx_queue_t *queue)
{
    if (!queue->build_head) return;
    rx_packet_t *packet = queue->build_head;
    if (queue->build_prev) queue->build_prev->next = NULL;
    else queue->head = NULL;
    queue->tail = queue->build_prev;
    while (packet) {
        rx_packet_t *next = packet->next;
        queue->size -= packet->length - packet->offset;
        rx_packet_free_locked(packet);
        packet = next;
    }
    queue->build_head = NULL;
    queue->build_prev = NULL;
    queue->build_length = 0U;
}

static bool rx_datagram_append_locked(socket_rx_queue_t *queue, const uint8_t *data, size_t length,
                                      uint32_t addr, uint16_t port, bool final)
{
    if (queue->build_dropped) {
        if (final) queue->build_dropped = false;
        return true;
    }
    if (length > UDP_MAX_DATAGRAM || length > UDP_MAX_DATAGRAM - queue->build_length) {
        rx_datagram_drop_build_locked(queue);
        queue->build_dropped = !final;
        return true;
    }
    rx_packet_t *packet = rx_packet_alloc_locked(data, length, addr, port, final);
    if (!packet) {
        rx_datagram_drop_build_locked(queue);
        queue->build_dropped = !final;
        return false;
    }
    if (!queue->build_head) {
        queue->build_head = packet;
        queue->build_prev = queue->tail;
    }
    queue->build_length += length;
    rx_queue_append_packet_locked(queue, packet);
    if (final) {
        ++queue->completed_datagrams;
        queue->build_head = NULL;
        queue->build_prev = NULL;
        queue->build_length = 0U;
    }
    return true;
}

static size_t rx_datagram_length_locked(const socket_rx_queue_t *queue)
{
    size_t total = 0U;
    for (const rx_packet_t *packet = queue->head; packet; packet = packet->next) {
        total += packet->length - packet->offset;
        if (packet->final) return total;
    }
    return 0U;
}

static size_t rx_datagram_read_locked(socket_rx_queue_t *queue, void *buffer, size_t length,
                                      uint32_t *addr, uint16_t *port)
{
    if (!queue->completed_datagrams || !queue->head) return 0U;
    size_t total = rx_datagram_length_locked(queue);
    if (addr) *addr = queue->head->addr;
    if (port) *port = queue->head->port;
    size_t take = total < length ? total : length;
    if (take) (void)rx_queue_read_locked(queue, buffer, take);
    rx_queue_discard_locked(queue, total - take);
    if (!take) {
        while (queue->head) {
            rx_packet_t *packet = queue->head;
            bool final = packet->final;
            queue->head = packet->next;
            if (!queue->head) queue->tail = NULL;
            rx_packet_free_locked(packet);
            if (final) break;
        }
    }
    --queue->completed_datagrams;
    return take;
}

static void socket_slot_clear_locked(socket_slot_t *slot)
{
    rx_queue_clear_locked(&slot->rx);
    memset(slot, 0, sizeof(*slot));
}

static void pending_raw_clear_locked(pending_raw_t *pending)
{
    rx_queue_clear_locked(&pending->rx);
    memset(pending, 0, sizeof(*pending));
}

static void pending_datagram_clear_locked(pending_datagram_t *pending)
{
    rx_queue_clear_locked(&pending->rx);
    memset(pending, 0, sizeof(*pending));
}

static at_socket_errno_t fail_with(int error)
{
    s_last_errno = error;
    return (at_socket_errno_t)error;
}

void at_socket_ap_listeners_invalidate(bool invalid)
{
    unsigned count = 0U;
    if (!state_lock()) return;
    for (unsigned i = 0U; i < SOCKET_COUNT; ++i) {
        socket_slot_t *slot = &s_slots[i];
        if ((slot->state == SLOT_LISTENER || slot->state == SLOT_STARTING_LISTENER) &&
            (slot->type == AT_SOCKET_TYPE_STREAM || slot->type == AT_SOCKET_TYPE_DATAGRAM)) {
            slot->ap_listener_invalid = invalid;
            if (invalid) ++count;
        }
    }
    state_unlock();
    if (invalid && count) AT_SUMMARY("AP vif switch invalidated %u listener(s)", count);
}

static int32_t run_at(const char *command, const void *payload, size_t payload_length, bool prompt,
                      char *reply, size_t reply_capacity, size_t *reply_length, uint32_t *watermark)
{
    at_request_t request = {0};
    at_result_t result = AT_RESULT_NONE;
    request.command = command;
    request.command_length = strlen(command);
    request.payload = payload;
    request.payload_length = payload_length;
    request.wait_prompt = prompt;
    request.reply = reply;
    request.reply_capacity = reply_capacity;
    request.reply_length = reply_length;
    request.event_watermark = watermark;
    request.timeout_ms = RPC_TIMEOUT_MS;
    AT_LOG("AT TX %s", command);
    int32_t status = at_device_execute(&request, &result);
    size_t received = reply_length ? *reply_length : 0U;
    if (reply && received) {
        AT_LOG("AT RX status=%ld result=%d len=%lu reply=%.*s", (long)status, (int)result,
                  (unsigned long)received, (int)received, reply);
    } else {
        AT_LOG("AT RX status=%ld result=%d len=%lu", (long)status, (int)result, (unsigned long)received);
    }
    if (status != AT_STATUS_OK || result != AT_RESULT_OK) {
#ifndef GD32H77X_AT_SOCKET_TEST
        if (at_session_is_poisoned()) AT_SUMMARY("transport is poisoned");
#endif
        AT_SUMMARY("AT failed cmd=%.*s status=%ld result=%d reply=%.*s", (int)request.command_length,
                      command, (long)status, (int)result, (int)received, reply ? reply : "");
    }
    if (status != AT_STATUS_OK) return status;
    return result == AT_RESULT_OK ? AT_STATUS_OK : AT_STATUS_IO_ERROR;
}

static int32_t run_at_with_timeout(const char *command, const void *payload, size_t payload_length, bool prompt,
                                   char *reply, size_t reply_capacity, size_t *reply_length,
                                   uint32_t *watermark, uint32_t timeout_ms)
{
    at_request_t request = {0};
    at_result_t result = AT_RESULT_NONE;
    request.command = command;
    request.command_length = strlen(command);
    request.payload = payload;
    request.payload_length = payload_length;
    request.wait_prompt = prompt;
    request.reply = reply;
    request.reply_capacity = reply_capacity;
    request.reply_length = reply_length;
    request.event_watermark = watermark;
    request.timeout_ms = timeout_ms;
    AT_LOG("AT TX %s timeout=%lums", command, (unsigned long)timeout_ms);
    int32_t status = at_device_execute(&request, &result);
    size_t received = reply_length ? *reply_length : 0U;
    if (reply && received) {
        AT_LOG("AT RX status=%ld result=%d len=%lu reply=%.*s", (long)status, (int)result,
                  (unsigned long)received, (int)received, reply);
    } else {
        AT_LOG("AT RX status=%ld result=%d len=%lu", (long)status, (int)result, (unsigned long)received);
    }
    if (status != AT_STATUS_OK || result != AT_RESULT_OK) {
#ifndef GD32H77X_AT_SOCKET_TEST
        if (at_session_is_poisoned()) AT_SUMMARY("transport is poisoned");
#endif
        AT_SUMMARY("AT failed cmd=%.*s status=%ld result=%d reply=%.*s", (int)request.command_length,
                      command, (long)status, (int)result, (int)received, reply ? reply : "");
    }
    if (status != AT_STATUS_OK) return status;
    return result == AT_RESULT_OK ? AT_STATUS_OK : AT_STATUS_IO_ERROR;
}

static void addr_to_text(uint32_t addr, char text[16])
{
    (void)snprintf(text, 16, "%u.%u.%u.%u", (unsigned)((addr >> 24) & 255U),
                   (unsigned)((addr >> 16) & 255U), (unsigned)((addr >> 8) & 255U), (unsigned)(addr & 255U));
}

static void log_rx_preview(int fd, const uint8_t *data, size_t length)
{
    (void)fd;
    char preview[161];
    size_t take = length < sizeof(preview) - 1U ? length : sizeof(preview) - 1U;
    for (size_t i = 0U; i < take; ++i) {
        preview[i] = data[i] >= 0x20U && data[i] <= 0x7eU ? (char)data[i] : '.';
    }
    preview[take] = '\0';
    AT_LOG("SOCKET RX fd=%d len=%lu preview=%s", fd, (unsigned long)length, preview);
#ifndef GD32H77X_AT_SOCKET_TEST
    if (take >= 5U && memcmp(data, "+IPD,", 5U) == 0) {
        /* An AT frame header inside socket payload means the parser lost frame
         * sync; dump the raw bytes that produced it. */
        at_session_dump_rx_trace("RX LEAK");
    }
#endif
}

static bool parse_ipv4(const char *text, uint32_t *address)
{
    unsigned a, b, c, d;
    char tail;
    if (!text || !address || sscanf(text, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 ||
        a > 255U || b > 255U || c > 255U || d > 255U) return false;
    *address = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
    return true;
}

static bool parse_rx_ready(const char *line, size_t length, int *remote_fd)
{
    static const char prefix[] = AT_WIRE_RX_READY_PREFIX;
    const size_t prefix_length = sizeof(prefix) - 1U;
    char value[24];
    char *end = NULL;
    if (!line || !remote_fd || length <= prefix_length ||
        memcmp(line, prefix, prefix_length) != 0 ||
        length - prefix_length >= sizeof(value)) {
        return false;
    }
    size_t value_length = length - prefix_length;
    memcpy(value, line + prefix_length, value_length);
    value[value_length] = '\0';
    long parsed = strtol(value, &end, 10);
    if (!end || *end != '\0' || parsed < 0 || parsed > INT_MAX) {
        return false;
    }
    *remote_fd = (int)parsed;
    return true;
}

static int find_logical(int fd)
{
    return fd >= (int)SOCKET_FD_BASE && (unsigned)fd < SOCKET_COUNT && s_slots[fd].state != SLOT_FREE ? fd : -1;
}

static int reserve_slot(at_socket_type_t type)
{
    if (!state_lock()) return -1;
    for (unsigned i = SOCKET_FD_BASE; i < SOCKET_COUNT; ++i) {
        if (s_slots[i].state == SLOT_FREE) {
            uint32_t generation = s_slots[i].generation + 1U;
            socket_slot_clear_locked(&s_slots[i]);
            s_slots[i].generation = generation;
            s_slots[i].state = SLOT_CREATED;
            s_slots[i].type = type;
            s_slots[i].tcp_options = (at_socket_tcp_options_t){1, 0, 60, 1, 3};
            s_slots[i].remote_id = -1;
            s_slots[i].lwip_fd = -1;
            s_slots[i].recv_timeout = -1;
            s_slots[i].send_timeout = (int)RPC_TIMEOUT_MS;
            state_unlock();
            AT_LOG("socket create fd=%u type=%d", i, (int)type);
            return (int)i;
        }
    }
    state_unlock();
    return -1;
}

at_status_t at_socket_init(void)
{
    uint32_t wait_start = at_socket_now_ms();
    while (__atomic_test_and_set(&s_sync_init_lock, __ATOMIC_ACQUIRE)) {
        if ((uint32_t)(at_socket_now_ms() - wait_start) >= RPC_TIMEOUT_MS)
            return AT_STATUS_NOT_READY;
        at_socket_sleep(1U);
    }
    if (!s_state_mutex || !s_ready) {
        if (!s_state_mutex) s_state_mutex = xSemaphoreCreateMutex();
        if (!s_ready) s_ready = xSemaphoreCreateCounting(16U, 0U);
    }
    bool sync_ready = s_state_mutex && s_ready;
    __atomic_clear(&s_sync_init_lock, __ATOMIC_RELEASE);
    if (!sync_ready) return AT_STATUS_NO_MEMORY;
    /* the network manager starts immediately after link-up and kicks off this init from
     * its own thread. The MQTT DNS lookup then landed here while
     * s_initializing was still true and got AT_STATUS_NOT_READY, which is
     * why the first MQTT connect failed on every boot. Wait for the in-flight
     * init instead of failing a concurrent caller. */
    uint32_t init_wait_start = at_socket_now_ms();
    for (;;) {
        if (!state_lock()) return AT_STATUS_IO_ERROR;
        if (s_initialized) { state_unlock(); return AT_STATUS_OK; }
        if (!s_initializing) { s_initializing = true; state_unlock(); break; }
        state_unlock();
        if ((uint32_t)(at_socket_now_ms() - init_wait_start) >= RPC_TIMEOUT_MS) {
            return AT_STATUS_NOT_READY;
        }
        at_socket_sleep(5U);
    }

    at_status_t status = AT_STATUS_OK;
    if (!s_subscribed) {
        status = at_device_subscribe(at_socket_on_device_event, NULL);
        if (status == AT_STATUS_OK && state_lock()) { s_subscribed = true; state_unlock(); }
    }
    if (status == AT_STATUS_OK) status = at_device_negotiate(RPC_TIMEOUT_MS);
    if (status != AT_STATUS_OK) AT_LOG("CAP negotiation failed status=%ld", (long)status);
    at_capabilities_t caps = {0};
    if (status == AT_STATUS_OK) status = at_device_get_capabilities(&caps);
    if (status != AT_STATUS_OK) AT_LOG("capability read failed status=%ld", (long)status);
    /* The stock AT AT fallback supports outbound client sockets but no
     * extended listener service, so max_servers is legitimately zero. */
    if (status == AT_STATUS_OK && (caps.version > 3U || caps.max_clients == 0U || caps.max_send == 0U ||
                              caps.max_send > AT_SESSION_MAX_PAYLOAD))
        status = AT_STATUS_IO_ERROR;
    if (status == AT_STATUS_OK) {
        s_udp_send_id = caps.udp_send_id;
        s_max_send = caps.max_send;
        s_max_clients = caps.max_clients < SOCKET_COUNT ? caps.max_clients : SOCKET_COUNT;
        s_max_servers = caps.max_servers < SOCKET_COUNT ? caps.max_servers : SOCKET_COUNT;
        /* CIPMUX=1 is the stock multi-client wire mode used by the validated
         * vendor image. The AT survives an H779 reset, so query first and
         * preserve an already-enabled mode instead of reconfiguring it while
         * legacy client slots are still owned by the module. */
        status = ensure_multi_connection_mode(caps.max_clients);
        if (status != AT_STATUS_OK) AT_LOG("CIPMUX setup failed status=%ld", (long)status);
    }
    if (state_lock()) {
        s_initializing = false;
        s_initialized = status == AT_STATUS_OK;
        state_unlock();
    }
    if (status == AT_STATUS_OK) at_socket_dns_cache_flush();
    return status;
}

static at_status_t ensure_multi_connection_mode(uint16_t max_clients)
{
    char reply[96] = {0};
    size_t reply_length = 0U;
    at_status_t status = run_at("AT+CIPMUX?", NULL, 0, false, reply, sizeof(reply), &reply_length, NULL);
    if (status != AT_STATUS_OK) {
        AT_LOG("CIPMUX query failed status=%ld", (long)status);
        return status;
    }

    char *line = strstr(reply, "+CIPMUX:");
    int mode = -1;
    if (line == NULL || sscanf(line, "+CIPMUX:%d", &mode) != 1 || (mode != 0 && mode != 1)) {
        AT_LOG("CIPMUX query reply invalid len=%lu reply=%.*s",
                  (unsigned long)reply_length, (int)reply_length, reply);
        return AT_STATUS_IO_ERROR;
    }
    if (mode == 1) {
        AT_SUMMARY("CIPMUX already enabled; keeping existing client slots");
        return AT_STATUS_OK;
    }

    status = run_at("AT+CIPMODE=0", NULL, 0, false, NULL, 0, NULL, NULL);
    if (status != AT_STATUS_OK) return status;
    status = run_at("AT+CIPMUX=1", NULL, 0, false, NULL, 0, NULL, NULL);
    if (status == AT_STATUS_OK) {
        at_socket_sleep(CIPMUX_SETTLE_MS);
        return AT_STATUS_OK;
    }

    /* CIPMUX=0 is only valid for a single client. A previous host instance can
     * leave that client owned by the module after the H779 resets. Close the
     * client slot set once, then switch to multi-client mode; this is a defined
     * recovery transition, not a retry of a failed request. */
    char close_all[32];
    (void)snprintf(close_all, sizeof(close_all), "AT+CIPCLOSE=%u", (unsigned)max_clients);
    if (run_at(close_all, NULL, 0, false, NULL, 0, NULL, NULL) != AT_STATUS_OK) {
        return AT_STATUS_IO_ERROR;
    }
    at_socket_sleep(CIPMUX_SETTLE_MS);
    status = run_at("AT+CIPMUX=1", NULL, 0, false, NULL, 0, NULL, NULL);
    if (status == AT_STATUS_OK) at_socket_sleep(CIPMUX_SETTLE_MS);
    return status;
}

typedef enum { REMOTE_STATE_UNKNOWN = -2, REMOTE_STATE_ABSENT = -1,
               REMOTE_STATE_ACTIVE, REMOTE_STATE_RETIRED } remote_state_t;

typedef struct { remote_state_t state; int raw_fd; uint32_t generation; } remote_snapshot_t;

static remote_snapshot_t query_remote(int remote_id)
{
    remote_snapshot_t snapshot = {.state = REMOTE_STATE_UNKNOWN, .raw_fd = -1, .generation = 0U};
    char reply[1024] = {0};
    size_t length = 0;
    int32_t status = run_at("AT+CIPSTATE?", NULL, 0, false, reply, sizeof(reply), &length, NULL);
    AT_LOG("CIPSTATE remote=%d status=%ld len=%lu reply=%.*s", remote_id, (long)status,
              (unsigned long)length, (int)length, reply);
    if (status != AT_STATUS_OK) return snapshot;
    if (length < 4U || length >= sizeof(reply) - 1U || memcmp(reply + length - 4U, "OK\r\n", 4U) != 0)
        return snapshot;
    const char *line = reply;
    while ((line = strstr(line, "+CIPSTATE:")) != NULL) {
        int id = -1, remote_port = -1, local_port = -1, raw = -1, role = -1;
        unsigned generation = 0;
        char type[4] = {0}, ip[16] = {0};
        if (sscanf(line, "+CIPSTATE:%d,%3[^,],%15[^,],%d,%d,%d,%d,%u", &id, type, ip, &remote_port,
                   &local_port, &raw, &role, &generation) >= 7 && id == remote_id && raw >= 0 &&
            (strcmp(type, "TCP") == 0 || strcmp(type, "UDP") == 0)) {
            snapshot.state = REMOTE_STATE_ACTIVE;
            snapshot.raw_fd = raw;
            snapshot.generation = generation;
            return snapshot;
        }
        line += sizeof("+CIPSTATE:") - 1U;
    }
    snapshot.state = REMOTE_STATE_ABSENT;
    return snapshot;
}

static int query_remote_fd(int remote_id, uint32_t *generation)
{
    remote_snapshot_t snapshot = query_remote(remote_id);
    if (snapshot.state == REMOTE_STATE_UNKNOWN) return -2;
    if (snapshot.state != REMOTE_STATE_ACTIVE) return -1;
    if (generation) *generation = snapshot.generation;
    return snapshot.raw_fd;
}

static int allocate_remote_id_locked(void)
{
    for (uint16_t candidate = 0U; candidate < s_max_clients; ++candidate) {
        bool used = false;
        for (unsigned i = 0U; i < SOCKET_COUNT; ++i) {
            if (s_slots[i].state != SLOT_FREE && s_slots[i].remote_id == (int)candidate) {
                used = true;
                break;
            }
        }
        if (!used) return (int)candidate;
    }
    return -1;
}

static int get_slot(int fd, socket_meta_t *copy)
{
    if (!state_lock()) return -1;
    int idx = find_logical(fd);
    if (idx >= 0 && copy) {
        const socket_slot_t *slot = &s_slots[idx];
        *copy = (socket_meta_t){.state = slot->state, .type = slot->type, .generation = slot->generation,
                                .remote_id = slot->remote_id, .remote_generation = slot->remote_generation,
                                .lwip_fd = slot->lwip_fd, .listener_generation = slot->listener_generation,
                                .local_port = slot->local_port, .peer_addr = slot->peer_addr,
                                .peer_port = slot->peer_port, .nonblock = slot->nonblock,
                                .broadcast = slot->broadcast,
                                .ap_listener_invalid = slot->ap_listener_invalid,
                                .tcp_options_configured = slot->tcp_options_configured,
                                .tcp_options = slot->tcp_options,
                                .recv_timeout = slot->recv_timeout,
                                .send_timeout = slot->send_timeout,
                                .stream_size = slot->type == AT_SOCKET_TYPE_STREAM ? slot->rx.size : 0U,
                                .rx_events = slot->rx_events, .rx_bytes = slot->rx_bytes};
    }
    state_unlock();
    return idx;
}

static pending_raw_t *pending_raw_find_or_alloc_locked(int raw_fd)
{
    pending_raw_t *free_entry = NULL;
    for (unsigned i = 0; i < PENDING_RAW_COUNT; ++i) {
        if (s_pending_raw[i].used && s_pending_raw[i].raw_fd == raw_fd) return &s_pending_raw[i];
        if (!s_pending_raw[i].used && !free_entry) free_entry = &s_pending_raw[i];
    }
    if (free_entry) {
        memset(free_entry, 0, sizeof(*free_entry));
        free_entry->used = true;
        free_entry->raw_fd = raw_fd;
    }
    return free_entry;
}

static bool pending_raw_append_locked(int raw_fd, const uint8_t *bytes, size_t length, bool datagram_final)
{
    if (length > RX_RING_SIZE || (!bytes && length)) return false;
    pending_raw_t *entry = pending_raw_find_or_alloc_locked(raw_fd);
    if (!entry || length > RX_RING_SIZE - entry->rx.size ||
        (datagram_final && entry->datagram_count >= PENDING_FINAL_COUNT)) return false;
    if (!rx_queue_append_locked(&entry->rx, bytes, length, 0U, 0U, datagram_final)) {
        pending_raw_clear_locked(entry);
        return false;
    }
    if (datagram_final) ++entry->datagram_count;
    return true;
}

static bool udp_consume_locked(socket_slot_t *slot, const uint8_t *data, size_t length,
                               uint32_t addr, uint16_t port, bool final)
{
    /* UDP is lossy by contract. The old code failed the whole slot with
     * ENOBUFS when the 2-entry queue overflowed, which made the network manager tear down
     * its UDP service socket on every discovery burst. Drop the offending
     * datagram (or the oldest queued one) and keep the socket alive. */
    if (!rx_datagram_append_locked(&slot->rx, data, length, addr, port, final)) return false;
    while (slot->rx.completed_datagrams > UDP_QUEUE_DEPTH) {
        AT_LOG("UDP queue full, dropped oldest datagram");
        (void)rx_datagram_read_locked(&slot->rx, NULL, 0U, NULL, NULL);
    }
    return true;
}

static bool pending_datagram_add_locked(int listener_fd, uint32_t peer_addr, uint16_t peer_port,
                                        const uint8_t *data, size_t length, bool final)
{
    pending_datagram_t *free_entry = NULL;
    pending_datagram_t *entry = NULL;
    for (unsigned i = 0; i < PENDING_UDP_COUNT; ++i) {
        pending_datagram_t *candidate = &s_pending_datagrams[i];
        if (candidate->used && !candidate->complete && candidate->listener_fd == listener_fd &&
            candidate->peer_addr == peer_addr && candidate->peer_port == peer_port) {
            entry = candidate;
            break;
        }
        if (!candidate->used && !free_entry) free_entry = candidate;
    }
    if (!entry) {
        entry = free_entry;
        if (!entry) return false;
        memset(entry, 0, sizeof(*entry));
        entry->used = true;
        entry->listener_fd = listener_fd;
        entry->peer_addr = peer_addr;
        entry->peer_port = peer_port;
    }
    if (!rx_datagram_append_locked(&entry->rx, data, length, peer_addr, peer_port, final)) {
        pending_datagram_clear_locked(entry);
        return false;
    }
    entry->complete = final;
    return true;
}

static void replay_pending_datagrams_locked(socket_slot_t *slot, int listener_fd)
{
    for (unsigned i = 0; i < PENDING_UDP_COUNT; ++i) {
        pending_datagram_t *pending = &s_pending_datagrams[i];
        if (!pending->used || (pending->listener_fd != listener_fd && pending->listener_fd >= 0)) continue;
        if (slot->rx.build_length) {
            slot->state = SLOT_FAILED; s_last_errno = AT_SOCKET_ERRNO_NO_BUFFER;
        } else {
            if (!pending->complete) {
                slot->rx.build_head = pending->rx.head;
                slot->rx.build_prev = slot->rx.tail;
                slot->rx.build_length = pending->rx.size;
            }
            rx_queue_splice_locked(&slot->rx, &pending->rx);
            if (pending->complete) ++slot->rx.completed_datagrams;
            while (slot->rx.completed_datagrams > UDP_QUEUE_DEPTH)
                (void)rx_datagram_read_locked(&slot->rx, NULL, 0U, NULL, NULL);
        }
        pending_datagram_clear_locked(pending);
    }
}

static void discard_pending_datagrams_locked(int listener_fd)
{
    for (unsigned i = 0; i < PENDING_UDP_COUNT; ++i) {
        pending_datagram_t *pending = &s_pending_datagrams[i];
        if (pending->used && (pending->listener_fd == listener_fd || pending->listener_fd < 0)) {
            pending_datagram_clear_locked(pending);
        }
    }
}

static at_socket_errno_t release_listener_slot(int idx, const socket_meta_t *slot)
{
    if (!state_lock()) return fail_with(AT_SOCKET_ERRNO_BUSY);
    if (s_slots[idx].generation == slot->generation) {
        discard_pending_datagrams_locked(slot->lwip_fd);
        uint32_t gen = slot->generation + 1U;
        socket_slot_clear_locked(&s_slots[idx]);
        s_slots[idx].generation = gen;
    }
    state_unlock();
    return AT_SOCKET_ERRNO_NONE;
}

static bool accept_v2_locked(const pending_accept_t *pending)
{
    int listener_idx = -1;
    for (unsigned i = 0; i < SOCKET_COUNT; ++i) {
        socket_slot_t *listener = &s_slots[i];
        if ((listener->state == SLOT_LISTENER || listener->state == SLOT_STARTING_LISTENER) &&
            listener->type == AT_SOCKET_TYPE_STREAM && listener->lwip_fd == pending->listener_fd &&
            listener->listener_generation == pending->listener_generation) { listener_idx = (int)i; break; }
    }
    if (listener_idx < 0) return false;
    socket_slot_t *listener = &s_slots[listener_idx];
    if (listener->accepted_count >= ACCEPT_QUEUE_DEPTH) { listener->state = SLOT_FAILED; return false; }
    int child = -1;
    for (unsigned i = SOCKET_FD_BASE; i < SOCKET_COUNT; ++i)
        if (s_slots[i].state == SLOT_FREE) { child = (int)i; break; }
    if (child < 0) { listener->state = SLOT_FAILED; return false; }
    socket_slot_t *accepted = &s_slots[child]; uint32_t gen = accepted->generation + 1U;
    socket_slot_clear_locked(accepted); accepted->generation = gen; accepted->state = SLOT_CONNECTED;
    accepted->type = AT_SOCKET_TYPE_STREAM; accepted->remote_id = (int)pending->con_id;
    accepted->remote_generation = pending->generation; accepted->lwip_fd = pending->lwip_fd;
    accepted->recv_timeout = -1; accepted->send_timeout = (int)RPC_TIMEOUT_MS;
    (void)parse_ipv4(pending->peer_ipv4, &accepted->peer_addr); accepted->peer_port = pending->peer_port;
    unsigned at = (listener->accepted_head + listener->accepted_count++) % ACCEPT_QUEUE_DEPTH;
    listener->accepted[at] = child; listener->accepted_addr[at] = accepted->peer_addr;
    listener->accepted_port[at] = accepted->peer_port;
    for (unsigned p = 0; p < PENDING_RAW_COUNT; ++p) {
        pending_raw_t *raw = &s_pending_raw[p];
        if (!raw->used || raw->raw_fd != accepted->lwip_fd) continue;
        if (raw->rx.size > RX_RING_SIZE - accepted->rx.size) {
            accepted->state = SLOT_FAILED;
            pending_raw_clear_locked(raw);
            break;
        }
        rx_queue_splice_locked(&accepted->rx, &raw->rx);
        pending_raw_clear_locked(raw);
    }
    return true;
}

static bool route_accept_v2_locked(const pending_accept_t *pending)
{
    if (accept_v2_locked(pending)) return true;
    bool starting = false;
    for (unsigned i = 0; i < SOCKET_COUNT; ++i)
        if (s_slots[i].state == SLOT_STARTING_LISTENER && s_slots[i].type == AT_SOCKET_TYPE_STREAM) starting = true;
    if (!starting) return false;
    for (unsigned i = 0; i < ACCEPT_QUEUE_DEPTH; ++i) {
        if (s_pending_accepts[i].used) continue;
        s_pending_accepts[i] = *pending;
        s_pending_accepts[i].used = true;
        return true;
    }
    return false;
}

static void replay_pending_accepts_locked(void)
{
    for (unsigned i = 0; i < ACCEPT_QUEUE_DEPTH; ++i) {
        if (!s_pending_accepts[i].used) continue;
        if (accept_v2_locked(&s_pending_accepts[i])) memset(&s_pending_accepts[i], 0, sizeof(s_pending_accepts[i]));
    }
}

static at_status_t set_tcp_options(int fd, const at_socket_tcp_options_t *options);

int at_socket_create(at_socket_type_t type)
{
    if (type != AT_SOCKET_TYPE_STREAM && type != AT_SOCKET_TYPE_DATAGRAM) { s_last_errno = AT_SOCKET_ERRNO_INVALID; return -1; }
    int32_t init_status = at_socket_init();
    if (init_status != AT_STATUS_OK) {
        s_last_errno = AT_SOCKET_ERRNO_NETWORK_DOWN;
        AT_LOG("socket backend init failed status=%ld", (long)init_status);
        return -1;
    }
    int fd = reserve_slot(type);
    if (fd < 0) s_last_errno = AT_SOCKET_ERRNO_NO_FD;
    return fd;
}

at_socket_errno_t at_socket_connect(int fd, uint32_t addr, uint16_t port)
{
    if (s_wifi_join_active) return fail_with(AT_SOCKET_ERRNO_NETWORK_DOWN);
    uint32_t connect_start = at_socket_now_ms();
    socket_meta_t slot;
    int idx = get_slot(fd, &slot);
    if (idx < 0 || port == 0U || slot.state != SLOT_CREATED) return fail_with(AT_SOCKET_ERRNO_INVALID);
    if (!state_lock()) return fail_with(AT_SOCKET_ERRNO_BUSY);
    if (s_slots[idx].generation != slot.generation || s_slots[idx].state != SLOT_CREATED) {
        state_unlock(); return fail_with(AT_SOCKET_ERRNO_BUSY);
    }
    int remote = allocate_remote_id_locked();
    if (remote < 0) {
        state_unlock(); return fail_with(AT_SOCKET_ERRNO_NO_FD);
    }
    uint32_t generation = s_slots[idx].generation;
    s_slots[idx].state = SLOT_CONNECTING;
    s_slots[idx].remote_id = remote;
    s_slots[idx].peer_addr = addr;
    s_slots[idx].peer_port = port;
    state_unlock();
    char ip[16], command[96];
    char reply[256] = {0};
    size_t reply_length = 0U;
    addr_to_text(addr, ip);
    /* An H77 reset does not reset the AT: the module keeps its AT client
     * slots, so CIPSTART=<id> fails with ERROR when the host reuses an id the
     * module still holds from the previous session. Clear this id first; a
     * free id just answers ERROR and is harmless. */
    char preclose[32];
    (void)snprintf(preclose, sizeof(preclose), "AT+CIPCLOSE=%d", remote);
    (void)run_at(preclose, NULL, 0, false, NULL, 0, NULL, NULL);
    if (slot.type == AT_SOCKET_TYPE_STREAM)
        (void)snprintf(command, sizeof(command), "AT+CIPSTART=%d,\"TCP\",\"%s\",%u,0", remote, ip,
                       (unsigned)port);
    else
        (void)snprintf(command, sizeof(command), "AT+CIPSTART=%d,\"UDP\",\"%s\",%u", remote, ip,
                       (unsigned)port);
    int32_t status = run_at(command, NULL, 0, false, reply, sizeof(reply), &reply_length, NULL);
    if (status != AT_STATUS_OK) {
        AT_LOG("%s failed status=%ld reply=%.*s", command, (long)status, (int)reply_length, reply);
        if (reply_length == 0U) {
            /* An empty reply means the AT task did not answer within the RPC
             * timeout. Resetting AT here also tears down the station link
             * and forces a full re-join, which is far more disruptive than the
             * failed socket. Leave the module running; the upper layer retries
             * and the transport recovers independently. */
            AT_SUMMARY("CIPSTART con=%d timed out after %lums, leaving AT running", remote,
                          (unsigned long)(at_socket_now_ms() - connect_start));
            return fail_with(AT_SOCKET_ERRNO_CONNECTION_RESET);
        }
    }
    remote_snapshot_t remote_status = query_remote(remote);
    uint32_t remote_generation = remote_status.generation;
    int raw = remote_status.state == REMOTE_STATE_ACTIVE ? remote_status.raw_fd : -1;
    if (!state_lock()) return fail_with(AT_SOCKET_ERRNO_CONNECTION_RESET);
    if (s_slots[idx].generation != generation || s_slots[idx].state != SLOT_CONNECTING) {
        state_unlock(); return fail_with(AT_SOCKET_ERRNO_CONNECTION_RESET);
    }
    if (status != AT_STATUS_OK || raw < 0) {
        AT_SUMMARY("CIPSTART con=%d failed status=%ld raw=%d state=%d elapsed=%lums", remote,
                      (long)status, raw, (int)remote_status.state,
                      (unsigned long)(at_socket_now_ms() - connect_start));
        if (remote_status.state == REMOTE_STATE_ABSENT) {
            s_slots[idx].state = SLOT_NO_REMOTE;
            s_slots[idx].remote_id = -1; s_slots[idx].lwip_fd = -1;
            s_slots[idx].remote_generation = 0U;
        } else {
            s_slots[idx].state = SLOT_QUARANTINE;
            s_slots[idx].remote_id = remote;
            s_slots[idx].lwip_fd = remote_status.raw_fd;
            s_slots[idx].remote_generation = remote_status.generation;
        }
        state_unlock();
        if (remote_status.state == REMOTE_STATE_ACTIVE || remote_status.state == REMOTE_STATE_RETIRED) {
            if (at_socket_close(fd) != AT_SOCKET_ERRNO_NONE) return fail_with(AT_SOCKET_ERRNO_CONNECTION_RESET);
        } else if (remote_status.state == REMOTE_STATE_UNKNOWN) {
            return fail_with(AT_SOCKET_ERRNO_CONNECTION_RESET);
        }
        return fail_with(status == AT_STATUS_OK ? AT_SOCKET_ERRNO_CONNECTION_RESET : AT_SOCKET_ERRNO_CONNECTION_REFUSED);
    }
    s_slots[idx].lwip_fd = raw;
    s_slots[idx].remote_generation = remote_generation;
    s_slots[idx].state = SLOT_CONNECTED;
    for (unsigned p = 0; p < PENDING_RAW_COUNT; ++p) {
        pending_raw_t *pending = &s_pending_raw[p];
        if (!pending->used || pending->raw_fd != raw) continue;
        socket_slot_t *connected = &s_slots[idx];
        if (connected->type == AT_SOCKET_TYPE_DATAGRAM) {
            rx_packet_t *build_head = NULL;
            size_t build_length = 0U;
            uint8_t completed = 0U;
            for (rx_packet_t *packet = pending->rx.head; packet; packet = packet->next) {
                if (!build_head) build_head = packet;
                build_length += packet->length;
                if (build_length > UDP_MAX_DATAGRAM) {
                    connected->state = SLOT_FAILED; s_last_errno = AT_SOCKET_ERRNO_MESSAGE_TOO_LARGE;
                    pending_raw_clear_locked(pending);
                    state_unlock(); return AT_SOCKET_ERRNO_MESSAGE_TOO_LARGE;
                }
                packet->addr = connected->peer_addr;
                packet->port = connected->peer_port;
                if (packet->final) {
                    if (completed == UINT8_MAX) break;
                    ++completed;
                    build_head = NULL;
                    build_length = 0U;
                }
            }
            if (completed > UDP_QUEUE_DEPTH || connected->rx.build_length) {
                connected->state = SLOT_FAILED; s_last_errno = AT_SOCKET_ERRNO_NO_BUFFER;
                pending_raw_clear_locked(pending);
                state_unlock(); return AT_SOCKET_ERRNO_NO_BUFFER;
            }
            connected->rx.build_head = build_head;
            connected->rx.build_prev = build_head ? connected->rx.tail : NULL;
            connected->rx.build_length = build_length;
            connected->rx.completed_datagrams = completed;
            rx_queue_splice_locked(&connected->rx, &pending->rx);
        } else {
            if (pending->rx.size > RX_RING_SIZE - connected->rx.size) {
                connected->state = SLOT_FAILED; s_last_errno = AT_SOCKET_ERRNO_NO_BUFFER;
                pending_raw_clear_locked(pending);
                state_unlock(); return AT_SOCKET_ERRNO_NO_BUFFER;
            }
            size_t pending_size = pending->rx.size;
            rx_queue_splice_locked(&connected->rx, &pending->rx);
            connected->rx_events += pending->datagram_count ? pending->datagram_count : 1U;
            connected->rx_bytes += (uint32_t)pending_size;
        }
        pending_raw_clear_locked(pending);
    }
    state_unlock();
    wake_waiters();
    if (slot.type == AT_SOCKET_TYPE_STREAM && slot.tcp_options_configured &&
        set_tcp_options(fd, &slot.tcp_options) != AT_STATUS_OK) {
        (void)at_socket_close(fd);
        return fail_with(AT_SOCKET_ERRNO_CONNECTION_RESET);
    }
    AT_SUMMARY("CIPSTART con=%d raw=%d connected elapsed=%lums", remote, raw,
                  (unsigned long)(at_socket_now_ms() - connect_start));
    return AT_SOCKET_ERRNO_NONE;
}

static int send_common(int fd, const void *buf, uint32_t length, bool explicit_peer,
                       uint32_t peer, uint16_t port);
static int start_server(at_socket_type_t type, uint16_t port, uint32_t *listener_generation);

static int send_common(int fd, const void *buf, uint32_t length, bool explicit_peer,
                       uint32_t peer, uint16_t port)
{
    if (s_wifi_join_active) return fail_with(AT_SOCKET_ERRNO_NETWORK_DOWN);
    /* The AP provisioning path may queue one final discovery broadcast after
     * the radio has already left SoftAP. It cannot be transmitted, so preserve
     * the caller-visible success without consuming an AT transaction. */
    if (explicit_peer && peer == UINT32_MAX && s_ap_teardown) return (int)length;
    socket_meta_t slot;
    int idx = get_slot(fd, &slot);
    if (idx < 0) {
        unsigned active = 0U;
        for (unsigned i = 0U; i < SOCKET_COUNT; ++i)
            if (s_slots[i].state != SLOT_FREE) ++active;
        AT_SUMMARY("socket send invalid fd=%d active=%u", fd, active);
        return fail_with(AT_SOCKET_ERRNO_BAD_FD);
    }
    if (!buf) {
        AT_SUMMARY("socket send null buffer fd=%d length=%u", fd, (unsigned)length);
        return fail_with(AT_SOCKET_ERRNO_INVALID);
    }
    if (length == 0U) return AT_SOCKET_ERRNO_NONE;
    if (slot.type == AT_SOCKET_TYPE_DATAGRAM && length > s_max_send) return fail_with(AT_SOCKET_ERRNO_MESSAGE_TOO_LARGE);
    uint32_t stream_chunk_size = s_max_send < STREAM_TX_CHUNK_SIZE ? s_max_send : STREAM_TX_CHUNK_SIZE;
    if (slot.type == AT_SOCKET_TYPE_STREAM && length > stream_chunk_size) {
        uint32_t sent = 0U;
        while (sent < length) {
            uint32_t chunk = length - sent;
            if (chunk > stream_chunk_size) chunk = stream_chunk_size;
            int result = send_common(fd, (const uint8_t *)buf + sent, chunk, false, 0U, 0U);
            if (result <= 0) return sent ? (int)sent : result;
            sent += (uint32_t)result;
            if (sent < length) at_socket_sleep(STREAM_TX_GAP_MS);
        }
        return (int)sent;
    }
    /* A UDP socket that was only created (no bind, no connect) is a client:
     * MAX_CLIENT_NUM selects the module UDP *server*, so sending through it
     * fails with ERROR while no listener exists. Connect lazily instead. */
    if (slot.type == AT_SOCKET_TYPE_DATAGRAM && explicit_peer && slot.state == SLOT_CREATED) {
        at_socket_errno_t linked = at_socket_connect(fd, peer, port);
        if (linked != AT_SOCKET_ERRNO_NONE) return linked;
        idx = get_slot(fd, &slot);
        if (idx < 0) return fail_with(AT_SOCKET_ERRNO_BAD_FD);
    }
    if (slot.state != SLOT_CONNECTED &&
        !(slot.type == AT_SOCKET_TYPE_DATAGRAM && explicit_peer && slot.state == SLOT_LISTENER)) {
        AT_SUMMARY("socket send fd=%d not connected state=%d type=%d length=%u",
                   fd, (int)slot.state, (int)slot.type, (unsigned)length);
        return fail_with(AT_SOCKET_ERRNO_NOT_CONNECTED);
    }
    char command[128];
    char ip[16];
    addr_to_text(peer, ip);
    if (slot.type == AT_SOCKET_TYPE_DATAGRAM && explicit_peer && slot.state == SLOT_LISTENER) {
        /* CAP2 AP-candidate path: UDP_SEND_ID selects the UDP listener so the
         * source port stays 6667. The trailing listener fd is not supported by
         * that proven wire format. */
        (void)snprintf(command, sizeof(command), "AT+CIPSEND=%u,%u,%s,%u",
                       (unsigned)s_udp_send_id, (unsigned)length, ip, (unsigned)port);
    } else if (slot.type == AT_SOCKET_TYPE_DATAGRAM && explicit_peer) {
        /* Connected UDP client: keep the destination explicit so one socket can
         * still reach several peers. */
        if (slot.remote_id < 0) return fail_with(AT_SOCKET_ERRNO_NOT_CONNECTED);
        (void)snprintf(command, sizeof(command), "AT+CIPSEND=%d,%u,%s,%u", slot.remote_id,
                       (unsigned)length, ip, (unsigned)port);
    } else if (explicit_peer) {
        return fail_with(AT_SOCKET_ERRNO_INVALID);
    } else if (slot.remote_id >= 0) {
        (void)snprintf(command, sizeof(command), "AT+CIPSEND=%d,%u", slot.remote_id, (unsigned)length);
    } else {
        return fail_with(AT_SOCKET_ERRNO_NOT_CONNECTED);
    }
    char reply[256]; size_t reply_length = 0;
    uint32_t requested_timeout = slot.send_timeout < 0 ? RPC_TIMEOUT_MS : (uint32_t)slot.send_timeout;
    uint32_t timeout = requested_timeout;
    if (timeout > RPC_TIMEOUT_MS) {
        AT_LOG("socket send timeout capped fd=%d requested=%lums capped=%lums",
                  fd, (unsigned long)requested_timeout, (unsigned long)RPC_TIMEOUT_MS);
        timeout = RPC_TIMEOUT_MS;
    }
    uint32_t send_start = at_socket_now_ms();
    int32_t status = AT_STATUS_OK;
    uint8_t send_fail_retries = 0U;
    bool reopened_listener = false;
    for (;;) {
        reply_length = 0U;
        reply[0] = '\0';
        status = run_at_with_timeout(command, buf, length, true, reply, sizeof(reply), &reply_length,
                                     NULL, timeout);
        bool send_failed = false;
        bool send_ok = false;
        for (size_t i = 0U; i + 9U <= reply_length; ++i) {
            if (memcmp(reply + i, "SEND FAIL", 9U) == 0) {
                send_failed = true;
            }
        }
        for (size_t i = 0U; i + 7U <= reply_length; ++i) {
            if (memcmp(reply + i, "SEND OK", 7U) == 0) {
                send_ok = true;
                break;
            }
        }
        /* A UDP listener created while the module was still in SoftAP mode
         * keeps its old vif binding after the switch to STA, so every reply is
         * rejected with ERROR (observed every 3 s from the network manager). Re-open the
         * listener on the current interface and retry the datagram. */
        if (slot.type == AT_SOCKET_TYPE_DATAGRAM && slot.state == SLOT_LISTENER && slot.local_port &&
            !reopened_listener && (status != AT_STATUS_OK || (reply_length && !send_ok))) {
            reopened_listener = true;
            AT_SUMMARY("UDP listener send failed, reopening port=%u", (unsigned)slot.local_port);
            char stop[48];
            (void)snprintf(stop, sizeof(stop), AT_WIRE_SERVER_STOP_FMT, "UDP",
                           (unsigned)slot.local_port);
            (void)run_at(stop, NULL, 0, false, NULL, 0, NULL, NULL);
            uint32_t generation = 0U;
            int raw = start_server(AT_SOCKET_TYPE_DATAGRAM, slot.local_port, &generation);
            if (raw >= 0) {
                if (state_lock()) {
                    if (s_slots[idx].generation == slot.generation) {
                        s_slots[idx].lwip_fd = raw;
                        s_slots[idx].listener_generation = generation;
                    }
                    state_unlock();
                }
                continue;
            }
        }
        if (!send_failed || send_fail_retries >= SEND_FAIL_RETRY_COUNT) {
            break;
        }
        /* The module's local socket queue rejected the payload before any TCP
         * byte was sent (SEND FAIL). Give the Wi-Fi manager task time to drain
         * and resend the same payload once. */
        ++send_fail_retries;
        uint32_t delay_ms = 250U << (send_fail_retries - 1U);
        AT_SUMMARY("socket send SEND FAIL, retry=%u/%u delay=%lums fd=%d remote=%d len=%u",
                      (unsigned)send_fail_retries, (unsigned)SEND_FAIL_RETRY_COUNT,
                      (unsigned long)delay_ms, fd, slot.remote_id, (unsigned)length);
        at_socket_sleep(delay_ms);
    }
    uint32_t send_elapsed = at_socket_now_ms() - send_start;
    if (send_elapsed >= 500U) {
        AT_SUMMARY("slow CIPSEND fd=%d remote=%d len=%u elapsed=%lums status=%ld", fd,
                      slot.remote_id, (unsigned)length, (unsigned long)send_elapsed, (long)status);
    }
    if (status != AT_STATUS_OK) {
        AT_SUMMARY("socket send failed fd=%d remote=%d len=%u status=%ld reply=%.*s", fd, slot.remote_id,
                      (unsigned)length, (long)status, (int)reply_length, reply);
        return fail_with(AT_SOCKET_ERRNO_CONNECTION_RESET);
    }
    bool send_ok = false;
    for (size_t i = 0; i + 7U <= reply_length; ++i)
        if (memcmp(reply + i, "SEND OK", 7U) == 0) send_ok = true;
    if (reply_length && !send_ok) {
        AT_SUMMARY("socket send missing SEND OK fd=%d remote=%d len=%u reply=%.*s", fd, slot.remote_id,
                      (unsigned)length, (int)reply_length, reply);
        return fail_with(AT_SOCKET_ERRNO_CONNECTION_RESET);
    }
    return (int)length;
}

int at_socket_send(int fd, const void *buf, uint32_t length)
{ return send_common(fd, buf, length, false, 0U, 0U); }
int at_socket_sendto(int fd, const void *buf, uint32_t length, uint32_t addr, uint16_t port)
{
    if (port == 0U) return fail_with(AT_SOCKET_ERRNO_INVALID);
    return send_common(fd, buf, length, true, addr, port);
}

at_socket_errno_t at_socket_close(int fd)
{
    socket_meta_t slot;
    int idx = get_slot(fd, &slot);
    AT_LOG("socket close fd=%d idx=%d state=%d", fd, idx, idx >= 0 ? (int)slot.state : -1);
    if (idx < 0) return fail_with(AT_SOCKET_ERRNO_BAD_FD);
    if (slot.state == SLOT_CREATED) {
        if (!state_lock()) return fail_with(AT_SOCKET_ERRNO_BUSY);
        socket_slot_clear_locked(&s_slots[idx]);
        state_unlock();
        return AT_SOCKET_ERRNO_NONE;
    }
    if (slot.state == SLOT_NO_REMOTE) {
        if (!state_lock()) return fail_with(AT_SOCKET_ERRNO_BUSY);
        if (s_slots[idx].generation == slot.generation && s_slots[idx].state == SLOT_NO_REMOTE) {
            uint32_t gen = slot.generation + 1U; socket_slot_clear_locked(&s_slots[idx]);
            s_slots[idx].generation = gen;
        }
        state_unlock(); return AT_SOCKET_ERRNO_NONE;
    }
    if (slot.state == SLOT_QUARANTINE) {
        remote_snapshot_t remote = query_remote(slot.remote_id);
        if (remote.state == REMOTE_STATE_UNKNOWN) return fail_with(AT_SOCKET_ERRNO_CONNECTION_RESET);
        if (remote.state == REMOTE_STATE_ABSENT) {
            if (!state_lock()) return fail_with(AT_SOCKET_ERRNO_BUSY);
            if (s_slots[idx].generation == slot.generation && s_slots[idx].state == SLOT_QUARANTINE) {
                uint32_t gen = slot.generation + 1U; socket_slot_clear_locked(&s_slots[idx]);
                s_slots[idx].generation = gen;
            }
            state_unlock(); return AT_SOCKET_ERRNO_NONE;
        }
        if (remote.state == REMOTE_STATE_RETIRED) {
            char ack[96];
            (void)snprintf(ack, sizeof(ack), AT_WIRE_CLOSE_FMT, slot.remote_id, remote.raw_fd,
                           (unsigned long)remote.generation);
            if (run_at(ack, NULL, 0, false, NULL, 0, NULL, NULL) != AT_STATUS_OK)
                return fail_with(AT_SOCKET_ERRNO_CONNECTION_RESET);
            if (!state_lock()) return fail_with(AT_SOCKET_ERRNO_BUSY);
            if (s_slots[idx].generation == slot.generation && s_slots[idx].state == SLOT_QUARANTINE) {
                uint32_t gen = slot.generation + 1U; socket_slot_clear_locked(&s_slots[idx]);
                s_slots[idx].generation = gen;
            }
            state_unlock(); return AT_SOCKET_ERRNO_NONE;
        }
        if (!state_lock()) return fail_with(AT_SOCKET_ERRNO_BUSY);
        if (s_slots[idx].generation != slot.generation || s_slots[idx].state != SLOT_QUARANTINE) {
            state_unlock(); return fail_with(AT_SOCKET_ERRNO_BUSY);
        }
        s_slots[idx].lwip_fd = remote.raw_fd; s_slots[idx].remote_generation = remote.generation;
        slot.lwip_fd = remote.raw_fd; slot.remote_generation = remote.generation;
        state_unlock();
    }
    if (slot.state == SLOT_LISTENER) {
        if (slot.ap_listener_invalid) {
            /* AT closed every listener when the default vif left SoftAP.
             * Do not send a stop that can only answer ERROR. */
            AT_LOG("AP listener already closed by vif switch port=%u", (unsigned)slot.local_port);
            return release_listener_slot(idx, &slot);
        }
        char command[48], reply[256] = {0}; size_t reply_len = 0; uint32_t watermark = 0;
        const char *kind = slot.type == AT_SOCKET_TYPE_STREAM ? "TCP" : "UDP";
        /* The module stops a listener only for the explicit server stop form;
         * port 0 is rejected with ERROR, which used to leave the slot
         * allocated forever and eventually exhausted the fd table (ENFILE). */
        (void)snprintf(command, sizeof(command), AT_WIRE_SERVER_STOP_FMT, kind,
                       (unsigned)slot.local_port);
        int32_t stop_status = run_at(command, NULL, 0, false, reply, sizeof(reply), &reply_len,
                                     &watermark);
        if (stop_status != AT_STATUS_OK && reply_len == 0U) {
            /* CWJAP owns the shared AT channel for up to 30 s, so this stop
             * never reached AT (the log shows status=-1, empty reply).
             * The listener lives on the AP vif, and AT now closes every
             * server when that vif switches to STA. Release the logical slot
             * instead of leaking it and making the LAN server start that
             * follows activation fail. */
            AT_SUMMARY("listener stop deferred port=%u", (unsigned)slot.local_port);
            return release_listener_slot(idx, &slot);
        }
        if (stop_status != AT_STATUS_OK) {
            /* The module answered ERROR. It may already have dropped this
             * listener together with the AP vif; the table check decides. */
            AT_SUMMARY("listener stop rejected port=%u, verifying", (unsigned)slot.local_port);
        } else if (watermark && at_device_wait_events(watermark, RPC_TIMEOUT_MS) != AT_STATUS_OK) {
            AT_SUMMARY("listener stop ack timeout port=%u, verifying", (unsigned)slot.local_port);
        }
        char tcp_line[64], udp_line[64];
        (void)snprintf(tcp_line, sizeof(tcp_line), AT_WIRE_SERVER_EVENT_TCP_FMT,
                       (unsigned)slot.local_port);
        (void)snprintf(udp_line, sizeof(udp_line), AT_WIRE_SERVER_EVENT_UDP_FMT,
                       (unsigned)slot.local_port);
        const char *listen_line = slot.type == AT_SOCKET_TYPE_STREAM ? tcp_line : udp_line;
        bool still_listening = true;
        bool query_ok = false;
        for (unsigned attempt = 0U; attempt < 50U; ++attempt) {
            memset(reply, 0, sizeof(reply));
            reply_len = 0U;
            if (run_at(AT_WIRE_SERVER_QUERY, NULL, 0, false, reply, sizeof(reply),
                       &reply_len, NULL) != AT_STATUS_OK)
                break;
            query_ok = true;
            if (!strstr(reply, listen_line)) {
                still_listening = false;
                break;
            }
            at_socket_sleep(20U);
        }
        if (query_ok && still_listening) return fail_with(AT_SOCKET_ERRNO_BUSY);
        if (!query_ok) {
            /* The module table is unreadable right now (the vif switch owns the
             * AT task). The listener cannot survive that switch, so releasing
             * the host slot is safer than leaking it into the LAN start. */
            AT_SUMMARY("listener state unknown port=%u, releasing slot",
                          (unsigned)slot.local_port);
        }
        return release_listener_slot(idx, &slot);
    }
    /* Remote IDs stay quarantined until the V2 close-after-close event. */
    if (slot.state == SLOT_EOF) {
        char ack[96];
        (void)snprintf(ack, sizeof(ack), AT_WIRE_CLOSE_FMT, slot.remote_id, slot.lwip_fd,
                       (unsigned long)slot.remote_generation);
        if (run_at(ack, NULL, 0, false, NULL, 0, NULL, NULL) != AT_STATUS_OK) return fail_with(AT_SOCKET_ERRNO_CONNECTION_RESET);
        if (!state_lock()) return fail_with(AT_SOCKET_ERRNO_BUSY);
        if (s_slots[idx].generation == slot.generation && s_slots[idx].state == SLOT_EOF) {
            uint32_t gen = slot.generation + 1U;
            socket_slot_clear_locked(&s_slots[idx]);
            s_slots[idx].generation = gen;
        }
        state_unlock();
        return AT_SOCKET_ERRNO_NONE;
    }
    if (!state_lock()) return fail_with(AT_SOCKET_ERRNO_BUSY);
    uint32_t generation = s_slots[idx].generation;
    s_slots[idx].state = SLOT_CLOSING;
    state_unlock();
    char command[48];
    if (slot.remote_id >= 0)
        (void)snprintf(command, sizeof(command), "AT+CIPCLOSE=%d", slot.remote_id);
    else
        (void)snprintf(command, sizeof(command), "AT+CIPCLOSE");
    uint32_t watermark = 0;
    int32_t status = run_at(command, NULL, 0, false, NULL, 0, NULL, &watermark);
#ifndef GD32H77X_AT_SOCKET_TEST
    if (status != AT_STATUS_OK && at_session_is_poisoned()) {
        /* The fault that aborted the previous transaction also fails this
         * close. Waiting for the transport to recover and retrying keeps the
         * con_id from leaking: the module only reuses it after CIPCLOSE. */
        for (unsigned wait = 0U; wait < 80U && at_session_is_poisoned(); ++wait)
            at_socket_sleep(25U);
        status = run_at(command, NULL, 0, false, NULL, 0, NULL, &watermark);
    }
#endif
    if (status != AT_STATUS_OK) return fail_with(AT_SOCKET_ERRNO_CONNECTION_RESET);
    /* AT+CIPCLOSE only raises stop_flag. The AT receive worker retires the
     * slot asynchronously and sends the close event later. Do not reuse con_id
     * before that transition: the next CIPSTART then races the old tuple and
     * returns ERROR. */
    for (unsigned attempt = 0U; attempt < 50U; ++attempt) {
        socket_meta_t current;
        if (get_slot(fd, &current) < 0 || current.generation != generation) return AT_SOCKET_ERRNO_NONE;
        if (current.state == SLOT_EOF) {
            char ack[96];
            (void)snprintf(ack, sizeof(ack), AT_WIRE_CLOSE_FMT, current.remote_id,
                           current.lwip_fd, (unsigned long)current.remote_generation);
            if (run_at(ack, NULL, 0, false, NULL, 0, NULL, NULL) != AT_STATUS_OK)
                return fail_with(AT_SOCKET_ERRNO_CONNECTION_RESET);
            if (!state_lock()) return fail_with(AT_SOCKET_ERRNO_BUSY);
            if (s_slots[idx].generation == generation && s_slots[idx].state == SLOT_EOF) {
                uint32_t gen = generation + 1U;
                socket_slot_clear_locked(&s_slots[idx]);
                s_slots[idx].generation = gen;
            }
            state_unlock();
            return AT_SOCKET_ERRNO_NONE;
        }
        at_socket_sleep(20U);
    }

    remote_snapshot_t remote = query_remote(slot.remote_id);
    if (remote.state == REMOTE_STATE_ABSENT) {
        if (!state_lock()) return fail_with(AT_SOCKET_ERRNO_BUSY);
        if (s_slots[idx].generation == generation) {
            uint32_t gen = generation + 1U;
            socket_slot_clear_locked(&s_slots[idx]);
            s_slots[idx].generation = gen;
        }
        state_unlock();
        return AT_SOCKET_ERRNO_NONE;
    }

    /* Keep the tuple quarantined until a later close event/ACK. A new socket
     * will allocate another con_id instead of racing this one. */
    if (!state_lock()) return fail_with(AT_SOCKET_ERRNO_BUSY);
    if (s_slots[idx].generation == generation && s_slots[idx].state == SLOT_CLOSING) {
        s_slots[idx].state = SLOT_QUARANTINE;
        s_slots[idx].remote_id = slot.remote_id;
        s_slots[idx].lwip_fd = remote.raw_fd;
    }
    state_unlock();
    return AT_SOCKET_ERRNO_NONE;
}

void at_socket_on_device_event(void *context, const at_device_event_t *event)
{
    (void)context;
    if (!event || !state_lock()) return;
    bool wake = false;
    if (event->kind == AT_DEVICE_EVENT_LINE) {
        int raw = -1;
        if (parse_rx_ready(event->value.line.text, event->value.line.length, &raw)) {
            for (unsigned i = 0; i < SOCKET_COUNT; ++i) {
                socket_slot_t *slot = &s_slots[i];
                if (slot->state == SLOT_CONNECTED && slot->type == AT_SOCKET_TYPE_STREAM &&
                    slot->lwip_fd == raw) {
                    if (!slot->rx_ready) {
                        ++slot->rx_ready_events;
                        if (slot->rx_ready_events == 1U) {
                            AT_SUMMARY("socket on-demand ready fd=%d raw=%d", (int)i, raw);
                        }
                    }
                    slot->rx_ready = true;
                    wake = true;
                    AT_LOG("socket rx ready fd=%d raw=%d", (int)i, raw);
                    break;
                }
            }
        }
    } else if (event->kind == AT_DEVICE_EVENT_DATA) {
        int raw = event->value.data.remote_fd;
        int slot_idx = -1;
        for (unsigned i = 0; i < SOCKET_COUNT; ++i)
            if (s_slots[i].state == SLOT_CONNECTED && s_slots[i].lwip_fd == raw) { slot_idx = (int)i; break; }
        if (slot_idx >= 0) {
            socket_slot_t *slot = &s_slots[slot_idx];
            size_t len = event->value.data.length;
            if (len) {
                ++slot->rx_events;
                slot->rx_bytes += (uint32_t)len;
            }
            if (slot->type == AT_SOCKET_TYPE_DATAGRAM) {
                if (!udp_consume_locked(slot, event->value.data.data, len, slot->peer_addr,
                                        slot->peer_port, event->value.data.final)) {
                    slot->state = SLOT_FAILED; s_last_errno = AT_SOCKET_ERRNO_NO_BUFFER;
                }
            } else if (len > RX_RING_SIZE - slot->rx.size) {
                AT_SUMMARY("socket rx overflow fd=%d raw=%d buffered=%lu incoming=%lu capacity=%u",
                          (int)(slot - s_slots), raw, (unsigned long)slot->rx.size,
                          (unsigned long)len, (unsigned)RX_RING_SIZE);
                slot->state = SLOT_FAILED;
                s_last_errno = AT_SOCKET_ERRNO_NO_BUFFER;
            } else {
                if (!rx_queue_append_locked(&slot->rx, event->value.data.data, len, 0U, 0U,
                                            event->value.data.final)) {
                    AT_SUMMARY("socket rx allocation failed fd=%d raw=%d buffered=%lu in_use=%lu limit=%u",
                              (int)(slot - s_slots), raw, (unsigned long)slot->rx.size,
                              (unsigned long)s_rx_dynamic_bytes, (unsigned)RX_DYNAMIC_LIMIT);
                    slot->state = SLOT_FAILED;
                    s_last_errno = AT_SOCKET_ERRNO_NO_BUFFER;
                }
            }
            wake = true;
        } else {
            /* IPD may race CIPSTART's reply. Aggregate chunks by raw fd so
             * they are assigned only after STATE identifies that descriptor. */
            size_t len = event->value.data.length;
            bool can_hold = false;
            for (unsigned i = 0; i < SOCKET_COUNT; ++i)
                if (s_slots[i].state == SLOT_CONNECTING || s_slots[i].state == SLOT_STARTING_LISTENER) can_hold = true;
            for (unsigned i = 0; i < ACCEPT_QUEUE_DEPTH; ++i)
                if (s_pending_accepts[i].used && s_pending_accepts[i].lwip_fd == raw) can_hold = true;
            if (can_hold) {
                if (!pending_raw_append_locked(raw, event->value.data.data, len, event->value.data.final)) {
                    AT_LOG("pending raw overflow raw=%d incoming=%lu final=%d",
                              raw, (unsigned long)len, event->value.data.final ? 1 : 0);
                    for (unsigned i = 0; i < SOCKET_COUNT; ++i)
                        if (s_slots[i].state == SLOT_CONNECTING || s_slots[i].state == SLOT_STARTING_LISTENER)
                            s_slots[i].state = SLOT_FAILED;
                    s_last_errno = AT_SOCKET_ERRNO_NO_BUFFER;
                }
                wake = true;
            } else {
                AT_LOG("socket data dropped: unmatched fd=%d len=%lu", raw, (unsigned long)len);
            }
        }
    } else if (event->kind == AT_DEVICE_EVENT_DATAGRAM) {
        int listener_fd = event->value.datagram.listener_fd;
        uint32_t peer_addr = 0;
        (void)parse_ipv4(event->value.datagram.peer_ipv4, &peer_addr);
        bool routed = false;
        for (unsigned i = 0; i < SOCKET_COUNT; ++i) {
            socket_slot_t *slot = &s_slots[i];
            if (slot->state != SLOT_LISTENER || slot->type != AT_SOCKET_TYPE_DATAGRAM) continue;
            if (listener_fd >= 0 && slot->lwip_fd != listener_fd) continue;
            if (!udp_consume_locked(slot, event->value.datagram.data, event->value.datagram.length,
                                    peer_addr, event->value.datagram.peer_port,
                                    event->value.datagram.final)) {
                slot->state = SLOT_FAILED; s_last_errno = AT_SOCKET_ERRNO_NO_BUFFER;
            }
            routed = true; wake = true; break;
        }
        if (!routed) {
            bool starting = false;
            for (unsigned i = 0; i < SOCKET_COUNT; ++i)
                if (s_slots[i].state == SLOT_STARTING_LISTENER && s_slots[i].type == AT_SOCKET_TYPE_DATAGRAM)
                    starting = true;
            if (starting) {
                if (!pending_datagram_add_locked(listener_fd, peer_addr, event->value.datagram.peer_port,
                                                 event->value.datagram.data, event->value.datagram.length,
                                                 event->value.datagram.final)) {
                    s_last_errno = AT_SOCKET_ERRNO_NO_BUFFER;
                    AT_LOG("pending UDP overflow listener=%d len=%lu final=%d", listener_fd,
                              (unsigned long)event->value.datagram.length, event->value.datagram.final ? 1 : 0);
                }
            } else {
                AT_LOG("UDP data dropped: unmatched listener=%d len=%lu", listener_fd,
                          (unsigned long)event->value.datagram.length);
            }
        }
    } else if (event->kind == AT_DEVICE_EVENT_ACCEPT) {
        for (unsigned i = 0; i < SOCKET_COUNT; ++i) {
            socket_slot_t *listener = &s_slots[i];
            if (listener->state != SLOT_LISTENER || listener->type != AT_SOCKET_TYPE_STREAM ||
                listener->remote_id != event->value.accept.listener_fd) continue;
            if (listener->accepted_count >= ACCEPT_QUEUE_DEPTH) { s_last_errno = AT_SOCKET_ERRNO_NO_BUFFER; listener->state = SLOT_FAILED; }
            else {
                int child = -1;
                for (unsigned j = SOCKET_FD_BASE; j < SOCKET_COUNT; ++j)
                    if (s_slots[j].state == SLOT_FREE) { child = (int)j; break; }
                if (child >= 0) {
                    socket_slot_t *accepted = &s_slots[child];
                    uint32_t gen = accepted->generation + 1U;
                    socket_slot_clear_locked(accepted);
                    accepted->generation = gen; accepted->state = SLOT_CONNECTED; accepted->type = AT_SOCKET_TYPE_STREAM;
                    accepted->remote_id = event->value.accept.client_fd; accepted->lwip_fd = event->value.accept.client_fd;
                    (void)parse_ipv4(event->value.accept.peer_ipv4, &accepted->peer_addr);
                    accepted->peer_port = event->value.accept.peer_port;
            unsigned at = (listener->accepted_head + listener->accepted_count++) % ACCEPT_QUEUE_DEPTH;
                    listener->accepted[at] = child;
                    listener->accepted_addr[at] = accepted->peer_addr; listener->accepted_port[at] = accepted->peer_port;
                } else { listener->state = SLOT_FAILED; s_last_errno = AT_SOCKET_ERRNO_NO_FD; }
            }
            wake = true; break;
        }
    } else if (event->kind == AT_DEVICE_EVENT_ACCEPT_V2) {
        pending_accept_t pending = {.used = true, .con_id = event->value.accept_v2.con_id,
            .generation = event->value.accept_v2.generation, .listener_generation = event->value.accept_v2.listener_generation,
            .listener_fd = event->value.accept_v2.listener_fd, .lwip_fd = event->value.accept_v2.lwip_fd,
            .peer_port = event->value.accept_v2.peer_port};
        if (event->value.accept_v2.peer_ipv4)
            (void)snprintf(pending.peer_ipv4, sizeof(pending.peer_ipv4), "%s", event->value.accept_v2.peer_ipv4);
        if (!route_accept_v2_locked(&pending)) s_last_errno = AT_SOCKET_ERRNO_NO_BUFFER;
        wake = true;
    } else if (event->kind == AT_DEVICE_EVENT_CLOSE_V2) {
        const uint32_t con_id = event->value.close_v2.con_id;
        const int rawfd = event->value.close_v2.lwip_fd;
        const uint32_t generation = event->value.close_v2.generation;
        for (unsigned i = 0; i < SOCKET_COUNT; ++i) {
            socket_slot_t *slot = &s_slots[i];
            if (slot->remote_id == (int)con_id &&
                (slot->lwip_fd < 0 || slot->lwip_fd == rawfd) &&
                (slot->remote_generation == 0U || slot->remote_generation == generation) &&
                slot->state != SLOT_FREE) {
                /* CLOSE is ordered after every old IPD. Keep the Tuya fd and
                 * buffered bytes, but immediately remove the raw-fd route. */
                slot->state = SLOT_EOF; slot->lwip_fd = -1;
                slot->rx_ready = false;
                slot->rx_fetching = false;
                /* Preserve the exact value required by the release ACK. */
                slot->lwip_fd = rawfd; slot->remote_generation = generation;
                wake = true; break;
            }
        }
    } else if (event->kind == AT_DEVICE_EVENT_FAULT) {
        s_last_errno = AT_SOCKET_ERRNO_NETWORK_RESET;
        for (unsigned i = 0; i < SOCKET_COUNT; ++i)
            if (s_slots[i].state != SLOT_FREE) s_slots[i].state = SLOT_FAILED;
        wake = true;
    } else if (event->kind == AT_DEVICE_EVENT_RESET) {
        /* A module reset invalidates every logical descriptor and every raw
         * connection tuple. Preserve only the generation monotonicity. */
        unsigned invalidated = 0U;
        for (unsigned i = 0; i < SOCKET_COUNT; ++i) {
            if (s_slots[i].state != SLOT_FREE) ++invalidated;
        }
        AT_SUMMARY("socket reset invalidates descriptors=%u", invalidated);
        for (unsigned i = 0; i < SOCKET_COUNT; ++i) {
            uint32_t generation = s_slots[i].generation + 1U;
            socket_slot_clear_locked(&s_slots[i]);
            s_slots[i].generation = generation;
        }
        for (unsigned i = 0; i < PENDING_RAW_COUNT; ++i)
            pending_raw_clear_locked(&s_pending_raw[i]);
        memset(s_pending_accepts, 0, sizeof(s_pending_accepts));
        for (unsigned i = 0; i < PENDING_UDP_COUNT; ++i)
            pending_datagram_clear_locked(&s_pending_datagrams[i]);
        s_initialized = false;
        s_initializing = false;
        s_last_errno = AT_SOCKET_ERRNO_NETWORK_RESET;
        wake = true;
    }
    state_unlock();
    if (wake) wake_waiters();
}

static bool fd_requested(const at_socket_fd_set_t *set, unsigned fd)
{ return set && (set->bits[fd / 8U] & (uint8_t)(1U << (fd % 8U))) != 0U; }

static bool socket_has_readable_data(const socket_slot_t *slot)
{
    if (slot->type == AT_SOCKET_TYPE_STREAM) return slot->rx.size != 0U || slot->rx_ready;
    return slot->rx.completed_datagrams != 0U;
}

static void log_select_timeout(const at_socket_fd_set_t *read_set, unsigned bound, uint32_t timeout)
{
    (void)timeout;
    unsigned pending = 0U;
    for (unsigned i = 0U; i < PENDING_RAW_COUNT; ++i)
        if (s_pending_raw[i].used) ++pending;
    for (unsigned i = 0U; i < bound; ++i) {
        socket_slot_t *slot = &s_slots[i];
        if (!fd_requested(read_set, i) || slot->state == SLOT_FREE) continue;
        AT_LOG("socket select timeout fd=%u timeout=%lums state=%d raw=%d buffered=%lu rx_events=%lu rx_bytes=%lu pending_raw=%u",
                  i, (unsigned long)timeout, (int)slot->state, slot->remote_id,
                  (unsigned long)(slot->type == AT_SOCKET_TYPE_STREAM ? slot->rx.size : 0U),
                  (unsigned long)slot->rx_events,
                  (unsigned long)slot->rx_bytes, pending);
    }
}
static void fd_mark(at_socket_fd_set_t *set, unsigned fd)
{ if (set) set->bits[fd / 8U] |= (uint8_t)(1U << (fd % 8U)); }

int at_socket_select(int maxfd, at_socket_fd_set_t *r, at_socket_fd_set_t *w, at_socket_fd_set_t *e, uint32_t timeout)
{
    at_socket_fd_set_t want_r = {0}, want_w = {0}, want_e = {0};
    if (maxfd < 0) return -1;
    /* the TLS stack passes a millisecond value into its seconds-based overtime_s and
     * converts it again (*1000), so a 10 s TLS wait becomes ~2.8 h. Match the
     * send-path cap: a lost ServerHello must fail and retry, not block until
     * the app tears the provisioning session down. */
    if (timeout > RPC_TIMEOUT_MS) {
        AT_LOG("socket select timeout capped requested=%lums capped=%lums",
                  (unsigned long)timeout, (unsigned long)RPC_TIMEOUT_MS);
        timeout = RPC_TIMEOUT_MS;
    }
    if (r) want_r = *r;
    if (w) want_w = *w;
    if (e) want_e = *e;
    unsigned bound = (unsigned)maxfd < SOCKET_COUNT ? (unsigned)maxfd : SOCKET_COUNT;
    uint32_t start = at_socket_now_ms();
    for (;;) {
        if (r) memset(r, 0, sizeof(*r));
        if (w) memset(w, 0, sizeof(*w));
        if (e) memset(e, 0, sizeof(*e));
        int ready = 0;
        if (!state_lock()) return -1;
        for (unsigned i = 0; i < bound; ++i) {
            socket_slot_t *slot = &s_slots[i]; if (slot->state == SLOT_FREE) continue;
            bool readable = socket_has_readable_data(slot) || slot->accepted_count ||
                            slot->state == SLOT_FAILED || slot->state == SLOT_EOF;
            bool writable = slot->state == SLOT_CONNECTED || slot->state == SLOT_LISTENER;
            bool exceptional = slot->state == SLOT_FAILED;
            if ((fd_requested(&want_r, i) && readable) || (fd_requested(&want_w, i) && writable) ||
                (fd_requested(&want_e, i) && exceptional)) {
                if (fd_requested(&want_r, i) && readable) fd_mark(r, i);
                if (fd_requested(&want_w, i) && writable) fd_mark(w, i);
                if (fd_requested(&want_e, i) && exceptional) fd_mark(e, i);
                ++ready;
            }
        }
        state_unlock();
        if (ready || timeout == 0U) return ready;
        uint32_t now = at_socket_now_ms();
        if ((uint32_t)(now - start) >= timeout) {
            if (state_lock()) {
                log_select_timeout(&want_r, bound, timeout);
                state_unlock();
            }
            return 0;
        }
        (void)xSemaphoreTake(s_ready, pdMS_TO_TICKS(10U));
    }
}

int at_socket_get_nonblock(int fd)
{ socket_meta_t slot; int idx = get_slot(fd, &slot); return idx < 0 ? -1 : (slot.nonblock ? 1 : 0); }
at_status_t at_socket_set_block(int fd, bool block)
{ if (!state_lock()) return AT_STATUS_IO_ERROR; int idx = find_logical(fd); if (idx < 0) { state_unlock(); return AT_STATUS_INVALID_PARAM; } s_slots[idx].nonblock = !block; state_unlock(); return AT_STATUS_OK; }
at_socket_errno_t at_socket_shutdown(int fd, int how) { (void)fd; (void)how; return AT_SOCKET_ERRNO_UNSUPPORTED; }
static int start_server(at_socket_type_t type, uint16_t port, uint32_t *listener_generation)
{
    if (s_wifi_join_active) return -1;
    char command[80], reply[384] = {0}; size_t len = 0;
    const char *name = type == AT_SOCKET_TYPE_STREAM ? "TCP" : "UDP";
    (void)snprintf(command, sizeof(command), AT_WIRE_SERVER_START_FMT, name, (unsigned)port);
    if (run_at(command, NULL, 0, false, NULL, 0, NULL, NULL) != AT_STATUS_OK) return -1;
    if (run_at(AT_WIRE_SERVER_QUERY, NULL, 0, false, reply, sizeof(reply), &len, NULL) != AT_STATUS_OK) return -1;
    char type_text[4]; unsigned found_port, gen; int listener_fd;
    const char *line = reply;
    bool found = false;
    while ((line = strstr(line, AT_WIRE_SERVER_EVENT_PREFIX)) != NULL) {
        if (sscanf(line, AT_WIRE_SERVER_EVENT_SCAN_FMT, type_text, &found_port,
                   &listener_fd, &gen) == 4 &&
            strcmp(type_text, name) == 0 && found_port == port && listener_fd >= 0) {
            found = true;
            break;
        }
        line += sizeof(AT_WIRE_SERVER_EVENT_PREFIX) - 1U;
    }
    if (!found) return -1;
    if (listener_generation) *listener_generation = (uint32_t)gen;
    return listener_fd;
}

at_socket_errno_t at_socket_bind(int fd, uint32_t addr, uint16_t port)
{
    socket_meta_t slot; int idx = get_slot(fd, &slot);
    if (idx < 0 || addr == UINT32_MAX) return fail_with(AT_SOCKET_ERRNO_INVALID);
    if (slot.type == AT_SOCKET_TYPE_STREAM) {
        if (!state_lock()) return fail_with(AT_SOCKET_ERRNO_BUSY);
        if (s_slots[idx].state != SLOT_CREATED) { state_unlock(); return fail_with(AT_SOCKET_ERRNO_INVALID); }
        /* AT assigns the local IP/port inside CIPSTART. A bind to the active
         * interface with port 0 is therefore a valid no-op for outbound TCP. */
        if (port) s_slots[idx].local_port = port;
        state_unlock(); return AT_SOCKET_ERRNO_NONE;
    }
    if (slot.type != AT_SOCKET_TYPE_DATAGRAM) return fail_with(AT_SOCKET_ERRNO_ADDRESS_IN_USE);
    if (port == 0U) return AT_SOCKET_ERRNO_NONE;
    if (!state_lock()) return fail_with(AT_SOCKET_ERRNO_BUSY);
    if (s_slots[idx].generation != slot.generation || s_slots[idx].state != SLOT_CREATED) {
        state_unlock(); return fail_with(AT_SOCKET_ERRNO_ADDRESS_IN_USE);
    }
    s_slots[idx].state = SLOT_STARTING_LISTENER; s_slots[idx].local_port = port;
    state_unlock();
    uint32_t listener_gen = 0;
    int raw = start_server(AT_SOCKET_TYPE_DATAGRAM, port, &listener_gen);
    if (raw < 0) {
        if (state_lock()) { if (s_slots[idx].generation == slot.generation) s_slots[idx].state = SLOT_CREATED; state_unlock(); }
        return fail_with(AT_SOCKET_ERRNO_ADDRESS_IN_USE);
    }
    if (!state_lock()) return fail_with(AT_SOCKET_ERRNO_BUSY);
    if (s_slots[idx].generation != slot.generation || s_slots[idx].state != SLOT_STARTING_LISTENER) {
        state_unlock(); return fail_with(AT_SOCKET_ERRNO_CONNECTION_RESET);
    }
    s_slots[idx].state = SLOT_LISTENER; s_slots[idx].lwip_fd = raw; s_slots[idx].local_port = port;
    s_slots[idx].listener_generation = listener_gen;
    replay_pending_accepts_locked();
    replay_pending_datagrams_locked(&s_slots[idx], raw);
    state_unlock(); wake_waiters(); return AT_SOCKET_ERRNO_NONE;
}

at_socket_errno_t at_socket_listen(int fd, int backlog)
{
    socket_meta_t slot; int idx = get_slot(fd, &slot);
    if (idx < 0 || slot.type != AT_SOCKET_TYPE_STREAM || slot.state != SLOT_CREATED || !slot.local_port || backlog < 1)
        return fail_with(AT_SOCKET_ERRNO_INVALID);
    if (!state_lock()) return fail_with(AT_SOCKET_ERRNO_BUSY);
    if (s_slots[idx].generation != slot.generation || s_slots[idx].state != SLOT_CREATED) {
        state_unlock(); return fail_with(AT_SOCKET_ERRNO_BUSY);
    }
    for (unsigned i = 0; i < SOCKET_COUNT; ++i)
        if (i != (unsigned)idx && s_slots[i].type == AT_SOCKET_TYPE_STREAM &&
            (s_slots[i].state == SLOT_STARTING_LISTENER || s_slots[i].state == SLOT_LISTENER) &&
            s_slots[i].local_port == slot.local_port) {
            state_unlock(); return fail_with(AT_SOCKET_ERRNO_ADDRESS_IN_USE);
        }
    s_slots[idx].state = SLOT_STARTING_LISTENER;
    state_unlock();
    uint32_t listener_gen = 0;
    int raw = start_server(AT_SOCKET_TYPE_STREAM, slot.local_port, &listener_gen);
    if (raw < 0) {
        if (state_lock()) { if (s_slots[idx].generation == slot.generation) s_slots[idx].state = SLOT_CREATED; state_unlock(); }
        return fail_with(AT_SOCKET_ERRNO_ADDRESS_IN_USE);
    }
    if (!state_lock()) return fail_with(AT_SOCKET_ERRNO_BUSY);
    if (s_slots[idx].generation != slot.generation || s_slots[idx].state != SLOT_STARTING_LISTENER) {
        state_unlock(); return fail_with(AT_SOCKET_ERRNO_CONNECTION_RESET);
    }
    s_slots[idx].state = SLOT_LISTENER; s_slots[idx].lwip_fd = raw;
    s_slots[idx].listener_generation = listener_gen;
    replay_pending_accepts_locked();
    state_unlock(); wake_waiters(); return AT_SOCKET_ERRNO_NONE;
}

int at_socket_accept(int fd, uint32_t *addr, uint16_t *port)
{
    socket_meta_t snapshot; int idx = get_slot(fd, &snapshot);
    if (idx < 0 || snapshot.state != SLOT_LISTENER || snapshot.type != AT_SOCKET_TYPE_STREAM) return fail_with(AT_SOCKET_ERRNO_INVALID);
    uint32_t start = at_socket_now_ms();
    for (;;) {
        if (!state_lock()) return fail_with(AT_SOCKET_ERRNO_BUSY);
        socket_slot_t *listener = &s_slots[idx];
        if (listener->accepted_count) {
            unsigned at = listener->accepted_head; int child = listener->accepted[at];
            if (addr) *addr = listener->accepted_addr[at];
            if (port) *port = listener->accepted_port[at];
            listener->accepted_head = (uint8_t)((listener->accepted_head + 1U) % UDP_QUEUE_DEPTH);
            --listener->accepted_count; state_unlock(); return child;
        }
        bool nonblock = listener->nonblock; int timeout = listener->recv_timeout;
        state_unlock();
        if (nonblock) return fail_with(AT_SOCKET_ERRNO_AGAIN);
        if (timeout >= 0 && (uint32_t)(at_socket_now_ms() - start) >= (uint32_t)timeout)
            return fail_with(AT_SOCKET_ERRNO_TIMEOUT);
        (void)xSemaphoreTake(s_ready, pdMS_TO_TICKS(10U));
    }
}
int at_socket_recv(int fd, void *buf, uint32_t len)
{
    socket_meta_t slot; int idx = get_slot(fd, &slot); if (idx < 0) return fail_with(AT_SOCKET_ERRNO_BAD_FD);
    if (!len) return 0;
    if (!buf) return fail_with(AT_SOCKET_ERRNO_INVALID);
    if (slot.type == AT_SOCKET_TYPE_DATAGRAM) return at_socket_recvfrom(fd, buf, len, NULL, NULL);
    if (slot.state == SLOT_LISTENER) return fail_with(AT_SOCKET_ERRNO_INVALID);
    uint32_t start = at_socket_now_ms();
    for (;;) {
        bool fetch = false;
        int fetch_raw = -1;
        uint32_t fetch_length = 0U;
        if (!state_lock()) return fail_with(AT_SOCKET_ERRNO_BUSY);
        socket_slot_t *current = &s_slots[idx];
        if (current->rx.size) {
            size_t take = current->rx.size < len ? current->rx.size : len;
            take = rx_queue_read_locked(&current->rx, buf, take);
            size_t buffered = current->rx.size;
            state_unlock();
            uint32_t recv_elapsed = at_socket_now_ms() - start;
            if (recv_elapsed >= 500U) {
                AT_SUMMARY("slow recv fd=%d raw=%d got=%lu elapsed=%lums buffered=%lu", fd,
                              slot.remote_id, (unsigned long)take, (unsigned long)recv_elapsed,
                              (unsigned long)buffered);
            }
            log_rx_preview(fd, buf, take);
            return (int)take;
        }
        if (current->state == SLOT_CONNECTED && current->rx_ready && !current->rx_fetching) {
            current->rx_ready = false;
            current->rx_fetching = true;
            fetch = true;
            fetch_raw = current->lwip_fd;
            /* TLS first asks for a 5-byte record header. Pull a full bounded
             * chunk so that header and body reads share one VW553 round trip. */
            fetch_length = STREAM_RX_FETCH_SIZE;
        }
        slot_state_t state = current->state; bool nonblock = current->nonblock; int timeout = current->recv_timeout;
        state_unlock();
        if (fetch) {
            char command[48];
            const int command_length = snprintf(command, sizeof(command), AT_WIRE_RECVDATA_FMT,
                                                fetch_raw, (unsigned)fetch_length);
            uint32_t watermark = 0U;
            at_status_t status = AT_STATUS_IO_ERROR;
            if (command_length > 0 && (size_t)command_length < sizeof(command)) {
                status = run_at_with_timeout(command, NULL, 0U, false, NULL, 0U, NULL,
                                             &watermark, RPC_TIMEOUT_MS);
            }
            if (status == AT_STATUS_OK && watermark) {
                status = at_device_wait_events(watermark, RPC_TIMEOUT_MS);
            }
            if (!state_lock()) return fail_with(AT_SOCKET_ERRNO_BUSY);
            current = &s_slots[idx];
            current->rx_fetching = false;
            bool first_fetch = false;
            if (status == AT_STATUS_OK) {
                ++current->rx_fetches;
                first_fetch = current->rx_fetches == 1U;
            }
            if (status != AT_STATUS_OK && current->state == SLOT_CONNECTED) {
                /* The module clears its own ready latch while servicing the
                 * request. The next select pass will notify us again if the
                 * TCP receive queue is still non-empty. */
                current->rx_ready = false;
            }
            state_unlock();
            if (status == AT_STATUS_OK) {
                if (first_fetch) {
                    AT_SUMMARY("socket on-demand fetch fd=%d raw=%d request=%u", fd, fetch_raw,
                               (unsigned)fetch_length);
                }
                AT_LOG("socket recv fetched fd=%d raw=%d request=%u", fd, fetch_raw,
                          (unsigned)fetch_length);
                continue;
            }
            AT_LOG("socket recv fetch failed fd=%d raw=%d request=%u status=%ld", fd, fetch_raw,
                      (unsigned)fetch_length, (long)status);
            if (nonblock) return fail_with(AT_SOCKET_ERRNO_AGAIN);
            (void)xSemaphoreTake(s_ready, pdMS_TO_TICKS(10U));
            continue;
        }
        if (!nonblock && timeout < 0) {
            /* Never block forever: the TLS read after a lost ServerHello used
             * to freeze the whole activation flow until the App tore the
             * session down, with no AT traffic and no retry in between. */
            timeout = (int)RPC_TIMEOUT_MS;
        }
        if (state == SLOT_EOF) return 0;
        if (state == SLOT_FAILED) {
            AT_LOG("socket recv failed fd=%d raw=%d buffered=%lu", fd, slot.remote_id,
                      (unsigned long)slot.stream_size);
            return fail_with(AT_SOCKET_ERRNO_CONNECTION_RESET);
        }
        if (nonblock) return fail_with(AT_SOCKET_ERRNO_AGAIN);
        if (timeout >= 0 && (uint32_t)(at_socket_now_ms() - start) >= (uint32_t)timeout) {
            AT_SUMMARY("socket recv timeout fd=%d raw=%d state=%d buffered=%lu rx_events=%lu rx_bytes=%lu timeout=%d elapsed=%lums",
                          fd, slot.remote_id, (int)state, (unsigned long)slot.stream_size,
                          (unsigned long)slot.rx_events, (unsigned long)slot.rx_bytes, timeout,
                          (unsigned long)(at_socket_now_ms() - start));
            int raw = query_remote_fd(slot.remote_id, NULL);
            if (raw == -2) return fail_with(AT_SOCKET_ERRNO_CONNECTION_RESET);
            if (!state_lock()) return fail_with(AT_SOCKET_ERRNO_BUSY);
            if (raw < 0) s_slots[idx].state = SLOT_EOF;
            state_unlock();
            if (raw < 0) { wake_waiters(); return 0; }
            return fail_with(AT_SOCKET_ERRNO_TIMEOUT);
        }
        (void)xSemaphoreTake(s_ready, pdMS_TO_TICKS(10U));
    }
}
int at_socket_recv_nd_size(int fd, void *buf, uint32_t size, uint32_t needed)
{ if (needed > size) return -1; uint32_t received = 0; while (received < needed) { int n = at_socket_recv(fd, (uint8_t *)buf + received, needed - received); if (n <= 0) return -1; received += (uint32_t)n; } return (int)received; }
int at_socket_recvfrom(int fd, void *buf, uint32_t len, uint32_t *addr, uint16_t *port)
{
    if (!buf && len) return fail_with(AT_SOCKET_ERRNO_INVALID);
    socket_meta_t meta; int idx = get_slot(fd, &meta);
    if (idx < 0) return fail_with(AT_SOCKET_ERRNO_BAD_FD);
    if (meta.type != AT_SOCKET_TYPE_DATAGRAM) return fail_with(AT_SOCKET_ERRNO_INVALID);
    if (!len) return 0;
    uint32_t start = at_socket_now_ms();
    for (;;) {
        if (!state_lock()) return fail_with(AT_SOCKET_ERRNO_BUSY);
        socket_slot_t *slot = &s_slots[idx];
        if (slot->rx.completed_datagrams) {
            size_t n = rx_datagram_read_locked(&slot->rx, buf, len, addr, port);
            state_unlock(); return (int)n;
        }
        bool nonblock = slot->nonblock; int timeout = slot->recv_timeout; slot_state_t state = slot->state;
        state_unlock();
        if (!nonblock && timeout < 0) timeout = (int)RPC_TIMEOUT_MS;
        if (state == SLOT_FAILED) return fail_with(AT_SOCKET_ERRNO_NO_BUFFER);
        if (nonblock) return fail_with(AT_SOCKET_ERRNO_AGAIN);
        if (timeout >= 0 && (uint32_t)(at_socket_now_ms() - start) >= (uint32_t)timeout) return fail_with(AT_SOCKET_ERRNO_TIMEOUT);
        (void)xSemaphoreTake(s_ready, pdMS_TO_TICKS(10U));
    }
}

static bool dns_cache_lookup(const char *host, uint32_t *addr)
{
    uint32_t now = at_socket_now_ms();
    bool hit = false;
    if (!state_lock()) return false;
    for (unsigned i = 0U; i < DNS_CACHE_SIZE; ++i) {
        dns_cache_entry_t *entry = &s_dns_cache[i];
        if (!entry->used) continue;
        if ((int32_t)(now - entry->expires_ms) >= 0) {
            entry->used = false;
            continue;
        }
        if (strcmp(entry->host, host) == 0) {
            entry->last_used_ms = now;
            *addr = entry->addr;
            hit = true;
            break;
        }
    }
    state_unlock();
    return hit;
}

static void dns_cache_store(const char *host, uint32_t addr)
{
    size_t len = strlen(host);
    if (len == 0U || len >= DNS_CACHE_HOST_SIZE) return;
    uint32_t now = at_socket_now_ms();
    if (!state_lock()) return;
    int victim = -1;
    int lru = -1;
    for (unsigned i = 0U; i < DNS_CACHE_SIZE; ++i) {
        dns_cache_entry_t *entry = &s_dns_cache[i];
        if (entry->used && strcmp(entry->host, host) == 0) {
            victim = (int)i;
            break;
        }
        if (!entry->used || (int32_t)(now - entry->expires_ms) >= 0) {
            victim = (int)i;
            break;
        }
        if (lru < 0 || (int32_t)(entry->last_used_ms - s_dns_cache[lru].last_used_ms) < 0)
            lru = (int)i;
    }
    if (victim < 0) victim = lru >= 0 ? lru : 0;
    dns_cache_entry_t *entry = &s_dns_cache[victim];
    memset(entry, 0, sizeof(*entry));
    entry->used = true;
    memcpy(entry->host, host, len);
    entry->host[len] = '\0';
    entry->addr = addr;
    entry->expires_ms = now + DNS_CACHE_TTL_MS;
    entry->last_used_ms = now;
    state_unlock();
}

void at_socket_dns_cache_flush(void)
{
    if (!s_state_mutex) return;
    if (!state_lock()) return;
    memset(s_dns_cache, 0, sizeof(s_dns_cache));
    state_unlock();
}

at_status_t at_socket_gethostbyname(const char *domain, uint32_t *addr)
{
    if (s_wifi_join_active) {
        s_last_errno = AT_SOCKET_ERRNO_NETWORK_DOWN;
        return AT_STATUS_IO_ERROR;
    }
    if (!domain || !addr || !*domain || strlen(domain) > 253U) return AT_STATUS_INVALID_PARAM;
    int32_t init_status = at_socket_init();
    if (init_status != AT_STATUS_OK) {
        s_last_errno = AT_SOCKET_ERRNO_NETWORK_DOWN;
        return init_status;
    }
    if (dns_cache_lookup(domain, addr)) {
        AT_LOG("DNS %s cache hit", domain);
        return AT_STATUS_OK;
    }
    bool previous_dot = true;
    size_t label_length = 0;
    for (const unsigned char *p = (const unsigned char *)domain; *p; ++p) {
        if (*p == '.') {
            if (previous_dot || label_length == 0U || label_length > 63U) return AT_STATUS_INVALID_PARAM;
            previous_dot = true; label_length = 0U;
        } else if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                   (*p >= '0' && *p <= '9') || *p == '-') {
            if (previous_dot && *p == '-') return AT_STATUS_INVALID_PARAM;
            previous_dot = false; ++label_length;
        } else return AT_STATUS_INVALID_PARAM;
    }
    if (previous_dot || label_length == 0U || label_length > 63U) return AT_STATUS_INVALID_PARAM;
    char command[300], reply[128];
    if (snprintf(command, sizeof(command), "AT+CIPDOMAIN=\"%s\"", domain) >= (int)sizeof(command)) return AT_STATUS_INVALID_PARAM;
    for (unsigned attempt = 1U; attempt <= DNS_RETRY_COUNT; ++attempt) {
        size_t len = 0;
        memset(reply, 0, sizeof(reply));
        uint32_t timeout = attempt == 1U ? DNS_TIMEOUT_MS : DNS_RETRY_TIMEOUT_MS;
        uint32_t dns_start = at_socket_now_ms();
        int32_t status = run_at_with_timeout(command, NULL, 0, false, reply, sizeof(reply), &len, NULL,
                                             timeout);
        uint32_t dns_elapsed = at_socket_now_ms() - dns_start;
        AT_SUMMARY("DNS %s attempt=%u/%u elapsed=%lums status=%ld reply_len=%lu", domain,
                      attempt, DNS_RETRY_COUNT, (unsigned long)dns_elapsed, (long)status,
                      (unsigned long)len);
        if (status == AT_STATUS_OK) {
            const char *p = strstr(reply, "+CIPDOMAIN:<\"");
            if (p) {
                p += strlen("+CIPDOMAIN:<\"");
                char ip[16]; size_t i = 0;
                while (p[i] && p[i] != '"' && i + 1U < sizeof(ip)) { ip[i] = p[i]; ++i; }
                ip[i] = '\0';
                if (parse_ipv4(ip, addr)) {
                    dns_cache_store(domain, *addr);
                    return AT_STATUS_OK;
                }
            }
            AT_SUMMARY("DNS %s returned an invalid reply", domain);
        }
        /* The AT resolver occasionally returns ERROR immediately while its
         * lwIP stack is still settling. A fresh AT request normally succeeds,
         * so do not surface that transient failure to the cloud stack. */
        if (attempt < DNS_RETRY_COUNT) at_socket_sleep(DNS_RETRY_DELAY_MS);
    }
    return AT_STATUS_IO_ERROR;
}
at_status_t at_socket_set_timeout(int fd, int timeout, at_socket_transfer_t type)
{ if (timeout < 0 || !state_lock()) return AT_STATUS_INVALID_PARAM; int idx = find_logical(fd); if (idx < 0) { state_unlock(); return AT_STATUS_INVALID_PARAM; } if (type == AT_SOCKET_TRANSFER_RECV) s_slots[idx].recv_timeout = timeout; else if (type == AT_SOCKET_TRANSFER_SEND) s_slots[idx].send_timeout = timeout; else { state_unlock(); return AT_STATUS_INVALID_PARAM; } state_unlock(); return AT_STATUS_OK; }
at_status_t at_socket_set_bufsize(int fd, int size, at_socket_transfer_t type)
{ (void)type; if (size <= 0) return AT_STATUS_INVALID_PARAM; socket_meta_t slot; return get_slot(fd, &slot) >= 0 && size <= (int)RX_RING_SIZE ? AT_STATUS_OK : AT_STATUS_INVALID_PARAM; }
at_status_t at_socket_set_broadcast(int fd)
{ if (!state_lock()) return AT_STATUS_IO_ERROR; int idx = find_logical(fd); if (idx < 0 || s_slots[idx].type != AT_SOCKET_TYPE_DATAGRAM) { state_unlock(); return AT_STATUS_INVALID_PARAM; } s_slots[idx].broadcast = true; state_unlock(); return AT_STATUS_OK; }
static at_status_t tcp_options_rpc(const socket_meta_t *slot,
                                   const at_socket_tcp_options_t *desired,
                                   at_socket_tcp_options_t *actual)
{
    char command[128], reply[192] = {0};
    size_t length = 0;
    if (!slot->remote_generation) {
        AT_SUMMARY("TCP options require CIPSTATE generation; update VW553 firmware");
        return AT_STATUS_UNSUPPORTED;
    }
    if (desired) {
        (void)snprintf(command, sizeof(command), "AT+CIPTCPOPT=%d,%lu,%d,%d,%d,%d,%d",
                       slot->remote_id, (unsigned long)slot->remote_generation,
                       desired->nodelay, desired->alive, desired->idle, desired->interval, desired->count);
    } else {
        (void)snprintf(command, sizeof(command), "AT+CIPTCPOPT=%d,%lu",
                       slot->remote_id, (unsigned long)slot->remote_generation);
    }
    at_status_t status = run_at(command, NULL, 0, false, reply, sizeof(reply), &length, NULL);
    if (status != AT_STATUS_OK) return status;
    const char *line = strstr(reply, "+CIPTCPOPT:");
    AT_SUMMARY("TCP option readback con=%d generation=%lu bytes=%lu line=%.*s", slot->remote_id,
               (unsigned long)slot->remote_generation, (unsigned long)length,
               line ? (int)strcspn(line, "\r\n") : 0, line ? line : "");
    int id; unsigned generation;
    if (!line || sscanf(line, "+CIPTCPOPT:%d,%u,%d,%d,%d,%d,%d", &id, &generation,
                        &actual->nodelay, &actual->alive, &actual->idle,
                        &actual->interval, &actual->count) != 7 ||
        id != slot->remote_id || generation != slot->remote_generation) return AT_STATUS_IO_ERROR;
    if (desired && (actual->nodelay != desired->nodelay || actual->alive != desired->alive ||
        actual->idle != desired->idle || actual->interval != desired->interval ||
        actual->count != desired->count)) return AT_STATUS_IO_ERROR;
    return AT_STATUS_OK;
}

at_status_t at_socket_get_tcp_options(int fd, at_socket_tcp_options_t *options)
{
    socket_meta_t slot;
    if (!options || get_slot(fd, &slot) < 0 || slot.type != AT_SOCKET_TYPE_STREAM)
        return AT_STATUS_INVALID_PARAM;
    if (slot.state == SLOT_CREATED) { *options = slot.tcp_options; return AT_STATUS_OK; }
    if (slot.state != SLOT_CONNECTED) return AT_STATUS_NOT_READY;
    return tcp_options_rpc(&slot, NULL, options);
}

static at_status_t set_tcp_options(int fd, const at_socket_tcp_options_t *options)
{
    socket_meta_t slot;
    int idx = get_slot(fd, &slot);
    if (idx < 0 || slot.type != AT_SOCKET_TYPE_STREAM) return AT_STATUS_INVALID_PARAM;
    if (slot.state == SLOT_CONNECTED) {
        at_socket_tcp_options_t actual;
        at_status_t status = tcp_options_rpc(&slot, options, &actual);
        if (status != AT_STATUS_OK) return status;
        AT_SUMMARY("TCP options verified fd=%d nodelay=%d alive=%d idle=%d interval=%d count=%d",
                   fd, actual.nodelay, actual.alive, actual.idle, actual.interval, actual.count);
    } else if (slot.state != SLOT_CREATED) return AT_STATUS_NOT_READY;
    if (!state_lock()) return AT_STATUS_BUSY;
    if (s_slots[idx].generation != slot.generation || s_slots[idx].state != slot.state) {
        state_unlock(); return AT_STATUS_NOT_READY;
    }
    s_slots[idx].tcp_options = *options;
    s_slots[idx].tcp_options_configured = true;
    state_unlock();
    return AT_STATUS_OK;
}

at_status_t at_socket_disable_nagle(int fd)
{
    at_socket_tcp_options_t options;
    at_status_t status = at_socket_get_tcp_options(fd, &options);
    if (status != AT_STATUS_OK) return status;
    options.nodelay = 1;
    return set_tcp_options(fd, &options);
}

at_status_t at_socket_set_keepalive(int fd, bool alive, uint32_t idle, uint32_t interval, uint32_t count)
{
    if (alive && (!idle || idle > INT_MAX / 1000U || !interval ||
                  interval > INT_MAX / 1000U || !count || count > 127U)) return AT_STATUS_INVALID_PARAM;
    at_socket_tcp_options_t options;
    at_status_t status = at_socket_get_tcp_options(fd, &options);
    if (status != AT_STATUS_OK) return status;
    options.alive = alive ? 1 : 0;
    if (alive) { options.idle = (int)idle; options.interval = (int)interval; options.count = (int)count; }
    return set_tcp_options(fd, &options);
}
at_status_t at_socket_getsockname(int fd, uint32_t *addr, uint16_t *port)
{ if (!addr || !port) return AT_STATUS_INVALID_PARAM; socket_meta_t slot; if (get_slot(fd, &slot) < 0) return AT_STATUS_INVALID_PARAM; *addr = 0U; *port = (slot.state == SLOT_LISTENER) ? slot.local_port : 0U; return AT_STATUS_OK; }
at_status_t at_socket_getpeername(int fd, uint32_t *addr, uint16_t *port)
{ if (!addr || !port) return AT_STATUS_INVALID_PARAM; socket_meta_t slot; if (get_slot(fd, &slot) < 0 || slot.state != SLOT_CONNECTED) return AT_STATUS_INVALID_PARAM; *addr = slot.peer_addr; *port = slot.peer_port; return AT_STATUS_OK; }

