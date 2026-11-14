/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "channel.h"
#include "check.h"
#include "frames.h"
#include "halo_detect.h"
#include "halo_fm.h"
#include "prng.h"

#define NFM (CH_SAMPLES / HALO_FM_DECIMATION)
#define MAX_FOUND 8

static uint32_t raw[CH_SAMPLES];
static float hz[NFM];
static halo_detection_t found[MAX_FOUND];

static void add(const halo_esb_frame_t *f, long start, double offset, double amp)
{
    uint8_t bits[HALO_ESB_MAX_BITS];
    size_t n = halo_esb_encode(f, bits);
    ch_add_frame(bits, n, start, offset, amp);
}

static size_t detect(void)
{
    ch_render(raw);
    size_t n = halo_fm_demod(raw, CH_SAMPLES, hz, NFM);
    return halo_detect(hz, n, found, MAX_FOUND);
}

static void test_clean(void)
{
    double worst_offset = 0, worst_time = 0;
    for (int t = 0; t < 200; t++) {
        halo_esb_frame_t f = random_frame(t & 1);
        double offset = (ch_urand() * 2 - 1) * 150000;
        long start = 500 + test_rand() % 5000;
        ch_reset(ch_gauss() * 8, ch_gauss() * 8);
        add(&f, start, offset, 100);
        size_t n = detect();
        CHECK(n == 1 && same_frame(&found[0].frame, &f), "frame %d: found %zu", t, n);
        if (n == 1) {
            double eo = fabs(found[0].offset_hz - offset), et = fabs(found[0].start_us - start / 16.0);
            worst_offset = eo > worst_offset ? eo : worst_offset;
            worst_time = et > worst_time ? et : worst_time;
            /* hz[n] sits 8 * (n + 1) input samples in, and start_us is that rounded. */
            const halo_detection_t *d = &found[0];
            CHECK(fabs((d->start_sample + 1) * 8.0 - start) <= 4 * 8, "frame %d: start sample %u, want ~%ld", t,
                  (unsigned)d->start_sample, start / 8 - 1);
            CHECK(d->start_us == (d->start_sample + 1) / 2, "frame %d: start_us %u from sample %u", t,
                  (unsigned)d->start_us, (unsigned)d->start_sample);
        }
    }
    printf("  clean: offset estimate within %.1f kHz, start within %.1f us\n", worst_offset / 1000, worst_time);
    /* The worst of 200 frames ran 7-15 kHz over 20 random sequences; a broken
     * estimate would be off by tens of kHz against the +-160 kHz swing.
     */
    CHECK(worst_offset < 20000, "offset off by %.0f Hz", worst_offset);
    CHECK(worst_time <= 2, "start off by %.1f us", worst_time);
}

/* An ACK far off our carrier must still decode: 99 % out to +-150 kHz from 4 dB up. */
static void test_rate(void)
{
    const int offsets[] = {-200, -150, -100, -50, 0, 50, 100, 150, 200};
    const double snrs[] = {-3, 0, 4, 8};
    printf("  ACK found, %% of 100, by offset (kHz)\n     SNR");
    for (unsigned o = 0; o < 9; o++) {
        printf("%5d", offsets[o]);
    }
    printf("\n");
    for (unsigned s = 0; s < 4; s++) {
        printf("  %4.0f dB", snrs[s]);
        for (unsigned o = 0; o < 9; o++) {
            unsigned hits = 0;
            for (int t = 0; t < 100; t++) {
                halo_esb_frame_t f = random_frame(true);
                ch_reset(ch_gauss() * 8, ch_gauss() * 8);
                add(&f, 2000 + test_rand() % 3000, offsets[o] * 1000.0 + (ch_urand() * 2 - 1) * 2000, 100);
                ch_add_noise(100, snrs[s]);
                size_t n = detect();
                hits += halo_detect_find_ack(found, n, f.address, f.pid) != NULL;
            }
            printf("%5u", hits);
            if (snrs[s] >= 4 && abs(offsets[o]) <= 150) {
                CHECK(hits >= 99, "%.0f dB, %d kHz: %u %%", snrs[s], offsets[o], hits);
            }
        }
        printf("\n");
    }
}

/* Two whole frames never fit in one 1.02 ms capture, so the real case is a cut-off
 * command followed by the lamp's ACK 184 us after it ends. Only the ACK may come out.
 */
