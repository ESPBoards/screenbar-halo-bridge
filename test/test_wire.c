/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "halo_wire.h"
#include "prng.h"

/* Feeds bytes and counts every frame that comes out, keeping the first max. */
static size_t receive(halo_wire_rx_t *rx, const uint8_t *bytes, size_t n, halo_wire_msg_t *out, size_t max)
{
    size_t got = 0;
    for (size_t k = 0; k < n; k++) {
        halo_wire_msg_t m;
        if (halo_wire_rx(rx, bytes[k], &m)) {
            if (got < max) {
                out[got] = m;
            }
            got++;
        }
    }
    return got;
}

static bool same(const halo_wire_msg_t *a, const halo_wire_msg_t *b)
{
    return a->type == b->type && a->seq == b->seq && a->len == b->len && !memcmp(a->payload, b->payload, a->len);
}

static halo_wire_msg_t random_msg(void)
{
    halo_wire_msg_t m = {.type = (uint8_t)test_rand(), .seq = (uint8_t)test_rand(), .len = (uint8_t)(test_rand() % 25)};
    for (unsigned k = 0; k < m.len; k++) {
        m.payload[k] = test_rand() % 3 ? (uint8_t)test_rand() : 0;   /* plenty of zeros for COBS to deal with */
    }
    return m;
}

static void test_round_trip(void)
{
    halo_wire_rx_t rx;
    halo_wire_rx_init(&rx);
    unsigned wrong = 0, worst = 0;
    for (int t = 0; t < 20000; t++) {
        halo_wire_msg_t m = random_msg(), got[2];
        uint8_t wire[HALO_WIRE_MAX_ENCODED];
        size_t n = halo_wire_encode(&m, wire);
        worst = n > worst ? (unsigned)n : worst;
        for (size_t k = 0; k + 1 < n; k++) {
            wrong += wire[k] == 0;   /* only the delimiter may be zero */
        }
        wrong += receive(&rx, wire, n, got, 2) != 1 || !same(&m, &got[0]);
    }
    CHECK(wrong == 0, "%u bad round trips", wrong);
    CHECK(worst <= HALO_WIRE_MAX_ENCODED, "encoded %u bytes", worst);
    CHECK(rx.bad == 0, "%u bad frames", rx.bad);
}

/* Any single flipped bit, a lost byte or an extra byte must be caught and
 * counted, and only the next good frame may come out.
 */
static void test_damage(void)
{
    unsigned leaked = 0, recovered = 0, uncounted = 0, total = 0;
    for (int t = 0; t < 5000; t++) {
        halo_wire_msg_t m = random_msg(), next, got[4];
        do {
            next = random_msg();
        } while (same(&next, &m));   /* so a damaged m that slips through can't pass for next */
        uint8_t wire[2 * HALO_WIRE_MAX_ENCODED + 1];
        size_t n = halo_wire_encode(&m, wire);
        int kind = t % 3;
        size_t at = (size_t)test_rand() % (n - 1);
        if (kind == 0) {
            wire[at] ^= (uint8_t)(1u << (test_rand() % 8));
        } else if (kind == 1) {
            memmove(wire + at, wire + at + 1, n - at - 1);
            n--;
        } else {
            memmove(wire + at + 1, wire + at, n - at);
            wire[at] = (uint8_t)(1 + test_rand() % 255);
            n++;
        }
        n += halo_wire_encode(&next, wire + n);
        halo_wire_rx_t rx;
        halo_wire_rx_init(&rx);
        size_t k = receive(&rx, wire, n, got, 4);
        for (size_t j = 0; j < k && j < 4; j++) {
            leaked += !same(&got[j], &next);
        }
        recovered += k == 1 && same(&got[0], &next);
        uncounted += rx.bad == 0;
        total++;
    }
    CHECK(leaked == 0, "%u damaged frames came out", leaked);
    CHECK(recovered == total, "only %u of %u recovered at the next frame", recovered, total);
    CHECK(uncounted == 0, "%u damaged frames not counted bad", uncounted);
}

/* Written out again, so a frame can be built that halo_wire_encode would never make. */
static uint16_t crc_ccitt(const uint8_t *p, size_t n)
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

