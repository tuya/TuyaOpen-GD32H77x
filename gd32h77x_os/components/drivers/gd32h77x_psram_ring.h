/**
 * @file gd32h77x_psram_ring.h
 * @brief Byte ring buffer backed by an indirect GD32H77X PSRAM pool block.
 *
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#ifndef GD32H77X_PSRAM_RING_H
#define GD32H77X_PSRAM_RING_H

#include "gd32h77x_psram_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gd32h77x_psram_pool_handle_t handle;
    uint32_t head;
    uint32_t tail;
    uint32_t used;
    uint32_t capacity;
    uint32_t high_watermark;
    void *lock;
    bool ready;
} gd32h77x_psram_ring_t;

gd32h77x_status_t gd32h77x_psram_ring_init(gd32h77x_psram_ring_t *ring,
                                           size_t capacity);
gd32h77x_status_t gd32h77x_psram_ring_deinit(gd32h77x_psram_ring_t *ring);

/**
 * @brief Append bytes from internal memory into the PSRAM ring.
 *
 * Writing is partial when the ring has insufficient free space. The number of
 * bytes accepted is returned through written.
 */
gd32h77x_status_t gd32h77x_psram_ring_write(gd32h77x_psram_ring_t *ring,
                                           const void *data, size_t size,
                                           size_t *written);

/**
 * @brief Consume bytes from the PSRAM ring into internal memory.
 */
gd32h77x_status_t gd32h77x_psram_ring_read(gd32h77x_psram_ring_t *ring,
                                          void *data, size_t size,
                                          size_t *read);
gd32h77x_status_t gd32h77x_psram_ring_reset(gd32h77x_psram_ring_t *ring);
uint32_t gd32h77x_psram_ring_used(gd32h77x_psram_ring_t *ring);
uint32_t gd32h77x_psram_ring_free(gd32h77x_psram_ring_t *ring);
uint32_t gd32h77x_psram_ring_high_watermark(gd32h77x_psram_ring_t *ring);

#ifdef __cplusplus
}
#endif

#endif /* GD32H77X_PSRAM_RING_H */
