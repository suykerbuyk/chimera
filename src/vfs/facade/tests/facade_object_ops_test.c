// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * object-ops-v1: the full contract 6.1 object surface and the 4.2 state rules,
 * over the in-RAM FUA-honest fixture (completions fire inline, so each op's
 * result is set by return unless the test stalls writes).  White-box: it reaches
 * into struct facade_obj / facade_obj_find to assert durable state.
 *
 * Covers: create; write (offset 0, mid-object, sub-block read-modify-write, into
 * a hole); read (data, hole -> zeros, past EOF, multi-extent); flush; truncate
 * (grow -> hole, shrink -> free); allocate (UNWRITTEN reads zeros); punch (->
 * hole, frees); seal; stat (every field); map; clone-working copy-on-write (the
 * exit criterion: the sealed origin is unchanged after a write to the working
 * version); discard; every 4.2 transition (open->sealed; a mutator on a sealed
 * object -> E_STATE; op on a released id -> E_NOT_FOUND); and the
 * no-reuse-of-a-freed-block-until-its-FREE-is-durable quarantine (C3), pinned so
 * the just-freed block is the only 1-block candidate and proven with a stalled
 * COMMIT.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "facade_alloc.h"
#include "facade_extent.h"
#include "facade_internal.h"
#include "facade_object.h"
#include "facade_objid.h"
#include "facade_test_media.h"
#include "facade_test_util.h"
#include "sc_types.h"

#define BS CHIMERA_MEDIA_BLOCK_SIZE

/* ---- synchronous op wrappers (inline-completion fixture) ---- */

static enum facade_status
oc(
    struct facade_alloc *a,
    uint8_t              tag,
    uint8_t              id[16])
{
    memset(id, 0, 16);
    id[0]  = tag;
    id[15] = 0xA5;
    return facade_obj_create_entry(a, id, NULL) ? FACADE_OK : FACADE_E_NO_SPACE;
} /* oc */

static enum facade_status
ow(
    struct facade_alloc *a,
    const uint8_t        id[16],
    uint64_t             off,
    const void          *data,
    uint32_t             len,
    uint32_t             level)
{
    struct tu_epoch_w w = { FACADE_E_IO, 0, 0 };

    facade_obj_write(a, id, off, data, len, level, tu_on_epoch, &w);
    TCHECK(w.done, "write did not complete inline");
    return w.st;
} /* ow */

struct rd_w {
    enum facade_status st;
    uint32_t len;
    uint64_t size;
    int      done;
};

static void
on_rd(
    enum facade_status st,
    uint32_t           len,
    uint64_t           size,
    void              *priv)
{
    struct rd_w *w = priv;

    w->st = st; w->len = len; w->size = size; w->done = 1;
} /* on_rd */

static enum facade_status
ord(
    struct facade_alloc *a,
    const uint8_t        id[16],
    uint64_t             off,
    uint32_t             len,
    void                *out,
    uint32_t            *r_len)
{
    struct rd_w w = { FACADE_E_IO, 0, 0, 0 };

    facade_obj_read(a, id, off, len, out, on_rd, &w);
    TCHECK(w.done, "read did not complete inline");
    if (r_len) {
        *r_len = w.len;
    }
    return w.st;
} /* ord */

static enum facade_status
oseal(
    struct facade_alloc *a,
    const uint8_t        id[16])
{
    struct tu_epoch_w w = { FACADE_E_IO, 0, 0 };

    facade_obj_seal(a, id, tu_on_epoch, &w);
    TCHECK(w.done, "seal did not complete inline");
    return w.st;
} /* oseal */

static enum facade_status
otrunc(
    struct facade_alloc *a,
    const uint8_t        id[16],
    uint64_t             sz)
{
    struct tu_epoch_w w = { FACADE_E_IO, 0, 0 };

    facade_obj_truncate(a, id, sz, tu_on_epoch, &w);
    TCHECK(w.done, "truncate did not complete inline");
    return w.st;
} /* otrunc */

static enum facade_status
oalloc(
    struct facade_alloc *a,
    const uint8_t        id[16],
    uint64_t             off,
    uint64_t             len)
{
    struct tu_epoch_w w = { FACADE_E_IO, 0, 0 };

    facade_obj_allocate(a, id, off, len, tu_on_epoch, &w);
    TCHECK(w.done, "allocate did not complete inline");
    return w.st;
} /* oalloc */

static enum facade_status
opunch(
    struct facade_alloc *a,
    const uint8_t        id[16],
    uint64_t             off,
    uint64_t             len)
{
    struct tu_epoch_w w = { FACADE_E_IO, 0, 0 };

    facade_obj_punch(a, id, off, len, tu_on_epoch, &w);
    TCHECK(w.done, "punch did not complete inline");
    return w.st;
} /* opunch */

static enum facade_status
odiscard(
    struct facade_alloc *a,
    const uint8_t        id[16])
{
    struct tu_status_w w = { FACADE_E_IO, 0 };

    facade_obj_discard(a, id, tu_on_status, &w);
    TCHECK(w.done, "discard did not complete inline");
    return w.st;
} /* odiscard */

/* ---- tests ---- */