/* A PAIR frame with a valid CRC, whatever its framing byte. With no zero
 * bytes in it, COBS is one code byte in front. Returns 0 if the CRC has a zero.
 */
static size_t hand_frame(uint8_t framing, uint8_t seq, uint8_t out[9])
{
    uint8_t f[7] = {framing, HALO_WIRE_MSG_PAIR, seq, 1, 30};
    uint16_t crc = crc_ccitt(f, 5);
    f[5] = (uint8_t)crc;
    f[6] = (uint8_t)(crc >> 8);
    if (!f[5] || !f[6]) {
        return 0;
    }
    out[0] = sizeof f + 1;
    memcpy(out + 1, f, sizeof f);
    out[8] = 0;
    return 9;
}

/* Another framing byte means another frame layout, so even a good CRC mustn't get it in. */
static void test_wrong_framing(void)
{
    uint8_t good[9], bad[9];
    uint8_t seq = 1;
    while (!hand_frame(HALO_WIRE_FRAMING, seq, good) || !hand_frame(HALO_WIRE_FRAMING + 1, seq, bad)) {
        seq++;
    }
    halo_wire_rx_t rx;
    halo_wire_rx_init(&rx);
    halo_wire_msg_t got[2];
    size_t k = receive(&rx, good, sizeof good, got, 2);
    CHECK(k == 1 && got[0].type == HALO_WIRE_MSG_PAIR && got[0].seq == seq && got[0].len == 1 &&
              got[0].payload[0] == 30 && rx.bad == 0,
          "hand-built frame refused");
    k = receive(&rx, bad, sizeof bad, got, 2);
    CHECK(k == 0, "frame with framing byte %u accepted", HALO_WIRE_FRAMING + 1);
    CHECK(rx.bad == 1 && rx.frames == 1, "wrong framing byte: %u bad, %u frames", rx.bad, rx.frames);
}

/* One payload byte more than the buffer holds, with a good CRC: only the
 * length cap keeps it from being copied past payload[].
 */
static void test_too_long(void)
{
    uint8_t f[HALO_WIRE_MAX_FRAME + 1], wire[sizeof f + 2];
    f[0] = HALO_WIRE_FRAMING;
    f[1] = HALO_WIRE_MSG_PAIR;
    f[3] = HALO_WIRE_MAX_PAYLOAD + 1;
    memset(f + 4, 0x5a, HALO_WIRE_MAX_PAYLOAD + 1);
    for (uint8_t seq = 1;; seq++) {   /* a CRC with no zero byte keeps COBS to one code byte */
        f[2] = seq;
        uint16_t crc = crc_ccitt(f, sizeof f - 2);
        f[sizeof f - 2] = (uint8_t)crc;
        f[sizeof f - 1] = (uint8_t)(crc >> 8);
        if (f[sizeof f - 2] && f[sizeof f - 1]) {
            break;
        }
    }
    wire[0] = sizeof f + 1;
    memcpy(wire + 1, f, sizeof f);
    wire[sizeof wire - 1] = 0;
    halo_wire_rx_t rx;
    halo_wire_rx_init(&rx);
    halo_wire_msg_t got[2];
    size_t k = receive(&rx, wire, sizeof wire, got, 2);
    CHECK(k == 0 && rx.bad == 1, "%u-byte payload: %zu frames out, %u bad", HALO_WIRE_MAX_PAYLOAD + 1, k, rx.bad);
}

/* Zeros in a row are an idle line, not damage. */
static void test_idle(void)
{
    halo_wire_rx_t rx;
    halo_wire_rx_init(&rx);
    const uint8_t idle[4] = {0};
    halo_wire_msg_t got[1];
    size_t k = receive(&rx, idle, sizeof idle, got, 1);
    CHECK(k == 0 && rx.bad == 0, "idle line: %zu frames, %u bad", k, rx.bad);
}

static void test_noise_between(void)
{
    halo_wire_rx_t rx;
    halo_wire_rx_init(&rx);
    uint8_t junk[300];
    for (size_t k = 0; k < sizeof junk; k++) {
        junk[k] = (uint8_t)test_rand();
    }
    halo_wire_msg_t got[64];
    receive(&rx, junk, sizeof junk, got, 64);   /* long garbage, overflowing the buffer */
    halo_wire_msg_t m = random_msg();
    uint8_t wire[HALO_WIRE_MAX_ENCODED];
    size_t n = halo_wire_encode(&m, wire);
    halo_wire_rx(&rx, 0, &got[0]);   /* a line glitch ends the garbage */
    size_t k = receive(&rx, wire, n, got, 2);
    CHECK(k == 1 && same(&got[0], &m), "frame after garbage lost");
}

