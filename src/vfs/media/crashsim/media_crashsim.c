// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * The crash-simulating media provider.  See chimera_media_crashsim.h for the
 * contract.
 *
 * Layout.  One crash-sim struct chimera_media_space wraps one delegate space.
 * Non-durable writes (fua == 0) are copied into a journal of records kept in
 * submission order; they are NOT written to the delegate until a flush or a
 * crash drains them.  Force-unit-access writes are written through to the
 * delegate immediately (durable) and also recorded in the journal (marked
 * durable) so a read still sees them layered newest-over-oldest.  Reads issue a
 * delegate read, then overlay journal records oldest-to-newest, so the newest
 * write to each block wins.
 *
 * Completion.  Validation failures complete synchronously (a caller bug, per
 * the media contract); a staged write that passed validation completes
 * asynchronously through an evpl deferral, so nothing reaches the caller inline
 * from the submitting call.  The control ops (crash/corrupt), which are test
 * actions driven at the top level, pump the loop internally and are
 * synchronous; the vtable flush is asynchronous and chains delegate writes.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "evpl/evpl.h"

#include "common/macros.h"
#include "chimera_media_crashsim.h"
#include "media_crashsim_rng.h"

/* A teardown-time invariant that must survive NDEBUG: a loud abort, never a
 * silent use-after-free.  (The full in-flight quiesce is the follow-on task
 * crashsim-inflight-op-teardown-quiesce; this only catches the convention
 * being broken.) */
#define CRASHSIM_GUARD(cond, msg)                                        \
        do {                                                             \
            if (!(cond)) {                                               \
                fprintf(stderr, "crashsim: %s (%s:%d)\n",                \
                        (msg), __FILE__, __LINE__);                      \
                abort();                                                 \
            }                                                            \
        } while (0)

/* ---- state ---- */

/* Per-record crash outcome, drawn by the policy and consumed by the drain.
 * Kept separate from `durable` so the two never conflate. */
enum crashsim_outcome {
    CRASHSIM_KEEP = 0,   /* survived intact */
    CRASHSIM_DROP,       /* lost */
    CRASHSIM_TEAR,       /* survived only partially */
};

/* One staged (or written-through) write, retained until the next durability
 * point (flush or crash) clears the journal. */
struct crashsim_wrec {
    uint64_t              seq;      /* submission order */
    chimera_media_block_t lba;
    uint32_t              nblocks;
    uint8_t               durable;  /* 1 => already in the delegate (fua) */
    uint8_t               outcome;  /* enum crashsim_outcome; crash-drain scratch */
    void                 *bytes;    /* nblocks << CHIMERA_MEDIA_BLOCK_SHIFT */
    struct crashsim_wrec *prev;     /* older */
    struct crashsim_wrec *next;     /* newer */
};

/* A pending asynchronous completion, drained by the deferral. */
struct crashsim_pending {
    chimera_media_io_cb_t    cb;
    void                    *private_data;
    enum chimera_media_error status;
    struct crashsim_pending *next;
};

struct chimera_media_space {
    struct chimera_media_crashsim       *handle;
    struct evpl                         *evpl;
    const struct chimera_media_provider *delegate;
    struct chimera_media_space          *dspace;        /* delegate's space */

    uint64_t                             nblocks;
    uint32_t                             max_io_blocks;

    struct crashsim_wrec                *j_head;        /* oldest */
    struct crashsim_wrec                *j_tail;        /* newest */
    uint64_t                             next_seq;

    struct crashsim_rng                  rng;
    struct chimera_media_crashsim_stats  stats;

    struct crashsim_pending             *p_head;
    struct crashsim_pending             *p_tail;
    struct evpl_deferral                 deferral;

    /* Outstanding asynchronous delegate operations started by the vtable
     * (read / FUA write / flush).  The drain-before-teardown convention
     * requires this to be zero at close() and crash(); a guard enforces it. */
    int                                  inflight;
};

struct chimera_media_crashsim {
    struct chimera_media_crashsim_config cfg;
};

/* ---- asynchronous completion via the deferral ---- */

static void
crashsim_pending_drain(
    struct evpl *evpl,
    void        *private_data)
{
    struct chimera_media_space *space = private_data;
    struct crashsim_pending    *p     = space->p_head;
    struct crashsim_pending    *next;

    (void) evpl;

    /* Detach the batch first: a completion that submits a new op enqueues onto
     * a fresh list and re-arms the deferral, so it runs on a later pass. */
    space->p_head = NULL;
    space->p_tail = NULL;

    while (p) {
        next = p->next;
        p->cb(p->status, p->private_data);
        free(p);
        p = next;
    }
} /* crashsim_pending_drain */

