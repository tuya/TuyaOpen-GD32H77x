/**
 * @file mock_cnvm.c
 * @brief Host mock of the GD32H77X mapped CNVM adapter.
 */
#include "gd32h77x_cnvm.h"
#include "gd32h77x_memory_layout.h"
#include "mock_cnvm.h"

#include <stddef.h>
#include <string.h>

#define MOCK_CNVM_MAX_SIZE UINT32_C(0x00400000)
static uint8_t s_memory[MOCK_CNVM_MAX_SIZE];
static mock_cnvm_stats_t s_stats;
static bool s_initialized;
static bool s_busy;
static bool s_dieen;
static bool s_fail_read;
static bool s_fail_program;
static bool s_fail_erase;

static bool mock_cnvm_range_valid(uint32_t address, uint32_t size,
                                  uint32_t base, uint32_t capacity)
{
    const uint32_t end = base + capacity;
    return address >= base && address <= end && size <= end - address;
}

static const gd32h77x_layout_device_t *mock_cnvm_device_for_physical_range(
    uint32_t address, uint32_t size)
{
    for (uint32_t i = 0U; i < GD32H77X_LAYOUT_DEVICE_COUNT; ++i) {
        const gd32h77x_layout_device_t *device = &gd32h77x_layout_devices[i];
        if (device->region != NULL && strcmp(device->region, "CNVM") == 0 &&
            device->access == GD32H77X_LAYOUT_ACCESS_MAPPED &&
            mock_cnvm_range_valid(address, size, device->cpu_base,
                                  device->capacity_bytes)) {
            return device;
        }
    }
    return NULL;
}

static const gd32h77x_layout_device_t *mock_cnvm_kv_device_for_range(
    uint32_t address, uint32_t size)
{
    const gd32h77x_layout_partition_t *partition =
        &gd32h77x_layout_partitions[GD32H77X_LAYOUT_PART_KV];
    const uint64_t end = (uint64_t)address + size;
    if (size == 0U || end > (uint64_t)UINT32_MAX + 1U) {
        return NULL;
    }
    for (uint32_t i = 0U; i < partition->segment_count; ++i) {
        const uint32_t segment_index = partition->first_segment + i;
        if (segment_index >= GD32H77X_LAYOUT_SEGMENT_COUNT) {
            return NULL;
        }
        const gd32h77x_layout_segment_t *segment =
            &gd32h77x_layout_segments[segment_index];
        if (segment->device_index >= GD32H77X_LAYOUT_DEVICE_COUNT) {
            return NULL;
        }
        const gd32h77x_layout_device_t *device =
            &gd32h77x_layout_devices[segment->device_index];
        const uint64_t base = (uint64_t)device->cpu_base + segment->device_offset_bytes;
        if (device->region != NULL && strcmp(device->region, "CNVM") == 0 &&
            device->access == GD32H77X_LAYOUT_ACCESS_MAPPED &&
            address >= base && end <= base + segment->size_bytes) {
            return device;
        }
    }
    return NULL;
}

static uint32_t mock_cnvm_memory_base(void)
{
    for (uint32_t i = 0U; i < GD32H77X_LAYOUT_DEVICE_COUNT; ++i) {
        const gd32h77x_layout_device_t *device = &gd32h77x_layout_devices[i];
        if (device->region != NULL && strcmp(device->region, "CNVM") == 0 &&
            device->access == GD32H77X_LAYOUT_ACCESS_MAPPED) {
            return device->cpu_base;
        }
    }
    return 0U;
}

static bool mock_cnvm_program_valid(uint32_t address, uint32_t size)
{
    const gd32h77x_layout_device_t *device =
        mock_cnvm_kv_device_for_range(address, size);
    return device != NULL && device->program_bytes != 0U &&
           address % device->program_bytes == 0U &&
           size % device->program_bytes == 0U;
}

