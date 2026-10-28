/**
 * @file gd32h77x_cnvm.h
 * @brief Runtime access to the board-layout-mapped GD32H77X CNVM.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */
#ifndef GD32H77X_CNVM_H
#define GD32H77X_CNVM_H

#include "tuya_cloud_types.h"

#include <stdbool.h>
#include <stdint.h>

#define GD32H77X_CNVM_ERASE_SIZE      UINT32_C(0x00010000)
#define GD32H77X_CNVM_PROGRAM_SIZE    UINT32_C(32)

OPERATE_RET gd32h77x_cnvm_init(void);
bool gd32h77x_cnvm_is_initialized(void);
OPERATE_RET gd32h77x_cnvm_read(uint32_t address, uint8_t *destination, uint32_t size);
OPERATE_RET gd32h77x_cnvm_program(uint32_t address, const uint8_t *source, uint32_t size);
OPERATE_RET gd32h77x_cnvm_erase(uint32_t address, uint32_t size);

#endif /* GD32H77X_CNVM_H */
