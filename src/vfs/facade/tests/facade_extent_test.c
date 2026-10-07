// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Exit criterion: overlapping-write extent correctness (the H8 fix), plus
 * holes-by-absence and the OBJ_MAP segment enumeration (O12).
 *
 * The map is pure in-memory, so this needs no media.
 */

#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "facade_extent.h"

#define TCHECK(cond, ...)                               \
        do {                                            \
            if (!(cond)) {                              \
                fprintf(stderr, "FAIL %s:%d: ",         \
                        __FILE__, __LINE__);            \
                fprintf(stderr, __VA_ARGS__);           \
                fprintf(stderr, "\n");                  \
                abort();                                \
            }                                           \
        } while (0)

/* A log of the physical ranges the map reports freed. */
struct freelog {
    struct {
        chimera_media_block_t phys;
        uint32_t              n;
    }   ent[32];
    int count;
};

static void
on_freed(
    chimera_media_block_t phys,
    uint32_t              n,
    void                 *priv)
{
    struct freelog *l = priv;

    TCHECK(l->count < 32, "freelog overflow");
    l->ent[l->count].phys = phys;
    l->ent[l->count].n    = n;
    l->count++;
} /* on_freed */

/* phys backing a mapped logical block (record start phys + offset). */
static chimera_media_block_t
phys_at(
    struct facade_extent_map *m,
    uint64_t                  lb)
{
    struct facade_extent ext;

    TCHECK(facade_extent_map_lookup(m, lb, &ext) == 1, "block %llu not mapped",
           (unsigned long long) lb);
    return ext.phys_block + (lb - ext.logical_block);
} /* phys_at */

static void
assert_hole(
    struct facade_extent_map *m,
    uint64_t                  lb)
{
    struct facade_extent ext;

    TCHECK(facade_extent_map_lookup(m, lb, &ext) == 0, "block %llu should be a hole",
           (unsigned long long) lb);
    TCHECK(ext.kind == CHIMERA_SC_SEG_HOLE, "hole kind");
} /* assert_hole */

int
main(void)
{
    struct facade_extent_map *m = facade_extent_map_create();
    struct freelog            l = { { { 0, 0 } }, 0 };
    struct chimera_sc_segment segs[16];
    uint32_t                  nsegs, more;

    /* A = [0,8) -> phys 1000. */
    facade_extent_map_insert(m, 0, 8, 1000, CHIMERA_SC_SEG_DATA, 1, on_freed, &l);
    TCHECK(l.count == 0, "no overlap yet");

    /* B = [4,12) -> phys 2000 (tail overlap of A). */
    facade_extent_map_insert(m, 4, 8, 2000, CHIMERA_SC_SEG_DATA, 1, on_freed, &l);

    /* A's [4,8) (phys 1004..1008) must be the only freed range. */
    TCHECK(l.count == 1, "exactly one freed range, got %d", l.count);
    TCHECK(l.ent[0].phys == 1004 && l.ent[0].n == 4,
           "freed A tail wrong: phys=%llu n=%u",
           (unsigned long long) l.ent[0].phys, l.ent[0].n);

    /* No stale tail: [0,4) still A, [4,12) all B. */
    TCHECK(phys_at(m, 0) == 1000 && phys_at(m, 3) == 1003, "A head intact");
    TCHECK(phys_at(m, 4) == 2000 && phys_at(m, 5) == 2001 && phys_at(m, 11) == 2007,
           "B owns the overlap with no stale A bytes");

    /* C = [2,4) -> phys 3000, strictly inside A's surviving head [0,4). */
    l.count = 0;
    facade_extent_map_insert(m, 2, 2, 3000, CHIMERA_SC_SEG_DATA, 1, on_freed, &l);
    TCHECK(l.count == 1 && l.ent[0].phys == 1002 && l.ent[0].n == 2,
           "interior overlap should free exactly A[2,4) phys 1002 n 2");
    TCHECK(phys_at(m, 0) == 1000 && phys_at(m, 1) == 1001, "A[0,2) intact");
    TCHECK(phys_at(m, 2) == 3000 && phys_at(m, 3) == 3001, "C owns [2,4), no stale");

    /* Holes-by-absence. */
    assert_hole(m, 100);

    /* UNWRITTEN reservation at [20,24). */
    facade_extent_map_insert(m, 20, 4, 4000, CHIMERA_SC_SEG_UNWRITTEN, 1, NULL, NULL);
    {
        struct facade_extent ext;
        TCHECK(facade_extent_map_lookup(m, 21, &ext) == 1 &&
               ext.kind == CHIMERA_SC_SEG_UNWRITTEN, "unwritten mapped");
    }

    /* Segments over [0,16): DATA[0,2) DATA[2,4) DATA[4,12) HOLE[12,16). */
    nsegs = facade_extent_map_segments(m, 0, 16, segs, 16, &more);
    TCHECK(more == 0, "range fit");
    TCHECK(nsegs == 4, "expected 4 segments, got %u", nsegs);
    TCHECK(segs[0].kind == CHIMERA_SC_SEG_DATA &&
           segs[0].extent.offset == 0 &&
           segs[0].extent.length == (2ull << CHIMERA_MEDIA_BLOCK_SHIFT), "seg0");
    TCHECK(segs[1].kind == CHIMERA_SC_SEG_DATA &&
           segs[1].extent.offset == (2ull << CHIMERA_MEDIA_BLOCK_SHIFT) &&
           segs[1].extent.length == (2ull << CHIMERA_MEDIA_BLOCK_SHIFT), "seg1");
    TCHECK(segs[2].kind == CHIMERA_SC_SEG_DATA &&
           segs[2].extent.offset == (4ull << CHIMERA_MEDIA_BLOCK_SHIFT) &&
           segs[2].extent.length == (8ull << CHIMERA_MEDIA_BLOCK_SHIFT), "seg2");
    TCHECK(segs[3].kind == CHIMERA_SC_SEG_HOLE &&
           segs[3].extent.offset == (12ull << CHIMERA_MEDIA_BLOCK_SHIFT) &&
           segs[3].extent.length == (4ull << CHIMERA_MEDIA_BLOCK_SHIFT), "seg3 hole");

    /* Punch inside B: [5,7) becomes a hole, splitting B; frees phys 2001 n2. */
    l.count = 0;
    facade_extent_map_punch(m, 5, 7 - 5, on_freed, &l);
    TCHECK(l.count == 1 && l.ent[0].phys == 2001 && l.ent[0].n == 2,
           "punch should free B[5,7) phys 2001 n 2");
    assert_hole(m, 5);
    assert_hole(m, 6);
    TCHECK(phys_at(m, 4) == 2000, "B head [4,5) survives");
    TCHECK(phys_at(m, 7) == 2003 && phys_at(m, 11) == 2007, "B tail [7,12) survives");

    facade_extent_map_destroy(m);

    printf("facade_extent_test: ok\n");
    return 0;
} /* main */
