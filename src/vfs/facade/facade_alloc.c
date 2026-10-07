// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Backend-facade block allocator, object table, and A/B checkpoint.  See
 * facade_alloc.h for the interface and facade_super.h for the on-media format.
 *
 * media-log-and-recovery extends block-allocator-and-identity: the checkpoint
 * now holds the free set AND a durable object table, it ping-pongs between two
 * slots (A/B) for crash-atomic compaction, and the superblock carries the log
 * geometry, the active-slot selector, the clean/dirty mark and the persisted
 * power-loss generation.  The durability log (facade_log.c) and crash recovery
 * (facade_recover.c) layer on top.
 *
 * Every media access goes through the provider vtable (facade_media_* helpers);
 * the core links no libevpl (it references struct evpl_iovec only as the vtable
 * buffer type).  An allocator is bound to the media space's evpl thread; each
 * async op is a self-contained state machine the caller awaits, so at most one
 * op is in flight (plus the deliberate teardown-race path, handled by deferring
 * the free until inflight drops to zero).
 */

#include <limits.h>
#include <string.h>
#include <sys/random.h>

#include "evpl/evpl.h"   /* struct evpl_iovec (type only; not linked) */

#include "facade_internal.h"
#include "facade_log.h"
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

static void
facade_free_insert(
    struct facade_alloc *a,
    uint64_t             start,
    uint64_t             nblocks)
{
    struct facade_free_ext *pv = NULL;
    struct facade_free_ext *nx = NULL;
    struct facade_free_ext *e;

    rb_tree_query_exact(&a->free_by_start, start + nblocks, start, nx);
    rb_tree_query_floor(&a->free_by_start, start, start, pv);

    FACADE_CHECK(pv == NULL || pv->start + pv->nblocks <= start,
                 "double-free or overlap");

    if (pv && pv->start + pv->nblocks == start) {
        pv->nblocks += nblocks;
        if (nx) {
            pv->nblocks += nx->nblocks;
            rb_tree_remove(&a->free_by_start, &nx->node);
            free(nx);
        }
    } else if (nx) {
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

int
facade_free_remove(
    struct facade_alloc  *a,
    chimera_media_block_t start,
    uint64_t              nblocks)
{
    struct facade_free_ext *e;
    uint64_t                estart, eend;

    rb_tree_query_floor(&a->free_by_start, start, start, e);
    if (!e || e->start > start || e->start + e->nblocks < start + nblocks) {
        return -1;   /* range is not wholly free: inconsistent log record */
    }
    estart = e->start;
    eend   = e->start + e->nblocks;
    rb_tree_remove(&a->free_by_start, &e->node);
    free(e);
    if (estart < start) {
        facade_free_insert(a, estart, start - estart);
    }
    if (start + nblocks < eend) {
        facade_free_insert(a, start + nblocks, eend - (start + nblocks));
    }
    a->free_blocks -= nblocks;
    return 0;
} /* facade_free_remove */

void
facade_free_add(
    struct facade_alloc  *a,
    chimera_media_block_t start,
    uint64_t              nblocks)
{
    facade_free_insert(a, start, nblocks);
    a->free_blocks += nblocks;
} /* facade_free_add */

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

SYMBOL_EXPORT uint64_t
facade_alloc_power_loss_generation(const struct facade_alloc *a)
{
    return a->power_loss_generation;
} /* facade_alloc_power_loss_generation */

/* ------------------------------------------------------------------ */
/* object table                                                       */
/* ------------------------------------------------------------------ */

SYMBOL_EXPORT struct facade_obj *
facade_obj_find(
    struct facade_alloc *a,
    const uint8_t        id[16])
{
    uint32_t i;

    for (i = 0; i < FACADE_OBJ_BUDGET; i++) {
        if (a->objs[i].used && memcmp(a->objs[i].id, id, 16) == 0) {
            return &a->objs[i];
        }
    }
    return NULL;
} /* facade_obj_find */

/* Find a free slot and initialise an empty, unused entry; NULL if the budget is
 * exhausted.  The caller fills the scalars and the map. */
static struct facade_obj *
facade_obj_alloc_slot(struct facade_alloc *a)
{
    uint32_t i;

    for (i = 0; i < FACADE_OBJ_BUDGET; i++) {
        if (!a->objs[i].used) {
            struct facade_obj *o = &a->objs[i];

            memset(o, 0, sizeof(*o));
            o->used = 1;
            o->map  = facade_extent_map_create();
            a->nobjs++;
            return o;
        }
    }
    return NULL;
} /* facade_obj_alloc_slot */

SYMBOL_EXPORT struct facade_obj *
facade_obj_create_entry(
    struct facade_alloc            *a,
    const uint8_t                   id[16],
    const struct chimera_sc_stamps *stamps)
{
    struct facade_obj *o;

    FACADE_CHECK(facade_obj_find(a, id) == NULL, "object id already present");
    o = facade_obj_alloc_slot(a);
    if (!o) {
        return NULL;
    }
    memcpy(o->id, id, 16);
    o->state = CHIMERA_SC_OBJ_OPEN;
    o->size  = 0;
    o->flags = 0;
    if (stamps) {
        o->stamps = *stamps;
    }
    return o;
} /* facade_obj_create_entry */

SYMBOL_EXPORT void
facade_obj_serialize_hdr(
    const struct facade_obj *o,
    struct facade_obj_rec   *hdr)
{
    memset(hdr, 0, sizeof(*hdr));
    memcpy(hdr->obj_id, o->id, 16);
    hdr->size               = o->size;
    hdr->change             = o->change;
    hdr->policy_set         = o->stamps.policy_set;
    hdr->qd_owner           = o->stamps.quota_domains.owner;
    hdr->qd_group           = o->stamps.quota_domains.group;
    hdr->qd_project         = o->stamps.quota_domains.project;
    hdr->owner_principal    = o->stamps.owner_principal;
    hdr->retention_until_ns = o->retention_until_ns;
    hdr->flags              = o->flags;
    hdr->state              = o->state;
    hdr->qd_valid           = o->stamps.quota_domains.valid;
    hdr->origin             = o->stamps.origin;
    hdr->retention_mode     = o->retention_mode;
    hdr->nextents           = facade_extent_map_count(o->map);
} /* facade_obj_serialize_hdr */

SYMBOL_EXPORT struct facade_obj *
facade_obj_put_rec(
    struct facade_alloc             *a,
    const struct facade_obj_rec     *hdr,
    const struct facade_ckpt_objext *exts)
{
    struct facade_obj *o = facade_obj_find(a, hdr->obj_id);
    uint32_t           k;

    if (hdr->nextents > FACADE_OBJ_MAX_EXT) {
        return NULL;
    }
    if (o) {
        /* Replace: drop the old map, rebuild from the record. */
        facade_extent_map_destroy(o->map);
        o->map = facade_extent_map_create();
    } else {
        o = facade_obj_alloc_slot(a);
        if (!o) {
            return NULL;
        }
        memcpy(o->id, hdr->obj_id, 16);
    }
    o->flags                        = hdr->flags;
    o->state                        = hdr->state;
    o->size                         = hdr->size;
    o->change                       = hdr->change;
    o->stamps.policy_set            = hdr->policy_set;
    o->stamps.quota_domains.owner   = hdr->qd_owner;
    o->stamps.quota_domains.group   = hdr->qd_group;
    o->stamps.quota_domains.project = hdr->qd_project;
    o->stamps.quota_domains.valid   = hdr->qd_valid;
    o->stamps.owner_principal       = hdr->owner_principal;
    o->stamps.origin                = hdr->origin;
    o->retention_until_ns           = hdr->retention_until_ns;
    o->retention_mode               = hdr->retention_mode;
    for (k = 0; k < hdr->nextents; k++) {
        if (exts[k].nblocks == 0 ||
            (exts[k].kind != CHIMERA_SC_SEG_DATA &&
             exts[k].kind != CHIMERA_SC_SEG_UNWRITTEN) ||
            exts[k].nblocks > UINT32_MAX) {
            return NULL;   /* self-inconsistent record */
        }
        facade_extent_map_insert(o->map, exts[k].logical_block,
                                 (uint32_t) exts[k].nblocks, exts[k].phys_block,
                                 exts[k].kind, exts[k].owned ? 1u : 0u, NULL, NULL);
    }
    return o;
} /* facade_obj_put_rec */

SYMBOL_EXPORT void
facade_obj_remove(
    struct facade_alloc *a,
    struct facade_obj   *o)
{
    FACADE_CHECK(o->used, "remove of an unused object slot");
    facade_extent_map_destroy(o->map);
    o->map  = NULL;
    o->used = 0;
    a->nobjs--;
} /* facade_obj_remove */

SYMBOL_EXPORT uint32_t
facade_obj_slot_count(const struct facade_alloc *a)
{
    (void) a;
    return FACADE_OBJ_BUDGET;
} /* facade_obj_slot_count */

SYMBOL_EXPORT struct facade_obj *
facade_obj_at(
    struct facade_alloc *a,
    uint32_t             slot)
{
    struct facade_obj *o = &a->objs[slot];

    return o->used ? o : NULL;
} /* facade_obj_at */

/* ------------------------------------------------------------------ */
/* durable-object-transaction FIFO (real concurrency)                 */
/* ------------------------------------------------------------------ */

SYMBOL_EXPORT void
facade_txn_acquire(
    struct facade_alloc *a,
    void (              *start )(void *arg),
    void                *arg)
{
    if (!a->txn_busy) {
        a->txn_busy = 1;
        start(arg);
        return;
    }
    {
        struct facade_txn_waiter *w = calloc(1, sizeof(*w));

        FACADE_CHECK(w != NULL, "oom txn waiter");
        w->start = start;
        w->arg   = arg;
        if (a->txn_tail) {
            a->txn_tail->next = w;
        } else {
            a->txn_head = w;
        }
        a->txn_tail = w;
    }
} /* facade_txn_acquire */

SYMBOL_EXPORT void
facade_txn_release(struct facade_alloc *a)
{
    struct facade_txn_waiter *w = a->txn_head;

    FACADE_CHECK(a->txn_busy, "txn_release without a held slot");
    if (!w) {
        a->txn_busy = 0;
        return;
    }
    a->txn_head = w->next;
    if (!a->txn_head) {
        a->txn_tail = NULL;
    }
    /* Keep txn_busy set: the slot passes directly to the next starter. */
    {
        void  (*start)(
            void *) = w->start;
        void *arg = w->arg;

        free(w);
        start(arg);
    }
} /* facade_txn_release */

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
    a->objs      = calloc(FACADE_OBJ_BUDGET, sizeof(*a->objs));
    if (!a->super_buf || !a->objs) {
        free(a->super_buf);
        free(a->objs);
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
    struct facade_waiter     *w = a->waiters;
    struct facade_txn_waiter *tw;
    uint32_t                  i;

    while (w) {
        struct facade_waiter *nx = w->next;
        free(w);
        w = nx;
    }
    /* A txn starter still queued at teardown is dropped (its caller is torn down
     * with the allocator); this only happens on an abrupt abandon with work
     * parked, which the tests do not do, but free the nodes regardless. */
    tw = a->txn_head;
    while (tw) {
        struct facade_txn_waiter *nx = tw->next;
        free(tw);
        tw = nx;
    }
    for (i = 0; i < FACADE_OBJ_BUDGET; i++) {
        if (a->objs[i].used) {
            facade_extent_map_destroy(a->objs[i].map);
        }
    }
    rb_tree_destroy(&a->free_by_start, facade_free_node_destroy, NULL);
    free(a->objs);
    free(a->super_buf);
    free(a);
} /* facade_alloc_destroy */

/* Tear the allocator down: resume parked draws (they fail cleanly because
 * shutting_down is set), then free.  Called only when no media op is in flight. */
static void
facade_alloc_teardown(struct facade_alloc *a)
{
    a->shutting_down = 1;
    facade_objid_drain_waiters(a);
    facade_alloc_destroy(a);
} /* facade_alloc_teardown */

static void
facade_maybe_teardown(struct facade_alloc *a)
{
    /* Defer while any media completion is still on the stack (an inline-completing
     * provider nests them): only the outermost completion may free the allocator. */
    if (a->cb_depth == 0 && a->destroy_after && !a->mw_busy && a->inflight == 0) {
        facade_alloc_teardown(a);
    }
} /* facade_maybe_teardown */

static void facade_mark_dirty_issue(
    struct facade_alloc *a,
    facade_cb_t          cb,
    void                *private_data);

/* The in-flight block-0 write (id-floor extension) has completed. */
void
facade_mw_complete(struct facade_alloc *a)
{
    a->mw_busy = 0;

    if (a->destroy_after) {
        facade_maybe_teardown(a);
        return;
    }
    if (a->ext_pending) {
        a->ext_pending = 0;
        facade_objid_start_extension(a);
        return;
    }
    if (a->dirty_mark_pending) {
        facade_cb_t cb = a->dirty_cb;
        void       *cp = a->dirty_priv;

        a->dirty_mark_pending = 0;
        a->dirty_cb           = NULL;
        a->dirty_priv         = NULL;
        facade_mark_dirty_issue(a, cb, cp);   /* the slot is free now */
        return;
    }
    if (a->compact_pending) {
        facade_cb_t cb = a->compact_cb;
        void       *cp = a->compact_priv;

        a->compact_pending = 0;
        a->compact_cb      = NULL;
        a->compact_priv    = NULL;
        facade_compact(a, cb, cp);   /* the slot is free now (mw_busy == 0) */
    }
} /* facade_mw_complete */

void
facade_inflight_begin(struct facade_alloc *a)
{
    a->inflight++;
} /* facade_inflight_begin */

void
facade_inflight_end(struct facade_alloc *a)
{
    FACADE_CHECK(a->inflight > 0, "inflight underflow");
    a->inflight--;
} /* facade_inflight_end */

/* ------------------------------------------------------------------ */
/* chunked media helpers                                              */
/* ------------------------------------------------------------------ */

struct facade_io {
    struct facade_alloc *a;
    char                *buf;        /* read dest / write src (we only read data+length) */
    uint64_t             lba;
    uint64_t             nblocks;
    uint64_t             done;       /* blocks completed */
    uint64_t             chunk;      /* blocks in the chunk just issued */
    int                  is_write;
    int                  fua;
    struct evpl_iovec    iov;
    facade_cb_t          cb;
    void                *priv;
};

static void facade_io_cb(
    enum chimera_media_error st,
    void                    *priv);

static void
facade_io_step(struct facade_io *io)
{
    struct facade_alloc *a      = io->a;
    uint64_t             remain = io->nblocks - io->done;
    uint64_t             n      = remain < a->max_io_blocks ? remain : a->max_io_blocks;

    io->chunk = n;
    memset(&io->iov, 0, sizeof(io->iov));
    io->iov.data   = io->buf + (io->done << CHIMERA_MEDIA_BLOCK_SHIFT);
    io->iov.length = (unsigned int) (n << CHIMERA_MEDIA_BLOCK_SHIFT);

    facade_inflight_begin(a);
    if (io->is_write) {
        a->provider->write(a->space, &io->iov, 1, io->lba + io->done,
                           (uint32_t) n, io->fua, facade_io_cb, io);
    } else {
        a->provider->read(a->space, &io->iov, 1, io->lba + io->done,
                          (uint32_t) n, facade_io_cb, io);
    }
} /* facade_io_step */

static void
facade_io_cb(
    enum chimera_media_error st,
    void                    *priv)
{
    struct facade_io    *io = priv;
    struct facade_alloc *a  = io->a;
    facade_cb_t          cb;
    void                *cbpriv;

    a->cb_depth++;
    facade_inflight_end(a);

    if (st != CHIMERA_MEDIA_OK) {
        cb     = io->cb;
        cbpriv = io->priv;
        free(io);
        cb(FACADE_E_IO, cbpriv);
    } else {
        io->done += io->chunk;
        if (io->done < io->nblocks) {
            facade_io_step(io);   /* nested completion (inline provider); depth guards it */
        } else {
            cb     = io->cb;
            cbpriv = io->priv;
            free(io);
            cb(FACADE_OK, cbpriv);
        }
    }

    a->cb_depth--;
    facade_maybe_teardown(a);
} /* facade_io_cb */

static void
facade_io_start(
    struct facade_alloc *a,
    void                *buf,
    uint64_t             lba,
    uint64_t             nblocks,
    int                  is_write,
    int                  fua,
    facade_cb_t          cb,
    void                *priv)
{
    struct facade_io *io = calloc(1, sizeof(*io));

    FACADE_CHECK(io != NULL, "oom media io ctx");
    io->a        = a;
    io->buf      = buf;
    io->lba      = lba;
    io->nblocks  = nblocks;
    io->is_write = is_write;
    io->fua      = fua;
    io->cb       = cb;
    io->priv     = priv;
    facade_io_step(io);
} /* facade_io_start */

void
facade_media_write(
    struct facade_alloc *a,
    const void          *buf,
    uint64_t             lba,
    uint64_t             nblocks,
    int                  fua,
    facade_cb_t          cb,
    void                *private_data)
{
    facade_io_start(a, (void *) (uintptr_t) buf, lba, nblocks, 1, fua, cb, private_data);
} /* facade_media_write */

SYMBOL_EXPORT void
facade_media_read(
    struct facade_alloc *a,
    void                *buf,
    uint64_t             lba,
    uint64_t             nblocks,
    facade_cb_t          cb,
    void                *private_data)
{
    facade_io_start(a, buf, lba, nblocks, 0, 0, cb, private_data);
} /* facade_media_read */

struct facade_flush_ctx {
    struct facade_alloc *a;
    facade_cb_t          cb;
    void                *priv;
};

static void
facade_flush_cb(
    enum chimera_media_error st,
    void                    *priv)
{
    struct facade_flush_ctx *c  = priv;
    struct facade_alloc     *a  = c->a;
    facade_cb_t              cb = c->cb;
    void                    *cp = c->priv;

    a->cb_depth++;
    facade_inflight_end(a);
    free(c);
    cb(st == CHIMERA_MEDIA_OK ? FACADE_OK : FACADE_E_IO, cp);
    a->cb_depth--;
    facade_maybe_teardown(a);
} /* facade_flush_cb */

void
facade_media_flush(
    struct facade_alloc *a,
    facade_cb_t          cb,
    void                *private_data)
{
    struct facade_flush_ctx *c = calloc(1, sizeof(*c));

    FACADE_CHECK(c != NULL, "oom flush ctx");
    c->a    = a;
    c->cb   = cb;
    c->priv = private_data;
    facade_inflight_begin(a);
    a->provider->flush(a->space, facade_flush_cb, c);
} /* facade_media_flush */

/* ------------------------------------------------------------------ */
/* checkpoint (de)serialization                                       */
/* ------------------------------------------------------------------ */

/* facade_extent_map_foreach callback: serialize one extent into *pp and advance
 * it (the checkpoint and the log OBJ record share this on-media objext shape). */
static void
facade_ckpt_write_objext(
    const struct facade_extent *rec,
    void                       *private_data)
{
    uint8_t                 **pp = private_data;
    struct facade_ckpt_objext xr;

    memset(&xr, 0, sizeof(xr));
    xr.logical_block = rec->logical_block;
    xr.phys_block    = rec->phys_block;
    xr.nblocks       = rec->nblocks;
    xr.kind          = rec->kind;
    xr.owned         = rec->owned;
    memcpy(*pp, &xr, sizeof(xr));
    *pp += sizeof(xr);
} /* facade_ckpt_write_objext */

SYMBOL_EXPORT void
facade_ckpt_serialize_objext(
    const struct facade_extent_map *map,
    uint8_t                       **pp)
{
    facade_extent_map_foreach(map, facade_ckpt_write_objext, pp);
} /* facade_ckpt_serialize_objext */

uint64_t
facade_ckpt_serialize(
    struct facade_alloc *a,
    uint64_t             generation,
    uint64_t             checkpoint_seq,
    void               **r_buf)
{
    struct facade_free_ext *fe;
    struct facade_ckpt_hdr *hdr;
    uint64_t                nfree = 0, bytes, blocks;
    uint32_t                i;
    uint8_t                *p;
    void                   *buf;

    rb_tree_first(&a->free_by_start, fe);
    while (fe) {
        nfree++;
        fe = facade_free_next(a, fe);
    }

    bytes = sizeof(struct facade_ckpt_hdr)
        + nfree * sizeof(struct facade_ckpt_ext);
    for (i = 0; i < FACADE_OBJ_BUDGET; i++) {
        if (a->objs[i].used) {
            bytes += sizeof(struct facade_obj_rec)
                + facade_extent_map_count(a->objs[i].map)
                * sizeof(struct facade_ckpt_objext);
        }
    }

    blocks = (bytes + CHIMERA_MEDIA_BLOCK_SIZE - 1) >> CHIMERA_MEDIA_BLOCK_SHIFT;
    if (blocks == 0) {
        blocks = 1;
    }
    FACADE_CHECK(blocks <= a->ckpt_nblocks, "checkpoint slot overflow");

    buf = calloc(blocks, CHIMERA_MEDIA_BLOCK_SIZE);
    FACADE_CHECK(buf != NULL, "oom checkpoint buffer");

    hdr                 = buf;
    hdr->magic          = FACADE_CKPT_MAGIC;
    hdr->generation     = generation;
    hdr->checkpoint_seq = checkpoint_seq;
    hdr->n_free_ext     = nfree;
    hdr->n_objs         = a->nobjs;
    hdr->content_blocks = blocks;
    hdr->reserved       = 0;
    hdr->crc32          = 0;

    p = (uint8_t *) buf + sizeof(struct facade_ckpt_hdr);

    rb_tree_first(&a->free_by_start, fe);
    while (fe) {
        struct facade_ckpt_ext rec = { fe->start, fe->nblocks };
        memcpy(p, &rec, sizeof(rec));
        p += sizeof(rec);
        fe = facade_free_next(a, fe);
    }

    for (i = 0; i < FACADE_OBJ_BUDGET; i++) {
        struct facade_obj    *o = &a->objs[i];
        struct facade_obj_rec orec;

        if (!o->used) {
            continue;
        }
        facade_obj_serialize_hdr(o, &orec);
        memcpy(p, &orec, sizeof(orec));
        p += sizeof(orec);
        /* Extents follow the header, serialized from the map in logical order. */
        facade_extent_map_foreach(o->map, facade_ckpt_write_objext, &p);
    }

    hdr->crc32 = facade_crc32(buf, (uint32_t) (blocks << CHIMERA_MEDIA_BLOCK_SHIFT));

    *r_buf = buf;
    return blocks;
} /* facade_ckpt_serialize */

/* Parse a checkpoint slot already read into buf (content_blocks * 4 KiB) whose
 * header was validated.  Rebuilds the free set and object table.  Returns 0 on
 * success, -1 on a self-inconsistent record (rejected, not aborted). */
static int
facade_ckpt_load(
    struct facade_alloc          *a,
    const void                   *buf,
    uint64_t                      content_blocks,
    const struct facade_ckpt_hdr *hdr)
{
    const uint8_t *p   = (const uint8_t *) buf + sizeof(struct facade_ckpt_hdr);
    const uint8_t *end = (const uint8_t *) buf + (content_blocks << CHIMERA_MEDIA_BLOCK_SHIFT);
    uint64_t       i;
    uint64_t       prev_end = a->data_first;

    a->free_blocks = 0;
    a->nobjs       = 0;
    memset(a->objs, 0, FACADE_OBJ_BUDGET * sizeof(*a->objs));

    for (i = 0; i < hdr->n_free_ext; i++) {
        struct facade_ckpt_ext rec;

        if (p + sizeof(rec) > end) {
            return -1;
        }
        memcpy(&rec, p, sizeof(rec));
        p += sizeof(rec);
        if (rec.nblocks == 0 || rec.start < prev_end ||
            rec.start >= a->data_last ||
            rec.nblocks > a->data_last - rec.start) {
            return -1;
        }
        facade_free_insert(a, rec.start, rec.nblocks);
        a->free_blocks += rec.nblocks;
        prev_end        = rec.start + rec.nblocks;
    }

    for (i = 0; i < hdr->n_objs; i++) {
        struct facade_obj_rec     orec;
        struct facade_ckpt_objext ext[FACADE_OBJ_MAX_EXT] = { { 0 } };
        uint32_t                  k;

        if (a->nobjs >= FACADE_OBJ_BUDGET || p + sizeof(orec) > end) {
            return -1;
        }
        memcpy(&orec, p, sizeof(orec));
        p += sizeof(orec);
        if (orec.nextents > FACADE_OBJ_MAX_EXT) {
            return -1;
        }
        for (k = 0; k < orec.nextents; k++) {
            if (p + sizeof(ext[k]) > end) {
                return -1;
            }
            memcpy(&ext[k], p, sizeof(ext[k]));
            p += sizeof(ext[k]);
        }
        if (!facade_obj_put_rec(a, &orec, ext)) {
            return -1;
        }
    }
    return 0;
} /* facade_ckpt_load */

/* ------------------------------------------------------------------ */
/* geometry                                                           */
/* ------------------------------------------------------------------ */

static void
facade_compute_geometry(struct facade_alloc *a)
{
    struct chimera_media_space_info info;
    uint64_t                        obj_budget, free_bytes, obj_bytes, ckpt_bytes;

    a->provider->info(a->space, &info);
    a->nblocks       = info.nblocks;
    a->max_io_blocks = info.max_io_blocks ? info.max_io_blocks : 1u;
    if (a->max_io_blocks > (UINT_MAX >> CHIMERA_MEDIA_BLOCK_SHIFT)) {
        a->max_io_blocks = UINT_MAX >> CHIMERA_MEDIA_BLOCK_SHIFT;
    }

    /* Each checkpoint slot holds a header, the worst-case free-set
     * fragmentation (<= nblocks/2 + 1 extents), and the object area sized for a
     * budget that scales down on a small space so a tiny prototype still leaves
     * a data region. */
    obj_budget = FACADE_OBJ_BUDGET;
    if (obj_budget > a->nblocks / 16) {
        obj_budget = a->nblocks / 16;
    }
    if (obj_budget == 0) {
        obj_budget = 1;
    }
    free_bytes = sizeof(struct facade_ckpt_hdr)
        + (a->nblocks / 2 + 1) * sizeof(struct facade_ckpt_ext);
    obj_bytes = obj_budget * (sizeof(struct facade_obj_rec)
                              + FACADE_OBJ_MAX_EXT * sizeof(struct facade_ckpt_objext));
    ckpt_bytes = free_bytes + obj_bytes;

    a->ckpt_nblocks = (ckpt_bytes + CHIMERA_MEDIA_BLOCK_SIZE - 1) >> CHIMERA_MEDIA_BLOCK_SHIFT;
    if (a->ckpt_nblocks == 0) {
        a->ckpt_nblocks = 1;
    }

    /* Log region: a modest span, small enough that a prototype test can fill it
     * to force a compaction, large enough for ordinary bursts. */
    a->log_nblocks = a->nblocks / 16;
    if (a->log_nblocks > 64) {
        a->log_nblocks = 64;
    }
    if (a->log_nblocks < 4) {
        a->log_nblocks = 4;   /* floor: a durable object txn is 3 records */
    }

    a->ckpt_a_block = FACADE_SUPER_BLOCK + 1;
    a->ckpt_b_block = a->ckpt_a_block + a->ckpt_nblocks;
    a->log_block    = a->ckpt_b_block + a->ckpt_nblocks;
    a->data_first   = a->log_block + a->log_nblocks;
    a->data_last    = a->nblocks;
} /* facade_compute_geometry */

/* ------------------------------------------------------------------ */
/* mark-dirty / compaction                                            */
/* ------------------------------------------------------------------ */

struct facade_cb_ctx {
    struct facade_alloc *a;
    facade_cb_t          cb;
    void                *priv;
};

static void
facade_mark_dirty_done(
    enum facade_status st,
    void              *priv)
{
    struct facade_cb_ctx *c  = priv;
    struct facade_alloc  *a  = c->a;
    facade_cb_t           cb = c->cb;
    void                 *cp = c->priv;

    if (st == FACADE_OK) {
        a->dirty_armed = 1;
    }
    free(c);
    /* Continue the caller first (a forced compaction it triggers will defer
     * behind the still-held block-0 slot), then release the slot and run the
     * next deferred block-0 op. */
    cb(st, cp);
    facade_mw_complete(a);
} /* facade_mark_dirty_done */

/* Issue the clean=0 superblock write, holding the single block-0 slot (mw_busy)
 * for its duration so it can never race an id-floor extension or a compaction
 * super write on block 0 (F1). */
static void
facade_mark_dirty_issue(
    struct facade_alloc *a,
    facade_cb_t          cb,
    void                *private_data)
{
    struct facade_cb_ctx *c = calloc(1, sizeof(*c));

    FACADE_CHECK(c != NULL, "oom mark-dirty ctx");
    c->a    = a;
    c->cb   = cb;
    c->priv = private_data;

    a->mw_busy = 1;
    a->clean   = 0;
    facade_super_pack(a, a->super_buf);
    facade_media_write(a, a->super_buf, FACADE_SUPER_BLOCK, 1, CHIMERA_MEDIA_WRITE_FUA,
                       facade_mark_dirty_done, c);
} /* facade_mark_dirty_issue */

void
facade_mark_dirty(
    struct facade_alloc *a,
    facade_cb_t          cb,
    void                *private_data)
{
    if (a->dirty_armed) {
        cb(FACADE_OK, private_data);
        return;
    }
    /* Serialize the block-0 write behind any in-flight block-0 op. */
    if (a->mw_busy) {
        FACADE_CHECK(!a->dirty_mark_pending, "a dirty-mark is already queued");
        a->dirty_mark_pending = 1;
        a->dirty_cb           = cb;
        a->dirty_priv         = private_data;
        return;
    }
    facade_mark_dirty_issue(a, cb, private_data);
} /* facade_mark_dirty */

struct facade_compact_ctx {
    struct facade_alloc *a;
    facade_cb_t          cb;
    void                *priv;
    void                *buf;
    uint32_t             target;     /* slot being written (0=A, 1=B) */
    uint64_t             new_gen;
    uint64_t             new_ckpt_seq;
    int                  flipped;    /* in-memory active_ckpt advanced to target */
    uint32_t             old_active; /* prior active_ckpt, to restore on flip failure */
    uint64_t             old_gen;
    uint64_t             old_ckpt_seq;
};

static void
facade_compact_finish(
    struct facade_compact_ctx *c,
    enum facade_status         st)
{
    struct facade_alloc *a  = c->a;
    facade_cb_t          cb = c->cb;
    void                *cp = c->priv;

    /* A failed super flip must not leave the in-memory active-slot pointer
     * diverged from the durable superblock (LOW): restore it so a later
     * compaction does not overwrite the still-valid old slot. */
    if (st != FACADE_OK && c->flipped) {
        a->active_ckpt     = c->old_active;
        a->ckpt_generation = c->old_gen;
        a->checkpoint_seq  = c->old_ckpt_seq;
        a->clean           = 0;   /* the flip did not commit; stay dirty */
    }

    free(c->buf);
    free(c);
    cb(st, cp);
    /* Release the block-0 slot and run the next deferred block-0 op. */
    facade_mw_complete(a);
} /* facade_compact_finish */

static void
facade_compact_flush_done(
    enum facade_status st,
    void              *priv)
{
    struct facade_compact_ctx *c = priv;
    struct facade_alloc       *a = c->a;

    if (st == FACADE_OK) {
        /* The flip is durable: reset the log and re-arm the dirty window. */
        a->log_next      = 0;
        a->dirty_armed   = 0;
        a->next_seq      = a->durable_seq + 1;
        a->durable_epoch = a->cur_epoch;
    }
    facade_compact_finish(c, st);
} /* facade_compact_flush_done */

static void
facade_compact_super_done(
    enum facade_status st,
    void              *priv)
{
    struct facade_compact_ctx *c = priv;

    if (st != FACADE_OK) {
        facade_compact_finish(c, st);
        return;
    }
    facade_media_flush(c->a, facade_compact_flush_done, c);
} /* facade_compact_super_done */

static void
facade_compact_slot_done(
    enum facade_status st,
    void              *priv)
{
    struct facade_compact_ctx *c = priv;
    struct facade_alloc       *a = c->a;

    if (st != FACADE_OK) {
        facade_compact_finish(c, st);
        return;
    }
    /* The inactive slot is durable: flip the superblock (FUA) to name it.  Save
     * the prior pointer so a failed flip can be rolled back (LOW). */
    c->old_active      = a->active_ckpt;
    c->old_gen         = a->ckpt_generation;
    c->old_ckpt_seq    = a->checkpoint_seq;
    c->flipped         = 1;
    a->active_ckpt     = c->target;
    a->ckpt_generation = c->new_gen;
    a->checkpoint_seq  = c->new_ckpt_seq;
    a->clean           = 1;
    facade_super_pack(a, a->super_buf);
    facade_media_write(a, a->super_buf, FACADE_SUPER_BLOCK, 1, CHIMERA_MEDIA_WRITE_FUA,
                       facade_compact_super_done, c);
} /* facade_compact_slot_done */

void
facade_compact(
    struct facade_alloc *a,
    facade_cb_t          cb,
    void                *private_data)
{
    struct facade_compact_ctx *c;
    uint64_t                   blocks, target_block;

    /* Serialize behind an in-flight id-floor extension (block-0 write): defer so
     * the two never write block 0 concurrently (F1).  Run by mw_complete(). */
    if (a->mw_busy) {
        FACADE_CHECK(!a->compact_pending, "a compaction is already queued");
        a->compact_pending = 1;
        a->compact_cb      = cb;
        a->compact_priv    = private_data;
        return;
    }

    FACADE_CHECK(a->next_seq == a->durable_seq + 1,
                 "compaction must run at a transaction boundary (G1)");

    /* Hold the single block-0 slot for the whole compaction (its super flip is a
     * block-0 write), so no id-floor extension can race it (F1).  Released in
     * facade_compact_finish via mw_complete(). */
    a->mw_busy = 1;

    c = calloc(1, sizeof(*c));
    FACADE_CHECK(c != NULL, "oom compact ctx");
    c->a            = a;
    c->cb           = cb;
    c->priv         = private_data;
    c->target       = 1u - a->active_ckpt;
    c->new_gen      = a->ckpt_generation + 1;
    c->new_ckpt_seq = a->durable_seq;

    blocks       = facade_ckpt_serialize(a, c->new_gen, c->new_ckpt_seq, &c->buf);
    target_block = c->target == 0 ? a->ckpt_a_block : a->ckpt_b_block;

    facade_media_write(a, c->buf, target_block, blocks, CHIMERA_MEDIA_WRITE_FUA,
                       facade_compact_slot_done, c);
} /* facade_compact */

/* ------------------------------------------------------------------ */
/* format                                                             */
/* ------------------------------------------------------------------ */

struct facade_format_ctx {
    struct facade_alloc *a;
    facade_cb_t          cb;
    void                *priv;
};

static void
facade_format_done(
    enum facade_status st,
    void              *priv)
{
    struct facade_format_ctx *f  = priv;
    struct facade_alloc      *a  = f->a;
    facade_cb_t               cb = f->cb;
    void                     *cp = f->priv;

    free(f);
    /* Format hands back no handle: release the scratch allocator.  Defer the
     * free to the enclosing media completion's teardown check (destroy_after),
     * so this callback -- which runs inside that completion -- never frees `a`
     * out from under it. */
    a->shutting_down = 1;
    a->destroy_after = 1;
    cb(st, cp);
} /* facade_format_done */

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

    facade_free_insert(a, a->data_first, a->data_last - a->data_first);
    a->free_blocks = a->data_last - a->data_first;

    a->id_nonce = facade_random_u64();
    a->id_next  = 0;
    a->id_floor = FACADE_ID_RESERVE;

    a->power_loss_generation = 1;
    a->active_ckpt           = 1;   /* so the first compact writes slot A and flips to it */
    a->ckpt_generation       = 0;
    a->checkpoint_seq        = 0;
    a->durable_seq           = 0;
    a->next_seq              = 1;
    a->log_next              = 0;
    a->clean                 = 1;
    a->dirty_armed           = 0;

    /* Lay down the initial checkpoint (slot A) + superblock, then release the
     * scratch allocator -- the caller gets a usable handle from facade_alloc_open. */
    {
        struct facade_format_ctx *f = calloc(1, sizeof(*f));

        FACADE_CHECK(f != NULL, "oom format ctx");
        f->a    = a;
        f->cb   = cb;
        f->priv = private_data;
        facade_compact(a, facade_format_done, f);
    }
} /* facade_alloc_format */

/* ------------------------------------------------------------------ */
/* open + recovery entry                                              */
/* ------------------------------------------------------------------ */

struct facade_open_ctx {
    struct facade_alloc *a;
    facade_open_cb_t     cb;
    void                *priv;
    void                *hdr_buf;        /* 1 block, slot header */
    void                *ckpt_buf;       /* content_blocks blocks */
    uint64_t             content_blocks;
    uint64_t             slot_block;
};

static void
facade_open_fail(
    struct facade_open_ctx *c,
    enum facade_status      status)
{
    struct facade_alloc *a    = c->a;
    facade_open_cb_t     cb   = c->cb;
    void                *priv = c->priv;

    free(c->hdr_buf);
    free(c->ckpt_buf);
    free(c);
    cb(status, NULL, priv);
    /* Discard the scratch allocator.  Defer the free to the enclosing media
     * completion's teardown check (destroy_after), so we never free `a` inside
     * its own callback. */
    a->shutting_down = 1;
    a->destroy_after = 1;
    facade_maybe_teardown(a);
} /* facade_open_fail */

static void
facade_open_finish(struct facade_open_ctx *c)
{
    struct facade_alloc *a    = c->a;
    facade_open_cb_t     cb   = c->cb;
    void                *priv = c->priv;

    free(c->hdr_buf);
    free(c->ckpt_buf);
    free(c);
    cb(FACADE_OK, a, priv);
} /* facade_open_finish */

static void
facade_open_recovered(
    enum facade_status st,
    void              *priv)
{
    struct facade_open_ctx *c = priv;

    if (st != FACADE_OK) {
        facade_open_fail(c, st);
        return;
    }
    facade_open_finish(c);
} /* facade_open_recovered */

static void
facade_open_body_done(
    enum facade_status st,
    void              *priv)
{
    struct facade_open_ctx *c = priv;
    struct facade_alloc    *a = c->a;
    struct facade_ckpt_hdr  hdr;
    uint32_t                stored, calc;

    if (st != FACADE_OK) {
        facade_open_fail(c, FACADE_E_IO);
        return;
    }

    memcpy(&hdr, c->ckpt_buf, sizeof(hdr));
    stored                                          = hdr.crc32;
    ((struct facade_ckpt_hdr *) c->ckpt_buf)->crc32 = 0;
    calc                                            = facade_crc32(c->ckpt_buf,
                                                                   (uint32_t) (c->content_blocks <<
                                                                               CHIMERA_MEDIA_BLOCK_SHIFT));
    if (hdr.magic != FACADE_CKPT_MAGIC || calc != stored) {
        facade_open_fail(c, FACADE_E_IO);
        return;
    }

    a->ckpt_generation = hdr.generation;
    if (facade_ckpt_load(a, c->ckpt_buf, c->content_blocks, &hdr) != 0) {
        facade_open_fail(c, FACADE_E_IO);
        return;
    }

    if (a->clean) {
        facade_open_finish(c);
        return;
    }
    /* Unclean prior shutdown: replay the log, then (inside replay) bump the
     * power-loss generation and write a fresh clean checkpoint. */
    facade_recover_replay(a, facade_open_recovered, c);
} /* facade_open_body_done */

static void
facade_open_hdr_done(
    enum facade_status st,
    void              *priv)
{
    struct facade_open_ctx *c = priv;
    struct facade_alloc    *a = c->a;
    struct facade_ckpt_hdr  hdr;

    if (st != FACADE_OK) {
        facade_open_fail(c, FACADE_E_IO);
        return;
    }
    memcpy(&hdr, c->hdr_buf, sizeof(hdr));
    if (hdr.magic != FACADE_CKPT_MAGIC || hdr.content_blocks == 0 ||
        hdr.content_blocks > a->ckpt_nblocks) {
        facade_open_fail(c, FACADE_E_IO);
        return;
    }
    c->content_blocks = hdr.content_blocks;
    c->ckpt_buf       = calloc(c->content_blocks, CHIMERA_MEDIA_BLOCK_SIZE);
    FACADE_CHECK(c->ckpt_buf != NULL, "oom checkpoint read buffer");
    facade_media_read(a, c->ckpt_buf, c->slot_block, c->content_blocks,
                      facade_open_body_done, c);
} /* facade_open_hdr_done */

static void
facade_open_super_done(
    enum facade_status st,
    void              *priv)
{
    struct facade_open_ctx *c = priv;
    struct facade_alloc    *a = c->a;
    struct facade_super     s;
    uint32_t                stored, calc;

    if (st != FACADE_OK) {
        facade_open_fail(c, FACADE_E_IO);
        return;
    }

    memcpy(&s, a->super_buf, sizeof(s));
    stored                                        = s.crc32;
    ((struct facade_super *) a->super_buf)->crc32 = 0;
    calc                                          = facade_crc32(a->super_buf, FACADE_SUPER_SIZE);

    if (s.magic != FACADE_SUPER_MAGIC || calc != stored) {
        /* Absent, zeroed or corrupt -- not a recognized format. */
        facade_open_fail(c, FACADE_E_IO);
        return;
    }
    if (s.version != FACADE_FORMAT_VERSION || s.block_size != CHIMERA_MEDIA_BLOCK_SIZE) {
        /* Recognized superblock, unsupported format: refuse clearly (L41). */
        facade_open_fail(c, FACADE_E_FORMAT);
        return;
    }

    /* Geometry sanity before trusting any offset (a->nblocks is the live size). */
    if (s.ckpt_a_block != FACADE_SUPER_BLOCK + 1 ||
        s.ckpt_b_block != s.ckpt_a_block + s.ckpt_nblocks ||
        s.ckpt_nblocks == 0 ||
        s.log_block != s.ckpt_b_block + s.ckpt_nblocks ||
        s.data_first != s.log_block + s.log_nblocks ||
        s.data_first >= s.data_last || s.data_last > a->nblocks ||
        s.active_ckpt > 1) {
        facade_open_fail(c, FACADE_E_IO);
        return;
    }

    a->nblocks               = s.nblocks;
    a->data_first            = s.data_first;
    a->data_last             = s.data_last;
    a->ckpt_a_block          = s.ckpt_a_block;
    a->ckpt_b_block          = s.ckpt_b_block;
    a->ckpt_nblocks          = s.ckpt_nblocks;
    a->log_block             = s.log_block;
    a->log_nblocks           = s.log_nblocks;
    a->active_ckpt           = s.active_ckpt;
    a->checkpoint_seq        = s.checkpoint_seq;
    a->power_loss_generation = s.power_loss_generation;
    a->id_nonce              = s.id_nonce;
    a->id_floor              = s.id_floor;
    a->id_next               = s.id_floor;   /* resume at the durable floor (no reuse) */
    a->clean                 = s.clean;
    a->dirty_armed           = (s.clean == 0);
    a->durable_seq           = s.checkpoint_seq;
    a->next_seq              = s.checkpoint_seq + 1;
    a->log_next              = 0;

    c->slot_block = (a->active_ckpt == 0) ? a->ckpt_a_block : a->ckpt_b_block;
    c->hdr_buf    = calloc(1, CHIMERA_MEDIA_BLOCK_SIZE);
    FACADE_CHECK(c->hdr_buf != NULL, "oom checkpoint header buffer");
    facade_media_read(a, c->hdr_buf, c->slot_block, 1, facade_open_hdr_done, c);
} /* facade_open_super_done */

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
    provider->info(space, &info);
    a->max_io_blocks = info.max_io_blocks ? info.max_io_blocks : 1u;
    if (a->max_io_blocks > (UINT_MAX >> CHIMERA_MEDIA_BLOCK_SHIFT)) {
        a->max_io_blocks = UINT_MAX >> CHIMERA_MEDIA_BLOCK_SHIFT;
    }
    a->nblocks = info.nblocks;

    c = calloc(1, sizeof(*c));
    FACADE_CHECK(c != NULL, "oom open ctx");
    c->a    = a;
    c->cb   = cb;
    c->priv = private_data;

    facade_media_read(a, a->super_buf, FACADE_SUPER_BLOCK, 1,
                      facade_open_super_done, c);
} /* facade_alloc_open */

