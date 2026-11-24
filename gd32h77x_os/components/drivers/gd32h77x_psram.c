/**
 * @file gd32h77x_psram.c
 * @brief GD32H77X PSRAM device abstraction and profile selection.
 *
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "gd32h77x_psram.h"
#include "gd32h77x_psram_aps6404l.h"
#include "gd32h77x_status.h"

#include <stddef.h>
#include <string.h>

static gd32h77x_psram_device_t s_psram;

static const gd32h77x_psram_profile_ops_t *__select_profile(
    const char *profile)
{
    if (profile == NULL) {
        return NULL;
    }
    if (strcmp(profile, gd32h77x_psram_aps6404l_ops.name) == 0) {
        return &gd32h77x_psram_aps6404l_ops;
    }
    return NULL;
}

gd32h77x_status_t gd32h77x_psram_init(const gd32h77x_psram_config_t *config)
{
    if (config == NULL || config->capacity_bytes == 0U ||
        config->clock_hz == 0U ||
        (config->access_mode != GD32H77X_PSRAM_MODE_INDIRECT &&
         config->access_mode != GD32H77X_PSRAM_MODE_MAPPED) ||
        (config->access_mode == GD32H77X_PSRAM_MODE_MAPPED &&
         config->mapped_base == 0U)) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    if (s_psram.ready) {
        return GD32H77X_STATUS_OK;
    }

    const gd32h77x_psram_profile_ops_t *ops =
        __select_profile(config->profile);
    if (ops == NULL || config->capacity_bytes > ops->capacity_bytes ||
        config->clock_hz > ops->max_clock_hz || ops->reset == NULL ||
        ops->probe == NULL || ops->selftest == NULL || ops->read == NULL ||
        ops->write == NULL ||
        (config->access_mode == GD32H77X_PSRAM_MODE_MAPPED &&
         ops->configure_memory_mapped == NULL)) {
        return GD32H77X_STATUS_UNSUPPORTED;
    }

    memset(&s_psram, 0, sizeof(s_psram));
    s_psram.ops = ops;
    s_psram.mapped_base =
        config->access_mode == GD32H77X_PSRAM_MODE_MAPPED
            ? config->mapped_base
            : 0U;
    s_psram.capacity_bytes = config->capacity_bytes;
    s_psram.clock_hz = config->clock_hz;
    s_psram.access_mode = config->access_mode;

    s_psram.stage = GD32H77X_PSRAM_STAGE_CONTROLLER;
    gd32h77x_status_t rt =
        gd32h77x_ospi_init(&s_psram.controller, &config->controller);
    if (rt != GD32H77X_STATUS_OK) {
        return rt;
    }

    uint32_t probe_clock_hz = ops->probe_clock_hz != 0U &&
                                      ops->probe_clock_hz < s_psram.clock_hz
                                  ? ops->probe_clock_hz
                                  : s_psram.clock_hz;
    rt = gd32h77x_ospi_set_clock(&s_psram.controller, probe_clock_hz);
    if (rt != GD32H77X_STATUS_OK) {
        (void)gd32h77x_psram_deinit();
        return rt;
    }

    s_psram.stage = GD32H77X_PSRAM_STAGE_RESET;
    rt = ops->reset(&s_psram);
    if (rt == GD32H77X_STATUS_OK) {
        s_psram.stage = GD32H77X_PSRAM_STAGE_PROBE;
        rt = ops->probe(&s_psram);
    }
    if (rt == GD32H77X_STATUS_OK) {
        /* Validate the run-time clock before accepting the device. */
        rt = gd32h77x_ospi_set_clock(&s_psram.controller, s_psram.clock_hz);
    }
    if (rt == GD32H77X_STATUS_OK) {
        s_psram.stage = GD32H77X_PSRAM_STAGE_SELFTEST;
        rt = ops->selftest(&s_psram);
    }
    if (rt == GD32H77X_STATUS_OK &&
        s_psram.access_mode == GD32H77X_PSRAM_MODE_MAPPED) {
        s_psram.stage = GD32H77X_PSRAM_STAGE_MEMORY_MAPPED;
        rt = ops->configure_memory_mapped(&s_psram);
    }
    if (rt != GD32H77X_STATUS_OK) {
        (void)gd32h77x_psram_deinit();
        return rt;
    }

    s_psram.stage = GD32H77X_PSRAM_STAGE_READY;
    s_psram.ready = true;
    return GD32H77X_STATUS_OK;
}

