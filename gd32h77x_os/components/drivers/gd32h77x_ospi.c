/**
 * @file gd32h77x_ospi.c
 * @brief GD32H77X OSPI/OSPIM controller abstraction.
 *
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "gd32h77x_ospi.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "gd32h77x_78x.h"
#include "gd32h77x_78x_gpio.h"
#include "gd32h77x_78x_ospi.h"
#include "gd32h77x_78x_ospim.h"
#include "gd32h77x_78x_rcu.h"
#include "gd32h77x_78x_syscfg.h"
#include "gd32h77x_status.h"

#include <stddef.h>
#include <string.h>

#define GD32H77X_OSPI_POLL_LIMIT 2000000U
#define GD32H77X_OSPI_TCFG_MASK                                             \
    (OSPI_TCFG_IMOD | OSPI_TCFG_INSDTR | OSPI_TCFG_INSSZ |               \
     OSPI_TCFG_ADDRMOD | OSPI_TCFG_ADDRDTR | OSPI_TCFG_ADDRSZ |           \
     OSPI_TCFG_ALTEMOD | OSPI_TCFG_ABDTR | OSPI_TCFG_ALTESZ |             \
     OSPI_TCFG_DATAMOD | OSPI_TCFG_DADTR | OSPI_TCFG_DQSEN |              \
     OSPI_TCFG_SIOO)
#define GD32H77X_OSPI_WTCFG_MASK                                            \
    (OSPI_WTCFG_IMOD | OSPI_WTCFG_INSDTR | OSPI_WTCFG_INSSZ |             \
     OSPI_WTCFG_ADDRMOD | OSPI_WTCFG_ADDRDTR | OSPI_WTCFG_ADDRSZ |         \
     OSPI_WTCFG_ALTEMOD | OSPI_WTCFG_ABDTR | OSPI_WTCFG_ALTESZ |           \
     OSPI_WTCFG_DATAMOD | OSPI_WTCFG_DADTR | OSPI_WTCFG_DQSEN)

void OSPI0_IRQHandler(void)
{
    /* A mapped-mode timeout releases CS, but TMOUT is sticky. The controller
     * does not guarantee subsequent mapped reads until it is cleared. This
     * handler must remain independent of FreeRTOS and of PSRAM memory. */
    if ((OSPI_STAT(OSPI0) & OSPI_FLAG_TMOUT) != 0U) {
        OSPI_STATC(OSPI0) = OSPI_STATC_TMOUTC;
        __DSB();
    }
}

static bool __pin_valid(const gd32h77x_ospi_pin_t *pin)
{
    return pin != NULL && pin->port != 0U && pin->clock != 0U &&
           pin->pin != 0U && pin->alternate <= GPIO_AF_15;
}

static uint32_t __device_size(uint32_t capacity_bytes)
{
    switch (capacity_bytes) {
    case 2U * 1024U * 1024U:
        return OSPI_MESZ_2_MBS;
    case 4U * 1024U * 1024U:
        return OSPI_MESZ_4_MBS;
    case 8U * 1024U * 1024U:
        return OSPI_MESZ_8_MBS;
    case 16U * 1024U * 1024U:
        return OSPI_MESZ_16_MBS;
    case 32U * 1024U * 1024U:
        return OSPI_MESZ_32_MBS;
    case 64U * 1024U * 1024U:
        return OSPI_MESZ_64_MBS;
    default:
        return UINT32_MAX;
    }
}

static int __lock(gd32h77x_ospi_t *ospi)
{
    return ospi->lock != NULL &&
                   xSemaphoreTake((SemaphoreHandle_t)ospi->lock,
                                  portMAX_DELAY) == pdTRUE
               ? 0
               : -1;
}

static void __unlock(gd32h77x_ospi_t *ospi)
{
    (void)xSemaphoreGive((SemaphoreHandle_t)ospi->lock);
}

