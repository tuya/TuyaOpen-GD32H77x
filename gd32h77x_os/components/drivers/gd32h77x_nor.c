/**
 * @file gd32h77x_nor.c
 * @brief GD32H77x indirect OSPI access for the GD25Q64E profile.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "gd32h77x_nor.h"

#include <stddef.h>
#include <string.h>

#define GD32H77X_NOR_CAPACITY       UINT32_C(0x00800000)
#define GD32H77X_NOR_PAGE_BYTES     UINT32_C(256)
#define GD32H77X_NOR_ERASE_BYTES    UINT32_C(4096)
#define GD32H77X_NOR_EXPECTED_ID    UINT32_C(0x00C84017)
#define GD32H77X_NOR_CMD_READ_ID    UINT8_C(0x9F)
#define GD32H77X_NOR_CMD_READ       UINT8_C(0x03)
#define GD32H77X_NOR_CMD_READ_SR1   UINT8_C(0x05)
#define GD32H77X_NOR_CMD_READ_SR2   UINT8_C(0x35)
#define GD32H77X_NOR_CMD_WREN       UINT8_C(0x06)
#define GD32H77X_NOR_CMD_PROGRAM    UINT8_C(0x02)
#define GD32H77X_NOR_CMD_ERASE_4K   UINT8_C(0x20)
#define GD32H77X_NOR_STATUS_WIP     UINT8_C(0x01)
#define GD32H77X_NOR_STATUS_WEL     UINT8_C(0x02)
#define GD32H77X_NOR_STATUS_BP_MASK UINT8_C(0x7C)
#define GD32H77X_NOR_STATUS2_LOCK_MASK UINT8_C(0xC0)
#define GD32H77X_NOR_POLL_LIMIT     UINT32_C(100000)
#define GD32H77X_NOR_MAX_READ       UINT32_C(256)
#define GD32H77X_NOR_MAX_PROGRAM    UINT32_C(256)

typedef struct {
    const char *pin;
    const char *signal;
} gd32h77x_nor_pinmux_name_t;

/* Chip-level GD32H77x routes. Board layout supplies the selected pin labels. */
static const gd32h77x_nor_pinmux_name_t s_pinmux[] = {
    {"PC11", "OSPI0_CSN"}, {"PF10", "OSPI0_SCK"},
    {"PF8", "OSPI0_IO0"}, {"PF9", "OSPI0_IO1"},
    {"PF7", "OSPI0_IO2"}, {"PA1", "OSPI0_IO3"},
};

static const char *const s_signal_names[] = {
    "OSPI0_CSN", "OSPI0_SCK", "OSPI0_IO0", "OSPI0_IO1", "OSPI0_IO2", "OSPI0_IO3"
};

static uint8_t s_ready[GD32H77X_LAYOUT_DEVICE_COUNT];
static uint32_t s_jedec_id[GD32H77X_LAYOUT_DEVICE_COUNT];

#ifdef GD32H77X_NOR_HOST_TEST
extern int gd32h77x_nor_host_configure(const gd32h77x_layout_binding_t *binding);
extern int gd32h77x_nor_host_transfer(uint8_t instruction, uint32_t address,
                                      int has_address, uint8_t *data, uint32_t size);
extern int gd32h77x_nor_host_write(uint8_t instruction, uint32_t address,
                                   int has_address, const uint8_t *data, uint32_t size);
#define nor_hardware_configure(binding) gd32h77x_nor_host_configure(binding)
#define nor_hardware_transfer(instruction, address, has_address, data, size) \
    gd32h77x_nor_host_transfer(instruction, address, has_address, data, size)
#define nor_hardware_write(instruction, address, has_address, data, size) \
    gd32h77x_nor_host_write(instruction, address, has_address, data, size)
#else
#include "gd32h77x_78x_gpio.h"
#include "gd32h77x_78x_ospi.h"
#include "gd32h77x_78x_ospim.h"
#include "gd32h77x_78x_rcu.h"
#include "gd32h77x_78x_syscfg.h"
#include "FreeRTOS.h"
#include "semphr.h"

