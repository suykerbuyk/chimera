// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * In-RAM unit tests for the namespace-record store (namespace-record-store) over
 * the FUA-honest fixture (facade_test_media), covering:
 *   - exact-key get/put/delete and prefix scan order (M32);
 *   - stable scan cursor across interleaved puts/deletes, incl. a page boundary
 *     inside a shared-prefix group of >30 B keys (the H1 guard) and a
 *     foreign/evicted cursor -> CHIMERA_SC_E_INVALID;
 *   - revisions and the revision-0 (must-not-exist) compare;
 *   - the NS_TXN evaluator: compare mismatch (E_COMPARE_FAILED + r_failed_*),
 *     delta preconditions, the referenced-and-held rejection, rename as one txn,
 *     and the per-txn log-block cap (E_NO_SPACE);
 *   - crash atomicity: a committed multi-put txn survives a crash + reopen; and
 *   - the C1 G1 guard: a txn that forces a compaction still commits and survives.
 *
 * Compiled -UNDEBUG so assert()/TCHECK stay live in every build type.  Links no
 * evpl (the fixture reads only iovec data/length).
 */

#undef NDEBUG
#include <assert.h>
#include <string.h>

#include "evpl/evpl.h"   /* struct evpl_iovec (type only) */

#include "facade_test_util.h"
#include "facade_internal.h"
#include "facade_ns.h"

/* In-RAM fixture size (the backing is allocated eagerly, so keep it modest); a
 * few thousand blocks gives the full ns_budget without a large allocation. */
#define ONE_GIB (16ULL << 20)

/* ---- NS_TXN driver (the fixture completes inline) ---- */

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

static struct facade_ns_txn_result
put_one(
    struct facade_alloc *a,
    const char          *key,
    const char          *val)
{
    struct chimera_sc_record      put;
    struct chimera_sc_ns_txn_args args;

    memset(&put, 0, sizeof(put));
    put.key.data   = (const uint8_t *) key;
    put.key.len    = (uint32_t) strlen(key);
    put.value.data = (const uint8_t *) val;
    put.value.len  = (uint32_t) strlen(val);

    memset(&args, 0, sizeof(args));
    args.puts  = &put;
    args.nputs = 1;
    return run_txn(a, &args);
} /* put_one */

static int
get_eq(
    struct facade_alloc *a,
    const char          *key,
    const char          *val)
{
    const uint8_t *v;
    uint32_t       vl;
    uint64_t       rev;
    uint32_t       st = facade_ns_get(a, (const uint8_t *) key,
                                      (uint32_t) strlen(key), &v, &vl, &rev);

    if (st != CHIMERA_SC_OK) {
        return 0;
    }
    return vl == strlen(val) && memcmp(v, val, vl) == 0;
} /* get_eq */

/* ---- exact key + revisions ---- */

static void
test_exact_key_and_revisions(void)
{
    struct chimera_media_space *space = tu_media_open("ns://exact", ONE_GIB);
    struct facade_alloc        *a;
    const uint8_t              *v;
    uint32_t                    vl;
    uint64_t                    rev;

    tu_format(space);
    a = tu_open(space);

    TCHECK(put_one(a, "a", "1").status == CHIMERA_SC_OK, "put a");
    TCHECK(put_one(a, "ab", "2").status == CHIMERA_SC_OK, "put ab");
    TCHECK(put_one(a, "b", "3").status == CHIMERA_SC_OK, "put b");

    /* M32: "a", "ab" and "b" are distinct records, compared by exact bytes. */
    TCHECK(get_eq(a, "a", "1") && get_eq(a, "ab", "2") && get_eq(a, "b", "3"),
           "exact-key gets");
    TCHECK(facade_ns_get(a, (const uint8_t *) "ac", 2, &v, &vl, &rev)
           == CHIMERA_SC_E_NOT_FOUND, "absent key");

    /* revisions: first put is 1, a replace bumps to 2. */
    TCHECK(facade_ns_get(a, (const uint8_t *) "a", 1, &v, &vl, &rev) == CHIMERA_SC_OK
           && rev == 1, "rev 1");
    TCHECK(put_one(a, "a", "1b").status == CHIMERA_SC_OK, "replace a");
    TCHECK(facade_ns_get(a, (const uint8_t *) "a", 1, &v, &vl, &rev) == CHIMERA_SC_OK
           && rev == 2 && vl == 2, "rev 2");

    /* delete then absent. */
    {
        struct chimera_sc_bytes       del = { (const uint8_t *) "ab", 2, 0 };
        struct chimera_sc_ns_txn_args args;

        memset(&args, 0, sizeof(args));
        args.deletes  = &del;
        args.ndeletes = 1;
        TCHECK(run_txn(a, &args).status == CHIMERA_SC_OK, "delete ab");
    }
    TCHECK(facade_ns_get(a, (const uint8_t *) "ab", 2, &v, &vl, &rev)
           == CHIMERA_SC_E_NOT_FOUND, "ab deleted");

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
} /* test_exact_key_and_revisions */

