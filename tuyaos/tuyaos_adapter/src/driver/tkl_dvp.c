/**
 * @file tkl_dvp.c
 * @brief GD32H77X DCI/DMA DVP backend.
 *
 * The sensor driver owns MCLK, power, reset and SCCB. This layer owns the DCI
 * pin mux, DMA1 channel 7 and hand-off of complete YUV422 frames to the camera
 * TDD layer.
 *
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "tkl_dvp.h"

#include "tal_api.h"

#ifdef BIT
#undef BIT
#endif

#include "gd32h77x_78x.h"
#include "gd32h77x_78x_dci.h"
#include "gd32h77x_78x_dma.h"
#include "tkl_pinmux.h"

#define GD32H77X_DVP_WIDTH             320U
#define GD32H77X_DVP_HEIGHT            240U
#define GD32H77X_DVP_BYTES             (GD32H77X_DVP_WIDTH * GD32H77X_DVP_HEIGHT * 2U)
#define GD32H77X_DVP_WORDS             (GD32H77X_DVP_BYTES / 4U)
#define GD32H77X_DVP_TIMEOUT_MS        1500U

typedef struct {
    TUYA_PIN_FUNC_E func;
    uint32_t port;
    uint32_t pin;
    uint32_t af;
} DVP_PIN_T;

#define DVP_PIN(func_name, port_name, pin_num, af_val) \
    {func_name, port_name, GPIO_PIN_##pin_num, af_val}

/* Default DCI routing used on the verified camera board.  Comments include
 * the matching Tuya GPIO ID; tkl_pinmux overwrites entries before init.
 */
static DVP_PIN_T s_dvp_pins[] = {
    DVP_PIN(TUYA_DVP0_D0,     GPIOC, 6,  13), /* PC6 / TUYA_GPIO_NUM_38 / AF13 */
    DVP_PIN(TUYA_DVP0_D1,     GPIOC, 7,  13), /* PC7 / TUYA_GPIO_NUM_39 / AF13 */
    DVP_PIN(TUYA_DVP0_D2,     GPIOB, 13, 13), /* PB13 / TUYA_GPIO_NUM_29 / AF13 */
    DVP_PIN(TUYA_DVP0_D3,     GPIOH, 12, 13), /* PH12 / TUYA_GPIO_NUM_124 / AF13 */
    DVP_PIN(TUYA_DVP0_D4,     GPIOH, 14, 13), /* PH14 / TUYA_GPIO_NUM_126 / AF13 */
    DVP_PIN(TUYA_DVP0_D5,     GPIOD, 3,  13), /* PD3 / TUYA_GPIO_NUM_51 / AF13 */
    DVP_PIN(TUYA_DVP0_D6,     GPIOB, 8,  13), /* PB8 / TUYA_GPIO_NUM_24 / AF13 */
    DVP_PIN(TUYA_DVP0_D7,     GPIOB, 9,  13), /* PB9 / TUYA_GPIO_NUM_25 / AF13 */
    DVP_PIN(TUYA_DVP0_PIXCLK, GPIOA, 6,  13), /* PA6 / TUYA_GPIO_NUM_6 / AF13 */
    DVP_PIN(TUYA_DVP0_HSYNC,  GPIOH, 15, 1),  /* PH15 / TUYA_GPIO_NUM_127 / AF1 */
    DVP_PIN(TUYA_DVP0_VSYNC,  GPIOB, 7,  13), /* PB7 / TUYA_GPIO_NUM_23 / AF13 */
};

void __tkl_dvp_set_pin(TUYA_PIN_FUNC_E func, uint32_t gpio_port,
                       uint32_t gpio_pin, uint32_t af)
{
    if (func < TUYA_DVP0_D0 || func > TUYA_DVP0_VSYNC) {
        return;
    }
    for (uint32_t i = 0U; i < sizeof(s_dvp_pins) / sizeof(s_dvp_pins[0]); ++i) {
        if (s_dvp_pins[i].func == func) {
            s_dvp_pins[i].port = gpio_port;
            s_dvp_pins[i].pin = gpio_pin;
            s_dvp_pins[i].af = af;
            return;
        }
    }
}

typedef struct {
    TUYA_DVP_CFG_T cfg;
    TUYA_DVP_FRAME_MANAGE_T *frame;
    THREAD_HANDLE task;
    volatile bool started;
    volatile bool busy;
    volatile bool exit_requested;
    uint32_t frame_id;
    uint32_t capture_attempts;
    uint32_t last_capture_ms;
} GD32H77X_DVP_STATE;

