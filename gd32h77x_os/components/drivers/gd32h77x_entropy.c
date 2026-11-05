/**
 * @file gd32h77x_entropy.c
 * @brief GD32H77x TRNG adapter entry point.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "gd32h77x_entropy.h"

#ifndef GD32H77X_ENTROPY_TEST
#include "gd32h77x_78x_rcu.h"
#include "gd32h77x_78x_trng.h"
#endif
#include "gd32h77x_status.h"

#include <stdbool.h>
#include <string.h>

#define GD32H77X_TRNG_POLL_LIMIT  UINT32_C(4000000)
#define GD32H77X_TRNG_SAMPLE_SIZE GD32H77X_ENTROPY_SAMPLE_SIZE
#define GD32H77X_TRNG_RECOVER_ATTEMPTS 3U

#ifdef GD32H77X_ENTROPY_TEST
static gd32h77x_entropy_sample_reader_t s_test_sample_reader;
static void                            *s_test_context;
#endif

static volatile uint32_t s_busy;
static bool              s_initialized;
static bool              s_failed;
static bool              s_have_previous;
static uint8_t           s_previous[GD32H77X_TRNG_SAMPLE_SIZE];
static uint8_t           s_pending[GD32H77X_TRNG_SAMPLE_SIZE];
static size_t            s_pending_offset = GD32H77X_TRNG_SAMPLE_SIZE;
static uint32_t          s_stage;
static gd32h77x_entropy_diagnostics_t s_diagnostics;

static void gd32h77x_entropy_record_failure(gd32h77x_status_t result)
{
    if (s_diagnostics.error != 0) {
        return;
    }
    s_diagnostics.error = result;
    s_diagnostics.stage = s_stage;
#ifndef GD32H77X_ENTROPY_TEST
    s_diagnostics.rcu_addctl0 = RCU_ADDCTL0;
    s_diagnostics.rcu_ctl = RCU_CTL;
    s_diagnostics.trng_clock_enabled = (RCU_AHB2EN & RCU_AHB2EN_TRNGEN) != 0U;
    if (s_diagnostics.trng_clock_enabled != 0U) {
        s_diagnostics.trng_stat = TRNG_STAT;
        s_diagnostics.trng_ctl = TRNG_CTL;
        s_diagnostics.trng_htcfg = TRNG_HTCFG;
        s_diagnostics.trng_registers_valid = 1U;
    }
#endif
}

#ifndef GD32H77X_ENTROPY_TEST
static gd32h77x_status_t gd32h77x_trng_wait_ready(void)
{
    s_stage = GD32H77X_ENTROPY_STAGE_WAIT_READY;
    for (uint32_t attempt = 0U; attempt < GD32H77X_TRNG_POLL_LIMIT; ++attempt) {
        uint32_t status = TRNG_STAT;
        /* Match the vendor H77 mbedTLS port: DRDY means the sample is
         * readable. NIST health flags may remain latched alongside DRDY and
         * must not turn every subsequent sample into a fatal error. */
        if ((status & TRNG_STAT_DRDY) != 0U) {
            s_diagnostics.last_ready_stat = status;
            return GD32H77X_STATUS_OK;
        }
        if ((status & (TRNG_STAT_CECS | TRNG_STAT_SECS | TRNG_STAT_ERRSTA |
                       TRNG_STAT_CEIF | TRNG_STAT_SEIF)) != 0U) {
            return GD32H77X_STATUS_IO_ERROR;
        }
    }
    return GD32H77X_STATUS_TIMEOUT;
}
#endif

