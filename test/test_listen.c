/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "channel.h"
#include "check.h"
#include "frames.h"
#include "halo_blocks.h"
#include "halo_esb.h"
#include "halo_fm.h"
#include "halo_listen.h"
#include "prng.h"

#define RING    16384u               /* the S3's ADC ring */
#define STREAM  (64u * RING)         /* ~65 ms */
#define HZ_MAX  6143u                /* as on the S3 */
#define FRAMES  40u
#define AMP     100.0
#define WARMUP_BLOCKS (64u * HALO_LISTEN_SEG + 2 * 4u * HALO_LISTEN_SEG)   /* the boot warmup plus its margins */

typedef struct {
    halo_esb_frame_t f;
    long start;
    double offset;
    unsigned seed, hits;
} placed_t;

static uint32_t stream[STREAM], tmp[CH_SAMPLES];
static int16_t ring_i[HALO_BLOCKS_RING], ring_q[HALO_BLOCKS_RING];
static float hz[HZ_MAX];
static placed_t placed[FRAMES];
static unsigned nplaced;
static long burst_every;   /* samples between wideband bursts; 0 for none */
static double burst_hz;    /* 0: white noise bursts; else a tone this far off the carrier */
static halo_blocks_t blocks;
static halo_listen_t listen;

static long frame_samples(const halo_esb_frame_t *f)
{
    return (long)halo_esb_frame_bits(f->length) * CH_SPB;
}

/* Commands at random gaps, some followed by an ACK 184 us later, the way the lamp answers. */
static void place(unsigned count)
{
    nplaced = 0;
    long at = 2000 + test_rand() % 8000;
    while (nplaced < count) {
        placed_t *p = &placed[nplaced++];
        p->f = random_frame(false);
        p->start = at;
        p->offset = (ch_urand() * 2 - 1) * 120000;
        p->seed = (unsigned)test_rand();
        p->hits = 0;
        at += frame_samples(&p->f);
        if (nplaced < count && test_rand() % 2) {
            placed_t *a = &placed[nplaced++];
            a->f = random_frame(true);
            a->start = at + 184 * 16;
            a->offset = p->offset + 5000;
            a->seed = (unsigned)test_rand();
            a->hits = 0;
            at = a->start + frame_samples(&a->f);
        }
        at += 2000 + test_rand() % 20000;
        if (at + 30000 > (long)STREAM) {
            break;
        }
    }
}

/* channel.h works in 16380-sample pieces. Frames are re-seeded per piece so a
 * frame crossing two pieces keeps its phase.
 */
static void render(double snr_db, double dc_i, double dc_q, unsigned noise_seed)
{
    for (size_t w0 = 0; w0 < STREAM; w0 += CH_SAMPLES) {
        ch_reset(dc_i, dc_q);
        for (unsigned k = 0; k < nplaced; k++) {
            const placed_t *p = &placed[k];
            long rel = p->start - (long)w0;
            if (rel + frame_samples(&p->f) + 4 * CH_SPB < 0 || rel - 4 * CH_SPB > CH_SAMPLES) {
                continue;
            }
            uint8_t bits[HALO_ESB_MAX_BITS];
            size_t n = halo_esb_encode(&p->f, bits);
            test_srand(p->seed);
            ch_add_frame(bits, n, rel, p->offset, AMP);
        }
        test_srand(noise_seed + (unsigned)w0);
        ch_add_noise(AMP, snr_db);
        for (size_t j = 0; burst_every && j < CH_SAMPLES; j++) {
            if ((long)(w0 + j) % burst_every < 25600) {   /* 1.6 ms, like a Wi-Fi beacon */
                if (burst_hz) {
                    double ph = 2 * CH_PI * burst_hz * (double)(w0 + j) / CH_RATE;
                    ch_i[j] += AMP * cos(ph);
                    ch_q[j] += AMP * sin(ph);
                } else {
                    ch_i[j] += ch_gauss() * AMP;
                    ch_q[j] += ch_gauss() * AMP;
                }
            }
        }
        ch_render(tmp);
        size_t n = STREAM - w0 < CH_SAMPLES ? STREAM - w0 : CH_SAMPLES;
        memcpy(stream + w0, tmp, n * sizeof(uint32_t));
    }
}

