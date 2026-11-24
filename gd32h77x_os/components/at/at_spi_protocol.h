/**
 * @file at_spi_protocol.h
 * @brief Wire format for the GD32H77X <-> GD32VW553 SPI AT link.
 */
#ifndef AT_SPI_PROTOCOL_H
#define AT_SPI_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AT_SPI_FRAME_SIZE 256U
#define AT_SPI_HEADER_SIZE 16U
#define AT_SPI_PAYLOAD_SIZE (AT_SPI_FRAME_SIZE - AT_SPI_HEADER_SIZE)
#ifndef AT_SPI_CS_ONLY
#define AT_SPI_CS_ONLY 0
#endif
#if AT_SPI_CS_ONLY
#define AT_SPI_PROTOCOL_VERSION 2U
#else
#define AT_SPI_PROTOCOL_VERSION 1U
#endif
#define AT_SPI_FLAG_HELLO 0x01U
#define AT_SPI_FLAG_HELLO_ACK 0x02U

typedef enum {
    AT_SPI_LINK_START = 0,
    AT_SPI_LINK_HANDSHAKE_ACK,
    AT_SPI_LINK_ESTABLISHED,
} at_spi_link_phase_t;

typedef struct {
    uint8_t flags;
    uint16_t seq;
    uint16_t ack;
    uint16_t length;
    const uint8_t *payload;
} at_spi_frame_t;

typedef struct {
    at_spi_link_phase_t phase;
    bool tx_pending;
    uint16_t tx_next;
    uint16_t tx_seq;
    uint16_t rx_next;
    uint16_t tx_length;
    uint8_t tx_payload[AT_SPI_PAYLOAD_SIZE];
} at_spi_link_state_t;

typedef enum {
    AT_SPI_LINK_RX_INVALID = 0,
    AT_SPI_LINK_RX_CONTROL,
    AT_SPI_LINK_RX_ACCEPTED,
    AT_SPI_LINK_RX_DUPLICATE,
    AT_SPI_LINK_RX_BACKPRESSURE,
    AT_SPI_LINK_RX_RESET,
} at_spi_link_rx_result_t;

typedef enum {
    AT_SPI_FRAME_VALID = 0,
    AT_SPI_FRAME_INVALID_MAGIC,
    AT_SPI_FRAME_INVALID_VERSION,
    AT_SPI_FRAME_INVALID_FLAGS,
    AT_SPI_FRAME_INVALID_RESERVED,
    AT_SPI_FRAME_INVALID_HEADER_CRC,
    AT_SPI_FRAME_INVALID_LENGTH,
    AT_SPI_FRAME_INVALID_PAYLOAD_CRC,
    AT_SPI_FRAME_INVALID_PADDING,
} at_spi_frame_validation_t;

/* Encodes one complete fixed-size frame. Unused payload bytes are zeroed. */
bool at_spi_frame_encode(uint8_t output[AT_SPI_FRAME_SIZE],
                         const at_spi_frame_t *frame);

/* Rejects invalid headers, CRCs, flags, lengths, or nonzero padding. */
bool at_spi_frame_decode(const uint8_t input[AT_SPI_FRAME_SIZE],
                         at_spi_frame_t *frame,
                         uint8_t payload[AT_SPI_PAYLOAD_SIZE]);
/* Decode with a stable reason code for transport diagnostics. */
bool at_spi_frame_decode_checked(const uint8_t input[AT_SPI_FRAME_SIZE],
                                 at_spi_frame_t *frame,
                                 uint8_t payload[AT_SPI_PAYLOAD_SIZE],
                                 at_spi_frame_validation_t *validation);

uint16_t at_spi_crc16_ccitt_false(const uint8_t *data, size_t length);

/* Pure stop-and-wait state helpers; safe to compile in host tests. */
void at_spi_link_init(at_spi_link_state_t *link);
bool at_spi_link_stage_tx(at_spi_link_state_t *link, const uint8_t *data,
                          uint16_t length);
void at_spi_link_build_frame(const at_spi_link_state_t *link,
                             uint8_t output[AT_SPI_FRAME_SIZE]);
at_spi_link_rx_result_t at_spi_link_process_frame(at_spi_link_state_t *link,
                                                  const at_spi_frame_t *frame,
                                                  bool accept_payload,
                                                  const uint8_t **rx_data,
                                                  uint16_t *rx_length);

#endif /* AT_SPI_PROTOCOL_H */
