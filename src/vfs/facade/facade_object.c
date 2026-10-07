// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Backend-facade object lifecycle (object-ops-v1).  See facade_object.h.
 *
 * The generalized durable transaction.  A durable mutation is one
 * {ALLOC*, FREE*, OBJ, COMMIT} log transaction -- the slice's single-extent
 * write was the first caller of this shape.  The machinery owns the ordering
 * the slice hard-coded: acquire the one-at-a-time transaction FIFO; arm the
 * dirty window; if the transaction will not fit the remaining log, force a
 * compaction FIRST (G1: a consistent cut, before any in-memory allocation
 * delta) and re-arm; snapshot the object for rollback; run the op's prepare
 * (which allocates post-compaction, mutates the extent map, and emits the
 * ALLOC/FREE records, quarantining freed owned blocks); FUA the data block if
 * any; append ALLOC/FREE, then the OBJ record, then COMMIT, each FUA, so the
 * COMMIT is durable only after the data (G2); on the durable COMMIT, release the
 * quarantined frees to the free set (no reuse until the FREE is durable) and
 * report.  Any media failure rolls the whole transaction back.
 *
 * An object release (discard / epoch-held) takes the other durable path: mutate
 * in memory and force a compaction (crash-atomic free set + object table), still
 * under the FIFO; re-ATTACH also GCs OPEN && !HELD leftovers, so a release never
 * resurrects across a crash.
 *
 * Links no libevpl; all media I/O is through the facade_media_* helpers.
 */

#include <string.h>

#include "common/macros.h"
#include "facade_internal.h"
#include "facade_log.h"
#include "facade_object.h"
#include "media_types.h"
#include "sc_request.h"   /* CHIMERA_SC_SEG_*, struct chimera_sc_segment */

#define FACADE_TXN_MAX_AF (FACADE_OBJ_MAX_EXT + 1u)   /* <=1 ALLOC + <=CAP FREE */

struct facade_obj_txn {
    struct facade_alloc      *a;
    uint8_t                   id[16];   /* the object mutated/created */
    facade_epoch_cb_t         cb;
    void                     *priv;
    uint32_t                  max_recs; /* upper bound on log records (gate) */
    int                       target_may_be_new; /* 1 = create/clone (t->id does
                                                  * not yet exist); 0 = a mutator
                                                  * whose target must still exist
                                                  * when prepare runs (CR-3) */
    uint32_t                  (*prepare)(
        struct facade_obj_txn *t);      /* post-compaction mutation; facade_status */

    struct {
        uint32_t type;                  /* FACADE_LR_ALLOC / FACADE_LR_FREE */
        uint64_t start;
        uint64_t nblocks;
    }                    af[FACADE_TXN_MAX_AF];
    uint32_t                  naf;

    struct {
        uint64_t start;
        uint64_t nblocks;
    }                    quar[FACADE_OBJ_MAX_EXT];   /* freed owned; released on COMMIT */
    uint32_t                  nquar;

    const void               *data_buf; /* FUA'd before the records (OBJ_WRITE COW) */
    chimera_media_block_t     data_phys;
    uint64_t                  data_nblocks;
    int                       has_data;

    /* rollback snapshot of the target object. */
    int                       obj_preexisted;
    struct facade_extent_map *saved_map;
    uint32_t                  saved_flags, saved_state, saved_ret_mode;
    uint64_t                  saved_size, saved_change, saved_ret_until;
    struct chimera_sc_stamps  saved_stamps;

    /* log append state. */
    uint64_t                  txn_id;
    uint64_t                  pre_seq, pre_log_next;
    int                       txn_started;
    uint32_t                  cur; /* 0..naf-1 = af; naf = OBJ; naf+1 = COMMIT */
    uint32_t                  nrecs; /* naf + 2 */
    void                     *rec_buf; /* one 4 KiB block, reused */
    uint8_t                  *obj_payload;
    uint32_t                  obj_len;

    /* op-specific args (one op per transaction). */
    union {
        struct {
            uint64_t offset;
            uint32_t len;
            uint32_t level;
            uint8_t  staging[CHIMERA_MEDIA_BLOCK_SIZE];
        } write;
        struct {
            uint64_t new_size;
        } trunc;
        struct {
            uint64_t offset;
            uint64_t length;
        } space;
        struct {
            uint8_t                  sealed_id[16];
            struct chimera_sc_obj_id new_id;
        } clone;
    } u;
};

/* ---- record emission used by the op prepare callbacks ---- */

static void
facade_txn_on_freed(
    chimera_media_block_t phys,
    uint32_t              nblocks,
    void                 *private_data)
{
    struct facade_obj_txn *t = private_data;

    FACADE_CHECK(t->naf < FACADE_TXN_MAX_AF, "txn FREE record overflow");
    FACADE_CHECK(t->nquar < FACADE_OBJ_MAX_EXT, "txn quarantine overflow");
    t->af[t->naf].type    = FACADE_LR_FREE;
    t->af[t->naf].start   = phys;
    t->af[t->naf].nblocks = nblocks;
    t->naf++;
    t->quar[t->nquar].start   = phys;
    t->quar[t->nquar].nblocks = nblocks;
    t->nquar++;
} /* facade_txn_on_freed */

/* Allocate nblocks for this transaction (post-compaction, so no in-memory delta
 * was in the compaction snapshot -- G1), logging the ALLOC.  FACADE_OK + *r, or
 * FACADE_E_NO_SPACE. */