static gd32h77x_status_t gd32h77x_trng_start(void)
{
#ifdef GD32H77X_ENTROPY_TEST
    return s_test_sample_reader != NULL ? GD32H77X_STATUS_OK : GD32H77X_STATUS_NOT_READY;
#else
    s_stage = GD32H77X_ENTROPY_STAGE_CLOCK_INIT;
    rcu_osci_on(RCU_IRC48M);
    uint32_t clock_timeout = GD32H77X_TRNG_POLL_LIMIT;
    while (rcu_flag_get(RCU_FLAG_IRC48MSTB) != SET && clock_timeout != 0U) {
        --clock_timeout;
    }
    if (clock_timeout == 0U) {
        return GD32H77X_STATUS_TIMEOUT;
    }
    rcu_ck48m_clock_config(RCU_CK48MSRC_IRC48M);
    rcu_periph_clock_enable(RCU_TRNG);
    trng_deinit();

    /* Use the vendor H77 configuration: NIST mode, CK48M / 8 and high analog
     * power. The conditioning block is optional and enabling it on this
     * silicon leaves the generator in GSSTA without ever asserting DRDY.
     *
     * With HCLK at 600 MHz, clock-error detection cannot be enabled at the
     * recommended 6 MHz TRNG clock: its documented threshold is HCLK / 16. */
    s_stage = GD32H77X_ENTROPY_STAGE_TRNG_CONFIG;
    trng_clockdiv_config(TRNG_CLK_DIV8);
    trng_powermode_config(TRNG_NR_HIGH);
    trng_mode_config(TRNG_MODSEL_NIST);
    trng_clockerror_detection_disable();
    trng_enable();

    return GD32H77X_STATUS_OK;
#endif
}

static gd32h77x_status_t gd32h77x_trng_sample(uint8_t sample[GD32H77X_TRNG_SAMPLE_SIZE])
{
#ifdef GD32H77X_ENTROPY_TEST
    s_stage = GD32H77X_ENTROPY_STAGE_READ_SAMPLE;
    if (s_test_sample_reader == NULL) {
        return GD32H77X_STATUS_NOT_READY;
    }
    return s_test_sample_reader(s_test_context, sample);
#else
    gd32h77x_status_t result = gd32h77x_trng_wait_ready();
    if (result != GD32H77X_STATUS_OK) {
        return result;
    }

    for (size_t i = 0U; i < GD32H77X_TRNG_SAMPLE_SIZE / sizeof(uint32_t); ++i) {
        s_stage = GD32H77X_ENTROPY_STAGE_READ_SAMPLE;
        uint32_t word       = trng_get_true_random_data();
        sample[i * 4U + 0U] = (uint8_t)word;
        sample[i * 4U + 1U] = (uint8_t)(word >> 8U);
        sample[i * 4U + 2U] = (uint8_t)(word >> 16U);
        sample[i * 4U + 3U] = (uint8_t)(word >> 24U);
    }

    s_stage = GD32H77X_ENTROPY_STAGE_WAIT_READY;
    return GD32H77X_STATUS_OK;
#endif
}

static gd32h77x_status_t gd32h77x_trng_next_sample(void)
{
    uint8_t sample[GD32H77X_TRNG_SAMPLE_SIZE];
    int     result;

    /* Discard the first generated NIST sample as required by the manual. */
    if (!s_have_previous) {
        result = gd32h77x_trng_sample(sample);
        if (result == GD32H77X_STATUS_OK) {
            memcpy(s_previous, sample, sizeof(sample));
            s_have_previous = true;
        }
        memset(sample, 0, sizeof(sample));
        if (result != GD32H77X_STATUS_OK) {
            return result;
        }
    }

    result = gd32h77x_trng_sample(sample);
    if (result == GD32H77X_STATUS_OK) {
        s_stage = GD32H77X_ENTROPY_STAGE_CONTINUOUS_TEST;
        if (memcmp(sample, s_previous, sizeof(sample)) == 0) {
            result = GD32H77X_STATUS_IO_ERROR;
        }
    }
    if (result == GD32H77X_STATUS_OK) {
        memcpy(s_previous, sample, sizeof(sample));
        memcpy(s_pending, sample, sizeof(sample));
        s_pending_offset = 0U;
    }
    memset(sample, 0, sizeof(sample));
    return result;
}

