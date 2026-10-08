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
#include <time.h>

#include "evpl/evpl.h"

#include "common/macros.h"
#include "facade_internal.h"   /* struct facade_alloc, facade_obj_*, facade_media_read */
#include "facade_log.h"
#include "facade_ns.h"         /* the full 6.2 namespace surface */
#include "facade_object.h"     /* the full 6.1 object surface */
#include "facade_sc_backend.h"

/* Single-node watch registry (D10): NS_WATCH prefixes; events fire after an
 * NS_TXN's COMMIT is durable.  Multi-node delivery is E8's. */
#define FSB_MAX_WATCH 16

/* Map a facade status to the contract error the op returns. */
static uint32_t
fsb_status(enum facade_status st)
{
    switch (st) {
        case FACADE_OK:          return CHIMERA_SC_OK;
        case FACADE_E_NO_SPACE:  return CHIMERA_SC_E_NO_SPACE;
        case FACADE_E_STATE:     return CHIMERA_SC_E_STATE;
        case FACADE_E_NOT_FOUND: return CHIMERA_SC_E_NOT_FOUND;
        case FACADE_E_INVAL:     return CHIMERA_SC_E_INVALID;
        case FACADE_E_SHUTDOWN:  return CHIMERA_SC_E_FENCED;
        default:                 return CHIMERA_SC_E_IO;
    } /* switch */
} /* fsb_status */

#define FSB_LEASE_NS      10000000000ULL

/* The prototype declares no optional capabilities (contract O5, declared at
 * ATTACH per D12): no spokes, no protection classes, no group recall, no escrow
 * quota (sc_types.h:167-170).  Mandatory behaviour is never a capability, so a
 * zero set is the honest declaration for this backend, not a stub -- the
 * degenerate RECALL/RECALL_WAIT the O5 position still mandates belong to a later
 * unit, not here. */
#define FSB_DECLARED_CAPS 0ULL

struct fsb_state {
    const struct chimera_media_provider         *provider;
    const struct chimera_media_crashsim_control *control;
    struct chimera_media_space                  *space;
    struct facade_alloc                         *alloc;      /* NULL until ATTACH */
    struct evpl                                 *evpl;       /* set at thread_init */
    uint64_t                                     next_epoch;
    struct chimera_sc_node_id                    node;
    uint64_t                                     node_epoch;        /* 0 = no live lease */
    uint64_t                                     epoch_deadline_ns; /* monotonic expiry of node_epoch */
    uint64_t                                     (*now_ns)(
        void *arg);
    void                                        *now_arg;
    int                                          down;       /* crashed; recover() pending */
    const struct chimera_sc_upcalls             *upcalls;
    void                                        *up_private;

    /* namespace-record-store: scan cursor table (H1) + watch registry. */
    struct facade_ns_cursor_table                cursors;
    struct {
        uint64_t id;        /* 0 = empty */
        uint8_t *prefix;
        uint32_t plen;
    }                                            watches[FSB_MAX_WATCH];
    uint64_t                                     next_watch_id;
};

/* Default lease clock: the host CLOCK_MONOTONIC, used when the bind injects no
 * clock seam.  A test overrides it through facade_sc_bind to advance time
 * deterministically. */
static uint64_t
fsb_real_now_ns(void *arg)
{
    struct timespec ts;

    (void) arg;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
} /* fsb_real_now_ns */

/* ---- per-async-op contexts ---- */

struct fsb_op {
    struct fsb_state          *s;
    struct chimera_sc_request *req;
    void                      *buf;   /* OBJ_READ temp block */
    struct chimera_sc_obj_id   nid;   /* OBJ_CLONE_WORKING: the drawn new id */
};

/* ---- ATTACH ---- */

