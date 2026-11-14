/* SPDX-License-Identifier: GPL-3.0-only */
/* The always-on listener, printed once a second. A FRAME marked ACROSS spanned
 * a lap seam (bank flip plus ring wrap), REGAIN one of the RX gain re-forces.
 */
#include <stdio.h>

#include "bench.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "listener.h"

#define AUDIT_EVERY 4   /* every Nth second demods everything, to catch what the gate misses; 0: never */
#define GATE_DETAIL 0   /* 1: also print run and coherence histograms, to tune the gate */
#define TASK_STATS  0   /* 1: CPU per task. Its kernel lock stalls core 1's tick and can make a flip late */
#define GATE_RUN    12  /* loud segments the gate needs; fewer under a frame means it could have been missed */

#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

typedef struct {
    halo_heard_t h;
    unsigned seams, regains, restarts;
    bool audit;
} line_t;

static QueueHandle_t lines;
static bool auditing;
static unsigned frames, across, regained, gate_would_miss;

/* Runs in the listener's worker: queue, never print. The USB console moves
 * ~64 bytes per ms and would stall the worker long enough to lose windows.
 */
static void on_heard(const halo_heard_t *h, void *ctx)
{
    (void)ctx;
    line_t line = {.h = *h, .audit = auditing};
    uint32_t len = (uint32_t)halo_esb_frame_bits(h->d.frame.length) * HALO_FM_SPB;
    listener_events(h->block, h->block + len, &line.seams, &line.regains, &line.restarts);
    xQueueSend(lines, &line, 0);
}

static void print_task(void *arg)
{
    (void)arg;
    line_t line;
    for (;;) {
        xQueueReceive(lines, &line, portMAX_DELAY);
        const halo_detection_t *x = &line.h.d;
        uint32_t segs = (uint32_t)halo_esb_frame_bits(x->frame.length) * HALO_FM_SPB / HALO_LISTEN_SEG + 1;
        bool weak_gate = line.h.loud < GATE_RUN;
        printf("FRAME %08lX len %u pid %u payload %02X%02X offset %.0f weak %u loud %u/%lu%s%s%s%s%s\n",
               (unsigned long)x->frame.address, x->frame.length, x->frame.pid, x->frame.payload[0],
               x->frame.payload[1], x->offset_hz, x->weak_bits, line.h.loud, (unsigned long)segs,
               line.seams ? " ACROSS" : "", line.regains ? " REGAIN" : "", line.restarts ? " RESTART" : "",
               line.audit ? " AUDIT" : "", weak_gate ? " GATE_WOULD_MISS" : "");
        frames++;
        across += line.seams > 0;
        regained += line.regains > 0;
        gate_would_miss += weak_gate;
    }
}

#define DELTA(field) ((unsigned long)(s->field - was->field))

static double pct(double part, double whole)
{
    return whole > 0 ? 100.0 * part / whole : 0;
}

static void print_core1(const listener_stats_t *s, const listener_stats_t *was)
{
    uint32_t chunks = s->chunks - was->chunks;
    printf("CORE1 laps %lu starts %lu (failed %lu late %lu stale %lu) regains %lu | feed avg %llu max %lu us"
           " | slack_min %lu us | regain_max %lu us\n",
           (unsigned long)chunks, DELTA(starts), DELTA(start_failed), DELTA(late), DELTA(stale), DELTA(regains),
           chunks ? (unsigned long long)((s->feed_us_total - was->feed_us_total) / chunks) : 0ull,
           (unsigned long)s->feed_us_max, (unsigned long)s->slack_us_min, (unsigned long)s->regain_us_max);
}

static void print_core0(const listener_stats_t *s, const listener_stats_t *was, double span_us)
{
    double segs = (double)(s->listen.segments - was->listen.segments);
    printf("CORE0 busy %.1f%% (windows %.1f%%) | loud %.2f%% off_band %lu windows %lu demod %.1f%% dropped %lu"
           " lag_max %lu | heard %.1f%%%s\n",
           pct((double)(s->worker_us - was->worker_us), span_us), pct((double)(s->window_us - was->window_us), span_us),
           pct((double)DELTA(listen.loud), segs), DELTA(listen.off_band), DELTA(listen.windows),
           pct((double)DELTA(listen.demod_blocks), segs * HALO_LISTEN_SEG), DELTA(listen.dropped),
           (unsigned long)s->lag_blocks_max, pct(segs * HALO_LISTEN_SEG / 2, span_us),
           auditing ? " (next: AUDIT)" : "");
    printf("HEARD frames %u across %u regain %u gate_would_miss %u\n", frames, across, regained, gate_would_miss);
}