static int __wait_flag(gd32h77x_ospi_t *ospi, uint32_t flag, bool set)
{
    for (uint32_t count = 0U; count < GD32H77X_OSPI_POLL_LIMIT; ++count) {
        uint32_t status = OSPI_STAT(ospi->periph);
        if ((status & OSPI_FLAG_TERR) != 0U) {
            return -1;
        }
        if (((status & flag) != 0U) == set) {
            return 0;
        }
    }
    return -1;
}

static void __clear_status(gd32h77x_ospi_t *ospi)
{
    OSPI_STATC(ospi->periph) =
        OSPI_STATC_TCC | OSPI_STATC_TERRC | OSPI_STATC_TMOUTC;
}

static void __abort_locked(gd32h77x_ospi_t *ospi)
{
    if ((OSPI_STAT(ospi->periph) & OSPI_FLAG_BUSY) != 0U) {
        OSPI_CTL(ospi->periph) |= OSPI_CTL_ABORT;
        (void)__wait_flag(ospi, OSPI_FLAG_BUSY, false);
    }
    __clear_status(ospi);
}

static void __prepare_pin_before_af(const gd32h77x_ospi_pin_t *pin,
                                    bool high)
{
    rcu_periph_clock_enable(pin->clock);
    if (high) {
        gpio_bit_set(pin->port, pin->pin);
    } else {
        gpio_bit_reset(pin->port, pin->pin);
    }
    gpio_output_options_set(pin->port, GPIO_OTYPE_PP, GPIO_OSPEED_100_220MHZ,
                            pin->pin);
    gpio_mode_set(pin->port, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, pin->pin);
}

static void __apply_af(const gd32h77x_ospi_pin_t *pin, uint32_t pull)
{
    rcu_periph_clock_enable(pin->clock);
    gpio_af_set(pin->port, pin->alternate, pin->pin);
    gpio_mode_set(pin->port, GPIO_MODE_AF, pull, pin->pin);
    gpio_output_options_set(pin->port, GPIO_OTYPE_PP, GPIO_OSPEED_100_220MHZ,
                            pin->pin);
}

static void __configure_analog_switch(const gd32h77x_ospi_config_t *config)
{
    if (config->set_pa1_switch) {
        if (config->pa1_switch_open) {
            syscfg_analog_switch_enable(SYSCFG_PA1_ANALOG_SWITCH);
        } else {
            syscfg_analog_switch_disable(SYSCFG_PA1_ANALOG_SWITCH);
        }
    }
    if (config->set_pc2_switch) {
        if (config->pc2_switch_open) {
            syscfg_analog_switch_enable(SYSCFG_PC2_ANALOG_SWITCH);
        } else {
            syscfg_analog_switch_disable(SYSCFG_PC2_ANALOG_SWITCH);
        }
    }
}

static void __configure_gpio(const gd32h77x_ospi_config_t *config)
{
    __configure_analog_switch(config);

    /* Hold the device deselected with CLK and data low before switching AF. */
    __prepare_pin_before_af(&config->cs, true);
    __prepare_pin_before_af(&config->sck, false);
    for (uint32_t index = 0U; index < 4U; ++index) {
        __prepare_pin_before_af(&config->io[index], false);
    }

    __apply_af(&config->cs, GPIO_PUPD_PULLUP);
    __apply_af(&config->sck, GPIO_PUPD_NONE);
    __apply_af(&config->io[0], GPIO_PUPD_NONE);
    __apply_af(&config->io[1], GPIO_PUPD_NONE);
    /* APS6404L exposes WP/HOLD (SIO2/SIO3) on these pads. In single-line
     * mode the MCU does not drive them, so keep them deasserted until QPI
     * transactions explicitly take ownership of the bus. */
    __apply_af(&config->io[2], GPIO_PUPD_PULLUP);
    __apply_af(&config->io[3], GPIO_PUPD_PULLUP);
}