/* After the (re)open and the epoch-held GC: fill the attach reply and complete. */
static void
fsb_attach_finish(
    enum facade_status st,
    void              *priv)
{
    struct fsb_op                 *op  = priv;
    struct fsb_state              *s   = op->s;
    struct chimera_sc_request     *req = op->req;
    struct chimera_sc_attach_args *a   = &req->attach;

    free(op);
    if (st != FACADE_OK) {
        req->status = CHIMERA_SC_E_IO;
        req->complete(req);
        return;
    }

    /* The epoch is a lease (6.5, D10): grant it a monotonic deadline.  S14 ties
     * r_lease_ns to the moment the caller dispatched and says the backend never
     * expires sooner than r_lease_ns after it completed the request, so the
     * deadline is set here, at completion, not at dispatch. */
    s->epoch_deadline_ns = s->now_ns(s->now_arg) + FSB_LEASE_NS;

    a->r_caps             = FSB_DECLARED_CAPS;
    a->r_npclasses        = 0;
    a->r_fence_epoch      = s->node_epoch;
    a->r_lease_ns         = FSB_LEASE_NS;
    a->r_durability_epoch = facade_alloc_power_loss_generation(s->alloc);
    memset(&a->r_limits, 0, sizeof(a->r_limits));
    a->r_limits.max_txn_ops   = FACADE_NS_MAX_TXN_LOG_BLOCKS - 1u; /* records + COMMIT */
    a->r_limits.max_key_len   = FACADE_NS_MAX_KEY;
    a->r_limits.max_value_len = FACADE_NS_MAX_VALUE;
    a->r_limits.max_io        = CHIMERA_MEDIA_BLOCK_SIZE;
    a->r_limits.max_watch     = FSB_MAX_WATCH;
    a->r_limits.lease_ns      = FSB_LEASE_NS;
    req->status               = CHIMERA_SC_OK;
    req->complete(req);
} /* fsb_attach_finish */

