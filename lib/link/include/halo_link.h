/* SPDX-License-Identifier: GPL-3.0-only */
/* Turns "the lamp should look like this" into radio frames, and keeps track of
 * what the lamp actually is, from our ACKs and from what the remote sends.
 *
 * Pure logic: the caller does the radio work and the waiting:
 *
 *   while (halo_link_next(&l, &tx)) {
 *       wait tx.delay_ms, passing every decoded frame to halo_link_heard();
 *       if (!halo_link_pending(&l)) continue;   // the remote took over meanwhile
 *       send tx.frame at tx.gain;
 *       event = halo_link_result(&l, acked);
 *   }
 *
 * halo_link_set_target() may be called at any time: latest wins.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "halo_esb.h"
#include "halo_lamp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t address;
    uint8_t gain;            /* first two tries */
    uint8_t retry_gain;      /* from the third: a weak link gets a bit more power */
    uint8_t max_attempts;
    uint16_t retry_min_ms;   /* random gap before the second try, so we don't stay in step */
    uint16_t retry_max_ms;   /* with whatever made the first fail; each later gap is 3x wider */
} halo_link_config_t;

typedef struct {
    halo_esb_frame_t frame;
    uint8_t gain;
    uint16_t delay_ms;       /* wait this long before sending */
    uint8_t attempt;         /* 1 for a fresh frame */
} halo_link_tx_t;

typedef enum {
    HALO_LINK_NONE,
    HALO_LINK_DONE,          /* the lamp now matches the target */
    HALO_LINK_FAILED,        /* gave up; target dropped, and what the frame carried is now unknown */
    HALO_LINK_REMOTE,        /* the remote changed the lamp; known state updated */
    HALO_LINK_CANCELLED,     /* ...and that replaced a target we were still sending */
} halo_link_event_t;

typedef struct {
    halo_link_config_t cfg;
    halo_known_t known;
    halo_lamp_t target;
    bool has_target;
    bool retrying;           /* current isn't confirmed yet: the same payload again is its retry, same PID */
    bool awaiting_result;    /* next() handed out a frame, result() not called yet */
    halo_link_tx_t current;
    uint32_t rng;
} halo_link_t;

/* seed: anything random (esp_random on the S3). It picks the first PID, so a
 * reboot doesn't reuse the lamp's last one.
 */
void halo_link_init(halo_link_t *l, const halo_link_config_t *cfg, uint32_t seed);

/* Latest wins: replaces any target still being sent. Returns DONE when the lamp
 * already matches, so the caller gets an answer even when nothing is sent.
 * Power on with neither light means off: the lamp can't show that.
 */
halo_link_event_t halo_link_set_target(halo_link_t *l, const halo_lamp_t *target);

/* The next frame to send. False when there's nothing to do. */
bool halo_link_next(halo_link_t *l, halo_link_tx_t *tx);

#define HALO_LINK_RETRY_CAP_MS 1000u
/* The random gap before try `attempt` (2 or later) is drawn from [lo, hi]. */
void halo_link_retry_window(const halo_link_config_t *cfg, unsigned attempt, unsigned *lo, unsigned *hi);

/* Whether the frame next() handed out should still be sent. A remote press
 * during the wait withdraws it: sending it would undo what the person just did.
 */
bool halo_link_pending(const halo_link_t *l);

/* Report the outcome of the frame next() returned. */
halo_link_event_t halo_link_result(halo_link_t *l, bool acked);

/* Feed every frame the listener decodes. Ignores anything not from our lamp's remote. */
halo_link_event_t halo_link_heard(halo_link_t *l, const halo_esb_frame_t *frame);

#ifdef __cplusplus
}
#endif
