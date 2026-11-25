/**
 * @file tkl_uart.c
 * @brief GD32H77X interrupt-driven UART TKL backend.
 *
 * TUYA_UART_NUM_0..7 map one-to-one to USART0, USART1, USART2, UART3, UART4,
 * USART5, UART6 and UART7.  The platform console binding is independent from this mapping.
 *
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "tkl_uart.h"

#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "stream_buffer.h"
#include "task.h"

#include "gd32h77x_78x.h"
#include "gd32h77x_freertos.h"
#include "tkl_pinmux.h"
#include "tuya_error_code.h"
#include "uart_hooks.h"

/* The CI3422 voice link runs at 1 Mbps. Keep at least 32 ms of RX data
 * buffered while the worker drains the stream. */
/* The TKL stream is only the ISR-to-worker handoff.  Keep it small; the
 * per-port TAL ring buffer is the application-facing burst buffer. */
#define UART_RX_STREAM_SIZE 2048U
#define UART_RX_TASK_STACK  512U
#define UART_RX_TASK_PRIO   (tskIDLE_PRIORITY + 5U)
#define UART_IRQ_PRIORITY   5U
#define UART_TX_TIMEOUT_MS  2000U

typedef struct {
    uint32_t port;
    uint32_t pin;
    uint32_t af;
} UART_PIN_T;

typedef struct {
    UART_PIN_T tx;
    UART_PIN_T rx;
} UART_PIN_CFG_T;

typedef struct {
    uint32_t usart;
    uint32_t clock;
    IRQn_Type irqn;
} UART_HW_T;

typedef struct {
    StreamBufferHandle_t stream;
    SemaphoreHandle_t    data_ready;
    SemaphoreHandle_t    callback_lock;
    SemaphoreHandle_t    worker_exited;
    TaskHandle_t         worker;
    TUYA_UART_IRQ_CB     callback;
    volatile uint32_t    rx_bytes;
    volatile uint32_t    rx_stream_dropped;
    volatile uint32_t    rx_overrun_errors;
    volatile uint32_t    rx_frame_errors;
    volatile uint32_t    rx_noise_errors;
    volatile uint32_t    rx_parity_errors;
    volatile uint32_t    rx_callback_count;
    volatile BaseType_t  initializing;
    volatile BaseType_t  initialized;
    volatile BaseType_t  stopping;
} GD32_UART_STATE;

