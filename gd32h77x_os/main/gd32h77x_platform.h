/**
 * @file gd32h77x_platform.h
 * @brief GD32H77x platform bring-up and FreeRTOS adaptation.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#ifndef GD32H77X_PLATFORM_H
#define GD32H77X_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

void     gd32h77x_platform_init(void);
void     gd32h77x_platform_log_write(const char *data, size_t size);
void     gd32h77x_platform_panic(const char *message);
uint32_t gd32h77x_platform_reset_flags(void);

#endif
