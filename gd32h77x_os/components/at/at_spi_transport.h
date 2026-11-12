/**
 * @file at_spi_transport.h
 * @brief GD32H779V_START SPI master transport for the VW553 AT gateway.
 */
#ifndef AT_SPI_TRANSPORT_H
#define AT_SPI_TRANSPORT_H

#include "at_transport.h"

#include <stdint.h>

typedef struct {
    uint32_t ready_high_timeout;
    uint32_t ready_low_timeout;
    uint32_t ready_idle_timeout;
    uint32_t spi_tp_timeout;
    uint32_t spi_rp_timeout;
    uint32_t spi_et_timeout;
    uint32_t invalid_frame_count;
    uint32_t valid_frame_count;
    uint32_t gpio_istat;
    uint32_t gpio_octl;
    uint32_t spi_stat;
    uint32_t spi_ctl0;
    uint32_t spi_cfg0;
    uint32_t spi_cfg1;
    uint8_t link_phase;
    uint8_t tx_pending;
    uint16_t tx_next;
    uint16_t rx_next;
    uint8_t last_rx_header[16];
} at_spi_transport_diag_t;

at_transport_t *at_spi_transport_get(void);
void at_spi_transport_diag_get(at_spi_transport_diag_t *diag);

#endif /* AT_SPI_TRANSPORT_H */
