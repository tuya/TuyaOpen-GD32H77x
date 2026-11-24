/**
 * @file gd32h77x_psram_cache.c
 * @brief Asset cache backed by the indirect GD32H77X PSRAM pool.
 *
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "gd32h77x_psram_cache.h"

#include "FreeRTOS.h"
#include "semphr.h"

#include <stddef.h>
#include <string.h>

#define GD32H77X_PSRAM_CACHE_MAX_ENTRIES 24U

typedef enum {
    GD32H77X_PSRAM_CACHE_ENTRY_EMPTY = 0,
    GD32H77X_PSRAM_CACHE_ENTRY_LOADING,
    GD32H77X_PSRAM_CACHE_ENTRY_READY,
} gd32h77x_psram_cache_entry_state_t;

typedef struct {
    uint32_t asset_id;
    gd32h77x_psram_pool_handle_t handle;
    uint32_t size;
    uint32_t written;
    uint32_t last_access;
    gd32h77x_psram_cache_entry_state_t state;
} gd32h77x_psram_cache_entry_t;

static gd32h77x_psram_cache_entry_t s_entries[GD32H77X_PSRAM_CACHE_MAX_ENTRIES];
static SemaphoreHandle_t s_cache_lock;
static uint32_t s_capacity;
static uint32_t s_hits;
static uint32_t s_misses;
static uint32_t s_access_clock;
static bool s_cache_ready;

static bool __lock(void)
{
    return s_cache_ready && s_cache_lock != NULL &&
           xSemaphoreTake(s_cache_lock, portMAX_DELAY) == pdTRUE;
}

static void __unlock(void)
{
    (void)xSemaphoreGive(s_cache_lock);
}

static gd32h77x_psram_cache_entry_t *__find_entry(uint32_t asset_id)
{
    for (uint32_t index = 0U; index < GD32H77X_PSRAM_CACHE_MAX_ENTRIES;
         ++index) {
        if (s_entries[index].state != GD32H77X_PSRAM_CACHE_ENTRY_EMPTY &&
            s_entries[index].asset_id == asset_id) {
            return &s_entries[index];
        }
    }
    return NULL;
}

static gd32h77x_psram_cache_entry_t *__find_empty_entry(void)
{
    for (uint32_t index = 0U; index < GD32H77X_PSRAM_CACHE_MAX_ENTRIES;
         ++index) {
        if (s_entries[index].state == GD32H77X_PSRAM_CACHE_ENTRY_EMPTY) {
            return &s_entries[index];
        }
    }
    return NULL;
}

static uint32_t __used_bytes(void)
{
    uint32_t used = 0U;
    for (uint32_t index = 0U; index < GD32H77X_PSRAM_CACHE_MAX_ENTRIES;
         ++index) {
        if (s_entries[index].state != GD32H77X_PSRAM_CACHE_ENTRY_EMPTY) {
            used += s_entries[index].handle.size;
        }
    }
    return used;
}

static void __release_entry(gd32h77x_psram_cache_entry_t *entry)
{
    if (entry->state != GD32H77X_PSRAM_CACHE_ENTRY_EMPTY) {
        (void)gd32h77x_psram_pool_free(&entry->handle);
    }
    memset(entry, 0, sizeof(*entry));
}

gd32h77x_status_t gd32h77x_psram_cache_init(uint32_t capacity_bytes)
{
    if (!gd32h77x_psram_pool_is_ready() || capacity_bytes == 0U ||
        (capacity_bytes % GD32H77X_PSRAM_POOL_ALIGNMENT) != 0U) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    gd32h77x_psram_pool_stats_t pool;
    if (gd32h77x_psram_pool_get_stats(&pool) != GD32H77X_STATUS_OK ||
        capacity_bytes > pool.free_size) {
        return GD32H77X_STATUS_NO_MEMORY;
    }
    if (s_cache_ready) {
        return s_capacity == capacity_bytes
                   ? GD32H77X_STATUS_OK
                   : GD32H77X_STATUS_INVALID_PARAM;
    }

    s_cache_lock = xSemaphoreCreateMutex();
    if (s_cache_lock == NULL) {
        return GD32H77X_STATUS_NO_MEMORY;
    }
    memset(s_entries, 0, sizeof(s_entries));
    s_capacity = capacity_bytes;
    s_hits = 0U;
    s_misses = 0U;
    s_access_clock = 0U;
    s_cache_ready = true;
    return GD32H77X_STATUS_OK;
}

gd32h77x_status_t gd32h77x_psram_cache_deinit(void)
{
    if (!s_cache_ready) {
        return GD32H77X_STATUS_OK;
    }
    (void)gd32h77x_psram_cache_clear();
    vSemaphoreDelete(s_cache_lock);
    s_cache_lock = NULL;
    s_cache_ready = false;
    s_capacity = 0U;
    return GD32H77X_STATUS_OK;
}

bool gd32h77x_psram_cache_is_ready(void)
{
    return s_cache_ready;
}

gd32h77x_status_t gd32h77x_psram_cache_begin(uint32_t asset_id, size_t size)
{
    if (asset_id == 0U || size == 0U || size > UINT32_MAX || !__lock()) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    if (__find_entry(asset_id) != NULL) {
        __unlock();
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    gd32h77x_psram_cache_entry_t *entry = __find_empty_entry();
    if (entry == NULL) {
        __unlock();
        return GD32H77X_STATUS_NO_MEMORY;
    }

    gd32h77x_psram_pool_handle_t handle;
    gd32h77x_status_t status = gd32h77x_psram_pool_alloc(size, &handle);
    if (status != GD32H77X_STATUS_OK) {
        __unlock();
        return status;
    }
    if (__used_bytes() + handle.size > s_capacity) {
        (void)gd32h77x_psram_pool_free(&handle);
        __unlock();
        return GD32H77X_STATUS_NO_MEMORY;
    }

    memset(entry, 0, sizeof(*entry));
    entry->asset_id = asset_id;
    entry->handle = handle;
    entry->size = (uint32_t)size;
    entry->state = GD32H77X_PSRAM_CACHE_ENTRY_LOADING;
    __unlock();
    return GD32H77X_STATUS_OK;
}

gd32h77x_status_t gd32h77x_psram_cache_append(
    uint32_t asset_id, const void *data, size_t size)
{
    if (data == NULL || size == 0U || !__lock()) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    gd32h77x_psram_cache_entry_t *entry = __find_entry(asset_id);
    if (entry == NULL || entry->state != GD32H77X_PSRAM_CACHE_ENTRY_LOADING ||
        entry->written > entry->size ||
        size > entry->size - entry->written) {
        __unlock();
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    gd32h77x_status_t status = gd32h77x_psram_pool_write(
        &entry->handle, entry->written, data, size);
    if (status == GD32H77X_STATUS_OK) {
        entry->written += (uint32_t)size;
    }
    __unlock();
    return status;
}

gd32h77x_status_t gd32h77x_psram_cache_commit(uint32_t asset_id)
{
    if (!__lock()) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    gd32h77x_psram_cache_entry_t *entry = __find_entry(asset_id);
    if (entry == NULL || entry->state != GD32H77X_PSRAM_CACHE_ENTRY_LOADING ||
        entry->written != entry->size) {
        __unlock();
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    entry->state = GD32H77X_PSRAM_CACHE_ENTRY_READY;
    entry->last_access = ++s_access_clock;
    __unlock();
    return GD32H77X_STATUS_OK;
}

gd32h77x_status_t gd32h77x_psram_cache_abort(uint32_t asset_id)
{
    if (!__lock()) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    gd32h77x_psram_cache_entry_t *entry = __find_entry(asset_id);
    if (entry == NULL || entry->state != GD32H77X_PSRAM_CACHE_ENTRY_LOADING) {
        __unlock();
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    __release_entry(entry);
    __unlock();
    return GD32H77X_STATUS_OK;
}

gd32h77x_status_t gd32h77x_psram_cache_put(
    uint32_t asset_id, const void *data, size_t size)
{
    if (data == NULL || size == 0U) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    gd32h77x_status_t status = gd32h77x_psram_cache_begin(asset_id, size);
    if (status != GD32H77X_STATUS_OK) {
        return status;
    }
    status = gd32h77x_psram_cache_append(asset_id, data, size);
    if (status == GD32H77X_STATUS_OK) {
        status = gd32h77x_psram_cache_commit(asset_id);
    }
    if (status != GD32H77X_STATUS_OK) {
        (void)gd32h77x_psram_cache_abort(asset_id);
    }
    return status;
}

gd32h77x_status_t gd32h77x_psram_cache_read(
    uint32_t asset_id, uint32_t offset, void *data, size_t size)
{
    if (data == NULL || !__lock()) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    gd32h77x_psram_cache_entry_t *entry = __find_entry(asset_id);
    if (entry == NULL || entry->state != GD32H77X_PSRAM_CACHE_ENTRY_READY) {
        ++s_misses;
        __unlock();
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    if (offset > entry->size || size > (size_t)entry->size - offset) {
        ++s_misses;
        __unlock();
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    ++s_hits;
    entry->last_access = ++s_access_clock;
    gd32h77x_status_t status =
        gd32h77x_psram_pool_read(&entry->handle, offset, data, size);
    __unlock();
    return status;
}

gd32h77x_status_t gd32h77x_psram_cache_get_size(
    uint32_t asset_id, uint32_t *size)
{
    if (size == NULL || !__lock()) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    gd32h77x_psram_cache_entry_t *entry = __find_entry(asset_id);
    if (entry == NULL || entry->state != GD32H77X_PSRAM_CACHE_ENTRY_READY) {
        __unlock();
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    *size = entry->size;
    __unlock();
    return GD32H77X_STATUS_OK;
}

gd32h77x_status_t gd32h77x_psram_cache_remove(uint32_t asset_id)
{
    if (!__lock()) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    gd32h77x_psram_cache_entry_t *entry = __find_entry(asset_id);
    if (entry == NULL) {
        __unlock();
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    __release_entry(entry);
    __unlock();
    return GD32H77X_STATUS_OK;
}

gd32h77x_status_t gd32h77x_psram_cache_clear(void)
{
    if (!__lock()) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    for (uint32_t index = 0U; index < GD32H77X_PSRAM_CACHE_MAX_ENTRIES;
         ++index) {
        __release_entry(&s_entries[index]);
    }
    s_hits = 0U;
    s_misses = 0U;
    s_access_clock = 0U;
    __unlock();
    return GD32H77X_STATUS_OK;
}

gd32h77x_status_t gd32h77x_psram_cache_get_stats(
    gd32h77x_psram_cache_stats_t *stats)
{
    if (stats == NULL || !__lock()) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    memset(stats, 0, sizeof(*stats));
    stats->capacity_bytes = s_capacity;
    stats->used_bytes = __used_bytes();
    stats->hits = s_hits;
    stats->misses = s_misses;
    for (uint32_t index = 0U; index < GD32H77X_PSRAM_CACHE_MAX_ENTRIES;
         ++index) {
        gd32h77x_psram_cache_entry_t *entry = &s_entries[index];
        if (entry->state == GD32H77X_PSRAM_CACHE_ENTRY_EMPTY) {
            continue;
        }
        ++stats->entry_count;
        stats->logical_bytes += entry->size;
        if (entry->state == GD32H77X_PSRAM_CACHE_ENTRY_LOADING) {
            ++stats->loading_count;
        } else {
            ++stats->ready_count;
        }
    }
    __unlock();
    return GD32H77X_STATUS_OK;
}
