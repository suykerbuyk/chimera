// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Backend-facade block allocator.  See facade_alloc.h for the interface and
 * the threading / media-vtable rules.
 *
 * Free space is an offset-keyed red-black tree of free extents plus a running
 * free-block count (O(1) NO_SPACE).  Allocation is best-fit, cut from the tail
 * of the chosen extent so the tree key (start) never moves.  The authoritative
 * free set is in memory; it is condensed to the on-media checkpoint at a clean
 * persist (flush/close) and reloaded at open.  Crash replay of allocations made
 * since the last checkpoint is out of scope (media-log-and-recovery).
 *
 * Every media access goes through the provider vtable.  The allocator builds
 * the struct evpl_iovec for its own metadata blocks over its own buffers (data
 * + length only) and never calls a libevpl function, so this library links no
 * evpl.  An iovec passed to an async op lives in that op's heap context until
 * its completion fires.
 */

#include <limits.h>
#include <string.h>
#include <sys/random.h>

#include "evpl/evpl.h"   /* struct evpl_iovec (type only; not linked) */

#include "facade_internal.h"
#include "facade_extent.h"
#include "media_provider.h"

/* ------------------------------------------------------------------ */
/* free-space tree                                                    */
/* ------------------------------------------------------------------ */

static void
facade_free_node_destroy(
    struct rb_node *node,
    void           *private_data)
{
    (void) private_data;
    free(container_of(node, struct facade_free_ext, node));
} /* facade_free_node_destroy */

/* In-order successor in the free tree.
 *
 * This open-codes the traversal of common/rbtree.h's rb_tree_next_element
 * instead of calling it, with one difference: the parent-chain walk guards the
 * parent pointer for NULL (p && ...).  In a valid tree a node's parent is the
 * nil sentinel, never NULL, so the guard never changes behaviour -- but it lets
 * scan-build analyse the best-fit and serialize loops fully, instead of
 * tripping a false null-dereference inside the shared inline (which this unit
 * must not edit).  No analyzer suppression is needed. */
static struct facade_free_ext *
facade_free_next(
    struct facade_alloc    *a,
    struct facade_free_ext *e)
{
    const struct rb_node *nil = &a->free_by_start.nil;
    struct rb_node       *n   = &e->node;
    struct rb_node       *p;

    if (n->right != nil) {
        n = n->right;
        while (n->left != nil) {
            n = n->left;
        }
        return container_of(n, struct facade_free_ext, node);
    }
    p = n->parent;
    while (p && p != nil && n == p->right) {
        n = p;
        p = p->parent;
    }
    return (!p || p == nil) ? NULL : container_of(p, struct facade_free_ext, node);
} /* facade_free_next */

/* Coalescing insert of [start, start+nblocks) into the free tree. */
static void
facade_free_insert(
    struct facade_alloc *a,
    uint64_t             start,
    uint64_t             nblocks)
{
    struct facade_free_ext *pv = NULL;
    struct facade_free_ext *nx = NULL;
    struct facade_free_ext *e;

    /* An extent starting exactly where this one ends, and the extent at or
     * before this start. */
    rb_tree_query_exact(&a->free_by_start, start + nblocks, start, nx);
    rb_tree_query_floor(&a->free_by_start, start, start, pv);

    FACADE_CHECK(pv == NULL || pv->start + pv->nblocks <= start,
                 "double-free or overlap");

    if (pv && pv->start + pv->nblocks == start) {
        /* Merge into the predecessor; its start key is unchanged. */
        pv->nblocks += nblocks;
        if (nx) {
            pv->nblocks += nx->nblocks;
            rb_tree_remove(&a->free_by_start, &nx->node);
            free(nx);
        }
    } else if (nx) {
        /* Extend the successor downward: its start key changes, so reinsert. */
        rb_tree_remove(&a->free_by_start, &nx->node);
        nx->start    = start;
        nx->nblocks += nblocks;
        rb_tree_insert(&a->free_by_start, start, nx);
    } else {
        e = malloc(sizeof(*e));
        FACADE_CHECK(e != NULL, "oom free node");
        e->start   = start;
        e->nblocks = nblocks;
        rb_tree_insert(&a->free_by_start, start, e);
    }
} /* facade_free_insert */

