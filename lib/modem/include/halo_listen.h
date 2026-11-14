/* SPDX-License-Identifier: GPL-3.0-only */
/* The consumer half of always-on listening, over halo_blocks' gap-free ring.
 *
 * Demodulates only stretches that look like a Halo frame: loud, narrowband
 * (Wi-Fi isn't) and at least as long as the shortest frame. It may fall behind
 * by most of the ring, which absorbs a burst of traffic.
 */
#pragma once
/* C only, so no extern "C": C++ has no _Atomic. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "halo_blocks.h"
#include "halo_detect.h"
#include "halo_fm.h"

#define HALO_LISTEN_SEG    64u      /* gate resolution: 32 us */
#define HALO_LISTEN_SPAN   1344u    /* longest frame in blocks, plus the detector's alignment search */
#define HALO_LISTEN_FRAME_SEGS 24u  /* a command with its ramps fills ~22 segments */
/* Diagnostic histograms: coherence in tenths, frequency in 100 kHz bins from
 * -1 MHz (bin 0) to +1 MHz (bin 20).
 */
#define HALO_LISTEN_COHERENCE_BINS 10u
#define HALO_LISTEN_FREQ_BINS      21u
#define HALO_LISTEN_FREQ_BIN_HZ    100000.0f
#define HALO_LISTEN_FREQ_LOW_HZ    (-1000000.0f)

typedef struct {
    halo_detection_t d;   /* d's start times are relative to the window; use block */
    uint32_t block;       /* stream position of the preamble, 0.5 us per block */
    uint8_t loud;         /* gate segments under the frame that counted as loud */
} halo_heard_t;

typedef struct {
    uint32_t segments, loud, windows, demod_blocks, frames, dupes;
    uint32_t dropped;     /* windows or stretches lost because we fell too far behind */
    uint32_t runs[6];     /* loud runs by length in segments: 1, 2-3, 4-7, 8-15, 16-31, 32+ */
    uint32_t coherence[HALO_LISTEN_COHERENCE_BINS];   /* segments over the power threshold, by lag-1 coherence */
    uint32_t off_band;        /* of those, centred more than 380 kHz off our carrier */
    /* Runs that opened a window, [0] frame-sized, [1] longer: mean frequency and coherence. */
    uint32_t run_freq[2][HALO_LISTEN_FREQ_BINS];
    uint32_t run_coherence[2][HALO_LISTEN_COHERENCE_BINS];
    float floor;
} halo_listen_stats_t;

typedef struct {
    halo_blocks_t *src;
    float *hz;
    size_t window;        /* blocks per demod window */
    bool gate;
    uint32_t checked;     /* blocks the gate has looked at */
    bool open;
    uint32_t w0, w_end;   /* the window being collected */
    bool in_run;
    uint32_t run_start;
    unsigned run_segs, run_gap;
    float run_re, run_im, run_p;
    uint32_t earliest;    /* frames starting before this were already searched */
    float floor;          /* block variance of quiet segments */
    unsigned warm;
    uint32_t loud_bits[HALO_BLOCKS_RING / HALO_LISTEN_SEG / 32];
    uint32_t recent[4];
    unsigned next_recent;
    halo_listen_stats_t stats;
} halo_listen_t;

/* hz needs room for at least 3 * HALO_LISTEN_SPAN values. gate false demods
 * everything, to check what the gate would have missed (it can't keep up on the S3).
 */
void halo_listen_init(halo_listen_t *l, halo_blocks_t *src, float *hz, size_t hz_max, bool gate);

/* At most one window of work per call. Returns false once it has caught up. */
bool halo_listen_poll(halo_listen_t *l, halo_heard_t *out, size_t max, size_t *count);
