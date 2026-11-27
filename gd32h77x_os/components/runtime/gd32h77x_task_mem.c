/**
 * @file gd32h77x_task_mem.c
 * @brief Transparent DTCM-first task memory allocator for GD32H77X.
 *
 * FreeRTOS allocates task stacks through pvPortMallocStack() and task TCBs
 * through pvPortMallocTask(). This file initially serves both from a DTCM
 * arena and falls back to the normal FreeRTOS heap when the arena is full.
 * The application and TAL layers do not need to know which region was used.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "portable.h"
#include "gd32h77x_task_mem.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

extern uint8_t __dtcm_task_heap_start[];
extern uint8_t __dtcm_task_heap_end[];

#define TASK_ALLOC_MAGIC       UINT32_C(0x5441534B) /* TASK */
#define DTCM_BLOCK_MAGIC       UINT32_C(0x4454434D) /* DTCM */
#define TASK_MEM_ALIGN         UINT32_C(8)

#define TASK_ALLOC_REGION_DTCM UINT16_C(1)
#define TASK_ALLOC_REGION_HEAP UINT16_C(2)

typedef struct __attribute__((aligned(8))) {
    uint32_t magic;
    uint16_t region;
    uint16_t reserved;
    size_t   size;
} task_alloc_header_t;

typedef struct __attribute__((aligned(8))) dtcm_block {
    size_t             size;
    struct dtcm_block *next;
    uint32_t           free;
    uint32_t           magic;
} dtcm_block_t;

_Static_assert((sizeof(task_alloc_header_t) % TASK_MEM_ALIGN) == 0U,
               "task allocation header must preserve payload alignment");
_Static_assert((sizeof(dtcm_block_t) % TASK_MEM_ALIGN) == 0U,
               "DTCM block header must preserve payload alignment");

static bool           s_initialized;
static uint8_t       *s_heap_start;
static uint8_t       *s_heap_end;
static dtcm_block_t  *s_first_block;
static uint32_t       s_dtcm_allocations;
static uint32_t       s_axisram_fallbacks;
static uint32_t       s_free_calls;

static UBaseType_t task_mem_lock(void)
{
    /* taskENTER_CRITICAL() is not valid before the scheduler starts because
     * the port's critical nesting count is still at its sentinel value. */
    return portSET_INTERRUPT_MASK_FROM_ISR();
}

static void task_mem_unlock(UBaseType_t mask)
{
    portCLEAR_INTERRUPT_MASK_FROM_ISR(mask);
}

static size_t align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static bool dtcm_heap_init_locked(void)
{
    if (s_initialized) {
        return true;
    }

    uintptr_t start = (uintptr_t)__dtcm_task_heap_start;
    uintptr_t end   = (uintptr_t)__dtcm_task_heap_end;
    start = (start + TASK_MEM_ALIGN - 1U) & ~(uintptr_t)(TASK_MEM_ALIGN - 1U);
    end   = end & ~(uintptr_t)(TASK_MEM_ALIGN - 1U);
    if (end < start + sizeof(dtcm_block_t) + TASK_MEM_ALIGN) {
        return false;
    }

    s_heap_start = (uint8_t *)start;
    s_heap_end   = (uint8_t *)end;
    s_first_block = (dtcm_block_t *)s_heap_start;
    s_first_block->size  = (size_t)(s_heap_end - s_heap_start) - sizeof(*s_first_block);
    s_first_block->next  = NULL;
    s_first_block->free  = 1U;
    s_first_block->magic = DTCM_BLOCK_MAGIC;
    s_initialized = true;
    return true;
}

static void *dtcm_alloc_locked(size_t wanted)
{
    if (!dtcm_heap_init_locked() || wanted == 0U) {
        return NULL;
    }

    wanted = align_up(wanted, TASK_MEM_ALIGN);
    for (dtcm_block_t *block = s_first_block; block != NULL; block = block->next) {
        if (!block->free || block->size < wanted) {
            continue;
        }

        size_t remaining = block->size - wanted;
        if (remaining >= sizeof(dtcm_block_t) + TASK_MEM_ALIGN) {
            dtcm_block_t *split = (dtcm_block_t *)((uint8_t *)(block + 1) + wanted);
            split->size  = remaining - sizeof(*split);
            split->next  = block->next;
            split->free  = 1U;
            split->magic = DTCM_BLOCK_MAGIC;
            block->next  = split;
            block->size  = wanted;
        }

        block->free = 0U;
        return block + 1;
    }

    return NULL;
}