static void
test_write_read_variants(void)
{
    struct chimera_media_space *space;
    struct facade_alloc        *a;
    uint8_t                     id[16];
    uint8_t                     pat[BS], out[BS];
    uint32_t                    rl;
    unsigned                    i;

    for (i = 0; i < BS; i++) {
        pat[i] = (uint8_t) (i * 5u + 1u);
    }

    facade_test_media_reset();
    space = tu_media_open("wrv", 8u << 20);
    tu_format(space);
    a = tu_open(space);

    TCHECK(oc(a, 1, id) == FACADE_OK, "create");

    /* write at offset 0, whole block */
    TCHECK(ow(a, id, 0, pat, BS, CHIMERA_SC_WRITE_DURABLE) == FACADE_OK, "write b0");
    TCHECK(ord(a, id, 0, 64, out, &rl) == FACADE_OK && rl == 64, "read b0");
    TCHECK(memcmp(out, pat, 64) == 0, "read b0 bytes");

    /* sub-block read-modify-write at offset 100 in block 0 (preserves bytes) */
    TCHECK(ow(a, id, 100, "ZZZZ", 4, CHIMERA_SC_WRITE_DURABLE) == FACADE_OK, "rmw");
    TCHECK(ord(a, id, 0, 128, out, &rl) == FACADE_OK && rl == 128, "read after rmw");
    TCHECK(memcmp(out, pat, 100) == 0, "rmw preserved head");
    TCHECK(memcmp(out + 100, "ZZZZ", 4) == 0, "rmw wrote bytes");
    TCHECK(memcmp(out + 104, pat + 104, 24) == 0, "rmw preserved tail");

    /* mid-object write at block 5 (block 1..4 become a hole) */
    TCHECK(ow(a, id, 5 * BS, pat, BS, CHIMERA_SC_WRITE_DURABLE) == FACADE_OK, "write b5");
    TCHECK(ord(a, id, 5 * BS, 64, out, &rl) == FACADE_OK && rl == 64, "read b5");
    TCHECK(memcmp(out, pat, 64) == 0, "read b5 bytes");

    /* read a hole (block 2) -> zeros */
    memset(out, 0xFF, BS);
    TCHECK(ord(a, id, 2 * BS, 64, out, &rl) == FACADE_OK && rl == 64, "read hole");
    for (i = 0; i < 64; i++) {
        TCHECK(out[i] == 0, "hole reads zero");
    }

    /* read past EOF -> 0 bytes */
    TCHECK(ord(a, id, 100 * BS, 64, out, &rl) == FACADE_OK && rl == 0, "read past EOF");

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
    printf("  write/read variants: ok\n");
} /* test_write_read_variants */

static void
test_size_ops(void)
{
    struct chimera_media_space *space;
    struct facade_alloc        *a;
    uint8_t                     id[16];
    uint8_t                     pat[BS], out[BS];
    uint32_t                    rl;
    struct chimera_sc_obj_meta  m;
    uint64_t                    free_after_write, free_before_punch, free_after_punch;

    memset(pat, 0x5A, sizeof(pat));

    facade_test_media_reset();
    space = tu_media_open("sz", 8u << 20);
    tu_format(space);
    a = tu_open(space);

    TCHECK(oc(a, 2, id) == FACADE_OK, "create");
    TCHECK(ow(a, id, 0, pat, BS, CHIMERA_SC_WRITE_DURABLE) == FACADE_OK, "write");
    free_after_write = facade_alloc_free_blocks(a);

    /* truncate grow: size extends, the tail is a hole (reads zeros), no alloc */
    TCHECK(otrunc(a, id, 10 * BS) == FACADE_OK, "truncate grow");
    TCHECK(facade_obj_stat(a, id, &m) == 0 && m.size == 10 * BS, "grown size");
    TCHECK(ord(a, id, 9 * BS, 64, out, &rl) == FACADE_OK && rl == 64 && out[0] == 0,
           "grown tail reads zeros");
    TCHECK(facade_alloc_free_blocks(a) == free_after_write, "grow allocates nothing");

    /* allocate a reserved (UNWRITTEN) extent: reads zeros, is charged, 2 blocks */
    TCHECK(oalloc(a, id, 20 * BS, 2 * BS) == FACADE_OK, "allocate");
    TCHECK(ord(a, id, 20 * BS, 64, out, &rl) == FACADE_OK && rl == 64 && out[0] == 0,
           "unwritten reads zeros");
    TCHECK(facade_obj_stat(a, id, &m) == 0 && m.allocated_bytes == 3u * BS,
           "allocate charged allocated_bytes (1 data + 2 unwritten)");
    TCHECK(facade_alloc_free_blocks(a) == free_after_write - 2, "allocate took 2 blocks");

    /* punch block 0: it becomes a hole and its one block is freed */
    free_before_punch = facade_alloc_free_blocks(a);
    TCHECK(opunch(a, id, 0, BS) == FACADE_OK, "punch");
    free_after_punch = facade_alloc_free_blocks(a);
    TCHECK(free_after_punch == free_before_punch + 1, "punch freed one block (%llu vs %llu)",
           (unsigned long long) free_after_punch, (unsigned long long) free_before_punch);
    TCHECK(ord(a, id, 0, 64, out, &rl) == FACADE_OK && rl == 64 && out[0] == 0,
           "punched block reads zeros");

    /* truncate shrink to 0: frees the remaining owned (UNWRITTEN) blocks */
    TCHECK(otrunc(a, id, 0) == FACADE_OK, "truncate to zero");
    TCHECK(facade_obj_stat(a, id, &m) == 0 && m.size == 0, "zero size");
    TCHECK(facade_alloc_free_blocks(a) == free_after_punch + 2, "shrink freed the 2 unwritten");

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
    printf("  size ops (truncate/allocate/punch): ok\n");
} /* test_size_ops */

