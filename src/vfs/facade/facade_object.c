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
#include "common/crypto.h"   /* chimera_crypto_hash_* (MD5 object content hash) */
#include "facade_internal.h"
#include "facade_log.h"
#include "facade_object.h"
#include "media_types.h"
#include "sc_request.h"   /* CHIMERA_SC_SEG_*, struct chimera_sc_segment */
#include "sc_types.h"     /* CHIMERA_SC_CHECKSUM_MD5 */

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
    int                       is_seal_txn; /* 1 = the SEAL txn; exempt from the
                                            * prepare-chokepoint o->sealing reject
                                            * (it is the op that set the flag) */
    uint64_t                  seal_change; /* o->change captured at the seal barrier;
                                            * prep_seal fails if it moved */
    uint8_t                   seal_csum[16]; /* the content MD5 to stamp at SEAL */
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
    /* seal rollback: the object content hash + the in-memory sealing flag (so a
    * failed seal txn restores an OPEN object with checksum NONE, never MD5). */
    uint8_t                   saved_obj_csum[16];
    uint32_t                  saved_obj_csum_algo, saved_obj_csum_len;
    uint8_t                   saved_sealing;

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
            uint32_t pre_csum;      /* expected checksum of the RMW pre-read block */
            uint32_t pre_csum_algo; /* enum facade_data_csum_algo (0 = no pre-read) */
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
            memcpy(o->obj_csum, t->saved_obj_csum, 16);
            o->obj_csum_algo = t->saved_obj_csum_algo;
            o->obj_csum_len  = t->saved_obj_csum_len;
            o->sealing       = t->saved_sealing;
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
        memcpy(t->saved_obj_csum, o->obj_csum, 16);
        t->saved_obj_csum_algo = o->obj_csum_algo;
        t->saved_obj_csum_len  = o->obj_csum_len;
        t->saved_sealing       = o->sealing;
    }

    /* Freeze chokepoint (data-verification): once a SEAL has set o->sealing, no
     * other mutator may change the object's content between the hash and the
     * seal commit.  A mutator that passed its entry check before the flag was set
     * (including one parked in an async RMW read) fails HERE, before prepare
     * mutates the live map -- so the seal hashes a stable image.  The seal txn
     * itself is exempt (it is the op that set the flag). */
    if (o && o->sealing && !t->is_seal_txn) {
        facade_txn_fail(t, FACADE_E_STATE);
        return;
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

    /* Per-block data checksum (v5): crc32 over the exact 4 KiB block that is
     * FUA'd to media (staging is zero-padded for a sub-block write), so the
     * verify-on-read path (P2) can detect a corrupted stored block. */
    facade_extent_map_insert(o->map, lb, 1, p, CHIMERA_SC_SEG_DATA, 1u,
                             facade_crc32(t->u.write.staging, CHIMERA_MEDIA_BLOCK_SIZE),
                             FACADE_DATA_CSUM_CRC32, facade_txn_on_freed, t);
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

    /* The pre-read block must verify before it is merged: a corrupted existing
     * block cannot be the base of a copy-on-write.  A verify failure is dropped
     * exactly like a pre-read I/O failure -- free the bufs and call the user cb;
     * the txn has not been submitted, so the FIFO slot is NOT held and
     * facade_txn_fail (which would release it) must NOT be called. */
    /* This callback runs only after a DATA-block pre-read, which must carry a
     * usable (CRC32) checksum; an un-checksummed or mismatched pre-read block is
     * unrecoverable -- fail CLOSED (never silently merge it, H1). */
    if (st == FACADE_OK &&
        (t->u.write.pre_csum_algo != FACADE_DATA_CSUM_CRC32 ||
         facade_crc32(t->u.write.staging, CHIMERA_MEDIA_BLOCK_SIZE) != t->u.write.pre_csum)) {
        st = FACADE_E_DATA_UNRECOVERABLE;
    }
    if (st != FACADE_OK) {
        /* The pre-read failed (I/O or verification); report through the write cb
         * and drop the txn. */
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
    if (o->state != CHIMERA_SC_OBJ_OPEN || o->sealing) {
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
         * merges it); stash it in data_buf for that step.  The pre-read block is
         * verified there (v5) so a sub-block overwrite cannot launder corruption
         * into the freshly-checksummed merged block. */
        t->data_buf              = data;
        t->u.write.pre_csum      = cur.data_csum;
        t->u.write.pre_csum_algo = cur.csum_algo;
        facade_media_read(a, t->u.write.staging, phys, 1,
                          facade_write_after_read, t);
        return;
    }
    memcpy(t->u.write.staging + boff, data, len);
    facade_txn_submit(t);
} /* facade_obj_write */

