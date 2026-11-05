/**
 * @file gd32h77x_ecnvm.h
 * @brief Runtime access to the board-layout-mapped GD32H77X ECNVM.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */
#ifndef GD32H77X_ECNVM_H
#define GD32H77X_ECNVM_H

#include "gd32h77x_memory_layout.h"

#include "gd32h77x_status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GD32H77X_ECNVM_ERASE_SIZE      UINT32_C(0x00001000)
#define GD32H77X_ECNVM_PROGRAM_SIZE    UINT32_C(8)
#define GD32H77X_ECNVM_PROGRAM_32_SIZE UINT32_C(32)

static inline bool gd32h77x_ecnvm_source_range_valid(const void *source, size_t size)
{
    const uintptr_t start = (uintptr_t)source;
    const uintptr_t ram_end = (uintptr_t)GD32H77X_LAYOUT_AXISRAM_BASE +
                              (uintptr_t)GD32H77X_LAYOUT_AXISRAM_SIZE;
    if (size == 0U || start < (uintptr_t)GD32H77X_LAYOUT_AXISRAM_BASE ||
        start > ram_end || size > ram_end - start) {
        return false;
    }
    return true;
}

bool gd32h77x_ecnvm_is_present(void);
gd32h77x_status_t gd32h77x_ecnvm_init(void);
bool gd32h77x_ecnvm_is_initialized(void);
gd32h77x_status_t gd32h77x_ecnvm_read(uint32_t address, uint8_t *destination, uint32_t size);
gd32h77x_status_t gd32h77x_ecnvm_program(uint32_t address, const uint8_t *source, uint32_t size);
gd32h77x_status_t gd32h77x_ecnvm_erase(uint32_t address, uint32_t size);

#endif /* GD32H77X_ECNVM_H */
