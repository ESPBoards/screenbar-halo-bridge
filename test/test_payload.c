/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>

#include "check.h"
#include "halo_payload.h"

#define BRI HALO_PAYLOAD_BRIGHTNESS
#define TMP HALO_PAYLOAD_TEMPERATURE

/* Bytes the real lamp ACKed and visibly obeyed on 2026-11-14. */
static void test_encode_matches_lamp(void)
{
    const struct {
        halo_payload_t p;
        uint8_t want[2];
    } cases[] = {
        {{BRI, true, true, true, 0xa4}, {0xc5, 0xa4}},     /* both, 50 % */
        {{BRI, true, true, false, 0x92}, {0xc4, 0x92}},    /* front only */
        {{BRI, true, false, true, 0xb6}, {0x85, 0xb6}},    /* back only */
        {{BRI, false, true, false, 0x4c}, {0x44, 0x4c}},   /* off */
        {{TMP, true, true, true, 0}, {0xc3, 0x00}},        /* coldest */
        {{TMP, true, true, true, 100}, {0xc3, 0x64}},      /* warmest */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t out[2] = {0};
        bool ok = halo_payload_encode(&cases[i].p, out);
        CHECK(ok && out[0] == cases[i].want[0] && out[1] == cases[i].want[1], "case %u: got %02X%02X, want %02X%02X",
              i, out[0], out[1], cases[i].want[0], cases[i].want[1]);
    }
}

static void test_encode_rejects(void)
{
    uint8_t out[2];
    const halo_payload_t bad[] = {
        {BRI, true, false, false, 0xa4},                       /* no light to turn on */
        {BRI, true, true, true, 0x4b},                         /* below the floor */
        {BRI, true, true, true, 0xff},
        {TMP, true, true, true, 101},
        {HALO_PAYLOAD_AUTO, true, true, true, 1},              /* remote-only */
        {HALO_PAYLOAD_FAVOURITE, true, true, true, 0},
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(!halo_payload_encode(&bad[i], out), "bad case %u encoded", i);
    }
}

/* Frames the real remote sent, with what each press did. */
static void test_decode_remote(void)
{
    const struct {
        uint8_t in[2];
        halo_payload_kind_t kind;
        bool power, front, back;
    } cases[] = {
        {{0xc5, 0x9f}, BRI, true, true, true},                        /* dial */
        {{0xc3, 0x48}, TMP, true, true, true},                        /* dial in temperature mode */
        {{0xc4, 0xfe}, BRI, true, true, false},
        {{0x44, 0x4c}, BRI, false, true, false},                      /* power off */
        {{0xe1, 0x01}, HALO_PAYLOAD_AUTO, true, true, true},          /* auto button */
        {{0x60, 0x01}, HALO_PAYLOAD_AUTO, false, true, false},        /* from published protocol notes */
        {{0xc9, 0x00}, HALO_PAYLOAD_FAVOURITE, true, true, true},     /* heart */
        {{0xfa, 0xb8}, HALO_PAYLOAD_AUTO_STREAM, true, true, false},
        {{0xfb, 0xfe}, HALO_PAYLOAD_AUTO_STREAM, true, true, true},
        {{0xfc, 0x36}, HALO_PAYLOAD_AUTO_STREAM, true, true, false},
        {{0xfe, 0x00}, HALO_PAYLOAD_WAKE, true, true, false},
        {{0xff, 0x00}, HALO_PAYLOAD_WAKE, true, true, true},
        {{0x91, 0x00}, HALO_PAYLOAD_UNKNOWN, true, false, true},      /* bit 4 alone, never explained */
        {{0xc7, 0x10}, HALO_PAYLOAD_UNKNOWN, true, true, true},       /* two settings at once */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        halo_payload_t p = halo_payload_decode(cases[i].in);
        CHECK(p.kind == cases[i].kind, "%02X%02X: kind %d, want %d", cases[i].in[0], cases[i].in[1], p.kind,
              cases[i].kind);
        CHECK(p.power == cases[i].power && p.front == cases[i].front && p.back == cases[i].back,
              "%02X%02X: lights wrong", cases[i].in[0], cases[i].in[1]);
    }
}

static void test_round_trip(void)
{
    for (unsigned flags = 0; flags < 256; flags++) {
        for (unsigned value = 0; value < 256; value++) {
            uint8_t in[2] = {(uint8_t)flags, (uint8_t)value}, out[2];
            halo_payload_t p = halo_payload_decode(in);
            if (halo_payload_encode(&p, out)) {
                CHECK(out[0] == in[0] && out[1] == in[1], "%02X%02X re-encoded as %02X%02X", in[0], in[1], out[0],
                      out[1]);
            }
        }
    }
}

static void test_scales(void)
{
    CHECK(halo_brightness_from_level(1) == HALO_BRIGHTNESS_MIN, "level 1");
    CHECK(halo_brightness_from_level(255) == HALO_BRIGHTNESS_MAX, "level 255");
    CHECK(halo_brightness_from_level(0) == HALO_BRIGHTNESS_MIN, "level 0 clamps");
    CHECK(halo_brightness_to_level(0) == 1 && halo_brightness_to_level(0xff) == 255, "raw out of range clamps");
    uint8_t last = 0;
    for (unsigned level = 1; level <= 255; level++) {
        uint8_t raw = halo_brightness_from_level((uint8_t)level);
        CHECK(raw >= last, "brightness not monotonic at %u", level);
        last = raw;
    }
    /* What the listener reports back must re-send as the same byte. */
    for (unsigned raw = HALO_BRIGHTNESS_MIN; raw <= HALO_BRIGHTNESS_MAX; raw++) {
        uint8_t again = halo_brightness_from_level(halo_brightness_to_level((uint8_t)raw));
        CHECK(again == raw, "raw %02X -> level -> %02X", raw, again);
    }

    CHECK(halo_temperature_from_mired(HALO_MIRED_COLD) == HALO_TEMPERATURE_COLD &&
              halo_temperature_from_mired(HALO_MIRED_WARM) == HALO_TEMPERATURE_WARM,
          "ends");
    CHECK(halo_temperature_from_mired(100) == HALO_TEMPERATURE_COLD &&
              halo_temperature_from_mired(500) == HALO_TEMPERATURE_WARM,
          "clamps");
    CHECK(halo_temperature_to_mired(101) == HALO_MIRED_WARM && halo_temperature_to_mired(255) == HALO_MIRED_WARM,
          "temperature clamps");
    for (unsigned t = HALO_TEMPERATURE_COLD; t <= HALO_TEMPERATURE_WARM; t++) {
        uint8_t again = halo_temperature_from_mired(halo_temperature_to_mired((uint8_t)t));
        CHECK(again == t, "temperature %u -> mired -> %u", t, again);
    }
}

int main(void)
{
    test_encode_matches_lamp();
    test_encode_rejects();
    test_decode_remote();
    test_round_trip();
    test_scales();
    return check_report("test_payload");
}
