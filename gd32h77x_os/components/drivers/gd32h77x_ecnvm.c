/**
 * @file gd32h77x_ecnvm.c
 * @brief Fail-closed runtime ECNVM access using SRAM-resident commands.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */
#include "gd32h77x_ecnvm.h"
#include "gd32h77x_memory_layout.h"
#include "gd32h77x_status.h"

#include "gd32h77x_78x_nvmc.h"
#include "cachel1_armv7.h"

#include <stddef.h>
#include <string.h>

enum {
    GD32H77X_ECNVM_RAM_OK = 0,
    GD32H77X_ECNVM_RAM_UNSUPPORTED,
    GD32H77X_ECNVM_RAM_ERROR,
};

enum {
    GD32H77X_ECNVM_COMMAND_PROGRAM = 1,
    GD32H77X_ECNVM_COMMAND_ERASE = 2,
};

#define GD32H77X_ECNVM_STATUS_FLAGS \
    (NVMC_ECNVM_STAT_ENDF | NVMC_ECNVM_STAT_WPERR | NVMC_ECNVM_STAT_PGSERR | \
     NVMC_ECNVM_STAT_RPERR | NVMC_ECNVM_STAT_RSERR | NVMC_ECNVM_STAT_ECCDET)
#define GD32H77X_ECNVM_ERROR_FLAGS \
    (NVMC_ECNVM_STAT_WPERR | NVMC_ECNVM_STAT_PGSERR | NVMC_ECNVM_STAT_RPERR | \
     NVMC_ECNVM_STAT_RSERR | NVMC_ECNVM_STAT_ECCDET)

static volatile uint32_t s_initialized;

static bool gd32h77x_ecnvm_is_mapped_device(const gd32h77x_layout_device_t *device)
{
    return device != NULL && device->region != NULL &&
           strcmp(device->region, "ECNVM") == 0 &&
           device->access == GD32H77X_LAYOUT_ACCESS_MAPPED &&
           device->cpu_base != 0U;
}

static const gd32h77x_layout_device_t *gd32h77x_ecnvm_device_for_range(
    uint32_t address, uint32_t size)
{
    const uint64_t end = (uint64_t)address + size;
    if (size == 0U || end > (uint64_t)UINT32_MAX + 1U) {
        return NULL;
    }
    for (uint32_t i = 0U; i < GD32H77X_LAYOUT_DEVICE_COUNT; ++i) {
        const gd32h77x_layout_device_t *device = &gd32h77x_layout_devices[i];
        if (!gd32h77x_ecnvm_is_mapped_device(device)) {
            continue;
        }
        const uint64_t base = device->cpu_base;
        if (address >= base && end <= base + device->capacity_bytes) {
            return device;
        }
    }
    return NULL;
}

static bool gd32h77x_ecnvm_physical_range_valid(uint32_t address, uint32_t size)
{
    return size == 0U || gd32h77x_ecnvm_device_for_range(address, size) != NULL;
}

/*
 * This self-contained function executes from AXI SRAM. The current application
 * text starts in DECNVM0, while the KV target is the separate Data ECNVM1 region
 * (MAIN_ECNVM_DECNVM1_BASE_ADDRESS=0x08900000, size=0x80000 on GD32H779VW).
 * No ECNVM instruction or data fetch is allowed between START and completion:
 * source bytes are staged on the SRAM stack, interrupts are masked, and the
 * routine does not return until BUSY clears.
 */
