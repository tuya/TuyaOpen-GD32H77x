/**
 * @file gd32h77x_dma.c
 * @brief Internal GD32H77X DMA resource manager.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "gd32h77x_dma.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "gd32h77x_78x_dma.h"

#define GD32H77X_DMA_POOL_SIZE 14U
#define GD32H77X_DMA_ERROR_INTS \
    (DMA_INT_FEE | DMA_INT_SDE | DMA_INT_TAE)
#define GD32H77X_DMA_ALL_INTS \
    (GD32H77X_DMA_ERROR_INTS | DMA_INT_HTF | DMA_INT_FTF)
#define GD32H77X_DMA_ERROR_FLAGS \
    (DMA_INT_FLAG_FEE | DMA_INT_FLAG_SDE | DMA_INT_FLAG_TAE)
#define GD32H77X_DMA_ALL_FLAGS \
    (GD32H77X_DMA_ERROR_FLAGS | DMA_INT_FLAG_HTF | DMA_INT_FLAG_FTF)

typedef struct {
    uint32_t dma_periph;
    dma_channel_enum channel;
    IRQn_Type irqn;
} GD32H77X_DMA_RESOURCE_T;

static const GD32H77X_DMA_RESOURCE_T s_dma_resources[GD32H77X_DMA_POOL_SIZE] = {
    {DMA0, DMA_CH0, DMA0_Channel0_IRQn},
    {DMA0, DMA_CH1, DMA0_Channel1_IRQn},
    {DMA0, DMA_CH2, DMA0_Channel2_IRQn},
    {DMA0, DMA_CH3, DMA0_Channel3_IRQn},
    {DMA0, DMA_CH4, DMA0_Channel4_IRQn},
    {DMA0, DMA_CH5, DMA0_Channel5_IRQn},
    {DMA0, DMA_CH6, DMA0_Channel6_IRQn},
    {DMA1, DMA_CH0, DMA1_Channel0_IRQn},
    {DMA1, DMA_CH1, DMA1_Channel1_IRQn},
    {DMA1, DMA_CH2, DMA1_Channel2_IRQn},
    {DMA1, DMA_CH3, DMA1_Channel3_IRQn},
    {DMA1, DMA_CH4, DMA1_Channel4_IRQn},
    {DMA1, DMA_CH5, DMA1_Channel5_IRQn},
    {DMA1, DMA_CH6, DMA1_Channel6_IRQn},
};

static gd32h77x_dma_handle_t *s_dma_owners[GD32H77X_DMA_POOL_SIZE];

static uint32_t __dma_enter_critical(void)
{
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    return mask;
}

static void __dma_exit_critical(uint32_t mask)
{
    __set_PRIMASK(mask);
}

static int32_t __dma_owner_index(uint32_t dma_periph,
                                 dma_channel_enum channel)
{
    for (uint32_t i = 0U; i < GD32H77X_DMA_POOL_SIZE; ++i) {
        if (s_dma_resources[i].dma_periph == dma_periph &&
            s_dma_resources[i].channel == channel) {
            return (int32_t)i;
        }
    }
    return -1;
}

static void __dma_clear_channel(uint32_t dma_periph,
                                dma_channel_enum channel)
{
    dma_channel_disable(dma_periph, channel);
    dma_interrupt_disable(dma_periph, channel, GD32H77X_DMA_ALL_INTS);
    dma_interrupt_flag_clear(dma_periph, channel, GD32H77X_DMA_ALL_FLAGS);
    dma_flag_clear(dma_periph, channel,
                   DMA_FLAG_FEE | DMA_FLAG_SDE | DMA_FLAG_TAE |
                   DMA_FLAG_HTF | DMA_FLAG_FTF);
}

static void __dma_irq_dispatch(uint32_t dma_periph,
                               dma_channel_enum channel)
{
    int32_t index = __dma_owner_index(dma_periph, channel);
    gd32h77x_dma_handle_t *handle;
    GD32H77X_DMA_EVENT_E event;

    if (index < 0 || s_dma_owners[index] == NULL) {
        __dma_clear_channel(dma_periph, channel);
        return;
    }

    handle = s_dma_owners[index];
    if (dma_interrupt_flag_get(dma_periph, channel,
                               DMA_INT_FLAG_FTF) == SET) {
        event = GD32H77X_DMA_EVENT_COMPLETE;
    } else if (dma_interrupt_flag_get(dma_periph, channel,
                                      DMA_INT_FLAG_TAE) == SET ||
               ((dma_interrupt_flag_get(dma_periph, channel,
                                        DMA_INT_FLAG_FEE |
                                        DMA_INT_FLAG_SDE) == SET) &&
                (DMA_CHCTL(dma_periph, channel) &
                 DMA_CHXCTL_CHEN) == RESET)) {
        event = GD32H77X_DMA_EVENT_ERROR;
    } else {
        /* FEEIF also reports non-fatal FIFO underflow/overflow anomalies.
         * Keep the channel running and only clear the status source here. */
        dma_interrupt_flag_clear(dma_periph, channel,
                                 DMA_INT_FLAG_FEE | DMA_INT_FLAG_SDE |
                                 DMA_INT_FLAG_HTF);
        return;
    }

    dma_channel_disable(dma_periph, channel);
    dma_interrupt_disable(dma_periph, channel, GD32H77X_DMA_ALL_INTS);
    dma_interrupt_flag_clear(dma_periph, channel,
                             GD32H77X_DMA_ALL_FLAGS);
    NVIC_DisableIRQ(handle->irqn);
    handle->irq_enabled = 0U;

    if (handle->callback != NULL) {
        handle->callback(handle, event, handle->arg);
    }
}