SYMBOL_EXPORT enum facade_status
facade_alloc_blocks(
    struct facade_alloc   *a,
    uint32_t               nblocks,
    chimera_media_block_t *r_start,
    uint64_t              *r_epoch)
{
    struct facade_free_ext *e;
    struct facade_free_ext *best = NULL;
    uint64_t start;

    if (nblocks == 0) {
        return FACADE_E_INVAL;
    }
    if (a->free_blocks < nblocks) {
        return FACADE_E_NO_SPACE;
    }

    /* Best-fit: smallest free run that fits.  A single pass; an exact match
     * ends it early. */
    rb_tree_first(&a->free_by_start, e);
    while (e) {
        if (e->nblocks >= nblocks && (!best || e->nblocks < best->nblocks)) {
            best = e;
            if (best->nblocks == nblocks) {
                break;
            }
        }
        e = facade_free_next(a, e);
    }

    if (!best) {
        /* Enough free blocks in total, but no single contiguous run. */
        return FACADE_E_NO_SPACE;
    }

    start = best->start + best->nblocks - nblocks;   /* cut from the tail */
    if (best->nblocks == nblocks) {
        rb_tree_remove(&a->free_by_start, &best->node);
        free(best);
    } else {
        best->nblocks -= nblocks;
    }

    a->free_blocks -= nblocks;
    if (r_start) {
        *r_start = start;
    }
    if (r_epoch) {
        *r_epoch = a->cur_epoch;
    }
    return FACADE_OK;
} /* facade_alloc_blocks */

SYMBOL_EXPORT void
facade_alloc_free(
    struct facade_alloc  *a,
    chimera_media_block_t start,
    uint32_t              nblocks)
{
    FACADE_CHECK(nblocks > 0, "free of zero blocks");
    FACADE_CHECK(start >= a->data_first && start + nblocks <= a->data_last,
                 "free outside the data region");
    facade_free_insert(a, start, nblocks);
    a->free_blocks += nblocks;
} /* facade_alloc_free */

SYMBOL_EXPORT uint64_t
facade_alloc_free_blocks(const struct facade_alloc *a)
{
    return a->free_blocks;
} /* facade_alloc_free_blocks */

SYMBOL_EXPORT uint64_t
facade_alloc_total_blocks(const struct facade_alloc *a)
{
    return a->data_last - a->data_first;
} /* facade_alloc_total_blocks */

SYMBOL_EXPORT uint64_t
facade_alloc_durable_epoch(const struct facade_alloc *a)
{
    return a->durable_epoch;
} /* facade_alloc_durable_epoch */

/* ------------------------------------------------------------------ */
/* construction / teardown                                            */
/* ------------------------------------------------------------------ */

static uint64_t
facade_random_u64(void)
{
    uint64_t v;
    ssize_t  n = getrandom(&v, sizeof(v), 0);

    FACADE_CHECK(n == (ssize_t) sizeof(v), "getrandom failed");
    return v;
} /* facade_random_u64 */

static struct facade_alloc *
facade_alloc_new(
    const struct chimera_media_provider *provider,
    struct chimera_media_space          *space)
{
    struct facade_alloc *a = calloc(1, sizeof(*a));

    if (!a) {
        return NULL;
    }
    a->provider  = provider;
    a->space     = space;
    a->super_buf = calloc(1, FACADE_SUPER_SIZE);
    if (!a->super_buf) {
        free(a);
        return NULL;
    }
    rb_tree_init(&a->free_by_start);
    a->cur_epoch     = 1;
    a->durable_epoch = 0;
    return a;
} /* facade_alloc_new */

