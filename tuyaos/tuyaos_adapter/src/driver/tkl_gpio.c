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
#include "gd32h77x_78x_exti.h"
#include "gd32h77x_78x_syscfg.h"
#include "tkl_output.h"
#include "tuya_error_code.h"

#define GPIO_IRQ_LINE_COUNT 16U
#define GPIO_IRQ_PRIORITY   6U

typedef struct {
    TUYA_GPIO_NUM_E pin;
    TUYA_GPIO_IRQ_CB cb;
    void *arg;
    bool configured;
    bool enabled;
} GPIO_IRQ_CONTEXT_T;

static GPIO_IRQ_CONTEXT_T s_gpio_irq_context[GPIO_IRQ_LINE_COUNT];

static const uint8_t s_gpio_exti_source[] = {
    EXTI_SOURCE_GPIOA, EXTI_SOURCE_GPIOB, EXTI_SOURCE_GPIOC, EXTI_SOURCE_GPIOD,
    EXTI_SOURCE_GPIOE, EXTI_SOURCE_GPIOF, EXTI_SOURCE_GPIOG, EXTI_SOURCE_GPIOH,
    EXTI_SOURCE_GPIOJ, EXTI_SOURCE_GPIOK,
};

static const char s_gpio_port_letters[] = "ABCDEFGHJK";

static const exti_line_enum s_gpio_exti_line[GPIO_IRQ_LINE_COUNT] = {
    EXTI_0,  EXTI_1,  EXTI_2,  EXTI_3,  EXTI_4,  EXTI_5,  EXTI_6,  EXTI_7,
    EXTI_8,  EXTI_9,  EXTI_10, EXTI_11, EXTI_12, EXTI_13, EXTI_14, EXTI_15,
};

static bool __gpio_decode_pin(uint32_t pin_id, uint32_t *port, uint32_t *pin,
                              uint32_t *rcu, uint8_t *port_index)
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

    uint32_t index = pin_id / 16U;
    uint32_t pin_number = pin_id % 16U;
    if (index >= sizeof(ports) / sizeof(ports[0])) {
        return false;
    }

    *port = ports[index];
    *pin = (uint32_t)1U << pin_number;
    *rcu = rcus[index];
    if (port_index != NULL) {
        *port_index = (uint8_t)index;
    }
    return true;
}

static char __gpio_port_letter(TUYA_GPIO_NUM_E pin_id)
{
    return s_gpio_port_letters[pin_id / 16U];
}

static IRQn_Type __gpio_irq_vector(uint8_t line)
{
    switch (line) {
        case 0U:
            return EXTI0_IRQn;
        case 1U:
            return EXTI1_IRQn;
        case 2U:
            return EXTI2_IRQn;
        case 3U:
            return EXTI3_IRQn;
        case 4U:
            return EXTI4_IRQn;
        case 5U:
        case 6U:
        case 7U:
        case 8U:
        case 9U:
            return EXTI5_9_IRQn;
        default:
            return EXTI10_15_IRQn;
    }
}

static bool __gpio_irq_vector_in_use(uint8_t line, uint8_t exclude_line)
{
    IRQn_Type vector = __gpio_irq_vector(line);
    uint8_t index;

    for (index = 0U; index < GPIO_IRQ_LINE_COUNT; index++) {
        if (index != exclude_line && s_gpio_irq_context[index].enabled &&
            __gpio_irq_vector(index) == vector) {
            return true;
        }
    }

    return false;
}

static OPERATE_RET __gpio_irq_trigger(TUYA_GPIO_IRQ_E mode,
                                      exti_trig_type_enum *trigger)
{
    switch (mode) {
        case TUYA_GPIO_IRQ_RISE:
            *trigger = EXTI_TRIG_RISING;
            return OPRT_OK;
        case TUYA_GPIO_IRQ_FALL:
            *trigger = EXTI_TRIG_FALLING;
            return OPRT_OK;
        case TUYA_GPIO_IRQ_RISE_FALL:
            *trigger = EXTI_TRIG_BOTH;
            return OPRT_OK;
        case TUYA_GPIO_IRQ_LOW:
        case TUYA_GPIO_IRQ_HIGH:
            return OPRT_NOT_SUPPORTED;
        default:
            return OPRT_INVALID_PARM;
    }
}

