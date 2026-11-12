/**
 * @file at_spi_transport.c
 * @brief SPI master transport for GD32H779V_START <-> GD32VW553.
 */
#include "at_spi_transport.h"

#include "at_spi_protocol.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "gd32h77x_78x.h"
#include "gd32h77x_78x_gpio.h"
#include "gd32h77x_78x_rcu.h"
#include "gd32h77x_78x_spi.h"
#include "gd32h77x_config.h"

#include <string.h>

#define AT_SPI_PERIPH SPI3
#define AT_SPI_REQUEST_PORT GPIOE
#define AT_SPI_REQUEST_PIN GPIO_PIN_4
#define AT_SPI_READY_PORT GPIOE
#define AT_SPI_READY_PIN GPIO_PIN_3
#define AT_SPI_CLOCK_PIN GPIO_PIN_2
#define AT_SPI_MISO_PIN GPIO_PIN_5
#define AT_SPI_MOSI_PIN GPIO_PIN_6
#define AT_SPI_PIN_MASK (AT_SPI_CLOCK_PIN | AT_SPI_MISO_PIN | AT_SPI_MOSI_PIN)
#define AT_SPI_READY_TIMEOUT_MS 100U
#define AT_SPI_READY_IDLE_TIMEOUT_MS 1200U
#define AT_SPI_FRAME_TIMEOUT_MS 100U
#define AT_SPI_WRITE_QUEUE_LIMIT 16384U
#define AT_SPI_WORKER_STACK 1024U
#define AT_SPI_WORKER_PRIORITY (tskIDLE_PRIORITY + 3U)
#define AT_SPI_POLL_DELAY_MS 2U
#define AT_SPI_BAD_FRAME_LIMIT 4U

typedef struct {
    bool opened;
    volatile bool suspended;
    volatile bool abort_write;
    volatile bool write_active;
    volatile bool write_done;
    volatile bool write_success;
    bool stop_requested;
    volatile bool worker_idle;
    bool transaction_started;
    const uint8_t *write_data;
    size_t write_length;
    volatile size_t write_offset;
    uint16_t staged_length;
    uint8_t tx_frame[AT_SPI_FRAME_SIZE];
    uint8_t rx_frame[AT_SPI_FRAME_SIZE];
    uint8_t rx_payload[AT_SPI_PAYLOAD_SIZE];
    at_spi_link_state_t link;
    SemaphoreHandle_t write_mutex;
    SemaphoreHandle_t write_done_sem;
    SemaphoreHandle_t worker_exit_sem;
    SemaphoreHandle_t worker_idle_sem;
    TaskHandle_t worker;
    uint32_t rx_burst_high_water;
    uint8_t bad_frame_count;
    at_spi_transport_diag_t diag;
} at_spi_context_t;

static at_spi_context_t s_spi;
static at_transport_t s_transport;
static void finish_write(bool success);
static void spi_suspend(at_transport_t *transport);

static uint32_t spi_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static bool wait_pin(uint32_t port, uint32_t pin, FlagStatus value,
                     uint32_t timeout_ms)
{
    const uint32_t start = spi_now_ms();
    while (gpio_input_bit_get(port, pin) != value) {
        if (s_spi.suspended || s_spi.stop_requested) return false;
        if ((uint32_t)(spi_now_ms() - start) >= timeout_ms) return false;
        vTaskDelay(1U);
    }
    return true;
}

static bool wait_spi_flag(uint32_t flag, uint32_t start_ms)
{
    while ((SPI_STAT(AT_SPI_PERIPH) & flag) == RESET) {
        if (s_spi.suspended || s_spi.stop_requested) return false;
        if ((SPI_STAT(AT_SPI_PERIPH) & (SPI_FLAG_CONFERR | SPI_FLAG_RXORERR |
                         SPI_FLAG_TXURERR)) != RESET) {
            return false;
        }
        if ((uint32_t)(spi_now_ms() - start_ms) >= AT_SPI_FRAME_TIMEOUT_MS) {
            if (!s_spi.suspended && !s_spi.stop_requested) {
                if (flag == SPI_FLAG_TP) s_spi.diag.spi_tp_timeout++;
                else if (flag == SPI_FLAG_RP) s_spi.diag.spi_rp_timeout++;
                else if (flag == SPI_FLAG_ET) s_spi.diag.spi_et_timeout++;
            }
            return false;
        }
    }
    return true;
}

