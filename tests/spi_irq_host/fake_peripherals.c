#include <stdint.h>
#include <string.h>

#include "gd32h77x_78x.h"
#include "gd32h77x_78x_spi.h"
#include "tkl_pinmux.h"
#include "tkl_system.h"

#define FAKE_SPI_COUNT 6U
#define FAKE_RX_QUEUE_CAPACITY 4096U

uint32_t fake_stat[FAKE_SPI_COUNT];
uint32_t fake_ctl0[FAKE_SPI_COUNT];
uint32_t fake_statc[FAKE_SPI_COUNT];
uint32_t fake_tdata[FAKE_SPI_COUNT];
uint32_t fake_rdata[FAKE_SPI_COUNT];
uint32_t fake_int[FAKE_SPI_COUNT];
uint32_t fake_spi_frames[FAKE_SPI_COUNT];
unsigned int fake_spi_disable_count[FAKE_SPI_COUNT];
unsigned int fake_spi_statc_write_count[FAKE_SPI_COUNT];
unsigned int fake_spi_suspend_probe_count[FAKE_SPI_COUNT];

unsigned fake_nvic_enabled[FAKE_SPI_COUNT];
unsigned fake_nvic_pending[FAKE_SPI_COUNT];

unsigned int fake_critical_depth;
unsigned int fake_critical_entries;
unsigned int fake_critical_exits;
SYS_TIME_T fake_time_ms;

static uint32_t fake_tx_pending[FAKE_SPI_COUNT];
static uint32_t fake_tx_remaining[FAKE_SPI_COUNT];
static uint32_t fake_rx_head[FAKE_SPI_COUNT];
static uint32_t fake_rx_tail[FAKE_SPI_COUNT];
static uint32_t fake_rx_count[FAKE_SPI_COUNT];
static uint32_t fake_rx_queue[FAKE_SPI_COUNT][FAKE_RX_QUEUE_CAPACITY];

static void fake_spi_commit_pending_tx(uint32_t periph)
{
    uint32_t index;

    if (fake_tx_pending[periph] == 0U) {
        return;
    }

    if (fake_rx_count[periph] < FAKE_RX_QUEUE_CAPACITY) {
        index = fake_rx_tail[periph];
        fake_rx_queue[periph][index] = fake_tdata[periph];
        fake_rx_tail[periph] = (index + 1U) % FAKE_RX_QUEUE_CAPACITY;
        fake_rx_count[periph]++;
        fake_stat[periph] |= SPI_FLAG_RP;
    }
    fake_tx_pending[periph] = 0U;
}

uint32_t *fake_spi_ctl0_reg(uint32_t periph)
{
    if ((fake_ctl0[periph] & SPI_CTL0_MSPDR) != 0U) {
        fake_spi_suspend_probe_count[periph]++;
    }
    /* Model a transaction completing as soon as the master suspend path
     * observes it, so suspend never spins forever in this host fake. */
    fake_ctl0[periph] &= ~SPI_CTL0_MSTART;
    return &fake_ctl0[periph];
}

uint32_t *fake_spi_statc_reg(uint32_t periph)
{
    fake_spi_statc_write_count[periph]++;
    /* STATUS clear registers are write-one-to-clear in the real peripheral.
     * Clearing ET/error status before the write gives the same observable
     * folded result for the driver's clear-only uses. */
    fake_stat[periph] &= ~(SPI_FLAG_ET | SPI_FLAG_ERRORS);
    return &fake_statc[periph];
}

uint32_t *fake_spi_tdata_reg(uint32_t periph)
{
    /* This hook runs when the driver evaluates the TDATA lvalue. It commits
     * the previous frame, drains one slot from the fake TX FIFO, and updates
     * TP/ET before the actual register store happens. */
    fake_spi_commit_pending_tx(periph);
    fake_tx_pending[periph] = 1U;

    if (fake_tx_remaining[periph] > 0U) {
        fake_tx_remaining[periph]--;
    }
    if (fake_tx_remaining[periph] == 0U) {
        fake_stat[periph] &= ~SPI_FLAG_TP;
        fake_stat[periph] |= SPI_FLAG_ET;
    }

    return &fake_tdata[periph];
}

uint32_t fake_spi_rdata_read(uint32_t periph)
{
    uint32_t index;
    uint32_t value;

    /* Commit the last frame written by the TX loop before draining RX. */
    fake_spi_commit_pending_tx(periph);
    if (fake_rx_count[periph] == 0U) {
        fake_stat[periph] &= ~SPI_FLAG_RP;
        return fake_rdata[periph];
    }

    index = fake_rx_head[periph];
    value = fake_rx_queue[periph][index];
    fake_rx_head[periph] = (index + 1U) % FAKE_RX_QUEUE_CAPACITY;
    fake_rx_count[periph]--;
    if (fake_rx_count[periph] == 0U) {
        fake_stat[periph] &= ~SPI_FLAG_RP;
    }
    return value;
}

