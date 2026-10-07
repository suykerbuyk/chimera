// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Backend-facade per-object extent map.  See facade_extent.h for the
 * representation (logical-block-indexed records, holes-by-absence) and the
 * overlap/H8 contract.
 *
 * Records are held in a red-black tree keyed by logical start block and kept
 * non-overlapping.  An insert first clears its logical range -- trimming the
 * head/tail of a straddling record and removing enclosed records, reporting
 * the physical blocks it drops -- then inserts the new record, all in one
 * mutation.  A punch is the same clear without the insert.
 */

#include <stdlib.h>

#include "common/rbtree.h"

#include "facade_extent.h"
#include "facade_internal.h"   /* FACADE_CHECK */

struct facade_ext_rec {
    struct rb_node        node;
    uint64_t              logical_block;
    uint64_t              nblocks;
    chimera_media_block_t phys_block;
    uint32_t              kind;    /* CHIMERA_SC_SEG_DATA | CHIMERA_SC_SEG_UNWRITTEN */
};

struct facade_extent_map {
    struct rb_tree tree;
};

static void
facade_ext_rec_destroy(
    struct rb_node *node,
    void           *private_data)
{
    (void) private_data;
    free(container_of(node, struct facade_ext_rec, node));
} /* facade_ext_rec_destroy */

static struct facade_ext_rec *
facade_ext_rec_new(
    uint64_t              logical_block,
    uint64_t              nblocks,
    chimera_media_block_t phys_block,
    uint32_t              kind)
{
    struct facade_ext_rec *r = malloc(sizeof(*r));

    FACADE_CHECK(r != NULL, "oom extent record");
    r->logical_block = logical_block;
    r->nblocks       = nblocks;
    r->phys_block    = phys_block;
    r->kind          = kind;
    return r;
} /* facade_ext_rec_new */

SYMBOL_EXPORT struct facade_extent_map *
facade_extent_map_create(void)
{
    struct facade_extent_map *m = malloc(sizeof(*m));

    FACADE_CHECK(m != NULL, "oom extent map");
    rb_tree_init(&m->tree);
    return m;
} /* facade_extent_map_create */

SYMBOL_EXPORT void
facade_extent_map_destroy(struct facade_extent_map *m)
{
    if (!m) {
        return;
    }
    rb_tree_destroy(&m->tree, facade_ext_rec_destroy, NULL);
    free(m);
} /* facade_extent_map_destroy */

/* Report the physical blocks under [from, to) of record r to on_freed. */
static void
facade_report_freed(
    const struct facade_ext_rec *r,
    uint64_t                     from,
    uint64_t                     to,
    facade_freed_cb_t            on_freed,
    void                        *private_data)
{
    if (on_freed && to > from) {
        on_freed(r->phys_block + (from - r->logical_block),
                 (uint32_t) (to - from), private_data);
    }
} /* facade_report_freed */

/* Remove every mapping in [lb, end) in one pass, reporting dropped physical
 * ranges via on_freed.  Leaves the range a hole.
 *
 * Enclosed records are unlinked and chained onto a local dead-list (via the
 * reused rb_node.parent link, valid once detached) and freed only after the
 * walk completes -- so no node is freed while the tree still walks it, which
 * also keeps the loop free of the free-then-requery pattern scan-build cannot
 * model. */
static void
facade_clear_range(
    struct facade_extent_map *m,
    uint64_t                  lb,
    uint64_t                  end,
    facade_freed_cb_t         on_freed,
    void                     *private_data)
{
    struct facade_ext_rec *r, *tail;
    struct rb_node        *dead = NULL;
    uint64_t               rend, send;

    /* A record starting strictly before lb that reaches into the range. */
    rb_tree_query_floor(&m->tree, lb, logical_block, r);
    if (r && r->logical_block < lb && r->logical_block + r->nblocks > lb) {
        rend = r->logical_block + r->nblocks;
        facade_report_freed(r, lb, rend < end ? rend : end, on_freed, private_data);
        if (rend > end) {
            /* The record encloses the range: split into head + tail. */
            tail = facade_ext_rec_new(end, rend - end,
                                      r->phys_block + (end - r->logical_block),
                                      r->kind);
            r->nblocks = lb - r->logical_block;
            rb_tree_insert(&m->tree, logical_block, tail);
            return;
        }
        r->nblocks = lb - r->logical_block;   /* trim tail; start key unchanged */
    }

    /* Records starting at or after lb that overlap the range. */
    for (;;) {
        struct facade_ext_rec *s;

        rb_tree_query_ceil(&m->tree, lb, logical_block, s);
        if (!s || s->logical_block >= end) {
            break;
        }
        send = s->logical_block + s->nblocks;
        if (send <= end) {
            /* Fully inside: unlink now, free after the walk. */
            facade_report_freed(s, s->logical_block, send, on_freed, private_data);
            rb_tree_remove(&m->tree, &s->node);
            s->node.parent = dead;        /* reuse the detached link as a dead-list */
            dead           = &s->node;
        } else {
            /* Starts in range, extends beyond: trim its head (start key
             * changes, so reinsert) and stop. */
            facade_report_freed(s, s->logical_block, end, on_freed, private_data);
            rb_tree_remove(&m->tree, &s->node);
            s->phys_block   += end - s->logical_block;
            s->nblocks       = send - end;
            s->logical_block = end;
            rb_tree_insert(&m->tree, logical_block, s);
            break;
        }
    }

    while (dead) {
        struct rb_node *nx = dead->parent;
        free(container_of(dead, struct facade_ext_rec, node));
        dead = nx;
    }
} /* facade_clear_range */