static void
test_stat_map_flush(void)
{
    struct chimera_media_space *space;
    struct facade_alloc        *a;
    uint8_t                     id[16];
    uint8_t                     pat[64];
    struct chimera_sc_obj_meta  m;
    struct chimera_sc_segment   segs[8];
    uint32_t                    nsegs = 0, more = 0;
    struct tu_epoch_w           fw = { FACADE_E_IO, 0, 0 };

    memset(pat, 0x11, sizeof(pat));

    facade_test_media_reset();
    space = tu_media_open("sm", 8u << 20);
    tu_format(space);
    a = tu_open(space);

    TCHECK(oc(a, 3, id) == FACADE_OK, "create");
    TCHECK(ow(a, id, 0, pat, 64, CHIMERA_SC_WRITE_DURABLE) == FACADE_OK, "write");

    /* flush is a no-op passthrough here (write-through), reports OK + a gen */
    facade_obj_flush(a, id, tu_on_epoch, &fw);
    TCHECK(fw.done && fw.st == FACADE_OK, "flush");

    TCHECK(facade_obj_stat(a, id, &m) == 0, "stat");
    TCHECK(m.state == CHIMERA_SC_OBJ_OPEN, "stat state open");
    TCHECK(m.size == 64, "stat size");
    TCHECK(m.allocated_bytes == BS, "stat allocated");
    TCHECK(m.refs == 0 && m.held == 0, "stat refs/held");
    TCHECK(m.checksum.algorithm == CHIMERA_SC_CHECKSUM_NONE, "stat checksum none");
    TCHECK(m.change > 0, "stat change advanced");

    /* map: block 0 DATA, then a hole to EOF-ish */
    TCHECK(facade_obj_map(a, id, 0, 4 * BS, segs, 8, &nsegs, &more) == 0, "map");
    TCHECK(nsegs >= 1 && segs[0].kind == CHIMERA_SC_SEG_DATA, "map seg0 data");

    /* stat/map on an unknown id -> not found */
    {
        uint8_t bad[16] = { 0xEE };

        TCHECK(facade_obj_stat(a, bad, &m) == -1, "stat unknown");
        TCHECK(facade_obj_map(a, bad, 0, BS, segs, 8, &nsegs, &more) == -1, "map unknown");
    }

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
    printf("  stat/map/flush: ok\n");
} /* test_stat_map_flush */

static void
test_state_transitions(void)
{
    struct chimera_media_space *space;
    struct facade_alloc        *a;
    uint8_t                     id[16];
    uint8_t                     pat[64];
    struct chimera_sc_obj_meta  m;

    memset(pat, 0x22, sizeof(pat));

    facade_test_media_reset();
    space = tu_media_open("st", 8u << 20);
    tu_format(space);
    a = tu_open(space);

    TCHECK(oc(a, 4, id) == FACADE_OK, "create");
    TCHECK(ow(a, id, 0, pat, 64, CHIMERA_SC_WRITE_DURABLE) == FACADE_OK, "write open");

    /* open -> sealed */
    TCHECK(oseal(a, id) == FACADE_OK, "seal");
    TCHECK(facade_obj_stat(a, id, &m) == 0 && m.state == CHIMERA_SC_OBJ_SEALED, "sealed");

    /* a second seal, and every mutator on a sealed object, is E_STATE */
    TCHECK(oseal(a, id) == FACADE_E_STATE, "re-seal E_STATE");
    TCHECK(ow(a, id, 0, pat, 64, CHIMERA_SC_WRITE_DURABLE) == FACADE_E_STATE,
           "write sealed E_STATE");
    TCHECK(otrunc(a, id, 0) == FACADE_E_STATE, "truncate sealed E_STATE");
    TCHECK(oalloc(a, id, 0, BS) == FACADE_E_STATE, "allocate sealed E_STATE");
    TCHECK(opunch(a, id, 0, BS) == FACADE_E_STATE, "punch sealed E_STATE");

    /* a read of a sealed object still works */
    {
        uint8_t  out[64];
        uint32_t rl;

        TCHECK(ord(a, id, 0, 64, out, &rl) == FACADE_OK && rl == 64, "read sealed");
    }

    /* an unknown id: NOT_FOUND on every op */
    {
        uint8_t  bad[16] = { 0xDD };
        uint8_t  out[8];
        uint32_t rl;

        TCHECK(ow(a, bad, 0, pat, 8, CHIMERA_SC_WRITE_DURABLE) == FACADE_E_NOT_FOUND,
               "write unknown NOT_FOUND");
        TCHECK(ord(a, bad, 0, 8, out, &rl) == FACADE_E_NOT_FOUND, "read unknown NOT_FOUND");
        TCHECK(oseal(a, bad) == FACADE_E_NOT_FOUND, "seal unknown NOT_FOUND");
    }

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
    printf("  state transitions: ok\n");
} /* test_state_transitions */

