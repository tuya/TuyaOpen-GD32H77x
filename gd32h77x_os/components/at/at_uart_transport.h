/**
 * @file at_uart_transport.h
 * @brief UART transport implementation for the AT stack.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */
#ifndef AT_UART_H
#define AT_UART_H

#include "at_transport.h"
#include "gd32h77x_78x.h"

typedef struct {
    uint32_t usart;
    rcu_periph_enum usart_clock;
    rcu_periph_enum gpio_clock;
    uint32_t gpio_port;
    uint32_t tx_pin;
    uint32_t rx_pin;
    uint32_t alternate_function;
    rcu_periph_enum rx_gpio_clock;
    uint32_t rx_gpio_port;
    uint32_t rx_alternate_function;
    IRQn_Type irq;
    uint32_t baudrate;
    uint32_t word_length;
    uint32_t stop_bits;
    uint32_t parity;
    uint32_t irq_priority;
} at_uart_config_t;

at_transport_t *at_uart_transport_get(void);

#endif /* AT_UART_H */
