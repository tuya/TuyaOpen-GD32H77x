/**
 * @file gd32h77x_vw553_bridge.h
 * @brief Raw USART1 <-> UART4 bridge for VW553 vendor AT debugging.
 */
#ifndef GD32H77X_VW553_BRIDGE_H
#define GD32H77X_VW553_BRIDGE_H

#include <stdbool.h>

/* These hooks are called from the existing platform UART ISR handlers. The
 * return value is true when bridge mode consumed the interrupt. */
bool gd32h77x_vw553_bridge_uart4_rx_isr(void);
bool gd32h77x_vw553_bridge_usart1_rx_isr(void);

/* Starts two bounded FreeRTOS forwarding tasks. The VW553 is reset once before
 * the bridge is exposed so the vendor AT firmware starts from a known state. */
void gd32h77x_vw553_bridge_start(void);

#endif
