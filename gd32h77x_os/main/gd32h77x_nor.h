#ifndef GD32H77X_NOR_H
#define GD32H77X_NOR_H

#include <stdint.h>
#include "gd32h77x_memory_layout.h"

/* Low-level OSPI NOR protocol API. These entry points are not routed by TKL Flash. */
int gd32h77x_nor_init(uint32_t device_index);
int gd32h77x_nor_get_jedec_id(uint32_t device_index, uint32_t *jedec_id);
int gd32h77x_nor_read(uint32_t device_index, uint32_t offset,
                      void *destination, uint32_t size);
int gd32h77x_nor_program(uint32_t device_index, uint32_t offset,
                         const void *source, uint32_t size);
int gd32h77x_nor_erase_4k(uint32_t device_index, uint32_t offset);
int gd32h77x_nor_binding_valid(const gd32h77x_layout_device_t *device,
                              const gd32h77x_layout_binding_t *binding);

#endif