static void
crashsim_defer_complete(
    struct chimera_media_space *space,
    chimera_media_io_cb_t       cb,
    void                       *private_data,
    enum chimera_media_error    status)
{
    struct crashsim_pending *p = malloc(sizeof(*p));

    if (!p) {
        /* Submission-time OOM: the media contract allows a synchronous
         * completion here (it is not a media failure). */
        cb(status, private_data);
        return;
    }
    p->cb           = cb;
    p->private_data = private_data;
    p->status       = status;
    p->next         = NULL;

    if (space->p_tail) {
        space->p_tail->next = p;
    } else {
        space->p_head = p;
    }
    space->p_tail = p;

    evpl_defer(space->evpl, &space->deferral);
} /* crashsim_defer_complete */

/* A completion that does nothing: handed to the delegate on teardown paths that
 * have no caller to notify, so the delegate (which calls its callback
 * unconditionally) never dereferences a NULL cb. */
static void
crashsim_noop_io_cb(
    enum chimera_media_error status,
    void                    *private_data)
{
    (void) status;
    (void) private_data;
} /* crashsim_noop_io_cb */

/* ---- iovec helpers (always whole 4 KiB blocks) ---- */

/* Copy `len` bytes out of the iovec chain (from offset 0) into dst. */
static void
crashsim_iov_copy_out(
    void                    *dst,
    const struct evpl_iovec *iov,
    int                      niov,
    uint64_t                 len)
{
    uint8_t *d = dst;
    int      i;

    for (i = 0; i < niov && len > 0; i++) {
        uint64_t seg = evpl_iovec_length(&iov[i]);

        if (seg > len) {
            seg = len;
        }
        memcpy(d, evpl_iovec_data(&iov[i]), seg);
        d   += seg;
        len -= seg;
    }
} /* crashsim_iov_copy_out */

/* Copy `len` bytes from src into the iovec chain starting at byte_off. */
static void
crashsim_iov_copy_in(
    const struct evpl_iovec *iov,
    int                      niov,
    uint64_t                 byte_off,
    const void              *src,
    uint64_t                 len)
{
    const uint8_t *s = src;
    int            i;

    for (i = 0; i < niov && len > 0; i++) {
        uint64_t seg = evpl_iovec_length(&iov[i]);
        uint64_t here;

        if (byte_off >= seg) {
            byte_off -= seg;
            continue;
        }
        here = seg - byte_off;
        if (here > len) {
            here = len;
        }
        memcpy((uint8_t *) evpl_iovec_data(&iov[i]) + byte_off, s, here);
        s       += here;
        len     -= here;
        byte_off = 0;
    }
} /* crashsim_iov_copy_in */

/* ---- argument validation (mirrors the loopback provider) ---- */

static enum chimera_media_error
crashsim_check_io(
    const struct chimera_media_space *space,
    int                               niov,
    const struct evpl_iovec          *iov,
    chimera_media_block_t             lba,
    uint32_t                          nblocks)
{
    uint64_t total = 0;
    int i;

    if (!iov || niov <= 0 || nblocks == 0 || nblocks > space->max_io_blocks) {
        return CHIMERA_MEDIA_E_INVAL;
    }
    /* Overflow-safe range check, before the length check so a range error is
     * reported as E_RANGE rather than masked by a length mismatch. */
    if (nblocks > space->nblocks || lba > space->nblocks - nblocks) {
        return CHIMERA_MEDIA_E_RANGE;
    }
    for (i = 0; i < niov; i++) {
        total += evpl_iovec_length(&iov[i]);
    }
    if (total != ((uint64_t) nblocks << CHIMERA_MEDIA_BLOCK_SHIFT)) {
        return CHIMERA_MEDIA_E_INVAL;
    }
    return CHIMERA_MEDIA_OK;
} /* crashsim_check_io */

/* ---- journal ---- */

static struct crashsim_wrec *
crashsim_journal_append(
    struct chimera_media_space *space,
    chimera_media_block_t       lba,
    uint32_t                    nblocks,
    int                         durable)
{
    struct crashsim_wrec *rec = malloc(sizeof(*rec));

    if (!rec) {
        return NULL;
    }
    rec->bytes = malloc((size_t) nblocks << CHIMERA_MEDIA_BLOCK_SHIFT);
    if (!rec->bytes) {
        free(rec);
        return NULL;
    }
    rec->seq     = space->next_seq++;
    rec->lba     = lba;
    rec->nblocks = nblocks;
    rec->durable = (uint8_t) (durable ? 1 : 0);
    rec->next    = NULL;
    rec->prev    = space->j_tail;
    if (space->j_tail) {
        space->j_tail->next = rec;
    } else {
        space->j_head = rec;
    }
    space->j_tail = rec;
    return rec;
} /* crashsim_journal_append */

