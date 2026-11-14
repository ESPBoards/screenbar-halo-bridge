/* SPDX-License-Identifier: GPL-3.0-only */
/* S3 DAC and ADC sample words: signed 10-bit I in bits 9..0, Q in bits 19..10. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* No clipping here: the ADC can't exceed the range and TX stays at amplitude 128. */
static inline uint32_t halo_iq_pack(int i, int q)
{
    return ((uint32_t)i & 1023u) | (((uint32_t)q & 1023u) << 10);
}

/* Shifts, not compare-and-subtract: the demodulator runs this 32k times per
 * capture. The cast and the right shift of a negative value are
 * implementation-defined; GCC and Clang sign-extend.
 */
static inline int halo_iq_i(uint32_t word)
{
    return (int32_t)(word << 22) >> 22;
}

static inline int halo_iq_q(uint32_t word)
{
    return halo_iq_i(word >> 10);
}

#ifdef __cplusplus
}
#endif
