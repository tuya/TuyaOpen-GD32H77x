#include "FreeRTOS.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t __heap_guard_before[], __heap_guard_after[];
static unsigned suspended;
void vTaskSuspendAll(void) { ++suspended; }
int xTaskResumeAll(void) { assert(suspended > 0); --suspended; return 0; }
void gd32h77x_platform_panic(const char *message) { fprintf(stderr, "%s\n", message); abort(); }

static void check_guards(void)
{
    for (unsigned i = 0; i < 32; ++i) {
        assert(__heap_guard_before[i] == 0xA5);
        assert(__heap_guard_after[i] == 0x5A);
    }
    assert(suspended == 0);
}

static void check_block(void *ptr, size_t size, uint8_t value)
{
    assert((uintptr_t)ptr % portBYTE_ALIGNMENT == 0);
    assert((uintptr_t)ptr >= (uintptr_t)__rtos_heap_start);
    assert((uintptr_t)ptr + size <= (uintptr_t)__rtos_heap_end);
    for (size_t i = 0; i < size; ++i) assert(((uint8_t *)ptr)[i] == value);
}

int main(void)
{
    memset(__heap_guard_before, 0xA5, 32);
    memset(__heap_guard_after, 0x5A, 32);
    /* Linker-owned heap storage is NOLOAD and need not contain zeroes. */
    memset(__rtos_heap_start, 0xCD, configTOTAL_HEAP_SIZE);
    /* Lazy initialization must work before any scheduler is started. */
    void *first = pvPortMalloc(1);
    assert(first != NULL);
    vPortFree(first);
    size_t initial = xPortGetFreeHeapSize();
    assert(initial > configTOTAL_HEAP_SIZE - 64);
    assert(initial < configTOTAL_HEAP_SIZE);
    assert(pvPortMalloc(0) == NULL);
    assert(pvPortMalloc(SIZE_MAX) == NULL);
    assert(pvPortMalloc(configTOTAL_HEAP_SIZE) == NULL);
    assert(pvPortCalloc(SIZE_MAX, 2) == NULL);
    vPortFree(NULL);
    assert(xPortGetFreeHeapSize() == initial);

    uint8_t *zero = pvPortCalloc(37, 3);
    assert(zero != NULL);
    check_block(zero, 111, 0);
    vPortFree(zero);

    /* Exhaustion must return NULL, preserve active payloads and recover fully. */
    void *blocks[4096];
    size_t count = 0;
    while (count < 4096 && (blocks[count] = pvPortMalloc(127)) != NULL) {
        memset(blocks[count], (uint8_t)count, 127);
        ++count;
    }
    assert(count > 0 && count < 4096);
    for (size_t i = 0; i < count; ++i) check_block(blocks[i], 127, (uint8_t)i);
    for (size_t i = 0; i < count; i += 2) vPortFree(blocks[i]);
    for (size_t i = 1; i < count; i += 2) vPortFree(blocks[i]);
    assert(xPortGetFreeHeapSize() == initial);

    /* Mixed lifetimes and repeated fragmentation/coalescing, deterministic seed. */
    struct { void *ptr; size_t size; uint8_t tag; } slots[64] = {0};
    uint32_t random = 0x12345678U;
    for (unsigned step = 0; step < 50000; ++step) {
        random = random * 1664525U + 1013904223U;
        unsigned slot = (random >> 16) % 64;
        if (slots[slot].ptr != NULL) {
            check_block(slots[slot].ptr, slots[slot].size, slots[slot].tag);
            vPortFree(slots[slot].ptr);
            slots[slot].ptr = NULL;
        } else {
            size_t size = (random >> 8) % 2048 + 1;
            void *ptr = pvPortMalloc(size);
            if (ptr != NULL) {
                uint8_t tag = (uint8_t)(step + 1);
                memset(ptr, tag, size);
                slots[slot].ptr = ptr; slots[slot].size = size; slots[slot].tag = tag;
            }
        }
        if (step % 100 == 0) check_guards();
    }
    for (unsigned i = 0; i < 64; ++i) {
        if (slots[i].ptr != NULL) {
            check_block(slots[i].ptr, slots[i].size, slots[i].tag);
            vPortFree(slots[i].ptr);
        }
    }
    HeapStats_t stats;
    vPortGetHeapStats(&stats);
    assert(stats.xAvailableHeapSpaceInBytes == initial);
    assert(stats.xNumberOfFreeBlocks == 1);
    assert(stats.xNumberOfSuccessfulAllocations == stats.xNumberOfSuccessfulFrees);
    assert(xPortGetMinimumEverFreeHeapSize() < initial);
    /* A large contiguous allocation proves adjacent free blocks were merged. */
    void *large = pvPortMalloc(initial - 32);
    assert(large != NULL);
    memset(large, 0xC3, initial - 32);
    check_block(large, initial - 32, 0xC3);
    vPortFree(large);
    assert(xPortGetFreeHeapSize() == initial);
    check_guards();
    printf("heap capacity=%zu; 50000 allocation/free cycles passed\n", configTOTAL_HEAP_SIZE);
    return 0;
}
