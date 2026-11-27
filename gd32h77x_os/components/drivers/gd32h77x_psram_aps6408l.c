/**
 * @file gd32h77x_psram_aps6408l.c
 * @brief AP Memory APS6408L-3OBM eight-line PSRAM profile.
 *
 * Device-specific commands and timing live here; all controller register
 * access goes through the generic GD32H77X OSPI layer.
 *
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "gd32h77x_psram_aps6408l.h"

#include "FreeRTOS.h"
#include "task.h"
#include "gd32h77x_78x_ospi.h"
#include "gd32h77x_status.h"

#include <string.h>

#define APS6408L_CAPACITY_BYTES (8U * 1024U * 1024U)
#define APS6408L_ROW_SIZE       1024U
#define APS6408L_MAX_CLOCK_HZ   84000000U
#define APS6408L_READ_DUMMY     10U
#define APS6408L_WRITE_DUMMY    4U
#define APS6408L_MR0            0x28U
#define APS6408L_VENDOR_ID      0x0D93U
#define APS6408L_DEVICE_ID      0x93E0U

#define APS6408L_CMD_READ_REG     0x4040U
#define APS6408L_CMD_WRITE_REG    0xC0C0U
#define APS6408L_CMD_LINEAR_READ  0x2020U
#define APS6408L_CMD_LINEAR_WRITE 0xA0A0U

static const gd32h77x_ospi_timing_t s_aps6408l_timing = {
    .sample_shift = OSPI_SAMPLE_SHIFTING_NONE,
    .fifo_threshold = OSPI_FIFO_THRESHOLD_32,
    .device_size = OSPI_MESZ_512_MBS,
    .cs_high_time = OSPI_CS_HIGH_TIME_8_CYCLE,
    .memory_type = OSPI_MICRON_MODE,
    .delay_hold_cycle = OSPI_DELAY_HOLD_QUARTER_CYCLE,
    .sample_delay = OSPI_SAMPLE_DELAY_DISABLE,
    .refrate = 400U,
    .dqs_rck_add_enable = true,
    .receive_delay_block_enable = true,
    .tck_delay_tap = 6U,
    .rck_delay_tap = 16U,
    .receive_sck_delay_chain_select = true,
};

static gd32h77x_status_t __transfer(gd32h77x_psram_device_t *device,
                                    uint32_t instruction, uint32_t address,
                                    bool has_address, bool read,
                                    uint8_t dummy_cycles, uint8_t *data,
                                    uint32_t size)
{
    gd32h77x_ospi_transfer_t transfer = {
        .frame = {
            .instruction = instruction,
            .instruction_mode = OSPI_INSTRUCTION_8_LINES,
            .instruction_size = OSPI_INSTRUCTION_16_BITS,
            .address_mode = has_address ? OSPI_ADDRESS_8_LINES
                                        : OSPI_ADDRESS_NONE,
            .address_size = OSPI_ADDRESS_32_BITS,
            .data_mode = size != 0U ? OSPI_DATA_8_LINES : OSPI_DATA_NONE,
            .instruction_dtr = true,
            .address_dtr = true,
            .data_dtr = true,
            .dqs_enable = size != 0U,
            .dummy_cycles = dummy_cycles,
        },
        .address = address,
        .has_address = has_address,
        .read = read,
        .data = data,
        .size = size,
    };
    return gd32h77x_ospi_transfer(&device->controller, &transfer);
}

static gd32h77x_status_t __read_register(gd32h77x_psram_device_t *device,
                                         uint32_t address, uint16_t *value)
{
    uint8_t data[2] = {0};
    gd32h77x_status_t rt = __transfer(device, APS6408L_CMD_READ_REG, address,
                                      true, true, APS6408L_READ_DUMMY, data,
                                      sizeof(data));
    if (rt != GD32H77X_STATUS_OK) {
        return rt;
    }
    if (value != NULL) {
        *value = ((uint16_t)data[0] << 8U) | data[1];
    }
    return GD32H77X_STATUS_OK;
}

static gd32h77x_status_t __write_register(gd32h77x_psram_device_t *device,
                                          uint32_t address, uint16_t value)
{
    uint8_t data[2] = {
        (uint8_t)(value >> 8U),
        (uint8_t)value,
    };
    return __transfer(device, APS6408L_CMD_WRITE_REG, address, true, false,
                      0U, data, sizeof(data));
}

static gd32h77x_status_t __mode_config(gd32h77x_psram_device_t *device)
{
    uint16_t value = 0U;

    gd32h77x_status_t rt = __write_register(
        device, 0x00U, (uint16_t)(APS6408L_MR0 << 8U));
    if (rt != GD32H77X_STATUS_OK) {
        return rt;
    }

    rt = __read_register(device, 0x00U, &value);
    if (rt != GD32H77X_STATUS_OK ||
        (uint8_t)(value >> 8U) != APS6408L_MR0) {
        return GD32H77X_STATUS_IO_ERROR;
    }

    rt = __read_register(device, 0x08U, &value);
    if (rt != GD32H77X_STATUS_OK) {
        return rt;
    }
    value = (uint16_t)(((value >> 8U) | 0x08U) & 0x7FU);
    rt = __write_register(device, 0x08U, (uint16_t)(value << 8U));
    if (rt != GD32H77X_STATUS_OK) {
        return rt;
    }

    uint16_t readback = 0U;
    rt = __read_register(device, 0x08U, &readback);
    if (rt != GD32H77X_STATUS_OK ||
        (uint8_t)(readback >> 8U) != (uint8_t)value) {
        return GD32H77X_STATUS_IO_ERROR;
    }
    return GD32H77X_STATUS_OK;
}

static gd32h77x_status_t __array_write(gd32h77x_psram_device_t *device,
                                       uint32_t offset, const void *data,
                                       size_t size)
{
    const uint8_t *bytes = data;

    while (size > 0U) {
        uint32_t chunk = APS6408L_ROW_SIZE -
                         (offset & (APS6408L_ROW_SIZE - 1U));
        if (chunk > size) {
            chunk = (uint32_t)size;
        }
        gd32h77x_status_t rt = __transfer(
            device, APS6408L_CMD_LINEAR_WRITE, offset, true, false,
            APS6408L_WRITE_DUMMY, (uint8_t *)bytes, chunk);
        if (rt != GD32H77X_STATUS_OK) {
            return rt;
        }
        offset += chunk;
        bytes += chunk;
        size -= chunk;
    }
    return GD32H77X_STATUS_OK;
}

static gd32h77x_status_t __array_read(gd32h77x_psram_device_t *device,
                                      uint32_t offset, void *data,
                                      size_t size)
{
    uint8_t *bytes = data;

    while (size > 0U) {
        uint32_t chunk = APS6408L_ROW_SIZE -
                         (offset & (APS6408L_ROW_SIZE - 1U));
        if (chunk > size) {
            chunk = (uint32_t)size;
        }
        gd32h77x_status_t rt = __transfer(
            device, APS6408L_CMD_LINEAR_READ, offset, true, true,
            APS6408L_READ_DUMMY, bytes, chunk);
        if (rt != GD32H77X_STATUS_OK) {
            return rt;
        }
        offset += chunk;
        bytes += chunk;
        size -= chunk;
    }
    return GD32H77X_STATUS_OK;
}

static gd32h77x_status_t __reset(gd32h77x_psram_device_t *device)
{
    if (device->controller.reset_enabled) {
        gd32h77x_status_t rt = gd32h77x_ospi_reset_device(
            &device->controller, 1U, 2U);
        if (rt != GD32H77X_STATUS_OK) {
            return rt;
        }
    } else {
        vTaskDelay(pdMS_TO_TICKS(2U));
    }
    return GD32H77X_STATUS_OK;
}

static gd32h77x_status_t __probe(gd32h77x_psram_device_t *device)
{
    uint16_t vendor_id = 0U;
    uint16_t device_id = 0U;

    gd32h77x_status_t rt = __read_register(device, 0x01U, &vendor_id);
    if (rt == GD32H77X_STATUS_OK) {
        rt = __read_register(device, 0x02U, &device_id);
    }

    device->last_probe_data[0] = (uint8_t)(vendor_id >> 8U);
    device->last_probe_data[1] = (uint8_t)vendor_id;
    device->last_probe_data[2] = (uint8_t)(device_id >> 8U);
    device->last_probe_data[3] = (uint8_t)device_id;
    device->last_probe_data_len = 4U;
    device->last_id = (uint8_t)device_id;
    if (rt != GD32H77X_STATUS_OK) {
        return rt;
    }
    return vendor_id == APS6408L_VENDOR_ID &&
                   device_id == APS6408L_DEVICE_ID
               ? GD32H77X_STATUS_OK
               : GD32H77X_STATUS_UNSUPPORTED;
}

static gd32h77x_status_t __selftest(gd32h77x_psram_device_t *device)
{
    static const uint32_t pattern[8] = {
        0x13572468U, 0x89ABCDEFU, 0xA5C35A3CU, 0x5A5A5A5AU,
        0x0F1E2D3CU, 0xF0E1D2C3U, 0x55AAAA55U, 0xAA55AA55U,
    };
    uint32_t readback[8] = {0};
    const uint32_t offset = 0x1000U;

    gd32h77x_status_t rt = __mode_config(device);
    if (rt != GD32H77X_STATUS_OK) {
        return rt;
    }
    rt = __array_write(device, offset, pattern, sizeof(pattern));
    if (rt != GD32H77X_STATUS_OK) {
        return rt;
    }
    rt = __array_read(device, offset, readback, sizeof(readback));
    if (rt != GD32H77X_STATUS_OK) {
        return rt;
    }
    return memcmp(pattern, readback, sizeof(pattern)) == 0
               ? GD32H77X_STATUS_OK
               : GD32H77X_STATUS_IO_ERROR;
}

static gd32h77x_status_t __configure_memory_mapped(
    gd32h77x_psram_device_t *device)
{
    gd32h77x_ospi_memory_mapped_t config = {
        .read_frame = {
            .instruction = APS6408L_CMD_LINEAR_READ,
            .instruction_mode = OSPI_INSTRUCTION_8_LINES,
            .instruction_size = OSPI_INSTRUCTION_16_BITS,
            .address_mode = OSPI_ADDRESS_8_LINES,
            .address_size = OSPI_ADDRESS_32_BITS,
            .data_mode = OSPI_DATA_8_LINES,
            .instruction_dtr = true,
            .address_dtr = true,
            .data_dtr = true,
            .dqs_enable = true,
            .dummy_cycles = APS6408L_READ_DUMMY,
        },
        .write_frame = {
            .instruction = APS6408L_CMD_LINEAR_WRITE,
            .instruction_mode = OSPI_INSTRUCTION_8_LINES,
            .instruction_size = OSPI_INSTRUCTION_16_BITS,
            .address_mode = OSPI_ADDRESS_8_LINES,
            .address_size = OSPI_ADDRESS_32_BITS,
            .data_mode = OSPI_DATA_8_LINES,
            .instruction_dtr = true,
            .address_dtr = true,
            .data_dtr = true,
            .dqs_enable = true,
            .dummy_cycles = APS6408L_WRITE_DUMMY,
        },
    };
    return gd32h77x_ospi_enter_memory_mapped(&device->controller, &config);
}

const gd32h77x_psram_profile_ops_t gd32h77x_psram_aps6408l_ops = {
    .name = "APS6408L-3OBM",
    .capacity_bytes = APS6408L_CAPACITY_BYTES,
    .max_clock_hz = APS6408L_MAX_CLOCK_HZ,
    .probe_clock_hz = 0U,
    .timing = &s_aps6408l_timing,
    .reset = __reset,
    .probe = __probe,
    .selftest = __selftest,
    .read = __array_read,
    .write = __array_write,
    .configure_memory_mapped = __configure_memory_mapped,
};
