// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * The minimal real storage-contract backend over the E14 facade.  See
 * facade_sc_backend.h for what it is (the durable-survives-crash op slice) and
 * what object-ops-v1 / namespace-record-store extend.
 *
 * Every op is a switch in dispatch(); media-touching ops (ATTACH open, OBJ_WRITE
 * durable, OBJ_READ) complete from the facade's completion callback on the
 * space's evpl thread, the in-memory ops (OBJ_CREATE, NS_TXN HOLD) complete on
 * the dispatching thread.  crash/recover drive the crash-sim control seam.
 */

#include <stdlib.h>
#include <string.h>

#include "evpl/evpl.h"

#include "common/macros.h"
#include "facade_internal.h"   /* struct facade_alloc, facade_obj_*, facade_media_read */
#include "facade_log.h"        /* facade_obj_write_durable */
#include "facade_sc_backend.h"

#define FSB_LEASE_NS 10000000000ULL

struct fsb_state {
    const struct chimera_media_provider         *provider;
    const struct chimera_media_crashsim_control *control;
    struct chimera_media_space                  *space;
    struct facade_alloc                         *alloc;      /* NULL until ATTACH */
    struct evpl                                 *evpl;       /* set at thread_init */
    uint64_t                                     next_epoch;
    struct chimera_sc_node_id                    node;
    uint64_t                                     node_epoch;
    int                                          down;       /* crashed; recover() pending */
    const struct chimera_sc_upcalls             *upcalls;
};

/* ---- per-async-op contexts ---- */

struct fsb_op {
    struct fsb_state          *s;
    struct chimera_sc_request *req;
    void                      *buf;   /* OBJ_READ temp block */
};

/* ---- ATTACH ---- */

static void
fsb_attach_open_done(
    enum facade_status   status,
    struct facade_alloc *alloc,
    void                *priv)
{
    struct fsb_op                 *op  = priv;
    struct fsb_state              *s   = op->s;
    struct chimera_sc_request     *req = op->req;
    struct chimera_sc_attach_args *a   = &req->attach;

    free(op);

    if (status != FACADE_OK) {
        /* A recognized-but-unsupported on-media format maps to a version
         * refusal; anything else is a media failure. */
        req->status = (status == FACADE_E_FORMAT) ? CHIMERA_SC_E_VERSION
                                                  : CHIMERA_SC_E_IO;
        req->complete(req);
        return;
    }

    s->alloc      = alloc;
    s->node       = req->caller.node;
    s->node_epoch = ++s->next_epoch;   /* re-attach revokes the old epoch */

    a->r_caps             = 0;
    a->r_npclasses        = 0;
    a->r_fence_epoch      = s->node_epoch;
    a->r_lease_ns         = FSB_LEASE_NS;
    a->r_durability_epoch = facade_alloc_power_loss_generation(alloc);
    memset(&a->r_limits, 0, sizeof(a->r_limits));
    a->r_limits.max_txn_ops   = 8;
    a->r_limits.max_key_len   = 64;
    a->r_limits.max_value_len = 256;
    a->r_limits.max_io        = CHIMERA_MEDIA_BLOCK_SIZE;
    a->r_limits.lease_ns      = FSB_LEASE_NS;
    req->status               = CHIMERA_SC_OK;
    req->complete(req);
} /* fsb_attach_open_done */

static void
fsb_attach(
    struct fsb_state          *s,
    struct chimera_sc_request *req)
{
    struct chimera_sc_attach_args  *a    = &req->attach;
    const struct chimera_sc_version have = {
        CHIMERA_SC_CONTRACT_MAJOR, CHIMERA_SC_CONTRACT_MINOR, CHIMERA_SC_CONTRACT_PATCH
    };
    struct fsb_op                  *op;

    if (s->down) {
        req->status = CHIMERA_SC_E_IO;   /* powered off; recover() must run first */
        req->complete(req);
        return;
    }
    if (chimera_sc_version_negotiate(&a->want, &have, &a->r_version) != 0) {
        req->status = CHIMERA_SC_E_VERSION;
        req->complete(req);
        return;
    }

    /* Re-attach after a crash+recover: drop the stale in-memory handle (no media
     * writes) and reopen, which runs recovery over the post-crash image. */
    if (s->alloc) {
        facade_alloc_abandon(s->alloc);
        s->alloc = NULL;
    }

