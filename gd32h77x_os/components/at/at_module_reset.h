/**
 * @file module_reset.h
 * @brief Host-testable AT startup and recovery reset sequencing.
 */
#ifndef AT_MODULE_RESET_H
#define AT_MODULE_RESET_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AT_MODULE_RESET_STARTUP,
    AT_MODULE_RESET_RECOVERY,
    AT_MODULE_RESET_SOFT_FIRST,
} at_module_reset_mode_t;

typedef struct {
    bool (*probe)(void *context);
    bool (*request_soft_reset)(void *context);
    bool (*wait_ready)(void *context, uint32_t timeout_ms);
    bool (*recover_transport)(void *context);
    bool (*hardware_reset)(void *context);
    /* Optional: a boot READY preempted the initial startup probe. */
    bool (*ready_observed)(void *context);
} at_module_reset_ops_t;

typedef struct {
    bool software_reset_requested;
    bool hardware_reset_attempted;
} at_module_reset_result_t;

/* Runs one bounded reset sequence. There are deliberately no command retries:
 * startup may recover once when a boot READY interrupted its first probe, so it
 * can distinguish a booting module from an unresponsive one. Recovery first
 * tries local transport recovery, then the selected reset path uses AT+RST
 * before the hardware reset fallback. */
bool at_module_reset_run(at_module_reset_mode_t mode, const at_module_reset_ops_t *ops,
                            void *context, uint32_t ready_timeout_ms,
                            at_module_reset_result_t *result);

#endif
