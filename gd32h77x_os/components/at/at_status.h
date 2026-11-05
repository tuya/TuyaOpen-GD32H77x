/**
 * @file at_status.h
 * @brief Neutral status values used by the AT component.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */
#ifndef AT_STATUS_H
#define AT_STATUS_H

typedef enum {
    AT_STATUS_OK = 0,
    AT_STATUS_ERROR,
    AT_STATUS_INVALID_PARAM,
    AT_STATUS_NOT_READY,
    AT_STATUS_UNSUPPORTED,
    AT_STATUS_IO_ERROR,
    AT_STATUS_TIMEOUT,
    AT_STATUS_NO_BUFFER,
    AT_STATUS_BUSY,
    AT_STATUS_NO_MEMORY,
} at_status_t;

#endif /* AT_STATUS_H */
