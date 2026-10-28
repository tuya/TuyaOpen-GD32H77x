/*
 * Copyright (c) 2026 Tuya Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * SRAM-resident, single-line SPI programmer for a resolved GD25Q64E device.
 * OSPI0 board_binding pins are supplied at build time and operated as GPIO.
 * No flash status register is written.
 */
#include "gd32h77x_mailbox.h"
#ifdef OSPI_HOST_TEST
extern void ospi_test_select(void);
extern void ospi_test_deselect(void);
extern void ospi_test_clock_low(void);
extern void ospi_test_clock_high(void);
extern void ospi_test_data_low(void);
extern void ospi_test_data_high(void);
extern uint8_t ospi_test_spi_byte(uint8_t out);
#define select_flash() ospi_test_select()
#define deselect_flash() ospi_test_deselect()
#define clock_low() ospi_test_clock_low()
#define clock_high() ospi_test_clock_high()
#define data_low() ospi_test_data_low()
#define data_high() ospi_test_data_high()
#else
#include "gd32h77x_78x.h"
#include "gd32h77x_78x_gpio.h"
#include "gd32h77x_78x_rcu.h"
#ifdef OSPI_UART_LOADER
#include "gd32h77x_78x_usart.h"
#endif
#ifndef OSPI_CS_GPIO
#error "OSPI CS GPIO binding is required"
#endif
#ifndef OSPI_CLK_GPIO
#error "OSPI CLK GPIO binding is required"
#endif
#ifndef OSPI_MOSI_GPIO
#error "OSPI MOSI GPIO binding is required"
#endif
#ifndef OSPI_MISO_GPIO
#error "OSPI MISO GPIO binding is required"
#endif
#define select_flash() (GPIO_BC(OSPI_CS_GPIO) = OSPI_CS_PIN)
#define deselect_flash() (GPIO_BOP(OSPI_CS_GPIO) = OSPI_CS_PIN)
#define clock_low() (GPIO_BC(OSPI_CLK_GPIO) = OSPI_CLK_PIN)
#define clock_high() (GPIO_BOP(OSPI_CLK_GPIO) = OSPI_CLK_PIN)
#define data_low() (GPIO_BC(OSPI_MOSI_GPIO) = OSPI_MOSI_PIN)
#define data_high() (GPIO_BOP(OSPI_MOSI_GPIO) = OSPI_MOSI_PIN)
#endif

#define NOR_CAPACITY UINT32_C(0x00800000)
#define NOR_SECTOR UINT32_C(4096)
#define NOR_PAGE UINT32_C(256)
#define READY_WAIT_LIMIT UINT32_C(200000)
#define WEL_WAIT_LIMIT UINT32_C(10000)
#define NOR_EXPECTED_JEDEC UINT32_C(0xC84017)

static inline void delay_edge(void)
{
#ifndef OSPI_HOST_TEST
    volatile unsigned count = 24;
    while (count-- != 0U) { __NOP(); }
#endif
}

static uint8_t spi_byte(uint8_t out)
{
#ifdef OSPI_HOST_TEST
    return ospi_test_spi_byte(out);
#else
    uint8_t in = 0;
    unsigned bit;
    for (bit = 0; bit < 8U; ++bit) {
        clock_low();
        if ((out & 0x80U) != 0U) data_high(); else data_low();
        delay_edge();
        clock_high();
        delay_edge();
        in = (uint8_t)((in << 1) | ((GPIO_ISTAT(OSPI_MISO_GPIO) & OSPI_MISO_PIN) != 0U));
        out <<= 1;
    }
    clock_low();
    return in;
#endif
}

static void address24(uint32_t address)
{
    (void)spi_byte((uint8_t)(address >> 16));
    (void)spi_byte((uint8_t)(address >> 8));
    (void)spi_byte((uint8_t)address);
}

static uint8_t read_status1(void)
{
    uint8_t value;
    select_flash();
    (void)spi_byte(0x05U);
    value = spi_byte(0xFFU);
    deselect_flash();
    return value;
}