static bool spi_master_suspend(void)
{
    const uint32_t start = spi_now_ms();
    if ((SPI_CTL0(AT_SPI_PERIPH) & SPI_CTL0_MSTART) == RESET) {
        SPI_STATC(AT_SPI_PERIPH) = SPI_STATC_SPDC;
        return true;
    }
    SPI_CTL0(AT_SPI_PERIPH) |= SPI_CTL0_MSPDR;
    while ((SPI_CTL0(AT_SPI_PERIPH) & SPI_CTL0_MSTART) != RESET) {
        if (s_spi.suspended || s_spi.stop_requested) return false;
        if ((uint32_t)(spi_now_ms() - start) >= AT_SPI_READY_TIMEOUT_MS) return false;
    }
    SPI_STATC(AT_SPI_PERIPH) = SPI_STATC_SPDC;
    return true;
}

static bool spi_transfer_frame(const uint8_t tx[AT_SPI_FRAME_SIZE],
                               uint8_t rx[AT_SPI_FRAME_SIZE])
{
    if (!wait_pin(AT_SPI_READY_PORT, AT_SPI_READY_PIN, SET,
                  AT_SPI_READY_TIMEOUT_MS)) {
        if (!s_spi.suspended && !s_spi.stop_requested) {
            s_spi.diag.ready_high_timeout++;
        }
        return false;
    }
    if (!spi_master_suspend()) return false;

    SPI_CTL0(AT_SPI_PERIPH) &= ~SPI_CTL0_SPIEN;
    SPI_STATC(AT_SPI_PERIPH) = SPI_STATC_ETC | SPI_STATC_SPDC |
                               SPI_STATC_TXURERRC | SPI_STATC_RXORERRC |
                               SPI_STATC_CONFERRC;
    spi_current_data_num_config(AT_SPI_PERIPH, AT_SPI_FRAME_SIZE);
    SPI_CTL0(AT_SPI_PERIPH) |= SPI_CTL0_SPIEN;
    spi_master_transfer_start(AT_SPI_PERIPH, SPI_TRANS_START);
    s_spi.transaction_started = true;

    const uint32_t frame_start = spi_now_ms();
    for (size_t index = 0U; index < AT_SPI_FRAME_SIZE; ++index) {
        if (!wait_spi_flag(SPI_FLAG_TP, frame_start)) return false;
        SPI_TDATA(AT_SPI_PERIPH) = tx[index];
        if (!wait_spi_flag(SPI_FLAG_RP, frame_start)) return false;
        rx[index] = (uint8_t)SPI_RDATA(AT_SPI_PERIPH);
    }
    if (!wait_spi_flag(SPI_FLAG_ET, frame_start)) return false;
    if (!spi_master_suspend()) return false;
    s_spi.transaction_started = false;
    return true;
}

static void spi_gpio_and_peripheral_init(void)
{
    spi_parameter_struct config;

    rcu_periph_clock_enable(RCU_GPIOE);
    rcu_spi_clock_config(IDX_SPI3, RCU_SPISRC_APB2);
    rcu_periph_clock_enable(RCU_SPI3);
    gpio_af_set(GPIOE, GPIO_AF_5, AT_SPI_PIN_MASK);
    gpio_mode_set(GPIOE, GPIO_MODE_AF, GPIO_PUPD_NONE, AT_SPI_PIN_MASK);
    gpio_output_options_set(GPIOE, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, AT_SPI_PIN_MASK);

    gpio_mode_set(AT_SPI_REQUEST_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE,
                  AT_SPI_REQUEST_PIN);
    gpio_output_options_set(AT_SPI_REQUEST_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ,
                            AT_SPI_REQUEST_PIN);
    gpio_mode_set(AT_SPI_READY_PORT, GPIO_MODE_INPUT, GPIO_PUPD_PULLDOWN,
                  AT_SPI_READY_PIN);
    gpio_bit_reset(AT_SPI_REQUEST_PORT, AT_SPI_REQUEST_PIN);

    spi_disable(AT_SPI_PERIPH);
    rcu_periph_reset_enable(RCU_SPI3RST);
    rcu_periph_reset_disable(RCU_SPI3RST);
    spi_struct_para_init(&config);
    config.device_mode = SPI_MASTER;
    config.trans_mode = SPI_TRANSMODE_FULLDUPLEX;
    config.data_size = SPI_DATASIZE_8BIT;
    config.nss = SPI_NSS_SOFT;
    config.endian = SPI_ENDIAN_MSB;
    config.clock_polarity_phase = SPI_CK_PL_LOW_PH_1EDGE;
#if ENABLE_GD32H77X_AT_SPI_PSC_64
    config.prescale = SPI_PSC_64;
#elif ENABLE_GD32H77X_AT_SPI_PSC_128
    config.prescale = SPI_PSC_128;
#else
    config.prescale = SPI_PSC_256;
#endif
    spi_init(AT_SPI_PERIPH, &config);
    spi_nss_internal_high(AT_SPI_PERIPH);
    spi_byte_access_enable(AT_SPI_PERIPH);
    spi_enable(AT_SPI_PERIPH);
}

