/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>

#include "check.h"
#include "halo_link.h"
#include "halo_payload.h"
#include "prng.h"

#define LAMP 0x1a2b3c4du

static const halo_link_config_t cfg = {
    .address = LAMP, .gain = 8, .retry_gain = 9, .max_attempts = 5, .retry_min_ms = 5, .retry_max_ms = 15};

/* A lamp that hears, misses and ACKs like an ESB receiver: a repeat of the
 * last PID and payload is dropped but still ACKed.
 */
typedef struct {
    halo_lamp_t lamp;
    bool have_last;
    uint8_t last_pid, last_payload[2];
    unsigned applied;
    double miss, ack_loss;
} sim_t;

static double urand(void)
{
    return test_rand() / (TEST_RAND_MAX + 1.0);
}

static void sim_apply(sim_t *s, const uint8_t payload[2])
{
    halo_payload_t p = halo_payload_decode(payload);
    s->lamp.power = p.power;
    s->lamp.front = p.front;
    s->lamp.back = p.back;
    if (p.power && p.kind == HALO_PAYLOAD_BRIGHTNESS) {
        s->lamp.brightness = p.value;
    }
    if (p.power && p.kind == HALO_PAYLOAD_TEMPERATURE) {
        s->lamp.temperature = p.value;
    }
}

static bool sim_receive(sim_t *s, const halo_esb_frame_t *f)
{
    if (urand() < s->miss) {
        return false;
    }
    bool repeat = s->have_last && f->pid == s->last_pid && f->payload[0] == s->last_payload[0] &&
                  f->payload[1] == s->last_payload[1];
    if (!repeat) {
        sim_apply(s, f->payload);
        s->applied++;
    }
    s->have_last = true;
    s->last_pid = f->pid;
    s->last_payload[0] = f->payload[0];
    s->last_payload[1] = f->payload[1];
    return urand() >= s->ack_loss;
}

static bool matches(const halo_lamp_t *lamp, const halo_lamp_t *t)
{
    if (!t->power && !t->front && !t->back) {
        return !lamp->power;   /* off with neither light keeps whatever lights the lamp had */
    }
    if (lamp->power != t->power || lamp->front != t->front || lamp->back != t->back) {
        return false;
    }
    return !t->power || (lamp->brightness == t->brightness && lamp->temperature == t->temperature);
}

/* Drive the link until it has nothing left to send; returns the last event. */
static halo_link_event_t run(halo_link_t *l, sim_t *s, unsigned *frames)
{
    halo_link_event_t last = HALO_LINK_NONE;
    halo_link_tx_t tx;
    while (halo_link_next(l, &tx)) {
        (*frames)++;
        halo_link_event_t e = halo_link_result(l, sim_receive(s, &tx.frame));
        if (e != HALO_LINK_NONE) {
            last = e;
        }
    }
    return last;
}

static void test_basic(void)
{
    halo_link_t l;
    halo_link_init(&l, &cfg, 1);
    sim_t s = {0};
    halo_lamp_t t = {.power = true, .front = true, .back = true, .brightness = 0xa5, .temperature = 50};
    CHECK(halo_link_set_target(&l, &t) == HALO_LINK_NONE, "unknown lamp can't already match");
    unsigned frames = 0;
    CHECK(run(&l, &s, &frames) == HALO_LINK_DONE, "not done");
    CHECK(frames == 2 && matches(&s.lamp, &t), "want brightness + temperature frames, got %u", frames);
    CHECK(halo_link_set_target(&l, &t) == HALO_LINK_DONE, "same target again should be done at once");

    t.brightness = 0xc0;
    frames = 0;
    halo_link_set_target(&l, &t);
    CHECK(run(&l, &s, &frames) == HALO_LINK_DONE && frames == 1, "brightness-only change took %u frames", frames);

    t.power = false;   /* off: one frame, lights kept */
    frames = 0;
    halo_link_set_target(&l, &t);
    CHECK(run(&l, &s, &frames) == HALO_LINK_DONE && frames == 1, "off took %u frames", frames);
    CHECK(!s.lamp.power && s.lamp.front && s.lamp.back, "off should keep the light bits");
}

static void test_neither_light_means_off(void)
{
    halo_link_t l;
    halo_link_init(&l, &cfg, 2);
    sim_t s = {0};
    halo_lamp_t t = {.power = true, .front = false, .back = true, .brightness = 0xa5, .temperature = 10};
    unsigned frames = 0;
    halo_link_set_target(&l, &t);
    run(&l, &s, &frames);
    t.back = false;
    halo_link_set_target(&l, &t);
    run(&l, &s, &frames);
    CHECK(!s.lamp.power && s.lamp.back && !s.lamp.front, "should be off with the back light remembered");
}

