#include "vw553_module_reset.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char events[32];
    size_t length;
    unsigned probe_calls;
    unsigned soft_calls;
    unsigned ready_calls;
    unsigned recover_calls;
    unsigned hardware_calls;
    bool local_recovery;
    bool initial_probe_ok;
    bool local_probe_ok;
    bool soft_probe_ok;
    bool hardware_probe_ok;
    bool soft_ok;
    bool ready_ok;
    bool recover_ok;
    bool hardware_recover_ok;
    bool hardware_ok;
} fake_t;

static void append(fake_t *fake, char event)
{
    assert(fake->length + 1U < sizeof(fake->events));
    fake->events[fake->length++] = event;
    fake->events[fake->length] = '\0';
}

static bool fake_probe(void *context)
{
    fake_t *fake = context;
    ++fake->probe_calls;
    append(fake, 'p');
    if (fake->hardware_calls) return fake->hardware_probe_ok;
    if (fake->soft_calls) return fake->soft_probe_ok;
    if (fake->local_recovery && fake->recover_calls) return fake->local_probe_ok;
    return fake->initial_probe_ok;
}

static bool fake_soft_reset(void *context)
{
    fake_t *fake = context;
    ++fake->soft_calls;
    append(fake, 's');
    return fake->soft_ok;
}

static bool fake_wait_ready(void *context, uint32_t timeout_ms)
{
    fake_t *fake = context;
    ++fake->ready_calls;
    append(fake, 'w');
    return timeout_ms == 5000U && fake->ready_ok;
}

static bool fake_recover(void *context)
{
    fake_t *fake = context;
    ++fake->recover_calls;
    append(fake, 'r');
    return fake->hardware_calls ? fake->hardware_recover_ok : fake->recover_ok;
}

static bool fake_hardware_reset(void *context)
{
    fake_t *fake = context;
    ++fake->hardware_calls;
    append(fake, 'h');
    return fake->hardware_ok;
}

static const vw553_module_reset_ops_t OPS = {
    .probe = fake_probe,
    .request_soft_reset = fake_soft_reset,
    .wait_ready = fake_wait_ready,
    .recover_transport = fake_recover,
    .hardware_reset = fake_hardware_reset,
};

static fake_t initial_fake(void)
{
    return (fake_t){
        .initial_probe_ok = true,
        .local_probe_ok = true,
        .soft_probe_ok = true,
        .hardware_probe_ok = true,
        .soft_ok = true,
        .ready_ok = true,
        .recover_ok = true,
        .hardware_recover_ok = true,
        .hardware_ok = true,
    };
}

#define EXPECT_EVENTS(fake, ...) do { \
    const char expected_events[] = {__VA_ARGS__, '\0'}; \
    assert(strcmp((fake).events, expected_events) == 0); \
} while (0)

static void startup_prefers_soft_reset(void)
{
    fake_t fake = initial_fake();
    vw553_module_reset_result_t result;
    assert(vw553_module_reset_run(VW553_MODULE_RESET_STARTUP, &OPS, &fake, 5000U, &result));
    EXPECT_EVENTS(fake, 'p', 's', 'w', 'r', 'p');
    assert(fake.hardware_calls == 0U);
    assert(result.software_reset_requested && !result.hardware_reset_attempted);
}

static void startup_probe_failure_uses_hardware_fallback(void)
{
    fake_t fake = initial_fake();
    fake.initial_probe_ok = false;
    vw553_module_reset_result_t result;
    assert(vw553_module_reset_run(VW553_MODULE_RESET_STARTUP, &OPS, &fake, 5000U, &result));
    EXPECT_EVENTS(fake, 'p', 'h', 'r', 'p');
    assert(fake.soft_calls == 0U && fake.hardware_calls == 1U);
    assert(!result.software_reset_requested && result.hardware_reset_attempted);
}

