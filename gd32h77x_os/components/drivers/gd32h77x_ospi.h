/**
 * @file gd32h77x_ospi.h
 * @brief GD32H77X OSPI/OSPIM controller abstraction.
 *
 * This layer knows how to drive the OSPI controller but does not contain
 * PSRAM or NOR command semantics.
 *
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#ifndef GD32H77X_OSPI_H
#define GD32H77X_OSPI_H

#include <stdbool.h>
#include <stdint.h>
#include "gd32h77x_status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t port;
    uint32_t clock;
    uint32_t pin;
    uint32_t alternate;
} gd32h77x_ospi_pin_t;

typedef struct {
    uint32_t ospim_port;
    uint32_t clock_hz;
    uint32_t capacity_bytes;
    bool set_pa1_switch;
    bool pa1_switch_open;
    bool set_pc2_switch;
    bool pc2_switch_open;
    gd32h77x_ospi_pin_t cs;
    gd32h77x_ospi_pin_t sck;
    gd32h77x_ospi_pin_t io[4];
} gd32h77x_ospi_config_t;

typedef struct {
    uint8_t instruction;
    uint32_t address;
    bool has_address;
    bool read;
    uint32_t instruction_mode;
    uint32_t address_mode;
    uint32_t address_size;
    uint32_t data_mode;
    uint8_t dummy_cycles;
    uint8_t *data;
    uint32_t size;
} gd32h77x_ospi_transfer_t;

typedef struct {
    uint8_t read_instruction;
    uint8_t write_instruction;
    uint8_t read_dummy_cycles;
    uint32_t timeout_cycles;
    uint32_t refresh_cycles;
} gd32h77x_ospi_memory_mapped_t;

typedef struct {
    uint32_t periph;
    uint32_t ospim_port;
    uint32_t capacity_bytes;
    bool initialized;
    bool memory_mapped;
    void *lock;
} gd32h77x_ospi_t;

gd32h77x_status_t gd32h77x_ospi_init(gd32h77x_ospi_t *ospi,
                              const gd32h77x_ospi_config_t *config);
gd32h77x_status_t gd32h77x_ospi_set_clock(gd32h77x_ospi_t *ospi,
                                         uint32_t clock_hz);
gd32h77x_status_t gd32h77x_ospi_transfer(gd32h77x_ospi_t *ospi,
                                  const gd32h77x_ospi_transfer_t *transfer);
gd32h77x_status_t gd32h77x_ospi_enter_memory_mapped(
    gd32h77x_ospi_t *ospi, const gd32h77x_ospi_memory_mapped_t *config);
gd32h77x_status_t gd32h77x_ospi_abort(gd32h77x_ospi_t *ospi);
void gd32h77x_ospi_deinit(gd32h77x_ospi_t *ospi);

#ifdef __cplusplus
}
#endif

#endif /* GD32H77X_OSPI_H */
