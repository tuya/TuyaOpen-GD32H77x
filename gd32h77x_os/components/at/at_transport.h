/**
 * @file transport.h
 * @brief Transport-independent byte channel used by the AT stack.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */
#ifndef AT_TRANSPORT_H
#define AT_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct at_transport at_transport_t;
typedef void (*at_transport_rx_cb_t)(void *context, const uint8_t *data, size_t length);
typedef bool (*at_transport_rx_ready_cb_t)(void *context, size_t length);
typedef enum {
    AT_TRANSPORT_FAULT_RX_OVERFLOW = 1U << 0,
    AT_TRANSPORT_FAULT_OVERRUN = 1U << 1,
    AT_TRANSPORT_FAULT_FRAMING = 1U << 2,
    AT_TRANSPORT_FAULT_NOISE = 1U << 3,
    AT_TRANSPORT_FAULT_PARITY = 1U << 4,
} at_transport_fault_t;
typedef void (*at_transport_fault_cb_t)(void *context, uint32_t faults);

typedef enum {
    AT_TRANSPORT_OK = 0,
    AT_TRANSPORT_ERROR,
    AT_TRANSPORT_TIMEOUT,
} at_transport_status_t;

typedef struct {
    const char *name;
    int (*open)(at_transport_t *transport);
    void (*close)(at_transport_t *transport);
    int (*write)(at_transport_t *transport, const uint8_t *data, size_t length,
                 uint32_t timeout_ms, size_t *sent);
    void (*suspend)(at_transport_t *transport);
    int (*resume)(at_transport_t *transport);
    void (*flush)(at_transport_t *transport);
    uint32_t (*rx_burst_high_water)(const at_transport_t *transport);
} at_transport_ops_t;

struct at_transport {
    const at_transport_ops_t *ops;
    void *context;
    at_transport_rx_cb_t rx_callback;
    void *rx_context;
    at_transport_rx_ready_cb_t rx_ready_callback;
    void *rx_ready_context;
    at_transport_fault_cb_t fault_callback;
    void *fault_context;
};

at_transport_t *at_transport_get(void);

static inline int at_transport_open(at_transport_t *transport)
{
    return transport && transport->ops && transport->ops->open
               ? transport->ops->open(transport)
               : AT_TRANSPORT_ERROR;
}

static inline void at_transport_close(at_transport_t *transport)
{
    if (transport && transport->ops && transport->ops->close) transport->ops->close(transport);
}

static inline int at_transport_write(at_transport_t *transport, const uint8_t *data,
                                     size_t length, uint32_t timeout_ms, size_t *sent)
{
    return transport && transport->ops && transport->ops->write
               ? transport->ops->write(transport, data, length, timeout_ms, sent)
               : AT_TRANSPORT_ERROR;
}

static inline void at_transport_suspend(at_transport_t *transport)
{
    if (transport && transport->ops && transport->ops->suspend) transport->ops->suspend(transport);
}

static inline int at_transport_resume(at_transport_t *transport)
{
    return transport && transport->ops && transport->ops->resume
               ? transport->ops->resume(transport)
               : AT_TRANSPORT_ERROR;
}

static inline void at_transport_flush(at_transport_t *transport)
{
    if (transport && transport->ops && transport->ops->flush) transport->ops->flush(transport);
}

static inline uint32_t at_transport_rx_burst_high_water(const at_transport_t *transport)
{
    return transport && transport->ops && transport->ops->rx_burst_high_water
               ? transport->ops->rx_burst_high_water(transport)
               : 0U;
}

#endif /* AT_TRANSPORT_H */
