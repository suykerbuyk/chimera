// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Exit criterion: in-RAM unit tests for media-log-and-recovery over the
 * FUA-honest, stall-capable fixture (facade_test_media), covering:
 *
 *   1. a durable object round-trips a clean close + reopen;
 *   2. the L41 refusal split -- a recognized-but-unsupported on-media format is
 *      FACADE_E_FORMAT (never read as blank), while an absent/garbage superblock
 *      is FACADE_E_IO;
 *   3. the teardown-race: abandon() while a facade write is stalled in flight;
 *      the stalled completion must fire and free cleanly (ASan/LSan), no UAF.
 *
 * Compiled -UNDEBUG so assert()/TCHECK stay live in every build type.  Builds
 * the iovecs by hand (data + length) like the fixture expects, so it links no
 * evpl.
 */

#undef NDEBUG
#include <assert.h>
#include <string.h>

#include "evpl/evpl.h"   /* struct evpl_iovec (type + fields only; not linked) */

#include "facade_test_util.h"
#include "facade_internal.h"   /* white-box: struct facade_obj, facade_obj_find */
#include "facade_log.h"
#include "facade_super.h"
#include "sc_request.h"        /* CHIMERA_SC_SEG_DATA */

static void
on_mio(
    enum chimera_media_error st,
    void                    *priv)
{
    (void) st;
    *(int *) priv = 1;
} /* on_mio */

/* Read one block at `lba` into out[BS] via the fixture (manual iovec). */
static void
read_block(
    struct chimera_media_space *space,
    uint64_t                    lba,
    uint8_t                    *out)
{
    struct evpl_iovec iov;
    uint8_t          *buf  = malloc(CHIMERA_MEDIA_BLOCK_SIZE);
    int               done = 0;

    TCHECK(buf != NULL, "oom read buf");
    memset(&iov, 0, sizeof(iov));
    iov.data   = buf;
    iov.length = CHIMERA_MEDIA_BLOCK_SIZE;
    facade_test_media()->read(space, &iov, 1, lba, 1, on_mio, &done);
    TCHECK(done, "read not completed");
    memcpy(out, buf, CHIMERA_MEDIA_BLOCK_SIZE);
    free(buf);
} /* read_block */

/* Write one 4 KiB block `buf` at `lba` (FUA) via the fixture. */
static void
write_block(
    struct chimera_media_space *space,
    uint64_t                    lba,
    const void                 *buf)
{
    struct evpl_iovec iov;
    int               done = 0;

    memset(&iov, 0, sizeof(iov));
    iov.data   = (void *) (uintptr_t) buf;
    iov.length = CHIMERA_MEDIA_BLOCK_SIZE;
    facade_test_media()->write(space, &iov, 1, lba, 1, CHIMERA_MEDIA_WRITE_FUA,
                               on_mio, &done);
    TCHECK(done, "write not completed");
} /* write_block */

/* ---- durable write round-trip across a clean reopen ---- */

static void
test_roundtrip(void)
{
    struct chimera_media_space *space;
    struct facade_alloc        *a;
    uint8_t                     id[16] = { 0x01 };
    uint8_t                     pat[64], blk[CHIMERA_MEDIA_BLOCK_SIZE];
    struct facade_obj          *o;
    struct tu_epoch_w           ew = { FACADE_E_IO, 0, 0 };
    unsigned                    i;

    for (i = 0; i < sizeof(pat); i++) {
        pat[i] = (uint8_t) (i * 7u + 3u);
    }

    facade_test_media_reset();
    space = tu_media_open("rt", 4u << 20);
    tu_format(space);
    a = tu_open(space);

    facade_obj_write_durable(a, id, pat, sizeof(pat), tu_on_epoch, &ew);
    TCHECK(ew.done && ew.st == FACADE_OK, "durable write failed");

    tu_close(a);
    a = tu_open(space);

    o = facade_obj_find(a, id);
    TCHECK(o != NULL && o->nextents == 1, "object missing after reopen");
    read_block(space, o->ext[0].phys_block, blk);
    TCHECK(memcmp(blk, pat, sizeof(pat)) == 0, "object bytes differ after reopen");

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
} /* test_roundtrip */

