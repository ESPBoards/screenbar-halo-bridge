/* SPDX-License-Identifier: GPL-3.0-only */
#include "halo_listen.h"

#include <assert.h>
#include <math.h>
#include <stdatomic.h>

#include "halo_fm.h"

#define MASK      (HALO_BLOCKS_RING - 1u)
#define SEG       HALO_LISTEN_SEG
#define NSEG      (HALO_BLOCKS_RING / SEG)
#define SPAN      HALO_LISTEN_SPAN
#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))
_Static_assert((HALO_BLOCKS_RING & MASK) == 0, "the ring is indexed with a mask");
_Static_assert(HALO_BLOCKS_RING % HALO_LISTEN_SEG == 0, "segments must not wrap");
_Static_assert(NSEG % 32 == 0, "loud_bits holds 32 segments per word");
#define LOUD      2.0f    /* x noise floor: about -9 dB per input sample, below what the detector can use */
#define COHERENT  0.45f   /* Halo frames measure 0.6-0.9 even at -3 dB; Wi-Fi measured mostly < 0.3 */
/* cos(1.194 rad per block) = 380 kHz: the detector's 200 kHz offset plus the full 160 kHz deviation, plus noise. */
#define MAX_STEP_COS 0.368f
#define WARMUP    64u     /* 2 ms; the quietest segment in it seeds the floor */
#define MIN_FLOOR 1.0f    /* a flat segment at start-up mustn't switch the power gate off for good */
#define DUP       128u    /* two real frames never start within 64 us */
#define MIN_RUN   12u     /* loud segments; the shortest frame (an ACK, 520 us) fills 15 */
#define MAX_GAP   1u      /* quiet segments a run may skip, for noise dips in a weak frame */
/* A frame's partly filled edge segments may not count as loud. 4 leaves slack, at 128 us more demod each side. */
#define MARGIN    (4u * SEG)
#define MAX_FOUND 8u

/* The producer may be writing up to HALO_BLOCKS_PUBLISH blocks past what we
 * see; anything older than this could be half overwritten.
 */
#define SAFE_LAG (HALO_BLOCKS_RING - HALO_BLOCKS_PUBLISH - 64u)

void halo_listen_init(halo_listen_t *l, halo_blocks_t *src, float *hz, size_t hz_max, bool gate)
{
    assert(hz_max >= 3 * SPAN);
    *l = (halo_listen_t){.src = src, .hz = hz, .window = hz_max + 1, .gate = gate};
    l->checked = l->earliest = atomic_load(&src->written) & ~(SEG - 1u);
    for (unsigned k = 0; k < COUNTOF(l->recent); k++) {
        l->recent[k] = l->checked - 0x80000000u;   /* far from anything real */
    }
}

static bool too_old(const halo_listen_t *l, uint32_t block)
{
    return atomic_load_explicit(&l->src->written, memory_order_acquire) - block > SAFE_LAG;
}

static bool seg_loud(const halo_listen_t *l, uint32_t s)
{
    uint32_t k = (s / SEG) & (NSEG - 1u);
    return l->loud_bits[k / 32] >> (k % 32) & 1u;
}

static uint32_t later(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0 ? a : b;
}

/* A decodable frame makes every segment it covers loud (the gate sits ~6 dB
 * below what the detector can use), so a window only opens for a run as long
 * as the shortest frame, and only needs a small margin around it. Wi-Fi blips
 * shorter than that cost nothing.
 */
static unsigned coherence_bin(float coherence)
{
    unsigned c = (unsigned)(coherence * HALO_LISTEN_COHERENCE_BINS);
    return c < HALO_LISTEN_COHERENCE_BINS ? c : HALO_LISTEN_COHERENCE_BINS - 1;
}

/* Nearest bin; anything further out counts in the end bins. */
static unsigned freq_bin(float hz)
{
    int f = (int)((hz - HALO_LISTEN_FREQ_LOW_HZ + HALO_LISTEN_FREQ_BIN_HZ / 2) / HALO_LISTEN_FREQ_BIN_HZ);
    return f < 0 ? 0 : f >= (int)HALO_LISTEN_FREQ_BINS ? HALO_LISTEN_FREQ_BINS - 1 : (unsigned)f;
}