static void soft_reset_failures_fall_back_once(void)
{
    fake_t no_command = initial_fake();
    no_command.soft_ok = false;
    vw553_module_reset_result_t result;
    assert(vw553_module_reset_run(VW553_MODULE_RESET_STARTUP, &OPS, &no_command, 5000U, &result));
    EXPECT_EVENTS(no_command, 'p', 's', 'h', 'r', 'p');
    assert(no_command.hardware_calls == 1U);

    fake_t no_ready = initial_fake();
    no_ready.ready_ok = false;
    assert(vw553_module_reset_run(VW553_MODULE_RESET_STARTUP, &OPS, &no_ready, 5000U, &result));
    EXPECT_EVENTS(no_ready, 'p', 's', 'w', 'h', 'r', 'p');
    assert(no_ready.hardware_calls == 1U);

    fake_t recover_failure = initial_fake();
    recover_failure.recover_ok = false;
    assert(vw553_module_reset_run(VW553_MODULE_RESET_STARTUP, &OPS, &recover_failure, 5000U, &result));
    EXPECT_EVENTS(recover_failure, 'p', 's', 'w', 'r', 'h', 'r', 'p');
    assert(recover_failure.hardware_calls == 1U);

    fake_t verify_failure = initial_fake();
    verify_failure.soft_probe_ok = false;
    assert(vw553_module_reset_run(VW553_MODULE_RESET_STARTUP, &OPS, &verify_failure, 5000U, &result));
    EXPECT_EVENTS(verify_failure, 'p', 's', 'w', 'r', 'p', 'h', 'r', 'p');
    assert(verify_failure.hardware_calls == 1U);
}

static void recovery_tries_transport_before_reset(void)
{
    fake_t local = initial_fake();
    local.local_recovery = true;
    vw553_module_reset_result_t result;
    assert(vw553_module_reset_run(VW553_MODULE_RESET_RECOVERY, &OPS, &local, 5000U, &result));
    EXPECT_EVENTS(local, 'r', 'p');
    assert(local.soft_calls == 0U && local.hardware_calls == 0U);

    fake_t fallback = initial_fake();
    fallback.local_recovery = true;
    fallback.local_probe_ok = false;
    assert(vw553_module_reset_run(VW553_MODULE_RESET_RECOVERY, &OPS, &fallback, 5000U, &result));
    EXPECT_EVENTS(fallback, 'r', 'p', 's', 'w', 'r', 'p');
    assert(fallback.soft_calls == 1U && fallback.hardware_calls == 0U);
}

static void soft_first_does_not_probe_before_reset(void)
{
    fake_t fake = initial_fake();
    vw553_module_reset_result_t result;
    assert(vw553_module_reset_run(VW553_MODULE_RESET_SOFT_FIRST, &OPS, &fake, 5000U, &result));
    EXPECT_EVENTS(fake, 's', 'w', 'r', 'p');
    assert(fake.soft_calls == 1U && fake.hardware_calls == 0U);
}

static void invalid_inputs_and_hardware_failure_are_terminal(void)
{
    vw553_module_reset_result_t result;
    fake_t fake = initial_fake();
    assert(!vw553_module_reset_run(VW553_MODULE_RESET_STARTUP, NULL, &fake, 5000U, &result));
    assert(!vw553_module_reset_run(VW553_MODULE_RESET_STARTUP, &OPS, &fake, 0U, &result));

    fake = initial_fake();
    fake.soft_ok = false;
    fake.hardware_ok = false;
    assert(!vw553_module_reset_run(VW553_MODULE_RESET_STARTUP, &OPS, &fake, 5000U, &result));
    EXPECT_EVENTS(fake, 'p', 's', 'h');
    assert(fake.probe_calls == 1U);
    assert(fake.soft_calls == 1U && fake.ready_calls == 0U && fake.recover_calls == 0U);
    assert(fake.hardware_calls == 1U);
}

int main(void)
{
    startup_prefers_soft_reset();
    startup_probe_failure_uses_hardware_fallback();
    soft_reset_failures_fall_back_once();
    recovery_tries_transport_before_reset();
    soft_first_does_not_probe_before_reset();
    invalid_inputs_and_hardware_failure_are_terminal();
    puts("VW553 module reset sequence checks passed");
    return 0;
}
