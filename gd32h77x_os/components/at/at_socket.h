/**
 * @file at_socket.h
 * @brief Transport-independent socket semantics over the AT device hub.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */
#ifndef AT_SOCKET_H
#define AT_SOCKET_H

#include "at_device.h"
#include "at_status.h"

#include <stdbool.h>
#include <stdint.h>

#define AT_SOCKET_FD_BASE 3
#define AT_SOCKET_FD_LIMIT 15
#define AT_SOCKET_FD_SET_SIZE ((AT_SOCKET_FD_LIMIT + 8) / 8)

typedef enum {
    AT_SOCKET_TYPE_STREAM,
    AT_SOCKET_TYPE_DATAGRAM,
} at_socket_type_t;

typedef enum {
    AT_SOCKET_TRANSFER_RECV,
    AT_SOCKET_TRANSFER_SEND,
} at_socket_transfer_t;

typedef enum {
    AT_SOCKET_ERRNO_NONE = 0,
    AT_SOCKET_ERRNO_INVALID = -1,
    AT_SOCKET_ERRNO_BUSY = -2,
    AT_SOCKET_ERRNO_BAD_FD = -3,
    AT_SOCKET_ERRNO_NO_FD = -4,
    AT_SOCKET_ERRNO_NO_BUFFER = -5,
    AT_SOCKET_ERRNO_TIMEOUT = -6,
    AT_SOCKET_ERRNO_AGAIN = -7,
    AT_SOCKET_ERRNO_NOT_CONNECTED = -8,
    AT_SOCKET_ERRNO_CONNECTION_RESET = -9,
    AT_SOCKET_ERRNO_CONNECTION_REFUSED = -10,
    AT_SOCKET_ERRNO_ADDRESS_IN_USE = -11,
    AT_SOCKET_ERRNO_MESSAGE_TOO_LARGE = -12,
    AT_SOCKET_ERRNO_NETWORK_DOWN = -13,
    AT_SOCKET_ERRNO_NETWORK_RESET = -14,
    AT_SOCKET_ERRNO_UNSUPPORTED = -15,
} at_socket_errno_t;

typedef struct {
    uint8_t bits[AT_SOCKET_FD_SET_SIZE];
} at_socket_fd_set_t;

at_status_t at_socket_init(void);
bool at_socket_has_active_socket(void);
void at_socket_on_device_event(void *context, const at_device_event_t *event);
at_socket_errno_t at_socket_get_errno(void);

void at_socket_ap_teardown_set(bool active);
bool at_socket_ap_teardown_active(void);
void at_socket_ap_listeners_invalidate(bool invalid);
void at_socket_wifi_join_set(bool active);
bool at_socket_wifi_join_active(void);
void at_socket_dns_cache_flush(void);

int at_socket_select(int maxfd, at_socket_fd_set_t *readfds,
                     at_socket_fd_set_t *writefds,
                     at_socket_fd_set_t *errorfds, uint32_t timeout_ms);
int at_socket_get_nonblock(int fd);
at_status_t at_socket_set_block(int fd, bool block);
at_socket_errno_t at_socket_close(int fd);
at_socket_errno_t at_socket_shutdown(int fd, int how);
int at_socket_create(at_socket_type_t type);
at_socket_errno_t at_socket_connect(int fd, uint32_t addr, uint16_t port);
at_socket_errno_t at_socket_bind(int fd, uint32_t addr, uint16_t port);
at_socket_errno_t at_socket_listen(int fd, int backlog);
int at_socket_accept(int fd, uint32_t *addr, uint16_t *port);
int at_socket_send(int fd, const void *buf, uint32_t length);
int at_socket_sendto(int fd, const void *buf, uint32_t length, uint32_t addr, uint16_t port);
int at_socket_recv(int fd, void *buf, uint32_t length);
int at_socket_recv_nd_size(int fd, void *buf, uint32_t size, uint32_t needed);
int at_socket_recvfrom(int fd, void *buf, uint32_t length, uint32_t *addr, uint16_t *port);
at_status_t at_socket_gethostbyname(const char *domain, uint32_t *addr);
at_status_t at_socket_set_timeout(int fd, int timeout_ms, at_socket_transfer_t type);
at_status_t at_socket_set_bufsize(int fd, int size, at_socket_transfer_t type);
at_status_t at_socket_set_broadcast(int fd);
at_status_t at_socket_getsockname(int fd, uint32_t *addr, uint16_t *port);
at_status_t at_socket_getpeername(int fd, uint32_t *addr, uint16_t *port);

#endif /* AT_SOCKET_H */
