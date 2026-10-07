// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Exit criterion: unit tests for allocation and free.
 *
 * Covers disjointness and exact free-block accounting, best-fit (the smallest
 * adequate run wins over the big remainder), and coalescing (after freeing
 * every run the whole region is allocatable again as one run).
 */

#undef NDEBUG
#include <assert.h>

#include "facade_test_util.h"

static int
disjoint(
    chimera_media_block_t a,
    uint32_t              na,
    chimera_media_block_t b,
    uint32_t              nb)
{
    return a + na <= b || b + nb <= a;
} /* disjoint */

int
main(void)
{
    struct chimera_media_space *space;
    struct facade_alloc        *a;
    chimera_media_block_t       base, s, s1, s2, s3;
    chimera_media_block_t       r[7];
    uint64_t                    total, e;
    int                         i;

    facade_test_media_reset();

    /* --- accounting + disjointness --- */
    space = tu_media_open("alloc", 4u << 20);   /* 4 MiB => 1024 blocks */
    tu_format(space);
    a = tu_open(space);

    total = facade_alloc_total_blocks(a);
    TCHECK(total > 64, "expected a usable data region, got %llu",
           (unsigned long long) total);
    TCHECK(facade_alloc_free_blocks(a) == total, "fresh free count");

    TCHECK(facade_alloc_blocks(a, 10, &s1, &e) == FACADE_OK, "alloc 10");
    TCHECK(facade_alloc_blocks(a, 20, &s2, &e) == FACADE_OK, "alloc 20");
    TCHECK(facade_alloc_blocks(a, 5, &s3, &e) == FACADE_OK, "alloc 5");
    TCHECK(facade_alloc_free_blocks(a) == total - 35, "free count after allocs");
    TCHECK(disjoint(s1, 10, s2, 20) && disjoint(s1, 10, s3, 5) &&
           disjoint(s2, 20, s3, 5), "allocations must be disjoint");

    facade_alloc_free(a, s1, 10);
    facade_alloc_free(a, s2, 20);
    facade_alloc_free(a, s3, 5);
    TCHECK(facade_alloc_free_blocks(a) == total, "free count back to full");

    tu_close(a);
    tu_media_close(space);

    /* --- best-fit: the smallest adequate run wins over the remainder --- */
    facade_test_media_reset();
    space = tu_media_open("bestfit", 4u << 20);
    tu_format(space);
    a     = tu_open(space);
    total = facade_alloc_total_blocks(a);
    TCHECK(facade_alloc_free_blocks(a) == total, "fresh free (bestfit)");

    /* Seven contiguous single-block allocations (tail-cut => descending). */
    for (i = 0; i < 7; i++) {
        TCHECK(facade_alloc_blocks(a, 1, &r[i], &e) == FACADE_OK, "carve r[%d]", i);
    }
    /* Free r[1] (an isolated 1-block hole) and the adjacent pair r[3],r[4]
     * (a 2-block hole), each separated from the big remainder and from each
     * other by still-allocated blocks. */
    facade_alloc_free(a, r[1], 1);
    facade_alloc_free(a, r[4], 1);
    facade_alloc_free(a, r[3], 1);   /* r[3],r[4] coalesce into one 2-block hole */

    /* A 1-block request takes the exact 1-block hole, not the big remainder. */
    TCHECK(facade_alloc_blocks(a, 1, &s, &e) == FACADE_OK, "best-fit 1");
    TCHECK(s == r[1], "best-fit should reuse the exact 1-block hole");

    /* A 2-block request takes the 2-block hole (tail-cut start == r[4]). */
    TCHECK(facade_alloc_blocks(a, 2, &s, &e) == FACADE_OK, "best-fit 2");
    TCHECK(s == r[4], "best-fit should reuse the exact 2-block hole");

    tu_close(a);
    tu_media_close(space);

    /* --- coalescing: free everything, then the whole region is one run --- */
    facade_test_media_reset();
    space = tu_media_open("coalesce", 4u << 20);
    tu_format(space);
    a     = tu_open(space);
    total = facade_alloc_total_blocks(a);

    /* Allocate the region in three contiguous pieces, then free them in an
     * order that forces merges on both sides. */
    TCHECK(facade_alloc_blocks(a, 100, &s1, &e) == FACADE_OK, "piece 1");
    TCHECK(facade_alloc_blocks(a, 100, &s2, &e) == FACADE_OK, "piece 2");
    TCHECK(facade_alloc_blocks(a, 100, &s3, &e) == FACADE_OK, "piece 3");
    facade_alloc_free(a, s1, 100);
    facade_alloc_free(a, s3, 100);
    facade_alloc_free(a, s2, 100);   /* bridges s1 and s3 */
    TCHECK(facade_alloc_free_blocks(a) == total, "all free");

    /* The whole region allocatable as a single run proves full coalescing. */
    TCHECK(facade_alloc_blocks(a, (uint32_t) total, &base, &e) == FACADE_OK,
           "whole region in one run (coalesced)");
    TCHECK(facade_alloc_free_blocks(a) == 0, "fully allocated");
    facade_alloc_free(a, base, (uint32_t) total);

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();

    printf("facade_alloc_test: ok\n");
    return 0;
} /* main */