typedef struct {
    unsigned found, wrong, misplaced, across;
} result_t;

static void heard(const halo_heard_t *h, size_t n, result_t *r)
{
    for (size_t j = 0; j < n; j++) {
        bool matched = false;
        for (unsigned k = 0; k < nplaced; k++) {
            placed_t *p = &placed[k];
            if (!same_frame(&h[j].d.frame, &p->f)) {
                continue;
            }
            matched = true;
            p->hits++;
            long err = (long)h[j].block - p->start / 8;
            r->misplaced += err < -6 || err > 6;
            /* Lap boundaries stand in for the seams, which sit up to 84 samples
             * earlier (first + k * RING): close enough to show frames cross them.
             */
            r->across += p->start / RING != (p->start + frame_samples(&p->f)) / RING;
        }
        r->wrong += !matched;
        r->found++;
    }
}

/* Feeds the stream the way the S3 delivers it: a short first chunk up to the
 * first flip, then whole laps, each split at the ring wrap. Polls after each.
 */
static result_t run(bool gate, size_t first, size_t unpolled_from, size_t unpolled_to)
{
    halo_blocks_init(&blocks, ring_i, ring_q);
    halo_listen_init(&listen, &blocks, hz, HZ_MAX, gate);
    result_t r = {0};
    for (unsigned k = 0; k < nplaced; k++) {
        placed[k].hits = 0;
    }
    halo_heard_t out[8];
    size_t at = 0, chunk = 0;
    while (at < STREAM) {
        size_t len = at ? RING : first;
        len = len < STREAM - at ? len : STREAM - at;
        size_t wrap = len > 64 ? 64 : len;   /* tail of the old lap, then the new lap */
        halo_blocks_feed(&blocks, stream + at, wrap);
        halo_blocks_feed(&blocks, stream + at + wrap, len - wrap);
        at += len;
        if (chunk < unpolled_from || chunk >= unpolled_to) {
            size_t n;
            while (halo_listen_poll(&listen, out, 8, &n)) {
                heard(out, n, &r);
            }
        }
        chunk++;
    }
    return r;
}

static unsigned missed_or_twice(void)
{
    unsigned bad = 0;
    for (unsigned k = 0; k < nplaced; k++) {
        bad += placed[k].hits != 1;
    }
    return bad;
}

/* Splitting the samples anywhere, even mid-block, must give exactly the blocks of the whole. */
static void test_pieces(void)
{
    /* More than one ring's worth, so the ring wraps and the comparison covers all of it. */
    enum { SAMPLES = 200000, BLOCKS = SAMPLES / HALO_FM_DECIMATION };
    _Static_assert(BLOCKS > HALO_BLOCKS_RING, "the ring must wrap");
    _Static_assert(SAMPLES <= STREAM, "stream too small");
    static int16_t whole_i[BLOCKS], whole_q[BLOCKS];
    for (size_t j = 0; j < SAMPLES; j++) {
        stream[j] = halo_iq_pack(test_rand() % 1024 - 512, test_rand() % 1024 - 512);
    }
    halo_fm_blocks(stream, SAMPLES, whole_i, whole_q);
    for (int t = 0; t < 20; t++) {
        halo_blocks_init(&blocks, ring_i, ring_q);
        size_t at = 0;
        while (at < SAMPLES) {
            size_t n = 1 + (size_t)test_rand() % (t < 10 ? 20 : 5000);
            n = n < SAMPLES - at ? n : SAMPLES - at;
            halo_blocks_feed(&blocks, stream + at, n);
            at += n;
        }
        CHECK(atomic_load(&blocks.written) == BLOCKS, "%u blocks", (unsigned)atomic_load(&blocks.written));
        unsigned diff = 0, compared = 0;
        for (size_t b = BLOCKS - HALO_BLOCKS_RING; b < BLOCKS; b++) {
            diff += ring_i[b % HALO_BLOCKS_RING] != whole_i[b] || ring_q[b % HALO_BLOCKS_RING] != whole_q[b];
            compared++;
        }
        CHECK(compared == HALO_BLOCKS_RING && diff == 0, "pieces %d: %u of %u blocks differ", t, diff, compared);
    }
}