static void dtcm_free_locked(void *ptr)
{
    if (ptr == NULL || s_heap_start == NULL) {
        return;
    }

    dtcm_block_t *block = (dtcm_block_t *)ptr - 1;
    configASSERT(block->magic == DTCM_BLOCK_MAGIC);
    configASSERT((uint8_t *)block >= s_heap_start);
    configASSERT((uint8_t *)(block + 1) <= s_heap_end);
    block->free = 1U;

    for (dtcm_block_t *current = s_first_block; current != NULL; current = current->next) {
        while (current->free && current->next != NULL && current->next->free) {
            dtcm_block_t *next = current->next;
            current->size += sizeof(*next) + next->size;
            current->next = next->next;
        }
    }
}

void gd32h77x_task_mem_init(void)
{
    UBaseType_t mask = task_mem_lock();
    (void)dtcm_heap_init_locked();
    task_mem_unlock(mask);
}

void *pvPortMallocTask(size_t size)
{
    if (size == 0U || size > SIZE_MAX - sizeof(task_alloc_header_t) - TASK_MEM_ALIGN) {
        return NULL;
    }

    size_t payload_size = align_up(size, TASK_MEM_ALIGN);
    size_t total_size   = sizeof(task_alloc_header_t) + payload_size;

    UBaseType_t mask = task_mem_lock();
    task_alloc_header_t *header = (task_alloc_header_t *)dtcm_alloc_locked(total_size);
    if (header != NULL) {
        header->magic    = TASK_ALLOC_MAGIC;
        header->region   = TASK_ALLOC_REGION_DTCM;
        header->reserved = 0U;
        header->size     = size;
        ++s_dtcm_allocations;
    }
    task_mem_unlock(mask);

    if (header != NULL) {
        return header + 1;
    }

    header = (task_alloc_header_t *)pvPortMalloc(total_size);
    if (header == NULL) {
        return NULL;
    }

    header->magic    = TASK_ALLOC_MAGIC;
    header->region   = TASK_ALLOC_REGION_HEAP;
    header->reserved = 0U;
    header->size     = size;
    ++s_axisram_fallbacks;
    return header + 1;
}

void vPortFreeTask(void *ptr)
{
    if (ptr == NULL) {
        return;
    }

    task_alloc_header_t *header = (task_alloc_header_t *)ptr - 1;
    configASSERT(header->magic == TASK_ALLOC_MAGIC);
    header->magic = 0U;
    ++s_free_calls;

    if (header->region == TASK_ALLOC_REGION_DTCM) {
        UBaseType_t mask = task_mem_lock();
        dtcm_free_locked(header);
        task_mem_unlock(mask);
    } else {
        configASSERT(header->region == TASK_ALLOC_REGION_HEAP);
        vPortFree(header);
    }
}

void *pvPortMallocStack(size_t size)
{
    return pvPortMallocTask(size);
}

void vPortFreeStack(void *ptr)
{
    vPortFreeTask(ptr);
}

void gd32h77x_task_mem_get_stats(gd32h77x_task_mem_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }

    UBaseType_t mask = task_mem_lock();
    (void)dtcm_heap_init_locked();
    memset(stats, 0, sizeof(*stats));
    stats->total_bytes         = (size_t)(s_heap_end - s_heap_start);
    stats->dtcm_allocations   = s_dtcm_allocations;
    stats->axisram_fallbacks  = s_axisram_fallbacks;
    stats->free_calls         = s_free_calls;
    for (dtcm_block_t *block = s_first_block; block != NULL; block = block->next) {
        if (block->free) {
            stats->free_bytes += block->size;
        }
    }
    stats->used_bytes = stats->total_bytes - stats->free_bytes;
    task_mem_unlock(mask);
}
