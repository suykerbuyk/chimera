// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Backend-facade durability log.  See facade_log.h for the record format and
 * the gate contract (G1 compact-at-txn-boundary, G2 commit-after-data-FUA).
 *
 * One record per 4 KiB block.  Every record is written force-unit-access, so a
 * record that recovery reads is durable; a transaction's COMMIT is the last
 * record issued, and it is issued only after the object-data FUA has completed
 * (G2), so a durable COMMIT always implies durable data + ALLOC + OBJ records.
 * Recovery (facade_recover.c) stops at the first gap and drops a torn-off tail
 * transaction whose COMMIT is absent.  The log never wraps between checkpoints;
 * if a transaction will not fit, a compaction runs first at the transaction
 * boundary -- before any in-memory allocation delta -- which is the consistent
 * cut G1 requires.
 *
 * This file links no libevpl: all media access is through the facade_media_*
 * helpers (facade_alloc.c) over the provider vtable.
 */

#include <string.h>

#include "common/macros.h"
#include "facade_internal.h"
#include "facade_log.h"
#include "media_types.h"
#include "sc_request.h"   /* CHIMERA_SC_SEG_DATA */

/* Build one log record into a zeroed 4 KiB block. */
static void
facade_lr_build(
    void               *block,
    enum facade_lr_type type,
    uint64_t            seq,
    uint64_t            txn,
    const void         *payload,
    uint32_t            len)
{
    struct facade_lr_hdr *h = block;
    uint32_t              hc;

    FACADE_CHECK(sizeof(*h) + len <= CHIMERA_MEDIA_BLOCK_SIZE, "log record too large");

    memset(block, 0, CHIMERA_MEDIA_BLOCK_SIZE);
    h->magic    = FACADE_LR_MAGIC;
    h->seq      = seq;
    h->txn      = txn;
    h->type     = (uint32_t) type;
    h->format   = FACADE_LR_FORMAT;
    h->len      = len;
    h->hdr_crc  = 0;
    h->body_crc = 0;
    h->reserved = 0;

    /* hdr_crc covers the header with BOTH crc fields zeroed (validated before
     * len is trusted); body_crc covers the payload. */
    hc = facade_crc32(h, (uint32_t) sizeof(*h));
    if (len) {
        memcpy((uint8_t *) block + sizeof(*h), payload, len);
    }
    h->body_crc = facade_crc32((uint8_t *) block + sizeof(*h), len);
    h->hdr_crc  = hc;
} /* facade_lr_build */

/* ------------------------------------------------------------------ */
/* durable object write (the {ALLOC, OBJ, COMMIT} transaction)         */
/* ------------------------------------------------------------------ */

#define FACADE_OW_RECORDS 3u   /* ALLOC + OBJ + COMMIT */

struct facade_ow_ctx {
    struct facade_alloc  *a;
    uint8_t               id[16];
    facade_epoch_cb_t     cb;
    void                 *priv;
    uint32_t              len;
    uint64_t              nblocks;   /* data blocks */
    chimera_media_block_t phys;      /* allocated data start */
    uint64_t              txn;       /* == the ALLOC record's seq */
    uint32_t              flags;     /* object flags (hold) at write time */
    void                 *data_buf;  /* nblocks * 4 KiB */
    void                 *rec_buf;   /* one 4 KiB record block, reused */

    /* rollback state for a mid-transaction failure (MEDIUM): return the just-
     * allocated blocks to the free set, and restore the object entry to its
     * pre-write state (or remove it), so a failed write never leaves a phantom
     * object / leaked blocks a later compaction could make durable. */
    int                   allocated;     /* facade_alloc_blocks succeeded */
    int                   obj_applied;   /* facade_obj_put ran this txn */
    int                   obj_preexisted;/* the entry existed before this write */
    uint32_t              prior_flags;
    uint32_t              prior_nextents;
    struct facade_objext  prior_ext[FACADE_OBJ_MAX_EXT];
    int                   txn_started;   /* the log appends began (cursors advanced) */
    uint64_t              pre_seq;        /* next_seq before this txn's appends */
    uint64_t              pre_log_next;   /* log_next before this txn's appends */
};