__attribute__((section(".ramfunc"), noinline, used))
static int gd32h77x_ecnvm_execute_ram(uint32_t address, const uint8_t *source,
                                     uint32_t size, uint32_t command,
                                     uint32_t program_size, uint32_t erase_size)
{
    const uint32_t primask = __get_PRIMASK();
    uint32_t cursor = address;
    uint32_t remaining = size;
    uint32_t attempted = 0U;
    uint32_t result = GD32H77X_ECNVM_RAM_OK;
    uint32_t control;
    uint32_t status;

    __disable_irq();
    __DSB();
    __ISB();

    /* Data-integrity-enhancement mode reserves ECNVM banks for CNVM repair. */
    if ((NVMC_OBREPCTL_EFT & NVMC_OBREPCTL_EFT_DIEEN) != 0U) {
        result = GD32H77X_ECNVM_RAM_UNSUPPORTED;
        goto restore_interrupts;
    }

    while ((NVMC_ECNVM_STAT & NVMC_ECNVM_STAT_BUSY) != 0U) {
    }

    control = NVMC_ECNVM_CTL;
    if ((control & NVMC_ECNVM_CTL_LK) != 0U) {
        NVMC_ECNVM_KEY = UNLOCK_KEY0;
        NVMC_ECNVM_KEY = UNLOCK_KEY1;
    }

    while (remaining != 0U) {
        status = NVMC_ECNVM_STAT;
        NVMC_ECNVM_STAT = (status & ~GD32H77X_ECNVM_STATUS_FLAGS) |
                          GD32H77X_ECNVM_STATUS_FLAGS;

        if (command == GD32H77X_ECNVM_COMMAND_PROGRAM) {
            uint64_t staged[4];
            uint32_t unit = program_size;

            if (remaining >= GD32H77X_ECNVM_PROGRAM_32_SIZE &&
                (cursor % GD32H77X_ECNVM_PROGRAM_32_SIZE) == 0U) {
                unit = GD32H77X_ECNVM_PROGRAM_32_SIZE;
            }

            /* Snapshot from caller RAM before enabling the program command. */
            for (uint32_t i = 0U; i < unit; ++i) {
                ((volatile uint8_t *)staged)[i] = source[i];
            }

            NVMC_PERFCTL &= ~NVMC_PERFCTL_ECNVM_PG_SZ;
            NVMC_PERFCTL |= unit == GD32H77X_ECNVM_PROGRAM_32_SIZE
                                ? ECNVM_PROG_SIZE_32BYTES
                                : ECNVM_PROG_SIZE_8BYTES;
            NVMC_ECNVM_CTL |= NVMC_ECNVM_CTL_PG;
            __DSB();
            __ISB();

            for (uint32_t word = 0U; word < unit; word += 8U) {
                __asm volatile("strd %Q[data], %R[data], [%[addr]]"
                               :
                               : [addr] "r"(cursor + word), [data] "r"(staged[word / 8U])
                               : "memory");
                __DMB();
            }

            cursor += unit;
            remaining -= unit;
            source += unit;
        } else {
            NVMC_ECNVM_CTL |= NVMC_ECNVM_CTL_PER;
            NVMC_ECNVM_ADDR = cursor;
            NVMC_ECNVM_CTL |= NVMC_ECNVM_CTL_START;
            cursor += erase_size;
            remaining -= erase_size;
        }

        attempted = 1U;
        __DSB();
        __ISB();

        for (uint32_t start_wait = 0U;
             start_wait < 0xFFU &&
             (command == GD32H77X_ECNVM_COMMAND_ERASE) &&
             (NVMC_ECNVM_STAT & NVMC_ECNVM_STAT_BUSY) == 0U;
             ++start_wait) {
        }
        while ((NVMC_ECNVM_STAT & NVMC_ECNVM_STAT_BUSY) != 0U) {
        }

        control = NVMC_ECNVM_CTL;
        control &= ~(NVMC_ECNVM_CTL_PG | NVMC_ECNVM_CTL_PER |
                     NVMC_ECNVM_CTL_START);
        NVMC_ECNVM_CTL = control;

        status = NVMC_ECNVM_STAT;
        if ((status & GD32H77X_ECNVM_ERROR_FLAGS) != 0U) {
            NVMC_ECNVM_STAT = (status & ~GD32H77X_ECNVM_STATUS_FLAGS) |
                              GD32H77X_ECNVM_ERROR_FLAGS;
            result = GD32H77X_ECNVM_RAM_ERROR;
            break;
        }

        status &= ~GD32H77X_ECNVM_STATUS_FLAGS;
        NVMC_ECNVM_STAT = status | NVMC_ECNVM_STAT_ENDF;
    }

    NVMC_ECNVM_CTL |= NVMC_ECNVM_CTL_LK;

    if (attempted != 0U) {
        const uintptr_t cache_start = (uintptr_t)address & ~(uintptr_t)31U;
        const uintptr_t cache_end = ((uintptr_t)address + size + 31U) & ~(uintptr_t)31U;
        SCB_InvalidateDCache_by_Addr((volatile void *)cache_start,
                                     (int32_t)(cache_end - cache_start));
        SCB_InvalidateICache_by_Addr((volatile void *)cache_start,
                                     (int32_t)(cache_end - cache_start));
        __DSB();
        __ISB();
    }

restore_interrupts:
    __set_PRIMASK(primask);
    return result;
}

