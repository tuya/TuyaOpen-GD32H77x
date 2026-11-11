/**
 * @file tkl_spi.c
 * @brief GD32H77X SPI TKL backend with polling, IRQ and DMA paths.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "tkl_spi.h"

#ifdef BIT
#undef BIT
#endif

#include "gd32h77x_78x.h"
#include "gd32h77x_78x_spi.h"
#include "tkl_pinmux.h"
#include "tkl_system.h"
#include "tuya_error_code.h"

#ifndef GD32H77X_SPI_HOST_TEST
#include "gd32h77x_dma.h"
#endif

#define GD32H77X_SPI_TIMEOUT_MS       50U
#define GD32H77X_SPI_DMA_MAX_FRAMES   0xFFFFU
#define GD32H77X_SPI_DMA_TIMEOUT_MS   1000U

typedef struct {
    uint32_t port;
    uint32_t pin;
    uint32_t af;
} SPI_PIN_T;

typedef struct {
    SPI_PIN_T cs;
    SPI_PIN_T clk;
    SPI_PIN_T miso;
    SPI_PIN_T mosi;
} SPI_PIN_CFG_T;

#define SPI_PIN(port_name, pin_num, af_val) \
    {port_name, GPIO_PIN_##pin_num, af_val}

typedef struct {
    uint32_t periph;
    uint32_t rcu;
    TUYA_SPI_BASE_CFG_T cfg;
    uint32_t base_freq;
    uint8_t initialized;
    uint8_t frame_bytes;
    int32_t data_count;
    TUYA_SPI_IRQ_CB irq_cb;
    const uint8_t *irq_tx;
    uint8_t *irq_rx;
    uint32_t irq_frames;
    uint32_t irq_tx_count;
    uint32_t irq_rx_count;
    TUYA_SPI_IRQ_EVT_E irq_complete_event;
    uint8_t irq_enabled;
    uint8_t transfer_active;
    uint8_t data_lost;
    uint8_t mode_fault;
#ifndef GD32H77X_SPI_HOST_TEST
    uint8_t dma_async;
    uint8_t *dma_rx_buf;
    uint32_t dma_tx_size;
    uint32_t dma_rx_size;
    gd32h77x_dma_handle_t dma_tx;
    gd32h77x_dma_handle_t dma_rx;
#endif
} GD32H77X_SPI_STATE;

static const IRQn_Type s_spi_irqn[TUYA_SPI_NUM_MAX] = {
    SPI0_IRQn, SPI1_IRQn, SPI2_IRQn, SPI3_IRQn, SPI4_IRQn, SPI5_IRQn,
};
static const uint32_t s_spi_periph[TUYA_SPI_NUM_MAX] = {
    SPI0, SPI1, SPI2, SPI3, SPI4, SPI5,
};
static const uint32_t s_spi_rcu[TUYA_SPI_NUM_MAX] = {
    RCU_SPI0, RCU_SPI1, RCU_SPI2, RCU_SPI3, RCU_SPI4, RCU_SPI5,
};
static GD32H77X_SPI_STATE s_spi[TUYA_SPI_NUM_MAX];

static SPI_PIN_CFG_T s_spi_pins[TUYA_SPI_NUM_MAX] = {
    [TUYA_SPI_NUM_0] = {
        .cs = SPI_PIN(GPIOA, 4, 5), .clk = SPI_PIN(GPIOA, 5, 5),
        .miso = SPI_PIN(GPIOA, 6, 5), .mosi = SPI_PIN(GPIOA, 7, 5),
    },
    [TUYA_SPI_NUM_1] = {
        .cs = SPI_PIN(GPIOB, 12, 5), .clk = SPI_PIN(GPIOB, 13, 5),
        .miso = SPI_PIN(GPIOB, 14, 5), .mosi = SPI_PIN(GPIOB, 15, 5),
    },
    [TUYA_SPI_NUM_2] = {
        .cs = SPI_PIN(GPIOA, 15, 5), .clk = SPI_PIN(GPIOC, 10, 5),
        .miso = SPI_PIN(GPIOC, 11, 5), .mosi = SPI_PIN(GPIOC, 12, 5),
    },
    [TUYA_SPI_NUM_3] = {
        .cs = SPI_PIN(GPIOE, 11, 5), .clk = SPI_PIN(GPIOE, 12, 5),
        .miso = SPI_PIN(GPIOE, 13, 5), .mosi = SPI_PIN(GPIOE, 14, 5),
    },
    [TUYA_SPI_NUM_4] = {
        .cs = SPI_PIN(GPIOF, 6, 5), .clk = SPI_PIN(GPIOF, 7, 5),
        .miso = SPI_PIN(GPIOF, 8, 5), .mosi = SPI_PIN(GPIOF, 9, 5),
    },
    [TUYA_SPI_NUM_5] = {
        .cs = SPI_PIN(GPIOA, 4, 8), .clk = SPI_PIN(GPIOA, 5, 8),
        .miso = SPI_PIN(GPIOA, 6, 8), .mosi = SPI_PIN(GPIOA, 7, 8),
    },
};

#ifndef GD32H77X_SPI_HOST_TEST
static const uint32_t s_spi_tx_request[TUYA_SPI_NUM_MAX] = {
    DMA_REQUEST_SPI0_TX, DMA_REQUEST_SPI1_TX, DMA_REQUEST_SPI2_TX,
    DMA_REQUEST_SPI3_TX, DMA_REQUEST_SPI4_TX, DMA_REQUEST_SPI5_TX,
};
static const uint32_t s_spi_rx_request[TUYA_SPI_NUM_MAX] = {
    DMA_REQUEST_SPI0_RX, DMA_REQUEST_SPI1_RX, DMA_REQUEST_SPI2_RX,
    DMA_REQUEST_SPI3_RX, DMA_REQUEST_SPI4_RX, DMA_REQUEST_SPI5_RX,
};
static uint32_t s_spi_dma_dummy __attribute__((aligned(32))) = 0xFFFFFFFFU;
#endif

void __tkl_spi_set_pin(TUYA_SPI_NUM_E port, TUYA_PIN_FUNC_E func,
                       uint32_t gpio_port, uint32_t gpio_pin, uint32_t af)
{
    uint32_t controller;
    uint32_t channel;

    if (func >= TUYA_SPI0_MISO && func <= TUYA_SPI2_CS) {
        controller = (func - TUYA_SPI0_MISO) / 4U;
        channel = (func - TUYA_SPI0_MISO) % 4U;
    } else if (func >= TUYA_SPI3_MISO && func <= TUYA_SPI5_CS) {
        controller = (func - TUYA_SPI3_MISO) / 4U + 3U;
        channel = (func - TUYA_SPI3_MISO) % 4U;
    } else {
        return;
    }
    if (controller != (uint32_t)port || port >= TUYA_SPI_NUM_MAX) {
        return;
    }

    SPI_PIN_T *pin_cfg = NULL;
    switch (channel) {
        case 0U: pin_cfg = &s_spi_pins[port].miso; break;
        case 1U: pin_cfg = &s_spi_pins[port].mosi; break;
        case 2U: pin_cfg = &s_spi_pins[port].clk; break;
        case 3U: pin_cfg = &s_spi_pins[port].cs; break;
        default: return;
    }
    pin_cfg->port = gpio_port;
    pin_cfg->pin = gpio_pin;
    pin_cfg->af = af;
}

static uint32_t __spi_gpio_rcu(uint32_t port)
{
    static const uint32_t ports[] = {
        GPIOA, GPIOB, GPIOC, GPIOD, GPIOE, GPIOF, GPIOG, GPIOH, GPIOJ, GPIOK,
    };
    static const uint32_t rcus[] = {
        RCU_GPIOA, RCU_GPIOB, RCU_GPIOC, RCU_GPIOD, RCU_GPIOE,
        RCU_GPIOF, RCU_GPIOG, RCU_GPIOH, RCU_GPIOJ, RCU_GPIOK,
    };
    for (uint32_t i = 0U; i < sizeof(ports) / sizeof(ports[0]); ++i) {
        if (ports[i] == port) return rcus[i];
    }
    return 0U;
}

static void __spi_apply_one_pin(const SPI_PIN_T *pin)
{
    uint32_t rcu = __spi_gpio_rcu(pin->port);
    if (rcu == 0U) return;
    rcu_periph_clock_enable(rcu);
    gpio_af_set(pin->port, pin->af, pin->pin);
    gpio_mode_set(pin->port, GPIO_MODE_AF, GPIO_PUPD_NONE, pin->pin);
    gpio_output_options_set(pin->port, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ,
                            pin->pin);
}

static void __spi_init_gpio(TUYA_SPI_NUM_E port)
{
    __spi_apply_one_pin(&s_spi_pins[port].cs);
    __spi_apply_one_pin(&s_spi_pins[port].clk);
    __spi_apply_one_pin(&s_spi_pins[port].miso);
    __spi_apply_one_pin(&s_spi_pins[port].mosi);
}

static OPERATE_RET __spi_wait_flag(uint32_t periph, uint32_t flag)
{
    uint32_t start_ms = (uint32_t)tkl_system_get_millisecond();
    while ((SPI_STAT(periph) & flag) == RESET) {
        if ((SPI_STAT(periph) & (SPI_FLAG_CONFERR | SPI_FLAG_RXORERR |
                                 SPI_FLAG_TXURERR)) != RESET) {
            return OPRT_COM_ERROR;
        }
        if ((uint32_t)tkl_system_get_millisecond() - start_ms >=
            GD32H77X_SPI_TIMEOUT_MS) {
            return OPRT_TIMEOUT;
        }
    }
    return OPRT_OK;
}

static OPERATE_RET __spi_master_suspend(uint32_t periph)
{
    uint32_t start_ms;
    if ((SPI_CTL0(periph) & SPI_CTL0_MSTART) == RESET) {
        SPI_STATC(periph) = SPI_STATC_SPDC;
        return OPRT_OK;
    }
    SPI_CTL0(periph) |= SPI_CTL0_MSPDR;
    start_ms = (uint32_t)tkl_system_get_millisecond();
    while ((SPI_CTL0(periph) & SPI_CTL0_MSTART) != RESET) {
        if ((uint32_t)tkl_system_get_millisecond() - start_ms >=
            GD32H77X_SPI_TIMEOUT_MS) {
            return OPRT_TIMEOUT;
        }
    }
    SPI_STATC(periph) = SPI_STATC_SPDC;
    return OPRT_OK;
}

static uint32_t __spi_prescaler(uint32_t base_freq, uint32_t target_freq)
{
    static const uint32_t dividers[] = {2U, 4U, 8U, 16U, 32U, 64U, 128U, 256U};
    static const uint32_t prescalers[] = {
        SPI_PSC_2, SPI_PSC_4, SPI_PSC_8, SPI_PSC_16,
        SPI_PSC_32, SPI_PSC_64, SPI_PSC_128, SPI_PSC_256,
    };
    if (target_freq == 0U) return SPI_PSC_256;
    for (uint32_t i = 0U; i < sizeof(dividers) / sizeof(dividers[0]); ++i) {
        if ((base_freq / dividers[i]) <= target_freq) return prescalers[i];
    }
    return SPI_PSC_256;
}

static uint32_t __spi_mode(TUYA_SPI_MODE_E mode)
{
    switch (mode) {
        case TUYA_SPI_MODE1: return SPI_CK_PL_LOW_PH_2EDGE;
        case TUYA_SPI_MODE2: return SPI_CK_PL_HIGH_PH_1EDGE;
        case TUYA_SPI_MODE3: return SPI_CK_PL_HIGH_PH_2EDGE;
        case TUYA_SPI_MODE0:
        default: return SPI_CK_PL_LOW_PH_1EDGE;
    }
}

OPERATE_RET tkl_spi_init(TUYA_SPI_NUM_E port, const TUYA_SPI_BASE_CFG_T *cfg)
{
    spi_parameter_struct spi_cfg;
    uint32_t periph;
    uint32_t spi_clock_source;
    uint32_t spi_clock_freq;
    OPERATE_RET rt;

    if (port >= TUYA_SPI_NUM_MAX || cfg == NULL ||
        (cfg->role != TUYA_SPI_ROLE_MASTER &&
         cfg->role != TUYA_SPI_ROLE_SLAVE) ||
        (cfg->databits != TUYA_SPI_DATA_BIT8 &&
         cfg->databits != TUYA_SPI_DATA_BIT16) ||
        cfg->freq_hz == 0U) {
        return OPRT_INVALID_PARM;
    }

    __spi_init_gpio(port);
    periph = s_spi_periph[port];
    if (s_spi[port].initialized &&
        s_spi[port].cfg.role == TUYA_SPI_ROLE_MASTER) {
        rt = __spi_master_suspend(periph);
        if (rt != OPRT_OK) return rt;
    }
    spi_disable(periph);

    if (port <= TUYA_SPI_NUM_2) {
        spi_clock_source = RCU_SPISRC_PLL0Q;
        spi_clock_freq = rcu_clock_freq_get(CK_PLL0Q);
    } else {
        spi_clock_source = RCU_SPISRC_APB2;
        spi_clock_freq = rcu_clock_freq_get(CK_APB2);
    }
    rcu_spi_clock_config((spi_idx_enum)port, spi_clock_source);
    rcu_periph_clock_enable(s_spi_rcu[port]);

    spi_struct_para_init(&spi_cfg);
    spi_cfg.trans_mode = SPI_TRANSMODE_FULLDUPLEX;
    spi_cfg.device_mode = cfg->role == TUYA_SPI_ROLE_MASTER ?
                          SPI_MASTER : SPI_SLAVE;
    spi_cfg.data_size = cfg->databits == TUYA_SPI_DATA_BIT16 ?
                        SPI_DATASIZE_16BIT : SPI_DATASIZE_8BIT;
    spi_cfg.clock_polarity_phase = __spi_mode(cfg->mode);
    spi_cfg.nss = cfg->type == TUYA_SPI_AUTO_TYPE ? SPI_NSS_HARD : SPI_NSS_SOFT;
    spi_cfg.prescale = __spi_prescaler(spi_clock_freq, cfg->freq_hz);
    spi_cfg.endian = cfg->bitorder == TUYA_SPI_ORDER_LSB2MSB ?
                     SPI_ENDIAN_LSB : SPI_ENDIAN_MSB;
    spi_init(periph, &spi_cfg);
    spi_byte_access_enable(periph);
    if (cfg->role == TUYA_SPI_ROLE_MASTER &&
        cfg->type == TUYA_SPI_AUTO_TYPE) {
        spi_nss_output_control(periph, SPI_NSS_HOLD_UNTIL_TRANS_END);
        spi_nss_polarity_set(periph, SPI_NSS_POLARITY_LOW);
        spi_nss_output_enable(periph);
    } else {
        spi_nss_output_disable(periph);
        if (cfg->type == TUYA_SPI_SOFT_TYPE) spi_nss_internal_high(periph);
    }
    spi_enable(periph);

    memset(&s_spi[port], 0, sizeof(s_spi[port]));
    s_spi[port].periph = periph;
    s_spi[port].rcu = s_spi_rcu[port];
    s_spi[port].cfg = *cfg;
    s_spi[port].base_freq = spi_clock_freq;
    s_spi[port].frame_bytes = cfg->databits == TUYA_SPI_DATA_BIT16 ? 2U : 1U;
    s_spi[port].initialized = 1U;
    return OPRT_OK;
}

OPERATE_RET tkl_spi_deinit(TUYA_SPI_NUM_E port)
{
    if (port >= TUYA_SPI_NUM_MAX || !s_spi[port].initialized) {
        return OPRT_INVALID_PARM;
    }
    if (s_spi[port].irq_cb != NULL || s_spi[port].irq_enabled) {
        (void)tkl_spi_irq_disable(port);
    }
#ifndef GD32H77X_SPI_HOST_TEST
    if (s_spi[port].dma_tx.claimed) gd32h77x_dma_release(&s_spi[port].dma_tx);
    if (s_spi[port].dma_rx.claimed) gd32h77x_dma_release(&s_spi[port].dma_rx);
#endif
    if (s_spi[port].cfg.role == TUYA_SPI_ROLE_MASTER) {
        OPERATE_RET rt = __spi_master_suspend(s_spi[port].periph);
        if (rt != OPRT_OK) return rt;
    }
    spi_disable(s_spi[port].periph);
    memset(&s_spi[port], 0, sizeof(s_spi[port]));
    return OPRT_OK;
}

static OPERATE_RET __spi_transfer_once(TUYA_SPI_NUM_E port,
                                       const void *send_buf,
                                       void *receive_buf, uint32_t frames)
{
    uint32_t periph = s_spi[port].periph;
    uint32_t frame_bytes = s_spi[port].frame_bytes;
    OPERATE_RET rt;

    if (s_spi[port].cfg.role == TUYA_SPI_ROLE_MASTER) {
        rt = __spi_master_suspend(periph);
        if (rt != OPRT_OK) return rt;
        spi_disable(periph);
    }
    SPI_STATC(periph) = SPI_STATC_ETC | SPI_STATC_SPDC |
        SPI_STATC_TXURERRC | SPI_STATC_RXORERRC | SPI_STATC_CONFERRC;
    spi_current_data_num_config(periph, frames);
    if (s_spi[port].cfg.role == TUYA_SPI_ROLE_MASTER) spi_enable(periph);
    spi_master_transfer_start(periph, SPI_TRANS_START);

    for (uint32_t i = 0U; i < frames; ++i) {
        rt = __spi_wait_flag(periph, SPI_FLAG_TP);
        if (rt != OPRT_OK) goto transfer_error;
        uint32_t tx = frame_bytes == 2U ? 0xFFFFU : 0xFFU;
        if (send_buf != NULL) {
            tx = frame_bytes == 2U ? ((const uint16_t *)send_buf)[i] :
                                     ((const uint8_t *)send_buf)[i];
        }
        SPI_TDATA(periph) = tx;

        rt = __spi_wait_flag(periph, SPI_FLAG_RP);
        if (rt != OPRT_OK) goto transfer_error;
        uint32_t rx = SPI_RDATA(periph);
        if (receive_buf != NULL) {
            if (frame_bytes == 2U) ((uint16_t *)receive_buf)[i] = (uint16_t)rx;
            else ((uint8_t *)receive_buf)[i] = (uint8_t)rx;
        }
    }
    if (s_spi[port].cfg.role == TUYA_SPI_ROLE_MASTER) {
        rt = __spi_wait_flag(periph, SPI_FLAG_ET);
        if (rt != OPRT_OK) goto transfer_error;
    }
    return OPRT_OK;

transfer_error:
    if (s_spi[port].cfg.role == TUYA_SPI_ROLE_MASTER) {
        (void)__spi_master_suspend(periph);
    }
    return rt;
}

#ifndef GD32H77X_SPI_HOST_TEST
static bool __spi_dcache_enabled(void)
{
    return (SCB->CCR & SCB_CCR_DC_Msk) != 0U;
}

static void __spi_dma_cache_prepare(const void *tx, uint32_t tx_size,
                                    void *rx, uint32_t rx_size)
{
    if (!__spi_dcache_enabled()) return;
    if (tx != NULL && tx_size != 0U)
        SCB_CleanDCache_by_Addr((volatile void *)tx, (int32_t)tx_size);
    if (rx != NULL && rx_size != 0U)
        SCB_CleanDCache_by_Addr((volatile void *)rx, (int32_t)rx_size);
}

static void __spi_dma_cache_invalidate(void *rx, uint32_t rx_size)
{
    if (__spi_dcache_enabled() && rx != NULL && rx_size != 0U)
        SCB_InvalidateDCache_by_Addr((volatile void *)rx, (int32_t)rx_size);
}

static void __spi_dma_release(TUYA_SPI_NUM_E port)
{
    GD32H77X_SPI_STATE *state = &s_spi[port];
    spi_dma_disable(state->periph, SPI_DMA_TRANSMIT);
    spi_dma_disable(state->periph, SPI_DMA_RECEIVE);
    spi_word_access_disable(state->periph);
    spi_byte_access_enable(state->periph);
    spi_fifo_threshold_level_set(state->periph, SPI_FIFO_TH_01DATA);
    if (state->dma_tx.claimed) gd32h77x_dma_release(&state->dma_tx);
    if (state->dma_rx.claimed) gd32h77x_dma_release(&state->dma_rx);
}

static void __spi_dma_deliver_event(TUYA_SPI_NUM_E port,
                                     TUYA_SPI_IRQ_EVT_E event)
{
    GD32H77X_SPI_STATE *state = &s_spi[port];
    TUYA_SPI_IRQ_CB cb;

    spi_i2s_interrupt_disable(state->periph, SPI_I2S_INT_TP | SPI_I2S_INT_RP |
        SPI_I2S_INT_ESTC | SPI_I2S_INT_TXURE | SPI_I2S_INT_RXORE |
        SPI_I2S_INT_CONFE);
    __spi_dma_release(port);
    SPI_STATC(state->periph) = SPI_STATC_ETC | SPI_STATC_TXURERRC |
        SPI_STATC_RXORERRC | SPI_STATC_CONFERRC;

    if (event == TUYA_SPI_EVENT_DATA_LOST ||
        event == TUYA_SPI_EVENT_MODE_FAULT) {
        SPI_CTL0(state->periph) |= SPI_CTL0_MSPDR;
        state->data_count = 0;
    } else {
        __spi_dma_cache_invalidate(state->dma_rx_buf, state->dma_rx_size);
        state->data_count = (int32_t)state->dma_tx_size;
    }

    state->transfer_active = 0U;
    state->dma_async = 0U;
    state->irq_tx = NULL;
    state->irq_rx = NULL;
    cb = state->irq_cb;
    if (cb != NULL && state->irq_enabled) cb(port, event);
}

static void __spi_dma_arm_spi_et(TUYA_SPI_NUM_E port)
{
    GD32H77X_SPI_STATE *state = &s_spi[port];

    state->dma_async = 2U;
    spi_i2s_interrupt_enable(state->periph, SPI_I2S_INT_ESTC |
        SPI_I2S_INT_TXURE | SPI_I2S_INT_RXORE | SPI_I2S_INT_CONFE);
    NVIC_ClearPendingIRQ(s_spi_irqn[port]);
    NVIC_EnableIRQ(s_spi_irqn[port]);
}

static OPERATE_RET __spi_dma_claim_pair(TUYA_SPI_NUM_E port)
{
    GD32H77X_SPI_STATE *state = &s_spi[port];
    OPERATE_RET rt;

    __spi_dma_release(port);
    rt = gd32h77x_dma_claim(&state->dma_tx, s_spi_tx_request[port], NULL, NULL);
    if (rt != OPRT_OK) return OPRT_COM_ERROR;
    rt = gd32h77x_dma_claim(&state->dma_rx, s_spi_rx_request[port], NULL, NULL);
    if (rt != OPRT_OK) {
        gd32h77x_dma_release(&state->dma_tx);
        return OPRT_COM_ERROR;
    }
    return OPRT_OK;
}

static OPERATE_RET __spi_dma_config_pair(TUYA_SPI_NUM_E port,
                                         const void *tx, void *rx,
                                         uint32_t frames)
{
    GD32H77X_SPI_STATE *state = &s_spi[port];
    GD32H77X_DMA_SINGLE_CFG_T cfg = {
        .width = DMA_PERIPH_WIDTH_8BIT,
        .frames = frames,
        .priority = DMA_PRIORITY_HIGH,
    };
    int dma_rt;

    cfg.periph_addr = (uint32_t)&SPI_TDATA(state->periph);
    cfg.memory_addr = tx != NULL ? (uint32_t)(uintptr_t)tx :
                                   (uint32_t)(uintptr_t)&s_spi_dma_dummy;
    cfg.memory_increment = tx != NULL;
    cfg.direction = DMA_MEMORY_TO_PERIPH;
    dma_rt = gd32h77x_dma_single_config(&state->dma_tx, &cfg);
    if (dma_rt != GD32H77X_DMA_OK) return OPRT_COM_ERROR;

    cfg.periph_addr = (uint32_t)&SPI_RDATA(state->periph);
    cfg.memory_addr = rx != NULL ? (uint32_t)(uintptr_t)rx :
                                   (uint32_t)(uintptr_t)&s_spi_dma_dummy;
    cfg.memory_increment = rx != NULL;
    cfg.direction = DMA_PERIPH_TO_MEMORY;
    dma_rt = gd32h77x_dma_single_config(&state->dma_rx, &cfg);
    return dma_rt == GD32H77X_DMA_OK ? OPRT_OK : OPRT_COM_ERROR;
}

static OPERATE_RET __spi_dma_begin(TUYA_SPI_NUM_E port,
                                   const void *send_buf, void *receive_buf,
                                   uint32_t size)
{
    uint32_t periph = s_spi[port].periph;
    OPERATE_RET rt;

    if (s_spi[port].cfg.role != TUYA_SPI_ROLE_MASTER ||
        s_spi[port].frame_bytes != 1U || size == 0U ||
        size > GD32H77X_SPI_DMA_MAX_FRAMES)
        return OPRT_NOT_SUPPORTED;

    rt = __spi_dma_claim_pair(port);
    if (rt != OPRT_OK) return rt;
    rt = __spi_master_suspend(periph);
    if (rt != OPRT_OK) { __spi_dma_release(port); return rt; }
    spi_disable(periph);
    SPI_STATC(periph) = SPI_STATC_ETC | SPI_STATC_SPDC |
        SPI_STATC_TXURERRC | SPI_STATC_RXORERRC | SPI_STATC_CONFERRC;
    spi_word_access_disable(periph);
    spi_byte_access_enable(periph);
    spi_fifo_threshold_level_set(periph, SPI_FIFO_TH_01DATA);
    spi_current_data_num_config(periph, size);
    __spi_dma_cache_prepare(send_buf, size, receive_buf, size);
    rt = __spi_dma_config_pair(port, send_buf, receive_buf, size);
    if (rt == OPRT_OK) {
        spi_dma_enable(periph, SPI_DMA_TRANSMIT);
        spi_dma_enable(periph, SPI_DMA_RECEIVE);
    } else {
        __spi_dma_release(port);
    }
    return rt;
}

static OPERATE_RET __spi_dma_transfer_blocking(TUYA_SPI_NUM_E port,
                                               const void *send_buf,
                                               void *receive_buf, uint32_t size)
{
    uint32_t offset = 0U;
    OPERATE_RET rt = OPRT_OK;

    while (offset < size) {
        uint32_t bytes = size - offset;
        if (bytes > GD32H77X_SPI_DMA_MAX_FRAMES)
            bytes = GD32H77X_SPI_DMA_MAX_FRAMES;
        const uint8_t *tx8 = send_buf == NULL ? NULL :
                             (const uint8_t *)send_buf + offset;
        uint8_t *rx8 = receive_buf == NULL ? NULL :
                       (uint8_t *)receive_buf + offset;

        rt = __spi_dma_begin(port, tx8, rx8, bytes);
        if (rt != OPRT_OK) break;
        spi_enable(s_spi[port].periph);
        (void)gd32h77x_dma_start(&s_spi[port].dma_rx, false);
        (void)gd32h77x_dma_start(&s_spi[port].dma_tx, false);
        spi_master_transfer_start(s_spi[port].periph, SPI_TRANS_START);

        int dma_rt = GD32H77X_DMA_OK;
        if (tx8 != NULL)
            dma_rt = gd32h77x_dma_wait(&s_spi[port].dma_tx,
                                        GD32H77X_SPI_DMA_TIMEOUT_MS);
        if (dma_rt == GD32H77X_DMA_OK && rx8 != NULL)
            dma_rt = gd32h77x_dma_wait(&s_spi[port].dma_rx,
                                        GD32H77X_SPI_DMA_TIMEOUT_MS);
        if (dma_rt == GD32H77X_DMA_OK)
            rt = __spi_wait_flag(s_spi[port].periph, SPI_FLAG_ET);
        else
            rt = dma_rt == GD32H77X_DMA_TIMEOUT ? OPRT_TIMEOUT : OPRT_COM_ERROR;
        if (rt != OPRT_OK) (void)__spi_master_suspend(s_spi[port].periph);
        __spi_dma_release(port);
        if (rt != OPRT_OK) break;
        offset += bytes;
    }

    if (rt == OPRT_OK) {
        __spi_dma_cache_invalidate(receive_buf, size);
        s_spi[port].data_count = (int32_t)size;
    } else {
        s_spi[port].data_count = (int32_t)offset;
    }
    return rt;
}

static OPERATE_RET __spi_dma_start_async(TUYA_SPI_NUM_E port,
                                         const void *send_buf,
                                         void *receive_buf, uint32_t size)
{
    GD32H77X_SPI_STATE *state = &s_spi[port];
    uint32_t mask;
    OPERATE_RET rt;

    if (state->frame_bytes != 1U || size == 0U ||
        size > GD32H77X_SPI_DMA_MAX_FRAMES)
        return OPRT_NOT_SUPPORTED;
    mask = tkl_system_enter_critical();
    if (state->transfer_active) {
        tkl_system_exit_critical(mask);
        return OPRT_COM_ERROR;
    }
    state->transfer_active = 1U;
    state->data_lost = 0U;
    state->mode_fault = 0U;
    state->dma_async = 1U;
    state->dma_rx_buf = receive_buf;
    state->dma_tx_size = size;
    state->dma_rx_size = size;
    state->irq_complete_event = send_buf != NULL && receive_buf != NULL ?
        TUYA_SPI_EVENT_TRANSFER_COMPLETE :
        (send_buf != NULL ? TUYA_SPI_EVENT_TX_COMPLETE :
                            TUYA_SPI_EVENT_RX_COMPLETE);
    tkl_system_exit_critical(mask);

    rt = __spi_dma_begin(port, send_buf, receive_buf, size);
    if (rt != OPRT_OK) {
        mask = tkl_system_enter_critical();
        state->transfer_active = 0U;
        state->dma_async = 0U;
        tkl_system_exit_critical(mask);
        return rt;
    }
    spi_enable(state->periph);
    (void)gd32h77x_dma_start(&state->dma_rx, false);
    (void)gd32h77x_dma_start(&state->dma_tx, false);
    __spi_dma_arm_spi_et(port);
    spi_master_transfer_start(state->periph, SPI_TRANS_START);
    return OPRT_OK;
}
#endif

static uint32_t __spi_irq_frame_read(uint32_t periph, uint32_t frame_bytes)
{
    uint32_t value = SPI_RDATA(periph);
    return frame_bytes == 2U ? value & 0xFFFFU : value & 0xFFU;
}

static void __spi_irq_store_rx(GD32H77X_SPI_STATE *state, uint32_t value)
{
    if (state->irq_rx != NULL) {
        if (state->frame_bytes == 2U)
            ((uint16_t *)state->irq_rx)[state->irq_rx_count] = (uint16_t)value;
        else
            state->irq_rx[state->irq_rx_count] = (uint8_t)value;
    }
    state->irq_rx_count++;
}

static void __spi_irq_handler(TUYA_SPI_NUM_E port)
{
    GD32H77X_SPI_STATE *state = &s_spi[port];
    uint32_t periph = state->periph;
    uint32_t stat;
    uint32_t event;
    TUYA_SPI_IRQ_CB cb;

    if (!state->irq_enabled) {
        return;
    }
#ifndef GD32H77X_SPI_HOST_TEST
    if (state->dma_async == 2U) {
        stat = SPI_STAT(periph);
        if ((stat & (SPI_FLAG_ET | SPI_FLAG_CONFERR | SPI_FLAG_RXORERR |
                     SPI_FLAG_TXURERR)) == 0U) {
            return;
        }
        if ((stat & SPI_FLAG_CONFERR) != 0U) {
            state->mode_fault = 1U;
            event = TUYA_SPI_EVENT_MODE_FAULT;
        } else if ((stat & (SPI_FLAG_RXORERR | SPI_FLAG_TXURERR)) != 0U) {
            state->data_lost = 1U;
            event = TUYA_SPI_EVENT_DATA_LOST;
        } else {
            event = state->irq_complete_event;
        }
        __spi_dma_deliver_event(port, (TUYA_SPI_IRQ_EVT_E)event);
        return;
    }
#endif
    if (!state->transfer_active ||
        (state->irq_tx == NULL && state->irq_rx == NULL)) {
        spi_i2s_interrupt_disable(periph, SPI_I2S_INT_TP | SPI_I2S_INT_RP |
            SPI_I2S_INT_ESTC | SPI_I2S_INT_TXURE | SPI_I2S_INT_RXORE |
            SPI_I2S_INT_CONFE);
        SPI_STATC(periph) = SPI_STATC_ETC | SPI_STATC_TXURERRC |
            SPI_STATC_RXORERRC | SPI_STATC_CONFERRC;
        return;
    }

    stat = SPI_STAT(periph);
    if ((stat & (SPI_FLAG_CONFERR | SPI_FLAG_RXORERR |
                 SPI_FLAG_TXURERR)) != 0U) {
        if ((stat & SPI_FLAG_CONFERR) != 0U) {
            state->mode_fault = 1U;
            event = TUYA_SPI_EVENT_MODE_FAULT;
        } else {
            state->data_lost = 1U;
            event = TUYA_SPI_EVENT_DATA_LOST;
        }
        SPI_CTL0(periph) |= SPI_CTL0_MSPDR;
        SPI_STATC(periph) = SPI_STATC_ETC | SPI_STATC_TXURERRC |
            SPI_STATC_RXORERRC | SPI_STATC_CONFERRC;
        goto complete;
    }

    while ((SPI_STAT(periph) & SPI_FLAG_RP) != 0U &&
           state->irq_rx_count < state->irq_frames) {
        __spi_irq_store_rx(state, __spi_irq_frame_read(periph,
                                                       state->frame_bytes));
    }
    while ((SPI_STAT(periph) & SPI_FLAG_TP) != 0U &&
           state->irq_tx_count < state->irq_frames) {
        uint32_t value = state->frame_bytes == 2U ? 0xFFFFU : 0xFFU;
        if (state->irq_tx != NULL) {
            value = state->frame_bytes == 2U ?
                ((const uint16_t *)state->irq_tx)[state->irq_tx_count] :
                state->irq_tx[state->irq_tx_count];
        }
        SPI_TDATA(periph) = value;
        state->irq_tx_count++;
    }
    if (state->irq_tx_count == state->irq_frames)
        spi_i2s_interrupt_disable(periph, SPI_I2S_INT_TP);
    if (state->irq_rx_count == state->irq_frames)
        spi_i2s_interrupt_disable(periph, SPI_I2S_INT_RP);

    stat = SPI_STAT(periph);
    if ((stat & SPI_FLAG_ET) != 0U) {
        while ((SPI_STAT(periph) & SPI_FLAG_RP) != 0U &&
               state->irq_rx_count < state->irq_frames) {
            __spi_irq_store_rx(state, __spi_irq_frame_read(
                periph, state->frame_bytes));
        }
        if (state->irq_tx_count == state->irq_frames &&
            state->irq_rx_count == state->irq_frames) {
            event = state->irq_complete_event;
            SPI_STATC(periph) = SPI_STATC_ETC;
            goto complete;
        }
        state->data_lost = 1U;
        state->data_count = (int32_t)(state->irq_rx_count *
                                      state->frame_bytes);
        SPI_CTL0(periph) |= SPI_CTL0_MSPDR;
        SPI_STATC(periph) = SPI_STATC_ETC | SPI_STATC_TXURERRC |
            SPI_STATC_RXORERRC | SPI_STATC_CONFERRC;
        event = TUYA_SPI_EVENT_DATA_LOST;
        goto complete;
    }
    return;

complete:
    spi_i2s_interrupt_disable(periph, SPI_I2S_INT_TP | SPI_I2S_INT_RP |
        SPI_I2S_INT_ESTC | SPI_I2S_INT_TXURE | SPI_I2S_INT_RXORE |
        SPI_I2S_INT_CONFE);
    state->transfer_active = 0U;
    state->irq_tx = NULL;
    state->irq_rx = NULL;
    cb = state->irq_cb;
    if (cb != NULL && state->irq_enabled) cb(port, (TUYA_SPI_IRQ_EVT_E)event);
}

void SPI0_IRQHandler(void) { __spi_irq_handler(TUYA_SPI_NUM_0); }
void SPI1_IRQHandler(void) { __spi_irq_handler(TUYA_SPI_NUM_1); }
void SPI2_IRQHandler(void) { __spi_irq_handler(TUYA_SPI_NUM_2); }
void SPI3_IRQHandler(void) { __spi_irq_handler(TUYA_SPI_NUM_3); }
void SPI4_IRQHandler(void) { __spi_irq_handler(TUYA_SPI_NUM_4); }
void SPI5_IRQHandler(void) { __spi_irq_handler(TUYA_SPI_NUM_5); }

static OPERATE_RET __spi_transfer(TUYA_SPI_NUM_E port, const void *send_buf,
                                  void *receive_buf, uint32_t size)
{
    uint32_t frame_bytes;
    uint32_t frames;
    uint32_t offset = 0U;

    if (port >= TUYA_SPI_NUM_MAX || !s_spi[port].initialized)
        return OPRT_INVALID_PARM;
    if ((send_buf == NULL && receive_buf == NULL) || size == 0U)
        return OPRT_INVALID_PARM;
    frame_bytes = s_spi[port].frame_bytes;
    if ((size % frame_bytes) != 0U) return OPRT_INVALID_PARM;
    frames = size / frame_bytes;

#ifndef GD32H77X_SPI_HOST_TEST
    if (s_spi[port].cfg.spi_dma_flags != 0U && frame_bytes == 1U) {
        if (s_spi[port].irq_enabled)
            return __spi_dma_start_async(port, send_buf, receive_buf, size);
        uint32_t dma_mask = tkl_system_enter_critical();
        if (s_spi[port].transfer_active) {
            tkl_system_exit_critical(dma_mask);
            return OPRT_COM_ERROR;
        }
        s_spi[port].transfer_active = 1U;
        tkl_system_exit_critical(dma_mask);
        OPERATE_RET dma_rt = __spi_dma_transfer_blocking(port, send_buf,
                                                          receive_buf, size);
        dma_mask = tkl_system_enter_critical();
        s_spi[port].transfer_active = 0U;
        tkl_system_exit_critical(dma_mask);
        return dma_rt;
    }
#endif

    if (s_spi[port].irq_enabled) {
        GD32H77X_SPI_STATE *state = &s_spi[port];
        if (state->cfg.role != TUYA_SPI_ROLE_MASTER) return OPRT_NOT_SUPPORTED;
        if (frames > 0xFFFFU) return OPRT_NOT_SUPPORTED;
        uint32_t irq_mask = tkl_system_enter_critical();
        if (state->transfer_active) {
            tkl_system_exit_critical(irq_mask);
            return OPRT_COM_ERROR;
        }
        state->transfer_active = 1U;
        state->data_lost = 0U;
        state->mode_fault = 0U;
        state->irq_tx = send_buf;
        state->irq_rx = receive_buf;
        state->irq_frames = frames;
        state->irq_tx_count = 0U;
        state->irq_rx_count = 0U;
        state->data_count = 0;
        state->irq_complete_event = send_buf != NULL && receive_buf != NULL ?
            TUYA_SPI_EVENT_TRANSFER_COMPLETE :
            (send_buf != NULL ? TUYA_SPI_EVENT_TX_COMPLETE :
                                TUYA_SPI_EVENT_RX_COMPLETE);
        uint32_t periph = state->periph;
        spi_i2s_interrupt_disable(periph, SPI_I2S_INT_TP | SPI_I2S_INT_RP |
            SPI_I2S_INT_ESTC | SPI_I2S_INT_TXURE | SPI_I2S_INT_RXORE |
            SPI_I2S_INT_CONFE);
        NVIC_DisableIRQ(s_spi_irqn[port]);
        tkl_system_exit_critical(irq_mask);
        OPERATE_RET rt = __spi_master_suspend(periph);
        if (rt != OPRT_OK) {
            irq_mask = tkl_system_enter_critical();
            state->transfer_active = 0U;
            state->irq_tx = NULL;
            state->irq_rx = NULL;
            if (state->irq_enabled) NVIC_EnableIRQ(s_spi_irqn[port]);
            tkl_system_exit_critical(irq_mask);
            return rt;
        }
        irq_mask = tkl_system_enter_critical();
        if (!state->irq_enabled || !state->transfer_active) {
            tkl_system_exit_critical(irq_mask);
            return OPRT_COM_ERROR;
        }
        spi_disable(periph);
        SPI_STATC(periph) = SPI_STATC_ETC | SPI_STATC_SPDC |
            SPI_STATC_TXURERRC | SPI_STATC_RXORERRC | SPI_STATC_CONFERRC;
        spi_current_data_num_config(periph, frames);
        NVIC_ClearPendingIRQ(s_spi_irqn[port]);
        spi_enable(periph);
        spi_master_transfer_start(periph, SPI_TRANS_START);
        spi_i2s_interrupt_enable(periph, SPI_I2S_INT_TP | SPI_I2S_INT_RP |
            SPI_I2S_INT_ESTC | SPI_I2S_INT_TXURE | SPI_I2S_INT_RXORE |
            SPI_I2S_INT_CONFE);
        NVIC_EnableIRQ(s_spi_irqn[port]);
        tkl_system_exit_critical(irq_mask);
        return OPRT_OK;
    }

    uint32_t mask = tkl_system_enter_critical();
    if (s_spi[port].transfer_active) {
        tkl_system_exit_critical(mask);
        return OPRT_COM_ERROR;
    }
    s_spi[port].transfer_active = 1U;
    s_spi[port].data_lost = 0U;
    s_spi[port].mode_fault = 0U;
    tkl_system_exit_critical(mask);
    while (frames != 0U) {
        uint32_t chunk = frames > 0xFFFFU ? 0xFFFFU : frames;
        const uint8_t *tx8 = send_buf == NULL ? NULL :
                             (const uint8_t *)send_buf + offset * frame_bytes;
        uint8_t *rx8 = receive_buf == NULL ? NULL :
                       (uint8_t *)receive_buf + offset * frame_bytes;
        OPERATE_RET rt = __spi_transfer_once(port, tx8, rx8, chunk);
        if (rt != OPRT_OK) {
            s_spi[port].data_count = (int32_t)(offset * frame_bytes);
            mask = tkl_system_enter_critical();
            s_spi[port].transfer_active = 0U;
            tkl_system_exit_critical(mask);
            return rt;
        }
        offset += chunk;
        frames -= chunk;
    }
    s_spi[port].data_count = (int32_t)size;
    mask = tkl_system_enter_critical();
    s_spi[port].transfer_active = 0U;
    tkl_system_exit_critical(mask);
    return OPRT_OK;
}

OPERATE_RET tkl_spi_send(TUYA_SPI_NUM_E port, void *data, uint32_t size)
{
    return __spi_transfer(port, data, NULL, size);
}
OPERATE_RET tkl_spi_recv(TUYA_SPI_NUM_E port, void *data, uint32_t size)
{
    return __spi_transfer(port, NULL, data, size);
}
OPERATE_RET tkl_spi_transfer(TUYA_SPI_NUM_E port, void *send_buf,
                             void *receive_buf, uint32_t length)
{
    return __spi_transfer(port, send_buf, receive_buf, length);
}
OPERATE_RET tkl_spi_transfer_with_length(TUYA_SPI_NUM_E port, void *send_buf,
                                         uint32_t send_len, void *receive_buf,
                                         uint32_t receive_len)
{
    if (send_len != 0U && receive_len != 0U)
        return send_len == receive_len ?
               __spi_transfer(port, send_buf, receive_buf, send_len) :
               OPRT_NOT_SUPPORTED;
    if (send_len != 0U) return __spi_transfer(port, send_buf, NULL, send_len);
    if (receive_len != 0U) return __spi_transfer(port, NULL, receive_buf,
                                                  receive_len);
    return OPRT_INVALID_PARM;
}

OPERATE_RET tkl_spi_abort_transfer(TUYA_SPI_NUM_E port)
{
    if (port >= TUYA_SPI_NUM_MAX || !s_spi[port].initialized)
        return OPRT_INVALID_PARM;
    GD32H77X_SPI_STATE *state = &s_spi[port];
    uint32_t mask = tkl_system_enter_critical();
    if (state->irq_enabled) {
        spi_i2s_interrupt_disable(state->periph, SPI_I2S_INT_TP | SPI_I2S_INT_RP |
            SPI_I2S_INT_ESTC | SPI_I2S_INT_TXURE | SPI_I2S_INT_RXORE |
            SPI_I2S_INT_CONFE);
    }
    state->transfer_active = 0U;
    state->irq_tx = NULL;
    state->irq_rx = NULL;
    tkl_system_exit_critical(mask);
#ifndef GD32H77X_SPI_HOST_TEST
    if (state->dma_tx.claimed) gd32h77x_dma_release(&state->dma_tx);
    if (state->dma_rx.claimed) gd32h77x_dma_release(&state->dma_rx);
#endif
    OPERATE_RET rt = __spi_master_suspend(state->periph);
    if (rt != OPRT_OK) return rt;
    SPI_STATC(state->periph) = SPI_STATC_ETC | SPI_STATC_TXURERRC |
                               SPI_STATC_RXORERRC | SPI_STATC_CONFERRC;
    return OPRT_OK;
}

OPERATE_RET tkl_spi_get_status(TUYA_SPI_NUM_E port, TUYA_SPI_STATUS_T *status)
{
    if (port >= TUYA_SPI_NUM_MAX || status == NULL || !s_spi[port].initialized)
        return OPRT_INVALID_PARM;
    memset(status, 0, sizeof(*status));
    status->busy = s_spi[port].transfer_active;
    status->data_lost = s_spi[port].data_lost ||
        (SPI_STAT(s_spi[port].periph) & (SPI_FLAG_RXORERR |
                                         SPI_FLAG_TXURERR)) != RESET;
    status->mode_fault = s_spi[port].mode_fault;
    return OPRT_OK;
}

int32_t tkl_spi_get_data_count(TUYA_SPI_NUM_E port)
{
    if (port >= TUYA_SPI_NUM_MAX || !s_spi[port].initialized)
        return OPRT_INVALID_PARM;
    return s_spi[port].data_count;
}

uint32_t tkl_spi_get_max_dma_data_length(void)
{
    return GD32H77X_SPI_DMA_MAX_FRAMES;
}

OPERATE_RET tkl_spi_irq_init(TUYA_SPI_NUM_E port, TUYA_SPI_IRQ_CB cb)
{
    if (port >= TUYA_SPI_NUM_MAX || !s_spi[port].initialized || cb == NULL)
        return OPRT_INVALID_PARM;
    if (s_spi[port].transfer_active) return OPRT_COM_ERROR;
    if (s_spi[port].cfg.role != TUYA_SPI_ROLE_MASTER)
        return OPRT_NOT_SUPPORTED;
    uint32_t mask = tkl_system_enter_critical();
    spi_i2s_interrupt_disable(s_spi[port].periph, SPI_I2S_INT_TP |
        SPI_I2S_INT_RP | SPI_I2S_INT_ESTC | SPI_I2S_INT_TXURE |
        SPI_I2S_INT_RXORE | SPI_I2S_INT_CONFE);
    s_spi[port].irq_cb = cb;
    s_spi[port].irq_enabled = 0U;
    NVIC_DisableIRQ(s_spi_irqn[port]);
    NVIC_ClearPendingIRQ(s_spi_irqn[port]);
    NVIC_SetPriority(s_spi_irqn[port], 6U);
    tkl_system_exit_critical(mask);
    return OPRT_OK;
}

OPERATE_RET tkl_spi_irq_enable(TUYA_SPI_NUM_E port)
{
    if (port >= TUYA_SPI_NUM_MAX || !s_spi[port].initialized)
        return OPRT_INVALID_PARM;
    if (s_spi[port].irq_cb == NULL) return OPRT_INVALID_PARM;
#ifndef GD32H77X_SPI_HOST_TEST
    if (s_spi[port].cfg.spi_dma_flags != 0U) {
        s_spi[port].irq_enabled = 1U;
        return OPRT_OK;
    }
#endif
    uint32_t mask = tkl_system_enter_critical();
    spi_i2s_interrupt_disable(s_spi[port].periph, SPI_I2S_INT_TP |
        SPI_I2S_INT_RP | SPI_I2S_INT_ESTC | SPI_I2S_INT_TXURE |
        SPI_I2S_INT_RXORE | SPI_I2S_INT_CONFE);
    SPI_STATC(s_spi[port].periph) = SPI_STATC_ETC | SPI_STATC_TXURERRC |
        SPI_STATC_RXORERRC | SPI_STATC_CONFERRC;
    s_spi[port].irq_enabled = 1U;
    NVIC_ClearPendingIRQ(s_spi_irqn[port]);
    NVIC_EnableIRQ(s_spi_irqn[port]);
    tkl_system_exit_critical(mask);
    return OPRT_OK;
}

OPERATE_RET tkl_spi_irq_disable(TUYA_SPI_NUM_E port)
{
    if (port >= TUYA_SPI_NUM_MAX || !s_spi[port].initialized)
        return OPRT_INVALID_PARM;
    uint32_t mask = tkl_system_enter_critical();
    GD32H77X_SPI_STATE *state = &s_spi[port];
    state->irq_enabled = 0U;
    NVIC_DisableIRQ(s_spi_irqn[port]);
    NVIC_ClearPendingIRQ(s_spi_irqn[port]);
    spi_i2s_interrupt_disable(state->periph, SPI_I2S_INT_TP | SPI_I2S_INT_RP |
        SPI_I2S_INT_ESTC | SPI_I2S_INT_TXURE | SPI_I2S_INT_RXORE |
        SPI_I2S_INT_CONFE);
    state->transfer_active = 0U;
    state->irq_tx = NULL;
    state->irq_rx = NULL;
    SPI_STATC(state->periph) = SPI_STATC_ETC | SPI_STATC_TXURERRC |
        SPI_STATC_RXORERRC | SPI_STATC_CONFERRC;
    tkl_system_exit_critical(mask);
#ifndef GD32H77X_SPI_HOST_TEST
    if (state->dma_tx.claimed) gd32h77x_dma_release(&state->dma_tx);
    if (state->dma_rx.claimed) gd32h77x_dma_release(&state->dma_rx);
#endif
    if (state->cfg.role == TUYA_SPI_ROLE_MASTER)
        return __spi_master_suspend(state->periph);
    return OPRT_OK;
}

OPERATE_RET tkl_spi_ioctl(TUYA_SPI_NUM_E port, uint32_t cmd, void *args)
{
    (void)port; (void)cmd; (void)args;
    return OPRT_NOT_SUPPORTED;
}
