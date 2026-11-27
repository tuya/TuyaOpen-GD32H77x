/**
 * @file gd32h77x_psram_aps6404l.c
 * @brief AP Memory APS6404L PSRAM profile.
 *
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "gd32h77x_psram_aps6404l.h"

#include "FreeRTOS.h"
#include "task.h"
#include "gd32h77x_78x_ospi.h"
#include "gd32h77x_status.h"

#include <string.h>

#define APS6404L_CAPACITY_BYTES (8U * 1024U * 1024U)
#define APS6404L_PAGE_SIZE      1024U
#define APS6404L_MAX_CLOCK_HZ   84000000U
#define APS6404L_PROBE_CLOCK_HZ 2000000U
#define APS6404L_TCEM_NS        8000U
#define APS6404L_MIN_CHUNK      8U
#define APS6404L_CMD_READ_ID    0x9FU
#define APS6404L_CMD_RESET_EN   0x66U
#define APS6404L_CMD_RESET      0x99U
#define APS6404L_CMD_QUAD_READ  0xEBU
#define APS6404L_CMD_QUAD_WRITE 0x38U
#define APS6404L_READ_DUMMY     6U
#define APS6404L_MF_ID          0x0DU
#define APS6404L_KGD_PASS       0x5DU
#define APS6404L_MMAP_TIMEOUT   32U

static const gd32h77x_ospi_timing_t s_aps6404l_timing = {
    .sample_shift = OSPI_SAMPLE_SHIFTING_HALF_CYCLE,
    .fifo_threshold = OSPI_FIFO_THRESHOLD_4,
    .device_size = OSPI_MESZ_8_MBS,
    .cs_high_time = OSPI_CS_HIGH_TIME_3_CYCLE,
    .memory_type = OSPI_STANDARD_MODE,
    .delay_hold_cycle = OSPI_DELAY_HOLD_NONE,
    .sample_delay = OSPI_SAMPLE_DELAY_DISABLE,
    .refrate = 0U,
};

static gd32h77x_status_t __transfer(gd32h77x_psram_device_t *device,
                              uint8_t instruction, uint32_t address,
                              bool has_address, bool read, uint32_t data_mode,
                              uint8_t dummy_cycles, uint8_t *data,
                              uint32_t size)
{
    gd32h77x_ospi_transfer_t transfer = {
        .frame = {
            .instruction = instruction,
            .instruction_mode = OSPI_INSTRUCTION_1_LINE,
            .instruction_size = OSPI_INSTRUCTION_8_BITS,
            .address_mode = has_address ? OSPI_ADDRESS_4_LINES
                                        : OSPI_ADDRESS_NONE,
            .address_size = OSPI_ADDRESS_24_BITS,
            .data_mode = data_mode,
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

static uint32_t __aps_chunk_limit(const gd32h77x_psram_device_t *device)
{
    /* Keep a conservative half-tCEM budget. In SPI mode 0xEB/0x38 use one
     * instruction lane, four address lanes and four data lanes. */
    uint32_t tcem_cycles =
        (uint32_t)(((uint64_t)device->clock_hz * APS6404L_TCEM_NS) /
                   1000000000ULL);
    uint32_t overhead = 8U + 6U + APS6404L_READ_DUMMY;
    uint32_t budget = tcem_cycles / 2U;
    if (budget < overhead + 2U * APS6404L_MIN_CHUNK) {
        return 0U;
    }
    uint32_t limit = (budget - overhead) / 2U;
    return limit > APS6404L_PAGE_SIZE ? APS6404L_PAGE_SIZE : limit;
}

static uint32_t __aps_next_len(const gd32h77x_psram_device_t *device,
                               uint32_t offset, uint32_t remaining)
{
    uint32_t limit = __aps_chunk_limit(device);
    uint32_t page_left =
        APS6404L_PAGE_SIZE - (offset & (APS6404L_PAGE_SIZE - 1U));
    if (limit > page_left) {
        limit = page_left;
    }
    return limit > remaining ? remaining : limit;
}