/* ---- prefix scan order + cursor stability ---- */

static void
test_scan_order_and_cursor(void)
{
    struct chimera_media_space   *space = tu_media_open("ns://scan", ONE_GIB);
    struct facade_alloc          *a;
    struct facade_ns_cursor_table ct;
    struct chimera_sc_record      recs[8];
    uint32_t                      n, eof;
    uint8_t                       cur[32], cur2[32];

    tu_format(space);
    a = tu_open(space);
    facade_ns_cursor_table_init(&ct);

    /* Insert out of order; scan must return exact byte order. */
    put_one(a, "p/3", "c");
    put_one(a, "p/1", "a");
    put_one(a, "q/0", "z");
    put_one(a, "p/2", "b");

    memset(cur, 0, sizeof(cur));
    TCHECK(facade_ns_scan(a, &ct, (const uint8_t *) "p/", 2, cur, recs, 8,
                          &n, cur2, &eof) == CHIMERA_SC_OK, "scan p/");
    TCHECK(n == 3 && eof == 1, "p/ returns 3, eof");
    TCHECK(recs[0].key.len == 3 && memcmp(recs[0].key.data, "p/1", 3) == 0 &&
           memcmp(recs[1].key.data, "p/2", 3) == 0 &&
           memcmp(recs[2].key.data, "p/3", 3) == 0, "sorted p/ order");

    /* Paged scan (cap 1) with an interleaved delete of the last-returned key and
     * an insert before the cursor: the cursor is value-based, so neither perturbs
     * the resume. */
    memset(cur, 0, sizeof(cur));
    TCHECK(facade_ns_scan(a, &ct, (const uint8_t *) "p/", 2, cur, recs, 1,
                          &n, cur2, &eof) == CHIMERA_SC_OK && n == 1 && eof == 0,
           "page1");
    TCHECK(memcmp(recs[0].key.data, "p/1", 3) == 0, "page1 = p/1");
    /* delete the just-returned key and insert one before the cursor. */
    {
        struct chimera_sc_bytes       del = { (const uint8_t *) "p/1", 3, 0 };
        struct chimera_sc_ns_txn_args args;

        memset(&args, 0, sizeof(args));
        args.deletes = &del; args.ndeletes = 1;
        run_txn(a, &args);
    }
    put_one(a, "p/0", "x");   /* sorts before the cursor (p/1); must NOT reappear */
    memcpy(cur, cur2, sizeof(cur));
    TCHECK(facade_ns_scan(a, &ct, (const uint8_t *) "p/", 2, cur, recs, 8,
                          &n, cur2, &eof) == CHIMERA_SC_OK, "page2");
    TCHECK(n == 2 && eof == 1, "page2 returns p/2,p/3");
    TCHECK(memcmp(recs[0].key.data, "p/2", 3) == 0 &&
           memcmp(recs[1].key.data, "p/3", 3) == 0, "resume after p/1");

    facade_ns_cursor_table_destroy(&ct);
    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
} /* test_scan_order_and_cursor */

/* ---- long-key cursor (H1 guard) + foreign cursor ---- */

