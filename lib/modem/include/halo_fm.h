/* SPDX-License-Identifier: GPL-3.0-only */
/* FM demodulator for raw S3 ADC captures (16 MS/s).
 *
 * Decimates to 2 MS/s first: 16 samples per bit is plenty for slicing and
 * keeps the per-capture cost down, which decides how often the listener hears.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HALO_FM_IN_RATE    16000000u
#define HALO_FM_DECIMATION 8u
#define HALO_FM_RATE       (HALO_FM_IN_RATE / HALO_FM_DECIMATION)
#define HALO_FM_SPB        (HALO_FM_RATE / 125000u)
#define HALO_FM_MAX_BLOCKS 2048u   /* one 16380-sample capture; longer input is cut */
#define HALO_FM_HZ_PER_RAD ((float)HALO_FM_RATE / 6.28318531f)   /* phase step per sample -> frequency */

/* Instantaneous frequency in Hz, already corrected for the S3's inverted IQ.
 * Sample n sits about 8 * (n + 1) input samples into the capture.
 * Returns the number written: count / 8 - 1, at most max.
 * Its block sums live in file-static scratch, so only one caller at a time.
 */
size_t halo_fm_demod(const uint32_t *raw, size_t count, float *hz, size_t max);

/* The same in two stages. Block sums can be taken piece by piece (halo_blocks
 * keeps the listener's in a ring); pieces must be multiples of 8 samples to
 * keep the blocks aligned. Then demod from a plain array or from that ring.
 */
size_t halo_fm_blocks(const uint32_t *raw, size_t count, int16_t *bi, int16_t *bq);
size_t halo_fm_demod_blocks(const int16_t *bi, const int16_t *bq, size_t blocks, float *hz, size_t max);

/* Same, reading blocks first.. from a ring: index & mask (size a power of two). */
size_t halo_fm_demod_ring(const int16_t *bi, const int16_t *bq, size_t mask, size_t first, size_t blocks, float *hz,
                          size_t max);

/* halo_listen uses it too, for a frame's carrier offset. */
float halo_fm_atan2(float y, float x);

#ifdef __cplusplus
}
#endif