static SemaphoreHandle_t s_ospi_mutex;

typedef struct {
    uint32_t port;
    uint32_t pin;
    rcu_periph_enum clock;
    uint32_t alternate;
} gd32h77x_nor_pinmux_t;

static const gd32h77x_nor_pinmux_t s_pinmux_values[] = {
    {GPIOC, GPIO_PIN_11, RCU_GPIOC, GPIO_AF_9},
    {GPIOF, GPIO_PIN_10, RCU_GPIOF, GPIO_AF_9},
    {GPIOF, GPIO_PIN_8, RCU_GPIOF, GPIO_AF_10},
    {GPIOF, GPIO_PIN_9, RCU_GPIOF, GPIO_AF_10},
    {GPIOF, GPIO_PIN_7, RCU_GPIOF, GPIO_AF_10},
    {GPIOA, GPIO_PIN_1, RCU_GPIOA, GPIO_AF_9},
};

static int nor_lock_init(void)
{
    if (s_ospi_mutex != NULL) {
        return 0;
    }
    s_ospi_mutex = xSemaphoreCreateMutex();
    return s_ospi_mutex != NULL ? 0 : -1;
}

static int nor_lock(void)
{
    return s_ospi_mutex != NULL &&
                   xSemaphoreTake(s_ospi_mutex, portMAX_DELAY) == pdTRUE ? 0 : -1;
}

static void nor_unlock(void)
{
    (void)xSemaphoreGive(s_ospi_mutex);
}

static int nor_wait_flag(uint32_t flag, int set)
{
    uint32_t count;
    for (count = 0; count < GD32H77X_NOR_POLL_LIMIT; ++count) {
        const int is_set = (OSPI_STAT(OSPI0) & flag) != 0U;
        if (is_set == set) {
            return 0;
        }
        if ((OSPI_STAT(OSPI0) & OSPI_FLAG_TERR) != 0U) {
            return -1;
        }
    }
    return -1;
}

static void nor_abort(void)
{
    uint32_t count;
    OSPI_CTL(OSPI0) |= OSPI_CTL_ABORT;
    for (count = 0; count < GD32H77X_NOR_POLL_LIMIT; ++count) {
        if ((OSPI_STAT(OSPI0) & OSPI_FLAG_BUSY) == 0U) {
            break;
        }
    }
    OSPI_STATC(OSPI0) = OSPI_STATC_TCC | OSPI_STATC_TERRC;
}

