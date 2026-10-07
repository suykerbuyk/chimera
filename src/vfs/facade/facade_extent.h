// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * Backend-facade per-object extent map (contract 4.1 "read/written by extent";
 * O12 holes).
 *
 * The map is the object's logical-block space mapped onto physical media
 * blocks.  Representation (O12, stated explicitly): a logical-block-indexed
 * set of records, holes-by-absence.
 *
 *   - A record covers a logical run and names its physical start and a kind,
 *     CHIMERA_SC_SEG_DATA (written data) or CHIMERA_SC_SEG_UNWRITTEN (reserved
 *     by allocate, reads as zeros until written).
 *   - A HOLE is the ABSENCE of a record: any unmapped logical range reads as
 *     zeros (CHIMERA_SC_SEG_HOLE).  No HOLE records are stored; OBJ_MAP
 *     synthesizes them for the gaps.
 *
 * This maps one-to-one onto the contract's segment kinds, so the segments
 * enumeration feeds OBJ_MAP directly.  The record form is a compact,
 * serializable array so the map can later be the unit that moves whole to a
 * spoke (P5); serialization/movement itself is out of scope here.
 *
 * Overlapping writes (the H8 fix): an insert that overlaps existing records
 * splits and replaces the overlap in ONE mutation -- the new record wins for
 * the overlapping range, the old record's non-overlapping head/tail survive,
 * and the physical blocks of fully-replaced ranges are reported through
 * on_freed for the caller to return to the allocator.  A reader never sees a
 * torn map.
 *
 * The map is a pure in-memory structure (no media I/O); persisting it into
 * durable object records is a later facade unit.
 */

#include <stdint.h>
#include "media_types.h"   /* chimera_media_block_t */
#include "sc_request.h"    /* struct chimera_sc_segment, CHIMERA_SC_SEG_* */

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

struct facade_extent_map;

/* The resolved mapping of a logical block (a lookup result). */
struct facade_extent {
    uint64_t              logical_block;
    uint32_t              nblocks;
    uint32_t              kind;        /* CHIMERA_SC_SEG_DATA / _UNWRITTEN / _HOLE */
    chimera_media_block_t phys_block;  /* media lba; unused when kind != DATA/UNWRITTEN */
};

/* Reports physical blocks the map no longer references (overlap replace or
 * punch) so the caller can free them. */
typedef void (*facade_freed_cb_t)(
    chimera_media_block_t phys,
    uint32_t              nblocks,
    void                 *private_data);

struct facade_extent_map *
facade_extent_map_create(
    void);

void
facade_extent_map_destroy(
    struct facade_extent_map *map);

/* Map [logical_block, logical_block + nblocks) to physical phys, kind
 * CHIMERA_SC_SEG_DATA or CHIMERA_SC_SEG_UNWRITTEN.  Overlapping ranges are
 * split/replaced atomically; replaced physical ranges are reported via
 * on_freed (may be NULL). */
void
facade_extent_map_insert(
    struct facade_extent_map *map,
    uint64_t                  logical_block,
    uint32_t                  nblocks,
    chimera_media_block_t     phys,
    uint32_t                  kind,
    facade_freed_cb_t         on_freed,
    void                     *private_data);

/* Resolve one logical block.  Returns 1 and fills *r_ext (kind DATA/UNWRITTEN)
 * when mapped; returns 0 for a hole and fills *r_ext with kind HOLE spanning
 * the gap that contains logical_block. */
int
facade_extent_map_lookup(
    const struct facade_extent_map *map,
    uint64_t                        logical_block,
    struct facade_extent           *r_ext);

/* Enumerate [logical_block, logical_block + nblocks) as contiguous
 * DATA/HOLE/UNWRITTEN segments into segs[0..segs_cap).  Segment extents are in
 * BYTES (offset = block << 12, length = run << 12), matching chimera_sc_segment
 * for OBJ_MAP.  Returns the count written; *r_more (optional) is set nonzero
 * when the range did not fit. */
uint32_t
facade_extent_map_segments(
    const struct facade_extent_map *map,
    uint64_t                        logical_block,
    uint64_t                        nblocks,
    struct chimera_sc_segment      *segs,
    uint32_t                        segs_cap,
    uint32_t                       *r_more);

/* Drop any DATA/UNWRITTEN records in [logical_block, logical_block + nblocks)
 * (they become holes); freed physical ranges are reported via on_freed. */
void
facade_extent_map_punch(
    struct facade_extent_map *map,
    uint64_t                  logical_block,
    uint32_t                  nblocks,
    facade_freed_cb_t         on_freed,
    void                     *private_data);

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
