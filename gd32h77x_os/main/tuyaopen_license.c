/**
 * @file tuyaopen_license.c
 * @brief Explicitly unsupported OTP license storage for GD32H77X.
 *
 * The selected board layout has no dedicated OTP/license partition. Do not
 * alias metadata, boot, or KV storage here: the caller can use its normal KV
 * authorization path instead.
 */
#include "tuyaopen_license.h"
#include "tuya_error_code.h"
#include <stddef.h>

int tuyaopen_license_write(const char *data, uint32_t data_len)
{
    (void)data;
    (void)data_len;
    return OPRT_NOT_SUPPORTED;
}

int tuyaopen_license_read(char **data, uint32_t *data_len)
{
    if (data) {
        *data = NULL;
    }
    if (data_len) {
        *data_len = 0U;
    }
    return OPRT_NOT_SUPPORTED;
}