static int nor_hardware_configure(const gd32h77x_layout_binding_t *binding)
{
    ospi_parameter_struct parameters;
    uint32_t source_hz;
    uint32_t divider;
    uint32_t index;

    if (binding == NULL || nor_lock_init() != 0 || nor_lock() != 0) {
        return -1;
    }
    rcu_ospi_clock_config(RCU_OSPI_AHB);
    rcu_periph_clock_enable(RCU_OSPIM);
    rcu_periph_clock_enable(RCU_OSPI0);
    ospim_deinit();
    ospi_deinit(OSPI0);

    for (index = 0; index < sizeof(s_pinmux_values) / sizeof(s_pinmux_values[0]); ++index) {
        rcu_periph_clock_enable(s_pinmux_values[index].clock);
        gpio_af_set(s_pinmux_values[index].port, s_pinmux_values[index].alternate,
                    s_pinmux_values[index].pin);
        gpio_mode_set(s_pinmux_values[index].port, GPIO_MODE_AF, GPIO_PUPD_NONE,
                      s_pinmux_values[index].pin);
        gpio_output_options_set(s_pinmux_values[index].port, GPIO_OTYPE_PP,
                                GPIO_OSPEED_100_220MHZ, s_pinmux_values[index].pin);
    }

    ospim_port_sck_config(OSPIM_PORT0, OSPIM_PORT_SCK_ENABLE);
    ospim_port_dqs_config(OSPIM_PORT0, OSPIM_PORT_DQS_DISABLE);
    ospim_port_csn_config(OSPIM_PORT0, OSPIM_PORT_CSN_ENABLE);
    ospim_port_io3_0_config(OSPIM_PORT0, OSPIM_IO_LOW_ENABLE);
    ospim_port_io7_4_config(OSPIM_PORT0, OSPIM_IO_HIGH_DISABLE);
    ospim_port_sck_source_select(OSPIM_PORT0, OSPIM_SCK_SOURCE_OSPI0_SCK);
    ospim_port_csn_source_select(OSPIM_PORT0, OSPIM_CSN_SOURCE_OSPI0_CSN);
    ospim_port_io3_0_source_select(OSPIM_PORT0, OSPIM_SRCPLIO_OSPI0_IO_LOW);

    source_hz = rcu_clock_freq_get(CK_AHB);
    if (source_hz == 0U) {
        nor_unlock();
        return -1;
    }
    divider = (source_hz + UINT32_C(99999999)) / UINT32_C(100000000);
    if (divider == 0U) {
        divider = 1U;
    }
    if (divider > 256U || nor_wait_flag(OSPI_FLAG_BUSY, 0) != 0) {
        nor_unlock();
        return -1;
    }

    ospi_struct_init(&parameters);
    parameters.prescaler = divider - 1U;
    parameters.sample_shift = OSPI_SAMPLE_SHIFTING_HALF_CYCLE;
    parameters.fifo_threshold = OSPI_FIFO_THRESHOLD_16;
    parameters.device_size = OSPI_MESZ_8_MBS;
    parameters.cs_hightime = OSPI_CS_HIGH_TIME_3_CYCLE;
    parameters.memory_type = OSPI_STANDARD_MODE;
    parameters.delay_hold_cycle = OSPI_DELAY_HOLD_NONE;
    parameters.dual_quad = OSPI_DUAL_QUAD_MODE_DISABLE;
    parameters.free_clock_run = OSPI_CLOCK_FREE_RUN_DISABLE;
    parameters.sample_delay = OSPI_SAMPLE_DELAY_DISABLE;
    parameters.refrate = 0U;
    ospi_init(OSPI0, &parameters);
    ospi_enable(OSPI0);
    nor_unlock();
    return 0;
}

static int nor_hardware_transfer(uint8_t instruction, uint32_t address,
                                int has_address, uint8_t *data, uint32_t size)
{
    ospi_parameter_struct parameters;
    ospi_regular_cmd_struct command;
    uint32_t index;
    uint32_t address_mode = has_address ? OSPI_ADDRESS_1_LINE : OSPI_ADDRESS_NONE;

    if (data == NULL || size == 0U || size > GD32H77X_NOR_MAX_READ || nor_lock() != 0) {
        return -1;
    }
    if (nor_wait_flag(OSPI_FLAG_BUSY, 0) != 0) {
        nor_unlock();
        return -1;
    }

    memset(&command, 0, sizeof(command));
    ospi_struct_init(&parameters);
    command.operation_type = OSPI_OPTYPE_COMMON_CFG;
    command.device_id = OSPI_DEVICE0_SELECT;
    command.instruction = instruction;
    command.ins_mode = OSPI_INSTRUCTION_1_LINE;
    command.ins_size = OSPI_INSTRUCTION_8_BITS;
    command.addr_mode = address_mode;
    command.addr_size = has_address ? OSPI_ADDRESS_24_BITS : OSPI_ADDRESS_8_BITS;
    command.address = address;
    command.data_mode = OSPI_DATA_1_LINE;
    command.nbdata = size;
    command.dummy_cycles = 0U;
    command.dqs_mode = OSPI_DQS_DISABLE;
    command.sioo_mode = OSPI_SIOO_INST_EVERY_CMD;
    ospi_command_config(OSPI0, &parameters, &command);

    OSPI_CTL(OSPI0) = (OSPI_CTL(OSPI0) & ~OSPI_CTL_FMOD) | OSPI_INDIRECT_READ;
    if (has_address) {
        OSPI_ADDR(OSPI0) = address;
    } else {
        OSPI_INS(OSPI0) = instruction;
    }

    for (index = 0; index < size; ++index) {
        if (nor_wait_flag(OSPI_FLAG_FT | OSPI_FLAG_TC, 1) != 0) {
            nor_abort();
            nor_unlock();
            return -1;
        }
        data[index] = *((__IO uint8_t *)&OSPI_DATA(OSPI0));
    }
    if (nor_wait_flag(OSPI_FLAG_TC, 1) != 0) {
        nor_abort();
        nor_unlock();
        return -1;
    }
    OSPI_STATC(OSPI0) = OSPI_STATC_TCC;
    nor_unlock();
    return 0;
}

