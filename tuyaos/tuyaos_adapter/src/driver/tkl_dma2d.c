/**
 * @file tkl_dma2d.c
 * @brief GD32H77X IPA-backed DMA2D TKL implementation.
 *
 * The GD32H77X image processing accelerator (IPA) is the hardware equivalent
 * of the DMA2D block exposed by the common TKL API.  This driver maps the
 * foreground-to-destination IPA mode to both same-format blits and pixel
 * format conversion.
 *
 * IPA line offsets are expressed in pixels and describe the pixels remaining
 * on a line after the copied block.  They are therefore configured as
 * frame_width - copy_width, while the memory base addresses include the
 * requested axis offset.
 *
 * Transfers are asynchronous.  IPA_IRQHandler() reports completion through
 * the callback registered by tkl_dma2d_init().
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "tkl_dma2d.h"

#include <stdbool.h>
#include <stdint.h>

#include "gd32h77x_78x.h"
#include "gd32h77x_78x_ipa.h"
#include "tkl_system.h"

#define GD32H77X_DMA2D_IRQ_PRIORITY 6U
#define GD32H77X_DMA2D_LINEOFF_MASK 0x3FFFU
#define GD32H77X_DMA2D_WIDTH_MASK   0x3FFFU
#define GD32H77X_DMA2D_HEIGHT_MASK  0xFFFFU

#define GD32H77X_DMA2D_INT_ERROR (IPA_INT_TAE | IPA_INT_WCF)
#define GD32H77X_DMA2D_INT_ALL   (GD32H77X_DMA2D_INT_ERROR | IPA_INT_FTF)
#define GD32H77X_DMA2D_FLAG_ALL  (IPA_FLAG_TAE | IPA_FLAG_WCF | IPA_FLAG_FTF)

typedef struct {
    uint32_t ppf;
    uint32_t dpf;
    uint8_t  bytes_per_pixel;
    bool     is_yuv422;
} GD32H77X_DMA2D_FORMAT;

typedef struct {
    uint32_t src_addr;
    uint32_t dst_addr;
    uint32_t src_lineoff;
    uint32_t dst_lineoff;
    uint32_t block_width;
    uint32_t block_height;
    uint32_t src_frame_bytes;
    uint32_t dst_frame_bytes;
    uint32_t ppf;
    uint32_t dpf;
    bool     convert;
} GD32H77X_DMA2D_CONFIG;

typedef struct {
    TUYA_DMA2D_IRQ_CB cb;
    void             *arg;
    volatile uint8_t  initialized;
    volatile uint8_t  busy;
    uint8_t          *dst_cache_addr;
    uint32_t          dst_cache_size;
} GD32H77X_DMA2D_STATE;

static GD32H77X_DMA2D_STATE s_dma2d;

static OPERATE_RET __dma2d_format_get(TUYA_FRAME_FMT_E type, bool destination,
                                      GD32H77X_DMA2D_FORMAT *format)
{
    switch (type) {
        case TUYA_FRAME_FMT_RGB565:
            format->ppf = FOREGROUND_PPF_RGB565;
            format->dpf = IPA_DPF_RGB565;
            format->bytes_per_pixel = 2U;
            format->is_yuv422 = false;
            return OPRT_OK;
        case TUYA_FRAME_FMT_RGB888:
            format->ppf = FOREGROUND_PPF_RGB888;
            format->dpf = IPA_DPF_RGB888;
            format->bytes_per_pixel = 3U;
            format->is_yuv422 = false;
            return OPRT_OK;
        case TUYA_FRAME_FMT_YUV422:
            if (destination) {
                /* IPA has no YUV422 destination pixel format. */
                return OPRT_NOT_SUPPORTED;
            }
            /* TUYA_FRAME_FMT_YUV422 defaults to packed UYVY byte order. */
            format->ppf = FOREGROUND_PPF_UYVY422_1P;
            format->dpf = 0U;
            format->bytes_per_pixel = 2U;
            format->is_yuv422 = true;
            return OPRT_OK;
        default:
            return OPRT_NOT_SUPPORTED;
    }
}

static bool __dma2d_addressable(const TKL_DMA2D_FRAME_INFO_T *frame,
                                uint32_t bytes)
{
    uintptr_t start = (uintptr_t)frame->pbuf;
    uintptr_t end = start + bytes;

    if (bytes == 0U || start > UINT32_MAX || end < start) {
        return false;
    }

    /* IPA memory address registers are 32-bit. */
    return end - 1U <= UINT32_MAX;
}

