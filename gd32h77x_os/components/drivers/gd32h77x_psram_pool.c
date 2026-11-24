/**
 * @file gd32h77x_psram_pool.c
 * @brief Offset-based allocator for indirect GD32H77X PSRAM access.
 *
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "gd32h77x_psram_pool.h"

#include "FreeRTOS.h"
#include "semphr.h"

#include <stddef.h>
#include <string.h>

#define GD32H77X_PSRAM_POOL_MAX_BLOCKS 128U
#define GD32H77X_PSRAM_POOL_HANDLE_MAGIC 0x50534844U

typedef struct {
    uint32_t offset;
    uint32_t size;
    uint32_t generation;
    bool used;
} gd32h77x_psram_pool_block_t;

static gd32h77x_psram_pool_block_t s_blocks[GD32H77X_PSRAM_POOL_MAX_BLOCKS];
static SemaphoreHandle_t s_pool_lock;
static uint32_t s_pool_base;
static uint32_t s_pool_size;
static uint32_t s_block_count;
static uint32_t s_generation = 1U;
static bool s_pool_ready;

static uint32_t __next_generation(void)
{
    ++s_generation;
    if (s_generation == 0U) {
        s_generation = 1U;
    }
    return s_generation;
}

static bool __lock(void)
{
    return s_pool_ready && s_pool_lock != NULL &&
           xSemaphoreTake(s_pool_lock, portMAX_DELAY) == pdTRUE;
}

static void __unlock(void)
{
    (void)xSemaphoreGive(s_pool_lock);
}

static bool __range_valid(uint32_t offset, size_t size)
{
    return size <= UINT32_MAX &&
           (uint64_t)offset + size <= (uint64_t)s_pool_base + s_pool_size;
}

static int __handle_lookup(const gd32h77x_psram_pool_handle_t *handle)
{
    if (handle == NULL || handle->magic != GD32H77X_PSRAM_POOL_HANDLE_MAGIC ||
        handle->size == 0U ||
        (handle->size % GD32H77X_PSRAM_POOL_ALIGNMENT) != 0U ||
        !__range_valid(handle->offset, handle->size)) {
        return -1;
    }
    for (uint32_t index = 0U; index < s_block_count; ++index) {
        const gd32h77x_psram_pool_block_t *block = &s_blocks[index];
        if (block->used && block->offset == handle->offset &&
            block->size == handle->size &&
            block->generation == handle->generation) {
            return (int)index;
        }
    }
    return -1;
}

static void __remove_block(uint32_t index)
{
    if (index + 1U < s_block_count) {
        memmove(&s_blocks[index], &s_blocks[index + 1U],
                (s_block_count - index - 1U) * sizeof(s_blocks[0]));
    }
    --s_block_count;
}

static void __merge_free_blocks(void)
{
    uint32_t index = 0U;
    while (index + 1U < s_block_count) {
        gd32h77x_psram_pool_block_t *current = &s_blocks[index];
        gd32h77x_psram_pool_block_t *next = &s_blocks[index + 1U];
        if (!current->used && !next->used &&
            current->offset + current->size == next->offset) {
            current->size += next->size;
            current->generation = __next_generation();
            __remove_block(index + 1U);
        } else {
            ++index;
        }
    }
}

gd32h77x_status_t gd32h77x_psram_pool_init(uint32_t offset, uint32_t size)
{
    if (size == 0U ||
        (offset % GD32H77X_PSRAM_POOL_ALIGNMENT) != 0U ||
        (size % GD32H77X_PSRAM_POOL_ALIGNMENT) != 0U) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    if (s_pool_ready) {
        return s_pool_base == offset && s_pool_size == size
                   ? GD32H77X_STATUS_OK
                   : GD32H77X_STATUS_INVALID_PARAM;
    }

    s_pool_lock = xSemaphoreCreateMutex();
    if (s_pool_lock == NULL) {
        return GD32H77X_STATUS_NO_MEMORY;
    }

    memset(s_blocks, 0, sizeof(s_blocks));
    s_pool_base = offset;
    s_pool_size = size;
    s_block_count = 1U;
    s_blocks[0].offset = offset;
    s_blocks[0].size = size;
    s_blocks[0].generation = __next_generation();
    s_blocks[0].used = false;
    s_pool_ready = true;
    return GD32H77X_STATUS_OK;
}

gd32h77x_status_t gd32h77x_psram_pool_deinit(void)
{
    if (!s_pool_ready) {
        return GD32H77X_STATUS_OK;
    }
    vSemaphoreDelete(s_pool_lock);
    s_pool_lock = NULL;
    s_pool_ready = false;
    s_pool_base = 0U;
    s_pool_size = 0U;
    s_block_count = 0U;
    memset(s_blocks, 0, sizeof(s_blocks));
    return GD32H77X_STATUS_OK;
}

bool gd32h77x_psram_pool_is_ready(void)
{
    return s_pool_ready;
}

gd32h77x_status_t gd32h77x_psram_pool_alloc(
    size_t size, gd32h77x_psram_pool_handle_t *handle)
{
    if (handle == NULL || size == 0U || size > UINT32_MAX ||
        size > UINT32_MAX - (GD32H77X_PSRAM_POOL_ALIGNMENT - 1U) ||
        !__lock()) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }

    uint32_t aligned_size =
        ((uint32_t)size + GD32H77X_PSRAM_POOL_ALIGNMENT - 1U) &
        ~(GD32H77X_PSRAM_POOL_ALIGNMENT - 1U);
    gd32h77x_status_t status = GD32H77X_STATUS_NO_MEMORY;

    for (uint32_t index = 0U; index < s_block_count; ++index) {
        gd32h77x_psram_pool_block_t *block = &s_blocks[index];
        if (block->used || block->size < aligned_size) {
            continue;
        }

        uint32_t remainder = block->size - aligned_size;
        if (remainder >= GD32H77X_PSRAM_POOL_ALIGNMENT) {
            if (s_block_count >= GD32H77X_PSRAM_POOL_MAX_BLOCKS) {
                break;
            }
            memmove(&s_blocks[index + 2U], &s_blocks[index + 1U],
                    (s_block_count - index - 1U) * sizeof(s_blocks[0]));
            ++s_block_count;
            s_blocks[index + 1U].offset = block->offset + aligned_size;
            s_blocks[index + 1U].size = remainder;
            s_blocks[index + 1U].generation = __next_generation();
            s_blocks[index + 1U].used = false;
            block->size = aligned_size;
        }

        block->used = true;
        block->generation = __next_generation();
        handle->offset = block->offset;
        handle->size = block->size;
        handle->generation = block->generation;
        handle->magic = GD32H77X_PSRAM_POOL_HANDLE_MAGIC;
        status = GD32H77X_STATUS_OK;
        break;
    }

    __unlock();
    return status;
}

gd32h77x_status_t gd32h77x_psram_pool_free(
    gd32h77x_psram_pool_handle_t *handle)
{
    if (handle == NULL || !__lock()) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }

    int index = __handle_lookup(handle);
    if (index < 0) {
        __unlock();
        return GD32H77X_STATUS_INVALID_PARAM;
    }

    s_blocks[index].used = false;
    s_blocks[index].generation = __next_generation();
    memset(handle, 0, sizeof(*handle));
    __merge_free_blocks();
    __unlock();
    return GD32H77X_STATUS_OK;
}

gd32h77x_status_t gd32h77x_psram_pool_read(
    const gd32h77x_psram_pool_handle_t *handle, uint32_t offset, void *data,
    size_t size)
{
    if (data == NULL ||
        gd32h77x_psram_access_mode() != GD32H77X_PSRAM_MODE_INDIRECT ||
        !__lock()) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    int index = __handle_lookup(handle);
    if (index < 0 || offset > handle->size ||
        size > (size_t)handle->size - offset) {
        __unlock();
        return GD32H77X_STATUS_INVALID_PARAM;
    }

    gd32h77x_status_t status =
        gd32h77x_psram_read(handle->offset + offset, data, size);
    __unlock();
    return status;
}

gd32h77x_status_t gd32h77x_psram_pool_write(
    const gd32h77x_psram_pool_handle_t *handle, uint32_t offset,
    const void *data, size_t size)
{
    if (data == NULL ||
        gd32h77x_psram_access_mode() != GD32H77X_PSRAM_MODE_INDIRECT ||
        !__lock()) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    int index = __handle_lookup(handle);
    if (index < 0 || offset > handle->size ||
        size > (size_t)handle->size - offset) {
        __unlock();
        return GD32H77X_STATUS_INVALID_PARAM;
    }

    gd32h77x_status_t status =
        gd32h77x_psram_write(handle->offset + offset, data, size);
    __unlock();
    return status;
}

gd32h77x_status_t gd32h77x_psram_pool_fill(
    const gd32h77x_psram_pool_handle_t *handle, uint32_t offset,
    uint8_t value, size_t size)
{
    if (gd32h77x_psram_access_mode() != GD32H77X_PSRAM_MODE_INDIRECT ||
        !__lock()) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    int index = __handle_lookup(handle);
    if (index < 0 || offset > handle->size ||
        size > (size_t)handle->size - offset) {
        __unlock();
        return GD32H77X_STATUS_INVALID_PARAM;
    }

    gd32h77x_status_t status =
        gd32h77x_psram_fill(handle->offset + offset, value, size);
    __unlock();
    return status;
}

gd32h77x_status_t gd32h77x_psram_pool_get_stats(
    gd32h77x_psram_pool_stats_t *stats)
{
    if (stats == NULL || !__lock()) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }

    memset(stats, 0, sizeof(*stats));
    stats->base_offset = s_pool_base;
    stats->total_size = s_pool_size;
    stats->block_count = s_block_count;
    for (uint32_t index = 0U; index < s_block_count; ++index) {
        const gd32h77x_psram_pool_block_t *block = &s_blocks[index];
        if (block->used) {
            stats->used_size += block->size;
            ++stats->used_blocks;
        } else {
            stats->free_size += block->size;
            ++stats->free_blocks;
            if (block->size > stats->largest_free_block) {
                stats->largest_free_block = block->size;
            }
        }
    }
    __unlock();
    return GD32H77X_STATUS_OK;
}
