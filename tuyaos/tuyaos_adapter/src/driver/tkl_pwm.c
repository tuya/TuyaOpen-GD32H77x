/**
 * @file tkl_pwm.c
 * @brief GD32H77X PWM TKL backend.
 *
 * Twelve TKL PWM channels map to TIMER0/1/2 channel groups. Default routing is
 * owned here; tkl_pinmux only updates the selected channel entries.
 *
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "tkl_pwm.h"

#ifdef BIT
#undef BIT
#endif

#include "gd32h77x_78x.h"
#include "gd32h77x_78x_timer.h"
#include "tkl_pinmux.h"
#include "tuya_error_code.h"

#include <string.h>

#define GD32H77X_PWM_CHANNELS       12U
#define GD32H77X_PWM_TIMERS         3U
#define GD32H77X_PWM_DUTY_FULL      10000U
#define GD32H77X_PWM_PERIOD_MAX     0x10000U

typedef struct {
    uint32_t port;
    uint32_t pin;
    uint32_t af;
} PWM_PIN_T;

typedef struct {
    uint32_t timer;
    uint16_t channel;
    uint8_t timer_index;
    PWM_PIN_T pin;
} PWM_CHANNEL_CFG_T;

typedef struct {
    TUYA_PWM_BASE_CFG_T cfg;
    uint8_t initialized;
    uint8_t running;
} PWM_CHANNEL_STATE_T;

typedef struct {
    uint32_t prescaler;
    uint32_t counter_ticks;
    uint32_t frequency;
    TUYA_PWM_COUNT_E count_mode;
    uint8_t initialized;
    uint8_t running;
    uint8_t users;
} PWM_TIMER_STATE_T;

#define PWM_PIN(port_name, pin_num, af_val) \
    {port_name, GPIO_PIN_##pin_num, af_val}

static const uint32_t s_pwm_timers[GD32H77X_PWM_TIMERS] = {
    TIMER0, TIMER1, TIMER2,
};
static const uint32_t s_pwm_rcus[GD32H77X_PWM_TIMERS] = {
    RCU_TIMER0, RCU_TIMER1, RCU_TIMER2,
};

/* Default routing for the public TKL PWM channel IDs.  The timer group is
 * shared by four channels; comments include the matching TUYA_GPIO_NUM.
 */
static PWM_CHANNEL_CFG_T s_pwm_cfg[GD32H77X_PWM_CHANNELS] = {
    [TUYA_PWM_NUM_0] = {TIMER0, TIMER_CH_0, 0U, PWM_PIN(GPIOA, 8, 1)},
    [TUYA_PWM_NUM_1] = {TIMER0, TIMER_CH_1, 0U, PWM_PIN(GPIOA, 9, 1)},
    [TUYA_PWM_NUM_2] = {TIMER0, TIMER_CH_2, 0U, PWM_PIN(GPIOA, 10, 1)},
    [TUYA_PWM_NUM_3] = {TIMER0, TIMER_CH_3, 0U, PWM_PIN(GPIOA, 11, 1)},
    [TUYA_PWM_NUM_4] = {TIMER1, TIMER_CH_0, 1U, PWM_PIN(GPIOA, 0, 1)},
    [TUYA_PWM_NUM_5] = {TIMER1, TIMER_CH_1, 1U, PWM_PIN(GPIOA, 1, 1)},
    [TUYA_PWM_NUM_6] = {TIMER1, TIMER_CH_2, 1U, PWM_PIN(GPIOA, 2, 1)},
    [TUYA_PWM_NUM_7] = {TIMER1, TIMER_CH_3, 1U, PWM_PIN(GPIOA, 3, 1)},
    [TUYA_PWM_NUM_8] = {TIMER2, TIMER_CH_0, 2U, PWM_PIN(GPIOA, 6, 2)},
    [TUYA_PWM_NUM_9] = {TIMER2, TIMER_CH_1, 2U, PWM_PIN(GPIOA, 7, 2)},
    [TUYA_PWM_NUM_10] = {TIMER2, TIMER_CH_2, 2U, PWM_PIN(GPIOB, 0, 2)},
    [TUYA_PWM_NUM_11] = {TIMER2, TIMER_CH_3, 2U, PWM_PIN(GPIOB, 1, 2)},
};

static PWM_CHANNEL_STATE_T s_pwm_state[GD32H77X_PWM_CHANNELS];
static PWM_TIMER_STATE_T s_pwm_timer[GD32H77X_PWM_TIMERS];

