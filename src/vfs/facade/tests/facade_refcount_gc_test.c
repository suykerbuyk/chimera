// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * refcount-and-gc unit tests (quick tier, in-RAM facade_test_media; no evpl,
 * I/O or privilege).  Covers, over the facade_ns NS_TXN path:
 *
 *   - per-event refcount + collection: CREATE -> refs 1; DELETE / REPLACE-old /
 *     rename-replace -> the object is collected (removed, owned blocks freed);
 *   - the retention refuse-before-emit guard (mode-first lapse rule) + a lapse;
 *   - the two-part collect-extent gate (<= FACADE_NS_MAX_TXN_COLLECT_EXT owned
 *     runs; over-cap -> NO_SPACE with nothing applied; at-cap commits);
 *   - crash durability of the tombstone: a committed collection survives
 *     abandon+reopen (object gone, binding gone, REPLACE survivor intact), and a
 *     second reopen (post-recovery checkpoint) is stable (one-apply, not a
 *     literal double-apply).
 *
 * Owned-extent FREE accounting is checked on the LIVE path (the white-box alloc
 * used here logs no ALLOC record, so a crash/reopen cannot be free-set
 * consistent for owned blocks); the owned-run FREE *replay* across a real crash
 * is exercised by the async object-op crash tests and the E3
 * conformance-txn-atomicity deep arm, which drive the logged ALLOC+OBJ path.
 */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "facade_test_util.h"
#include "facade_internal.h"
#include "facade_ns.h"
#include "facade_extent.h"
#include "facade_super.h"

/* ---- NS_TXN driver (the in-RAM fixture completes inline) ---- */

struct txn_w {
    int                         done;
    struct facade_ns_txn_result res;
};

static void
on_txn(
    const struct facade_ns_txn_result *r,
    void                              *priv)
{
    struct txn_w *w = priv;

    w->res  = *r;
    w->done = 1;
} /* on_txn */

static struct facade_ns_txn_result
run_txn(
    struct facade_alloc           *a,
    struct chimera_sc_ns_txn_args *args)
{
    struct txn_w w = { 0, { 0, 0, 0, 0 } };

    facade_ns_txn(a, args, on_txn, &w);
    TCHECK(w.done, "ns txn did not complete inline");
    return w.res;
} /* run_txn */

/* Create an OPEN object entry with zero stamps. */
static void
mk_obj(
    struct facade_alloc *a,
    const uint8_t        id[16])
{
    struct chimera_sc_stamps stamps;

    memset(&stamps, 0, sizeof(stamps));
    TCHECK(facade_obj_create_entry(a, id, &stamps) != NULL, "create obj entry");
} /* mk_obj */

/* Bind an object: a CREATE delta drives refs 0 -> 1 (stamps must equal the
 * entry's zero stamps). */
static uint32_t
bind_obj(
    struct facade_alloc *a,
    const uint8_t        id[16])
{
    struct chimera_sc_delta       d;
    struct chimera_sc_ns_txn_args args;

    memset(&d, 0, sizeof(d));
    d.kind = CHIMERA_SC_DELTA_CREATE;
    memcpy(d.create.obj.b, id, 16);
    memset(&d.create.stamps, 0, sizeof(d.create.stamps));
    memset(&args, 0, sizeof(args));
    args.deltas = &d; args.ndeltas = 1;
    return run_txn(a, &args).status;
} /* bind_obj */

/* Delete a binding: a DELETE delta drives refs 1 -> 0 (collect). */
static uint32_t
del_obj(
    struct facade_alloc *a,
    const uint8_t        id[16])
{
    struct chimera_sc_delta       d;
    struct chimera_sc_ns_txn_args args;

    memset(&d, 0, sizeof(d));
    d.kind = CHIMERA_SC_DELTA_DELETE;
    memcpy(d.del.obj.b, id, 16);
    memset(&args, 0, sizeof(args));
    args.deltas = &d; args.ndeltas = 1;
    return run_txn(a, &args).status;
} /* del_obj */

/* Give an object `n` distinct 1-block OWNED DATA extents (white-box: allocate
 * from the free set and record them in the map).  Returns the free-block count
 * BEFORE the allocation, so the caller can assert the collection returns them. */
static uint64_t
give_owned_extents(
    struct facade_alloc *a,
    const uint8_t        id[16],
    uint32_t             n)
{
    struct facade_obj *o     = facade_obj_find(a, id);
    uint64_t           free0 = facade_alloc_free_blocks(a);
    uint32_t           i;

    TCHECK(o != NULL, "object present for extents");
    for (i = 0; i < n; i++) {
        chimera_media_block_t start;

        TCHECK(facade_alloc_blocks(a, 1, &start, NULL) == FACADE_OK, "alloc blk");
        facade_extent_map_insert(o->map, i, 1, start, CHIMERA_SC_SEG_DATA,
                                 1u /* owned */, 0u, FACADE_DATA_CSUM_NONE,
                                 NULL, NULL);
    }
    return free0;
} /* give_owned_extents */