/* After a gap in the samples, a block must not mix samples from either side of it. */
static void test_break(void)
{
    int16_t want_i[1], want_q[1];
    for (size_t j = 0; j < 11; j++) {
        stream[j] = halo_iq_pack(test_rand() % 1024 - 512, test_rand() % 1024 - 512);
    }
    halo_fm_blocks(stream + 3, HALO_FM_DECIMATION, want_i, want_q);
    halo_blocks_init(&blocks, ring_i, ring_q);
    halo_blocks_feed(&blocks, stream, 3);
    halo_blocks_break(&blocks);
    halo_blocks_feed(&blocks, stream + 3, HALO_FM_DECIMATION);
    CHECK(atomic_load(&blocks.written) == 1 && ring_i[0] == want_i[0] && ring_q[0] == want_q[0],
          "partial block kept across a break");
}

static void test_every_frame_once(void)
{
    unsigned across = 0, total = 0, dupes = 0;
    uint64_t demod = 0, blocks_total = 0;
    for (int t = 0; t < 6; t++) {
        place(FRAMES);
        render(10, 3, 8, (unsigned)t);
        size_t first = 16300 + (size_t)test_rand() % 80;
        for (int gate = 1; gate >= 0; gate--) {
            result_t r = run(gate, first, 0, 0);
            CHECK(missed_or_twice() == 0 && r.wrong == 0 && r.misplaced == 0,
                  "stream %d gate %d: %u of %u not found exactly once, %u wrong, %u misplaced", t, gate,
                  missed_or_twice(), nplaced, r.wrong, r.misplaced);
            if (gate) {
                across += r.across;
                total += nplaced;
                demod += listen.stats.demod_blocks;
                blocks_total += STREAM / 8;
            } else {
                dupes += listen.stats.dupes;
            }
        }
    }
    printf("  10 dB: %u frames each found once, %u of them across a chunk seam; "
           "gate demods %.1f%% of this dense traffic\n",
           total, across, 100.0 * (double)demod / (double)blocks_total);
    CHECK(across > total / 3, "too few frames across seams to prove anything (%u)", across);
    CHECK(dupes > 0, "no frame seen twice in overlapping windows, so the dedupe went untested");
}

/* The gate must not cost frames, weak ones included, compared with demodding
 * everything. Near the detector's limit a shorter window shifts the DC
 * estimate a little, so single frames come and go either way: at 0 dB that's
 * within 2 %. At -3 dB only about half the frames are found at all, and over
 * 20 random sequences the difference swung from -3 to +9, so it's only shown.
 */
static void test_gate_loses_nothing(void)
{
    const double snrs[] = {-3, 0, 2, 4};
    for (unsigned s = 0; s < 4; s++) {
        unsigned with = 0, without = 0, total = 0;
        for (int t = 0; t < 4; t++) {
            place(FRAMES);
            render(snrs[s], 3, 8, 100u + (unsigned)t);
            run(false, 16340, 0, 0);
            for (unsigned k = 0; k < nplaced; k++) {
                without += placed[k].hits == 1;
            }
            run(true, 16340, 0, 0);
            for (unsigned k = 0; k < nplaced; k++) {
                with += placed[k].hits == 1;
            }
            total += nplaced;
        }
        printf("  %4.0f dB: found %u of %u with the gate, %u without\n", snrs[s], with, total, without);
        if (snrs[s] < 0) {
            continue;
        }
        unsigned slack = snrs[s] > 0 ? 0 : total / 50;
        CHECK(with + slack >= without, "%.0f dB: the gate lost %u frames", snrs[s], without - with);
    }
}

static void test_quiet(void)
{
    nplaced = 0;
    render(0, 3, 8, 7);   /* noise only, at the level of a 0 dB frame */
    result_t r = run(true, 16340, 0, 0);
    printf("  noise only: %u of %u segments loud, %u blocks demodded, %u frames\n", listen.stats.loud,
           listen.stats.segments, listen.stats.demod_blocks, r.found);
    CHECK(r.found == 0 && r.wrong == 0, "frames in pure noise");
    CHECK(listen.stats.loud * 1000 < listen.stats.segments, "%u loud segments in noise", listen.stats.loud);
}