#define UART_PIN(port_name, pin_num, af_val) \
    {port_name, GPIO_PIN_##pin_num, af_val}

static const UART_HW_T s_uart_hw[TUYA_UART_NUM_MAX] = {
    [TUYA_UART_NUM_0] = {USART0, RCU_USART0, USART0_IRQn},
    [TUYA_UART_NUM_1] = {USART1, RCU_USART1, USART1_IRQn},
    [TUYA_UART_NUM_2] = {USART2, RCU_USART2, USART2_IRQn},
    [TUYA_UART_NUM_3] = {UART3, RCU_UART3, UART3_IRQn},
    [TUYA_UART_NUM_4] = {UART4, RCU_UART4, UART4_IRQn},
    [TUYA_UART_NUM_5] = {USART5, RCU_USART5, USART5_IRQn},
    [TUYA_UART_NUM_6] = {UART6, RCU_UART6, UART6_IRQn},
    [TUYA_UART_NUM_7] = {UART7, RCU_UART7, UART7_IRQn},
};

/* Default TX/RX routing for the eight TKL UART instances.  Each entry carries
 * the matching TUYA_GPIO_NUM so pinmux overrides remain self-documenting.
 */
static UART_PIN_CFG_T s_uart_pins[TUYA_UART_NUM_MAX] = {
    [TUYA_UART_NUM_0] = {
        .tx = UART_PIN(GPIOA, 9, 7),   /* PA9 / TUYA_GPIO_NUM_9 / AF7 */
        .rx = UART_PIN(GPIOA, 10, 7),  /* PA10 / TUYA_GPIO_NUM_10 / AF7 */
    },
    [TUYA_UART_NUM_1] = {
        .tx = UART_PIN(GPIOA, 2, 7),   /* PA2 / TUYA_GPIO_NUM_2 / AF7 */
        .rx = UART_PIN(GPIOA, 3, 7),   /* PA3 / TUYA_GPIO_NUM_3 / AF7 */
    },
    [TUYA_UART_NUM_2] = {
        .tx = UART_PIN(GPIOH, 2, 1),   /* PH2 / TUYA_GPIO_NUM_114 / AF1 */
        .rx = UART_PIN(GPIOH, 4, 1),   /* PH4 / TUYA_GPIO_NUM_116 / AF1 */
    },
    [TUYA_UART_NUM_3] = {
        .tx = UART_PIN(GPIOA, 0, 8),   /* PA0 / TUYA_GPIO_NUM_0 / AF8 */
        .rx = UART_PIN(GPIOA, 1, 8),   /* PA1 / TUYA_GPIO_NUM_1 / AF8 */
    },
    [TUYA_UART_NUM_4] = {
        .tx = UART_PIN(GPIOB, 13, 14), /* PB13 / TUYA_GPIO_NUM_29 / AF14 */
        .rx = UART_PIN(GPIOB, 12, 14), /* PB12 / TUYA_GPIO_NUM_28 / AF14 */
    },
    [TUYA_UART_NUM_5] = {
        .tx = UART_PIN(GPIOC, 6, 7),   /* PC6 / TUYA_GPIO_NUM_38 / AF7 */
        .rx = UART_PIN(GPIOC, 7, 7),   /* PC7 / TUYA_GPIO_NUM_39 / AF7 */
    },
    [TUYA_UART_NUM_6] = {
        .tx = UART_PIN(GPIOE, 8, 7),   /* PE8 / TUYA_GPIO_NUM_72 / AF7 */
        .rx = UART_PIN(GPIOE, 7, 7),   /* PE7 / TUYA_GPIO_NUM_71 / AF7 */
    },
    [TUYA_UART_NUM_7] = {
        .tx = UART_PIN(GPIOE, 1, 8),   /* PE1 / TUYA_GPIO_NUM_65 / AF8 */
        .rx = UART_PIN(GPIOE, 0, 8),   /* PE0 / TUYA_GPIO_NUM_64 / AF8 */
    },
};

static GD32_UART_STATE s_uart[TUYA_UART_NUM_MAX];

void __tkl_uart_set_pin(TUYA_UART_NUM_E port, TUYA_PIN_FUNC_E func,
                        uint32_t gpio_port, uint32_t gpio_pin, uint32_t af)
{
    uint32_t controller;
    uint32_t channel;
    UART_PIN_T *pin_cfg;

    if (func < TUYA_UART0_TX || func > TUYA_UART7_CTS ||
        port >= TUYA_UART_NUM_MAX) {
        return;
    }
    controller = (func - TUYA_UART0_TX) / 4U;
    channel = (func - TUYA_UART0_TX) % 4U;
    if (controller != (uint32_t)port || channel > 1U) {
        return;
    }

    pin_cfg = channel == 0U ? &s_uart_pins[port].tx : &s_uart_pins[port].rx;
    pin_cfg->port = gpio_port;
    pin_cfg->pin = gpio_pin;
    pin_cfg->af = af;
}

static bool __uart_port_number(TUYA_UART_NUM_E port_id, uint32_t *port)
{
    if (TUYA_UART_GET_PORT_TYPE(port_id) != TUYA_UART_SYS) {
        return false;
    }
    uint32_t number = TUYA_UART_GET_PORT_NUMBER(port_id);
    if (number >= TUYA_UART_NUM_MAX) {
        return false;
    }
    if (port != NULL) {
        *port = number;
    }
    return true;
}

static uint32_t __uart_gpio_rcu(uint32_t port)
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

static void __uart_apply_pin(const UART_PIN_T *pin, uint32_t speed)
{
    uint32_t rcu = __uart_gpio_rcu(pin->port);

    if (rcu == 0U) {
        return;
    }
    rcu_periph_clock_enable(rcu);
    gpio_af_set(pin->port, pin->af, pin->pin);
    gpio_mode_set(pin->port, GPIO_MODE_AF, GPIO_PUPD_PULLUP, pin->pin);
    gpio_output_options_set(pin->port, GPIO_OTYPE_PP, speed, pin->pin);
}

static uint32_t __uart_tx_speed(uint32_t baudrate)
{
    /* GPIO slew rate is not the baud rate. Choose a conservative edge rate
     * from the configured UART speed so applications do not need board-specific
     * knowledge. High-rate links also cover series resistors and longer traces. */
    if (baudrate > 921600U) {
        return GPIO_OSPEED_100_220MHZ;
    }
    if (baudrate > 460800U) {
        return GPIO_OSPEED_85MHZ;
    }
    if (baudrate > 115200U) {
        return GPIO_OSPEED_60MHZ;
    }
    return GPIO_OSPEED_12MHZ;
}

static void __uart_init_gpio(TUYA_UART_NUM_E port, uint32_t baudrate)
{
    __uart_apply_pin(&s_uart_pins[port].tx, __uart_tx_speed(baudrate));
    __uart_apply_pin(&s_uart_pins[port].rx, GPIO_OSPEED_60MHZ);
}

static uint32_t __uart_word_length(TUYA_UART_DATA_LEN_E databits)
{
    switch (databits) {
        case TUYA_UART_DATA_LEN_7BIT: return USART_WL_7BIT;
        case TUYA_UART_DATA_LEN_8BIT: return USART_WL_8BIT;
        default: return UINT32_MAX;
    }
}

static uint32_t __uart_stop_bits(TUYA_UART_STOP_LEN_E stopbits)
{
    switch (stopbits) {
        case TUYA_UART_STOP_LEN_1BIT: return USART_STB_1BIT;
        case TUYA_UART_STOP_LEN_1_5BIT1: return USART_STB_1_5BIT;
        case TUYA_UART_STOP_LEN_2BIT: return USART_STB_2BIT;
        default: return UINT32_MAX;
    }
}

static uint32_t __uart_parity(TUYA_UART_PARITY_TYPE_E parity)
{
    switch (parity) {
        case TUYA_UART_PARITY_TYPE_NONE: return USART_PM_NONE;
        case TUYA_UART_PARITY_TYPE_ODD: return USART_PM_ODD;
        case TUYA_UART_PARITY_TYPE_EVEN: return USART_PM_EVEN;
        default: return UINT32_MAX;
    }
}

static bool __uart_config_supported(const TUYA_UART_BASE_CFG_T *cfg)
{
    return cfg != NULL && cfg->baudrate != 0U &&
           cfg->flowctrl == TUYA_UART_FLOWCTRL_NONE &&
           __uart_word_length(cfg->databits) != UINT32_MAX &&
           __uart_stop_bits(cfg->stopbits) != UINT32_MAX &&
           __uart_parity(cfg->parity) != UINT32_MAX;
}

static void __uart_configure_hw(TUYA_UART_NUM_E port,
                                const TUYA_UART_BASE_CFG_T *cfg)
{
    const UART_HW_T *hw = &s_uart_hw[port];

    rcu_periph_clock_enable(hw->clock);
    usart_deinit(hw->usart);
    usart_baudrate_set(hw->usart, cfg->baudrate);
    usart_word_length_set(hw->usart, __uart_word_length(cfg->databits));
    usart_stop_bit_set(hw->usart, __uart_stop_bits(cfg->stopbits));
    usart_parity_config(hw->usart, __uart_parity(cfg->parity));
    usart_transmit_config(hw->usart, USART_TRANSMIT_ENABLE);
    usart_receive_config(hw->usart, USART_RECEIVE_ENABLE);
    /* Use the 16-entry hardware RX FIFO plus the receive data buffer.  With
     * FIFO disabled, a 1 Mbps stream has only 10 us of ISR slack per byte. */
    usart_fifo_enable(hw->usart);
    usart_receive_fifo_threshold_config(hw->usart,
                                        USART_RFTCFG_THRESHOLD_1_4);
    usart_enable(hw->usart);
}

static void __uart_rx_worker(void *argument)
{
    uint32_t port = (uint32_t)(uintptr_t)argument - 1U;
    GD32_UART_STATE *state = &s_uart[port];

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        taskENTER_CRITICAL();
        BaseType_t stopping = state->stopping;
        taskEXIT_CRITICAL();
        if (stopping != pdFALSE) {
            break;
        }

        if (xSemaphoreTake(state->callback_lock, portMAX_DELAY) == pdTRUE) {
            TUYA_UART_IRQ_CB callback = state->callback;
            if (callback != NULL &&
                xStreamBufferBytesAvailable(state->stream) != 0U) {
                state->rx_callback_count++;
                callback((TUYA_UART_NUM_E)port);
            }
            xSemaphoreGive(state->callback_lock);
        }
    }

    xSemaphoreGive(state->worker_exited);
    /* The deinit caller owns deletion after observing worker_exited. */
    vTaskSuspend(NULL);
}