static gd32h77x_status_t gd32h77x_trng_recover(void)
{
#ifdef GD32H77X_ENTROPY_TEST
    s_have_previous  = false;
    s_pending_offset = sizeof(s_pending);
    return GD32H77X_STATUS_OK;
#else
    trng_disable();
    trng_interrupt_flag_clear(TRNG_INT_FLAG_CEIF);
    trng_interrupt_flag_clear(TRNG_INT_FLAG_SEIF);
    trng_deinit();

    s_have_previous  = false;
    s_pending_offset = sizeof(s_pending);
    memset(s_previous, 0, sizeof(s_previous));
    memset(s_pending, 0, sizeof(s_pending));
    return gd32h77x_trng_start();
#endif
}

gd32h77x_status_t gd32h77x_entropy_read(uint8_t *buffer, size_t size)
{
    if (size != 0U && buffer == NULL) {
        return GD32H77X_STATUS_INVALID_PARAM;
    }
    if (size == 0U) {
        return GD32H77X_STATUS_OK;
    }
    if (__atomic_test_and_set(&s_busy, __ATOMIC_ACQUIRE)) {
        return GD32H77X_STATUS_NOT_READY;
    }

    uint8_t *request_buffer = buffer;
    size_t   request_size   = size;
    gd32h77x_status_t result = GD32H77X_STATUS_OK;
    if (s_failed) {
        result = GD32H77X_STATUS_IO_ERROR;
        goto fail;
    }
    if (!s_initialized) {
        result = gd32h77x_trng_start();
        if (result != GD32H77X_STATUS_OK) {
            goto fail;
        }
        s_initialized = true;
    }

    uint32_t recover_attempts = 0U;
    while (size != 0U) {
        if (s_pending_offset == sizeof(s_pending)) {
            result = gd32h77x_trng_next_sample();
            if (result != GD32H77X_STATUS_OK) {
                if (result == GD32H77X_STATUS_IO_ERROR && recover_attempts < GD32H77X_TRNG_RECOVER_ATTEMPTS) {
                    gd32h77x_entropy_record_failure(result);
                    ++recover_attempts;
                    result = gd32h77x_trng_recover();
                    if (result != GD32H77X_STATUS_OK) {
                        goto fail;
                    }
                    s_initialized = true;
                    continue;
                }
                goto fail;
            }
        }
        size_t available = sizeof(s_pending) - s_pending_offset;
        size_t count     = size < available ? size : available;
        memcpy(buffer, &s_pending[s_pending_offset], count);
        memset(&s_pending[s_pending_offset], 0, count);
        buffer += count;
        size -= count;
        s_pending_offset += count;
    }
    goto exit;

fail:
    gd32h77x_entropy_record_failure(result);
    s_failed = true;
    memset(s_previous, 0, sizeof(s_previous));
    memset(s_pending, 0, sizeof(s_pending));
    s_pending_offset = sizeof(s_pending);
    memset(request_buffer, 0, request_size);

exit:
    __atomic_clear(&s_busy, __ATOMIC_RELEASE);
    return result;
}

void gd32h77x_entropy_get_diagnostics(gd32h77x_entropy_diagnostics_t *diagnostics)
{
    if (diagnostics != NULL) {
        memset(diagnostics, 0, sizeof(*diagnostics));
        if (__atomic_test_and_set(&s_busy, __ATOMIC_ACQUIRE)) {
            return;
        }
        *diagnostics = s_diagnostics;
        diagnostics->snapshot_available = 1U;
        __atomic_clear(&s_busy, __ATOMIC_RELEASE);
    }
}

#ifdef GD32H77X_ENTROPY_TEST
void gd32h77x_entropy_test_set_sample_reader(gd32h77x_entropy_sample_reader_t reader, void *context)
{
    s_test_sample_reader = reader;
    s_test_context       = context;
}

void gd32h77x_entropy_test_reset(void)
{
    s_busy           = 0U;
    s_initialized    = false;
    s_failed         = false;
    s_have_previous  = false;
    s_pending_offset = sizeof(s_pending);
    s_stage = GD32H77X_ENTROPY_STAGE_NONE;
    memset(&s_diagnostics, 0, sizeof(s_diagnostics));
    memset(s_previous, 0, sizeof(s_previous));
    memset(s_pending, 0, sizeof(s_pending));
}
#endif
