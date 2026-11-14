/* SPDX-License-Identifier: GPL-3.0-only */
#include "halo_detect.h"

#include <math.h>

#include "halo_fm.h"

#define SPB             HALO_FM_SPB
#define PREAMBLE        8u
#define BODY_MAX        (HALO_ESB_MAX_BITS - PREAMBLE)
#define MIN_SWING_HZ    80000.0f   /* real preambles reach ~120 kHz; noise rarely alternates this cleanly */
#define MAX_SWING_HZ    250000.0f  /* +-160 kHz deviation plus noise; anything wilder is noise */
#define WEAK_HZ         30000.0f
#define ALIGN_SEARCH    12u        /* samples after the first hit to look for a better-centred one */
_Static_assert(BODY_MAX <= 255, "weak_bits can count every bit");
_Static_assert(HALO_FM_RATE % 1000000u == 0, "whole samples per microsecond");

static float centre(const float *hz, size_t p, unsigned bit)
{
    return hz[p + SPB / 2 + bit * SPB];
}

/* Whether a preamble starts at p; if so, its offset and summed swing. */
static bool preamble_at(const float *hz, size_t p, float *offset, float *swing)
{
    /* Cheap reject first, since almost every position fails: neighbours must
     * differ by two swings, alternately up and down. A real preamble steps
     * ~240-320 kHz; the upper bound stops most noise (+-1 MHz) at the first read.
     */
    float prev = centre(hz, p, 0), last_step = 0;
    for (unsigned k = 1; k < PREAMBLE; k++) {
        float c = centre(hz, p, k), step = c - prev, size = fabsf(step);
        if (size < 2 * MIN_SWING_HZ || size > 2 * MAX_SWING_HZ || (k > 1 && (step > 0) == (last_step > 0))) {
            return false;
        }
        last_step = step;
        prev = c;
    }

    float sum = 0;
    for (unsigned k = 0; k < PREAMBLE; k++) {
        sum += centre(hz, p, k);
    }
    float mean = sum / PREAMBLE, total = 0, last = 0;
    for (unsigned k = 0; k < PREAMBLE; k++) {
        float d = centre(hz, p, k) - mean;
        if (fabsf(d) < MIN_SWING_HZ || (k && (d > 0) == (last > 0))) {
            return false;
        }
        total += fabsf(d);
        last = d;
    }
    *offset = mean;
    *swing = total;
    return true;
}

static bool decode_at(const float *hz, size_t n, size_t p, float offset, halo_detection_t *d)
{
    uint8_t bits[BODY_MAX];
    size_t count = 0;
    while (count < BODY_MAX && p + SPB / 2 + (PREAMBLE + count) * SPB < n) {
        bits[count] = centre(hz, p, PREAMBLE + (unsigned)count) > offset;
        count++;
    }
    if (!halo_esb_decode(bits, count, &d->frame)) {
        return false;
    }
    size_t used = halo_esb_frame_bits(d->frame.length) - PREAMBLE;
    unsigned weak = 0;
    for (size_t k = 0; k < used; k++) {
        weak += fabsf(centre(hz, p, PREAMBLE + (unsigned)k) - offset) < WEAK_HZ;
    }
    d->offset_hz = offset;
    d->start_sample = (uint32_t)p;
    d->start_us = (uint32_t)((p + 1) / (HALO_FM_RATE / 1000000u));   /* hz[p] sits at block p + 1 */
    d->weak_bits = (uint8_t)weak;
    return true;
}

size_t halo_detect(const float *hz, size_t n, halo_detection_t *out, size_t max)
{
    size_t found = 0;
    const size_t span = SPB / 2 + PREAMBLE * SPB;
    for (size_t p = 0; p + span < n && found < max; p++) {
        float first_offset, first_swing;
        if (!preamble_at(hz, p, &first_offset, &first_swing)) {
            continue;
        }
        /* The first position that passes sits at the edge of the eye; a few
         * samples later is usually better centred, which gives cleaner bits
         * and a truer offset and start time.
         */
        size_t best = p;
        float best_offset = first_offset, best_swing = first_swing;
        for (size_t q = p + 1; q <= p + ALIGN_SEARCH && q + span < n; q++) {
            float o, s;
            if (preamble_at(hz, q, &o, &s) && s > best_swing) {
                best_swing = s;
                best = q;
                best_offset = o;
            }
        }
        halo_detection_t d;
        size_t at = best;
        bool ok = decode_at(hz, n, best, best_offset, &d);
        if (!ok) {
            at = p;
            ok = decode_at(hz, n, p, first_offset, &d);
        }
        if (ok) {
            out[found++] = d;
            p = at + halo_esb_frame_bits(d.frame.length) * SPB - 1;   /* skip the rest of this frame */
        }
    }
    return found;
}

const halo_detection_t *halo_detect_find_ack(const halo_detection_t *found, size_t count, uint32_t address,
                                             uint8_t pid)
{
    for (size_t k = 0; k < count; k++) {
        if (halo_esb_is_ack(&found[k].frame, address, pid)) {
            return &found[k];
        }
    }
    return NULL;
}
