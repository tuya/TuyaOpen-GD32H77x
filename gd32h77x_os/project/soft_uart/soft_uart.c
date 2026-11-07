/**
 * @file soft_uart.c
 * @brief GPIO bit-banged UART on GD32H779I Watch EVAL.
 *
 * PF4 is TX, PF5 is RX, 9600 8N1. The TX path is continuous; RX bytes are
 * echoed while the main loop is idle.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "gd32h77x_78x.h"

#include <stdbool.h>
#include <stdint.h>

#define SOFT_UART_TX_PORT       GPIOF
#define SOFT_UART_TX_PIN        GPIO_PIN_4
#define SOFT_UART_RX_PORT       GPIOF
#define SOFT_UART_RX_PIN        GPIO_PIN_5
#define SOFT_UART_BAUD          9600U
#define SOFT_UART_TX_MESSAGE    "SOFT_UART PF4=TX PF5=RX 9600 8N1\r\n"

static uint32_t s_bit_cycles;

static void soft_uart_delay_cycles(uint32_t cycles)
{
    const uint32_t start = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < cycles) {
        __NOP();
    }
}

static void soft_uart_delay_bit(void)
{
    soft_uart_delay_cycles(s_bit_cycles);
}

static void soft_uart_init(void)
{
    rcu_periph_clock_enable(RCU_GPIOF);

    gpio_mode_set(SOFT_UART_TX_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE,
                  SOFT_UART_TX_PIN);
    gpio_output_options_set(SOFT_UART_TX_PORT, GPIO_OTYPE_PP,
                            GPIO_OSPEED_12MHZ, SOFT_UART_TX_PIN);
    gpio_bit_set(SOFT_UART_TX_PORT, SOFT_UART_TX_PIN);

    gpio_mode_set(SOFT_UART_RX_PORT, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP,
                  SOFT_UART_RX_PIN);

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->LAR = UINT32_C(0xC5ACCE55);
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    s_bit_cycles = SystemCoreClock / SOFT_UART_BAUD;
    if (s_bit_cycles == 0U) {
        s_bit_cycles = 1U;
    }
}

static void soft_uart_write_byte(uint8_t value)
{
    taskENTER_CRITICAL();

    gpio_bit_reset(SOFT_UART_TX_PORT, SOFT_UART_TX_PIN); /* start */
    soft_uart_delay_bit();
    for (uint32_t bit = 0U; bit < 8U; ++bit) {
        if ((value & (uint8_t)(1U << bit)) != 0U) {
            gpio_bit_set(SOFT_UART_TX_PORT, SOFT_UART_TX_PIN);
        } else {
            gpio_bit_reset(SOFT_UART_TX_PORT, SOFT_UART_TX_PIN);
        }
        soft_uart_delay_bit();
    }
    gpio_bit_set(SOFT_UART_TX_PORT, SOFT_UART_TX_PIN); /* stop */
    soft_uart_delay_bit();

    taskEXIT_CRITICAL();
}

static void soft_uart_write_string(const char *text)
{
    while (*text != '\0') {
        soft_uart_write_byte((uint8_t)*text++);
    }
}

static bool soft_uart_wait_start(uint32_t timeout_cycles)
{
    const uint32_t start = DWT->CYCCNT;
    while (gpio_input_bit_get(SOFT_UART_RX_PORT, SOFT_UART_RX_PIN) == SET) {
        if ((DWT->CYCCNT - start) >= timeout_cycles) {
            return false;
        }
    }
    return true;
}

static bool soft_uart_read_byte(uint8_t *value, uint32_t timeout_ms)
{
    if (value == NULL) {
        return false;
    }
    const uint32_t timeout_cycles = (SystemCoreClock / 1000U) * timeout_ms;
    if (!soft_uart_wait_start(timeout_cycles)) {
        return false;
    }

    /* Sample the middle of each data bit. Detection occurs just after the
     * falling edge of the start bit. */
    soft_uart_delay_cycles(s_bit_cycles + (s_bit_cycles / 2U));
    uint8_t result = 0U;
    for (uint32_t bit = 0U; bit < 8U; ++bit) {
        if (gpio_input_bit_get(SOFT_UART_RX_PORT, SOFT_UART_RX_PIN) == SET) {
            result |= (uint8_t)(1U << bit);
        }
        soft_uart_delay_bit();
    }
    soft_uart_delay_bit(); /* stop bit */
    *value = result;
    return true;
}

void app_entry(void *argument)
{
    (void)argument;
    soft_uart_init();

    for (;;) {
        soft_uart_write_string(SOFT_UART_TX_MESSAGE);

        uint8_t received = 0U;
        if (soft_uart_read_byte(&received, 1U)) {
            soft_uart_write_byte(received);
        }
    }
}