static void __uart_irq_handler(uint32_t port)
{
    if (port == TUYA_UART_NUM_1 && gd32h77x_at_bridge_host_rx_isr()) {
        return;
    }

    GD32_UART_STATE *state = &s_uart[port];
    uint32_t usart = s_uart_hw[port].usart;
    BaseType_t higher_priority_task_woken = pdFALSE;
    BaseType_t received = pdFALSE;

    if (state->initialized == pdFALSE || state->stream == NULL) {
        return;
    }

    if (usart_flag_get(usart, USART_FLAG_ORERR) == SET) {
        usart_flag_clear(usart, USART_FLAG_ORERR);
        state->rx_overrun_errors++;
    }
    if (usart_flag_get(usart, USART_FLAG_FERR) == SET) {
        usart_flag_clear(usart, USART_FLAG_FERR);
        state->rx_frame_errors++;
    }
    if (usart_flag_get(usart, USART_FLAG_NERR) == SET) {
        usart_flag_clear(usart, USART_FLAG_NERR);
        state->rx_noise_errors++;
    }
    if (usart_flag_get(usart, USART_FLAG_PERR) == SET) {
        usart_flag_clear(usart, USART_FLAG_PERR);
        state->rx_parity_errors++;
    }

    /*
     * Drain the USART FIFO in batches.  At 1 Mbps a byte arrives every
     * 10 us; calling xStreamBufferSendFromISR() once per byte is slower than
     * the line rate and causes hardware RX overruns.  Read a local batch first,
     * then make one FreeRTOS stream-buffer call for the whole batch.
     */
    while (usart_flag_get(usart, USART_FLAG_RFNE) == SET) {
        uint8_t batch[64];
        size_t batch_len = 0U;

        while (usart_flag_get(usart, USART_FLAG_RFNE) == SET &&
               batch_len < sizeof(batch)) {
            batch[batch_len++] = (uint8_t)usart_data_receive(usart);
        }

        size_t sent = xStreamBufferSendFromISR(state->stream, batch,
                                               batch_len,
                                               &higher_priority_task_woken);
        state->rx_bytes += sent;
        if (sent != 0U) {
            received = pdTRUE;
        }
        if (sent < batch_len) {
            state->rx_stream_dropped += (uint32_t)(batch_len - sent);
        }
    }

    if (received != pdFALSE) {
        (void)xSemaphoreGiveFromISR(state->data_ready,
                                    &higher_priority_task_woken);
        vTaskNotifyGiveFromISR(state->worker, &higher_priority_task_woken);
    }

    portYIELD_FROM_ISR(higher_priority_task_woken);
}