static GD32H77X_DVP_STATE s_dvp;
static uint8_t s_dma_frame[GD32H77X_DVP_BYTES] __attribute__((aligned(32)));

static uint32_t __dvp_gpio_rcu(uint32_t port)
{
    static const uint32_t ports[] = {
        GPIOA, GPIOB, GPIOC, GPIOD, GPIOE, GPIOF, GPIOG, GPIOH, GPIOJ, GPIOK,
    };
    static const uint32_t rcus[] = {
        RCU_GPIOA, RCU_GPIOB, RCU_GPIOC, RCU_GPIOD, RCU_GPIOE,
        RCU_GPIOF, RCU_GPIOG, RCU_GPIOH, RCU_GPIOJ, RCU_GPIOK,
    };

    for (uint32_t i = 0U; i < sizeof(ports) / sizeof(ports[0]); ++i) {
        if (ports[i] == port) {
            return rcus[i];
        }
    }
    return 0U;
}

static void __dvp_dci_gpio_config(void)
{
    for (uint32_t i = 0U; i < sizeof(s_dvp_pins) / sizeof(s_dvp_pins[0]); ++i) {
        const DVP_PIN_T *pin = &s_dvp_pins[i];
        uint32_t rcu = __dvp_gpio_rcu(pin->port);

        if (rcu == 0U) {
            continue;
        }
        rcu_periph_clock_enable(rcu);
        gpio_af_set(pin->port, pin->af, pin->pin);
        gpio_mode_set(pin->port, GPIO_MODE_AF, GPIO_PUPD_NONE, pin->pin);
        gpio_output_options_set(pin->port, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ,
                                pin->pin);
    }
}

static void __dvp_dci_config(void)
{
    dci_parameter_struct cfg;

    rcu_periph_clock_enable(RCU_DCI);
    memset(&cfg, 0, sizeof(cfg));
    cfg.capture_mode = DCI_CAPTURE_MODE_SNAPSHOT;
    cfg.clock_polarity = DCI_CK_POLARITY_RISING;
    cfg.hsync_polarity = DCI_HSYNC_POLARITY_LOW;
    cfg.vsync_polarity = DCI_VSYNC_POLARITY_LOW;
    cfg.frame_rate = DCI_FRAME_RATE_ALL;
    cfg.interface_format = DCI_INTERFACE_FORMAT_8BITS;
    dci_deinit();
    dci_init(&cfg);
}

static void __dvp_dma_config(uint32_t memory_addr)
{
    dma_multi_data_parameter_struct cfg;

    rcu_periph_clock_enable(RCU_DMA1);
    rcu_periph_clock_enable(RCU_DMAMUX);

    dma_channel_deinit(DMA1, DMA_CH7);
    dma_multi_data_para_struct_init(&cfg);
    cfg.request = DMA_REQUEST_DCI;
    cfg.periph_addr = (uint32_t)&DCI_DATA;
    cfg.periph_width = DMA_PERIPH_WIDTH_32BIT;
    cfg.periph_inc = DMA_PERIPH_INCREASE_DISABLE;
    cfg.memory0_addr = memory_addr;
    cfg.memory_width = DMA_MEMORY_WIDTH_32BIT;
    cfg.memory_inc = DMA_MEMORY_INCREASE_ENABLE;
    cfg.memory_burst_width = DMA_MEMORY_BURST_4_BEAT;
    cfg.periph_burst_width = DMA_PERIPH_BURST_SINGLE;
    cfg.critical_value = DMA_FIFO_4_WORD;
    cfg.circular_mode = DMA_CIRCULAR_MODE_DISABLE;
    cfg.direction = DMA_PERIPH_TO_MEMORY;
    cfg.number = GD32H77X_DVP_WORDS;
    cfg.priority = DMA_PRIORITY_ULTRA_HIGH;
    dma_multi_data_mode_init(DMA1, DMA_CH7, &cfg);
}

static void __dvp_stop_hw(void)
{
    dci_capture_disable();
    dci_disable();
    dma_channel_disable(DMA1, DMA_CH7);
    dma_flag_clear(DMA1, DMA_CH7, DMA_FLAG_FTF);
    dma_flag_clear(DMA1, DMA_CH7, DMA_FLAG_SDE);
    dma_flag_clear(DMA1, DMA_CH7, DMA_FLAG_TAE);
    dci_interrupt_flag_clear(0x1FFU);
}