static void
fsb_attach_open_done(
    enum facade_status   status,
    struct facade_alloc *alloc,
    void                *priv)
{
    struct fsb_op             *op  = priv;
    struct fsb_state          *s   = op->s;
    struct chimera_sc_request *req = op->req;

    if (status != FACADE_OK) {
        /* A recognized-but-unsupported on-media format maps to a version
         * refusal; anything else is a media failure. */
        free(op);
        req->status = (status == FACADE_E_FORMAT) ? CHIMERA_SC_E_VERSION
                                                  : CHIMERA_SC_E_IO;
        req->complete(req);
        return;
    }

    s->alloc      = alloc;
    s->node       = req->caller.node;
    s->node_epoch = ++s->next_epoch;   /* re-attach revokes the old epoch */

    /* A new epoch ends the previous one: release every OPEN && !HELD
     * (epoch-held) object recovery may have resurrected -- a never-referenced,
     * unheld working version does not survive an epoch change (only sealed/held
     * objects do).  Made durable by a forced compaction inside the release. */
    facade_obj_release_epoch_held(s->alloc, fsb_attach_finish, op);
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
    if (req->caller.fence_epoch != 0) {
        /* ATTACH carries epoch 0 (contract 6.5); a nonzero epoch is a malformed
         * request, not a fencing failure. */
        req->status = CHIMERA_SC_E_INVALID;
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

/* The single gate every non-ATTACH op passes.  One predicate covers all three
 * ways an epoch is not usable, each -> CHIMERA_SC_E_FENCED at the call site:
 *   - revoked / stale value: caller.fence_epoch != the live node_epoch (a
 *     re-ATTACH, a DETACH or a FENCE-of-self has moved or cleared it);
 *   - wrong node: the caller is not the node that holds the epoch (today a
 *     different node carrying a coincidentally-equal epoch would slip through);
 *   - expired: the monotonic lease deadline has passed (D10: the backend
 *     expires an epoch when its lease runs out). */
static int
fsb_epoch_ok(
    struct fsb_state                *s,
    const struct chimera_sc_request *req)
{
    return !s->down && s->alloc && s->node_epoch != 0 &&
           req->caller.fence_epoch == s->node_epoch &&
           memcmp(&req->caller.node, &s->node, sizeof(s->node)) == 0 &&
           s->now_ns(s->now_arg) < s->epoch_deadline_ns;
} /* fsb_epoch_ok */

/* ---- RENEW ---- */

static void
fsb_renew(
    struct fsb_state          *s,
    struct chimera_sc_request *req)
{
    /* The gate already proved the epoch is live, matching and unexpired, so
     * RENEW only replaces the remaining lease with a fresh one (S14: RENEW
     * replaces, it does not add).  A revoked or expired epoch never reaches
     * here -- it is CHIMERA_SC_E_FENCED at the gate -- which is exactly the
     * contract's "a lease is never revived." */
    s->epoch_deadline_ns  = s->now_ns(s->now_arg) + FSB_LEASE_NS;
    req->renew.r_lease_ns = FSB_LEASE_NS;
    req->status           = CHIMERA_SC_OK;
    req->complete(req);
} /* fsb_renew */

/* ---- FENCE (single-node subset) ---- */

static void
fsb_fence(
    struct fsb_state          *s,
    struct chimera_sc_request *req)
{
    /* The gate proved the caller holds a live epoch (FENCE requires one, else
     * E_FENCED).  Single-node subset (sc_request.h:258-266): fencing the
     * caller's own node is E_INVALID -- a node ends its own epoch with DETACH --
     * and fencing any OTHER node id completes OK at once, because in the
     * single-node prototype no other node holds a live epoch, so FENCE's "a node
     * with no live epoch completes OK" postcondition already holds.  Revoking a
     * different, LIVE node's epoch and draining its in-flight ops is multi-node
     * fencing (E8), out of scope. */
    if (memcmp(&req->fence.node, &s->node, sizeof(s->node)) == 0) {
        req->status = CHIMERA_SC_E_INVALID;
    } else {
        req->status = CHIMERA_SC_OK;
    }
    req->complete(req);
} /* fsb_fence */

/* ---- DETACH ---- */

static void
fsb_detach_released(
    enum facade_status st,
    void              *priv)
{
    struct fsb_op             *op  = priv;
    struct fsb_state          *s   = op->s;
    struct chimera_sc_request *req = op->req;

    free(op);
    /* The epoch ends regardless of the release result; clearing node_epoch makes
     * every later op gate to E_FENCED. */
    s->node_epoch        = 0;
    s->epoch_deadline_ns = 0;
    req->status          = (st == FACADE_OK) ? CHIMERA_SC_OK : CHIMERA_SC_E_IO;
    req->complete(req);
} /* fsb_detach_released */

static void
fsb_detach(
    struct fsb_state          *s,
    struct chimera_sc_request *req)
{
    /* End the caller's own epoch (the gate proved it is live) and release its
     * epoch-held objects (sc_request.h:271-272): the lease-scoped,
     * never-referenced working versions (OBJ_CLONE_WORKING) and fresh opens --
     * every OPEN && !HELD object.  This is DISTINCT from the S13 namespace HOLD
     * flag (FACADE_OBJ_HELD), which is a durable record state, not epoch-scoped,
     * and survives.  The release is made durable (forced compaction) before the
     * epoch is cleared and the DETACH completes. */
    {
        struct fsb_op *op = calloc(1, sizeof(*op));

        op->s   = s;
        op->req = req;
        facade_obj_release_epoch_held(s->alloc, fsb_detach_released, op);
    }
} /* fsb_detach */

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
        req->status = facade_obj_create_entry(op->s->alloc, req->obj_create.r_id.b,
                                              &req->obj_create.stamps)
                      ? CHIMERA_SC_OK : CHIMERA_SC_E_NO_SPACE;
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
        req->status = facade_obj_create_entry(s->alloc, req->obj_create.r_id.b,
                                              &req->obj_create.stamps)
                      ? CHIMERA_SC_OK : CHIMERA_SC_E_NO_SPACE;
    } else {
        req->status = CHIMERA_SC_E_IO;
    }
    free(op);
    req->complete(req);
} /* fsb_obj_create */

