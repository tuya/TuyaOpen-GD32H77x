/**
 * @file gd32h77x_ospi.c
 * @brief Thin GD32H77X OSPI/OSPIM adapter over the vendor peripheral driver.
 *
 * This file does not implement its own OSPI command engine. It validates the
 * board-level configuration, translates PSRAM profile frames to vendor command
 * structures, and delegates register/FIFO handling to the GigaDevice standard
 * peripheral library.
 *
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "gd32h77x_ospi.h"

#include "FreeRTOS.h"
#include "task.h"
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

void OSPI0_IRQHandler(void)
{
    if (SET == ospi_flag_get(OSPI0, OSPI_FLAG_TMOUT)) {
        ospi_flag_clear(OSPI0, OSPI_FLAG_TMOUT);
        __DSB();
    }
}

static bool __pin_valid(const gd32h77x_ospi_pin_t *pin)
{
    return pin != NULL && pin->port != 0U && pin->clock != 0U &&
           pin->pin != 0U && pin->alternate <= GPIO_AF_15;
}

static bool __gpio_pin_valid(const gd32h77x_ospi_pin_t *pin)
{
    return pin != NULL && pin->port != 0U && pin->clock != 0U &&
           pin->pin != 0U;
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

static void __clear_status(gd32h77x_ospi_t *ospi)
{
    ospi_flag_clear(ospi->periph,
                    OSPI_FLAG_TC | OSPI_FLAG_TERR | OSPI_FLAG_TMOUT);
}

static void __abort_locked(gd32h77x_ospi_t *ospi)
{
    ospi_timeout_disable(ospi->periph);
    ospi_interrupt_disable(ospi->periph, OSPI_INT_TMOUT);
    ospi_transmission_abort(ospi->periph);
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
    uint32_t io_count = config->octal ? 8U : 4U;

    __configure_analog_switch(config);

    if (config->reset_enabled) {
        rcu_periph_clock_enable(config->reset.clock);
        gpio_bit_set(config->reset.port, config->reset.pin);
        gpio_output_options_set(config->reset.port, GPIO_OTYPE_PP,
                                GPIO_OSPEED_100_220MHZ, config->reset.pin);
        gpio_mode_set(config->reset.port, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE,
                      config->reset.pin);
    }

    __prepare_pin_before_af(&config->cs, true);
    __prepare_pin_before_af(&config->sck, false);
    for (uint32_t index = 0U; index < io_count; ++index) {
        __prepare_pin_before_af(&config->io[index], false);
    }
    if (config->octal) {
        __prepare_pin_before_af(&config->dqs, false);
    }

    __apply_af(&config->cs, config->octal ? GPIO_PUPD_NONE
                                          : GPIO_PUPD_PULLUP);
    __apply_af(&config->sck, GPIO_PUPD_NONE);
    for (uint32_t index = 0U; index < io_count; ++index) {
        __apply_af(&config->io[index], GPIO_PUPD_NONE);
    }
    if (config->octal) {
        __apply_af(&config->dqs, GPIO_PUPD_NONE);
    }
}

static void __configure_ospim(const gd32h77x_ospi_config_t *config)
{
    ospim_port_sck_config(config->ospim_port, OSPIM_PORT_SCK_ENABLE);
    ospim_port_dqs_config(config->ospim_port, config->octal
                                                       ? OSPIM_PORT_DQS_ENABLE
                                                       : OSPIM_PORT_DQS_DISABLE);
    ospim_port_csn_config(config->ospim_port, OSPIM_PORT_CSN_ENABLE);
    ospim_port_io3_0_config(config->ospim_port, OSPIM_IO_LOW_ENABLE);
    ospim_port_io7_4_config(config->ospim_port, config->octal
                                                       ? OSPIM_IO_HIGH_ENABLE
                                                       : OSPIM_IO_HIGH_DISABLE);
    ospim_port_sck_source_select(config->ospim_port,
                                 OSPIM_SCK_SOURCE_OSPI0_SCK);
    ospim_port_csn_source_select(config->ospim_port,
                                 OSPIM_CSN_SOURCE_OSPI0_CSN);
    ospim_port_io3_0_source_select(config->ospim_port,
                                   OSPIM_SRCPLIO_OSPI0_IO_LOW);
    if (config->octal) {
        ospim_port_io7_4_source_select(config->ospim_port,
                                       OSPIM_SRCPHIO_OSPI0_IO_HIGH);
        ospim_port_dqs_source_select(config->ospim_port,
                                     OSPIM_DQS_SOURCE_OSPI0_DQS);
    }
}

static uint32_t __prescaler(uint32_t clock_hz)
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
    return divider - 1U;
}

static void __command_parameters(const gd32h77x_ospi_t *ospi,
                                 ospi_parameter_struct *parameters)
{
    ospi_struct_init(parameters);
    parameters->memory_type = ospi->memory_type;
    parameters->delay_hold_cycle = ospi->delay_hold_cycle;
    parameters->dual_quad = OSPI_DUAL_QUAD_MODE_DISABLE;
}

static void __frame_to_command(const gd32h77x_ospi_frame_t *frame,
                               uint32_t operation_type, uint32_t address,
                               bool has_address, uint32_t data_size,
                               ospi_regular_cmd_struct *command)
{
    memset(command, 0, sizeof(*command));
    command->operation_type = operation_type;
    command->device_id = OSPI_DEVICE0_SELECT;
    command->instruction = frame->instruction;
    command->ins_mode = frame->instruction_mode;
    command->ins_size = frame->instruction_size != 0U
                            ? frame->instruction_size
                            : OSPI_INSTRUCTION_8_BITS;
    command->ins_dtr_mode = frame->instruction_dtr
                                ? OSPI_INSDTR_MODE_ENABLE
                                : OSPI_INSDTR_MODE_DISABLE;
    command->address = has_address ? address : 0U;
    command->addr_mode = frame->address_mode;
    command->addr_size = frame->address_size;
    command->addr_dtr_mode = frame->address_dtr ? OSPI_ADDRDTR_MODE_ENABLE
                                                : OSPI_ADDRDTR_MODE_DISABLE;
    command->alter_bytes = 0U;
    command->alter_bytes_mode = OSPI_ALTERNATE_BYTES_NONE;
    command->alter_bytes_size = OSPI_ALTERNATE_BYTES_8_BITS;
    command->alter_bytes_dtr_mode = OSPI_ABDTR_MODE_DISABLE;
    command->data_mode = frame->data_mode;
    command->nbdata = data_size == 0U ? 1U : data_size;
    command->data_dtr_mode = frame->data_dtr ? OSPI_DADTR_MODE_ENABLE
                                             : OSPI_DADTR_MODE_DISABLE;
    command->dummy_cycles = frame->dummy_cycles;
    command->dqs_mode = frame->dqs_enable ? OSPI_DQS_ENABLE
                                          : OSPI_DQS_DISABLE;
    command->sioo_mode = OSPI_SIOO_INST_EVERY_CMD;
}

gd32h77x_status_t gd32h77x_ospi_init(gd32h77x_ospi_t *ospi,
                                     const gd32h77x_ospi_config_t *config,
                                     const gd32h77x_ospi_timing_t *timing)
{
    if (ospi == NULL || config == NULL || timing == NULL ||
        config->ospim_port > 1U ||
        __device_size(config->capacity_bytes) == UINT32_MAX ||
        !__pin_valid(&config->cs) || !__pin_valid(&config->sck)) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    uint32_t io_count = config->octal ? 8U : 4U;
    for (uint32_t index = 0U; index < io_count; ++index) {
        if (!__pin_valid(&config->io[index])) {
            return GD32H77X_STATUS_INVALID_PARAM;
        }
    }
    if (config->octal && !__pin_valid(&config->dqs)) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    if (config->reset_enabled && !__gpio_pin_valid(&config->reset)) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    if (ospi->initialized) {
        return GD32H77X_STATUS_OK;
    }

    memset(ospi, 0, sizeof(*ospi));
    ospi->periph = OSPI0;
    ospi->ospim_port = config->ospim_port;
    ospi->capacity_bytes = config->capacity_bytes;
    ospi->reset_enabled = config->reset_enabled;
    ospi->memory_type = timing->memory_type;
    ospi->delay_hold_cycle = timing->delay_hold_cycle;
    if (config->reset_enabled) {
        ospi->reset_port = config->reset.port;
        ospi->reset_pin = config->reset.pin;
    }
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

    uint32_t prescaler = __prescaler(config->clock_hz);
    if (prescaler == UINT32_MAX) {
        vSemaphoreDelete((SemaphoreHandle_t)ospi->lock);
        ospi->lock = NULL;
        return GD32H77X_STATUS_INVALID_PARAM;
    }

    ospi_parameter_struct parameters;
    ospi_struct_init(&parameters);
    parameters.prescaler = prescaler;
    parameters.sample_shift = timing->sample_shift;
    parameters.fifo_threshold = timing->fifo_threshold;
    parameters.device_size = timing->device_size != 0U
                                 ? timing->device_size
                                 : __device_size(config->capacity_bytes);
    parameters.cs_hightime = timing->cs_high_time;
    parameters.memory_type = timing->memory_type;
    parameters.delay_hold_cycle = timing->delay_hold_cycle;
    parameters.sample_delay = timing->sample_delay;
    parameters.refrate = timing->refrate;
    ospi_init(OSPI0, &parameters);
    ospi_enable(OSPI0);

    if (timing->dqs_rck_add_enable) {
        ospi_dqs_rck_add_enable(OSPI0);
    }
    if (timing->receive_delay_block_enable) {
        ospi_receive_delay_block_enable(OSPI0);
    }
    if (timing->tck_delay_tap != 0U) {
        ospi_tck_delay_chain_config(OSPI0, timing->tck_delay_tap);
        ospi_tck_delay_chain_enable(OSPI0);
    }
    if (timing->rck_delay_tap != 0U) {
        ospi_rck_delay_chain_config(OSPI0, timing->rck_delay_tap);
    }
    if (timing->receive_sck_delay_chain_select) {
        ospi_receive_sck_delay_chain_select(OSPI0);
    }

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
    uint32_t prescaler = __prescaler(clock_hz);
    if (prescaler == UINT32_MAX) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    if (__lock(ospi) != 0) {
        return GD32H77X_STATUS_IO_ERROR;
    }
    gd32h77x_status_t rt = gd32h77x_ospi_wait_ready(ospi);
    if (rt == GD32H77X_STATUS_OK) {
        ospi_prescaler_config(ospi->periph, prescaler);
        __DSB();
    }
    __unlock(ospi);
    return rt;
}

gd32h77x_status_t gd32h77x_ospi_wait_ready(gd32h77x_ospi_t *ospi)
{
    if (ospi == NULL || !ospi->initialized) {
        return GD32H77X_STATUS_NOT_READY;
    }

    for (uint32_t count = 0U; count < GD32H77X_OSPI_POLL_LIMIT; ++count) {
        if (SET == ospi_flag_get(ospi->periph, OSPI_FLAG_TERR)) {
            return GD32H77X_STATUS_IO_ERROR;
        }
        if (SET == ospi_flag_get(ospi->periph, OSPI_FLAG_TMOUT)) {
            return GD32H77X_STATUS_TIMEOUT;
        }
        if (ospi->memory_mapped ||
            RESET == ospi_flag_get(ospi->periph, OSPI_FLAG_BUSY)) {
            return GD32H77X_STATUS_OK;
        }
    }
    return GD32H77X_STATUS_TIMEOUT;
}

gd32h77x_status_t gd32h77x_ospi_reset_device(gd32h77x_ospi_t *ospi,
                                            uint32_t low_ms,
                                            uint32_t high_ms)
{
    if (ospi == NULL || !ospi->initialized || !ospi->reset_enabled) {
        return GD32H77X_STATUS_UNSUPPORTED;
    }

    gpio_bit_reset(ospi->reset_port, ospi->reset_pin);
    if (low_ms != 0U) {
        vTaskDelay(pdMS_TO_TICKS(low_ms));
    }
    gpio_bit_set(ospi->reset_port, ospi->reset_pin);
    if (high_ms != 0U) {
        vTaskDelay(pdMS_TO_TICKS(high_ms));
    }
    return GD32H77X_STATUS_OK;
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
    gd32h77x_status_t rt = gd32h77x_ospi_wait_ready(ospi);
    if (rt != GD32H77X_STATUS_OK) {
        __abort_locked(ospi);
        __unlock(ospi);
        return rt;
    }

    __clear_status(ospi);
    ospi_parameter_struct parameters;
    ospi_regular_cmd_struct command;
    __command_parameters(ospi, &parameters);
    __frame_to_command(&transfer->frame, OSPI_OPTYPE_COMMON_CFG,
                       transfer->address, transfer->has_address,
                       transfer->size, &command);
    ospi_command_config(ospi->periph, &parameters, &command);

    if (transfer->read) {
        if (transfer->size != 0U) {
            ospi_receive(ospi->periph, &parameters, transfer->data);
            if (transfer->frame.dqs_enable &&
                SET == ospi_flag_get(ospi->periph, OSPI_FLAG_BUSY)) {
                ospi_transmission_abort(ospi->periph);
            }
        }
    } else if (transfer->size != 0U) {
        ospi_transmit(ospi->periph, transfer->data);
    }
    __clear_status(ospi);
    __unlock(ospi);
    return GD32H77X_STATUS_OK;
}

gd32h77x_status_t gd32h77x_ospi_enter_memory_mapped(
    gd32h77x_ospi_t *ospi, const gd32h77x_ospi_memory_mapped_t *config)
{
    if (ospi == NULL || config == NULL || !ospi->initialized ||
        config->read_frame.instruction == 0U ||
        config->write_frame.instruction == 0U) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    if (__lock(ospi) != 0) {
        return GD32H77X_STATUS_IO_ERROR;
    }
    gd32h77x_status_t rt = gd32h77x_ospi_wait_ready(ospi);
    if (rt != GD32H77X_STATUS_OK) {
        __abort_locked(ospi);
        __unlock(ospi);
        return rt;
    }

    ospi_parameter_struct parameters;
    ospi_regular_cmd_struct command;
    __command_parameters(ospi, &parameters);
    __clear_status(ospi);

    __frame_to_command(&config->read_frame, OSPI_OPTYPE_READ_CFG, 0U, false,
                       0U, &command);
    ospi_command_config(ospi->periph, &parameters, &command);

    __frame_to_command(&config->write_frame, OSPI_OPTYPE_WRITE_CFG, 0U, false,
                       0U, &command);
    ospi_command_config(ospi->periph, &parameters, &command);

    if (config->refresh_cycles != 0U) {
        ospi_refresh_config(ospi->periph, config->refresh_cycles);
    }
    if (config->timeout_cycles != 0U) {
        ospi_timeout_cycle_config(ospi->periph,
                                  (uint16_t)config->timeout_cycles);
        ospi_timeout_enable(ospi->periph);
    }

    ospi_memorymapped_struct memory_mapped = {
        .timeout_activation = OSPI_TIMEOUT_COUNT_DISABLE,
        .timeout_period = 0U,
    };
    ospi_memorymapped_mode(ospi->periph, &memory_mapped);
    if (config->timeout_cycles != 0U) {
        NVIC_SetPriority(OSPI0_IRQn, 0U);
        NVIC_ClearPendingIRQ(OSPI0_IRQn);
        NVIC_EnableIRQ(OSPI0_IRQn);
        ospi_interrupt_enable(ospi->periph, OSPI_INT_TMOUT);
    }
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