static uint8_t read_status2(void)
{
    uint8_t value;
    select_flash();
    (void)spi_byte(0x35U);
    value = spi_byte(0xFFU);
    deselect_flash();
    return value;
}

static uint32_t read_jedec(void)
{
    uint32_t id;
    select_flash();
    (void)spi_byte(0x9FU);
    id = ((uint32_t)spi_byte(0xFFU) << 16);
    id |= ((uint32_t)spi_byte(0xFFU) << 8);
    id |= spi_byte(0xFFU);
    deselect_flash();
    return id;
}

static int wait_ready(void)
{
    uint32_t count;
    for (count = 0; count < READY_WAIT_LIMIT; ++count) {
        if ((read_status1() & 1U) == 0U) return 0;
    }
    return 1;
}

static int nor_ready_for_write(void)
{
    if (read_jedec() != NOR_EXPECTED_JEDEC) return 0;
    uint8_t sr1 = read_status1();
    uint8_t sr2 = read_status2();
    /* SR1 BP[4:0] and SR2 CMP select main-array protection. Never unlock. */
    if ((sr1 & 0x7DU) != 0U || (sr2 & 0x40U) != 0U) return 0;
    /* Unknown/protected execution states fail closed. */
    if ((sr1 & 1U) != 0U || (sr2 & 0xC0U) != 0U) return 0;
    return 1;
}

static int write_enable(void)
{
    uint32_t count;
    select_flash();
    (void)spi_byte(0x06U);
    deselect_flash();
    for (count = 0; count < WEL_WAIT_LIMIT; ++count) {
        if ((read_status1() & 2U) != 0U) return 0;
    }
    return 1;
}

static int read_data(uint32_t offset, uint8_t *dst, uint32_t length)
{
    uint32_t i;
    if (length == 0U || length > NOR_PAGE || offset > NOR_CAPACITY ||
        length > NOR_CAPACITY - offset) return 1;
    if (wait_ready() != 0) return 2;
    select_flash();
    (void)spi_byte(0x03U);
    address24(offset);
    for (i = 0; i < length; ++i) dst[i] = spi_byte(0xFFU);
    deselect_flash();
    return 0;
}

static int erase_sector(uint32_t offset)
{
    if ((offset % NOR_SECTOR) != 0U || offset >= NOR_CAPACITY) return 1;
    if (wait_ready() != 0 || !nor_ready_for_write() || write_enable() != 0) return 2;
    select_flash();
    (void)spi_byte(0x20U);
    address24(offset);
    deselect_flash();
    return wait_ready();
}

static int program_page(uint32_t offset, const uint8_t *src, uint32_t length)
{
    uint32_t i;
    if (length == 0U || length > NOR_PAGE || offset >= NOR_CAPACITY ||
        length > NOR_CAPACITY - offset ||
        (offset / NOR_PAGE) != ((offset + length - 1U) / NOR_PAGE)) return 1;
    if (wait_ready() != 0 || !nor_ready_for_write() || write_enable() != 0) return 2;
    select_flash();
    (void)spi_byte(0x02U);
    address24(offset);
    for (i = 0; i < length; ++i) (void)spi_byte(src[i]);
    deselect_flash();
    return wait_ready();
}

int ospi_process_request(volatile ospi_mailbox_t *mb, uint8_t *buffer)
{
    int result = 0;
    switch (mb->command) {
    case OSPI_CMD_READ_ID:
        mb->jedec_id = read_jedec();
        mb->status1 = read_status1();
        mb->status2 = read_status2();
        break;
    case OSPI_CMD_READ_STATUS:
        mb->status1 = read_status1();
        mb->status2 = read_status2();
        break;
    case OSPI_CMD_READ:
        if (mb->length > NOR_PAGE) return 1;
        result = read_data(mb->offset, buffer, mb->length);
        break;
    case OSPI_CMD_ERASE_4K:
        result = mb->length == NOR_SECTOR ? erase_sector(mb->offset) : 1;
        break;
    case OSPI_CMD_PROGRAM:
        if (mb->length > NOR_PAGE) return 1;
        result = program_page(mb->offset, buffer, mb->length);
        break;
    default:
        result = 1;
        break;
    }
    return result;
}

