/**
 * @file test_entropy.c
 * @brief Exercise fail-closed entropy handling through a mock TRNG source.
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "gd32h77x_entropy.h"

#include "tuya_error_code.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint8_t         samples[4][GD32H77X_ENTROPY_SAMPLE_SIZE];
    int             results[4];
    size_t          count;
    size_t          index;
    pthread_mutex_t mutex;
    pthread_cond_t  condition;
    int             block_first;
    int             entered;
    int             release;
} mock_trng_t;

static int mock_read_sample(void *context, uint8_t sample[GD32H77X_ENTROPY_SAMPLE_SIZE])
{
    mock_trng_t *mock = context;
    pthread_mutex_lock(&mock->mutex);
    size_t index = mock->index++;
    if (mock->block_first && index == 0U) {
        mock->entered = 1;
        pthread_cond_broadcast(&mock->condition);
        while (!mock->release) {
            pthread_cond_wait(&mock->condition, &mock->mutex);
        }
    }
    int result = index < mock->count ? mock->results[index] : OPRT_COM_ERROR;
    if (result == OPRT_OK) {
        memcpy(sample, mock->samples[index], GD32H77X_ENTROPY_SAMPLE_SIZE);
    }
    pthread_mutex_unlock(&mock->mutex);
    return result;
}

static void mock_init(mock_trng_t *mock)
{
    memset(mock, 0, sizeof(*mock));
    assert(pthread_mutex_init(&mock->mutex, NULL) == 0);
    assert(pthread_cond_init(&mock->condition, NULL) == 0);
    for (size_t i = 0U; i < 4U; ++i) {
        mock->results[i] = OPRT_OK;
    }
}

static void mock_destroy(mock_trng_t *mock)
{
    assert(pthread_cond_destroy(&mock->condition) == 0);
    assert(pthread_mutex_destroy(&mock->mutex) == 0);
}

static void mock_pattern(uint8_t sample[GD32H77X_ENTROPY_SAMPLE_SIZE], uint8_t seed)
{
    for (size_t i = 0U; i < GD32H77X_ENTROPY_SAMPLE_SIZE; ++i) {
        sample[i] = (uint8_t)(seed + i);
    }
}

static void begin_test(mock_trng_t *mock)
{
    gd32h77x_entropy_test_reset();
    gd32h77x_entropy_test_set_sample_reader(mock_read_sample, mock);
}

static void test_first_sample_discard_and_partial_cache(void)
{
    mock_trng_t mock;
    mock_init(&mock);
    mock.count = 3U;
    mock_pattern(mock.samples[0], 0x10U);
    mock_pattern(mock.samples[1], 0x40U);
    mock_pattern(mock.samples[2], 0x80U);
    begin_test(&mock);

    uint8_t first[7];
    uint8_t rest[GD32H77X_ENTROPY_SAMPLE_SIZE - sizeof(first)];
    assert(gd32h77x_entropy_read(first, sizeof(first)) == OPRT_OK);
    assert(gd32h77x_entropy_read(rest, sizeof(rest)) == OPRT_OK);
    assert(mock.index == 2U);
    assert(memcmp(first, mock.samples[1], sizeof(first)) == 0);
    assert(memcmp(rest, &mock.samples[1][sizeof(first)], sizeof(rest)) == 0);
    mock_destroy(&mock);
}

static void test_failure_clears_whole_request(void)
{
    mock_trng_t mock;
    mock_init(&mock);
    mock.count      = 3U;
    mock.results[2] = OPRT_TIMEOUT;
    mock_pattern(mock.samples[0], 0x10U);
    mock_pattern(mock.samples[1], 0x40U);
    begin_test(&mock);

    uint8_t output[40];
    memset(output, 0xA5, sizeof(output));
    assert(gd32h77x_entropy_read(output, sizeof(output)) == OPRT_TIMEOUT);
    for (size_t i = 0U; i < sizeof(output); ++i) {
        assert(output[i] == 0U);
    }
    mock_destroy(&mock);
}

static void test_health_error_clears_request(void)
{
    mock_trng_t mock;
    mock_init(&mock);
    mock.count      = 2U;
    mock.results[1] = OPRT_COM_ERROR;
    mock_pattern(mock.samples[0], 0x10U);
    begin_test(&mock);

    uint8_t output[16];
    memset(output, 0xA5, sizeof(output));
    assert(gd32h77x_entropy_read(output, sizeof(output)) == OPRT_COM_ERROR);
    for (size_t i = 0U; i < sizeof(output); ++i) {
        assert(output[i] == 0U);
    }
    gd32h77x_entropy_diagnostics_t diagnostics = {0};
    gd32h77x_entropy_get_diagnostics(&diagnostics);
    assert(diagnostics.snapshot_available == 1U);
    assert(diagnostics.error == OPRT_COM_ERROR);
    assert(diagnostics.stage == GD32H77X_ENTROPY_STAGE_READ_SAMPLE);
    assert(gd32h77x_entropy_read(output, sizeof(output)) == OPRT_COM_ERROR);
    gd32h77x_entropy_diagnostics_t latched = {0};
    gd32h77x_entropy_get_diagnostics(&latched);
    assert(latched.snapshot_available == 1U);
    assert(latched.error == diagnostics.error && latched.stage == diagnostics.stage);
    mock_destroy(&mock);
}

static void test_health_error_recovers_once(void)
{
    mock_trng_t mock;
    mock_init(&mock);
    mock.count      = 4U;
    mock.results[1] = OPRT_COM_ERROR;
    mock_pattern(mock.samples[0], 0x10U);
    mock_pattern(mock.samples[2], 0x20U);
    mock_pattern(mock.samples[3], 0x40U);
    begin_test(&mock);

    uint8_t output[GD32H77X_ENTROPY_SAMPLE_SIZE];
    assert(gd32h77x_entropy_read(output, sizeof(output)) == OPRT_OK);
    assert(memcmp(output, mock.samples[3], sizeof(output)) == 0);

    gd32h77x_entropy_diagnostics_t diagnostics = {0};
    gd32h77x_entropy_get_diagnostics(&diagnostics);
    assert(diagnostics.snapshot_available == 1U);
    assert(diagnostics.error == OPRT_COM_ERROR);
    mock_destroy(&mock);
}

static void test_repeated_sample_fails_closed(void)
{
    mock_trng_t mock;
    mock_init(&mock);
    mock.count = 2U;
    mock_pattern(mock.samples[0], 0x10U);
    memcpy(mock.samples[1], mock.samples[0], sizeof(mock.samples[0]));
    begin_test(&mock);

    uint8_t output[8];
    memset(output, 0xA5, sizeof(output));
    assert(gd32h77x_entropy_read(output, sizeof(output)) == OPRT_COM_ERROR);
    for (size_t i = 0U; i < sizeof(output); ++i) {
        assert(output[i] == 0U);
    }
    gd32h77x_entropy_diagnostics_t diagnostics = {0};
    gd32h77x_entropy_get_diagnostics(&diagnostics);
    assert(diagnostics.error == OPRT_COM_ERROR);
    assert(diagnostics.stage == GD32H77X_ENTROPY_STAGE_CONTINUOUS_TEST);
    mock_destroy(&mock);
}

typedef struct {
    uint8_t output[16];
    int     result;
} reader_thread_t;

static void *reader_thread(void *context)
{
    reader_thread_t *thread = context;
    thread->result          = gd32h77x_entropy_read(thread->output, sizeof(thread->output));
    return NULL;
}

static void test_busy_call_does_not_touch_output_or_shared_state(void)
{
    mock_trng_t mock;
    mock_init(&mock);
    mock.count       = 2U;
    mock.block_first = 1;
    mock_pattern(mock.samples[0], 0x10U);
    mock_pattern(mock.samples[1], 0x40U);
    begin_test(&mock);

    reader_thread_t thread = {0};
    pthread_t       worker;
    assert(pthread_create(&worker, NULL, reader_thread, &thread) == 0);
    pthread_mutex_lock(&mock.mutex);
    while (!mock.entered) {
        pthread_cond_wait(&mock.condition, &mock.mutex);
    }
    pthread_mutex_unlock(&mock.mutex);

    uint8_t concurrent_output[8];
    memset(concurrent_output, 0xA5, sizeof(concurrent_output));
    assert(gd32h77x_entropy_read(concurrent_output, sizeof(concurrent_output)) == OPRT_RESOURCE_NOT_READY);
    for (size_t i = 0U; i < sizeof(concurrent_output); ++i) {
        assert(concurrent_output[i] == 0xA5U);
    }
    gd32h77x_entropy_diagnostics_t unavailable = {0};
    gd32h77x_entropy_get_diagnostics(&unavailable);
    assert(unavailable.snapshot_available == 0U);

    pthread_mutex_lock(&mock.mutex);
    mock.release = 1;
    pthread_cond_broadcast(&mock.condition);
    pthread_mutex_unlock(&mock.mutex);
    assert(pthread_join(worker, NULL) == 0);
    assert(thread.result == OPRT_OK);
    assert(memcmp(thread.output, mock.samples[1], sizeof(thread.output)) == 0);
    mock_destroy(&mock);
}

int main(void)
{
    test_first_sample_discard_and_partial_cache();
    test_failure_clears_whole_request();
    test_health_error_clears_request();
    test_health_error_recovers_once();
    test_repeated_sample_fails_closed();
    test_busy_call_does_not_touch_output_or_shared_state();
    return 0;
}