static enum facade_status
facade_txn_alloc(
    struct facade_obj_txn *t,
    uint32_t               nblocks,
    chimera_media_block_t *r_start)
{
    chimera_media_block_t start;
    enum facade_status st = facade_alloc_blocks(t->a, nblocks, &start, NULL);

    if (st != FACADE_OK) {
        return st;
    }
    FACADE_CHECK(t->naf < FACADE_TXN_MAX_AF, "txn ALLOC record overflow");
    t->af[t->naf].type    = FACADE_LR_ALLOC;
    t->af[t->naf].start   = start;
    t->af[t->naf].nblocks = nblocks;
    t->naf++;
    *r_start = start;
    return FACADE_OK;
} /* facade_txn_alloc */

/* ---- the transaction state machine ---- */

static void facade_txn_append_next(
    struct facade_obj_txn *t);

static void
facade_txn_free_bufs(struct facade_obj_txn *t)
{
    free(t->rec_buf);
    free(t->obj_payload);
    free(t);
} /* facade_txn_free_bufs */

static void
facade_txn_fail(
    struct facade_obj_txn *t,
    enum facade_status     st)
{
    struct facade_alloc *a  = t->a;
    facade_epoch_cb_t    cb = t->cb;
    void                *cp = t->priv;
    uint64_t             g  = a->power_loss_generation;
    struct facade_obj   *o;
    uint32_t             i;

    /* Rewind the log cursors this uncommitted txn advanced (durable-but-
     * uncommitted records on media are harmless: recovery drops a txn with no
     * COMMIT, and the slots are overwritten by the next append). */
    if (t->txn_started) {
        a->next_seq = t->pre_seq;
        a->log_next = t->pre_log_next;
    }

    /* Restore the target object to its pre-transaction state. */
    o = facade_obj_find(a, t->id);
    if (t->obj_preexisted) {
        if (o) {
            facade_extent_map_destroy(o->map);
            o->map                = t->saved_map;
            t->saved_map          = NULL;
            o->flags              = t->saved_flags;
            o->state              = t->saved_state;
            o->size               = t->saved_size;
            o->change             = t->saved_change;
            o->stamps             = t->saved_stamps;
            o->retention_until_ns = t->saved_ret_until;
            o->retention_mode     = t->saved_ret_mode;
        }
    } else if (o) {
        facade_obj_remove(a, o);        /* created by prepare (clone); undo it */
    }
    facade_extent_map_destroy(t->saved_map);   /* no-op if transferred/NULL */
    t->saved_map = NULL;

    /* Return blocks this txn allocated; leave quarantined frees allocated (the
     * restored map still references them). */
    for (i = 0; i < t->naf; i++) {
        if (t->af[i].type == FACADE_LR_ALLOC) {
            facade_alloc_free(a, t->af[i].start, (uint32_t) t->af[i].nblocks);
        }
    }

    facade_txn_free_bufs(t);
    cb(st, g, cp);
    facade_txn_release(a);
} /* facade_txn_fail */

static void
facade_txn_finish_ok(struct facade_obj_txn *t)
{
    struct facade_alloc *a  = t->a;
    facade_epoch_cb_t    cb = t->cb;
    void                *cp = t->priv;
    uint64_t             g  = a->power_loss_generation;
    uint32_t             i;

    /* The COMMIT is durable: the whole transaction is durable.  Only now return
     * the freed owned blocks to the allocatable free set (no reuse of a freed
     * block until its FREE record is durable). */
    for (i = 0; i < t->nquar; i++) {
        facade_alloc_free(a, t->quar[i].start, (uint32_t) t->quar[i].nblocks);
    }
    a->durable_seq   = t->txn_id + t->nrecs - 1;
    a->durable_epoch = a->cur_epoch;
    facade_extent_map_destroy(t->saved_map);   /* the pre-txn clone, no longer needed */
    t->saved_map = NULL;

    facade_txn_free_bufs(t);
    cb(FACADE_OK, g, cp);
    facade_txn_release(a);
} /* facade_txn_finish_ok */

static void
facade_txn_append_done(
    enum facade_status st,
    void              *priv)
{
    struct facade_obj_txn *t = priv;

    if (st != FACADE_OK) {
        facade_txn_fail(t, st);
        return;
    }
    if (t->a->shutting_down) {
        facade_txn_fail(t, FACADE_E_SHUTDOWN);
        return;
    }
    t->cur++;
    if (t->cur < t->nrecs) {
        facade_txn_append_next(t);
        return;
    }
    facade_txn_finish_ok(t);   /* the COMMIT (last record) is durable */
} /* facade_txn_append_done */

static void
facade_txn_append_next(struct facade_obj_txn *t)
{
    struct facade_alloc *a    = t->a;
    uint64_t             seq  = a->next_seq;
    uint64_t             slot = a->log_next;

    FACADE_CHECK(slot < a->log_nblocks, "log overflow past compaction");
    if (t->cur == 0) {
        t->txn_id = seq;   /* the first record's seq is the transaction id */
    }
    a->next_seq++;
    a->log_next++;

    if (t->cur < t->naf) {
        struct facade_lr_alloc payload = { t->af[t->cur].start, t->af[t->cur].nblocks };

        facade_lr_build(t->rec_buf, (enum facade_lr_type) t->af[t->cur].type,
                        seq, t->txn_id, &payload, (uint32_t) sizeof(payload));
    } else if (t->cur == t->naf) {
        facade_lr_build(t->rec_buf, FACADE_LR_OBJ, seq, t->txn_id,
                        t->obj_payload, t->obj_len);
    } else {
        struct facade_lr_commit commit = { t->txn_id, t->nrecs };

        facade_lr_build(t->rec_buf, FACADE_LR_COMMIT, seq, t->txn_id,
                        &commit, (uint32_t) sizeof(commit));
    }
    facade_media_write(a, t->rec_buf, a->log_block + slot, 1,
                       CHIMERA_MEDIA_WRITE_FUA, facade_txn_append_done, t);
} /* facade_txn_append_next */

