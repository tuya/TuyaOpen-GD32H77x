/**
 * @file tkl_freertos.h
 * @brief GD32H77x platform bring-up and FreeRTOS adaptation.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#ifndef TKL_FREERTOS_H
#define TKL_FREERTOS_H

#include "FreeRTOS.h"
#include "task.h"
#include <stdint.h>

static inline TickType_t tkl_timeout_ticks(uint32_t milliseconds)
{
    if (milliseconds == UINT32_MAX) {
        return portMAX_DELAY;
    }
    uint64_t ticks = ((uint64_t)milliseconds * configTICK_RATE_HZ + 999U) / 1000U;
    return ticks >= portMAX_DELAY ? portMAX_DELAY - 1U : (TickType_t)ticks;
}

#endif
