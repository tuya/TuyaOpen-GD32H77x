/**
 * @file gd32h77x_kv_test.h
 * @brief Platform-owned lookup for the reserved ECNVM KV diagnostic page.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */
#ifndef GD32H77X_KV_TEST_H
#define GD32H77X_KV_TEST_H

#include <stdbool.h>
#include <stdint.h>

bool gd32h77x_tuya_reserved_test_page_get(uint32_t *address, uint32_t *size);

#endif /* GD32H77X_KV_TEST_H */
