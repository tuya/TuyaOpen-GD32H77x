#ifndef TEST_SEMPHR_H
#define TEST_SEMPHR_H
#include "FreeRTOS.h"
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateCounting(unsigned int max, unsigned int initial);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
#endif
