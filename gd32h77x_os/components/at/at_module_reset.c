/**
 * @file at_module_reset.c
 * @brief Bounded AT soft-reset and hardware-fallback orchestration.
 */
#include "at_module_reset.h"

#include <string.h>

static bool soft_reset_path(const at_module_reset_ops_t *ops, void *context, uint32_t ready_timeout_ms,
                            at_module_reset_result_t *result)
{
    /* A failed probe can leave the AT session poisoned. Restore the local
     * transport before trying to put AT+RST on the wire, otherwise the reset
     * command is rejected before transmission. */
    if (!ops->recover_transport(context)) return false;
    result->software_reset_requested = true;
    return ops->request_soft_reset(context) && ops->recover_transport(context) &&
           ops->wait_ready(context, ready_timeout_ms) && ops->recover_transport(context) &&
           ops->probe(context);
}

static bool hardware_reset_path(const at_module_reset_ops_t *ops, void *context,
                                uint32_t ready_timeout_ms,
                                at_module_reset_result_t *result)
{
    result->hardware_reset_attempted = true;
    return ops->hardware_reset(context) && ops->recover_transport(context) &&
           ops->wait_ready(context, ready_timeout_ms) && ops->recover_transport(context) &&
           ops->probe(context);
}

bool at_module_reset_run(at_module_reset_mode_t mode, const at_module_reset_ops_t *ops,
                            void *context, uint32_t ready_timeout_ms,
                            at_module_reset_result_t *result)
{
    if (!ops || !result || !ops->probe || !ops->request_soft_reset || !ops->wait_ready ||
        !ops->recover_transport || !ops->hardware_reset || !ready_timeout_ms ||
        (unsigned)mode > (unsigned)AT_MODULE_RESET_SOFT_FIRST) {
        return false;
    }
    memset(result, 0, sizeof(*result));

    if (mode == AT_MODULE_RESET_RECOVERY &&
        ops->recover_transport(context) && ops->probe(context)) {
        return true;
    }
    if (mode == AT_MODULE_RESET_STARTUP) {
        const bool probed = ops->probe(context);
        const bool ready_observed = ops->ready_observed && ops->ready_observed(context);
        if (probed && ready_observed) {
            /* READY followed by the probe's own OK proves a clean boot. Do not
             * issue AT+RST and create a second startup transition. */
            return true;
        }
        if (!probed) {
            /* If a boot READY arrived but the probe still failed, recover the
             * local transport and prove liveness once before treating the
             * module as unresponsive. */
            if (ready_observed && ops->recover_transport(context) && ops->probe(context)) {
                return true;
            }
            return hardware_reset_path(ops, context, ready_timeout_ms, result);
        }
        return soft_reset_path(ops, context, ready_timeout_ms, result) ||
               hardware_reset_path(ops, context, ready_timeout_ms, result);
    }
    return soft_reset_path(ops, context, ready_timeout_ms, result) ||
           hardware_reset_path(ops, context, ready_timeout_ms, result);
}
