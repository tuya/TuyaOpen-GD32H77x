/**
 * @file gd32h77x_flash_route.h
 * @brief Internal routing between software flash addresses and physical devices.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */
#ifndef GD32H77X_FLASH_ROUTE_H
#define GD32H77X_FLASH_ROUTE_H

#include "gd32h77x_memory_layout.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    GD32H77X_FLASH_OP_READ = 0,
    GD32H77X_FLASH_OP_WRITE,
    GD32H77X_FLASH_OP_ERASE,
} gd32h77x_flash_op_t;

typedef enum {
    GD32H77X_FLASH_ROUTE_OK = 0,
    GD32H77X_FLASH_ROUTE_INVALID,
    GD32H77X_FLASH_ROUTE_UNSUPPORTED,
} gd32h77x_flash_route_status_t;

typedef struct {
    const gd32h77x_layout_partition_t *partition;
    const gd32h77x_layout_segment_t *segment;
    const gd32h77x_layout_device_t *device;
    uint32_t logical_address;
    uint32_t device_offset;
    uint32_t size;
} gd32h77x_flash_route_chunk_t;

typedef bool (*gd32h77x_flash_route_callback_t)(
    const gd32h77x_flash_route_chunk_t *chunk, void *context);

/* Validate the complete range and every segment before invoking any callback. */
gd32h77x_flash_route_status_t gd32h77x_flash_preflight(
    uint32_t address, uint32_t size, gd32h77x_flash_op_t operation);
bool gd32h77x_flash_route(uint32_t address, uint32_t size,
                          gd32h77x_flash_op_t operation,
                          gd32h77x_flash_route_callback_t callback,
                          void *context);
bool gd32h77x_flash_range_valid(uint32_t partition_index, uint32_t address,
                                uint32_t size, gd32h77x_flash_op_t operation);
bool gd32h77x_flash_range_valid_any(uint32_t address, uint32_t size,
                                    gd32h77x_flash_op_t operation);

#endif
