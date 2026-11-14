/* SPDX-License-Identifier: GPL-3.0-only */
#include "halo_link.h"

#include "halo_payload.h"

static uint32_t next_random(halo_link_t *l)
{
    uint32_t x = l->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return l->rng = x;
}

void halo_link_init(halo_link_t *l, const halo_link_config_t *cfg, uint32_t seed)
{
    *l = (halo_link_t){.cfg = *cfg, .rng = seed ? seed : 0x9e3779b9u};
    l->current.frame.pid = (uint8_t)(next_random(l) & 3);
}

static bool lights_match(const halo_known_t *k, const halo_lamp_t *t)
{
    return k->lights_known && k->lamp.power == t->power && k->lamp.front == t->front && k->lamp.back == t->back;
}

/* The next payload the target needs, or false if the lamp already matches.
 * Brightness first: on a change of both, that's the one people notice.
 */
static bool plan(const halo_link_t *l, halo_payload_t *p)
{
    const halo_lamp_t *t = &l->target;
    const halo_known_t *k = &l->known;
    bool lights = lights_match(k, t);
    *p = (halo_payload_t){.kind = HALO_PAYLOAD_BRIGHTNESS, .power = t->power, .front = t->front, .back = t->back,
                          .value = t->brightness};
    if (!t->power) {
        return !lights;   /* brightness and temperature don't matter while off */
    }
    if (!lights || !k->brightness_known || k->lamp.brightness != t->brightness) {
        return true;
    }
    if (!k->temperature_known || k->lamp.temperature != t->temperature) {
        p->kind = HALO_PAYLOAD_TEMPERATURE;
        p->value = t->temperature;
        return true;
    }
    return false;
}

/* What a state frame tells us. Only a lit lamp shows its brightness and
 * temperature; we don't know what the lamp does with the value in an "off" frame.
 */
static void learn(halo_known_t *k, const halo_payload_t *p)
{
    k->lamp.power = p->power;
    k->lamp.front = p->front;
    k->lamp.back = p->back;
    k->lights_known = true;
    if (!p->power) {
        return;
    }
    if (p->kind == HALO_PAYLOAD_BRIGHTNESS) {
        k->lamp.brightness = p->value;
        k->brightness_known = true;
    } else if (p->kind == HALO_PAYLOAD_TEMPERATURE) {
        k->lamp.temperature = p->value;
        k->temperature_known = true;
    }
}

halo_link_event_t halo_link_set_target(halo_link_t *l, const halo_lamp_t *target)
{
    halo_lamp_t t = *target;
    if (t.brightness < HALO_BRIGHTNESS_MIN) {
        t.brightness = HALO_BRIGHTNESS_MIN;
    }
    if (t.brightness > HALO_BRIGHTNESS_MAX) {
        t.brightness = HALO_BRIGHTNESS_MAX;
    }
    if (t.temperature > HALO_TEMPERATURE_WARM) {
        t.temperature = HALO_TEMPERATURE_WARM;
    }
    if (!t.front && !t.back) {
        t.power = false;
        bool keep = l->known.lights_known && (l->known.lamp.front || l->known.lamp.back);
        t.front = keep ? l->known.lamp.front : true;
        t.back = keep ? l->known.lamp.back : true;
    }
    l->target = t;
    l->has_target = true;
    /* With a frame on the air the known state is about to change, so only
     * result() can say whether we're done.
     */
    halo_payload_t p;
    if (!l->awaiting_result && !plan(l, &p)) {
        l->has_target = false;
        l->retrying = false;
        return HALO_LINK_DONE;
    }
    return HALO_LINK_NONE;
}

void halo_link_retry_window(const halo_link_config_t *cfg, unsigned attempt, unsigned *lo, unsigned *hi)
{
    /* Growing gaps: with fixed 5-15 ms ones all five tries fell inside ~0.1 s,
     * and one burst of interference took every one of them.
     */
    unsigned a = cfg->retry_min_ms, b = cfg->retry_max_ms > a ? cfg->retry_max_ms : a;
    for (unsigned k = 2; k < attempt && b < HALO_LINK_RETRY_CAP_MS; k++) {
        a *= 3;
        b *= 3;
    }
    *lo = a < HALO_LINK_RETRY_CAP_MS ? a : HALO_LINK_RETRY_CAP_MS;
    *hi = b < HALO_LINK_RETRY_CAP_MS ? b : HALO_LINK_RETRY_CAP_MS;
}

