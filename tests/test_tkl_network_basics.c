/**
 * @file test_tkl_network_basics.c
 * @brief Verify the standard TKL fd-set and IPv4 conversion semantics.
 */

#include "tkl_network.h"
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

int main(void)
{
    fd_set_bounds_and_bitmap();
    strict_ipv4_host_order();
    puts("TKL network common semantics checks passed");
    return 0;
}
