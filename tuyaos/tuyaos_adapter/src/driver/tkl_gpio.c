/**
 * @file tkl_gpio.c
 * @brief GD32H77X compact GPIO adapter.
 *
 * TUYA_GPIO_NUM_E maps ports A-H, J and K as contiguous groups of 16 pins:
 * A0..A15 = 0..15, B0..B15 = 16..31, and so on.  This keeps the full device
 * pin space addressable without a sparse lookup table.
 *
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "tkl_gpio.h"

#ifdef BIT
#undef BIT
#endif

#include "gd32h77x_78x.h"
#include "tuya_error_code.h"

static bool __gpio_decode_pin(uint32_t pin_id, uint32_t *port, uint32_t *pin,
                              uint32_t *rcu)
{
    static const uint32_t ports[] = {
        GPIOA, GPIOB, GPIOC, GPIOD, GPIOE, GPIOF, GPIOG, GPIOH, GPIOJ, GPIOK,
    };
    static const uint32_t rcus[] = {
        RCU_GPIOA, RCU_GPIOB, RCU_GPIOC, RCU_GPIOD, RCU_GPIOE,
        RCU_GPIOF, RCU_GPIOG, RCU_GPIOH, RCU_GPIOJ, RCU_GPIOK,
    };

    if (port == NULL || pin == NULL || rcu == NULL ||
        pin_id >= TUYA_GPIO_NUM_MAX) {
        return false;
    }

    uint32_t port_index = pin_id / 16U;
    uint32_t pin_index = pin_id % 16U;
    if (port_index >= sizeof(ports) / sizeof(ports[0])) {
        return false;
    }
    *port = ports[port_index];
    *pin = (uint32_t)1U << pin_index;
    *rcu = rcus[port_index];
    return true;
}

OPERATE_RET tkl_gpio_init(TUYA_GPIO_NUM_E pin_id, const TUYA_GPIO_BASE_CFG_T *cfg)
{
    uint32_t port;
    uint32_t pin;
    uint32_t rcu;

    if (cfg == NULL || !__gpio_decode_pin(pin_id, &port, &pin, &rcu)) {
        return OPRT_INVALID_PARM;
    }

    rcu_periph_clock_enable(rcu);

    if (cfg->direct == TUYA_GPIO_OUTPUT) {
        if (cfg->level == TUYA_GPIO_LEVEL_HIGH) {
            GPIO_BOP(port) = pin;
        } else {
            GPIO_BC(port) = pin;
        }

        gpio_mode_set(port, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, pin);
        gpio_output_options_set(port, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, pin);
        return OPRT_OK;
    }

    switch (cfg->mode) {
        case TUYA_GPIO_PULLUP:
            gpio_mode_set(port, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP, pin);
            break;
        case TUYA_GPIO_PULLDOWN:
            gpio_mode_set(port, GPIO_MODE_INPUT, GPIO_PUPD_PULLDOWN, pin);
            break;
        case TUYA_GPIO_FLOATING:
        case TUYA_GPIO_HIGH_IMPEDANCE:
        default:
            gpio_mode_set(port, GPIO_MODE_INPUT, GPIO_PUPD_NONE, pin);
            break;
    }

    return OPRT_OK;
}

OPERATE_RET tkl_gpio_deinit(TUYA_GPIO_NUM_E pin_id)
{
    uint32_t port;
    uint32_t pin;
    uint32_t rcu;

    if (!__gpio_decode_pin(pin_id, &port, &pin, &rcu)) {
        return OPRT_INVALID_PARM;
    }

    gpio_mode_set(port, GPIO_MODE_INPUT, GPIO_PUPD_NONE, pin);
    return OPRT_OK;
}

OPERATE_RET tkl_gpio_write(TUYA_GPIO_NUM_E pin_id, TUYA_GPIO_LEVEL_E level)
{
    uint32_t port;
    uint32_t pin;
    uint32_t rcu;

    if (!__gpio_decode_pin(pin_id, &port, &pin, &rcu) ||
        level == TUYA_GPIO_LEVEL_NONE) {
        return OPRT_INVALID_PARM;
    }

    if (level == TUYA_GPIO_LEVEL_HIGH) {
        GPIO_BOP(port) = pin;
    } else {
        GPIO_BC(port) = pin;
    }

    return OPRT_OK;
}

OPERATE_RET tkl_gpio_read(TUYA_GPIO_NUM_E pin_id, TUYA_GPIO_LEVEL_E *level)
{
    uint32_t port;
    uint32_t pin;
    uint32_t rcu;

    if (level == NULL || !__gpio_decode_pin(pin_id, &port, &pin, &rcu)) {
        return OPRT_INVALID_PARM;
    }

    *level = gpio_input_bit_get(port, pin) == SET ?
             TUYA_GPIO_LEVEL_HIGH : TUYA_GPIO_LEVEL_LOW;
    return OPRT_OK;
}

OPERATE_RET tkl_gpio_irq_init(TUYA_GPIO_NUM_E pin_id, const TUYA_GPIO_IRQ_T *cfg)
{
    (void)pin_id;
    (void)cfg;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_gpio_irq_enable(TUYA_GPIO_NUM_E pin_id)
{
    (void)pin_id;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_gpio_irq_disable(TUYA_GPIO_NUM_E pin_id)
{
    (void)pin_id;
    return OPRT_NOT_SUPPORTED;
}