/* A length past the payload buffer must be cut to what fits, not read past it. */
static void test_encode_clamps(void)
{
    halo_wire_msg_t m = random_msg(), got[2];
    m.len = HALO_WIRE_MAX_PAYLOAD + 1;
    uint8_t wire[HALO_WIRE_MAX_ENCODED];
    size_t n = halo_wire_encode(&m, wire);
    halo_wire_rx_t rx;
    halo_wire_rx_init(&rx);
    m.len = HALO_WIRE_MAX_PAYLOAD;
    CHECK(n <= HALO_WIRE_MAX_ENCODED && receive(&rx, wire, n, got, 2) == 1 && same(&got[0], &m),
          "overlong message not cut to %u bytes", HALO_WIRE_MAX_PAYLOAD);
}

/* What put_* wrote must be the length the receiver expects for that type. */
static void check_len(const halo_wire_msg_t *m, const char *what)
{
    CHECK(m->len == halo_wire_payload_len(m->type), "%s: put wrote %u bytes, the table says %d", what, m->len,
          halo_wire_payload_len(m->type));
}

static void test_payloads(void)
{
    halo_wire_msg_t m;
    halo_wire_put_empty(&m, HALO_WIRE_MSG_HELLO_REQ);
    check_len(&m, "hello request");

    halo_wire_hello_t h = {.version = 1, .firmware = 0x0102, .reset_reason = 3, .address = 0x1a2b3c4du}, h2;
    halo_wire_put_hello(&m, &h);
    check_len(&m, "hello");
    CHECK(halo_wire_get_hello(&m, &h2) && h2.version == 1 && h2.firmware == 0x0102 && h2.reset_reason == 3 &&
              h2.address == h.address,
          "hello");

    halo_wire_config_t c = {.address = 0x1a2b3c4du, .gain = 8, .retry_gain = 9, .max_attempts = 5, .listen = true}, c2;
    halo_wire_put_config(&m, &c);
    check_len(&m, "config");
    CHECK(halo_wire_get_config(&m, &c2) && c2.address == c.address && c2.gain == 8 && c2.retry_gain == 9 &&
              c2.max_attempts == 5 && c2.listen,
          "config");

    for (int bits = 0; bits < 8; bits++) {
        halo_lamp_t t = {.power = bits & 1, .front = bits & 2, .back = bits & 4, .brightness = 0xa5, .temperature = 42},
                    t2;
        halo_wire_put_set(&m, &t);
        check_len(&m, "set");
        CHECK(halo_wire_get_set(&m, &t2) && !memcmp(&t, &t2, sizeof t), "set %d", bits);
    }

    for (int bits = 0; bits < 64; bits++) {
        halo_wire_state_t s = {.event = HALO_WIRE_EVENT_REMOTE,
                               .known = {.lamp = {.power = bits & 1, .front = bits & 2, .back = bits & 4,
                                                  .brightness = 0x4c, .temperature = 100},
                                         .lights_known = bits & 8,
                                         .brightness_known = bits & 16,
                                         .temperature_known = bits & 32}},
                          s2;
        halo_wire_put_state(&m, &s);
        check_len(&m, "state");
        CHECK(halo_wire_get_state(&m, &s2) && !memcmp(&s, &s2, sizeof s), "state %d", bits);
    }

    halo_wire_heard_t d = {.address = 0x02040608u, .pid = 3, .payload = {0xc5, 0x4c}, .offset_khz = -120}, d2;
    halo_wire_put_heard(&m, &d);
    check_len(&m, "heard");
    CHECK(halo_wire_get_heard(&m, &d2) && d2.address == d.address && d2.pid == 3 && d2.payload[0] == 0xc5 &&
              d2.payload[1] == 0x4c && d2.offset_khz == -120,
          "heard");

    halo_wire_heartbeat_t b = {.uptime_s = 123456, .sent = 77, .acked = 70, .failed = 2, .heard_permille = 998,
                               .dropped = 5, .restarts = 1, .wire_bad = 9},
                          b2;
    halo_wire_put_heartbeat(&m, &b);
    check_len(&m, "heartbeat");
    CHECK(halo_wire_get_heartbeat(&m, &b2) && b2.uptime_s == b.uptime_s && b2.sent == 77 && b2.acked == 70 &&
              b2.failed == 2 && b2.heard_permille == 998 && b2.dropped == 5 && b2.restarts == 1 &&
              b2.wire_bad == 9,
          "heartbeat");

    uint8_t s;
    halo_wire_put_pair(&m, 30);
    check_len(&m, "pair");
    CHECK(halo_wire_get_pair(&m, &s) && s == 30, "pair");
    halo_wire_status_t st;
    halo_wire_put_ack(&m, HALO_WIRE_STATUS_BUSY);
    check_len(&m, "ack");
    CHECK(halo_wire_get_ack(&m, &st) && st == HALO_WIRE_STATUS_BUSY, "ack");

    /* Enum bytes from the wire are checked before they become enums. */
    m.payload[0] = HALO_WIRE_STATUS_BUSY + 1;
    CHECK(!halo_wire_get_ack(&m, &st), "unknown ACK status accepted");
    halo_wire_put_state(&m, &(halo_wire_state_t){.event = HALO_WIRE_EVENT_CANCELLED});
    CHECK(halo_wire_get_state(&m, &(halo_wire_state_t){0}), "last event refused");
    m.payload[0] = HALO_WIRE_EVENT_CANCELLED + 1;
    CHECK(!halo_wire_get_state(&m, &(halo_wire_state_t){0}), "unknown STATE event accepted");

    /* get_* must refuse the wrong length or type, and every length must fit a frame. */
    const halo_wire_type_t types[] = {HALO_WIRE_MSG_HELLO_REQ, HALO_WIRE_MSG_CONFIG, HALO_WIRE_MSG_SET,
                                      HALO_WIRE_MSG_PAIR,      HALO_WIRE_MSG_HELLO,  HALO_WIRE_MSG_ACK,
                                      HALO_WIRE_MSG_STATE,     HALO_WIRE_MSG_HEARD,  HALO_WIRE_MSG_HEARTBEAT};
    halo_wire_put_heartbeat(&m, &b);
    m.len--;
    CHECK(!halo_wire_get_heartbeat(&m, &b2), "short heartbeat accepted");
    halo_wire_put_set(&m, &(halo_lamp_t){0});
    CHECK(!halo_wire_get_state(&m, &(halo_wire_state_t){0}), "SET read as STATE");
    for (unsigned k = 0; k < sizeof types / sizeof types[0]; k++) {
        CHECK(halo_wire_payload_len(types[k]) >= 0 && halo_wire_payload_len(types[k]) <= (int)HALO_WIRE_MAX_PAYLOAD,
              "type %02X length", types[k]);
    }
    CHECK(halo_wire_payload_len(0x7f) < 0, "unknown type has a length");
}

/* A peer on another message version must still get its HELLO through, or the
 * C3 can't tell "wrong firmware" from "no radio".
 */
static void test_newer_peer(void)
{
    halo_wire_msg_t m, got;
    halo_wire_put_hello(&m, &(halo_wire_hello_t){.version = HALO_WIRE_VERSION + 1, .firmware = 7});
    m.seq = 9;
    uint8_t wire[HALO_WIRE_MAX_ENCODED];
    size_t n = halo_wire_encode(&m, wire);
    halo_wire_rx_t rx;
    halo_wire_rx_init(&rx);
    bool ok = false;
    for (size_t k = 0; k < n; k++) {
        ok |= halo_wire_rx(&rx, wire[k], &got);
    }
    halo_wire_hello_t h;
    CHECK(ok && halo_wire_get_hello(&got, &h) && h.version == HALO_WIRE_VERSION + 1,
          "a newer peer's HELLO didn't get through");
}

int main(void)
{
    test_srand(3);
    test_round_trip();
    test_damage();
    test_wrong_framing();
    test_too_long();
    test_idle();
    test_noise_between();
    test_encode_clamps();
    test_payloads();
    test_newer_peer();
    return check_report("test_wire");
}
