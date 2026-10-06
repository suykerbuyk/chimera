// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * The in-memory conformance fixture backend.  See sc_fixture.h for what it is
 * and is not.  It implements the subset of operations the harness self-test
 * and the obligation primitives exercise; operations outside that subset return
 * CHIMERA_SC_E_NOT_SUPPORTED, a deliberate shortcut for a self-test double (the
 * real backend is E14).
 */

#include <stdlib.h>
#include <string.h>

#include "evpl/evpl.h"

#include "common/macros.h"
#include "sc_fixture.h"

#define SCF_MAX_OBJS  32
#define SCF_MAX_RECS  32
#define SCF_MAX_NODES 4
#define SCF_OBJ_CAP   512
#define SCF_KEY_CAP   64
#define SCF_VAL_CAP   CHIMERA_SC_CONF_MAX_VALUE
#define SCF_LEASE_NS  10000000000ULL

struct scf_obj {
    int                      used;
    struct chimera_sc_obj_id id;
    uint32_t                 refs;          /* 0 or 1 */
    uint32_t                 held;          /* 0 or 1 */
    uint32_t                 state;         /* enum chimera_sc_obj_state */
    uint64_t                 creator_epoch;
    uint8_t                  data[SCF_OBJ_CAP];
    uint32_t                 size;          /* bytes present */
    uint32_t                 durable_size;  /* bytes guaranteed durable */
    int                      corrupted;
};

struct scf_rec {
    int      used;
    uint8_t  key[SCF_KEY_CAP];
    uint32_t klen;
    uint8_t  val[SCF_VAL_CAP];
    uint32_t vlen;
    uint64_t revision;
    int      durable;
};

struct scf_node {
    int                       used;
    struct chimera_sc_node_id node;
    uint64_t                  epoch;
};

struct scf_state {
    enum chimera_sc_fixture_profile  profile;
    struct scf_obj                   objs[SCF_MAX_OBJS];
    struct scf_rec                   recs[SCF_MAX_RECS];
    struct scf_node                  nodes[SCF_MAX_NODES];
    uint64_t                         next_epoch;
    uint64_t                         next_id;
    uint64_t                         next_ticket;
    uint64_t                         revision;
    uint64_t                         durability_epoch;
    int                              down;    /* crashed, awaiting recover */
    int                              full;    /* fill() -> NO_SPACE */
    const struct chimera_sc_upcalls *upcalls;
};

/*
 * Pending requests form an intrusive FIFO threaded through each request's
 * backend_private (the contract's backend scratch field), so there is no fixed
 * depth and no overflow path that would have to complete inline.
 */
struct scf_thread {
    struct scf_state          *s;
    struct evpl               *evpl;
    struct evpl_deferral       deferral;
    struct chimera_sc_request *head;
    struct chimera_sc_request *tail;
    uint8_t                    get_buf[SCF_VAL_CAP];
};

static const struct chimera_sc_protection_class scf_pclasses[] = {
    { .id = 1, .kind = CHIMERA_SC_PROTECT_REPLICA, .n = 2 },
};

/*
 * The single source of truth for cheat tokens, indexed by profile.  Both
 * scf_init (token -> profile) and chimera_sc_fixture_init (profile -> token)
 * use it, so a one-sided edit cannot silently route a profile to HONEST -- a
 * cheat that does not cheat would be a false green in test infrastructure.
 */
static const char *const                        scf_profile_tokens[] = {
    [CHIMERA_SC_FIXTURE_HONEST]                      = "honest",
    [CHIMERA_SC_FIXTURE_ACK_DURABLE_WITHOUT_PERSIST] = "ack_durable_without_persist",
    [CHIMERA_SC_FIXTURE_SPLIT_TXN]                   = "split_txn",
    [CHIMERA_SC_FIXTURE_RETURN_CORRUPT]              = "return_corrupt",
    [CHIMERA_SC_FIXTURE_S11_CHEAT]                   = "s11_cheat",
    [CHIMERA_SC_FIXTURE_DROP_COMPLETION]             = "drop_completion",
};

#define SCF_PROFILE_COUNT \
        (sizeof(scf_profile_tokens) / sizeof(scf_profile_tokens[0]))