static void
facade_txn_data_done(
    enum facade_status st,
    void              *priv)
{
    struct facade_obj_txn *t = priv;

    if (st != FACADE_OK) {
        facade_txn_fail(t, st);
        return;
    }
    if (t->a->shutting_down) {
        facade_txn_fail(t, FACADE_E_SHUTDOWN);
        return;
    }
    facade_txn_append_next(t);
} /* facade_txn_data_done */

/* Serialize the (mutated) object into the OBJ record payload. */
static void
facade_txn_build_obj_payload(struct facade_obj_txn *t)
{
    struct facade_obj    *o = facade_obj_find(t->a, t->id);
    struct facade_obj_rec hdr;
    uint8_t              *p;

    FACADE_CHECK(o != NULL, "txn OBJ record names a missing object");
    facade_obj_serialize_hdr(o, &hdr);
    t->obj_len = (uint32_t) (sizeof(hdr) + (uint64_t) hdr.nextents
                             * sizeof(struct facade_ckpt_objext));
    t->obj_payload = calloc(1, t->obj_len ? t->obj_len : 1);
    FACADE_CHECK(t->obj_payload != NULL, "oom obj payload");
    memcpy(t->obj_payload, &hdr, sizeof(hdr));
    p = t->obj_payload + sizeof(hdr);
    facade_ckpt_serialize_objext(o->map, &p);   /* helper in facade_alloc.c */
} /* facade_txn_build_obj_payload */

static void
facade_txn_prepare_phase(struct facade_obj_txn *t)
{
    struct facade_alloc *a = t->a;
    struct facade_obj   *o;
    enum facade_status   st;

    if (a->shutting_down) {
        facade_txn_fail(t, FACADE_E_SHUTDOWN);
        return;
    }

    /* CR-3: a mutator's target may have been removed between its dispatch-time
     * entry check and now -- a discard (or epoch-held release) queued ahead of
     * it on the FIFO removes the object, and this op would then dereference the
     * missing entry in its prepare (a NULL-deref SEGV).  Fail NOT_FOUND here,
     * before prepare runs.  create/clone are exempt: their t->id is the
     * intentionally-new id that prepare is about to create. */
    o = facade_obj_find(a, t->id);
    if (!o && !t->target_may_be_new) {
        facade_txn_fail(t, FACADE_E_NOT_FOUND);
        return;
    }

    /* Snapshot the target object for rollback. */
    t->obj_preexisted = (o != NULL);
    if (o) {
        t->saved_map       = facade_extent_map_clone(o->map, 0);
        t->saved_flags     = o->flags;
        t->saved_state     = o->state;
        t->saved_size      = o->size;
        t->saved_change    = o->change;
        t->saved_stamps    = o->stamps;
        t->saved_ret_until = o->retention_until_ns;
        t->saved_ret_mode  = o->retention_mode;
    }

    st = t->prepare(t);
    if (st != FACADE_OK) {
        facade_txn_fail(t, st);
        return;
    }

    facade_txn_build_obj_payload(t);

    t->txn_started  = 1;
    t->pre_seq      = a->next_seq;
    t->pre_log_next = a->log_next;
    t->nrecs        = t->naf + 2u;   /* ALLOC/FREE... + OBJ + COMMIT */
    t->cur          = 0;

    if (t->has_data) {
        facade_media_write(a, t->data_buf, t->data_phys, t->data_nblocks,
                           CHIMERA_MEDIA_WRITE_FUA, facade_txn_data_done, t);
    } else {
        facade_txn_append_next(t);
    }
} /* facade_txn_prepare_phase */

static void
facade_txn_compacted(
    enum facade_status st,
    void              *priv);

static void
facade_txn_remark_done(
    enum facade_status st,
    void              *priv)
{
    struct facade_obj_txn *t = priv;

    if (st != FACADE_OK) {
        facade_txn_fail(t, st);
        return;
    }
    facade_txn_prepare_phase(t);
} /* facade_txn_remark_done */

static void
facade_txn_compacted(
    enum facade_status st,
    void              *priv)
{
    struct facade_obj_txn *t = priv;

    if (st != FACADE_OK) {
        facade_txn_fail(t, st);
        return;
    }
    /* The compaction set clean=1 and cleared the dirty window; re-arm clean=0
     * before appending this transaction's post-compaction records. */
    facade_mark_dirty(t->a, facade_txn_remark_done, t);
} /* facade_txn_compacted */

static void
facade_txn_dirty_done(
    enum facade_status st,
    void              *priv)
{
    struct facade_obj_txn *t = priv;
    struct facade_alloc   *a = t->a;

    if (st != FACADE_OK) {
        facade_txn_fail(t, st);
        return;
    }
    if (a->shutting_down) {
        facade_txn_fail(t, FACADE_E_SHUTDOWN);
        return;
    }
    /* G1: if the transaction will not fit the remaining log, compact first at
    * the transaction boundary (no in-memory allocation delta applied yet). */
    if (a->log_next + t->max_recs > a->log_nblocks) {
        facade_compact(a, facade_txn_compacted, t);
        return;
    }
    facade_txn_prepare_phase(t);
} /* facade_txn_dirty_done */