/* ------------------------------------------------------------------ */
/* flush / close / abandon                                            */
/* ------------------------------------------------------------------ */

struct facade_flush_epoch_ctx {
    struct facade_alloc *a;
    facade_epoch_cb_t    cb;
    void                *priv;
};

static void
facade_flush_compact_done(
    enum facade_status st,
    void              *priv)
{
    struct facade_flush_epoch_ctx *c  = priv;
    struct facade_alloc           *a  = c->a;
    facade_epoch_cb_t              cb = c->cb;
    void                          *cp = c->priv;
    uint64_t                       de = a->durable_epoch;

    free(c);
    cb(st, de, cp);
} /* facade_flush_compact_done */

SYMBOL_EXPORT void
facade_alloc_flush(
    struct facade_alloc *a,
    facade_epoch_cb_t    cb,
    void                *private_data)
{
    struct facade_flush_epoch_ctx *c = calloc(1, sizeof(*c));

    FACADE_CHECK(c != NULL, "oom flush ctx");
    c->a    = a;
    c->cb   = cb;
    c->priv = private_data;
    facade_compact(a, facade_flush_compact_done, c);
} /* facade_alloc_flush */

struct facade_close_ctx {
    struct facade_alloc *a;
    facade_cb_t          cb;
    void                *priv;
};

