/**
 * @file gd32h77x_psram_mem.h
 * @brief Private helpers for safely moving data through mapped GD32H77X PSRAM.
 *
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */
#ifndef GD32H77X_PSRAM_MEM_H
#define GD32H77X_PSRAM_MEM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *gd32h77x_psram_memcpy(void *dst, const void *src, size_t size);
void *gd32h77x_psram_memset(void *dst, int value, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* GD32H77X_PSRAM_MEM_H */
