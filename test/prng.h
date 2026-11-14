/* SPDX-License-Identifier: GPL-3.0-only */
/* The tests' own random numbers. libc rand() gives different sequences on
 * macOS and Linux, and the statistical tests must see the same noise
 * everywhere. PRNG_SALT shifts every sequence, to check a limit isn't tuned
 * to one.
 */
#pragma once
#include <stdint.h>

#ifndef PRNG_SALT
#define PRNG_SALT 0u
#endif

#define TEST_RAND_MAX 0x7fffffff

static uint32_t prng_state = 2463534242u;

static inline void test_srand(unsigned seed)
{
    prng_state = (uint32_t)seed ^ (uint32_t)PRNG_SALT ^ 2463534242u;
    if (!prng_state) {
        prng_state = 2463534242u;
    }
}

/* xorshift32, returned like rand(): a non-negative int. */
static inline int test_rand(void)
{
    uint32_t x = prng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    prng_state = x;
    return (int)(x >> 1);
}
