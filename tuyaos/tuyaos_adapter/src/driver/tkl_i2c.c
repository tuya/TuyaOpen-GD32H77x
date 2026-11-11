/**
 * @file tkl_i2c.c
 * @brief GD32H77X I2C master adapter.
 *
 * Default routing is owned by this driver.  tkl_pinmux only updates the
 * selected entries; tkl_i2c_init() applies the resulting table directly.
 * This adapter implements the blocking master subset required by the common
 * TDD TP layer. Interrupt and slave modes intentionally remain unsupported
 * until a board needs them.
 *
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "tkl_i2c.h"
#include "tal_api.h"

#ifdef BIT
#undef BIT
#endif

#include "gd32h77x_78x.h"
#include "gd32h77x_78x_i2c.h"
#include "tkl_system.h"
#include "tuya_error_code.h"

#define GD32H77X_I2C_TIMEOUT_MS          20U

typedef struct {
    uint32_t port;
    uint32_t pin;
    uint32_t af;
} I2C_PIN_T;

typedef struct {
    I2C_PIN_T scl;
    I2C_PIN_T sda;
} I2C_PIN_CFG_T;

typedef struct {
    uint32_t periph;
    uint8_t initialized;
    uint8_t pending;
    uint32_t speed;
    uint32_t transfer_count;
} GD32H77X_I2C_STATE;

#define I2C_PIN(port_name, pin_num, af_val) \
    {port_name, GPIO_PIN_##pin_num, af_val}

static const uint32_t s_i2c_periph[TUYA_I2C_NUM_MAX] = {
    I2C0, I2C1, I2C2, I2C3, 0U, 0U,
};
static const uint32_t s_i2c_rcu[TUYA_I2C_NUM_MAX] = {
    RCU_I2C0, RCU_I2C1, RCU_I2C2, RCU_I2C3, 0U, 0U,
};
static const uint8_t s_i2c_rcu_idx[TUYA_I2C_NUM_MAX] = {
    IDX_I2C0, IDX_I2C1, IDX_I2C2, IDX_I2C3, 0U, 0U,
};
static GD32H77X_I2C_STATE s_i2c[TUYA_I2C_NUM_MAX];

/* Default routing for each I2C controller.  Every entry records the physical
 * port/pin/AF used by tkl_i2c_init(); the comments also carry the matching
 * TUYA_GPIO_NUM so pinmux overrides remain self-documenting.
 */
static I2C_PIN_CFG_T s_i2c_pins[TUYA_I2C_NUM_MAX] = {
    [TUYA_I2C_NUM_0] = {
        .scl = I2C_PIN(GPIOB, 8, 4),  /* PB8 / TUYA_GPIO_NUM_24 / AF4 */
        .sda = I2C_PIN(GPIOB, 9, 4),  /* PB9 / TUYA_GPIO_NUM_25 / AF4 */
    },
    [TUYA_I2C_NUM_1] = {
        .scl = I2C_PIN(GPIOF, 1, 4),  /* PF1 / TUYA_GPIO_NUM_81 / AF4 */
        .sda = I2C_PIN(GPIOF, 0, 4),  /* PF0 / TUYA_GPIO_NUM_80 / AF4 */
    },
    [TUYA_I2C_NUM_2] = {
        .scl = I2C_PIN(GPIOH, 7, 4),  /* PH7 / TUYA_GPIO_NUM_119 / AF4 */
        .sda = I2C_PIN(GPIOH, 8, 4),  /* PH8 / TUYA_GPIO_NUM_120 / AF4 */
    },
    [TUYA_I2C_NUM_3] = {
        .scl = I2C_PIN(GPIOD, 12, 4), /* PD12 / TUYA_GPIO_NUM_60 / AF4 */
        .sda = I2C_PIN(GPIOD, 13, 4), /* PD13 / TUYA_GPIO_NUM_61 / AF4 */
    },
};

void __tkl_i2c_set_pin(TUYA_I2C_NUM_E port, TUYA_PIN_FUNC_E func,
                       uint32_t gpio_port, uint32_t gpio_pin, uint32_t af)
{
    uint32_t controller;
    uint32_t channel;
    I2C_PIN_T *pin_cfg;

    if (func > TUYA_IIC5_SDA || port >= TUYA_I2C_NUM_MAX) {
        return;
    }
    controller = func / 2U;
    channel = func % 2U;
    if (controller != (uint32_t)port || s_i2c_periph[port] == 0U) {
        return;
    }

    pin_cfg = channel == 0U ? &s_i2c_pins[port].scl : &s_i2c_pins[port].sda;
    pin_cfg->port = gpio_port;
    pin_cfg->pin = gpio_pin;
    pin_cfg->af = af;
}

