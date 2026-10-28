/*
 * @file gd32h77x_uart.h
 * @brief GD32H77X UART diagnostic counters.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#ifndef GD32H77X_UART_H
#define GD32H77X_UART_H

#include <stdint.h>

typedef struct {
    uint32_t rx_bytes;
    uint32_t rx_stream_dropped;
    uint32_t rx_overrun_errors;
    uint32_t rx_frame_errors;
    uint32_t rx_noise_errors;
    uint32_t rx_parity_errors;
    uint32_t rx_callback_count;
} gd32h77x_uart_stats_t;

/* Snapshot counters accumulated by the USART1 RX path. */
void gd32h77x_uart_get_stats(gd32h77x_uart_stats_t *stats);

#endif