/* ---- 1. per-event refcount + live collection ---- */

static void
test_events(void)
{
    struct chimera_media_space *space = tu_media_open("gc://ev",
                                                      4096ULL * CHIMERA_MEDIA_BLOCK_SIZE);
    struct facade_alloc        *a;
    const uint8_t               oid[16] = { 0x01 };
    const uint8_t               nid[16] = { 0x02 };

    tu_format(space);
    a = tu_open(space);
    TCHECK(a->log_nblocks == FACADE_NS_MAX_TXN_LOG_BLOCKS, "log floor 128");

    /* CREATE binding -> refs 1. */
    mk_obj(a, oid);
    TCHECK(bind_obj(a, oid) == CHIMERA_SC_OK, "bind ok");
    TCHECK(facade_obj_find(a, oid) && facade_obj_find(a, oid)->refs == 1, "refs==1");

    /* owned-extent collection, live: DELETE frees the blocks and removes the obj. */
    {
        uint64_t free0 = give_owned_extents(a, oid, 8);

        TCHECK(facade_alloc_free_blocks(a) == free0 - 8, "8 blocks allocated");
        TCHECK(del_obj(a, oid) == CHIMERA_SC_OK, "delete ok");
        TCHECK(facade_obj_find(a, oid) == NULL, "object collected (gone)");
        TCHECK(facade_alloc_free_blocks(a) == free0, "owned blocks returned to free");
    }

    /* REPLACE: old collected, new refs 1. */
    mk_obj(a, oid);
    mk_obj(a, nid);
    TCHECK(bind_obj(a, oid) == CHIMERA_SC_OK, "re-bind old");
    {
        uint64_t                      free0 = give_owned_extents(a, oid, 4);
        struct chimera_sc_delta       d;
        struct chimera_sc_ns_txn_args args;

        memset(&d, 0, sizeof(d));
        d.kind = CHIMERA_SC_DELTA_REPLACE;
        memcpy(d.replace.old_obj.b, oid, 16);
        memcpy(d.replace.new_obj.b, nid, 16);
        memset(&args, 0, sizeof(args));
        args.deltas = &d; args.ndeltas = 1;
        TCHECK(run_txn(a, &args).status == CHIMERA_SC_OK, "replace ok");
        TCHECK(facade_obj_find(a, oid) == NULL, "replace: old collected");
        TCHECK(facade_obj_find(a, nid) && facade_obj_find(a, nid)->refs == 1,
               "replace: new refs==1");
        TCHECK(facade_alloc_free_blocks(a) == free0, "replace: old blocks freed");
    }

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
    printf("  events: refcount + live collection ok\n");
} /* test_events */

/* ---- 2. collect-extent gate (over-cap refused, at-cap commits) ---- */

static void
test_collect_cap(void)
{
    struct chimera_media_space *space = tu_media_open("gc://cap",
                                                      4096ULL * CHIMERA_MEDIA_BLOCK_SIZE);
    struct facade_alloc        *a;
    const uint8_t               over[16] = { 0x10 };
    const uint8_t               atc[16]  = { 0x11 };

    tu_format(space);
    a = tu_open(space);

    /* Over-cap: COLLECT_EXT + 1 owned runs -> NO_SPACE in ns_evaluate, nothing
     * applied (object still bound). */
    mk_obj(a, over);
    TCHECK(bind_obj(a, over) == CHIMERA_SC_OK, "bind over");
    give_owned_extents(a, over, FACADE_NS_MAX_TXN_COLLECT_EXT + 1);
    TCHECK(del_obj(a, over) == CHIMERA_SC_E_NO_SPACE, "over-cap collect -> NO_SPACE");
    TCHECK(facade_obj_find(a, over) && facade_obj_find(a, over)->refs == 1,
           "over-cap: nothing applied, still bound");

    /* At cap: exactly COLLECT_EXT owned runs commits and collects. */
    mk_obj(a, atc);
    TCHECK(bind_obj(a, atc) == CHIMERA_SC_OK, "bind atcap");
    {
        uint64_t free0 = give_owned_extents(a, atc, FACADE_NS_MAX_TXN_COLLECT_EXT);

        TCHECK(del_obj(a, atc) == CHIMERA_SC_OK, "at-cap collect ok");
        TCHECK(facade_obj_find(a, atc) == NULL, "at-cap collected");
        TCHECK(facade_alloc_free_blocks(a) == free0, "at-cap blocks freed");
    }

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
    printf("  collect cap: over -> NO_SPACE, at-cap commits ok\n");
} /* test_collect_cap */

/* ---- 3. retention refuse-before-emit + lapse (mode-first) ---- */

static uint64_t g_fake_rt_ns;

static uint64_t
fake_rt(void *arg)
{
    (void) arg;
    return g_fake_rt_ns;
} /* fake_rt */

