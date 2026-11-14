/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>
#include <time.h>

#include "channel.h"
#include "check.h"
#include "halo_esb.h"
#include "halo_fm.h"
#include "prng.h"

#define NFM (CH_SAMPLES / HALO_FM_DECIMATION)

static uint32_t raw[CH_SAMPLES];
static float hz[NFM];

static void test_atan2(void)
{
    double worst = 0;
    for (int k = 0; k < 3600; k++) {
        double angle = -CH_PI + 2 * CH_PI * k / 3600;
        for (double r = 0.01; r < 2000; r *= 7) {
            double got = halo_fm_atan2((float)(r * sin(angle)), (float)(r * cos(angle)));
            double err = fabs(got - atan2(sin(angle), cos(angle)));
            if (err > CH_PI) {
                err = 2 * CH_PI - err;   /* +pi and -pi are the same angle */
            }
            worst = err > worst ? err : worst;
        }
    }
    printf("  atan2: worst error %.6f rad (%.0f Hz at 2 MS/s)\n", worst, worst * HALO_FM_RATE / (2 * CH_PI));
    /* Under 1 kHz is nothing next to the +-160 kHz we slice on. */
    CHECK(worst < 1e-3, "atan2 error %.6f rad", worst);
}

/* A plain carrier must read as its own offset, with the right sign. The weak
 * case matters most: a distant ACK can be smaller than the S3's own DC offset.
 */
static void test_tone(void)
{
    for (int khz = -300; khz <= 300; khz += 50) {
        bool weak = khz % 100 == 0;
        ch_reset(weak ? 60 : 9, weak ? -45 : -6);
        ch_add_tone(khz * 1000.0, weak ? 30 : 100);
        ch_render(raw);
        size_t n = halo_fm_demod(raw, CH_SAMPLES, hz, NFM);
        CHECK(n == NFM - 1, "got %zu samples", n);
        double mean = 0;
        for (size_t j = 1; j + 1 < n; j++) {
            mean += hz[j];
        }
        mean /= (double)(n - 2);
        CHECK(fabs(mean - khz * 1000.0) < 500, "tone %d kHz read as %.0f Hz", khz, mean);
    }
}

static size_t frame_bits(uint8_t bits[HALO_ESB_MAX_BITS], unsigned seed)
{
    test_srand(seed);
    halo_esb_frame_t f = {.address = (uint32_t)test_rand() ^ ((uint32_t)test_rand() << 16), .pid = test_rand() & 3,
                          .length = 2, .payload = {(uint8_t)test_rand(), (uint8_t)test_rand()}};
    return halo_esb_encode(&f, bits);
}

/* Slice each bit at its centre, against the known offset. Finding frames is test_detect's job. */
static unsigned bit_errors(const uint8_t *bits, size_t nbits, long start, double offset)
{
    unsigned errors = 0;
    for (size_t k = 0; k < nbits; k++) {
        double centre = (double)start + k * CH_SPB + CH_SPB / 2.0;
        size_t idx = (size_t)lround(centre / HALO_FM_DECIMATION) - 1;
        errors += (hz[idx] > offset) != bits[k];
    }
    return errors;
}

static void test_frame_clean(void)
{
    uint8_t bits[HALO_ESB_MAX_BITS];
    for (unsigned seed = 1; seed <= 50; seed++) {
        size_t nbits = frame_bits(bits, seed);
        double offset = (seed % 2 ? 1 : -1) * 55000.0;
        long start = 2000 + (long)(seed * 37);
        ch_reset(9, -6);
        ch_add_frame(bits, nbits, start, offset, 100);
        ch_render(raw);
        halo_fm_demod(raw, CH_SAMPLES, hz, NFM);
        CHECK(bit_errors(bits, nbits, start, offset) == 0, "frame %u has bit errors without noise", seed);
    }
}

/* Raw bit error rate at bit centres, no tricks. The detector must do better than this. */
static void test_frame_noise(void)
{
    uint8_t bits[HALO_ESB_MAX_BITS];
    const double snrs[] = {-6, -3, 0, 4};
    printf("  bit errors at bit centres, 300 frames each:");
    for (unsigned s = 0; s < 4; s++) {
        unsigned errors = 0, total = 0;
        for (unsigned seed = 1; seed <= 300; seed++) {
            size_t nbits = frame_bits(bits, seed + 1000 * s);
            double offset = (ch_urand() * 2 - 1) * 80000;
            long start = 1000 + test_rand() % 4000;
            ch_reset(ch_gauss() * 8, ch_gauss() * 8);
            ch_add_frame(bits, nbits, start, offset, 100);
            ch_add_noise(100, snrs[s]);
            ch_render(raw);
            halo_fm_demod(raw, CH_SAMPLES, hz, NFM);
            errors += bit_errors(bits, nbits, start, offset);
            total += (unsigned)nbits;
        }
        printf("  %.0f dB: %.4f%%", snrs[s], 100.0 * errors / total);
        if (snrs[s] >= 4) {
            CHECK(errors == 0, "%u bit errors at %.0f dB", errors, snrs[s]);
        }
    }
    printf("\n");
}

/* Block sums taken piece by piece, concatenated, must give exactly what one
 * unbroken capture gives.
 */
static void test_pieces(void)
{
    static float whole[NFM], pieced[NFM];
    static int16_t bi[NFM], bq[NFM];
    uint8_t bits[HALO_ESB_MAX_BITS];
    size_t nbits = frame_bits(bits, 3);
    ch_reset(7, -4);
    ch_add_frame(bits, nbits, 7000, 60000, 100);   /* across the 8000-sample cut below */
    ch_add_noise(100, 10);
    ch_render(raw);
    size_t count = CH_SAMPLES / 8 * 8;
    size_t n1 = halo_fm_demod(raw, count, whole, NFM);
    const size_t cuts[] = {0, 8000, 8064, count};
    size_t blocks = 0;
    for (int c = 0; c < 3; c++) {
        blocks += halo_fm_blocks(raw + cuts[c], cuts[c + 1] - cuts[c], bi + blocks, bq + blocks);
    }
    size_t n2 = halo_fm_demod_blocks(bi, bq, blocks, pieced, NFM);
    unsigned diff = n1 != n2;
    for (size_t j = 0; j < n1 && j < n2; j++) {
        diff += whole[j] != pieced[j];
    }
    CHECK(diff == 0, "pieced demod differs from whole in %u samples", diff);
}

static void report_speed(void)
{
    clock_t t0 = clock();
    for (int r = 0; r < 200; r++) {
        halo_fm_demod(raw, CH_SAMPLES, hz, NFM);
    }
    printf("  one capture: %.0f us on this computer\n", (double)(clock() - t0) / CLOCKS_PER_SEC * 1e6 / 200);
}

int main(void)
{
    test_atan2();
    test_tone();
    test_frame_clean();
    test_frame_noise();
    test_pieces();
    report_speed();
    return check_report("test_fm");
}