static void
facade_txn_start(void *arg)
{
    struct facade_obj_txn *t = arg;
    struct facade_alloc   *a = t->a;

    if (a->shutting_down) {
        facade_txn_fail(t, FACADE_E_SHUTDOWN);
        return;
    }
    /* Transaction-size bound: a txn that cannot fit an empty log would deadlock
     * compaction-before-wrap.  Refuse it cleanly (NO_SPACE-shaped). */
    if (t->max_recs > a->log_nblocks) {
        facade_txn_fail(t, FACADE_E_NO_SPACE);
        return;
    }
    facade_mark_dirty(a, facade_txn_dirty_done, t);
} /* facade_txn_start */

/* Allocate + queue a durable object transaction behind the FIFO. */
static struct facade_obj_txn *
facade_txn_new(
    struct facade_alloc *a,
    const uint8_t        id[16],
    uint32_t             max_recs,
    uint32_t (          *prepare )(struct facade_obj_txn *),
    facade_epoch_cb_t    cb,
    void                *priv)
{
    struct facade_obj_txn *t = calloc(1, sizeof(*t));

    FACADE_CHECK(t != NULL, "oom obj txn");
    t->a = a;
    memcpy(t->id, id, 16);
    t->max_recs = max_recs;
    t->prepare  = prepare;
    t->cb       = cb;
    t->priv     = priv;
    t->rec_buf  = calloc(1, CHIMERA_MEDIA_BLOCK_SIZE);
    FACADE_CHECK(t->rec_buf != NULL, "oom txn rec buf");
    return t;
} /* facade_txn_new */

static void
facade_txn_submit(struct facade_obj_txn *t)
{
    facade_txn_acquire(t->a, facade_txn_start, t);
} /* facade_txn_submit */

/* ====================================================================== */
/* op entry points                                                        */
/* ====================================================================== */

/* ---- OBJ_WRITE ---- */

static uint32_t
facade_prep_write(struct facade_obj_txn *t)
{
    struct facade_obj    *o = facade_obj_find(t->a, t->id);
    chimera_media_block_t p;
    enum facade_status    st;
    uint64_t              lb = t->u.write.offset >> CHIMERA_MEDIA_BLOCK_SHIFT;
    uint64_t              end;

    st = facade_txn_alloc(t, 1, &p);
    if (st != FACADE_OK) {
        return st;
    }
    t->data_buf     = t->u.write.staging;
    t->data_phys    = p;
    t->data_nblocks = 1;
    t->has_data     = 1;

    facade_extent_map_insert(o->map, lb, 1, p, CHIMERA_SC_SEG_DATA, 1u,
                             facade_txn_on_freed, t);
    /* Extent-cap guard (internal.h FACADE_OBJ_MAX_EXT): an op whose map would
     * exceed the per-object cap is refused NO_SPACE-shaped BEFORE any record is
     * logged -- the machinery rolls back the map mutation and frees the block.
     * Without this an over-cap OBJ record is acknowledged, then its nextents >
     * cap stops the durable prefix on recovery and silently drops the write. */
    if (facade_extent_map_count(o->map) > FACADE_OBJ_MAX_EXT) {
        return FACADE_E_NO_SPACE;
    }
    end = t->u.write.offset + t->u.write.len;
    if (end > o->size) {
        o->size = end;
    }
    o->change++;
    return FACADE_OK;
} /* facade_prep_write */

static void
facade_write_after_read(
    enum facade_status st,
    void              *priv)
{
    struct facade_obj_txn *t    = priv;
    uint32_t               boff = (uint32_t) (t->u.write.offset & (CHIMERA_MEDIA_BLOCK_SIZE - 1));

    if (st != FACADE_OK) {
        /* The pre-read failed; report through the write cb and drop the txn. */
        facade_epoch_cb_t cb = t->cb;
        void             *cp = t->priv;
        uint64_t          g  = t->a->power_loss_generation;

        facade_txn_free_bufs(t);
        cb(st, g, cp);
        return;
    }
    /* staging now holds the block's current bytes; merge the new bytes in. */
    memcpy(t->u.write.staging + boff, t->data_buf, t->u.write.len);
    t->data_buf = NULL;   /* reset; prepare sets it to staging */
    facade_txn_submit(t);
} /* facade_write_after_read */

SYMBOL_EXPORT void
facade_obj_write(
    struct facade_alloc *a,
    const uint8_t        id[16],
    uint64_t             offset,
    const void          *data,
    uint32_t             len,
    uint32_t             level,
    facade_epoch_cb_t    cb,
    void                *private_data)
{
    struct facade_obj     *o = facade_obj_find(a, id);
    struct facade_obj_txn *t;
    uint32_t               boff = (uint32_t) (offset & (CHIMERA_MEDIA_BLOCK_SIZE - 1));
    uint64_t               lb;
    struct facade_extent   cur;

    if (!o) {
        cb(FACADE_E_NOT_FOUND, a->power_loss_generation, private_data);
        return;
    }
    if (o->state != CHIMERA_SC_OBJ_OPEN) {
        cb(FACADE_E_STATE, a->power_loss_generation, private_data);
        return;
    }
    if (len == 0 || (uint64_t) boff + len > CHIMERA_MEDIA_BLOCK_SIZE) {
        /* The prototype writes one logical block, with no block straddle; the
         * namespace layer splits larger / straddling writes. */
        cb(FACADE_E_INVAL, a->power_loss_generation, private_data);
        return;
    }

    t = facade_txn_new(a, id, 4u /* ALLOC + FREE + OBJ + COMMIT */,
                       facade_prep_write, cb, private_data);
    t->u.write.offset = offset;
    t->u.write.len    = len;
    t->u.write.level  = level;
    lb                = offset >> CHIMERA_MEDIA_BLOCK_SHIFT;

    /* Build the block's content for the copy-on-write.  A partial write over an
     * existing DATA block reads it first (preserving the untouched bytes, from a
     * clone origin too); otherwise the untouched bytes are zeros. */
    memset(t->u.write.staging, 0, CHIMERA_MEDIA_BLOCK_SIZE);
    if ((boff != 0 || len != CHIMERA_MEDIA_BLOCK_SIZE) &&
        facade_extent_map_lookup(o->map, lb, &cur) && cur.kind == CHIMERA_SC_SEG_DATA) {
        chimera_media_block_t phys = cur.phys_block + (lb - cur.logical_block);

        /* data is borrowed until the read completes (facade_write_after_read
         * merges it); stash it in data_buf for that step. */
        t->data_buf = data;
        facade_media_read(a, t->u.write.staging, phys, 1,
                          facade_write_after_read, t);
        return;
    }
    memcpy(t->u.write.staging + boff, data, len);
    facade_txn_submit(t);
} /* facade_obj_write */