static uint32_t __i2c_periph(TUYA_I2C_NUM_E port)
{
    return port < TUYA_I2C_NUM_MAX ? s_i2c_periph[port] : 0U;
}

static bool __i2c_error_flag(uint32_t periph)
{
    return i2c_flag_get(periph, I2C_FLAG_NACK) == SET ||
           i2c_flag_get(periph, I2C_FLAG_BERR) == SET ||
           i2c_flag_get(periph, I2C_FLAG_LOSTARB) == SET ||
           i2c_flag_get(periph, I2C_FLAG_OUERR) == SET ||
           i2c_flag_get(periph, I2C_FLAG_TIMEOUT) == SET;
}

static void __i2c_clear_errors(uint32_t periph)
{
    i2c_flag_clear(periph, I2C_FLAG_NACK);
    i2c_flag_clear(periph, I2C_FLAG_BERR);
    i2c_flag_clear(periph, I2C_FLAG_LOSTARB);
    i2c_flag_clear(periph, I2C_FLAG_OUERR);
    i2c_flag_clear(periph, I2C_FLAG_TIMEOUT);
}

static void __i2c_configure_peripheral(TUYA_I2C_NUM_E port)
{
    uint32_t periph = s_i2c[port].periph;
    uint32_t kernel = rcu_irc64mdiv_freq_get();
    uint32_t prescaler = (kernel + 3999999U) / 4000000U;
    uint32_t scl_high = 15U;
    uint32_t scl_low = 23U;
    uint32_t scl_delay = 4U;
    uint32_t sda_delay = 1U;

    if (s_i2c[port].speed == TUYA_IIC_BUS_SPEED_400K) {
        scl_high = 4U;
        scl_low = 5U;
        scl_delay = 1U;
        sda_delay = 0U;
    }
    if (prescaler == 0U) {
        prescaler = 1U;
    }
    i2c_deinit(periph);
    i2c_timing_config(periph, prescaler - 1U, scl_delay, sda_delay);
    i2c_master_clock_config(periph, scl_high, scl_low);
    i2c_analog_noise_filter_enable(periph);
    i2c_reload_disable(periph);
    i2c_enable(periph);
    PR_NOTICE("i2c%u cfg periph=0x%08X speed=%u kernel=%u psc=%u timing=0x%08X cfg3=0x%08X",
              (unsigned)port, (unsigned)periph, (unsigned)s_i2c[port].speed,
              (unsigned)kernel, (unsigned)(prescaler - 1U),
              (unsigned)I2C_TIMING(periph), (unsigned)RCU_CFG3);
    __i2c_clear_errors(periph);
    i2c_flag_clear(periph, I2C_FLAG_STPDET);
    i2c_reload_disable(periph);
}

static OPERATE_RET __i2c_wait_flag(uint32_t periph, uint32_t flag)
{
    uint32_t start_ms = (uint32_t)tkl_system_get_millisecond();

    while (i2c_flag_get(periph, flag) == RESET) {
        if (__i2c_error_flag(periph)) {
            return OPRT_COM_ERROR;
        }
        if ((uint32_t)tkl_system_get_millisecond() - start_ms >=
            GD32H77X_I2C_TIMEOUT_MS) {
            PR_ERR("i2c wait flag timeout periph=0x%08X flag=0x%08X stat=0x%08X ctl0=0x%08X ctl1=0x%08X",
                   (unsigned)periph, (unsigned)flag, (unsigned)I2C_STAT(periph),
                   (unsigned)I2C_CTL0(periph), (unsigned)I2C_CTL1(periph));
            return OPRT_TIMEOUT;
        }
    }

    return __i2c_error_flag(periph) ? OPRT_COM_ERROR : OPRT_OK;
}