#if defined(__GNUC__)
#define TKL_UART_WEAK __attribute__((weak))
#else
#define TKL_UART_WEAK
#endif

void USART0_IRQHandler(void)
{
    __uart_irq_handler(TUYA_UART_NUM_0);
}

void USART1_IRQHandler(void)
{
    __uart_irq_handler(TUYA_UART_NUM_1);
}

void USART2_IRQHandler(void)
{
    __uart_irq_handler(TUYA_UART_NUM_2);
}

void UART3_IRQHandler(void)
{
    __uart_irq_handler(TUYA_UART_NUM_3);
}

/* The platform AT transport may own UART4 on boards where AT is enabled. */
TKL_UART_WEAK void UART4_IRQHandler(void)
{
    __uart_irq_handler(TUYA_UART_NUM_4);
}

void USART5_IRQHandler(void)
{
    __uart_irq_handler(TUYA_UART_NUM_5);
}

void UART6_IRQHandler(void)
{
    __uart_irq_handler(TUYA_UART_NUM_6);
}

void UART7_IRQHandler(void)
{
    __uart_irq_handler(TUYA_UART_NUM_7);
}

void gd32h77x_uart_get_port_stats(uint32_t port_id, gd32h77x_uart_stats_t *stats)
{
    uint32_t port;

    if (stats == NULL || !__uart_port_number((TUYA_UART_NUM_E)port_id, &port)) {
        return;
    }

    memset(stats, 0, sizeof(*stats));
    taskENTER_CRITICAL();
    stats->rx_bytes = s_uart[port].rx_bytes;
    stats->rx_stream_dropped = s_uart[port].rx_stream_dropped;
    stats->rx_overrun_errors = s_uart[port].rx_overrun_errors;
    stats->rx_frame_errors = s_uart[port].rx_frame_errors;
    stats->rx_noise_errors = s_uart[port].rx_noise_errors;
    stats->rx_parity_errors = s_uart[port].rx_parity_errors;
    stats->rx_callback_count = s_uart[port].rx_callback_count;
    taskEXIT_CRITICAL();
}