/* ---- OBJ_SEAL (object content MD5, three-step freeze; data-verification) ----
 *
 * SEAL computes an MD5 over the object's content and persists it in the durable
 * object record (for OBJ_STAT / the S3 single-part ETag).  It is a three-step
 * operation on the evpl thread:
 *
 *   Step 0  freeze: set o->sealing synchronously at dispatch, so every mutator
 *           class refuses the object -- the entry rejects, the prepare chokepoint
 *           (facade_txn_prepare_phase), and the NS-txn ns_evaluate reject.
 *   Step 1  quiesce: take one brief FIFO-slot barrier; every pre-seal mutator has
 *           drained and every later one is frozen out, so the DATA extent list is
 *           final.  Snapshot it (metadata only) + o->change, then release.
 *   Step 2  hash: an async callback chain over the SNAPSHOT -- read a block,
 *           verify its checksum, crypto_update, read the next; a corrupt block
 *           aborts SEAL DATA_UNRECOVERABLE.  Media completions drive it; there is
 *           NO blocking wait (that would deadlock crash-sim/loopback).
 *   Step 3  commit: a seal txn stamps the digest and flips OPEN->SEALED, with the
 *           hash rechecked against o->change under the slot and full rollback.
 *
 * o->sealing is in-memory only (never serialized) and is cleared on EVERY exit:
 * a failed seal leaves the object OPEN and writable.
 */

struct facade_seal_ext {
    uint64_t              logical;
    chimera_media_block_t phys;
    uint32_t              data_csum;
    uint32_t              csum_algo;
};

struct facade_seal_ctx {
    struct facade_alloc        *a;
    uint8_t                     id[16];
    facade_epoch_cb_t           cb;
    void                       *priv;
    uint64_t                    seal_change; /* o->change captured under the barrier */
    uint64_t                    size;        /* content length at the barrier */
    struct chimera_crypto_hash *md5;
    void                       *block;       /* 4 KiB read buffer */
    struct facade_seal_ext      exts[FACADE_OBJ_MAX_EXT]; /* DATA snapshot, logical order */
    uint32_t                    nexts;
    uint32_t                    cur;
    uint8_t                     digest[16];
};

static void facade_seal_hash_next(
    struct facade_seal_ctx *c);

/* facade_extent_map_foreach callback: collect the DATA extents (logical order). */
static void
facade_seal_collect_cb(
    const struct facade_extent *rec,
    void                       *priv)
{
    struct facade_seal_ctx *c = priv;

    if (rec->kind != CHIMERA_SC_SEG_DATA) {
        return;   /* holes/UNWRITTEN read as zeros; not fetched from media */
    }
    FACADE_CHECK(c->nexts < FACADE_OBJ_MAX_EXT, "seal snapshot extent overflow");
    c->exts[c->nexts].logical   = rec->logical_block;
    c->exts[c->nexts].phys      = rec->phys_block;
    c->exts[c->nexts].data_csum = rec->data_csum;
    c->exts[c->nexts].csum_algo = rec->csum_algo;
    c->nexts++;
} /* facade_seal_collect_cb */

/* Step 3 prepare: recheck the content did not move, then stamp + flip. */
static uint32_t
facade_prep_seal(struct facade_obj_txn *t)
{
    struct facade_obj *o = facade_obj_find(t->a, t->id);

    /* (i) the content hashed under the freeze must still describe this object
     * (the freeze guarantees it; the recheck is the belt-and-suspenders the plan
     * requires, and it must run BEFORE the o->change++ below). */
    if (o->change != t->seal_change) {
        return FACADE_E_STATE;
    }
    /* (ii) stamp the content hash; (iii) flip OPEN -> SEALED. */
    memcpy(o->obj_csum, t->seal_csum, 16);
    o->obj_csum_algo = CHIMERA_SC_CHECKSUM_MD5;
    o->obj_csum_len  = 16;
    o->state         = CHIMERA_SC_OBJ_SEALED;
    o->change++;
    return FACADE_OK;
} /* facade_prep_seal */