/* ---- L41: FACADE_E_FORMAT vs FACADE_E_IO ---- */

static void
test_format_refusal(void)
{
    struct chimera_media_space *space;
    struct tu_open_w            ow = { FACADE_E_IO, NULL, 0 };
    void                       *blk;

    /* A recognized superblock with an unsupported version => FACADE_E_FORMAT. */
    facade_test_media_reset();
    space = tu_media_open("fmt", 4u << 20);
    tu_format(space);

    blk = calloc(1, FACADE_SUPER_SIZE);
    TCHECK(blk != NULL, "oom");
    {
        struct facade_super s;
        memset(&s, 0, sizeof(s));
        s.magic      = FACADE_SUPER_MAGIC;
        s.version    = FACADE_FORMAT_VERSION + 99u;   /* unsupported */
        s.block_size = CHIMERA_MEDIA_BLOCK_SIZE;
        memcpy(blk, &s, sizeof(s));
        ((struct facade_super *) blk)->crc32 = facade_crc32(blk, FACADE_SUPER_SIZE);
    }
    write_block(space, FACADE_SUPER_BLOCK, blk);
    tu_media_close(space);

    space   = tu_media_open("fmt", 4u << 20);   /* crash view: durable image */
    ow.done = 0;
    facade_alloc_open(facade_test_media(), space, tu_on_open, &ow);
    TCHECK(ow.done && ow.st == FACADE_E_FORMAT,
           "unsupported version must be FACADE_E_FORMAT, got %d", ow.st);
    tu_media_close(space);
    free(blk);

    /* A zeroed/garbage superblock => FACADE_E_IO (not blank, not FORMAT). */
    facade_test_media_reset();
    space = tu_media_open("io", 4u << 20);
    tu_format(space);

    blk = calloc(1, FACADE_SUPER_SIZE);   /* all zero: magic == 0 */
    TCHECK(blk != NULL, "oom");
    write_block(space, FACADE_SUPER_BLOCK, blk);
    tu_media_close(space);

    space   = tu_media_open("io", 4u << 20);
    ow.done = 0;
    ow.st   = FACADE_OK;
    facade_alloc_open(facade_test_media(), space, tu_on_open, &ow);
    TCHECK(ow.done && ow.st == FACADE_E_IO,
           "garbage superblock must be FACADE_E_IO, got %d", ow.st);
    tu_media_close(space);
    free(blk);
    facade_test_media_reset();
} /* test_format_refusal */

/* ---- teardown-race: abandon() while a facade write is stalled in flight ---- */

static void
test_teardown_race(void)
{
    struct chimera_media_space *space;
    struct facade_alloc        *a;
    uint8_t                     id[16] = { 0x02 };
    uint8_t                     pat[64];
    struct tu_epoch_w           ew = { FACADE_E_IO, 0, 0 };

    memset(pat, 0x5A, sizeof(pat));

    facade_test_media_reset();
    space = tu_media_open("race", 4u << 20);
    tu_format(space);
    a = tu_open(space);

    /* Stall the fixture: the first write of the durable-write chain (the
     * clean=0 dirty-mark superblock write) is queued PENDING. */
    facade_test_media_stall_writes(space, 1);
    facade_obj_write_durable(a, id, pat, sizeof(pat), tu_on_epoch, &ew);
    TCHECK(!ew.done, "write should be stalled, not yet complete");
    TCHECK(facade_test_media_pending_count(space) > 0, "expected a pending write");

    /* Abandon with the write in flight: teardown is deferred until it drains. */
    facade_alloc_abandon(a);

    /* Release the stalled completion(s): the chain must fire its callback and
     * free everything with no use-after-free (ASan) and no leak (LSan). */
    facade_test_media_release_all(space);
    TCHECK(ew.done, "stalled write completion must fire after release");
    TCHECK(ew.st == FACADE_E_SHUTDOWN || ew.st == FACADE_OK,
           "abandoned write reports a terminal status, got %d", ew.st);

    tu_media_close(space);
    facade_test_media_reset();
} /* test_teardown_race */