static void test_pid_rules(void)
{
    halo_link_t l;
    halo_link_init(&l, &cfg, 3);
    sim_t s = {.ack_loss = 1.0};   /* the lamp hears everything but no ACK ever arrives */
    halo_lamp_t t = {.power = true, .front = true, .back = false, .brightness = 0x80, .temperature = 0};
    halo_link_set_target(&l, &t);
    halo_link_tx_t tx;
    uint8_t first_pid = 0;
    unsigned total_ms = 0;
    halo_link_event_t e = HALO_LINK_NONE;
    for (unsigned a = 1; halo_link_next(&l, &tx); a++) {
        if (a == 1) {
            first_pid = tx.frame.pid;
            CHECK(tx.delay_ms == 0 && tx.gain == cfg.gain, "first try should go now at the base gain");
        } else {
            CHECK(tx.frame.pid == first_pid, "retry %u changed PID", a);
            unsigned lo, hi;
            halo_link_retry_window(&cfg, a, &lo, &hi);
            CHECK(tx.delay_ms >= lo && tx.delay_ms <= hi, "try %u gap %u ms, not in %u-%u", a, tx.delay_ms, lo, hi);
            total_ms += tx.delay_ms;
            CHECK(tx.gain == (a >= 3 ? cfg.retry_gain : cfg.gain), "try %u gain %u", a, tx.gain);
        }
        CHECK(tx.attempt == a, "attempt %u reported as %u", a, tx.attempt);
        e = halo_link_result(&l, sim_receive(&s, &tx.frame));
    }
    CHECK(e == HALO_LINK_FAILED, "should give up after %u tries", cfg.max_attempts);
    CHECK(total_ms >= 200, "all tries within %u ms: one burst of interference can take them all", total_ms);
    unsigned lo, hi;
    halo_link_retry_window(&cfg, 2, &lo, &hi);
    CHECK(lo == cfg.retry_min_ms && hi == cfg.retry_max_ms, "first gap %u-%u", lo, hi);
    halo_link_retry_window(&cfg, 5, &lo, &hi);
    CHECK(lo == 27u * cfg.retry_min_ms && hi == 27u * cfg.retry_max_ms, "fourth gap %u-%u", lo, hi);
    halo_link_retry_window(&cfg, 200, &lo, &hi);
    CHECK(hi == HALO_LINK_RETRY_CAP_MS && lo <= hi, "gaps must stop growing: %u-%u", lo, hi);
    CHECK(s.applied == 1, "retries were applied %u times; the lamp should drop repeats", s.applied);
    CHECK(!l.known.lights_known && !l.known.brightness_known, "after a failure the lamp state is unknown");

    /* The next command must not reuse that PID, or the lamp would drop it. */
    s.ack_loss = 0;
    t.brightness = 0x90;
    halo_link_set_target(&l, &t);
    CHECK(halo_link_next(&l, &tx) && tx.frame.pid != first_pid, "new frame reused PID %u", first_pid);
    halo_link_result(&l, sim_receive(&s, &tx.frame));
}

/* A lost ACK leaves the lamp unknown, so a later target that "matches" the old
 * known state still gets sent.
 */
static void test_lost_ack_is_not_trusted(void)
{
    halo_link_t l;
    halo_link_init(&l, &cfg, 4);
    sim_t s = {0};
    halo_lamp_t a = {.power = true, .front = true, .back = true, .brightness = 0x80, .temperature = 20};
    unsigned frames = 0;
    halo_link_set_target(&l, &a);
    run(&l, &s, &frames);

    halo_lamp_t b = a;
    b.brightness = 0xf0;
    halo_link_set_target(&l, &b);
    halo_link_tx_t tx;
    halo_link_next(&l, &tx);
    s.ack_loss = 1.0;
    halo_link_result(&l, sim_receive(&s, &tx.frame));   /* lamp took 0xf0, we never heard */
    s.ack_loss = 0;
    CHECK(halo_link_set_target(&l, &a) == HALO_LINK_NONE, "back to A must be sent, the lamp may be at B");
    run(&l, &s, &frames);
    CHECK(matches(&s.lamp, &a), "lamp should be back at A");
}