/* Completion shim wrapping the seal txn's user callback: clear o->sealing on
 * EVERY exit (success, change-mismatch, or append/commit failure via
 * facade_txn_fail) so a failed seal leaves an OPEN, writable object. */
static void
facade_seal_txn_done(
    enum facade_status st,
    uint64_t           epoch,
    void              *priv)
{
    struct facade_seal_ctx *c  = priv;
    struct facade_obj      *o  = facade_obj_find(c->a, c->id);
    facade_epoch_cb_t       cb = c->cb;
    void                   *cp = c->priv;

    if (o) {
        o->sealing = 0;
    }
    free(c->block);
    free(c);
    cb(st, epoch, cp);
} /* facade_seal_txn_done */

/* Step-2 abort (before the seal txn is queued): clear the freeze and report. */
static void
facade_seal_abort(
    struct facade_seal_ctx *c,
    enum facade_status      st)
{
    struct facade_alloc *a  = c->a;
    struct facade_obj   *o  = facade_obj_find(a, c->id);
    facade_epoch_cb_t    cb = c->cb;
    void                *cp = c->priv;
    uint64_t             g  = a->power_loss_generation;

    if (o) {
        o->sealing = 0;
    }
    if (c->md5) {
        chimera_crypto_hash_free(c->md5);
    }
    free(c->block);
    free(c);
    cb(st, g, cp);
} /* facade_seal_abort */

/* Step 3: queue the seal txn carrying the digest + the captured change. */
static void
facade_seal_submit_txn(struct facade_seal_ctx *c)
{
    struct facade_obj_txn *t = facade_txn_new(c->a, c->id, 2u, facade_prep_seal,
                                              facade_seal_txn_done, c);

    t->is_seal_txn = 1;
    t->seal_change = c->seal_change;
    memcpy(t->seal_csum, c->digest, 16);
    facade_txn_submit(t);
} /* facade_seal_submit_txn */

static void
facade_seal_finalize(struct facade_seal_ctx *c)
{
    int rc = chimera_crypto_final(c->md5, c->digest, sizeof(c->digest));

    FACADE_CHECK(rc == 1, "md5 final failed");
    chimera_crypto_hash_free(c->md5);
    c->md5 = NULL;
    facade_seal_submit_txn(c);
} /* facade_seal_finalize */

static void
facade_seal_read_done(
    enum facade_status st,
    void              *priv)
{
    struct facade_seal_ctx *c = priv;
    uint64_t                off, n;

    if (st != FACADE_OK) {
        facade_seal_abort(c, st);
        return;
    }
    /* Verify the snapshotted block before it enters the hash: a corrupt block
     * must not be sealed into a published content hash. */
    if (c->exts[c->cur].csum_algo != FACADE_DATA_CSUM_CRC32 ||
        facade_crc32(c->block, CHIMERA_MEDIA_BLOCK_SIZE) != c->exts[c->cur].data_csum) {
        facade_seal_abort(c, FACADE_E_DATA_UNRECOVERABLE);
        return;
    }
    /* Hash only the content bytes of this block (the last block is trimmed to the
     * object size; a full-block-written object hashes exactly its content). */
    off = c->exts[c->cur].logical << CHIMERA_MEDIA_BLOCK_SHIFT;
    n   = (off < c->size)
          ? ((c->size - off < CHIMERA_MEDIA_BLOCK_SIZE) ? (c->size - off)
                                                        : CHIMERA_MEDIA_BLOCK_SIZE)
          : 0;
    if (n) {
        chimera_crypto_update(c->md5, c->block, (size_t) n);
    }
    c->cur++;
    facade_seal_hash_next(c);
} /* facade_seal_read_done */

static void
facade_seal_hash_next(struct facade_seal_ctx *c)
{
    if (c->cur >= c->nexts) {
        facade_seal_finalize(c);
        return;
    }
    facade_media_read(c->a, c->block, c->exts[c->cur].phys, 1,
                      facade_seal_read_done, c);
} /* facade_seal_hash_next */