static void print_histogram(const char *name, const uint32_t *now, const uint32_t *before, unsigned n)
{
    printf("%s", name);
    for (unsigned k = 0; k < n; k++) {
        printf(" %lu", (unsigned long)(now[k] - before[k]));
    }
    printf("\n");
}

static void print_gate_detail(const listener_stats_t *s, const listener_stats_t *was)
{
    printf("FLOOR %.0f\n", (double)s->listen.floor);
    const halo_listen_stats_t *now = &s->listen, *before = &was->listen;
    print_histogram("RUNS by segments 1 2-3 4-7 8-15 16-31 32+:", now->runs, before->runs, COUNTOF(now->runs));
    print_histogram("COHERENCE of loud segments, 0.1 bins:", now->coherence, before->coherence,
                    COUNTOF(now->coherence));
    print_histogram("RUNFREQ frame-sized, 100 kHz bins -1000..+1000:", now->run_freq[0], before->run_freq[0],
                    COUNTOF(now->run_freq[0]));
    print_histogram("RUNFREQ longer:", now->run_freq[1], before->run_freq[1], COUNTOF(now->run_freq[1]));
}

/* CPU share per task since the last call, from FreeRTOS run-time stats. */
static void print_tasks(void)
{
    enum { MAX_TASKS = 24 };
    static TaskStatus_t now[MAX_TASKS];
    static struct {
        TaskHandle_t h;
        configRUN_TIME_COUNTER_TYPE t;
    } was[MAX_TASKS];
    static configRUN_TIME_COUNTER_TYPE total_was;
    configRUN_TIME_COUNTER_TYPE total;
    UBaseType_t n = uxTaskGetSystemState(now, MAX_TASKS, &total);
    configRUN_TIME_COUNTER_TYPE span = total - total_was;
    printf("TASKS");
    for (UBaseType_t k = 0; k < n; k++) {
        configRUN_TIME_COUNTER_TYPE before = 0;
        for (unsigned j = 0; j < MAX_TASKS; j++) {
            before = was[j].h == now[k].xHandle ? was[j].t : before;
        }
        double p = pct((double)(now[k].ulRunTimeCounter - before), (double)span);
        if (p >= 0.5) {
            printf(" %s %.1f%%", now[k].pcTaskName, p);
        }
    }
    printf("\n");
    for (UBaseType_t k = 0; k < n && k < MAX_TASKS; k++) {
        was[k].h = now[k].xHandle;
        was[k].t = now[k].ulRunTimeCounter;
    }
    total_was = total;
}

static bool forever(void *ctx)
{
    (void)ctx;
    return true;
}

static void ring_task(void *arg)
{
    (void)arg;
    listener_ring(forever, NULL);
}

void bench_ring_forever(void)
{
    lines = xQueueCreate(32, sizeof(line_t));
    xTaskCreatePinnedToCore(print_task, "print", 4096, NULL, 1, NULL, 0);
    listener_start(on_heard, NULL);
    /* Below the IPC task, so flash operations from core 0 can still pause core 1. */
    xTaskCreatePinnedToCore(ring_task, "ring", 4096, NULL, configMAX_PRIORITIES - 2, NULL, 1);
    listener_stats_t was = {0};
    int64_t t_was = esp_timer_get_time();
    for (unsigned second = 1;; second++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        auditing = AUDIT_EVERY && second % AUDIT_EVERY == 0;
        listener_audit(auditing);
        listener_stats_t s;
        listener_stats(&s);
        listener_reset_peaks();
        int64_t now = esp_timer_get_time();
        print_core1(&s, &was);
        print_core0(&s, &was, (double)(now - t_was));
        if (GATE_DETAIL) {
            print_gate_detail(&s, &was);
        }
        if (TASK_STATS) {
            print_tasks();
        }
        was = s;
        t_was = now;
    }
}
