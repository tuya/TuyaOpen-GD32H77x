/**
 * @file gd32h77x_psram.h
 * @brief GD32H77X PSRAM device abstraction.
 *
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#ifndef GD32H77X_PSRAM_H
#define GD32H77X_PSRAM_H

#define GD32H77X_PSRAM_PROBE_DATA_MAX 8U

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "gd32h77x_ospi.h"
#include "gd32h77x_status.h"

#ifdef __cplusplus
extern "C" {
#endif

struct psram_device;

typedef enum {
    GD32H77X_PSRAM_MODE_INDIRECT = 0,
    GD32H77X_PSRAM_MODE_MAPPED,
} gd32h77x_psram_access_mode_t;

typedef enum {
    GD32H77X_PSRAM_STAGE_NONE = 0,
    GD32H77X_PSRAM_STAGE_CONTROLLER,
    GD32H77X_PSRAM_STAGE_RESET,
    GD32H77X_PSRAM_STAGE_PROBE,
    GD32H77X_PSRAM_STAGE_SELFTEST,
    GD32H77X_PSRAM_STAGE_MEMORY_MAPPED,
    GD32H77X_PSRAM_STAGE_READY,
} gd32h77x_psram_stage_t;

typedef struct {
    const char *name;
    uint32_t capacity_bytes;
    uint32_t max_clock_hz;
    uint32_t probe_clock_hz;
    gd32h77x_status_t (*reset)(struct psram_device *device);
    gd32h77x_status_t (*probe)(struct psram_device *device);
    gd32h77x_status_t (*selftest)(struct psram_device *device);
    gd32h77x_status_t (*read)(struct psram_device *device, uint32_t offset,
                              void *data, size_t size);
    gd32h77x_status_t (*write)(struct psram_device *device, uint32_t offset,
                               const void *data, size_t size);
    gd32h77x_status_t (*configure_memory_mapped)(struct psram_device *device);
} gd32h77x_psram_profile_ops_t;

typedef struct psram_device {
    const gd32h77x_psram_profile_ops_t *ops;
    gd32h77x_ospi_t controller;
    uintptr_t mapped_base;
    uint32_t capacity_bytes;
    uint32_t clock_hz;
    gd32h77x_psram_access_mode_t access_mode;
    gd32h77x_psram_stage_t stage;
    uint8_t last_id;
    uint8_t last_probe_data[GD32H77X_PSRAM_PROBE_DATA_MAX];
    uint32_t last_probe_data_len;
    bool ready;
} gd32h77x_psram_device_t;

typedef struct {
    const char *profile;
    uintptr_t mapped_base;
    uint32_t capacity_bytes;
    uint32_t clock_hz;
    gd32h77x_psram_access_mode_t access_mode;
    gd32h77x_ospi_config_t controller;
} gd32h77x_psram_config_t;

extern const gd32h77x_psram_profile_ops_t gd32h77x_psram_aps6404l_ops;

gd32h77x_status_t gd32h77x_psram_init(const gd32h77x_psram_config_t *config);
gd32h77x_status_t gd32h77x_psram_deinit(void);
bool gd32h77x_psram_is_ready(void);
uintptr_t gd32h77x_psram_base(void);
uint32_t gd32h77x_psram_size(void);
gd32h77x_psram_access_mode_t gd32h77x_psram_access_mode(void);
gd32h77x_psram_stage_t gd32h77x_psram_stage(void);
gd32h77x_status_t gd32h77x_psram_read(uint32_t offset, void *data, size_t size);
gd32h77x_status_t gd32h77x_psram_write(uint32_t offset, const void *data,
                                       size_t size);
gd32h77x_status_t gd32h77x_psram_fill(uint32_t offset, uint8_t value,
                                      size_t size);
uint8_t gd32h77x_psram_last_id(void);
uint32_t gd32h77x_psram_last_probe_data(uint8_t *data, uint32_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* GD32H77X_PSRAM_H */
