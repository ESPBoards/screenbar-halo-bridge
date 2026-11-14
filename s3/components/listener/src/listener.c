/* SPDX-License-Identifier: GPL-3.0-only */
#include "listener.h"

#include <stdatomic.h>

#include "esp_cpu.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "halo_blocks.h"
#include "phy_s3.h"

#define REGAIN_CHUNKS    20u     /* ~20 ms, as esp-sdr-zephyr suggests against AGC drift */
#define HZ_MAX           6143u   /* 3 ms windows: a command and its ACK nearly fit in one */
#define CYCLES_PER_US    PHY_S3_CYCLES_PER_US
#define CYCLES_PER_BLOCK (HALO_FM_DECIMATION * PHY_S3_RING_CYCLES_PER_SAMPLE)
#define EVENTS           64u     /* ~60 ms of seams, far more than the worker ever lags */
#define FRAMES_PER_POLL  8u      /* a poll demods one window, which holds a few frames at most */
#define REST_EVERY_US    50000   /* longest the worker hogs core 0 while busy */
#define WORKER_STACK     6144
#define WORKER_CORE      0

#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

_Static_assert(2 * HALO_BLOCKS_RING * sizeof(int16_t) <= PHY_S3_SPARE_BYTES, "block ring must fit the spare bank");
_Static_assert(HZ_MAX >= 3 * HALO_LISTEN_SPAN, "halo_listen needs room for three spans");

enum { EV_NONE, EV_SEAM, EV_REGAIN, EV_RESTART };

static halo_blocks_t blocks;
static halo_listen_t state;
static float hz[HZ_MAX];
/* Each field has one writer, the ring loop or the worker, and is one word, so a
 * reader's copy never tears a field. Peaks are reset by their writer, on request.
 */
static listener_stats_t stats;
static atomic_bool reset_ring_peaks, reset_worker_peaks;
static listener_frame_fn frame_fn;
static void *frame_ctx;

static struct {
    uint32_t block;
    uint8_t kind;
} events[EVENTS];
static atomic_uint next_event;

static void event(uint32_t block, uint8_t kind)
{
    unsigned k = atomic_load(&next_event) % EVENTS;
    events[k].block = block;
    events[k].kind = kind;
    atomic_fetch_add(&next_event, 1);
}

static void feed(const phy_s3_chunk_t *c)
{
    if (c->from < c->to) {
        halo_blocks_feed(&blocks, c->samples + c->from, c->to - c->from);
    } else {
        halo_blocks_feed(&blocks, c->samples + c->from, PHY_S3_RING_SAMPLES - c->from);
        halo_blocks_feed(&blocks, c->samples, c->to);
    }
}

/* Never blocks while the ring runs: each lap must be read before the next flip. */
void listener_ring(bool (*keep_going)(void *ctx), void *ctx)
{
    static unsigned since_regain;
    while (keep_going(ctx)) {
        phy_s3_ring_t ring;
        phy_s3_chunk_t c;
        if (!phy_s3_ring_start(&ring, &c)) {
            stats.start_failed++;
            vTaskDelay(1);
            continue;
        }
        uint32_t flipped = esp_cpu_get_cycle_count();
        stats.starts++;
        halo_blocks_break(&blocks);
        event(atomic_load(&blocks.written), EV_RESTART);
        for (;;) {
            if (atomic_load_explicit(&reset_ring_peaks, memory_order_relaxed)) {
                atomic_store_explicit(&reset_ring_peaks, false, memory_order_relaxed);
                stats.feed_us_max = stats.regain_us_max = 0;
                stats.slack_us_min = UINT32_MAX;
            }
            uint32_t t0 = esp_cpu_get_cycle_count();
            feed(&c);
            uint32_t t1 = esp_cpu_get_cycle_count(), us = (t1 - t0) / CYCLES_PER_US;
            stats.chunks++;
            stats.feed_us_total += us;
            stats.feed_us_max = us > stats.feed_us_max ? us : stats.feed_us_max;

            if (++since_regain == REGAIN_CHUNKS) {
                since_regain = 0;
                /* The ADC is this far into the lap after the one just fed. */
                uint32_t block = atomic_load(&blocks.written) + (t1 - flipped) / CYCLES_PER_BLOCK;
                phy_s3_ring_regain();
                us = (esp_cpu_get_cycle_count() - t1) / CYCLES_PER_US;
                stats.regains++;
                stats.regain_us_max = us > stats.regain_us_max ? us : stats.regain_us_max;
                event(block, EV_REGAIN);
            }
            if (phy_s3_ring_stale()) {
                stats.stale++;
                phy_s3_ring_stop();
                break;
            }
            if (!keep_going(ctx)) {
                phy_s3_ring_stop();
                return;
            }
            int32_t slack = (int32_t)(ring.next - esp_cpu_get_cycle_count()) / (int32_t)CYCLES_PER_US;
            if (slack >= 0 && (uint32_t)slack < stats.slack_us_min) {
                stats.slack_us_min = (uint32_t)slack;
            }
            if (!phy_s3_ring_next(&ring, &c)) {
                stats.late++;
                break;
            }
            flipped = esp_cpu_get_cycle_count();
            event(atomic_load(&blocks.written), EV_SEAM);
        }
    }
}