/* Wi-Fi shares the band. It's wideband, so the block sums whiten it, and it
 * must not open windows however loud it is.
 */
static void test_wideband(void)
{
    nplaced = 0;
    burst_every = 64000;
    render(0, 3, 8, 13);
    result_t r = run(true, 16340, 0, 0);
    unsigned over = 0;
    for (unsigned c = 0; c < HALO_LISTEN_COHERENCE_BINS; c++) {
        over += listen.stats.coherence[c];
    }
    printf("  wideband bursts: %u segments over the power threshold, %u loud, %u blocks demodded\n", over,
           listen.stats.loud, listen.stats.demod_blocks);
    CHECK(over > listen.stats.segments / 4, "the bursts didn't register (%u)", over);
    CHECK(listen.stats.loud * 100 < over, "%u of %u burst segments loud", listen.stats.loud, over);
    CHECK(r.found == 0 && listen.stats.demod_blocks <= WARMUP_BLOCKS, "%u blocks demodded", listen.stats.demod_blocks);
    burst_every = 0;
}

/* Interferers seen on hardware were narrowband but ~900 and ~400 kHz off our
 * carrier: coherent, so only their frequency gives them away. Halo frames at
 * the edge of the detector's offset range must still pass.
 */
static void test_off_band(void)
{
    const double tones[] = {-900000, 400000};
    for (unsigned t = 0; t < 2; t++) {
        nplaced = 0;
        burst_every = 64000;
        burst_hz = tones[t];
        render(0, 3, 8, 15);
        run(true, 16340, 0, 0);
        printf("  tone bursts at %+.0f kHz: %u segments off band, %u loud, %u blocks demodded\n", tones[t] / 1000,
               listen.stats.off_band, listen.stats.loud, listen.stats.demod_blocks);
        CHECK(listen.stats.off_band > listen.stats.segments / 4, "bursts not seen as off band (%u)",
              listen.stats.off_band);
        CHECK(listen.stats.demod_blocks <= WARMUP_BLOCKS, "%u blocks demodded", listen.stats.demod_blocks);
    }
    burst_every = 0;
    burst_hz = 0;

    for (int edge = -1; edge <= 1; edge += 2) {
        place(FRAMES);
        for (unsigned k = 0; k < nplaced; k++) {
            placed[k].offset = edge * 200000.0;
        }
        render(4, 3, 8, 16);
        run(true, 16340, 0, 0);
        CHECK(missed_or_twice() == 0, "offset %+d kHz: %u of %u frames missed", edge * 200, missed_or_twice(), nplaced);
    }
}

/* Falling behind must lose only what was overwritten, then carry on. */
static void test_lag(void)
{
    place(FRAMES);
    render(10, 3, 8, 9);
    run(true, 16340, 10, 22);   /* 12 chunks unread: more than the ring holds */
    CHECK(listen.stats.dropped > 0, "no drop reported");
    unsigned late = 0, late_found = 0;
    for (unsigned k = 0; k < nplaced; k++) {
        if (placed[k].start > 24 * (long)RING) {
            late++;
            late_found += placed[k].hits == 1;
        }
    }
    CHECK(late && late_found == late, "after the drop: %u of %u found", late_found, late);
}

static void report_speed(void)
{
    place(FRAMES);
    render(10, 3, 8, 11);
    clock_t t0 = clock();
    run(true, 16340, 0, 0);
    printf("  gated listen over %.0f ms of stream: %.1f ms on this computer\n", STREAM / 16000.0,
           (double)(clock() - t0) / CLOCKS_PER_SEC * 1e3);
}

int main(void)
{
    test_srand(61);
    test_pieces();
    test_break();
    test_every_frame_once();
    test_gate_loses_nothing();
    test_quiet();
    test_wideband();
    test_off_band();
    test_lag();
    report_speed();
    return check_report("test_listen");
}
