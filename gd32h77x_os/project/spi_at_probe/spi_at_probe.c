/**
 * @file spi_at_probe.c
 * @brief Small GD32H779V_START console probe for the VW553 SPI AT gateway.
 */
#include "FreeRTOS.h"
#include "stream_buffer.h"
#include "task.h"
#include "gd32h77x_console.h"
#include "at_transport.h"
#include "at_spi_transport.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PROBE_RX_SIZE 2048U
#define PROBE_REPLY_SIZE 768U
#define PROBE_WRITE_TIMEOUT_MS 15000U
#define PROBE_REPLY_TIMEOUT_MS 3000U
#define PROBE_PERIOD_MS 1000U
#define PROBE_MAX_CYCLES 60U

static uint32_t probe_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static at_transport_t *s_transport;
static StreamBufferHandle_t s_rx_stream;

static bool probe_rx_ready(void *context, size_t length)
{
    (void)context;
    return s_rx_stream && xStreamBufferSpacesAvailable(s_rx_stream) >= length;
}

static void probe_rx(void *context, const uint8_t *data, size_t length)
{
    (void)context;
    if (!s_rx_stream || !data || length == 0U) return;
    BaseType_t wake = pdFALSE;
    if (xPortIsInsideInterrupt()) {
        (void)xStreamBufferSendFromISR(s_rx_stream, data, length, &wake);
        portYIELD_FROM_ISR(wake);
    } else {
        (void)xStreamBufferSend(s_rx_stream, data, length, 0U);
    }
}

static bool reply_has_line(const uint8_t *reply, size_t length, const char *line)
{
    const size_t line_length = strlen(line);
    for (size_t i = 0U; i + line_length <= length; ++i) {
        bool at_line_start = i == 0U ||
            (i >= 2U && reply[i - 2U] == '\r' && reply[i - 1U] == '\n');
        bool at_line_end = i + line_length == length ||
            (i + line_length + 1U < length && reply[i + line_length] == '\r' &&
             reply[i + line_length + 1U] == '\n');
        if (at_line_start && at_line_end &&
            memcmp(reply + i, line, line_length) == 0) {
            return true;
        }
    }
    return false;
}

static bool probe_command(uint32_t cycle, const char *command)
{
    uint8_t reply[PROBE_REPLY_SIZE];
    size_t reply_length = 0U;
    size_t sent = 0U;
    int write_status;
    const size_t command_length = strlen(command);

    (void)xStreamBufferReset(s_rx_stream);
    gd32h77x_console_printf("SPI_AT_TX cycle=%lu %s\r\n",
                            (unsigned long)cycle, command);
    write_status = at_transport_write(s_transport, (const uint8_t *)command,
                                      command_length, PROBE_WRITE_TIMEOUT_MS,
                                      &sent);
    if (write_status != AT_TRANSPORT_OK || sent != command_length) {
        gd32h77x_console_printf(
            "SPI_AT_WRITE_FAIL cycle=%lu status=%d sent=%u/%u\r\n",
            (unsigned long)cycle, write_status, (unsigned)sent,
            (unsigned)command_length);
        return false;
    }

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(PROBE_REPLY_TIMEOUT_MS);
    while ((int32_t)(deadline - xTaskGetTickCount()) > 0 &&
           reply_length < sizeof(reply) - 1U) {
        size_t count = xStreamBufferReceive(s_rx_stream, reply + reply_length,
                                            sizeof(reply) - reply_length - 1U,
                                            pdMS_TO_TICKS(100U));
        if (count == 0U) continue;
        reply_length += count;
        reply[reply_length] = '\0';
        if (reply_has_line(reply, reply_length, "OK") ||
            reply_has_line(reply, reply_length, "ERROR") ||
            reply_has_line(reply, reply_length, "FAIL")) {
            gd32h77x_console_printf("SPI_AT_RX cycle=%lu %s\r\n",
                                    (unsigned long)cycle, (const char *)reply);
            return reply_has_line(reply, reply_length, "OK");
        }
    }
    reply[reply_length] = '\0';
    gd32h77x_console_printf("SPI_AT_TIMEOUT cycle=%lu partial=%s\r\n",
                            (unsigned long)cycle, (const char *)reply);
    return false;
}

