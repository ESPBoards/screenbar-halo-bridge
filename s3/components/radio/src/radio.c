/* SPDX-License-Identifier: GPL-3.0-only */
#include "radio.h"

#include <stdatomic.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "halo_detect.h"
#include "halo_fm.h"
#include "halo_gfsk.h"
#include "listener.h"
#include "phy_s3.h"

#define NFM          (PHY_S3_CAPTURE_SAMPLES / HALO_FM_DECIMATION)
#define INBOX        16
#define LEAD_SILENCE 400   /* 10 us of zero IQ at 40 MS/s before the preamble: halo-esp32s3's waveform, known to work */
#define FEED_LAPS    256   /* ~0.26 s: often enough for the task watchdog, rare enough to cost nothing */
#define DEAF_US      5000000
#define IDLE_WAKE_MS 1000  /* the watchdog's feed when nothing else wakes the task */
#define FOUND_MAX    4u    /* frames kept per capture while looking for the ACK */

/* Fallbacks for config values out of range; the same as the C3's own defaults. */
#define DEFAULT_GAIN         4u
#define DEFAULT_RETRY_GAIN   6u
#define DEFAULT_MAX_ATTEMPTS 5u
/* The second try's random gap, short so a missed ACK costs little; later gaps widen from it. */
#define RETRY_MIN_MS 5u
#define RETRY_MAX_MS 15u

#define TASK_STACK    6144
#define TASK_CORE     1
/* Below the IPC task, so flash operations from core 0 can still pause core 1. */
#define TASK_PRIORITY (configMAX_PRIORITIES - 2)

#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

typedef enum { CMD_SET, CMD_CONFIG, CMD_PAIR, CMD_SYNC, CMD_HEARD } cmd_kind_t;

typedef struct {
    cmd_kind_t kind;
    union {
        halo_lamp_t target;
        radio_config_t config;
        uint8_t seconds;
        halo_esb_frame_t frame;
    };
} cmd_t;

static const char *TAG = "radio";

static QueueHandle_t inbox;
static radio_state_fn state_fn;
static radio_heard_fn heard_fn;
static halo_link_t lamp_link;
static bool listen_on;   /* radio task only */
/* ms since boot, 0 when not pairing. 32 bits so the worker's read needs no lock. */
static _Atomic uint32_t pair_until;
static _Atomic uint32_t address;
static struct {
    _Atomic uint32_t sent, acked, failed, lost;   /* read by the bridge task on core 0 */
} counts;

static bool has_tx;
static halo_link_tx_t tx;
static int64_t tx_due;

static float ack_hz[NFM];
static halo_detection_t found[FOUND_MAX];

static bool post(const cmd_t *c)
{
    return xQueueSend(inbox, c, 0) == pdTRUE;
}

bool radio_set(const halo_lamp_t *target)
{
    return post(&(cmd_t){.kind = CMD_SET, .target = *target});
}

bool radio_config(const radio_config_t *config)
{
    return post(&(cmd_t){.kind = CMD_CONFIG, .config = *config});
}

bool radio_pair(uint8_t seconds)
{
    return post(&(cmd_t){.kind = CMD_PAIR, .seconds = seconds});
}

bool radio_sync(void)
{
    return post(&(cmd_t){.kind = CMD_SYNC});
}

uint32_t radio_address(void)
{
    return atomic_load(&address);
}