static void reset_link_and_notify(void)
{
    finish_write(false);
    s_spi.staged_length = 0U;
    at_spi_link_init(&s_spi.link);
    s_spi.bad_frame_count = 0U;
    if (s_transport.fault_callback) {
        s_transport.fault_callback(s_transport.fault_context,
                                   AT_TRANSPORT_FAULT_FRAMING);
    }
}

static void notify_link_fault(void)
{
    if (s_transport.fault_callback) {
        s_transport.fault_callback(s_transport.fault_context,
                                   AT_TRANSPORT_FAULT_FRAMING);
    }
}

static void finish_write(bool success)
{
    bool notify = false;
    taskENTER_CRITICAL();
    if (s_spi.write_active) {
        s_spi.write_success = success;
        s_spi.write_done = true;
        s_spi.write_active = false;
        s_spi.write_data = NULL;
        s_spi.write_length = 0U;
        s_spi.staged_length = 0U;
        notify = true;
    }
    taskEXIT_CRITICAL();
    if (notify && s_spi.write_done_sem) xSemaphoreGive(s_spi.write_done_sem);
}

static bool stage_write_chunk(void)
{
    size_t remaining;
    uint16_t chunk;
    bool staged;

    taskENTER_CRITICAL();
    if (!s_spi.write_active || s_spi.link.tx_pending || s_spi.abort_write ||
        s_spi.write_offset >= s_spi.write_length) {
        taskEXIT_CRITICAL();
        return false;
    }
    remaining = s_spi.write_length - s_spi.write_offset;
    chunk = (uint16_t)(remaining > AT_SPI_PAYLOAD_SIZE ? AT_SPI_PAYLOAD_SIZE : remaining);
    staged = at_spi_link_stage_tx(&s_spi.link,
                                  &s_spi.write_data[s_spi.write_offset], chunk);
    if (staged) s_spi.staged_length = chunk;
    taskEXIT_CRITICAL();
    return staged;
}

static bool rx_backpressure(const at_spi_frame_t *frame)
{
    if (frame->flags != 0U || frame->length == 0U ||
        frame->seq != s_spi.link.rx_next) {
        return false;
    }
    return s_transport.rx_ready_callback &&
           !s_transport.rx_ready_callback(s_transport.rx_ready_context,
                                          frame->length);
}