/* ---- lookup helpers ---- */

static struct scf_node *
scf_node_find(
    struct scf_state                *s,
    const struct chimera_sc_node_id *node,
    int                              create)
{
    int i;

    for (i = 0; i < SCF_MAX_NODES; i++) {
        if (s->nodes[i].used &&
            memcmp(&s->nodes[i].node, node, sizeof(*node)) == 0) {
            return &s->nodes[i];
        }
    }
    for (i = 0; create && i < SCF_MAX_NODES; i++) {
        if (!s->nodes[i].used) {
            s->nodes[i].used = 1;
            s->nodes[i].node = *node;
            return &s->nodes[i];
        }
    }
    return NULL;
} /* scf_node_find */

static struct scf_obj *
scf_obj_find(
    struct scf_state               *s,
    const struct chimera_sc_obj_id *id)
{
    int i;

    for (i = 0; i < SCF_MAX_OBJS; i++) {
        if (s->objs[i].used &&
            memcmp(&s->objs[i].id, id, sizeof(*id)) == 0) {
            return &s->objs[i];
        }
    }
    return NULL;
} /* scf_obj_find */

static struct scf_rec *
scf_rec_find(
    struct scf_state *s,
    const uint8_t    *key,
    uint32_t          klen)
{
    int i;

    for (i = 0; i < SCF_MAX_RECS; i++) {
        if (s->recs[i].used && s->recs[i].klen == klen &&
            memcmp(s->recs[i].key, key, klen) == 0) {
            return &s->recs[i];
        }
    }
    return NULL;
} /* scf_rec_find */

/* ---- operation handlers ---- */

static void
scf_attach(
    struct scf_state          *s,
    struct chimera_sc_request *req)
{
    struct chimera_sc_attach_args  *a = &req->attach;
    struct scf_node                *node;
    const struct chimera_sc_version have = {
        CHIMERA_SC_CONTRACT_MAJOR, CHIMERA_SC_CONTRACT_MINOR, CHIMERA_SC_CONTRACT_PATCH
    };
    uint32_t                        n = sizeof(scf_pclasses) / sizeof(scf_pclasses[0]);
    uint32_t                        i;

    if (s->down) {
        /* The store is powered off; recover() must run first. */
        req->status = CHIMERA_SC_E_IO;
        return;
    }
    if (chimera_sc_version_negotiate(&a->want, &have, &a->r_version) != 0) {
        req->status = CHIMERA_SC_E_VERSION;
        return;
    }
    node = scf_node_find(s, &req->caller.node, 1);
    if (!node) {
        req->status = CHIMERA_SC_E_INVALID;
        return;
    }
    node->epoch = s->next_epoch++;   /* re-attach revokes the old epoch */

    for (i = 0; i < n && i < a->pclasses_cap; i++) {
        a->pclasses[i] = scf_pclasses[i];
    }
    a->r_npclasses        = n;
    a->r_caps             = CHIMERA_SC_CAP_PROTECTION_CLASSES;
    a->r_fence_epoch      = node->epoch;
    a->r_lease_ns         = SCF_LEASE_NS;
    a->r_durability_epoch = s->durability_epoch;
    memset(&a->r_limits, 0, sizeof(a->r_limits));
    a->r_limits.max_txn_ops   = 64;
    a->r_limits.max_key_len   = SCF_KEY_CAP;
    a->r_limits.max_value_len = SCF_VAL_CAP;
    a->r_limits.max_io        = SCF_OBJ_CAP;
    a->r_limits.lease_ns      = SCF_LEASE_NS;
    req->status               = CHIMERA_SC_OK;
} /* scf_attach */

static int
scf_epoch_ok(
    struct scf_state                *s,
    const struct chimera_sc_request *req)
{
    struct scf_node *n;

    if (s->down) {
        return 0;
    }
    n = scf_node_find(s, &req->caller.node, 0);
    return n && n->epoch != 0 && req->caller.fence_epoch == n->epoch;
} /* scf_epoch_ok */

static void
scf_obj_create(
    struct scf_state          *s,
    struct chimera_sc_request *req)
{
    struct scf_obj *o = NULL;
    int             i;

