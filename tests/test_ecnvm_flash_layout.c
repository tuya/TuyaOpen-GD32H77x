/** @file test_ecnvm_flash_layout.c @brief H779 ECNVM KV geometry and routing test. */
#include "gd32h77x_ecnvm.h"
#include "gd32h77x_flash_route.h"
#include "gd32h77x_kv_test.h"
#include "mock_cnvm.h"
#include "mock_ecnvm.h"
#include "tkl_flash.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
    TUYA_FLASH_BASE_INFO_T info;
    uint8_t pattern[32];
    uint8_t readback[32];
    for (uint32_t i = 0U; i < sizeof(pattern); ++i) {
        pattern[i] = (uint8_t)(0xA5U ^ i);
    }

    assert(GD32H77X_ECNVM_CODE_END == UINT32_C(0x08900000));
    assert(GD32H77X_LAYOUT_TUYA_LOGICAL_BASE == UINT32_C(0x08900000));
    assert(GD32H77X_LAYOUT_TUYA_END == UINT32_C(0x08980000));
    assert(GD32H77X_LAYOUT_TUYA_ERASE_BYTES == UINT32_C(0x00001000));
    assert(GD32H77X_LAYOUT_TUYA_PROGRAM_BYTES == UINT32_C(8));

    mock_cnvm_reset();
    mock_ecnvm_reset();
    assert(gd32h77x_ecnvm_is_present());
    assert(gd32h77x_ecnvm_init() == OPRT_OK);
    uint32_t test_address = 0U;
    uint32_t test_size = 0U;
    assert(gd32h77x_tuya_reserved_test_page_get(&test_address, &test_size));
    assert(test_address == UINT32_C(0x08977000));
    assert(test_size == UINT32_C(0x1000));
    assert(tkl_flash_get_one_type_info(TUYA_FLASH_TYPE_UF, &info) == OPRT_OK);
    assert(info.partition_num == 1U);
    assert(info.partition[0].start_addr == UINT32_C(0x08900000));
    assert(info.partition[0].size == UINT32_C(0x00040000));
    assert(info.partition[0].block_size == UINT32_C(0x00001000));

    assert(gd32h77x_flash_preflight(UINT32_C(0x08900000), UINT32_C(0x1000),
                                    GD32H77X_FLASH_OP_ERASE) == GD32H77X_FLASH_ROUTE_OK);
    assert(gd32h77x_flash_preflight(UINT32_C(0x08900000), UINT32_C(8),
                                    GD32H77X_FLASH_OP_WRITE) == GD32H77X_FLASH_ROUTE_OK);
    assert(gd32h77x_flash_preflight(UINT32_C(0x08900000), UINT32_C(4),
                                    GD32H77X_FLASH_OP_WRITE) == GD32H77X_FLASH_ROUTE_INVALID);
    assert(gd32h77x_flash_preflight(UINT32_C(0x0897FFF0), UINT32_C(32),
                                    GD32H77X_FLASH_OP_WRITE) == GD32H77X_FLASH_ROUTE_INVALID);
    assert(gd32h77x_flash_preflight(UINT32_C(0x08900000) - UINT32_C(8), UINT32_C(8),
                                    GD32H77X_FLASH_OP_WRITE) == GD32H77X_FLASH_ROUTE_OK);
    assert(gd32h77x_flash_preflight(UINT32_C(0x08900000) - UINT32_C(8), UINT32_C(16),
                                    GD32H77X_FLASH_OP_WRITE) == GD32H77X_FLASH_ROUTE_INVALID);

    assert(gd32h77x_ecnvm_source_range_valid(
        (const void *)(uintptr_t)GD32H77X_LAYOUT_AXISRAM_BASE, sizeof(pattern)));
    assert(!gd32h77x_ecnvm_source_range_valid(
        (const void *)(uintptr_t)GD32H77X_ECNVM_CODE_BASE, sizeof(pattern)));
    assert(!gd32h77x_ecnvm_source_range_valid(
        (const void *)(uintptr_t)(GD32H77X_LAYOUT_AXISRAM_BASE - 1U), 1U));
    assert(!gd32h77x_ecnvm_source_range_valid(
        (const void *)(uintptr_t)(GD32H77X_LAYOUT_AXISRAM_BASE +
                                  GD32H77X_LAYOUT_AXISRAM_SIZE - 16U), 32U));

    assert(tkl_flash_erase(UINT32_C(0x08900000), UINT32_C(0x1000)) == OPRT_OK);
    assert(tkl_flash_write(UINT32_C(0x08900000),
                           (const uint8_t *)(uintptr_t)GD32H77X_ECNVM_CODE_BASE,
                           sizeof(pattern)) == OPRT_INVALID_PARM);
    assert(tkl_flash_write(UINT32_C(0x08900000), pattern, sizeof(pattern)) == OPRT_OK);
    assert(tkl_flash_read(UINT32_C(0x08900000), readback, sizeof(readback)) == OPRT_OK);
    assert(memcmp(pattern, readback, sizeof(pattern)) == 0);
    assert(mock_ecnvm_stats()->program_requests == 2U);
    assert(mock_ecnvm_stats()->program_commands == 1U);
    assert(mock_ecnvm_stats()->erase_requests == 1U);
    assert(mock_ecnvm_stats()->erase_commands == 1U);
    assert(mock_cnvm_stats()->read_requests == 0U);
    assert(mock_cnvm_stats()->program_requests == 0U);
    assert(mock_cnvm_stats()->erase_requests == 0U);
    return 0;
}
