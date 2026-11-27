/**
 * @file gd32h77x_task_mem.h
 * @brief Transparent DTCM-first task memory allocator for GD32H77X.
 */
#ifndef GD32H77X_TASK_MEM_H
#define GD32H77X_TASK_MEM_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t   total_bytes;
    size_t   free_bytes;
    size_t   used_bytes;
    uint32_t dtcm_allocations;
    uint32_t axisram_fallbacks;
    uint32_t free_calls;
} gd32h77x_task_mem_stats_t;

void gd32h77x_task_mem_init(void);
void *pvPortMallocTask(size_t size);
void vPortFreeTask(void *ptr);
void *pvPortMallocStack(size_t size);
void vPortFreeStack(void *ptr);
void gd32h77x_task_mem_get_stats(gd32h77x_task_mem_stats_t *stats);

#endif /* GD32H77X_TASK_MEM_H */