gd32h77x_status_t gd32h77x_psram_deinit(void)
{
    gd32h77x_ospi_deinit(&s_psram.controller);
    s_psram.ready = false;
    s_psram.stage = GD32H77X_PSRAM_STAGE_NONE;
    return GD32H77X_STATUS_OK;
}

bool gd32h77x_psram_is_ready(void)
{
    return s_psram.ready;
}

uintptr_t gd32h77x_psram_base(void)
{
    return s_psram.mapped_base;
}

uint32_t gd32h77x_psram_size(void)
{
    return s_psram.capacity_bytes;
}

gd32h77x_psram_access_mode_t gd32h77x_psram_access_mode(void)
{
    return s_psram.access_mode;
}

gd32h77x_psram_stage_t gd32h77x_psram_stage(void)
{
    return s_psram.stage;
}

uint8_t gd32h77x_psram_last_id(void)
{
    return s_psram.last_id;
}

gd32h77x_status_t gd32h77x_psram_read(uint32_t offset, void *data, size_t size)
{
    if (!s_psram.ready) {
        return GD32H77X_STATUS_NOT_READY;
    }
    if (s_psram.access_mode != GD32H77X_PSRAM_MODE_INDIRECT) {
        return GD32H77X_STATUS_UNSUPPORTED;
    }
    if (size == 0U) {
        return GD32H77X_STATUS_OK;
    }
    if (data == NULL || size > UINT32_MAX ||
        (uint64_t)offset + size > s_psram.capacity_bytes) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    return s_psram.ops->read(&s_psram, offset, data, size);
}

gd32h77x_status_t gd32h77x_psram_write(uint32_t offset, const void *data,
                                       size_t size)
{
    if (!s_psram.ready) {
        return GD32H77X_STATUS_NOT_READY;
    }
    if (s_psram.access_mode != GD32H77X_PSRAM_MODE_INDIRECT) {
        return GD32H77X_STATUS_UNSUPPORTED;
    }
    if (size == 0U) {
        return GD32H77X_STATUS_OK;
    }
    if (data == NULL || size > UINT32_MAX ||
        (uint64_t)offset + size > s_psram.capacity_bytes) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    return s_psram.ops->write(&s_psram, offset, data, size);
}

gd32h77x_status_t gd32h77x_psram_fill(uint32_t offset, uint8_t value,
                                      size_t size)
{
    uint8_t chunk[64];

    if (!s_psram.ready) {
        return GD32H77X_STATUS_NOT_READY;
    }
    if (s_psram.access_mode != GD32H77X_PSRAM_MODE_INDIRECT) {
        return GD32H77X_STATUS_UNSUPPORTED;
    }
    if (size > UINT32_MAX ||
        (uint64_t)offset + size > s_psram.capacity_bytes) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    if (size == 0U) {
        return GD32H77X_STATUS_OK;
    }

    memset(chunk, value, sizeof(chunk));
    while (size > 0U) {
        size_t length = size < sizeof(chunk) ? size : sizeof(chunk);
        gd32h77x_status_t rt =
            gd32h77x_psram_write(offset, chunk, length);
        if (rt != GD32H77X_STATUS_OK) {
            return rt;
        }
        offset += (uint32_t)length;
        size -= length;
    }
    return GD32H77X_STATUS_OK;
}

uint32_t gd32h77x_psram_last_probe_data(uint8_t *data, uint32_t capacity)
{
    if (data == NULL || capacity == 0U) {
        return 0U;
    }
    uint32_t length = s_psram.last_probe_data_len;
    if (length > GD32H77X_PSRAM_PROBE_DATA_MAX) {
        length = GD32H77X_PSRAM_PROBE_DATA_MAX;
    }
    if (length > capacity) {
        length = capacity;
    }
    memcpy(data, s_psram.last_probe_data, length);
    return length;
}
