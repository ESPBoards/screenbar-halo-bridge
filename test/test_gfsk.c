/* SPDX-License-Identifier: GPL-3.0-only */
#include <math.h>
#include <stdio.h>
#include <time.h>

#include "channel.h"
#include "check.h"
#include "halo_gfsk.h"
#include "halo_iq.h"
#include "prng.h"

#define SPB  HALO_GFSK_SPB
#define LEAD 400

/* Exact GFSK: filter against every bit, not just the 3-bit window the tables
 * use, so the test measures what that shortcut costs.
 */
static double reference_level(const uint8_t *bits, size_t nbits, double t)
{
    const double a = CH_PI * HALO_GFSK_BT * sqrt(2.0 / log(2.0)) / SPB;
    double level = 0;
    for (long k = -4; k < (long)nbits + 4; k++) {
        unsigned bit = bits[k < 0 ? 0 : k >= (long)nbits ? (long)nbits - 1 : k];
        double start = (double)k * SPB;
        level += (bit ? 1.0 : -1.0) * 0.5 * (erf(a * (t - start)) - erf(a * (t - start - SPB)));
    }
    return level;
}

static double wrap(double x)
{
    while (x > CH_PI) {
        x -= 2 * CH_PI;
    }
    while (x < -CH_PI) {
        x += 2 * CH_PI;
    }
    return x;
}

static void make_frame(uint8_t bits[HALO_ESB_MAX_BITS], size_t *nbits, unsigned seed)
{
    test_srand(seed);
    halo_esb_frame_t f = {.address = (uint32_t)test_rand() ^ ((uint32_t)test_rand() << 16) ^ 0x1a2b3c4du,
                          .pid = test_rand() & 3, .length = 2, .payload = {(uint8_t)test_rand(), (uint8_t)test_rand()}};
    *nbits = halo_esb_encode(&f, bits);
}

static void test_against_reference(void)
{
    double worst_phase = 0, worst_env = 0;
    for (unsigned seed = 1; seed <= 20; seed++) {
        uint8_t bits[HALO_ESB_MAX_BITS];
        size_t nbits;
        make_frame(bits, &nbits, seed);
        halo_gfsk_t g;
        halo_gfsk_prepare(&g, bits, nbits, LEAD);
        CHECK(halo_gfsk_length(&g) == LEAD + nbits * SPB, "length");

        double ref = 0;
        for (size_t n = 0; n < halo_gfsk_length(&g); n++) {
            int16_t i, q;
            halo_gfsk_sample(&g, n, &i, &q);
            if (n < LEAD) {
                CHECK(i == 0 && q == 0, "sample %zu before the frame is not silent", n);
                continue;
            }
            ref += reference_level(bits, nbits, (double)(n - LEAD)) * 2 * CH_PI * HALO_GFSK_DEVIATION / HALO_GFSK_RATE;
            double err = fabs(wrap(atan2(q, i) - ref));
            double env = fabs(hypot(i, q) - HALO_GFSK_AMPLITUDE);
            worst_phase = err > worst_phase ? err : worst_phase;
            worst_env = env > worst_env ? env : worst_env;
        }
        int16_t i, q;
        halo_gfsk_sample(&g, halo_gfsk_length(&g), &i, &q);
        CHECK(i == 0 && q == 0, "sample after the frame is not silent");
    }
    printf("  vs exact GFSK: worst phase error %.4f rad, envelope +-%.2f\n", worst_phase, worst_env);
    /* 0.05 rad is far below what moves a bit decision; the table step alone is 0.006. */
    CHECK(worst_phase < 0.05, "phase error %.4f rad", worst_phase);
    CHECK(worst_env <= 1.0, "envelope off by %.2f", worst_env);
}

