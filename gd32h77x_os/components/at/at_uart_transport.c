/**
 * @file at_uart_transport.c
 * @brief UART implementation of the AT transport contract.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */
#include "at_uart_transport.h"

#include "gd32h77x_78x_dma.h"
#include "gd32h77x_config.h"
#include "FreeRTOS.h"
#include "task.h"

#include <stddef.h>
#include <string.h>

#define AT_UART_RX_DMA_PERIPH DMA0
#define AT_UART_RX_DMA_CH DMA_CH7
#define AT_UART_RX_DMA_IRQ DMA0_Channel7_IRQn
#define AT_UART_RX_DMA_SIZE 4096U

typedef struct {
    at_uart_config_t config;
    bool opened;
    uint8_t rx_dma_buffer[AT_UART_RX_DMA_SIZE] __attribute__((aligned(4)));
    volatile uint32_t rx_dma_pos;
    volatile uint32_t rx_dma_burst_high_water;
} at_uart_context_t;

static at_uart_context_t s_uart;
static at_transport_t s_transport;

static uint32_t uart_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void uart_notify_fault(uint32_t faults)
{
    if (s_transport.fault_callback && faults) {
        s_transport.fault_callback(s_transport.fault_context, faults);
    }
}

static uint32_t uart_dma_drain(void)
{
    const uint32_t written = AT_UART_RX_DMA_SIZE -
                             dma_transfer_number_get(AT_UART_RX_DMA_PERIPH, AT_UART_RX_DMA_CH);
    uint32_t burst_size = (written + AT_UART_RX_DMA_SIZE - s_uart.rx_dma_pos) % AT_UART_RX_DMA_SIZE;
    if (written == AT_UART_RX_DMA_SIZE && s_uart.rx_dma_pos == 0U) {
        burst_size = AT_UART_RX_DMA_SIZE;
    }
    if (burst_size > s_uart.rx_dma_burst_high_water) {
        s_uart.rx_dma_burst_high_water = burst_size;
    }
    uint32_t copied = 0U;
    while (s_uart.rx_dma_pos != written) {
        const uint32_t start = s_uart.rx_dma_pos;
        uint32_t contiguous = written > start ? written - start : AT_UART_RX_DMA_SIZE - start;
        if (s_transport.rx_callback) {
            s_transport.rx_callback(s_transport.rx_context, &s_uart.rx_dma_buffer[start], contiguous);
        }
        copied += contiguous;
        s_uart.rx_dma_pos = (start + contiguous) % AT_UART_RX_DMA_SIZE;
    }
    if (copied > AT_UART_RX_DMA_SIZE) {
        uart_notify_fault(AT_TRANSPORT_FAULT_RX_OVERFLOW);
    }
    return copied;
}

static void uart_dma_configure(void)
{
    dma_single_data_parameter_struct init;
    rcu_periph_clock_enable(RCU_DMA0);
    rcu_periph_clock_enable(RCU_DMAMUX);
    dma_deinit(AT_UART_RX_DMA_PERIPH);
    dma_single_data_para_struct_init(&init);
    init.periph_addr = (uint32_t)&USART_RDATA(s_uart.config.usart);
    init.periph_inc = DMA_PERIPH_INCREASE_DISABLE;
    init.memory0_addr = (uint32_t)s_uart.rx_dma_buffer;
    init.memory_inc = DMA_MEMORY_INCREASE_ENABLE;
    init.periph_memory_width = DMA_PERIPH_WIDTH_8BIT;
    init.circular_mode = DMA_CIRCULAR_MODE_ENABLE;
    init.direction = DMA_PERIPH_TO_MEMORY;
    init.number = AT_UART_RX_DMA_SIZE;
    init.priority = DMA_PRIORITY_HIGH;
    dma_single_data_mode_init(AT_UART_RX_DMA_PERIPH, AT_UART_RX_DMA_CH, &init);
    dma_circulation_enable(AT_UART_RX_DMA_PERIPH, AT_UART_RX_DMA_CH);
    DMAMUX_RM_CH7CFG = AT_UART_DMA_RX_REQUEST;
    dma_channel_enable(AT_UART_RX_DMA_PERIPH, AT_UART_RX_DMA_CH);
    s_uart.rx_dma_pos = 0U;
    usart_dma_receive_config(s_uart.config.usart, USART_RECEIVE_DMA_ENABLE);
    dma_interrupt_enable(AT_UART_RX_DMA_PERIPH, AT_UART_RX_DMA_CH, DMA_INT_FTF);
    nvic_irq_enable(AT_UART_RX_DMA_IRQ, s_uart.config.irq_priority, 0U);
}

