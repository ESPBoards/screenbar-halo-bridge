/* SPDX-License-Identifier: GPL-3.0-only */
/* Always-on listener. Core 1 keeps the ADC ring running and folds each lap
 * into block sums; core 0 gates on energy and demods only around loud
 * stretches. The radio is the listener's while listener_ring runs.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "halo_listen.h"

/* The worker's priority on core 0, for tasks that must preempt its demodding. */
#define LISTENER_WORKER_PRIORITY 5

/* Runs on core 0, in the listener's own task. */
typedef void (*listener_frame_fn)(const halo_heard_t *heard, void *ctx);

typedef struct {
    /* core 1 */
    uint32_t chunks, starts, start_failed, late, stale, regains;
    uint32_t feed_us_max, slack_us_min, regain_us_max;
    uint32_t feed_us_total;   /* totals wrap after ~71 min: compare by difference */
    /* core 0 */
    uint32_t worker_us, window_us;   /* window_us: polls that demodded a window */
    uint32_t lag_blocks_max;
    halo_listen_stats_t listen;
} listener_stats_t;

/* Starts the worker on core 0. Nothing is heard until someone runs listener_ring. */
void listener_start(listener_frame_fn on_frame, void *ctx);

/* Runs the ring on the calling core until keep_going says stop, restarting it
 * after a late flip or a PHY recalibration. Call it from a task on core 1 that
 * nothing else needs: it never blocks while the ring runs. keep_going is asked
 * once per lap (~1 ms), so it must be quick.
 */
void listener_ring(bool (*keep_going)(void *ctx), void *ctx);

/* Diagnostics: demod everything for a while, to see which frames the gate would have missed. */
void listener_audit(bool on);

/* A snapshot, each field whole but not all from the same moment. */
void listener_stats(listener_stats_t *out);
void listener_reset_peaks(void);   /* takes effect within a lap */

/* Diagnostics: lap seams, gain re-forces and ring restarts between two stream positions. */
void listener_events(uint32_t from, uint32_t to, unsigned *seams, unsigned *regains, unsigned *restarts);
