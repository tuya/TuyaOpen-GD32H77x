/**
 * @file gd32h77x_cnvm.c
 * @brief Fail-closed runtime CNVM access constrained by the resolved KV layout.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */
#include "gd32h77x_cnvm.h"
#include "gd32h77x_memory_layout.h"
#include "gd32h77x_status.h"

#include "gd32h77x_78x_nvmc.h"
#include "cachel1_armv7.h"

#include <stddef.h>
#include <string.h>

enum {
    GD32H77X_CNVM_RAM_OK = 0,
    GD32H77X_CNVM_RAM_INVALID,
    GD32H77X_CNVM_RAM_UNSUPPORTED,
    GD32H77X_CNVM_RAM_ERROR,
};

enum {
    GD32H77X_CNVM_COMMAND_PROGRAM = 1,
    GD32H77X_CNVM_COMMAND_ERASE = 2,
};

#define GD32H77X_CNVM_STATUS_FLAGS \
    (NVMC_CNVM_STAT_ENDF | NVMC_CNVM_STAT_WPERR | NVMC_CNVM_STAT_PGSERR | \
     NVMC_CNVM_STAT_RPERR | NVMC_CNVM_STAT_RSERR)
#define GD32H77X_CNVM_ERROR_FLAGS \
    (NVMC_CNVM_STAT_WPERR | NVMC_CNVM_STAT_PGSERR | NVMC_CNVM_STAT_RPERR | \
     NVMC_CNVM_STAT_RSERR)

static volatile uint32_t s_initialized;

/*
 * This is deliberately one self-contained RAM function. In particular, do
 * not call the vendor nvmc_cnvm_* routines here: their code executes from
 * CNVM, which cannot be fetched safely while a CNVM command is busy.
 */