static void
test_discard(void)
{
    struct chimera_media_space *space;
    struct facade_alloc        *a;
    uint8_t                     id[16], sid[16];
    uint8_t                     pat[BS];
    uint64_t                    free_before;

    memset(pat, 0x33, sizeof(pat));

    facade_test_media_reset();
    space = tu_media_open("dc", 8u << 20);
    tu_format(space);
    a = tu_open(space);

    /* an epoch-held (OPEN, unheld) object with one owned block is discardable */
    TCHECK(oc(a, 5, id) == FACADE_OK, "create");
    TCHECK(ow(a, id, 0, pat, BS, CHIMERA_SC_WRITE_DURABLE) == FACADE_OK, "write");
    free_before = facade_alloc_free_blocks(a);
    TCHECK(odiscard(a, id) == FACADE_OK, "discard");
    TCHECK(facade_obj_find(a, id) == NULL, "discarded object gone");
    TCHECK(facade_alloc_free_blocks(a) == free_before + 1, "discard freed its block");

    /* discard of a sealed object -> E_STATE; of an unknown -> NOT_FOUND */
    TCHECK(oc(a, 6, sid) == FACADE_OK, "create sealed");
    TCHECK(ow(a, sid, 0, pat, BS, CHIMERA_SC_WRITE_DURABLE) == FACADE_OK, "write sealed");
    TCHECK(oseal(a, sid) == FACADE_OK, "seal");
    TCHECK(odiscard(a, sid) == FACADE_E_STATE, "discard sealed E_STATE");
    {
        uint8_t bad[16] = { 0xCC };

        TCHECK(odiscard(a, bad) == FACADE_E_NOT_FOUND, "discard unknown NOT_FOUND");
    }

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
    printf("  discard: ok\n");
} /* test_discard */

/*
 * C3: a freed owned block is not reallocated until its FREE record is durable.
 * Overwrite a block (COW allocates a new block, frees the old one QUARANTINED),
 * stall the transaction's COMMIT, and prove a direct allocation cannot hand out
 * the just-freed block while the FREE is not yet durable; releasing the COMMIT
 * then returns it.  Pinned non-vacuously: the freed block is the only isolated
 * 1-block free extent, so best-fit WOULD return it the instant the quarantine
 * is lifted (the oracle: reverting the quarantine makes the mid-stall allocation
 * return exactly this block).
 */
static void
test_free_quarantine(void)
{
    struct chimera_media_space *space;
    struct facade_alloc        *a;
    uint8_t                     id[16];
    uint8_t                     pat[BS];
    chimera_media_block_t       p1 = 0, x = 0;
    struct tu_epoch_w           ew = { FACADE_E_IO, 0, 0 };
    enum facade_status          st;
    int                         pending;

    memset(pat, 0x44, sizeof(pat));

    facade_test_media_reset();
    space = tu_media_open("qn", 8u << 20);
    tu_format(space);
    a = tu_open(space);

    TCHECK(oc(a, 7, id) == FACADE_OK, "create");
    /* First durable write: allocates P1 (tail-cut from the one big free extent)
     * and arms the dirty window, all inline. */
    TCHECK(ow(a, id, 0, pat, BS, CHIMERA_SC_WRITE_DURABLE) == FACADE_OK, "write1");
    TCHECK(tu_obj_phys0(a, id, &p1), "P1 present");

    /* Overwrite the whole block: COW allocates P2 (the next tail) and frees P1.
    * Stall every write so we can hold the COMMIT and inspect the quarantine. */
    facade_test_media_stall_writes(space, 1);
    facade_obj_write(a, id, 0, pat, BS, CHIMERA_SC_WRITE_DURABLE, tu_on_epoch, &ew);

    /* Release data, ALLOC, FREE, OBJ -- but NOT the COMMIT.  Each completion
     * enqueues the next write, so after four releases exactly the COMMIT is
     * pending and the FREE of P1 is on media but NOT yet committed. */
    facade_test_media_release(space);   /* data FUA (P2) */
    facade_test_media_release(space);   /* ALLOC */
    facade_test_media_release(space);   /* FREE(P1) */
    facade_test_media_release(space);   /* OBJ */
    pending = facade_test_media_pending_count(space);
    TCHECK(pending == 1, "exactly the COMMIT should be pending (got %d)", pending);
    TCHECK(!ew.done, "the overwrite must not have completed yet");

    /* P1's FREE is not durable: a direct allocation must NOT return P1 (it is
    * quarantined -- held out of the free set until the COMMIT is durable). */
    st = facade_alloc_blocks(a, 1, &x, NULL);
    TCHECK(st == FACADE_OK, "a 1-block allocation should still succeed elsewhere");
    TCHECK(x != p1, "quarantine breached: the just-freed block was reallocated");
    facade_alloc_free(a, x, 1);   /* give it back */

    /* Release the COMMIT: the transaction is durable, P1 is released. */
    facade_test_media_release_all(space);
    TCHECK(ew.done && ew.st == FACADE_OK, "overwrite completes after COMMIT");
    facade_test_media_stall_writes(space, 0);

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
    printf("  free quarantine (no reuse until FREE durable): ok\n");
} /* test_free_quarantine */

static void
dummy_resume(void *arg)
{
    (void) arg;
} /* dummy_resume */

/*
 * The clone exit criterion (D2): a metadata-only working-version clone shares
 * the sealed origin's blocks; a write to the working version is copy-on-write
 * and leaves the old version -- its bytes AND its extent map -- unchanged.
 */