static void __configure_ospim(const gd32h77x_ospi_config_t *config)
{
    ospim_port_sck_config(config->ospim_port, OSPIM_PORT_SCK_ENABLE);
    ospim_port_dqs_config(config->ospim_port, OSPIM_PORT_DQS_DISABLE);
    ospim_port_csn_config(config->ospim_port, OSPIM_PORT_CSN_ENABLE);
    ospim_port_io3_0_config(config->ospim_port, OSPIM_IO_LOW_ENABLE);
    ospim_port_io7_4_config(config->ospim_port, OSPIM_IO_HIGH_DISABLE);
    ospim_port_sck_source_select(config->ospim_port,
                                 OSPIM_SCK_SOURCE_OSPI0_SCK);
    ospim_port_csn_source_select(config->ospim_port,
                                 OSPIM_CSN_SOURCE_OSPI0_CSN);
    ospim_port_io3_0_source_select(config->ospim_port,
                                   OSPIM_SRCPLIO_OSPI0_IO_LOW);
}

static uint32_t __prescaler(uint32_t clock_hz, uint32_t *actual_hz)
{
    uint32_t source_hz = rcu_clock_freq_get(CK_AHB);
    if (source_hz == 0U || clock_hz == 0U) {
        return UINT32_MAX;
    }
    uint32_t divider = (source_hz + clock_hz - 1U) / clock_hz;
    if (divider == 0U) {
        divider = 1U;
    }
    if (divider > 256U) {
        return UINT32_MAX;
    }
    if (actual_hz != NULL) {
        *actual_hz = source_hz / divider;
    }
    return divider - 1U;
}

gd32h77x_status_t gd32h77x_ospi_init(gd32h77x_ospi_t *ospi,
                              const gd32h77x_ospi_config_t *config)
{
    if (ospi == NULL || config == NULL || config->ospim_port > 1U ||
        __device_size(config->capacity_bytes) == UINT32_MAX ||
        !__pin_valid(&config->cs) || !__pin_valid(&config->sck)) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    for (uint32_t index = 0U; index < 4U; ++index) {
        if (!__pin_valid(&config->io[index])) {
            return GD32H77X_STATUS_INVALID_PARAM;
        }
    }
    if (ospi->initialized) {
        return GD32H77X_STATUS_OK;
    }

    memset(ospi, 0, sizeof(*ospi));
    ospi->periph = OSPI0;
    ospi->ospim_port = config->ospim_port;
    ospi->capacity_bytes = config->capacity_bytes;
    ospi->lock = xSemaphoreCreateMutex();
    if (ospi->lock == NULL) {
        return GD32H77X_STATUS_IO_ERROR;
    }

    rcu_ospi_clock_config(RCU_OSPI_AHB);
    rcu_periph_clock_enable(RCU_SYSCFG);
    rcu_periph_clock_enable(RCU_OSPIM);
    rcu_periph_clock_enable(RCU_OSPI0);
    ospim_deinit();
    ospi_deinit(OSPI0);

    __configure_gpio(config);
    __configure_ospim(config);

    uint32_t actual_hz = 0U;
    uint32_t prescaler = __prescaler(config->clock_hz, &actual_hz);
    if (prescaler == UINT32_MAX) {
        vSemaphoreDelete((SemaphoreHandle_t)ospi->lock);
        ospi->lock = NULL;
        return GD32H77X_STATUS_INVALID_PARAM;
    }

    uint32_t dcfg0 = OSPI_DCFG0(OSPI0);
    dcfg0 &= ~(OSPI_DCFG0_DTYSEL | OSPI_DCFG0_MESZ | OSPI_DCFG0_CSHC |
               OSPI_DCFG0_RCKADD | OSPI_DCFG0_RSCKDEN | OSPI_DCFG0_RXSFT |
               OSPI_DCFG0_FRCK);
    dcfg0 |= OSPI_STANDARD_MODE | __device_size(config->capacity_bytes) |
             OSPI_CS_HIGH_TIME_3_CYCLE | OSPI_SAMPLE_DELAY_DISABLE;
    OSPI_DCFG0(OSPI0) = dcfg0;
    OSPI_DCFG1(OSPI0) = (OSPI_DCFG1(OSPI0) & ~OSPI_DCFG1_PSC) |
                        OSPI_PSC(prescaler);
    OSPI_CTL(OSPI0) = (OSPI_CTL(OSPI0) &
                       ~(OSPI_CTL_FTL | OSPI_CTL_DQMOD |
                         OSPI_CTL_TERRIE | OSPI_CTL_TCIE | OSPI_CTL_FTIE |
                         OSPI_CTL_SMIE | OSPI_CTL_TMOUTIE)) |
                      OSPI_FIFO_THRESHOLD_16 | OSPI_DUAL_QUAD_MODE_DISABLE;
    OSPI_TIMCFG(OSPI0) =
        (OSPI_TIMCFG(OSPI0) & ~(OSPI_TIMCFG_SSAMPLE | OSPI_TIMCFG_DEHQC |
                                OSPI_TIMCFG_DUMYC)) |
        OSPI_SAMPLE_SHIFTING_HALF_CYCLE | OSPI_DELAY_HOLD_NONE;
    OSPI_CTL(OSPI0) |= OSPI_CTL_OSPIEN;
    __clear_status(ospi);

    ospi->initialized = true;
    return GD32H77X_STATUS_OK;
}