static OPERATE_RET __dma2d_config_prepare(const TKL_DMA2D_FRAME_INFO_T *src,
                                          const TKL_DMA2D_FRAME_INFO_T *dst,
                                          bool convert,
                                          GD32H77X_DMA2D_CONFIG *cfg)
{
    GD32H77X_DMA2D_FORMAT src_format = {0};
    GD32H77X_DMA2D_FORMAT dst_format = {0};
    uint32_t block_width;
    uint32_t block_height;
    uint64_t src_offset;
    uint64_t dst_offset;
    uint64_t src_bytes;
    uint64_t dst_bytes;
    OPERATE_RET rt;

    if (src == NULL || dst == NULL || cfg == NULL ||
        src->pbuf == NULL || dst->pbuf == NULL ||
        src->width == 0U || src->height == 0U ||
        dst->width == 0U || dst->height == 0U ||
        src->axis.x_axis >= src->width || src->axis.y_axis >= src->height ||
        dst->axis.x_axis >= dst->width || dst->axis.y_axis >= dst->height) {
        return OPRT_INVALID_PARM;
    }

    rt = __dma2d_format_get(src->type, false, &src_format);
    if (rt != OPRT_OK) {
        return rt;
    }
    rt = __dma2d_format_get(dst->type, true, &dst_format);
    if (rt != OPRT_OK) {
        return rt;
    }

    block_width = src->width_cp == 0U ? src->width : src->width_cp;
    block_height = src->height_cp == 0U ? src->height : src->height_cp;

    if (block_width > src->width - src->axis.x_axis) {
        block_width = src->width - src->axis.x_axis;
    }
    if (block_height > src->height - src->axis.y_axis) {
        block_height = src->height - src->axis.y_axis;
    }
    if (block_width > dst->width - dst->axis.x_axis) {
        block_width = dst->width - dst->axis.x_axis;
    }
    if (block_height > dst->height - dst->axis.y_axis) {
        block_height = dst->height - dst->axis.y_axis;
    }

    if (block_width == 0U || block_height == 0U ||
        block_width > GD32H77X_DMA2D_WIDTH_MASK ||
        block_height > GD32H77X_DMA2D_HEIGHT_MASK) {
        return OPRT_INVALID_PARM;
    }

    if (src_format.is_yuv422 &&
        (((src->axis.x_axis & 1U) != 0U) || ((block_width & 1U) != 0U))) {
        /* A packed YUV422 sample always contains two pixels. */
        return OPRT_INVALID_PARM;
    }

    cfg->src_lineoff = src->width - block_width;
    cfg->dst_lineoff = dst->width - block_width;
    if (cfg->src_lineoff > GD32H77X_DMA2D_LINEOFF_MASK ||
        cfg->dst_lineoff > GD32H77X_DMA2D_LINEOFF_MASK) {
        return OPRT_INVALID_PARM;
    }

    src_offset = ((uint64_t)src->axis.y_axis * src->width + src->axis.x_axis) *
                 src_format.bytes_per_pixel;
    dst_offset = ((uint64_t)dst->axis.y_axis * dst->width + dst->axis.x_axis) *
                 dst_format.bytes_per_pixel;
    src_bytes = (uint64_t)src->width * src->height * src_format.bytes_per_pixel;
    dst_bytes = (uint64_t)dst->width * dst->height * dst_format.bytes_per_pixel;

    if (src_offset > UINT32_MAX || dst_offset > UINT32_MAX ||
        src_bytes > UINT32_MAX || dst_bytes > UINT32_MAX ||
        !__dma2d_addressable(src, (uint32_t)src_bytes) ||
        !__dma2d_addressable(dst, (uint32_t)dst_bytes)) {
        return OPRT_INVALID_PARM;
    }

    cfg->src_addr = (uint32_t)((uintptr_t)src->pbuf + (uintptr_t)src_offset);
    cfg->dst_addr = (uint32_t)((uintptr_t)dst->pbuf + (uintptr_t)dst_offset);
    cfg->block_width = block_width;
    cfg->block_height = block_height;
    cfg->src_frame_bytes = (uint32_t)src_bytes;
    cfg->dst_frame_bytes = (uint32_t)dst_bytes;
    cfg->ppf = src_format.ppf;
    cfg->dpf = dst_format.dpf;
    cfg->convert = convert || src->type != dst->type;
    return OPRT_OK;
}