    for (i = 0; i < SCF_MAX_OBJS; i++) {
        if (!s->objs[i].used) {
            o = &s->objs[i];
            break;
        }
    }
    if (!o) {
        req->status = CHIMERA_SC_E_NO_SPACE;
        return;
    }
    memset(o, 0, sizeof(*o));
    o->used          = 1;
    o->state         = CHIMERA_SC_OBJ_OPEN;
    o->creator_epoch = req->caller.fence_epoch;
    memset(&o->id, 0, sizeof(o->id));
    memcpy(o->id.b, &s->next_id, sizeof(s->next_id));
    s->next_id++;

    req->obj_create.r_id = o->id;
    req->status          = CHIMERA_SC_OK;
} /* scf_obj_create */

static void
scf_obj_write(
    struct scf_state          *s,
    struct chimera_sc_request *req)
{
    struct chimera_sc_obj_write_args *w = &req->obj_write;
    struct scf_obj                   *o = scf_obj_find(s, &w->id);
    uint32_t                          len;
    uint64_t                          end;

    if (!o) {
        req->status = CHIMERA_SC_E_NOT_FOUND;
        return;
    }
    if (s->full) {
        req->status = CHIMERA_SC_E_NO_SPACE;
        return;
    }
    len = w->niov > 0 ? evpl_iovec_length(&w->iov[0]) : 0;
    end = w->offset + len;
    if (end > SCF_OBJ_CAP) {
        req->status = CHIMERA_SC_E_INVALID;
        return;
    }
    if (len) {
        memcpy(o->data + w->offset, evpl_iovec_data(&w->iov[0]), len);
    }
    if (end > o->size) {
        o->size = (uint32_t) end;
    }

    if (w->level == CHIMERA_SC_WRITE_DURABLE &&
        s->profile != CHIMERA_SC_FIXTURE_ACK_DURABLE_WITHOUT_PERSIST) {
        /* Honest: a DURABLE write is actually persisted. */
        if (end > o->durable_size) {
            o->durable_size = (uint32_t) end;
        }
    }
    /* The cheat reports DURABLE but leaves durable_size behind, so a crash
     * drops the data it claimed to persist. */
    w->r_level            = CHIMERA_SC_WRITE_DURABLE;
    w->r_length           = len;
    w->r_durability_epoch = s->durability_epoch;
    req->status           = CHIMERA_SC_OK;
} /* scf_obj_write */

static void
scf_obj_flush(
    struct scf_state          *s,
    struct chimera_sc_request *req)
{
    struct scf_obj *o = scf_obj_find(s, &req->obj_flush.id);

    if (!o) {
        req->status = CHIMERA_SC_E_NOT_FOUND;
        return;
    }
    o->durable_size = o->size;
    s->durability_epoch++;
    req->obj_flush.r_durability_epoch = s->durability_epoch;
    req->status                       = CHIMERA_SC_OK;
} /* scf_obj_flush */

static void
scf_obj_read(
    struct scf_thread         *t,
    struct chimera_sc_request *req)
{
    struct scf_state                *s = t->s;
    struct chimera_sc_obj_read_args *r = &req->obj_read;
    struct scf_obj                  *o = scf_obj_find(s, &r->id);
    uint32_t                         want;
    uint32_t                         avail;
    int                              n;

    if (!o) {
        req->status = CHIMERA_SC_E_NOT_FOUND;
        return;
    }
    if (o->corrupted &&
        s->profile != CHIMERA_SC_FIXTURE_RETURN_CORRUPT) {
        /* Honest: verification failed and cannot be repaired (D9). */
        req->status = CHIMERA_SC_E_DATA_UNRECOVERABLE;
        return;
    }

    avail = o->size > (uint32_t) r->extent.offset
            ? o->size - (uint32_t) r->extent.offset : 0;
    want = (uint32_t) r->extent.length;
    if (want > avail) {
        want = avail;
    }

    r->r_niov   = 0;
    r->r_length = 0;
    r->r_eof    = 1;