bool gd32h77x_ecnvm_is_present(void)
{
    for (uint32_t i = 0U; i < GD32H77X_LAYOUT_DEVICE_COUNT; ++i) {
        if (gd32h77x_ecnvm_is_mapped_device(&gd32h77x_layout_devices[i])) {
            return true;
        }
    }
    return false;
}

gd32h77x_status_t gd32h77x_ecnvm_init(void)
{
    if (!gd32h77x_ecnvm_is_present()) {
        return GD32H77X_STATUS_UNSUPPORTED;
    }
    if ((NVMC_ECNVM_STAT & NVMC_ECNVM_STAT_BUSY) != 0U) {
        return GD32H77X_STATUS_NOT_READY;
    }
    s_initialized = 1U;
    return GD32H77X_STATUS_OK;
}

bool gd32h77x_ecnvm_is_initialized(void)
{
    return s_initialized != 0U;
}

gd32h77x_status_t gd32h77x_ecnvm_read(uint32_t address, uint8_t *destination, uint32_t size)
{
    if ((size != 0U && destination == NULL) ||
        !gd32h77x_ecnvm_physical_range_valid(address, size)) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    for (uint32_t i = 0U; i < size; ++i) {
        destination[i] = ((const volatile uint8_t *)(uintptr_t)address)[i];
    }
    return GD32H77X_STATUS_OK;
}

gd32h77x_status_t gd32h77x_ecnvm_program(uint32_t address, const uint8_t *source, uint32_t size)
{
    if (size == 0U) {
        return GD32H77X_STATUS_OK;
    }
    const gd32h77x_layout_device_t *device =
        gd32h77x_ecnvm_device_for_range(address, size);
    if (source == NULL || device == NULL || device->program_bytes != 8U ||
        (address % device->program_bytes) != 0U ||
        (size % device->program_bytes) != 0U ||
        !gd32h77x_ecnvm_source_range_valid(source, size)) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    if (!gd32h77x_ecnvm_is_initialized()) {
        return GD32H77X_STATUS_NOT_READY;
    }
    if ((NVMC_ECNVM_STAT & NVMC_ECNVM_STAT_BUSY) != 0U) {
        return GD32H77X_STATUS_NOT_READY;
    }
    if ((NVMC_OBREPCTL_EFT & NVMC_OBREPCTL_EFT_DIEEN) != 0U) {
        return GD32H77X_STATUS_UNSUPPORTED;
    }

    const int result = gd32h77x_ecnvm_execute_ram(
        address, source, size, GD32H77X_ECNVM_COMMAND_PROGRAM,
        device->program_bytes, device->erase_bytes);
    if (result == GD32H77X_ECNVM_RAM_UNSUPPORTED) {
        return GD32H77X_STATUS_UNSUPPORTED;
    }
    return result == GD32H77X_ECNVM_RAM_OK ? GD32H77X_STATUS_OK : GD32H77X_STATUS_IO_ERROR;
}

gd32h77x_status_t gd32h77x_ecnvm_erase(uint32_t address, uint32_t size)
{
    const gd32h77x_layout_device_t *device =
        gd32h77x_ecnvm_device_for_range(address, size);
    if (device == NULL || device->erase_bytes != GD32H77X_ECNVM_ERASE_SIZE ||
        (address % device->erase_bytes) != 0U ||
        (size % device->erase_bytes) != 0U) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    if (!gd32h77x_ecnvm_is_initialized()) {
        return GD32H77X_STATUS_NOT_READY;
    }
    if ((NVMC_ECNVM_STAT & NVMC_ECNVM_STAT_BUSY) != 0U) {
        return GD32H77X_STATUS_NOT_READY;
    }
    if ((NVMC_OBREPCTL_EFT & NVMC_OBREPCTL_EFT_DIEEN) != 0U) {
        return GD32H77X_STATUS_UNSUPPORTED;
    }

    const int result = gd32h77x_ecnvm_execute_ram(
        address, NULL, size, GD32H77X_ECNVM_COMMAND_ERASE,
        device->program_bytes, device->erase_bytes);
    if (result == GD32H77X_ECNVM_RAM_UNSUPPORTED) {
        return GD32H77X_STATUS_UNSUPPORTED;
    }
    return result == GD32H77X_ECNVM_RAM_OK ? GD32H77X_STATUS_OK : GD32H77X_STATUS_IO_ERROR;
}
