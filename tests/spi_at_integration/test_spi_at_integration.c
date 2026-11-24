#include "at_spi_protocol.h"
#include "gateway_spi_protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void check_layout(void)
{
    assert(AT_SPI_FRAME_SIZE == VW553_SPI_FRAME_SIZE);
    assert(AT_SPI_HEADER_SIZE == VW553_SPI_HEADER_SIZE);
    assert(AT_SPI_PAYLOAD_SIZE == VW553_SPI_PAYLOAD_SIZE);
    assert(AT_SPI_PAYLOAD_SIZE == 240U);
    assert(at_spi_crc16_ccitt_false((const uint8_t *)"123456789", 9) == 0x29B1U);
    assert(vw553_spi_crc16((const uint8_t *)"123456789", 9) == 0x29B1U);
}

static void check_codecs(void)
{
    uint8_t payload[AT_SPI_PAYLOAD_SIZE];
    uint8_t h_frame[AT_SPI_FRAME_SIZE], v_frame[VW553_SPI_FRAME_SIZE];
    uint8_t decoded[AT_SPI_PAYLOAD_SIZE];
    at_spi_frame_t h_in = {0}, h_out;
    const uint8_t *v_payload;
    uint8_t v_flags;
    uint16_t v_seq, v_ack, v_len;
    size_t i;

    for (i = 0; i < sizeof(payload); ++i) payload[i] = (uint8_t)(i * 37U + 11U);
    payload[0] = 0;
    payload[1] = 0x7E;
    payload[2] = 0x7D;
    h_in.flags = 0;
    h_in.seq = 0x12FE;
    h_in.ack = 0xABCD;
    h_in.length = (uint16_t)sizeof(payload);
    h_in.payload = payload;

    assert(at_spi_frame_encode(h_frame, &h_in));
    assert(vw553_spi_frame_encode(v_frame, 0, h_in.seq, h_in.ack,
                                  payload, h_in.length) == 0);
    assert(memcmp(h_frame, v_frame, sizeof(h_frame)) == 0);
    assert(vw553_spi_frame_decode(h_frame, &v_flags, &v_seq, &v_ack,
                                  &v_payload, &v_len) == 0);
    assert(v_flags == h_in.flags && v_seq == h_in.seq && v_ack == h_in.ack);
    assert(v_len == h_in.length && memcmp(v_payload, payload, v_len) == 0);
    assert(at_spi_frame_decode(v_frame, &h_out, decoded));
    assert(h_out.flags == h_in.flags && h_out.seq == h_in.seq && h_out.ack == h_in.ack);
    assert(h_out.length == h_in.length && memcmp(decoded, payload, sizeof(payload)) == 0);

    /* A valid-CRC snapshot from the opposite CS/READY protocol is rejected. */
    v_frame[2] = AT_SPI_PROTOCOL_VERSION == 1U ? 2U : 1U;
    uint16_t crc = at_spi_crc16_ccitt_false(v_frame, 14U);
    v_frame[14] = (uint8_t)crc;
    v_frame[15] = (uint8_t)(crc >> 8);
    assert(!at_spi_frame_decode(v_frame, &h_out, decoded));
    assert(vw553_spi_frame_decode(v_frame, &v_flags, &v_seq, &v_ack,
                                  &v_payload, &v_len) != 0);
    memcpy(v_frame, h_frame, sizeof(v_frame));

    /* A payload bit error, header bit error, or nonzero pad must be rejected. */
    v_frame[AT_SPI_HEADER_SIZE + 17] ^= 0x80;
    assert(!at_spi_frame_decode(v_frame, &h_out, decoded));
    assert(vw553_spi_frame_decode(v_frame, &v_flags, &v_seq, &v_ack,
                                  &v_payload, &v_len) != 0);
    v_frame[AT_SPI_HEADER_SIZE + 17] ^= 0x80;
    v_frame[6] ^= 0x01;
    assert(!at_spi_frame_decode(v_frame, &h_out, decoded));
    assert(vw553_spi_frame_decode(v_frame, &v_flags, &v_seq, &v_ack,
                                  &v_payload, &v_len) != 0);
    v_frame[6] ^= 0x01;
    h_in.length = 1;
    h_in.payload = payload;
    assert(at_spi_frame_encode(h_frame, &h_in));
    h_frame[AT_SPI_FRAME_SIZE - 1] = 1;
    assert(!at_spi_frame_decode(h_frame, &h_out, decoded));
    assert(vw553_spi_frame_decode(h_frame, &v_flags, &v_seq, &v_ack,
                                  &v_payload, &v_len) != 0);
}