#ifndef OSPI_HOST_TEST
static void init_gpio(void)
{
    rcu_periph_clock_enable(OSPI_CS_RCU);
    rcu_periph_clock_enable(OSPI_CLK_RCU);
    rcu_periph_clock_enable(OSPI_MOSI_RCU);
    rcu_periph_clock_enable(OSPI_MISO_RCU);
    GPIO_BOP(OSPI_CS_GPIO) = OSPI_CS_PIN;
    GPIO_BC(OSPI_CLK_GPIO) = OSPI_CLK_PIN;
    GPIO_BC(OSPI_MOSI_GPIO) = OSPI_MOSI_PIN;
    gpio_mode_set(OSPI_CS_GPIO, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, OSPI_CS_PIN);
    gpio_output_options_set(OSPI_CS_GPIO, GPIO_OTYPE_PP, GPIO_OSPEED_12MHZ, OSPI_CS_PIN);
    gpio_mode_set(OSPI_CLK_GPIO, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, OSPI_CLK_PIN);
    gpio_output_options_set(OSPI_CLK_GPIO, GPIO_OTYPE_PP, GPIO_OSPEED_12MHZ, OSPI_CLK_PIN);
    gpio_mode_set(OSPI_MOSI_GPIO, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, OSPI_MOSI_PIN);
    gpio_output_options_set(OSPI_MOSI_GPIO, GPIO_OTYPE_PP, GPIO_OSPEED_12MHZ, OSPI_MOSI_PIN);
    gpio_mode_set(OSPI_MISO_GPIO, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP, OSPI_MISO_PIN);
#ifdef OSPI_WP_GPIO
    rcu_periph_clock_enable(OSPI_WP_RCU);
    GPIO_BOP(OSPI_WP_GPIO) = OSPI_WP_PIN;
    gpio_mode_set(OSPI_WP_GPIO, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, OSPI_WP_PIN);
    gpio_output_options_set(OSPI_WP_GPIO, GPIO_OTYPE_PP, GPIO_OSPEED_12MHZ, OSPI_WP_PIN);
#endif
#ifdef OSPI_HOLD_GPIO
    rcu_periph_clock_enable(OSPI_HOLD_RCU);
    GPIO_BOP(OSPI_HOLD_GPIO) = OSPI_HOLD_PIN;
    gpio_mode_set(OSPI_HOLD_GPIO, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, OSPI_HOLD_PIN);
    gpio_output_options_set(OSPI_HOLD_GPIO, GPIO_OTYPE_PP, GPIO_OSPEED_12MHZ, OSPI_HOLD_PIN);
#endif
    clock_low();
}

#ifdef OSPI_UART_LOADER

#define UART_LOADER_BAUD      115200U
#define UART_FRAME_SYNC       0x5AU
#define UART_FRAME_SIZE       9U
#define UART_BODY_SIZE        7U
#define UART_RX_TIMEOUT       600000U
#define UART_STATUS_OK        0x06U
#define UART_STATUS_ERROR     0x15U

#define UART_CMD_PING         0x01U
#define UART_CMD_READ         0x02U
#define UART_CMD_ERASE        0x03U
#define UART_CMD_PROGRAM      0x04U
#define UART_CMD_RESET        0x05U
#define UART_CMD_INFO         0x06U

static uint32_t uart_base = USART1;
static uint8_t uart_payload[256];

static void loader_system_reset(void)
{
    *(volatile uint32_t *)0xE000ED0CU = (UINT32_C(0x5FA) << 16) | UINT32_C(0x4);
    __DSB();
}

