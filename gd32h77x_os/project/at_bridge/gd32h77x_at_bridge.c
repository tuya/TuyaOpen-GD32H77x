/**
 * @file gd32h77x_at_bridge.c
 * @brief Raw USART1 <-> configured AT transport bridge.
 */

#include "gd32h77x_at_bridge.h"

#include "FreeRTOS.h"
#include "task.h"

#include "gd32h77x_78x.h"
#include "gd32h77x_config.h"
#include "at_transport.h"

#include <stddef.h>
#include <stdint.h>

#define BRIDGE_RING_SIZE       8192U
#define BRIDGE_RING_MASK       (BRIDGE_RING_SIZE - 1U)
#define BRIDGE_TASK_STACK      1024U
#define BRIDGE_TASK_PRIORITY   4U
#define BRIDGE_TX_CHUNK        64U
#define BRIDGE_RESET_LOW_MS    20U
#define BRIDGE_BOOT_WAIT_MS    800U

_Static_assert((BRIDGE_RING_SIZE & BRIDGE_RING_MASK) == 0U, "bridge ring must be power-of-two");

typedef struct {
    uint8_t data[BRIDGE_RING_SIZE];
    volatile uint16_t head;
    volatile uint16_t tail;
    volatile uint32_t dropped;
} bridge_ring_t;

static bridge_ring_t s_host_to_module;
static bridge_ring_t s_module_to_host;
static volatile bool s_bridge_active;
static at_transport_t *s_module_transport;

static bool ring_push(bridge_ring_t *ring, uint8_t byte)
{
    uint16_t head = ring->head;
    uint16_t next = (uint16_t)((head + 1U) & BRIDGE_RING_MASK);
    if (next == ring->tail) {
        ++ring->dropped;
        return false;
    }
    ring->data[head] = byte;
    ring->head = next;
    return true;
}

static size_t ring_pop(bridge_ring_t *ring, uint8_t *data, size_t capacity)
{
    size_t count = 0U;
    while (count < capacity && ring->tail != ring->head) {
        data[count++] = ring->data[ring->tail];
        ring->tail = (uint16_t)((ring->tail + 1U) & BRIDGE_RING_MASK);
    }
    return count;
}

static void usart_clear_errors(uint32_t usart)
{
    if (usart_flag_get(usart, USART_FLAG_ORERR) == SET) {
        usart_flag_clear(usart, USART_FLAG_ORERR);
    }
    if (usart_flag_get(usart, USART_FLAG_FERR) == SET) {
        usart_flag_clear(usart, USART_FLAG_FERR);
    }
    if (usart_flag_get(usart, USART_FLAG_NERR) == SET) {
        usart_flag_clear(usart, USART_FLAG_NERR);
    }
    if (usart_flag_get(usart, USART_FLAG_PERR) == SET) {
        usart_flag_clear(usart, USART_FLAG_PERR);
    }
}

static void usart_drain_to_ring(uint32_t usart, bridge_ring_t *ring)
{
    usart_clear_errors(usart);
    while (usart_flag_get(usart, USART_FLAG_RBNE) == SET) {
        (void)ring_push(ring, (uint8_t)usart_data_receive(usart));
    }
}

static void module_rx(void *context, const uint8_t *data, size_t length)
{
    (void)context;
    if (!s_bridge_active || !data) return;
    for (size_t index = 0U; index < length; ++index) {
        (void)ring_push(&s_module_to_host, data[index]);
    }
}

bool gd32h77x_at_bridge_host_rx_isr(void)
{
    if (!s_bridge_active) {
        return false;
    }
    usart_drain_to_ring(USART1, &s_host_to_module);
    return true;
}

static void usart_write_bytes(uint32_t usart, const uint8_t *data, size_t length)
{
    for (size_t i = 0U; i < length; ++i) {
        while (usart_flag_get(usart, USART_FLAG_TBE) == RESET) {
        }
        usart_data_transmit(usart, data[i]);
    }
}

static void host_to_module_task(void *argument)
{
    (void)argument;
    uint8_t data[BRIDGE_TX_CHUNK];
    for (;;) {
        size_t count = ring_pop(&s_host_to_module, data, sizeof(data));
        if (count != 0U) {
            size_t sent = 0U;
            (void)at_transport_write(s_module_transport, data, count,
                                     pdMS_TO_TICKS(100U), &sent);
        } else {
            vTaskDelay(pdMS_TO_TICKS(1U));
        }
    }
}

static void module_to_host_task(void *argument)
{
    (void)argument;
    uint8_t data[BRIDGE_TX_CHUNK];
    for (;;) {
        size_t count = ring_pop(&s_module_to_host, data, sizeof(data));
        if (count != 0U) {
            usart_write_bytes(USART1, data, count);
        } else {
            vTaskDelay(pdMS_TO_TICKS(1U));
        }
    }
}

static void at_hardware_reset(void)
{
    rcu_periph_clock_enable(AT_RESET_CLOCK);
    gpio_mode_set(AT_RESET_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, AT_RESET_PIN);
    gpio_output_options_set(AT_RESET_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, AT_RESET_PIN);
    gpio_bit_set(AT_RESET_PORT, AT_RESET_PIN);
    vTaskDelay(pdMS_TO_TICKS(10U));
    gpio_bit_reset(AT_RESET_PORT, AT_RESET_PIN);
    vTaskDelay(pdMS_TO_TICKS(BRIDGE_RESET_LOW_MS));
    gpio_bit_set(AT_RESET_PORT, AT_RESET_PIN);
    vTaskDelay(pdMS_TO_TICKS(BRIDGE_BOOT_WAIT_MS));
}

void gd32h77x_at_bridge_start(void)
{
    s_host_to_module.head = s_host_to_module.tail = 0U;
    s_module_to_host.head = s_module_to_host.tail = 0U;
    s_bridge_active = false;

    at_hardware_reset();
    s_module_transport = at_transport_get();
    if (!s_module_transport) {
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000U));
    }
    s_module_transport->rx_callback = module_rx;
    s_module_transport->rx_context = NULL;
    if (at_transport_open(s_module_transport) != AT_TRANSPORT_OK) {
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000U));
    }

    if (xTaskCreate(host_to_module_task, "at_host", BRIDGE_TASK_STACK, NULL,
                    BRIDGE_TASK_PRIORITY, NULL) != pdPASS ||
        xTaskCreate(module_to_host_task, "at_module", BRIDGE_TASK_STACK, NULL,
                    BRIDGE_TASK_PRIORITY, NULL) != pdPASS) {
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000U));
        }
    }

    s_bridge_active = true;
    usart_interrupt_enable(USART1, USART_INT_RBNE);
    usart_interrupt_enable(USART1, USART_INT_ERR);
    nvic_irq_enable(USART1_IRQn, 6U, 0U);
}
