/**
 * @file gd32h77x_psram_cache.h
 * @brief Asset cache backed by the indirect GD32H77X PSRAM pool.
 *
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#ifndef GD32H77X_PSRAM_CACHE_H
#define GD32H77X_PSRAM_CACHE_H

#include "gd32h77x_psram_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t capacity_bytes;
    uint32_t used_bytes;
    uint32_t logical_bytes;
    uint32_t entry_count;
    uint32_t loading_count;
    uint32_t ready_count;
    uint32_t hits;
    uint32_t misses;
} gd32h77x_psram_cache_stats_t;

gd32h77x_status_t gd32h77x_psram_cache_init(uint32_t capacity_bytes);
gd32h77x_status_t gd32h77x_psram_cache_deinit(void);
bool gd32h77x_psram_cache_is_ready(void);

/**
 * @brief Start a streaming asset load.
 *
 * Only one loading entry per asset ID is allowed. Use append/commit to finish,
 * or abort to release the reservation.
 */
gd32h77x_status_t gd32h77x_psram_cache_begin(uint32_t asset_id, size_t size);
gd32h77x_status_t gd32h77x_psram_cache_append(
    uint32_t asset_id, const void *data, size_t size);
gd32h77x_status_t gd32h77x_psram_cache_commit(uint32_t asset_id);
gd32h77x_status_t gd32h77x_psram_cache_abort(uint32_t asset_id);

gd32h77x_status_t gd32h77x_psram_cache_put(
    uint32_t asset_id, const void *data, size_t size);
gd32h77x_status_t gd32h77x_psram_cache_read(
    uint32_t asset_id, uint32_t offset, void *data, size_t size);
gd32h77x_status_t gd32h77x_psram_cache_get_size(
    uint32_t asset_id, uint32_t *size);
gd32h77x_status_t gd32h77x_psram_cache_remove(uint32_t asset_id);
gd32h77x_status_t gd32h77x_psram_cache_clear(void);
gd32h77x_status_t gd32h77x_psram_cache_get_stats(
    gd32h77x_psram_cache_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* GD32H77X_PSRAM_CACHE_H */