static void
facade_alloc_destroy(struct facade_alloc *a)
{
    struct facade_waiter *w = a->waiters;

    while (w) {
        struct facade_waiter *nx = w->next;
        free(w);
        w = nx;
    }
    rb_tree_destroy(&a->free_by_start, facade_free_node_destroy, NULL);
    free(a->super_buf);
    free(a);
} /* facade_alloc_destroy */

struct facade_persist_ctx {
    struct facade_alloc       *alloc;
    struct evpl_iovec          iov;
    void                      *ckpt_buf;
    uint64_t                   ckpt_total;    /* checkpoint blocks to write */
    uint64_t                   ckpt_written;
    uint64_t                   epoch;         /* the epoch this persist makes durable */
    int                        do_release;    /* close: destroy the allocator after */
    facade_cb_t                cb;            /* format / close status (nullable) */
    facade_epoch_cb_t          ecb;           /* flush epoch (nullable) */
    void                      *priv;
    struct facade_persist_ctx *next;          /* FIFO link while queued */
};

static void facade_persist_run(
    struct facade_alloc       *a,
    struct facade_persist_ctx *c);

/* Tear the allocator down: fail any queued persists, resume parked draws (they
 * fail cleanly because shutting_down is set), then free.  Called only when no
 * block-0 write is in flight. */
static void
facade_alloc_teardown(struct facade_alloc *a)
{
    struct facade_persist_ctx *c;

    a->shutting_down = 1;

    while (a->persist_head) {
        c               = a->persist_head;
        a->persist_head = c->next;
        if (c->cb) {
            c->cb(FACADE_E_SHUTDOWN, c->priv);
        }
        if (c->ecb) {
            c->ecb(FACADE_E_SHUTDOWN, a->durable_epoch, c->priv);
        }
        free(c->ckpt_buf);
        free(c);
    }
    a->persist_tail = NULL;

    facade_objid_drain_waiters(a);
    facade_alloc_destroy(a);
} /* facade_alloc_teardown */

/* The in-flight block-0 write has completed: release the slot and start the
 * next waiting op (a queued persist first, then a coalesced extension), or tear
 * the allocator down if a close/abandon arrived while the write was in flight. */
void
facade_mw_complete(struct facade_alloc *a)
{
    struct facade_persist_ctx *c;

    a->mw_busy = 0;

    if (a->destroy_after) {
        facade_alloc_teardown(a);
        return;
    }
    if (a->persist_head) {
        c               = a->persist_head;
        a->persist_head = c->next;
        if (!a->persist_head) {
            a->persist_tail = NULL;
        }
        facade_persist_run(a, c);
        return;
    }
    if (a->ext_pending) {
        a->ext_pending = 0;
        facade_objid_start_extension(a);
    }
} /* facade_mw_complete */

/* ------------------------------------------------------------------ */
/* free-set serialization                                             */
/* ------------------------------------------------------------------ */

/* Serialize the free tree into a freshly-allocated block-aligned buffer.
 * Returns the number of blocks to write, sets *r_buf (NULL if none) and
 * *r_count (extent count). */
static uint64_t
facade_serialize_free(
    struct facade_alloc *a,
    void               **r_buf,
    uint64_t            *r_count)
{
    struct facade_free_ext *e;
    struct facade_ckpt_ext *rec;
    uint64_t                count = 0;
    uint64_t                bytes, blocks;
    void                   *buf;

    rb_tree_first(&a->free_by_start, e);
    while (e) {
        count++;
        e = facade_free_next(a, e);
    }

    bytes  = count * sizeof(struct facade_ckpt_ext);
    blocks = (bytes + CHIMERA_MEDIA_BLOCK_SIZE - 1) >> CHIMERA_MEDIA_BLOCK_SHIFT;
    FACADE_CHECK(blocks <= a->ckpt_nblocks, "checkpoint region overflow");

    buf = NULL;
    if (blocks) {
        buf = calloc(blocks, CHIMERA_MEDIA_BLOCK_SIZE);
        FACADE_CHECK(buf != NULL, "oom checkpoint buffer");
        rec = buf;
        rb_tree_first(&a->free_by_start, e);
        while (e) {
            rec->start   = e->start;
            rec->nblocks = e->nblocks;
            rec++;
            e = facade_free_next(a, e);
        }
    }

    *r_buf   = buf;
    *r_count = count;
    return blocks;
} /* facade_serialize_free */

