/* SPDX-License-Identifier: GPL-3.0-only */
/* Bring-up bench on the real chip: timings, a command burst to the lamp, and
 * the listeners, all printed over USB for the host to read.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "halo_detect.h"
#include "halo_fm.h"
#include "phy_s3.h"

#define BENCH_NFM   (PHY_S3_CAPTURE_SAMPLES / HALO_FM_DECIMATION)
#define BENCH_FOUND 4u
/* 10 us of zero IQ at 40 MS/s before the preamble: halo-esp32s3's waveform, known to work. */
#define BENCH_LEAD_SILENCE 400

/* One demod buffer for everything that runs one capture at a time. */
extern float bench_hz[BENCH_NFM];
extern halo_detection_t bench_found[BENCH_FOUND];

void bench_gfsk_fill(size_t first, uint32_t *out, size_t count, void *ctx);

void bench_time_tx_path(void);
void bench_time_demod(void);
void bench_time_bank_reads(void);

/* Needs BENCH_LAMP_ADDRESS from the git-ignored bench_local.h; does nothing without it. */
void bench_send(void);

void bench_capture_forever(void);   /* one 1 ms capture at a time */
void bench_ring_forever(void);      /* the always-on listener */
