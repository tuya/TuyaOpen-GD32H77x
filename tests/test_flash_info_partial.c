/**
 * @file test_flash_info_partial.c
 * @brief Do not advertise KV when the layout backend cannot write and erase it.
 */
#include "gd32h77x_cnvm.h"
#include "tkl_flash.h"

#include <assert.h>

int main(void)
{
    TUYA_FLASH_BASE_INFO_T info;
    assert(gd32h77x_cnvm_init() == GD32H77X_STATUS_OK);
    assert(tkl_flash_get_one_type_info(TUYA_FLASH_TYPE_UF, &info) == OPRT_NOT_SUPPORTED);
    assert(tkl_flash_get_one_type_info(TUYA_FLASH_TYPE_KV_DATA, &info) == OPRT_NOT_SUPPORTED);
    return 0;
}
