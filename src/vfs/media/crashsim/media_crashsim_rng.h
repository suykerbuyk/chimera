// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * The crash-sim's deterministic PRNG: SplitMix64.
 *
 * A self-contained 64-bit generator, so a crash replay from a seed is bit-for-
 * bit identical on every platform -- unlike libc rand(), whose sequence is
 * implementation-defined.  The crash policy consumes draws in a fixed order
 * (one per non-durable write record, in submission order), so the same seed
 * and the same write/flush trace always produce the same post-crash image.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

struct crashsim_rng {
    uint64_t state;
};

static inline void
crashsim_rng_seed(
    struct crashsim_rng *r,
    uint64_t             seed)
{
    r->state = seed;
} /* crashsim_rng_seed */

static inline uint64_t
crashsim_rng_next(struct crashsim_rng *r)
{
    uint64_t z = (r->state += 0x9E3779B97F4A7C15ULL);

    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
} /* crashsim_rng_next */

/* A draw in [0, n).  n must be > 0.  The modulo bias is immaterial for a
 * fault-injection simulator and keeps the draw deterministic and cheap. */
static inline uint64_t
crashsim_rng_below(
    struct crashsim_rng *r,
    uint64_t             n)
{
    return crashsim_rng_next(r) % n;
} /* crashsim_rng_below */

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
