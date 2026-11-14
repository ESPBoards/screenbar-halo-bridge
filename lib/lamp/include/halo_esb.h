/* SPDX-License-Identifier: GPL-3.0-only */
/* ESB frames for the ScreenBar Halo (1st gen). Kept free of ESP-IDF so it can be
 * tested on a PC.
 *
 *   preamble 8 | address 32 | len 6 | pid 2 | no_ack 1 | payload 8*len | crc 16
 *
 * Bit arrays are one bit per byte because that's what the modem works with.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HALO_ESB_MAX_PAYLOAD 2u
#define HALO_ESB_MAX_BITS (8u + 32u + 9u + 8u * HALO_ESB_MAX_PAYLOAD + 16u)

typedef struct {
    uint32_t address;   /* on-air byte order */
    uint8_t pid;
    bool no_ack;
    uint8_t length;     /* 2 for a command, 0 for the lamp's ACK */
    uint8_t payload[HALO_ESB_MAX_PAYLOAD];
} halo_esb_frame_t;

/* Bitwise because the 9-bit control field breaks byte alignment. */
uint16_t halo_esb_crc16(const uint8_t *bits, size_t count);

size_t halo_esb_frame_bits(uint8_t length);

/* Returns 0 for a frame the lamp would never accept. */
size_t halo_esb_encode(const halo_esb_frame_t *frame, uint8_t bits[HALO_ESB_MAX_BITS]);

/* bits starts at the address: the receiver has already locked onto the preamble. */
bool halo_esb_decode(const uint8_t *bits, size_t count, halo_esb_frame_t *frame);

/* The strict ACK rule: our address, our PID, empty, NO_ACK set. Anything else
 * means we don't know whether the lamp got the command.
 */
bool halo_esb_is_ack(const halo_esb_frame_t *frame, uint32_t address, uint8_t pid);

/* A remote's command, from any address: the only frames that carry lamp state. */
bool halo_esb_is_command(const halo_esb_frame_t *frame);

#ifdef __cplusplus
}
#endif
