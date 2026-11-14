/* SPDX-License-Identifier: GPL-3.0-only */
/* The simple listener: one 1 ms capture, demod, detect, repeat. Kept to
 * compare against the ring and to dump raw captures for tools/replay.c.
 */
#include <math.h>
#include <stdio.h>

#include "bench.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "halo_iq.h"

#define LOUD_RMS      70.0      /* well above the ~27 rms noise floor */
#define DUMP_EVERY_US 2000000   /* rate limit: a dump takes ~0.3 s over USB */
#define DUMPS_ON      0         /* 1 to send loud undecoded captures to the host */
#define LOUD_EVERY    50        /* check loudness on 1 capture in 50 */

/* Integer, on every 4th sample: a double-precision version cost ~40 ms per call
 * on the S3 and swamped the measurements.
 */
static double capture_rms(const uint32_t *raw)
{
    int64_t si = 0, sq = 0, ss = 0;
    size_t n = 0;
    for (size_t j = 0; j < PHY_S3_CAPTURE_SAMPLES; j += 4, n++) {
        int32_t i = halo_iq_i(raw[j]), q = halo_iq_q(raw[j]);
        si += i;
        sq += q;
        ss += i * i + q * q;
    }
    double mi = (double)si / n, mq = (double)sq / n;
    return sqrt((double)ss / n - mi * mi - mq * mq);
}

static void dump(const uint32_t *raw, unsigned id)
{
    printf("RAW %u\n", id);
    for (size_t j = 0; j < PHY_S3_CAPTURE_SAMPLES; j += 16) {
        for (size_t k = j; k < j + 16 && k < PHY_S3_CAPTURE_SAMPLES; k++) {
            printf("%05lx", (unsigned long)(raw[k] & HALO_IQ_MASK));   /* replay reads 5 digits a word */
        }
        printf("\n");
    }
    printf("RAWEND %u\n", id);
}

/* DC and noise level, from one capture per second. */
static void level(const uint32_t *raw, double *dc_i, double *dc_q, double *rms)
{
    double si = 0, sq = 0, ss = 0;
    for (size_t j = 0; j < PHY_S3_CAPTURE_SAMPLES; j++) {
        si += halo_iq_i(raw[j]);
        sq += halo_iq_q(raw[j]);
    }
    *dc_i = si / PHY_S3_CAPTURE_SAMPLES;
    *dc_q = sq / PHY_S3_CAPTURE_SAMPLES;
    for (size_t j = 0; j < PHY_S3_CAPTURE_SAMPLES; j++) {
        double di = halo_iq_i(raw[j]) - *dc_i, dq = halo_iq_q(raw[j]) - *dc_q;
        ss += di * di + dq * dq;
    }
    *rms = sqrt(ss / PHY_S3_CAPTURE_SAMPLES);
}

void bench_capture_forever(void)
{
    unsigned dumps = 0;
    int64_t last_dump = 0;
    for (;;) {
        unsigned captures = 0, failed = 0, frames = 0, loud = 0;
        int64_t capture_us = 0, demod_us = 0, detect_us = 0;
        double dc_i = 0, dc_q = 0, rms = 0;
        int64_t start = esp_timer_get_time();
        while (esp_timer_get_time() - start < 1000000) {
            int64_t a = esp_timer_get_time();
            bool ok = phy_s3_listen();
            int64_t b = esp_timer_get_time();
            captures++;
            if (!ok) {
                failed++;
                continue;
            }
            const uint32_t *raw = phy_s3_capture();
            size_t n = halo_fm_demod(raw, PHY_S3_CAPTURE_SAMPLES, bench_hz, BENCH_NFM);
            int64_t c = esp_timer_get_time();
            size_t k = halo_detect(bench_hz, n, bench_found, BENCH_FOUND);
            int64_t d = esp_timer_get_time();
            capture_us += b - a;
            demod_us += c - b;
            detect_us += d - c;

            if (captures == 1) {
                level(raw, &dc_i, &dc_q, &rms);
            }
            if (captures % LOUD_EVERY == 0 && capture_rms(raw) > LOUD_RMS) {
                loud++;
                if (DUMPS_ON && !k && esp_timer_get_time() - last_dump > DUMP_EVERY_US) {
                    dump(raw, dumps++);
                    last_dump = esp_timer_get_time();
                }
            }
            for (size_t j = 0; j < k; j++) {
                const halo_detection_t *x = &bench_found[j];
                printf("FRAME %08lX len %u pid %u payload %02X%02X offset %.0f weak %u\n",
                       (unsigned long)x->frame.address, x->frame.length, x->frame.pid, x->frame.payload[0],
                       x->frame.payload[1], x->offset_hz, x->weak_bits);
                frames++;
            }
        }
        unsigned ok = captures - failed;
        printf("STATS captures %u failed %u frames %u loud %u"
               " | per capture: capture %lld us demod %lld us detect %lld us"
               " | duty %.1f%% | dc %.1f %.1f rms %.1f\n",
               captures, failed, frames, loud, ok ? capture_us / ok : 0, ok ? demod_us / ok : 0,
               ok ? detect_us / ok : 0, ok * 1.024 / 10.0, dc_i, dc_q, rms);
        vTaskDelay(1);   /* let the console drain */
    }
}
