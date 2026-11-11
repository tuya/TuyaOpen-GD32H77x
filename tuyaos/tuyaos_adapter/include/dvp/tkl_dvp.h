/**
 * @file tkl_dvp.h
 * @brief GD32H77X DVP TKL interface.
 *
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#ifndef __TKL_DVP_H__
#define __TKL_DVP_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

OPERATE_RET tkl_dvp_init(TUYA_DVP_CFG_T *dvp_cfg);
OPERATE_RET tkl_dvp_deinit(void);
OPERATE_RET tkl_dvp_start(void);
OPERATE_RET tkl_dvp_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* __TKL_DVP_H__ */