/* Pin comments are kept next to the physical defaults:
 * PA8/PA9/PA10/PA11 = TUYA_GPIO_NUM_8/9/10/11, TIMER0 CH0..3 AF1
 * PA0/PA1/PA2/PA3 = TUYA_GPIO_NUM_0/1/2/3, TIMER1 CH0..3 AF1
 * PA6/PA7/PB0/PB1 = TUYA_GPIO_NUM_6/7/16/17, TIMER2 CH0..3 AF2
 */

void __tkl_pwm_set_pin(TUYA_PWM_NUM_E port, TUYA_PIN_FUNC_E func,
                       uint32_t gpio_port, uint32_t gpio_pin, uint32_t af)
{
    if (func < TUYA_PWM0 || func > TUYA_PWM11 ||
        port >= GD32H77X_PWM_CHANNELS ||
        (uint32_t)(func - TUYA_PWM0) != (uint32_t)port) {
        return;
    }

    s_pwm_cfg[port].pin.port = gpio_port;
    s_pwm_cfg[port].pin.pin = gpio_pin;
    s_pwm_cfg[port].pin.af = af;
}

static uint32_t __pwm_gpio_rcu(uint32_t port)
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

static void __pwm_init_gpio(TUYA_PWM_NUM_E port)
{
    const PWM_PIN_T *pin = &s_pwm_cfg[port].pin;
    uint32_t rcu = __pwm_gpio_rcu(pin->port);

    if (rcu == 0U) {
        return;
    }
    rcu_periph_clock_enable(rcu);
    gpio_af_set(pin->port, pin->af, pin->pin);
    gpio_mode_set(pin->port, GPIO_MODE_AF, GPIO_PUPD_NONE, pin->pin);
    gpio_output_options_set(pin->port, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ,
                            pin->pin);
}

static uint32_t __pwm_timer_clock(uint32_t timer)
{
    uint32_t ahb = rcu_clock_freq_get(CK_AHB);
    uint32_t pclk = rcu_clock_freq_get(timer == TIMER0 ? CK_APB2 : CK_APB1);
    uint32_t divider;
    uint32_t mul4 = (RCU_CFG1 & RCU_CFG1_TIMERSEL) != 0U;

    if (pclk == 0U) {
        return ahb;
    }
    divider = ahb / pclk;
    if ((!mul4 && divider <= 2U) || (mul4 && divider <= 4U)) {
        return ahb;
    }
    return pclk * (mul4 ? 4U : 2U);
}

static bool __pwm_duty_valid(const TUYA_PWM_BASE_CFG_T *cfg)
{
    uint32_t full_scale = cfg->cycle == 0U ? GD32H77X_PWM_DUTY_FULL : cfg->cycle;
    return cfg->frequency != 0U && cfg->duty <= full_scale &&
           (cfg->polarity == TUYA_PWM_NEGATIVE ||
            cfg->polarity == TUYA_PWM_POSITIVE) &&
           (cfg->count_mode == TUYA_PWM_CNT_UP ||
            cfg->count_mode == TUYA_PWM_CNT_UP_AND_DOWN);
}

static OPERATE_RET __pwm_calc_timing(uint32_t timer, uint32_t frequency,
                                     TUYA_PWM_COUNT_E count_mode,
                                     uint32_t *prescaler,
                                     uint32_t *counter_ticks)
{
    uint64_t denominator;
    uint32_t clock = __pwm_timer_clock(timer);
    uint32_t divider;
    uint32_t ticks;

    if (clock == 0U || frequency == 0U) {
        return OPRT_INVALID_PARM;
    }
    denominator = (uint64_t)frequency * GD32H77X_PWM_PERIOD_MAX;
    divider = (uint32_t)((clock + denominator - 1U) / denominator);
    if (divider == 0U) {
        divider = 1U;
    }
    if (divider > GD32H77X_PWM_PERIOD_MAX) {
        return OPRT_NOT_SUPPORTED;
    }
    ticks = clock / (frequency * divider);
    if (ticks < 2U) {
        return OPRT_NOT_SUPPORTED;
    }
    if (count_mode == TUYA_PWM_CNT_UP_AND_DOWN) {
        ticks /= 2U;
        if (ticks < 2U) {
            return OPRT_NOT_SUPPORTED;
        }
    }
    *prescaler = divider - 1U;
    *counter_ticks = ticks;
    return OPRT_OK;
}

