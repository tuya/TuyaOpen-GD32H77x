#include "fakes/wrapper_os.h"

#include "../../../../platform/GD32/gd32_os/MSDK/vw553_at_gateway/gateway_spi_transport.c"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned sema_down_calls;
static uint32_t last_sema_timeout;

uint32_t sys_current_time_get(void) { return 0; }
void sys_ms_sleep(uint32_t ms) { (void)ms; }
int sys_mutex_init(os_mutex_t *m) { (void)m; return 0; }
void sys_mutex_get(os_mutex_t *m) { (void)m; }
void sys_mutex_put(os_mutex_t *m) { (void)m; }
int sys_sema_init(os_sema_t *s, unsigned count) { s->count = count; return 0; }
int32_t sys_sema_down(os_sema_t *s, int32_t timeout)
{
    ++sema_down_calls;
    last_sema_timeout = (uint32_t)timeout;
    /* Match MSDK: timeout 0 means forever and must not be used to poll. */
    assert(timeout != 0);
    if (s->count != 0) {
        --s->count;
        return OS_OK;
    }
    return OS_TIMEOUT;
}
void sys_sema_up(os_sema_t *s) { ++s->count; }
os_task_t sys_task_create_dynamic(const uint8_t *n, uint32_t s, uint32_t p,
                                  void (*f)(void *), void *a)
{ (void)n; (void)s; (void)p; (void)f; (void)a; return (void *)1; }
void sys_task_delete(void *task) { (void)task; }
void sys_enter_critical(void) {}
void sys_exit_critical(void) {}

void rcu_periph_clock_enable(uint32_t x) { (void)x; }
void gpio_af_set(uint32_t a, uint32_t b, uint32_t c) { (void)a; (void)b; (void)c; }
void gpio_mode_set(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{ (void)a; (void)b; (void)c; (void)d; }
void gpio_output_options_set(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{ (void)a; (void)b; (void)c; (void)d; }
void gpio_bit_reset(uint32_t a, uint32_t b) { (void)a; (void)b; }
void gpio_bit_set(uint32_t a, uint32_t b) { (void)a; (void)b; }
FlagStatus gpio_input_bit_get(uint32_t a, uint32_t b) { (void)a; (void)b; return RESET; }
void spi_deinit(void) {}
void spi_struct_para_init(spi_parameter_struct *p) { memset(p, 0, sizeof(*p)); }
void spi_init(spi_parameter_struct *p) { (void)p; }
void spi_nss_internal_low(void) {}
void spi_enable(void) {}
void spi_disable(void) {}
FlagStatus spi_flag_get(uint32_t x) { (void)x; return RESET; }
void spi_rx_flush(void) {}
void spi_crc_error_clear(void) {}
void spi_dma_enable(uint32_t x) { (void)x; }
void spi_dma_disable(uint32_t x) { (void)x; }
void dma_deinit(uint32_t x) { (void)x; }
void dma_single_data_mode_init(uint32_t x, dma_single_data_parameter_struct *p)
{ (void)x; (void)p; }
void dma_single_data_para_struct_init(dma_single_data_parameter_struct *p)
{ memset(p, 0, sizeof(*p)); }
void dma_channel_subperipheral_select(uint32_t a, uint32_t b) { (void)a; (void)b; }
void dma_interrupt_disable(uint32_t a, uint32_t b) { (void)a; (void)b; }
void dma_channel_disable(uint32_t x) { (void)x; }
void dma_flag_clear(uint32_t a, uint32_t b) { (void)a; (void)b; }
void dma_memory_address_config(uint32_t a, uint32_t b, uint32_t c)
{ (void)a; (void)b; (void)c; }
void dma_transfer_number_config(uint32_t a, uint32_t b) { (void)a; (void)b; }
void dma_memory_address_generation_config(uint32_t a, uint32_t b) { (void)a; (void)b; }
void dma_channel_enable(uint32_t x) { (void)x; }
FlagStatus dma_flag_get(uint32_t a, uint32_t b) { (void)a; (void)b; return RESET; }

int main(void)
{
    uint8_t target[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const uint8_t binary[] = {0, 'A', 0, 0x7E, 0xFF};
    unsigned before;

    /* Transport preflight reserves room for an entire frame before sequence
     * state is advanced, so a short target cannot consume a partial payload. */
    s_rx_target = target;
    s_rx_target_len = 4;
    s_rx_target_received = 0;
    assert(transport_can_accept(5) == 0);
    assert(transport_can_accept(4) == 1);
    s_rx_target = NULL;

    /* The first arm must only use bounded stale-token draining. */
    before = sema_down_calls;
    assert(gateway_spi_transport_arm_rx(target, sizeof(target)) == 0);
    assert(sema_down_calls == before + 1 && last_sema_timeout == 1);
    transport_deliver(binary, sizeof(binary));
    assert(gateway_spi_transport_wait_rx(20) == 0);
    assert(memcmp(target, binary, sizeof(binary)) == 0);

    /* A completion left by an earlier receive is discarded before re-arm. */
    s_rx_done.count = 1;
    assert(gateway_spi_transport_arm_rx(target, sizeof(target)) == 0);
    assert(s_rx_done.count == 0);
    assert(gateway_spi_transport_wait_rx(10) == -1);
    assert(s_rx_target == NULL && s_reset_requested != 0);

    /* Timeout clears the target; a following request can be armed. */
    s_reset_requested = 0;
    assert(gateway_spi_transport_arm_rx(target, sizeof(target)) == 0);
    assert(gateway_spi_transport_wait_rx(10) == -1);
    assert(s_rx_target == NULL);

    /* Peer/session reset wakes a waiter and cancels the armed buffer. */
    s_reset_requested = 0;
    assert(gateway_spi_transport_arm_rx(target, sizeof(target)) == 0);
    transport_reset_session();
    assert(gateway_spi_transport_wait_rx(20) == -1);
    assert(s_rx_target == NULL && s_rx_result == -1);

    puts("VW553 transport RX arm/wait checks passed");
    return 0;
}