/* ---- NS_WATCH event delivery (single node) ---- */

/* Fire watch_event for every registered watch whose prefix matches a committed
 * put/delete key.  The rec is built on the stack over the request's own key/value
 * bytes (valid through the upcall, S11). */
static void
fsb_ns_fire_watches(
    struct fsb_state                    *s,
    const struct chimera_sc_ns_txn_args *t)
{
    uint32_t w, i;

    if (!s->upcalls || !s->upcalls->watch_event) {
        return;
    }
    for (w = 0; w < FSB_MAX_WATCH; w++) {
        if (s->watches[w].id == 0) {
            continue;
        }
        for (i = 0; i < t->nputs; i++) {
            const struct chimera_sc_bytes *k = &t->puts[i].key;

            if (k->len >= s->watches[w].plen &&
                memcmp(k->data, s->watches[w].prefix, s->watches[w].plen) == 0) {
                struct chimera_sc_record rec = { t->puts[i].key, t->puts[i].value, 0 };

                s->upcalls->watch_event(s->watches[w].id, &rec, 0, 0, s->up_private);
            }
        }
        for (i = 0; i < t->ndeletes; i++) {
            const struct chimera_sc_bytes *k = &t->deletes[i];

            if (k->len >= s->watches[w].plen &&
                memcmp(k->data, s->watches[w].prefix, s->watches[w].plen) == 0) {
                struct chimera_sc_bytes  empty = { NULL, 0, 0 };
                struct chimera_sc_record rec   = { t->deletes[i], empty, 0 };

                s->upcalls->watch_event(s->watches[w].id, &rec, 1, 0, s->up_private);
            }
        }
    }
} /* fsb_ns_fire_watches */

/* ---- NS_TXN (full compare-and-set + deltas, durable, atomic) ---- */

struct fsb_ns_txn_op {
    struct fsb_state          *s;
    struct chimera_sc_request *req;
};

static void
fsb_ns_txn_done(
    const struct facade_ns_txn_result *res,
    void                              *priv)
{
    struct fsb_ns_txn_op          *op  = priv;
    struct fsb_state              *s   = op->s;
    struct chimera_sc_request     *req = op->req;
    struct chimera_sc_ns_txn_args *txn = &req->ns_txn;

    txn->r_revision     = res->r_revision;
    txn->r_failed_part  = res->r_failed_part;
    txn->r_failed_index = res->r_failed_index;
    req->status         = res->status;
    if (res->status == CHIMERA_SC_OK) {
        fsb_ns_fire_watches(s, txn);
    }
    free(op);
    req->complete(req);
} /* fsb_ns_txn_done */

static void
fsb_ns_txn(
    struct fsb_state          *s,
    struct chimera_sc_request *req)
{
    struct fsb_ns_txn_op *op = calloc(1, sizeof(*op));

    FACADE_CHECK(op != NULL, "oom ns txn op");
    op->s   = s;
    op->req = req;
    facade_ns_txn(s->alloc, &req->ns_txn, fsb_ns_txn_done, op);
} /* fsb_ns_txn */

/* ---- NS_GET ---- */

static void
fsb_ns_get(
    struct fsb_state          *s,
    struct chimera_sc_request *req)
{
    struct chimera_sc_ns_get_args *g = &req->ns_get;
    const uint8_t                 *val;
    uint32_t                       vlen;
    uint64_t                       rev;

    req->status = facade_ns_get(s->alloc, g->key.data, g->key.len, &val, &vlen, &rev);
    if (req->status == CHIMERA_SC_OK) {
        g->r_value.data     = val;    /* into the live record; valid through complete() (S11) */
        g->r_value.len      = vlen;
        g->r_value.reserved = 0;
        g->r_revision       = rev;
    }
    req->complete(req);
} /* fsb_ns_get */

/* ---- NS_SCAN ---- */

static void
fsb_ns_scan(
    struct fsb_state          *s,
    struct chimera_sc_request *req)
{
    struct chimera_sc_ns_scan_args *sc = &req->ns_scan;