    op      = calloc(1, sizeof(*op));
    op->s   = s;
    op->req = req;
    facade_alloc_open(s->provider, s->space, fsb_attach_open_done, op);
} /* fsb_attach */

static int
fsb_epoch_ok(
    struct fsb_state                *s,
    const struct chimera_sc_request *req)
{
    return !s->down && s->alloc && s->node_epoch != 0 &&
           req->caller.fence_epoch == s->node_epoch;
} /* fsb_epoch_ok */

/* ---- OBJ_CREATE ---- */

static void
fsb_create_resume(void *arg)
{
    struct fsb_op             *op  = arg;
    struct chimera_sc_request *req = op->req;
    enum facade_status         st;

    st = facade_objid_next(op->s->alloc, &req->obj_create.r_id,
                           fsb_create_resume, op);
    if (st == FACADE_AGAIN) {
        return;   /* re-parked; a later resume completes it */
    }
    if (st == FACADE_OK) {
        facade_obj_put(op->s->alloc, req->obj_create.r_id.b, 0, NULL, 0);
        req->status = CHIMERA_SC_OK;
    } else {
        req->status = CHIMERA_SC_E_IO;
    }
    free(op);
    req->complete(req);
} /* fsb_create_resume */

static void
fsb_obj_create(
    struct fsb_state          *s,
    struct chimera_sc_request *req)
{
    struct fsb_op     *op = calloc(1, sizeof(*op));
    enum facade_status st;

    op->s   = s;
    op->req = req;
    st      = facade_objid_next(s->alloc, &req->obj_create.r_id,
                                fsb_create_resume, op);
    if (st == FACADE_AGAIN) {
        return;   /* parked; fsb_create_resume completes it */
    }
    if (st == FACADE_OK) {
        if (!facade_obj_put(s->alloc, req->obj_create.r_id.b, 0, NULL, 0)) {
            req->status = CHIMERA_SC_E_NO_SPACE;
        } else {
            req->status = CHIMERA_SC_OK;
        }
    } else {
        req->status = CHIMERA_SC_E_IO;
    }
    free(op);
    req->complete(req);
} /* fsb_obj_create */

/* ---- NS_TXN (single HOLD delta, the obligation's shape) ---- */

static void
fsb_ns_txn(
    struct fsb_state          *s,
    struct chimera_sc_request *req)
{
    struct chimera_sc_ns_txn_args *txn = &req->ns_txn;
    const struct chimera_sc_delta *d;
    struct facade_obj             *o;

    /* This slice implements only a single HOLD delta; the full NS_TXN surface
     * (records, compares, other deltas, multi-record atomicity) is
     * namespace-record-store's. */
    if (txn->ncompares != 0 || txn->nputs != 0 || txn->ndeletes != 0 ||
        txn->ndeltas != 1) {
        req->status = CHIMERA_SC_E_NOT_SUPPORTED;
        req->complete(req);
        return;
    }
    d = &txn->deltas[0];
    if (d->kind != CHIMERA_SC_DELTA_HOLD) {
        req->status = CHIMERA_SC_E_NOT_SUPPORTED;
        req->complete(req);
        return;
    }
    o = facade_obj_find(s->alloc, d->hold.obj.b);
    if (!o || (o->flags & FACADE_OBJ_HELD)) {
        txn->r_failed_part  = CHIMERA_SC_TXN_DELTA;
        txn->r_failed_index = 0;
        req->status         = CHIMERA_SC_E_STATE;   /* HOLD needs an unheld object */
        req->complete(req);
        return;
    }
    o->flags       |= FACADE_OBJ_HELD;
    txn->r_revision = 0;
    req->status     = CHIMERA_SC_OK;
    req->complete(req);
} /* fsb_ns_txn */

/* ---- OBJ_WRITE (DURABLE) ---- */

static void
fsb_write_done(
    enum facade_status st,
    uint64_t           generation,
    void              *priv)
{
    struct fsb_op                    *op  = priv;
    struct chimera_sc_request        *req = op->req;
    struct chimera_sc_obj_write_args *w   = &req->obj_write;

    free(op);

    if (st != FACADE_OK) {
        req->status = (st == FACADE_E_NO_SPACE) ? CHIMERA_SC_E_NO_SPACE
                      : (st == FACADE_E_SHUTDOWN) ? CHIMERA_SC_E_FENCED
                      : CHIMERA_SC_E_IO;
        req->complete(req);
        return;
    }
    w->r_level            = CHIMERA_SC_WRITE_DURABLE;   /* the level actually achieved */
    w->r_durability_epoch = generation;
    req->status           = CHIMERA_SC_OK;
    req->complete(req);
} /* fsb_write_done */

