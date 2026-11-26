/**
 * @file tkl_vad.c
 * @brief GD32H77X VAD adapter stubs.
 *
 * Push-to-talk uses the manual VAD mode and never calls this backend. The
 * stubs are intentionally no-ops so generic AI components can be compiled.
 *
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */
#include "tkl_vad.h"

OPERATE_RET tkl_vad_init(TKL_VAD_CONFIG_T *config)
{
    (void)config;
    return OPRT_OK;
}

OPERATE_RET tkl_vad_feed(uint8_t *data, uint32_t len)
{
    (void)data;
    (void)len;
    return OPRT_OK;
}

TKL_VAD_STATUS_T tkl_vad_get_status(void)
{
    return TKL_VAD_STATUS_NONE;
}

OPERATE_RET tkl_vad_start(void)
{
    return OPRT_OK;
}

OPERATE_RET tkl_vad_stop(void)
{
    return OPRT_OK;
}

OPERATE_RET tkl_vad_deinit(void)
{
    return OPRT_OK;
}

OPERATE_RET tkl_vad_set_threshold(TKL_AUDIO_VAD_LEVEL_E level)
{
    (void)level;
    return OPRT_OK;
}
