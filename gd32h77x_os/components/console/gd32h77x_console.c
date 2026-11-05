/**
 * @file gd32h77x_console.c
 * @brief Polled command console for native GD32H77X projects.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */
#include "gd32h77x_console.h"

#include "gd32h77x_78x.h"
#include "gd32h77x_config.h"
#include "gd32h77x_platform.h"
#include "FreeRTOS.h"
#include "task.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define GD32H77X_CONSOLE_LINE_SIZE 128U
#define GD32H77X_CONSOLE_MAX_ARGS 5
#define GD32H77X_CONSOLE_TASK_STACK 1024U
#define GD32H77X_CONSOLE_TASK_PRIORITY 2U

static const gd32h77x_console_command_t *s_commands;
static size_t s_command_count;

static void console_write(const char *text)
{
    gd32h77x_platform_log_write(text, strlen(text));
}

void gd32h77x_console_printf(const char *format, ...)
{
    char buffer[192];
    va_list args;

    va_start(args, format);
    (void)vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    console_write(buffer);
}

static void console_help(void)
{
    console_write("commands:\r\n");
    for (size_t index = 0U; index < s_command_count; ++index) {
        gd32h77x_console_printf("  %-20s %s\r\n", s_commands[index].name,
                                s_commands[index].help ? s_commands[index].help : "");
    }
}

static void console_execute(char *line)
{
    char *argv[GD32H77X_CONSOLE_MAX_ARGS];
    int argc = 0;
    char *cursor = line;

    while (*cursor != '\0' && argc < GD32H77X_CONSOLE_MAX_ARGS) {
        while (*cursor == ' ' || *cursor == '\t') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }
        argv[argc++] = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
            ++cursor;
        }
        if (*cursor != '\0') {
            *cursor++ = '\0';
        }
    }

    if (argc == 0) {
        return;
    }
    if (strcmp(argv[0], "help") == 0) {
        console_help();
        return;
    }
    for (size_t index = 0U; index < s_command_count; ++index) {
        if (strcmp(argv[0], s_commands[index].name) == 0) {
            s_commands[index].handler(argc, argv);
            return;
        }
    }
    gd32h77x_console_printf("unknown command: %s\r\n", argv[0]);
}

static void console_task(void *argument)
{
    char line[GD32H77X_CONSOLE_LINE_SIZE];
    size_t length = 0U;
    (void)argument;

    console_write("\r\nH77 native console ready. Type 'help'.\r\n> ");
    for (;;) {
        if (usart_flag_get(GD32H77X_CONSOLE_USART, USART_FLAG_RBNE) == RESET) {
            vTaskDelay(pdMS_TO_TICKS(5U));
            continue;
        }

        const char character = (char)(uint8_t)usart_data_receive(GD32H77X_CONSOLE_USART);
        if (character == '\r' || character == '\n') {
            if (length != 0U) {
                line[length] = '\0';
                console_write("\r\n");
                console_execute(line);
                length = 0U;
            }
            console_write("> ");
        } else if (character == '\b' || character == 0x7F) {
            if (length != 0U) {
                --length;
                console_write("\b \b");
            }
        } else if (character >= 0x20 && character != 0x7F && length + 1U < sizeof(line)) {
            line[length++] = character;
            char echo[2] = {character, '\0'};
            console_write(echo);
        }
    }
}

void gd32h77x_console_start(const gd32h77x_console_command_t *commands, size_t count)
{
    s_commands = commands;
    s_command_count = count;
    configASSERT(xTaskCreate(console_task, "h77_console", GD32H77X_CONSOLE_TASK_STACK, NULL,
                             GD32H77X_CONSOLE_TASK_PRIORITY, NULL) == pdPASS);
}