static void process_received_frame(void)
{
    at_spi_frame_t frame;
    at_spi_link_rx_result_t result;
    const uint8_t *rx_data = NULL;
    uint16_t rx_length = 0U;

    memcpy(s_spi.diag.last_rx_header, s_spi.rx_frame, sizeof(s_spi.diag.last_rx_header));
    if (!at_spi_frame_decode(s_spi.rx_frame, &frame, s_spi.rx_payload)) {
        s_spi.diag.invalid_frame_count++;
        if (++s_spi.bad_frame_count >= AT_SPI_BAD_FRAME_LIMIT) {
            s_spi.bad_frame_count = 0U;
            if (s_spi.link.phase == AT_SPI_LINK_ESTABLISHED) {
                reset_link_and_notify();
            }
        }
        return;
    }
    s_spi.diag.valid_frame_count++;
    s_spi.bad_frame_count = 0U;
    bool accept_payload = !rx_backpressure(&frame);

    const bool had_inflight = s_spi.link.tx_pending;
    result = at_spi_link_process_frame(&s_spi.link, &frame, accept_payload,
                                       &rx_data, &rx_length);
    if (result == AT_SPI_LINK_RX_RESET) {
        finish_write(false);
        s_spi.staged_length = 0U;
        notify_link_fault();
        return;
    }
    if (result == AT_SPI_LINK_RX_ACCEPTED && s_transport.rx_callback) {
        s_transport.rx_callback(s_transport.rx_context, rx_data, rx_length);
        if (rx_length > s_spi.rx_burst_high_water) s_spi.rx_burst_high_water = rx_length;
    }
    if (had_inflight && !s_spi.link.tx_pending && s_spi.write_active) {
        taskENTER_CRITICAL();
        s_spi.write_offset += s_spi.staged_length;
        s_spi.staged_length = 0U;
        bool complete = s_spi.write_offset >= s_spi.write_length;
        taskEXIT_CRITICAL();
        if (complete) finish_write(true);
    }
}