/*
 * Keep the transmit path local instead of calling ospi_transmit(): the vendor
 * helper has unbounded FIFO/TC waits. Command setup mirrors the vendor driver's
 * 1-line TCFG programming, while all waits here are explicitly bounded.
 */
static int nor_hardware_write(uint8_t instruction, uint32_t address,
                              int has_address, const uint8_t *data, uint32_t size)
{
    const uint32_t tcfg_mask = OSPI_TCFG_IMOD | OSPI_TCFG_INSDTR | OSPI_TCFG_INSSZ |
                               OSPI_TCFG_ADDRMOD | OSPI_TCFG_ADDRDTR | OSPI_TCFG_ADDRSZ |
                               OSPI_TCFG_ALTEMOD | OSPI_TCFG_ABDTR | OSPI_TCFG_ALTESZ |
                               OSPI_TCFG_DATAMOD | OSPI_TCFG_DADTR | OSPI_TCFG_DQSEN |
                               OSPI_TCFG_SIOO;
    uint32_t tcfg;
    uint32_t index;

    if (size > GD32H77X_NOR_MAX_PROGRAM || (size != 0U && data == NULL) ||
        nor_lock() != 0) {
        return -1;
    }
    if (nor_wait_flag(OSPI_FLAG_BUSY, 0) != 0) {
        nor_unlock();
        return -1;
    }

    tcfg = OSPI_INSTRUCTION_1_LINE | OSPI_INSTRUCTION_8_BITS | OSPI_SIOO_INST_EVERY_CMD;
    if (has_address) {
        tcfg |= OSPI_ADDRESS_1_LINE | OSPI_ADDRESS_24_BITS;
    }
    if (size != 0U) {
        tcfg |= OSPI_DATA_1_LINE;
    }
    OSPI_CTL(OSPI0) = (OSPI_CTL(OSPI0) & ~OSPI_CTL_DEVS) | OSPI_DEVICE0_SELECT;
    OSPI_TIMCFG(OSPI0) &= ~OSPI_TIMCFG_DUMYC;
    OSPI_DTLEN(OSPI0) = size == 0U ? 0U : size - 1U;
    OSPI_TCFG(OSPI0) = (OSPI_TCFG(OSPI0) & ~tcfg_mask) | tcfg;

    OSPI_CTL(OSPI0) = (OSPI_CTL(OSPI0) & ~OSPI_CTL_FMOD) | OSPI_INDIRECT_WRITE;
    OSPI_INS(OSPI0) = instruction;
    if (has_address) {
        OSPI_ADDR(OSPI0) = address;
    }
    if (size != 0U) {
        for (index = 0; index < size; ++index) {
            if (nor_wait_flag(OSPI_FLAG_FT, 1) != 0) {
                nor_abort();
                nor_unlock();
                return -1;
            }
            *((__IO uint8_t *)&OSPI_DATA(OSPI0)) = data[index];
        }
    }
    if (nor_wait_flag(OSPI_FLAG_TC, 1) != 0) {
        nor_abort();
        nor_unlock();
        return -1;
    }
    OSPI_STATC(OSPI0) = OSPI_STATC_TCC | OSPI_STATC_TERRC;
    nor_unlock();
    return 0;
}

