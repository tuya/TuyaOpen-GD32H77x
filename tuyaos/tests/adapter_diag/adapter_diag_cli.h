/**
 * @file adapter_diag_cli.h
 * @brief GD32H77x bring-up log and bounded diagnostic CLI entry points.
 */

#ifndef ADAPTER_DIAG_CLI_H
#define ADAPTER_DIAG_CLI_H

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

OPERATE_RET adapter_diag_log_init(void);
OPERATE_RET adapter_diag_cli_init(void);

#ifdef __cplusplus
}
#endif

#endif /* ADAPTER_DIAG_CLI_H */