static at_spi_frame_validation_t rejected_reason(uint8_t frame[AT_SPI_FRAME_SIZE])
{
    at_spi_frame_t decoded;
    uint8_t payload[AT_SPI_PAYLOAD_SIZE];
    at_spi_frame_validation_t reason = AT_SPI_FRAME_VALID;
    assert(!at_spi_frame_decode_checked(frame, &decoded, payload, &reason));
    return reason;
}

static void check_validation_reasons(void)
{
    uint8_t valid[AT_SPI_FRAME_SIZE];
    uint8_t mutated[AT_SPI_FRAME_SIZE];
    uint8_t payload[4] = {'A', 'T', '\r', '\n'};
    at_spi_frame_t frame = {.flags = 0U, .seq = 1U, .ack = 2U,
                            .length = sizeof(payload), .payload = payload};
    at_spi_frame_t decoded;
    uint8_t decoded_payload[AT_SPI_PAYLOAD_SIZE];
    at_spi_frame_validation_t reason = AT_SPI_FRAME_INVALID_MAGIC;

    assert(at_spi_frame_encode(valid, &frame));
    assert(at_spi_frame_decode_checked(valid, &decoded, decoded_payload, &reason));
    assert(reason == AT_SPI_FRAME_VALID);

    memcpy(mutated, valid, sizeof(mutated));
    mutated[0] ^= 1U;
    assert(rejected_reason(mutated) == AT_SPI_FRAME_INVALID_MAGIC);

    memcpy(mutated, valid, sizeof(mutated));
    mutated[2] ^= 1U;
    assert(rejected_reason(mutated) == AT_SPI_FRAME_INVALID_VERSION);

    memcpy(mutated, valid, sizeof(mutated));
    mutated[3] = 3U;
    assert(rejected_reason(mutated) == AT_SPI_FRAME_INVALID_FLAGS);

    memcpy(mutated, valid, sizeof(mutated));
    mutated[10] = 1U;
    assert(rejected_reason(mutated) == AT_SPI_FRAME_INVALID_RESERVED);

    memcpy(mutated, valid, sizeof(mutated));
    mutated[4] ^= 1U;
    assert(rejected_reason(mutated) == AT_SPI_FRAME_INVALID_HEADER_CRC);

    memcpy(mutated, valid, sizeof(mutated));
    mutated[8] = (uint8_t)(AT_SPI_PAYLOAD_SIZE + 1U);
    uint16_t crc = at_spi_crc16_ccitt_false(mutated, 14U);
    mutated[14] = (uint8_t)crc;
    mutated[15] = (uint8_t)(crc >> 8);
    assert(rejected_reason(mutated) == AT_SPI_FRAME_INVALID_LENGTH);

    memcpy(mutated, valid, sizeof(mutated));
    mutated[AT_SPI_HEADER_SIZE] ^= 1U;
    assert(rejected_reason(mutated) == AT_SPI_FRAME_INVALID_PAYLOAD_CRC);

    memcpy(mutated, valid, sizeof(mutated));
    mutated[AT_SPI_FRAME_SIZE - 1U] = 1U;
    assert(rejected_reason(mutated) == AT_SPI_FRAME_INVALID_PADDING);
}

static void h779_build(at_spi_link_state_t *link, uint8_t frame[AT_SPI_FRAME_SIZE])
{
    at_spi_link_build_frame(link, frame);
}