void radio_stats(radio_stats_t *out)
{
    *out = (radio_stats_t){.sent = atomic_load_explicit(&counts.sent, memory_order_relaxed),
                           .acked = atomic_load_explicit(&counts.acked, memory_order_relaxed),
                           .failed = (uint16_t)atomic_load_explicit(&counts.failed, memory_order_relaxed),
                           .lost = atomic_load_explicit(&counts.lost, memory_order_relaxed)};
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool pairing(void)
{
    uint32_t until = atomic_load_explicit(&pair_until, memory_order_relaxed);
    return until && (int32_t)(until - now_ms()) > 0;
}

static void report_link(halo_link_event_t ev)
{
    if (ev != HALO_LINK_NONE) {
        if (ev == HALO_LINK_FAILED) {
            atomic_fetch_add_explicit(&counts.failed, 1, memory_order_relaxed);
        }
        state_fn(ev, &lamp_link.known);
    }
}

/* A new address means a different lamp: forget everything known about the old one. */
static void configure(const radio_config_t *c)
{
    halo_link_config_t cfg = {.address = c->address,
                              .gain = c->gain <= PHY_S3_GAIN_MAX ? c->gain : DEFAULT_GAIN,
                              .retry_gain = c->retry_gain <= PHY_S3_GAIN_MAX ? c->retry_gain : DEFAULT_RETRY_GAIN,
                              .max_attempts = c->max_attempts ? c->max_attempts : DEFAULT_MAX_ATTEMPTS,
                              .retry_min_ms = RETRY_MIN_MS,
                              .retry_max_ms = RETRY_MAX_MS};
    if (c->address != lamp_link.cfg.address) {
        halo_link_init(&lamp_link, &cfg, esp_random());
        has_tx = false;
        state_fn(HALO_LINK_NONE, &lamp_link.known);   /* a new lamp: what we knew was about the old one */
    } else {
        lamp_link.cfg = cfg;
    }
    atomic_store(&address, c->address);
    listen_on = c->listen;
}

static void handle(const cmd_t *c)
{
    switch (c->kind) {
    case CMD_SET:
        report_link(halo_link_set_target(&lamp_link, &c->target));
        break;
    case CMD_CONFIG:
        configure(&c->config);
        break;
    case CMD_PAIR:
        atomic_store(&pair_until, now_ms() + c->seconds * 1000u);
        break;
    case CMD_SYNC:
        state_fn(HALO_LINK_NONE, &lamp_link.known);
        break;
    case CMD_HEARD:
        report_link(halo_link_heard(&lamp_link, &c->frame));
        if (has_tx && !halo_link_pending(&lamp_link)) {
            has_tx = false;   /* the remote took over during the retry gap */
        }
        break;
    }
}

static bool tx_due_now(void)
{
    return has_tx && esp_timer_get_time() >= tx_due;
}

/* Radio task only. A deadline left in place would read as in the future again
 * ~25 days later (the compare is signed 32-bit), so clear it once it has passed.
 */
static bool listening(void)
{
    bool p = pairing();
    if (!p && atomic_load(&pair_until)) {
        atomic_store(&pair_until, 0);
    }
    return listen_on || p;
}

/* The watchdog only sees a stuck task. A radio that stopped delivering samples
 * while the code still runs would hear nothing forever. The board has no reset
 * line for the C3 to pull, so restart from here.
 */
static void check_deaf(void)
{
    static uint32_t segments_was;
    static int64_t deaf_since;   /* 0: hearing, or not meant to */
    listener_stats_t l;
    listener_stats(&l);
    if (!listening() || l.listen.segments != segments_was) {
        segments_was = l.listen.segments;
        deaf_since = 0;
        return;
    }
    int64_t now = esp_timer_get_time();
    if (!deaf_since) {
        deaf_since = now;
    } else if (now - deaf_since >= DEAF_US) {
        ESP_LOGE(TAG, "listener heard nothing for %lld ms, restarting", (now - deaf_since) / 1000);
        esp_restart();
    }
}

/* Asked once per lap. Remote frames are only bookkeeping, so take them here:
 * stopping the ring for each one cut the stream dozens of times per dial turn.
 * Anything else needs the radio.
 */
static bool keep_listening(void *ctx)
{
    (void)ctx;
    static unsigned laps;
    if (++laps == FEED_LAPS) {
        laps = 0;
        esp_task_wdt_reset();
        check_deaf();
    }
    cmd_t c;
    while (xQueuePeek(inbox, &c, 0) == pdTRUE && c.kind == CMD_HEARD) {
        xQueueReceive(inbox, &c, 0);
        handle(&c);
    }
    return listening() && !uxQueueMessagesWaiting(inbox) && !tx_due_now();
}

static void fill(size_t first, uint32_t *o, size_t count, void *ctx)
{
    halo_gfsk_fill(ctx, first, o, count);
}

static void transmit(void)
{
    static halo_gfsk_t g;
    uint8_t bits[HALO_ESB_MAX_BITS];
    size_t nbits = halo_esb_encode(&tx.frame, bits);
    halo_gfsk_prepare(&g, bits, nbits, LEAD_SILENCE);
    if (tx.attempt == 1) {
        phy_s3_reset_rx();   /* settle before a new command, as halo-esp32s3 does */
    }
    phy_s3_tx_result_t r = phy_s3_send(fill, &g, tx.gain);
    bool acked = false;
    if (r.captured) {
        size_t n = halo_fm_demod(phy_s3_capture(), PHY_S3_CAPTURE_SAMPLES, ack_hz, NFM);
        size_t k = halo_detect(ack_hz, n, found, COUNTOF(found));
        acked = halo_detect_find_ack(found, k, lamp_link.cfg.address, tx.frame.pid) != NULL;
    }
    atomic_fetch_add_explicit(&counts.sent, 1, memory_order_relaxed);
    if (acked) {
        atomic_fetch_add_explicit(&counts.acked, 1, memory_order_relaxed);
    }
    has_tx = false;
    report_link(halo_link_result(&lamp_link, acked));
    if (!lamp_link.retrying) {
        phy_s3_reset_rx();   /* and after the last try */
    }
}

static void radio_task(void *arg)
{
    (void)arg;
    cmd_t c;
    /* Core 1 never runs its idle task, so this task is what the watchdog watches there. */
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    for (;;) {
        esp_task_wdt_reset();
        check_deaf();
        while (xQueueReceive(inbox, &c, 0) == pdTRUE) {
            handle(&c);
        }
        if (!has_tx && lamp_link.cfg.address && halo_link_next(&lamp_link, &tx)) {
            has_tx = true;
            tx_due = esp_timer_get_time() + (int64_t)tx.delay_ms * 1000;
        }
        if (tx_due_now()) {
            transmit();
        } else if (listening()) {
            listener_ring(keep_listening, NULL);
        } else {
            /* Nothing to hear: sleep until a command arrives or a retry is due, and
             * wake each second anyway to feed the watchdog.
             */
            TickType_t wait = pdMS_TO_TICKS(IDLE_WAKE_MS);
            if (has_tx) {
                int64_t us = tx_due - esp_timer_get_time();
                wait = us > 0 ? pdMS_TO_TICKS(us / 1000) + 1 : 0;
            }
            if (xQueueReceive(inbox, &c, wait) == pdTRUE) {
                handle(&c);
            }
        }
    }
}

/* In the listener's worker on core 0. */
static void heard(const halo_heard_t *h, void *ctx)
{
    (void)ctx;
    const halo_esb_frame_t *f = &h->d.frame;
    if (!halo_esb_is_command(f)) {
        return;   /* the lamp's ACKs carry no state */
    }
    if (f->address == atomic_load(&address) && !post(&(cmd_t){.kind = CMD_HEARD, .frame = *f})) {
        atomic_fetch_add_explicit(&counts.lost, 1, memory_order_relaxed);
    }
    if (pairing()) {
        heard_fn(f, h->d.offset_hz);
    }
}

void radio_start(radio_state_fn on_state, radio_heard_fn on_heard)
{
    phy_s3_init();
    halo_gfsk_init();
    state_fn = on_state;
    heard_fn = on_heard;
    inbox = xQueueCreate(INBOX, sizeof(cmd_t));
    configASSERT(inbox);
    halo_link_init(&lamp_link, &(halo_link_config_t){0}, esp_random());
    listener_start(heard, NULL);
    BaseType_t ok = xTaskCreatePinnedToCore(radio_task, "radio", TASK_STACK, NULL, TASK_PRIORITY, NULL, TASK_CORE);
    configASSERT(ok == pdPASS);
}
