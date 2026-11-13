/**
 * @file tkl_timer.c
 * @brief GD32H77X hardware timer adapter.
 *
 * TIMER3, TIMER4 and TIMER6 provide the common one-shot/periodic timer API.
 * TIMER0/1/2 remain assigned to PWM, while TIMER0_MCH1 is reserved for the
 * Watch board WS2812 transport.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "tkl_timer.h"

#include <string.h>

#ifdef BIT
#undef BIT
#endif

#include "gd32h77x_78x.h"
#include "gd32h77x_78x_rcu.h"
#include "gd32h77x_78x_timer.h"
#include "tkl_system.h"
#include "tuya_error_code.h"

#define GD32H77X_TKL_TIMER_COUNT       3U
#define GD32H77X_TKL_TIMER_PRIORITY    6U
#define GD32H77X_TIMER_MAX_TICKS       0x10000U

typedef struct {
    uint32_t timer;
    rcu_periph_enum rcu;
    IRQn_Type irqn;
    TUYA_TIMER_BASE_CFG_T cfg;
    uint32_t interval_us;
    uint32_t quantum_us;
    uint8_t initialized;
    uint8_t running;
} GD32H77X_TKL_TIMER_T;

static GD32H77X_TKL_TIMER_T s_timers[GD32H77X_TKL_TIMER_COUNT] = {
    {TIMER3, RCU_TIMER3, TIMER3_IRQn, {0}, 0U, 1U, 0U, 0U},
    {TIMER4, RCU_TIMER4, TIMER4_IRQn, {0}, 0U, 1U, 0U, 0U},
    {TIMER6, RCU_TIMER6, TIMER6_IRQn, {0}, 0U, 1U, 0U, 0U},
};

static uint32_t __timer_clock(uint32_t timer)
{
    uint32_t ahb = rcu_clock_freq_get(CK_AHB);
    uint32_t pclk = rcu_clock_freq_get(CK_APB1);
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

static GD32H77X_TKL_TIMER_T *__timer_get(TUYA_TIMER_NUM_E timer_id)
{
    if (timer_id >= GD32H77X_TKL_TIMER_COUNT) {
        return NULL;
    }
    return &s_timers[timer_id];
}

static void __timer_irq(GD32H77X_TKL_TIMER_T *timer)
{
    if (timer == NULL || !timer->initialized || !timer->running ||
        timer_interrupt_flag_get(timer->timer, TIMER_INT_FLAG_UP) == RESET) {
        return;
    }

    timer_interrupt_flag_clear(timer->timer, TIMER_INT_FLAG_UP);
    if (timer->cfg.mode == TUYA_TIMER_MODE_ONCE) {
        timer_disable(timer->timer);
        timer->running = 0U;
    }
    if (timer->cfg.cb != NULL) {
        timer->cfg.cb(timer->cfg.args);
    }
}

OPERATE_RET tkl_timer_init(TUYA_TIMER_NUM_E timer_id, TUYA_TIMER_BASE_CFG_T *cfg)
{
    GD32H77X_TKL_TIMER_T *timer;

    if (cfg == NULL || cfg->cb == NULL ||
        (cfg->mode != TUYA_TIMER_MODE_ONCE &&
         cfg->mode != TUYA_TIMER_MODE_PERIOD)) {
        return OPRT_INVALID_PARM;
    }

    timer = __timer_get(timer_id);
    if (timer == NULL) {
        return OPRT_NOT_SUPPORTED;
    }
    if (timer->initialized) {
        (void)tkl_timer_stop(timer_id);
    }

    rcu_periph_clock_enable(timer->rcu);
    timer_deinit(timer->timer);
    timer->cfg = *cfg;
    timer->interval_us = 0U;
    timer->quantum_us = 1U;
    timer->running = 0U;
    timer->initialized = 1U;
    NVIC_ClearPendingIRQ(timer->irqn);
    NVIC_SetPriority(timer->irqn, GD32H77X_TKL_TIMER_PRIORITY);
    return OPRT_OK;
}

OPERATE_RET tkl_timer_start(TUYA_TIMER_NUM_E timer_id, uint32_t us)
{
    GD32H77X_TKL_TIMER_T *timer = __timer_get(timer_id);
    timer_parameter_struct timer_cfg;
    uint32_t timer_clock;
    uint32_t quantum_us;
    uint32_t ticks;
    uint64_t prescaler_div;

    if (timer == NULL || !timer->initialized || us == 0U) {
        return OPRT_INVALID_PARM;
    }
    if (timer->running) {
        (void)tkl_timer_stop(timer_id);
    }

    quantum_us = (us + GD32H77X_TIMER_MAX_TICKS - 1U) /
                 GD32H77X_TIMER_MAX_TICKS;
    if (quantum_us == 0U) {
        quantum_us = 1U;
    }
    ticks = (us + quantum_us - 1U) / quantum_us;
    if (ticks == 0U) {
        ticks = 1U;
    }

    timer_clock = __timer_clock(timer->timer);
    prescaler_div = ((uint64_t)timer_clock * quantum_us + 999999U) /
                    1000000U;
    if (timer_clock == 0U || prescaler_div == 0U ||
        prescaler_div > 0x10000U) {
        return OPRT_NOT_SUPPORTED;
    }

    timer_deinit(timer->timer);
    timer_struct_para_init(&timer_cfg);
    timer_cfg.prescaler = (uint32_t)prescaler_div - 1U;
    timer_cfg.alignedmode = TIMER_COUNTER_EDGE;
    timer_cfg.counterdirection = TIMER_COUNTER_UP;
    timer_cfg.period = ticks - 1U;
    timer_cfg.clockdivision = TIMER_CKDIV_DIV1;
    timer_cfg.repetitioncounter = 0U;
    timer_init(timer->timer, &timer_cfg);

    timer->interval_us = us;
    timer->quantum_us = quantum_us;
    timer_interrupt_flag_clear(timer->timer, TIMER_INT_FLAG_UP);
    timer_interrupt_enable(timer->timer, TIMER_INT_UP);
    NVIC_ClearPendingIRQ(timer->irqn);
    NVIC_EnableIRQ(timer->irqn);
    timer->running = 1U;
    timer_enable(timer->timer);
    return OPRT_OK;
}

OPERATE_RET tkl_timer_stop(TUYA_TIMER_NUM_E timer_id)
{
    GD32H77X_TKL_TIMER_T *timer = __timer_get(timer_id);

    if (timer == NULL || !timer->initialized) {
        return OPRT_INVALID_PARM;
    }

    timer_disable(timer->timer);
    timer_interrupt_disable(timer->timer, TIMER_INT_UP);
    timer_interrupt_flag_clear(timer->timer, TIMER_INT_FLAG_UP);
    NVIC_DisableIRQ(timer->irqn);
    NVIC_ClearPendingIRQ(timer->irqn);
    timer->running = 0U;
    return OPRT_OK;
}

OPERATE_RET tkl_timer_deinit(TUYA_TIMER_NUM_E timer_id)
{
    GD32H77X_TKL_TIMER_T *timer = __timer_get(timer_id);

    if (timer == NULL || !timer->initialized) {
        return OPRT_INVALID_PARM;
    }

    (void)tkl_timer_stop(timer_id);
    timer_deinit(timer->timer);
    timer->initialized = 0U;
    memset(&timer->cfg, 0, sizeof(timer->cfg));
    timer->interval_us = 0U;
    return OPRT_OK;
}

OPERATE_RET tkl_timer_get_current_value(TUYA_TIMER_NUM_E timer_id, uint32_t *us)
{
    GD32H77X_TKL_TIMER_T *timer = __timer_get(timer_id);

    if (timer == NULL || !timer->initialized || us == NULL) {
        return OPRT_INVALID_PARM;
    }

    *us = timer_counter_read(timer->timer) * timer->quantum_us;
    return OPRT_OK;
}

OPERATE_RET tkl_timer_get(TUYA_TIMER_NUM_E timer_id, uint32_t *us)
{
    GD32H77X_TKL_TIMER_T *timer = __timer_get(timer_id);

    if (timer == NULL || !timer->initialized || us == NULL) {
        return OPRT_INVALID_PARM;
    }

    *us = timer->interval_us;
    return OPRT_OK;
}

void TIMER3_IRQHandler(void)
{
    __timer_irq(&s_timers[0]);
}

void TIMER4_IRQHandler(void)
{
    __timer_irq(&s_timers[1]);
}

void TIMER6_IRQHandler(void)
{
    __timer_irq(&s_timers[2]);
}
