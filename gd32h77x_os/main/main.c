/**
 * @file main.c
 * @brief GD32H77x platform bring-up and FreeRTOS adaptation.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "gd32h77x_78x.h"
#include "FreeRTOS.h"
#include "task.h"
#include "gd32h77x_platform.h"
#include "gd32h77x_cnvm.h"
#include "tkl_output.h"
#include "tuya_kconfig.h"
#if defined(GD32H77X_DIAG_CLI) && (GD32H77X_DIAG_CLI == 1)
#include "gd32h77x_diag_cli.h"
#endif

extern void tuya_app_main(void);
extern void vw553_probe(void);

static void app_entry(void *argument)
{
    (void)argument;
    tkl_log_output("%s (%s): FreeRTOS %s, CPU %lu Hz, CPUID %08lx\r\n", PLATFORM_CHIP, PLATFORM_BOARD,
                   tskKERNEL_VERSION_NUMBER,
                   (unsigned long)SystemCoreClock, (unsigned long)SCB->CPUID);
#if defined(GD32H77X_AT_PROBE) && GD32H77X_AT_PROBE == 1
    vw553_probe();
#endif
#if defined(GD32H77X_DIAG_CLI) && (GD32H77X_DIAG_CLI == 1)
    if (gd32h77x_diag_cli_init() != OPRT_OK) {
        tkl_log_output("GD32H77X diag CLI init failed\r\n");
    }
#endif
    tuya_app_main();
    vTaskDelete(NULL);
}

int main(void)
{
    gd32h77x_platform_init();
    if (gd32h77x_cnvm_init() != OPRT_OK) {
        gd32h77x_platform_panic("CNVM initialization failed");
    }
    configASSERT(xTaskCreate(app_entry, "tuya_entry", 2048, NULL, 4, NULL) == pdPASS);
    vTaskStartScheduler();
    gd32h77x_platform_panic("scheduler returned");
    return 1;
}
