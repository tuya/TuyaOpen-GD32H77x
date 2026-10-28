/**
 * @file gd32h77x_diag_cli.h
 * @brief GD32H77x bring-up log and bounded diagnostic CLI entry points.
 */

#ifndef GD32H77X_DIAG_CLI_H
#define GD32H77X_DIAG_CLI_H

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

OPERATE_RET gd32h77x_diag_log_init(void);
OPERATE_RET gd32h77x_diag_cli_init(void);

#ifdef __cplusplus
}
#endif

#endif /* GD32H77X_DIAG_CLI_H */
