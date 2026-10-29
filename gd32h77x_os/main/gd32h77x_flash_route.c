/**
 * @file tkl_flash_route.c
 * @brief Resolve logical flash ranges against generated physical segments.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */
#include "gd32h77x_flash_route.h"

#include <stddef.h>
#include <string.h>

typedef struct {
    const gd32h77x_layout_partition_t *partition;
    uint32_t partition_index;
    uint32_t offset;
    uint32_t size;
} gd32h77x_flash_resolved_range_t;

static gd32h77x_flash_route_status_t gd32h77x_find_partition(
    uint32_t address, uint32_t size, uint32_t requested_index,
    gd32h77x_flash_resolved_range_t *resolved)
{
    const uint64_t end = (uint64_t)address + size;
    const uint64_t address_space_end = (uint64_t)UINT32_MAX + 1U;
    if (end > address_space_end) {
        return GD32H77X_FLASH_ROUTE_INVALID;
    }

    uint32_t matches = 0U;
    bool found_interior = false;
    gd32h77x_flash_resolved_range_t end_match = {0};
    uint32_t end_matches = 0U;
    for (uint32_t i = 0U; i < GD32H77X_LAYOUT_PARTITION_COUNT; ++i) {
        if (requested_index != UINT32_MAX && requested_index != i) {
            continue;
        }
        const gd32h77x_layout_partition_t *part = &gd32h77x_layout_partitions[i];
        const uint64_t part_start = part->logical_base;
        const uint64_t part_end = part_start + part->size_bytes;
        if ((uint64_t)address < part_start || (uint64_t)address > part_end || end > part_end) {
            continue;
        }
        if (size != 0U && (uint64_t)address == part_end) {
            continue;
        }
        if (size == 0U && (uint64_t)address == part_end) {
            ++end_matches;
            end_match.partition = part;
            end_match.partition_index = i;
            end_match.offset = part->size_bytes;
            end_match.size = 0U;
            continue;
        }
        ++matches;
        found_interior = true;
        resolved->partition = part;
        resolved->partition_index = i;
        resolved->offset = address - part->logical_base;
        resolved->size = size;
    }
    if (size == 0U && !found_interior && matches == 0U && end_matches == 1U) {
        *resolved = end_match;
        return GD32H77X_FLASH_ROUTE_OK;
    }
    return matches == 1U ? GD32H77X_FLASH_ROUTE_OK : GD32H77X_FLASH_ROUTE_INVALID;
}

static gd32h77x_flash_route_status_t gd32h77x_validate_segments(
    const gd32h77x_flash_resolved_range_t *range, gd32h77x_flash_op_t operation)
{
    const gd32h77x_layout_partition_t *part = range->partition;
    const uint64_t range_end = (uint64_t)range->offset + range->size;
    uint64_t cursor = range->offset;
    bool unsupported = false;

    if (operation == GD32H77X_FLASH_OP_WRITE) {
        if (part->program_bytes != 0U && (range->offset % part->program_bytes != 0U ||
            range->size % part->program_bytes != 0U)) {
            return GD32H77X_FLASH_ROUTE_INVALID;
        }
    } else if (operation == GD32H77X_FLASH_OP_ERASE) {
        if (range->size == 0U || part->erase_bytes == 0U ||
            range->offset % part->erase_bytes != 0U ||
            range->size % part->erase_bytes != 0U) {
            return GD32H77X_FLASH_ROUTE_INVALID;
        }
    } else if (operation != GD32H77X_FLASH_OP_READ) {
        return GD32H77X_FLASH_ROUTE_INVALID;
    }

    for (uint32_t i = 0U; i < part->segment_count && cursor < range_end; ++i) {
        const uint32_t segment_index = part->first_segment + i;
        if (segment_index >= GD32H77X_LAYOUT_SEGMENT_COUNT) {
            return GD32H77X_FLASH_ROUTE_INVALID;
        }
        const gd32h77x_layout_segment_t *segment = &gd32h77x_layout_segments[segment_index];
        const uint64_t segment_start = segment->logical_offset_bytes;
        const uint64_t segment_end = segment_start + segment->size_bytes;
        if (segment_end <= cursor) {
            continue;
        }
        if (segment_start > cursor || cursor >= segment_end || segment->device_index >= GD32H77X_LAYOUT_DEVICE_COUNT) {
            return GD32H77X_FLASH_ROUTE_INVALID;
        }

        const gd32h77x_layout_device_t *device = &gd32h77x_layout_devices[segment->device_index];
        const uint64_t chunk_end = segment_end < range_end ? segment_end : range_end;
        const uint32_t device_offset = segment->device_offset_bytes + (uint32_t)(cursor - segment_start);
        const uint32_t chunk_size = (uint32_t)(chunk_end - cursor);
        if ((uint64_t)device_offset + chunk_size > device->capacity_bytes) {
            return GD32H77X_FLASH_ROUTE_INVALID;
        }
        if (operation == GD32H77X_FLASH_OP_READ) {
            const bool mapped_nvm = device->access == GD32H77X_LAYOUT_ACCESS_MAPPED &&
                                    device->cpu_base != 0U && device->region != NULL &&
                                    (strcmp(device->region, "CNVM") == 0 ||
                                     strcmp(device->region, "ECNVM") == 0);
            const bool indirect_nor = device->access == GD32H77X_LAYOUT_ACCESS_INDIRECT &&
                                      device->region != NULL && strcmp(device->region, "NOR") == 0 &&
                                      device->backend_status == GD32H77X_LAYOUT_BACKEND_IMPLEMENTED;
            if (!mapped_nvm && !indirect_nor) {
                unsupported = true;
            }
        } else {
            if (device->backend_status != GD32H77X_LAYOUT_BACKEND_IMPLEMENTED) {
                unsupported = true;
            }
            const uint32_t unit = operation == GD32H77X_FLASH_OP_WRITE
                                      ? (device->program_bytes ? device->program_bytes : device->page_bytes)
                                      : device->erase_bytes;
            if (operation == GD32H77X_FLASH_OP_WRITE && unit == 0U) {
                unsupported = true;
            } else if (unit != 0U && (device_offset % unit != 0U || chunk_size % unit != 0U)) {
                return GD32H77X_FLASH_ROUTE_INVALID;
            }
        }
        cursor = chunk_end;
    }
    if (cursor != range_end) {
        return GD32H77X_FLASH_ROUTE_INVALID;
    }
    return unsupported ? GD32H77X_FLASH_ROUTE_UNSUPPORTED : GD32H77X_FLASH_ROUTE_OK;
}

