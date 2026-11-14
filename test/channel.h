/* SPDX-License-Identifier: GPL-3.0-only */
/* Simulated S3 receive path for tests. Deliberately independent of halo_gfsk,
 * so the receiver isn't only ever tested against our own transmitter.
 *
 *   ch_reset(dc) -> ch_add_frame(...) / ch_add_tone(...) -> ch_add_noise(...) -> ch_render(raw)
 */
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "halo_iq.h"
#include "prng.h"

#define CH_PI      3.14159265358979323846
#define CH_RATE    16000000.0
#define CH_SPB     128
#define CH_SAMPLES 16380   /* one S3 capture bank, ~1.02 ms */
#define CH_DEV_HZ  160000.0

static double ch_i[CH_SAMPLES], ch_q[CH_SAMPLES];

static inline double ch_urand(void)
{
    return test_rand() / (TEST_RAND_MAX + 1.0);
}

static inline double ch_gauss(void)
{
    double u = ch_urand() + 1e-12, v = ch_urand();
    return sqrt(-2 * log(u)) * cos(2 * CH_PI * v);
}

static inline void ch_reset(double dc_i, double dc_q)
{
    for (size_t j = 0; j < CH_SAMPLES; j++) {
        ch_i[j] = dc_i;
        ch_q[j] = dc_q;
    }
}

static inline double ch_level(const uint8_t *bits, size_t nbits, double t)
{
    const double a = CH_PI * 0.5 * sqrt(2.0 / log(2.0)) / CH_SPB;
    double y = 0;
    long k0 = (long)floor(t / CH_SPB) - 3;
    for (long k = k0; k <= k0 + 7; k++) {
        if (k < 0 || k >= (long)nbits) {
            continue;   /* outside the frame: unmodulated carrier */
        }
        y += (bits[k] ? 1 : -1) * 0.5 * (erf(a * (t - k * CH_SPB)) - erf(a * (t - (k + 1) * CH_SPB)));
    }
    return y;
}

/* A frame starting at input sample start, with two bits of plain carrier either
 * side like a real transmitter's ramp. Parts outside the capture are cut off.
 */
static inline void ch_add_frame(const uint8_t *bits, size_t nbits, long start, double offset_hz, double amp)
{
    double phase = ch_urand() * 2 * CH_PI;
    long ramp = 2 * CH_SPB;
    for (long rel = -ramp; rel < (long)nbits * CH_SPB + ramp; rel++) {
        phase += 2 * CH_PI * (offset_hz + CH_DEV_HZ * ch_level(bits, nbits, (double)rel)) / CH_RATE;
        long j = start + rel;
        if (j >= 0 && j < CH_SAMPLES) {
            ch_i[j] += amp * cos(phase);
            ch_q[j] += amp * sin(phase);
        }
    }
}

static inline void ch_add_tone(double offset_hz, double amp)
{
    double phase = ch_urand() * 2 * CH_PI;
    for (size_t j = 0; j < CH_SAMPLES; j++) {
        phase += 2 * CH_PI * offset_hz / CH_RATE;
        ch_i[j] += amp * cos(phase);
        ch_q[j] += amp * sin(phase);
    }
}

/* SNR per input sample against a signal of amplitude amp. */
static inline void ch_add_noise(double amp, double snr_db)
{
    double sigma = amp / sqrt(2 * pow(10, snr_db / 10));
    for (size_t j = 0; j < CH_SAMPLES; j++) {
        ch_i[j] += ch_gauss() * sigma;
        ch_q[j] += ch_gauss() * sigma;
    }
}

/* Quantize like the S3 ADC: 10 bits, clipped, Q inverted. */
static inline void ch_render(uint32_t raw[CH_SAMPLES])
{
    for (size_t j = 0; j < CH_SAMPLES; j++) {
        long i = lround(ch_i[j]), q = lround(-ch_q[j]);
        i = i > 511 ? 511 : i < -512 ? -512 : i;
        q = q > 511 ? 511 : q < -512 ? -512 : q;
        raw[j] = halo_iq_pack((int)i, (int)q);
    }
}
