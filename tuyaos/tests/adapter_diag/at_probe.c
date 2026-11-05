/**
 * @file at_probe.c
 * @brief Optional non-mutating AT UART liveness probe.
 */

#include "at_device.h"
#include "tkl_output.h"
#include "tuya_error_code.h"
#include "FreeRTOS.h"
#include "task.h"

void at_probe(void)
{
    at_result_t result = AT_RESULT_NONE;
    int32_t status = at_device_init();
    if (status != OPRT_OK) {
        tkl_log_output("553 AT PROBE: init failed (%d)\r\n", status);
        return;
    }

    /* Drain module startup text while the RX task owns the parser. */
    vTaskDelay(pdMS_TO_TICKS(200U));
    static const at_request_t request = {
        .command = "AT", .command_length = 2U, .timeout_ms = 3000U};
    status = at_device_execute(&request, &result);
    tkl_log_output("553 AT PROBE: %s (transport=%d, result=%d)\r\n",
                   status == OPRT_OK && result == AT_RESULT_OK ? "OK" : "FAILED", status, result);
}
