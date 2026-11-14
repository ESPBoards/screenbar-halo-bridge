/* SPDX-License-Identifier: GPL-3.0-only */
#include "bench.h"

#include "halo_gfsk.h"

float bench_hz[BENCH_NFM];
halo_detection_t bench_found[BENCH_FOUND];

void bench_gfsk_fill(size_t first, uint32_t *out, size_t count, void *ctx)
{
    halo_gfsk_fill(ctx, first, out, count);
}