/* ---- HIGH: block-0 serialization (mark_dirty vs id-floor extension) ---- */

static void
test_block0_serialization(void)
{
    struct chimera_media_space *space;
    struct facade_alloc        *a;
    struct tu_draw              d, d2;
    struct tu_epoch_w           ew = { FACADE_E_IO, 0, 0 };
    struct facade_obj          *o;
    uint8_t                     id[16] = { 0x11 };
    uint8_t                     pat[64], rb[CHIMERA_MEDIA_BLOCK_SIZE];
    uint64_t                    raced_seq, nonce;
    unsigned                    i;

    for (i = 0; i < sizeof(pat); i++) {
        pat[i] = (uint8_t) (i + 0x90u);
    }

    facade_test_media_reset();
    space = tu_media_open("b0", 4u << 20);
    tu_format(space);
    a = tu_open(space);
    /* Reopen so the next draw sits at the durable floor and must extend it. */
    tu_close(a);
    a = tu_open(space);

    /* Hold the floor-extension's block-0 write in flight. */
    facade_test_media_stall_writes(space, 1);
    TCHECK(tu_draw_start(&d, a) == FACADE_AGAIN, "draw at the floor should park");
    TCHECK(facade_test_media_pending_count(space) == 1,
           "one block-0 write (the extension) in flight, got %d",
           facade_test_media_pending_count(space));

    /* A durable object write starts with a clean=0 dirty-mark (a block-0 write).
     * It MUST defer behind the in-flight extension, not start a second block-0
     * write -- the F1 single-writer invariant. */
    facade_obj_write_durable(a, id, pat, sizeof(pat),
                             tu_on_epoch, &ew);
    TCHECK(facade_test_media_pending_count(space) == 1,
           "mark_dirty must not race block 0: pending=%d",
           facade_test_media_pending_count(space));
    TCHECK(ew.done == 0, "obj write must not complete while the extension is in flight");

    /* Drain: extension completes, the parked draw issues, the deferred dirty-mark
     * and the rest of the obj write run -- all serialized on block 0. */
    facade_test_media_release_all(space);
    TCHECK(ew.done && ew.st == FACADE_OK, "obj write should complete cleanly");
    TCHECK(d.got == 1, "the parked draw should have issued an id");
    raced_seq = tu_id_seq(&d.id);
    nonce     = tu_id_nonce(&d.id);

    /* Crash reopen: durable image only.  clean=0 landed, so recovery runs and the
     * object survives; the id floor did not regress (no lost block-0 update). */
    facade_alloc_abandon(a);   /* drop the pre-crash handle (no media writes) */
    tu_media_close(space);
    space = tu_media_open("b0", 4u << 20);
    a     = tu_open(space);

    o = facade_obj_find(a, id);
    TCHECK(o != NULL && o->nextents == 1, "object lost across crash (block-0 race?)");
    read_block(space, o->ext[0].phys_block, rb);
    TCHECK(memcmp(rb, pat, sizeof(pat)) == 0, "object bytes differ after crash");

    TCHECK(tu_draw_start(&d2, a) == FACADE_OK && d2.got == 1, "post-crash draw");
    TCHECK(tu_id_nonce(&d2.id) == nonce, "instance nonce changed across crash");
    TCHECK(tu_id_seq(&d2.id) > raced_seq,
           "durable id floor regressed (block-0 lost update => id reuse): %llu <= %llu",
           (unsigned long long) tu_id_seq(&d2.id), (unsigned long long) raced_seq);

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
} /* test_block0_serialization */