bool halo_link_next(halo_link_t *l, halo_link_tx_t *tx)
{
    halo_payload_t p;
    if (!l->has_target || !plan(l, &p)) {
        return false;
    }
    uint8_t payload[2];
    if (!halo_payload_encode(&p, payload)) {
        /* set_target's clamps rule this out; if one ever misses, drop the target rather than send junk. */
        l->has_target = false;
        l->retrying = false;
        return false;
    }
    halo_link_tx_t *c = &l->current;
    if (l->retrying && c->frame.payload[0] == payload[0] && c->frame.payload[1] == payload[1]) {
        /* Same frame again: same PID, so if the lamp did get the last one and
         * only its ACK was lost, the lamp drops this as a duplicate.
         */
        c->attempt++;
        unsigned lo, hi;
        halo_link_retry_window(&l->cfg, c->attempt, &lo, &hi);
        c->delay_ms = (uint16_t)(lo + next_random(l) % (hi - lo + 1u));
        c->gain = c->attempt >= 3 ? l->cfg.retry_gain : l->cfg.gain;
    } else {
        /* New content needs a new PID, or the lamp could drop it as a repeat. */
        c->frame = (halo_esb_frame_t){.address = l->cfg.address, .pid = (uint8_t)((c->frame.pid + 1) & 3),
                                      .length = 2, .payload = {payload[0], payload[1]}};
        c->attempt = 1;
        c->delay_ms = 0;
        c->gain = l->cfg.gain;
        l->retrying = true;
    }
    l->awaiting_result = true;
    *tx = *c;
    return true;
}

bool halo_link_pending(const halo_link_t *l)
{
    return l->awaiting_result;
}

halo_link_event_t halo_link_result(halo_link_t *l, bool acked)
{
    if (!l->awaiting_result) {
        return HALO_LINK_NONE;
    }
    l->awaiting_result = false;
    halo_payload_t sent = halo_payload_decode(l->current.frame.payload);
    if (acked) {
        learn(&l->known, &sent);
        l->retrying = false;
    } else {
        /* The lamp may have applied it and only the ACK got lost, so whatever
         * this frame carries is now unknown and gets sent again.
         */
        l->known.lights_known = false;
        if (sent.power && sent.kind == HALO_PAYLOAD_BRIGHTNESS) {
            l->known.brightness_known = false;
        }
        if (sent.power && sent.kind == HALO_PAYLOAD_TEMPERATURE) {
            l->known.temperature_known = false;
        }
        if (l->current.attempt >= l->cfg.max_attempts) {
            l->retrying = false;
            l->has_target = false;
            return HALO_LINK_FAILED;
        }
    }
    halo_payload_t p;
    if (!plan(l, &p)) {
        l->has_target = false;
        l->retrying = false;
        return HALO_LINK_DONE;
    }
    return HALO_LINK_NONE;
}

static bool same_known(const halo_known_t *a, const halo_known_t *b)
{
    return a->lights_known == b->lights_known && a->brightness_known == b->brightness_known &&
           a->temperature_known == b->temperature_known && a->lamp.power == b->lamp.power &&
           a->lamp.front == b->lamp.front && a->lamp.back == b->lamp.back &&
           (!a->brightness_known || a->lamp.brightness == b->lamp.brightness) &&
           (!a->temperature_known || a->lamp.temperature == b->lamp.temperature);
}

halo_link_event_t halo_link_heard(halo_link_t *l, const halo_esb_frame_t *frame)
{
    if (frame->address != l->cfg.address || !halo_esb_is_command(frame)) {
        return HALO_LINK_NONE;
    }
    halo_payload_t p = halo_payload_decode(frame->payload);
    halo_known_t before = l->known;
    switch (p.kind) {
    case HALO_PAYLOAD_BRIGHTNESS:
    case HALO_PAYLOAD_TEMPERATURE:
        learn(&l->known, &p);
        break;
    case HALO_PAYLOAD_FAVOURITE:
        learn(&l->known, &p);
        l->known.brightness_known = l->known.temperature_known = false;   /* preset values aren't in the frame */
        break;
    case HALO_PAYLOAD_AUTO:
    case HALO_PAYLOAD_AUTO_STREAM:
        /* Auto frames don't carry the lights reliably (60 01 has power clear), and the lamp now picks
         * brightness and temperature itself.
         */
        l->known.brightness_known = l->known.temperature_known = false;
        break;
    default:
        return HALO_LINK_NONE;   /* wake-up and unknown frames carry no state */
    }
    if (same_known(&before, &l->known)) {
        return HALO_LINK_NONE;   /* the remote repeats every state 3 times */
    }
    if (l->has_target) {
        /* A person just used the remote: newer than whatever HA asked for.
         * A frame still waiting for its retry gap is withdrawn too.
         */
        l->has_target = false;
        l->retrying = false;
        l->awaiting_result = false;
        return HALO_LINK_CANCELLED;
    }
    return HALO_LINK_REMOTE;
}