void gd32h77x_uart_get_stats(gd32h77x_uart_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }

    memset(stats, 0, sizeof(*stats));
    taskENTER_CRITICAL();
    for (uint32_t port = 0U; port < TUYA_UART_NUM_MAX; ++port) {
        stats->rx_bytes += s_uart[port].rx_bytes;
        stats->rx_stream_dropped += s_uart[port].rx_stream_dropped;
        stats->rx_overrun_errors += s_uart[port].rx_overrun_errors;
        stats->rx_frame_errors += s_uart[port].rx_frame_errors;
        stats->rx_noise_errors += s_uart[port].rx_noise_errors;
        stats->rx_parity_errors += s_uart[port].rx_parity_errors;
        stats->rx_callback_count += s_uart[port].rx_callback_count;
    }
    taskEXIT_CRITICAL();
}

OPERATE_RET tkl_uart_init(TUYA_UART_NUM_E port_id, TUYA_UART_BASE_CFG_T *cfg)
{
    uint32_t port;

    if (!__uart_port_number(port_id, &port) ||
        !__uart_config_supported(cfg)) {
        return OPRT_INVALID_PARM;
    }

    GD32_UART_STATE *state = &s_uart[port];
    taskENTER_CRITICAL();
    if (state->initializing != pdFALSE || state->initialized != pdFALSE ||
        state->stopping != pdFALSE) {
        taskEXIT_CRITICAL();
        return OPRT_INIT_MORE_THAN_ONCE;
    }
    state->initializing = pdTRUE;
    taskEXIT_CRITICAL();

    StreamBufferHandle_t stream = xStreamBufferCreate(UART_RX_STREAM_SIZE, 1U);
    SemaphoreHandle_t data_ready = xSemaphoreCreateBinary();
    SemaphoreHandle_t callback_lock = xSemaphoreCreateMutex();
    SemaphoreHandle_t worker_exited = xSemaphoreCreateBinary();
    TaskHandle_t worker = NULL;

    if (stream == NULL || data_ready == NULL || callback_lock == NULL ||
        worker_exited == NULL) {
        if (stream != NULL) {
            vStreamBufferDelete(stream);
        }
        if (data_ready != NULL) {
            vSemaphoreDelete(data_ready);
        }
        if (callback_lock != NULL) {
            vSemaphoreDelete(callback_lock);
        }
        if (worker_exited != NULL) {
            vSemaphoreDelete(worker_exited);
        }
        taskENTER_CRITICAL();
        state->initializing = pdFALSE;
        taskEXIT_CRITICAL();
        return OPRT_MALLOC_FAILED;
    }

    if (xTaskCreate(__uart_rx_worker, "uart_rx", UART_RX_TASK_STACK,
                    (void *)(uintptr_t)(port + 1U), UART_RX_TASK_PRIO,
                    &worker) != pdPASS) {
        vStreamBufferDelete(stream);
        vSemaphoreDelete(data_ready);
        vSemaphoreDelete(callback_lock);
        vSemaphoreDelete(worker_exited);
        taskENTER_CRITICAL();
        state->initializing = pdFALSE;
        taskEXIT_CRITICAL();
        return OPRT_MALLOC_FAILED;
    }

    __uart_init_gpio((TUYA_UART_NUM_E)port, cfg->baudrate);
    __uart_configure_hw((TUYA_UART_NUM_E)port, cfg);

    taskENTER_CRITICAL();
    state->stream = stream;
    state->data_ready = data_ready;
    state->callback_lock = callback_lock;
    state->worker_exited = worker_exited;
    state->worker = worker;
    state->callback = NULL;
    state->rx_bytes = 0U;
    state->rx_stream_dropped = 0U;
    state->rx_overrun_errors = 0U;
    state->rx_frame_errors = 0U;
    state->rx_noise_errors = 0U;
    state->rx_parity_errors = 0U;
    state->rx_callback_count = 0U;
    state->stopping = pdFALSE;
    state->initialized = pdTRUE;
    state->initializing = pdFALSE;
    usart_interrupt_enable(s_uart_hw[port].usart, USART_INT_RFNE);
    usart_interrupt_enable(s_uart_hw[port].usart, USART_INT_ERR);
    NVIC_ClearPendingIRQ(s_uart_hw[port].irqn);
    NVIC_SetPriority(s_uart_hw[port].irqn, UART_IRQ_PRIORITY);
    NVIC_EnableIRQ(s_uart_hw[port].irqn);
    taskEXIT_CRITICAL();

    return OPRT_OK;
}