static void
crashsim_journal_clear(struct chimera_media_space *space)
{
    struct crashsim_wrec *rec = space->j_head;
    struct crashsim_wrec *next;

    while (rec) {
        next = rec->next;
        free(rec->bytes);
        free(rec);
        rec = next;
    }
    space->j_head               = NULL;
    space->j_tail               = NULL;
    space->stats.staged_current = 0;
} /* crashsim_journal_clear */

/* ---- read: delegate read, then overlay the journal oldest-to-newest ---- */

struct crashsim_read_ctx {
    struct chimera_media_space *space;
    struct evpl_iovec          *iov;
    int                         niov;
    chimera_media_block_t       lba;
    uint32_t                    nblocks;
    chimera_media_io_cb_t       cb;
    void                       *private_data;
};

static void
crashsim_read_done(
    enum chimera_media_error status,
    void                    *private_data)
{
    struct crashsim_read_ctx   *rc    = private_data;
    struct chimera_media_space *space = rc->space;
    struct crashsim_wrec       *rec;

    space->inflight--;

    if (status != CHIMERA_MEDIA_OK) {
        rc->cb(status, rc->private_data);
        free(rc);
        return;
    }

    /* Overlay every overlapping record, oldest first, so the newest write to a
     * block ends up on top (newest wins) and a durable write-through correctly
     * masks an older staged write to the same block. */
    for (rec = space->j_head; rec; rec = rec->next) {
        uint64_t ov_start = rec->lba > rc->lba ? rec->lba : rc->lba;
        uint64_t rec_end  = rec->lba + rec->nblocks;
        uint64_t req_end  = rc->lba + rc->nblocks;
        uint64_t ov_end   = rec_end < req_end ? rec_end : req_end;

        if (ov_start >= ov_end) {
            continue;
        }
        crashsim_iov_copy_in(
            rc->iov, rc->niov,
            (ov_start - rc->lba) << CHIMERA_MEDIA_BLOCK_SHIFT,
                (const uint8_t *) rec->bytes
                + ((ov_start - rec->lba) << CHIMERA_MEDIA_BLOCK_SHIFT),
                (ov_end - ov_start) << CHIMERA_MEDIA_BLOCK_SHIFT);
    }

    rc->cb(CHIMERA_MEDIA_OK, rc->private_data);
    free(rc);
} /* crashsim_read_done */

static void
crashsim_read(
    struct chimera_media_space *space,
    struct evpl_iovec          *iov,
    int                         niov,
    chimera_media_block_t       lba,
    uint32_t                    nblocks,
    chimera_media_io_cb_t       cb,
    void                       *private_data)
{
    enum chimera_media_error  err;
    struct crashsim_read_ctx *rc;

    err = crashsim_check_io(space, niov, iov, lba, nblocks);
    if (err != CHIMERA_MEDIA_OK) {
        cb(err, private_data);
        return;
    }

    rc = malloc(sizeof(*rc));
    if (!rc) {
        cb(CHIMERA_MEDIA_E_IO, private_data);
        return;
    }
    rc->space        = space;
    rc->iov          = iov;
    rc->niov         = niov;
    rc->lba          = lba;
    rc->nblocks      = nblocks;
    rc->cb           = cb;
    rc->private_data = private_data;

    space->inflight++;
    space->delegate->read(space->dspace, iov, niov, lba, nblocks,
                          crashsim_read_done, rc);
} /* crashsim_read */

/* ---- write ---- */

/* Context for a force-unit-access write-through to the delegate. */
struct crashsim_fua_ctx {
    struct chimera_media_space *space;
    struct crashsim_wrec       *rec;
    chimera_media_io_cb_t       cb;
    void                       *private_data;
};

static void
crashsim_fua_done(
    enum chimera_media_error status,
    void                    *private_data)
{
    struct crashsim_fua_ctx *fc = private_data;

    fc->space->inflight--;

    /* The record is durable only if the delegate actually persisted it; on
     * failure it stays non-durable (and counted as staged) so a later crash
     * can still drop it. */
    if (status == CHIMERA_MEDIA_OK) {
        fc->rec->durable = 1;
        if (fc->space->stats.staged_current > 0) {
            fc->space->stats.staged_current--;
        }
    }
    fc->cb(status, fc->private_data);
    free(fc);
} /* crashsim_fua_done */

