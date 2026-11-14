/* SPDX-License-Identifier: GPL-3.0-only */
/* Finds Halo frames in a demodulated capture. One detector for both jobs: the
 * ACK after our own command, and anything the remote sends while we listen.
 *
 * The preamble is 8 alternating bits, so its average is the carrier offset.
 * Slicing against that keeps every transmitter centred, whatever its crystal does.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "halo_esb.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    halo_esb_frame_t frame;
    float offset_hz;         /* measured on this frame's preamble */
    uint32_t start_us;       /* preamble start, from the beginning of the capture */
    uint32_t start_sample;   /* the same as an index into hz[]: exact, where start_us is rounded */
    uint8_t weak_bits;       /* bits that sliced close to the threshold */
} halo_detection_t;

/* Every CRC-valid frame in hz[] (from halo_fm_demod), earliest first. */
size_t halo_detect(const float *hz, size_t n, halo_detection_t *out, size_t max);

/* The first one that is our ACK, by halo_esb_is_ack. */
const halo_detection_t *halo_detect_find_ack(const halo_detection_t *found, size_t count, uint32_t address,
                                             uint8_t pid);

#ifdef __cplusplus
}
#endif
