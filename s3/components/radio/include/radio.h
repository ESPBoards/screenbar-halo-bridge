/* SPDX-License-Identifier: GPL-3.0-only */
/* The radio task: owns the link layer and the radio, on core 1. Listens with
 * the ring whenever there's nothing to send, and restarts the S3 if the ring
 * stops hearing anything.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "halo_esb.h"
#include "halo_lamp.h"
#include "halo_link.h"

typedef struct {
    uint32_t address;                         /* 0: send nothing */
    uint8_t gain, retry_gain, max_attempts;   /* out of range: the defaults */
    bool listen;                              /* keep listening for the remote when idle */
} radio_config_t;

typedef struct {
    uint32_t sent, acked;   /* radio frames */
    uint16_t failed;        /* targets given up on */
    uint32_t lost;          /* remote frames heard but dropped: the radio's queue was full */
} radio_stats_t;

/* What the lamp is now, after every link event. HALO_LINK_NONE answers radio_sync. */
typedef void (*radio_state_fn)(halo_link_event_t event, const halo_known_t *known);

/* Every remote command heard, any address, while pairing. */
typedef void (*radio_heard_fn)(const halo_esb_frame_t *frame, float offset_hz);

/* Brings up the radio and starts its task and the listener. Both callbacks
 * run from then on, from the radio task and the listener's worker, so they
 * must already work and must not block.
 */
void radio_start(radio_state_fn on_state, radio_heard_fn on_heard);

/* From any task on core 0. False if the queue is full: tell the C3 to resend. */
bool radio_set(const halo_lamp_t *target);
bool radio_config(const radio_config_t *config);
bool radio_pair(uint8_t seconds);
bool radio_sync(void);   /* report the state with what we know */

uint32_t radio_address(void);
void radio_stats(radio_stats_t *out);