static int uart_open(at_transport_t *transport)
{
    (void)transport;
    at_uart_config_t *config = &s_uart.config;
    rcu_periph_clock_enable(config->gpio_clock);
    rcu_periph_clock_enable(config->rx_gpio_clock);
    rcu_periph_clock_enable(config->usart_clock);
    gpio_af_set(config->gpio_port, config->alternate_function, config->tx_pin);
    gpio_af_set(config->rx_gpio_port, config->rx_alternate_function, config->rx_pin);
    gpio_mode_set(config->gpio_port, GPIO_MODE_AF, GPIO_PUPD_PULLUP, config->tx_pin);
    gpio_mode_set(config->rx_gpio_port, GPIO_MODE_AF, GPIO_PUPD_PULLUP, config->rx_pin);
    gpio_output_options_set(config->gpio_port, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ,
                            config->tx_pin);
    gpio_output_options_set(config->rx_gpio_port, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ,
                            config->rx_pin);
    usart_deinit(config->usart);
    usart_baudrate_set(config->usart, config->baudrate);
    usart_word_length_set(config->usart, config->word_length);
    usart_stop_bit_set(config->usart, config->stop_bits);
    usart_parity_config(config->usart, config->parity);
    usart_transmit_config(config->usart, USART_TRANSMIT_ENABLE);
    usart_receive_config(config->usart, USART_RECEIVE_ENABLE);
    usart_fifo_enable(config->usart);
    usart_receive_fifo_threshold_config(config->usart, 0U);
    uart_dma_configure();
    usart_interrupt_enable(config->usart, USART_INT_IDLE);
    usart_interrupt_enable(config->usart, USART_INT_ERR);
    nvic_irq_enable(config->irq, config->irq_priority, 0U);
    usart_enable(config->usart);
    s_uart.opened = true;
    return AT_TRANSPORT_OK;
}

static void uart_close(at_transport_t *transport)
{
    (void)transport;
    if (!s_uart.opened) return;
    usart_interrupt_disable(s_uart.config.usart, USART_INT_RBNE);
    usart_interrupt_disable(s_uart.config.usart, USART_INT_IDLE);
    usart_interrupt_disable(s_uart.config.usart, USART_INT_ERR);
    nvic_irq_disable(s_uart.config.irq);
    nvic_irq_disable(AT_UART_RX_DMA_IRQ);
    dma_channel_disable(AT_UART_RX_DMA_PERIPH, AT_UART_RX_DMA_CH);
    s_uart.opened = false;
}

static int uart_write(at_transport_t *transport, const uint8_t *data, size_t length,
                      uint32_t timeout_ms, size_t *sent)
{
    (void)transport;
    if (sent) *sent = 0U;
    if (!s_uart.opened || (!data && length)) return AT_TRANSPORT_ERROR;
    const uint32_t start = uart_now_ms();
    for (size_t index = 0U; index < length; ++index) {
        while (usart_flag_get(s_uart.config.usart, USART_FLAG_TBE) == RESET) {
            if ((uint32_t)(uart_now_ms() - start) >= timeout_ms) return AT_TRANSPORT_TIMEOUT;
            vTaskDelay(1U);
        }
        usart_data_transmit(s_uart.config.usart, data[index]);
        if (sent) *sent = index + 1U;
    }
    while (usart_flag_get(s_uart.config.usart, USART_FLAG_TC) == RESET) {
        if ((uint32_t)(uart_now_ms() - start) >= timeout_ms) return AT_TRANSPORT_TIMEOUT;
        vTaskDelay(1U);
    }
    return AT_TRANSPORT_OK;
}