static void
test_long_key_cursor(void)
{
    struct chimera_media_space   *space = tu_media_open("ns://long", ONE_GIB);
    struct facade_alloc          *a;
    struct facade_ns_cursor_table ct;
    struct chimera_sc_record      recs[8];
    uint32_t                      n, eof, i, total = 0;
    uint8_t                       cur[32], cur2[32];
    char                          key[48];
    /* 10 keys sharing a 36-byte prefix (> 30, so the cursor uses the table),
    * differing only in the last two digits -> exact byte order by suffix. */
    static const char             pfx[] = "longkey/aaaaaaaaaaaaaaaaaaaaaaaaaaa/";

    tu_format(space);
    a = tu_open(space);
    facade_ns_cursor_table_init(&ct);

    for (i = 0; i < 10; i++) {
        snprintf(key, sizeof(key), "%s%02u", pfx, i);
        put_one(a, key, "v");
    }
    TCHECK(strlen(pfx) + 2 > FACADE_NS_CURSOR_INLINE_MAX, "keys exceed inline cursor");

    /* Page through cap 2 at a time; a boundary lands inside the shared-prefix
     * group.  Collect keys and assert exactly the 10, in order, no dup/skip. */
    memset(cur, 0, sizeof(cur));
    for (;;) {
        TCHECK(facade_ns_scan(a, &ct, (const uint8_t *) pfx, (uint32_t) strlen(pfx),
                              cur, recs, 2, &n, cur2, &eof) == CHIMERA_SC_OK, "long scan");
        for (i = 0; i < n; i++) {
            char want[48];

            snprintf(want, sizeof(want), "%s%02u", pfx, total);
            TCHECK(recs[i].key.len == strlen(want) &&
                   memcmp(recs[i].key.data, want, recs[i].key.len) == 0,
                   "long-key order at %u", total);
            total++;
        }
        if (eof) {
            break;
        }
        TCHECK(cur2[0] == 2, "long cursor uses the table (tag 2)");
        memcpy(cur, cur2, sizeof(cur));
    }
    TCHECK(total == 10, "all 10 long keys, no dup/skip (got %u)", total);

    /* A foreign/evicted long cursor fails loud, never a wrong page. */
    {
        uint8_t  bogus[32] = { 0 };
        uint64_t id        = 0x7777777777777777ULL;

        bogus[0] = 2;
        memcpy(bogus + 1, &id, sizeof(id));
        TCHECK(facade_ns_scan(a, &ct, (const uint8_t *) pfx, (uint32_t) strlen(pfx),
                              bogus, recs, 2, &n, cur2, &eof) == CHIMERA_SC_E_INVALID,
               "foreign cursor -> E_INVALID");
    }

    facade_ns_cursor_table_destroy(&ct);
    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
} /* test_long_key_cursor */

/* ---- compares + delta preconditions ---- */