/* ------------------------------------------------------------------ */
/* persist state machine (format / flush / close)                     */
/* ------------------------------------------------------------------ */

static void facade_persist_write_ckpt(
    struct facade_persist_ctx *c);
static void facade_persist_ckpt_done(
    enum chimera_media_error st,
    void                    *priv);
static void facade_persist_write_super(
    struct facade_persist_ctx *c);
static void facade_persist_super_done(
    enum chimera_media_error st,
    void                    *priv);
static void facade_persist_flush_done(
    enum chimera_media_error st,
    void                    *priv);

static void
facade_persist_finish(
    struct facade_persist_ctx *c,
    enum facade_status         status)
{
    struct facade_alloc *a       = c->alloc;
    facade_cb_t          cb      = c->cb;
    facade_epoch_cb_t    ecb     = c->ecb;
    void                *priv    = c->priv;
    int                  release = c->do_release;
    uint64_t             de;

    if (status == FACADE_OK) {
        if (c->epoch > a->durable_epoch) {
            a->durable_epoch = c->epoch;
        }
    }
    de = a->durable_epoch;

    free(c->ckpt_buf);
    free(c);

    /* Report the result, then hand the metadata slot back.  A close/format
     * defers the actual free to mw_complete() via destroy_after, so no
     * in-flight write ever lands on freed memory. */
    if (cb) {
        cb(status, priv);
    }
    if (ecb) {
        ecb(status, de, priv);
    }
    if (release) {
        a->destroy_after = 1;
    }
    facade_mw_complete(a);
} /* facade_persist_finish */

/* Run a persist now (the metadata slot is ours): snapshot the free set, claim
 * an epoch, and drive the write(checkpoint) -> write(super, fua) -> flush
 * chain.  Takes the slot via mw_busy. */
static void
facade_persist_run(
    struct facade_alloc       *a,
    struct facade_persist_ctx *c)
{
    uint64_t count;

    a->mw_busy = 1;

    c->ckpt_total = facade_serialize_free(a, &c->ckpt_buf, &count);
    a->ckpt_count = count;
    a->clean      = 1;

    /* Allocations issued before now are the ones this persist makes durable;
     * later allocations get the next epoch and are not in the snapshot. */
    c->epoch     = a->cur_epoch;
    a->cur_epoch = c->epoch + 1;

    facade_persist_write_ckpt(c);
} /* facade_persist_run */

/* Submit a persist: run it if the metadata slot is free, else queue it behind
 * the in-flight write (FIFO). */
static void
facade_persist_submit(
    struct facade_alloc *a,
    int                  do_release,
    facade_cb_t          cb,
    facade_epoch_cb_t    ecb,
    void                *priv)
{
    struct facade_persist_ctx *c = calloc(1, sizeof(*c));

    FACADE_CHECK(c != NULL, "oom persist ctx");
    FACADE_CHECK(!a->shutting_down && !a->destroy_after,
                 "persist submitted after close/abandon");
    c->alloc      = a;
    c->do_release = do_release;
    c->cb         = cb;
    c->ecb        = ecb;
    c->priv       = priv;
    c->next       = NULL;

    if (a->mw_busy) {
        if (a->persist_tail) {
            a->persist_tail->next = c;
        } else {
            a->persist_head = c;
        }
        a->persist_tail = c;
        return;
    }
    facade_persist_run(a, c);
} /* facade_persist_submit */