/* What a receiver would see: instantaneous frequency at each bit's centre. */
static void test_loopback(void)
{
    double worst_margin = 1e9;
    for (unsigned seed = 100; seed < 300; seed++) {
        uint8_t bits[HALO_ESB_MAX_BITS];
        size_t nbits;
        make_frame(bits, &nbits, seed);
        halo_gfsk_t g;
        halo_gfsk_prepare(&g, bits, nbits, LEAD);
        for (size_t k = 0; k < nbits; k++) {
            size_t c = LEAD + k * SPB + SPB / 2;
            int16_t i0, q0, i1, q1;
            halo_gfsk_sample(&g, c - 20, &i0, &q0);
            halo_gfsk_sample(&g, c + 20, &i1, &q1);
            double hz = atan2((double)q1 * i0 - (double)i1 * q0, (double)i1 * i0 + (double)q1 * q0) / (2 * CH_PI) *
                        HALO_GFSK_RATE / 40;
            CHECK((hz > 0) == bits[k], "frame %u bit %zu decoded wrong (%.0f Hz)", seed, k, hz);
            worst_margin = fabs(hz) < worst_margin ? fabs(hz) : worst_margin;
        }
    }
    printf("  loopback: every bit right, weakest bit centre %.0f kHz\n", worst_margin / 1000);
    /* Alternating bits (the preamble) are the weak case for BT 0.5 GFSK. */
    CHECK(worst_margin > 100000, "weakest bit only %.0f Hz", worst_margin);
}

/* fill() is the fast path the DAC uses; it must give exactly what sample() gives. */
static void test_fill_matches_sample(void)
{
    uint8_t bits[HALO_ESB_MAX_BITS];
    size_t nbits;
    make_frame(bits, &nbits, 42);
    halo_gfsk_t g;
    halo_gfsk_prepare(&g, bits, nbits, LEAD);
    static uint32_t out[30000];
    unsigned mismatches = 0;
    for (int t = 0; t < 300; t++) {
        size_t first = (size_t)test_rand() % (halo_gfsk_length(&g) + 600);
        size_t count = 1 + (size_t)test_rand() % 3000;
        halo_gfsk_fill(&g, first, out, count);
        for (size_t j = 0; j < count; j++) {
            int16_t i, q;
            halo_gfsk_sample(&g, first + j, &i, &q);
            mismatches += out[j] != halo_iq_pack(i, q);
        }
    }
    halo_gfsk_fill(&g, 0, out, halo_gfsk_length(&g) + 400);   /* the whole thing in one go */
    for (size_t n = 0; n < halo_gfsk_length(&g) + 400; n++) {
        int16_t i, q;
        halo_gfsk_sample(&g, n, &i, &q);
        mismatches += out[n] != halo_iq_pack(i, q);
    }
    CHECK(mismatches == 0, "fill differs from sample in %u words", mismatches);
}

/* The demodulator reads every ADC word through these, so each value must come back, whatever the other half holds. */
static void test_iq_words(void)
{
    unsigned wrong = 0;
    for (int v = -512; v < 512; v++) {
        uint32_t word = halo_iq_pack(v, -1 - v) | 0xfff00000u;   /* the ADC's top bits aren't sample data */
        wrong += halo_iq_i(word) != v || halo_iq_q(word) != -1 - v;
    }
    CHECK(wrong == 0, "%u IQ words unpacked wrong", wrong);
}

/* Not a pass/fail: just the order of magnitude, to spot a slowdown. */
static void report_speed(void)
{
    uint8_t bits[HALO_ESB_MAX_BITS];
    size_t nbits;
    make_frame(bits, &nbits, 7);
    halo_gfsk_t g;
    volatile int32_t sink = 0;
    clock_t t0 = clock();
    for (int r = 0; r < 100; r++) {
        halo_gfsk_prepare(&g, bits, nbits, LEAD);
        for (size_t n = 0; n < halo_gfsk_length(&g); n++) {
            int16_t i, q;
            halo_gfsk_sample(&g, n, &i, &q);
            sink += i ^ q;
        }
    }
    double us = (double)(clock() - t0) / CLOCKS_PER_SEC * 1e6 / 100;
    printf("  one full frame: %.0f us on this computer\n", us);
}

int main(void)
{
    halo_gfsk_init();
    test_against_reference();
    test_loopback();
    test_fill_matches_sample();
    test_iq_words();
    report_speed();
    return check_report("test_gfsk");
}