static void __gpio_irq_dispatch(uint8_t line)
{
    GPIO_IRQ_CONTEXT_T *context = &s_gpio_irq_context[line];
    exti_line_enum exti_line = s_gpio_exti_line[line];

    if (exti_interrupt_flag_get(exti_line) == RESET) {
        return;
    }

    exti_interrupt_flag_clear(exti_line);
    if (context->enabled && context->configured && context->cb != NULL) {
        context->cb(context->arg);
    }
}

OPERATE_RET tkl_gpio_init(TUYA_GPIO_NUM_E pin_id, const TUYA_GPIO_BASE_CFG_T *cfg)
{
    uint32_t port;
    uint32_t pin;
    uint32_t rcu;

    if (cfg == NULL || !__gpio_decode_pin(pin_id, &port, &pin, &rcu, NULL)) {
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

    if (!__gpio_decode_pin(pin_id, &port, &pin, &rcu, NULL)) {
        return OPRT_INVALID_PARM;
    }

    GPIO_IRQ_CONTEXT_T *context = &s_gpio_irq_context[pin_id % 16U];
    if (context->configured && context->pin == pin_id) {
        (void)tkl_gpio_irq_disable(pin_id);
        context->configured = false;
        context->cb = NULL;
        context->arg = NULL;
    }

    gpio_mode_set(port, GPIO_MODE_INPUT, GPIO_PUPD_NONE, pin);
    return OPRT_OK;
}

OPERATE_RET tkl_gpio_write(TUYA_GPIO_NUM_E pin_id, TUYA_GPIO_LEVEL_E level)
{
    uint32_t port;
    uint32_t pin;
    uint32_t rcu;

    if (!__gpio_decode_pin(pin_id, &port, &pin, &rcu, NULL) ||
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

    if (level == NULL || !__gpio_decode_pin(pin_id, &port, &pin, &rcu, NULL)) {
        return OPRT_INVALID_PARM;
    }

    *level = gpio_input_bit_get(port, pin) == SET ?
             TUYA_GPIO_LEVEL_HIGH : TUYA_GPIO_LEVEL_LOW;
    return OPRT_OK;
}

OPERATE_RET tkl_gpio_irq_init(TUYA_GPIO_NUM_E pin_id, const TUYA_GPIO_IRQ_T *cfg)
{
    uint32_t port;
    uint32_t pin;
    uint32_t rcu;
    uint8_t port_index;
    uint8_t line;
    exti_trig_type_enum trigger;
    GPIO_IRQ_CONTEXT_T *context;
    OPERATE_RET ret;

    if (cfg == NULL || cfg->cb == NULL ||
        !__gpio_decode_pin(pin_id, &port, &pin, &rcu, &port_index)) {
        return OPRT_INVALID_PARM;
    }

    ret = __gpio_irq_trigger(cfg->mode, &trigger);
    if (ret != OPRT_OK) {
        if (ret == OPRT_NOT_SUPPORTED) {
            tkl_log_output("[GPIO IRQ] P%c%u mode %u is unsupported: GD32 EXTI supports "
                           "rising edge, falling edge and both edges, but not level trigger\r\n",
                           __gpio_port_letter(pin_id), (unsigned)(pin_id % 16U),
                           (unsigned)cfg->mode);
        }
        return ret;
    }

    line = (uint8_t)(pin_id % 16U);
    context = &s_gpio_irq_context[line];
    if (context->configured && context->pin != pin_id) {
        tkl_log_output("[GPIO IRQ] conflict: P%c%u and P%c%u both map to EXTI%u; "
                       "GD32 EXTI can select only one GPIO port for each line\r\n",
                       __gpio_port_letter(context->pin),
                       (unsigned)(context->pin % 16U),
                       __gpio_port_letter(pin_id), (unsigned)(pin_id % 16U),
                       (unsigned)line);
        return OPRT_COM_ERROR;
    }

    if (context->configured) {
        exti_interrupt_disable(s_gpio_exti_line[line]);
        exti_interrupt_flag_clear(s_gpio_exti_line[line]);
        context->enabled = false;
        if (!__gpio_irq_vector_in_use(line, line)) {
            NVIC_DisableIRQ(__gpio_irq_vector(line));
            NVIC_ClearPendingIRQ(__gpio_irq_vector(line));
        }
    }

    rcu_periph_clock_enable(RCU_SYSCFG);
    syscfg_exti_line_config(s_gpio_exti_source[port_index], line);
    exti_init(s_gpio_exti_line[line], EXTI_INTERRUPT, trigger);
    exti_interrupt_disable(s_gpio_exti_line[line]);
    exti_interrupt_flag_clear(s_gpio_exti_line[line]);

    context->pin = pin_id;
    context->cb = cfg->cb;
    context->arg = cfg->arg;
    context->configured = true;
    context->enabled = false;
    return OPRT_OK;
}

OPERATE_RET tkl_gpio_irq_enable(TUYA_GPIO_NUM_E pin_id)
{
    uint8_t line;
    GPIO_IRQ_CONTEXT_T *context;
    IRQn_Type vector;

    if (pin_id >= TUYA_GPIO_NUM_MAX) {
        return OPRT_INVALID_PARM;
    }

    line = (uint8_t)(pin_id % 16U);
    context = &s_gpio_irq_context[line];
    if (!context->configured || context->pin != pin_id) {
        return OPRT_INVALID_PARM;
    }

    vector = __gpio_irq_vector(line);
    NVIC_SetPriority(vector, GPIO_IRQ_PRIORITY);
    if (!__gpio_irq_vector_in_use(line, line)) {
        NVIC_ClearPendingIRQ(vector);
    }
    exti_interrupt_flag_clear(s_gpio_exti_line[line]);
    context->enabled = true;
    NVIC_EnableIRQ(vector);
    exti_interrupt_enable(s_gpio_exti_line[line]);
    return OPRT_OK;
}

OPERATE_RET tkl_gpio_irq_disable(TUYA_GPIO_NUM_E pin_id)
{
    uint8_t line;
    GPIO_IRQ_CONTEXT_T *context;
    IRQn_Type vector;

    if (pin_id >= TUYA_GPIO_NUM_MAX) {
        return OPRT_INVALID_PARM;
    }

    line = (uint8_t)(pin_id % 16U);
    context = &s_gpio_irq_context[line];
    if (!context->configured || context->pin != pin_id) {
        return OPRT_INVALID_PARM;
    }

    exti_interrupt_disable(s_gpio_exti_line[line]);
    exti_interrupt_flag_clear(s_gpio_exti_line[line]);
    context->enabled = false;

    vector = __gpio_irq_vector(line);
    if (!__gpio_irq_vector_in_use(line, line)) {
        NVIC_DisableIRQ(vector);
        NVIC_ClearPendingIRQ(vector);
    }

    return OPRT_OK;
}

void EXTI0_IRQHandler(void)
{
    __gpio_irq_dispatch(0U);
}

void EXTI1_IRQHandler(void)
{
    __gpio_irq_dispatch(1U);
}

void EXTI2_IRQHandler(void)
{
    __gpio_irq_dispatch(2U);
}

void EXTI3_IRQHandler(void)
{
    __gpio_irq_dispatch(3U);
}

void EXTI4_IRQHandler(void)
{
    __gpio_irq_dispatch(4U);
}

void EXTI5_9_IRQHandler(void)
{
    uint8_t line;

    for (line = 5U; line <= 9U; line++) {
        __gpio_irq_dispatch(line);
    }
}

void EXTI10_15_IRQHandler(void)
{
    uint8_t line;

    for (line = 10U; line <= 15U; line++) {
        __gpio_irq_dispatch(line);
    }
}