static OPERATE_RET __i2c_wait_idle(uint32_t periph)
{
    uint32_t start_ms = (uint32_t)tkl_system_get_millisecond();

    while (i2c_flag_get(periph, I2C_FLAG_I2CBSY) == SET) {
        if ((uint32_t)tkl_system_get_millisecond() - start_ms >=
            GD32H77X_I2C_TIMEOUT_MS) {
            return OPRT_TIMEOUT;
        }
    }

    return OPRT_OK;
}

static OPERATE_RET __i2c_finish(uint32_t periph, BOOL_T xfer_pending)
{
    OPERATE_RET rt;

    if (xfer_pending) {
        rt = __i2c_wait_flag(periph, I2C_FLAG_TC);
    } else {
        rt = __i2c_wait_flag(periph, I2C_FLAG_STPDET);
        if (rt == OPRT_OK) {
            i2c_flag_clear(periph, I2C_FLAG_STPDET);
        }
    }

    if (rt != OPRT_OK) {
        __i2c_clear_errors(periph);
    }

    return rt;
}

static uint32_t __i2c_gpio_rcu(uint32_t port)
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

static void __i2c_apply_pin(const I2C_PIN_T *pin)
{
    uint32_t rcu = __i2c_gpio_rcu(pin->port);

    if (rcu == 0U) {
        return;
    }
    rcu_periph_clock_enable(rcu);
    gpio_af_set(pin->port, pin->af, pin->pin);
    gpio_mode_set(pin->port, GPIO_MODE_AF, GPIO_PUPD_PULLUP, pin->pin);
    gpio_output_options_set(pin->port, GPIO_OTYPE_OD, GPIO_OSPEED_12MHZ,
                            pin->pin);
}

static void __i2c_gpio_config(TUYA_I2C_NUM_E port)
{
    const I2C_PIN_T *scl = &s_i2c_pins[port].scl;
    const I2C_PIN_T *sda = &s_i2c_pins[port].sda;

    rcu_periph_clock_enable(__i2c_gpio_rcu(scl->port));
    rcu_periph_clock_enable(__i2c_gpio_rcu(sda->port));
    rcu_i2c_clock_config((i2c_idx_enum)s_i2c_rcu_idx[port], RCU_I2CSRC_IRC64MDIV);
    rcu_periph_clock_enable(s_i2c_rcu[port]);

    gpio_af_set(scl->port, scl->af, scl->pin);
    gpio_af_set(sda->port, sda->af, sda->pin);
    gpio_mode_set(sda->port, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP, sda->pin);
    tkl_system_sleep(2U);
    PR_NOTICE("i2c%u pre-recovery bus raw=0x%08X scl=%u sda=%u",
              (unsigned)port, (unsigned)GPIO_ISTAT(scl->port),
              (unsigned)((GPIO_ISTAT(scl->port) & scl->pin) != 0U),
              (unsigned)((GPIO_ISTAT(sda->port) & sda->pin) != 0U));

    if ((GPIO_ISTAT(sda->port) & sda->pin) == 0U) {
        gpio_bit_set(scl->port, scl->pin);
        gpio_mode_set(scl->port, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, scl->pin);
        gpio_output_options_set(scl->port, GPIO_OTYPE_OD, GPIO_OSPEED_12MHZ,
                                scl->pin);

        for (uint32_t i = 0U; i < 9U; ++i) {
            gpio_bit_reset(scl->port, scl->pin);
            tkl_system_sleep(1U);
            gpio_bit_set(scl->port, scl->pin);
            tkl_system_sleep(1U);
        }

        gpio_bit_reset(scl->port, scl->pin);
        gpio_bit_reset(sda->port, sda->pin);
        gpio_mode_set(sda->port, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, sda->pin);
        gpio_output_options_set(sda->port, GPIO_OTYPE_OD, GPIO_OSPEED_12MHZ,
                                sda->pin);
        tkl_system_sleep(1U);
        gpio_bit_set(scl->port, scl->pin);
        tkl_system_sleep(1U);
        gpio_bit_set(sda->port, sda->pin);
        gpio_mode_set(sda->port, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP, sda->pin);
        tkl_system_sleep(2U);
    }

    gpio_mode_set(scl->port, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP, scl->pin);
    PR_NOTICE("i2c%u post-recovery bus scl=%u sda=%u",
              (unsigned)port,
              (unsigned)((GPIO_ISTAT(scl->port) & scl->pin) != 0U),
              (unsigned)((GPIO_ISTAT(sda->port) & sda->pin) != 0U));

    __i2c_apply_pin(scl);
    __i2c_apply_pin(sda);
}