int gd32h77x_dma_claim(gd32h77x_dma_handle_t *handle,
                               uint32_t request,
                               GD32H77X_DMA_CB callback, void *arg)
{
    uint32_t mask;
    int32_t index = -1;

    if (handle == NULL || request == 0U) {
        return GD32H77X_DMA_INVALID;
    }

    mask = __dma_enter_critical();
    for (uint32_t i = 0U; i < GD32H77X_DMA_POOL_SIZE; ++i) {
        if (s_dma_owners[i] == NULL) {
            index = (int32_t)i;
            break;
        }
    }
    if (index < 0) {
        __dma_exit_critical(mask);
        return GD32H77X_DMA_ERROR;
    }

    memset(handle, 0, sizeof(*handle));
    handle->dma_periph = s_dma_resources[index].dma_periph;
    handle->channel = s_dma_resources[index].channel;
    handle->irqn = s_dma_resources[index].irqn;
    handle->request = request;
    handle->callback = callback;
    handle->arg = arg;
    handle->claimed = 1U;
    s_dma_owners[index] = handle;
    __dma_exit_critical(mask);

    rcu_periph_clock_enable(RCU_DMA0);
    rcu_periph_clock_enable(RCU_DMA1);
    rcu_periph_clock_enable(RCU_DMAMUX);
    __dma_clear_channel(handle->dma_periph, handle->channel);
    dma_channel_deinit(handle->dma_periph, handle->channel);
    return GD32H77X_DMA_OK;
}

int gd32h77x_dma_single_config(gd32h77x_dma_handle_t *handle,
                                       const GD32H77X_DMA_SINGLE_CFG_T *cfg)
{
    dma_single_data_parameter_struct dma_cfg;

    if (handle == NULL || !handle->claimed || cfg == NULL ||
        cfg->frames == 0U || cfg->frames > 0xFFFFU) {
        return GD32H77X_DMA_INVALID;
    }

    dma_single_data_para_struct_init(&dma_cfg);
    dma_cfg.request = handle->request;
    dma_cfg.periph_addr = cfg->periph_addr;
    dma_cfg.periph_inc = DMA_PERIPH_INCREASE_DISABLE;
    dma_cfg.periph_memory_width = cfg->width;
    dma_cfg.memory0_addr = cfg->memory_addr;
    dma_cfg.memory_inc = cfg->memory_increment ?
                         DMA_MEMORY_INCREASE_ENABLE :
                         DMA_MEMORY_INCREASE_DISABLE;
    dma_cfg.circular_mode = DMA_CIRCULAR_MODE_DISABLE;
    dma_cfg.direction = cfg->direction;
    dma_cfg.number = cfg->frames;
    dma_cfg.priority = cfg->priority;
    dma_single_data_mode_init(handle->dma_periph, handle->channel, &dma_cfg);
    return GD32H77X_DMA_OK;
}

