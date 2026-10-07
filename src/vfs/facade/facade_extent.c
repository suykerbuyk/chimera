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
    uint32_t              owned;   /* 1 = this object's blocks; 0 = borrowed (clone) */
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
    uint32_t              kind,
    uint32_t              owned)
{
    struct facade_ext_rec *r = malloc(sizeof(*r));

    FACADE_CHECK(r != NULL, "oom extent record");
    r->logical_block = logical_block;
    r->nblocks       = nblocks;
    r->phys_block    = phys_block;
    r->kind          = kind;
    r->owned         = owned;
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

/* Report the physical blocks under [from, to) of record r to on_freed.  Only an
 * OWNED record is reported: a borrowed range (owned == 0) belongs to a clone
 * origin and is dropped from the map without being freed (D2 copy-on-write). */
static void
facade_report_freed(
    const struct facade_ext_rec *r,
    uint64_t                     from,
    uint64_t                     to,
    facade_freed_cb_t            on_freed,
    void                        *private_data)
{
    if (on_freed && r->owned && to > from) {
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
                                      r->kind, r->owned);
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
    uint32_t                  owned,
    facade_freed_cb_t         on_freed,
    void                     *private_data)
{
    struct facade_ext_rec *r;

    FACADE_CHECK(nblocks > 0, "insert of zero blocks");
    FACADE_CHECK(kind == CHIMERA_SC_SEG_DATA || kind == CHIMERA_SC_SEG_UNWRITTEN,
                 "insert kind must be DATA or UNWRITTEN");

    facade_clear_range(m, logical_block, logical_block + nblocks,
                       on_freed, private_data);

    r = facade_ext_rec_new(logical_block, nblocks, phys, kind, owned);
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
        r_ext->owned         = r->owned;
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
    r_ext->owned      = 0;
    return 0;
} /* facade_extent_map_lookup */

/* In-order walk of the stored records (no holes), via facade_free_next-style
 * rb successor traversal. */
static struct facade_ext_rec *
facade_ext_first(const struct facade_extent_map *m)
{
    struct facade_ext_rec *r;

    rb_tree_first(&m->tree, r);
    return r;
} /* facade_ext_first */

static struct facade_ext_rec *
facade_ext_next(
    const struct facade_extent_map *m,
    struct facade_ext_rec          *e)
{
    const struct rb_node *nil = &m->tree.nil;
    struct rb_node       *n   = &e->node;
    struct rb_node       *p;

    if (n->right != nil) {
        n = n->right;
        while (n->left != nil) {
            n = n->left;
        }
        return container_of(n, struct facade_ext_rec, node);
    }
    p = n->parent;
    while (p && p != nil && n == p->right) {
        n = p;
        p = p->parent;
    }
    return (!p || p == nil) ? NULL : container_of(p, struct facade_ext_rec, node);
} /* facade_ext_next */

SYMBOL_EXPORT struct facade_extent_map *
facade_extent_map_clone(
    const struct facade_extent_map *src,
    int                             force_borrow)
{
    struct facade_extent_map *dst = facade_extent_map_create();
    struct facade_ext_rec    *r;

    for (r = facade_ext_first(src); r; r = facade_ext_next(src, r)) {
        struct facade_ext_rec *c =
            facade_ext_rec_new(r->logical_block, r->nblocks, r->phys_block,
                               r->kind, force_borrow ? 0u : r->owned);

        rb_tree_insert(&dst->tree, logical_block, c);
    }
    return dst;
} /* facade_extent_map_clone */

SYMBOL_EXPORT void
facade_extent_map_foreach(
    const struct facade_extent_map *m,
    facade_extent_each_cb_t         cb,
    void                           *private_data)
{
    struct facade_ext_rec *r;

    for (r = facade_ext_first(m); r; r = facade_ext_next(m, r)) {
        struct facade_extent e;

        e.logical_block = r->logical_block;
        e.nblocks       = (uint32_t) r->nblocks;
        e.kind          = r->kind;
        e.phys_block    = r->phys_block;
        e.owned         = r->owned;
        cb(&e, private_data);
    }
} /* facade_extent_map_foreach */

SYMBOL_EXPORT uint32_t
facade_extent_map_count(const struct facade_extent_map *m)
{
    struct facade_ext_rec *r;
    uint32_t               n = 0;

    for (r = facade_ext_first(m); r; r = facade_ext_next(m, r)) {
        n++;
    }
    return n;
} /* facade_extent_map_count */

SYMBOL_EXPORT uint64_t
facade_extent_map_owned_blocks(const struct facade_extent_map *m)
{
    struct facade_ext_rec *r;
    uint64_t               n = 0;

    for (r = facade_ext_first(m); r; r = facade_ext_next(m, r)) {
        if (r->owned) {
            n += r->nblocks;
        }
    }
    return n;
} /* facade_extent_map_owned_blocks */

SYMBOL_EXPORT uint64_t
facade_extent_map_max_logical(const struct facade_extent_map *m)
{
    struct facade_ext_rec *r;
    uint64_t               max = 0;

    for (r = facade_ext_first(m); r; r = facade_ext_next(m, r)) {
        if (r->logical_block + r->nblocks > max) {
            max = r->logical_block + r->nblocks;
        }
    }
    return max;
} /* facade_extent_map_max_logical */

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