static void
crashsim_write(
    struct chimera_media_space *space,
    const struct evpl_iovec    *iov,
    int                         niov,
    chimera_media_block_t       lba,
    uint32_t                    nblocks,
    int                         fua,
    chimera_media_io_cb_t       cb,
    void                       *private_data)
{
    enum chimera_media_error err;
    struct crashsim_wrec    *rec;

    err = crashsim_check_io(space, niov, iov, lba, nblocks);
    if (err != CHIMERA_MEDIA_OK) {
        cb(err, private_data);
        return;
    }

    /* A new record is not durable yet; a FUA write becomes durable only when
     * the delegate confirms (crashsim_fua_done). */
    rec = crashsim_journal_append(space, lba, nblocks, 0);
    if (!rec) {
        cb(CHIMERA_MEDIA_E_IO, private_data);
        return;
    }
    crashsim_iov_copy_out(rec->bytes, iov, niov,
                          (uint64_t) nblocks << CHIMERA_MEDIA_BLOCK_SHIFT);
    space->stats.writes_total++;

    if (fua) {
        /* Write through to the delegate now; the record becomes durable when
         * the delegate completes.  The delegate completion is itself async. */
        struct crashsim_fua_ctx *fc = malloc(sizeof(*fc));

        if (!fc) {
            /* Keep the record staged; report OOM synchronously. */
            space->stats.staged_current++;
            cb(CHIMERA_MEDIA_E_IO, private_data);
            return;
        }
        fc->space        = space;
        fc->rec          = rec;
        fc->cb           = cb;
        fc->private_data = private_data;
        /* Counted as staged until the delegate confirms durability. */
        space->stats.staged_current++;
        space->inflight++;
        space->delegate->write(space->dspace, iov, niov, lba, nblocks,
                               1 /* sync */, crashsim_fua_done, fc);
        return;
    }

    /* Buffered: stage only, do not touch the delegate.  Complete async. */
    space->stats.staged_current++;
    crashsim_defer_complete(space, cb, private_data, CHIMERA_MEDIA_OK);
} /* crashsim_write */

/* ---- flush: chain a one-block-at-a-time drain of the journal ---- */

struct crashsim_flush_ctx {
    struct chimera_media_space *space;
    struct crashsim_wrec       *rec;        /* current record being drained */
    uint32_t                    blk;        /* block index within rec */
    struct evpl_iovec           iov;
    int                         iov_valid;
    enum chimera_media_error status;
    chimera_media_io_cb_t       cb;
    void                       *private_data;
};

static void crashsim_flush_step(
    struct crashsim_flush_ctx *fc);

static void
crashsim_flush_flushed(
    enum chimera_media_error status,
    void                    *private_data)
{
    struct crashsim_flush_ctx  *fc    = private_data;
    struct chimera_media_space *space = fc->space;
    chimera_media_io_cb_t       cb    = fc->cb;
    void                       *priv  = fc->private_data;
    enum chimera_media_error    st    = fc->status;

    if (st == CHIMERA_MEDIA_OK) {
        st = status;
    }
    /* Everything prior is now durable: clear the whole journal and bump epoch. */
    crashsim_journal_clear(space);
    space->inflight--;
    free(fc);
    cb(st, priv);
} /* crashsim_flush_flushed */

static void
crashsim_flush_wrote(
    enum chimera_media_error status,
    void                    *private_data)
{
    struct crashsim_flush_ctx *fc = private_data;

    if (fc->iov_valid) {
        evpl_iovec_release(fc->space->evpl, &fc->iov);
        fc->iov_valid = 0;
    }
    if (status != CHIMERA_MEDIA_OK && fc->status == CHIMERA_MEDIA_OK) {
        fc->status = status;
    }
    fc->blk++;
    if (fc->blk >= fc->rec->nblocks) {
        fc->rec = fc->rec->next;
        fc->blk = 0;
    }
    crashsim_flush_step(fc);
} /* crashsim_flush_wrote */

static void
crashsim_flush_step(struct crashsim_flush_ctx *fc)
{
    struct chimera_media_space *space = fc->space;
    int                         n;

    if (!fc->rec) {
        space->delegate->flush(space->dspace, crashsim_flush_flushed, fc);
        return;
    }

    n = evpl_iovec_alloc(space->evpl, CHIMERA_MEDIA_BLOCK_SIZE,
                         CHIMERA_MEDIA_BLOCK_SIZE, 1, 0, &fc->iov);
    if (n != 1) {
        /* Could not get a buffer: stop draining and flush what landed. */
        if (fc->status == CHIMERA_MEDIA_OK) {
            fc->status = CHIMERA_MEDIA_E_IO;
        }
        space->delegate->flush(space->dspace, crashsim_flush_flushed, fc);
        return;
    }
    fc->iov_valid = 1;
    memcpy(evpl_iovec_data(&fc->iov),
           (const uint8_t *) fc->rec->bytes
           + ((uint64_t) fc->blk << CHIMERA_MEDIA_BLOCK_SHIFT),
           CHIMERA_MEDIA_BLOCK_SIZE);

    space->delegate->write(space->dspace, &fc->iov, 1,
                           fc->rec->lba + fc->blk, 1, 1 /* sync */,
                           crashsim_flush_wrote, fc);
} /* crashsim_flush_step */