void app_entry(void *argument)
{
    (void)argument;
    s_transport = at_transport_get();
    s_rx_stream = xStreamBufferCreate(PROBE_RX_SIZE, 1U);
    if (!s_transport || !s_rx_stream) {
        gd32h77x_console_printf("SPI_AT_PROBE_INIT_FAIL\r\n");
        vTaskDelete(NULL);
        return;
    }
    s_transport->rx_callback = probe_rx;
    s_transport->rx_context = NULL;
    s_transport->rx_ready_callback = probe_rx_ready;
    s_transport->rx_ready_context = NULL;
    if (at_transport_open(s_transport) != AT_TRANSPORT_OK) {
        gd32h77x_console_printf("SPI_AT_TRANSPORT_OPEN_FAIL\r\n");
        vTaskDelete(NULL);
        return;
    }

    gd32h77x_console_printf("SPI_AT_PROBE_READY transport=%s\r\n",
                            s_transport->ops->name);
    uint32_t at_passes = 0U;
    uint32_t gmr_passes = 0U;
    uint32_t at_failures = 0U;
    uint32_t gmr_failures = 0U;
    vTaskDelay(pdMS_TO_TICKS(100U));
    for (uint32_t cycle = 1U; cycle <= PROBE_MAX_CYCLES; ++cycle) {
        bool at_ok;
        bool version_ok = false;
        gd32h77x_console_printf("SPI_AT_CYCLE_BEGIN %lu ms=%lu\r\n",
                                (unsigned long)cycle,
                                (unsigned long)probe_now_ms());
        at_ok = probe_command(cycle, "AT\r\n");
        if (at_ok) {
            ++at_passes;
            version_ok = probe_command(cycle, "AT+GMR\r\n");
            if (version_ok) ++gmr_passes;
            else ++gmr_failures;
        } else {
            ++at_failures;
        }
        gd32h77x_console_printf(
            "SPI_AT_CYCLE_RESULT %lu ms=%lu %s at=%u gmr=%u totals=%lu/%lu/%lu/%lu\r\n",
            (unsigned long)cycle, (unsigned long)probe_now_ms(),
            (at_ok && version_ok) ? "PASS" : "FAIL", at_ok ? 1U : 0U,
            version_ok ? 1U : 0U,
            (unsigned long)at_passes, (unsigned long)gmr_passes,
            (unsigned long)at_failures, (unsigned long)gmr_failures);
#if defined(AT_TRANSPORT_SPI) && AT_TRANSPORT_SPI
        if (!at_ok || !version_ok || (cycle % 5U) == 0U) {
            at_spi_transport_diag_t diag;
            at_spi_transport_diag_get(&diag);
            gd32h77x_console_printf(
                "SPI_AT_DIAG cycle=%lu phase=%u pending=%u txnext=%u rxnext=%u "
                "ready_hi=%lu ready_lo=%lu idle_to=%lu tp=%lu rp=%lu et=%lu "
                "invalid=%lu valid=%lu stat=%08lx ctl0=%08lx cfg0=%08lx cfg1=%08lx\r\n",
                (unsigned long)cycle, diag.link_phase, diag.tx_pending,
                diag.tx_next, diag.rx_next,
                (unsigned long)diag.ready_high_timeout,
                (unsigned long)diag.ready_low_timeout,
                (unsigned long)diag.ready_idle_timeout,
                (unsigned long)diag.spi_tp_timeout,
                (unsigned long)diag.spi_rp_timeout,
                (unsigned long)diag.spi_et_timeout,
                (unsigned long)diag.invalid_frame_count,
                (unsigned long)diag.valid_frame_count,
                (unsigned long)diag.spi_stat, (unsigned long)diag.spi_ctl0,
                (unsigned long)diag.spi_cfg0, (unsigned long)diag.spi_cfg1);
            gd32h77x_console_printf("SPI_AT_RXH cycle=%lu ",
                                    (unsigned long)cycle);
            for (size_t i = 0U; i < sizeof(diag.last_rx_header); ++i) {
                gd32h77x_console_printf("%02x", diag.last_rx_header[i]);
            }
            gd32h77x_console_printf("\r\n");
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(PROBE_PERIOD_MS));
    }
    gd32h77x_console_printf(
        "SPI_AT_PROBE_DONE cycles=%u at_ok=%lu gmr_ok=%lu at_fail=%lu gmr_fail=%lu\r\n",
        PROBE_MAX_CYCLES, (unsigned long)at_passes,
        (unsigned long)gmr_passes, (unsigned long)at_failures,
        (unsigned long)gmr_failures);
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000U));
}
