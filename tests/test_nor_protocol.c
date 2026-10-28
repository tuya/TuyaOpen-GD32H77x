#include "gd32h77x_nor.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static int s_configure_result;
static int s_transfer_result;
static int s_id_match;
static int s_busy_forever;
static int s_stick_wip_on_write;
static int s_reject_wel;
static int s_protected;
static uint32_t s_read_calls;
static uint32_t s_program_calls;
static uint32_t s_erase_calls;
static uint32_t s_program_offsets[4];
static uint32_t s_program_sizes[4];
static uint8_t s_last_instruction;
static uint8_t s_nor_memory[UINT32_C(0x00800000)];
static int s_memory_initialized;

int gd32h77x_nor_host_configure(const gd32h77x_layout_binding_t *binding)
{
    return binding == NULL ? -1 : s_configure_result;
}

int gd32h77x_nor_host_transfer(uint8_t instruction, uint32_t address,
                              int has_address, uint8_t *data, uint32_t size)
{
    (void)address;
    (void)has_address;
    s_last_instruction = instruction;
    if (s_transfer_result != 0 || data == NULL || size == 0U) {
        return -1;
    }
    if (instruction == UINT8_C(0x9F) && size == 3U) {
        data[0] = s_id_match ? 0xC8U : 0xEFU;
        data[1] = 0x40U;
        data[2] = 0x17U;
    } else if (instruction == UINT8_C(0x05) && size == 1U) {
        data[0] = (uint8_t)((s_busy_forever ? 1U : 0U) | (s_reject_wel ? 0U : 2U));
    } else if (instruction == UINT8_C(0x35) && size == 1U) {
        data[0] = s_protected ? 0x40U : 0U;
    } else if (instruction == UINT8_C(0x03)) {
        if (address > sizeof(s_nor_memory) || size > sizeof(s_nor_memory) - address) {
            return -1;
        }
        if (!s_memory_initialized) {
            memset(s_nor_memory, 0U, sizeof(s_nor_memory));
            s_memory_initialized = 1;
        }
        memcpy(data, s_nor_memory + address, size);
        ++s_read_calls;
    } else {
        return -1;
    }
    return 0;
}

int gd32h77x_nor_host_write(uint8_t instruction, uint32_t address,
                            int has_address, const uint8_t *data, uint32_t size)
{
    uint32_t i;
    s_last_instruction = instruction;
    if (s_transfer_result != 0 || (size != 0U && data == NULL)) {
        return -1;
    }
    if (instruction == UINT8_C(0x06) && !has_address && size == 0U) {
        if (!s_reject_wel) {
            s_reject_wel = 0;
        }
        return 0;
    }
    if (instruction == UINT8_C(0x20) && has_address && size == 0U) {
        if (address >= sizeof(s_nor_memory) || (address % 4096U) != 0U || s_reject_wel) {
            return -1;
        }
        memset(s_nor_memory + address, 0xFF, 4096U);
        ++s_erase_calls;
        if (s_stick_wip_on_write) {
            s_busy_forever = 1;
        }
        return 0;
    }
    if (instruction == UINT8_C(0x02) && has_address && size != 0U) {
        if (address > sizeof(s_nor_memory) || size > sizeof(s_nor_memory) - address ||
            (address / 256U) != ((address + size - 1U) / 256U) || s_reject_wel) {
            return -1;
        }
        for (i = 0; i < size; ++i) {
            s_nor_memory[address + i] &= data[i];
        }
        if (s_program_calls < 4U) {
            s_program_offsets[s_program_calls] = address;
            s_program_sizes[s_program_calls] = size;
        }
        ++s_program_calls;
        if (s_stick_wip_on_write) {
            s_busy_forever = 1;
        }
        return 0;
    }
    return -1;
}

