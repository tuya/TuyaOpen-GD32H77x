/**
 * @file FreeRTOSConfig.h
 * @brief GD32H77x platform bring-up and FreeRTOS adaptation.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>
#include <stddef.h>
extern uint32_t SystemCoreClock;
void            gd32h77x_platform_panic(const char *message);

#define configUSE_PREEMPTION                         1
#define configCPU_CLOCK_HZ                           SystemCoreClock
#define configTICK_RATE_HZ                           1000U
#define configTICK_TYPE_WIDTH_IN_BITS                TICK_TYPE_WIDTH_32_BITS
#define configMAX_PRIORITIES                         8U
#define configMINIMAL_STACK_SIZE                     256U
/* The linker gives FreeRTOS all AXI SRAM remaining after static data and
 * the separate libc arena, up to the reserved main/interrupt stack. */
extern uint8_t __rtos_heap_start[];
extern uint8_t __rtos_heap_end[];
#define configAPPLICATION_ALLOCATED_HEAP             1
#define configTOTAL_HEAP_SIZE                        ((size_t)((uintptr_t)__rtos_heap_end - (uintptr_t)__rtos_heap_start))
#define configMAX_TASK_NAME_LEN                      24U
#define configUSE_MUTEXES                            1
#define configUSE_RECURSIVE_MUTEXES                  1
#define configUSE_COUNTING_SEMAPHORES                1
#define configSUPPORT_DYNAMIC_ALLOCATION             1
#define configSUPPORT_STATIC_ALLOCATION              0
#define configSTACK_ALLOCATION_FROM_SEPARATE_HEAP    1
#define configTASK_ALLOCATION_FROM_DTCM              1
#define configUSE_TIMERS                             1
#define configTIMER_TASK_PRIORITY                    2U
#define configTIMER_QUEUE_LENGTH                     16U
#define configTIMER_TASK_STACK_DEPTH                 512U
#define configCHECK_FOR_STACK_OVERFLOW               2
#define configUSE_MALLOC_FAILED_HOOK                 0
#define configUSE_NEWLIB_REENTRANT                   1
#define configUSE_IDLE_HOOK                          0
#define configUSE_TICK_HOOK                          1
#define configUSE_TRACE_FACILITY                     1
#define configQUEUE_REGISTRY_SIZE                    0
#define configPRIO_BITS                              4U
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY      15U
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5U
#define configKERNEL_INTERRUPT_PRIORITY              (15U << (8U - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY         (5U << (8U - configPRIO_BITS))
#define configASSERT(condition)                                                                                        \
    do {                                                                                                               \
        if (!(condition))                                                                                              \
            gd32h77x_platform_panic(#condition);                                                                       \
    } while (0)

#define INCLUDE_vTaskDelete                 1
#define INCLUDE_vTaskDelay                  1
#define INCLUDE_vTaskSuspend                1
#define INCLUDE_xTaskGetSchedulerState      1
#define INCLUDE_xTaskGetCurrentTaskHandle   1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_uxTaskPriorityGet           1
#define INCLUDE_vTaskPrioritySet            1
#define vPortSVCHandler                     SVC_Handler
#define xPortPendSVHandler                  PendSV_Handler
#define xPortSysTickHandler                 SysTick_Handler

#endif