gd32h77x_status_t gd32h77x_ospi_set_clock(gd32h77x_ospi_t *ospi,
                                         uint32_t clock_hz)
{
    if (ospi == NULL || !ospi->initialized || clock_hz == 0U) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    uint32_t prescaler = __prescaler(clock_hz, NULL);
    if (prescaler == UINT32_MAX) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    if (__lock(ospi) != 0) {
        return GD32H77X_STATUS_IO_ERROR;
    }
    if (__wait_flag(ospi, OSPI_FLAG_BUSY, false) != 0) {
        __abort_locked(ospi);
        __unlock(ospi);
        return GD32H77X_STATUS_TIMEOUT;
    }
    OSPI_DCFG1(ospi->periph) =
        (OSPI_DCFG1(ospi->periph) & ~OSPI_DCFG1_PSC) | OSPI_PSC(prescaler);
    __DSB();
    __unlock(ospi);
    return GD32H77X_STATUS_OK;
}

static void __prepare_common_command(gd32h77x_ospi_t *ospi,
                                     const gd32h77x_ospi_transfer_t *transfer)
{
    uint32_t tcfg = transfer->instruction_mode | OSPI_INSTRUCTION_8_BITS |
                    transfer->address_mode | transfer->address_size |
                    transfer->data_mode | OSPI_INSDTR_MODE_DISABLE |
                    OSPI_ADDRDTR_MODE_DISABLE | OSPI_DADTR_MODE_DISABLE |
                    OSPI_DQS_DISABLE | OSPI_SIOO_INST_EVERY_CMD;

    OSPI_CTL(ospi->periph) =
        (OSPI_CTL(ospi->periph) & ~(OSPI_CTL_FMOD | OSPI_CTL_DEVS)) |
        OSPI_DEVICE0_SELECT;
    OSPI_DTLEN(ospi->periph) = transfer->size == 0U ? 0U : transfer->size - 1U;
    OSPI_TCFG(ospi->periph) =
        (OSPI_TCFG(ospi->periph) & ~GD32H77X_OSPI_TCFG_MASK) | tcfg;
    OSPI_TIMCFG(ospi->periph) =
        (OSPI_TIMCFG(ospi->periph) & ~OSPI_TIMCFG_DUMYC) |
        OSPI_DUMYC(transfer->dummy_cycles);
    /* The instruction register is consumed by every transfer frame, including
     * indirect reads with an address. Omitting it leaves the previous command
     * in OSPI_INS and sends the wrong opcode. */
    OSPI_INS(ospi->periph) = transfer->instruction;
    if (transfer->has_address) {
        OSPI_ADDR(ospi->periph) = transfer->address;
    }
}

