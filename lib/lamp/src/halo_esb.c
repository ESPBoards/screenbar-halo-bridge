/* SPDX-License-Identifier: GPL-3.0-only */
#include "halo_esb.h"

#define PREAMBLE_BITS 8u
#define ADDRESS_BITS  32u
#define LENGTH_BITS   6u
#define PID_BITS      2u
#define NO_ACK_BITS   1u
#define PCF_BITS      (LENGTH_BITS + PID_BITS + NO_ACK_BITS)
#define CRC_BITS      16u

static void put_bits(uint8_t *bits, size_t *n, uint32_t value, unsigned width)
{
    while (width--) {
        bits[(*n)++] = (value >> width) & 1u;
    }
}

static uint32_t get_bits(const uint8_t *bits, size_t *n, unsigned width)
{
    uint32_t value = 0;
    while (width--) {
        value = (value << 1) | (bits[(*n)++] & 1u);
    }
    return value;
}

uint16_t halo_esb_crc16(const uint8_t *bits, size_t count)
{
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < count; i++) {
        bool feedback = ((crc >> 15) ^ bits[i]) & 1u;
        crc = (uint16_t)(crc << 1);
        if (feedback) {
            crc ^= 0x1021;
        }
    }
    return crc;
}

size_t halo_esb_frame_bits(uint8_t length)
{
    return PREAMBLE_BITS + ADDRESS_BITS + PCF_BITS + 8u * length + CRC_BITS;
}

size_t halo_esb_encode(const halo_esb_frame_t *frame, uint8_t bits[HALO_ESB_MAX_BITS])
{
    if (frame->pid > 3 || (frame->length != 0 && frame->length != 2)) {
        return 0;
    }
    size_t n = 0;
    /* ESB wants the preamble to end on the opposite bit to the address. */
    put_bits(bits, &n, (frame->address >> 31) ? 0xaa : 0x55, PREAMBLE_BITS);
    put_bits(bits, &n, frame->address, ADDRESS_BITS);
    put_bits(bits, &n, frame->length, LENGTH_BITS);
    put_bits(bits, &n, frame->pid, PID_BITS);
    put_bits(bits, &n, frame->no_ack, NO_ACK_BITS);
    for (unsigned i = 0; i < frame->length; i++) {
        put_bits(bits, &n, frame->payload[i], 8);
    }
    put_bits(bits, &n, halo_esb_crc16(bits + PREAMBLE_BITS, n - PREAMBLE_BITS), CRC_BITS);
    return n;
}

bool halo_esb_decode(const uint8_t *bits, size_t count, halo_esb_frame_t *frame)
{
    if (count < ADDRESS_BITS + PCF_BITS) {
        return false;
    }
    size_t n = 0;
    halo_esb_frame_t f = {0};
    f.address = get_bits(bits, &n, ADDRESS_BITS);
    f.length = (uint8_t)get_bits(bits, &n, LENGTH_BITS);
    f.pid = (uint8_t)get_bits(bits, &n, PID_BITS);
    f.no_ack = get_bits(bits, &n, NO_ACK_BITS);
    if (f.length != 0 && f.length != 2) {
        return false;
    }
    if (count < n + 8u * f.length + CRC_BITS) {
        return false;
    }
    for (unsigned i = 0; i < f.length; i++) {
        f.payload[i] = (uint8_t)get_bits(bits, &n, 8);
    }
    uint16_t expected = halo_esb_crc16(bits, n);
    if (get_bits(bits, &n, CRC_BITS) != expected) {
        return false;
    }
    *frame = f;
    return true;
}

bool halo_esb_is_ack(const halo_esb_frame_t *frame, uint32_t address, uint8_t pid)
{
    return frame->address == address && frame->pid == pid && frame->length == 0 && frame->no_ack;
}

bool halo_esb_is_command(const halo_esb_frame_t *frame)
{
    return frame->length == 2 && !frame->no_ack;
}
