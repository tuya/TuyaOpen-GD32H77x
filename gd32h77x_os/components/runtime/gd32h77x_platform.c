/**
 * @file gd32h77x_platform.c
 * @brief GD32H77x platform bring-up and FreeRTOS adaptation.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "gd32h77x_platform.h"
#include "gd32h77x_config.h"
#include "gd32h77x_78x.h"
#include "gd32h77x_78x_misc.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <stdio.h>
#include <stdbool.h>

static SemaphoreHandle_t log_mutex;
static uint32_t          reset_flags;
extern uint32_t          __vector_base;

static void raw_write(const char *data, size_t size);

#if defined(ENABLE_GD32H77X_PSRAM) && (ENABLE_GD32H77X_PSRAM == 1) && \
    defined(GD32H77X_PSRAM_ACCESS_MAPPED) && \
    (GD32H77X_PSRAM_ACCESS_MAPPED == 1)
static void __configure_psram_mpu(void)
{
    mpu_region_init_struct region;

    ARM_MPU_Disable();
    mpu_region_struct_para_init(&region);

    /* Vendor configuration: deny the unused external-memory windows first.
     * Speculative accesses outside the real PSRAM can otherwise lock the AXI
     * bus even though the 8 MiB PSRAM region itself is valid. */
    region.region_base_address = 0U;
    region.region_size = MPU_REGION_SIZE_4GB;
    region.subregion_disable = 0x87U;
    region.tex_type = MPU_TEX_TYPE0;
    region.access_permission = MPU_AP_NO_ACCESS;
    region.access_shareable = MPU_ACCESS_SHAREABLE;
    region.access_cacheable = MPU_ACCESS_NON_CACHEABLE;
    region.access_bufferable = MPU_ACCESS_NON_BUFFERABLE;
    region.instruction_exec = MPU_INSTRUCTION_EXEC_NOT_PERMIT;
    region.region_number = MPU_REGION_NUMBER0;
    mpu_region_config(&region);
    mpu_region_enable();

    region.region_base_address = 0x24000000U;
    region.region_size = MPU_REGION_SIZE_1MB;
    region.subregion_disable = MPU_SUBREGION_ENABLE;
    region.access_permission = MPU_AP_FULL_ACCESS;
    region.access_shareable = MPU_ACCESS_NON_SHAREABLE;
    region.access_cacheable = MPU_ACCESS_CACHEABLE;
    region.access_bufferable = MPU_ACCESS_NON_BUFFERABLE;
    region.instruction_exec = MPU_INSTRUCTION_EXEC_PERMIT;
    region.region_number = MPU_REGION_NUMBER1;
    mpu_region_config(&region);
    mpu_region_enable();

    region.region_base_address = GD32H77X_PSRAM_MAPPED_BASE;
    region.region_size = MPU_REGION_SIZE_8MB;
    region.subregion_disable = MPU_SUBREGION_ENABLE;
    region.tex_type = MPU_TEX_TYPE0;
    region.access_permission = MPU_AP_FULL_ACCESS;
    /* The mapped OSPI controller cannot safely serve cache line fills or
     * reordered Normal-memory traffic. Use the vendor-proven shareable Device
     * mapping and keep D-Cache disabled for this region. */
    region.access_shareable = MPU_ACCESS_SHAREABLE;
    region.access_cacheable = MPU_ACCESS_NON_CACHEABLE;
    region.access_bufferable = MPU_ACCESS_BUFFERABLE;
    region.instruction_exec = MPU_INSTRUCTION_EXEC_NOT_PERMIT;
    region.region_number = MPU_REGION_NUMBER2;
    mpu_region_config(&region);
    mpu_region_enable();

    ARM_MPU_Enable(MPU_MODE_PRIV_DEFAULT);
    SCB_EnableICache();
    SCB_EnableDCache();
}
#endif

__attribute__((weak)) bool gd32h77x_at_bridge_host_rx_isr(void)
{
    return false;
}