gd32h77x_status_t gd32h77x_ospi_transfer(gd32h77x_ospi_t *ospi,
                                  const gd32h77x_ospi_transfer_t *transfer)
{
    if (ospi == NULL || transfer == NULL || !ospi->initialized ||
        ospi->memory_mapped ||
        (transfer->size != 0U && transfer->data == NULL) ||
        (transfer->has_address &&
         ((uint64_t)transfer->address + transfer->size >
          ospi->capacity_bytes))) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    if (__lock(ospi) != 0) {
        return GD32H77X_STATUS_IO_ERROR;
    }
    if (__wait_flag(ospi, OSPI_FLAG_BUSY, false) != 0) {
        __abort_locked(ospi);
        __unlock(ospi);
        return GD32H77X_STATUS_TIMEOUT;
    }

    __clear_status(ospi);
    __prepare_common_command(ospi, transfer);

    if (transfer->read) {
        OSPI_CTL(ospi->periph) =
            (OSPI_CTL(ospi->periph) & ~OSPI_CTL_FMOD) | OSPI_INDIRECT_READ;
        if (transfer->has_address) {
            OSPI_ADDR(ospi->periph) = transfer->address;
        }
        for (uint32_t index = 0U; index < transfer->size; ++index) {
            if (__wait_flag(ospi, OSPI_FLAG_FT | OSPI_FLAG_TC, true) != 0) {
                __abort_locked(ospi);
                __unlock(ospi);
                return GD32H77X_STATUS_TIMEOUT;
            }
            transfer->data[index] =
                *((__IO uint8_t *)&OSPI_DATA(ospi->periph));
        }
    } else {
        OSPI_CTL(ospi->periph) =
            (OSPI_CTL(ospi->periph) & ~OSPI_CTL_FMOD) | OSPI_INDIRECT_WRITE;
        for (uint32_t index = 0U; index < transfer->size; ++index) {
            if (__wait_flag(ospi, OSPI_FLAG_FT, true) != 0) {
                __abort_locked(ospi);
                __unlock(ospi);
                return GD32H77X_STATUS_TIMEOUT;
            }
            *((__IO uint8_t *)&OSPI_DATA(ospi->periph)) =
                transfer->data[index];
        }
    }

    if (__wait_flag(ospi, OSPI_FLAG_TC, true) != 0) {
        __abort_locked(ospi);
        __unlock(ospi);
        return GD32H77X_STATUS_TIMEOUT;
    }
    __clear_status(ospi);
    __unlock(ospi);
    return GD32H77X_STATUS_OK;
}