static bool __aps_range_valid(const gd32h77x_psram_device_t *device,
                              uint32_t offset, size_t size)
{
    return device != NULL && size <= UINT32_MAX &&
           (uint64_t)offset + size <= device->capacity_bytes;
}

static gd32h77x_status_t __aps_write(gd32h77x_psram_device_t *device,
                                     uint32_t offset, const void *data,
                                     size_t size)
{
    if (!__aps_range_valid(device, offset, size) ||
        (size != 0U && data == NULL) || __aps_chunk_limit(device) == 0U) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }

    const uint8_t *bytes = data;
    while (size > 0U) {
        uint32_t length = __aps_next_len(device, offset, (uint32_t)size);
        gd32h77x_status_t rt =
            __transfer(device, APS6404L_CMD_QUAD_WRITE, offset, true, false,
                       OSPI_DATA_4_LINES, 0U, (uint8_t *)bytes, length);
        if (rt != GD32H77X_STATUS_OK) {
            return rt;
        }
        offset += length;
        bytes += length;
        size -= length;
    }
    return GD32H77X_STATUS_OK;
}

static gd32h77x_status_t __aps_read(gd32h77x_psram_device_t *device,
                                    uint32_t offset, void *data, size_t size)
{
    if (!__aps_range_valid(device, offset, size) ||
        (size != 0U && data == NULL) || __aps_chunk_limit(device) == 0U) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }

    uint8_t *bytes = data;
    while (size > 0U) {
        uint32_t length = __aps_next_len(device, offset, (uint32_t)size);
        gd32h77x_status_t rt =
            __transfer(device, APS6404L_CMD_QUAD_READ, offset, true, true,
                       OSPI_DATA_4_LINES, APS6404L_READ_DUMMY, bytes, length);
        if (rt != GD32H77X_STATUS_OK) {
            return rt;
        }
        offset += length;
        bytes += length;
        size -= length;
    }
    return GD32H77X_STATUS_OK;
}

static gd32h77x_status_t __aps_reset(gd32h77x_psram_device_t *device)
{
    /* The PSRAM may still be in QPI mode after an MCU-only reset. An
     * incomplete QPI command is ignored by a device already in SPI mode. */
    gd32h77x_ospi_transfer_t command = {
        .frame = {
            .instruction = APS6404L_CMD_RESET_EN,
            .instruction_mode = OSPI_INSTRUCTION_4_LINES,
            .instruction_size = OSPI_INSTRUCTION_8_BITS,
            .address_mode = OSPI_ADDRESS_NONE,
            .address_size = OSPI_ADDRESS_24_BITS,
            .data_mode = OSPI_DATA_NONE,
        },
    };
    gd32h77x_status_t rt =
        gd32h77x_ospi_transfer(&device->controller, &command);
    if (rt != GD32H77X_STATUS_OK) {
        return rt;
    }
    command.frame.instruction = APS6404L_CMD_RESET;
    rt = gd32h77x_ospi_transfer(&device->controller, &command);
    if (rt != GD32H77X_STATUS_OK) {
        return rt;
    }

    command.frame.instruction = APS6404L_CMD_RESET_EN;
    command.frame.instruction_mode = OSPI_INSTRUCTION_1_LINE;
    rt = gd32h77x_ospi_transfer(&device->controller, &command);
    if (rt != GD32H77X_STATUS_OK) {
        return rt;
    }
    command.frame.instruction = APS6404L_CMD_RESET;
    rt = gd32h77x_ospi_transfer(&device->controller, &command);
    if (rt == GD32H77X_STATUS_OK) {
        vTaskDelay(pdMS_TO_TICKS(1U));
    }
    return rt;
}