static at_spi_link_rx_result_t h779_receive(at_spi_link_state_t *link,
                                             const uint8_t frame[AT_SPI_FRAME_SIZE],
                                             bool accept_payload,
                                             const uint8_t **data, uint16_t *len)
{
    at_spi_frame_t decoded;
    uint8_t payload[AT_SPI_PAYLOAD_SIZE];
    at_spi_link_rx_result_t result;

    if (!at_spi_frame_decode(frame, &decoded, payload)) return AT_SPI_LINK_RX_INVALID;
    result = at_spi_link_process_frame(link, &decoded, accept_payload, data, len);
    /* process_frame returns a pointer into decoded payload; callers consume it
     * synchronously before the local payload buffer goes out of scope. */
    if (result == AT_SPI_LINK_RX_ACCEPTED) {
        static uint8_t accepted[AT_SPI_PAYLOAD_SIZE];
        memcpy(accepted, *data, *len);
        *data = accepted;
    }
    return result;
}

static void vw553_build(vw553_spi_link_t *link, const uint8_t *stream,
                        size_t stream_len, uint8_t frame[VW553_SPI_FRAME_SIZE])
{
    assert(vw553_spi_link_prepare(link, stream, stream_len, frame) == 0);
}

static vw553_spi_rx_result_t vw553_receive(vw553_spi_link_t *link,
                                            const uint8_t frame[VW553_SPI_FRAME_SIZE],
                                            bool accept_payload,
                                            const uint8_t **data, uint16_t *len)
{
    return vw553_spi_link_receive(link, frame, accept_payload, data, len);
}

/* A transaction uses frames prepared before either endpoint consumes MOSI,
 * matching SPI full-duplex snapshot semantics. */
static void establish(at_spi_link_state_t *h, vw553_spi_link_t *v)
{
    at_spi_link_rx_result_t hr;
    vw553_spi_rx_result_t vr;
    uint8_t h_tx[AT_SPI_FRAME_SIZE], v_tx[VW553_SPI_FRAME_SIZE];
    const uint8_t *rx;
    uint16_t rx_len;
    unsigned n;

    at_spi_link_init(h);
    vw553_spi_link_init(v);
    for (n = 0; n < 8 &&
         (h->phase != AT_SPI_LINK_ESTABLISHED || !v->established); ++n) {
        h779_build(h, h_tx);
        vw553_build(v, NULL, 0, v_tx);
        hr = h779_receive(h, v_tx, true, &rx, &rx_len);
        vr = vw553_receive(v, h_tx, true, &rx, &rx_len);
        assert(hr != AT_SPI_LINK_RX_INVALID);
        assert(vr != VW553_SPI_RX_INVALID);
    }
    assert(h->phase == AT_SPI_LINK_ESTABLISHED && v->established);
}

static void check_retransmit_and_deduplicate(void)
{
    at_spi_link_state_t h;
    vw553_spi_link_t v;
    at_spi_link_rx_result_t hr;
    vw553_spi_rx_result_t vr;
    uint8_t msg[] = {0, 'A', 0x7E, 0x7D, 'T'};
    uint8_t h_tx[AT_SPI_FRAME_SIZE], v_tx[VW553_SPI_FRAME_SIZE];
    const uint8_t *rx;
    uint16_t rx_len;

    establish(&h, &v);
    assert(at_spi_link_stage_tx(&h, msg, sizeof(msg)));
    h779_build(&h, h_tx);
    vw553_build(&v, NULL, 0, v_tx);
    vr = vw553_receive(&v, h_tx, true, &rx, &rx_len);
    assert(vr == VW553_SPI_RX_ACCEPTED && rx_len == sizeof(msg));
    assert(memcmp(rx, msg, sizeof(msg)) == 0);

    /* Lose the ACK-bearing return frame and resend the exact same transaction. */
    h779_build(&h, h_tx);
    vw553_build(&v, NULL, 0, v_tx);
    vr = vw553_receive(&v, h_tx, true, &rx, &rx_len);
    assert(vr == VW553_SPI_RX_DUPLICATE && rx_len == 0);
    /* Drop this ACK-bearing MISO snapshot at H779. */
    assert(h.tx_pending);

    /* A later snapshot carries the cumulative ACK and retires exactly one send. */
    h779_build(&h, h_tx);
    vw553_build(&v, NULL, 0, v_tx);
    hr = h779_receive(&h, v_tx, true, &rx, &rx_len);
    assert(hr == AT_SPI_LINK_RX_CONTROL);
    assert(!h.tx_pending && h.tx_next == 1U);
    assert(v.rx_expected == 1U);
}