OPERATE_RET tkl_uart_deinit(TUYA_UART_NUM_E port_id)
{
    uint32_t port;

    if (!__uart_port_number(port_id, &port)) {
        return OPRT_INVALID_PARM;
    }
    if (xPortIsInsideInterrupt() != pdFALSE) {
        return OPRT_NOT_SUPPORTED;
    }

    GD32_UART_STATE *state = &s_uart[port];
    taskENTER_CRITICAL();
    if (state->initialized == pdFALSE || state->stopping != pdFALSE) {
        taskEXIT_CRITICAL();
        return OPRT_RESOURCE_NOT_READY;
    }
    state->stopping = pdTRUE;
    usart_interrupt_disable(s_uart_hw[port].usart, USART_INT_RFNE);
    usart_interrupt_disable(s_uart_hw[port].usart, USART_INT_ERR);
    NVIC_DisableIRQ(s_uart_hw[port].irqn);
    NVIC_ClearPendingIRQ(s_uart_hw[port].irqn);
    taskEXIT_CRITICAL();

    (void)xSemaphoreTake(state->callback_lock, portMAX_DELAY);
    state->callback = NULL;
    xSemaphoreGive(state->callback_lock);
    xTaskNotifyGive(state->worker);
    (void)xSemaphoreTake(state->worker_exited, portMAX_DELAY);

    vTaskDelete(state->worker);
    vStreamBufferDelete(state->stream);
    vSemaphoreDelete(state->data_ready);
    vSemaphoreDelete(state->callback_lock);
    vSemaphoreDelete(state->worker_exited);

    taskENTER_CRITICAL();
    state->stream = NULL;
    state->data_ready = NULL;
    state->callback_lock = NULL;
    state->worker_exited = NULL;
    state->worker = NULL;
    state->initialized = pdFALSE;
    state->stopping = pdFALSE;
    taskEXIT_CRITICAL();

    /* Keep the peripheral enabled in case this instance is the platform
     * console.  Only TKL RX interrupt delivery is stopped. */
    return OPRT_OK;
}

int tkl_uart_write(TUYA_UART_NUM_E port_id, void *buff, uint16_t len)
{
    uint32_t port;

    if (!__uart_port_number(port_id, &port)) {
        return OPRT_INVALID_PARM;
    }
    if (buff == NULL && len != 0U) {
        return OPRT_INVALID_PARM;
    }
    if (len == 0U) {
        return 0;
    }
    if (s_uart[port].initialized == pdFALSE ||
        xPortIsInsideInterrupt() != pdFALSE) {
        return OPRT_RESOURCE_NOT_READY;
    }

    const uint8_t *bytes = (const uint8_t *)buff;
    uint32_t usart = s_uart_hw[port].usart;
    TickType_t start = xTaskGetTickCount();
    for (uint16_t sent = 0U; sent < len; ++sent) {
        while (usart_flag_get(usart, USART_FLAG_TBE) == RESET) {
            if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(UART_TX_TIMEOUT_MS)) {
                return OPRT_COM_ERROR;
            }
        }
        usart_data_transmit(usart, bytes[sent]);
    }
    return (int)len;
}

