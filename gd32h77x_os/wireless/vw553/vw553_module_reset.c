/**
 * @file vw553_module_reset.c
 * @brief Bounded VW553 soft-reset and hardware-fallback orchestration.
 */
#include "vw553_module_reset.h"

#include <string.h>

static bool soft_reset_path(const vw553_module_reset_ops_t *ops, void *context, uint32_t ready_timeout_ms,
                            vw553_module_reset_result_t *result)
{
    result->software_reset_requested = true;
    return ops->request_soft_reset(context) && ops->wait_ready(context, ready_timeout_ms) &&
           ops->recover_transport(context) && ops->probe(context);
}

static bool hardware_reset_path(const vw553_module_reset_ops_t *ops, void *context,
                                vw553_module_reset_result_t *result)
{
    result->hardware_reset_attempted = true;
    return ops->hardware_reset(context) && ops->recover_transport(context) && ops->probe(context);
}

bool vw553_module_reset_run(vw553_module_reset_mode_t mode, const vw553_module_reset_ops_t *ops,
                            void *context, uint32_t ready_timeout_ms,
                            vw553_module_reset_result_t *result)
{
    if (!ops || !result || !ops->probe || !ops->request_soft_reset || !ops->wait_ready ||
        !ops->recover_transport || !ops->hardware_reset || !ready_timeout_ms ||
        (unsigned)mode > (unsigned)VW553_MODULE_RESET_SOFT_FIRST) {
        return false;
    }
    memset(result, 0, sizeof(*result));

    if (mode == VW553_MODULE_RESET_RECOVERY &&
        ops->recover_transport(context) && ops->probe(context)) {
        return true;
    }
    if (mode == VW553_MODULE_RESET_STARTUP && !ops->probe(context)) {
        return hardware_reset_path(ops, context, result);
    }
    return soft_reset_path(ops, context, ready_timeout_ms, result) ||
           hardware_reset_path(ops, context, result);
}