/* A new target arriving between next() and result(): the frame on the air still counts. */
static void test_target_changes_mid_air(void)
{
    halo_link_t l;
    halo_link_init(&l, &cfg, 5);
    sim_t s = {0};
    halo_lamp_t a = {.power = true, .front = true, .back = true, .brightness = 0x80, .temperature = 20};
    unsigned frames = 0;
    halo_link_set_target(&l, &a);
    run(&l, &s, &frames);

    halo_lamp_t b = a;
    b.brightness = 0xf0;
    halo_link_set_target(&l, &b);
    halo_link_tx_t tx;
    halo_link_next(&l, &tx);
    halo_link_set_target(&l, &a);   /* user dragged the slider back before the frame finished */
    halo_link_result(&l, sim_receive(&s, &tx.frame));
    run(&l, &s, &frames);
    CHECK(matches(&s.lamp, &a), "lamp left at %02X, want A", s.lamp.brightness);
}

/* A target that can't be encoded is dropped, not sent. set_target never makes one, so build it by hand. */
static void test_unencodable_target(void)
{
    halo_link_t l;
    halo_link_init(&l, &cfg, 4);
    l.target = (halo_lamp_t){.power = true, .brightness = 0xa5};   /* no light to turn on */
    l.has_target = true;
    halo_link_tx_t tx;
    CHECK(!halo_link_next(&l, &tx) && !l.has_target && !halo_link_pending(&l), "unencodable target sent");
}

static void test_heard(void)
{
    halo_link_t l;
    halo_link_init(&l, &cfg, 6);
    sim_t s = {0};
    halo_lamp_t t = {.power = true, .front = true, .back = true, .brightness = 0x80, .temperature = 20};
    unsigned frames = 0;
    halo_link_set_target(&l, &t);
    run(&l, &s, &frames);

    halo_esb_frame_t dial = {.address = LAMP, .pid = 1, .length = 2, .payload = {0xc5, 0x9f}};
    CHECK(halo_link_heard(&l, &dial) == HALO_LINK_REMOTE, "remote dial not reported");
    CHECK(l.known.brightness_known && l.known.lamp.brightness == 0x9f, "brightness not learned");
    CHECK(halo_link_heard(&l, &dial) == HALO_LINK_NONE, "the remote's repeats shouldn't count again");

    halo_esb_frame_t other = dial;
    other.address = LAMP ^ 1;
    other.payload[1] = 0x50;
    CHECK(halo_link_heard(&l, &other) == HALO_LINK_NONE, "another lamp's remote was believed");
    halo_esb_frame_t ack = {.address = LAMP, .pid = 1, .no_ack = true};
    CHECK(halo_link_heard(&l, &ack) == HALO_LINK_NONE, "an ACK was taken as state");
    halo_esb_frame_t wake = {.address = LAMP, .length = 2, .payload = {0xff, 0x00}};
    CHECK(halo_link_heard(&l, &wake) == HALO_LINK_NONE, "wake-up frame changed state");

    halo_esb_frame_t heart = {.address = LAMP, .length = 2, .payload = {0xc9, 0x00}};
    CHECK(halo_link_heard(&l, &heart) == HALO_LINK_REMOTE, "favourite not reported");
    CHECK(!l.known.brightness_known && !l.known.temperature_known, "favourite should make values unknown");

    halo_esb_frame_t temp = {.address = LAMP, .length = 2, .payload = {0xc3, 0x30}};
    halo_link_heard(&l, &temp);
    halo_esb_frame_t stream = {.address = LAMP, .length = 2, .payload = {0xfc, 0x36}};
    halo_link_heard(&l, &stream);
    CHECK(!l.known.temperature_known && l.known.lamp.front && l.known.lamp.back,
          "auto stream should blank the values but leave the lights");

    /* The remote while HA's target is still going out: the person wins. */
    t.brightness = 0xe0;
    halo_link_set_target(&l, &t);
    CHECK(halo_link_heard(&l, &dial) == HALO_LINK_CANCELLED, "target not cancelled by the remote");
    halo_link_tx_t tx;
    CHECK(!halo_link_next(&l, &tx), "still sending a cancelled target");
}

/* The remote used while a retry waits out its gap: the retry must not go out,
 * or the lamp would undo the person's change.
 */
static void test_remote_during_retry_gap(void)
{
    halo_link_t l;
    halo_link_init(&l, &cfg, 8);
    halo_lamp_t t = {.power = true, .front = true, .back = true, .brightness = 0xa5, .temperature = 50};
    halo_link_set_target(&l, &t);
    halo_link_tx_t tx;
    while (halo_link_next(&l, &tx)) {
        halo_link_result(&l, true);   /* settle both values first */
    }
    t.brightness = 0xc0;
    halo_link_set_target(&l, &t);
    CHECK(halo_link_next(&l, &tx), "nothing to send");
    halo_link_result(&l, false);
    CHECK(halo_link_next(&l, &tx) && tx.attempt == 2 && tx.delay_ms > 0, "no retry waiting");
    CHECK(halo_link_pending(&l), "the retry should be pending before the remote");

    halo_esb_frame_t dial = {.address = LAMP, .pid = 1, .length = 2, .payload = {0xc5, 0x60}};
    CHECK(halo_link_heard(&l, &dial) == HALO_LINK_CANCELLED, "the remote didn't cancel");
    CHECK(!halo_link_pending(&l), "the retry is still pending after the remote");
    CHECK(halo_link_result(&l, true) == HALO_LINK_NONE && l.known.lamp.brightness == 0x60,
          "a withdrawn frame changed the known state");
    CHECK(!halo_link_next(&l, &tx), "still sending after the remote took over");
}

