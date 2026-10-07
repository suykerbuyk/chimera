// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Exit criterion: NO_SPACE is raised on exhaustion (contract section 11).
 *
 * The allocator must return FACADE_E_NO_SPACE cleanly -- no abort, free count
 * stays correct, the allocator stays usable -- and a fragmentation variant
 * shows NO_SPACE for a multi-block request that no single run can satisfy even
 * though enough blocks are free.
 */

#undef NDEBUG
#include <assert.h>

#include "facade_test_util.h"

int
main(void)
{
    struct chimera_media_space *space;
    struct facade_alloc        *a;
    chimera_media_block_t       base, s;
    uint64_t                    total, e;

    facade_test_media_reset();

    /* --- exhaustion --- */
    space = tu_media_open("nospace", 128u << 10);   /* 128 KiB => 32 blocks */
    tu_format(space);
    a = tu_open(space);

    total = facade_alloc_total_blocks(a);
    TCHECK(total > 0 && total < 64, "small data region expected, got %llu",
           (unsigned long long) total);

    TCHECK(facade_alloc_blocks(a, (uint32_t) total, &base, &e) == FACADE_OK,
           "allocate the whole region");
    TCHECK(facade_alloc_free_blocks(a) == 0, "fully allocated");

    /* Exhausted: a further request is NO_SPACE, cleanly. */
    TCHECK(facade_alloc_blocks(a, 1, &s, &e) == FACADE_E_NO_SPACE,
           "exhausted pool must raise NO_SPACE");
    TCHECK(facade_alloc_free_blocks(a) == 0, "still fully allocated after refusal");

    /* Free one block and the allocator is usable again. */
    facade_alloc_free(a, base, 1);
    TCHECK(facade_alloc_free_blocks(a) == 1, "one block free");
    TCHECK(facade_alloc_blocks(a, 1, &s, &e) == FACADE_OK, "alloc after free");
    TCHECK(s == base, "the freed block is reused");

    tu_close(a);
    tu_media_close(space);

    /* --- fragmentation: enough free blocks, but no contiguous run --- */
    facade_test_media_reset();
    space = tu_media_open("frag", 128u << 10);
    tu_format(space);
    a     = tu_open(space);
    total = facade_alloc_total_blocks(a);

    TCHECK(facade_alloc_blocks(a, (uint32_t) total, &base, &e) == FACADE_OK,
           "allocate the whole region");

    /* Three isolated 1-block holes, each separated by an allocated block. */
    facade_alloc_free(a, base, 1);
    facade_alloc_free(a, base + 2, 1);
    facade_alloc_free(a, base + 4, 1);
    TCHECK(facade_alloc_free_blocks(a) == 3, "three blocks free");

    /* A 2-block request cannot be satisfied by any single run. */
    TCHECK(facade_alloc_blocks(a, 2, &s, &e) == FACADE_E_NO_SPACE,
           "fragmented pool: no 2 contiguous blocks => NO_SPACE");
    /* A 1-block request still succeeds. */
    TCHECK(facade_alloc_blocks(a, 1, &s, &e) == FACADE_OK,
           "1-block request still satisfiable");

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();

    printf("facade_enospc_test: ok\n");
    return 0;
} /* main */