static void check_fragment_boundaries(size_t size, size_t alignment)
{
    at_spi_link_state_t h;
    vw553_spi_link_t v;
    _Alignas(8) uint8_t storage[4096 + 8];
    uint8_t *source = storage + alignment;
    uint8_t h_tx[AT_SPI_FRAME_SIZE], v_tx[VW553_SPI_FRAME_SIZE];
    const uint8_t *rx;
    uint16_t rx_len;
    at_spi_link_rx_result_t hr;
    vw553_spi_rx_result_t vr;
    size_t offset = 0;
    unsigned accepted = 0;
    size_t i;

    assert(size > 0 && size <= 4096U && alignment < 8U);
    for (i = 0; i < 4096U; ++i) source[i] = (uint8_t)(i * 13U);
    source[0] = 0;
    source[239] = 0;
    source[240] = 0;
    source[480] = 0;
    establish(&h, &v);
    while (offset < size) {
        size_t chunk = size - offset;
        if (chunk > AT_SPI_PAYLOAD_SIZE) chunk = AT_SPI_PAYLOAD_SIZE;
        assert(at_spi_link_stage_tx(&h, source + offset, (uint16_t)chunk));
        h779_build(&h, h_tx);
        vw553_build(&v, NULL, 0, v_tx);
        vr = vw553_receive(&v, h_tx, true, &rx, &rx_len);
        assert(vr == VW553_SPI_RX_ACCEPTED && rx_len == chunk);
        assert(memcmp(rx, source + offset, chunk) == 0);
        offset += chunk;
        ++accepted;
        /* Clock the current ACK back; next loop iteration may then stage. */
        h779_build(&h, h_tx);
        vw553_build(&v, NULL, 0, v_tx);
        (void)h779_receive(&h, v_tx, true, &rx, &rx_len);
        (void)vw553_receive(&v, h_tx, true, &rx, &rx_len);
        h779_build(&h, h_tx);
        vw553_build(&v, NULL, 0, v_tx);
        hr = h779_receive(&h, v_tx, true, &rx, &rx_len);
        (void)vw553_receive(&v, h_tx, true, &rx, &rx_len);
        assert(hr == AT_SPI_LINK_RX_CONTROL);
        assert(!h.tx_pending);
    }
    assert(accepted == (size + AT_SPI_PAYLOAD_SIZE - 1U) / AT_SPI_PAYLOAD_SIZE);
}

static void check_vw553_to_h779_fragments(size_t size, size_t alignment)
{
    at_spi_link_state_t h;
    vw553_spi_link_t v;
    _Alignas(8) uint8_t storage[4096 + 8];
    uint8_t *source = storage + alignment;
    uint8_t h_tx[AT_SPI_FRAME_SIZE], v_tx[VW553_SPI_FRAME_SIZE];
    const uint8_t *rx;
    uint16_t rx_len;
    at_spi_link_rx_result_t hr;
    vw553_spi_rx_result_t vr;
    size_t offset = 0;
    unsigned accepted = 0;
    size_t i;

    assert(size > 0 && size <= 4096U && alignment < 8U);
    for (i = 0; i < 4096U; ++i) source[i] = (uint8_t)(255U - i * 7U);
    source[0] = 0;
    source[239] = 0;
    source[240] = 0;
    source[480] = 0;
    establish(&h, &v);
    while (offset < size) {
        size_t chunk = size - offset;
        if (chunk > VW553_SPI_PAYLOAD_SIZE) chunk = VW553_SPI_PAYLOAD_SIZE;
        h779_build(&h, h_tx);
        vw553_build(&v, source + offset, chunk, v_tx);
        hr = h779_receive(&h, v_tx, true, &rx, &rx_len);
        assert(hr == AT_SPI_LINK_RX_ACCEPTED && rx_len == chunk);
        assert(memcmp(rx, source + offset, chunk) == 0);
        (void)vw553_receive(&v, h_tx, true, &rx, &rx_len);
        offset += chunk;
        ++accepted;

        /* H779's cumulative ACK is visible in the next preloaded snapshot. */
        h779_build(&h, h_tx);
        vw553_build(&v, NULL, 0, v_tx);
        hr = h779_receive(&h, v_tx, true, &rx, &rx_len);
        vr = vw553_receive(&v, h_tx, true, &rx, &rx_len);
        assert(hr == AT_SPI_LINK_RX_DUPLICATE && rx_len == 0);
        assert(vr == VW553_SPI_RX_CONTROL);
    }
    assert(accepted == (size + VW553_SPI_PAYLOAD_SIZE - 1U) / VW553_SPI_PAYLOAD_SIZE);
    assert(!v.pending_valid && h.rx_next == accepted);
}

