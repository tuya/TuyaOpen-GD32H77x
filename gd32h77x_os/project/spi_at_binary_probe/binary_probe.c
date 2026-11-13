/* Length-delimited binary diagnostic; never searches binary bodies as text. */
#include "FreeRTOS.h"
#include "task.h"
#include "stream_buffer.h"
#include "at_transport.h"
#include "at_spi_transport.h"
#include "gd32h77x_console.h"
#include "gd32h77x_78x.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static at_transport_t *link;
static StreamBufferHandle_t rx;
static uint8_t outgoing[4096], incoming[4096];
static volatile bool reset_done;
static volatile uint32_t faults;
static void fault(void *arg, uint32_t mask) { (void)arg; if (mask) ++faults; }
static uint32_t crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xffffffffU;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}
static uint32_t now(void) { return xTaskGetTickCount() * portTICK_PERIOD_MS; }
static bool ready(void *arg, size_t n) { (void)arg; return xStreamBufferSpacesAvailable(rx) >= n; }
static void receive(void *arg, const uint8_t *data, size_t n)
{
    (void)arg;
    if (xStreamBufferSend(rx, data, n, 0) != n)
        gd32h77x_console_printf("BINARY_RX_OVERFLOW\r\n");
}
static bool read_exact(void *buffer, size_t size, uint32_t deadline)
{
    uint8_t *p = buffer;
    size_t used = 0;
    while (used < size && (int32_t)(deadline - now()) > 0)
        used += xStreamBufferReceive(rx, p + used, size - used, pdMS_TO_TICKS(20));
    return used == size;
}
static bool write_bytes(const void *data, size_t n)
{
    size_t sent = 0;
    return at_transport_write(link, data, n, 10000, &sent) == AT_TRANSPORT_OK && sent == n;
}
static bool read_line(char *line, size_t capacity, uint32_t deadline)
{
    size_t n = 0;
    while (n + 1 < capacity) {
        if (!read_exact(line + n, 1, deadline)) return false;
        if (line[n++] == '\n') { line[n] = 0; return true; }
    }
    return false;
}
static void pattern(size_t size, unsigned kind, unsigned sequence)
{
    static const uint8_t special[] = {0, 255, '\r', '\n', 0xa7, 0x5c, 'O', 'K', '>', 0x7e, 0x7d};
    uint32_t random = 0x91234U ^ sequence;
    for (size_t i = 0; i < size; ++i) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        outgoing[i] = kind == 0 ? 0 : kind == 1 ? 255 :
                      kind == 2 ? special[i % sizeof(special)] : (uint8_t)random;
    }
}
#if defined(BINARY_PROBE_AUTOMATIC_RESET) && BINARY_PROBE_AUTOMATIC_RESET
static void reset_peer(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_bit_reset(GPIOC, GPIO_PIN_5);
    gd32h77x_console_printf("BINARY_RESET_ASSERT\r\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_bit_set(GPIOC, GPIO_PIN_5);
    reset_done = true;
    gd32h77x_console_printf("BINARY_RESET_RELEASE\r\n");
    vTaskDelete(NULL);
}
#endif
static bool run_case(size_t size, unsigned kind, unsigned sequence, bool inject)
{
    char command[80], header[128], trailer[6];
    unsigned got_sequence, got_size;
    unsigned long got_crc;
    uint32_t deadline;
    uint8_t ch = 0;
    pattern(size, kind, sequence);
    (void)xStreamBufferReset(rx);
    int n = snprintf(command, sizeof(command), "AT+TY553XLOOP=%u,%u\r\n", (unsigned)size, sequence);
    if (n <= 0 || !write_bytes(command, (size_t)n)) return false;
    deadline = now() + 10000;
    do { if (!read_exact(&ch, 1, deadline)) return false; } while (ch != '>');
    if (inject) {
#if defined(BINARY_PROBE_AUTOMATIC_RESET) && BINARY_PROBE_AUTOMATIC_RESET
        reset_done = false;
        if (xTaskCreate(reset_peer, "bin_reset", 512, NULL, 6, NULL) != pdPASS) return false;
#else
        return false;
#endif
    }
    if (!write_bytes(outgoing, size)) return false;
    deadline = now() + 10000;
    if (!read_line(header, sizeof(header), deadline)) return false;
    if (sscanf(header, "+TY553XLOOP:%u,%u,%lx", &got_sequence, &got_size, &got_crc) != 3 ||
        got_sequence != sequence || got_size != size || got_crc != crc32(outgoing, size)) return false;
    if (!read_exact(incoming, size, deadline) || !read_exact(trailer, sizeof(trailer), deadline)) return false;
    return memcmp(outgoing, incoming, size) == 0 && crc32(incoming, size) == got_crc &&
           memcmp(trailer, "\r\nOK\r\n", 6) == 0;
}
void app_entry(void *arg)
{
    (void)arg;
    static const size_t lengths[] = {1,239,240,241,1024,4096};
    unsigned sequence = 1, pass = 0, failed = 0;
    rx = xStreamBufferCreate(8192, 1);
    link = at_transport_get();
    if (!rx || !link) { gd32h77x_console_printf("BINARY_INIT_FAIL\r\n"); vTaskDelete(NULL); return; }
    link->fault_callback = fault; link->fault_context = NULL;
    link->rx_callback = receive; link->rx_context = NULL;
    link->rx_ready_callback = ready; link->rx_ready_context = NULL;
    if (at_transport_open(link) != AT_TRANSPORT_OK) { vTaskDelete(NULL); return; }
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (crc32((const uint8_t *)"123456789", 9) != 0xcbf43926U) {
        gd32h77x_console_printf("BINARY_CRC_SELFTEST_FAIL\r\n"); vTaskDelete(NULL); return;
    }
    for (unsigned round = 0; round < 5; ++round)
        for (unsigned kind = 0; kind < 4; ++kind)
            for (unsigned i = 0; i < sizeof(lengths)/sizeof(lengths[0]); ++i) {
                bool ok = run_case(lengths[i], kind, sequence++, false);
                if (ok) ++pass; else ++failed;
                gd32h77x_console_printf("BINARY_CASE seq=%u len=%u pattern=%u %s\r\n", sequence-1,
                                        (unsigned)lengths[i], kind, ok ? "PASS" : "FAIL");
                if (!ok) goto done;
            }
#if defined(BINARY_PROBE_AUTOMATIC_RESET) && BINARY_PROBE_AUTOMATIC_RESET
    rcu_periph_clock_enable(RCU_GPIOC);
    gpio_bit_set(GPIOC, GPIO_PIN_5);
    gpio_output_options_set(GPIOC, GPIO_OTYPE_OD, GPIO_OSPEED_60MHZ, GPIO_PIN_5);
    gpio_mode_set(GPIOC, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, GPIO_PIN_5);
    {
        bool interrupted_ok = run_case(4096, 3, sequence++, true);
        uint32_t limit = now() + 2000;
        while (!reset_done && (int32_t)(limit-now())>0) vTaskDelay(1);
        gd32h77x_console_printf("BINARY_INTERRUPTED rejected=%u released=%u\r\n", !interrupted_ok, reset_done);
        if (interrupted_ok || !reset_done) { ++failed; goto done; }
    }
#else
    {
        char cmd[96]; uint8_t ch = 0;
        uint32_t before;
        int n = snprintf(cmd, sizeof(cmd), "AT+TY553XLOOP=4096,%u,120000\r\n", sequence++);
        (void)xStreamBufferReset(rx);
        if (!write_bytes(cmd, (size_t)n)) { ++failed; goto done; }
        uint32_t deadline = now() + 10000;
        do { if (!read_exact(&ch, 1, deadline)) { ++failed; goto done; } } while (ch != '>');
        pattern(4096, 3, sequence);
        before = faults;
        if (!write_bytes(outgoing, 240)) { ++failed; goto done; }
        gd32h77x_console_printf("BINARY_MANUAL_RESET_READY old_seq=%u sent=240/4096 window_ms=110000\r\n", sequence-1);
        deadline = now() + 110000;
        while (faults == before && (int32_t)(deadline-now()) > 0) vTaskDelay(pdMS_TO_TICKS(20));
        if (faults == before) {
            gd32h77x_console_printf("BINARY_RESET_NOT_RUN data_pass=%u\r\n", pass);
            vTaskDelete(NULL); return;
        }
        gd32h77x_console_printf("BINARY_PARTIAL_INTERRUPTED old_seq=%u fault=%lu\r\n", sequence-1, (unsigned long)faults);
    }
#endif
    at_transport_suspend(link); at_transport_flush(link); at_transport_resume(link);
    vTaskDelay(pdMS_TO_TICKS(1500));
    for (unsigned i = 0; i < 10; ++i) {
        bool ok = run_case(4096, 3, sequence++, false);
        gd32h77x_console_printf("BINARY_RECOVERY seq=%u %s\r\n", sequence-1, ok ? "PASS" : "FAIL");
        if (ok) ++pass; else { ++failed; break; }
    }
done:
    gd32h77x_console_printf("BINARY_RESULT %s pass=%u fail=%u next_seq=%u\r\n", failed ? "FAIL" : "PASS", pass, failed, sequence);
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
