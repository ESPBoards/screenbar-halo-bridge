/* SPDX-License-Identifier: GPL-3.0-only */
#include "halo_wire.h"

#include <string.h>

#include "halo_payload.h"

/* The COBS here never splits a long run of non-zero bytes, so frames must stay under 254 bytes. */
_Static_assert(HALO_WIRE_MAX_FRAME < 254, "frame too long for this COBS");

#define LEN_HELLO_REQ 0
#define LEN_CONFIG    8
#define LEN_SET       3
#define LEN_PAIR      1
#define LEN_HELLO     8
#define LEN_ACK       1
#define LEN_STATE     4
#define LEN_HEARD     9
#define LEN_HEARTBEAT 22   /* the longest */
_Static_assert(LEN_HEARTBEAT <= HALO_WIRE_MAX_PAYLOAD, "every payload must fit a frame");

/* Byte-wise, unlike halo_esb's: this one runs over bytes, not air bits. */
static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xffffu;
    while (n--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int k = 0; k < 8; k++) {
            crc = crc & 0x8000u ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

size_t halo_wire_encode(const halo_wire_msg_t *m, uint8_t out[HALO_WIRE_MAX_ENCODED])
{
    uint8_t frame[HALO_WIRE_MAX_FRAME];
    size_t len = m->len > HALO_WIRE_MAX_PAYLOAD ? HALO_WIRE_MAX_PAYLOAD : m->len;
    frame[0] = HALO_WIRE_FRAMING;
    frame[1] = m->type;
    frame[2] = m->seq;
    frame[3] = (uint8_t)len;
    memcpy(frame + HALO_WIRE_HEADER, m->payload, len);
    size_t n = HALO_WIRE_HEADER + len;
    uint16_t crc = crc16(frame, n);
    frame[n++] = (uint8_t)crc;
    frame[n++] = (uint8_t)(crc >> 8);

    /* COBS: each code byte says how far to the next zero. */
    size_t code_at = 0, o = 1;
    uint8_t code = 1;
    for (size_t k = 0; k < n; k++) {
        if (frame[k]) {
            out[o++] = frame[k];
            code++;
        } else {
            out[code_at] = code;
            code_at = o++;
            code = 1;
        }
    }
    out[code_at] = code;
    out[o++] = 0;
    return o;
}

void halo_wire_rx_init(halo_wire_rx_t *rx)
{
    memset(rx, 0, sizeof *rx);
}

static bool unstuff(const uint8_t *in, size_t n, uint8_t *out, size_t *len)
{
    size_t o = 0, k = 0;
    while (k < n) {
        uint8_t code = in[k++];
        if (!code || k + code - 1 > n) {
            return false;
        }
        for (uint8_t j = 1; j < code; j++) {
            out[o++] = in[k++];
        }
        if (code < 0xff && k < n) {
            out[o++] = 0;
        }
    }
    *len = o;
    return true;
}

static bool check(const uint8_t *frame, size_t n, halo_wire_msg_t *m)
{
    const size_t overhead = HALO_WIRE_HEADER + HALO_WIRE_CRC;
    if (n < overhead || frame[0] != HALO_WIRE_FRAMING || frame[3] != n - overhead ||
        frame[3] > HALO_WIRE_MAX_PAYLOAD) {
        return false;
    }
    uint16_t crc = (uint16_t)(frame[n - 2] | frame[n - 1] << 8);
    if (crc != crc16(frame, n - HALO_WIRE_CRC)) {
        return false;
    }
    m->type = frame[1];
    m->seq = frame[2];
    m->len = frame[3];
    memcpy(m->payload, frame + HALO_WIRE_HEADER, m->len);
    return true;
}

bool halo_wire_rx(halo_wire_rx_t *rx, uint8_t byte, halo_wire_msg_t *m)
{
    if (byte) {
        if (rx->n < sizeof rx->buf) {
            rx->buf[rx->n++] = byte;
        } else {
            rx->overflow = true;
        }
        return false;
    }
    size_t n = rx->n;
    bool overflow = rx->overflow;
    rx->n = 0;
    rx->overflow = false;
    if (!n) {
        return false;   /* back-to-back zeros: idle line, not an error */
    }
    uint8_t frame[HALO_WIRE_MAX_ENCODED];
    size_t len;
    if (overflow || !unstuff(rx->buf, n, frame, &len) || !check(frame, len, m)) {
        rx->bad++;
        return false;
    }
    rx->frames++;
    return true;
}

static void put8(halo_wire_msg_t *m, uint8_t v)
{
    m->payload[m->len++] = v;
}

static void put16(halo_wire_msg_t *m, uint16_t v)
{
    put8(m, (uint8_t)v);
    put8(m, (uint8_t)(v >> 8));
}

static void put32(halo_wire_msg_t *m, uint32_t v)
{
    put16(m, (uint16_t)v);
    put16(m, (uint16_t)(v >> 16));
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
    return get16(p) | (uint32_t)get16(p + 2) << 16;
}

static void start(halo_wire_msg_t *m, halo_wire_type_t type)
{
    m->type = (uint8_t)type;
    m->len = 0;
}

static bool is(const halo_wire_msg_t *m, halo_wire_type_t type)
{
    return m->type == type && m->len == halo_wire_payload_len(type);
}

int halo_wire_payload_len(uint8_t type)
{
    switch (type) {
    case HALO_WIRE_MSG_HELLO_REQ:
        return LEN_HELLO_REQ;
    case HALO_WIRE_MSG_CONFIG:
        return LEN_CONFIG;
    case HALO_WIRE_MSG_SET:
        return LEN_SET;
    case HALO_WIRE_MSG_PAIR:
        return LEN_PAIR;
    case HALO_WIRE_MSG_HELLO:
        return LEN_HELLO;
    case HALO_WIRE_MSG_ACK:
        return LEN_ACK;
    case HALO_WIRE_MSG_STATE:
        return LEN_STATE;
    case HALO_WIRE_MSG_HEARD:
        return LEN_HEARD;
    case HALO_WIRE_MSG_HEARTBEAT:
        return LEN_HEARTBEAT;
    default:
        return -1;
    }
}

void halo_wire_put_empty(halo_wire_msg_t *m, halo_wire_type_t type)
{
    start(m, type);
}

void halo_wire_put_hello(halo_wire_msg_t *m, const halo_wire_hello_t *h)
{
    start(m, HALO_WIRE_MSG_HELLO);
    put8(m, h->version);
    put16(m, h->firmware);
    put8(m, h->reset_reason);
    put32(m, h->address);
}

bool halo_wire_get_hello(const halo_wire_msg_t *m, halo_wire_hello_t *h)
{
    if (!is(m, HALO_WIRE_MSG_HELLO)) {
        return false;
    }
    const uint8_t *p = m->payload;
    *h = (halo_wire_hello_t){.version = p[0], .firmware = get16(p + 1), .reset_reason = p[3], .address = get32(p + 4)};
    return true;
}

void halo_wire_put_config(halo_wire_msg_t *m, const halo_wire_config_t *c)
{
    start(m, HALO_WIRE_MSG_CONFIG);
    put32(m, c->address);
    put8(m, c->gain);
    put8(m, c->retry_gain);
    put8(m, c->max_attempts);
    put8(m, c->listen);
}

bool halo_wire_get_config(const halo_wire_msg_t *m, halo_wire_config_t *c)
{
    if (!is(m, HALO_WIRE_MSG_CONFIG)) {
        return false;
    }
    const uint8_t *p = m->payload;
    *c = (halo_wire_config_t){
        .address = get32(p), .gain = p[4], .retry_gain = p[5], .max_attempts = p[6], .listen = p[7] != 0};
    return true;
}

/* Lights in one byte: the same bits the lamp uses, so a log reads like a sniff. */
#define BIT_LIGHTS_KNOWN 0x08u
#define BIT_BRIGHTNESS_KNOWN 0x04u
#define BIT_TEMPERATURE_KNOWN 0x02u

static uint8_t lights(const halo_lamp_t *l)
{
    return (uint8_t)((l->power ? HALO_FLAG_POWER : 0) | (l->front ? HALO_FLAG_FRONT : 0) |
                     (l->back ? HALO_FLAG_BACK : 0));
}

static void set_lights(halo_lamp_t *l, uint8_t bits)
{
    l->power = bits & HALO_FLAG_POWER;
    l->front = bits & HALO_FLAG_FRONT;
    l->back = bits & HALO_FLAG_BACK;
}

void halo_wire_put_set(halo_wire_msg_t *m, const halo_lamp_t *t)
{
    start(m, HALO_WIRE_MSG_SET);
    put8(m, lights(t));
    put8(m, t->brightness);
    put8(m, t->temperature);
}

bool halo_wire_get_set(const halo_wire_msg_t *m, halo_lamp_t *t)
{
    if (!is(m, HALO_WIRE_MSG_SET)) {
        return false;
    }
    *t = (halo_lamp_t){.brightness = m->payload[1], .temperature = m->payload[2]};
    set_lights(t, m->payload[0]);
    return true;
}

void halo_wire_put_pair(halo_wire_msg_t *m, uint8_t seconds)
{
    start(m, HALO_WIRE_MSG_PAIR);
    put8(m, seconds);
}

bool halo_wire_get_pair(const halo_wire_msg_t *m, uint8_t *seconds)
{
    if (!is(m, HALO_WIRE_MSG_PAIR)) {
        return false;
    }
    *seconds = m->payload[0];
    return true;
}

void halo_wire_put_ack(halo_wire_msg_t *m, halo_wire_status_t status)
{
    start(m, HALO_WIRE_MSG_ACK);
    put8(m, (uint8_t)status);
}

bool halo_wire_get_ack(const halo_wire_msg_t *m, halo_wire_status_t *status)
{
    if (!is(m, HALO_WIRE_MSG_ACK) || m->payload[0] > HALO_WIRE_STATUS_BUSY) {
        return false;   /* a status we don't know can't be acted on */
    }
    *status = (halo_wire_status_t)m->payload[0];
    return true;
}

void halo_wire_put_state(halo_wire_msg_t *m, const halo_wire_state_t *s)
{
    const halo_known_t *k = &s->known;
    start(m, HALO_WIRE_MSG_STATE);
    put8(m, (uint8_t)s->event);
    put8(m, (uint8_t)(lights(&k->lamp) | (k->lights_known ? BIT_LIGHTS_KNOWN : 0) |
                      (k->brightness_known ? BIT_BRIGHTNESS_KNOWN : 0) |
                      (k->temperature_known ? BIT_TEMPERATURE_KNOWN : 0)));
    put8(m, k->lamp.brightness);
    put8(m, k->lamp.temperature);
}

bool halo_wire_get_state(const halo_wire_msg_t *m, halo_wire_state_t *s)
{
    const uint8_t *p = m->payload;
    if (!is(m, HALO_WIRE_MSG_STATE) || p[0] > HALO_WIRE_EVENT_CANCELLED) {
        return false;
    }
    *s = (halo_wire_state_t){.event = (halo_wire_event_t)p[0]};
    halo_known_t *k = &s->known;
    set_lights(&k->lamp, p[1]);
    k->lights_known = p[1] & BIT_LIGHTS_KNOWN;
    k->brightness_known = p[1] & BIT_BRIGHTNESS_KNOWN;
    k->temperature_known = p[1] & BIT_TEMPERATURE_KNOWN;
    k->lamp.brightness = p[2];
    k->lamp.temperature = p[3];
    return true;
}

void halo_wire_put_heard(halo_wire_msg_t *m, const halo_wire_heard_t *h)
{
    start(m, HALO_WIRE_MSG_HEARD);
    put32(m, h->address);
    put8(m, h->pid);
    put8(m, h->payload[0]);
    put8(m, h->payload[1]);
    put16(m, (uint16_t)h->offset_khz);
}

bool halo_wire_get_heard(const halo_wire_msg_t *m, halo_wire_heard_t *h)
{
    if (!is(m, HALO_WIRE_MSG_HEARD)) {
        return false;
    }
    const uint8_t *p = m->payload;
    *h = (halo_wire_heard_t){.address = get32(p), .pid = p[4], .payload = {p[5], p[6]},
                             .offset_khz = (int16_t)get16(p + 7)};
    return true;
}

void halo_wire_put_heartbeat(halo_wire_msg_t *m, const halo_wire_heartbeat_t *h)
{
    start(m, HALO_WIRE_MSG_HEARTBEAT);
    put32(m, h->uptime_s);
    put32(m, h->sent);
    put32(m, h->acked);
    put16(m, h->failed);
    put16(m, h->heard_permille);
    put16(m, h->dropped);
    put16(m, h->restarts);
    put16(m, h->wire_bad);
}

bool halo_wire_get_heartbeat(const halo_wire_msg_t *m, halo_wire_heartbeat_t *h)
{
    if (!is(m, HALO_WIRE_MSG_HEARTBEAT)) {
        return false;
    }
    const uint8_t *p = m->payload;
    *h = (halo_wire_heartbeat_t){.uptime_s = get32(p), .sent = get32(p + 4), .acked = get32(p + 8),
                                 .failed = get16(p + 12), .heard_permille = get16(p + 14),
                                 .dropped = get16(p + 16), .restarts = get16(p + 18), .wire_bad = get16(p + 20)};
    return true;
}
