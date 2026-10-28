/**
 * @file gd32h77x_entropy.h
 * @brief Fail-closed hardware entropy adapter.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */
#ifndef GD32H77X_ENTROPY_H
#define GD32H77X_ENTROPY_H

#include <stddef.h>
#include <stdint.h>

#define GD32H77X_ENTROPY_SAMPLE_SIZE 16U

typedef enum {
    GD32H77X_ENTROPY_STAGE_NONE = 0,
    GD32H77X_ENTROPY_STAGE_CLOCK_INIT = 1,
    GD32H77X_ENTROPY_STAGE_TRNG_CONFIG = 2,
    GD32H77X_ENTROPY_STAGE_WAIT_READY = 3,
    GD32H77X_ENTROPY_STAGE_READ_SAMPLE = 4,
    GD32H77X_ENTROPY_STAGE_CONTINUOUS_TEST = 5,
} gd32h77x_entropy_stage_t;

/**
 * Read bytes from the GD32H77x hardware TRNG.
 *
 * Returns OPRT_OK on success or a Tuya error code on failure. On a failure
 * after this call acquires the generator, the entire requested output range is
 * cleared. If another call already owns the generator, returns
 * OPRT_RESOURCE_NOT_READY without touching the output buffer or shared state;
 * concurrent callers must use non-overlapping output buffers.
 */
int gd32h77x_entropy_read(uint8_t *buffer, size_t size);

/* First-failure register snapshot. This contains status/configuration only,
 * never generated entropy or random output bytes. */
typedef struct {
    int32_t  error;
    uint32_t stage;
    uint32_t trng_stat;
    uint32_t trng_ctl;
    uint32_t trng_htcfg;
    uint32_t rcu_addctl0;
    uint32_t rcu_ctl;
    uint32_t trng_clock_enabled;
    uint32_t trng_registers_valid;
    uint32_t last_ready_stat;
    uint32_t snapshot_available;
} gd32h77x_entropy_diagnostics_t;

void gd32h77x_entropy_get_diagnostics(gd32h77x_entropy_diagnostics_t *diagnostics);

#if defined(GD32H77X_DIAG_CLI) && (GD32H77X_DIAG_CLI == 1)
/* Inject one synthetic source error so the real hardware recovery path can be
 * exercised on a board without waiting for a rare analog seed fault. */
void gd32h77x_entropy_diag_inject_failure_once(void);
#endif

#ifdef GD32H77X_ENTROPY_TEST
typedef int (*gd32h77x_entropy_sample_reader_t)(void *context, uint8_t sample[GD32H77X_ENTROPY_SAMPLE_SIZE]);
void gd32h77x_entropy_test_set_sample_reader(gd32h77x_entropy_sample_reader_t reader, void *context);
void gd32h77x_entropy_test_reset(void);
#endif

#endif