static void
test_retention(void)
{
    struct chimera_media_space *space = tu_media_open("gc://ret",
                                                      4096ULL * CHIMERA_MEDIA_BLOCK_SIZE);
    struct facade_alloc        *a;
    const uint8_t               tid[16] = { 0x20 };
    const uint8_t               iid[16] = { 0x21 };
    struct facade_obj          *o;

    tu_format(space);
    a = tu_open(space);

    facade_ns_set_retention_clock(fake_rt, NULL);
    g_fake_rt_ns = 1000;

    /* LOCKED with a future deadline: DELETE refused, binding intact. */
    mk_obj(a, tid);
    TCHECK(bind_obj(a, tid) == CHIMERA_SC_OK, "bind ret");
    o                     = facade_obj_find(a, tid);
    o->retention_mode     = CHIMERA_SC_RETENTION_LOCKED;
    o->retention_until_ns = 2000;   /* now (1000) < until -> locked */
    TCHECK(del_obj(a, tid) == CHIMERA_SC_E_RETENTION_LOCKED, "locked -> refused");
    TCHECK(facade_obj_find(a, tid) && facade_obj_find(a, tid)->refs == 1,
           "locked: binding intact");

    /* Lapse: advance the (separate) retention clock past the deadline -> ok. */
    g_fake_rt_ns = 2000;   /* now >= until -> lapsed */
    TCHECK(del_obj(a, tid) == CHIMERA_SC_OK, "lapsed -> collect ok");
    TCHECK(facade_obj_find(a, tid) == NULL, "lapsed: collected");

    /* LOCKED indefinite (until_ns == 0): refused regardless of the clock. */
    mk_obj(a, iid);
    TCHECK(bind_obj(a, iid) == CHIMERA_SC_OK, "bind indef");
    o                     = facade_obj_find(a, iid);
    o->retention_mode     = CHIMERA_SC_RETENTION_LOCKED;
    o->retention_until_ns = 0;      /* indefinite */
    g_fake_rt_ns          = (uint64_t) -1;
    TCHECK(del_obj(a, iid) == CHIMERA_SC_E_RETENTION_LOCKED, "indefinite -> refused");

    /* NONE is never locked (mode-first), even with a nonzero until. */
    o->retention_mode     = CHIMERA_SC_RETENTION_NONE;
    o->retention_until_ns = (uint64_t) -1;
    TCHECK(del_obj(a, iid) == CHIMERA_SC_OK, "NONE never locked -> collect ok");

    facade_ns_set_retention_clock(NULL, NULL);
    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
    printf("  retention: refuse-before-emit + mode-first lapse ok\n");
} /* test_retention */

/* ---- 4. crash durability of the tombstone (empty-map objects) ---- */

static void
test_crash_tombstone(void)
{
    const uint64_t              img   = 4096ULL * CHIMERA_MEDIA_BLOCK_SIZE;
    struct chimera_media_space *space = tu_media_open("gc://crash", img);
    struct facade_alloc        *a;
    const uint8_t               oid[16] = { 0x30 };
    const uint8_t               nid[16] = { 0x31 };
    uint64_t                    free_after_recover;

    tu_format(space);
    a = tu_open(space);

    /* Durable object (its binding CREATE delta logs the OBJ record), then a
     * committed DELETE (logs the OBJ_REMOVE tombstone). */
    mk_obj(a, oid);
    TCHECK(bind_obj(a, oid) == CHIMERA_SC_OK, "bind");
    TCHECK(del_obj(a, oid) == CHIMERA_SC_OK, "delete (commit)");

    /* A survivor that must remain after recovery: bound and not deleted. */
    mk_obj(a, nid);
    TCHECK(bind_obj(a, nid) == CHIMERA_SC_OK, "bind survivor");

    /* Crash + reopen: the committed collection replays (object gone), the
     * survivor is intact. */
    facade_alloc_abandon(a);
    tu_media_close(space);
    space = tu_media_open("gc://crash", img);
    a     = tu_open(space);
    TCHECK(facade_obj_find(a, oid) == NULL, "collected object gone after recover");
    TCHECK(facade_obj_find(a, nid) && facade_obj_find(a, nid)->refs == 1,
           "survivor intact after recover");
    free_after_recover = facade_alloc_free_blocks(a);

    /* One-apply-then-second-open: reopen again loads the post-recovery
     * checkpoint (empty log prefix); state is identical, not a double-apply. */
    facade_alloc_abandon(a);
    tu_media_close(space);
    space = tu_media_open("gc://crash", img);
    a     = tu_open(space);
    TCHECK(facade_obj_find(a, oid) == NULL, "second open: still gone");
    TCHECK(facade_obj_find(a, nid) && facade_obj_find(a, nid)->refs == 1,
           "second open: survivor stable");
    TCHECK(facade_alloc_free_blocks(a) == free_after_recover,
           "second open: free set identical (idempotent)");

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
    printf("  crash: tombstone replay + survivor + idempotent reopen ok\n");
} /* test_crash_tombstone */

int
main(void)
{
    test_events();
    test_collect_cap();
    test_retention();
    test_crash_tombstone();
    printf("facade_refcount_gc_test: ok\n");
    return 0;
} /* main */
