/* SPDX-License-Identifier: GPL-3.0-only */
#include "halo_payload.h"

static uint8_t lights(const halo_payload_t *p)
{
    return (uint8_t)((p->front ? HALO_FLAG_FRONT : 0) | (p->back ? HALO_FLAG_BACK : 0));
}

bool halo_payload_encode(const halo_payload_t *p, uint8_t out[2])
{
    if (!p->front && !p->back) {
        return false;   /* the lamp would have nothing to turn back on */
    }
    uint8_t flags = lights(p) | (p->power ? HALO_FLAG_POWER : 0);
    switch (p->kind) {
    case HALO_PAYLOAD_BRIGHTNESS:
        if (p->value < HALO_BRIGHTNESS_MIN || p->value > HALO_BRIGHTNESS_MAX) {
            return false;
        }
        flags |= HALO_FLAG_BRIGHTNESS;
        break;
    case HALO_PAYLOAD_TEMPERATURE:
        if (p->value > HALO_TEMPERATURE_WARM) {
            return false;
        }
        flags |= HALO_FLAG_TEMPERATURE;
        break;
    default:
        return false;
    }
    out[0] = flags;
    out[1] = p->value;
    return true;
}

static halo_payload_kind_t classify(uint8_t flags, uint8_t value)
{
    /* Order matters: wake and stream frames set nearly every bit. */
    if (flags >= 0xfd && value == 0) {
        return HALO_PAYLOAD_WAKE;
    }
    if ((flags & (HALO_FLAG_STREAM | HALO_FLAG_FAVOURITE)) == (HALO_FLAG_STREAM | HALO_FLAG_FAVOURITE)) {
        return HALO_PAYLOAD_AUTO_STREAM;
    }
    if (flags & HALO_FLAG_AUTO) {
        return HALO_PAYLOAD_AUTO;
    }
    if (flags & HALO_FLAG_STREAM) {
        return HALO_PAYLOAD_UNKNOWN;
    }
    if (flags & HALO_FLAG_FAVOURITE) {
        return HALO_PAYLOAD_FAVOURITE;
    }
    switch (flags & (HALO_FLAG_BRIGHTNESS | HALO_FLAG_TEMPERATURE)) {
    case HALO_FLAG_BRIGHTNESS:
        return HALO_PAYLOAD_BRIGHTNESS;
    case HALO_FLAG_TEMPERATURE:
        return HALO_PAYLOAD_TEMPERATURE;
    default:
        return HALO_PAYLOAD_UNKNOWN;
    }
}

halo_payload_t halo_payload_decode(const uint8_t in[2])
{
    return (halo_payload_t){
        .kind = classify(in[0], in[1]),
        .power = in[0] & HALO_FLAG_POWER,
        .front = in[0] & HALO_FLAG_FRONT,
        .back = in[0] & HALO_FLAG_BACK,
        .value = in[1],
    };
}

/* Integer division rounded to nearest, so round trips land back on the same step. */
static unsigned div_round(unsigned a, unsigned b)
{
    return (a + b / 2) / b;
}

uint8_t halo_brightness_from_level(uint8_t level)
{
    if (level < 1) {
        level = 1;
    }
    return (uint8_t)(HALO_BRIGHTNESS_MIN +
                     div_round((level - 1u) * (HALO_BRIGHTNESS_MAX - HALO_BRIGHTNESS_MIN), 254));
}

uint8_t halo_brightness_to_level(uint8_t raw)
{
    if (raw < HALO_BRIGHTNESS_MIN) {
        raw = HALO_BRIGHTNESS_MIN;
    }
    if (raw > HALO_BRIGHTNESS_MAX) {
        raw = HALO_BRIGHTNESS_MAX;
    }
    return (uint8_t)(1 + div_round((raw - HALO_BRIGHTNESS_MIN) * 254u, HALO_BRIGHTNESS_MAX - HALO_BRIGHTNESS_MIN));
}

uint8_t halo_temperature_from_mired(uint16_t mired)
{
    if (mired < HALO_MIRED_COLD) {
        mired = HALO_MIRED_COLD;
    }
    if (mired > HALO_MIRED_WARM) {
        mired = HALO_MIRED_WARM;
    }
    return (uint8_t)div_round((mired - HALO_MIRED_COLD) * HALO_TEMPERATURE_WARM, HALO_MIRED_WARM - HALO_MIRED_COLD);
}

uint16_t halo_temperature_to_mired(uint8_t temperature)
{
    if (temperature > HALO_TEMPERATURE_WARM) {
        temperature = HALO_TEMPERATURE_WARM;
    }
    return (uint16_t)(HALO_MIRED_COLD +
                      div_round(temperature * (HALO_MIRED_WARM - HALO_MIRED_COLD), HALO_TEMPERATURE_WARM));
}