OPERATE_RET tkl_i2c_init(TUYA_I2C_NUM_E port, const TUYA_IIC_BASE_CFG_T *cfg)
{
    uint32_t periph;

    if (port >= TUYA_I2C_NUM_MAX || cfg == NULL ||
        cfg->role != TUYA_IIC_MODE_MASTER ||
        cfg->addr_width != TUYA_IIC_ADDRESS_7BIT) {
        return OPRT_INVALID_PARM;
    }

    if (cfg->speed != TUYA_IIC_BUS_SPEED_100K &&
        cfg->speed != TUYA_IIC_BUS_SPEED_400K) {
        return OPRT_NOT_SUPPORTED;
    }

    periph = __i2c_periph(port);
    if (periph == 0U) {
        return OPRT_NOT_SUPPORTED;
    }

    if (s_i2c[port].initialized) {
        return OPRT_OK;
    }

    __i2c_gpio_config(port);

    s_i2c[port].periph = periph;
    s_i2c[port].speed = cfg->speed;
    __i2c_configure_peripheral(port);
    if (__i2c_wait_idle(periph) != OPRT_OK) {
        i2c_disable(periph);
        return OPRT_TIMEOUT;
    }

    s_i2c[port].initialized = 1U;
    s_i2c[port].pending = 0U;
    s_i2c[port].transfer_count = 0U;

    return OPRT_OK;
}

OPERATE_RET tkl_i2c_deinit(TUYA_I2C_NUM_E port)
{
    if (port >= TUYA_I2C_NUM_MAX) {
        return OPRT_INVALID_PARM;
    }

    if (s_i2c[port].initialized) {
        i2c_disable(s_i2c[port].periph);
        s_i2c[port].initialized = 0U;
        s_i2c[port].pending = 0U;
    }

    return OPRT_OK;
}

OPERATE_RET tkl_i2c_master_send(TUYA_I2C_NUM_E port, uint16_t dev_addr,
                                const void *data, uint32_t size,
                                BOOL_T xfer_pending)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t periph;
    OPERATE_RET rt;

    if (port >= TUYA_I2C_NUM_MAX || !s_i2c[port].initialized ||
        data == NULL || size == 0U || size > 255U) {
        return OPRT_INVALID_PARM;
    }

    periph = s_i2c[port].periph;

    if (!s_i2c[port].pending) {
        rt = __i2c_wait_idle(periph);
        if (rt != OPRT_OK) {
            __i2c_configure_peripheral(port);
            return rt;
        }
    }

    __i2c_clear_errors(periph);
    i2c_flag_clear(periph, I2C_FLAG_STPDET);

    i2c_master_addressing(periph, (uint32_t)dev_addr << 1U,
                          I2C_MASTER_TRANSMIT);
    i2c_transfer_byte_number_config(periph, size);
    if (xfer_pending) {
        i2c_automatic_end_disable(periph);
    } else {
        i2c_automatic_end_enable(periph);
    }

    i2c_start_on_bus(periph);
    i2c_data_transmit(periph, bytes[0]);

    for (uint32_t i = 1U; i < size; ++i) {
        rt = __i2c_wait_flag(periph, I2C_FLAG_TI);
        if (rt != OPRT_OK) {
            __i2c_clear_errors(periph);
            __i2c_configure_peripheral(port);
            s_i2c[port].pending = 0U;
            return rt;
        }
        i2c_data_transmit(periph, bytes[i]);
    }

    rt = __i2c_wait_flag(periph, I2C_FLAG_TI);
    if (rt != OPRT_OK) {
        __i2c_clear_errors(periph);
        __i2c_configure_peripheral(port);
        s_i2c[port].pending = 0U;
        return rt;
    }

    rt = __i2c_finish(periph, xfer_pending);
    if (rt == OPRT_OK) {
        s_i2c[port].pending = xfer_pending ? 1U : 0U;
        s_i2c[port].transfer_count = size;
    } else {
        __i2c_configure_peripheral(port);
        s_i2c[port].pending = 0U;
    }

    return rt;
}