static void uart_suspend(at_transport_t *transport)
{
    (void)transport;
    usart_interrupt_disable(s_uart.config.usart, USART_INT_RBNE);
    usart_interrupt_disable(s_uart.config.usart, USART_INT_IDLE);
    usart_interrupt_disable(s_uart.config.usart, USART_INT_ERR);
    nvic_irq_disable(s_uart.config.irq);
    nvic_irq_disable(AT_UART_RX_DMA_IRQ);
}

static int uart_resume(at_transport_t *transport)
{
    (void)transport;
    usart_interrupt_enable(s_uart.config.usart, USART_INT_IDLE);
    usart_interrupt_enable(s_uart.config.usart, USART_INT_ERR);
    nvic_irq_enable(s_uart.config.irq, s_uart.config.irq_priority, 0U);
    nvic_irq_enable(AT_UART_RX_DMA_IRQ, s_uart.config.irq_priority, 0U);
    return AT_TRANSPORT_OK;
}

static void uart_flush(at_transport_t *transport)
{
    (void)transport;
    const uint32_t written = AT_UART_RX_DMA_SIZE -
                             dma_transfer_number_get(AT_UART_RX_DMA_PERIPH, AT_UART_RX_DMA_CH);
    s_uart.rx_dma_pos = written % AT_UART_RX_DMA_SIZE;
}

static uint32_t uart_rx_burst_high_water(const at_transport_t *transport)
{
    (void)transport;
    return s_uart.rx_dma_burst_high_water;
}

static const at_transport_ops_t s_uart_ops = {
    .name = "uart",
    .open = uart_open,
    .close = uart_close,
    .write = uart_write,
    .suspend = uart_suspend,
    .resume = uart_resume,
    .flush = uart_flush,
    .rx_burst_high_water = uart_rx_burst_high_water,
};

at_transport_t *at_uart_transport_get(void)
{
    static const at_uart_config_t default_config = AT_UART_CONFIG_INITIALIZER;
    if (s_transport.ops == NULL) {
        s_uart.config = default_config;
        s_transport.ops = &s_uart_ops;
        s_transport.context = &s_uart;
    }
    return &s_transport;
}

#ifndef AT_UART_IRQ_HANDLER
#error "The selected board must define AT_UART_IRQ_HANDLER"
#endif

void DMA0_Channel7_IRQHandler(void)
{
    const uint32_t slot = (uint32_t)AT_UART_RX_DMA_CH - 4U;
    if (DMA_INTF1(AT_UART_RX_DMA_PERIPH) & DMA_FLAG_ADD(DMA_INTF_FTFIF, slot)) {
        DMA_INTC1(AT_UART_RX_DMA_PERIPH) = DMA_FLAG_ADD(DMA_INTC_FTFIFC, slot);
    }
    (void)uart_dma_drain();
}

void AT_UART_IRQ_HANDLER(void)
{
    if (!s_uart.opened) return;

    uint32_t faults = 0U;
    const uint32_t usart = s_uart.config.usart;
    if (usart_flag_get(usart, USART_FLAG_ORERR) == SET) {
        usart_flag_clear(usart, USART_FLAG_ORERR);
        faults |= AT_TRANSPORT_FAULT_OVERRUN;
    }
    if (usart_flag_get(usart, USART_FLAG_FERR) == SET) {
        usart_flag_clear(usart, USART_FLAG_FERR);
        faults |= AT_TRANSPORT_FAULT_FRAMING;
    }
    if (usart_flag_get(usart, USART_FLAG_NERR) == SET) {
        usart_flag_clear(usart, USART_FLAG_NERR);
        faults |= AT_TRANSPORT_FAULT_NOISE;
    }
    if (usart_flag_get(usart, USART_FLAG_PERR) == SET) {
        usart_flag_clear(usart, USART_FLAG_PERR);
        faults |= AT_TRANSPORT_FAULT_PARITY;
    }
    if (usart_flag_get(usart, USART_FLAG_IDLE) == SET) {
        usart_flag_clear(usart, USART_FLAG_IDLE);
        (void)uart_dma_drain();
    }
    uart_notify_fault(faults);
}