static void
facade_ow_fail(
    struct facade_ow_ctx *c,
    enum facade_status    st)
{
    struct facade_alloc *a  = c->a;
    facade_epoch_cb_t    cb = c->cb;
    void                *cp = c->priv;
    uint64_t             g  = a->power_loss_generation;

    /* Roll back this transaction's in-memory effects so nothing phantom is ever
     * checkpointed and no blocks leak. */
    if (c->obj_applied) {
        struct facade_obj *o = facade_obj_find(a, c->id);

        if (o) {
            if (c->obj_preexisted) {
                o->flags    = c->prior_flags;
                o->nextents = c->prior_nextents;
                memcpy(o->ext, c->prior_ext,
                       c->prior_nextents * sizeof(o->ext[0]));
            } else {
                o->used = 0;
                a->nobjs--;
            }
        }
    }
    if (c->allocated) {
        facade_alloc_free(a, c->phys, (uint32_t) c->nblocks);
    }
    /* Roll back the log cursors this (uncommitted) txn advanced, so the next
     * compaction still sees a clean transaction boundary (next_seq ==
     * durable_seq + 1).  The durable-but-uncommitted records already on media are
     * harmless: recovery drops any txn without a COMMIT, and the slots are
     * overwritten by the next append. */
    if (c->txn_started) {
        a->next_seq = c->pre_seq;
        a->log_next = c->pre_log_next;
    }

    free(c->data_buf);
    free(c->rec_buf);
    free(c);
    cb(st, g, cp);
} /* facade_ow_fail */

static void facade_ow_after_room(
    struct facade_ow_ctx *c);

static void
facade_ow_commit_done(
    enum facade_status st,
    void              *priv)
{
    struct facade_ow_ctx *c = priv;
    struct facade_alloc  *a = c->a;
    facade_epoch_cb_t     cb;
    void                 *cp;
    uint64_t              g;

    if (st != FACADE_OK) {
        facade_ow_fail(c, st);
        return;
    }
    /* The COMMIT is durable: the whole transaction is now durable. */
    a->durable_seq   = c->txn + 2;           /* ALLOC, OBJ, COMMIT seqs */
    a->durable_epoch = a->cur_epoch;

    cb = c->cb;
    cp = c->priv;
    g  = a->power_loss_generation;
    free(c->data_buf);
    free(c->rec_buf);
    free(c);
    cb(FACADE_OK, g, cp);
} /* facade_ow_commit_done */

static void
facade_ow_obj_done(
    enum facade_status st,
    void              *priv)
{
    struct facade_ow_ctx   *c = priv;
    struct facade_alloc    *a = c->a;
    struct facade_lr_commit commit;

    if (st != FACADE_OK) {
        facade_ow_fail(c, st);
        return;
    }
    if (a->shutting_down) {
        facade_ow_fail(c, FACADE_E_SHUTDOWN);
        return;
    }

    /* G2 is already satisfied: the data FUA completed before any log record was
    * even issued, so the COMMIT (issued now) is durable only after the data. */
    commit.txn      = c->txn;
    commit.nrecords = FACADE_OW_RECORDS;
    {
        uint64_t seq  = a->next_seq;
        uint64_t slot = a->log_next;

        FACADE_CHECK(slot < a->log_nblocks, "log overflow past compaction");
        a->next_seq++;
        a->log_next++;
        facade_lr_build(c->rec_buf, FACADE_LR_COMMIT, seq, c->txn,
                        &commit, (uint32_t) sizeof(commit));
        facade_media_write(a, c->rec_buf, a->log_block + slot, 1,
                           CHIMERA_MEDIA_WRITE_FUA, facade_ow_commit_done, c);
    }
} /* facade_ow_obj_done */

