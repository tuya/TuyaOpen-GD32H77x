/**
 * @file at_bridge_app.c
 * @brief Minimal H77-side byte bridge for an attached AT module console.
 *
 * Runtime path: host USB-UART -> USART1 -> selected AT transport -> AT firmware, and back.
 * No Wi-Fi, network manager, socket backend or Tuya CLI is initialized.
 */

#include "gd32h77x_at_bridge.h"
#include "FreeRTOS.h"
#include "task.h"

void app_entry(void *argument)
{
    (void)argument;
    gd32h77x_at_bridge_start();
    vTaskDelete(NULL);
}