static void spi_worker(void *argument)
{
    (void)argument;
    for (;;) {
        if (s_spi.stop_requested) {
            finish_write(false);
            xSemaphoreGive(s_spi.worker_exit_sem);
            vTaskDelete(NULL);
        }
        if (s_spi.suspended) {
            s_spi.worker_idle = true;
            xSemaphoreGive(s_spi.worker_idle_sem);
            vTaskDelay(pdMS_TO_TICKS(AT_SPI_POLL_DELAY_MS));
            continue;
        }
        s_spi.worker_idle = false;
        if (s_spi.abort_write) {
            finish_write(false);
            reset_link_and_notify();
            s_spi.abort_write = false;
        }
        /* A reset during a slave DMA frame can leave READY asserted until
         * its DMA timeout drains the partial transfer. Keep REQUEST low and
         * wait for that old transaction to finish before issuing a new one. */
        if (!wait_pin(AT_SPI_READY_PORT, AT_SPI_READY_PIN, RESET,
                      AT_SPI_READY_IDLE_TIMEOUT_MS)) {
            if (!s_spi.suspended && !s_spi.stop_requested) {
                s_spi.diag.ready_idle_timeout++;
                if (s_spi.link.phase == AT_SPI_LINK_ESTABLISHED) {
                    finish_write(false);
                    reset_link_and_notify();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(AT_SPI_POLL_DELAY_MS));
            continue;
        }
        (void)stage_write_chunk();
        at_spi_link_build_frame(&s_spi.link, s_spi.tx_frame);
        gpio_bit_set(AT_SPI_REQUEST_PORT, AT_SPI_REQUEST_PIN);
        bool transferred = spi_transfer_frame(s_spi.tx_frame, s_spi.rx_frame);
        gpio_bit_reset(AT_SPI_REQUEST_PORT, AT_SPI_REQUEST_PIN);
        if (!wait_pin(AT_SPI_READY_PORT, AT_SPI_READY_PIN, RESET,
                      AT_SPI_READY_TIMEOUT_MS)) {
            if (!s_spi.suspended && !s_spi.stop_requested) {
                s_spi.diag.ready_low_timeout++;
            }
            transferred = false;
        }
        if (transferred) {
            process_received_frame();
        } else {
            /* A write can be queued while the peer is still booting. During
             * START/HANDSHAKE_ACK only HELLO control frames are on the wire;
             * a READY timeout must leave that queued write pending until its
             * caller's deadline. Once established, a transfer failure may
             * have interrupted an actual payload and must fail the write. */
            bool notify = s_spi.link.phase == AT_SPI_LINK_ESTABLISHED;
            if (notify && !s_spi.suspended && !s_spi.stop_requested) {
                finish_write(false);
                reset_link_and_notify();
            }
            if (s_spi.transaction_started) {
                spi_gpio_and_peripheral_init();
                s_spi.transaction_started = false;
            }
            vTaskDelay(pdMS_TO_TICKS(AT_SPI_POLL_DELAY_MS));
        }
        vTaskDelay(pdMS_TO_TICKS(AT_SPI_POLL_DELAY_MS));
    }
}

static int spi_open(at_transport_t *transport)
{
    (void)transport;
    if (s_spi.opened) return AT_TRANSPORT_OK;
    memset(&s_spi, 0, sizeof(s_spi));
    at_spi_link_init(&s_spi.link);
    s_spi.write_mutex = xSemaphoreCreateMutex();
    s_spi.write_done_sem = xSemaphoreCreateBinary();
    s_spi.worker_exit_sem = xSemaphoreCreateBinary();
    s_spi.worker_idle_sem = xSemaphoreCreateBinary();
    if (s_spi.write_mutex == NULL || s_spi.write_done_sem == NULL ||
        s_spi.worker_exit_sem == NULL || s_spi.worker_idle_sem == NULL) {
        if (s_spi.write_mutex) vSemaphoreDelete(s_spi.write_mutex);
        if (s_spi.write_done_sem) vSemaphoreDelete(s_spi.write_done_sem);
        if (s_spi.worker_exit_sem) vSemaphoreDelete(s_spi.worker_exit_sem);
        if (s_spi.worker_idle_sem) vSemaphoreDelete(s_spi.worker_idle_sem);
        memset(&s_spi, 0, sizeof(s_spi));
        return AT_TRANSPORT_ERROR;
    }
    spi_gpio_and_peripheral_init();
    s_spi.opened = true;
    if (xTaskCreate(spi_worker, "at_spi", AT_SPI_WORKER_STACK, NULL,
                    AT_SPI_WORKER_PRIORITY, &s_spi.worker) != pdPASS) {
        s_spi.opened = false;
        spi_disable(AT_SPI_PERIPH);
        vSemaphoreDelete(s_spi.write_mutex);
        vSemaphoreDelete(s_spi.write_done_sem);
        vSemaphoreDelete(s_spi.worker_exit_sem);
        vSemaphoreDelete(s_spi.worker_idle_sem);
        memset(&s_spi, 0, sizeof(s_spi));
        return AT_TRANSPORT_ERROR;
    }
    return AT_TRANSPORT_OK;
}

static void spi_close(at_transport_t *transport)
{
    (void)transport;
    if (!s_spi.opened) return;
    spi_suspend(transport);
    s_spi.stop_requested = true;
    if (s_spi.worker_exit_sem) {
        (void)xSemaphoreTake(s_spi.worker_exit_sem, portMAX_DELAY);
    }
    if (s_spi.write_mutex) {
        (void)xSemaphoreTake(s_spi.write_mutex, portMAX_DELAY);
        xSemaphoreGive(s_spi.write_mutex);
    }
    s_spi.opened = false;
    gpio_bit_reset(AT_SPI_REQUEST_PORT, AT_SPI_REQUEST_PIN);
    spi_disable(AT_SPI_PERIPH);
    if (s_spi.write_mutex) vSemaphoreDelete(s_spi.write_mutex);
    if (s_spi.write_done_sem) vSemaphoreDelete(s_spi.write_done_sem);
    if (s_spi.worker_exit_sem) vSemaphoreDelete(s_spi.worker_exit_sem);
    if (s_spi.worker_idle_sem) vSemaphoreDelete(s_spi.worker_idle_sem);
    memset(&s_spi, 0, sizeof(s_spi));
}

static int spi_write(at_transport_t *transport, const uint8_t *data, size_t length,
                     uint32_t timeout_ms, size_t *sent)
{
    (void)transport;
    if (sent) *sent = 0U;
    if (!s_spi.opened || (!data && length) || length > AT_SPI_WRITE_QUEUE_LIMIT) {
        return AT_TRANSPORT_ERROR;
    }
    if (length == 0U) return AT_TRANSPORT_OK;
    uint32_t wait_start = spi_now_ms();
    if (xSemaphoreTake(s_spi.write_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return AT_TRANSPORT_TIMEOUT;
    }
    while (s_spi.abort_write) {
        if ((uint32_t)(spi_now_ms() - wait_start) >= timeout_ms) {
            xSemaphoreGive(s_spi.write_mutex);
            return AT_TRANSPORT_TIMEOUT;
        }
        vTaskDelay(1U);
    }
    while (xSemaphoreTake(s_spi.write_done_sem, 0U) == pdTRUE) {
    }
    taskENTER_CRITICAL();
    s_spi.write_data = data;
    s_spi.write_length = length;
    s_spi.write_offset = 0U;
    s_spi.staged_length = 0U;
    s_spi.write_active = true;
    s_spi.write_done = false;
    s_spi.write_success = false;
    taskEXIT_CRITICAL();

    uint32_t elapsed = (uint32_t)(spi_now_ms() - wait_start);
    uint32_t remain_ms = elapsed >= timeout_ms ? 0U : timeout_ms - elapsed;
    BaseType_t completed = xSemaphoreTake(s_spi.write_done_sem, pdMS_TO_TICKS(remain_ms));
    taskENTER_CRITICAL();
    bool is_done = s_spi.write_done;
    bool success = s_spi.write_success;
    size_t acknowledged = s_spi.write_offset;
    if (!is_done && completed != pdTRUE) {
        s_spi.write_active = false;
        s_spi.write_data = NULL;
        s_spi.abort_write = true;
    }
    taskEXIT_CRITICAL();
    if (is_done || completed == pdTRUE) {
        if (sent) *sent = acknowledged;
        int result = success ? AT_TRANSPORT_OK : AT_TRANSPORT_ERROR;
        xSemaphoreGive(s_spi.write_mutex);
        return result;
    }
    if (sent) *sent = acknowledged;
    xSemaphoreGive(s_spi.write_mutex);
    return AT_TRANSPORT_TIMEOUT;
}

static void spi_suspend(at_transport_t *transport)
{
    (void)transport;
    s_spi.suspended = true;
    while (s_spi.opened && !s_spi.worker_idle && s_spi.worker_idle_sem) {
        (void)xSemaphoreTake(s_spi.worker_idle_sem, portMAX_DELAY);
    }
}

static int spi_resume(at_transport_t *transport)
{
    (void)transport;
    while (s_spi.worker_idle_sem &&
           xSemaphoreTake(s_spi.worker_idle_sem, 0U) == pdTRUE) {
    }
    s_spi.worker_idle = false;
    s_spi.suspended = false;
    return AT_TRANSPORT_OK;
}

static void spi_flush(at_transport_t *transport)
{
    (void)transport;
    spi_suspend(transport);
    finish_write(false);
    s_spi.abort_write = false;
    s_spi.staged_length = 0U;
    at_spi_link_init(&s_spi.link);
    s_spi.bad_frame_count = 0U;
}

static uint32_t spi_rx_burst_high_water(const at_transport_t *transport)
{
    (void)transport;
    return s_spi.rx_burst_high_water;
}

static const at_transport_ops_t s_spi_ops = {
    .name = "spi",
    .open = spi_open,
    .close = spi_close,
    .write = spi_write,
    .suspend = spi_suspend,
    .resume = spi_resume,
    .flush = spi_flush,
    .rx_burst_high_water = spi_rx_burst_high_water,
};

at_transport_t *at_spi_transport_get(void)
{
    if (s_transport.ops == NULL) {
        s_transport.ops = &s_spi_ops;
        s_transport.context = &s_spi;
    }
    return &s_transport;
}

void at_spi_transport_diag_get(at_spi_transport_diag_t *diag)
{
    if (diag == NULL) return;
    taskENTER_CRITICAL();
    *diag = s_spi.diag;
    diag->link_phase = (uint8_t)s_spi.link.phase;
    diag->tx_pending = s_spi.link.tx_pending ? 1U : 0U;
    diag->tx_next = s_spi.link.tx_next;
    diag->rx_next = s_spi.link.rx_next;
    diag->gpio_istat = GPIO_ISTAT(GPIOE);
    diag->gpio_octl = GPIO_OCTL(GPIOE);
    diag->spi_stat = SPI_STAT(AT_SPI_PERIPH);
    diag->spi_ctl0 = SPI_CTL0(AT_SPI_PERIPH);
    diag->spi_cfg0 = SPI_CFG0(AT_SPI_PERIPH);
    diag->spi_cfg1 = SPI_CFG1(AT_SPI_PERIPH);
    taskEXIT_CRITICAL();
}
