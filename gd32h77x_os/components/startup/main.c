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
#include "gd32h77x_ecnvm.h"
#include "gd32h77x_status.h"

extern void app_entry(void *argument);

int main(void)
{
    /* Application .text/.rodata execute from ECNVM on GD32H779V_START.
     * ECNVM has a high instruction-fetch latency; without I-Cache, tight
     * software crypto loops in mbedTLS run several times slower. Keep D-Cache
     * disabled here because UART/DMA buffers require explicit coherency work. */
#if defined(__ICACHE_PRESENT) && (__ICACHE_PRESENT == 1U)
    SCB_EnableICache();
#endif
    gd32h77x_platform_init();
    if (gd32h77x_cnvm_init() != GD32H77X_STATUS_OK) {
        gd32h77x_platform_panic("CNVM initialization failed");
    }
    if (gd32h77x_ecnvm_is_present() && gd32h77x_ecnvm_init() != GD32H77X_STATUS_OK) {
        gd32h77x_platform_panic("ECNVM initialization failed");
    }
    configASSERT(xTaskCreate(app_entry, "app_entry", 2048, NULL, 4, NULL) == pdPASS);
    vTaskStartScheduler();
    gd32h77x_platform_panic("scheduler returned");
    return 1;
}
