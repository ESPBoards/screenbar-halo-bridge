/* SPDX-License-Identifier: GPL-3.0-only */
/* The two payload bytes of a command: [flags][value].
 *
 * Every state frame is absolute (power, both lights, one setting), so a lost
 * frame never leaves the lamp half-updated.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "halo_lamp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HALO_FLAG_POWER       0x80u
#define HALO_FLAG_FRONT       0x40u
#define HALO_FLAG_AUTO        0x20u
#define HALO_FLAG_STREAM      0x10u   /* with FAVOURITE: auto-mode stream */
#define HALO_FLAG_FAVOURITE   0x08u
#define HALO_FLAG_BRIGHTNESS  0x04u
#define HALO_FLAG_TEMPERATURE 0x02u
#define HALO_FLAG_BACK        0x01u

#define HALO_MIRED_COLD 154u   /* 6500 K */
#define HALO_MIRED_WARM 370u   /* 2700 K */

typedef enum {
    HALO_PAYLOAD_BRIGHTNESS,
    HALO_PAYLOAD_TEMPERATURE,
    HALO_PAYLOAD_AUTO,          /* remote's auto button; value = press number */
    HALO_PAYLOAD_AUTO_STREAM,   /* sent while auto runs, probably sensor data */
    HALO_PAYLOAD_FAVOURITE,
    HALO_PAYLOAD_WAKE,          /* remote waking up, carries no state */
    HALO_PAYLOAD_UNKNOWN,
} halo_payload_kind_t;

typedef struct {
    halo_payload_kind_t kind;
    bool power;
    bool front;   /* lights that are lit while power is on; kept when off so */
    bool back;    /* the remote's power button brings the same ones back */
    uint8_t value;
} halo_payload_t;

/* Only brightness and temperature: auto and favourite stay with the remote. */
bool halo_payload_encode(const halo_payload_t *p, uint8_t out[2]);

/* Lights fields are only meaningful for brightness, temperature and favourite; auto frames don't carry them
 * reliably (60 01 has power clear).
 */
halo_payload_t halo_payload_decode(const uint8_t in[2]);

/* Home Assistant scales. Linear, because the lamp applies its own curve. */
uint8_t halo_brightness_from_level(uint8_t level);   /* 1..255 */
uint8_t halo_brightness_to_level(uint8_t raw);
uint8_t halo_temperature_from_mired(uint16_t mired);
uint16_t halo_temperature_to_mired(uint8_t temperature);

#ifdef __cplusplus
}
#endif
