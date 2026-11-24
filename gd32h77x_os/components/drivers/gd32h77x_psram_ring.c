/**
 * @file gd32h77x_psram_ring.c
 * @brief Byte ring buffer backed by an indirect GD32H77X PSRAM pool block.
 *
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "gd32h77x_psram_ring.h"

#include "FreeRTOS.h"
#include "semphr.h"

#include <stddef.h>
#include <string.h>

static bool __ring_lock(gd32h77x_psram_ring_t *ring)
{
    return ring != NULL && ring->ready && ring->lock != NULL &&
           xSemaphoreTake((SemaphoreHandle_t)ring->lock,
                          portMAX_DELAY) == pdTRUE;
}

static void __ring_unlock(gd32h77x_psram_ring_t *ring)
{
    (void)xSemaphoreGive((SemaphoreHandle_t)ring->lock);
}

gd32h77x_status_t gd32h77x_psram_ring_init(gd32h77x_psram_ring_t *ring,
                                           size_t capacity)
{
    if (ring == NULL || capacity == 0U || capacity > UINT32_MAX) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    if (ring->ready) {
        return ring->capacity == capacity ? GD32H77X_STATUS_OK
                                          : GD32H77X_STATUS_INVALID_PARAM;
    }

    memset(ring, 0, sizeof(*ring));
    gd32h77x_status_t status =
        gd32h77x_psram_pool_alloc(capacity, &ring->handle);
    if (status != GD32H77X_STATUS_OK) {
        return status;
    }
    ring->lock = xSemaphoreCreateMutex();
    if (ring->lock == NULL) {
        (void)gd32h77x_psram_pool_free(&ring->handle);
        memset(ring, 0, sizeof(*ring));
        return GD32H77X_STATUS_NO_MEMORY;
    }
    ring->capacity = ring->handle.size;
    ring->ready = true;
    return GD32H77X_STATUS_OK;
}

gd32h77x_status_t gd32h77x_psram_ring_deinit(gd32h77x_psram_ring_t *ring)
{
    if (ring == NULL) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    if (!ring->ready) {
        return GD32H77X_STATUS_OK;
    }
    if (!__ring_lock(ring)) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    ring->ready = false;
    vSemaphoreDelete((SemaphoreHandle_t)ring->lock);
    ring->lock = NULL;
    gd32h77x_status_t status = gd32h77x_psram_pool_free(&ring->handle);
    memset(ring, 0, sizeof(*ring));
    return status;
}

gd32h77x_status_t gd32h77x_psram_ring_write(gd32h77x_psram_ring_t *ring,
                                           const void *data, size_t size,
                                           size_t *written)
{
    if (!__ring_lock(ring)) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    uint32_t accepted = ring->capacity - ring->used;
    if (accepted > size) {
        accepted = (uint32_t)size;
    }
    if (accepted != 0U && data == NULL) {
        __ring_unlock(ring);
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    if (accepted != 0U) {
        uint32_t first = ring->capacity - ring->head;
        if (first > accepted) {
            first = accepted;
        }
        gd32h77x_status_t status =
            gd32h77x_psram_pool_write(&ring->handle, ring->head, data, first);
        if (status != GD32H77X_STATUS_OK) {
            __ring_unlock(ring);
            return status;
        }
        if (accepted > first) {
            status = gd32h77x_psram_pool_write(
                &ring->handle, 0U, (const uint8_t *)data + first,
                accepted - first);
            if (status != GD32H77X_STATUS_OK) {
                __ring_unlock(ring);
                return status;
            }
        }
        ring->head = (ring->head + accepted) % ring->capacity;
        ring->used += accepted;
        if (ring->used > ring->high_watermark) {
            ring->high_watermark = ring->used;
        }
    }
    if (written != NULL) {
        *written = accepted;
    }
    __ring_unlock(ring);
    return GD32H77X_STATUS_OK;
}

gd32h77x_status_t gd32h77x_psram_ring_read(gd32h77x_psram_ring_t *ring,
                                          void *data, size_t size,
                                          size_t *read)
{
    if (!__ring_lock(ring)) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    uint32_t available = ring->used;
    if (available > size) {
        available = (uint32_t)size;
    }
    if (available != 0U && data == NULL) {
        __ring_unlock(ring);
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    if (available != 0U) {
        uint32_t first = ring->capacity - ring->tail;
        if (first > available) {
            first = available;
        }
        gd32h77x_status_t status = gd32h77x_psram_pool_read(
            &ring->handle, ring->tail, data, first);
        if (status != GD32H77X_STATUS_OK) {
            __ring_unlock(ring);
            return status;
        }
        if (available > first) {
            status = gd32h77x_psram_pool_read(
                &ring->handle, 0U, (uint8_t *)data + first, available - first);
            if (status != GD32H77X_STATUS_OK) {
                __ring_unlock(ring);
                return status;
            }
        }
        ring->tail = (ring->tail + available) % ring->capacity;
        ring->used -= available;
    }
    if (read != NULL) {
        *read = available;
    }
    __ring_unlock(ring);
    return GD32H77X_STATUS_OK;
}

gd32h77x_status_t gd32h77x_psram_ring_reset(gd32h77x_psram_ring_t *ring)
{
    if (!__ring_lock(ring)) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    ring->head = 0U;
    ring->tail = 0U;
    ring->used = 0U;
    ring->high_watermark = 0U;
    __ring_unlock(ring);
    return GD32H77X_STATUS_OK;
}

uint32_t gd32h77x_psram_ring_used(gd32h77x_psram_ring_t *ring)
{
    if (!__ring_lock(ring)) {
        return 0U;
    }
    uint32_t used = ring->used;
    __ring_unlock(ring);
    return used;
}

uint32_t gd32h77x_psram_ring_free(gd32h77x_psram_ring_t *ring)
{
    if (!__ring_lock(ring)) {
        return 0U;
    }
    uint32_t free_size = ring->capacity - ring->used;
    __ring_unlock(ring);
    return free_size;
}

uint32_t gd32h77x_psram_ring_high_watermark(gd32h77x_psram_ring_t *ring)
{
    if (!__ring_lock(ring)) {
        return 0U;
    }
    uint32_t watermark = ring->high_watermark;
    __ring_unlock(ring);
    return watermark;
}
