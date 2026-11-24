#include "gateway_tcp_options.h"
#include <assert.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>

int main(void)
{
    int tcp = socket(AF_INET, SOCK_STREAM, 0);
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    assert(tcp >= 0 && udp >= 0);
    gateway_tcp_options_t wanted = {1, 1, 100, 5, 1}, actual;
    assert(gateway_tcp_options_set(tcp, &wanted) == 0);
    assert(gateway_tcp_options_get(tcp, &actual) == 0);
    assert(actual.nodelay == 1 && actual.alive == 1 && actual.idle == 100 && actual.interval == 5 && actual.count == 1);
    wanted.alive = 0;
    assert(gateway_tcp_options_set(tcp, &wanted) == 0);
    assert(gateway_tcp_options_get(tcp, &actual) == 0 && actual.alive == 0);
    wanted.alive = 1; wanted.idle = 101; wanted.interval = 7; wanted.count = 2;
    assert(gateway_tcp_options_set(tcp, &wanted) == 0);
    wanted.interval = 0;
    assert(gateway_tcp_options_set(tcp, &wanted) != 0);
    assert(gateway_tcp_options_get(tcp, &actual) == 0);
    assert(actual.alive == 1 && actual.idle == 101 && actual.interval == 7 && actual.count == 2);
    assert(gateway_tcp_options_get(udp, &actual) != 0);
    assert(gateway_tcp_options_get(-1, &actual) != 0);
    close(tcp); close(udp);
    assert(gateway_tcp_options_get(tcp, &actual) != 0);
    puts("VW553 real TCP option set/readback checks passed");
    return 0;
}