static void
test_txn_compares_and_deltas(void)
{
    struct chimera_media_space *space = tu_media_open("ns://txn", ONE_GIB);
    struct facade_alloc        *a;
    struct chimera_sc_stamps    stamps;
    uint8_t                     oid[16] = { 0xA1 };

    tu_format(space);
    a = tu_open(space);
    memset(&stamps, 0, sizeof(stamps));

    put_one(a, "k", "v0");   /* k is at revision 1 */

    /* compare: revision mismatch -> E_COMPARE_FAILED + r_failed_part/index. */
    {
        struct chimera_sc_compare     cmp[2];
        struct chimera_sc_ns_txn_args args;
        struct facade_ns_txn_result   r;

        memset(cmp, 0, sizeof(cmp));
        cmp[0].key.data = (const uint8_t *) "k"; cmp[0].key.len = 1; cmp[0].revision = 1;
        cmp[1].key.data = (const uint8_t *) "k"; cmp[1].key.len = 1; cmp[1].revision = 99;
        memset(&args, 0, sizeof(args));
        args.compares = cmp; args.ncompares = 2;
        r             = run_txn(a, &args);
        TCHECK(r.status == CHIMERA_SC_E_COMPARE_FAILED, "compare mismatch status");
        TCHECK(r.r_failed_part == CHIMERA_SC_TXN_COMPARE && r.r_failed_index == 1,
               "compare r_failed part/index");
    }

    /* revision-0 compare = must-not-exist: passes for an absent key, fails for a
     * present one. */
    {
        struct chimera_sc_compare     cmp;
        struct chimera_sc_ns_txn_args args;

        memset(&cmp, 0, sizeof(cmp));
        cmp.key.data = (const uint8_t *) "absent"; cmp.key.len = 6; cmp.revision = 0;
        memset(&args, 0, sizeof(args));
        args.compares = &cmp; args.ncompares = 1;
        TCHECK(run_txn(a, &args).status == CHIMERA_SC_OK, "rev0 absent ok");
        cmp.key.data = (const uint8_t *) "k"; cmp.key.len = 1; cmp.revision = 0;
        TCHECK(run_txn(a, &args).status == CHIMERA_SC_E_COMPARE_FAILED,
               "rev0 present fails");
    }

    /* HOLD precondition: an object must exist and be never-referenced + unheld. */
    TCHECK(facade_obj_create_entry(a, oid, &stamps) != NULL, "create obj");
    {
        struct chimera_sc_delta       d;
        struct chimera_sc_ns_txn_args args;

        memset(&d, 0, sizeof(d));
        d.kind = CHIMERA_SC_DELTA_HOLD;
        memcpy(d.hold.obj.b, oid, 16);
        memset(&args, 0, sizeof(args));
        args.deltas = &d; args.ndeltas = 1;
        TCHECK(run_txn(a, &args).status == CHIMERA_SC_OK, "hold ok");
        TCHECK(run_txn(a, &args).status == CHIMERA_SC_E_STATE, "double hold -> E_STATE");
    }

    /* referenced-and-held rejection: HOLD then CREATE(ref) on the same object in
     * one txn is refused (CREATE needs an unheld object). */
    {
        uint8_t                       oid2[16] = { 0xB2 };
        struct chimera_sc_delta       d[2];
        struct chimera_sc_ns_txn_args args;
        struct facade_ns_txn_result   r;

        TCHECK(facade_obj_create_entry(a, oid2, &stamps) != NULL, "create obj2");
        memset(d, 0, sizeof(d));
        d[0].kind = CHIMERA_SC_DELTA_HOLD;
        memcpy(d[0].hold.obj.b, oid2, 16);
        d[1].kind = CHIMERA_SC_DELTA_CREATE;
        memcpy(d[1].create.obj.b, oid2, 16);
        d[1].create.stamps = stamps;
        memset(&args, 0, sizeof(args));
        args.deltas = d; args.ndeltas = 2;
        r           = run_txn(a, &args);
        TCHECK(r.status == CHIMERA_SC_E_STATE && r.r_failed_part == CHIMERA_SC_TXN_DELTA,
               "held-then-referenced -> E_STATE");
        /* and the object is NOT held (the whole txn rolled back). */
        TCHECK(!(facade_obj_find(a, oid2)->flags & FACADE_OBJ_HELD),
               "failed txn applied nothing");
    }

    /* rename as one transaction: delete the old record and put the new one
     * atomically. */
    {
        struct chimera_sc_record      put = { { (const uint8_t *) "k2", 2, 0 },
                                              { (const uint8_t *) "v0", 2, 0 }, 0 };
        struct chimera_sc_bytes       del = { (const uint8_t *) "k", 1, 0 };
        struct chimera_sc_ns_txn_args args;

        memset(&args, 0, sizeof(args));
        args.puts    = &put; args.nputs = 1;
        args.deletes = &del; args.ndeletes = 1;
        TCHECK(run_txn(a, &args).status == CHIMERA_SC_OK, "rename txn");
        TCHECK(!get_eq(a, "k", "v0") && get_eq(a, "k2", "v0"), "rename applied");
    }

    /* per-txn log-block cap: too many deltas -> E_NO_SPACE. */
    {
        struct chimera_sc_delta       d[FACADE_NS_MAX_TXN_LOG_BLOCKS];
        struct chimera_sc_ns_txn_args args;
        uint32_t                      i;

        memset(d, 0, sizeof(d));
        for (i = 0; i < FACADE_NS_MAX_TXN_LOG_BLOCKS; i++) {
            d[i].kind = CHIMERA_SC_DELTA_RECALL_HINT;   /* cheap, but 2 blocks each in the bound */
        }
        memset(&args, 0, sizeof(args));
        args.deltas = d; args.ndeltas = FACADE_NS_MAX_TXN_LOG_BLOCKS;
        TCHECK(run_txn(a, &args).status == CHIMERA_SC_E_NO_SPACE, "over-cap -> E_NO_SPACE");
    }

    /* oversize key -> E_INVALID. */
    {
        static uint8_t                big[FACADE_NS_MAX_KEY + 1];
        struct chimera_sc_record      put = { { big,                   FACADE_NS_MAX_KEY + 1,                   0
                                              },
                                              { (const uint8_t *) "v", 1,                                       0
                                              }, 0 };
        struct chimera_sc_ns_txn_args args;

        memset(&args, 0, sizeof(args));
        args.puts = &put; args.nputs = 1;
        TCHECK(run_txn(a, &args).status == CHIMERA_SC_E_INVALID, "oversize key -> E_INVALID");
    }

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
} /* test_txn_compares_and_deltas */

