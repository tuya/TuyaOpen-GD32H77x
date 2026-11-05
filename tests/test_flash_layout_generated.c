/**
 * @file test_flash_layout_generated.c
 * @brief Exercise routing against the actual resolved board layout.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */
#include "gd32h77x_flash_route.h"
#include "gd32h77x_storage.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
    uint32_t device_index;
    int nor_binding_seen = 0;
    const gd32h77x_layout_partition_t *kv =
        &gd32h77x_layout_partitions[GD32H77X_LAYOUT_PART_TUYA];

    for (device_index = 0U; device_index < GD32H77X_LAYOUT_DEVICE_COUNT; ++device_index) {
        if (strcmp(gd32h77x_layout_devices[device_index].geometry_id, "GD25Q64ESIG") == 0) {
            nor_binding_seen = 1;
        }
    }
    assert(!nor_binding_seen);
    assert(kv->logical_base == GD32H77X_LAYOUT_TUYA_LOGICAL_BASE);
    assert(kv->size_bytes == GD32H77X_LAYOUT_TUYA_SIZE_BYTES);
    assert(kv->logical_base + kv->size_bytes == GD32H77X_LAYOUT_TUYA_END);
    assert(kv->erase_bytes == GD32H77X_LAYOUT_TUYA_ERASE_BYTES);
    assert(kv->program_bytes == GD32H77X_LAYOUT_TUYA_PROGRAM_BYTES);
    assert(gd32h77x_flash_preflight(GD32H77X_LAYOUT_TUYA_LOGICAL_BASE, 256U,
                                    GD32H77X_FLASH_OP_WRITE) == GD32H77X_FLASH_ROUTE_OK);
    assert(gd32h77x_flash_preflight(GD32H77X_LAYOUT_TUYA_END - 16U, 32U,
                                    GD32H77X_FLASH_OP_WRITE) == GD32H77X_FLASH_ROUTE_INVALID);
    assert(gd32h77x_flash_preflight(GD32H77X_LAYOUT_PRIMARY_END - 32U, 64U,
                                    GD32H77X_FLASH_OP_READ) == GD32H77X_FLASH_ROUTE_INVALID);
    assert(gd32h77x_flash_preflight(GD32H77X_STAGING_BASE, 32U,
                                    GD32H77X_FLASH_OP_READ) == GD32H77X_FLASH_ROUTE_OK);
    assert(gd32h77x_flash_preflight(UINT32_MAX - 3U, 8U,
                                    GD32H77X_FLASH_OP_READ) == GD32H77X_FLASH_ROUTE_INVALID);
    return 0;
}