void tkl_uart_rx_irq_cb_reg(TUYA_UART_NUM_E port_id, TUYA_UART_IRQ_CB rx_cb)
{
    uint32_t port;

    if (!__uart_port_number(port_id, &port) ||
        s_uart[port].initialized == pdFALSE ||
        xPortIsInsideInterrupt() != pdFALSE) {
        return;
    }

    GD32_UART_STATE *state = &s_uart[port];
    if (xSemaphoreTake(state->callback_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    taskENTER_CRITICAL();
    if (state->initialized != pdFALSE && state->stopping == pdFALSE) {
        state->callback = rx_cb;
    }
    taskEXIT_CRITICAL();

    if (rx_cb != NULL &&
        xStreamBufferBytesAvailable(state->stream) != 0U) {
        xTaskNotifyGive(state->worker);
    }
    xSemaphoreGive(state->callback_lock);
}

void tkl_uart_tx_irq_cb_reg(TUYA_UART_NUM_E port_id, TUYA_UART_IRQ_CB tx_cb)
{
    (void)port_id;
    (void)tx_cb;
}

int tkl_uart_read(TUYA_UART_NUM_E port_id, void *buff, uint16_t len)
{
    uint32_t port;

    if (!__uart_port_number(port_id, &port)) {
        return OPRT_INVALID_PARM;
    }
    if (buff == NULL && len != 0U) {
        return OPRT_INVALID_PARM;
    }
    if (len == 0U) {
        return 0;
    }
    if (s_uart[port].initialized == pdFALSE ||
        xPortIsInsideInterrupt() != pdFALSE) {
        return OPRT_RESOURCE_NOT_READY;
    }

    return (int)xStreamBufferReceive(s_uart[port].stream, buff, len, 0U);
}

OPERATE_RET tkl_uart_set_tx_int(TUYA_UART_NUM_E port_id, BOOL_T enable)
{
    (void)port_id;
    (void)enable;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_uart_set_rx_flowctrl(TUYA_UART_NUM_E port_id, BOOL_T enable)
{
    uint32_t port;

    if (!__uart_port_number(port_id, &port)) {
        return OPRT_INVALID_PARM;
    }
    return enable == FALSE ? OPRT_OK : OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_uart_wait_for_data(TUYA_UART_NUM_E port_id, int timeout_ms)
{
    uint32_t port;

    if (!__uart_port_number(port_id, &port)) {
        return OPRT_INVALID_PARM;
    }
    if (timeout_ms < -1) {
        return OPRT_INVALID_PARM;
    }

    GD32_UART_STATE *state = &s_uart[port];
    if (state->initialized == pdFALSE ||
        xPortIsInsideInterrupt() != pdFALSE) {
        return OPRT_RESOURCE_NOT_READY;
    }

    TickType_t start = xTaskGetTickCount();
    TickType_t limit = timeout_ms < 0 ? portMAX_DELAY :
                       gd32h77x_timeout_ticks((uint32_t)timeout_ms);
    for (;;) {
        if (xStreamBufferBytesAvailable(state->stream) != 0U) {
            return OPRT_OK;
        }
        if (timeout_ms == 0) {
            return OPRT_TIMEOUT;
        }

        TickType_t wait_ticks = limit;
        if (timeout_ms > 0) {
            TickType_t elapsed = xTaskGetTickCount() - start;
            if (elapsed >= limit) {
                return OPRT_TIMEOUT;
            }
            wait_ticks = limit - elapsed;
        }
        if (xSemaphoreTake(state->data_ready, wait_ticks) != pdTRUE &&
            timeout_ms > 0) {
            return OPRT_TIMEOUT;
        }
    }
}

OPERATE_RET tkl_uart_ioctl(TUYA_UART_NUM_E port_id, uint32_t cmd, void *arg)
{
    (void)port_id;
    (void)cmd;
    (void)arg;
    return OPRT_NOT_SUPPORTED;
}