static gd32h77x_status_t __aps_probe(gd32h77x_psram_device_t *device)
{
    uint8_t id[8] = {0};
    gd32h77x_ospi_transfer_t transfer = {
        .frame = {
            .instruction = APS6404L_CMD_READ_ID,
            .instruction_mode = OSPI_INSTRUCTION_1_LINE,
            .instruction_size = OSPI_INSTRUCTION_8_BITS,
            .address_mode = OSPI_ADDRESS_1_LINE,
            .address_size = OSPI_ADDRESS_24_BITS,
            .data_mode = OSPI_DATA_1_LINE,
        },
        .address = 0U,
        .has_address = true,
        .read = true,
        .data = id,
        .size = sizeof(id),
    };
    gd32h77x_status_t rt =
        gd32h77x_ospi_transfer(&device->controller, &transfer);
    memcpy(device->last_probe_data, id, sizeof(id));
    device->last_probe_data_len = sizeof(id);
    device->last_id = id[1];
    if (rt != GD32H77X_STATUS_OK) {
        return rt;
    }
    return id[0] == APS6404L_MF_ID && id[1] == APS6404L_KGD_PASS
               ? GD32H77X_STATUS_OK
               : GD32H77X_STATUS_UNSUPPORTED;
}

static gd32h77x_status_t __aps_selftest(gd32h77x_psram_device_t *device)
{
    static const uint32_t offsets[] = {
        0x000000U,
        0x0003F0U,
        0x123456U,
        8U * 1024U * 1024U - 64U,
    };
    uint8_t write_data[64];
    uint8_t read_data[64];

    for (uint32_t test = 0U;
         test < sizeof(offsets) / sizeof(offsets[0]); ++test) {
        for (uint32_t index = 0U; index < sizeof(write_data); ++index) {
            write_data[index] = (uint8_t)(index * 7U + test * 31U + 0x5AU);
        }
        memset(read_data, 0, sizeof(read_data));
        gd32h77x_status_t rt = __aps_write(
            device, offsets[test], write_data, sizeof(write_data));
        if (rt != GD32H77X_STATUS_OK) {
            return rt;
        }
        rt = __aps_read(device, offsets[test], read_data,
                        sizeof(read_data));
        if (rt != GD32H77X_STATUS_OK) {
            return rt;
        }
        if (memcmp(write_data, read_data, sizeof(write_data)) != 0) {
            return GD32H77X_STATUS_IO_ERROR;
        }
    }
    return GD32H77X_STATUS_OK;
}

static gd32h77x_status_t __aps_configure_memory_mapped(
    gd32h77x_psram_device_t *device)
{
    uint32_t tcem_cycles =
        (uint32_t)(((uint64_t)device->clock_hz * APS6404L_TCEM_NS) /
                   1000000000ULL);
    gd32h77x_ospi_memory_mapped_t config = {
        .read_frame = {
            .instruction = APS6404L_CMD_QUAD_READ,
            .instruction_mode = OSPI_INSTRUCTION_1_LINE,
            .instruction_size = OSPI_INSTRUCTION_8_BITS,
            .address_mode = OSPI_ADDRESS_4_LINES,
            .address_size = OSPI_ADDRESS_24_BITS,
            .data_mode = OSPI_DATA_4_LINES,
            .dummy_cycles = APS6404L_READ_DUMMY,
        },
        .write_frame = {
            .instruction = APS6404L_CMD_QUAD_WRITE,
            .instruction_mode = OSPI_INSTRUCTION_1_LINE,
            .instruction_size = OSPI_INSTRUCTION_8_BITS,
            .address_mode = OSPI_ADDRESS_4_LINES,
            .address_size = OSPI_ADDRESS_24_BITS,
            .data_mode = OSPI_DATA_4_LINES,
        },
        .timeout_cycles = APS6404L_MMAP_TIMEOUT,
        .refresh_cycles = tcem_cycles / 2U,
    };
    return gd32h77x_ospi_enter_memory_mapped(&device->controller, &config);
}

const gd32h77x_psram_profile_ops_t gd32h77x_psram_aps6404l_ops = {
    .name = "APS6404L-3SQR",
    .capacity_bytes = APS6404L_CAPACITY_BYTES,
    .max_clock_hz = APS6404L_MAX_CLOCK_HZ,
    .probe_clock_hz = APS6404L_PROBE_CLOCK_HZ,
    .timing = &s_aps6404l_timing,
    .reset = __aps_reset,
    .probe = __aps_probe,
    .selftest = __aps_selftest,
    .read = __aps_read,
    .write = __aps_write,
    .configure_memory_mapped = __aps_configure_memory_mapped,
};
