/**
 * @file gd32h77x_at_bridge.h
 * @brief Raw USART1 <-> configured AT transport bridge.
 */
#ifndef GD32H77X_AT_BRIDGE_H
#define GD32H77X_AT_BRIDGE_H

#include <stdbool.h>

/* Host-side UART hook called from the bridge project's UART ISR. */
bool gd32h77x_at_bridge_host_rx_isr(void);

/* Starts two bounded FreeRTOS forwarding tasks. The AT module is reset once before
 * the bridge is exposed so the vendor AT firmware starts from a known state. */
void gd32h77x_at_bridge_start(void);

#endif /* GD32H77X_AT_BRIDGE_H */
