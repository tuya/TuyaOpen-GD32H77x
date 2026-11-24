#ifndef TEST_HEAP_FREERTOS_H
#define TEST_HEAP_FREERTOS_H
#include <stddef.h>
#include <stdint.h>
#include "FreeRTOSConfig.h"
#define PRIVILEGED_DATA
#define PRIVILEGED_FUNCTION
#define configENABLE_HEAP_PROTECTOR 0
#define portPOINTER_SIZE_TYPE uintptr_t
#define portBYTE_ALIGNMENT 8U
#define portBYTE_ALIGNMENT_MASK 7U
#define portMAX_DELAY SIZE_MAX
#define mtCOVERAGE_TEST_MARKER() ((void)0)
#define traceMALLOC(ptr, size) ((void)0)
#define traceFREE(ptr, size) ((void)0)
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
typedef struct {
    size_t xAvailableHeapSpaceInBytes;
    size_t xSizeOfLargestFreeBlockInBytes;
    size_t xSizeOfSmallestFreeBlockInBytes;
    size_t xNumberOfFreeBlocks;
    size_t xMinimumEverFreeBytesRemaining;
    size_t xNumberOfSuccessfulAllocations;
    size_t xNumberOfSuccessfulFrees;
} HeapStats_t;
void *pvPortMalloc(size_t size);
void *pvPortCalloc(size_t count, size_t size);
void vPortFree(void *ptr);
size_t xPortGetFreeHeapSize(void);
size_t xPortGetMinimumEverFreeHeapSize(void);
void vPortGetHeapStats(HeapStats_t *stats);
#endif
