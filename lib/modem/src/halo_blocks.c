/* SPDX-License-Identifier: GPL-3.0-only */
#include "halo_blocks.h"

#define D    HALO_FM_DECIMATION
#define MASK (HALO_BLOCKS_RING - 1u)
_Static_assert((HALO_BLOCKS_RING & MASK) == 0, "the ring is indexed with a mask");

void halo_blocks_init(halo_blocks_t *b, int16_t *bi, int16_t *bq)
{
    b->bi = bi;
    b->bq = bq;
    b->ncarry = 0;
    atomic_store(&b->written, 0);
}

void halo_blocks_feed(halo_blocks_t *b, const uint32_t *raw, size_t count)
{
    uint32_t w = atomic_load_explicit(&b->written, memory_order_relaxed);
    if (b->ncarry) {
        while (b->ncarry < D && count) {
            b->carry[b->ncarry++] = *raw++;
            count--;
        }
        if (b->ncarry < D) {
            return;
        }
        halo_fm_blocks(b->carry, D, b->bi + (w & MASK), b->bq + (w & MASK));
        w++;
        b->ncarry = 0;
    }
    size_t blocks = count / D;
    while (blocks) {
        size_t at = w & MASK, run = HALO_BLOCKS_RING - at;
        run = run < blocks ? run : blocks;
        run = run < HALO_BLOCKS_PUBLISH ? run : HALO_BLOCKS_PUBLISH;
        halo_fm_blocks(raw, run * D, b->bi + at, b->bq + at);
        raw += run * D;
        count -= run * D;
        w += (uint32_t)run;
        blocks -= run;
        atomic_store_explicit(&b->written, w, memory_order_release);   /* often, so the consumer may lag further */
    }
    while (count--) {
        b->carry[b->ncarry++] = *raw++;
    }
    atomic_store_explicit(&b->written, w, memory_order_release);
}

void halo_blocks_break(halo_blocks_t *b)
{
    b->ncarry = 0;
}
