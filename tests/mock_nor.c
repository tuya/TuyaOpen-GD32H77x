#include "gd32h77x_nor.h"

int gd32h77x_nor_read(uint32_t device_index, uint32_t offset, void *destination, uint32_t size)
{
    (void)device_index;
    (void)offset;
    (void)destination;
    (void)size;
    return -1;
}

int gd32h77x_nor_program(uint32_t device_index, uint32_t offset, const void *source, uint32_t size)
{
    (void)device_index;
    (void)offset;
    (void)source;
    (void)size;
    return -1;
}

int gd32h77x_nor_erase_4k(uint32_t device_index, uint32_t offset)
{
    (void)device_index;
    (void)offset;
    return -1;
}
