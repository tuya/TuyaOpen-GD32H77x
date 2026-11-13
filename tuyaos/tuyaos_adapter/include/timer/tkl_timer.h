/**
 * @file tkl_timer.h
 * @brief Common hardware timer adapter API.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#ifndef __TKL_TIMER_H__
#define __TKL_TIMER_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

OPERATE_RET tkl_timer_init(TUYA_TIMER_NUM_E timer_id, TUYA_TIMER_BASE_CFG_T *cfg);
OPERATE_RET tkl_timer_start(TUYA_TIMER_NUM_E timer_id, uint32_t us);
OPERATE_RET tkl_timer_stop(TUYA_TIMER_NUM_E timer_id);
OPERATE_RET tkl_timer_deinit(TUYA_TIMER_NUM_E timer_id);
OPERATE_RET tkl_timer_get_current_value(TUYA_TIMER_NUM_E timer_id, uint32_t *us);
OPERATE_RET tkl_timer_get(TUYA_TIMER_NUM_E timer_id, uint32_t *us);

#ifdef __cplusplus
}
#endif

#endif /* __TKL_TIMER_H__ */
