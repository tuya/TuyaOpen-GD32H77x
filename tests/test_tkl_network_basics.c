/**
 * @file test_tkl_network_basics.c
 * @brief Verify the standard TKL fd-set and IPv4 conversion semantics.
 */

#include "tkl_network.h"
#include "at_socket.h"
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t before[8];
    TUYA_FD_SET_T set;
    uint8_t after[8];
} guarded_fd_set_t;

static int fake_send_result;
static int fake_recv_result;
static at_status_t fake_nagle_status;

at_status_t at_socket_disable_nagle(int fd)
{
    assert(fd == 3);
    return fake_nagle_status;
}

at_status_t at_socket_set_keepalive(int fd, bool alive, uint32_t idle, uint32_t interval, uint32_t count)
{
    assert(fd == 3 && alive && idle == 100 && interval == 5 && count == 1);
    return fake_nagle_status;
}

int at_socket_send(int fd, const void *buf, uint32_t length)
{
    (void)fd;
    (void)buf;
    (void)length;
    return fake_send_result;
}

int at_socket_sendto(int fd, const void *buf, uint32_t length, uint32_t addr, uint16_t port)
{
    (void)fd;
    (void)buf;
    (void)length;
    (void)addr;
    (void)port;
    return fake_send_result;
}

int at_socket_recv(int fd, void *buf, uint32_t length)
{
    (void)fd;
    (void)buf;
    (void)length;
    return fake_recv_result;
}

int at_socket_recvfrom(int fd, void *buf, uint32_t length, uint32_t *addr, uint16_t *port)
{
    (void)fd;
    (void)buf;
    (void)length;
    (void)addr;
    (void)port;
    return fake_recv_result;
}

at_socket_errno_t at_socket_get_errno(void)
{
    return AT_SOCKET_ERRNO_NONE;
}

static void fd_set_bounds_and_bitmap(void)
{
    guarded_fd_set_t guarded;
    memset(&guarded, 0xA5, sizeof(guarded));
    assert(tkl_net_fd_zero(&guarded.set) == OPRT_OK);
    for (size_t i = 0; i < sizeof(guarded.before); ++i) assert(guarded.before[i] == 0xA5);
    for (size_t i = 0; i < sizeof(guarded.after); ++i) assert(guarded.after[i] == 0xA5);

    const int fds[] = {0, 7, 8, TUYA_FD_MAX_COUNT - 1};
    for (size_t i = 0; i < sizeof(fds) / sizeof(fds[0]); ++i) {
        assert(tkl_net_fd_isset(fds[i], &guarded.set) == 0);
        assert(tkl_net_fd_set(fds[i], &guarded.set) == OPRT_OK);
        assert(tkl_net_fd_isset(fds[i], &guarded.set) == 1);
        assert(tkl_net_fd_set(fds[i], &guarded.set) == OPRT_OK);
    }
    assert(tkl_net_fd_clear(7, &guarded.set) == OPRT_OK);
    assert(tkl_net_fd_isset(7, &guarded.set) == 0);
    assert(tkl_net_fd_isset(8, &guarded.set) == 1);
    for (size_t i = 0; i < sizeof(guarded.before); ++i) assert(guarded.before[i] == 0xA5);
    for (size_t i = 0; i < sizeof(guarded.after); ++i) assert(guarded.after[i] == 0xA5);

    uint8_t snapshot[sizeof(guarded.set)];
    memcpy(snapshot, &guarded.set, sizeof(snapshot));
    const int invalid[] = {-1, TUYA_FD_MAX_COUNT, INT_MAX};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(tkl_net_fd_set(invalid[i], &guarded.set) == OPRT_INVALID_PARM);
        assert(tkl_net_fd_clear(invalid[i], &guarded.set) == OPRT_INVALID_PARM);
        assert(tkl_net_fd_isset(invalid[i], &guarded.set) == OPRT_INVALID_PARM);
        assert(memcmp(snapshot, &guarded.set, sizeof(snapshot)) == 0);
    }
    assert(tkl_net_fd_set(0, NULL) == OPRT_INVALID_PARM);
    assert(tkl_net_fd_clear(0, NULL) == OPRT_INVALID_PARM);
    assert(tkl_net_fd_isset(0, NULL) == OPRT_INVALID_PARM);
    assert(tkl_net_fd_zero(NULL) == OPRT_INVALID_PARM);
}

static void strict_ipv4_host_order(void)
{
    assert(tkl_net_str2addr("192.168.1.1") == UINT32_C(0xC0A80101));
    assert(tkl_net_str2addr("0.0.0.0") == UINT32_C(0));
    assert(tkl_net_str2addr("255.255.255.255") == UINT32_MAX);
    static const char *invalid[] = {
        "", "1", "1.2", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.999", "01.2.3.4",
        "1.02.3.4", "1.2.3.04", "1..3.4", ".1.2.3.4", "1.2.3.4.", "1.2.3.4x", "1.2.3.4 ",
        " 1.2.3.4", "1.2.3.-1", "1.2.3.999999999999999999999", "::1"
    };
    assert(tkl_net_str2addr(NULL) == UINT32_MAX);
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(tkl_net_str2addr(invalid[i]) == UINT32_MAX);
    }
}

static void send_length_passthrough(void)
{
    uint8_t payload[2048] = {0};
    for (int length = 127; length <= 129; ++length) {
        fake_send_result = length;
        assert(tkl_net_send(3, payload, (uint32_t)length) == length);
        assert(tkl_net_send_to(3, payload, (uint32_t)length, 0, 443) == length);
    }
    fake_send_result = 2048;
    assert(tkl_net_send(3, payload, sizeof(payload)) == (int)sizeof(payload));
}

static void recv_length_passthrough(void)
{
    uint8_t payload[2048] = {0};
    for (int length = 127; length <= 129; ++length) {
        fake_recv_result = length;
        assert(tkl_net_recv(3, payload, (uint32_t)length) == length);
        assert(tkl_net_recvfrom(3, payload, (uint32_t)length, NULL, NULL) == length);
    }
    fake_recv_result = 2048;
    assert(tkl_net_recv(3, payload, sizeof(payload)) == (int)sizeof(payload));

    fake_recv_result = 0;
    assert(tkl_net_recv(3, NULL, 0U) == 0);
    assert(tkl_net_recvfrom(3, NULL, 0U, NULL, NULL) == 0);
}

int main(void)
{
    fake_nagle_status = AT_STATUS_OK;
    assert(tkl_net_disable_nagle(3) == OPRT_OK);
    assert(tkl_net_set_keepalive(3, TRUE, 100, 5, 1) == OPRT_OK);
    fake_nagle_status = AT_STATUS_INVALID_PARAM;
    assert(tkl_net_disable_nagle(3) == OPRT_INVALID_PARM);
    assert(tkl_net_set_keepalive(3, TRUE, 100, 5, 1) == OPRT_INVALID_PARM);
    fd_set_bounds_and_bitmap();
    strict_ipv4_host_order();
    send_length_passthrough();
    recv_length_passthrough();
    puts("TKL network common semantics checks passed");
    return 0;
}