static void
facade_persist_write_ckpt(struct facade_persist_ctx *c)
{
    struct facade_alloc *a = c->alloc;
    uint64_t             remain, n;

    if (c->ckpt_written >= c->ckpt_total) {
        facade_persist_write_super(c);
        return;
    }
    remain = c->ckpt_total - c->ckpt_written;
    n      = remain < a->max_io_blocks ? remain : a->max_io_blocks;

    memset(&c->iov, 0, sizeof(c->iov));
    c->iov.data   = (char *) c->ckpt_buf + (c->ckpt_written << CHIMERA_MEDIA_BLOCK_SHIFT);
    c->iov.length = (unsigned int) (n << CHIMERA_MEDIA_BLOCK_SHIFT);

    a->provider->write(a->space, &c->iov, 1,
                       a->ckpt_block + c->ckpt_written, (uint32_t) n,
                       0 /* durable via the later flush */,
                       facade_persist_ckpt_done, c);
} /* facade_persist_write_ckpt */

static void
facade_persist_ckpt_done(
    enum chimera_media_error st,
    void                    *priv)
{
    struct facade_persist_ctx *c = priv;
    uint64_t                   remain, n;

    if (st != CHIMERA_MEDIA_OK) {
        facade_persist_finish(c, FACADE_E_IO);
        return;
    }
    remain           = c->ckpt_total - c->ckpt_written;
    n                = remain < c->alloc->max_io_blocks ? remain : c->alloc->max_io_blocks;
    c->ckpt_written += n;
    facade_persist_write_ckpt(c);
} /* facade_persist_ckpt_done */

static void
facade_persist_write_super(struct facade_persist_ctx *c)
{
    struct facade_alloc *a = c->alloc;

    facade_super_pack(a, a->super_buf);

    memset(&c->iov, 0, sizeof(c->iov));
    c->iov.data   = a->super_buf;
    c->iov.length = FACADE_SUPER_SIZE;

    a->provider->write(a->space, &c->iov, 1, FACADE_SUPER_BLOCK, 1,
                       CHIMERA_MEDIA_WRITE_FUA, facade_persist_super_done, c);
} /* facade_persist_write_super */

static void
facade_persist_super_done(
    enum chimera_media_error st,
    void                    *priv)
{
    struct facade_persist_ctx *c = priv;

    if (st != CHIMERA_MEDIA_OK) {
        facade_persist_finish(c, FACADE_E_IO);
        return;
    }
    c->alloc->provider->flush(c->alloc->space, facade_persist_flush_done, c);
} /* facade_persist_super_done */

static void
facade_persist_flush_done(
    enum chimera_media_error st,
    void                    *priv)
{
    struct facade_persist_ctx *c = priv;

    facade_persist_finish(c, st == CHIMERA_MEDIA_OK ? FACADE_OK : FACADE_E_IO);
} /* facade_persist_flush_done */

/* ------------------------------------------------------------------ */
/* format                                                             */
/* ------------------------------------------------------------------ */

/* Compute the metadata/data carve from the space geometry. */
static void
facade_compute_geometry(struct facade_alloc *a)
{
    struct chimera_media_space_info info;
    uint64_t                        ckpt_bytes, ckpt_nblocks;

    a->provider->info(a->space, &info);
    a->nblocks       = info.nblocks;
    a->max_io_blocks = info.max_io_blocks ? info.max_io_blocks : 1u;
    /* Clamp so a chunk length (n << block-shift) fits the iovec's 32-bit
     * length field; only a provider advertising an implausibly huge
     * max_io could hit this. */
    if (a->max_io_blocks > (UINT_MAX >> CHIMERA_MEDIA_BLOCK_SHIFT)) {
        a->max_io_blocks = UINT_MAX >> CHIMERA_MEDIA_BLOCK_SHIFT;
    }

    /* Checkpoint sized for the worst-case fragmentation of the whole space
     * (<= nblocks/2 + 1 free extents), so a clean persist never overflows it. */
    ckpt_bytes   = (a->nblocks / 2 + 1) * sizeof(struct facade_ckpt_ext);
    ckpt_nblocks = (ckpt_bytes + CHIMERA_MEDIA_BLOCK_SIZE - 1) >> CHIMERA_MEDIA_BLOCK_SHIFT;
    if (ckpt_nblocks == 0) {
        ckpt_nblocks = 1;
    }

    a->ckpt_block   = FACADE_SUPER_BLOCK + 1;
    a->ckpt_nblocks = ckpt_nblocks;
    a->data_first   = a->ckpt_block + a->ckpt_nblocks;
    a->data_last    = a->nblocks;
} /* facade_compute_geometry */

