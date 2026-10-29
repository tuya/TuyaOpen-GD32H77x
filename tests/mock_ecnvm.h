/** @file mock_ecnvm.h @brief Host mock for the mapped ECNVM adapter. */
#ifndef GD32H77X_TEST_MOCK_ECNVM_H
#define GD32H77X_TEST_MOCK_ECNVM_H

#include <stdint.h>

typedef struct {
    uint32_t read_requests;
    uint32_t program_requests;
    uint32_t erase_requests;
    uint32_t program_commands;
    uint32_t erase_commands;
    uint32_t last_address;
    uint32_t last_size;
} mock_ecnvm_stats_t;

void mock_ecnvm_reset(void);
const mock_ecnvm_stats_t *mock_ecnvm_stats(void);

#endif /* GD32H77X_TEST_MOCK_ECNVM_H */
