/**
 * @file at_status_map.h
 * @brief Map neutral AT status values into TuyaOpen adapter status codes.
 */
#ifndef AT_STATUS_MAP_H
#define AT_STATUS_MAP_H

#include "at_status.h"
#include "tuya_error_code.h"

static inline OPERATE_RET tuya_at_status_to_oprt(at_status_t status)
{
    switch (status) {
    case AT_STATUS_OK: return OPRT_OK;
    case AT_STATUS_INVALID_PARAM: return OPRT_INVALID_PARM;
    case AT_STATUS_NOT_READY: return OPRT_RESOURCE_NOT_READY;
    case AT_STATUS_UNSUPPORTED: return OPRT_NOT_SUPPORTED;
    case AT_STATUS_TIMEOUT: return OPRT_TIMEOUT;
    case AT_STATUS_NO_BUFFER: return OPRT_BUFFER_NOT_ENOUGH;
    case AT_STATUS_NO_MEMORY: return OPRT_MALLOC_FAILED;
    default: return OPRT_COM_ERROR;
    }
}

#endif /* AT_STATUS_MAP_H */
