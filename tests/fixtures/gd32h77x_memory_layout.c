/* @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved. */
#include <stddef.h>
#include "gd32h77x_memory_layout.h"

const gd32h77x_layout_device_t gd32h77x_layout_devices[GD32H77X_LAYOUT_DEVICE_COUNT] = {
#ifdef GD32H77X_TEST_CNVM_BACKEND_PARTIAL
    {"GD32H77X_DEV_CNVM", "GD32H77DIW_CNVM", "CNVM", 0x200000U, 0x10000U, 32U, 0U, 0x08000000U, 0U, 1U, 0U},
#else
    {"GD32H77X_DEV_CNVM", "GD32H77DIW_CNVM", "CNVM", 0x200000U, 0x10000U, 32U, 0U, 0x08000000U, 0U, 0U, 0U},
#endif
    {"GD32H77X_DEV_MAPPED_TEST", "test", "TEST", 0x20000U, 0x1000U, 4U, 0U, 0x20000000U, 0U, 0U, 0U},
    {"GD32H77X_DEV_NOR", "GD25Q64ESIG", "NOR", 0x800000U, 0x1000U, 1U, 256U, 0U, 1U, 2U, 0U},
};

const gd32h77x_layout_binding_t gd32h77x_layout_bindings[GD32H77X_LAYOUT_DEVICE_COUNT] = {
    {NULL, NULL, NULL, {NULL, NULL, NULL, NULL}},
    {NULL, NULL, NULL, {NULL, NULL, NULL, NULL}},
    {"OSPI0", "PC11", "PF10", {"PF8", "PF9", "PF7", "PA1"}},
};

const gd32h77x_layout_segment_t gd32h77x_layout_segments[GD32H77X_LAYOUT_SEGMENT_COUNT] = {
    {0U, 0x30000U, 0x1D0000U, 0U, 0U},
    {0U, 0x10000U, 0x100000U, 0U, 0U},
    {0x10000U, 0x10000U, 0U, 2U, 0U},
    {0U, 0x10000U, 0x120000U, 0U, 0U},
    {0x10000U, 0x10000U, 0x180000U, 0U, 0U},
};

const gd32h77x_layout_partition_t gd32h77x_layout_partitions[GD32H77X_LAYOUT_PARTITION_COUNT] = {
    {"GD32H77X_PART_KV", 0x081D0000U, 0x30000U, 0x10000U, 32U, 0U, 1U, 0U},
    {"GD32H77X_PART_MIXED", 0x70000000U, 0x20000U, 0x10000U, 32U, 1U, 2U, 0U},
    {"GD32H77X_PART_SPLIT_CNVM", 0x72000000U, 0x20000U, 0x10000U, 32U, 3U, 2U, 0U},
};
