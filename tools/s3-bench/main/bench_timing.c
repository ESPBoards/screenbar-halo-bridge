/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>

#include "bench.h"
#include "esp_cpu.h"
#include "esp_timer.h"
#include "halo_esb.h"
#include "halo_gfsk.h"

void bench_time_tx_path(void)
{
    halo_esb_frame_t f = {.address = 0x1a2b3c4du, .length = 2, .payload = {0xc5, 0xa4}};
    uint8_t bits[HALO_ESB_MAX_BITS];
    size_t nbits = halo_esb_encode(&f, bits);

    int64_t t0 = esp_timer_get_time();
    halo_gfsk_init();
    int64_t t1 = esp_timer_get_time();
    static halo_gfsk_t g;
    halo_gfsk_prepare(&g, bits, nbits, BENCH_LEAD_SILENCE);
    phy_s3_load(bench_gfsk_fill, &g);   /* fills the DAC banks, plays nothing */
    int64_t t2 = esp_timer_get_time();
    printf("TIMING gfsk_init_us %lld load_frame_us %lld\n", t1 - t0, t2 - t1);
}

/* Where demod time goes: max = 1 stops after the block-sum pass. */
void bench_time_demod(void)
{
    phy_s3_listen();
    const uint32_t *raw = phy_s3_capture();
    int64_t a = esp_timer_get_time();
    halo_fm_demod(raw, PHY_S3_CAPTURE_SAMPLES, bench_hz, 1);
    int64_t b = esp_timer_get_time();
    size_t n = halo_fm_demod(raw, PHY_S3_CAPTURE_SAMPLES, bench_hz, BENCH_NFM);
    int64_t c = esp_timer_get_time();
    volatile float sink = 0;
    for (size_t j = 0; j < n; j++) {
        sink += halo_fm_atan2(bench_hz[j], bench_hz[(j + 7) % n] + 1.0f);
    }
    int64_t d = esp_timer_get_time();
    halo_detect(bench_hz, n, bench_found, BENCH_FOUND);
    int64_t e = esp_timer_get_time();
    printf("TIMING demod_total_us %lld block_pass_us %lld atan2_x%zu_us %lld detect_us %lld\n", c - b, b - a, n,
           d - c, e - d);
}

/* Is the spare modem bank slower to read than normal RAM? */
void bench_time_bank_reads(void)
{
    static int16_t ram[8192];
    const int16_t *bank = phy_s3_spare();
    volatile int64_t sink = 0;
    for (int pass = 0; pass < 2; pass++) {
        const int16_t *p = pass ? bank : ram;
        uint32_t t0 = esp_cpu_get_cycle_count();
        int64_t ss = 0;
        for (int r = 0; r < 4; r++) {
            for (size_t j = 0; j < 8192; j++) {
                ss += p[j] * p[j];
            }
        }
        uint32_t t1 = esp_cpu_get_cycle_count();
        sink += ss;
        printf("TIMING %s int16 read+square: %.2f cycles each\n", pass ? "spare bank" : "ram", (t1 - t0) / 32768.0);
    }
}