static void
facade_close_compact_done(
    enum facade_status st,
    void              *priv)
{
    struct facade_close_ctx *c  = priv;
    struct facade_alloc     *a  = c->a;
    facade_cb_t              cb = c->cb;
    void                    *cp = c->priv;

    free(c);
    /* Defer the free to the enclosing completion's teardown check, so we do not
     * free `a` inside its own media callback. */
    a->shutting_down = 1;
    a->destroy_after = 1;
    cb(st, cp);
} /* facade_close_compact_done */

SYMBOL_EXPORT void
facade_alloc_close(
    struct facade_alloc *a,
    facade_cb_t          cb,
    void                *private_data)
{
    struct facade_close_ctx *c = calloc(1, sizeof(*c));

    FACADE_CHECK(c != NULL, "oom close ctx");
    c->a    = a;
    c->cb   = cb;
    c->priv = private_data;
    facade_compact(a, facade_close_compact_done, c);
} /* facade_alloc_close */

SYMBOL_EXPORT void
facade_alloc_abandon(struct facade_alloc *a)
{
    /* Drop without persisting.  If a media op is in flight, mark for teardown and
     * stop new work; the last completion frees the allocator (F2).  Media keeps
     * only what prior fua writes/flushes made durable -- in particular the id
     * floor and the last clean checkpoint. */
    a->shutting_down = 1;
    if (a->mw_busy || a->inflight > 0) {
        a->destroy_after = 1;
        return;
    }
    facade_alloc_teardown(a);
} /* facade_alloc_abandon */
