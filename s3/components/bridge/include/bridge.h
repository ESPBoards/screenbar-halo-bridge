/* SPDX-License-Identifier: GPL-3.0-only */
/* The serial link to the C3, on core 0: requests in, states, pairing frames
 * and a heartbeat out. UART1 normally; USB in a test build, so a PC can stand
 * in for the C3. All wire translation happens here.
 */
#pragma once
#include "halo_esb.h"
#include "halo_lamp.h"
#include "halo_link.h"

/* Init before the radio can report, start once the radio is up: the C3's
 * requests go straight to it.
 */
void bridge_init(void);
void bridge_start(void);

/* The radio's callbacks (radio_state_fn, radio_heard_fn). From any task; never
 * block. Dropped (and logged) if the queue to the C3 is full.
 */
void bridge_state(halo_link_event_t event, const halo_known_t *known);
void bridge_heard(const halo_esb_frame_t *frame, float offset_hz);