static void check_lost_initial_hello_converges(void)
{
    at_spi_link_state_t h;
    vw553_spi_link_t v;
    uint8_t h_tx[AT_SPI_FRAME_SIZE], v_tx[VW553_SPI_FRAME_SIZE];
    const uint8_t *rx;
    uint16_t rx_len;
    unsigned n;

    at_spi_link_init(&h);
    vw553_spi_link_init(&v);
    h779_build(&h, h_tx);
    vw553_build(&v, NULL, 0, v_tx);
    h_tx[0] ^= 1U; /* Drop/corrupt only H779's first HELLO. */
    assert(vw553_receive(&v, h_tx, true, &rx, &rx_len) == VW553_SPI_RX_INVALID);
    assert(h779_receive(&h, v_tx, true, &rx, &rx_len) == AT_SPI_LINK_RX_CONTROL);

    for (n = 0; n < 8 &&
         (h.phase != AT_SPI_LINK_ESTABLISHED || !v.established); ++n) {
        h779_build(&h, h_tx);
        vw553_build(&v, NULL, 0, v_tx);
        (void)h779_receive(&h, v_tx, true, &rx, &rx_len);
        (void)vw553_receive(&v, h_tx, true, &rx, &rx_len);
    }
    assert(h.phase == AT_SPI_LINK_ESTABLISHED && v.established);
}

static void check_sequence_wrap(void)
{
    at_spi_link_state_t h;
    vw553_spi_link_t v;
    uint8_t byte = 0x00;
    uint8_t h_tx[AT_SPI_FRAME_SIZE], v_tx[VW553_SPI_FRAME_SIZE];
    const uint8_t *rx;
    uint16_t rx_len;
    at_spi_link_rx_result_t hr;
    vw553_spi_rx_result_t vr;

    establish(&h, &v);
    h.tx_next = UINT16_MAX;
    v.rx_expected = UINT16_MAX;
    assert(at_spi_link_stage_tx(&h, &byte, 1));
    h779_build(&h, h_tx);
    vw553_build(&v, NULL, 0, v_tx);
    vr = vw553_receive(&v, h_tx, true, &rx, &rx_len);
    assert(vr == VW553_SPI_RX_ACCEPTED && rx_len == 1 && rx[0] == 0);
    assert(v.rx_expected == 0);

    h779_build(&h, h_tx);
    vw553_build(&v, NULL, 0, v_tx);
    hr = h779_receive(&h, v_tx, true, &rx, &rx_len);
    (void)vw553_receive(&v, h_tx, true, &rx, &rx_len);
    assert(hr == AT_SPI_LINK_RX_CONTROL);
    h779_build(&h, h_tx);
    vw553_build(&v, NULL, 0, v_tx);
    hr = h779_receive(&h, v_tx, true, &rx, &rx_len);
    (void)vw553_receive(&v, h_tx, true, &rx, &rx_len);
    assert(hr == AT_SPI_LINK_RX_CONTROL);
    assert(!h.tx_pending && h.tx_next == 0);
}