/* ---- crash atomicity of a committed multi-put txn ---- */

static void
test_crash_atomic(void)
{
    struct chimera_media_space   *space = tu_media_open("ns://crash", ONE_GIB);
    struct facade_alloc          *a;
    struct chimera_sc_record      puts[2];
    struct chimera_sc_ns_txn_args args;

    tu_format(space);
    a = tu_open(space);

    memset(puts, 0, sizeof(puts));
    puts[0].key.data   = (const uint8_t *) "t/a"; puts[0].key.len = 3;
    puts[0].value.data = (const uint8_t *) "AA"; puts[0].value.len = 2;
    puts[1].key.data   = (const uint8_t *) "t/b"; puts[1].key.len = 3;
    puts[1].value.data = (const uint8_t *) "BB"; puts[1].value.len = 2;
    memset(&args, 0, sizeof(args));
    args.puts = puts; args.nputs = 2;
    TCHECK(run_txn(a, &args).status == CHIMERA_SC_OK, "commit 2 puts");

    /* Crash: drop the handle without a clean close, then reopen the uri (a crash
     * view of only fua'd bytes) and run recovery. */
    facade_alloc_abandon(a);
    tu_media_close(space);
    space = tu_media_open("ns://crash", ONE_GIB);
    a     = tu_open(space);

    TCHECK(get_eq(a, "t/a", "AA") && get_eq(a, "t/b", "BB"),
           "both records survived the crash (atomic)");

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
} /* test_crash_atomic */

/* ---- C1 G1 guard: a txn that forces a compaction still commits + survives ---- */

static void
test_g1_compaction(void)
{
    /* Small media so log_nblocks is the raised floor (16); a run of small txns
     * fills it and forces a G1 compaction-before-wrap mid-sequence. */
    const uint64_t              small = 256ULL * CHIMERA_MEDIA_BLOCK_SIZE;
    struct chimera_media_space *space = tu_media_open("ns://g1", small);
    struct facade_alloc        *a;
    uint32_t                    i;
    char                        key[16];

    tu_format(space);
    a = tu_open(space);
    TCHECK(a->log_nblocks == FACADE_NS_MAX_TXN_LOG_BLOCKS, "small-media log floor = 16");

    /* 12 two-block txns (24 blocks) cannot fit a 16-block log without a
     * compaction partway, exercising the G1 path; all must commit. */
    for (i = 0; i < 12; i++) {
        snprintf(key, sizeof(key), "g/%02u", i);
        TCHECK(put_one(a, key, "v").status == CHIMERA_SC_OK, "g1 put %u", i);
    }

    /* Crash + reopen: every committed record survives (checkpoint + log replay). */
    facade_alloc_abandon(a);
    tu_media_close(space);
    space = tu_media_open("ns://g1", small);
    a     = tu_open(space);
    for (i = 0; i < 12; i++) {
        snprintf(key, sizeof(key), "g/%02u", i);
        TCHECK(get_eq(a, key, "v"), "g1 record %u survived", i);
    }

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
} /* test_g1_compaction */

int
main(void)
{
    test_exact_key_and_revisions();
    test_scan_order_and_cursor();
    test_long_key_cursor();
    test_txn_compares_and_deltas();
    test_crash_atomic();
    test_g1_compaction();
    printf("facade_ns_test: ok\n");
    return 0;
} /* main */
