/* SPDX-License-Identifier: GPL-3.0-only */
#include "halo_fm.h"

#include <math.h>

#include "halo_iq.h"

#define D HALO_FM_DECIMATION
_Static_assert(HALO_FM_IN_RATE % HALO_FM_DECIMATION == 0, "whole input samples per block");
_Static_assert(HALO_FM_RATE % 125000u == 0, "whole demod samples per bit");

/* 1/x for x > 0. A float divide measured ~140 cycles on the S3; a bit-trick
 * guess plus two Newton steps is good to ~1e-5, plenty for a phase we slice
 * at +-160 kHz.
 */
static inline float recip(float x)
{
    union {
        float f;
        uint32_t u;
    } v = {x};
    v.u = 0x7ef311c7u - v.u;
    float r = v.f;
    r = r * (2.0f - x * r);
    return r * (2.0f - x * r);
}

/* Polynomial atan2 so the demodulator doesn't depend on libm's speed. */
float halo_fm_atan2(float y, float x)
{
    float ax = fabsf(x), ay = fabsf(y);
    if (ax == 0.0f && ay == 0.0f) {
        return 0.0f;
    }
    float a = ax < ay ? ax * recip(ay) : ay * recip(ax);
    float s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) {
        r = 1.57079637f - r;
    }
    if (x < 0) {
        r = 3.14159274f - r;
    }
    return y < 0 ? -r : r;
}

size_t halo_fm_blocks(const uint32_t *raw, size_t count, int16_t *bi, int16_t *bq)
{
    size_t blocks = count / D;
    for (size_t b = 0; b < blocks; b++) {
        const uint32_t *w = raw + b * D;
        int32_t si = 0, sq = 0;
        for (size_t k = 0; k < D; k++) {
            si += halo_iq_i(w[k]);
            sq += halo_iq_q(w[k]);
        }
        bi[b] = (int16_t)si;   /* 8 x 512 fits easily */
        bq[b] = (int16_t)sq;
    }
    return blocks;
}

size_t halo_fm_demod_ring(const int16_t *bi, const int16_t *bq, size_t mask, size_t first, size_t blocks, float *hz,
                          size_t max)
{
    if (blocks < 2 || max == 0) {
        return 0;
    }
    /* The S3's RX path leaves a DC offset that would bias every reading. */
    int32_t sum_i = 0, sum_q = 0;
    for (size_t b = 0; b < blocks; b++) {
        size_t at = (first + b) & mask;
        sum_i += bi[at];
        sum_q += bq[at];
    }
    float dc_i = (float)sum_i / (float)blocks;
    float dc_q = (float)sum_q / (float)blocks;

    float prev_i = 0, prev_q = 0;
    size_t n = 0;
    for (size_t b = 0; b < blocks && n < max; b++) {
        size_t at = (first + b) & mask;
        float i = (float)bi[at] - dc_i;
        float q = dc_q - (float)bq[at];   /* S3 IQ is spectrally inverted */
        if (b) {
            hz[n++] = halo_fm_atan2(q * prev_i - i * prev_q, i * prev_i + q * prev_q) * HALO_FM_HZ_PER_RAD;
        }
        prev_i = i;
        prev_q = q;
    }

    /* Light smoothing: without it, detection at 4 dB drops from 100 % to ~22 %. */
    const float third = 1.0f / 3.0f;
    float before = hz[0];
    for (size_t j = 1; j + 1 < n; j++) {
        float here = hz[j];
        hz[j] = (before + here + hz[j + 1]) * third;
        before = here;
    }
    return n;
}

size_t halo_fm_demod_blocks(const int16_t *bi, const int16_t *bq, size_t blocks, float *hz, size_t max)
{
    return halo_fm_demod_ring(bi, bq, SIZE_MAX, 0, blocks, hz, max);
}

static int16_t block_i[HALO_FM_MAX_BLOCKS], block_q[HALO_FM_MAX_BLOCKS];

size_t halo_fm_demod(const uint32_t *raw, size_t count, float *hz, size_t max)
{
    if (count > HALO_FM_MAX_BLOCKS * D) {
        count = HALO_FM_MAX_BLOCKS * D;
    }
    size_t blocks = halo_fm_blocks(raw, count, block_i, block_q);
    return halo_fm_demod_blocks(block_i, block_q, blocks, hz, max);
}
