/**
 * @file gd32h77x_console.h
 * @brief Small native line console for GD32H77X standalone projects.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */
#ifndef GD32H77X_CONSOLE_H
#define GD32H77X_CONSOLE_H

#include <stddef.h>

typedef void (*gd32h77x_console_command_fn_t)(int argc, char *argv[]);

typedef struct {
    const char *name;
    const char *help;
    gd32h77x_console_command_fn_t handler;
} gd32h77x_console_command_t;

void gd32h77x_console_start(const gd32h77x_console_command_t *commands, size_t count);
void gd32h77x_console_printf(const char *format, ...);

#endif /* GD32H77X_CONSOLE_H */
