#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "gd32h77x_78x.h"
#include "gd32h77x_78x_spi.h"
#include "tkl_spi.h"
#include "tkl_system.h"

#define TEST_PORT TUYA_SPI_NUM_3
#define TEST_PERIPH SPI3
#define TEST_IRQ SPI3_IRQn
#define TEST_PAGE_SIZE 4096U

static const uint32_t completion_flag_mask =
    SPI_FLAG_RP | SPI_FLAG_TP | SPI_FLAG_ET | SPI_FLAG_ERRORS;

static TUYA_SPI_IRQ_EVT_E expected_event;
static unsigned int callback_count;
static TUYA_SPI_NUM_E callback_port;
static TUYA_SPI_IRQ_EVT_E callback_event;
static uint32_t callback_busy;
static uint32_t callback_irq_mask;
static uint32_t callback_status_flags;

static void spi_irq_callback(TUYA_SPI_NUM_E port, TUYA_SPI_IRQ_EVT_E event)
{
    TUYA_SPI_STATUS_T status;

    callback_count++;
    callback_port = port;
    callback_event = event;

    assert(port == TEST_PORT);
    assert(tkl_spi_get_status(port, &status) == OPRT_OK);
    callback_busy = status.busy;
    callback_irq_mask = fake_int[port];
    callback_status_flags = fake_stat[port];
}

static void spi_irq_test_begin(TUYA_SPI_IRQ_EVT_E event)
{
    const TUYA_SPI_BASE_CFG_T cfg = {
        .role = TUYA_SPI_ROLE_MASTER,
        .mode = TUYA_SPI_MODE0,
        .type = TUYA_SPI_SOFT_TYPE,
        .databits = TUYA_SPI_DATA_BIT8,
        .bitorder = TUYA_SPI_ORDER_MSB2LSB,
        .freq_hz = 1000000U,
        .spi_dma_flags = 0U,
    };

    callback_count = 0U;
    callback_port = TUYA_SPI_NUM_MAX;
    callback_event = TUYA_SPI_EVENT_MODE_FAULT + 1;
    callback_busy = 1U;
    callback_irq_mask = SPI_INT_COMPLETE_MASK;
    callback_status_flags = completion_flag_mask;
    expected_event = event;

    fake_spi_reset(TEST_PORT);
    assert(tkl_spi_init(TEST_PORT, &cfg) == OPRT_OK);
    assert(tkl_spi_irq_init(TEST_PORT, spi_irq_callback) == OPRT_OK);
    assert(tkl_spi_irq_enable(TEST_PORT) == OPRT_OK);
    assert(fake_nvic_enabled[TEST_IRQ] != 0U);
}

static void spi_irq_test_end(void)
{
    assert(tkl_spi_deinit(TEST_PORT) == OPRT_OK);
    fake_spi_reset(TEST_PORT);
    assert(fake_critical_depth == 0U);
    assert(fake_critical_entries == fake_critical_exits);
}

static void assert_completion_once(void)
{
    TUYA_SPI_STATUS_T status;

    assert(callback_count == 1U);
    assert(callback_port == TEST_PORT);
    assert(callback_event == expected_event);
    assert(callback_busy == 0U);
    assert((callback_irq_mask & SPI_INT_COMPLETE_MASK) == 0U);
    assert((callback_status_flags & completion_flag_mask) == 0U);

    assert(tkl_spi_get_status(TEST_PORT, &status) == OPRT_OK);
    assert(status.busy == 0U);
    assert((fake_int[TEST_PERIPH] & SPI_INT_COMPLETE_MASK) == 0U);
    assert((fake_stat[TEST_PERIPH] & completion_flag_mask) == 0U);
}

static void assert_repeated_et_is_ignored(void)
{
    fake_stat[TEST_PERIPH] |= SPI_FLAG_ET;
    SPI3_IRQHandler();
    assert(callback_count == 1U);
    assert((fake_int[TEST_PERIPH] & SPI_INT_COMPLETE_MASK) == 0U);
    assert((fake_stat[TEST_PERIPH] & SPI_FLAG_ET) == 0U);
}

