#include <stdint.h>

#include "gd32h77x_78x.h"
#include "gd32h77x_config.h"

#define EXPECTED_CHECKSUM 0xB1E15B36U

static const uint32_t ecnvm_test_words[8]
    __attribute__((section(".ecnvm_rodata"), used)) = {
        0x13579BDFU, 0x2468ACE0U, 0x0F1E2D3CU, 0x4B5A6978U,
        0x89ABCDEFU, 0x10293847U, 0x55667788U, 0xA5A55A5AU,
};

__attribute__((noinline, used, section(".ecnvm_text")))
static uint32_t ecnvm_test(uint32_t input, uint32_t *pc_out)
{
    const volatile uint32_t *words = ecnvm_test_words;
    uint32_t sum = input ^ 0xC001D00DU;
    uint32_t i;

    __asm volatile("mov %0, pc" : "=r"(*pc_out));
    for (i = 0U; i < 8U; ++i) {
        sum = (sum << 5) | (sum >> 27);
        sum ^= words[i] + (input ^ (i * 0x1020304U));
    }
    return sum;
}

static void uart_putc(char c)
{
    while (RESET == usart_flag_get(GD32H77X_CONSOLE_USART, USART_FLAG_TBE)) {
    }
    usart_data_transmit(GD32H77X_CONSOLE_USART, (uint8_t)c);
}

static void uart_text(const char *s)
{
    while (*s != '\0') {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
    }
}

static void uart_hex(uint32_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    int shift;

    uart_text("0x");
    for (shift = 28; shift >= 0; shift -= 4) {
        uart_putc(digits[(value >> shift) & 0xFU]);
    }
}

static void console_init(void)
{
    rcu_periph_clock_enable(GD32H77X_CONSOLE_GPIO_CLOCK);
    rcu_periph_clock_enable(GD32H77X_CONSOLE_USART_CLOCK);
    gpio_af_set(GD32H77X_CONSOLE_GPIO_PORT, GD32H77X_CONSOLE_AF,
                GD32H77X_CONSOLE_TX_PIN | GD32H77X_CONSOLE_RX_PIN);
    gpio_mode_set(GD32H77X_CONSOLE_GPIO_PORT, GPIO_MODE_AF,
                  GPIO_PUPD_PULLUP,
                  GD32H77X_CONSOLE_TX_PIN | GD32H77X_CONSOLE_RX_PIN);
    gpio_output_options_set(GD32H77X_CONSOLE_GPIO_PORT, GPIO_OTYPE_PP,
                            GPIO_OSPEED_60MHZ,
                            GD32H77X_CONSOLE_TX_PIN | GD32H77X_CONSOLE_RX_PIN);
    usart_deinit(GD32H77X_CONSOLE_USART);
    usart_baudrate_set(GD32H77X_CONSOLE_USART, 115200U);
    usart_word_length_set(GD32H77X_CONSOLE_USART, USART_WL_8BIT);
    usart_stop_bit_set(GD32H77X_CONSOLE_USART, USART_STB_1BIT);
    usart_parity_config(GD32H77X_CONSOLE_USART, USART_PM_NONE);
    usart_receive_config(GD32H77X_CONSOLE_USART, USART_RECEIVE_ENABLE);
    usart_transmit_config(GD32H77X_CONSOLE_USART, USART_TRANSMIT_ENABLE);
    usart_enable(GD32H77X_CONSOLE_USART);
}

static void print_result(uint32_t function_address, uint32_t data_address,
                         uint32_t return_pc, uint32_t checksum)
{
    uart_text("GD32H779VW CNVM+ECNVM split: ");
    uart_text((function_address >= 0x08200000U && function_address < 0x08210000U &&
               data_address >= 0x08200000U && data_address < 0x08210000U &&
               return_pc >= 0x08200000U && return_pc < 0x08210000U &&
               checksum == EXPECTED_CHECKSUM) ? "PASS\n" : "FAIL\n");
    uart_text("func=");
    uart_hex(function_address);
    uart_text(" data=");
    uart_hex(data_address);
    uart_text(" current_pc=");
    uart_hex(return_pc);
    uart_text(" checksum=");
    uart_hex(checksum);
    uart_text("\n");
}

int main(void)
{
    volatile uint32_t runtime_input = 0x31415926U;
    uint32_t function_address = (uint32_t)(uintptr_t)ecnvm_test;
    uint32_t data_address = (uint32_t)(uintptr_t)ecnvm_test_words;
    uint32_t return_pc;
    uint32_t checksum;

    console_init();
    checksum = ecnvm_test(runtime_input, &return_pc);
    for (;;) {
        volatile uint32_t delay = 60000000U;
        print_result(function_address, data_address, return_pc, checksum);
        while (delay-- != 0U) {
            __NOP();
        }
    }
}