static void
crashsim_flush(
    struct chimera_media_space *space,
    chimera_media_io_cb_t       cb,
    void                       *private_data)
{
    struct crashsim_flush_ctx *fc = malloc(sizeof(*fc));

    if (!fc) {
        cb(CHIMERA_MEDIA_E_IO, private_data);
        return;
    }
    fc->space = space;
    /* Drain EVERY record in submission order, not just the staged ones: a
     * durable (FUA) record is re-asserted at its submission position so an
     * OLDER staged write to the same block, drained before it, cannot clobber
     * the newer durable value (global newest-wins across the durable
     * boundary). */
    fc->rec          = space->j_head;
    fc->blk          = 0;
    fc->iov_valid    = 0;
    fc->status       = CHIMERA_MEDIA_OK;
    fc->cb           = cb;
    fc->private_data = private_data;

    space->inflight++;
    crashsim_flush_step(fc);
} /* crashsim_flush */

/* ---- synchronous helpers for the control ops (pump the loop) ---- */

struct crashsim_sync_io {
    int done;
    enum chimera_media_error status;
};

static void
crashsim_sync_cb(
    enum chimera_media_error status,
    void                    *private_data)
{
    struct crashsim_sync_io *w = private_data;

    w->status = status;
    w->done   = 1;
} /* crashsim_sync_cb */

static enum chimera_media_error
crashsim_sync_read_block(
    struct chimera_media_space *space,
    chimera_media_block_t       lba,
    void                       *buf)
{
    struct evpl_iovec       iov;
    struct crashsim_sync_io w = { 0, CHIMERA_MEDIA_E_IO };
    int n;

    n = evpl_iovec_alloc(space->evpl, CHIMERA_MEDIA_BLOCK_SIZE,
                         CHIMERA_MEDIA_BLOCK_SIZE, 1, 0, &iov);
    if (n != 1) {
        return CHIMERA_MEDIA_E_IO;
    }
    space->delegate->read(space->dspace, &iov, 1, lba, 1,
                          crashsim_sync_cb, &w);
    while (!w.done) {
        evpl_continue(space->evpl);
    }
    if (w.status == CHIMERA_MEDIA_OK) {
        memcpy(buf, evpl_iovec_data(&iov), CHIMERA_MEDIA_BLOCK_SIZE);
    }
    evpl_iovec_release(space->evpl, &iov);
    return w.status;
} /* crashsim_sync_read_block */

static enum chimera_media_error
crashsim_sync_write_block(
    struct chimera_media_space *space,
    chimera_media_block_t       lba,
    const void                 *buf)
{
    struct evpl_iovec       iov;
    struct crashsim_sync_io w = { 0, CHIMERA_MEDIA_E_IO };
    int n;

    n = evpl_iovec_alloc(space->evpl, CHIMERA_MEDIA_BLOCK_SIZE,
                         CHIMERA_MEDIA_BLOCK_SIZE, 1, 0, &iov);
    if (n != 1) {
        return CHIMERA_MEDIA_E_IO;
    }
    memcpy(evpl_iovec_data(&iov), buf, CHIMERA_MEDIA_BLOCK_SIZE);
    space->delegate->write(space->dspace, &iov, 1, lba, 1, 1 /* sync */,
                           crashsim_sync_cb, &w);
    while (!w.done) {
        evpl_continue(space->evpl);
    }
    evpl_iovec_release(space->evpl, &iov);
    return w.status;
} /* crashsim_sync_write_block */

static enum chimera_media_error
crashsim_sync_flush(struct chimera_media_space *space)
{
    struct crashsim_sync_io w = { 0, CHIMERA_MEDIA_E_IO };

    space->delegate->flush(space->dspace, crashsim_sync_cb, &w);
    while (!w.done) {
        evpl_continue(space->evpl);
    }
    return w.status;
} /* crashsim_sync_flush */

/* ---- control: crash / recover / corrupt / reseed / stats ---- */