    req->status = facade_ns_scan(s->alloc, &s->cursors, sc->prefix.data, sc->prefix.len,
                                 sc->cursor.b, sc->recs, sc->recs_cap, &sc->r_nrecs,
                                 sc->r_next_cursor.b, &sc->r_eof);
    req->complete(req);
} /* fsb_ns_scan */

/* ---- NS_WATCH / NS_UNWATCH (single node) ---- */

static void
fsb_ns_watch(
    struct fsb_state          *s,
    struct chimera_sc_request *req)
{
    struct chimera_sc_ns_watch_args *w = &req->ns_watch;
    uint32_t                         i;

    for (i = 0; i < FSB_MAX_WATCH; i++) {
        if (s->watches[i].id == 0) {
            s->watches[i].prefix = w->prefix.len ? malloc(w->prefix.len) : NULL;
            FACADE_CHECK(w->prefix.len == 0 || s->watches[i].prefix != NULL,
                         "oom watch prefix");
            if (w->prefix.len) {
                memcpy(s->watches[i].prefix, w->prefix.data, w->prefix.len);
            }
            s->watches[i].plen = w->prefix.len;
            s->watches[i].id   = ++s->next_watch_id;
            w->r_watch_id      = s->watches[i].id;
            req->status        = CHIMERA_SC_OK;
            req->complete(req);
            return;
        }
    }
    req->status = CHIMERA_SC_E_NO_SPACE;   /* watch table full (max_watch) */
    req->complete(req);
} /* fsb_ns_watch */

static void
fsb_ns_unwatch(
    struct fsb_state          *s,
    struct chimera_sc_request *req)
{
    uint64_t id = req->ns_unwatch.watch_id;
    uint32_t i;

    req->status = CHIMERA_SC_E_NOT_FOUND;
    for (i = 0; i < FSB_MAX_WATCH; i++) {
        if (s->watches[i].id == id && id != 0) {
            free(s->watches[i].prefix);
            s->watches[i].prefix = NULL;
            s->watches[i].id     = 0;
            s->watches[i].plen   = 0;
            req->status          = CHIMERA_SC_OK;
            break;
        }
    }
    req->complete(req);
} /* fsb_ns_unwatch */

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
        req->status = fsb_status(st);
        req->complete(req);
        return;
    }
    /* The prototype writes through: a BUFFERED request is also made durable, so
     * the level actually achieved is DURABLE (contract D5: a backend may report a
     * stronger level than requested). */
    w->r_level            = CHIMERA_SC_WRITE_DURABLE;
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

    if (w->level != CHIMERA_SC_WRITE_BUFFERED && w->level != CHIMERA_SC_WRITE_DURABLE) {
        req->status = CHIMERA_SC_E_INVALID;
        req->complete(req);
        return;
    }
    len = w->niov > 0 ? evpl_iovec_length(&w->iov[0]) : 0;
    if (len == 0 || len > CHIMERA_MEDIA_BLOCK_SIZE) {
        req->status = CHIMERA_SC_E_INVALID;   /* max_io is one block */
        req->complete(req);
        return;
    }
    data        = evpl_iovec_data(&w->iov[0]);
    w->r_length = len;

    op      = calloc(1, sizeof(*op));
    op->s   = s;
    op->req = req;
    facade_obj_write(s->alloc, w->id.b, w->offset, data, len, w->level,
                     fsb_write_done, op);
} /* fsb_obj_write */

/* ---- OBJ_READ ---- */

