/**
 * @file gd32h77x_psram_pool.h
 * @brief Offset-based allocator for indirect GD32H77X PSRAM access.
 *
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#ifndef GD32H77X_PSRAM_POOL_H
#define GD32H77X_PSRAM_POOL_H

#include "gd32h77x_psram.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GD32H77X_PSRAM_POOL_ALIGNMENT 32U

typedef struct {
    uint32_t offset;
    uint32_t size;
    uint32_t generation;
    uint32_t magic;
} gd32h77x_psram_pool_handle_t;

typedef struct {
    uint32_t base_offset;
    uint32_t total_size;
    uint32_t free_size;
    uint32_t used_size;
    uint32_t block_count;
    uint32_t free_blocks;
    uint32_t used_blocks;
    uint32_t largest_free_block;
} gd32h77x_psram_pool_stats_t;

/**
 * @brief Create a runtime PSRAM pool inside the already initialized device.
 *
 * The offset and size must both be 32-byte aligned. Metadata is kept in internal
 * SRAM; no allocator metadata is stored in PSRAM. Allocation/free are valid in
 * both indirect and mapped modes. The read/write/fill helpers below remain
 * indirect-only; mapped clients access returned pointers through the platform
 * PSRAM memory helpers.
 */
gd32h77x_status_t gd32h77x_psram_pool_init(uint32_t offset, uint32_t size);
gd32h77x_status_t gd32h77x_psram_pool_deinit(void);
bool gd32h77x_psram_pool_is_ready(void);

/**
 * @brief Allocate a 32-byte aligned PSRAM block.
 *
 * The returned handle is an opaque offset-based capability. It must not be
 * cast or dereferenced as a CPU pointer.
 */
gd32h77x_status_t gd32h77x_psram_pool_alloc(
    size_t size, gd32h77x_psram_pool_handle_t *handle);

/**
 * @brief Release a pool block and invalidate the supplied handle.
 */
gd32h77x_status_t gd32h77x_psram_pool_free(
    gd32h77x_psram_pool_handle_t *handle);

/**
 * @brief Access a byte range relative to the start of an allocated block.
 */
gd32h77x_status_t gd32h77x_psram_pool_read(
    const gd32h77x_psram_pool_handle_t *handle, uint32_t offset, void *data,
    size_t size);
gd32h77x_status_t gd32h77x_psram_pool_write(
    const gd32h77x_psram_pool_handle_t *handle, uint32_t offset,
    const void *data, size_t size);
gd32h77x_status_t gd32h77x_psram_pool_fill(
    const gd32h77x_psram_pool_handle_t *handle, uint32_t offset,
    uint8_t value, size_t size);

gd32h77x_status_t gd32h77x_psram_pool_get_stats(
    gd32h77x_psram_pool_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* GD32H77X_PSRAM_POOL_H */
