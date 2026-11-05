/**
 * @file app_entry.c
 * @brief Default TuyaOpen application entry for GD32H77X.
 *
 * Platform-specific projects provide their own app_entry and do not compile
 * this file. Generic TuyaOpen applications use this adapter to reach their
 * tuya_app_main().
 */
#include "gd32h77x_78x.h"
#include "FreeRTOS.h"
#include "task.h"
#include "tkl_output.h"
#include "gd32h77x_config.h"

extern void tuya_app_main(void);

void app_entry(void *argument)
{
    (void)argument;
    tkl_log_output("%s (%s): FreeRTOS %s, CPU %lu Hz, CPUID %08lx\r\n",
                   GD32H77X_CONFIG_CHIP, GD32H77X_CONFIG_BOARD,
                   tskKERNEL_VERSION_NUMBER,
                   (unsigned long)SystemCoreClock, (unsigned long)SCB->CPUID);
    tuya_app_main();
    vTaskDelete(NULL);
}
