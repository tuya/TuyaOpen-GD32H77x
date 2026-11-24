/**
 * @file gd32h77x_status.h
 * @brief Native status codes shared by GD32H77X target drivers.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */
#ifndef GD32H77X_STATUS_H
#define GD32H77X_STATUS_H

typedef enum {
    GD32H77X_STATUS_OK = 0,
    GD32H77X_STATUS_INVALID_PARAM,
    GD32H77X_STATUS_NOT_READY,
    GD32H77X_STATUS_UNSUPPORTED,
    GD32H77X_STATUS_IO_ERROR,
    GD32H77X_STATUS_TIMEOUT,
    GD32H77X_STATUS_NO_MEMORY,
} gd32h77x_status_t;

#endif /* GD32H77X_STATUS_H */
