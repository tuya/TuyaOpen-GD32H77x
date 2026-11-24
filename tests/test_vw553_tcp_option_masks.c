#include <assert.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include "gateway_tcp_options.h"

static gateway_tcp_options_t state = {1, 0, 60, 1, 3};
static int fail_count_once;
static int *field(int level, int name)
{
    if (level == SOL_SOCKET && name == SO_KEEPALIVE) return &state.alive;
    if (level == IPPROTO_TCP && name == TCP_NODELAY) return &state.nodelay;
    if (level == IPPROTO_TCP && name == TCP_KEEPIDLE) return &state.idle;
    if (level == IPPROTO_TCP && name == TCP_KEEPINTVL) return &state.interval;
    if (level == IPPROTO_TCP && name == TCP_KEEPCNT) return &state.count;
    return NULL;
}
static int fake_get(int fd, int level, int name, void *value, socklen_t *length)
{
    if (fd != 3 || *length != sizeof(int)) return -1;
    if (level == SOL_SOCKET && name == SO_TYPE) { *(int *)value = SOCK_STREAM; return 0; }
    int *source = field(level, name);
    if (!source) return -1;
    *(int *)value = *source;
    if (name == TCP_NODELAY && level == IPPROTO_TCP) *(int *)value *= 64;
    if (name == SO_KEEPALIVE && level == SOL_SOCKET) *(int *)value *= 8;
    return 0;
}
static int fake_set(int fd, int level, int name, const void *value, socklen_t length)
{
    if (fd != 3 || length != sizeof(int)) return -1;
    int *target = field(level, name);
    if (!target) return -1;
    if (name == TCP_KEEPCNT && fail_count_once) { fail_count_once = 0; return -1; }
    *target = *(const int *)value;
    return 0;
}
#define getsockopt fake_get
#define setsockopt fake_set
#define GATEWAY_TCP_OPTIONS_HOST_TEST 1
#include GATEWAY_TCP_OPTIONS_SOURCE

static int tiny_print(char *buffer, int capacity, const char *format, ...)
{
    assert(strchr(format, 'l') == NULL);
    va_list args;
    va_start(args, format);
    int result = vsnprintf(buffer, (size_t)capacity, format, args);
    va_end(args);
    return result;
}

int main(void)
{
    gateway_tcp_options_t desired = {1, 1, 100, 5, 1}, actual;
    char reply[128];
    assert(gateway_tcp_options_format(tiny_print, reply, sizeof(reply), 3, UINT_MAX, &desired) > 0);
    assert(strcmp(reply, "+CIPTCPOPT:3,4294967295,1,1,100,5,1\r\n") == 0);
    assert(gateway_tcp_options_format(tiny_print, reply, 4, 3, 17, &desired) < 0);
    assert(gateway_tcp_options_set(3, &desired) == 0);
    assert(gateway_tcp_options_get(3, &actual) == 0);
    assert(actual.nodelay == 1 && actual.alive == 1);
    fail_count_once = 1;
    desired.idle = 200; desired.interval = 10; desired.count = 2;
    assert(gateway_tcp_options_set(3, &desired) != 0);
    assert(gateway_tcp_options_get(3, &actual) == 0);
    assert(actual.idle == 100 && actual.interval == 5 && actual.count == 1 && actual.alive == 1);
    return 0;
}