static void loader_uart_pins(uint32_t base, uint32_t *tx_port,
                             uint32_t *rx_port, uint32_t *tx_pin,
                             uint32_t *rx_pin, rcu_periph_enum *port_clock,
                             rcu_periph_enum *usart_clock)
{
    if (base == USART0) {
        *tx_port = GPIOA;
        *rx_port = GPIOA;
        *tx_pin = GPIO_PIN_9;
        *rx_pin = GPIO_PIN_10;
        *port_clock = RCU_GPIOA;
        *usart_clock = RCU_USART0;
    } else if (base == USART2) {
        *tx_port = GPIOB;
        *rx_port = GPIOB;
        *tx_pin = GPIO_PIN_10;
        *rx_pin = GPIO_PIN_11;
        *port_clock = RCU_GPIOB;
        *usart_clock = RCU_USART2;
    } else {
        *tx_port = GPIOA;
        *rx_port = GPIOA;
        *tx_pin = GPIO_PIN_2;
        *rx_pin = GPIO_PIN_3;
        *port_clock = RCU_GPIOA;
        *usart_clock = RCU_USART1;
    }
}

static void loader_uart_init(void)
{
    uint32_t tx_port, rx_port, tx_pin, rx_pin;
    rcu_periph_enum port_clock, usart_clock;

    uart_base = *(volatile uint32_t *)UINT32_C(0x2400000C);
    if (uart_base != USART0 && uart_base != USART1 && uart_base != USART2) {
        uart_base = USART1;
    }
    loader_uart_pins(uart_base, &tx_port, &rx_port, &tx_pin, &rx_pin,
                     &port_clock, &usart_clock);
    rcu_periph_clock_enable(port_clock);
    rcu_periph_clock_enable(usart_clock);
    gpio_af_set(tx_port, GPIO_AF_7, tx_pin);
    gpio_mode_set(tx_port, GPIO_MODE_AF, GPIO_PUPD_PULLUP, tx_pin);
    gpio_output_options_set(tx_port, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, tx_pin);
    gpio_af_set(rx_port, GPIO_AF_7, rx_pin);
    gpio_mode_set(rx_port, GPIO_MODE_AF, GPIO_PUPD_PULLUP, rx_pin);
    gpio_output_options_set(rx_port, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, rx_pin);
    usart_deinit(uart_base);
    usart_baudrate_set(uart_base, UART_LOADER_BAUD);
    usart_word_length_set(uart_base, USART_WL_8BIT);
    usart_stop_bit_set(uart_base, USART_STB_1BIT);
    usart_parity_config(uart_base, USART_PM_NONE);
    usart_receive_config(uart_base, USART_RECEIVE_ENABLE);
    usart_transmit_config(uart_base, USART_TRANSMIT_ENABLE);
    usart_enable(uart_base);
}

static uint8_t uart_get(void)
{
    while (usart_flag_get(uart_base, USART_FLAG_RBNE) == RESET) {
    }
    return (uint8_t)usart_data_receive(uart_base);
}

static int uart_get_timeout(uint8_t *value, uint32_t limit)
{
    while (limit-- != 0U) {
        if (usart_flag_get(uart_base, USART_FLAG_RBNE) != RESET) {
            *value = (uint8_t)usart_data_receive(uart_base);
            return 0;
        }
    }
    return 1;
}

static void uart_put(uint8_t value)
{
    while (usart_flag_get(uart_base, USART_FLAG_TBE) == RESET) {
    }
    usart_data_transmit(uart_base, value);
}

static void uart_flush(void)
{
    while (usart_flag_get(uart_base, USART_FLAG_TC) == RESET) {
    }
}

static void uart_put_u32(uint32_t value)
{
    uart_put((uint8_t)(value >> 24));
    uart_put((uint8_t)(value >> 16));
    uart_put((uint8_t)(value >> 8));
    uart_put((uint8_t)value);
}