static bool __dma2d_dcache_enabled(void)
{
    return (SCB->CCR & SCB_CCR_DC_Msk) != 0U;
}

static void __dma2d_cache_clean(const GD32H77X_DMA2D_CONFIG *cfg,
                                const TKL_DMA2D_FRAME_INFO_T *src,
                                const TKL_DMA2D_FRAME_INFO_T *dst)
{
    if (!__dma2d_dcache_enabled()) {
        return;
    }

    SCB_CleanDCache_by_Addr((volatile void *)src->pbuf,
                            (int32_t)cfg->src_frame_bytes);
    /* Remove stale dirty lines before IPA writes the destination. */
    SCB_CleanDCache_by_Addr((volatile void *)dst->pbuf,
                            (int32_t)cfg->dst_frame_bytes);
}

static void __dma2d_cache_invalidate_destination(void)
{
    if (!__dma2d_dcache_enabled() || s_dma2d.dst_cache_addr == NULL) {
        return;
    }

    SCB_InvalidateDCache_by_Addr((volatile void *)s_dma2d.dst_cache_addr,
                                 (int32_t)s_dma2d.dst_cache_size);
}

static void __dma2d_hw_configure(const GD32H77X_DMA2D_CONFIG *cfg)
{
    ipa_foreground_parameter_struct foreground;
    ipa_destination_parameter_struct destination;

    ipa_foreground_struct_para_init(&foreground);
    foreground.foreground_memaddr = cfg->src_addr;
    foreground.foreground_lineoff = cfg->src_lineoff;
    foreground.foreground_pf = cfg->ppf;
    ipa_foreground_init(&foreground);

    ipa_destination_struct_para_init(&destination);
    destination.destination_memaddr = cfg->dst_addr;
    destination.destination_lineoff = cfg->dst_lineoff;
    destination.destination_pf = cfg->dpf;
    destination.image_width = cfg->block_width;
    destination.image_height = cfg->block_height;
    destination.image_scaling_width = cfg->block_width;
    destination.image_scaling_height = cfg->block_height;
    ipa_destination_init(&destination);

    if (cfg->convert) {
        ipa_conversion_parameter_struct conversion;

        ipa_color_conversion_struct_para_init(&conversion, IPA_COLORSPACE_YCBCR);
        ipa_color_conversion_config(&conversion);
        ipa_pixel_format_convert_mode_set(IPA_FGTODE_PF_CONVERT);
    } else {
        ipa_pixel_format_convert_mode_set(IPA_FGTODE);
    }
}

static OPERATE_RET __dma2d_start(const TKL_DMA2D_FRAME_INFO_T *src,
                                 const TKL_DMA2D_FRAME_INFO_T *dst,
                                 bool convert)
{
    GD32H77X_DMA2D_CONFIG cfg;
    uint32_t irq_mask;
    OPERATE_RET rt;

    rt = __dma2d_config_prepare(src, dst, convert, &cfg);
    if (rt != OPRT_OK) {
        return rt;
    }

    irq_mask = tkl_system_enter_critical();
    if (!s_dma2d.initialized || s_dma2d.busy) {
        tkl_system_exit_critical(irq_mask);
        return OPRT_COM_ERROR;
    }

    s_dma2d.busy = 1U;
    s_dma2d.dst_cache_addr = dst->pbuf;
    s_dma2d.dst_cache_size = cfg.dst_frame_bytes;
    ipa_interrupt_disable(GD32H77X_DMA2D_INT_ALL);
    NVIC_DisableIRQ(IPA_IRQn);
    ipa_flag_clear(GD32H77X_DMA2D_FLAG_ALL);
    NVIC_ClearPendingIRQ(IPA_IRQn);
    tkl_system_exit_critical(irq_mask);

    __dma2d_cache_clean(&cfg, src, dst);
    __dma2d_hw_configure(&cfg);

    irq_mask = tkl_system_enter_critical();
    ipa_flag_clear(GD32H77X_DMA2D_FLAG_ALL);
    ipa_interrupt_enable(GD32H77X_DMA2D_INT_ALL);
    NVIC_ClearPendingIRQ(IPA_IRQn);
    NVIC_EnableIRQ(IPA_IRQn);
    ipa_transfer_enable();
    tkl_system_exit_critical(irq_mask);

    return OPRT_OK;
}

