/**
 * @file vw553_module_reset.h
 * @brief Host-testable VW553 startup and recovery reset sequencing.
 */
#ifndef VW553_MODULE_RESET_H
#define VW553_MODULE_RESET_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    VW553_MODULE_RESET_STARTUP,
    VW553_MODULE_RESET_RECOVERY,
    VW553_MODULE_RESET_SOFT_FIRST,
} vw553_module_reset_mode_t;

typedef struct {
    bool (*probe)(void *context);
    bool (*request_soft_reset)(void *context);
    bool (*wait_ready)(void *context, uint32_t timeout_ms);
    bool (*recover_transport)(void *context);
    bool (*hardware_reset)(void *context);
} vw553_module_reset_ops_t;

typedef struct {
    bool software_reset_requested;
    bool hardware_reset_attempted;
} vw553_module_reset_result_t;

/* Runs one bounded reset sequence. There are deliberately no implicit retries:
 * startup probes once, recovery first tries local transport recovery, then the
 * selected reset path uses AT+RST before the hardware reset fallback. */
bool vw553_module_reset_run(vw553_module_reset_mode_t mode, const vw553_module_reset_ops_t *ops,
                            void *context, uint32_t ready_timeout_ms,
                            vw553_module_reset_result_t *result);

#endif
