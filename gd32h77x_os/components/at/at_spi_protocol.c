/**
 * @file at_spi_protocol.c
 * @brief Portable codec for the fixed-size SPI AT transport frame.
 */
#include "at_spi_protocol.h"

#include <string.h>

#define AT_SPI_MAGIC_0 0xA7U
#define AT_SPI_MAGIC_1 0x5CU
#define AT_SPI_HEADER_CRC_OFFSET 14U
#define AT_SPI_PAYLOAD_CRC_OFFSET 12U

static uint16_t get_u16_le(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static void put_u16_le(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

uint16_t at_spi_crc16_ccitt_false(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFFU;

    if (data == NULL && length != 0U) return 0U;
    for (size_t i = 0U; i < length; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) :
                                    (uint16_t)(crc << 1);
        }
    }
    return crc;
}

void at_spi_link_init(at_spi_link_state_t *link)
{
    if (link == NULL) return;
    memset(link, 0, sizeof(*link));
    link->phase = AT_SPI_LINK_START;
}

bool at_spi_link_stage_tx(at_spi_link_state_t *link, const uint8_t *data,
                          uint16_t length)
{
    if (link == NULL || data == NULL || length == 0U ||
        length > AT_SPI_PAYLOAD_SIZE || link->phase != AT_SPI_LINK_ESTABLISHED ||
        link->tx_pending) {
        return false;
    }
    memcpy(link->tx_payload, data, length);
    link->tx_length = length;
    link->tx_seq = link->tx_next;
    link->tx_pending = true;
    return true;
}

void at_spi_link_build_frame(const at_spi_link_state_t *link,
                             uint8_t output[AT_SPI_FRAME_SIZE])
{
    at_spi_frame_t frame = {0};

    if (link == NULL || output == NULL) return;
    if (link->phase == AT_SPI_LINK_START) {
        frame.flags = AT_SPI_FLAG_HELLO;
    } else if (link->phase == AT_SPI_LINK_HANDSHAKE_ACK) {
        frame.flags = AT_SPI_FLAG_HELLO_ACK;
    } else if (link->tx_pending) {
        frame.seq = link->tx_seq;
        frame.length = link->tx_length;
        frame.payload = link->tx_payload;
    } else {
        frame.seq = link->tx_next;
    }
    frame.ack = link->rx_next;
    (void)at_spi_frame_encode(output, &frame);
}

at_spi_link_rx_result_t at_spi_link_process_frame(at_spi_link_state_t *link,
                                                  const at_spi_frame_t *frame,
                                                  bool accept_payload,
                                                  const uint8_t **rx_data,
                                                  uint16_t *rx_length)
{
    if (rx_data) *rx_data = NULL;
    if (rx_length) *rx_length = 0U;
    if (link == NULL || frame == NULL || rx_data == NULL || rx_length == NULL) {
        return AT_SPI_LINK_RX_INVALID;
    }

    if (frame->flags == AT_SPI_FLAG_HELLO) {
        const bool peer_restarted = link->phase == AT_SPI_LINK_ESTABLISHED;
        if (peer_restarted) {
            link->tx_pending = false;
            link->tx_length = 0U;
            link->tx_next = 0U;
            link->tx_seq = 0U;
            link->rx_next = 0U;
        }
        if (link->phase != AT_SPI_LINK_HANDSHAKE_ACK) {
            link->phase = AT_SPI_LINK_HANDSHAKE_ACK;
        }
        return peer_restarted ? AT_SPI_LINK_RX_RESET : AT_SPI_LINK_RX_CONTROL;
    }

    if (frame->flags == AT_SPI_FLAG_HELLO_ACK) {
        if (link->phase != AT_SPI_LINK_ESTABLISHED) {
            link->phase = AT_SPI_LINK_ESTABLISHED;
        }
        return AT_SPI_LINK_RX_CONTROL;
    }
    if (frame->flags != 0U) return AT_SPI_LINK_RX_INVALID;
    if (link->phase == AT_SPI_LINK_START) return AT_SPI_LINK_RX_INVALID;
    if (link->phase == AT_SPI_LINK_HANDSHAKE_ACK) {
        link->phase = AT_SPI_LINK_ESTABLISHED;
    }

    if (link->tx_pending && frame->ack == (uint16_t)(link->tx_seq + 1U)) {
        link->tx_next = frame->ack;
        link->tx_pending = false;
        link->tx_length = 0U;
    }

    if (frame->length == 0U) return AT_SPI_LINK_RX_CONTROL;
    if (frame->seq == link->rx_next) {
        if (!accept_payload) return AT_SPI_LINK_RX_BACKPRESSURE;
        *rx_data = frame->payload;
        *rx_length = frame->length;
        link->rx_next = (uint16_t)(link->rx_next + 1U);
        return AT_SPI_LINK_RX_ACCEPTED;
    }
    if (frame->seq == (uint16_t)(link->rx_next - 1U)) {
        return AT_SPI_LINK_RX_DUPLICATE;
    }
    return AT_SPI_LINK_RX_INVALID;
}