static void
fsb_read_done(
    enum facade_status st,
    uint32_t           length,
    uint64_t           obj_size,
    void              *priv)
{
    struct fsb_op                   *op  = priv;
    struct chimera_sc_request       *req = op->req;
    struct chimera_sc_obj_read_args *r   = &req->obj_read;
    int                              n;

    if (st != FACADE_OK) {
        free(op->buf);
        free(op);
        req->status = fsb_status(st);
        req->complete(req);
        return;
    }

    r->r_niov   = 0;
    r->r_length = 0;
    r->r_eof    = (r->extent.offset + length >= obj_size) ? 1u : 0u;
    if (length > 0 && r->niov > 0) {
        n = evpl_iovec_alloc(op->s->evpl, length, 8, 1, 0, &r->iov[0]);
        if (n != 1) {
            free(op->buf);
            free(op);
            req->status = CHIMERA_SC_E_IO;
            req->complete(req);
            return;
        }
        memcpy(evpl_iovec_data(&r->iov[0]), op->buf, length);
        r->r_niov   = 1;
        r->r_length = length;
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
    struct fsb_op                   *op;
    uint32_t                         want = (uint32_t) r->extent.length;

    if (!facade_obj_find(s->alloc, r->id.b)) {
        req->status = CHIMERA_SC_E_NOT_FOUND;
        req->complete(req);
        return;
    }
    if (want > CHIMERA_MEDIA_BLOCK_SIZE) {
        want = CHIMERA_MEDIA_BLOCK_SIZE;   /* max_io is one block */
    }
    op      = calloc(1, sizeof(*op));
    op->s   = s;
    op->req = req;
    op->buf = calloc(1, CHIMERA_MEDIA_BLOCK_SIZE);
    FACADE_CHECK(op->buf != NULL, "oom obj-read buffer");
    facade_obj_read(s->alloc, r->id.b, r->extent.offset, want, op->buf,
                    fsb_read_done, op);
} /* fsb_obj_read */

/* ---- OBJ_SEAL ---- */

static void
fsb_seal_done(
    enum facade_status st,
    uint64_t           generation,
    void              *priv)
{
    struct fsb_op             *op  = priv;
    struct chimera_sc_request *req = op->req;

    (void) generation;
    free(op);
    req->status = fsb_status(st);
    req->complete(req);
} /* fsb_seal_done */

static void
fsb_obj_seal(
    struct fsb_state          *s,
    struct chimera_sc_request *req)
{
    struct fsb_op *op = calloc(1, sizeof(*op));

    op->s   = s;
    op->req = req;
    facade_obj_seal(s->alloc, req->obj_seal.id.b, fsb_seal_done, op);
} /* fsb_obj_seal */

/* ---- OBJ_FLUSH ---- */

static void
fsb_flush_done(
    enum facade_status st,
    uint64_t           generation,
    void              *priv)
{
    struct fsb_op             *op  = priv;
    struct chimera_sc_request *req = op->req;

    free(op);
    if (st == FACADE_OK) {
        req->obj_flush.r_durability_epoch = generation;
    }
    req->status = fsb_status(st);
    req->complete(req);
} /* fsb_flush_done */

static void
fsb_obj_flush(
    struct fsb_state          *s,
    struct chimera_sc_request *req)
{
    struct fsb_op *op = calloc(1, sizeof(*op));

    op->s   = s;
    op->req = req;
    facade_obj_flush(s->alloc, req->obj_flush.id.b, fsb_flush_done, op);
} /* fsb_obj_flush */

/* ---- OBJ_TRUNCATE / OBJ_ALLOCATE / OBJ_PUNCH (obj_space) ---- */

static void
fsb_space_done(
    enum facade_status st,
    uint64_t           generation,
    void              *priv)
{
    struct fsb_op             *op  = priv;
    struct chimera_sc_request *req = op->req;

    free(op);
    if (st == FACADE_OK) {
        req->obj_space.r_level            = CHIMERA_SC_WRITE_DURABLE;
        req->obj_space.r_durability_epoch = generation;
    }
    req->status = fsb_status(st);
    req->complete(req);
} /* fsb_space_done */

static void
fsb_obj_space(
    struct fsb_state          *s,
    struct chimera_sc_request *req)
{
    struct chimera_sc_obj_space_args *sp = &req->obj_space;
    struct fsb_op                    *op = calloc(1, sizeof(*op));

    op->s   = s;
    op->req = req;
    switch (req->opcode) {
        case CHIMERA_SC_OP_OBJ_TRUNCATE:
            facade_obj_truncate(s->alloc, sp->id.b, sp->extent.offset,
                                fsb_space_done, op);
            break;
        case CHIMERA_SC_OP_OBJ_ALLOCATE:
            facade_obj_allocate(s->alloc, sp->id.b, sp->extent.offset,
                                sp->extent.length, fsb_space_done, op);
            break;
        case CHIMERA_SC_OP_OBJ_PUNCH:
            facade_obj_punch(s->alloc, sp->id.b, sp->extent.offset,
                             sp->extent.length, fsb_space_done, op);
            break;
        default:
            free(op);
            req->status = CHIMERA_SC_E_NOT_SUPPORTED;
            req->complete(req);
            break;
    } /* switch */
} /* fsb_obj_space */

/* ---- OBJ_STAT ---- */

static void
fsb_obj_stat(
    struct fsb_state          *s,
    struct chimera_sc_request *req)
{
    req->status = facade_obj_stat(s->alloc, req->obj_stat.id.b, &req->obj_stat.r_meta)
        == 0 ? CHIMERA_SC_OK : CHIMERA_SC_E_NOT_FOUND;
    req->complete(req);
} /* fsb_obj_stat */

/* ---- OBJ_MAP ---- */

static void
fsb_obj_map(
    struct fsb_state          *s,
    struct chimera_sc_request *req)
{
    struct chimera_sc_obj_map_args *m = &req->obj_map;

    if (facade_obj_map(s->alloc, m->id.b, m->extent.offset, m->extent.length,
                       m->segs, m->segs_cap, &m->r_nsegs, &m->r_more) == 0) {
        req->status = CHIMERA_SC_OK;
    } else {
        req->status = CHIMERA_SC_E_NOT_FOUND;
    }
    req->complete(req);
} /* fsb_obj_map */

/* ---- OBJ_CLONE_WORKING (D2) ---- */

static void
fsb_clone_done(
    enum facade_status st,
    uint64_t           generation,
    void              *priv)
{
    struct fsb_op             *op  = priv;
    struct chimera_sc_request *req = op->req;

    (void) generation;
    if (st == FACADE_OK) {
        req->obj_clone_working.r_id = op->nid;
    }
    req->status = fsb_status(st);
    free(op);
    req->complete(req);
} /* fsb_clone_done */

static void
fsb_clone_issue(struct fsb_op *op)
{
    struct chimera_sc_request *req = op->req;

    facade_obj_clone_working(op->s->alloc, req->obj_clone_working.sealed_id.b,
                             &op->nid, fsb_clone_done, op);
} /* fsb_clone_issue */

static void
fsb_clone_resume(void *arg)
{
    struct fsb_op     *op = arg;
    enum facade_status st;

    st = facade_objid_next(op->s->alloc, &op->nid, fsb_clone_resume, op);
    if (st == FACADE_AGAIN) {
        return;
    }
    if (st != FACADE_OK) {
        struct chimera_sc_request *req = op->req;

        free(op);
        req->status = CHIMERA_SC_E_IO;
        req->complete(req);
        return;
    }
    fsb_clone_issue(op);
} /* fsb_clone_resume */

static void
fsb_obj_clone_working(
    struct fsb_state          *s,
    struct chimera_sc_request *req)
{
    struct fsb_op     *op = calloc(1, sizeof(*op));
    enum facade_status st;

    op->s   = s;
    op->req = req;
    st      = facade_objid_next(s->alloc, &op->nid, fsb_clone_resume, op);
    if (st == FACADE_AGAIN) {
        return;   /* parked; fsb_clone_resume issues the clone */
    }
    if (st != FACADE_OK) {
        free(op);
        req->status = CHIMERA_SC_E_IO;
        req->complete(req);
        return;
    }
    fsb_clone_issue(op);
} /* fsb_obj_clone_working */

/* ---- OBJ_DISCARD ---- */

static void
fsb_discard_done(
    enum facade_status st,
    void              *priv)
{
    struct fsb_op             *op  = priv;
    struct chimera_sc_request *req = op->req;

    free(op);
    req->status = fsb_status(st);
    req->complete(req);
} /* fsb_discard_done */

static void
fsb_obj_discard(
    struct fsb_state          *s,
    struct chimera_sc_request *req)
{
    struct fsb_op *op = calloc(1, sizeof(*op));

    op->s   = s;
    op->req = req;
    facade_obj_discard(s->alloc, req->obj_discard.id.b, fsb_discard_done, op);
} /* fsb_obj_discard */

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
        case CHIMERA_SC_OP_RENEW:
            fsb_renew(s, req);
            break;
        case CHIMERA_SC_OP_FENCE:
            fsb_fence(s, req);
            break;
        case CHIMERA_SC_OP_DETACH:
            fsb_detach(s, req);
            break;
        case CHIMERA_SC_OP_OBJ_CREATE:
            fsb_obj_create(s, req);
            break;
        case CHIMERA_SC_OP_NS_TXN:
            fsb_ns_txn(s, req);
            break;
        case CHIMERA_SC_OP_NS_GET:
            fsb_ns_get(s, req);
            break;
        case CHIMERA_SC_OP_NS_SCAN:
            fsb_ns_scan(s, req);
            break;
        case CHIMERA_SC_OP_NS_WATCH:
            fsb_ns_watch(s, req);
            break;
        case CHIMERA_SC_OP_NS_UNWATCH:
            fsb_ns_unwatch(s, req);
            break;
        case CHIMERA_SC_OP_OBJ_WRITE:
            fsb_obj_write(s, req);
            break;
        case CHIMERA_SC_OP_OBJ_READ:
            fsb_obj_read(s, req);
            break;
        case CHIMERA_SC_OP_OBJ_FLUSH:
            fsb_obj_flush(s, req);
            break;
        case CHIMERA_SC_OP_OBJ_SEAL:
            fsb_obj_seal(s, req);
            break;
        case CHIMERA_SC_OP_OBJ_TRUNCATE:
        case CHIMERA_SC_OP_OBJ_ALLOCATE:
        case CHIMERA_SC_OP_OBJ_PUNCH:
            fsb_obj_space(s, req);
            break;
        case CHIMERA_SC_OP_OBJ_STAT:
            fsb_obj_stat(s, req);
            break;
        case CHIMERA_SC_OP_OBJ_MAP:
            fsb_obj_map(s, req);
            break;
        case CHIMERA_SC_OP_OBJ_CLONE_WORKING:
            fsb_obj_clone_working(s, req);
            break;
        case CHIMERA_SC_OP_OBJ_DISCARD:
            fsb_obj_discard(s, req);
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
    s->now_ns     = bind->now_ns ? bind->now_ns : fsb_real_now_ns;
    s->now_arg    = bind->now_arg;
    facade_ns_cursor_table_init(&s->cursors);
    return s;
} /* fsb_init */

static void
fsb_destroy(void *private_data)
{
    struct fsb_state *s = private_data;
    uint32_t          i;

    if (s->alloc) {
        facade_alloc_abandon(s->alloc);
        s->alloc = NULL;
    }
    facade_ns_cursor_table_destroy(&s->cursors);
    for (i = 0; i < FSB_MAX_WATCH; i++) {
        free(s->watches[i].prefix);
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

    s->upcalls    = upcalls;
    s->up_private = up_private;
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
facade_sc_bind_init(
    struct facade_sc_bind                       *b,
    const struct chimera_media_provider         *provider,
    const struct chimera_media_crashsim_control *control,
    struct chimera_media_space                  *space)
{
    /* Zero the whole struct so the optional clock seam reads NULL (fsb_init
     * then installs the CLOCK_MONOTONIC default); set the required fields. */
    memset(b, 0, sizeof(*b));
    b->provider = provider;
    b->control  = control;
    b->space    = space;
} /* facade_sc_bind_init */

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