static void test_transfer_complete(void)
{
    uint8_t tx[] = {0x10U, 0x32U, 0x54U, 0x76U};
    uint8_t rx[sizeof(tx)] = {0U};

    spi_irq_test_begin(TUYA_SPI_EVENT_TRANSFER_COMPLETE);
    assert(tkl_spi_transfer(TEST_PORT, tx, rx, sizeof(tx)) == OPRT_OK);
    assert(callback_count == 0U);
    assert((fake_int[TEST_PERIPH] & SPI_INT_COMPLETE_MASK) ==
           SPI_INT_COMPLETE_MASK);

    SPI3_IRQHandler();

    assert_completion_once();
    assert(memcmp(tx, rx, sizeof(tx)) == 0);
    assert_repeated_et_is_ignored();
    spi_irq_test_end();
    puts("PASS: transfer callback is TUYA_SPI_EVENT_TRANSFER_COMPLETE");
}

static void test_send_complete(void)
{
    uint8_t tx[] = {0xA1U, 0xB2U, 0xC3U};

    spi_irq_test_begin(TUYA_SPI_EVENT_TX_COMPLETE);
    assert(tkl_spi_send(TEST_PORT, tx, sizeof(tx)) == OPRT_OK);
    assert(callback_count == 0U);
    assert((fake_int[TEST_PERIPH] & SPI_INT_COMPLETE_MASK) ==
           SPI_INT_COMPLETE_MASK);

    SPI3_IRQHandler();

    assert_completion_once();
    assert_repeated_et_is_ignored();
    spi_irq_test_end();
    puts("PASS: send callback is TUYA_SPI_EVENT_TX_COMPLETE");
}

static void test_recv_complete(void)
{
    uint8_t rx[5] = {0U};

    spi_irq_test_begin(TUYA_SPI_EVENT_RX_COMPLETE);
    assert(tkl_spi_recv(TEST_PORT, rx, sizeof(rx)) == OPRT_OK);
    assert(callback_count == 0U);
    assert((fake_int[TEST_PERIPH] & SPI_INT_COMPLETE_MASK) ==
           SPI_INT_COMPLETE_MASK);

    SPI3_IRQHandler();

    assert_completion_once();
    for (size_t i = 0U; i < sizeof(rx); ++i) {
        assert(rx[i] == 0xFFU);
    }
    assert_repeated_et_is_ignored();
    spi_irq_test_end();
    puts("PASS: recv callback is TUYA_SPI_EVENT_RX_COMPLETE");
}

static int test_error_case(const char *label, int full_duplex,
                           uint32_t error_flag, TUYA_SPI_IRQ_EVT_E event)
{
    uint8_t tx[] = {0x11U, 0x22U, 0x33U, 0x44U};
    uint8_t rx[sizeof(tx)] = {0U};
    unsigned int disable_before;
    unsigned int handler_disable_calls;
    unsigned int statc_before_abort;
    unsigned int abort_statc_calls;
    unsigned int suspend_before_abort;
    unsigned int abort_suspend_calls;
    int ok = 1;

    spi_irq_test_begin(event);
    if (full_duplex != 0) {
        assert(tkl_spi_transfer(TEST_PORT, tx, rx, sizeof(tx)) == OPRT_OK);
    } else {
        assert(tkl_spi_send(TEST_PORT, tx, sizeof(tx)) == OPRT_OK);
    }
    assert(callback_count == 0U);
    assert((fake_int[TEST_PERIPH] & SPI_INT_COMPLETE_MASK) ==
           SPI_INT_COMPLETE_MASK);

    disable_before = fake_spi_disable_count[TEST_PERIPH];
    fake_stat[TEST_PERIPH] |= error_flag;
    SPI3_IRQHandler();
    handler_disable_calls =
        fake_spi_disable_count[TEST_PERIPH] - disable_before;

    statc_before_abort = fake_spi_statc_write_count[TEST_PERIPH];
    suspend_before_abort = fake_spi_suspend_probe_count[TEST_PERIPH];
    assert(tkl_spi_abort_transfer(TEST_PORT) == OPRT_OK);
    abort_statc_calls =
        fake_spi_statc_write_count[TEST_PERIPH] - statc_before_abort;
    abort_suspend_calls =
        fake_spi_suspend_probe_count[TEST_PERIPH] - suspend_before_abort;

    {
        TUYA_SPI_STATUS_T status;
        assert(tkl_spi_get_status(TEST_PORT, &status) == OPRT_OK);
        assert(status.busy == 0U);
    }

    spi_irq_test_end();

    if (callback_count != 1U) {
        fprintf(stderr, "FAIL: %s IRQ callback count=%u, expected 1\n",
                label, callback_count);
        ok = 0;
    }
    if (callback_event != event) {
        fprintf(stderr, "FAIL: %s IRQ event=%d, expected %d\n",
                label, (int)callback_event, (int)event);
        ok = 0;
    }
    if (callback_busy != 0U) {
        fprintf(stderr, "FAIL: %s IRQ callback observed busy=%u\n",
                label, callback_busy);
        ok = 0;
    }
    if ((callback_irq_mask & SPI_INT_COMPLETE_MASK) != 0U) {
        fprintf(stderr, "FAIL: %s IRQ left interrupt mask=0x%x\n",
                label, callback_irq_mask & SPI_INT_COMPLETE_MASK);
        ok = 0;
    }
    if (handler_disable_calls != 0U) {
        fprintf(stderr,
                "FAIL: %s error IRQ called spi_disable %u time(s); expected 0\n",
                label, handler_disable_calls);
        ok = 0;
    }
    if (abort_suspend_calls == 0U) {
        fprintf(stderr, "FAIL: %s abort did not enter suspend cleanup path\n",
                label);
        ok = 0;
    }
    if (abort_statc_calls == 0U) {
        fprintf(stderr, "FAIL: %s abort did not write SPI_STATC cleanup\n",
                label);
        ok = 0;
    }

    if (ok != 0) {
        printf("PASS: %s IRQ event, callback, masks, and suspend cleanup\n",
               label);
    } else {
        printf("FAIL: %s IRQ error-path regression\n", label);
    }
    return ok;
}