static void check_backpressure_still_processes_reverse_ack(void)
{
    at_spi_link_state_t h;
    vw553_spi_link_t v;
    uint8_t h_data[] = {'H', 0, '7'};
    uint8_t v_data[] = {'V', 0, '5'};
    uint8_t h_tx[AT_SPI_FRAME_SIZE], v_tx[VW553_SPI_FRAME_SIZE];
    const uint8_t *rx;
    uint16_t rx_len;
    at_spi_link_rx_result_t hr;
    vw553_spi_rx_result_t vr;

    establish(&h, &v);
    assert(at_spi_link_stage_tx(&h, h_data, sizeof(h_data)));

    /* VW553 accepts H779's data; its cumulative ACK is in the next frame. */
    h779_build(&h, h_tx);
    vw553_build(&v, NULL, 0, v_tx);
    vr = vw553_receive(&v, h_tx, true, &rx, &rx_len);
    assert(vr == VW553_SPI_RX_ACCEPTED && rx_len == sizeof(h_data));
    hr = h779_receive(&h, v_tx, true, &rx, &rx_len);
    assert(hr == AT_SPI_LINK_RX_CONTROL && h.tx_pending);
    assert(vw553_spi_link_prepare(&v, v_data, sizeof(v_data), v_tx) == 0);

    /* H779's RX is full, but the same frame still ACKs H779's outbound data. */
    h779_build(&h, h_tx);
    hr = h779_receive(&h, v_tx, false, &rx, &rx_len);
    assert(hr == AT_SPI_LINK_RX_BACKPRESSURE);
    assert(h.tx_next == 1U && !h.tx_pending);
    assert(h.rx_next == 0U && rx_len == 0U);
    vr = vw553_receive(&v, h_tx, true, &rx, &rx_len);
    assert(vr == VW553_SPI_RX_DUPLICATE);
    assert(v.rx_expected == 1U && v.pending_valid);

    /* Once capacity returns, the retransmitted binary payload is accepted once. */
    h779_build(&h, h_tx);
    vw553_build(&v, NULL, 0, v_tx);
    hr = h779_receive(&h, v_tx, true, &rx, &rx_len);
    assert(hr == AT_SPI_LINK_RX_ACCEPTED && rx_len == sizeof(v_data));
    assert(memcmp(rx, v_data, sizeof(v_data)) == 0 && h.rx_next == 1U);
    (void)vw553_receive(&v, h_tx, true, &rx, &rx_len);

    h779_build(&h, h_tx);
    vw553_build(&v, NULL, 0, v_tx);
    hr = h779_receive(&h, v_tx, true, &rx, &rx_len);
    vr = vw553_receive(&v, h_tx, true, &rx, &rx_len);
    assert(hr == AT_SPI_LINK_RX_DUPLICATE && rx_len == 0U);
    assert(vr == VW553_SPI_RX_CONTROL && !v.pending_valid && v.tx_next == 1U);
    assert(h.rx_next == 1U);
}

static void check_vw553_backpressure_still_processes_ack(void)
{
    at_spi_link_state_t h;
    vw553_spi_link_t v;
    uint8_t v_first[] = {'V', '0'};
    uint8_t v_second[] = {'V', 0, '1'};
    uint8_t h_data[] = {'H', 0, '0'};
    uint8_t h_tx[AT_SPI_FRAME_SIZE], v_tx[VW553_SPI_FRAME_SIZE];
    const uint8_t *rx;
    uint16_t rx_len;
    at_spi_link_rx_result_t hr;
    vw553_spi_rx_result_t vr;

    establish(&h, &v);
    assert(vw553_spi_link_prepare(&v, v_first, sizeof(v_first), v_tx) == 0);
    h779_build(&h, h_tx);
    hr = h779_receive(&h, v_tx, true, &rx, &rx_len);
    assert(hr == AT_SPI_LINK_RX_ACCEPTED && rx_len == sizeof(v_first));
    (void)vw553_receive(&v, h_tx, true, &rx, &rx_len);

    /* Return H779's ACK so VW553 can stage the next sequence. */
    h779_build(&h, h_tx);
    vw553_build(&v, NULL, 0, v_tx);
    (void)h779_receive(&h, v_tx, true, &rx, &rx_len);
    (void)vw553_receive(&v, h_tx, true, &rx, &rx_len);
    assert(v.tx_next == 1U && !v.pending_valid);

    assert(vw553_spi_link_prepare(&v, v_second, sizeof(v_second), v_tx) == 0);
    h779_build(&h, h_tx);
    hr = h779_receive(&h, v_tx, true, &rx, &rx_len);
    assert(hr == AT_SPI_LINK_RX_ACCEPTED && rx_len == sizeof(v_second));
    assert(memcmp(rx, v_second, sizeof(v_second)) == 0 && h.rx_next == 2U);
    (void)vw553_receive(&v, h_tx, true, &rx, &rx_len);

    assert(at_spi_link_stage_tx(&h, h_data, sizeof(h_data)));
    h779_build(&h, h_tx); /* ACK=2 for VW553's outstanding v_second */
    vw553_build(&v, NULL, 0, v_tx);
    vr = vw553_receive(&v, h_tx, false, &rx, &rx_len);
    assert(vr == VW553_SPI_RX_CONTROL && rx_len == 0U);
    assert(v.tx_next == 2U && !v.pending_valid);
    assert(v.rx_expected == 0U); /* Full RX: do not consume H779's data seq. */
    hr = h779_receive(&h, v_tx, true, &rx, &rx_len);
    assert(hr == AT_SPI_LINK_RX_DUPLICATE);

    /* VW553's ACK still reached the sender; H779 keeps retrying its payload. */
    h779_build(&h, h_tx);
    vw553_build(&v, NULL, 0, v_tx);
    vr = vw553_receive(&v, h_tx, true, &rx, &rx_len);
    assert(vr == VW553_SPI_RX_ACCEPTED && rx_len == sizeof(h_data));
    assert(memcmp(rx, h_data, sizeof(h_data)) == 0 && v.rx_expected == 1U);
}