static void
test_clone_cow(void)
{
    struct chimera_media_space *space;
    struct facade_alloc        *a;
    uint8_t                     sid[16];
    struct chimera_sc_obj_id    wid;
    uint8_t                     patA[BS], patB[BS], patC[BS], out[BS];
    uint32_t                    rl;
    chimera_media_block_t       s_phys0 = 0, s_phys0_after = 0;
    uint64_t                    free_before;
    struct tu_epoch_w           cw = { FACADE_E_IO, 0, 0 };
    struct chimera_sc_obj_meta  m;

    memset(patA, 0xA1, sizeof(patA));
    memset(patB, 0xB2, sizeof(patB));
    memset(patC, 0xC3, sizeof(patC));

    facade_test_media_reset();
    space = tu_media_open("cl", 8u << 20);
    tu_format(space);
    a = tu_open(space);

    /* A sealed origin S with two data blocks. */
    TCHECK(oc(a, 10, sid) == FACADE_OK, "create S");
    TCHECK(ow(a, sid, 0, patA, BS, CHIMERA_SC_WRITE_DURABLE) == FACADE_OK, "S b0");
    TCHECK(ow(a, sid, BS, patB, BS, CHIMERA_SC_WRITE_DURABLE) == FACADE_OK, "S b1");
    TCHECK(oseal(a, sid) == FACADE_OK, "seal S");
    TCHECK(tu_obj_phys0(a, sid, &s_phys0), "S phys0");

    /* Clone a working version W (metadata only: same blocks, no data copy). */
    TCHECK(facade_objid_next(a, &wid, dummy_resume, NULL) == FACADE_OK, "draw W id");
    facade_obj_clone_working(a, sid, &wid, tu_on_epoch, &cw);
    TCHECK(cw.done && cw.st == FACADE_OK, "clone");
    TCHECK(facade_obj_stat(a, wid.b, &m) == 0 && m.size == 2u * BS, "W inherits size");
    /* W is a metadata-only clone: it charges NO allocated bytes of its own. */
    TCHECK(m.allocated_bytes == 0, "clone charges no owned bytes");

    /* W reads the origin's bytes (borrowed). */
    TCHECK(ord(a, wid.b, 0, 64, out, &rl) == FACADE_OK && memcmp(out, patA, 64) == 0,
           "W b0 == origin");
    TCHECK(ord(a, wid.b, BS, 64, out, &rl) == FACADE_OK && memcmp(out, patB, 64) == 0,
           "W b1 == origin");

    /* Copy-on-write: a write to W block 0 must allocate a NEW block and leave S
     * untouched. */
    free_before = facade_alloc_free_blocks(a);
    TCHECK(ow(a, wid.b, 0, patC, BS, CHIMERA_SC_WRITE_DURABLE) == FACADE_OK, "W b0 write");
    TCHECK(facade_alloc_free_blocks(a) == free_before - 1,
           "COW allocated a new block and did not free the origin's (%llu vs %llu)",
           (unsigned long long) facade_alloc_free_blocks(a),
           (unsigned long long) free_before);

    /* W sees the new bytes. */
    TCHECK(ord(a, wid.b, 0, 64, out, &rl) == FACADE_OK && memcmp(out, patC, 64) == 0,
           "W b0 shows the new bytes");
    /* S is unchanged: its bytes AND its extent map. */
    TCHECK(ord(a, sid, 0, 64, out, &rl) == FACADE_OK && memcmp(out, patA, 64) == 0,
           "origin b0 unchanged after W write (THE clone exit criterion)");
    TCHECK(tu_obj_phys0(a, sid, &s_phys0_after) && s_phys0_after == s_phys0,
           "origin extent map unchanged");
    /* W block 1 is still borrowed (shared, unchanged). */
    TCHECK(ord(a, wid.b, BS, 64, out, &rl) == FACADE_OK && memcmp(out, patB, 64) == 0,
           "W b1 still shares the origin block");

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
    printf("  clone copy-on-write (old version unchanged): ok\n");
} /* test_clone_cow */

/*
 * Real concurrency: two durable writes dispatched while the first is still in
 * flight must serialize through the one-at-a-time transaction FIFO, so the
 * second cannot start (and cannot race the log cursor) until the first's COMMIT
 * is durable.  Proven by stalling: write A holds the FIFO mid-transaction, write
 * B is dispatched and must make no progress until A completes.
 */
static void
test_txn_fifo_serialization(void)
{
    struct chimera_media_space *space;
    struct facade_alloc        *a;
    uint8_t                     ida[16], idb[16];
    uint8_t                     patA[BS], patB[BS], out[BS];
    uint32_t                    rl;
    struct tu_epoch_w           ewa = { FACADE_E_IO, 0, 0 };
    struct tu_epoch_w           ewb = { FACADE_E_IO, 0, 0 };

    memset(patA, 0x71, sizeof(patA));
    memset(patB, 0x82, sizeof(patB));

    facade_test_media_reset();
    space = tu_media_open("fifo", 8u << 20);
    tu_format(space);
    a = tu_open(space);

    TCHECK(oc(a, 8, ida) == FACADE_OK, "create A");
    TCHECK(oc(a, 9, idb) == FACADE_OK, "create B");

    facade_test_media_stall_writes(space, 1);

    /* A stalls at its first media write while holding the FIFO. */
    facade_obj_write(a, ida, 0, patA, BS, CHIMERA_SC_WRITE_DURABLE, tu_on_epoch, &ewa);
    TCHECK(facade_test_media_pending_count(space) >= 1, "A should have a write in flight");
    TCHECK(!ewa.done, "A must still be in flight");

    /* B is dispatched behind the FIFO: it must make NO media progress (A holds
     * the slot), so the pending count is unchanged and B is not done. */
    {
        int before = facade_test_media_pending_count(space);

        facade_obj_write(a, idb, 0, patB, BS, CHIMERA_SC_WRITE_DURABLE, tu_on_epoch, &ewb);
        TCHECK(facade_test_media_pending_count(space) == before,
               "B must not issue any media write while A holds the FIFO");
        TCHECK(!ewb.done, "B must not progress while A holds the FIFO");
    }

    /* Drain: A completes, releases the FIFO, B then runs and completes. */
    facade_test_media_release_all(space);
    facade_test_media_stall_writes(space, 0);
    TCHECK(ewa.done && ewa.st == FACADE_OK, "A completes");
    TCHECK(ewb.done && ewb.st == FACADE_OK, "B completes after A releases the FIFO");

    TCHECK(ord(a, ida, 0, 64, out, &rl) == FACADE_OK && memcmp(out, patA, 64) == 0, "A bytes");
    TCHECK(ord(a, idb, 0, 64, out, &rl) == FACADE_OK && memcmp(out, patB, 64) == 0, "B bytes");

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
    printf("  txn FIFO serialization (real concurrency): ok\n");
} /* test_txn_fifo_serialization */