static void
fsb_obj_write(
    struct fsb_state          *s,
    struct chimera_sc_request *req)
{
    struct chimera_sc_obj_write_args *w = &req->obj_write;
    struct fsb_op                    *op;
    const void                       *data;
    uint32_t                          len;

    if (!facade_obj_find(s->alloc, w->id.b)) {
        req->status = CHIMERA_SC_E_NOT_FOUND;
        req->complete(req);
        return;
    }
    /* The slice writes a single extent at logical offset 0 (object-ops-v1 adds
     * arbitrary offsets, sub-block read-modify-write and the BUFFERED level). */
    if (w->offset != 0 || w->level != CHIMERA_SC_WRITE_DURABLE) {
        req->status = CHIMERA_SC_E_NOT_SUPPORTED;
        req->complete(req);
        return;
    }
    len = w->niov > 0 ? evpl_iovec_length(&w->iov[0]) : 0;
    if (len > CHIMERA_MEDIA_BLOCK_SIZE) {
        req->status = CHIMERA_SC_E_INVALID;
        req->complete(req);
        return;
    }
    data        = len ? evpl_iovec_data(&w->iov[0]) : NULL;
    w->r_length = len;

    op      = calloc(1, sizeof(*op));
    op->s   = s;
    op->req = req;
    facade_obj_write_durable(s->alloc, w->id.b, data, len, fsb_write_done, op);
} /* fsb_obj_write */

/* ---- OBJ_READ ---- */

static void
fsb_read_done(
    enum facade_status st,
    void              *priv)
{
    struct fsb_op                   *op  = priv;
    struct chimera_sc_request       *req = op->req;
    struct chimera_sc_obj_read_args *r   = &req->obj_read;
    struct facade_obj               *o;
    uint32_t                         want;
    int                              n;

    if (st != FACADE_OK) {
        free(op->buf);
        free(op);
        req->status = CHIMERA_SC_E_IO;
        req->complete(req);
        return;
    }

    o    = facade_obj_find(op->s->alloc, r->id.b);
    want = (uint32_t) r->extent.length;
    if (o && o->nextents > 0) {
        uint32_t span = (uint32_t) (o->ext[0].nblocks << CHIMERA_MEDIA_BLOCK_SHIFT);
        if (want > span) {
            want = span;
        }
    }

    r->r_niov   = 0;
    r->r_length = 0;
    r->r_eof    = 1;
    if (want > 0 && r->niov > 0) {
        n = evpl_iovec_alloc(op->s->evpl, want, 8, 1, 0, &r->iov[0]);
        if (n != 1) {
            free(op->buf);
            free(op);
            req->status = CHIMERA_SC_E_IO;
            req->complete(req);
            return;
        }
        memcpy(evpl_iovec_data(&r->iov[0]), op->buf, want);
        r->r_niov   = 1;
        r->r_length = want;
    }
    free(op->buf);
    free(op);
    req->status = CHIMERA_SC_OK;
    req->complete(req);
} /* fsb_read_done */

static void
fsb_obj_read(
    struct fsb_state          *s,
    struct chimera_sc_request *req)
{
    struct chimera_sc_obj_read_args *r = &req->obj_read;
    struct facade_obj               *o = facade_obj_find(s->alloc, r->id.b);
    struct fsb_op                   *op;

    if (!o) {
        req->status = CHIMERA_SC_E_NOT_FOUND;
        req->complete(req);
        return;
    }
    if (r->extent.offset != 0 || o->nextents == 0) {
        /* Slice: a single extent at logical 0 (object-ops-v1 adds the rest). */
        req->status = CHIMERA_SC_E_NOT_SUPPORTED;
        req->complete(req);
        return;
    }

    op      = calloc(1, sizeof(*op));
    op->s   = s;
    op->req = req;
    op->buf = calloc(1, CHIMERA_MEDIA_BLOCK_SIZE);
    FACADE_CHECK(op->buf != NULL, "oom obj-read buffer");
    facade_media_read(s->alloc, op->buf, o->ext[0].phys_block, 1,
                      fsb_read_done, op);
} /* fsb_obj_read */