static void
facade_ow_alloc_done(
    enum facade_status st,
    void              *priv)
{
    struct facade_ow_ctx      *c = priv;
    struct facade_alloc       *a = c->a;
    uint8_t                    payload[sizeof(struct facade_lr_obj)
                                       + sizeof(struct facade_ckpt_objext)];
    struct facade_lr_obj      *orec = (struct facade_lr_obj *) payload;
    struct facade_ckpt_objext *xr   = (struct facade_ckpt_objext *) (payload + sizeof(*orec));
    struct facade_objext       ext;

    if (st != FACADE_OK) {
        facade_ow_fail(c, st);
        return;
    }
    if (a->shutting_down) {
        facade_ow_fail(c, FACADE_E_SHUTDOWN);
        return;
    }

    /* Update the in-memory object table (single extent at logical 0). */
    ext.logical_block = 0;
    ext.phys_block    = c->phys;
    ext.nblocks       = c->nblocks;
    ext.kind          = CHIMERA_SC_SEG_DATA;
    if (!facade_obj_put(a, c->id, c->flags, &ext, 1)) {
        facade_ow_fail(c, FACADE_E_NO_SPACE);
        return;
    }
    c->obj_applied = 1;

    memset(payload, 0, sizeof(payload));
    memcpy(orec->obj_id, c->id, 16);
    orec->flags       = c->flags;
    orec->nextents    = 1;
    xr->logical_block = 0;
    xr->phys_block    = c->phys;
    xr->nblocks       = c->nblocks;
    xr->kind          = CHIMERA_SC_SEG_DATA;
    xr->reserved      = 0;

    {
        uint64_t seq  = a->next_seq;
        uint64_t slot = a->log_next;

        FACADE_CHECK(slot < a->log_nblocks, "log overflow past compaction");
        a->next_seq++;
        a->log_next++;
        facade_lr_build(c->rec_buf, FACADE_LR_OBJ, seq, c->txn,
                        payload, (uint32_t) sizeof(payload));
        facade_media_write(a, c->rec_buf, a->log_block + slot, 1,
                           CHIMERA_MEDIA_WRITE_FUA, facade_ow_obj_done, c);
    }
} /* facade_ow_alloc_done */

static void
facade_ow_data_done(
    enum facade_status st,
    void              *priv)
{
    struct facade_ow_ctx  *c = priv;
    struct facade_alloc   *a = c->a;
    struct facade_lr_alloc arec;

    if (st != FACADE_OK) {
        facade_ow_fail(c, st);
        return;
    }
    if (a->shutting_down) {
        facade_ow_fail(c, FACADE_E_SHUTDOWN);
        return;
    }

    /* Data is durable (FUA completed).  Now log the transaction; the ALLOC seq
     * is the transaction id.  Claim the seq and log slot and advance the cursors
     * BEFORE issuing the write: a provider that completes inline runs the next
     * append nested, so the cursors must already be advanced. */
    {
        uint64_t seq  = a->next_seq;
        uint64_t slot = a->log_next;

        FACADE_CHECK(slot < a->log_nblocks, "log overflow past compaction");
        c->txn       = seq;
        arec.start   = c->phys;
        arec.nblocks = c->nblocks;
        a->next_seq++;
        a->log_next++;
        facade_lr_build(c->rec_buf, FACADE_LR_ALLOC, seq, c->txn,
                        &arec, (uint32_t) sizeof(arec));
        facade_media_write(a, c->rec_buf, a->log_block + slot, 1,
                           CHIMERA_MEDIA_WRITE_FUA, facade_ow_alloc_done, c);
    }
} /* facade_ow_data_done */

static void
facade_ow_after_room(struct facade_ow_ctx *c)
{
    struct facade_alloc  *a = c->a;
    enum facade_status    st;
    chimera_media_block_t start;

    if (a->shutting_down) {
        facade_ow_fail(c, FACADE_E_SHUTDOWN);
        return;
    }

    /* Allocate the data blocks now (after any compaction, so the compaction
     * snapshot never contained this delta -- G1). */
    st = facade_alloc_blocks(a, (uint32_t) c->nblocks, &start, NULL);
    if (st != FACADE_OK) {
        facade_ow_fail(c, st);
        return;
    }
    c->phys      = start;
    c->allocated = 1;

    /* Snapshot the log cursors (post-compaction) so a mid-txn failure can roll
     * them back to this transaction boundary. */
    c->txn_started  = 1;
    c->pre_seq      = a->next_seq;
    c->pre_log_next = a->log_next;

    /* data_buf was filled (payload at offset 0, zero-padded) before the chain
     * started, in facade_obj_write_durable. */
    facade_media_write(a, c->data_buf, c->phys, c->nblocks,
                       CHIMERA_MEDIA_WRITE_FUA, facade_ow_data_done, c);
} /* facade_ow_after_room */

/* The clean=0 dirty-mark issued after an intra-write compaction has landed; the
 * durable records this write is about to append are now covered by clean=0 on
 * media, so a crash after them runs recovery and the object is not lost. */