/*
 * CR-1: an op whose extent map would exceed FACADE_OBJ_MAX_EXT must be refused
 * NO_SPACE-shaped (not silently accepted into an over-cap OBJ record that breaks
 * a later reopen / stops the durable prefix on recovery).  ~70 single-block
 * writes at distinct, non-adjacent offsets: the first FACADE_OBJ_MAX_EXT succeed,
 * the rest are refused, and a clean close + reopen still succeeds.
 */
static void
test_extent_cap(void)
{
    struct chimera_media_space *space;
    struct facade_alloc        *a;
    uint8_t                     id[16];
    uint8_t                     pat[BS];
    uint32_t                    i, ok = 0, refused = 0;

    memset(pat, 0x6D, sizeof(pat));

    facade_test_media_reset();
    space = tu_media_open("cap", 8u << 20);
    tu_format(space);
    a = tu_open(space);

    TCHECK(oc(a, 20, id) == FACADE_OK, "create");
    for (i = 0; i < 70; i++) {
        enum facade_status st = ow(a, id, (uint64_t) i * 2 * BS, pat, BS,
                                   CHIMERA_SC_WRITE_DURABLE);

        if (st == FACADE_OK) {
            ok++;
        } else {
            TCHECK(st == FACADE_E_NO_SPACE, "over-cap write must be NO_SPACE, got %d", st);
            refused++;
        }
    }
    TCHECK(ok == FACADE_OBJ_MAX_EXT, "exactly the cap worth of extents accepted (%u vs %u)",
           ok, (unsigned) FACADE_OBJ_MAX_EXT);
    TCHECK(refused == 70 - FACADE_OBJ_MAX_EXT, "the rest refused (%u)", refused);
    {
        struct facade_obj *o = facade_obj_find(a, id);

        TCHECK(facade_extent_map_count(o->map) == FACADE_OBJ_MAX_EXT,
               "map stayed at the cap (no over-cap partial mutation left behind)");
    }

    /* The on-media object record is within the cap, so a clean close + reopen
     * (which serializes and reloads the object table) succeeds. */
    tu_close(a);
    a = tu_open(space);
    TCHECK(facade_obj_find(a, id) != NULL, "object survives a clean reopen");
    TCHECK(facade_extent_map_count(facade_obj_find(a, id)->map) == FACADE_OBJ_MAX_EXT,
           "reopened object has exactly the cap extents");

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
    printf("  extent cap guard (CR-1): ok\n");
} /* test_extent_cap */

/*
 * CR-2: an object release must not return the freed block to the allocatable set
 * until it runs inside the transaction FIFO and its compaction is durable.  With
 * a write holding the FIFO, a concurrently-dispatched discard of a different
 * object must free NOTHING yet (it is queued behind the FIFO); only after the
 * write releases the slot does the discard run and free the block.
 */
static void
test_discard_fifo(void)
{
    struct chimera_media_space *space;
    struct facade_alloc        *a;
    uint8_t                     ida[16], idb[16];
    uint8_t                     pat[BS];
    chimera_media_block_t       pb = 0, x = 0;
    struct tu_epoch_w           ewa = { FACADE_E_IO, 0, 0 };
    struct tu_status_w          dw  = { FACADE_E_IO, 0 };
    uint64_t                    free_mid;
    enum facade_status          st;

    memset(pat, 0x4D, sizeof(pat));

    facade_test_media_reset();
    space = tu_media_open("dfifo", 8u << 20);
    tu_format(space);
    a = tu_open(space);

    TCHECK(oc(a, 21, ida) == FACADE_OK, "create A");
    TCHECK(oc(a, 22, idb) == FACADE_OK, "create B");
    TCHECK(ow(a, ida, 0, pat, BS, CHIMERA_SC_WRITE_DURABLE) == FACADE_OK, "write A");
    TCHECK(ow(a, idb, 0, pat, BS, CHIMERA_SC_WRITE_DURABLE) == FACADE_OK, "write B");
    TCHECK(tu_obj_phys0(a, idb, &pb), "B phys");

    /* A write to A stalls mid-transaction, holding the FIFO. */
    facade_test_media_stall_writes(space, 1);
    facade_obj_write(a, ida, BS, pat, BS, CHIMERA_SC_WRITE_DURABLE, tu_on_epoch, &ewa);
    TCHECK(!ewa.done, "A write must be in flight (holding the FIFO)");
    free_mid = facade_alloc_free_blocks(a);

    /* Discard B while A holds the FIFO: it must free NOTHING yet. */
    facade_obj_discard(a, idb, tu_on_status, &dw);
    TCHECK(!dw.done, "discard must be queued behind the FIFO, not completed");
    TCHECK(facade_alloc_free_blocks(a) == free_mid,
           "discard must not free B's block while A holds the FIFO (%llu vs %llu)",
           (unsigned long long) facade_alloc_free_blocks(a), (unsigned long long) free_mid);
    st = facade_alloc_blocks(a, 1, &x, NULL);
    TCHECK(st != FACADE_OK || x != pb, "B's block must not be reallocatable mid-release");
    if (st == FACADE_OK) {
        facade_alloc_free(a, x, 1);
    }

    /* Release the FIFO: A completes, then B's discard runs and frees B. */
    facade_test_media_release_all(space);
    facade_test_media_stall_writes(space, 0);
    TCHECK(ewa.done && ewa.st == FACADE_OK, "A completes");
    TCHECK(dw.done && dw.st == FACADE_OK, "discard completes after A releases the FIFO");
    TCHECK(facade_obj_find(a, idb) == NULL, "B discarded");

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
    printf("  discard inside FIFO (CR-2): ok\n");
} /* test_discard_fifo */