/* ---- dispatch ---- */

static void
fsb_dispatch(
    struct chimera_sc_request *req,
    void                      *thread_private)
{
    struct fsb_state *s = thread_private;

    if (req->opcode == CHIMERA_SC_OP_ATTACH) {
        fsb_attach(s, req);
        return;
    }
    if (!fsb_epoch_ok(s, req)) {
        req->status = CHIMERA_SC_E_FENCED;
        req->complete(req);
        return;
    }

    switch (req->opcode) {
        case CHIMERA_SC_OP_OBJ_CREATE:
            fsb_obj_create(s, req);
            break;
        case CHIMERA_SC_OP_NS_TXN:
            fsb_ns_txn(s, req);
            break;
        case CHIMERA_SC_OP_OBJ_WRITE:
            fsb_obj_write(s, req);
            break;
        case CHIMERA_SC_OP_OBJ_READ:
            fsb_obj_read(s, req);
            break;
        default:
            req->status = CHIMERA_SC_E_NOT_SUPPORTED;
            req->complete(req);
            break;
    } /* switch */
} /* fsb_dispatch */

/* ---- lifecycle ---- */

static void *
fsb_init(
    const char                *cfgdata,
    struct prometheus_metrics *metrics)
{
    const struct facade_sc_bind *bind = (const struct facade_sc_bind *) cfgdata;
    struct fsb_state            *s;

    (void) metrics;

    if (!bind) {
        return NULL;
    }
    s = calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->provider   = bind->provider;
    s->control    = bind->control;
    s->space      = bind->space;
    s->next_epoch = 100;
    return s;
} /* fsb_init */

static void
fsb_destroy(void *private_data)
{
    struct fsb_state *s = private_data;

    if (s->alloc) {
        facade_alloc_abandon(s->alloc);
        s->alloc = NULL;
    }
    free(s);
} /* fsb_destroy */

static void *
fsb_thread_init(
    struct evpl *evpl,
    void        *private_data)
{
    struct fsb_state *s = private_data;

    s->evpl = evpl;
    return s;   /* single-threaded test double: thread_private == state */
} /* fsb_thread_init */

static void
fsb_thread_destroy(void *thread_private)
{
    (void) thread_private;
} /* fsb_thread_destroy */

static void
fsb_set_upcalls(
    void                            *private_data,
    const struct chimera_sc_upcalls *upcalls,
    void                            *up_private)
{
    struct fsb_state *s = private_data;

    (void) up_private;
    s->upcalls = upcalls;
} /* fsb_set_upcalls */

static const struct chimera_sc_backend fsb_backend = {
    .abi_version    = CHIMERA_SC_ABI_VERSION,
    .name           = "facade",
    .init           = fsb_init,
    .destroy        = fsb_destroy,
    .thread_init    = fsb_thread_init,
    .thread_destroy = fsb_thread_destroy,
    .dispatch       = fsb_dispatch,
    .set_upcalls    = fsb_set_upcalls,
};

/* ---- fixture-control hooks (drive the crash-sim seam) ---- */

static int
fsb_crash(void *backend_private)
{
    struct fsb_state *s = backend_private;
    int               rc;

    /* The durable write has fully completed before the obligation calls crash(),
     * so no media op is in flight (the crash-sim asserts inflight == 0). */
    rc      = s->control->crash(s->space);
    s->down = 1;
    return rc;
} /* fsb_crash */

static int
fsb_recover(void *backend_private)
{
    struct fsb_state *s = backend_private;
    int               rc;

    rc      = s->control->recover(s->space);
    s->down = 0;
    return rc;
} /* fsb_recover */

SYMBOL_EXPORT void
facade_sc_fixture_init(
    struct chimera_sc_conformance_fixture *fx,
    struct facade_sc_bind                 *bind)
{
    memset(fx, 0, sizeof(*fx));
    fx->name    = "facade";
    fx->backend = &fsb_backend;
    fx->cfgdata = (const char *) bind;
    fx->crash   = fsb_crash;
    fx->recover = fsb_recover;
    fx->corrupt = NULL;   /* D9 verification is object-ops-v1 */
    fx->fill    = NULL;
} /* facade_sc_fixture_init */