/* ---- OBJ_SEAL ---- */

static uint32_t
facade_prep_seal(struct facade_obj_txn *t)
{
    struct facade_obj *o = facade_obj_find(t->a, t->id);

    o->state = CHIMERA_SC_OBJ_SEALED;
    o->change++;
    return FACADE_OK;
} /* facade_prep_seal */

SYMBOL_EXPORT void
facade_obj_seal(
    struct facade_alloc *a,
    const uint8_t        id[16],
    facade_epoch_cb_t    cb,
    void                *private_data)
{
    struct facade_obj *o = facade_obj_find(a, id);

    if (!o) {
        cb(FACADE_E_NOT_FOUND, a->power_loss_generation, private_data);
        return;
    }
    if (o->state != CHIMERA_SC_OBJ_OPEN) {
        cb(FACADE_E_STATE, a->power_loss_generation, private_data);
        return;
    }
    facade_txn_submit(facade_txn_new(a, id, 2u, facade_prep_seal, cb, private_data));
} /* facade_obj_seal */

/* ---- OBJ_TRUNCATE ---- */

static uint32_t
facade_prep_truncate(struct facade_obj_txn *t)
{
    struct facade_obj *o         = facade_obj_find(t->a, t->id);
    uint64_t           new_size  = t->u.trunc.new_size;
    uint64_t           first_blk = (new_size + CHIMERA_MEDIA_BLOCK_SIZE - 1)
        >> CHIMERA_MEDIA_BLOCK_SHIFT;
    uint64_t           max_blk = facade_extent_map_max_logical(o->map);

    if (new_size < o->size && max_blk > first_blk) {
        facade_extent_map_punch(o->map, first_blk, (uint32_t) (max_blk - first_blk),
                                facade_txn_on_freed, t);
    }
    o->size = new_size;
    o->change++;
    return FACADE_OK;
} /* facade_prep_truncate */

SYMBOL_EXPORT void
facade_obj_truncate(
    struct facade_alloc *a,
    const uint8_t        id[16],
    uint64_t             new_size,
    facade_epoch_cb_t    cb,
    void                *private_data)
{
    struct facade_obj     *o = facade_obj_find(a, id);
    struct facade_obj_txn *t;

    if (!o) {
        cb(FACADE_E_NOT_FOUND, a->power_loss_generation, private_data);
        return;
    }
    if (o->state != CHIMERA_SC_OBJ_OPEN) {
        cb(FACADE_E_STATE, a->power_loss_generation, private_data);
        return;
    }
    /* Upper bound on log records: at most one FREE per existing extent, plus the
     * OBJ and COMMIT.  (A grow frees nothing; a shrink frees the tail extents.) */
    t = facade_txn_new(a, id, facade_extent_map_count(o->map) + 2u,
                       facade_prep_truncate, cb, private_data);
    t->u.trunc.new_size = new_size;
    facade_txn_submit(t);
} /* facade_obj_truncate */

/* ---- OBJ_ALLOCATE ---- */

static uint32_t
facade_prep_allocate(struct facade_obj_txn *t)
{
    struct facade_obj    *o    = facade_obj_find(t->a, t->id);
    uint64_t              off  = t->u.space.offset;
    uint64_t              lb   = off >> CHIMERA_MEDIA_BLOCK_SHIFT;
    uint64_t              end  = off + t->u.space.length;
    uint64_t              lend = (end + CHIMERA_MEDIA_BLOCK_SIZE - 1) >> CHIMERA_MEDIA_BLOCK_SHIFT;
    uint32_t              nblk = (uint32_t) (lend - lb);
    chimera_media_block_t p;
    enum facade_status    st;

    st = facade_txn_alloc(t, nblk, &p);
    if (st != FACADE_OK) {
        return st;
    }
    facade_extent_map_insert(o->map, lb, nblk, p, CHIMERA_SC_SEG_UNWRITTEN, 1u,
                             facade_txn_on_freed, t);
    if (facade_extent_map_count(o->map) > FACADE_OBJ_MAX_EXT) {
        return FACADE_E_NO_SPACE;   /* extent-cap guard (rolled back) */
    }
    if (end > o->size) {
        o->size = end;
    }
    o->change++;
    return FACADE_OK;
} /* facade_prep_allocate */