void IPA_IRQHandler(void)
{
    uint32_t flags = IPA_INTF & GD32H77X_DMA2D_FLAG_ALL;
    TUYA_DMA2D_IRQ_E event;
    TUYA_DMA2D_IRQ_CB cb;
    void *arg;

    if (flags == 0U) {
        return;
    }

    ipa_flag_clear(flags);
    if (!s_dma2d.busy) {
        return;
    }

    s_dma2d.busy = 0U;
    ipa_interrupt_disable(GD32H77X_DMA2D_INT_ALL);

    __dma2d_cache_invalidate_destination();
    s_dma2d.dst_cache_addr = NULL;
    s_dma2d.dst_cache_size = 0U;

    event = (flags & GD32H77X_DMA2D_INT_ERROR) != 0U ?
            TUYA_DMA2D_TRANS_ERROR_ISR : TUYA_DMA2D_TRANS_COMPLETE_ISR;
    cb = s_dma2d.cb;
    arg = s_dma2d.arg;

    if (cb != NULL) {
        cb(event, arg);
    }
}

OPERATE_RET tkl_dma2d_init(const TUYA_DMA2D_BASE_CFG_T *cfg)
{
    uint32_t irq_mask = tkl_system_enter_critical();

    if (s_dma2d.initialized) {
        if (cfg != NULL) {
            s_dma2d.cb = cfg->cb;
            s_dma2d.arg = cfg->arg;
        }
        tkl_system_exit_critical(irq_mask);
        return OPRT_OK;
    }

    rcu_periph_clock_enable(RCU_IPA);
    ipa_deinit();
    ipa_interrupt_disable(GD32H77X_DMA2D_INT_ALL);
    ipa_flag_clear(GD32H77X_DMA2D_FLAG_ALL);

    NVIC_DisableIRQ(IPA_IRQn);
    NVIC_ClearPendingIRQ(IPA_IRQn);
    NVIC_SetPriority(IPA_IRQn, GD32H77X_DMA2D_IRQ_PRIORITY);

    s_dma2d.cb = cfg == NULL ? NULL : cfg->cb;
    s_dma2d.arg = cfg == NULL ? NULL : cfg->arg;
    s_dma2d.busy = 0U;
    s_dma2d.dst_cache_addr = NULL;
    s_dma2d.dst_cache_size = 0U;
    s_dma2d.initialized = 1U;

    tkl_system_exit_critical(irq_mask);
    return OPRT_OK;
}

OPERATE_RET tkl_dma2d_deinit(void)
{
    uint32_t irq_mask = tkl_system_enter_critical();

    if (!s_dma2d.initialized) {
        tkl_system_exit_critical(irq_mask);
        return OPRT_OK;
    }

    NVIC_DisableIRQ(IPA_IRQn);
    NVIC_ClearPendingIRQ(IPA_IRQn);
    ipa_interrupt_disable(GD32H77X_DMA2D_INT_ALL);
    ipa_transfer_stop_enable();
    ipa_deinit();
    rcu_periph_clock_disable(RCU_IPA);

    s_dma2d.cb = NULL;
    s_dma2d.arg = NULL;
    s_dma2d.busy = 0U;
    s_dma2d.dst_cache_addr = NULL;
    s_dma2d.dst_cache_size = 0U;
    s_dma2d.initialized = 0U;

    tkl_system_exit_critical(irq_mask);
    return OPRT_OK;
}

OPERATE_RET tkl_dma2d_convert(TKL_DMA2D_FRAME_INFO_T *src,
                              TKL_DMA2D_FRAME_INFO_T *dst)
{
    if (src == NULL || dst == NULL) {
        return OPRT_INVALID_PARM;
    }

    return __dma2d_start(src, dst, src->type != dst->type);
}

OPERATE_RET tkl_dma2d_memcpy(TKL_DMA2D_FRAME_INFO_T *src,
                             TKL_DMA2D_FRAME_INFO_T *dst)
{
    if (src == NULL || dst == NULL) {
        return OPRT_INVALID_PARM;
    }

    return __dma2d_start(src, dst, src->type != dst->type);
}