static bool __dvp_capture_frame(void)
{
    uint32_t start_ms;
    uint32_t drain_start_ms = 0U;
    bool complete = false;
    bool draining = false;

    SCB_CleanInvalidateDCache_by_Addr((uint32_t *)s_dma_frame, GD32H77X_DVP_BYTES);
    __DSB();
    __dvp_dci_config();
    dma_transfer_number_config(DMA1, DMA_CH7, GD32H77X_DVP_WORDS);
    dma_flag_clear(DMA1, DMA_CH7, DMA_FLAG_FTF);
    dci_interrupt_flag_clear(0x1FFU);

    dma_channel_enable(DMA1, DMA_CH7);
    dci_enable();
    dci_capture_enable();

    start_ms = (uint32_t)tal_system_get_millisecond();
    while (s_dvp.started && !s_dvp.exit_requested) {
        bool dma_full = dma_flag_get(DMA1, DMA_CH7, DMA_FLAG_FTF) == SET;
        bool frame_end = dci_flag_get(DCI_FLAG_EF) == SET;

        if (dci_flag_get(DCI_FLAG_OVR) == SET ||
            dci_flag_get(DCI_FLAG_ESE) == SET ||
            dma_flag_get(DMA1, DMA_CH7,
                         DMA_FLAG_FEE | DMA_FLAG_SDE | DMA_FLAG_TAE) != RESET) {
            break;
        }

        if (dma_full && frame_end) {
            complete = true;
            break;
        }

        /* A full DMA buffer alone does not prove a complete DCI frame. Give the
         * receiver/FIFO 2 ms to produce the matching frame-end event, otherwise
         * reject the snapshot instead of handing a blank/partial frame upward. */
        if ((dma_full || frame_end) && !draining) {
            draining = true;
            drain_start_ms = (uint32_t)tal_system_get_millisecond();
        }
        if (draining &&
            (uint32_t)tal_system_get_millisecond() - drain_start_ms >= 2U) {
            break;
        }
        if ((uint32_t)tal_system_get_millisecond() - start_ms >=
            GD32H77X_DVP_TIMEOUT_MS) {
            break;
        }
        tal_system_sleep(1U);
    }

    uint32_t remaining = dma_transfer_number_get(DMA1, DMA_CH7);
    uint32_t stat0 = DCI_STAT0;
    uint32_t stat1 = DCI_STAT1;
    bool dma_full = dma_flag_get(DMA1, DMA_CH7, DMA_FLAG_FTF) == SET;
    bool frame_end = dci_flag_get(DCI_FLAG_EF) == SET;
    s_dvp.last_capture_ms = (uint32_t)tal_system_get_millisecond() - start_ms;
    __dvp_stop_hw();
    if (!complete && s_dvp.capture_attempts < 4U) {
        PR_WARN("DVP capture rejected attempt=%u dma_full=%u frame_end=%u remaining=%u stat0=%08X stat1=%08X",
                (unsigned)++s_dvp.capture_attempts,
                (unsigned)dma_full, (unsigned)frame_end,
                (unsigned)remaining, (unsigned)stat0, (unsigned)stat1);
    }
    return complete;
}

