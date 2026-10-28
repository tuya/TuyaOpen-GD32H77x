/**
 * @file mock_cnvm.h
 * @brief Host mock controls and operation counters for the CNVM adapter.
 */
#ifndef GD32H77X_TEST_MOCK_CNVM_H
#define GD32H77X_TEST_MOCK_CNVM_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t read_requests;
    uint32_t program_requests;
    uint32_t erase_requests;
    uint32_t program_commands;
    uint32_t erase_commands;
    uint32_t last_address;
    uint32_t last_size;
} mock_cnvm_stats_t;

void mock_cnvm_reset(void);
void mock_cnvm_set_busy(bool busy);
void mock_cnvm_set_dieen(bool enabled);
void mock_cnvm_fail_next_read(void);
void mock_cnvm_fail_next_program(void);
void mock_cnvm_fail_next_erase(void);
const mock_cnvm_stats_t *mock_cnvm_stats(void);

#endif /* GD32H77X_TEST_MOCK_CNVM_H */