#endif

static int binding_pin_matches(const char *pin, const char *signal)
{
    size_t index;
    if (pin == NULL || signal == NULL) {
        return 0;
    }
    for (index = 0; index < sizeof(s_pinmux) / sizeof(s_pinmux[0]); ++index) {
        if (strcmp(pin, s_pinmux[index].pin) == 0 && strcmp(signal, s_pinmux[index].signal) == 0) {
            return 1;
        }
    }
    return 0;
}

int gd32h77x_nor_binding_valid(const gd32h77x_layout_device_t *device,
                               const gd32h77x_layout_binding_t *binding)
{
    uint32_t index;
    if (device == NULL || binding == NULL || device->geometry_id == NULL ||
        device->region == NULL || binding->controller == NULL ||
        binding->chip_select == NULL || binding->clock == NULL ||
        strcmp(device->geometry_id, "GD25Q64ESIG") != 0 || strcmp(device->region, "NOR") != 0 ||
        device->access != GD32H77X_LAYOUT_ACCESS_INDIRECT ||
        device->capacity_bytes != GD32H77X_NOR_CAPACITY ||
        device->erase_bytes != GD32H77X_NOR_ERASE_BYTES ||
        device->page_bytes != GD32H77X_NOR_PAGE_BYTES ||
        strcmp(binding->controller, "OSPI0") != 0 ||
        !binding_pin_matches(binding->chip_select, s_signal_names[0]) ||
        !binding_pin_matches(binding->clock, s_signal_names[1])) {
        return 0;
    }
    for (index = 0; index < 4U; ++index) {
        if (!binding_pin_matches(binding->data[index], s_signal_names[index + 2U])) {
            return 0;
        }
    }
    return 1;
}

static int nor_read_status(uint8_t *status)
{
    return nor_hardware_transfer(GD32H77X_NOR_CMD_READ_SR1, 0U, 0, status, 1U);
}

static int nor_read_status2(uint8_t *status)
{
    return nor_hardware_transfer(GD32H77X_NOR_CMD_READ_SR2, 0U, 0, status, 1U);
}

static int nor_wait_ready(void)
{
    uint32_t count;
    uint8_t status;
    for (count = 0; count < GD32H77X_NOR_POLL_LIMIT; ++count) {
        if (nor_read_status(&status) != 0) {
            return -1;
        }
        if ((status & GD32H77X_NOR_STATUS_WIP) == 0U) {
            return 0;
        }
    }
    return -1;
}

static int nor_check_write_unprotected(void)
{
    uint8_t sr1;
    uint8_t sr2;

    if (nor_read_status(&sr1) != 0 || nor_read_status2(&sr2) != 0 ||
        (sr1 & (GD32H77X_NOR_STATUS_WIP | GD32H77X_NOR_STATUS_BP_MASK)) != 0U ||
        (sr2 & GD32H77X_NOR_STATUS2_LOCK_MASK) != 0U) {
        return -1;
    }
    return 0;
}

static int nor_write_enable(void)
{
    uint32_t count;
    uint8_t sr1;

    if (nor_hardware_write(GD32H77X_NOR_CMD_WREN, 0U, 0, NULL, 0U) != 0) {
        return -1;
    }
    for (count = 0; count < GD32H77X_NOR_POLL_LIMIT; ++count) {
        if (nor_read_status(&sr1) != 0) {
            return -1;
        }
        if ((sr1 & GD32H77X_NOR_STATUS_WEL) != 0U) {
            return 0;
        }
    }
    return -1;
}