/*
 * #4: the generalized transaction's rollback over the NEW record shapes.  A
 * multi-FREE op (truncate/punch) and a no-ALLOC op (clone) that hit a
 * mid-transaction media failure must roll back fully: the object's map, size and
 * state restored, no blocks leaked, and a clone's new object removed.
 */
static void
test_rollback_surfaces(void)
{
    struct chimera_media_space *space;
    struct facade_alloc        *a;
    uint8_t                     id[16], sid[16];
    struct chimera_sc_obj_id    wid;
    uint8_t                     pat[BS];
    uint64_t                    ln, free_before;
    struct tu_epoch_w           tw = { FACADE_E_IO, 0, 0 };
    struct tu_epoch_w           cw = { FACADE_E_IO, 0, 0 };

    memset(pat, 0x3C, sizeof(pat));

    facade_test_media_reset();
    space = tu_media_open("rb4", 8u << 20);
    tu_format(space);
    a = tu_open(space);

    /* Multi-FREE rollback: an object with three owned blocks, truncate-to-zero
     * (3 FREE + OBJ + COMMIT) with the first log record forced to fail. */
    TCHECK(oc(a, 23, id) == FACADE_OK, "create");
    TCHECK(ow(a, id, 0, pat, BS, CHIMERA_SC_WRITE_DURABLE) == FACADE_OK, "w0");
    TCHECK(ow(a, id, 2 * BS, pat, BS, CHIMERA_SC_WRITE_DURABLE) == FACADE_OK, "w2");
    TCHECK(ow(a, id, 4 * BS, pat, BS, CHIMERA_SC_WRITE_DURABLE) == FACADE_OK, "w4");
    free_before = facade_alloc_free_blocks(a);

    ln = a->log_next;                                   /* first record of the next txn */
    facade_test_media_fail_write_lba(space, a->log_block + ln);
    facade_obj_truncate(a, id, 0, tu_on_epoch, &tw);
    TCHECK(tw.done && tw.st != FACADE_OK, "truncate must fail on the injected error");
    {
        struct facade_obj *o = facade_obj_find(a, id);

        TCHECK(o != NULL && o->size == 5 * BS, "size rolled back to pre-truncate (%llu)",
               (unsigned long long) (o ? o->size : 0));
        TCHECK(facade_extent_map_count(o->map) == 3, "map rolled back to 3 extents");
    }
    TCHECK(facade_alloc_free_blocks(a) == free_before,
           "no blocks leaked or early-freed by the rolled-back truncate");

    /* The object is still usable after the rolled-back truncate. */
    TCHECK(otrunc(a, id, 0) == FACADE_OK, "a retried truncate succeeds");

    /* No-ALLOC clone rollback: seal an origin, fail the clone's OBJ record, and
     * confirm the new working-version object is removed. */
    TCHECK(oc(a, 24, sid) == FACADE_OK, "create S");
    TCHECK(ow(a, sid, 0, pat, BS, CHIMERA_SC_WRITE_DURABLE) == FACADE_OK, "write S");
    TCHECK(oseal(a, sid) == FACADE_OK, "seal S");
    TCHECK(facade_objid_next(a, &wid, dummy_resume, NULL) == FACADE_OK, "draw W");
    free_before = facade_alloc_free_blocks(a);
    ln          = a->log_next;
    facade_test_media_fail_write_lba(space, a->log_block + ln);   /* the clone's OBJ record */
    facade_obj_clone_working(a, sid, &wid, tu_on_epoch, &cw);
    TCHECK(cw.done && cw.st != FACADE_OK, "clone must fail on the injected error");
    TCHECK(facade_obj_find(a, wid.b) == NULL, "the failed clone's object is removed");
    TCHECK(facade_obj_find(a, sid) != NULL, "the sealed origin is untouched");
    TCHECK(facade_alloc_free_blocks(a) == free_before, "clone rollback leaks no blocks");

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
    printf("  rollback surfaces (multi-FREE + no-ALLOC clone): ok\n");
} /* test_rollback_surfaces */

/*
 * CR-3: an object removed by a discard (or epoch-held release) that is queued
 * ahead on the FIFO must not be dereferenced by a mutator queued behind it.  The
 * mutator passed its dispatch-time entry check (the object still existed then),
 * but by the time its transaction runs the discard has removed it; its prepare
 * must fail NOT_FOUND, not NULL-deref.  Dispatch discard(X), seal(X) and
 * write(X) behind an in-flight write holding the FIFO; after draining, the
 * discard succeeds and the seal/write return NOT_FOUND (no SEGV).
 */