/* Step 1: runs under the FIFO slot (quiesced).  Snapshot + release, then hash. */
static void
facade_seal_barrier(void *arg)
{
    struct facade_seal_ctx *c = arg;
    struct facade_alloc    *a = c->a;
    struct facade_obj      *o = facade_obj_find(a, c->id);

    if (!o) {
        /* An in-flight discard that was already past its re-validation when the
         * seal dispatched may have removed the object; abort cleanly. */
        facade_txn_release(a);
        facade_seal_abort(c, FACADE_E_NOT_FOUND);
        return;
    }
    c->seal_change = o->change;
    c->size        = o->size;
    c->nexts       = 0;
    facade_extent_map_foreach(o->map, facade_seal_collect_cb, c);
    facade_txn_release(a);   /* metadata-only snapshot: no I/O was done under the slot */

    /* Step 2: hash the snapshot, media-completion-driven. */
    c->md5 = chimera_crypto_hash_new(CHIMERA_CRYPTO_MD5, NULL, 0, NULL, 0);
    FACADE_CHECK(c->md5 != NULL, "md5 ctx alloc");
    c->block = calloc(1, CHIMERA_MEDIA_BLOCK_SIZE);
    FACADE_CHECK(c->block != NULL, "oom seal block");
    c->cur = 0;
    facade_seal_hash_next(c);
} /* facade_seal_barrier */