/* Draw one per-write outcome from the policy weights. */
static enum crashsim_outcome
crashsim_draw_outcome(
    struct chimera_media_space *space,
    uint32_t                    nblocks)
{
    const struct chimera_media_crashsim_config *cfg = &space->handle->cfg;
    uint32_t keep = cfg->w_keep;
    uint32_t drop = cfg->w_drop;
    uint32_t tear = cfg->w_tear;
    uint64_t sum;
    uint64_t r;

    if (keep == 0 && drop == 0 && tear == 0) {
        /* Default: KEEP-biased, occasional DROP, no tear. */
        keep = 3;
        drop = 1;
        tear = 0;
    }
    /* A single block cannot be torn at block granularity; fold tear into drop. */
    if (nblocks < 2) {
        drop += tear;
        tear  = 0;
    }
    sum = (uint64_t) keep + drop + tear;
    r   = crashsim_rng_below(&space->rng, sum);
    if (r < keep) {
        return CRASHSIM_KEEP;
    }
    if (r < (uint64_t) keep + drop) {
        return CRASHSIM_DROP;
    }
    return CRASHSIM_TEAR;
} /* crashsim_draw_outcome */

/* Drain a KEPT record to the delegate, whole. */
static void
crashsim_apply_keep(
    struct chimera_media_space *space,
    struct crashsim_wrec       *rec)
{
    uint32_t b;

    for (b = 0; b < rec->nblocks; b++) {
        crashsim_sync_write_block(
            space, rec->lba + b,
            (const uint8_t *) rec->bytes + ((uint64_t) b << CHIMERA_MEDIA_BLOCK_SHIFT));
    }
} /* crashsim_apply_keep */

/* Drain a TORN record: a number of leading blocks land whole, then one block
 * lands partially (first `t` bytes new, the rest the delegate's prior content),
 * and the remaining blocks are lost -- a torn write across the power cut. */
static void
crashsim_apply_tear(
    struct chimera_media_space *space,
    struct crashsim_wrec       *rec)
{
    uint8_t  blk[CHIMERA_MEDIA_BLOCK_SIZE];
    uint32_t full = (uint32_t) crashsim_rng_below(&space->rng, rec->nblocks);
    uint32_t t    = 1 + (uint32_t) crashsim_rng_below(&space->rng,
                                                      CHIMERA_MEDIA_BLOCK_SIZE - 1);
    uint32_t b;

    for (b = 0; b < full; b++) {
        crashsim_sync_write_block(
            space, rec->lba + b,
            (const uint8_t *) rec->bytes + ((uint64_t) b << CHIMERA_MEDIA_BLOCK_SHIFT));
    }
    /* Partial block `full`: new prefix over the prior stored content. */
    if (crashsim_sync_read_block(space, rec->lba + full, blk) == CHIMERA_MEDIA_OK) {
        memcpy(blk,
               (const uint8_t *) rec->bytes + ((uint64_t) full << CHIMERA_MEDIA_BLOCK_SHIFT),
               t);
        crashsim_sync_write_block(space, rec->lba + full, blk);
    }
} /* crashsim_apply_tear */

static int
crashsim_ctl_crash(struct chimera_media_space *space)
{
    struct crashsim_wrec *rec;

    /* The drain-before-teardown convention: no asynchronous vtable op (read,
     * FUA write, flush) may be in flight when a crash is injected. */
    CRASHSIM_GUARD(space->inflight == 0,
                   "crash() with delegate ops in flight");

    space->stats.last_crash_kept    = 0;
    space->stats.last_crash_dropped = 0;
    space->stats.last_crash_torn    = 0;

    /* Draw an outcome for each not-yet-durable record, in submission order, so
     * the policy is deterministic for a (seed, trace) pair.  Durable (FUA)
     * records always survive (D5) and carry no drawn outcome. */
    for (rec = space->j_head; rec; rec = rec->next) {
        if (rec->durable) {
            continue;
        }
        rec->outcome = (uint8_t) crashsim_draw_outcome(space, rec->nblocks);
    }

    /* Apply the surviving writes in strict submission order, re-asserting the
     * durable records at their own position: the newest writer of each block
     * lands last, so a durable write is never clobbered by an older staged
     * write to the same (or overlapping) blocks (global newest-wins across the
     * durable boundary).  Dropped records leave a gap; torn records land
     * partially. */
    for (rec = space->j_head; rec; rec = rec->next) {
        if (rec->durable) {
            crashsim_apply_keep(space, rec);   /* re-assert the durable value */
            continue;
        }
        switch ((enum crashsim_outcome) rec->outcome) {
            case CRASHSIM_KEEP:
                crashsim_apply_keep(space, rec);
                space->stats.last_crash_kept++;
                break;
            case CRASHSIM_TEAR:
                crashsim_apply_tear(space, rec);
                space->stats.last_crash_torn++;
                break;
            case CRASHSIM_DROP:
            default:
                space->stats.last_crash_dropped++;
                break;
        } /* switch */
    }

    crashsim_sync_flush(space);
    crashsim_journal_clear(space);
    return 0;
} /* crashsim_ctl_crash */

static int
crashsim_ctl_recover(struct chimera_media_space *space)
{
    /* The loopback delegate's file persists across the simulated power cycle,
     * so recovery is a logical reset: staging was already cleared by crash(). */
    (void) space;
    return 0;
} /* crashsim_ctl_recover */