SYMBOL_EXPORT void
facade_obj_allocate(
    struct facade_alloc *a,
    const uint8_t        id[16],
    uint64_t             offset,
    uint64_t             length,
    facade_epoch_cb_t    cb,
    void                *private_data)
{
    struct facade_obj     *o = facade_obj_find(a, id);
    struct facade_obj_txn *t;

    if (!o) {
        cb(FACADE_E_NOT_FOUND, a->power_loss_generation, private_data);
        return;
    }
    if (o->state != CHIMERA_SC_OBJ_OPEN) {
        cb(FACADE_E_STATE, a->power_loss_generation, private_data);
        return;
    }
    if (length == 0) {
        cb(FACADE_E_INVAL, a->power_loss_generation, private_data);
        return;
    }
    /* At most one FREE per existing extent the reservation overlaps, plus the
     * ALLOC, the OBJ and the COMMIT. */
    t = facade_txn_new(a, id, facade_extent_map_count(o->map) + 3u,
                       facade_prep_allocate, cb, private_data);
    t->u.space.offset = offset;
    t->u.space.length = length;
    facade_txn_submit(t);
} /* facade_obj_allocate */

/* ---- OBJ_PUNCH ---- */

static uint32_t
facade_prep_punch(struct facade_obj_txn *t)
{
    struct facade_obj *o    = facade_obj_find(t->a, t->id);
    uint64_t           off  = t->u.space.offset;
    uint64_t           lb   = off >> CHIMERA_MEDIA_BLOCK_SHIFT;
    uint64_t           end  = off + t->u.space.length;
    uint64_t           lend = end >> CHIMERA_MEDIA_BLOCK_SHIFT;   /* whole blocks only */

    if (lend > lb) {
        facade_extent_map_punch(o->map, lb, (uint32_t) (lend - lb),
                                facade_txn_on_freed, t);
    }
    /* Punching the middle of one extent splits it into head + tail (+1), so a
     * punch can also cross the cap; refuse NO_SPACE-shaped (rolled back). */
    if (facade_extent_map_count(o->map) > FACADE_OBJ_MAX_EXT) {
        return FACADE_E_NO_SPACE;
    }
    o->change++;
    return FACADE_OK;
} /* facade_prep_punch */

SYMBOL_EXPORT void
facade_obj_punch(
    struct facade_alloc *a,
    const uint8_t        id[16],
    uint64_t             offset,
    uint64_t             length,
    facade_epoch_cb_t    cb,
    void                *private_data)
{
    struct facade_obj     *o = facade_obj_find(a, id);
    struct facade_obj_txn *t;

    if (!o) {
        cb(FACADE_E_NOT_FOUND, a->power_loss_generation, private_data);
        return;
    }
    if (o->state != CHIMERA_SC_OBJ_OPEN) {
        cb(FACADE_E_STATE, a->power_loss_generation, private_data);
        return;
    }
    /* At most one FREE per existing extent in the punched range, plus OBJ and
     * COMMIT. */
    t = facade_txn_new(a, id, facade_extent_map_count(o->map) + 2u,
                       facade_prep_punch, cb, private_data);
    t->u.space.offset = offset;
    t->u.space.length = length;
    facade_txn_submit(t);
} /* facade_obj_punch */

/* ---- OBJ_CLONE_WORKING (D2) ---- */

static uint32_t
facade_prep_clone(struct facade_obj_txn *t)
{
    struct facade_obj *origin = facade_obj_find(t->a, t->u.clone.sealed_id);
    struct facade_obj *w;

    /* Re-validate under the FIFO slot (the origin cannot change -- sealed, never
     * removed in this unit -- but keep the check honest). */
    if (!origin) {
        return FACADE_E_NOT_FOUND;
    }
    if (origin->state != CHIMERA_SC_OBJ_SEALED) {
        return FACADE_E_STATE;
    }
    w = facade_obj_create_entry(t->a, t->id, &origin->stamps);
    if (!w) {
        return FACADE_E_NO_SPACE;
    }
    /* Metadata-only clone: borrow the origin's extent map (same blocks, no data
     * copy), inherit retention and size. */
    facade_extent_map_destroy(w->map);
    w->map                = facade_extent_map_clone(origin->map, 1 /* force borrow */);
    w->retention_until_ns = origin->retention_until_ns;
    w->retention_mode     = origin->retention_mode;
    w->size               = origin->size;
    return FACADE_OK;
} /* facade_prep_clone */

SYMBOL_EXPORT void
facade_obj_clone_working(
    struct facade_alloc            *a,
    const uint8_t                   sealed_id[16],
    const struct chimera_sc_obj_id *new_id,
    facade_epoch_cb_t               cb,
    void                           *private_data)
{
    struct facade_obj     *origin = facade_obj_find(a, sealed_id);
    struct facade_obj_txn *t;

    if (!origin) {
        cb(FACADE_E_NOT_FOUND, a->power_loss_generation, private_data);
        return;
    }
    if (origin->state != CHIMERA_SC_OBJ_SEALED) {
        cb(FACADE_E_STATE, a->power_loss_generation, private_data);
        return;
    }
    t                    = facade_txn_new(a, new_id->b, 2u, facade_prep_clone, cb, private_data);
    t->target_may_be_new = 1;   /* t->id is the new working-version id (CR-3 exempt) */
    memcpy(t->u.clone.sealed_id, sealed_id, 16);
    t->u.clone.new_id = *new_id;
    facade_txn_submit(t);
} /* facade_obj_clone_working */

/* ---- OBJ_FLUSH (write-through prototype: already durable) ---- */