SYMBOL_EXPORT void
facade_alloc_format(
    const struct chimera_media_provider *provider,
    struct chimera_media_space          *space,
    const struct facade_geometry        *geom,
    facade_cb_t                          cb,
    void                                *private_data)
{
    struct facade_alloc *a;

    (void) geom;

    a = facade_alloc_new(provider, space);
    if (!a) {
        cb(FACADE_E_IO, private_data);
        return;
    }

    facade_compute_geometry(a);
    FACADE_CHECK(a->data_first < a->data_last, "media space too small to format");

    /* Whole data region free. */
    facade_free_insert(a, a->data_first, a->data_last - a->data_first);
    a->free_blocks = a->data_last - a->data_first;

    /* Identity: random instance nonce, floor seeded a reserve ahead of 0. */
    a->id_nonce = facade_random_u64();
    a->id_next  = 0;
    a->id_floor = FACADE_ID_RESERVE;

    /* Format hands back no handle: persist, then release the scratch
     * allocator.  The caller gets a usable handle from facade_alloc_open. */
    facade_persist_submit(a, 1 /* release */, cb, NULL, private_data);
} /* facade_alloc_format */

/* ------------------------------------------------------------------ */
/* open                                                               */
/* ------------------------------------------------------------------ */

struct facade_open_ctx {
    struct facade_alloc *alloc;
    struct evpl_iovec    iov;
    facade_open_cb_t     cb;
    void                *priv;
    void                *ckpt_buf;
    uint64_t             ckpt_total;   /* checkpoint blocks to read */
    uint64_t             ckpt_read;
};

static void facade_open_read_ckpt(
    struct facade_open_ctx *c);
static void facade_open_ckpt_done(
    enum chimera_media_error st,
    void                    *priv);
static void facade_open_finish(
    struct facade_open_ctx *c);

static void
facade_open_fail(
    struct facade_open_ctx *c,
    enum facade_status      status)
{
    facade_open_cb_t cb   = c->cb;
    void            *priv = c->priv;

    facade_alloc_destroy(c->alloc);
    free(c->ckpt_buf);
    free(c);
    cb(status, NULL, priv);
} /* facade_open_fail */