void fake_spi_reset(uint32_t periph)
{
    fake_stat[periph] = 0U;
    fake_ctl0[periph] = 0U;
    fake_statc[periph] = 0U;
    fake_tdata[periph] = 0U;
    fake_rdata[periph] = 0xFFU;
    fake_int[periph] = 0U;
    fake_spi_frames[periph] = 0U;
    fake_spi_disable_count[periph] = 0U;
    fake_spi_statc_write_count[periph] = 0U;
    fake_spi_suspend_probe_count[periph] = 0U;

    fake_nvic_enabled[periph] = 0U;
    fake_nvic_pending[periph] = 0U;

    fake_tx_pending[periph] = 0U;
    fake_tx_remaining[periph] = 0U;
    fake_rx_head[periph] = 0U;
    fake_rx_tail[periph] = 0U;
    fake_rx_count[periph] = 0U;
    memset(fake_rx_queue[periph], 0, sizeof(fake_rx_queue[periph]));
}

void spi_disable(uint32_t periph)
{
    fake_spi_disable_count[periph]++;
    fake_ctl0[periph] &= ~SPI_CTL0_MSTART;
}

void spi_enable(uint32_t periph)
{
    (void)periph;
}

void spi_struct_para_init(spi_parameter_struct *cfg)
{
    if (cfg != NULL) {
        memset(cfg, 0, sizeof(*cfg));
    }
}

void spi_byte_access_enable(uint32_t periph)
{
    (void)periph;
}

void spi_nss_output_control(uint32_t periph, uint32_t control)
{
    (void)periph;
    (void)control;
}

void spi_nss_polarity_set(uint32_t periph, uint32_t polarity)
{
    (void)periph;
    (void)polarity;
}

void spi_nss_output_enable(uint32_t periph)
{
    (void)periph;
}

void spi_nss_output_disable(uint32_t periph)
{
    (void)periph;
}

void spi_nss_internal_high(uint32_t periph)
{
    (void)periph;
}

void spi_init(uint32_t periph, const spi_parameter_struct *cfg)
{
    (void)cfg;
    fake_ctl0[periph] = 0U;
}

void spi_current_data_num_config(uint32_t periph, uint32_t frames)
{
    fake_spi_frames[periph] = frames;
}

void spi_master_transfer_start(uint32_t periph, uint32_t start)
{
    (void)start;
    fake_ctl0[periph] |= SPI_CTL0_MSTART;

    fake_stat[periph] &= ~(SPI_FLAG_RP | SPI_FLAG_TP | SPI_FLAG_ET |
                           SPI_FLAG_ERRORS);
    fake_tx_pending[periph] = 0U;
    fake_tx_remaining[periph] = fake_spi_frames[periph];
    fake_rx_head[periph] = 0U;
    fake_rx_tail[periph] = 0U;
    fake_rx_count[periph] = 0U;
    if (fake_tx_remaining[periph] != 0U) {
        fake_stat[periph] |= SPI_FLAG_TP;
    }
}

void spi_i2s_interrupt_disable(uint32_t periph, uint32_t interrupts)
{
    fake_int[periph] &= ~interrupts;
}

void spi_i2s_interrupt_enable(uint32_t periph, uint32_t interrupts)
{
    fake_int[periph] |= interrupts;
}

void rcu_spi_clock_config(uint32_t spi_idx, uint32_t clock_source)
{
    (void)spi_idx;
    (void)clock_source;
}

void rcu_periph_clock_enable(uint32_t rcu)
{
    (void)rcu;
}

uint32_t rcu_clock_freq_get(uint32_t clock)
{
    (void)clock;
    return 240000000U;
}

uint32_t tkl_system_enter_critical(void)
{
    fake_critical_entries++;
    fake_critical_depth++;
    return (uint32_t)fake_critical_depth;
}

void tkl_system_exit_critical(uint32_t irq_mask)
{
    (void)irq_mask;
    fake_critical_exits++;
    if (fake_critical_depth != 0U) {
        fake_critical_depth--;
    }
}

SYS_TIME_T tkl_system_get_millisecond(void)
{
    fake_time_ms++;
    return fake_time_ms;
}

OPERATE_RET tkl_multi_io_pinmux_config(TUYA_MUL_PIN_CFG_T *cfg, uint16_t num)
{
    (void)cfg;
    (void)num;
    return OPRT_OK;
}

OPERATE_RET tkl_io_pinmux_config(TUYA_PIN_NAME_E pin, TUYA_PIN_FUNC_E pin_func)
{
    (void)pin;
    (void)pin_func;
    return OPRT_OK;
}

void gpio_af_set(uint32_t port, uint32_t af, uint32_t pin)
{
    (void)port;
    (void)af;
    (void)pin;
}

void gpio_mode_set(uint32_t port, uint32_t mode, uint32_t pupd, uint32_t pin)
{
    (void)port;
    (void)mode;
    (void)pupd;
    (void)pin;
}

void gpio_output_options_set(uint32_t port, uint32_t type, uint32_t speed,
                             uint32_t pin)
{
    (void)port;
    (void)type;
    (void)speed;
    (void)pin;
}
