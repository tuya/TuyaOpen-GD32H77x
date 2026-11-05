/**
 * @file gd32h77x_platform.c
 * @brief GD32H77x platform bring-up and FreeRTOS adaptation.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "gd32h77x_platform.h"
#include "gd32h77x_config.h"
#include "gd32h77x_78x.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <stdio.h>
#include <stdbool.h>

static SemaphoreHandle_t log_mutex;
static uint32_t          reset_flags;
extern uint32_t          __vector_base;

static void raw_write(const char *data, size_t size);

__attribute__((weak)) bool gd32h77x_at_bridge_host_rx_isr(void)
{
    return false;
}

void gd32h77x_platform_init(void)
{
    /* Do not inherit interrupt enables/pending state from a ROM loader. */
    SysTick->CTRL = 0;
    SysTick->VAL  = 0;
    for (uint32_t bank = 0; bank < ((SCnSCB->ICTR & 0xFU) + 1U); ++bank) {
        NVIC->ICER[bank] = UINT32_MAX;
        NVIC->ICPR[bank] = UINT32_MAX;
    }
    reset_flags = RCU_RSTSCK;
    rcu_all_reset_flag_clear();
    /* Use the application vector base selected by the resolved flash layout. */
    SCB->VTOR = (uint32_t)&__vector_base;
    __DSB();
    __ISB();
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->LAR = 0xC5ACCE55U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    nvic_priority_group_set(NVIC_PRIGROUP_PRE4_SUB0);
    rcu_periph_clock_enable(GD32H77X_CONSOLE_GPIO_CLOCK);
    rcu_periph_clock_enable(GD32H77X_CONSOLE_USART_CLOCK);
    gpio_af_set(GD32H77X_CONSOLE_GPIO_PORT, GD32H77X_CONSOLE_AF,
                GD32H77X_CONSOLE_TX_PIN | GD32H77X_CONSOLE_RX_PIN);
    gpio_mode_set(GD32H77X_CONSOLE_GPIO_PORT, GPIO_MODE_AF, GPIO_PUPD_PULLUP,
                  GD32H77X_CONSOLE_TX_PIN | GD32H77X_CONSOLE_RX_PIN);
    gpio_output_options_set(GD32H77X_CONSOLE_GPIO_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ,
                            GD32H77X_CONSOLE_TX_PIN | GD32H77X_CONSOLE_RX_PIN);
    usart_deinit(GD32H77X_CONSOLE_USART);
    usart_baudrate_set(GD32H77X_CONSOLE_USART, 115200U);
    usart_word_length_set(GD32H77X_CONSOLE_USART, USART_WL_8BIT);
    usart_stop_bit_set(GD32H77X_CONSOLE_USART, USART_STB_1BIT);
    usart_parity_config(GD32H77X_CONSOLE_USART, USART_PM_NONE);
    usart_transmit_config(GD32H77X_CONSOLE_USART, USART_TRANSMIT_ENABLE);
    usart_receive_config(GD32H77X_CONSOLE_USART, USART_RECEIVE_ENABLE);
    usart_enable(GD32H77X_CONSOLE_USART);
    log_mutex = xSemaphoreCreateMutex();
    configASSERT(log_mutex != NULL);
    /* Surface the reset cause: a silent restart otherwise looks like a crash
     * and cannot be told apart from a brownout or a software reset. */
    char boot_line[48];
    int boot_len = snprintf(boot_line, sizeof(boot_line), "\r\n[platform] reset flags=0x%08lx\r\n",
                            (unsigned long)reset_flags);
    if (boot_len > 0 && (size_t)boot_len < sizeof(boot_line)) raw_write(boot_line, (size_t)boot_len);
}

static void raw_write(const char *data, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        while (usart_flag_get(GD32H77X_CONSOLE_USART, USART_FLAG_TBE) == RESET) {
        }
        usart_data_transmit(GD32H77X_CONSOLE_USART, (uint8_t)data[i]);
    }
}

void gd32h77x_platform_log_write(const char *data, size_t size)
{
    BaseType_t running = xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
    if (running) {
        xSemaphoreTake(log_mutex, portMAX_DELAY);
    }
    raw_write(data, size);
    if (running) {
        xSemaphoreGive(log_mutex);
    }
}

uint32_t gd32h77x_platform_reset_flags(void)
{
    return reset_flags;
}

void gd32h77x_platform_panic(const char *message)
{
    __disable_irq();
    raw_write("\r\nPANIC: ", 9);
    while (*message) {
        raw_write(message++, 1);
    }
    for (;;) {
        __WFI();
    }
}

void vApplicationMallocFailedHook(void)
{
    gd32h77x_platform_panic("FreeRTOS heap exhausted");
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    gd32h77x_platform_panic(name);
}

void HardFault_Handler(void)
{
    gd32h77x_platform_panic("HardFault");
}