    if (want > 0 && r->niov > 0) {
        n = evpl_iovec_alloc(t->evpl, want, 8, 1, 0, &r->iov[0]);
        if (n != 1) {
            req->status = CHIMERA_SC_E_IO;
            return;
        }
        memcpy(evpl_iovec_data(&r->iov[0]),
               o->data + r->extent.offset, want);
        r->r_niov   = 1;
        r->r_length = want;
    }
    req->status = CHIMERA_SC_OK;
} /* scf_obj_read */

/* Check one delta's precondition, in order, against current state. */
static uint32_t
scf_delta_precondition(
    struct scf_state              *s,
    const struct chimera_sc_delta *d)
{
    struct scf_obj *o;

    switch (d->kind) {
        case CHIMERA_SC_DELTA_HOLD:
            o = scf_obj_find(s, &d->hold.obj);
            if (!o || o->refs != 0 || o->held != 0) {
                return CHIMERA_SC_E_STATE;
            }
            return CHIMERA_SC_OK;
        case CHIMERA_SC_DELTA_UNHOLD:
            o = scf_obj_find(s, &d->hold.obj);
            if (!o || o->held != 1) {
                return CHIMERA_SC_E_STATE;
            }
            return CHIMERA_SC_OK;
        case CHIMERA_SC_DELTA_CREATE:
            o = scf_obj_find(s, &d->create.obj);
            if (!o || o->refs != 0) {
                return CHIMERA_SC_E_STATE;
            }
            return CHIMERA_SC_OK;
        case CHIMERA_SC_DELTA_DELETE:
            o = scf_obj_find(s, &d->del.obj);
            if (!o || o->refs != 1) {
                return CHIMERA_SC_E_STATE;
            }
            return CHIMERA_SC_OK;
        case CHIMERA_SC_DELTA_RECALL_HINT:
            return CHIMERA_SC_OK;   /* advisory; always accepted */
        default:
            return CHIMERA_SC_E_NOT_SUPPORTED;
    } /* switch */
} /* scf_delta_precondition */

static void
scf_delta_apply(
    struct scf_state              *s,
    const struct chimera_sc_delta *d)
{
    struct scf_obj *o;

    switch (d->kind) {
        case CHIMERA_SC_DELTA_HOLD:
            o       = scf_obj_find(s, &d->hold.obj);
            o->held = 1;
            break;
        case CHIMERA_SC_DELTA_UNHOLD:
            o       = scf_obj_find(s, &d->hold.obj);
            o->held = 0;
            break;
        case CHIMERA_SC_DELTA_CREATE:
            o       = scf_obj_find(s, &d->create.obj);
            o->refs = 1;
            break;
        case CHIMERA_SC_DELTA_DELETE:
            o       = scf_obj_find(s, &d->del.obj);
            o->refs = 0;
            o->used = 0;   /* released */
            break;
        default:
            break;         /* RECALL_HINT: nothing to apply */
    } /* switch */
} /* scf_delta_apply */

static void
scf_ns_txn(
    struct scf_state          *s,
    struct chimera_sc_request *req)
{
    struct chimera_sc_ns_txn_args *txn = &req->ns_txn;
    uint32_t                       i;
    uint32_t                       rc;

    /* Compares first (D4 compare-and-set). */
    for (i = 0; i < txn->ncompares; i++) {
        const struct chimera_sc_compare *c    = &txn->compares[i];
        struct scf_rec                  *rec  = scf_rec_find(s, c->key.data, c->key.len);
        uint64_t                         have = rec ? rec->revision : 0;

        if (have != c->revision) {
            txn->r_failed_part  = CHIMERA_SC_TXN_COMPARE;
            txn->r_failed_index = i;
            req->status         = CHIMERA_SC_E_COMPARE_FAILED;
            return;
        }
    }

    /* Delta preconditions, in order, before any apply (atomicity). */
    for (i = 0; i < txn->ndeltas; i++) {
        rc = scf_delta_precondition(s, &txn->deltas[i]);
        if (rc != CHIMERA_SC_OK) {
            txn->r_failed_part  = CHIMERA_SC_TXN_DELTA;
            txn->r_failed_index = i;
            req->status         = rc;
            return;
        }
    }

