/**
 * @file tkl_qspi.c
 * @brief GD32H77X QSPI TKL backend built on the SPI3 quad-wire engine.
 *
 * QSPI resources are table-driven. Default SCK/IO0-IO3 pins are applied by
 * tkl_qspi_init(). Applications may override them, including optional hardware
 * NSS, through tkl_pinmux before init. CS is normally owned by the board layer.
 */

#include "tkl_qspi.h"

#ifdef BIT
#undef BIT
#endif

#include "gd32h77x_78x.h"
#include "gd32h77x_78x_spi.h"
#include "gd32h77x_dma.h"
#include "tkl_system.h"
#include "tuya_error_code.h"

#define QSPI_TIMEOUT_MS 50U
#define QSPI_DMA_TIMEOUT_MS 1000U
#define QSPI_MAX_FRAMES 0xFFFFU
#define QSPI_DMA_MIN_SIZE 256U
#define QSPI_DTCM_START 0x20000000U
#define QSPI_DTCM_END 0x20040000U

typedef struct {
    uint32_t periph;
    uint32_t clock_idx;
    uint32_t clock_src;
    uint32_t periph_rcu;
    bool valid;
} QSPI_HW_T;

typedef struct {
    bool enabled;
    uint32_t port;
    uint32_t pin;
    uint32_t af;
} QSPI_PIN_T;

typedef struct {
    QSPI_PIN_T sck;
    QSPI_PIN_T io0;
    QSPI_PIN_T io1;
    QSPI_PIN_T io2;
    QSPI_PIN_T io3;
    QSPI_PIN_T nss;
} QSPI_PIN_CFG_T;