SYMBOL_EXPORT void
facade_obj_flush(
    struct facade_alloc *a,
    const uint8_t        id[16],
    facade_epoch_cb_t    cb,
    void                *private_data)
{
    struct facade_obj *o = facade_obj_find(a, id);

    if (!o) {
        cb(FACADE_E_NOT_FOUND, a->power_loss_generation, private_data);
        return;
    }
    /* Durable writes went straight through; nothing is buffered to force. */
    cb(FACADE_OK, a->power_loss_generation, private_data);
} /* facade_obj_flush */

/* ---- OBJ_READ ---- */

struct facade_read_ctx {
    struct facade_alloc *a;
    facade_obj_read_cb_t cb;
    void                *priv;
    void                *out;
    uint32_t             boff;
    uint32_t             len;
    uint64_t             obj_size;
    void                *block;   /* 4 KiB temp for a DATA read */
};

static void
facade_read_done(
    enum facade_status st,
    void              *priv)
{
    struct facade_read_ctx *c    = priv;
    facade_obj_read_cb_t    cb   = c->cb;
    void                   *cp   = c->priv;
    uint32_t                len  = c->len;
    uint64_t                size = c->obj_size;

    if (st == FACADE_OK) {
        memcpy(c->out, (uint8_t *) c->block + c->boff, c->len);
    }
    free(c->block);
    free(c);
    cb(st, st == FACADE_OK ? len : 0u, size, cp);
} /* facade_read_done */

SYMBOL_EXPORT void
facade_obj_read(
    struct facade_alloc *a,
    const uint8_t        id[16],
    uint64_t             offset,
    uint32_t             len,
    void                *out,
    facade_obj_read_cb_t cb,
    void                *private_data)
{
    struct facade_obj      *o    = facade_obj_find(a, id);
    uint32_t                boff = (uint32_t) (offset & (CHIMERA_MEDIA_BLOCK_SIZE - 1));
    uint64_t                lb;
    uint64_t                avail;
    struct facade_extent    cur;
    struct facade_read_ctx *c;

    if (!o) {
        cb(FACADE_E_NOT_FOUND, 0, 0, private_data);
        return;
    }
    if ((uint64_t) boff + len > CHIMERA_MEDIA_BLOCK_SIZE) {
        cb(FACADE_E_INVAL, 0, o->size, private_data);   /* single-block reads */
        return;
    }
    /* Clamp to EOF: bytes past the logical size do not exist. */
    avail = offset < o->size ? o->size - offset : 0;
    if ((uint64_t) len > avail) {
        len = (uint32_t) avail;
    }
    if (len == 0) {
        cb(FACADE_OK, 0, o->size, private_data);
        return;
    }
    lb = offset >> CHIMERA_MEDIA_BLOCK_SHIFT;

    if (facade_extent_map_lookup(o->map, lb, &cur) && cur.kind == CHIMERA_SC_SEG_DATA) {
        chimera_media_block_t phys = cur.phys_block + (lb - cur.logical_block);

        c = calloc(1, sizeof(*c));
        FACADE_CHECK(c != NULL, "oom read ctx");
        c->a        = a;
        c->cb       = cb;
        c->priv     = private_data;
        c->out      = out;
        c->boff     = boff;
        c->len      = len;
        c->obj_size = o->size;
        c->block    = calloc(1, CHIMERA_MEDIA_BLOCK_SIZE);
        FACADE_CHECK(c->block != NULL, "oom read block");
        facade_media_read(a, c->block, phys, 1, facade_read_done, c);
        return;
    }
    /* Hole or UNWRITTEN: reads as zeros. */
    memset(out, 0, len);
    cb(FACADE_OK, len, o->size, private_data);
} /* facade_obj_read */

/* ---- OBJ_STAT (5.1) ---- */

SYMBOL_EXPORT int
facade_obj_stat(
    struct facade_alloc        *a,
    const uint8_t               id[16],
    struct chimera_sc_obj_meta *m)
{
    struct facade_obj *o = facade_obj_find(a, id);

    if (!o) {
        return -1;
    }
    memset(m, 0, sizeof(*m));
    memcpy(m->id.b, o->id, 16);
    m->state            = o->state;
    m->protection_class = 0;
    m->size             = o->size;
    m->allocated_bytes  = facade_extent_map_owned_blocks(o->map)
        << CHIMERA_MEDIA_BLOCK_SHIFT;
    m->checksum.algorithm = CHIMERA_SC_CHECKSUM_NONE;
    m->checksum.len       = 0;
    m->stamps             = o->stamps;
    m->retention.until_ns = o->retention_until_ns;
    m->retention.mode     = o->retention_mode;
    m->spoke_marker       = 0;
    m->change             = o->change;
    m->refs               = 0; /* bindings are namespace-record-store's */
    m->held               = (o->flags & FACADE_OBJ_HELD) ? 1u : 0u;
    return 0;
} /* facade_obj_stat */

/* ---- OBJ_MAP (S3, O12) ---- */

SYMBOL_EXPORT int
facade_obj_map(
    struct facade_alloc       *a,
    const uint8_t              id[16],
    uint64_t                   offset,
    uint64_t                   length,
    struct chimera_sc_segment *segs,
    uint32_t                   segs_cap,
    uint32_t                  *r_nsegs,
    uint32_t                  *r_more)
{
    struct facade_obj *o = facade_obj_find(a, id);
    uint64_t           lb, nblk;

    if (!o) {
        return -1;
    }
    lb       = offset >> CHIMERA_MEDIA_BLOCK_SHIFT;
    nblk     = ((offset + length + CHIMERA_MEDIA_BLOCK_SIZE - 1) >> CHIMERA_MEDIA_BLOCK_SHIFT) - lb;
    *r_nsegs = facade_extent_map_segments(o->map, lb, nblk, segs, segs_cap, r_more);
    return 0;
} /* facade_obj_map */

