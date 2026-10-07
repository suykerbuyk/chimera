// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Backend-facade object identity: never-reused 128-bit ids (contract 4.1; the
 * H2 fix).  See facade_objid.h for the id shape.
 *
 * The scheme is diskfs's reserve-ahead generation allocator
 * (src/vfs/diskfs/diskfs_inode.c, diskfs_gen_alloc / diskfs_gen_extend),
 * adapted to the 128-bit id and this unit's single-media-space allocator:
 *
 *   - ids are drawn from an in-memory counter id_next;
 *   - a durable floor id_floor sits a reserve ahead, written to the
 *     superblock with force-unit-access; the IN-MEMORY floor is advanced only
 *     when that write completes, never before, so a crash can never leave
 *     id_next past the durable floor;
 *   - open resumes id_next at the durable floor, so every value that was ever
 *     issued (all < the floor at issue time) is skipped -- no reuse, even
 *     after an unclean crash.
 *
 * The floor extension is a superblock (block 0) write, so it shares the one
 * metadata-write slot with the persist path (facade_alloc.c): the two never
 * write block 0 concurrently, which keeps the durable floor monotonic (F1) and
 * lets teardown wait for an in-flight extension before freeing the allocator
 * (F2).
 *
 * FACADE_AGAIN / park reachability.  The allocator is single-threaded per
 * space, so the only way id_next reaches the floor before an extension's fua
 * write completes is a burst of more than RESERVE/2 draws within a single
 * event-loop turn (e.g. a large multi-object transaction created back to back
 * before the loop can run the completion).  It is unlikely but reachable, and
 * parking is the only safe response: failing the draw would turn a legitimate
 * burst into an error, and issuing past the durable floor would reintroduce
 * exactly the reuse H2 is about.  The park is drained when the extension lands.
 */

#include <string.h>

#include "evpl/evpl.h"   /* struct evpl_iovec (type only; not linked) */

#include "facade_internal.h"
#include "media_provider.h"

/* The in-flight floor-extension write carries its own superblock buffer, so it
 * never aliases the persist path's super_buf. */
struct facade_gen_ext_ctx {
    struct facade_alloc *alloc;
    struct evpl_iovec    iov;
    uint64_t             new_floor;
    uint8_t              block[FACADE_SUPER_SIZE];
};

static void
facade_gen_ext_done(
    enum chimera_media_error st,
    void                    *priv)
{
    struct facade_gen_ext_ctx *c         = priv;
    struct facade_alloc       *a         = c->alloc;
    uint64_t                   new_floor = c->new_floor;

    free(c);

    /*
     * Advance the in-memory floor only now that the write is durable, and only
     * if we are not tearing down.  On a media error the floor does not advance;
     * a parked draw then re-kicks and retries (a persistently-failing provider
     * would re-park indefinitely -- livelock, never reuse; acceptable here).
     * During teardown we skip the normal resume: mw_complete() -> teardown
     * drains the waiters with shutting_down set so they fail cleanly.
     */
    if (!a->destroy_after) {
        if (st == CHIMERA_MEDIA_OK && new_floor > a->id_floor) {
            a->id_floor = new_floor;
        }
        facade_objid_drain_waiters(a);
    }

    facade_mw_complete(a);
} /* facade_gen_ext_done */

void
facade_objid_start_extension(struct facade_alloc *a)
{
    struct facade_gen_ext_ctx *c;
    uint64_t                   saved;

    a->mw_busy = 1;

    c = malloc(sizeof(*c));
    FACADE_CHECK(c != NULL, "oom id extension ctx");
    c->alloc     = a;
    c->new_floor = a->id_next + FACADE_ID_RESERVE;

    /* Pack a superblock carrying the advanced floor and the LAST-persisted
     * checkpoint descriptor (a->ckpt_count / a->clean), so this write is
     * consistent with the checkpoint already on media.  Keep the in-memory
     * floor at its current value until the write is durable. */
    saved       = a->id_floor;
    a->id_floor = c->new_floor;
    facade_super_pack(a, c->block);
    a->id_floor = saved;

    memset(&c->iov, 0, sizeof(c->iov));
    c->iov.data   = c->block;
    c->iov.length = FACADE_SUPER_SIZE;

    a->provider->write(a->space, &c->iov, 1, FACADE_SUPER_BLOCK, 1,
                       CHIMERA_MEDIA_WRITE_FUA, facade_gen_ext_done, c);
} /* facade_objid_start_extension */

/* Request a floor extension.  Starts it if the metadata slot is free, else
 * coalesces into ext_pending for mw_complete() to run next. */
static void
facade_objid_kick_extension(struct facade_alloc *a)
{
    if (a->shutting_down) {
        return;
    }
    if (a->mw_busy) {
        a->ext_pending = 1;
        return;
    }
    facade_objid_start_extension(a);
} /* facade_objid_kick_extension */

void
facade_objid_drain_waiters(struct facade_alloc *a)
{
    struct facade_waiter *w = a->waiters;

    /* Detach first: a resumed draw may park again (normal path) or fail
     * (shutting_down). */
    a->waiters = NULL;
    while (w) {
        struct facade_waiter *nx     = w->next;
        facade_resume_t       resume = w->resume;
        void                 *arg    = w->arg;

        free(w);
        resume(arg);
        w = nx;
    }
} /* facade_objid_drain_waiters */

SYMBOL_EXPORT enum facade_status
facade_objid_next(
    struct facade_alloc      *a,
    struct chimera_sc_obj_id *r_id,
    facade_resume_t           resume,
    void                     *arg)
{
    uint64_t seq;

    /* A draw resumed during teardown must not re-park on a dying allocator. */
    if (a->shutting_down) {
        return FACADE_E_SHUTDOWN;
    }

    seq        = a->id_next;   /* optimistic draw */
    a->id_next = seq + 1;
    FACADE_CHECK(a->id_next != 0, "object-id sequence exhausted");

    if (seq + FACADE_ID_RESERVE / 2 >= a->id_floor) {
        facade_objid_kick_extension(a);

        if (seq >= a->id_floor) {
            /* The drawn value is at or past the durable floor: it cannot be
             * issued yet.  Park (the value is wasted; the retry draws a fresh
             * one against the advanced floor). */
            struct facade_waiter *w = malloc(sizeof(*w));

            FACADE_CHECK(w != NULL, "oom id waiter");
            w->resume  = resume;
            w->arg     = arg;
            w->next    = a->waiters;
            a->waiters = w;
            return FACADE_AGAIN;
        }
    }

    memcpy(&r_id->b[0], &a->id_nonce, sizeof(a->id_nonce));
    memcpy(&r_id->b[8], &seq, sizeof(seq));
    return FACADE_OK;
} /* facade_objid_next */
