/**
 * @file tkl_qspi.h
 * @brief GD32H77X QSPI transport API.
 *
 * The public contract is the TuyaOpen common QSPI API. The H77 backend maps
 * TUYA_QSPI_NUM_0 to SPI3 and supports 1-wire or 4-wire command/address/data
 * phases, plus 1-wire and quad read streams. The board layer owns CS/reset/
 * backlight. Command reads with dummy cycles and the QSPI IRQ APIs are not
 * implemented by this backend and return OPRT_NOT_SUPPORTED.
 *
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#ifndef __TKL_QSPI_H__
#define __TKL_QSPI_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_QSPI_FIFO_SIZE (256)

OPERATE_RET tkl_qspi_init(TUYA_QSPI_NUM_E port, const TUYA_QSPI_BASE_CFG_T *cfg);
OPERATE_RET tkl_qspi_deinit(TUYA_QSPI_NUM_E port);
OPERATE_RET tkl_qspi_send(TUYA_QSPI_NUM_E port, void *data, uint32_t size);
OPERATE_RET tkl_qspi_recv(TUYA_QSPI_NUM_E port, void *data, uint32_t size);
OPERATE_RET tkl_qspi_comand(TUYA_QSPI_NUM_E port, TUYA_QSPI_CMD_T *command);
OPERATE_RET tkl_qspi_abort_transfer(TUYA_QSPI_NUM_E port);
OPERATE_RET tkl_qspi_irq_init(TUYA_QSPI_NUM_E port, TUYA_QSPI_IRQ_CB cb);
OPERATE_RET tkl_qspi_irq_enable(TUYA_QSPI_NUM_E port);
OPERATE_RET tkl_qspi_irq_disable(TUYA_QSPI_NUM_E port);

#ifdef __cplusplus
}
#endif

#endif /* __TKL_QSPI_H__ */
