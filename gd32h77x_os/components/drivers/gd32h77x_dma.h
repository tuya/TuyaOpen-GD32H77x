/**
 * @file gd32h77x_dma.h
 * @brief Internal GD32H77X DMA resource manager.
 *
 * Owns DMA0/DMA1 channels 0..6 and dispatches their interrupts. Channel 7 is
 * intentionally excluded because DMA0_CH7 is used by the AT UART transport and
 * DMA1_CH7 by DVP.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#ifndef __GD32H77X_DMA_H__
#define __GD32H77X_DMA_H__

#include <stdbool.h>

#include "gd32h77x_78x.h"
#include "gd32h77x_78x_dma.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gd32h77x_dma_handle gd32h77x_dma_handle_t;

typedef enum {
    GD32H77X_DMA_OK = 0,
    GD32H77X_DMA_INVALID,
    GD32H77X_DMA_BUSY,
    GD32H77X_DMA_TIMEOUT,
    GD32H77X_DMA_ERROR,
} GD32H77X_DMA_STATUS_E;

typedef enum {
    GD32H77X_DMA_EVENT_COMPLETE = 0,
    GD32H77X_DMA_EVENT_ERROR,
} GD32H77X_DMA_EVENT_E;

typedef void (*GD32H77X_DMA_CB)(gd32h77x_dma_handle_t *handle,
                                  GD32H77X_DMA_EVENT_E event, void *arg);

struct gd32h77x_dma_handle {
    uint32_t dma_periph;
    dma_channel_enum channel;
    IRQn_Type irqn;
    uint32_t request;
    GD32H77X_DMA_CB callback;
    void *arg;
    uint8_t claimed;
    uint8_t irq_enabled;
};

typedef struct {
    uint32_t periph_addr;
    uint32_t memory_addr;
    bool memory_increment;
    uint32_t width;
    uint32_t direction;
    uint32_t frames;
    uint32_t priority;
    uint32_t memory_burst;
    uint32_t periph_burst;
} GD32H77X_DMA_SINGLE_CFG_T;

/**
 * @brief Claim one free DMA channel for a peripheral request.
 *
 * The callback is optional. A non-NULL callback is invoked from the DMA IRQ
 * after the helper disables the channel and clears its interrupt flags.
 */
int gd32h77x_dma_claim(gd32h77x_dma_handle_t *handle,
                               uint32_t request,
                               GD32H77X_DMA_CB callback, void *arg);

/**
 * @brief Configure the claimed channel in DMA single-data mode.
 */
int gd32h77x_dma_single_config(gd32h77x_dma_handle_t *handle,
                                       const GD32H77X_DMA_SINGLE_CFG_T *cfg);

/**
 * @brief Enable the claimed channel.
 *
 * @param irq_enable true to enable completion/error IRQ and NVIC delivery.
 */
int gd32h77x_dma_start(gd32h77x_dma_handle_t *handle, bool irq_enable);

/**
 * @brief Wait for completion in polling mode.
 *
 * @return GD32H77X_DMA_OK on full transfer, GD32H77X_DMA_TIMEOUT on timeout,
 *         GD32H77X_DMA_ERROR on DMA error.
 */
int gd32h77x_dma_wait(gd32h77x_dma_handle_t *handle,
                              uint32_t timeout_ms);

/**
 * @brief Disable the channel, clear flags and disable its IRQ.
 */
void gd32h77x_dma_stop(gd32h77x_dma_handle_t *handle);

/**
 * @brief Release a claimed channel back to the pool.
 */
void gd32h77x_dma_release(gd32h77x_dma_handle_t *handle);

#ifdef __cplusplus
}
#endif

#endif /* __GD32H77X_DMA_H__ */