/* ---- MEDIUM: rollback on a mid-transaction media-write failure ---- */

static void
test_write_failure_rollback(void)
{
    struct chimera_media_space *space;
    struct facade_alloc        *a;
    uint8_t                     id[16] = { 0x07 };
    uint8_t                     pat[64];
    struct tu_epoch_w           ew = { FACADE_E_IO, 0, 0 };
    uint64_t                    free_before;

    memset(pat, 0x33, sizeof(pat));

    facade_test_media_reset();
    space = tu_media_open("rb", 4u << 20);
    tu_format(space);
    a = tu_open(space);

    free_before = facade_alloc_free_blocks(a);

    /* Fail the OBJ record write (txn records are ALLOC@log+0, OBJ@log+1,
     * COMMIT@log+2): the object entry was already put and a data block
     * allocated, so this exercises both rollback branches. */
    facade_test_media_fail_write_lba(space, a->log_block + 1);

    facade_obj_write_durable(a, id, pat, sizeof(pat), tu_on_epoch, &ew);
    TCHECK(ew.done && ew.st != FACADE_OK, "write over a failing OBJ record must fail");
    TCHECK(facade_obj_find(a, id) == NULL,
           "a failed write must leave no phantom object");
    TCHECK(facade_alloc_free_blocks(a) == free_before,
           "a failed write must return its allocated block (%llu != %llu)",
           (unsigned long long) facade_alloc_free_blocks(a),
           (unsigned long long) free_before);

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
} /* test_write_failure_rollback */

/* ---- MEDIUM: recovery skips an UNcommitted transaction mid-prefix ---- */

/* Build one log record block exactly as facade_lr_build does. */
static void
build_rec(
    uint8_t            *block,
    enum facade_lr_type type,
    uint64_t            seq,
    uint64_t            txn,
    const void         *payload,
    uint32_t            len)
{
    struct facade_lr_hdr *h = (struct facade_lr_hdr *) block;
    uint32_t              hc;

    memset(block, 0, CHIMERA_MEDIA_BLOCK_SIZE);
    h->magic    = FACADE_LR_MAGIC;
    h->seq      = seq;
    h->txn      = txn;
    h->type     = (uint32_t) type;
    h->format   = FACADE_LR_FORMAT;
    h->len      = len;
    h->hdr_crc  = 0;
    h->body_crc = 0;
    h->reserved = 0;
    hc          = facade_crc32(h, (uint32_t) sizeof(*h));
    if (len) {
        memcpy(block + sizeof(*h), payload, len);
    }
    h->body_crc = facade_crc32(block + sizeof(*h), len);
    h->hdr_crc  = hc;
} /* build_rec */

static void
write_obj_rec(
    struct chimera_media_space *space,
    uint64_t                    lba,
    uint64_t                    seq,
    uint64_t                    txn,
    const uint8_t               id[16],
    uint64_t                    phys)
{
    uint8_t                    blk[CHIMERA_MEDIA_BLOCK_SIZE];
    uint8_t                    payload[sizeof(struct facade_lr_obj)
                                       + sizeof(struct facade_ckpt_objext)];
    struct facade_lr_obj      *orec = (struct facade_lr_obj *) payload;
    struct facade_ckpt_objext *xr   = (struct facade_ckpt_objext *) (payload + sizeof(*orec));

    memset(payload, 0, sizeof(payload));
    memcpy(orec->obj_id, id, 16);
    orec->flags       = 0;
    orec->nextents    = 1;
    xr->logical_block = 0;
    xr->phys_block    = phys;
    xr->nblocks       = 1;
    xr->kind          = CHIMERA_SC_SEG_DATA;
    build_rec(blk, FACADE_LR_OBJ, seq, txn, payload, (uint32_t) sizeof(payload));
    write_block(space, lba, blk);
} /* write_obj_rec */