void gd32h77x_platform_init(void)
{
#if defined(ENABLE_GD32H77X_POWER_HOLD) && ENABLE_GD32H77X_POWER_HOLD
    rcu_periph_clock_enable(GD32H77X_POWER_HOLD_CLOCK);
    /* Set the latch first so switching to output cannot pulse power off. */
    if (GD32H77X_POWER_HOLD_LEVEL) {
        gpio_bit_set(GD32H77X_POWER_HOLD_PORT, GD32H77X_POWER_HOLD_PIN);
    } else {
        gpio_bit_reset(GD32H77X_POWER_HOLD_PORT, GD32H77X_POWER_HOLD_PIN);
    }
    gpio_output_options_set(GD32H77X_POWER_HOLD_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_12MHZ,
                            GD32H77X_POWER_HOLD_PIN);
    gpio_mode_set(GD32H77X_POWER_HOLD_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE,
                  GD32H77X_POWER_HOLD_PIN);
#endif
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
#if defined(ENABLE_GD32H77X_PSRAM) && (ENABLE_GD32H77X_PSRAM == 1) && \
    defined(GD32H77X_PSRAM_ACCESS_MAPPED) && \
    (GD32H77X_PSRAM_ACCESS_MAPPED == 1)
    __configure_psram_mpu();
#endif
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
    char heap_line[112];
    int heap_len = snprintf(heap_line, sizeof(heap_line),
                            "[platform] shared heap=%lu bytes free=%lu\r\n",
                            (unsigned long)configTOTAL_HEAP_SIZE,
                            (unsigned long)xPortGetFreeHeapSize());
    if (heap_len > 0 && (size_t)heap_len < sizeof(heap_line)) {
        raw_write(heap_line, (size_t)heap_len);
    }
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

typedef struct {
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r12;
    uint32_t lr;
    uint32_t pc;
    uint32_t xpsr;
} fault_frame_t;

static void fault_report(const char *kind, uint32_t *stack_frame)
{
    fault_frame_t *frame = (fault_frame_t *)stack_frame;
    char line[224];
    int length = snprintf(
        line, sizeof(line),
        "\r\nFAULT %s pc=%08lX lr=%08lX xpsr=%08lX "
        "cfsr=%08lX hfsr=%08lX mmar=%08lX bfar=%08lX "
        "r0=%08lX r1=%08lX r2=%08lX r3=%08lX\r\n",
        kind, (unsigned long)frame->pc, (unsigned long)frame->lr,
        (unsigned long)frame->xpsr, (unsigned long)SCB->CFSR,
        (unsigned long)SCB->HFSR, (unsigned long)SCB->MMFAR,
        (unsigned long)SCB->BFAR, (unsigned long)frame->r0,
        (unsigned long)frame->r1, (unsigned long)frame->r2,
        (unsigned long)frame->r3);
    if (length > 0) {
        size_t size = (size_t)length < sizeof(line) ? (size_t)length
                                                    : sizeof(line) - 1U;
        raw_write(line, size);
    }
    for (;;) {
        __WFI();
    }
}

__attribute__((naked, noreturn)) void HardFault_Handler(void)
{
    __asm volatile("mrs r0, msp\n"
                   "b hardfault_report\n");
}

__attribute__((naked, noreturn)) void MemManage_Handler(void)
{
    __asm volatile("mrs r0, msp\n"
                   "b memmanage_report\n");
}

__attribute__((naked, noreturn)) void BusFault_Handler(void)
{
    __asm volatile("mrs r0, msp\n"
                   "b busfault_report\n");
}

void hardfault_report(uint32_t *stack_frame)
{
    fault_report("HardFault", stack_frame);
}

void memmanage_report(uint32_t *stack_frame)
{
    fault_report("MemManage", stack_frame);
}

void busfault_report(uint32_t *stack_frame)
{
    fault_report("BusFault", stack_frame);
}
