// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Layout and PRNG proof for the crash-sim provider.  No libevpl, no I/O: pins
 * the ABI-visible struct layouts with _Static_assert (always on) and exercises
 * the SplitMix64 determinism contract with CHECK (survives NDEBUG).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "crashsim/chimera_media_crashsim.h"
#include "crashsim/media_crashsim_rng.h"

#define CHECK(cond)                                                      \
        do {                                                             \
            if (!(cond)) {                                               \
                fprintf(stderr, "CHECK failed: %s (%s:%d)\n",            \
                        #cond, __FILE__, __LINE__);                      \
                exit(1);                                                 \
            }                                                            \
        } while (0)

/* The config: an 8-byte pointer, a u64 seed, three u32 weights, a u8 switch and
 * a 7-byte reserved tail, no implicit padding on LP64 (sizeof 40). */
_Static_assert(offsetof(struct chimera_media_crashsim_config, delegate) == 0,
               "delegate at 0");
_Static_assert(offsetof(struct chimera_media_crashsim_config, seed) == 8,
               "seed at 8");
_Static_assert(offsetof(struct chimera_media_crashsim_config, w_keep) == 16,
               "w_keep at 16");
_Static_assert(offsetof(struct chimera_media_crashsim_config, w_drop) == 20,
               "w_drop at 20");
_Static_assert(offsetof(struct chimera_media_crashsim_config, w_tear) == 24,
               "w_tear at 24");
_Static_assert(offsetof(struct chimera_media_crashsim_config, reorder) == 28,
               "reorder at 28");
#if defined(__LP64__) || (defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8)
_Static_assert(sizeof(struct chimera_media_crashsim_config) == 40,
               "config is 40 bytes on LP64");
_Static_assert(sizeof(struct chimera_media_crashsim_control) == 5 * sizeof(void *),
               "control is five function pointers");
_Static_assert(sizeof(struct chimera_media_crashsim_stats) == 5 * sizeof(uint64_t),
               "stats is five u64 counters");
#endif /* LP64 */

/* The five stats counters are contiguous u64, no padding. */
_Static_assert(offsetof(struct chimera_media_crashsim_stats, writes_total) == 0,
               "writes_total at 0");
_Static_assert(offsetof(struct chimera_media_crashsim_stats, staged_current) == 8,
               "staged_current at 8");
_Static_assert(offsetof(struct chimera_media_crashsim_stats, last_crash_kept) == 16,
               "last_crash_kept at 16");
_Static_assert(offsetof(struct chimera_media_crashsim_stats, last_crash_dropped) == 24,
               "last_crash_dropped at 24");
_Static_assert(offsetof(struct chimera_media_crashsim_stats, last_crash_torn) == 32,
               "last_crash_torn at 32");

int
main(void)
{
    struct crashsim_rng a, b, c;
    int                 i;
    int                 differ = 0;

    /* Same seed -> identical sequence; different seed -> diverges. */
    crashsim_rng_seed(&a, 0x1234567890ABCDEFULL);
    crashsim_rng_seed(&b, 0x1234567890ABCDEFULL);
    crashsim_rng_seed(&c, 0x1234567890ABCDEEULL);
    for (i = 0; i < 64; i++) {
        uint64_t va = crashsim_rng_next(&a);
        uint64_t vb = crashsim_rng_next(&b);
        uint64_t vc = crashsim_rng_next(&c);

        CHECK(va == vb);
        if (va != vc) {
            differ = 1;
        }
    }
    CHECK(differ);

    /* rng_below stays in range and is reproducible. */
    crashsim_rng_seed(&a, 99);
    crashsim_rng_seed(&b, 99);
    for (i = 0; i < 1000; i++) {
        uint64_t n  = (uint64_t) (i % 37) + 1;
        uint64_t ra = crashsim_rng_below(&a, n);
        uint64_t rb = crashsim_rng_below(&b, n);

        CHECK(ra < n);
        CHECK(ra == rb);
    }

    printf("media_crashsim_header_test: ok\n");
    return 0;
} /* main */