static void
write_alloc_rec(
    struct chimera_media_space *space,
    uint64_t                    lba,
    uint64_t                    seq,
    uint64_t                    txn,
    uint64_t                    phys)
{
    uint8_t                blk[CHIMERA_MEDIA_BLOCK_SIZE];
    struct facade_lr_alloc ar = { phys, 1 };

    build_rec(blk, FACADE_LR_ALLOC, seq, txn, &ar, (uint32_t) sizeof(ar));
    write_block(space, lba, blk);
} /* write_alloc_rec */

static void
test_recover_skips_uncommitted_midprefix(void)
{
    struct chimera_media_space *space;
    struct facade_alloc        *a;
    uint8_t                     id1[16] = { 0xE1 };
    uint8_t                     id2[16] = { 0xE2 };
    uint8_t                     id3[16] = { 0xE3 };
    uint8_t                     patA[64], rb[CHIMERA_MEDIA_BLOCK_SIZE];
    struct tu_epoch_w           ew = { FACADE_E_IO, 0, 0 };
    uint64_t                    log_block, data_last, x1, x3;
    uint8_t                     cblk[CHIMERA_MEDIA_BLOCK_SIZE];
    struct facade_lr_commit     commit;
    unsigned                    i;

    for (i = 0; i < sizeof(patA); i++) {
        patA[i] = (uint8_t) (i ^ 0x5Au);
    }

    facade_test_media_reset();
    space = tu_media_open("ptxn", 4u << 20);
    tu_format(space);
    a         = tu_open(space);
    log_block = a->log_block;
    data_last = a->data_last;

    /* txn1: a real durable write (records at log_block + 0,1,2; sets clean=0). */
    facade_obj_write_durable(a, id1, patA, sizeof(patA), tu_on_epoch, &ew);
    TCHECK(ew.done && ew.st == FACADE_OK, "txn1 write");
    x1 = facade_obj_find(a, id1)->ext[0].phys_block;   /* = data_last - 1 (tail cut) */
    x3 = data_last - 2;                                 /* a distinct free block */
    TCHECK(x3 != x1 && x3 >= a->data_first, "distinct free block for txn3");

    /* Fabricate, in the durable log, an UNcommitted txn4 (seq 4,5) sandwiched
     * before a committed txn6 (seq 6,7,8), all contiguous. */
    write_alloc_rec(space, log_block + 3, 4, 4, data_last - 3);   /* txn4 ALLOC (X2) */
    write_obj_rec(space, log_block + 4, 5, 4, id2, data_last - 3);/* txn4 OBJ (no commit) */
    write_alloc_rec(space, log_block + 5, 6, 6, x3);    /* txn6 ALLOC */
    write_obj_rec(space, log_block + 6, 7, 6, id3, x3); /* txn6 OBJ */
    memset(&commit, 0, sizeof(commit));
    commit.txn      = 6;
    commit.nrecords = 3;
    build_rec(cblk, FACADE_LR_COMMIT, 8, 6, &commit, (uint32_t) sizeof(commit));
    write_block(space, log_block + 7, cblk);

    /* Crash reopen: recovery reads seq 1..8 (no gap).  txn1 + txn6 are committed
     * and applied; txn4 has no COMMIT and must be skipped even though txn6 after
     * it commits. */
    facade_alloc_abandon(a);   /* drop the pre-crash handle (no media writes) */
    tu_media_close(space);
    space = tu_media_open("ptxn", 4u << 20);
    a     = tu_open(space);

    TCHECK(facade_obj_find(a, id1) != NULL, "committed txn1 object must survive");
    read_block(space, facade_obj_find(a, id1)->ext[0].phys_block, rb);
    TCHECK(memcmp(rb, patA, sizeof(patA)) == 0, "txn1 bytes");
    TCHECK(facade_obj_find(a, id2) == NULL,
           "UNcommitted mid-prefix txn must be skipped");
    TCHECK(facade_obj_find(a, id3) != NULL, "committed txn3 object must survive");

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
} /* test_recover_skips_uncommitted_midprefix */