/* ---- object release (discard / epoch-held): the compaction path ---- */

struct facade_release_ctx {
    struct facade_alloc *a;
    facade_cb_t          cb;
    void                *priv;
    uint8_t              id[16];    /* discard: the single object to release */
    int                  is_discard; /* 1 = discard one; 0 = epoch-held sweep */
};

/* Free an object's owned blocks to the free set and remove it (in memory). */
static void
facade_release_free_cb(
    chimera_media_block_t phys,
    uint32_t              nblocks,
    void                 *private_data)
{
    struct facade_alloc *a = private_data;

    facade_alloc_free(a, phys, nblocks);
} /* facade_release_free_cb */

static void
facade_release_one(
    struct facade_alloc *a,
    struct facade_obj   *o)
{
    /* Punch the whole map so owned blocks go back to the free set; borrowed
    * blocks (a clone's) are dropped without freeing (origin keeps them). */
    uint64_t max_blk = facade_extent_map_max_logical(o->map);

    if (max_blk > 0) {
        facade_extent_map_punch(o->map, 0, (uint32_t) max_blk,
                                facade_release_free_cb, a);
    }
    facade_obj_remove(a, o);
} /* facade_release_one */

/* Forced compaction completed: report and release the FIFO slot. */
static void
facade_release_compacted(
    enum facade_status st,
    void              *priv)
{
    struct facade_release_ctx *c  = priv;
    struct facade_alloc       *a  = c->a;
    facade_cb_t                cb = c->cb;
    void                      *cp = c->priv;

    free(c);
    cb(st, cp);
    facade_txn_release(a);
} /* facade_release_compacted */

/*
 * FIFO starter (CR-2): the in-memory free + remove runs HERE, inside the held
 * transaction slot -- NOT at dispatch time -- so no allocation (another write's
 * facade_txn_alloc) can intervene between a block returning to the free set and
 * the release's compaction being durable.  A block freed here is handed out
 * again only by a later txn, which cannot start until this starter's compaction
 * completes and releases the slot (no reuse of a freed block until its FREE --
 * here, the compaction that records it -- is durable).  Re-validate under the
 * slot, since the object's state may have changed while this release was queued.
 */
static void
facade_release_start(void *arg)
{
    struct facade_release_ctx *c        = arg;
    struct facade_alloc       *a        = c->a;
    int                        released = 0;
    enum facade_status         skip_st  = FACADE_OK;

    if (c->is_discard) {
        struct facade_obj *o = facade_obj_find(a, c->id);

        if (!o) {
            skip_st = FACADE_E_NOT_FOUND;                  /* gone while queued */
        } else if (o->state != CHIMERA_SC_OBJ_OPEN || (o->flags & FACADE_OBJ_HELD)) {
            skip_st = FACADE_E_STATE;                      /* sealed/held while queued */
        } else {
            facade_release_one(a, o);
            released = 1;
        }
    } else {
        uint32_t i, n = facade_obj_slot_count(a);

        for (i = 0; i < n; i++) {
            struct facade_obj *o = facade_obj_at(a, i);

            if (o && o->state == CHIMERA_SC_OBJ_OPEN && !(o->flags & FACADE_OBJ_HELD)) {
                facade_release_one(a, o);
                released = 1;
            }
        }
    }

    if (released) {
        facade_compact(a, facade_release_compacted, c);   /* makes the frees durable */
    } else {
        /* Nothing released.  A discard whose target changed while it was queued
         * reports the refusal (E_STATE / E_NOT_FOUND) rather than a false OK; an
         * epoch-held sweep that found nothing completes OK. */
        facade_release_compacted(skip_st, c);
    }
} /* facade_release_start */

SYMBOL_EXPORT void
facade_obj_discard(
    struct facade_alloc *a,
    const uint8_t        id[16],
    facade_cb_t          cb,
    void                *private_data)
{
    struct facade_obj         *o = facade_obj_find(a, id);
    struct facade_release_ctx *c;

    /* Fast-fail validation without taking the slot; the actual free + remove is
     * re-validated and performed inside facade_release_start (CR-2). */
    if (!o) {
        cb(FACADE_E_NOT_FOUND, private_data);
        return;
    }
    if (o->state != CHIMERA_SC_OBJ_OPEN || (o->flags & FACADE_OBJ_HELD)) {
        cb(FACADE_E_STATE, private_data);   /* sealed/held: not epoch-held */
        return;
    }
    c = calloc(1, sizeof(*c));
    FACADE_CHECK(c != NULL, "oom discard ctx");
    c->a          = a;
    c->cb         = cb;
    c->priv       = private_data;
    c->is_discard = 1;
    memcpy(c->id, id, 16);
    facade_txn_acquire(a, facade_release_start, c);
} /* facade_obj_discard */

SYMBOL_EXPORT void
facade_obj_release_epoch_held(
    struct facade_alloc *a,
    facade_cb_t          cb,
    void                *private_data)
{
    struct facade_release_ctx *c = calloc(1, sizeof(*c));

    FACADE_CHECK(c != NULL, "oom epoch-release ctx");
    c->a          = a;
    c->cb         = cb;
    c->priv       = private_data;
    c->is_discard = 0;   /* sweep every OPEN && !HELD object inside the slot */
    facade_txn_acquire(a, facade_release_start, c);
} /* facade_obj_release_epoch_held */