    /* Capacity check for puts before any apply. */
    for (i = 0; i < txn->nputs; i++) {
        const struct chimera_sc_record *p = &txn->puts[i];

        if (p->key.len > SCF_KEY_CAP || p->value.len > SCF_VAL_CAP) {
            txn->r_failed_part  = CHIMERA_SC_TXN_PUT;
            txn->r_failed_index = i;
            req->status         = CHIMERA_SC_E_INVALID;
            return;
        }
        if (!scf_rec_find(s, p->key.data, p->key.len)) {
            int j, free_slot = -1;

            for (j = 0; j < SCF_MAX_RECS; j++) {
                if (!s->recs[j].used) {
                    free_slot = j;
                    break;
                }
            }
            if (free_slot < 0) {
                txn->r_failed_part  = CHIMERA_SC_TXN_PUT;
                txn->r_failed_index = i;
                req->status         = CHIMERA_SC_E_NO_SPACE;
                return;
            }
        }
    }

    /* Apply: deltas, then deletes, then puts. */
    for (i = 0; i < txn->ndeltas; i++) {
        scf_delta_apply(s, &txn->deltas[i]);
    }
    for (i = 0; i < txn->ndeletes; i++) {
        struct scf_rec *rec = scf_rec_find(s, txn->deletes[i].data,
                                           txn->deletes[i].len);
        if (rec) {
            rec->used = 0;
        }
    }
    for (i = 0; i < txn->nputs; i++) {
        const struct chimera_sc_record *p   = &txn->puts[i];
        struct scf_rec                 *rec = scf_rec_find(s, p->key.data, p->key.len);
        int                             j;

        if (!rec) {
            for (j = 0; j < SCF_MAX_RECS; j++) {
                if (!s->recs[j].used) {
                    rec = &s->recs[j];
                    break;
                }
            }
        }
        if (!rec) {
            continue;   /* unreachable: capacity was checked before any apply */
        }
        rec->used = 1;
        rec->klen = p->key.len;
        memcpy(rec->key, p->key.data, p->key.len);
        rec->vlen = p->value.len;
        if (p->value.len) {
            memcpy(rec->val, p->value.data, p->value.len);
        }
        rec->revision = ++s->revision;
        /* split_txn: the last put of a multi-put transaction is left
         * non-durable, so a crash drops it and tears the transaction. */
        rec->durable = !(s->profile == CHIMERA_SC_FIXTURE_SPLIT_TXN &&
                         txn->nputs > 1 && i == txn->nputs - 1);
    }

    txn->r_revision = s->revision;
    req->status     = CHIMERA_SC_OK;
} /* scf_ns_txn */

/* Stage an NS_GET value into the thread's get_buf and point r_value at it. */
static void
scf_ns_get(
    struct scf_thread         *t,
    struct chimera_sc_request *req)
{
    struct chimera_sc_ns_get_args *g   = &req->ns_get;
    struct scf_rec                *rec = scf_rec_find(t->s, g->key.data, g->key.len);

    if (!rec) {
        req->status = CHIMERA_SC_E_NOT_FOUND;
        return;
    }
    memcpy(t->get_buf, rec->val, rec->vlen);
    g->r_value.data = t->get_buf;
    g->r_value.len  = rec->vlen;
    g->r_revision   = rec->revision;
    req->status     = CHIMERA_SC_OK;
} /* scf_ns_get */

static void
scf_recall(
    struct scf_state          *s,
    struct chimera_sc_request *req)
{
    struct chimera_sc_recall_args *r = &req->recall;

    if (r->nids == 0 && r->group == 0) {
        req->status = CHIMERA_SC_E_INVALID;         /* group tag 0 is invalid */
    } else if (r->nids == 0) {
        req->status = CHIMERA_SC_E_NOT_SUPPORTED;   /* no CAP_GROUP_RECALL */
    } else {
        r->r_ticket = s->next_ticket++;
        req->status = CHIMERA_SC_OK;
    }
} /* scf_recall */

/* ---- processing + deferred completion ---- */

static void
scf_handle(
    struct scf_thread         *t,
    struct chimera_sc_request *req)
{
    struct scf_state *s = t->s;

    if (req->opcode == CHIMERA_SC_OP_ATTACH) {
        scf_attach(s, req);
        return;
    }
    if (!scf_epoch_ok(s, req)) {
        req->status = CHIMERA_SC_E_FENCED;
        return;
    }