/* Diagnostics only: how long loud runs are, and where window-opening ones sit. */
static void count_run(halo_listen_t *l)
{
    unsigned bucket = 0;
    while (bucket + 1 < COUNTOF(l->stats.runs) && l->run_segs >= 2u << bucket) {
        bucket++;
    }
    l->stats.runs[bucket]++;
    if (l->run_segs >= MIN_RUN && l->run_p > 0) {
        unsigned cls = l->run_segs > HALO_LISTEN_FRAME_SEGS;
        float hz = -halo_fm_atan2(l->run_im, l->run_re) * HALO_FM_HZ_PER_RAD;   /* IQ inverted */
        l->stats.run_freq[cls][freq_bin(hz)]++;
        float coherence = sqrtf(l->run_re * l->run_re + l->run_im * l->run_im) / l->run_p;
        l->stats.run_coherence[cls][coherence_bin(coherence)]++;
    }
}

static void run_segment(halo_listen_t *l, uint32_t s, bool hot)
{
    if (hot) {
        if (!l->in_run) {
            l->in_run = true;
            l->run_start = s;
            l->run_segs = 0;
            l->run_re = l->run_im = l->run_p = 0;
        }
        l->run_segs++;
        l->run_gap = 0;
        if (l->run_segs >= MIN_RUN || !l->gate || l->warm < WARMUP) {
            if (!l->open) {
                l->open = true;
                l->w0 = later(l->run_start - MARGIN, l->earliest);
            }
            l->w_end = s + SEG + MARGIN;
        }
    } else if (l->in_run && ++l->run_gap > MAX_GAP) {
        l->in_run = false;
        count_run(l);
    }
}

/* Lag-1 correlation over power: ~0.9 for a Halo frame (narrow), ~0 for white
 * noise and for wideband Wi-Fi, which the block sums whiten. 0 for anything
 * centred too far off our carrier to be a Halo frame. Only worked out for
 * segments that are loud already: half the gate's cost saved here.
 */
static float segment_coherence(halo_listen_t *l, uint32_t s, float mi, float mq, float var, float *seg_re,
                               float *seg_im)
{
    const int16_t *bi = l->src->bi + (s & MASK), *bq = l->src->bq + (s & MASK);
    int32_t pi = l->src->bi[(s - 1) & MASK] >> 1, pq = l->src->bq[(s - 1) & MASK] >> 1;
    int32_t re = 0, im = 0;
    for (unsigned b = 0; b < SEG; b++) {
        int32_t i = bi[b] >> 1, q = bq[b] >> 1;
        re += i * pi + q * pq;   /* b[n] * conj(b[n-1]) */
        im += q * pi - i * pq;
        pi = i;
        pq = q;
    }
    float cre = (float)re / SEG - mi * mi - mq * mq, cim = (float)im / SEG;
    *seg_re = cre;
    *seg_im = cim;
    float mag = sqrtf(cre * cre + cim * cim);
    /* The correlation's angle is the segment's mean frequency. */
    if (cre < MAX_STEP_COS * mag) {
        l->stats.off_band++;
        return 0;
    }
    return var > 0 ? mag / var : 0;
}

/* Variance, not raw power: the S3's DC offset would otherwise look like signal. */
static void check_segment(halo_listen_t *l, uint32_t s)
{
    if (!l->open && (int32_t)(s - HALO_BLOCKS_RING / 2 - l->earliest) > 0) {
        l->earliest = s - HALO_BLOCKS_RING / 2;   /* keep it near, or the wrapping compare goes wrong */
    }
    /* Halved so every sum fits 32 bits, which the S3 adds far faster than 64;
     * the noise is ~50 LSB, so no precision is lost. A segment never crosses
     * the ring's end.
     */
    const int16_t *bi = l->src->bi + (s & MASK), *bq = l->src->bq + (s & MASK);
    int32_t si = 0, sq = 0, ss = 0;
    for (unsigned b = 0; b < SEG; b++) {
        int32_t i = bi[b] >> 1, q = bq[b] >> 1;
        si += i;
        sq += q;
        ss += i * i + q * q;
    }
    float mi = (float)si / SEG, mq = (float)sq / SEG;
    float var = (float)ss / SEG - mi * mi - mq * mq;
    bool loud = false, warming = l->warm < WARMUP;
    if (warming) {
        /* Minimum, not mean: a frame early on would otherwise hide the next ones. */
        if (l->warm == 0 || var < l->floor) {
            l->floor = var;
        }
        l->warm++;
    } else if (var > LOUD * l->floor) {
        float seg_re, seg_im;
        float coherence = segment_coherence(l, s, mi, mq, var, &seg_re, &seg_im);
        l->stats.coherence[coherence_bin(coherence)]++;
        loud = coherence > COHERENT;
        if (loud) {
            l->run_re += seg_re;
            l->run_im += seg_im;
            l->run_p += var;
        }
        l->floor *= 1.0f + 1.0f / 1024;   /* creep up, so a lasting rise in noise stops counting as loud */
    } else {
        l->floor += (var - l->floor) * (1.0f / 64);
    }
    if (l->floor < MIN_FLOOR) {
        l->floor = MIN_FLOOR;
    }
    uint32_t k = (s / SEG) & (NSEG - 1u);
    l->loud_bits[k / 32] = (l->loud_bits[k / 32] & ~(1u << (k % 32))) | (uint32_t)loud << (k % 32);
    l->stats.segments++;
    l->stats.loud += loud;
    l->stats.floor = l->floor;
    run_segment(l, s, loud || warming || !l->gate);   /* no floor yet: demod it all, once at boot */
}