static bool mock_cnvm_erase_valid(uint32_t address, uint32_t size)
{
    const gd32h77x_layout_device_t *device =
        mock_cnvm_kv_device_for_range(address, size);
    return device != NULL && device->erase_bytes != 0U &&
           address % device->erase_bytes == 0U &&
           size % device->erase_bytes == 0U;
}

void mock_cnvm_reset(void)
{
    memset(s_memory, 0xFF, sizeof(s_memory));
    memset(&s_stats, 0, sizeof(s_stats));
    s_initialized = false;
    s_busy = false;
    s_dieen = false;
    s_fail_read = false;
    s_fail_program = false;
    s_fail_erase = false;
}

void mock_cnvm_set_busy(bool busy)
{
    s_busy = busy;
}

void mock_cnvm_set_dieen(bool enabled)
{
    s_dieen = enabled;
}

void mock_cnvm_fail_next_read(void)
{
    s_fail_read = true;
}

void mock_cnvm_fail_next_program(void)
{
    s_fail_program = true;
}

void mock_cnvm_fail_next_erase(void)
{
    s_fail_erase = true;
}

const mock_cnvm_stats_t *mock_cnvm_stats(void)
{
    return &s_stats;
}

gd32h77x_status_t gd32h77x_cnvm_init(void)
{
    if (s_busy) {
        return GD32H77X_STATUS_NOT_READY;
    }
    s_initialized = true;
    return GD32H77X_STATUS_OK;
}

bool gd32h77x_cnvm_is_initialized(void)
{
    return s_initialized;
}

gd32h77x_status_t gd32h77x_cnvm_read(uint32_t address, uint8_t *destination, uint32_t size)
{
    ++s_stats.read_requests;
    s_stats.last_address = address;
    s_stats.last_size = size;
    if ((size != 0U && destination == NULL) ||
        mock_cnvm_device_for_physical_range(address, size) == NULL) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    if (s_fail_read) {
        s_fail_read = false;
        return GD32H77X_STATUS_IO_ERROR;
    }
    memcpy(destination, &s_memory[address - mock_cnvm_memory_base()], size);
    return GD32H77X_STATUS_OK;
}

gd32h77x_status_t gd32h77x_cnvm_program(uint32_t address, const uint8_t *source, uint32_t size)
{
    ++s_stats.program_requests;
    s_stats.last_address = address;
    s_stats.last_size = size;
    if (size == 0U) {
        return GD32H77X_STATUS_OK;
    }
    if (source == NULL || !mock_cnvm_program_valid(address, size)) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    if (!s_initialized || s_busy) {
        return GD32H77X_STATUS_NOT_READY;
    }
    if (s_dieen) {
        return GD32H77X_STATUS_UNSUPPORTED;
    }

    ++s_stats.program_commands;
    if (s_fail_program) {
        s_fail_program = false;
        return GD32H77X_STATUS_IO_ERROR;
    }
    uint8_t *destination = &s_memory[address - mock_cnvm_memory_base()];
    for (uint32_t i = 0U; i < size; ++i) {
        if ((destination[i] & source[i]) != source[i]) {
            return GD32H77X_STATUS_IO_ERROR;
        }
    }
    for (uint32_t i = 0U; i < size; ++i) {
        destination[i] &= source[i];
    }
    return GD32H77X_STATUS_OK;
}

gd32h77x_status_t gd32h77x_cnvm_erase(uint32_t address, uint32_t size)
{
    ++s_stats.erase_requests;
    s_stats.last_address = address;
    s_stats.last_size = size;
    if (!mock_cnvm_erase_valid(address, size)) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    if (!s_initialized || s_busy) {
        return GD32H77X_STATUS_NOT_READY;
    }
    if (s_dieen) {
        return GD32H77X_STATUS_UNSUPPORTED;
    }

    s_stats.erase_commands += size / GD32H77X_CNVM_ERASE_SIZE;
    if (s_fail_erase) {
        s_fail_erase = false;
        return GD32H77X_STATUS_IO_ERROR;
    }
    memset(&s_memory[address - mock_cnvm_memory_base()], 0xFF, size);
    return GD32H77X_STATUS_OK;
}