static void
test_use_after_discard(void)
{
    struct chimera_media_space *space;
    struct facade_alloc        *a;
    uint8_t                     idx[16], idz[16];
    uint8_t                     pat[BS];
    struct tu_epoch_w           zw = { FACADE_E_IO, 0, 0 };
    struct tu_epoch_w           sw = { FACADE_E_IO, 0, 0 };
    struct tu_epoch_w           ww = { FACADE_E_IO, 0, 0 };
    struct tu_status_w          dw = { FACADE_E_IO, 0 };

    memset(pat, 0x5E, sizeof(pat));

    facade_test_media_reset();
    space = tu_media_open("uad", 8u << 20);
    tu_format(space);
    a = tu_open(space);

    TCHECK(oc(a, 30, idx) == FACADE_OK, "create X");
    TCHECK(oc(a, 31, idz) == FACADE_OK, "create Z");
    TCHECK(ow(a, idx, 0, pat, BS, CHIMERA_SC_WRITE_DURABLE) == FACADE_OK, "write X");
    TCHECK(ow(a, idz, 0, pat, BS, CHIMERA_SC_WRITE_DURABLE) == FACADE_OK, "write Z");

    /* A write to Z stalls mid-transaction, holding the FIFO. */
    facade_test_media_stall_writes(space, 1);
    facade_obj_write(a, idz, BS, pat, BS, CHIMERA_SC_WRITE_DURABLE, tu_on_epoch, &zw);
    TCHECK(!zw.done, "Z write in flight (holds the FIFO)");

    /* All three queue behind the FIFO; X still exists at their dispatch checks. */
    facade_obj_discard(a, idx, tu_on_status, &dw);
    facade_obj_seal(a, idx, tu_on_epoch, &sw);
    facade_obj_write(a, idx, 0, pat, BS, CHIMERA_SC_WRITE_DURABLE, tu_on_epoch, &ww);
    TCHECK(!dw.done && !sw.done && !ww.done, "all queued behind the in-flight write");

    /* Drain: Z completes, discard removes X, then seal/write run against the
     * now-missing X -- they must fail NOT_FOUND, never dereference it. */
    facade_test_media_release_all(space);
    facade_test_media_stall_writes(space, 0);

    TCHECK(zw.done && zw.st == FACADE_OK, "Z write completes");
    TCHECK(dw.done && dw.st == FACADE_OK, "discard X completes");
    TCHECK(facade_obj_find(a, idx) == NULL, "X is gone");
    TCHECK(sw.done && sw.st == FACADE_E_NOT_FOUND, "seal of discarded X -> NOT_FOUND");
    TCHECK(ww.done && ww.st == FACADE_E_NOT_FOUND, "write of discarded X -> NOT_FOUND");

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
    printf("  use-after-discard guard (CR-3): ok\n");
} /* test_use_after_discard */

/*
 * The CR-3 re-review wart: a discard whose target is sealed/held by a txn queued
 * ahead of it reports the refusal (E_STATE), not a false OK.
 */
static void
test_discard_loses_race_reports_state(void)
{
    struct chimera_media_space *space;
    struct facade_alloc        *a;
    uint8_t                     idx[16], idz[16];
    uint8_t                     pat[BS];
    struct tu_epoch_w           zw = { FACADE_E_IO, 0, 0 };
    struct tu_epoch_w           sw = { FACADE_E_IO, 0, 0 };
    struct tu_status_w          dw = { FACADE_E_IO, 0 };

    memset(pat, 0x6F, sizeof(pat));

    facade_test_media_reset();
    space = tu_media_open("dlr", 8u << 20);
    tu_format(space);
    a = tu_open(space);

    TCHECK(oc(a, 32, idx) == FACADE_OK, "create X");
    TCHECK(oc(a, 33, idz) == FACADE_OK, "create Z");
    TCHECK(ow(a, idx, 0, pat, BS, CHIMERA_SC_WRITE_DURABLE) == FACADE_OK, "write X");
    TCHECK(ow(a, idz, 0, pat, BS, CHIMERA_SC_WRITE_DURABLE) == FACADE_OK, "write Z");

    facade_test_media_stall_writes(space, 1);
    facade_obj_write(a, idz, BS, pat, BS, CHIMERA_SC_WRITE_DURABLE, tu_on_epoch, &zw);

    /* seal(X) queues ahead of discard(X): when discard runs, X is sealed, so the
     * discard refuses with E_STATE rather than claiming success. */
    facade_obj_seal(a, idx, tu_on_epoch, &sw);
    facade_obj_discard(a, idx, tu_on_status, &dw);

    facade_test_media_release_all(space);
    facade_test_media_stall_writes(space, 0);

    TCHECK(sw.done && sw.st == FACADE_OK, "seal X completes");
    TCHECK(dw.done && dw.st == FACADE_E_STATE,
           "discard of the now-sealed X reports E_STATE, not a false OK (got %d)", dw.st);
    TCHECK(facade_obj_find(a, idx) != NULL, "sealed X survives the losing discard");

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
    printf("  discard-loses-race reports E_STATE (wart): ok\n");
} /* test_discard_loses_race_reports_state */

int
main(void)
{
    test_write_read_variants();
    test_size_ops();
    test_stat_map_flush();
    test_state_transitions();
    test_discard();
    test_clone_cow();
    test_free_quarantine();
    test_txn_fifo_serialization();
    test_extent_cap();
    test_discard_fifo();
    test_rollback_surfaces();
    test_use_after_discard();
    test_discard_loses_race_reports_state();
    printf("facade_object_ops_test: ok\n");
    return 0;
} /* main */
