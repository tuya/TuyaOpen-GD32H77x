/**
 * @file test_storage_bounds.c
 * @brief Check KV flash boundaries and unavailable programming operations.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "gd32h77x_storage.h"
#include "gd32h77x_cnvm.h"
#include "mock_cnvm.h"
#include "tkl_flash.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
    uint8_t readback[32];
    TUYA_FLASH_BASE_INFO_T info_uf;
    TUYA_FLASH_BASE_INFO_T info_kv;
    TUYA_FLASH_BASE_INFO_T info_other;

    assert(GD32H77X_LAYOUT_KV_LOGICAL_BASE == UINT32_C(0x081D0000));
    assert(GD32H77X_LAYOUT_KV_SIZE_BYTES == UINT32_C(0x00030000));
    assert(GD32H77X_LAYOUT_KV_END == UINT32_C(0x08200000));
    assert(gd32h77x_tuya_range_valid(GD32H77X_LAYOUT_KV_LOGICAL_BASE,
                                   GD32H77X_LAYOUT_KV_SIZE_BYTES));
    assert(gd32h77x_tuya_range_valid(GD32H77X_LAYOUT_KV_END, 0U));
    assert(!gd32h77x_tuya_range_valid(GD32H77X_LAYOUT_KV_LOGICAL_BASE - 1U, 1U));
    assert(!gd32h77x_tuya_range_valid(GD32H77X_LAYOUT_KV_LOGICAL_BASE,
                                    GD32H77X_LAYOUT_KV_SIZE_BYTES + 1U));
    assert(!gd32h77x_tuya_range_valid(UINT32_MAX - 7U, 16U));
    assert(gd32h77x_tuya_program_range_valid(GD32H77X_LAYOUT_KV_LOGICAL_BASE, 32U));
    assert(gd32h77x_tuya_program_range_valid(GD32H77X_LAYOUT_KV_END - 32U, 32U));
    assert(!gd32h77x_tuya_program_range_valid(GD32H77X_LAYOUT_KV_LOGICAL_BASE + 1U, 32U));
    assert(!gd32h77x_tuya_program_range_valid(GD32H77X_LAYOUT_KV_LOGICAL_BASE, 31U));
    assert(gd32h77x_tuya_erase_range_valid(GD32H77X_LAYOUT_KV_LOGICAL_BASE,
                                         GD32H77X_LAYOUT_KV_ERASE_BYTES));
    assert(gd32h77x_tuya_erase_range_valid(GD32H77X_LAYOUT_KV_LOGICAL_BASE,
                                         GD32H77X_LAYOUT_KV_SIZE_BYTES));
    assert(!gd32h77x_tuya_erase_range_valid(GD32H77X_LAYOUT_KV_LOGICAL_BASE + 1U,
                                          GD32H77X_LAYOUT_KV_ERASE_BYTES));
    assert(!gd32h77x_tuya_erase_range_valid(GD32H77X_LAYOUT_KV_LOGICAL_BASE, 0U));
    assert(!gd32h77x_tuya_erase_range_valid(GD32H77X_LAYOUT_KV_LOGICAL_BASE,
                                          GD32H77X_LAYOUT_KV_ERASE_BYTES + 1U));
    mock_cnvm_reset();
    assert(tkl_flash_read(GD32H77X_LAYOUT_KV_LOGICAL_BASE - 32U, readback, sizeof(readback)) == OPRT_INVALID_PARM);
    assert(tkl_flash_read(GD32H77X_LAYOUT_KV_LOGICAL_BASE, readback, sizeof(readback)) == OPRT_OK);
    assert(mock_cnvm_stats()->read_requests == 1U);
    assert(tkl_flash_read(GD32H77X_LAYOUT_KV_END, readback, 1U) == OPRT_INVALID_PARM);
    assert(tkl_flash_write(GD32H77X_LAYOUT_KV_LOGICAL_BASE - 32U,
                           (const uint8_t *)"x", 32U) == OPRT_INVALID_PARM);
    assert(tkl_flash_write(GD32H77X_LAYOUT_KV_LOGICAL_BASE,
                           (const uint8_t *)"x", 32U) == OPRT_RESOURCE_NOT_READY);
    assert(tkl_flash_erase(GD32H77X_LAYOUT_KV_LOGICAL_BASE,
                           GD32H77X_LAYOUT_KV_ERASE_BYTES) == OPRT_RESOURCE_NOT_READY);
    /* A valid non-KV partition must stay inaccessible through the TKL API. */
    assert(tkl_flash_read(0x70000000U, readback, sizeof(readback)) == OPRT_NOT_SUPPORTED);
    assert(tkl_flash_write(0x70000000U, (const uint8_t *)"x", 32U) == OPRT_NOT_SUPPORTED);
    assert(tkl_flash_erase(0x70000000U, GD32H77X_LAYOUT_KV_ERASE_BYTES) == OPRT_NOT_SUPPORTED);
    assert(tkl_flash_read(0x71000000U, readback, sizeof(readback)) == OPRT_INVALID_PARM);
    assert(tkl_flash_write(0x71000000U, (const uint8_t *)"x", 32U) == OPRT_INVALID_PARM);
    assert(tkl_flash_erase(0x71000000U, GD32H77X_LAYOUT_KV_ERASE_BYTES) == OPRT_INVALID_PARM);
    assert(mock_cnvm_stats()->read_requests == 1U);
    assert(mock_cnvm_stats()->program_requests == 1U);
    assert(mock_cnvm_stats()->erase_requests == 1U);
    assert(tkl_flash_write(GD32H77X_LAYOUT_KV_LOGICAL_BASE, NULL, 0U) == OPRT_OK);

    mock_cnvm_reset();
    assert(tkl_flash_get_one_type_info(TUYA_FLASH_TYPE_UF, &info_uf) == OPRT_RESOURCE_NOT_READY);
    assert(tkl_flash_get_one_type_info(TUYA_FLASH_TYPE_KV_DATA, &info_kv) == OPRT_RESOURCE_NOT_READY);
    assert(gd32h77x_cnvm_init() == GD32H77X_STATUS_OK);
    assert(tkl_flash_read(GD32H77X_LAYOUT_KV_LOGICAL_BASE, readback, sizeof(readback)) == OPRT_OK);
    assert(readback[0] == 0xFFU && mock_cnvm_stats()->read_requests == 1U);
    assert(tkl_flash_erase(GD32H77X_LAYOUT_KV_LOGICAL_BASE,
                           GD32H77X_LAYOUT_KV_ERASE_BYTES) == OPRT_OK);
    assert(mock_cnvm_stats()->erase_requests == 1U && mock_cnvm_stats()->erase_commands == 1U);
    assert(tkl_flash_read(GD32H77X_LAYOUT_KV_LOGICAL_BASE, readback, sizeof(readback)) == OPRT_OK);
    for (size_t index = 0U; index < sizeof(readback); ++index) {
        assert(readback[index] == 0xFFU);
    }
    const uint8_t program_data[32] = {
        0xA5U, 0x5AU, 0xC3U, 0x3CU, 0xA5U, 0x5AU, 0xC3U, 0x3CU,
        0xA5U, 0x5AU, 0xC3U, 0x3CU, 0xA5U, 0x5AU, 0xC3U, 0x3CU,
        0xA5U, 0x5AU, 0xC3U, 0x3CU, 0xA5U, 0x5AU, 0xC3U, 0x3CU,
        0xA5U, 0x5AU, 0xC3U, 0x3CU, 0xA5U, 0x5AU, 0xC3U, 0x3CU,
    };
    assert(tkl_flash_write(GD32H77X_LAYOUT_KV_LOGICAL_BASE,
                           program_data, sizeof(program_data)) == OPRT_OK);
    assert(mock_cnvm_stats()->program_requests == 1U && mock_cnvm_stats()->program_commands == 1U);
    assert(tkl_flash_read(GD32H77X_LAYOUT_KV_LOGICAL_BASE,
                          readback, sizeof(program_data)) == OPRT_OK);
    assert(memcmp(readback, program_data, sizeof(program_data)) == 0);
    assert(tkl_flash_read(0x70000000U, readback, sizeof(readback)) == OPRT_NOT_SUPPORTED);
    assert(mock_cnvm_stats()->read_requests == 3U);

    assert(tkl_flash_get_one_type_info(TUYA_FLASH_TYPE_UF, &info_uf) == OPRT_OK);
    assert(tkl_flash_get_one_type_info(TUYA_FLASH_TYPE_KV_DATA, &info_kv) == OPRT_OK);
    assert(info_uf.partition_num == 1U && info_kv.partition_num == 1U);
    assert(info_uf.partition[0].start_addr == GD32H77X_LAYOUT_KV_LOGICAL_BASE);
    assert(info_uf.partition[0].size == UINT32_C(0x00010000));
    assert(info_uf.partition[0].block_size == GD32H77X_LAYOUT_KV_ERASE_BYTES);
    assert(info_kv.partition[0].start_addr == info_uf.partition[0].start_addr + UINT32_C(0x00010000));
    assert(info_kv.partition[0].size == UINT32_C(0x00010000));
    assert(tkl_flash_get_one_type_info(TUYA_FLASH_TYPE_APP, &info_other) != OPRT_OK);
    return 0;
}
