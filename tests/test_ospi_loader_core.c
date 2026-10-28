/*
 * Copyright (c) 2026 Tuya Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Executes the production mailbox/business code against a byte-level GD25Q64E
 * SPI model. The physical GPIO implementation is excluded by OSPI_HOST_TEST.
 */
#define OSPI_HOST_TEST
#include "../gd32h77x_os/tools/download/ospi_loader/gd32h77x_ospi_loader.c"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned char flash_mem[8192];
static unsigned char tx_payload[256];
static unsigned tx_count;
static unsigned tx_index;
static unsigned addr_count;
static uint32_t address;
static uint8_t opcode;
static uint8_t sr1;
static uint8_t sr2;
static uint8_t wel;
static int fail_wel;
static int stuck_wip;
static uint32_t fake_jedec = 0xC84017U;
static unsigned erase_ops;
static unsigned erase_commands;
static unsigned program_ops;

void ospi_test_select(void)
{
    tx_count = 0;
    tx_index = 0;
    addr_count = 0;
    address = 0;
    opcode = 0;
    memset(tx_payload, 0, sizeof(tx_payload));
}

void ospi_test_deselect(void)
{
    if (opcode == 0x20U && wel && !stuck_wip) {
        assert(address + 4096U <= sizeof(flash_mem));
        memset(flash_mem + address, 0xFF, 4096U);
        ++erase_ops;
        wel = 0;
    } else if (opcode == 0x02U && wel && !stuck_wip) {
        unsigned i;
        assert(tx_count <= 256U);
        assert(address + tx_count <= sizeof(flash_mem));
        for (i = 0; i < tx_count; ++i) flash_mem[address + i] &= tx_payload[i];
        ++program_ops;
        wel = 0;
    }
}

void ospi_test_clock_low(void) {}
void ospi_test_clock_high(void) {}
void ospi_test_data_low(void) {}
void ospi_test_data_high(void) {}

uint8_t ospi_test_spi_byte(uint8_t out)
{
    unsigned char ret = 0xFFU;
    if (tx_index++ == 0U) {
        opcode = out;
        if (opcode == 0x20U) ++erase_commands;
        if (opcode == 0x06U && !fail_wel && !stuck_wip) wel = 1U;
        return 0xFFU;
    }
    if (opcode == 0x05U) {
        ret = (uint8_t)(sr1 | (wel ? 2U : 0U) | (stuck_wip ? 1U : 0U));
    } else if (opcode == 0x35U) {
        ret = sr2;
    } else if (opcode == 0x9FU && tx_index <= 4U) {
        ret = (uint8_t)(fake_jedec >> (8U * (4U - tx_index)));
    } else if (opcode == 0x03U || opcode == 0x20U || opcode == 0x02U) {
        if (addr_count < 3U) {
            address = (address << 8) | out;
            ++addr_count;
        } else if (opcode == 0x03U) {
            if (address < sizeof(flash_mem)) ret = flash_mem[address++];
        } else if (opcode == 0x02U) {
            assert(tx_count < sizeof(tx_payload));
            tx_payload[tx_count++] = out;
        }
    }
    return ret;
}

static int request(volatile ospi_mailbox_t *mb, uint8_t *buffer)
{
    return ospi_process_request(mb, buffer);
}

int main(void)
{
    volatile ospi_mailbox_t mb;
    uint8_t buffer[256];
    unsigned i;
    memset((void *)&mb, 0, sizeof(mb));
    memset(flash_mem, 0x00, sizeof(flash_mem));
    memset(buffer, 0xA5, sizeof(buffer));

    mb.command = OSPI_CMD_READ_ID;
    assert(request(&mb, buffer) == 0);
    assert(mb.jedec_id == 0xC84017U);

    mb.command = OSPI_CMD_ERASE_4K;
    mb.offset = 0;
    mb.length = 4096;
    assert(request(&mb, buffer) == 0);
    assert(erase_ops == 1U);
    assert(erase_commands == 1U);
    for (i = 0; i < 4096U; ++i) assert(flash_mem[i] == 0xFFU);

    mb.command = OSPI_CMD_PROGRAM;
    mb.offset = 255;
    mb.length = 2;
    assert(request(&mb, buffer) != 0);
    assert(program_ops == 0U);

    mb.offset = 256;
    mb.length = 256;
    assert(request(&mb, buffer) == 0);
    assert(program_ops == 1U);

    mb.command = OSPI_CMD_ERASE_4K;
    mb.offset = 4096;
    mb.length = 4096;
    sr1 = 0x04U; /* BP0 */
    assert(request(&mb, buffer) != 0);
    assert(erase_ops == 1U);
    assert(erase_commands == 1U); /* protection check rejected before erase opcode */
    sr1 = 0;

    fake_jedec = 0x123456U;
    mb.command = OSPI_CMD_ERASE_4K;
    mb.offset = 0;
    mb.length = 4096;
    assert(request(&mb, buffer) != 0);
    assert(erase_commands == 1U);
    fake_jedec = 0xC84017U;

    mb.command = OSPI_CMD_PROGRAM;
    mb.offset = 512;
    mb.length = 1;
    sr2 = 0x40U; /* CMP */
    assert(request(&mb, buffer) != 0);
    assert(program_ops == 1U);
    sr2 = 0;
    fail_wel = 1;
    assert(request(&mb, buffer) != 0);
    assert(program_ops == 1U);
    fail_wel = 0;
    stuck_wip = 1;
    assert(request(&mb, buffer) != 0);
    assert(program_ops == 1U);
    stuck_wip = 0;

    mb.command = OSPI_CMD_READ;
    mb.offset = 256;
    mb.length = 256;
    assert(request(&mb, buffer) == 0);
    for (i = 0; i < 256U; ++i) assert(buffer[i] == 0xA5U);

    mb.offset = 0x007FFFFEU;
    mb.length = 4;
    assert(request(&mb, buffer) != 0);
    puts("production OSPI loader core model: PASS");
    return 0;
}