static int
crashsim_ctl_corrupt(
    struct chimera_media_space *space,
    chimera_media_block_t       lba,
    uint32_t                    nblocks)
{
    uint8_t  blk[CHIMERA_MEDIA_BLOCK_SIZE];
    uint32_t b;

    if (nblocks == 0 || nblocks > space->nblocks ||
        lba > space->nblocks - nblocks) {
        return -1;
    }
    for (b = 0; b < nblocks; b++) {
        uint64_t pos;

        if (crashsim_sync_read_block(space, lba + b, blk) != CHIMERA_MEDIA_OK) {
            return -1;
        }
        pos = crashsim_rng_below(&space->rng,
                                 (uint64_t) CHIMERA_MEDIA_BLOCK_SIZE * 8);
        blk[pos >> 3] ^= (uint8_t) (1u << (pos & 7));
        if (crashsim_sync_write_block(space, lba + b, blk) != CHIMERA_MEDIA_OK) {
            return -1;
        }
    }
    crashsim_sync_flush(space);
    return 0;
} /* crashsim_ctl_corrupt */

static void
crashsim_ctl_reseed(
    struct chimera_media_space *space,
    uint64_t                    seed)
{
    crashsim_rng_seed(&space->rng, seed);
    space->stats.last_crash_kept    = 0;
    space->stats.last_crash_dropped = 0;
    space->stats.last_crash_torn    = 0;
} /* crashsim_ctl_reseed */

static void
crashsim_ctl_stats(
    struct chimera_media_space          *space,
    struct chimera_media_crashsim_stats *out)
{
    *out = space->stats;
} /* crashsim_ctl_stats */

static const struct chimera_media_crashsim_control crashsim_control = {
    .crash   = crashsim_ctl_crash,
    .recover = crashsim_ctl_recover,
    .corrupt = crashsim_ctl_corrupt,
    .reseed  = crashsim_ctl_reseed,
    .stats   = crashsim_ctl_stats,
};

/* ---- open / close / geometry ---- */

static void
crashsim_close_done(
    enum chimera_media_error status,
    void                    *private_data);

struct crashsim_close_ctx {
    struct chimera_media_space *space;
    chimera_media_io_cb_t       cb;
    void                       *private_data;
};

static void
crashsim_close(
    struct chimera_media_space *space,
    chimera_media_io_cb_t       cb,
    void                       *private_data)
{
    struct crashsim_close_ctx *cc;
    struct crashsim_pending   *p;
    struct crashsim_pending   *pnext;

    /* No asynchronous vtable op (read / FUA write / flush) may be in flight at
     * close; the caller must have drained them first. */
    CRASHSIM_GUARD(space->inflight == 0,
                   "close() with delegate ops in flight");

    /* Fire any still-queued staged-write completions BEFORE disarming the
     * deferral -- evpl_remove_deferral only disarms, it does not run them, so
     * dropping them here would leak the nodes and silently lose the
     * completions.  The buffered writes were accepted (OK); close then discards
     * the buffered data, which was never flushed. */
    p             = space->p_head;
    space->p_head = NULL;
    space->p_tail = NULL;
    while (p) {
        pnext = p->next;
        p->cb(CHIMERA_MEDIA_OK, p->private_data);
        free(p);
        p = pnext;
    }
    evpl_remove_deferral(space->evpl, &space->deferral);

    /* Close drops buffered state (it is not a durability point). */
    crashsim_journal_clear(space);

    cc = malloc(sizeof(*cc));
    if (!cc) {
        /* Best effort: close the delegate fire-and-forget (a no-op completion,
         * never a NULL cb, which the delegate would call unconditionally), free
         * now. */
        space->delegate->close(space->dspace, crashsim_noop_io_cb, NULL);
        free(space);
        cb(CHIMERA_MEDIA_E_IO, private_data);
        return;
    }
    cc->space        = space;
    cc->cb           = cb;
    cc->private_data = private_data;
    space->delegate->close(space->dspace, crashsim_close_done, cc);
} /* crashsim_close */

static void
crashsim_close_done(
    enum chimera_media_error status,
    void                    *private_data)
{
    struct crashsim_close_ctx *cc   = private_data;
    chimera_media_io_cb_t      cb   = cc->cb;
    void                      *priv = cc->private_data;

    free(cc->space);
    free(cc);
    cb(status, priv);
} /* crashsim_close_done */

static uint64_t
crashsim_size_bytes(struct chimera_media_space *space)
{
    return space->delegate->size_bytes(space->dspace);
} /* crashsim_size_bytes */