static void check_peer_restart_drops_old_inflight(void)
{
    at_spi_link_state_t h;
    vw553_spi_link_t v;
    uint8_t old_data[] = {'O', 'L', 'D', 0, 'A', 'T'};
    uint8_t h_tx[AT_SPI_FRAME_SIZE], v_tx[VW553_SPI_FRAME_SIZE];
    const uint8_t *rx;
    uint16_t rx_len;
    at_spi_link_rx_result_t hr;
    vw553_spi_rx_result_t vr;
    unsigned n;

    establish(&h, &v);
    assert(at_spi_link_stage_tx(&h, old_data, sizeof(old_data)));
    /* VW553 restarts; H779's already prepared normal snapshot is now stale. */
    vw553_spi_link_init(&v);
    h779_build(&h, h_tx);
    vw553_build(&v, NULL, 0, v_tx);
    hr = h779_receive(&h, v_tx, true, &rx, &rx_len);
    vr = vw553_receive(&v, h_tx, true, &rx, &rx_len);
    assert(hr == AT_SPI_LINK_RX_RESET);
    assert(vr == VW553_SPI_RX_CONTROL && rx_len == 0);
    assert(h.phase == AT_SPI_LINK_HANDSHAKE_ACK && !h.tx_pending);

    for (n = 0; n < 8 &&
         (h.phase != AT_SPI_LINK_ESTABLISHED || !v.established); ++n) {
        h779_build(&h, h_tx);
        vw553_build(&v, NULL, 0, v_tx);
        hr = h779_receive(&h, v_tx, true, &rx, &rx_len);
        vr = vw553_receive(&v, h_tx, true, &rx, &rx_len);
        assert(hr != AT_SPI_LINK_RX_ACCEPTED);
        assert(vr != VW553_SPI_RX_ACCEPTED);
    }
    assert(h.phase == AT_SPI_LINK_ESTABLISHED && v.established);

    /* The reset discarded H779's pending bytes; recovery never resends them. */
    assert(!h.tx_pending);
    assert(v.rx_expected == 0);
}

int main(void)
{
    check_layout();
    check_codecs();
    check_validation_reasons();
    check_retransmit_and_deduplicate();
    static const size_t lengths[] = {
        1,2,3,4,5,7,8,9,15,16,17,31,32,33,63,64,65,127,128,129,
        239,240,241,255,256,257,479,480,481,1023,1024,1025,
        2003,2004,2005,2017,2023,4095,4096
    };
    for (size_t alignment = 0; alignment < 8; ++alignment) {
        for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
            check_fragment_boundaries(lengths[i], alignment);
            check_vw553_to_h779_fragments(lengths[i], alignment);
        }
    }
    check_lost_initial_hello_converges();
    check_sequence_wrap();
    check_backpressure_still_processes_reverse_ack();
    check_vw553_backpressure_still_processes_ack();
    check_peer_restart_drops_old_inflight();
    puts("SPI AT cross-end protocol checks passed");
    return 0;
}