__attribute__((section(".ramfunc"), noinline, used))
static int gd32h77x_cnvm_execute_ram(uint32_t address, const uint8_t *source,
                                    uint32_t size, uint32_t command,
                                    uint32_t program_size, uint32_t erase_size)
{
    const uint32_t primask = __get_PRIMASK();
    uint32_t cursor = address;
    uint32_t remaining = size;
    uint32_t attempted = 0U;
    uint32_t result = GD32H77X_CNVM_RAM_OK;
    uint32_t status;
    uint32_t control;

    __disable_irq();
    __DSB();
    __ISB();

    /* Repeat the option-byte check after entering SRAM and masking IRQs. */
    if ((NVMC_OBREPCTL_EFT & NVMC_OBREPCTL_EFT_DIEEN) != 0U) {
        result = GD32H77X_CNVM_RAM_UNSUPPORTED;
        goto restore_interrupts;
    }

    /* Do not leave SRAM until any earlier CNVM operation has completed. */
    while ((NVMC_CNVM_STAT & NVMC_CNVM_STAT_BUSY) != 0U) {
    }

    control = NVMC_CNVM_CTL;
    if ((control & NVMC_CNVM_CTL_LK) != 0U) {
        NVMC_CNVM_KEY = UNLOCK_KEY0;
        NVMC_CNVM_KEY = UNLOCK_KEY1;
    }

    while (remaining != 0U) {
        uint32_t flag_status = NVMC_CNVM_STAT;
        NVMC_CNVM_STAT = (flag_status & ~GD32H77X_CNVM_STATUS_FLAGS) |
                         GD32H77X_CNVM_STATUS_FLAGS;

        if (command == GD32H77X_CNVM_COMMAND_PROGRAM) {
            NVMC_CNVM_CTL |= NVMC_CNVM_CTL_PG;
            __DSB();
            __ISB();

            /* A CNVM program command is exactly four aligned doublewords. */
            for (uint32_t word = 0U; word < program_size; word += 8U) {
                const uint32_t low =
                    (uint32_t)source[0] | ((uint32_t)source[1] << 8U) |
                    ((uint32_t)source[2] << 16U) | ((uint32_t)source[3] << 24U);
                const uint32_t high =
                    (uint32_t)source[4] | ((uint32_t)source[5] << 8U) |
                    ((uint32_t)source[6] << 16U) | ((uint32_t)source[7] << 24U);

                __asm volatile("strd %0, %1, [%2]"
                               :
                               : "r"(low), "r"(high), "r"(cursor + word)
                               : "memory");
                __DMB();
                source += 8U;
            }
            cursor += program_size;
            remaining -= program_size;
        } else {
            NVMC_CNVM_CTL |= NVMC_CNVM_CTL_SER;
            NVMC_CNVM_ADDR = cursor;
            NVMC_CNVM_CTL |= NVMC_CNVM_CTL_START;
            cursor += erase_size;
            remaining -= erase_size;
        }

        attempted = 1U;
        __DSB();
        __ISB();

        /* Match the vendor start-edge allowance, then never escape while busy. */
        for (uint32_t start_wait = 0U;
             start_wait < 0xFFU &&
             (NVMC_CNVM_STAT & NVMC_CNVM_STAT_BUSY) == 0U;
             ++start_wait) {
        }
        while ((NVMC_CNVM_STAT & NVMC_CNVM_STAT_BUSY) != 0U) {
        }

        control = NVMC_CNVM_CTL;
        control &= ~(NVMC_CNVM_CTL_PG | NVMC_CNVM_CTL_SER | NVMC_CNVM_CTL_START);
        NVMC_CNVM_CTL = control;

        status = NVMC_CNVM_STAT;
        if ((status & GD32H77X_CNVM_ERROR_FLAGS) != 0U) {
            NVMC_CNVM_STAT = (status & ~GD32H77X_CNVM_STATUS_FLAGS) |
                             GD32H77X_CNVM_ERROR_FLAGS;
            result = GD32H77X_CNVM_RAM_ERROR;
            break;
        }

        status &= ~GD32H77X_CNVM_STATUS_FLAGS;
        NVMC_CNVM_STAT = status | NVMC_CNVM_STAT_ENDF;
    }

    /* Re-lock the controller even when a command reports a hardware error. */
    NVMC_CNVM_CTL |= NVMC_CNVM_CTL_LK;

    if (attempted != 0U) {
        SCB_InvalidateDCache_by_Addr((volatile void *)address, (int32_t)size);
        SCB_InvalidateICache_by_Addr((volatile void *)address, (int32_t)size);
        __DSB();
        __ISB();
    }

restore_interrupts:
    __set_PRIMASK(primask);
    return (int)result;
}

static bool gd32h77x_cnvm_physical_range_valid(uint32_t address, uint32_t size)
{
    const uint64_t end = (uint64_t)address + size;
    if (end > (uint64_t)UINT32_MAX + 1U) {
        return false;
    }
    for (uint32_t i = 0U; i < GD32H77X_LAYOUT_DEVICE_COUNT; ++i) {
        const gd32h77x_layout_device_t *device = &gd32h77x_layout_devices[i];
        const uint64_t base = device->cpu_base;
        const uint64_t device_end = base + device->capacity_bytes;
        if (device->region != NULL && strcmp(device->region, "CNVM") == 0 &&
            device->access == GD32H77X_LAYOUT_ACCESS_MAPPED && base != 0U &&
            address >= base && end <= device_end) {
            return true;
        }
    }
    return size == 0U;
}

/* Resolve a physical chunk against any board-declared CNVM device. */
static const gd32h77x_layout_device_t *gd32h77x_cnvm_device_for_range(
    uint32_t address, uint32_t size)
{
    const uint64_t end = (uint64_t)address + size;
    if (size == 0U || end > (uint64_t)UINT32_MAX + 1U) {
        return NULL;
    }
    for (uint32_t i = 0U; i < GD32H77X_LAYOUT_DEVICE_COUNT; ++i) {
        const gd32h77x_layout_device_t *device = &gd32h77x_layout_devices[i];
        if (device->region == NULL || strcmp(device->region, "CNVM") != 0 ||
            device->access != GD32H77X_LAYOUT_ACCESS_MAPPED || device->cpu_base == 0U) {
            continue;
        }
        const uint64_t base = device->cpu_base;
        const uint64_t device_end = base + device->capacity_bytes;
        if (address >= base && end <= device_end) {
            return device;
        }
    }
    return NULL;
}