static void
facade_ow_remark_done(
    enum facade_status st,
    void              *priv)
{
    struct facade_ow_ctx *c = priv;

    if (st != FACADE_OK) {
        facade_ow_fail(c, st);
        return;
    }
    if (c->a->shutting_down) {
        facade_ow_fail(c, FACADE_E_SHUTDOWN);
        return;
    }
    facade_ow_after_room(c);
} /* facade_ow_remark_done */

static void
facade_ow_compacted(
    enum facade_status st,
    void              *priv)
{
    struct facade_ow_ctx *c = priv;

    if (st != FACADE_OK) {
        facade_ow_fail(c, st);
        return;
    }
    /* The compaction set clean=1 and cleared dirty_armed.  Re-mark clean=0 BEFORE
     * appending this transaction's post-compaction records, so a crash after
     * them still triggers recovery (the records live only in the reset log, not
     * the just-written checkpoint).  mark_dirty takes the block-0 slot that the
     * compaction just released, serialized through mw_busy (F1). */
    facade_mark_dirty(c->a, facade_ow_remark_done, c);
} /* facade_ow_compacted */

static void
facade_ow_dirty_done(
    enum facade_status st,
    void              *priv)
{
    struct facade_ow_ctx *c = priv;
    struct facade_alloc  *a = c->a;

    if (st != FACADE_OK) {
        facade_ow_fail(c, st);
        return;
    }
    if (a->shutting_down) {
        facade_ow_fail(c, FACADE_E_SHUTDOWN);
        return;
    }

    /* G1: if the transaction will not fit the remaining log, compact first at
    * the transaction boundary (no in-memory allocation delta applied yet). */
    if (a->log_next + FACADE_OW_RECORDS > a->log_nblocks) {
        facade_compact(a, facade_ow_compacted, c);
        return;
    }
    facade_ow_after_room(c);
} /* facade_ow_dirty_done */

SYMBOL_EXPORT void
facade_obj_write_durable(
    struct facade_alloc *a,
    const uint8_t        id[16],
    const void          *data,
    uint32_t             len,
    facade_epoch_cb_t    cb,
    void                *private_data)
{
    struct facade_ow_ctx *c;
    struct facade_obj    *o;
    uint64_t              nblocks;

    nblocks = (len + CHIMERA_MEDIA_BLOCK_SIZE - 1) >> CHIMERA_MEDIA_BLOCK_SHIFT;
    if (nblocks == 0) {
        nblocks = 1;
    }

    /* Transaction-size bound: a txn that cannot fit an empty log would deadlock
     * compaction-before-wrap.  The data blocks are written directly (not logged),
     * so only the FACADE_OW_RECORDS log records count against the log. */
    if (FACADE_OW_RECORDS > a->log_nblocks) {
        cb(FACADE_E_INVAL, a->power_loss_generation, private_data);
        return;
    }
    if (a->shutting_down) {
        cb(FACADE_E_SHUTDOWN, a->power_loss_generation, private_data);
        return;
    }

    c = calloc(1, sizeof(*c));
    FACADE_CHECK(c != NULL, "oom obj-write ctx");
    c->a = a;
    memcpy(c->id, id, 16);
    c->cb      = cb;
    c->priv    = private_data;
    c->len     = len;
    c->nblocks = nblocks;

    o        = facade_obj_find(a, id);
    c->flags = o ? o->flags : 0u;
    /* Snapshot the entry's pre-write state for rollback on a mid-txn failure. */
    c->obj_preexisted = (o != NULL);
    if (o) {
        c->prior_flags    = o->flags;
        c->prior_nextents = o->nextents;
        memcpy(c->prior_ext, o->ext, o->nextents * sizeof(o->ext[0]));
    }

    c->data_buf = calloc(nblocks, CHIMERA_MEDIA_BLOCK_SIZE);
    c->rec_buf  = calloc(1, CHIMERA_MEDIA_BLOCK_SIZE);
    FACADE_CHECK(c->data_buf && c->rec_buf, "oom obj-write buffers");
    if (len) {
        memcpy(c->data_buf, data, len);
    }

    facade_mark_dirty(a, facade_ow_dirty_done, c);
} /* facade_obj_write_durable */