int main(void)
{
    gd32h77x_layout_device_t device = gd32h77x_layout_devices[2];
    gd32h77x_layout_binding_t binding = gd32h77x_layout_bindings[2];
    uint8_t data[300];
    uint8_t pattern[4] = {0x12U, 0x34U, 0x56U, 0x78U};
    uint32_t jedec_id = 0U;

    CHECK(gd32h77x_nor_binding_valid(&device, &binding));
    binding.data[2] = "PA0";
    CHECK(!gd32h77x_nor_binding_valid(&device, &binding));
    binding = gd32h77x_layout_bindings[2];
    binding.controller = "OSPI1";
    CHECK(!gd32h77x_nor_binding_valid(&device, &binding));
    binding = gd32h77x_layout_bindings[2];
    device.geometry_id = "OTHER_NOR";
    CHECK(!gd32h77x_nor_binding_valid(&device, &binding));
    device = gd32h77x_layout_devices[2];

    s_id_match = 0;
    CHECK(gd32h77x_nor_init(2U) != 0);
    CHECK(gd32h77x_nor_get_jedec_id(2U, &jedec_id) != 0);
    s_id_match = 1;
    CHECK(gd32h77x_nor_init(2U) == 0);
    CHECK(gd32h77x_nor_get_jedec_id(2U, &jedec_id) == 0);
    CHECK(jedec_id == UINT32_C(0x00C84017));
    CHECK(gd32h77x_nor_get_jedec_id(2U, NULL) != 0);
    CHECK(gd32h77x_nor_get_jedec_id(GD32H77X_LAYOUT_DEVICE_COUNT, &jedec_id) != 0);
    CHECK(s_last_instruction == UINT8_C(0x9F));
    CHECK(gd32h77x_nor_init(GD32H77X_LAYOUT_DEVICE_COUNT) != 0);

    CHECK(gd32h77x_nor_read(2U, gd32h77x_layout_devices[2].capacity_bytes - 4U,
                            data, 8U) != 0);
    CHECK(gd32h77x_nor_read(2U, 0U, NULL, 1U) != 0);
    CHECK(gd32h77x_nor_read(2U, 0U, data, sizeof(data)) == 0);
    CHECK(data[0] == 0U && data[299] == 0U);
    CHECK(s_read_calls == 2U && s_last_instruction == UINT8_C(0x03));
    s_busy_forever = 1;
    CHECK(gd32h77x_nor_read(2U, 0U, data, 1U) != 0);
    s_busy_forever = 0;

    CHECK(gd32h77x_nor_program(0U, 0U, pattern, sizeof(pattern)) != 0);
    CHECK(gd32h77x_nor_erase_4k(0U, 0U) != 0);
    CHECK(gd32h77x_nor_erase_4k(2U, 1U) != 0);
    CHECK(gd32h77x_nor_erase_4k(2U, UINT32_C(0x00800000)) != 0);
    CHECK(gd32h77x_nor_program(2U, UINT32_C(0x00800000), pattern,
                                sizeof(pattern)) != 0);
    CHECK(gd32h77x_nor_program(2U, 0U, NULL, 1U) != 0);
    s_reject_wel = 1;
    CHECK(gd32h77x_nor_program(2U, 0U, pattern, sizeof(pattern)) != 0);
    CHECK(s_program_calls == 0U);
    CHECK(gd32h77x_nor_erase_4k(2U, 0U) != 0);
    CHECK(s_erase_calls == 0U);
    s_reject_wel = 0;
    s_protected = 1;
    CHECK(gd32h77x_nor_program(2U, 0U, pattern, sizeof(pattern)) != 0);
    s_protected = 0;

    CHECK(gd32h77x_nor_erase_4k(2U, 0U) == 0);
    CHECK(s_erase_calls == 1U);
    CHECK(gd32h77x_nor_read(2U, 0U, data, 4U) == 0);
    CHECK(data[0] == 0xFFU && data[3] == 0xFFU);
    CHECK(gd32h77x_nor_program(2U, 255U, pattern, sizeof(pattern)) == 0);
    CHECK(s_program_calls == 2U);
    CHECK(s_program_offsets[0] == 255U && s_program_sizes[0] == 1U);
    CHECK(s_program_offsets[1] == 256U && s_program_sizes[1] == 3U);
    CHECK(gd32h77x_nor_read(2U, 255U, data, sizeof(pattern)) == 0);
    CHECK(memcmp(data, pattern, sizeof(pattern)) == 0);

    s_stick_wip_on_write = 1;
    CHECK(gd32h77x_nor_program(2U, 16U, pattern, sizeof(pattern)) != 0);
    CHECK(s_program_calls == 3U);
    s_busy_forever = 0;
    CHECK(gd32h77x_nor_erase_4k(2U, 4096U) != 0);
    return 0;
}