static void
facade_open_super_done(
    enum chimera_media_error st,
    void                    *priv)
{
    struct facade_open_ctx *c = priv;
    struct facade_alloc    *a = c->alloc;
    struct facade_super     s;
    uint32_t                stored, calc;

    if (st != CHIMERA_MEDIA_OK) {
        facade_open_fail(c, FACADE_E_IO);
        return;
    }

    memcpy(&s, a->super_buf, sizeof(s));
    stored                                        = s.crc32;
    ((struct facade_super *) a->super_buf)->crc32 = 0;
    calc                                          = facade_crc32(a->super_buf, FACADE_SUPER_SIZE);

    if (s.magic != FACADE_SUPER_MAGIC || s.version != FACADE_FORMAT_VERSION ||
        s.block_size != CHIMERA_MEDIA_BLOCK_SIZE || calc != stored) {
        facade_open_fail(c, FACADE_E_IO);
        return;
    }

    /* Geometry sanity (F3): reject a CRC-valid but self-inconsistent superblock
     * before trusting any of its offsets.  a->nblocks was set from the live
     * space geometry at open; the superblock may not claim more. */
    if (s.ckpt_block != FACADE_SUPER_BLOCK + 1 || s.ckpt_nblocks == 0 ||
        s.data_first != s.ckpt_block + s.ckpt_nblocks ||
        s.data_first >= s.data_last || s.data_last > a->nblocks) {
        facade_open_fail(c, FACADE_E_IO);
        return;
    }

    a->nblocks      = s.nblocks;
    a->data_first   = s.data_first;
    a->data_last    = s.data_last;
    a->ckpt_block   = s.ckpt_block;
    a->ckpt_nblocks = s.ckpt_nblocks;
    a->id_nonce     = s.id_nonce;
    a->id_floor     = s.id_floor;
    a->id_next      = s.id_floor;   /* resume at the durable floor (no reuse) */
    a->ckpt_count   = s.ckpt_count;
    a->clean        = s.clean;

    c->ckpt_total = (s.ckpt_count * sizeof(struct facade_ckpt_ext)
                     + CHIMERA_MEDIA_BLOCK_SIZE - 1) >> CHIMERA_MEDIA_BLOCK_SHIFT;

    /* The checkpoint must fit the region the superblock reserved for it (F3). */
    if (c->ckpt_total > a->ckpt_nblocks) {
        facade_open_fail(c, FACADE_E_IO);
        return;
    }

    if (c->ckpt_total == 0) {
        facade_open_finish(c);
        return;
    }

    c->ckpt_buf = calloc(c->ckpt_total, CHIMERA_MEDIA_BLOCK_SIZE);
    FACADE_CHECK(c->ckpt_buf != NULL, "oom checkpoint read buffer");
    facade_open_read_ckpt(c);
} /* facade_open_super_done */

static void
facade_open_read_ckpt(struct facade_open_ctx *c)
{
    struct facade_alloc *a = c->alloc;
    uint64_t             remain, n;

    if (c->ckpt_read >= c->ckpt_total) {
        facade_open_finish(c);
        return;
    }
    remain = c->ckpt_total - c->ckpt_read;
    n      = remain < a->max_io_blocks ? remain : a->max_io_blocks;

    memset(&c->iov, 0, sizeof(c->iov));
    c->iov.data   = (char *) c->ckpt_buf + (c->ckpt_read << CHIMERA_MEDIA_BLOCK_SHIFT);
    c->iov.length = (unsigned int) (n << CHIMERA_MEDIA_BLOCK_SHIFT);

    a->provider->read(a->space, &c->iov, 1,
                      a->ckpt_block + c->ckpt_read, (uint32_t) n,
                      facade_open_ckpt_done, c);
} /* facade_open_read_ckpt */

static void
facade_open_ckpt_done(
    enum chimera_media_error st,
    void                    *priv)
{
    struct facade_open_ctx *c = priv;
    uint64_t                remain, n;

    if (st != CHIMERA_MEDIA_OK) {
        facade_open_fail(c, FACADE_E_IO);
        return;
    }
    remain        = c->ckpt_total - c->ckpt_read;
    n             = remain < c->alloc->max_io_blocks ? remain : c->alloc->max_io_blocks;
    c->ckpt_read += n;
    facade_open_read_ckpt(c);
} /* facade_open_ckpt_done */