SYMBOL_EXPORT void
facade_extent_map_insert(
    struct facade_extent_map *m,
    uint64_t                  logical_block,
    uint32_t                  nblocks,
    chimera_media_block_t     phys,
    uint32_t                  kind,
    facade_freed_cb_t         on_freed,
    void                     *private_data)
{
    struct facade_ext_rec *r;

    FACADE_CHECK(nblocks > 0, "insert of zero blocks");
    FACADE_CHECK(kind == CHIMERA_SC_SEG_DATA || kind == CHIMERA_SC_SEG_UNWRITTEN,
                 "insert kind must be DATA or UNWRITTEN");

    facade_clear_range(m, logical_block, logical_block + nblocks,
                       on_freed, private_data);

    r = facade_ext_rec_new(logical_block, nblocks, phys, kind);
    rb_tree_insert(&m->tree, logical_block, r);
} /* facade_extent_map_insert */

SYMBOL_EXPORT void
facade_extent_map_punch(
    struct facade_extent_map *m,
    uint64_t                  logical_block,
    uint32_t                  nblocks,
    facade_freed_cb_t         on_freed,
    void                     *private_data)
{
    if (nblocks == 0) {
        return;
    }
    facade_clear_range(m, logical_block, logical_block + nblocks,
                       on_freed, private_data);
} /* facade_extent_map_punch */

SYMBOL_EXPORT int
facade_extent_map_lookup(
    const struct facade_extent_map *m,
    uint64_t                        logical_block,
    struct facade_extent           *r_ext)
{
    struct facade_ext_rec *r, *s;
    uint64_t               hole_start, hole_end;

    rb_tree_query_floor(&m->tree, logical_block, logical_block, r);
    if (r && r->logical_block <= logical_block &&
        r->logical_block + r->nblocks > logical_block) {
        r_ext->logical_block = r->logical_block;
        r_ext->nblocks       = (uint32_t) r->nblocks;
        r_ext->kind          = r->kind;
        r_ext->phys_block    = r->phys_block;
        return 1;
    }

    /* Hole: the gap containing logical_block. */
    hole_start = (r && r->logical_block + r->nblocks <= logical_block)
                 ? r->logical_block + r->nblocks : 0;
    rb_tree_query_ceil(&m->tree, logical_block, logical_block, s);
    hole_end = s ? s->logical_block : UINT64_MAX;

    r_ext->logical_block = hole_start;
    r_ext->nblocks       = (hole_end - hole_start) > UINT32_MAX
                           ? 0 /* unbounded/overlong hole */
                           : (uint32_t) (hole_end - hole_start);
    r_ext->kind       = CHIMERA_SC_SEG_HOLE;
    r_ext->phys_block = 0;
    return 0;
} /* facade_extent_map_lookup */

SYMBOL_EXPORT uint32_t
facade_extent_map_segments(
    const struct facade_extent_map *m,
    uint64_t                        logical_block,
    uint64_t                        nblocks,
    struct chimera_sc_segment      *segs,
    uint32_t                        segs_cap,
    uint32_t                       *r_more)
{
    uint64_t               cur = logical_block;
    uint64_t               end = logical_block + nblocks;
    uint32_t               out = 0;
    struct facade_ext_rec *r, *s;
    uint64_t               seg_end;
    uint32_t               kind;

    while (cur < end && out < segs_cap) {
        rb_tree_query_floor(&m->tree, cur, logical_block, r);
        if (r && r->logical_block <= cur && r->logical_block + r->nblocks > cur) {
            seg_end = r->logical_block + r->nblocks;
            kind    = r->kind;
        } else {
            rb_tree_query_ceil(&m->tree, cur, logical_block, s);
            seg_end = s ? s->logical_block : end;
            kind    = CHIMERA_SC_SEG_HOLE;
        }
        if (seg_end > end) {
            seg_end = end;
        }

        segs[out].extent.offset = cur << CHIMERA_MEDIA_BLOCK_SHIFT;
        segs[out].extent.length = (seg_end - cur) << CHIMERA_MEDIA_BLOCK_SHIFT;
        segs[out].kind          = kind;
        segs[out].reserved      = 0;
        out++;
        cur = seg_end;
    }

    if (r_more) {
        *r_more = cur < end ? 1u : 0u;
    }
    return out;
} /* facade_extent_map_segments */
