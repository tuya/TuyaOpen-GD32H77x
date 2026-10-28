/*
 * Copyright (c) 2026 Tuya Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef GD32H77X_OSPI_MAILBOX_H
#define GD32H77X_OSPI_MAILBOX_H

#include <stdint.h>

#define OSPI_MAILBOX_ADDR UINT32_C(0x24010000)
#define OSPI_BUFFER_ADDR  UINT32_C(0x24011000)
#define OSPI_MAILBOX_MAGIC UINT32_C(0x48574F53)
#define OSPI_REQUEST_MAGIC UINT32_C(0x52514553)
#define OSPI_RESPONSE_MAGIC UINT32_C(0x52535053)

enum ospi_command {
    OSPI_CMD_NONE = 0,
    OSPI_CMD_READ_ID = 1,
    OSPI_CMD_READ_STATUS = 2,
    OSPI_CMD_READ = 3,
    OSPI_CMD_ERASE_4K = 4,
    OSPI_CMD_PROGRAM = 5,
};

enum ospi_status {
    OSPI_STATUS_IDLE = 0,
    OSPI_STATUS_RUNNING = 1,
    OSPI_STATUS_DONE = 2,
    OSPI_STATUS_ERROR = 3,
};

typedef struct {
    volatile uint32_t magic;
    volatile uint32_t request_magic;
    volatile uint32_t sequence;
    volatile uint32_t command;
    volatile uint32_t offset;
    volatile uint32_t length;
    volatile uint32_t status;
    volatile uint32_t result;
    volatile uint32_t jedec_id;
    volatile uint32_t status1;
    volatile uint32_t status2;
} ospi_mailbox_t;

#endif
