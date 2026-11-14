/* SPDX-License-Identifier: GPL-3.0-only */
/* What the lamp looks like, and how much of that we actually know. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HALO_BRIGHTNESS_MIN     0x4cu   /* lamp ignores anything lower */
#define HALO_BRIGHTNESS_MAX     0xfeu
#define HALO_TEMPERATURE_COLD   0u
#define HALO_TEMPERATURE_WARM   100u

typedef struct {
    bool power;
    bool front, back;       /* kept while off, like the remote, so its power button brings them back */
    uint8_t brightness;     /* raw HALO_BRIGHTNESS_MIN..MAX */
    uint8_t temperature;    /* HALO_TEMPERATURE_COLD..WARM */
} halo_lamp_t;

typedef struct {
    halo_lamp_t lamp;
    bool lights_known;      /* power, front, back */
    bool brightness_known;  /* false after boot, auto mode or a favourite */
    bool temperature_known;
} halo_known_t;

#ifdef __cplusplus
}
#endif