int gd32h77x_dma_start(gd32h77x_dma_handle_t *handle, bool irq_enable)
{
    if (handle == NULL || !handle->claimed) {
        return GD32H77X_DMA_INVALID;
    }
    if (irq_enable && handle->callback == NULL) {
        return GD32H77X_DMA_INVALID;
    }

    __dma_clear_channel(handle->dma_periph, handle->channel);
    if (irq_enable) {
        dma_interrupt_enable(handle->dma_periph, handle->channel,
                             GD32H77X_DMA_ALL_INTS);
        NVIC_ClearPendingIRQ(handle->irqn);
        NVIC_SetPriority(handle->irqn, 6U);
        NVIC_EnableIRQ(handle->irqn);
        handle->irq_enabled = 1U;
    }
    dma_channel_enable(handle->dma_periph, handle->channel);
    return GD32H77X_DMA_OK;
}

int gd32h77x_dma_wait(gd32h77x_dma_handle_t *handle,
                              uint32_t timeout_ms)
{
    uint32_t start_ms;

    if (handle == NULL || !handle->claimed) {
        return GD32H77X_DMA_INVALID;
    }

    start_ms = (uint32_t)xTaskGetTickCount();
    while (1) {
        /* FTF must win over FEE: a FIFO anomaly can be raised while the
         * transfer itself has already completed successfully. */
        if (dma_flag_get(handle->dma_periph, handle->channel,
                         DMA_FLAG_FTF) == SET) {
            return GD32H77X_DMA_OK;
        }
        if (dma_flag_get(handle->dma_periph, handle->channel,
                         DMA_FLAG_TAE) != RESET ||
            ((DMA_CHCTL(handle->dma_periph, handle->channel) &
              DMA_CHXCTL_CHEN) == RESET &&
             DMA_CHCNT(handle->dma_periph, handle->channel) != 0U)) {
            return GD32H77X_DMA_ERROR;
        }
        if (((uint32_t)xTaskGetTickCount() - start_ms) * portTICK_PERIOD_MS >= timeout_ms) {
            return GD32H77X_DMA_TIMEOUT;
        }
    }
}

void gd32h77x_dma_stop(gd32h77x_dma_handle_t *handle)
{
    if (handle == NULL || !handle->claimed) {
        return;
    }
    __dma_clear_channel(handle->dma_periph, handle->channel);
    NVIC_DisableIRQ(handle->irqn);
    NVIC_ClearPendingIRQ(handle->irqn);
    handle->irq_enabled = 0U;
}

void gd32h77x_dma_release(gd32h77x_dma_handle_t *handle)
{
    uint32_t mask;

    if (handle == NULL || !handle->claimed) {
        return;
    }

    gd32h77x_dma_stop(handle);
    mask = __dma_enter_critical();
    for (uint32_t i = 0U; i < GD32H77X_DMA_POOL_SIZE; ++i) {
        if (s_dma_owners[i] == handle) {
            s_dma_owners[i] = NULL;
            break;
        }
    }
    memset(handle, 0, sizeof(*handle));
    __dma_exit_critical(mask);
}

void DMA0_Channel0_IRQHandler(void) { __dma_irq_dispatch(DMA0, DMA_CH0); }
void DMA0_Channel1_IRQHandler(void) { __dma_irq_dispatch(DMA0, DMA_CH1); }
void DMA0_Channel2_IRQHandler(void) { __dma_irq_dispatch(DMA0, DMA_CH2); }
void DMA0_Channel3_IRQHandler(void) { __dma_irq_dispatch(DMA0, DMA_CH3); }
void DMA0_Channel4_IRQHandler(void) { __dma_irq_dispatch(DMA0, DMA_CH4); }
void DMA0_Channel5_IRQHandler(void) { __dma_irq_dispatch(DMA0, DMA_CH5); }
void DMA0_Channel6_IRQHandler(void) { __dma_irq_dispatch(DMA0, DMA_CH6); }
void DMA1_Channel0_IRQHandler(void) { __dma_irq_dispatch(DMA1, DMA_CH0); }
void DMA1_Channel1_IRQHandler(void) { __dma_irq_dispatch(DMA1, DMA_CH1); }
void DMA1_Channel2_IRQHandler(void) { __dma_irq_dispatch(DMA1, DMA_CH2); }
void DMA1_Channel3_IRQHandler(void) { __dma_irq_dispatch(DMA1, DMA_CH3); }
void DMA1_Channel4_IRQHandler(void) { __dma_irq_dispatch(DMA1, DMA_CH4); }
void DMA1_Channel5_IRQHandler(void) { __dma_irq_dispatch(DMA1, DMA_CH5); }
void DMA1_Channel6_IRQHandler(void) { __dma_irq_dispatch(DMA1, DMA_CH6); }