    switch (req->opcode) {
        case CHIMERA_SC_OP_RENEW:
            req->renew.r_lease_ns = SCF_LEASE_NS;
            req->status           = CHIMERA_SC_OK;
            break;
        case CHIMERA_SC_OP_DETACH:
            scf_node_find(s, &req->caller.node, 0)->epoch = 0;
            req->status                                   = CHIMERA_SC_OK;
            break;
        case CHIMERA_SC_OP_OBJ_CREATE:
            scf_obj_create(s, req);
            break;
        case CHIMERA_SC_OP_OBJ_WRITE:
            scf_obj_write(s, req);
            break;
        case CHIMERA_SC_OP_OBJ_FLUSH:
            scf_obj_flush(s, req);
            break;
        case CHIMERA_SC_OP_OBJ_READ:
            scf_obj_read(t, req);
            break;
        case CHIMERA_SC_OP_NS_TXN:
            scf_ns_txn(s, req);
            break;
        case CHIMERA_SC_OP_NS_GET:
            scf_ns_get(t, req);
            break;
        case CHIMERA_SC_OP_RECALL:
            scf_recall(s, req);
            break;
        default:
            req->status = CHIMERA_SC_E_NOT_SUPPORTED;
            break;
    } /* switch */
} /* scf_handle */

static void
scf_complete(
    struct scf_thread         *t,
    struct chimera_sc_request *req)
{
    scf_handle(t, req);

    /* S11 byte-lifetime timing for NS_GET, and the cheat that breaks it. */
    if (req->opcode == CHIMERA_SC_OP_NS_GET && req->status == CHIMERA_SC_OK) {
        if (t->s->profile == CHIMERA_SC_FIXTURE_S11_CHEAT) {
            /* Overwrite the returned buffer BEFORE complete() returns: a
             * conformant consumer that copies inside complete() sees garbage. */
            memset(t->get_buf, 0xAA, sizeof(t->get_buf));
            req->complete(req);
        } else {
            req->complete(req);
            /* Honest: the buffer is reusable only after complete() returns. */
            memset(t->get_buf, 0xAA, sizeof(t->get_buf));
        }
        return;
    }
    req->complete(req);
} /* scf_complete */

static void
scf_drain(
    struct evpl *evpl,
    void        *private_data)
{
    struct scf_thread         *t = private_data;
    struct chimera_sc_request *req;
    struct chimera_sc_request *next;

    (void) evpl;

    /* Detach the current batch and process only it: a request re-dispatched
     * from inside complete() is enqueued onto a fresh list and re-arms the
     * deferral, so it runs on a later pass. */
    req     = t->head;
    t->head = NULL;
    t->tail = NULL;

    while (req) {
        next                 = req->backend_private;   /* saved before complete() */
        req->backend_private = NULL;
        scf_complete(t, req);                           /* must not touch req after */
        req = next;
    }
} /* scf_drain */

/* ---- backend descriptor ---- */

static void *
scf_init(
    const char                *cfgdata,
    struct prometheus_metrics *metrics)
{
    struct scf_state *s;

    (void) metrics;

    s = malloc(sizeof(*s));
    if (!s) {
        return NULL;
    }
    memset(s, 0, sizeof(*s));
    s->next_epoch       = 100;
    s->next_id          = 1;
    s->next_ticket      = 1;
    s->durability_epoch = 1;

    s->profile = CHIMERA_SC_FIXTURE_HONEST;
    if (cfgdata) {
        size_t i;

        for (i = 0; i < SCF_PROFILE_COUNT; i++) {
            if (scf_profile_tokens[i] &&
                strcmp(cfgdata, scf_profile_tokens[i]) == 0) {
                s->profile = (enum chimera_sc_fixture_profile) i;
                break;
            }
        }
    }
    return s;
} /* scf_init */

static void
scf_destroy(void *private_data)
{
    free(private_data);
} /* scf_destroy */