static void
facade_open_finish(struct facade_open_ctx *c)
{
    struct facade_alloc          *a    = c->alloc;
    const struct facade_ckpt_ext *rec  = c->ckpt_buf;
    facade_open_cb_t              cb   = c->cb;
    void                         *priv = c->priv;
    uint64_t                      i, prev_end = a->data_first;

    /*
     * Validate each loaded free extent before trusting it (F3): in range, and
     * strictly ascending and non-overlapping (serialize writes them that way).
     * A CRC-valid but crafted/corrupt checkpoint is rejected cleanly here
     * rather than tripping the double-free abort inside facade_free_insert.
     */
    a->free_blocks = 0;
    for (i = 0; i < a->ckpt_count; i++) {
        if (rec[i].nblocks == 0 ||
            rec[i].start < prev_end ||
            rec[i].start >= a->data_last ||
            rec[i].nblocks > a->data_last - rec[i].start) {
            facade_open_fail(c, FACADE_E_IO);
            return;
        }
        prev_end = rec[i].start + rec[i].nblocks;
    }
    for (i = 0; i < a->ckpt_count; i++) {
        facade_free_insert(a, rec[i].start, rec[i].nblocks);
        a->free_blocks += rec[i].nblocks;
    }

    free(c->ckpt_buf);
    free(c);
    cb(FACADE_OK, a, priv);
} /* facade_open_finish */

SYMBOL_EXPORT void
facade_alloc_open(
    const struct chimera_media_provider *provider,
    struct chimera_media_space          *space,
    facade_open_cb_t                     cb,
    void                                *private_data)
{
    struct facade_alloc            *a;
    struct facade_open_ctx         *c;
    struct chimera_media_space_info info;

    a = facade_alloc_new(provider, space);
    if (!a) {
        cb(FACADE_E_IO, NULL, private_data);
        return;
    }
    /* max_io_blocks chunks the checkpoint read; nblocks is the live space size,
     * the upper bound the superblock's geometry is validated against (F3).  The
     * rest of the geometry comes from the superblock. */
    provider->info(space, &info);
    a->max_io_blocks = info.max_io_blocks ? info.max_io_blocks : 1u;
    /* Clamp so a chunk length (n << block-shift) fits the iovec's 32-bit
     * length field; only a provider advertising an implausibly huge
     * max_io could hit this. */
    if (a->max_io_blocks > (UINT_MAX >> CHIMERA_MEDIA_BLOCK_SHIFT)) {
        a->max_io_blocks = UINT_MAX >> CHIMERA_MEDIA_BLOCK_SHIFT;
    }
    a->nblocks = info.nblocks;

    c = calloc(1, sizeof(*c));
    FACADE_CHECK(c != NULL, "oom open ctx");
    c->alloc = a;
    c->cb    = cb;
    c->priv  = private_data;

    memset(&c->iov, 0, sizeof(c->iov));
    c->iov.data   = a->super_buf;
    c->iov.length = FACADE_SUPER_SIZE;
    provider->read(space, &c->iov, 1, FACADE_SUPER_BLOCK, 1,
                   facade_open_super_done, c);
} /* facade_alloc_open */

/* ------------------------------------------------------------------ */
/* flush / close                                                      */
/* ------------------------------------------------------------------ */

SYMBOL_EXPORT void
facade_alloc_flush(
    struct facade_alloc *a,
    facade_epoch_cb_t    cb,
    void                *private_data)
{
    facade_persist_submit(a, 0 /* keep */, NULL, cb, private_data);
} /* facade_alloc_flush */

SYMBOL_EXPORT void
facade_alloc_close(
    struct facade_alloc *a,
    facade_cb_t          cb,
    void                *private_data)
{
    facade_persist_submit(a, 1 /* release */, cb, NULL, private_data);
} /* facade_alloc_close */

SYMBOL_EXPORT void
facade_alloc_abandon(struct facade_alloc *a)
{
    /* Drop without persisting.  If a block-0 write is in flight, defer the free
     * to mw_complete() so the deferred completion never lands on freed memory
     * (F2); otherwise tear down now.  Either way media keeps only what prior fua
     * writes/flushes made durable -- in particular the id floor. */
    if (a->mw_busy) {
        a->destroy_after = 1;
        return;
    }
    facade_alloc_teardown(a);
} /* facade_alloc_abandon */