gd32h77x_status_t gd32h77x_ospi_enter_memory_mapped(
    gd32h77x_ospi_t *ospi, const gd32h77x_ospi_memory_mapped_t *config)
{
    if (ospi == NULL || config == NULL || !ospi->initialized ||
        config->timeout_cycles == 0U) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    if (__lock(ospi) != 0) {
        return GD32H77X_STATUS_IO_ERROR;
    }
    if (__wait_flag(ospi, OSPI_FLAG_BUSY, false) != 0) {
        __abort_locked(ospi);
        __unlock(ospi);
        return GD32H77X_STATUS_TIMEOUT;
    }

    uint32_t tcfg = OSPI_INSTRUCTION_1_LINE | OSPI_INSTRUCTION_8_BITS |
                    OSPI_ADDRESS_4_LINES | OSPI_ADDRESS_24_BITS |
                    OSPI_DATA_4_LINES | OSPI_INSDTR_MODE_DISABLE |
                    OSPI_ADDRDTR_MODE_DISABLE | OSPI_DADTR_MODE_DISABLE |
                    OSPI_DQS_DISABLE | OSPI_SIOO_INST_EVERY_CMD;
    uint32_t wtcfg = OSPI_INSTRUCTION_1_LINE | OSPI_INSTRUCTION_8_BITS |
                     OSPI_ADDRESS_4_LINES | OSPI_ADDRESS_24_BITS |
                     OSPI_DATA_4_LINES | OSPI_INSDTR_MODE_DISABLE |
                     OSPI_ADDRDTR_MODE_DISABLE | OSPI_DADTR_MODE_DISABLE |
                     OSPI_DQS_DISABLE;

    OSPI_TCFG(ospi->periph) =
        (OSPI_TCFG(ospi->periph) & ~GD32H77X_OSPI_TCFG_MASK) | tcfg;
    OSPI_TIMCFG(ospi->periph) =
        (OSPI_TIMCFG(ospi->periph) & ~OSPI_TIMCFG_DUMYC) |
        OSPI_DUMYC(config->read_dummy_cycles);
    OSPI_INS(ospi->periph) = config->read_instruction;

    OSPI_WTCFG(ospi->periph) =
        (OSPI_WTCFG(ospi->periph) & ~GD32H77X_OSPI_WTCFG_MASK) | wtcfg;
    OSPI_WTIMCFG(ospi->periph) &= ~OSPI_WTIMCFG_DUMYC;
    OSPI_WINS(ospi->periph) = config->write_instruction;

    /* Cortex-M7 cache line fills may use the OSPI wrapped-read frame. */
    OSPI_WPTCFG(ospi->periph) =
        (OSPI_WPTCFG(ospi->periph) & ~GD32H77X_OSPI_TCFG_MASK) | tcfg;
    uint32_t read_timing =
        OSPI_TIMCFG(ospi->periph) & (OSPI_TIMCFG_DEHQC | OSPI_TIMCFG_SSAMPLE);
    OSPI_WPTIMCFG(ospi->periph) =
        (OSPI_WPTIMCFG(ospi->periph) &
         ~(OSPI_WPTIMCFG_DUMYC | OSPI_TIMCFG_DEHQC | OSPI_TIMCFG_SSAMPLE)) |
        OSPI_DUMYC(config->read_dummy_cycles) | read_timing;
    OSPI_WPINS(ospi->periph) = config->read_instruction;
    OSPI_WPALTE(ospi->periph) = 0U;

    /* Release CE# periodically so the PSRAM can perform internal refresh even
     * when the AXI master keeps a mapped transaction stream active. */
    OSPI_DCFG3(ospi->periph) = config->refresh_cycles;
    OSPI_LPOUT(ospi->periph) = config->timeout_cycles;
    __clear_status(ospi);
    /* Clear timeout even inside an RTOS critical section: no kernel APIs are
     * used by this priority-zero ISR, and mapped accesses must not see TMOUT. */
    NVIC_SetPriority(OSPI0_IRQn, 0U);
    NVIC_ClearPendingIRQ(OSPI0_IRQn);
    NVIC_EnableIRQ(OSPI0_IRQn);
    OSPI_CTL(ospi->periph) =
        (OSPI_CTL(ospi->periph) &
         ~(OSPI_CTL_TMOUTEN | OSPI_CTL_FMOD | OSPI_CTL_TMOUTIE)) |
        OSPI_TIMEOUT_COUNT_ENABLE | OSPI_CTL_TMOUTIE | OSPI_MEMORY_MAPPED;
    __DSB();
    __ISB();
    ospi->memory_mapped = true;
    __unlock(ospi);
    return GD32H77X_STATUS_OK;
}

gd32h77x_status_t gd32h77x_ospi_abort(gd32h77x_ospi_t *ospi)
{
    if (ospi == NULL || !ospi->initialized || __lock(ospi) != 0) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    NVIC_DisableIRQ(OSPI0_IRQn);
    OSPI_CTL(ospi->periph) &= ~OSPI_CTL_TMOUTIE;
    __abort_locked(ospi);
    NVIC_ClearPendingIRQ(OSPI0_IRQn);
    ospi->memory_mapped = false;
    __unlock(ospi);
    return GD32H77X_STATUS_OK;
}

void gd32h77x_ospi_deinit(gd32h77x_ospi_t *ospi)
{
    if (ospi == NULL) {
        return;
    }
    if (ospi->initialized && ospi->lock != NULL) {
        (void)gd32h77x_ospi_abort(ospi);
        vSemaphoreDelete((SemaphoreHandle_t)ospi->lock);
    }
    if (ospi->periph == OSPI0) {
        ospi_disable(OSPI0);
    }
    memset(ospi, 0, sizeof(*ospi));
}