/* ---- CRITICAL: clean=0 re-marked after an intra-write compaction ---- */

static void
test_recover_after_intrawrite_compaction(void)
{
    struct chimera_media_space *space;
    struct facade_alloc        *a;
    uint8_t                     id_pre[16]  = { 0xC1 };
    uint8_t                     id_trig[16] = { 0xC2 };
    uint8_t                     patP[64], patT[64], rb[CHIMERA_MEDIA_BLOCK_SIZE];
    struct tu_epoch_w           ew = { FACADE_E_IO, 0, 0 };
    uint64_t                    ckpt_gen_before, plg_before;
    unsigned                    i;

    for (i = 0; i < 64; i++) {
        patP[i] = (uint8_t) (i + 0xA0u);
        patT[i] = (uint8_t) (i ^ 0x3Cu);
    }

    /* A small space => a small log (log_nblocks == 4), so the 2nd durable write
     * forces a compaction mid-write. */
    facade_test_media_reset();
    space = tu_media_open("iwc", 256u << 10);   /* 64 blocks */
    tu_format(space);
    a = tu_open(space);
    TCHECK(a->log_nblocks <= 8, "test needs a small log, got %llu",
           (unsigned long long) a->log_nblocks);

    /* Pre-compaction object: its records fill the log toward the wrap point. */
    facade_obj_write_durable(a, id_pre, patP, sizeof(patP), tu_on_epoch, &ew);
    TCHECK(ew.done && ew.st == FACADE_OK, "pre-compaction write");

    ckpt_gen_before = a->ckpt_generation;

    /* Triggering object: its transaction does not fit, so it compacts first
     * (folding id_pre into the checkpoint), then appends its own records to the
     * reset log. */
    ew.done = 0;
    facade_obj_write_durable(a, id_trig, patT, sizeof(patT), tu_on_epoch, &ew);
    TCHECK(ew.done && ew.st == FACADE_OK, "triggering write");
    TCHECK(a->ckpt_generation > ckpt_gen_before,
           "the triggering write must force a compaction");

    plg_before = a->power_loss_generation;

    /* Crash IMMEDIATELY after the triggering write -- no further write.  The
     * triggering object lives only in the reset log, so it survives only if
     * clean=0 was re-marked after the intra-write compaction. */
    facade_alloc_abandon(a);
    tu_media_close(space);
    space = tu_media_open("iwc", 256u << 10);
    a     = tu_open(space);

    TCHECK(a->power_loss_generation > plg_before,
           "recovery must run (and bump the generation) after an intra-write "
           "compaction: clean=1 window => recovery skipped (gen %llu <= %llu)",
           (unsigned long long) a->power_loss_generation,
           (unsigned long long) plg_before);

    {
        struct facade_obj *o = facade_obj_find(a, id_trig);

        TCHECK(o != NULL && o->nextents == 1,
               "post-compaction durable object LOST (clean=1 window, HA-3)");
        read_block(space, o->ext[0].phys_block, rb);
        TCHECK(memcmp(rb, patT, sizeof(patT)) == 0, "triggering object bytes");
    }
    {
        struct facade_obj *o = facade_obj_find(a, id_pre);

        TCHECK(o != NULL && o->nextents == 1, "pre-compaction object lost");
        read_block(space, o->ext[0].phys_block, rb);
        TCHECK(memcmp(rb, patP, sizeof(patP)) == 0, "pre-compaction object bytes");
    }

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();
} /* test_recover_after_intrawrite_compaction */

int
main(void)
{
    test_roundtrip();
    test_format_refusal();
    test_teardown_race();
    test_block0_serialization();
    test_write_failure_rollback();
    test_recover_skips_uncommitted_midprefix();
    test_recover_after_intrawrite_compaction();
    printf("facade_log_test: ok\n");
    return 0;
} /* main */