static void *
scf_thread_init(
    struct evpl *evpl,
    void        *private_data)
{
    struct scf_thread *t = malloc(sizeof(*t));

    if (!t) {
        return NULL;
    }
    memset(t, 0, sizeof(*t));
    t->s    = private_data;
    t->evpl = evpl;
    evpl_deferral_init(&t->deferral, scf_drain, t);
    return t;
} /* scf_thread_init */

static void
scf_thread_destroy(void *thread_private)
{
    struct scf_thread *t = thread_private;

    evpl_remove_deferral(t->evpl, &t->deferral);
    free(t);
} /* scf_thread_destroy */

static void
scf_set_upcalls(
    void                            *private_data,
    const struct chimera_sc_upcalls *upcalls,
    void                            *up_private)
{
    struct scf_state *s = private_data;

    (void) up_private;
    s->upcalls = upcalls;
} /* scf_set_upcalls */

static void
scf_dispatch(
    struct chimera_sc_request *request,
    void                      *thread_private)
{
    struct scf_thread *t = thread_private;

    /* The drop-completion profile never completes a non-ATTACH request, so the
     * harness's bounded pump is exercised.  ATTACH still completes so the
     * lifecycle can start. */
    if (t->s->profile == CHIMERA_SC_FIXTURE_DROP_COMPLETION &&
        request->opcode != CHIMERA_SC_OP_ATTACH) {
        return;
    }

    /* Enqueue on the intrusive FIFO and defer: completion runs from the next
     * event-loop pass, never synchronously inside dispatch(), and there is no
     * fixed depth to overflow. */
    request->backend_private = NULL;
    if (t->tail) {
        t->tail->backend_private = request;
    } else {
        t->head = request;
    }
    t->tail = request;
    evpl_defer(t->evpl, &t->deferral);
} /* scf_dispatch */

static const struct chimera_sc_backend scf_backend = {
    .abi_version    = CHIMERA_SC_ABI_VERSION,
    .name           = "in-memory-fixture",
    .init           = scf_init,
    .destroy        = scf_destroy,
    .thread_init    = scf_thread_init,
    .thread_destroy = scf_thread_destroy,
    .dispatch       = scf_dispatch,
    .set_upcalls    = scf_set_upcalls,
};

/* ---- fixture-control hooks ---- */

static int
scf_crash(void *backend_private)
{
    struct scf_state *s = backend_private;
    int               i;

    s->down = 1;
    for (i = 0; i < SCF_MAX_NODES; i++) {
        s->nodes[i].epoch = 0;   /* in-flight epochs do not survive */
    }
    for (i = 0; i < SCF_MAX_OBJS; i++) {
        struct scf_obj *o = &s->objs[i];

        if (!o->used) {
            continue;
        }
        if (o->refs == 0 && o->held == 0) {
            o->used = 0;                 /* epoch-held: released with the epoch */
        } else {
            o->size = o->durable_size;   /* non-durable writes are lost */
        }
    }
    for (i = 0; i < SCF_MAX_RECS; i++) {
        if (s->recs[i].used && !s->recs[i].durable) {
            s->recs[i].used = 0;
        }
    }
    return 0;
} /* scf_crash */

static int
scf_recover(void *backend_private)
{
    struct scf_state *s = backend_private;

    s->down = 0;
    return 0;
} /* scf_recover */

static int
scf_corrupt(
    void                           *backend_private,
    const struct chimera_sc_obj_id *id)
{
    struct scf_obj *o = scf_obj_find(backend_private, id);

    if (!o) {
        return -1;
    }
    o->corrupted = 1;
    return 0;
} /* scf_corrupt */

static int
scf_fill(void *backend_private)
{
    struct scf_state *s = backend_private;

    s->full = 1;
    return 0;
} /* scf_fill */

SYMBOL_EXPORT void
chimera_sc_fixture_init(
    struct chimera_sc_conformance_fixture *fx,
    enum chimera_sc_fixture_profile        profile)
{
    memset(fx, 0, sizeof(*fx));
    fx->name    = scf_profile_tokens[profile];
    fx->backend = &scf_backend;
    fx->cfgdata = scf_profile_tokens[profile];
    fx->crash   = scf_crash;
    fx->recover = scf_recover;
    fx->corrupt = scf_corrupt;
    fx->fill    = scf_fill;
} /* chimera_sc_fixture_init */
