/** @file mock_ecnvm.c @brief Host mock for the mapped ECNVM adapter. */
#include "gd32h77x_ecnvm.h"
#include "gd32h77x_memory_layout.h"
#include "mock_ecnvm.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define MOCK_ECNVM_MAX_SIZE UINT32_C(0x00780000)
static uint8_t s_memory[MOCK_ECNVM_MAX_SIZE];
static mock_ecnvm_stats_t s_stats;
static bool s_initialized;

static const gd32h77x_layout_device_t *mock_ecnvm_device(void)
{
    for (uint32_t i = 0U; i < GD32H77X_LAYOUT_DEVICE_COUNT; ++i) {
        const gd32h77x_layout_device_t *device = &gd32h77x_layout_devices[i];
        if (device->region != NULL && strcmp(device->region, "ECNVM") == 0 &&
            device->access == GD32H77X_LAYOUT_ACCESS_MAPPED) {
            return device;
        }
    }
    return NULL;
}

static bool mock_ecnvm_range_valid(uint32_t address, uint32_t size)
{
    const gd32h77x_layout_device_t *device = mock_ecnvm_device();
    const uint64_t end = (uint64_t)address + size;
    return device != NULL && size != 0U && address >= device->cpu_base &&
           end <= (uint64_t)device->cpu_base + device->capacity_bytes;
}

void mock_ecnvm_reset(void)
{
    memset(s_memory, 0xFF, sizeof(s_memory));
    memset(&s_stats, 0, sizeof(s_stats));
    s_initialized = false;
}

const mock_ecnvm_stats_t *mock_ecnvm_stats(void)
{
    return &s_stats;
}

bool gd32h77x_ecnvm_is_present(void)
{
    return mock_ecnvm_device() != NULL;
}

gd32h77x_status_t gd32h77x_ecnvm_init(void)
{
    if (!gd32h77x_ecnvm_is_present()) {
        return GD32H77X_STATUS_UNSUPPORTED;
    }
    s_initialized = true;
    return GD32H77X_STATUS_OK;
}

bool gd32h77x_ecnvm_is_initialized(void)
{
    return s_initialized;
}

gd32h77x_status_t gd32h77x_ecnvm_read(uint32_t address, uint8_t *destination, uint32_t size)
{
    ++s_stats.read_requests;
    s_stats.last_address = address;
    s_stats.last_size = size;
    if ((size != 0U && destination == NULL) || !mock_ecnvm_range_valid(address, size)) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    const gd32h77x_layout_device_t *device = mock_ecnvm_device();
    memcpy(destination, &s_memory[address - device->cpu_base], size);
    return GD32H77X_STATUS_OK;
}

gd32h77x_status_t gd32h77x_ecnvm_program(uint32_t address, const uint8_t *source, uint32_t size)
{
    ++s_stats.program_requests;
    s_stats.last_address = address;
    s_stats.last_size = size;
    if (size == 0U) {
        return GD32H77X_STATUS_OK;
    }
    const gd32h77x_layout_device_t *device = mock_ecnvm_device();
    const bool source_valid =
#if defined(GD32H77X_ECNVM_HOST_TEST)
        source != NULL &&
        !((uintptr_t)source <
              (uintptr_t)(mock_ecnvm_device()->cpu_base + mock_ecnvm_device()->capacity_bytes) &&
          (uintptr_t)source + size > (uintptr_t)mock_ecnvm_device()->cpu_base);
#else
        gd32h77x_ecnvm_source_range_valid(source, size);
#endif
    if (source == NULL || device == NULL || address % device->program_bytes != 0U ||
        size % device->program_bytes != 0U || !mock_ecnvm_range_valid(address, size) ||
        !source_valid) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    if (!s_initialized) {
        return GD32H77X_STATUS_NOT_READY;
    }
    s_stats.program_commands += (size + GD32H77X_ECNVM_PROGRAM_32_SIZE - 1U) /
                                GD32H77X_ECNVM_PROGRAM_32_SIZE;
    uint8_t *destination = &s_memory[address - device->cpu_base];
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

gd32h77x_status_t gd32h77x_ecnvm_erase(uint32_t address, uint32_t size)
{
    ++s_stats.erase_requests;
    s_stats.last_address = address;
    s_stats.last_size = size;
    const gd32h77x_layout_device_t *device = mock_ecnvm_device();
    if (device == NULL || size == 0U || address % device->erase_bytes != 0U ||
        size % device->erase_bytes != 0U || !mock_ecnvm_range_valid(address, size)) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    if (!s_initialized) {
        return GD32H77X_STATUS_NOT_READY;
    }
    s_stats.erase_commands += size / device->erase_bytes;
    memset(&s_memory[address - device->cpu_base], 0xFF, size);
    return GD32H77X_STATUS_OK;
}