static void *map_test_buffer(void)
{
    void *buffer = mmap(NULL, TEST_PAGE_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    assert(buffer != MAP_FAILED);
    memset(buffer, 0xA5, TEST_PAGE_SIZE);
    return buffer;
}

static void test_abort_does_not_touch_buffers(void)
{
    void *tx = map_test_buffer();
    void *rx = map_test_buffer();

    spi_irq_test_begin(TUYA_SPI_EVENT_TRANSFER_COMPLETE);
    assert(tkl_spi_transfer(TEST_PORT, tx, rx, 4U) == OPRT_OK);
    assert(tkl_spi_abort_transfer(TEST_PORT) == OPRT_OK);

    {
        TUYA_SPI_STATUS_T status;
        assert(tkl_spi_get_status(TEST_PORT, &status) == OPRT_OK);
        assert(status.busy == 0U);
    }

    assert(munmap(tx, TEST_PAGE_SIZE) == 0);
    assert(munmap(rx, TEST_PAGE_SIZE) == 0);

    /* Stale interrupt flags must not make the handler dereference the two
     * now-unmapped buffers that abort cleared from the driver state. */
    fake_stat[TEST_PERIPH] |= SPI_FLAG_RP | SPI_FLAG_TP | SPI_FLAG_ET;
    fake_int[TEST_PERIPH] |= SPI_INT_COMPLETE_MASK;
    SPI3_IRQHandler();

    assert(callback_count == 0U);
    assert((fake_int[TEST_PERIPH] & SPI_INT_COMPLETE_MASK) == 0U);
    spi_irq_test_end();
    puts("PASS: abort leaves no callback and IRQ does not touch freed buffers");
}

int main(void)
{
    int failures = 0;

    test_transfer_complete();
    test_send_complete();
    test_recv_complete();
    test_abort_does_not_touch_buffers();
    failures += test_error_case("send/RXORERR", 0,
                                SPI_FLAG_RXORERR,
                                TUYA_SPI_EVENT_DATA_LOST) == 0;
    failures += test_error_case("transfer/CONFERR", 1,
                                SPI_FLAG_CONFERR,
                                TUYA_SPI_EVENT_MODE_FAULT) == 0;
    assert(fake_critical_depth == 0U);
    assert(fake_critical_entries == fake_critical_exits);

    if (failures != 0) {
        puts("GD32H77X SPI IRQ host regression: FAIL");
        return 1;
    }
    puts("GD32H77X SPI IRQ host regression: PASS");
    return 0;
}