static void test_tail_then_ack(void)
{
    for (int t = 0; t < 50; t++) {
        halo_esb_frame_t cmd = random_frame(false), ack = random_frame(true);
        long cmd_end = 200 + test_rand() % 2000;
        ch_reset(5, -3);
        add(&cmd, cmd_end - 81 * CH_SPB, 40000, 100);
        add(&ack, cmd_end + 184 * 16, 45000, 60);
        ch_add_noise(60, 10);
        size_t n = detect();
        CHECK(n == 1 && same_frame(&found[0].frame, &ack), "capture %d: found %zu", t, n);
    }
}

/* A frame running off the end must not decode as something else. */
static void test_cut_at_end(void)
{
    unsigned wrong = 0;
    for (int t = 0; t < 300; t++) {
        halo_esb_frame_t f = random_frame(t & 1);
        long len = (long)halo_esb_frame_bits(f.length) * CH_SPB;
        ch_reset(5, -3);
        add(&f, CH_SAMPLES - len + 64 + test_rand() % (int)(len - 64), 30000, 100);
        size_t n = detect();
        for (size_t k = 0; k < n; k++) {
            wrong += !same_frame(&found[k].frame, &f);
        }
    }
    CHECK(wrong == 0, "%u wrong frames from cut-off frames", wrong);
}

static void test_find_ack(void)
{
    halo_detection_t d[2] = {{.frame = {.address = 0x1a2b3c4du, .pid = 2, .no_ack = true}},
                             {.frame = {.address = 0x1a2b3c4du, .pid = 1, .length = 2}}};
    CHECK(halo_detect_find_ack(d, 2, 0x1a2b3c4du, 2) == &d[0], "real ACK missed");
    CHECK(!halo_detect_find_ack(d, 2, 0x1a2b3c4du, 1), "a command counted as ACK");
    CHECK(!halo_detect_find_ack(d, 2, 0x1a2b3c4du, 3), "wrong PID counted");
    CHECK(!halo_detect_find_ack(d, 2, 0x1a2b3c4cu, 2), "wrong address counted");
}

/* Noise can pass for a frame from a random address, so count how often. */
static void test_false_frames(void)
{
    unsigned false_frames = 0, captures = 0, worst_weak = 0;
    for (int t = 0; t < 4000; t++, captures++) {
        ch_reset(ch_gauss() * 20, ch_gauss() * 20);
        if (t % 2) {
            /* garbage with a perfect preamble: the hardest case for a CRC-only check */
            uint8_t bits[96];
            for (int k = 0; k < 96; k++) {
                bits[k] = k < 8 ? (uint8_t)(k & 1) : (uint8_t)(test_rand() & 1);
            }
            ch_add_frame(bits, 96, test_rand() % 4000, (ch_urand() * 2 - 1) * 100000, 100);
            ch_add_noise(100, ch_urand() * 20);
        } else {
            ch_add_noise(100, -10 + ch_urand() * 10);   /* noise only, strong to weak */
        }
        size_t n = detect();
        false_frames += (unsigned)n;
        for (size_t k = 0; k < n; k++) {
            worst_weak = found[k].weak_bits > worst_weak ? found[k].weak_bits : worst_weak;
        }
    }
    printf("  false frames: %u in %u captures of noise and garbage (most weak bits %u)\n", false_frames, captures,
           worst_weak);
    /* CRC-16 lets about 1 in 65536 garbage frames through, so a run sees one
     * now and then. They come from random addresses, which the radio ignores.
     * A weaker check would let through many.
     */
    CHECK(false_frames <= 3, "%u false frames", false_frames);
}

static void report_speed(void)
{
    clock_t t0 = clock();
    for (int r = 0; r < 200; r++) {
        size_t n = halo_fm_demod(raw, CH_SAMPLES, hz, NFM);
        halo_detect(hz, n, found, MAX_FOUND);
    }
    printf("  demod + detect, one capture: %.0f us on this computer\n",
           (double)(clock() - t0) / CLOCKS_PER_SEC * 1e6 / 200);
}

int main(void)
{
    test_srand(77);
    test_clean();
    test_rate();
    test_tail_then_ack();
    test_cut_at_end();
    test_find_ack();
    test_false_frames();
    report_speed();
    return check_report("test_detect");
}