static uint32_t __pwm_pulse_ticks(const TUYA_PWM_BASE_CFG_T *cfg,
                                  uint32_t counter_ticks)
{
    uint32_t full_scale = cfg->cycle == 0U ? GD32H77X_PWM_DUTY_FULL : cfg->cycle;
    uint32_t duty = cfg->duty;
    uint32_t pulse;

    if (full_scale == 0U) {
        return 0U;
    }
    if (duty > full_scale) {
        duty = full_scale;
    }
    pulse = (uint32_t)(((uint64_t)counter_ticks * duty) / full_scale);
    if (duty != 0U && pulse == 0U) {
        pulse = 1U;
    }
    if (pulse > counter_ticks) {
        pulse = counter_ticks;
    }
    return pulse;
}

static void __pwm_apply_channel(TUYA_PWM_NUM_E port)
{
    PWM_CHANNEL_CFG_T *cfg = &s_pwm_cfg[port];
    PWM_CHANNEL_STATE_T *state = &s_pwm_state[port];
    uint32_t timer_state = s_pwm_timer[cfg->timer_index].counter_ticks;
    timer_oc_parameter_struct oc = {0};

    timer_channel_output_struct_para_init(&oc);
    oc.outputstate = TIMER_CCX_ENABLE;
    oc.outputnstate = TIMER_CCXN_DISABLE;
    if (state->cfg.polarity == TUYA_PWM_NEGATIVE) {
        oc.ocpolarity = TIMER_OC_POLARITY_LOW;
        oc.ocidlestate = TIMER_OC_IDLE_STATE_HIGH;
    } else {
        oc.ocpolarity = TIMER_OC_POLARITY_HIGH;
        oc.ocidlestate = TIMER_OC_IDLE_STATE_LOW;
    }
    oc.ocnidlestate = TIMER_OCN_IDLE_STATE_LOW;

    timer_channel_output_config(cfg->timer, cfg->channel, &oc);
    timer_channel_output_mode_config(cfg->timer, cfg->channel,
                                     TIMER_OC_MODE_PWM0);
    timer_channel_output_shadow_config(cfg->timer, cfg->channel,
                                       TIMER_OC_SHADOW_ENABLE);
    timer_channel_output_pulse_value_config(cfg->timer, cfg->channel,
                                            __pwm_pulse_ticks(&state->cfg,
                                                              timer_state));
}

static OPERATE_RET __pwm_configure_timer(uint32_t timer_index,
                                         uint32_t frequency,
                                         TUYA_PWM_COUNT_E count_mode,
                                         bool preserve_running)
{
    PWM_TIMER_STATE_T *state = &s_pwm_timer[timer_index];
    timer_parameter_struct timer_cfg = {0};
    uint32_t prescaler;
    uint32_t ticks;
    OPERATE_RET rt;
    bool was_running = preserve_running && state->running != 0U;

    rt = __pwm_calc_timing(s_pwm_timers[timer_index], frequency, count_mode,
                           &prescaler, &ticks);
    if (rt != OPRT_OK) {
        return rt;
    }
    if (was_running) {
        timer_disable(s_pwm_timers[timer_index]);
    }

    timer_struct_para_init(&timer_cfg);
    timer_cfg.prescaler = (uint16_t)prescaler;
    timer_cfg.alignedmode = count_mode == TUYA_PWM_CNT_UP ?
                            TIMER_COUNTER_EDGE :
                            TIMER_COUNTER_CENTER_BOTH;
    timer_cfg.counterdirection = TIMER_COUNTER_UP;
    timer_cfg.period = ticks - 1U;
    timer_cfg.clockdivision = TIMER_CKDIV_DIV1;
    timer_cfg.repetitioncounter = 0U;

    rcu_periph_clock_enable(s_pwm_rcus[timer_index]);
    timer_deinit(s_pwm_timers[timer_index]);
    timer_init(s_pwm_timers[timer_index], &timer_cfg);
    timer_auto_reload_shadow_enable(s_pwm_timers[timer_index]);

    state->prescaler = prescaler;
    state->counter_ticks = ticks;
    state->frequency = frequency;
    state->count_mode = count_mode;
    state->initialized = 1U;

    for (uint32_t port = 0U; port < GD32H77X_PWM_CHANNELS; ++port) {
        if (s_pwm_cfg[port].timer_index == timer_index &&
            s_pwm_state[port].initialized != 0U) {
            s_pwm_state[port].cfg.frequency = frequency;
            s_pwm_state[port].cfg.count_mode = count_mode;
            __pwm_apply_channel((TUYA_PWM_NUM_E)port);
        }
    }
    if (s_pwm_timers[timer_index] == TIMER0) {
        timer_primary_output_config(s_pwm_timers[timer_index], ENABLE);
    }
    if (was_running) {
        timer_enable(s_pwm_timers[timer_index]);
    }
    return OPRT_OK;
}

