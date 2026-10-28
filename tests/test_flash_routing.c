/**
 * @file test_flash_routing.c
 * @brief Host checks for split flash routing and preflight guarantees.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */
#include "gd32h77x_flash_route.h"

#include <assert.h>
#include <stdint.h>

typedef struct {
    uint32_t count;
    uint32_t addresses[4];
    uint32_t device_offsets[4];
    uint32_t sizes[4];
} capture_t;

static bool capture_chunk(const gd32h77x_flash_route_chunk_t *chunk, void *context)
{
    capture_t *capture = context;
    assert(capture->count < 4U);
    capture->addresses[capture->count] = chunk->logical_address;
    capture->device_offsets[capture->count] = chunk->device_offset;
    capture->sizes[capture->count] = chunk->size;
    ++capture->count;
    return true;
}

int main(void)
{
    capture_t capture = {0};

    assert(gd32h77x_flash_preflight(0x7200F000U, 0x2000U,
                                    GD32H77X_FLASH_OP_READ) == GD32H77X_FLASH_ROUTE_OK);
    assert(gd32h77x_flash_route(0x7200F000U, 0x2000U, GD32H77X_FLASH_OP_READ,
                                capture_chunk, &capture));
    assert(capture.count == 2U);
    assert(capture.addresses[0] == 0x7200F000U && capture.sizes[0] == 0x1000U);
    assert(capture.device_offsets[0] == 0x12F000U);
    assert(capture.addresses[1] == 0x72010000U && capture.sizes[1] == 0x1000U);
    assert(capture.device_offsets[1] == 0x180000U);

    /* The later unsupported NOR segment rejects the whole read before callbacks. */
    capture.count = 0U;
    assert(gd32h77x_flash_preflight(0x7000F000U, 0x2000U,
                                    GD32H77X_FLASH_OP_READ) == GD32H77X_FLASH_ROUTE_UNSUPPORTED);
    assert(!gd32h77x_flash_route(0x7000F000U, 0x2000U, GD32H77X_FLASH_OP_READ,
                                 capture_chunk, &capture));
    assert(capture.count == 0U);

    /* A later unsupported device rejects the whole range before any callback. */
    capture.count = 0U;
    assert(gd32h77x_flash_preflight(0x70000000U, 0x20000U,
                                    GD32H77X_FLASH_OP_WRITE) == GD32H77X_FLASH_ROUTE_UNSUPPORTED);
    assert(!gd32h77x_flash_route(0x70000000U, 0x20000U, GD32H77X_FLASH_OP_WRITE,
                                 capture_chunk, &capture));
    assert(capture.count == 0U);

    /* Mixed erase geometry still obeys the partition's common 64 KiB unit. */
    assert(gd32h77x_flash_preflight(0x70000000U, 0x20000U,
                                    GD32H77X_FLASH_OP_ERASE) == GD32H77X_FLASH_ROUTE_UNSUPPORTED);
    assert(gd32h77x_flash_preflight(0x70000000U, 0x1000U,
                                    GD32H77X_FLASH_OP_ERASE) == GD32H77X_FLASH_ROUTE_INVALID);

    assert(gd32h77x_flash_preflight(0x7200F001U, 0x2000U,
                                    GD32H77X_FLASH_OP_READ) == GD32H77X_FLASH_ROUTE_OK);
    assert(gd32h77x_flash_preflight(0x7201F000U, 0x2000U,
                                    GD32H77X_FLASH_OP_READ) == GD32H77X_FLASH_ROUTE_INVALID);
    assert(gd32h77x_flash_preflight(UINT32_MAX - 7U, 16U,
                                    GD32H77X_FLASH_OP_READ) == GD32H77X_FLASH_ROUTE_INVALID);
    assert(gd32h77x_flash_preflight(0x72000000U, 0U,
                                    GD32H77X_FLASH_OP_READ) == GD32H77X_FLASH_ROUTE_OK);
    assert(gd32h77x_flash_preflight(0x72000000U, 0U,
                                    GD32H77X_FLASH_OP_ERASE) == GD32H77X_FLASH_ROUTE_INVALID);
    assert(gd32h77x_flash_range_valid(GD32H77X_LAYOUT_PART_KV,
                                      GD32H77X_KV_BASE, GD32H77X_KV_SIZE,
                                      GD32H77X_FLASH_OP_WRITE));
    assert(!gd32h77x_flash_range_valid(GD32H77X_LAYOUT_PART_KV,
                                       GD32H77X_KV_BASE + 1U, 32U,
                                       GD32H77X_FLASH_OP_WRITE));
    return 0;
}
