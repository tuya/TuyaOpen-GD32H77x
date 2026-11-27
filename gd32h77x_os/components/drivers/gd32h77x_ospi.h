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
    bool octal;
    gd32h77x_ospi_pin_t cs;
    gd32h77x_ospi_pin_t sck;
    gd32h77x_ospi_pin_t dqs;
    gd32h77x_ospi_pin_t io[8];
    bool reset_enabled;
    gd32h77x_ospi_pin_t reset;
} gd32h77x_ospi_config_t;

typedef struct {
    uint32_t sample_shift;
    uint32_t fifo_threshold;
    uint32_t device_size;
    uint32_t cs_high_time;
    uint32_t memory_type;
    uint32_t delay_hold_cycle;
    uint32_t sample_delay;
    uint32_t refrate;
    bool dqs_rck_add_enable;
    bool receive_delay_block_enable;
    uint8_t tck_delay_tap;
    uint8_t rck_delay_tap;
    bool receive_sck_delay_chain_select;
} gd32h77x_ospi_timing_t;

typedef struct {
    uint32_t instruction;
    uint32_t instruction_mode;
    uint32_t instruction_size;
    uint32_t address_mode;
    uint32_t address_size;
    uint32_t data_mode;
    bool instruction_dtr;
    bool address_dtr;
    bool data_dtr;
    bool dqs_enable;
    uint8_t dummy_cycles;
} gd32h77x_ospi_frame_t;

typedef struct {
    gd32h77x_ospi_frame_t frame;
    uint32_t address;
    bool has_address;
    bool read;
    uint8_t *data;
    uint32_t size;
} gd32h77x_ospi_transfer_t;

typedef struct {
    gd32h77x_ospi_frame_t read_frame;
    gd32h77x_ospi_frame_t write_frame;
    uint32_t timeout_cycles;
    uint32_t refresh_cycles;
} gd32h77x_ospi_memory_mapped_t;

typedef struct {
    uint32_t periph;
    uint32_t ospim_port;
    uint32_t capacity_bytes;
    bool initialized;
    bool memory_mapped;
    bool reset_enabled;
    uint32_t reset_port;
    uint32_t reset_pin;
    uint32_t memory_type;
    uint32_t delay_hold_cycle;
    void *lock;
} gd32h77x_ospi_t;

gd32h77x_status_t gd32h77x_ospi_init(gd32h77x_ospi_t *ospi,
                              const gd32h77x_ospi_config_t *config,
                              const gd32h77x_ospi_timing_t *timing);
gd32h77x_status_t gd32h77x_ospi_set_clock(gd32h77x_ospi_t *ospi,
                                         uint32_t clock_hz);
gd32h77x_status_t gd32h77x_ospi_wait_ready(gd32h77x_ospi_t *ospi);
gd32h77x_status_t gd32h77x_ospi_reset_device(gd32h77x_ospi_t *ospi,
                                            uint32_t low_ms,
                                            uint32_t high_ms);
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
