/**
 * @file at_transport.c
 * @brief Build-time selected AT transport.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */
#include "at_transport.h"

#if defined(AT_TRANSPORT_SPI) && AT_TRANSPORT_SPI
#include "at_spi_transport.h"

at_transport_t *at_transport_get(void)
{
    return at_spi_transport_get();
}
#elif defined(AT_TRANSPORT_UART) && AT_TRANSPORT_UART
#include "at_uart_transport.h"

at_transport_t *at_transport_get(void)
{
    return at_uart_transport_get();
}
#else
#error "Select exactly one AT transport (AT_TRANSPORT_UART or AT_TRANSPORT_SPI)"
#endif
