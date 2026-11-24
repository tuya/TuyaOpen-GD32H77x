/**
 * @file tkl_memory.h
 * @brief Common process - adapter the semaphore api provide by OS
 * @version 0.1
 * @date 2020-11-09
 *
 * @copyright Copyright 2021-2030 Tuya Inc. All Rights Reserved.
 *
 */
#ifndef __TKL_MEMORY_H__
#define __TKL_MEMORY_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Alloc memory of system
 *
 * @param[in] size: memory size
 *
 * @note This API is used to alloc memory of system.
 *
 * @return the memory address malloced
 */
void *tkl_system_malloc(size_t size);

/**
 * @brief Free memory of system
 *
 * @param[in] ptr: memory point
 *
 * @note This API is used to free memory of system.
 *
 * @return void
 */
void tkl_system_free(void *ptr);

/**
* @brief set memory
*
* @param[in] size: memory size
*
* @note This API is used to alloc memory of system.
*
* @return the memory address malloced
*/
void *tkl_system_memset(void* src, int ch, const size_t n);

/**
* @brief Alloc memory of system
*
* @param[in] size: memory size
*
* @note This API is used to alloc memory of system.
*
* @return the memory address malloced
*/
void *tkl_system_memcpy(void* src, const void* dst, const size_t n);

/**
 * @brief Allocate and clear the memory
 *
 * @param[in]       nitems      the numbers of memory block
 * @param[in]       size        the size of the memory block
 *
 * @return the memory address calloced
 */
void *tkl_system_calloc(size_t nitems, size_t size);

/**
 * @brief Re-allocate the memory
 *
 * @param[in]       nitems      source memory address
 * @param[in]       size        the size after re-allocate
 *
 * @return void
 */
void *tkl_system_realloc(void *ptr, size_t size);

/**
 * @brief Get system free heap size
 *
 * @param none
 *
 * @return heap size
 */
int tkl_system_get_free_heap_size(void);

/**
 * @brief Initialize the platform PSRAM device.
 *
 * In indirect mode this only validates and starts the device. The explicit
 * gd32h77x_psram_read/write/fill APIs are used for data access. In mapped mode
 * it also creates the TKL PSRAM heap. The implementation is compiled only when
 * ENABLE_GD32H77X_PSRAM is enabled.
 */
OPERATE_RET tkl_system_psram_init(void);

#if (defined(ENABLE_PSRAM) && (ENABLE_PSRAM == 1)) || \
    (defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1))
#define TKL_SYSTEM_PSRAM_ALIGNMENT 32U

/**
 * @brief Allocate a 32-byte aligned block from mapped PSRAM.
 */
void *tkl_system_psram_malloc(size_t size);

/**
 * @brief Free memory allocated from the external PSRAM heap.
 */
void tkl_system_psram_free(void *ptr);

/**
 * @brief Allocate and clear memory from the external PSRAM heap.
 */
void *tkl_system_psram_calloc(size_t nitems, size_t size);

/**
 * @brief Reallocate memory from the external PSRAM heap.
 */
void *tkl_system_psram_realloc(void *ptr, size_t size);

/**
 * @brief Get the free size of the external PSRAM heap.
 */
int tkl_system_psram_get_free_heap_size(void);
#endif

/**
 * @brief Compare two pieces of memory
 *
 * @param[in]       str1        memory 1
 * @param[in]       str2        memory 2
 * @param[in]       n           number of comparisons
 *
 * @return 0 means that the two pieces of memory are the same
 */
int tkl_system_memcmp(const void *str1, const void *str2, size_t n);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
