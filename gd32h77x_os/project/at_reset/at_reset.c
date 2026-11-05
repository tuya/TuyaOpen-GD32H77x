/**
 * @file at_reset.c
 * @brief Native H77 console helper that drives AT module NRST.
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "gd32h77x_78x.h"
#include "gd32h77x_config.h"
#include "gd32h77x_console.h"
#include "FreeRTOS.h"
#include "task.h"

#define NRST_PULSE_MS 20U
#define BOOT_DELAY_MS 10U
#define RECOVERY_DELAY_MS 500U

static void at_pulse_reset(void)
{
    gd32h77x_console_printf("at_reset: high -> low 20 ms -> high\r\n");
    rcu_periph_clock_enable(AT_RESET_CLOCK);
    gpio_mode_set(AT_RESET_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, AT_RESET_PIN);
    gpio_output_options_set(AT_RESET_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_12MHZ, AT_RESET_PIN);

    gpio_bit_set(AT_RESET_PORT, AT_RESET_PIN);
    vTaskDelay(pdMS_TO_TICKS(BOOT_DELAY_MS));
    gpio_bit_reset(AT_RESET_PORT, AT_RESET_PIN);
    vTaskDelay(pdMS_TO_TICKS(NRST_PULSE_MS));
    gpio_bit_set(AT_RESET_PORT, AT_RESET_PIN);
    vTaskDelay(pdMS_TO_TICKS(RECOVERY_DELAY_MS));
    gd32h77x_console_printf("at_reset: done\r\n");
}

static void reset_command(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    at_pulse_reset();
}

static const gd32h77x_console_command_t s_commands[] = {
    {"reset_at", "Pulse the configured AT module NRST", reset_command},
};

void app_entry(void *argument)
{
    (void)argument;
    gd32h77x_console_printf("AT reset helper ready\r\n");
    at_pulse_reset();
    gd32h77x_console_start(s_commands, sizeof(s_commands) / sizeof(s_commands[0]));
    vTaskDelete(NULL);
}
