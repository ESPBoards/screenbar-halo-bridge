/* SPDX-License-Identifier: GPL-3.0-only */
/* GFSK modulator for the Halo link (125 kbit/s, +-160 kHz, BT 0.5) at the S3's
 * 40 MS/s DAC rate.
 *
 * Every sample can be computed on its own because the S3's two DAC banks
 * overlap and get filled out of order.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "halo_esb.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HALO_GFSK_RATE      40000000u
#define HALO_GFSK_BIT_RATE  125000u
#define HALO_GFSK_SPB       (HALO_GFSK_RATE / HALO_GFSK_BIT_RATE)
#define HALO_GFSK_DEVIATION 160000.0
#define HALO_GFSK_BT        0.5
#define HALO_GFSK_AMPLITUDE 128   /* phy_s3's TX gain table assumes samples at this level */

typedef struct {
    size_t lead;   /* silent samples before the preamble */
    size_t nbits;
    uint8_t pattern[HALO_ESB_MAX_BITS];
    uint32_t phase[HALO_ESB_MAX_BITS];   /* carrier phase where each bit starts */
} halo_gfsk_t;

/* Builds the tables. Call at boot so the first command isn't slower; prepare()
 * calls it anyway if you forget.
 */
void halo_gfsk_init(void);

void halo_gfsk_prepare(halo_gfsk_t *g, const uint8_t *bits, size_t nbits, size_t lead);

size_t halo_gfsk_length(const halo_gfsk_t *g);

/* Zero outside the frame so the DAC is silent there. */
void halo_gfsk_sample(const halo_gfsk_t *g, size_t n, int16_t *i, int16_t *q);

/* Samples first .. first + count as packed DAC words (halo_iq_pack). Same values
 * as halo_gfsk_sample, without a divide per sample: loading the DAC is 29k samples.
 */
void halo_gfsk_fill(const halo_gfsk_t *g, size_t first, uint32_t *out, size_t count);

#ifdef __cplusplus
}
#endif