bool at_spi_frame_encode(uint8_t output[AT_SPI_FRAME_SIZE],
                         const at_spi_frame_t *frame)
{
    if (output == NULL || frame == NULL || frame->length > AT_SPI_PAYLOAD_SIZE ||
        (frame->length != 0U && frame->payload == NULL) ||
        (frame->flags & (uint8_t)~(AT_SPI_FLAG_HELLO | AT_SPI_FLAG_HELLO_ACK)) != 0U ||
        frame->flags == (AT_SPI_FLAG_HELLO | AT_SPI_FLAG_HELLO_ACK) ||
        (frame->flags != 0U && frame->length != 0U)) {
        return false;
    }

    memset(output, 0, AT_SPI_FRAME_SIZE);
    output[0] = AT_SPI_MAGIC_0;
    output[1] = AT_SPI_MAGIC_1;
    output[2] = AT_SPI_PROTOCOL_VERSION;
    output[3] = frame->flags;
    put_u16_le(&output[4], frame->seq);
    put_u16_le(&output[6], frame->ack);
    put_u16_le(&output[8], frame->length);
    if (frame->length != 0U) {
        memcpy(&output[AT_SPI_HEADER_SIZE], frame->payload, frame->length);
    }
    put_u16_le(&output[AT_SPI_PAYLOAD_CRC_OFFSET],
               at_spi_crc16_ccitt_false(&output[AT_SPI_HEADER_SIZE], frame->length));
    put_u16_le(&output[AT_SPI_HEADER_CRC_OFFSET],
               at_spi_crc16_ccitt_false(output, AT_SPI_HEADER_CRC_OFFSET));
    return true;
}

bool at_spi_frame_decode(const uint8_t input[AT_SPI_FRAME_SIZE],
                         at_spi_frame_t *frame,
                         uint8_t payload[AT_SPI_PAYLOAD_SIZE])
{
    uint16_t length;

    if (input == NULL || frame == NULL || payload == NULL ||
        input[0] != AT_SPI_MAGIC_0 || input[1] != AT_SPI_MAGIC_1 ||
        input[2] != AT_SPI_PROTOCOL_VERSION ||
        (input[3] & (uint8_t)~(AT_SPI_FLAG_HELLO | AT_SPI_FLAG_HELLO_ACK)) != 0U ||
        input[3] == (AT_SPI_FLAG_HELLO | AT_SPI_FLAG_HELLO_ACK) ||
        input[10] != 0U || input[11] != 0U ||
        get_u16_le(&input[AT_SPI_HEADER_CRC_OFFSET]) !=
            at_spi_crc16_ccitt_false(input, AT_SPI_HEADER_CRC_OFFSET)) {
        return false;
    }

    length = get_u16_le(&input[8]);
    if (length > AT_SPI_PAYLOAD_SIZE ||
        (input[3] != 0U && length != 0U) ||
        get_u16_le(&input[AT_SPI_PAYLOAD_CRC_OFFSET]) !=
            at_spi_crc16_ccitt_false(&input[AT_SPI_HEADER_SIZE], length)) {
        return false;
    }
    for (size_t i = AT_SPI_HEADER_SIZE + length; i < AT_SPI_FRAME_SIZE; ++i) {
        if (input[i] != 0U) return false;
    }

    frame->flags = input[3];
    frame->seq = get_u16_le(&input[4]);
    frame->ack = get_u16_le(&input[6]);
    frame->length = length;
    if (length != 0U) memcpy(payload, &input[AT_SPI_HEADER_SIZE], length);
    frame->payload = payload;
    return true;
}
