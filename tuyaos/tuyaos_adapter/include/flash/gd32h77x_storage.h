/**
 * @file gd32h77x_storage.h
 * @brief Layout-derived KV storage boundary shared by the TKL and host checks.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */
#ifndef GD32H77X_STORAGE_H
#define GD32H77X_STORAGE_H

#include "gd32h77x_memory_layout.h"

#include <stdbool.h>
#include <stdint.h>

/* Validates a half-open range without allowing integer wraparound. */
bool gd32h77x_tuya_range_valid(uint32_t address, uint32_t size);
bool gd32h77x_tuya_program_range_valid(uint32_t address, uint32_t size);
bool gd32h77x_tuya_erase_range_valid(uint32_t address, uint32_t size);

#endif