gd32h77x_status_t gd32h77x_cnvm_init(void)
{
    if ((NVMC_CNVM_STAT & NVMC_CNVM_STAT_BUSY) != 0U) {
        return GD32H77X_STATUS_NOT_READY;
    }
    s_initialized = 1U;
    return GD32H77X_STATUS_OK;
}

bool gd32h77x_cnvm_is_initialized(void)
{
    return s_initialized != 0U;
}

gd32h77x_status_t gd32h77x_cnvm_read(uint32_t address, uint8_t *destination, uint32_t size)
{
    if ((size != 0U && destination == NULL) ||
        !gd32h77x_cnvm_physical_range_valid(address, size)) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    for (uint32_t i = 0U; i < size; ++i) {
        destination[i] = ((const volatile uint8_t *)(uintptr_t)address)[i];
    }
    return GD32H77X_STATUS_OK;
}

gd32h77x_status_t gd32h77x_cnvm_program(uint32_t address, const uint8_t *source, uint32_t size)
{
    if (size == 0U) {
        return GD32H77X_STATUS_OK;
    }
    const gd32h77x_layout_device_t *device =
        gd32h77x_cnvm_device_for_range(address, size);
    if (source == NULL || device == NULL || device->program_bytes == 0U ||
        (device->program_bytes % 8U) != 0U ||
        (address % device->program_bytes) != 0U ||
        (size % device->program_bytes) != 0U ||
        !gd32h77x_cnvm_physical_range_valid(address, size)) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    if (!gd32h77x_cnvm_is_initialized()) {
        return GD32H77X_STATUS_NOT_READY;
    }
    if ((NVMC_CNVM_STAT & NVMC_CNVM_STAT_BUSY) != 0U) {
        return GD32H77X_STATUS_NOT_READY;
    }
    if ((NVMC_OBREPCTL_EFT & NVMC_OBREPCTL_EFT_DIEEN) != 0U) {
        return GD32H77X_STATUS_UNSUPPORTED;
    }

    const int result = gd32h77x_cnvm_execute_ram(
        address, source, size, GD32H77X_CNVM_COMMAND_PROGRAM,
        device->program_bytes, device->erase_bytes);
    if (result == GD32H77X_CNVM_RAM_UNSUPPORTED) {
        return GD32H77X_STATUS_UNSUPPORTED;
    }
    return result == GD32H77X_CNVM_RAM_OK ? GD32H77X_STATUS_OK : GD32H77X_STATUS_IO_ERROR;
}

gd32h77x_status_t gd32h77x_cnvm_erase(uint32_t address, uint32_t size)
{
    const gd32h77x_layout_device_t *device =
        gd32h77x_cnvm_device_for_range(address, size);
    if (device == NULL || device->erase_bytes == 0U ||
        (address % device->erase_bytes) != 0U ||
        (size % device->erase_bytes) != 0U ||
        !gd32h77x_cnvm_physical_range_valid(address, size)) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    if (!gd32h77x_cnvm_is_initialized()) {
        return GD32H77X_STATUS_NOT_READY;
    }
    if ((NVMC_CNVM_STAT & NVMC_CNVM_STAT_BUSY) != 0U) {
        return GD32H77X_STATUS_NOT_READY;
    }
    if ((NVMC_OBREPCTL_EFT & NVMC_OBREPCTL_EFT_DIEEN) != 0U) {
        return GD32H77X_STATUS_UNSUPPORTED;
    }

    const int result = gd32h77x_cnvm_execute_ram(
        address, NULL, size, GD32H77X_CNVM_COMMAND_ERASE,
        device->program_bytes, device->erase_bytes);
    if (result == GD32H77X_CNVM_RAM_UNSUPPORTED) {
        return GD32H77X_STATUS_UNSUPPORTED;
    }
    return result == GD32H77X_CNVM_RAM_OK ? GD32H77X_STATUS_OK : GD32H77X_STATUS_IO_ERROR;
}