static gd32h77x_flash_route_status_t gd32h77x_preflight_partition(
    uint32_t partition_index, uint32_t address, uint32_t size,
    gd32h77x_flash_op_t operation, gd32h77x_flash_resolved_range_t *range)
{
    gd32h77x_flash_route_status_t status = gd32h77x_find_partition(
        address, size, partition_index, range);
    if (status != GD32H77X_FLASH_ROUTE_OK) {
        return status;
    }
    return gd32h77x_validate_segments(range, operation);
}

gd32h77x_flash_route_status_t gd32h77x_flash_preflight(
    uint32_t address, uint32_t size, gd32h77x_flash_op_t operation)
{
    gd32h77x_flash_resolved_range_t range = {0};
    return gd32h77x_preflight_partition(UINT32_MAX, address, size, operation, &range);
}

bool gd32h77x_flash_range_valid(uint32_t partition_index, uint32_t address,
                                uint32_t size, gd32h77x_flash_op_t operation)
{
    gd32h77x_flash_resolved_range_t range = {0};
    return gd32h77x_preflight_partition(partition_index, address, size,
                                        operation, &range) != GD32H77X_FLASH_ROUTE_INVALID;
}

bool gd32h77x_flash_range_valid_any(uint32_t address, uint32_t size,
                                    gd32h77x_flash_op_t operation)
{
    return gd32h77x_flash_preflight(address, size, operation) != GD32H77X_FLASH_ROUTE_INVALID;
}

bool gd32h77x_flash_route(uint32_t address, uint32_t size,
                          gd32h77x_flash_op_t operation,
                          gd32h77x_flash_route_callback_t callback,
                          void *context)
{
    if (callback == NULL || gd32h77x_flash_preflight(address, size, operation) != GD32H77X_FLASH_ROUTE_OK) {
        return false;
    }
    if (size == 0U) {
        return true;
    }

    gd32h77x_flash_resolved_range_t range = {0};
    if (gd32h77x_find_partition(address, size, UINT32_MAX, &range) != GD32H77X_FLASH_ROUTE_OK) {
        return false;
    }
    const gd32h77x_layout_partition_t *part = range.partition;
    uint64_t cursor = range.offset;
    const uint64_t range_end = cursor + size;
    for (uint32_t i = 0U; i < part->segment_count && cursor < range_end; ++i) {
        const gd32h77x_layout_segment_t *segment = &gd32h77x_layout_segments[part->first_segment + i];
        const uint64_t segment_start = segment->logical_offset_bytes;
        const uint64_t segment_end = segment_start + segment->size_bytes;
        if (segment_end <= cursor) {
            continue;
        }
        const uint64_t chunk_end = segment_end < range_end ? segment_end : range_end;
        gd32h77x_flash_route_chunk_t chunk = {
            .partition = part,
            .segment = segment,
            .device = &gd32h77x_layout_devices[segment->device_index],
            .logical_address = part->logical_base + (uint32_t)cursor,
            .device_offset = segment->device_offset_bytes + (uint32_t)(cursor - segment_start),
            .size = (uint32_t)(chunk_end - cursor),
        };
        if (!callback(&chunk, context)) {
            return false;
        }
        cursor = chunk_end;
    }
    return cursor == range_end;
}
