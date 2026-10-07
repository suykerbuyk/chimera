// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Backend-facade crash recovery: replay the contiguous durable prefix of the
 * log over the checkpoint baseline (facade_alloc_open calls this when the
 * superblock's clean flag is 0).  See facade_log.h for the record format.
 *
 * The walk starts at log_block (the log never wraps between checkpoints) and, in
 * log order, requires each record's magic, header CRC (validated before `len` is
 * trusted), bounds-checked body CRC, and seq == the expected next seq.  It STOPS
 * at the first record that fails any check -- a missing, torn or stale-wrapped
 * record -- and never continues past it (the H4 fix; the exact opposite of
 * diskfs's scan-and-skip).  Records of a transaction whose COMMIT did not land
 * inside the prefix are dropped (atomicity).  Then the power-loss generation is
 * bumped and a fresh clean checkpoint is written, so the recovered state is the
 * new baseline and the log is reset.
 */

#include <string.h>

#include "facade_internal.h"
#include "facade_log.h"
#include "media_types.h"

struct rec_entry {
    uint32_t             type;
    uint64_t             seq;
    uint64_t             txn;
    /* payload (only the fields the applied types need) */
    uint64_t             start; /* ALLOC/FREE */
    uint64_t             nblocks; /* ALLOC/FREE */
    uint8_t              id[16]; /* OBJ */
    uint32_t             flags; /* OBJ */
    uint32_t             nextents; /* OBJ */
    struct facade_objext ext[FACADE_OBJ_MAX_EXT];
};

struct facade_recover_ctx {
    struct facade_alloc *a;
    facade_cb_t          cb;
    void                *priv;
    void                *blk;         /* 1-block read buffer */
    uint64_t             idx;         /* current log slot (0-based from log_block) */
    uint64_t             expect_seq;  /* next expected record seq */
    struct rec_entry    *recs;        /* up to log_nblocks entries */
    uint64_t             nrecs;
};

/* Validate the record block in c->blk.  Returns 1 and fills *e if it continues
 * the durable prefix; returns 0 at the first gap. */
static int
facade_recover_parse(
    struct facade_recover_ctx *c,
    struct rec_entry          *e)
{
    struct facade_lr_hdr hdr, tmp;
    uint32_t             stored_hdr, stored_body, calc;
    const uint8_t       *payload;

    memcpy(&hdr, c->blk, sizeof(hdr));
    if (hdr.magic != FACADE_LR_MAGIC) {
        return 0;
    }
    /* Header CRC, validated before `len` is trusted. */
    tmp          = hdr;
    stored_hdr   = hdr.hdr_crc;
    stored_body  = hdr.body_crc;
    tmp.hdr_crc  = 0;
    tmp.body_crc = 0;
    calc         = facade_crc32(&tmp, (uint32_t) sizeof(tmp));
    if (calc != stored_hdr) {
        return 0;
    }
    if (hdr.format != FACADE_LR_FORMAT ||
        hdr.len > CHIMERA_MEDIA_BLOCK_SIZE - sizeof(hdr)) {
        return 0;
    }
    payload = (const uint8_t *) c->blk + sizeof(hdr);
    calc    = facade_crc32(payload, hdr.len);
    if (calc != stored_body) {
        return 0;
    }
    if (hdr.seq != c->expect_seq) {
        return 0;   /* missing / stale-wrapped record: stop at the gap */
    }

    memset(e, 0, sizeof(*e));
    e->type = hdr.type;
    e->seq  = hdr.seq;
    e->txn  = hdr.txn;

    switch (hdr.type) {
        case FACADE_LR_ALLOC:
        case FACADE_LR_FREE: {
            struct facade_lr_alloc ar;

            if (hdr.len < sizeof(ar)) {
                return 0;
            }
            memcpy(&ar, payload, sizeof(ar));
            e->start   = ar.start;
            e->nblocks = ar.nblocks;
            break;
        }
        case FACADE_LR_OBJ: {
            struct facade_lr_obj orec;
            uint32_t             k;
            const uint8_t       *xp;

            if (hdr.len < sizeof(orec)) {
                return 0;
            }
            memcpy(&orec, payload, sizeof(orec));
            if (orec.nextents > FACADE_OBJ_MAX_EXT ||
                hdr.len < sizeof(orec) + orec.nextents * sizeof(struct facade_ckpt_objext)) {
                return 0;
            }
            memcpy(e->id, orec.obj_id, 16);
            e->flags    = orec.flags;
            e->nextents = orec.nextents;
            xp          = payload + sizeof(orec);
            for (k = 0; k < orec.nextents; k++) {
                struct facade_ckpt_objext xr;

                memcpy(&xr, xp, sizeof(xr));
                xp                     += sizeof(xr);
                e->ext[k].logical_block = xr.logical_block;
                e->ext[k].phys_block    = xr.phys_block;
                e->ext[k].nblocks       = xr.nblocks;
                e->ext[k].kind          = xr.kind;
            }
            break;
        }
        case FACADE_LR_COMMIT:
            break;   /* no fields needed beyond seq/txn */
        default:
            return 0;   /* unknown record type: stop */
    } /* switch */
    return 1;
} /* facade_recover_parse */

static void
facade_recover_finish(
    struct facade_recover_ctx *c,
    enum facade_status         st)
{
    facade_cb_t cb = c->cb;
    void       *cp = c->priv;

    free(c->blk);
    free(c->recs);
    free(c);
    cb(st, cp);
} /* facade_recover_finish */

static void
facade_recover_compacted(
    enum facade_status st,
    void              *priv)
{
    facade_recover_finish(priv, st);
} /* facade_recover_compacted */

/* Apply the contiguous prefix (dropping any uncommitted tail txn), then bump the
 * power-loss generation and write a fresh clean checkpoint. */
/* Is the transaction `txn` committed within the gathered prefix (does a COMMIT
 * record naming it survive)?  Applied records belong only to committed txns, so
 * an uncommitted transaction -- the torn tail, OR one sandwiched between two
 * committed txns once the dependent units break strict serialization -- is
 * never half-applied (atomicity). */
static int
facade_txn_committed(
    const struct facade_recover_ctx *c,
    uint64_t                         txn)
{
    uint64_t i;

    for (i = 0; i < c->nrecs; i++) {
        if (c->recs[i].type == FACADE_LR_COMMIT && c->recs[i].txn == txn) {
            return 1;
        }
    }
    return 0;
} /* facade_txn_committed */

static void
facade_recover_apply(struct facade_recover_ctx *c)
{
    struct facade_alloc *a               = c->a;
    uint64_t             last_commit_seq = 0;
    int                  have_commit     = 0;
    uint64_t             i;

    /* Apply each record only if its OWN transaction's COMMIT is in the prefix;
     * skip records of an uncommitted transaction even when a later one commits. */
    for (i = 0; i < c->nrecs; i++) {
        struct rec_entry *e = &c->recs[i];

        if (e->type == FACADE_LR_COMMIT) {
            last_commit_seq = e->seq;
            have_commit     = 1;
            continue;
        }
        if (!facade_txn_committed(c, e->txn)) {
            continue;   /* uncommitted transaction: do not half-apply it */
        }
        switch (e->type) {
            case FACADE_LR_ALLOC:
                if (facade_free_remove(a, e->start, e->nblocks) != 0) {
                    facade_recover_finish(c, FACADE_E_IO);
                    return;
                }
                break;
            case FACADE_LR_FREE:
                facade_free_add(a, e->start, e->nblocks);
                break;
            case FACADE_LR_OBJ:
                if (!facade_obj_put(a, e->id, e->flags, e->ext, e->nextents)) {
                    facade_recover_finish(c, FACADE_E_IO);
                    return;
                }
                break;
            default:
                break;
        } /* switch */
    }

    if (have_commit) {
        a->durable_seq = last_commit_seq;
    }
    /* else: no committed record survived; baseline stands (durable_seq stays
     * at checkpoint_seq). */

    a->next_seq = a->durable_seq + 1;

    /* The epoch changes because a crash may have lost buffered data (D5). */
    a->power_loss_generation++;

    facade_compact(a, facade_recover_compacted, c);
} /* facade_recover_apply */

static void facade_recover_read_next(
    struct facade_recover_ctx *c);

static void
facade_recover_read_done(
    enum facade_status st,
    void              *priv)
{
    struct facade_recover_ctx *c = priv;
    struct rec_entry           e;

    if (st != FACADE_OK) {
        /* A media read error inside the log region is the end of what we can
         * trust: apply the prefix gathered so far. */
        facade_recover_apply(c);
        return;
    }
    if (!facade_recover_parse(c, &e)) {
        facade_recover_apply(c);
        return;
    }

    c->recs[c->nrecs++] = e;
    c->expect_seq++;
    c->idx++;
    if (c->idx >= c->a->log_nblocks) {
        facade_recover_apply(c);   /* log region full: nothing more to read */
        return;
    }
    facade_recover_read_next(c);
} /* facade_recover_read_done */

static void
facade_recover_read_next(struct facade_recover_ctx *c)
{
    facade_media_read(c->a, c->blk, c->a->log_block + c->idx, 1,
                      facade_recover_read_done, c);
} /* facade_recover_read_next */

void
facade_recover_replay(
    struct facade_alloc *a,
    facade_cb_t          cb,
    void                *private_data)
{
    struct facade_recover_ctx *c = calloc(1, sizeof(*c));

    FACADE_CHECK(c != NULL, "oom recover ctx");
    c->a          = a;
    c->cb         = cb;
    c->priv       = private_data;
    c->expect_seq = a->checkpoint_seq + 1;
    c->idx        = 0;
    c->nrecs      = 0;
    c->blk        = calloc(1, CHIMERA_MEDIA_BLOCK_SIZE);
    c->recs       = calloc(a->log_nblocks, sizeof(*c->recs));
    FACADE_CHECK(c->blk && c->recs, "oom recover buffers");

    facade_recover_read_next(c);
} /* facade_recover_replay */