OPERATE_RET tkl_pwm_init(TUYA_PWM_NUM_E ch_id, const TUYA_PWM_BASE_CFG_T *cfg)
{
    PWM_CHANNEL_CFG_T *channel;
    PWM_CHANNEL_STATE_T *state;
    PWM_TIMER_STATE_T *timer;
    OPERATE_RET rt;

    if (ch_id >= GD32H77X_PWM_CHANNELS || cfg == NULL ||
        !__pwm_duty_valid(cfg)) {
        return OPRT_INVALID_PARM;
    }
    channel = &s_pwm_cfg[ch_id];
    state = &s_pwm_state[ch_id];
    timer = &s_pwm_timer[channel->timer_index];
    if (state->initialized != 0U) {
        return OPRT_INIT_MORE_THAN_ONCE;
    }
    if (timer->initialized != 0U &&
        (timer->frequency != cfg->frequency ||
         timer->count_mode != cfg->count_mode)) {
        return OPRT_NOT_SUPPORTED;
    }

    if (timer->initialized == 0U) {
        rt = __pwm_configure_timer(channel->timer_index, cfg->frequency,
                                   cfg->count_mode, false);
        if (rt != OPRT_OK) {
            return rt;
        }
    }

    __pwm_init_gpio(ch_id);
    state->cfg = *cfg;
    state->initialized = 1U;
    state->running = 0U;
    timer->users++;
    __pwm_apply_channel(ch_id);
    return OPRT_OK;
}

OPERATE_RET tkl_pwm_deinit(TUYA_PWM_NUM_E ch_id)
{
    if (ch_id >= GD32H77X_PWM_CHANNELS) {
        return OPRT_INVALID_PARM;
    }

    PWM_CHANNEL_CFG_T *channel = &s_pwm_cfg[ch_id];
    PWM_CHANNEL_STATE_T *state = &s_pwm_state[ch_id];
    PWM_TIMER_STATE_T *timer = &s_pwm_timer[channel->timer_index];
    (void)tkl_pwm_stop(ch_id);
    timer_channel_output_state_config(channel->timer, channel->channel,
                                      TIMER_CCX_DISABLE);
    memset(state, 0, sizeof(*state));
    if (timer->users != 0U) {
        timer->users--;
    }
    if (timer->users == 0U) {
        timer_disable(channel->timer);
        timer_deinit(channel->timer);
        memset(timer, 0, sizeof(*timer));
    }
    return OPRT_OK;
}

OPERATE_RET tkl_pwm_start(TUYA_PWM_NUM_E ch_id)
{
    if (ch_id >= GD32H77X_PWM_CHANNELS ||
        s_pwm_state[ch_id].initialized == 0U) {
        return OPRT_RESOURCE_NOT_READY;
    }

    PWM_CHANNEL_CFG_T *channel = &s_pwm_cfg[ch_id];
    PWM_TIMER_STATE_T *timer = &s_pwm_timer[channel->timer_index];
    if (s_pwm_state[ch_id].running != 0U) {
        return OPRT_OK;
    }

    timer_channel_output_state_config(channel->timer, channel->channel,
                                      TIMER_CCX_ENABLE);
    s_pwm_state[ch_id].running = 1U;
    if (timer->running == 0U) {
        timer_counter_value_config(channel->timer, 0U);
        timer_enable(channel->timer);
        timer->running = 1U;
    }
    return OPRT_OK;
}

OPERATE_RET tkl_pwm_stop(TUYA_PWM_NUM_E ch_id)
{
    if (ch_id >= GD32H77X_PWM_CHANNELS ||
        s_pwm_state[ch_id].initialized == 0U) {
        return OPRT_RESOURCE_NOT_READY;
    }

    PWM_CHANNEL_CFG_T *channel = &s_pwm_cfg[ch_id];
    PWM_TIMER_STATE_T *timer = &s_pwm_timer[channel->timer_index];
    timer_channel_output_state_config(channel->timer, channel->channel,
                                      TIMER_CCX_DISABLE);
    s_pwm_state[ch_id].running = 0U;

    bool any_running = false;
    for (uint32_t port = 0U; port < GD32H77X_PWM_CHANNELS; ++port) {
        if (s_pwm_cfg[port].timer_index == channel->timer_index &&
            s_pwm_state[port].running != 0U) {
            any_running = true;
            break;
        }
    }
    if (!any_running) {
        timer_disable(channel->timer);
        timer->running = 0U;
    }
    return OPRT_OK;
}