static bool duplicate(halo_listen_t *l, uint32_t block)
{
    for (unsigned k = 0; k < COUNTOF(l->recent); k++) {
        uint32_t d = block - l->recent[k];
        if (d + DUP < 2 * DUP) {   /* |d| < DUP */
            return true;
        }
    }
    l->recent[l->next_recent++ % COUNTOF(l->recent)] = block;
    return false;
}

static size_t process(halo_listen_t *l, uint32_t w0, uint32_t w1, halo_heard_t *out, size_t max)
{
    l->stats.windows++;
    /* Room for the quick DC pass, which reads the whole window first. */
    if (too_old(l, w0 - SEG * 4u)) {
        l->stats.dropped++;
        return 0;
    }
    size_t n = halo_fm_demod_ring(l->src->bi, l->src->bq, MASK, w0 & MASK, w1 - w0, l->hz, l->window - 1);
    /* The main pass reads oldest first and slower than the ring fills (~1 vs 2
     * blocks per us on the S3), so the last block read is the most at risk.
     */
    if (too_old(l, w1 - 1)) {
        l->stats.dropped++;
        return 0;
    }
    l->stats.demod_blocks += w1 - w0;

    halo_detection_t found[MAX_FOUND];
    size_t k = halo_detect(l->hz, n, found, MAX_FOUND), count = 0;
    for (size_t j = 0; j < k && count < max; j++) {
        uint32_t block = w0 + found[j].start_sample + 1;   /* hz[n] sits at block n + 1 */
        if (duplicate(l, block)) {
            l->stats.dupes++;
            continue;
        }
        uint32_t len = (uint32_t)halo_esb_frame_bits(found[j].frame.length) * HALO_FM_SPB;
        unsigned loud = 0;
        for (uint32_t s = block & ~(SEG - 1u); (int32_t)(s - (block + len)) < 0; s += SEG) {
            loud += seg_loud(l, s);
        }
        out[count++] = (halo_heard_t){.d = found[j], .block = block, .loud = (uint8_t)loud};
        l->stats.frames++;
    }
    return count;
}

bool halo_listen_poll(halo_listen_t *l, halo_heard_t *out, size_t max, size_t *count)
{
    *count = 0;
    uint32_t written = atomic_load_explicit(&l->src->written, memory_order_acquire);
    if (written - l->checked > SAFE_LAG) {
        /* Too far behind: what's left may be overwritten. Start again from now. */
        l->stats.dropped++;
        l->checked = l->earliest = written & ~(SEG - 1u);
        l->open = false;
    }
    while (written - l->checked >= SEG) {
        check_segment(l, l->checked);
        l->checked += SEG;
        if (!l->open) {
            continue;
        }
        if ((int32_t)(l->checked - l->w_end) >= 0) {
            l->open = false;
            l->earliest = l->w_end - SPAN;
            *count = process(l, l->w0, l->w_end, out, max);
            return true;
        }
        if (l->checked - l->w0 >= l->window) {
            /* A long loud stretch: overlap by SPAN so no frame falls between windows. */
            uint32_t w1 = l->w0 + (uint32_t)l->window;
            l->w0 = l->earliest = w1 - SPAN;
            *count = process(l, w1 - (uint32_t)l->window, w1, out, max);
            return true;
        }
    }
    return false;
}
