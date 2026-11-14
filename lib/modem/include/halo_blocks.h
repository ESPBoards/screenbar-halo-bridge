/* SPDX-License-Identifier: GPL-3.0-only */
/* The producer half of always-on listening: folds raw samples into block sums
 * (halo_fm_blocks) in a ring as they arrive. It must keep pace with the radio,
 * so it does nothing else; halo_listen reads the ring, possibly on another core.
 */
#pragma once
/* C only, so no extern "C": C++ has no _Atomic. */
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "halo_fm.h"

#define HALO_BLOCKS_RING    16384u   /* 8 ms of blocks; a power of two */
#define HALO_BLOCKS_PUBLISH 512u     /* most blocks between updates of written: how far past it the producer writes */

typedef struct {
    int16_t *bi, *bq;           /* HALO_BLOCKS_RING each */
    _Atomic uint32_t written;   /* blocks so far; wraps, compare by difference */
    uint32_t carry[HALO_FM_DECIMATION];   /* the start of a block split across two feeds */
    unsigned ncarry;
} halo_blocks_t;

void halo_blocks_init(halo_blocks_t *b, int16_t *bi, int16_t *bq);
void halo_blocks_feed(halo_blocks_t *b, const uint32_t *raw, size_t count);

/* The next sample doesn't follow the last one: drop the partial block. */
void halo_blocks_break(halo_blocks_t *b);