OPERATE_RET tkl_pwm_multichannel_start(TUYA_PWM_NUM_E *ch_id, uint8_t num)
{
    if (ch_id == NULL || num == 0U) {
        return OPRT_INVALID_PARM;
    }
    for (uint8_t i = 0U; i < num; ++i) {
        OPERATE_RET rt = tkl_pwm_start(ch_id[i]);
        if (rt != OPRT_OK) {
            return rt;
        }
    }
    return OPRT_OK;
}

OPERATE_RET tkl_pwm_multichannel_stop(TUYA_PWM_NUM_E *ch_id, uint8_t num)
{
    if (ch_id == NULL || num == 0U) {
        return OPRT_INVALID_PARM;
    }
    for (uint8_t i = 0U; i < num; ++i) {
        OPERATE_RET rt = tkl_pwm_stop(ch_id[i]);
        if (rt != OPRT_OK) {
            return rt;
        }
    }
    return OPRT_OK;
}

OPERATE_RET tkl_pwm_duty_set(TUYA_PWM_NUM_E ch_id, uint32_t duty)
{
    if (ch_id >= GD32H77X_PWM_CHANNELS ||
        s_pwm_state[ch_id].initialized == 0U) {
        return OPRT_RESOURCE_NOT_READY;
    }
    uint32_t full_scale = s_pwm_state[ch_id].cfg.cycle == 0U ?
                          GD32H77X_PWM_DUTY_FULL :
                          s_pwm_state[ch_id].cfg.cycle;
    if (duty > full_scale) {
        return OPRT_INVALID_PARM;
    }

    s_pwm_state[ch_id].cfg.duty = duty;
    PWM_CHANNEL_CFG_T *channel = &s_pwm_cfg[ch_id];
    timer_channel_output_pulse_value_config(channel->timer, channel->channel,
                                            __pwm_pulse_ticks(
                                                &s_pwm_state[ch_id].cfg,
                                                s_pwm_timer[channel->timer_index].
                                                    counter_ticks));
    return OPRT_OK;
}

OPERATE_RET tkl_pwm_frequency_set(TUYA_PWM_NUM_E ch_id, uint32_t frequency)
{
    if (ch_id >= GD32H77X_PWM_CHANNELS ||
        s_pwm_state[ch_id].initialized == 0U || frequency == 0U) {
        return OPRT_INVALID_PARM;
    }
    uint32_t timer_index = s_pwm_cfg[ch_id].timer_index;
    return __pwm_configure_timer(timer_index, frequency,
                                 s_pwm_state[ch_id].cfg.count_mode, true);
}

OPERATE_RET tkl_pwm_polarity_set(TUYA_PWM_NUM_E ch_id,
                                 TUYA_PWM_POLARITY_E polarity)
{
    if (ch_id >= GD32H77X_PWM_CHANNELS ||
        s_pwm_state[ch_id].initialized == 0U ||
        (polarity != TUYA_PWM_NEGATIVE && polarity != TUYA_PWM_POSITIVE)) {
        return OPRT_INVALID_PARM;
    }
    s_pwm_state[ch_id].cfg.polarity = polarity;
    __pwm_apply_channel(ch_id);
    return OPRT_OK;
}

OPERATE_RET tkl_pwm_info_set(TUYA_PWM_NUM_E ch_id,
                             const TUYA_PWM_BASE_CFG_T *info)
{
    if (ch_id >= GD32H77X_PWM_CHANNELS ||
        s_pwm_state[ch_id].initialized == 0U || info == NULL ||
        !__pwm_duty_valid(info)) {
        return OPRT_INVALID_PARM;
    }

    uint32_t timer_index = s_pwm_cfg[ch_id].timer_index;
    OPERATE_RET rt = __pwm_configure_timer(timer_index, info->frequency,
                                           info->count_mode, true);
    if (rt != OPRT_OK) {
        return rt;
    }
    s_pwm_state[ch_id].cfg = *info;
    __pwm_apply_channel(ch_id);
    return OPRT_OK;
}

OPERATE_RET tkl_pwm_info_get(TUYA_PWM_NUM_E ch_id, TUYA_PWM_BASE_CFG_T *info)
{
    if (ch_id >= GD32H77X_PWM_CHANNELS || info == NULL ||
        s_pwm_state[ch_id].initialized == 0U) {
        return OPRT_INVALID_PARM;
    }
    *info = s_pwm_state[ch_id].cfg;
    return OPRT_OK;
}

OPERATE_RET tkl_pwm_cap_start(TUYA_PWM_NUM_E ch_id,
                              const TUYA_PWM_CAP_IRQ_T *cfg)
{
    (void)ch_id;
    (void)cfg;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_pwm_cap_stop(TUYA_PWM_NUM_E ch_id)
{
    (void)ch_id;
    return OPRT_NOT_SUPPORTED;
}
