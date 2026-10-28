#ifndef GD32H77X_WIFI_TEST_TASK_H
#define GD32H77X_WIFI_TEST_TASK_H
#include "FreeRTOS.h"
typedef unsigned int TickType_t;
typedef void *TaskHandle_t;
typedef int BaseType_t;
#define pdPASS 1
#define tskIDLE_PRIORITY 0
#define pdMS_TO_TICKS(ms) (ms)
#define portTICK_PERIOD_MS 1U
BaseType_t xTaskCreate(void (*entry)(void *), const char *name, unsigned int stack, void *argument,
                       unsigned int priority, TaskHandle_t *handle);
void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);
#endif