static void __dvp_capture_task(void *arg)
{
    (void)arg;

    while (!s_dvp.exit_requested) {
        if (!s_dvp.started) {
            tal_system_sleep(2U);
            continue;
        }

        s_dvp.busy = true;
        if (s_dvp.frame == NULL && s_dvp.cfg.inter_cfg.assign_cb != NULL) {
            s_dvp.frame = s_dvp.cfg.inter_cfg.assign_cb(TUYA_FRAME_FMT_YUV422,
                                                         s_dvp.cfg.inter_cfg.cb_param);
        }

        if (s_dvp.frame == NULL || s_dvp.frame->data == NULL ||
            s_dvp.frame->data_len < GD32H77X_DVP_BYTES) {
            s_dvp.busy = false;
            tal_system_sleep(5U);
            continue;
        }

        if (__dvp_capture_frame()) {
            s_dvp.capture_attempts = 0U;
            SCB_InvalidateDCache_by_Addr((uint32_t *)s_dma_frame, GD32H77X_DVP_BYTES);
            memcpy(s_dvp.frame->data, s_dma_frame, GD32H77X_DVP_BYTES);
            s_dvp.frame->frame_id = ++s_dvp.frame_id;
            s_dvp.frame->is_frame_complete = 1U;
            s_dvp.frame->is_i_frame = 0U;
            s_dvp.frame->frame_fmt = TUYA_FRAME_FMT_YUV422;
            s_dvp.frame->width = GD32H77X_DVP_WIDTH;
            s_dvp.frame->height = GD32H77X_DVP_HEIGHT;
            s_dvp.frame->data_len = GD32H77X_DVP_BYTES;
            s_dvp.frame->total_frame_len = GD32H77X_DVP_BYTES;
            if (s_dvp.frame_id <= 5U || (s_dvp.frame_id % 30U) == 0U) {
                PR_NOTICE("DVP frame %u capture=%ums",
                          (unsigned)s_dvp.frame_id,
                          (unsigned)s_dvp.last_capture_ms);
            }
            if (s_dvp.cfg.inter_cfg.post_cb != NULL) {
                s_dvp.cfg.inter_cfg.post_cb(s_dvp.frame, s_dvp.cfg.inter_cfg.cb_param);
            }
            s_dvp.frame = NULL;
        } else {
            tal_system_sleep(10U);
        }
        s_dvp.busy = false;
    }
}

OPERATE_RET tkl_dvp_init(TUYA_DVP_CFG_T *dvp_cfg)
{
    THREAD_CFG_T task_cfg = {
        .stackDepth = 8192U,
        .priority = THREAD_PRIO_1,
        .thrdname = "gd32_dvp",
    };

    if (dvp_cfg == NULL) {
        return OPRT_INVALID_PARM;
    }
    if (dvp_cfg->width != GD32H77X_DVP_WIDTH ||
        dvp_cfg->height != GD32H77X_DVP_HEIGHT ||
        dvp_cfg->output_mode != TUYA_CAMERA_OUTPUT_YUV422 ||
        dvp_cfg->inter_cfg.assign_cb == NULL ||
        dvp_cfg->inter_cfg.post_cb == NULL) {
        return OPRT_NOT_SUPPORTED;
    }
    if (s_dvp.task != NULL) {
        return OPRT_OK;
    }

    memset(&s_dvp, 0, sizeof(s_dvp));
    memcpy(&s_dvp.cfg, dvp_cfg, sizeof(s_dvp.cfg));
    __dvp_dci_gpio_config();
    __dvp_dci_config();
    __dvp_dma_config((uint32_t)s_dma_frame);
    __dvp_stop_hw();

    if (s_dvp.cfg.inter_cfg.setup_cb != NULL) {
        OPERATE_RET rt = s_dvp.cfg.inter_cfg.setup_cb(s_dvp.cfg.inter_cfg.cb_param);
        if (rt != OPRT_OK) {
            return rt;
        }
    }

    return tal_thread_create_and_start(&s_dvp.task, NULL, NULL,
                                       __dvp_capture_task, NULL, &task_cfg);
}

OPERATE_RET tkl_dvp_deinit(void)
{
    if (s_dvp.task == NULL) {
        return OPRT_OK;
    }

    s_dvp.started = false;
    s_dvp.exit_requested = true;
    while (s_dvp.busy) {
        tal_system_sleep(1U);
    }
    __dvp_stop_hw();
    tal_thread_delete(s_dvp.task);
    s_dvp.task = NULL;
    s_dvp.frame = NULL;
    return OPRT_OK;
}

OPERATE_RET tkl_dvp_start(void)
{
    if (s_dvp.task == NULL) {
        return OPRT_COM_ERROR;
    }
    s_dvp.started = true;
    return OPRT_OK;
}

OPERATE_RET tkl_dvp_stop(void)
{
    uint32_t start_ms;

    if (s_dvp.task == NULL) {
        return OPRT_COM_ERROR;
    }

    s_dvp.started = false;
    start_ms = (uint32_t)tal_system_get_millisecond();
    while (s_dvp.busy) {
        if ((uint32_t)tal_system_get_millisecond() - start_ms >= 100U) {
            return OPRT_TIMEOUT;
        }
        tal_system_sleep(1U);
    }
    __dvp_stop_hw();
    return OPRT_OK;
}