OPERATE_RET tkl_i2c_master_receive(TUYA_I2C_NUM_E port, uint16_t dev_addr,
                                   void *data, uint32_t size,
                                   BOOL_T xfer_pending)
{
    uint8_t *bytes = (uint8_t *)data;
    uint32_t periph;
    OPERATE_RET rt;

    if (port >= TUYA_I2C_NUM_MAX || !s_i2c[port].initialized ||
        data == NULL || size == 0U || size > 255U) {
        return OPRT_INVALID_PARM;
    }

    periph = s_i2c[port].periph;

    if (!s_i2c[port].pending) {
        rt = __i2c_wait_idle(periph);
        if (rt != OPRT_OK) {
            __i2c_configure_peripheral(port);
            return rt;
        }
    }

    __i2c_clear_errors(periph);
    i2c_flag_clear(periph, I2C_FLAG_STPDET);
    i2c_reload_disable(periph);

    i2c_master_addressing(periph, (uint32_t)dev_addr << 1U,
                          I2C_MASTER_RECEIVE);
    i2c_transfer_byte_number_config(periph, size);
    if (xfer_pending) {
        i2c_automatic_end_disable(periph);
    } else {
        i2c_automatic_end_enable(periph);
    }

    i2c_start_on_bus(periph);

    for (uint32_t i = 0U; i < size; ++i) {
        rt = __i2c_wait_flag(periph, I2C_FLAG_RBNE);
        if (rt != OPRT_OK) {
            __i2c_clear_errors(periph);
            __i2c_configure_peripheral(port);
            s_i2c[port].pending = 0U;
            return rt;
        }
        bytes[i] = (uint8_t)i2c_data_receive(periph);
    }

    rt = __i2c_finish(periph, xfer_pending);
    if (rt == OPRT_OK) {
        s_i2c[port].pending = xfer_pending ? 1U : 0U;
        s_i2c[port].transfer_count = size;
    } else {
        __i2c_configure_peripheral(port);
        s_i2c[port].pending = 0U;
    }

    return rt;
}

OPERATE_RET tkl_i2c_irq_init(TUYA_I2C_NUM_E port, TUYA_I2C_IRQ_CB cb)
{
    (void)port;
    (void)cb;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_i2c_irq_enable(TUYA_I2C_NUM_E port)
{
    (void)port;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_i2c_irq_disable(TUYA_I2C_NUM_E port)
{
    (void)port;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_i2c_set_slave_addr(TUYA_I2C_NUM_E port, uint16_t dev_addr)
{
    (void)port;
    (void)dev_addr;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_i2c_slave_send(TUYA_I2C_NUM_E port, const void *data, uint32_t size)
{
    (void)port;
    (void)data;
    (void)size;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_i2c_slave_receive(TUYA_I2C_NUM_E port, void *data, uint32_t size)
{
    (void)port;
    (void)data;
    (void)size;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_i2c_get_status(TUYA_I2C_NUM_E port, TUYA_IIC_STATUS_T *status)
{
    uint32_t periph;

    if (port >= TUYA_I2C_NUM_MAX || status == NULL ||
        !s_i2c[port].initialized) {
        return OPRT_INVALID_PARM;
    }

    periph = s_i2c[port].periph;
    memset(status, 0, sizeof(*status));
    status->busy = i2c_flag_get(periph, I2C_FLAG_I2CBSY) == SET;
    status->mode = 1U;
    status->bus_error = i2c_flag_get(periph, I2C_FLAG_BERR) == SET;

    return OPRT_OK;
}

OPERATE_RET tkl_i2c_reset(TUYA_I2C_NUM_E port)
{
    if (port >= TUYA_I2C_NUM_MAX || !s_i2c[port].initialized) {
        return OPRT_INVALID_PARM;
    }

    i2c_disable(s_i2c[port].periph);
    __i2c_gpio_config(port);
    __i2c_configure_peripheral(port);
    s_i2c[port].pending = 0U;
    s_i2c[port].transfer_count = 0U;

    return OPRT_OK;
}

int32_t tkl_i2c_get_data_count(TUYA_I2C_NUM_E port)
{
    if (port >= TUYA_I2C_NUM_MAX || !s_i2c[port].initialized) {
        return OPRT_INVALID_PARM;
    }

    return (int32_t)s_i2c[port].transfer_count;
}

OPERATE_RET tkl_i2c_ioctl(TUYA_I2C_NUM_E port, uint32_t cmd, void *args)
{
    (void)port;
    (void)cmd;
    (void)args;
    return OPRT_NOT_SUPPORTED;
}
