/**
 * @file at_diag_app.c
 * @brief GD32H77X board diagnostic CLI entry point.
 */
#include "adapter_diag_cli.h"
#include "tkl_output.h"

void tuya_app_main(void)
{
    if (adapter_diag_cli_init() != OPRT_OK) {
        tkl_log_output("GD32H77X diag CLI init failed\r\n");
    }
}