int gd32h77x_nor_init(uint32_t device_index)
{
    uint8_t id[3];
    uint32_t actual_id;
    if (device_index >= GD32H77X_LAYOUT_DEVICE_COUNT ||
        !gd32h77x_nor_binding_valid(&gd32h77x_layout_devices[device_index],
                                    &gd32h77x_layout_bindings[device_index])) {
        return -1;
    }
    if (s_ready[device_index] != 0U) {
        return 0;
    }
    if (nor_hardware_configure(&gd32h77x_layout_bindings[device_index]) != 0 ||
        nor_hardware_transfer(GD32H77X_NOR_CMD_READ_ID, 0U, 0, id, sizeof(id)) != 0) {
        return -1;
    }
    actual_id = ((uint32_t)id[0] << 16U) | ((uint32_t)id[1] << 8U) | id[2];
    if (actual_id != GD32H77X_NOR_EXPECTED_ID) {
        return -1;
    }
    s_jedec_id[device_index] = actual_id;
    s_ready[device_index] = 1U;
    return 0;
}

int gd32h77x_nor_get_jedec_id(uint32_t device_index, uint32_t *jedec_id)
{
    if (jedec_id == NULL || device_index >= GD32H77X_LAYOUT_DEVICE_COUNT ||
        gd32h77x_nor_init(device_index) != 0) {
        return -1;
    }
    *jedec_id = s_jedec_id[device_index];
    return 0;
}

int gd32h77x_nor_read(uint32_t device_index, uint32_t offset,
                      void *destination, uint32_t size)
{
    uint8_t *out = destination;
    if (device_index >= GD32H77X_LAYOUT_DEVICE_COUNT ||
        (size != 0U && destination == NULL) || offset > GD32H77X_NOR_CAPACITY ||
        size > GD32H77X_NOR_CAPACITY - offset) {
        return -1;
    }
    if (size == 0U) {
        return 0;
    }
    if (gd32h77x_nor_init(device_index) != 0) {
        return -1;
    }
    while (size != 0U) {
        const uint32_t chunk = size > GD32H77X_NOR_MAX_READ ? GD32H77X_NOR_MAX_READ : size;
        if (nor_wait_ready() != 0 ||
            nor_hardware_transfer(GD32H77X_NOR_CMD_READ, offset, 1, out, chunk) != 0) {
            return -1;
        }
        offset += chunk;
        out += chunk;
        size -= chunk;
    }
    return 0;
}

int gd32h77x_nor_program(uint32_t device_index, uint32_t offset,
                         const void *source, uint32_t size)
{
    const uint8_t *input = source;
    if (device_index >= GD32H77X_LAYOUT_DEVICE_COUNT ||
        (size != 0U && source == NULL) || offset > GD32H77X_NOR_CAPACITY ||
        size > GD32H77X_NOR_CAPACITY - offset) {
        return -1;
    }
    if (gd32h77x_nor_init(device_index) != 0) {
        return -1;
    }
    while (size != 0U) {
        const uint32_t page_remaining = GD32H77X_NOR_PAGE_BYTES -
                                        (offset % GD32H77X_NOR_PAGE_BYTES);
        const uint32_t chunk = size < page_remaining ? size : page_remaining;
        if (nor_wait_ready() != 0 || nor_check_write_unprotected() != 0 ||
            nor_write_enable() != 0 ||
            nor_hardware_write(GD32H77X_NOR_CMD_PROGRAM, offset, 1, input, chunk) != 0 ||
            nor_wait_ready() != 0) {
            return -1;
        }
        offset += chunk;
        input += chunk;
        size -= chunk;
    }
    return 0;
}

int gd32h77x_nor_erase_4k(uint32_t device_index, uint32_t offset)
{
    if (device_index >= GD32H77X_LAYOUT_DEVICE_COUNT ||
        offset >= GD32H77X_NOR_CAPACITY || (offset % GD32H77X_NOR_ERASE_BYTES) != 0U ||
        gd32h77x_nor_init(device_index) != 0 || nor_wait_ready() != 0 ||
        nor_check_write_unprotected() != 0 || nor_write_enable() != 0 ||
        nor_hardware_write(GD32H77X_NOR_CMD_ERASE_4K, offset, 1, NULL, 0U) != 0 ||
        nor_wait_ready() != 0) {
        return -1;
    }
    return 0;
}