SYMBOL_EXPORT void
facade_obj_seal(
    struct facade_alloc *a,
    const uint8_t        id[16],
    facade_epoch_cb_t    cb,
    void                *private_data)
{
    struct facade_obj      *o = facade_obj_find(a, id);
    struct facade_seal_ctx *c;

    if (!o) {
        cb(FACADE_E_NOT_FOUND, a->power_loss_generation, private_data);
        return;
    }
    if (o->state != CHIMERA_SC_OBJ_OPEN || o->sealing) {
        cb(FACADE_E_STATE, a->power_loss_generation, private_data);
        return;
    }

    /* Step 0: freeze synchronously.  From here every object-op entry reject, the
     * prepare chokepoint, and the NS-txn ns_evaluate reject refuse this object. */
    o->sealing = 1;

    c = calloc(1, sizeof(*c));
    FACADE_CHECK(c != NULL, "oom seal ctx");
    c->a = a;
    memcpy(c->id, id, 16);
    c->cb   = cb;
    c->priv = private_data;

    /* Step 1: quiesce under one brief FIFO-slot barrier. */
    facade_txn_acquire(a, facade_seal_barrier, c);
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
    if (o->state != CHIMERA_SC_OBJ_OPEN || o->sealing) {
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
    /* UNWRITTEN blocks read as zeros from the map and never touch media, so
     * they carry no stored-data checksum (NONE). */
    facade_extent_map_insert(o->map, lb, nblk, p, CHIMERA_SC_SEG_UNWRITTEN, 1u,
                             0u, FACADE_DATA_CSUM_NONE, facade_txn_on_freed, t);
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
    if (o->state != CHIMERA_SC_OBJ_OPEN || o->sealing) {
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
    if (o->state != CHIMERA_SC_OBJ_OPEN || o->sealing) {
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
    void                *block;      /* 4 KiB temp for a DATA read */
    uint32_t             data_csum;  /* expected per-block checksum (v5) */
    uint32_t             csum_algo;  /* enum facade_data_csum_algo */
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
        /* Verify the stored block against its per-block checksum BEFORE any byte
         * reaches the caller.  This site is reached only for a DATA extent, which
         * must carry a usable (CRC32) checksum; an un-checksummed or mismatched
         * DATA block is unrecoverable -- fail CLOSED (never silently skip, H1) and
         * never return the bad bytes (c->out is left untouched; only the temp
         * block and ctx are freed). */
        if (c->csum_algo != FACADE_DATA_CSUM_CRC32 ||
            facade_crc32(c->block, CHIMERA_MEDIA_BLOCK_SIZE) != c->data_csum) {
            st = FACADE_E_DATA_UNRECOVERABLE;
        } else {
            memcpy(c->out, (uint8_t *) c->block + c->boff, c->len);
        }
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
        c->a         = a;
        c->cb        = cb;
        c->priv      = private_data;
        c->out       = out;
        c->boff      = boff;
        c->len       = len;
        c->obj_size  = o->size;
        c->data_csum = cur.data_csum;
        c->csum_algo = cur.csum_algo;
        c->block     = calloc(1, CHIMERA_MEDIA_BLOCK_SIZE);
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
    /* Object content hash (5.1): reported once SEALED; NONE while open or mid-seal
     * (the digest is stamped only by the seal txn's commit). */
    if (o->state == CHIMERA_SC_OBJ_SEALED && o->obj_csum_len > 0) {
        m->checksum.algorithm = o->obj_csum_algo;
        m->checksum.len       = o->obj_csum_len;
        memcpy(m->checksum.bytes, o->obj_csum, o->obj_csum_len);
    } else {
        m->checksum.algorithm = CHIMERA_SC_CHECKSUM_NONE;
        m->checksum.len       = 0;
    }
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
        } else if (o->state != CHIMERA_SC_OBJ_OPEN || (o->flags & FACADE_OBJ_HELD) ||
                   o->sealing) {
            skip_st = FACADE_E_STATE;                      /* sealed/held/sealing while queued */
        } else {
            facade_release_one(a, o);
            released = 1;
        }
    } else {
        uint32_t i, n = facade_obj_slot_count(a);

        for (i = 0; i < n; i++) {
            struct facade_obj *o = facade_obj_at(a, i);

            /* Skip a sealing object (data-verification, H1): the epoch-held sweep
             * must NOT punch+free the blocks the in-flight seal hash is reading,
             * or Step 3 would commit against a freed id.  The freeze defers the
             * release -- once the seal commits the object is SEALED (no longer
             * epoch-held), so it is correctly retained; a later sweep leaves it. */
            if (o && o->state == CHIMERA_SC_OBJ_OPEN && !(o->flags & FACADE_OBJ_HELD) &&
                !o->sealing) {
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
    if (o->state != CHIMERA_SC_OBJ_OPEN || (o->flags & FACADE_OBJ_HELD) || o->sealing) {
        cb(FACADE_E_STATE, private_data);   /* sealed/held/sealing: not epoch-held */
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

/* ---- SCRUB (data-verification): verify every stored DATA block ---- */

struct facade_scrub_ent {
    uint8_t               obj_id[16];
    uint64_t              logical;
    chimera_media_block_t phys;
    uint32_t              data_csum;
    uint32_t              csum_algo;
    uint64_t              reuse_stamp;   /* snapshot of the block's reuse stamp */
};

struct facade_scrub_ctx {
    struct facade_alloc     *a;
    facade_scrub_report_cb   report;
    facade_scrub_done_cb     done;
    void                    *priv;
    struct facade_scrub_ent *ents;       /* object-major snapshot (heap) */
    uint32_t                 nents;
    uint32_t                 cur;
    uint32_t                 nbad;
    void                    *block;      /* 4 KiB read buffer */
    const uint8_t           *collect_id; /* object id while collecting its extents */
    uint8_t                  cur_obj[16];
    int                      have_cur_obj;
    int                      cur_obj_bad; /* already reported the current object */
};

static void facade_scrub_verify_next(
    struct facade_scrub_ctx *c);

/* facade_extent_map_foreach callback: snapshot one DATA extent (filter BEFORE any
 * read -- UNWRITTEN/HOLE never reach media).  DATA extents are one block. */
static void
facade_scrub_collect_cb(
    const struct facade_extent *rec,
    void                       *priv)
{
    struct facade_scrub_ctx *c = priv;
    struct facade_scrub_ent *e;

    if (rec->kind != CHIMERA_SC_SEG_DATA) {
        return;
    }
    e = &c->ents[c->nents++];
    memcpy(e->obj_id, c->collect_id, 16);
    e->logical     = rec->logical_block;
    e->phys        = rec->phys_block;
    e->data_csum   = rec->data_csum;
    e->csum_algo   = rec->csum_algo;
    e->reuse_stamp = facade_reuse_stamp(c->a, rec->phys_block);
} /* facade_scrub_collect_cb */

static void
facade_scrub_finish(struct facade_scrub_ctx *c)
{
    facade_scrub_done_cb done = c->done;
    void                *priv = c->priv;
    uint32_t             nbad = c->nbad;

    free(c->ents);
    free(c->block);
    free(c);
    done(FACADE_OK, nbad, priv);
} /* facade_scrub_finish */

static void
facade_scrub_read_done(
    enum facade_status st,
    void              *priv)
{
    struct facade_scrub_ctx *c = priv;
    struct facade_scrub_ent *e = &c->ents[c->cur];

    /* A new object starts: reset the per-object "already reported" latch. */
    if (!c->have_cur_obj || memcmp(c->cur_obj, e->obj_id, 16) != 0) {
        memcpy(c->cur_obj, e->obj_id, 16);
        c->have_cur_obj = 1;
        c->cur_obj_bad  = 0;
    }

    if (st == FACADE_OK && !c->cur_obj_bad) {
        /* Recheck the reuse stamp AFTER the read: if it changed, the block was
         * freed/realloced since the snapshot and no longer holds this object's
         * content -- skip it, never a false DATA_UNRECOVERABLE. */
        if (facade_reuse_stamp(c->a, e->phys) == e->reuse_stamp &&
            (e->csum_algo != FACADE_DATA_CSUM_CRC32 ||
             facade_crc32(c->block, CHIMERA_MEDIA_BLOCK_SIZE) != e->data_csum)) {
            c->report(e->obj_id, e->logical, c->priv);   /* first bad block */
            c->cur_obj_bad = 1;
            c->nbad++;
        }
    }
    /* A transient media read error is not a verification failure; skip it. */
    c->cur++;
    facade_scrub_verify_next(c);
} /* facade_scrub_read_done */

static void
facade_scrub_verify_next(struct facade_scrub_ctx *c)
{
    while (c->cur < c->nents) {
        struct facade_scrub_ent *e = &c->ents[c->cur];

        /* Once an object is reported, skip its remaining extents (first bad
         * block is enough) without any further reads. */
        if (c->have_cur_obj && c->cur_obj_bad &&
            memcmp(c->cur_obj, e->obj_id, 16) == 0) {
            c->cur++;
            continue;
        }
        facade_media_read(c->a, c->block, e->phys, 1, facade_scrub_read_done, c);
        return;
    }
    facade_scrub_finish(c);
} /* facade_scrub_verify_next */

/* Runs under the FIFO slot: snapshot the DATA extents (metadata only, no I/O),
 * release the slot, then verify out of the slot. */
static void
facade_scrub_start(void *arg)
{
    struct facade_scrub_ctx *c = arg;
    struct facade_alloc     *a = c->a;
    uint32_t                 i, n = facade_obj_slot_count(a);
    uint64_t                 cap = 0;

    for (i = 0; i < n; i++) {
        struct facade_obj *o = facade_obj_at(a, i);

        if (o) {
            cap += facade_extent_map_count(o->map);   /* upper bound (DATA+UNWRITTEN) */
        }
    }
    c->ents = cap ? calloc((size_t) cap, sizeof(*c->ents)) : NULL;
    FACADE_CHECK(cap == 0 || c->ents != NULL, "oom scrub snapshot");
    c->nents = 0;
    for (i = 0; i < n; i++) {
        struct facade_obj *o = facade_obj_at(a, i);

        if (o) {
            c->collect_id = o->id;
            facade_extent_map_foreach(o->map, facade_scrub_collect_cb, c);
        }
    }

    facade_txn_release(a);   /* no media I/O was done under the slot */

    c->cur   = 0;
    c->block = calloc(1, CHIMERA_MEDIA_BLOCK_SIZE);
    FACADE_CHECK(c->block != NULL, "oom scrub block");
    facade_scrub_verify_next(c);
} /* facade_scrub_start */

SYMBOL_EXPORT void
facade_scrub(
    struct facade_alloc   *a,
    facade_scrub_report_cb report,
    facade_scrub_done_cb   done,
    void                  *private_data)
{
    struct facade_scrub_ctx *c = calloc(1, sizeof(*c));

    FACADE_CHECK(c != NULL, "oom scrub ctx");
    c->a      = a;
    c->report = report;
    c->done   = done;
    c->priv   = private_data;
    facade_txn_acquire(a, facade_scrub_start, c);
} /* facade_scrub */