void ospi_loader_main(void)
{
    volatile uint32_t *diag = (volatile uint32_t *)UINT32_C(0x2407F000);

    loader_uart_init();
    init_gpio();
    diag[0] = uart_base;
    diag[1] = RCU_CFG0;
    diag[2] = RCU_CFG1;
    diag[3] = RCU_ADDCTL1;
    diag[4] = USART_BAUD(uart_base);
    diag[5] = RCU_AHB4EN;
    diag[6] = RCU_APB1EN;
    diag[7] = RCU_APB2EN;
    diag[8] = USART_CTL0(uart_base);
    diag[9] = USART_STAT(uart_base);
    diag[10] = UINT32_C(0x11111111);
    diag[11] = UINT32_C(0x22222222);

    for (;;) {
        uint8_t frame[UART_FRAME_SIZE];
        uint8_t checksum = 0;
        uint8_t value;
        uint32_t offset;
        uint32_t length;
        uint32_t index;
        int timed_out = 0;

        do {
            if (uart_get_timeout(&value, UART_RX_TIMEOUT) != 0) {
                continue;
            }
        } while (value != UART_FRAME_SYNC);
        frame[0] = value;
        for (index = 1; index < UART_FRAME_SIZE; ++index) {
            if (uart_get_timeout(&value, UART_RX_TIMEOUT) != 0) {
                timed_out = 1;
                break;
            }
            frame[index] = value;
            if (index < UART_FRAME_SIZE - 1U) {
                checksum ^= value;
            }
        }
        if (timed_out != 0) {
            continue;
        }
        if (checksum != frame[UART_FRAME_SIZE - 1U]) {
            ++diag[13];
            continue;
        }
        diag[12] = UINT32_C(0x33333333);

        offset = ((uint32_t)frame[2] << 24) | ((uint32_t)frame[3] << 16) |
                 ((uint32_t)frame[4] << 8) | (uint32_t)frame[5];
        length = ((uint32_t)frame[6] << 8) | (uint32_t)frame[7];

        switch (frame[1]) {
        case UART_CMD_PING:
            uart_put(UART_STATUS_OK);
            uart_put_u32(read_jedec());
            break;
        case UART_CMD_READ:
            if (length == 0U || length > sizeof(uart_payload) ||
                read_data(offset, uart_payload, length) != 0) {
                uart_put(UART_STATUS_ERROR);
                break;
            }
            uart_put(UART_STATUS_OK);
            for (index = 0; index < length; ++index) {
                uart_put(uart_payload[index]);
            }
            break;
        case UART_CMD_ERASE:
            uart_put(erase_sector(offset) == 0 ? UART_STATUS_OK : UART_STATUS_ERROR);
            break;
        case UART_CMD_PROGRAM:
            if (length == 0U || length > sizeof(uart_payload)) {
                uart_put(UART_STATUS_ERROR);
                break;
            }
            uart_put(UART_STATUS_OK);
            uart_flush();
            for (index = 0; index < length; ++index) {
                uart_payload[index] = uart_get();
            }
            uart_put(program_page(offset, uart_payload, length) == 0
                     ? UART_STATUS_OK : UART_STATUS_ERROR);
            break;
        case UART_CMD_INFO:
            uart_put(UART_STATUS_OK);
            uart_put_u32(read_jedec());
            uart_put(read_status1());
            uart_put(read_status2());
            break;
        case UART_CMD_RESET:
            uart_put(UART_STATUS_OK);
            uart_flush();
            loader_system_reset();
            for (;;) {
            }
        default:
            uart_put(UART_STATUS_ERROR);
            break;
        }
        uart_flush();
    }
}

#else

void ospi_loader_main(void)
{
    volatile ospi_mailbox_t *mb = (volatile ospi_mailbox_t *)OSPI_MAILBOX_ADDR;
    volatile uint32_t last_sequence = 0;
    init_gpio();
    mb->magic = OSPI_MAILBOX_MAGIC;
    mb->status = OSPI_STATUS_IDLE;
    mb->result = 0;
    mb->jedec_id = read_jedec();
    mb->status1 = read_status1();
    mb->status2 = read_status2();
    __DSB();
    for (;;) {
        uint32_t seq = mb->sequence;
        uint8_t *buffer = (uint8_t *)OSPI_BUFFER_ADDR;
        if (mb->request_magic != OSPI_REQUEST_MAGIC || seq == last_sequence) continue;
        last_sequence = seq;
        mb->status = OSPI_STATUS_RUNNING;
        __DSB();
        int result = ospi_process_request(mb, buffer);
        mb->result = (uint32_t)result;
        mb->request_magic = 0;
        mb->status = result == 0 ? OSPI_STATUS_DONE : OSPI_STATUS_ERROR;
        __DSB();
    }
}

#endif
#endif
