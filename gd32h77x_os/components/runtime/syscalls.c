/**
 * @file syscalls.c
 * @brief newlib system call hooks for the GD32H77x platform.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "gd32h77x_platform.h"
#include "FreeRTOS.h"
#include "task.h"
#include <errno.h>
#include <reent.h>
#include <sys/types.h>

extern char      _end;
extern char      _heap_end;
static uintptr_t libc_break;

void *_sbrk(ptrdiff_t increment)
{
    taskENTER_CRITICAL();
    uintptr_t low  = (uintptr_t)&_end;
    uintptr_t high = (uintptr_t)&_heap_end;
    uintptr_t old  = libc_break ? libc_break : low;
    // The linker reserves a separate libc arena; never grow into the main stack.
    uintptr_t next;
    if (old < low || old > high) {
        taskEXIT_CRITICAL();
        errno = ENOMEM;
        return (void *)-1;
    }
    if (increment >= 0) {
        uintptr_t amount = (uintptr_t)increment;
        if (amount > high - old) {
            taskEXIT_CRITICAL();
            errno = ENOMEM;
            return (void *)-1;
        }
        next = old + amount;
    } else {
        uintptr_t amount = (uintptr_t)(-(increment + 1)) + 1U;
        if (amount > old - low) {
            taskEXIT_CRITICAL();
            errno = ENOMEM;
            return (void *)-1;
        }
        next = old - amount;
    }
    libc_break = next;
    taskEXIT_CRITICAL();
    return (void *)old;
}

void __malloc_lock(struct _reent *state)
{
    (void)state;
    vTaskSuspendAll();
}

void __malloc_unlock(struct _reent *state)
{
    (void)state;
    xTaskResumeAll();
}

int _write(int descriptor, const void *data, size_t size)
{
    if (descriptor != 1 && descriptor != 2) {
        errno = EBADF;
        return -1;
    }
    gd32h77x_platform_log_write(data, size);
    return (int)size;
}