/* Random everything. The rule: once DONE is reported, the lamp matches that
 * target from then on, until a new target or a remote press replaces it.
 * Checked after every step, not just when DONE arrives.
 */
static void test_soak(void)
{
    halo_link_t l;
    halo_link_init(&l, &cfg, 7);
    sim_t s = {.miss = 0.2, .ack_loss = 0.1};
    halo_lamp_t target = {0}, previous = {0};
    bool promised = false;
    unsigned done = 0, failed = 0, cancelled = 0, broken = 0, frames = 0;
    for (int step = 0; step < 50000; step++) {
        double r = urand();
        if (r < 0.3) {
            previous = target;
            target = (halo_lamp_t){.power = urand() < 0.8, .front = urand() < 0.7, .back = urand() < 0.7,
                                   .brightness = (uint8_t)(HALO_BRIGHTNESS_MIN + test_rand() % 179),
                                   .temperature = (uint8_t)(test_rand() % 101)};
            if (!target.front && !target.back) {
                target.front = true;
            }
            promised = halo_link_set_target(&l, &target) == HALO_LINK_DONE;
            done += promised;
        } else if (r < 0.35) {
            /* someone uses the remote; the listener hears it */
            uint8_t flags = 0x80 | (test_rand() & 1 ? 0x40 : 0) | 0x01 | (test_rand() & 1 ? 0x04 : 0x02);
            uint8_t value =
                flags & 0x04 ? (uint8_t)(HALO_BRIGHTNESS_MIN + test_rand() % 179) : (uint8_t)(test_rand() % 101);
            halo_esb_frame_t f = {.address = LAMP, .length = 2, .payload = {flags, value}};
            sim_apply(&s, f.payload);
            s.have_last = false;
            cancelled += halo_link_heard(&l, &f) == HALO_LINK_CANCELLED;
            promised = false;
        } else {
            halo_link_tx_t tx;
            if (halo_link_next(&l, &tx)) {
                if (tx.attempt > 1 && urand() < 0.1) {
                    /* the remote during the retry gap: the radio checks pending before sending */
                    uint8_t value = (uint8_t)(HALO_BRIGHTNESS_MIN + test_rand() % 179);
                    halo_esb_frame_t f = {.address = LAMP, .length = 2, .payload = {0xc5, value}};
                    sim_apply(&s, f.payload);
                    s.have_last = false;
                    cancelled += halo_link_heard(&l, &f) == HALO_LINK_CANCELLED;
                    promised = false;
                    if (!halo_link_pending(&l)) {
                        continue;
                    }
                }
                frames++;
                if (urand() < 0.1) {   /* a new target lands while this frame is on the air */
                    halo_lamp_t next = urand() < 0.5 ? previous : target;   /* half the time: dragged back */
                    previous = target;
                    target = next;
                    target.brightness = urand() < 0.5 ? target.brightness
                                                      : (uint8_t)(HALO_BRIGHTNESS_MIN + test_rand() % 179);
                    promised = halo_link_set_target(&l, &target) == HALO_LINK_DONE;
                    done += promised;
                }
                halo_link_event_t e = halo_link_result(&l, sim_receive(&s, &tx.frame));
                if (e == HALO_LINK_DONE) {
                    done++;
                    promised = true;
                }
                failed += e == HALO_LINK_FAILED;
            }
        }
        broken += promised && !matches(&s.lamp, &target);
    }
    printf("  soak: %u frames, %u done, %u failed, %u cancelled by the remote, %u steps with a broken promise\n",
           frames, done, failed, cancelled, broken);
    CHECK(broken == 0, "%u steps where the lamp didn't match a target reported DONE", broken);
    CHECK(done > 1000, "soak barely did anything (%u done)", done);
}

int main(void)
{
    test_srand(11);
    test_basic();
    test_neither_light_means_off();
    test_pid_rules();
    test_lost_ack_is_not_trusted();
    test_target_changes_mid_air();
    test_unencodable_target();
    test_heard();
    test_remote_during_retry_gap();
    test_soak();
    return check_report("test_link");
}