static void worker_task(void *arg)
{
    (void)arg;
    halo_heard_t out[FRAMES_PER_POLL];
    int64_t rested = esp_timer_get_time();
    for (;;) {
        int64_t a = esp_timer_get_time();
        uint32_t windows = state.stats.windows;
        size_t n;
        bool busy = halo_listen_poll(&state, out, COUNTOF(out), &n);
        int64_t b = esp_timer_get_time();
        stats.worker_us += (uint32_t)(b - a);
        if (state.stats.windows != windows) {
            stats.window_us += (uint32_t)(b - a);
        }
        if (atomic_load_explicit(&reset_worker_peaks, memory_order_relaxed)) {
            atomic_store_explicit(&reset_worker_peaks, false, memory_order_relaxed);
            stats.lag_blocks_max = 0;
        }
        uint32_t lag = atomic_load(&blocks.written) - state.checked;
        stats.lag_blocks_max = lag > stats.lag_blocks_max ? lag : stats.lag_blocks_max;
        stats.listen = state.stats;
        for (size_t j = 0; j < n; j++) {
            frame_fn(&out[j], frame_ctx);
        }
        /* Rest when idle, and now and then anyway so core 0's idle task (watchdog) runs. */
        if (!busy || b - rested > REST_EVERY_US) {
            vTaskDelay(1);
            rested = esp_timer_get_time();
        }
    }
}

void listener_start(listener_frame_fn on_frame, void *ctx)
{
    int16_t *spare = phy_s3_spare();
    halo_blocks_init(&blocks, spare, spare + HALO_BLOCKS_RING);
    halo_listen_init(&state, &blocks, hz, HZ_MAX, true);
    frame_fn = on_frame;
    frame_ctx = ctx;
    stats.slack_us_min = UINT32_MAX;
    BaseType_t ok = xTaskCreatePinnedToCore(worker_task, "listen", WORKER_STACK, NULL, LISTENER_WORKER_PRIORITY, NULL,
                                            WORKER_CORE);
    configASSERT(ok == pdPASS);
}

void listener_audit(bool on)
{
    state.gate = !on;   /* a plain bool the worker reads each segment; a stale read costs nothing */
}

void listener_stats(listener_stats_t *out)
{
    *out = stats;
}

void listener_reset_peaks(void)
{
    atomic_store_explicit(&reset_ring_peaks, true, memory_order_relaxed);
    atomic_store_explicit(&reset_worker_peaks, true, memory_order_relaxed);
}

void listener_events(uint32_t from, uint32_t to, unsigned *seams, unsigned *regains, unsigned *restarts)
{
    *seams = *regains = *restarts = 0;
    for (unsigned k = 0; k < EVENTS; k++) {
        uint32_t at = events[k].block;
        if ((int32_t)(at - from) <= 0 || (int32_t)(to - at) <= 0) {
            continue;
        }
        *seams += events[k].kind == EV_SEAM;
        *regains += events[k].kind == EV_REGAIN;
        *restarts += events[k].kind == EV_RESTART;
    }
}
