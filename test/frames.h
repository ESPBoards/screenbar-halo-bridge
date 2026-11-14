/* SPDX-License-Identifier: GPL-3.0-only */
/* Random Halo frames for the receive tests, and how to tell two apart. */
#pragma once
#include <stdbool.h>

#include "halo_esb.h"
#include "prng.h"

/* An ACK (empty, NO_ACK) or a command with a random payload, to a random address. */
static inline halo_esb_frame_t random_frame(bool ack)
{
    halo_esb_frame_t f = {.address = (uint32_t)test_rand() ^ ((uint32_t)test_rand() << 16), .pid = test_rand() & 3};
    if (ack) {
        f.no_ack = true;
    } else {
        f.length = 2;
        f.payload[0] = (uint8_t)test_rand();
        f.payload[1] = (uint8_t)test_rand();
    }
    return f;
}

static inline bool same_frame(const halo_esb_frame_t *a, const halo_esb_frame_t *b)
{
    return a->address == b->address && a->pid == b->pid && a->no_ack == b->no_ack && a->length == b->length &&
           (!a->length || (a->payload[0] == b->payload[0] && a->payload[1] == b->payload[1]));
}
