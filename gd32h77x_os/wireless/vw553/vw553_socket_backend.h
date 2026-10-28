/**
 * @file vw553_socket_backend.h
 * @brief Tuya TKL socket facade over the VW553 AT device hub.
 */
#ifndef VW553_SOCKET_BACKEND_H
#define VW553_SOCKET_BACKEND_H

#include "tkl_network.h"
#include "vw553_device.h"

/* Match native socket stacks: standard streams own 0/1/2, sockets start at 3. */
#define VW553_SOCKET_FD_BASE  3U
#define VW553_SOCKET_FD_LIMIT 15U

/* Idempotent. Registers the permanent event subscriber before using the hub. */
int32_t vw553_socket_backend_init(void);
/* True while an AT socket is being opened or is live.  Wi-Fi status polling
 * must not inject commands into the shared UART during an active stream. */
bool vw553_socket_backend_has_active_socket(void);
/* Exported for the device hub's static event fanout. */
void vw553_socket_backend_on_event(void *context, const vw553_device_event_t *event);
/* Only a firmware close-after-OS-close acknowledgement may release an ID. */
int vw553_socket_backend_errno(void);

/* Set while the Wi-Fi interface is leaving SoftAP and has not obtained a
 * station IP yet.  The AP netcfg thread can queue one final discovery
 * broadcast after the vif switch; that datagram can never leave the dead AP
 * interface, so the TKL layer drops it before it becomes AT traffic. */
void vw553_socket_ap_teardown_set(bool active);
bool vw553_socket_ap_teardown_active(void);
/* Mark existing listeners as already closed by the AP->STA vif switch.  Their
 * later close becomes a local slot release instead of an AT stop command,
 * which VW553 would reject because it closed them with the AP vif. */
void vw553_socket_ap_listeners_invalidate(bool invalid);
/* True while AT+CWJAP_CUR owns the shared AT channel.  DNS, connect, send and
 * listener-start commands fail fast instead of timing out and poisoning the
 * transport; the upper layer retries after the join completes. */
void vw553_socket_wifi_join_set(bool active);
bool vw553_socket_wifi_join_active(void);
/* Drop all cached hostname->IPv4 results (link change, AP switch, module
 * reinitialisation). The cache itself is bounded and TTL/LRU managed. */
void vw553_socket_dns_cache_flush(void);

int vw553_socket_select(int maxfd, TUYA_FD_SET_T *readfds, TUYA_FD_SET_T *writefds,
                        TUYA_FD_SET_T *errorfds, uint32_t timeout_ms);
int vw553_socket_get_nonblock(int fd);
OPERATE_RET vw553_socket_set_block(int fd, BOOL_T block);
TUYA_ERRNO vw553_socket_close(int fd);
TUYA_ERRNO vw553_socket_shutdown(int fd, int how);
int vw553_socket_create(TUYA_PROTOCOL_TYPE_E type);
TUYA_ERRNO vw553_socket_connect(int fd, TUYA_IP_ADDR_T addr, uint16_t port);
TUYA_ERRNO vw553_socket_bind(int fd, TUYA_IP_ADDR_T addr, uint16_t port);
TUYA_ERRNO vw553_socket_listen(int fd, int backlog);
TUYA_ERRNO vw553_socket_accept(int fd, TUYA_IP_ADDR_T *addr, uint16_t *port);
TUYA_ERRNO vw553_socket_send(int fd, const void *buf, uint32_t length);
TUYA_ERRNO vw553_socket_sendto(int fd, const void *buf, uint32_t length, TUYA_IP_ADDR_T addr, uint16_t port);
TUYA_ERRNO vw553_socket_recv(int fd, void *buf, uint32_t length);
int vw553_socket_recv_nd_size(int fd, void *buf, uint32_t size, uint32_t needed);
TUYA_ERRNO vw553_socket_recvfrom(int fd, void *buf, uint32_t length, TUYA_IP_ADDR_T *addr, uint16_t *port);
OPERATE_RET vw553_socket_gethostbyname(const char *domain, TUYA_IP_ADDR_T *addr);
OPERATE_RET vw553_socket_set_timeout(int fd, int timeout_ms, TUYA_TRANS_TYPE_E type);
OPERATE_RET vw553_socket_set_bufsize(int fd, int size, TUYA_TRANS_TYPE_E type);
OPERATE_RET vw553_socket_set_broadcast(int fd);
OPERATE_RET vw553_socket_getsockname(int fd, TUYA_IP_ADDR_T *addr, uint16_t *port);
OPERATE_RET vw553_socket_getpeername(int fd, TUYA_IP_ADDR_T *addr, uint16_t *port);

#endif