static void
crashsim_info(
    struct chimera_media_space      *space,
    struct chimera_media_space_info *out)
{
    space->delegate->info(space->dspace, out);
} /* crashsim_info */

static const struct chimera_media_provider crashsim_provider = {
    .abi_version = CHIMERA_MEDIA_ABI_VERSION,
    .reserved    = 0,
    .name        = "crashsim",
    .open        = NULL,   /* opened through chimera_media_crashsim_open */
    .read        = crashsim_read,
    .write       = crashsim_write,
    .flush       = crashsim_flush,
    .close       = crashsim_close,
    .size_bytes  = crashsim_size_bytes,
    .info        = crashsim_info,
};

/* ---- factory + typed open ---- */

struct crashsim_open_ctx {
    struct chimera_media_crashsim *handle;
    struct evpl                   *evpl;
    chimera_media_open_cb_t        cb;
    void                          *private_data;
};

static void
crashsim_delegate_opened(
    enum chimera_media_error    status,
    struct chimera_media_space *dspace,
    void                       *private_data)
{
    struct crashsim_open_ctx       *oc = private_data;
    struct chimera_media_space     *space;
    struct chimera_media_space_info info;

    if (status != CHIMERA_MEDIA_OK || !dspace) {
        oc->cb(status != CHIMERA_MEDIA_OK ? status : CHIMERA_MEDIA_E_NODEV,
               NULL, oc->private_data);
        free(oc);
        return;
    }

    space = calloc(1, sizeof(*space));
    if (!space) {
        /* A no-op completion, never a NULL cb (the delegate calls it
         * unconditionally). */
        oc->handle->cfg.delegate->close(dspace, crashsim_noop_io_cb, NULL);
        oc->cb(CHIMERA_MEDIA_E_NODEV, NULL, oc->private_data);
        free(oc);
        return;
    }

    space->handle   = oc->handle;
    space->evpl     = oc->evpl;
    space->delegate = oc->handle->cfg.delegate;
    space->dspace   = dspace;

    space->delegate->info(dspace, &info);
    space->nblocks       = info.nblocks;
    space->max_io_blocks = info.max_io_blocks;

    crashsim_rng_seed(&space->rng, oc->handle->cfg.seed);
    evpl_deferral_init(&space->deferral, crashsim_pending_drain, space);

    oc->cb(CHIMERA_MEDIA_OK, space, oc->private_data);
    free(oc);
} /* crashsim_delegate_opened */

SYMBOL_EXPORT void
chimera_media_crashsim_open(
    struct chimera_media_crashsim *handle,
    struct evpl                   *evpl,
    const char                    *uri,
    uint64_t                       min_size,
    struct prometheus_metrics     *metrics,
    chimera_media_open_cb_t        cb,
    void                          *private_data)
{
    struct crashsim_open_ctx *oc;
    enum chimera_media_error  err;

    oc = malloc(sizeof(*oc));
    if (!oc) {
        cb(CHIMERA_MEDIA_E_NODEV, NULL, private_data);
        return;
    }
    oc->handle       = handle;
    oc->evpl         = evpl;
    oc->cb           = cb;
    oc->private_data = private_data;

    /* Open the delegate through the standard entry (which re-checks its ABI). */
    err = chimera_media_open(handle->cfg.delegate, evpl, uri, min_size, metrics,
                             crashsim_delegate_opened, oc);
    if (err != CHIMERA_MEDIA_OK) {
        /* chimera_media_open did not call the callback (ABI mismatch). */
        cb(err, NULL, private_data);
        free(oc);
    }
} /* chimera_media_crashsim_open */

SYMBOL_EXPORT int
chimera_media_crashsim_create(
    const struct chimera_media_crashsim_config   *cfg,
    struct chimera_media_crashsim               **handle_out,
    const struct chimera_media_provider         **provider_out,
    const struct chimera_media_crashsim_control **control_out)
{
    struct chimera_media_crashsim *handle;

    /* handle_out is required: the handle owns the allocation the caller must
     * later destroy, and open() needs it, so there is no use for a NULL. */
    if (!cfg || !cfg->delegate || !handle_out ||
        cfg->delegate->abi_version != CHIMERA_MEDIA_ABI_VERSION) {
        return -1;
    }

    handle = malloc(sizeof(*handle));
    if (!handle) {
        return -1;
    }
    handle->cfg = *cfg;

    *handle_out = handle;
    if (provider_out) {
        *provider_out = &crashsim_provider;
    }
    if (control_out) {
        *control_out = &crashsim_control;
    }
    return 0;
} /* chimera_media_crashsim_create */

SYMBOL_EXPORT void
chimera_media_crashsim_destroy(struct chimera_media_crashsim *handle)
{
    free(handle);
} /* chimera_media_crashsim_destroy */
