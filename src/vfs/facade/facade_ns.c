// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Backend-facade namespace-record store (namespace-record-store).  See
 * facade_ns.h for the contract surface and facade_internal.h for the in-memory
 * record and the store/checkpoint/replay helper contract.
 *
 * The store is a sorted array of records ordered by EXACT key bytes (M32).  A
 * committed NS_TXN is one {FACADE_LR_NS*, FACADE_LR_OBJ*, COMMIT} durable log
 * transaction built as a NEW multi-record builder on the shared facade_txn FIFO,
 * running the same clean=0 dirty-window arm and G1 compaction-before-wrap the
 * object-txn starter runs (plan C1); the staged mutations touch live memory only
 * in the COMMIT-durable callback, so a crash with no COMMIT leaves memory at the
 * pre-transaction baseline (recovery drops a COMMIT-less txn).
 *
 * Object binding deltas drive a MINIMAL 0<->1 refcount (full refcount-and-gc is a
 * deferred unit).  A delta that releases an object (DELETE, REPLACE's old side)
 * removes it in memory on commit and returns its owned blocks to the free set;
 * crash-window reclamation rides on the existing epoch-held GC at re-ATTACH (the
 * released object is open and unheld), as the HOLD slice did.
 *
 * Links no libevpl; all media I/O is through the facade_media_* helpers.
 */

#include <stdlib.h>
#include <string.h>

#include "common/macros.h"
#include "facade_internal.h"
#include "facade_log.h"
#include "facade_ns.h"
#include "facade_object.h"
#include "facade_super.h"
#include "media_types.h"

/* ================================================================== */
/* key order + sorted-array store                                     */
/* ================================================================== */

SYMBOL_EXPORT int
facade_ns_keycmp(
    const uint8_t *a,
    uint32_t       alen,
    const uint8_t *b,
    uint32_t       blen)
{
    uint32_t n = alen < blen ? alen : blen;
    int      c = (n == 0) ? 0 : memcmp(a, b, n);

    if (c != 0) {
        return c < 0 ? -1 : 1;
    }
    if (alen < blen) {
        return -1;
    }
    return alen > blen ? 1 : 0;
} /* facade_ns_keycmp */

SYMBOL_EXPORT int
facade_ns_lower_bound(
    struct facade_alloc *a,
    const uint8_t       *key,
    uint32_t             klen,
    uint32_t            *r_idx)
{
    uint32_t lo = 0, hi = a->nns;

    while (lo < hi) {
        uint32_t              mid = lo + (hi - lo) / 2;
        struct facade_ns_rec *r   = a->ns_recs[mid];

        if (facade_ns_keycmp(r->key, r->key_len, key, klen) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    *r_idx = lo;
    if (lo < a->nns) {
        struct facade_ns_rec *r = a->ns_recs[lo];

        if (facade_ns_keycmp(r->key, r->key_len, key, klen) == 0) {
            return 1;
        }
    }
    return 0;
} /* facade_ns_lower_bound */

SYMBOL_EXPORT struct facade_ns_rec *
facade_ns_find(
    struct facade_alloc *a,
    const uint8_t       *key,
    uint32_t             klen)
{
    uint32_t idx;

    return facade_ns_lower_bound(a, key, klen, &idx) ? a->ns_recs[idx] : NULL;
} /* facade_ns_find */

static uint8_t *
facade_ns_dup(
    const uint8_t *src,
    uint32_t       len)
{
    uint8_t *p;

    if (len == 0) {
        return NULL;
    }
    p = malloc(len);
    FACADE_CHECK(p != NULL, "oom ns record bytes");
    memcpy(p, src, len);
    return p;
} /* facade_ns_dup */

SYMBOL_EXPORT int
facade_ns_apply_put(
    struct facade_alloc *a,
    const uint8_t       *key,
    uint32_t             klen,
    const uint8_t       *val,
    uint32_t             vlen,
    uint64_t             revision)
{
    uint32_t              idx;
    struct facade_ns_rec *rec;

    if (facade_ns_lower_bound(a, key, klen, &idx)) {
        /* Replace the value and bump the revision; the key stays. */
        rec = a->ns_recs[idx];
        free(rec->val);
        rec->val      = facade_ns_dup(val, vlen);
        rec->val_len  = vlen;
        rec->revision = revision;
        return 0;
    }

    if (a->nns >= a->ns_budget) {
        return -1;   /* store full (NO_SPACE-shaped) */
    }
    if (a->nns == a->ns_cap) {
        uint32_t               newcap = a->ns_cap ? a->ns_cap * 2u : 16u;
        struct facade_ns_rec **grown;

        if (newcap > a->ns_budget) {
            newcap = a->ns_budget;
        }
        grown = realloc(a->ns_recs, (size_t) newcap * sizeof(*grown));
        FACADE_CHECK(grown != NULL, "oom ns array");
        a->ns_recs = grown;
        a->ns_cap  = newcap;
    }

    rec = calloc(1, sizeof(*rec));
    FACADE_CHECK(rec != NULL, "oom ns record");
    rec->key      = facade_ns_dup(key, klen);
    rec->key_len  = klen;
    rec->val      = facade_ns_dup(val, vlen);
    rec->val_len  = vlen;
    rec->revision = revision;

    memmove(&a->ns_recs[idx + 1], &a->ns_recs[idx],
            (size_t) (a->nns - idx) * sizeof(*a->ns_recs));
    a->ns_recs[idx] = rec;
    a->nns++;
    return 0;
} /* facade_ns_apply_put */

SYMBOL_EXPORT void
facade_ns_apply_delete(
    struct facade_alloc *a,
    const uint8_t       *key,
    uint32_t             klen)
{
    uint32_t              idx;
    struct facade_ns_rec *rec;

    if (!facade_ns_lower_bound(a, key, klen, &idx)) {
        return;
    }
    rec = a->ns_recs[idx];
    free(rec->key);
    free(rec->val);
    free(rec);
    memmove(&a->ns_recs[idx], &a->ns_recs[idx + 1],
            (size_t) (a->nns - idx - 1) * sizeof(*a->ns_recs));
    a->nns--;
} /* facade_ns_apply_delete */

SYMBOL_EXPORT void
facade_ns_destroy_all(struct facade_alloc *a)
{
    uint32_t i;

    for (i = 0; i < a->nns; i++) {
        free(a->ns_recs[i]->key);
        free(a->ns_recs[i]->val);
        free(a->ns_recs[i]);
    }
    free(a->ns_recs);
    a->ns_recs = NULL;
    a->nns     = 0;
    a->ns_cap  = 0;
} /* facade_ns_destroy_all */

SYMBOL_EXPORT uint32_t
facade_ns_budget_for(uint64_t nblocks)
{
    uint64_t b = FACADE_NS_BUDGET;

    if (b > nblocks / 16) {
        b = nblocks / 16;
    }
    if (b == 0) {
        b = 1;
    }
    return (uint32_t) b;
} /* facade_ns_budget_for */

/* ================================================================== */
/* checkpoint (de)serialize                                           */
/* ================================================================== */

SYMBOL_EXPORT uint64_t
facade_ns_ckpt_bytes(const struct facade_alloc *a)
{
    uint64_t bytes = 0;
    uint32_t i;

    for (i = 0; i < a->nns; i++) {
        bytes += sizeof(struct facade_ns_ckpt_rec)
            + a->ns_recs[i]->key_len + a->ns_recs[i]->val_len;
    }
    return bytes;
} /* facade_ns_ckpt_bytes */

SYMBOL_EXPORT void
facade_ns_ckpt_serialize(
    struct facade_alloc *a,
    uint8_t            **pp)
{
    uint8_t *p = *pp;
    uint32_t i;

    for (i = 0; i < a->nns; i++) {
        struct facade_ns_rec     *r = a->ns_recs[i];
        struct facade_ns_ckpt_rec rec;

        memset(&rec, 0, sizeof(rec));
        rec.revision = r->revision;
        rec.key_len  = r->key_len;
        rec.val_len  = r->val_len;
        memcpy(p, &rec, sizeof(rec));
        p += sizeof(rec);
        if (r->key_len) {
            memcpy(p, r->key, r->key_len);
            p += r->key_len;
        }
        if (r->val_len) {
            memcpy(p, r->val, r->val_len);
            p += r->val_len;
        }
    }
    *pp = p;
} /* facade_ns_ckpt_serialize */

SYMBOL_EXPORT int
facade_ns_ckpt_load(
    struct facade_alloc *a,
    const uint8_t      **pp,
    const uint8_t       *end,
    uint64_t             n_ns)
{
    const uint8_t *p = *pp;
    uint64_t       i;

    for (i = 0; i < n_ns; i++) {
        struct facade_ns_ckpt_rec rec;
        const uint8_t            *key, *val;

        if (p + sizeof(rec) > end) {
            return -1;
        }
        memcpy(&rec, p, sizeof(rec));
        p += sizeof(rec);
        if (rec.key_len > FACADE_NS_MAX_KEY || rec.val_len > FACADE_NS_MAX_VALUE ||
            p + (uint64_t) rec.key_len + rec.val_len > end) {
            return -1;
        }
        key = p;
        p  += rec.key_len;
        val = p;
        p  += rec.val_len;
        if (facade_ns_apply_put(a, key, rec.key_len, val, rec.val_len,
                                rec.revision) != 0) {
            return -1;
        }
    }
    *pp = p;
    return 0;
} /* facade_ns_ckpt_load */

/* ================================================================== */
/* scan cursor table (H1)                                             */
/* ================================================================== */

SYMBOL_EXPORT void
facade_ns_cursor_table_init(struct facade_ns_cursor_table *t)
{
    memset(t, 0, sizeof(*t));
    t->next_id = 1;
} /* facade_ns_cursor_table_init */

SYMBOL_EXPORT void
facade_ns_cursor_table_destroy(struct facade_ns_cursor_table *t)
{
    uint32_t i;

    for (i = 0; i < FACADE_NS_CURSOR_SLOTS; i++) {
        free(t->slots[i].key);
        t->slots[i].key = NULL;
        t->slots[i].id  = 0;
    }
} /* facade_ns_cursor_table_destroy */

/* Intern a long key and return its opaque id (reusing an empty or LRU slot). */
static uint64_t
facade_ns_cursor_intern(
    struct facade_ns_cursor_table *t,
    const uint8_t                 *key,
    uint32_t                       klen)
{
    uint32_t i, victim = 0;
    uint64_t best_lru = ~0ULL;
    uint64_t id;

    for (i = 0; i < FACADE_NS_CURSOR_SLOTS; i++) {
        if (t->slots[i].id == 0) {
            victim = i;
            break;
        }
        if (t->slots[i].lru < best_lru) {
            best_lru = t->slots[i].lru;
            victim   = i;
        }
    }
    free(t->slots[victim].key);
    t->slots[victim].key     = facade_ns_dup(key, klen);
    t->slots[victim].key_len = klen;
    id                       = t->next_id++;
    if (t->next_id == 0) {
        t->next_id = 1;   /* never hand out 0 */
    }
    t->slots[victim].id  = id;
    t->slots[victim].lru = ++t->tick;
    return id;
} /* facade_ns_cursor_intern */

/* Look up a long-cursor id; returns the slot (and touches its LRU) or NULL. */
static struct facade_ns_cursor_slot *
facade_ns_cursor_lookup(
    struct facade_ns_cursor_table *t,
    uint64_t                       id)
{
    uint32_t i;

    if (id == 0) {
        return NULL;
    }
    for (i = 0; i < FACADE_NS_CURSOR_SLOTS; i++) {
        if (t->slots[i].id == id) {
            t->slots[i].lru = ++t->tick;
            return &t->slots[i];
        }
    }
    return NULL;
} /* facade_ns_cursor_lookup */

static void
facade_ns_cursor_encode(
    struct facade_ns_cursor_table *t,
    const uint8_t                 *key,
    uint32_t                       klen,
    uint8_t                        out[32])
{
    memset(out, 0, 32);
    if (klen <= FACADE_NS_CURSOR_INLINE_MAX) {
        out[0] = 1;
        out[1] = (uint8_t) klen;
        if (klen) {
            memcpy(out + 2, key, klen);
        }
    } else {
        uint64_t id = facade_ns_cursor_intern(t, key, klen);

        out[0] = 2;
        memcpy(out + 1, &id, sizeof(id));
    }
} /* facade_ns_cursor_encode */

/* ================================================================== */
/* NS_GET / NS_SCAN                                                   */
/* ================================================================== */

SYMBOL_EXPORT uint32_t
facade_ns_get(
    struct facade_alloc *a,
    const uint8_t       *key,
    uint32_t             klen,
    const uint8_t      **r_val,
    uint32_t            *r_vlen,
    uint64_t            *r_rev)
{
    struct facade_ns_rec *rec = facade_ns_find(a, key, klen);

    if (!rec) {
        return CHIMERA_SC_E_NOT_FOUND;
    }
    *r_val  = rec->val;
    *r_vlen = rec->val_len;
    *r_rev  = rec->revision;
    return CHIMERA_SC_OK;
} /* facade_ns_get */

static int
facade_ns_prefix_match(
    const struct facade_ns_rec *r,
    const uint8_t              *prefix,
    uint32_t                    plen)
{
    if (plen == 0) {
        return 1;
    }
    return r->key_len >= plen && memcmp(r->key, prefix, plen) == 0;
} /* facade_ns_prefix_match */

SYMBOL_EXPORT uint32_t
facade_ns_scan(
    struct facade_alloc           *a,
    struct facade_ns_cursor_table *ct,
    const uint8_t                 *prefix,
    uint32_t                       plen,
    const uint8_t                  cur_in[32],
    struct chimera_sc_record      *r_recs,
    uint32_t                       cap,
    uint32_t                      *r_n,
    uint8_t                        cur_out[32],
    uint32_t                      *r_eof)
{
    uint32_t start = 0, i, n = 0;

    *r_n   = 0;
    *r_eof = 0;
    memset(cur_out, 0, 32);

    if (cur_in[0] == 0) {
        facade_ns_lower_bound(a, prefix, plen, &start);     /* first key >= prefix */
    } else if (cur_in[0] == 1) {
        uint32_t klen = cur_in[1];
        int      found;

        found = facade_ns_lower_bound(a, cur_in + 2, klen, &start);
        if (found) {
            start++;                                         /* resume strictly after */
        }
    } else if (cur_in[0] == 2) {
        uint64_t                      id;
        struct facade_ns_cursor_slot *slot;
        int                           found;

        memcpy(&id, cur_in + 1, sizeof(id));
        slot = ct ? facade_ns_cursor_lookup(ct, id) : NULL;
        if (!slot) {
            return CHIMERA_SC_E_INVALID;   /* foreign / evicted cursor: never a wrong page */
        }
        found = facade_ns_lower_bound(a, slot->key, slot->key_len, &start);
        if (found) {
            start++;
        }
    } else {
        return CHIMERA_SC_E_INVALID;
    }

    for (i = start; i < a->nns && n < cap; i++) {
        struct facade_ns_rec *r = a->ns_recs[i];

        if (!facade_ns_prefix_match(r, prefix, plen)) {
            break;
        }
        r_recs[n].key.data       = r->key;
        r_recs[n].key.len        = r->key_len;
        r_recs[n].key.reserved   = 0;
        r_recs[n].value.data     = r->val;
        r_recs[n].value.len      = r->val_len;
        r_recs[n].value.reserved = 0;
        r_recs[n].revision       = r->revision;
        n++;
    }
    *r_n = n;

    if (n > 0 && i < a->nns && facade_ns_prefix_match(a->ns_recs[i], prefix, plen)) {
        struct facade_ns_rec *last = a->ns_recs[start + n - 1];

        facade_ns_cursor_encode(ct, last->key, last->key_len, cur_out);
        *r_eof = 0;
    } else {
        *r_eof = 1;
    }
    return CHIMERA_SC_OK;
} /* facade_ns_scan */

/* ================================================================== */
/* NS_TXN evaluator + durable builder                                 */
/* ================================================================== */

/* staged per-object effect, overlaid on the live object table. */
struct ns_stage_obj {
    uint8_t                  id[16];
    int                      present;   /* exists and not released */
    int                      changed;   /* survivor whose scalars changed: emit an OBJ record */
    int                      released;  /* was present, now released: remove on commit */
    uint32_t                 refs;
    uint32_t                 held;
    uint32_t                 state;
    uint32_t                 ret_mode;
    uint64_t                 ret_until;
    struct chimera_sc_stamps stamps;
};

/* one record to emit, in order (NS records, then OBJ records, then COMMIT). */
struct ns_emit {
    int            is_obj;
    /* NS record */
    uint32_t       op;
    const uint8_t *key;
    uint32_t       klen;
    const uint8_t *val;
    uint32_t       vlen;
    uint64_t       rev;
    /* OBJ record */
    void          *payload;
    uint32_t       len;
};

struct ns_txn_ctx {
    struct facade_alloc                 *a;
    const struct chimera_sc_ns_txn_args *args;
    facade_ns_txn_cb                     cb;
    void                                *priv;
    struct facade_ns_txn_result          res;

    struct ns_stage_obj                 *stage;
    uint32_t                             nstage;

    struct ns_emit                      *emit;
    uint32_t                             nemit;

    uint32_t                             max_recs;   /* upper bound (gate + G1) */

    /* append state */
    uint64_t                             txn_id, pre_seq, pre_log_next;
    uint32_t                             cur, nrecs;
    int                                  txn_started;
    void                                *rec_buf;
};

static void
ns_txn_free(struct ns_txn_ctx *c)
{
    uint32_t i;

    for (i = 0; i < c->nemit; i++) {
        free(c->emit[i].payload);
    }
    free(c->emit);
    free(c->stage);
    free(c->rec_buf);
    free(c);
} /* ns_txn_free */

static void
ns_txn_report(
    struct ns_txn_ctx *c,
    int                release_fifo)
{
    struct facade_alloc        *a   = c->a;
    facade_ns_txn_cb            cb  = c->cb;
    void                       *cp  = c->priv;
    struct facade_ns_txn_result res = c->res;

    ns_txn_free(c);
    cb(&res, cp);
    if (release_fifo) {
        facade_txn_release(a);
    }
} /* ns_txn_report */

/* ---- evaluation ---- */

static struct ns_stage_obj *
ns_stage_find(
    struct ns_txn_ctx              *c,
    const struct chimera_sc_obj_id *id)
{
    uint32_t             i;
    struct facade_obj   *o;
    struct ns_stage_obj *st;

    for (i = 0; i < c->nstage; i++) {
        if (memcmp(c->stage[i].id, id->b, 16) == 0) {
            return &c->stage[i];
        }
    }
    st = &c->stage[c->nstage++];
    memset(st, 0, sizeof(*st));
    memcpy(st->id, id->b, 16);
    o = facade_obj_find(c->a, id->b);
    if (o) {
        st->present   = 1;
        st->refs      = o->refs;
        st->held      = (o->flags & FACADE_OBJ_HELD) ? 1u : 0u;
        st->state     = o->state;
        st->ret_mode  = o->retention_mode;
        st->ret_until = o->retention_until_ns;
        st->stamps    = o->stamps;
    }
    return st;
} /* ns_stage_find */

static int
ns_stamps_equal(
    const struct chimera_sc_stamps *x,
    const struct chimera_sc_stamps *y)
{
    return x->policy_set == y->policy_set &&
           x->owner_principal == y->owner_principal &&
           x->origin == y->origin &&
           x->quota_domains.owner == y->quota_domains.owner &&
           x->quota_domains.group == y->quota_domains.group &&
           x->quota_domains.project == y->quota_domains.project &&
           x->quota_domains.valid == y->quota_domains.valid;
} /* ns_stamps_equal */

/* Apply one delta to the staged state.  Returns CHIMERA_SC_OK or an error. */
static uint32_t
ns_apply_delta(
    struct ns_txn_ctx             *c,
    const struct chimera_sc_delta *d)
{
    switch (d->kind) {
        case CHIMERA_SC_DELTA_CREATE: {
            struct ns_stage_obj *st = ns_stage_find(c, &d->create.obj);
            struct facade_obj   *o  = facade_obj_find(c->a, d->create.obj.b);

            if (!st->present) {
                return CHIMERA_SC_E_NOT_FOUND;
            }
            if (st->refs != 0 || st->held) {
                return CHIMERA_SC_E_STATE;   /* must be never-referenced, unheld */
            }
            if (!o || !ns_stamps_equal(&d->create.stamps, &o->stamps)) {
                return CHIMERA_SC_E_INVALID; /* stamps must equal OBJ_CREATE's */
            }
            st->refs    = 1;
            st->changed = 1;
            return CHIMERA_SC_OK;
        }
        case CHIMERA_SC_DELTA_REPLACE: {
            struct ns_stage_obj *nw = ns_stage_find(c, &d->replace.new_obj);
            struct ns_stage_obj *od = ns_stage_find(c, &d->replace.old_obj);

            if (!nw->present || !od->present) {
                return CHIMERA_SC_E_NOT_FOUND;
            }
            if (nw->refs != 0 || nw->held) {
                return CHIMERA_SC_E_STATE;
            }
            if (od->refs != 1) {
                return CHIMERA_SC_E_STATE;
            }
            nw->refs     = 1;
            nw->changed  = 1;
            od->refs     = 0;
            od->present  = 0;
            od->released = 1;
            return CHIMERA_SC_OK;
        }
        case CHIMERA_SC_DELTA_DELETE: {
            struct ns_stage_obj *st = ns_stage_find(c, &d->del.obj);

            if (!st->present) {
                return CHIMERA_SC_E_NOT_FOUND;
            }
            if (st->refs != 1) {
                return CHIMERA_SC_E_STATE;
            }
            st->refs     = 0;
            st->present  = 0;
            st->released = 1;
            return CHIMERA_SC_OK;
        }
        case CHIMERA_SC_DELTA_SEAL: {
            struct ns_stage_obj *st = ns_stage_find(c, &d->seal.obj);

            if (!st->present) {
                return CHIMERA_SC_E_NOT_FOUND;
            }
            if (st->state != CHIMERA_SC_OBJ_OPEN) {
                return CHIMERA_SC_E_STATE;
            }
            st->state   = CHIMERA_SC_OBJ_SEALED;
            st->changed = 1;
            return CHIMERA_SC_OK;
        }
        case CHIMERA_SC_DELTA_RETAG: {
            struct ns_stage_obj *st = ns_stage_find(c, &d->retag.obj);

            if (!st->present) {
                return CHIMERA_SC_E_NOT_FOUND;
            }
            st->stamps.policy_set    = d->retag.policy_set;
            st->stamps.quota_domains = d->retag.quota_domains;
            st->changed              = 1;
            return CHIMERA_SC_OK;
        }
        case CHIMERA_SC_DELTA_RECALL_HINT:
            return CHIMERA_SC_OK;   /* advisory; never fails (no CAP_GROUP_RECALL) */
        case CHIMERA_SC_DELTA_HOLD: {
            struct ns_stage_obj *st = ns_stage_find(c, &d->hold.obj);

            if (!st->present || st->refs != 0 || st->held) {
                return CHIMERA_SC_E_STATE;   /* needs a never-referenced, unheld object */
            }
            st->held    = 1;
            st->changed = 1;
            return CHIMERA_SC_OK;
        }
        case CHIMERA_SC_DELTA_UNHOLD: {
            struct ns_stage_obj *st = ns_stage_find(c, &d->hold.obj);

            if (!st->present || !st->held) {
                return CHIMERA_SC_E_STATE;
            }
            st->held    = 0;
            st->changed = 1;
            return CHIMERA_SC_OK;
        }
        default:
            return CHIMERA_SC_E_INVALID;
    } /* switch */
} /* ns_apply_delta */

/* Build the OBJ record payload for a staged survivor from the live object's
 * serialized header, patched with the staged scalar effects (H2: serialized from
 * the shadow, not applied to live until commit) plus the live extents. */
static void
ns_build_obj_payload(
    struct ns_txn_ctx         *c,
    const struct ns_stage_obj *st,
    struct ns_emit            *e)
{
    struct facade_obj    *o = facade_obj_find(c->a, st->id);
    struct facade_obj_rec hdr;
    uint8_t              *p;

    FACADE_CHECK(o != NULL, "staged survivor names a missing object");
    facade_obj_serialize_hdr(o, &hdr);
    hdr.refs  = st->refs;
    hdr.flags = st->held ? (o->flags | FACADE_OBJ_HELD)
                                  : (o->flags & ~FACADE_OBJ_HELD);
    hdr.state              = st->state;
    hdr.policy_set         = st->stamps.policy_set;
    hdr.qd_owner           = st->stamps.quota_domains.owner;
    hdr.qd_group           = st->stamps.quota_domains.group;
    hdr.qd_project         = st->stamps.quota_domains.project;
    hdr.qd_valid           = st->stamps.quota_domains.valid;
    hdr.owner_principal    = st->stamps.owner_principal;
    hdr.origin             = st->stamps.origin;
    hdr.retention_mode     = st->ret_mode;
    hdr.retention_until_ns = st->ret_until;

    e->is_obj = 1;
    e->len    = (uint32_t) (sizeof(hdr) + (uint64_t) hdr.nextents
                            * sizeof(struct facade_ckpt_objext));
    e->payload = calloc(1, e->len ? e->len : 1);
    FACADE_CHECK(e->payload != NULL, "oom ns obj payload");
    memcpy(e->payload, &hdr, sizeof(hdr));
    p = (uint8_t *) e->payload + sizeof(hdr);
    facade_ckpt_serialize_objext(o->map, &p);
} /* ns_build_obj_payload */

/* Evaluate the whole transaction into the staged state and the emit list.
 * Returns CHIMERA_SC_OK (emit list built, nothing applied to live yet) or an
 * error (res.r_failed_part/index set). */
static uint32_t
ns_evaluate(struct ns_txn_ctx *c)
{
    const struct chimera_sc_ns_txn_args *t = c->args;
    uint32_t                             i, ns_n = 0, obj_n = 0;
    int64_t                              net = 0;

    /* 1. compares, in order. */
    for (i = 0; i < t->ncompares; i++) {
        const struct chimera_sc_compare *cmp = &t->compares[i];
        struct facade_ns_rec            *rec = facade_ns_find(c->a, cmp->key.data,
                                                              cmp->key.len);
        uint64_t                         have = rec ? rec->revision : 0;

        if (have != cmp->revision) {
            c->res.r_failed_part  = CHIMERA_SC_TXN_COMPARE;
            c->res.r_failed_index = i;
            return CHIMERA_SC_E_COMPARE_FAILED;
        }
    }

    /* 2. deltas, in order, against the intermediate staged state. */
    for (i = 0; i < t->ndeltas; i++) {
        uint32_t st = ns_apply_delta(c, &t->deltas[i]);

        if (st != CHIMERA_SC_OK) {
            c->res.r_failed_part  = CHIMERA_SC_TXN_DELTA;
            c->res.r_failed_index = i;
            return st;
        }
    }

    /* 3. net invariant: never both referenced and held. */
    for (i = 0; i < c->nstage; i++) {
        struct ns_stage_obj *st = &c->stage[i];

        if (st->present && st->refs > 0 && st->held) {
            c->res.r_failed_part  = CHIMERA_SC_TXN_DELTA;
            c->res.r_failed_index = 0;
            return CHIMERA_SC_E_STATE;
        }
    }

    /* 4. record budget: projected live count after puts/deletes. */
    for (i = 0; i < t->nputs; i++) {
        if (!facade_ns_find(c->a, t->puts[i].key.data, t->puts[i].key.len)) {
            net++;
        }
    }
    for (i = 0; i < t->ndeletes; i++) {
        if (facade_ns_find(c->a, t->deletes[i].data, t->deletes[i].len)) {
            net--;
        }
    }
    if ((int64_t) c->a->nns + net > (int64_t) c->a->ns_budget) {
        c->res.r_failed_part  = CHIMERA_SC_TXN_PUT;
        c->res.r_failed_index = 0;
        return CHIMERA_SC_E_NO_SPACE;
    }

    /* 5. build the emit list: NS records, then OBJ records for survivors. */
    c->emit = calloc(c->max_recs, sizeof(*c->emit));
    FACADE_CHECK(c->emit != NULL, "oom ns emit list");

    for (i = 0; i < t->nputs; i++) {
        struct facade_ns_rec *rec = facade_ns_find(c->a, t->puts[i].key.data,
                                                   t->puts[i].key.len);
        struct ns_emit       *e = &c->emit[ns_n++];

        e->op             = FACADE_LR_NS_PUT;
        e->key            = t->puts[i].key.data;
        e->klen           = t->puts[i].key.len;
        e->val            = t->puts[i].value.data;
        e->vlen           = t->puts[i].value.len;
        e->rev            = rec ? rec->revision + 1 : 1;
        c->res.r_revision = e->rev;
    }
    for (i = 0; i < t->ndeletes; i++) {
        struct ns_emit *e = &c->emit[ns_n++];

        e->op   = FACADE_LR_NS_DELETE;
        e->key  = t->deletes[i].data;
        e->klen = t->deletes[i].len;
    }
    c->nemit = ns_n;
    for (i = 0; i < c->nstage; i++) {
        if (c->stage[i].changed && c->stage[i].present) {
            ns_build_obj_payload(c, &c->stage[i], &c->emit[c->nemit++]);
            obj_n++;
        }
    }
    FACADE_CHECK(c->nemit <= c->max_recs, "ns emit overflow");
    (void) obj_n;
    return CHIMERA_SC_OK;
} /* ns_evaluate */

/* ---- commit application ---- */

static void
ns_release_free_cb(
    chimera_media_block_t phys,
    uint32_t              nblocks,
    void                 *priv)
{
    facade_alloc_free((struct facade_alloc *) priv, phys, nblocks);
} /* ns_release_free_cb */

static void
ns_txn_apply_commit(struct ns_txn_ctx *c)
{
    struct facade_alloc *a = c->a;
    uint32_t             i;

    /* NS record puts/deletes. */
    for (i = 0; i < c->nemit; i++) {
        struct ns_emit *e = &c->emit[i];

        if (e->is_obj) {
            continue;
        }
        if (e->op == FACADE_LR_NS_PUT) {
            int rc = facade_ns_apply_put(a, e->key, e->klen, e->val, e->vlen, e->rev);

            FACADE_CHECK(rc == 0, "ns put over budget at commit (checked in eval)");
        } else {
            facade_ns_apply_delete(a, e->key, e->klen);
        }
    }

    /* object scalar changes + releases. */
    for (i = 0; i < c->nstage; i++) {
        struct ns_stage_obj *st = &c->stage[i];
        struct facade_obj   *o  = facade_obj_find(a, st->id);

        if (!o) {
            continue;
        }
        if (st->released) {
            uint64_t max_blk = facade_extent_map_max_logical(o->map);

            if (max_blk > 0) {
                facade_extent_map_punch(o->map, 0, (uint32_t) max_blk,
                                        ns_release_free_cb, a);
            }
            facade_obj_remove(a, o);
        } else if (st->changed) {
            o->refs  = st->refs;
            o->flags = st->held ? (o->flags | FACADE_OBJ_HELD)
                                  : (o->flags & ~FACADE_OBJ_HELD);
            o->state              = st->state;
            o->stamps             = st->stamps;
            o->retention_mode     = st->ret_mode;
            o->retention_until_ns = st->ret_until;
        }
    }
} /* ns_txn_apply_commit */

/* ---- durable append state machine ---- */

static void ns_txn_append_next(
    struct ns_txn_ctx *c);

static void
ns_txn_finish_ok(struct ns_txn_ctx *c)
{
    struct facade_alloc *a = c->a;

    ns_txn_apply_commit(c);
    a->durable_seq   = c->txn_id + c->nrecs - 1;
    a->durable_epoch = a->cur_epoch;
    c->res.status    = CHIMERA_SC_OK;
    ns_txn_report(c, 1);
} /* ns_txn_finish_ok */

static void
ns_txn_fail_appended(
    struct ns_txn_ctx *c,
    uint32_t           status)
{
    struct facade_alloc *a = c->a;

    /* Rewind the log cursors this uncommitted txn advanced; nothing was applied
     * to live memory, so there is no state to roll back. */
    if (c->txn_started) {
        a->next_seq = c->pre_seq;
        a->log_next = c->pre_log_next;
    }
    c->res.status = status;
    ns_txn_report(c, 1);
} /* ns_txn_fail_appended */

static void
ns_txn_append_done(
    enum facade_status st,
    void              *priv)
{
    struct ns_txn_ctx *c = priv;

    if (st != FACADE_OK) {
        ns_txn_fail_appended(c, CHIMERA_SC_E_IO);
        return;
    }
    if (c->a->shutting_down) {
        ns_txn_fail_appended(c, CHIMERA_SC_E_FENCED);
        return;
    }
    c->cur++;
    if (c->cur < c->nrecs) {
        ns_txn_append_next(c);
        return;
    }
    ns_txn_finish_ok(c);   /* the COMMIT (last record) is durable */
} /* ns_txn_append_done */

static void
ns_txn_append_next(struct ns_txn_ctx *c)
{
    struct facade_alloc *a    = c->a;
    uint64_t             seq  = a->next_seq;
    uint64_t             slot = a->log_next;

    FACADE_CHECK(slot < a->log_nblocks, "ns log overflow past compaction");
    if (c->cur == 0) {
        c->txn_id = seq;
    }
    a->next_seq++;
    a->log_next++;

    if (c->cur < c->nemit) {
        struct ns_emit *e = &c->emit[c->cur];

        if (e->is_obj) {
            facade_lr_build(c->rec_buf, FACADE_LR_OBJ, seq, c->txn_id,
                            e->payload, e->len);
        } else {
            uint8_t             payload[sizeof(struct facade_lr_ns)
                                        + FACADE_NS_MAX_KEY + FACADE_NS_MAX_VALUE];
            struct facade_lr_ns nh;
            uint8_t            *p = payload;
            uint32_t            len;

            memset(&nh, 0, sizeof(nh));
            nh.revision = e->rev;
            nh.key_len  = e->klen;
            nh.val_len  = e->vlen;
            nh.op       = e->op;
            memcpy(p, &nh, sizeof(nh));
            p += sizeof(nh);
            if (e->klen) {
                memcpy(p, e->key, e->klen);
                p += e->klen;
            }
            if (e->vlen) {
                memcpy(p, e->val, e->vlen);
                p += e->vlen;
            }
            len = (uint32_t) (p - payload);
            facade_lr_build(c->rec_buf, FACADE_LR_NS, seq, c->txn_id, payload, len);
        }
    } else {
        struct facade_lr_commit commit = { c->txn_id, c->nrecs };

        facade_lr_build(c->rec_buf, FACADE_LR_COMMIT, seq, c->txn_id,
                        &commit, (uint32_t) sizeof(commit));
    }
    facade_media_write(a, c->rec_buf, a->log_block + slot, 1,
                       CHIMERA_MEDIA_WRITE_FUA, ns_txn_append_done, c);
} /* ns_txn_append_next */

/* ---- starter: evaluate under the slot, then append ---- */

static void
ns_txn_run(struct ns_txn_ctx *c)
{
    uint32_t st;

    if (c->a->shutting_down) {
        ns_txn_fail_appended(c, CHIMERA_SC_E_FENCED);
        return;
    }
    st = ns_evaluate(c);
    if (st != CHIMERA_SC_OK) {
        /* Nothing appended, nothing applied: report and release. */
        c->res.status = st;
        ns_txn_report(c, 1);
        return;
    }

    c->rec_buf = calloc(1, CHIMERA_MEDIA_BLOCK_SIZE);
    FACADE_CHECK(c->rec_buf != NULL, "oom ns rec buf");
    c->txn_started  = 1;
    c->pre_seq      = c->a->next_seq;
    c->pre_log_next = c->a->log_next;
    c->nrecs        = c->nemit + 1u;   /* records + COMMIT */
    c->cur          = 0;
    ns_txn_append_next(c);
} /* ns_txn_run */

static void
ns_txn_remark_done(
    enum facade_status st,
    void              *priv)
{
    struct ns_txn_ctx *c = priv;

    if (st != FACADE_OK) {
        ns_txn_fail_appended(c, CHIMERA_SC_E_IO);
        return;
    }
    ns_txn_run(c);
} /* ns_txn_remark_done */

static void
ns_txn_compacted(
    enum facade_status st,
    void              *priv)
{
    struct ns_txn_ctx *c = priv;

    if (st != FACADE_OK) {
        ns_txn_fail_appended(c, CHIMERA_SC_E_IO);
        return;
    }
    /* Compaction set clean=1; re-arm clean=0 before appending post-compaction. */
    facade_mark_dirty(c->a, ns_txn_remark_done, c);
} /* ns_txn_compacted */

static void
ns_txn_dirty_done(
    enum facade_status st,
    void              *priv)
{
    struct ns_txn_ctx   *c = priv;
    struct facade_alloc *a = c->a;

    if (st != FACADE_OK) {
        ns_txn_fail_appended(c, CHIMERA_SC_E_IO);
        return;
    }
    if (a->shutting_down) {
        ns_txn_fail_appended(c, CHIMERA_SC_E_FENCED);
        return;
    }
    /* G1 (C1): if the worst case will not fit the remaining log, compact first at
     * the transaction boundary, before any append. */
    if (a->log_next + c->max_recs > a->log_nblocks) {
        facade_compact(a, ns_txn_compacted, c);
        return;
    }
    ns_txn_run(c);
} /* ns_txn_dirty_done */

static void
ns_txn_start(void *arg)
{
    struct ns_txn_ctx   *c = arg;
    struct facade_alloc *a = c->a;

    if (a->shutting_down) {
        ns_txn_fail_appended(c, CHIMERA_SC_E_FENCED);
        return;
    }
    /* Size bound: a txn that cannot fit an empty log is refused (NO_SPACE). */
    if (c->max_recs > a->log_nblocks) {
        c->res.status = CHIMERA_SC_E_NO_SPACE;
        ns_txn_report(c, 1);
        return;
    }
    /* C1(a): arm the dirty window (clean=0 durable) before the first log record. */
    facade_mark_dirty(a, ns_txn_dirty_done, c);
} /* ns_txn_start */

SYMBOL_EXPORT void
facade_ns_txn(
    struct facade_alloc                 *a,
    const struct chimera_sc_ns_txn_args *args,
    facade_ns_txn_cb                     cb,
    void                                *private_data)
{
    struct ns_txn_ctx          *c;
    struct facade_ns_txn_result bad = { CHIMERA_SC_E_INVALID, 0, 0, 0 };
    uint32_t                    i;
    uint64_t                    max_recs;

    /* Synchronous validation (before taking the FIFO slot). */
    for (i = 0; i < args->nputs; i++) {
        if (args->puts[i].key.len == 0 || args->puts[i].key.len > FACADE_NS_MAX_KEY ||
            args->puts[i].value.len > FACADE_NS_MAX_VALUE) {
            bad.r_failed_part  = CHIMERA_SC_TXN_PUT;
            bad.r_failed_index = i;
            cb(&bad, private_data);
            return;
        }
    }
    for (i = 0; i < args->ncompares; i++) {
        if (args->compares[i].key.len > FACADE_NS_MAX_KEY) {
            bad.r_failed_part  = CHIMERA_SC_TXN_COMPARE;
            bad.r_failed_index = i;
            cb(&bad, private_data);
            return;
        }
    }
    for (i = 0; i < args->ndeletes; i++) {
        if (args->deletes[i].len == 0 || args->deletes[i].len > FACADE_NS_MAX_KEY) {
            bad.r_failed_part  = CHIMERA_SC_TXN_DELETE;
            bad.r_failed_index = i;
            cb(&bad, private_data);
            return;
        }
    }

    /* Upper bound on log blocks: NS records + at most two OBJ records per delta
     * + COMMIT.  The gate and G1 use this bound (the object-txn pattern). */
    max_recs = (uint64_t) args->nputs + args->ndeletes
        + (uint64_t) args->ndeltas * 2u + 1u;
    if (max_recs > FACADE_NS_MAX_TXN_LOG_BLOCKS) {
        bad.status        = CHIMERA_SC_E_NO_SPACE;
        bad.r_failed_part = CHIMERA_SC_TXN_PUT;
        cb(&bad, private_data);
        return;
    }

    c = calloc(1, sizeof(*c));
    FACADE_CHECK(c != NULL, "oom ns txn ctx");
    c->a          = a;
    c->args       = args;
    c->cb         = cb;
    c->priv       = private_data;
    c->max_recs   = (uint32_t) max_recs;
    c->res.status = CHIMERA_SC_OK;
    if (args->ndeltas) {
        c->stage = calloc((size_t) args->ndeltas * 2u, sizeof(*c->stage));
        FACADE_CHECK(c->stage != NULL, "oom ns stage");
    }

    facade_txn_acquire(a, ns_txn_start, c);
} /* facade_ns_txn */