#define QSPI_PIN(port_name, pin_num, af_val) \
    {.enabled = true, .port = port_name, .pin = GPIO_PIN_##pin_num, .af = af_val}

typedef struct {
    TUYA_QSPI_WIRE_MODE_E lines;
    uint8_t initialized;
    uint8_t use_dma;
    uint32_t transfer_count;
} QSPI_STATE_T;

static const QSPI_HW_T s_qspi_hw[TUYA_QSPI_NUM_MAX] = {
    {
        SPI3,
        IDX_SPI3, RCU_SPISRC_APB2, RCU_SPI3, true,
    },
    {0},
};
static QSPI_PIN_CFG_T s_qspi_pins[TUYA_QSPI_NUM_MAX] = {
    [TUYA_QSPI_NUM_0] = {
        .sck = QSPI_PIN(GPIOE, 12, 5), /* PE12 / TUYA_GPIO_NUM_76 / AF5 */
        .io0 = QSPI_PIN(GPIOE, 14, 5), /* PE14 / TUYA_GPIO_NUM_78 / AF5 */
        .io1 = QSPI_PIN(GPIOE, 13, 5), /* PE13 / TUYA_GPIO_NUM_77 / AF5 */
        .io2 = QSPI_PIN(GPIOE, 9, 5),  /* PE9  / TUYA_GPIO_NUM_73 / AF5 */
        .io3 = QSPI_PIN(GPIOE, 10, 5), /* PE10 / TUYA_GPIO_NUM_74 / AF5 */
        /* No default NSS: the board layer owns the device chip-select pin. This
         * is required on boards where PE11 is already used by another bus. */
    },
};
static QSPI_STATE_T s_qspi[TUYA_QSPI_NUM_MAX];
static uint32_t s_qspi_dma_dummy __attribute__((aligned(32))) = 0xFFFFFFFFU;

static const QSPI_HW_T *__qspi_hw(TUYA_QSPI_NUM_E port)
{
    return (port < TUYA_QSPI_NUM_MAX && s_qspi_hw[port].valid) ?
           &s_qspi_hw[port] : NULL;
}

void __tkl_qspi_set_pin(TUYA_QSPI_NUM_E port, TUYA_PIN_FUNC_E func,
                        uint32_t gpio_port, uint32_t gpio_pin, uint32_t af)
{
    if (func < TUYA_QSPI0_SCK || func > TUYA_QSPI0_CS ||
        (uint32_t)port >= TUYA_QSPI_NUM_MAX ||
        (func - TUYA_QSPI0_SCK) / 6U != (uint32_t)port) {
        return;
    }

    QSPI_PIN_T *pin_cfg = NULL;
    switch ((func - TUYA_QSPI0_SCK) % 6U) {
        case 0U: pin_cfg = &s_qspi_pins[port].sck; break;
        case 1U: pin_cfg = &s_qspi_pins[port].io0; break;
        case 2U: pin_cfg = &s_qspi_pins[port].io1; break;
        case 3U: pin_cfg = &s_qspi_pins[port].io2; break;
        case 4U: pin_cfg = &s_qspi_pins[port].io3; break;
        case 5U: pin_cfg = &s_qspi_pins[port].nss; break;
        default: return;
    }
    pin_cfg->enabled = true;
    pin_cfg->port = gpio_port;
    pin_cfg->pin = gpio_pin;
    pin_cfg->af = af;
}

static uint32_t __qspi_gpio_rcu(uint32_t port)
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

static void __qspi_apply_one_pin(const QSPI_PIN_T *pin)
{
    if (!pin->enabled) return;
    uint32_t rcu = __qspi_gpio_rcu(pin->port);
    if (rcu == 0U) return;
    rcu_periph_clock_enable(rcu);
    gpio_af_set(pin->port, pin->af, pin->pin);
    gpio_mode_set(pin->port, GPIO_MODE_AF, GPIO_PUPD_NONE, pin->pin);
    gpio_output_options_set(pin->port, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ,
                            pin->pin);
}

static void __qspi_init_gpio(TUYA_QSPI_NUM_E port)
{
    __qspi_apply_one_pin(&s_qspi_pins[port].sck);
    __qspi_apply_one_pin(&s_qspi_pins[port].io0);
    __qspi_apply_one_pin(&s_qspi_pins[port].io1);
    __qspi_apply_one_pin(&s_qspi_pins[port].io2);
    __qspi_apply_one_pin(&s_qspi_pins[port].io3);
    __qspi_apply_one_pin(&s_qspi_pins[port].nss);
}

static OPERATE_RET __qspi_wait_flag(const QSPI_HW_T *hw, uint32_t flag)
{
    uint32_t start_ms = (uint32_t)tkl_system_get_millisecond();

    while ((SPI_STAT(hw->periph) & flag) == RESET) {
        if ((SPI_STAT(hw->periph) & (SPI_FLAG_CONFERR |
                                     SPI_FLAG_RXORERR |
                                     SPI_FLAG_TXURERR)) != RESET) {
            return OPRT_COM_ERROR;
        }
        if ((uint32_t)tkl_system_get_millisecond() - start_ms >=
            QSPI_TIMEOUT_MS) {
            return OPRT_TIMEOUT;
        }
    }
    return OPRT_OK;
}

/* SPIEN must not be cleared while MSTART is still set.  Request the hardware
 * suspend first, then wait until the master transaction has actually stopped.
 */
static OPERATE_RET __qspi_master_suspend(const QSPI_HW_T *hw)
{
    uint32_t start_ms;

    if ((SPI_CTL0(hw->periph) & SPI_CTL0_MSTART) == RESET) {
        SPI_STATC(hw->periph) = SPI_STATC_SPDC;
        return OPRT_OK;
    }

    SPI_CTL0(hw->periph) |= SPI_CTL0_MSPDR;
    start_ms = (uint32_t)tkl_system_get_millisecond();
    while ((SPI_CTL0(hw->periph) & SPI_CTL0_MSTART) != RESET) {
        if ((uint32_t)tkl_system_get_millisecond() - start_ms >=
            QSPI_TIMEOUT_MS) {
            return OPRT_TIMEOUT;
        }
    }
    SPI_STATC(hw->periph) = SPI_STATC_SPDC;
    return OPRT_OK;
}

static uint32_t __qspi_prescaler(uint32_t base_freq, uint32_t target_freq)
{
    static const uint32_t divs[] = {2U, 4U, 8U, 16U, 32U, 64U, 128U, 256U};
    static const uint32_t psc[] = {
        SPI_PSC_2, SPI_PSC_4, SPI_PSC_8, SPI_PSC_16,
        SPI_PSC_32, SPI_PSC_64, SPI_PSC_128, SPI_PSC_256,
    };

    if (target_freq == 0U) {
        return SPI_PSC_256;
    }
    for (uint32_t i = 0U; i < sizeof(divs) / sizeof(divs[0]); ++i) {
        if ((base_freq / divs[i]) <= target_freq) {
            return psc[i];
        }
    }
    return SPI_PSC_256;
}

static bool __qspi_lines_supported(TUYA_QSPI_WIRE_MODE_E lines)
{
    return lines == TUYA_QSPI_1WIRE || lines == TUYA_QSPI_4WIRE;
}

static OPERATE_RET __qspi_set_lines(const QSPI_HW_T *hw,
                                    TUYA_QSPI_WIRE_MODE_E lines, bool read)
{
    if (lines == TUYA_QSPI_1WIRE) {
        spi_quad_write_enable(hw->periph);
        spi_quad_disable(hw->periph);
        return OPRT_OK;
    }
    if (lines == TUYA_QSPI_4WIRE) {
        if (read) {
            spi_quad_read_enable(hw->periph);
        } else {
            spi_quad_write_enable(hw->periph);
        }
        spi_quad_enable(hw->periph);
        return OPRT_OK;
    }
    return OPRT_NOT_SUPPORTED;
}

static OPERATE_RET __qspi_prepare_session(const QSPI_HW_T *hw,
                                         TUYA_QSPI_WIRE_MODE_E lines,
                                         bool read, uint32_t frames,
                                         bool dma_tx, bool dma_rx)
{
    OPERATE_RET rt;
    uint32_t periph = hw->periph;

    if (frames == 0U || frames > QSPI_MAX_FRAMES ||
        !__qspi_lines_supported(lines)) {
        return OPRT_INVALID_PARM;
    }

    rt = __qspi_master_suspend(hw);
    if (rt != OPRT_OK) {
        return rt;
    }
    spi_disable(periph);
    SPI_STATC(periph) = SPI_STATC_ETC | SPI_STATC_SPDC |
        SPI_STATC_TXURERRC | SPI_STATC_RXORERRC | SPI_STATC_CONFERRC;
    if (lines == TUYA_QSPI_4WIRE) {
        /* Four-wire FIFO packets contain four 8-bit frames per word. */
        spi_byte_access_disable(periph);
        spi_word_access_enable(periph);
        spi_fifo_threshold_level_set(periph, SPI_FIFO_TH_04DATA);
    } else {
        spi_word_access_disable(periph);
        spi_byte_access_enable(periph);
        spi_fifo_threshold_level_set(periph, SPI_FIFO_TH_01DATA);
    }
    rt = __qspi_set_lines(hw, lines, read);
    if (rt != OPRT_OK) {
        spi_enable(periph);
        return rt;
    }

    spi_current_data_num_config(periph, frames);
    spi_dma_disable(periph, SPI_DMA_TRANSMIT);
    spi_dma_disable(periph, SPI_DMA_RECEIVE);
    if (dma_tx) {
        spi_dma_enable(periph, SPI_DMA_TRANSMIT);
    }
    if (dma_rx) {
        spi_dma_enable(periph, SPI_DMA_RECEIVE);
    }
    spi_enable(periph);
    return OPRT_OK;
}

static void __qspi_start_session(const QSPI_HW_T *hw)
{
    spi_master_transfer_start(hw->periph, SPI_TRANS_START);
}

static OPERATE_RET __qspi_end_session(const QSPI_HW_T *hw)
{
    OPERATE_RET rt = __qspi_wait_flag(hw, SPI_FLAG_ET);
    OPERATE_RET stop_rt = __qspi_master_suspend(hw);

    spi_dma_disable(hw->periph, SPI_DMA_TRANSMIT);
    spi_dma_disable(hw->periph, SPI_DMA_RECEIVE);
    if (stop_rt == OPRT_OK) {
        SPI_STATC(hw->periph) = SPI_STATC_ETC | SPI_STATC_TXURERRC |
            SPI_STATC_RXORERRC | SPI_STATC_CONFERRC;
    }
    return rt == OPRT_OK ? stop_rt : rt;
}

static bool __qspi_dcache_enabled(void)
{
    return (SCB->CCR & SCB_CCR_DC_Msk) != 0U;
}

static void __qspi_dma_cache_prepare(const void *tx, uint32_t tx_size,
                                     void *rx, uint32_t rx_size)
{
    if (!__qspi_dcache_enabled()) {
        return;
    }
    if (tx != NULL && tx_size != 0U) {
        SCB_CleanDCache_by_Addr((volatile void *)tx, (int32_t)tx_size);
    }
    if (rx != NULL && rx_size != 0U) {
        SCB_CleanDCache_by_Addr((volatile void *)rx, (int32_t)rx_size);
    }
}

static void __qspi_dma_cache_invalidate(void *rx, uint32_t rx_size)
{
    if (__qspi_dcache_enabled() && rx != NULL && rx_size != 0U) {
        SCB_InvalidateDCache_by_Addr((volatile void *)rx, (int32_t)rx_size);
    }
}

static OPERATE_RET __qspi_dma_status(int status)
{
    if (status == GD32H77X_DMA_OK) {
        return OPRT_OK;
    }
    if (status == GD32H77X_DMA_TIMEOUT) {
        return OPRT_TIMEOUT;
    }
    if (status == GD32H77X_DMA_INVALID) {
        return OPRT_INVALID_PARM;
    }
    return OPRT_COM_ERROR;
}

static OPERATE_RET __qspi_dma_config_channel(gd32h77x_dma_handle_t *handle,
                                             uint32_t periph_addr,
                                             uint32_t memory_addr,
                                             bool memory_increment,
                                             uint32_t width,
                                             uint32_t direction,
                                             uint32_t frames)
{
    uint32_t memory_burst = DMA_MEMORY_BURST_SINGLE;
    if (width == DMA_PERIPH_WIDTH_32BIT && direction == DMA_MEMORY_TO_PERIPH &&
        frames >= 8U && (memory_addr & 31U) == 0U) {
        memory_burst = DMA_MEMORY_BURST_8_BEAT;
    }
    GD32H77X_DMA_SINGLE_CFG_T cfg = {
        .periph_addr = periph_addr,
        .memory_addr = memory_addr,
        .memory_increment = memory_increment,
        .width = width,
        .direction = direction,
        .frames = frames,
        .priority = DMA_PRIORITY_HIGH,
        .memory_burst = memory_burst,
        .periph_burst = DMA_PERIPH_BURST_SINGLE,
    };

    return __qspi_dma_status(gd32h77x_dma_single_config(handle, &cfg));
}

static OPERATE_RET __qspi_dma_write_chunk(const QSPI_HW_T *hw,
                                          const uint8_t *data, uint32_t size,
                                          TUYA_QSPI_WIRE_MODE_E lines)
{
    gd32h77x_dma_handle_t tx = {0};
    gd32h77x_dma_handle_t rx = {0};
    uint32_t width = lines == TUYA_QSPI_4WIRE ? DMA_PERIPH_WIDTH_32BIT :
                                                DMA_PERIPH_WIDTH_8BIT;
    uint32_t frames = lines == TUYA_QSPI_4WIRE ? size / 4U : size;
    /* Quad write mode still produces receive packets. Keep RX DMA active so
     * RxFIFO cannot overrun and prevent end-of-transfer completion. */
    bool need_rx_dummy = true;
    OPERATE_RET rt;

    if ((lines == TUYA_QSPI_4WIRE &&
         ((size % 4U) != 0U || ((uintptr_t)data & 3U) != 0U)) ||
        frames == 0U || frames > QSPI_MAX_FRAMES) {
        return OPRT_NOT_SUPPORTED;
    }

    rt = __qspi_dma_status(gd32h77x_dma_claim(&tx, DMA_REQUEST_SPI3_TX,
                                               NULL, NULL));
    if (rt != OPRT_OK) {
        return rt;
    }
    if (need_rx_dummy) {
        rt = __qspi_dma_status(gd32h77x_dma_claim(&rx, DMA_REQUEST_SPI3_RX,
                                                   NULL, NULL));
        if (rt != OPRT_OK) {
            gd32h77x_dma_release(&tx);
            return rt;
        }
    }

    __qspi_dma_cache_prepare(data, size, NULL, 0U);
    rt = __qspi_dma_config_channel(&tx, (uint32_t)&SPI_TDATA(hw->periph),
                                   (uint32_t)(uintptr_t)data, true, width,
                                   DMA_MEMORY_TO_PERIPH, frames);
    if (rt == OPRT_OK && need_rx_dummy) {
        rt = __qspi_dma_config_channel(&rx, (uint32_t)&SPI_RDATA(hw->periph),
                                       (uint32_t)(uintptr_t)&s_qspi_dma_dummy,
                                       false, width, DMA_PERIPH_TO_MEMORY,
                                       frames);
    }
    if (rt != OPRT_OK) {
        goto exit;
    }

    rt = __qspi_prepare_session(hw, lines, false, size, true, need_rx_dummy);
    if (rt != OPRT_OK) {
        goto exit;
    }
    if (need_rx_dummy) {
        (void)gd32h77x_dma_start(&rx, false);
    }
    (void)gd32h77x_dma_start(&tx, false);
    __qspi_start_session(hw);

    rt = __qspi_dma_status(gd32h77x_dma_wait(&tx, QSPI_DMA_TIMEOUT_MS));
    if (rt == OPRT_OK && need_rx_dummy) {
        rt = __qspi_dma_status(gd32h77x_dma_wait(&rx, QSPI_DMA_TIMEOUT_MS));
    }
    OPERATE_RET stop_rt = __qspi_end_session(hw);
    if (rt == OPRT_OK) {
        rt = stop_rt;
    }
exit:
    gd32h77x_dma_release(&tx);
    gd32h77x_dma_release(&rx);
    return rt;
}

static OPERATE_RET __qspi_dma_read_chunk(const QSPI_HW_T *hw,
                                         uint8_t *data, uint32_t size,
                                         TUYA_QSPI_WIRE_MODE_E lines)
{
    gd32h77x_dma_handle_t tx = {0};
    gd32h77x_dma_handle_t rx = {0};
    uint32_t width = lines == TUYA_QSPI_4WIRE ? DMA_PERIPH_WIDTH_32BIT :
                                                DMA_PERIPH_WIDTH_8BIT;
    uint32_t frames = lines == TUYA_QSPI_4WIRE ? size / 4U : size;
    OPERATE_RET rt;

    if ((lines == TUYA_QSPI_4WIRE &&
         ((size % 4U) != 0U || ((uintptr_t)data & 3U) != 0U)) ||
        frames == 0U || frames > QSPI_MAX_FRAMES) {
        return OPRT_NOT_SUPPORTED;
    }

    rt = __qspi_dma_status(gd32h77x_dma_claim(&tx, DMA_REQUEST_SPI3_TX,
                                               NULL, NULL));
    if (rt != OPRT_OK) {
        return rt;
    }
    rt = __qspi_dma_status(gd32h77x_dma_claim(&rx, DMA_REQUEST_SPI3_RX,
                                               NULL, NULL));
    if (rt != OPRT_OK) {
        gd32h77x_dma_release(&tx);
        return rt;
    }

    __qspi_dma_cache_prepare(NULL, 0U, data, size);
    rt = __qspi_dma_config_channel(&tx, (uint32_t)&SPI_TDATA(hw->periph),
                                   (uint32_t)(uintptr_t)&s_qspi_dma_dummy,
                                   false, width, DMA_MEMORY_TO_PERIPH,
                                   frames);
    if (rt == OPRT_OK) {
        rt = __qspi_dma_config_channel(&rx, (uint32_t)&SPI_RDATA(hw->periph),
                                       (uint32_t)(uintptr_t)data, true, width,
                                       DMA_PERIPH_TO_MEMORY, frames);
    }
    if (rt != OPRT_OK) {
        goto exit;
    }

    rt = __qspi_prepare_session(hw, lines, true, size, true, true);
    if (rt != OPRT_OK) {
        goto exit;
    }
    (void)gd32h77x_dma_start(&rx, false);
    (void)gd32h77x_dma_start(&tx, false);
    __qspi_start_session(hw);

    rt = __qspi_dma_status(gd32h77x_dma_wait(&tx, QSPI_DMA_TIMEOUT_MS));
    if (rt == OPRT_OK) {
        rt = __qspi_dma_status(gd32h77x_dma_wait(&rx, QSPI_DMA_TIMEOUT_MS));
    }
    OPERATE_RET stop_rt = __qspi_end_session(hw);
    if (rt == OPRT_OK) {
        rt = stop_rt;
    }
    if (rt == OPRT_OK) {
        __qspi_dma_cache_invalidate(data, size);
    }

exit:
    gd32h77x_dma_release(&tx);
    gd32h77x_dma_release(&rx);
    return rt;
}

static OPERATE_RET __qspi_write_chunk(const QSPI_HW_T *hw,
                                      const uint8_t *data, uint32_t size,
                                      TUYA_QSPI_WIRE_MODE_E lines)
{
    OPERATE_RET rt;

    rt = __qspi_prepare_session(hw, lines, false, size, false, false);
    if (rt != OPRT_OK) {
        return rt;
    }
    __qspi_start_session(hw);

    if (lines == TUYA_QSPI_4WIRE) {
        for (uint32_t i = 0U; i < size; i += 4U) {
            uint32_t word = 0U;

            rt = __qspi_wait_flag(hw, SPI_FLAG_TP);
            if (rt != OPRT_OK) {
                break;
            }
            for (uint32_t j = 0U; j < 4U && i + j < size; ++j) {
                word |= (uint32_t)data[i + j] << (8U * j);
            }
            /* TXSIZE still stops the transaction at the exact byte count;
             * padding only fills the final four-frame FIFO packet.
             */
            SPI_TDATA(hw->periph) = word;
        }
    } else {
        for (uint32_t i = 0U; i < size; ++i) {
            rt = __qspi_wait_flag(hw, SPI_FLAG_TP);
            if (rt != OPRT_OK) {
                break;
            }
            SPI_TDATA(hw->periph) = data[i];

            /* SPI3 is configured full-duplex in 1-wire mode, so every
             * written frame also creates an RX frame. Drain it to avoid RX
             * overrun. Quad-write mode drives all four IO pins and has no
             * RX path.
             */
            rt = __qspi_wait_flag(hw, SPI_FLAG_RP);
            if (rt != OPRT_OK) {
                break;
            }
            (void)SPI_RDATA(hw->periph);
        }
    }

    OPERATE_RET stop_rt = __qspi_end_session(hw);
    return rt == OPRT_OK ? stop_rt : rt;
}

static OPERATE_RET __qspi_read_chunk(const QSPI_HW_T *hw, uint8_t *data,
                                     uint32_t size,
                                     TUYA_QSPI_WIRE_MODE_E lines)
{
    OPERATE_RET rt;

    if (lines == TUYA_QSPI_4WIRE && (size % 4U) != 0U) {
        return OPRT_INVALID_PARM;
    }
    rt = __qspi_prepare_session(hw, lines, true, size, false, false);
    if (rt != OPRT_OK) {
        return rt;
    }
    __qspi_start_session(hw);

    if (lines == TUYA_QSPI_4WIRE) {
        for (uint32_t i = 0U; i < size; i += 4U) {
            rt = __qspi_wait_flag(hw, SPI_FLAG_TP);
            if (rt != OPRT_OK) {
                break;
            }
            SPI_TDATA(hw->periph) = 0xFFFFFFFFU;
            rt = __qspi_wait_flag(hw, SPI_FLAG_RP);
            if (rt != OPRT_OK) {
                break;
            }
            uint32_t word = SPI_RDATA(hw->periph);
            for (uint32_t j = 0U; j < 4U; ++j) {
                data[i + j] = (uint8_t)(word >> (8U * j));
            }
        }
    } else {
        for (uint32_t i = 0U; i < size; ++i) {
            rt = __qspi_wait_flag(hw, SPI_FLAG_TP);
            if (rt != OPRT_OK) {
                break;
            }
            /* In 1-wire mode TDATA clocks MISO while MOSI sends dummy data. */
            SPI_TDATA(hw->periph) = 0xFFU;
            rt = __qspi_wait_flag(hw, SPI_FLAG_RP);
            if (rt != OPRT_OK) {
                break;
            }
            data[i] = (uint8_t)SPI_RDATA(hw->periph);
        }
    }

    OPERATE_RET stop_rt = __qspi_end_session(hw);
    return rt == OPRT_OK ? stop_rt : rt;
}

static bool __qspi_dma_range_allowed(const void *data, uint32_t size)
{
    if (data == NULL || size == 0U) {
        return true;
    }

    const uintptr_t begin = (uintptr_t)data;
    if (size > UINTPTR_MAX - begin) {
        return false;
    }
    const uintptr_t end = begin + size;

    /* GD32H77x DMA cannot access the CPU-private DTCM. Short transfers are
     * already handled by the CPU path; larger DTCM buffers must take it too. */
    return end <= QSPI_DTCM_START || begin >= QSPI_DTCM_END;
}

static OPERATE_RET __qspi_write_all(const QSPI_HW_T *hw,
                                    const uint8_t *data, uint32_t size,
                                    TUYA_QSPI_WIRE_MODE_E lines, bool use_dma)
{
    uint32_t offset = 0U;

    while (offset < size) {
        uint32_t max_chunk = lines == TUYA_QSPI_4WIRE ?
                             (QSPI_MAX_FRAMES & ~3U) : QSPI_MAX_FRAMES;
        uint32_t chunk = size - offset > max_chunk ?
                         max_chunk : size - offset;
        bool dma = use_dma && chunk > QSPI_DMA_MIN_SIZE &&
                   __qspi_dma_range_allowed(data + offset, chunk) &&
                   (lines != TUYA_QSPI_4WIRE ||
                    (((uintptr_t)(data + offset) & 3U) == 0U &&
                     (chunk % 4U) == 0U));
        OPERATE_RET rt = dma ?
            __qspi_dma_write_chunk(hw, data + offset, chunk, lines) :
            __qspi_write_chunk(hw, data + offset, chunk, lines);
        if (rt != OPRT_OK) {
            return rt;
        }
        offset += chunk;
    }
    return OPRT_OK;
}

static OPERATE_RET __qspi_read_all(const QSPI_HW_T *hw,
                                   uint8_t *data, uint32_t size,
                                   TUYA_QSPI_WIRE_MODE_E lines, bool use_dma)
{
    uint32_t offset = 0U;

    while (offset < size) {
        uint32_t max_chunk = lines == TUYA_QSPI_4WIRE ?
                             (QSPI_MAX_FRAMES & ~3U) : QSPI_MAX_FRAMES;
        uint32_t chunk = size - offset > max_chunk ?
                         max_chunk : size - offset;
        bool dma = use_dma && chunk > QSPI_DMA_MIN_SIZE &&
                   __qspi_dma_range_allowed(data + offset, chunk) &&
                   (lines != TUYA_QSPI_4WIRE ||
                    (((uintptr_t)(data + offset) & 3U) == 0U &&
                     (chunk % 4U) == 0U));
        OPERATE_RET rt = dma ?
            __qspi_dma_read_chunk(hw, data + offset, chunk, lines) :
            __qspi_read_chunk(hw, data + offset, chunk, lines);
        if (rt != OPRT_OK) {
            return rt;
        }
        offset += chunk;
    }
    return OPRT_OK;
}

OPERATE_RET tkl_qspi_init(TUYA_QSPI_NUM_E port,
                          const TUYA_QSPI_BASE_CFG_T *cfg)
{
    const QSPI_HW_T *hw;
    spi_parameter_struct spi_cfg;
    uint32_t mode;
    bool hardware_nss;
    OPERATE_RET rt;

    if (cfg == NULL || cfg->role != TUYA_QSPI_ROLE_MASTER ||
        cfg->freq_hz == 0U || !__qspi_lines_supported(cfg->dma_data_lines)) {
        return OPRT_NOT_SUPPORTED;
    }
    hw = __qspi_hw(port);
    if (hw == NULL) {
        return OPRT_NOT_SUPPORTED;
    }

    hardware_nss = s_qspi_pins[port].nss.enabled;
    __qspi_init_gpio(port);

    switch (cfg->mode) {
        case TUYA_QSPI_MODE1: mode = SPI_CK_PL_LOW_PH_2EDGE; break;
        case TUYA_QSPI_MODE2: mode = SPI_CK_PL_HIGH_PH_1EDGE; break;
        case TUYA_QSPI_MODE3: mode = SPI_CK_PL_HIGH_PH_2EDGE; break;
        case TUYA_QSPI_MODE0:
        default:              mode = SPI_CK_PL_LOW_PH_1EDGE; break;
    }

    if (s_qspi[port].initialized) {
        rt = __qspi_master_suspend(hw);
        if (rt != OPRT_OK) {
            return rt;
        }
    }

    rcu_spi_clock_config((spi_idx_enum)hw->clock_idx, hw->clock_src);
    rcu_periph_clock_enable(hw->periph_rcu);

    spi_disable(hw->periph);
    spi_struct_para_init(&spi_cfg);
    spi_cfg.trans_mode = SPI_TRANSMODE_FULLDUPLEX;
    spi_cfg.device_mode = SPI_MASTER;
    spi_cfg.data_size = SPI_DATASIZE_8BIT;
    spi_cfg.clock_polarity_phase = mode;
    spi_cfg.nss = hardware_nss ? SPI_NSS_HARD : SPI_NSS_SOFT;
    spi_cfg.prescale = __qspi_prescaler(rcu_clock_freq_get(CK_APB2),
                                        cfg->freq_hz);
    spi_cfg.endian = SPI_ENDIAN_MSB;
    spi_init(hw->periph, &spi_cfg);
    spi_word_access_disable(hw->periph);
    spi_byte_access_enable(hw->periph);
    spi_fifo_threshold_level_set(hw->periph, SPI_FIFO_TH_01DATA);
    if (hardware_nss) {
        spi_nss_output_control(hw->periph, SPI_NSS_HOLD_UNTIL_TRANS_END);
        spi_nss_polarity_set(hw->periph, SPI_NSS_POLARITY_LOW);
        spi_nss_output_enable(hw->periph);
    } else {
        spi_nss_internal_high(hw->periph);
    }
    rt = __qspi_set_lines(hw, TUYA_QSPI_1WIRE, false);
    if (rt != OPRT_OK) {
        return rt;
    }
    SPI_STATC(hw->periph) = SPI_STATC_ETC | SPI_STATC_SPDC |
        SPI_STATC_TXURERRC | SPI_STATC_RXORERRC | SPI_STATC_CONFERRC;
    spi_enable(hw->periph);
    spi_master_transfer_start(hw->periph, SPI_TRANS_IDLE);

    memset(&s_qspi[port], 0, sizeof(s_qspi[port]));
    s_qspi[port].lines = cfg->dma_data_lines;
    s_qspi[port].use_dma = cfg->use_dma ? 1U : 0U;
    s_qspi[port].initialized = 1U;
    return OPRT_OK;
}

OPERATE_RET tkl_qspi_deinit(TUYA_QSPI_NUM_E port)
{
    const QSPI_HW_T *hw = __qspi_hw(port);
    OPERATE_RET rt;

    if (hw == NULL || !s_qspi[port].initialized) {
        return OPRT_INVALID_PARM;
    }
    rt = __qspi_master_suspend(hw);
    if (rt != OPRT_OK) {
        return rt;
    }
    spi_disable(hw->periph);
    spi_quad_disable(hw->periph);
    SPI_STATC(hw->periph) = SPI_STATC_ETC | SPI_STATC_SPDC |
        SPI_STATC_TXURERRC | SPI_STATC_RXORERRC | SPI_STATC_CONFERRC;
    memset(&s_qspi[port], 0, sizeof(s_qspi[port]));
    return OPRT_OK;
}

OPERATE_RET tkl_qspi_send(TUYA_QSPI_NUM_E port, void *data, uint32_t size)
{
    const QSPI_HW_T *hw = __qspi_hw(port);
    OPERATE_RET rt;

    if (hw == NULL || !s_qspi[port].initialized ||
        (data == NULL && size != 0U)) {
        return OPRT_INVALID_PARM;
    }
    if (size == 0U) {
        return OPRT_OK;
    }
    rt = __qspi_write_all(hw, data, size, s_qspi[port].lines,
                          s_qspi[port].use_dma != 0U);
    if (rt == OPRT_OK) {
        s_qspi[port].transfer_count += size;
    }
    return rt;
}

OPERATE_RET tkl_qspi_recv(TUYA_QSPI_NUM_E port, void *data, uint32_t size)
{
    const QSPI_HW_T *hw = __qspi_hw(port);
    OPERATE_RET rt;

    if (hw == NULL || !s_qspi[port].initialized ||
        (data == NULL && size != 0U)) {
        return OPRT_INVALID_PARM;
    }
    if (size == 0U) {
        return OPRT_OK;
    }
    rt = __qspi_read_all(hw, data, size, s_qspi[port].lines,
                         s_qspi[port].use_dma != 0U);
    if (rt == OPRT_OK) {
        s_qspi[port].transfer_count += size;
    }
    return rt;
}

OPERATE_RET tkl_qspi_comand(TUYA_QSPI_NUM_E port, TUYA_QSPI_CMD_T *command)
{
    const QSPI_HW_T *hw = __qspi_hw(port);
    OPERATE_RET rt;
    uint32_t total;

    if (hw == NULL || !s_qspi[port].initialized || command == NULL) {
        return OPRT_INVALID_PARM;
    }
    if (command->op != TUYA_QSPI_WRITE ||
        command->dummy_cycle != 0U) {
        return OPRT_NOT_SUPPORTED;
    }
    if (command->cmd_size > sizeof(command->cmd) ||
        command->addr_size > sizeof(command->addr) ||
        (command->data_size != 0U && command->data == NULL) ||
        !__qspi_lines_supported(command->cmd_lines) ||
        !__qspi_lines_supported(command->addr_lines) ||
        !__qspi_lines_supported(command->data_lines)) {
        return OPRT_INVALID_PARM;
    }

    total = (uint32_t)command->cmd_size + command->addr_size +
            command->data_size;
    if (total == 0U) {
        return OPRT_INVALID_PARM;
    }

    if (command->cmd_size != 0U) {
        rt = __qspi_write_all(hw, command->cmd, command->cmd_size,
                              command->cmd_lines,
                              s_qspi[port].use_dma != 0U);
        if (rt != OPRT_OK) {
            goto exit;
        }
    }
    if (command->addr_size != 0U) {
        rt = __qspi_write_all(hw, command->addr, command->addr_size,
                              command->addr_lines,
                              s_qspi[port].use_dma != 0U);
        if (rt != OPRT_OK) {
            goto exit;
        }
    }
    if (command->data_size != 0U) {
        rt = __qspi_write_all(hw, command->data, command->data_size,
                              command->data_lines,
                              s_qspi[port].use_dma != 0U);
        if (rt != OPRT_OK) {
            goto exit;
        }
    }

    s_qspi[port].transfer_count += total;
    rt = OPRT_OK;

exit:
    return rt;
}

OPERATE_RET tkl_qspi_abort_transfer(TUYA_QSPI_NUM_E port)
{
    const QSPI_HW_T *hw = __qspi_hw(port);
    OPERATE_RET rt;

    if (hw == NULL || !s_qspi[port].initialized) {
        return OPRT_INVALID_PARM;
    }
    rt = __qspi_master_suspend(hw);
    if (rt == OPRT_OK) {
        SPI_STATC(hw->periph) = SPI_STATC_ETC | SPI_STATC_SPDC |
            SPI_STATC_TXURERRC | SPI_STATC_RXORERRC | SPI_STATC_CONFERRC;
    }
    return rt;
}

OPERATE_RET tkl_qspi_irq_init(TUYA_QSPI_NUM_E port, TUYA_QSPI_IRQ_CB cb)
{
    (void)port;
    (void)cb;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_qspi_irq_enable(TUYA_QSPI_NUM_E port)
{
    (void)port;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_qspi_irq_disable(TUYA_QSPI_NUM_E port)
{
    (void)port;
    return OPRT_NOT_SUPPORTED;
}
