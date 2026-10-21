// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * data-verification P1: per-block checksum PLUMBING and the v5 on-media format
 * (no verify-on-read yet -- that is P2).  A white-box test (it reaches into
 * facade_internal.h for the object table and the extent map) over the crash-sim
 * + loopback providers, covering:
 *
 *   1. SIZE PINS: sizeof(facade_ckpt_objext) == 40, sizeof(facade_obj_rec) == 136,
 *      and the one-block OBJ log-record fit 48 + 136 + 64*40 == 2744 <= 4096.
 *   2. ROUND-TRIP: a DATA extent's data_csum (facade_crc32 over the stored 4 KiB
 *      block) survives crash + recover (LOG replay) and a subsequent reopen
 *      (CHECKPOINT load); UNWRITTEN extents carry NONE.
 *   3. LOAD GUARD: facade_obj_put_rec refuses an OBJ record whose DATA extent
 *      spans more than one block (the inline csum covers exactly one block).
 *   4. N4 ORACLE: a format==1 OBJ log record under a v5 superblock STOPS the
 *      durable prefix (facade_recover.c) -- the record is NOT applied and
 *      facade_alloc_open still SUCCEEDS; it is NOT FACADE_E_FORMAT.
 *   5. VERSION REFUSAL: a v4 superblock IS refused with FACADE_E_FORMAT.
 *
 * Unprivileged: the delegate is the pread loopback on a sparse temp file.
 */

#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "evpl/evpl.h"

#include "crashsim/chimera_media_crashsim.h"
#include "loopback/media_loopback.h"

#include "facade_internal.h"   /* white-box: struct facade_alloc, facade_super */
#include "facade_extent.h"
#include "facade_log.h"
#include "facade_object.h"

#define ONE_GIB (1ULL << 30)
#define BS      CHIMERA_MEDIA_BLOCK_SIZE

#define CHECK(cond)                                                      \
        do {                                                             \
            if (!(cond)) {                                               \
                fprintf(stderr, "CHECK failed: %s (%s:%d)\n",            \
                        #cond, __FILE__, __LINE__);                      \
                exit(1);                                                 \
            }                                                            \
        } while (0)

struct ts {
    struct evpl                                 *evpl;
    struct chimera_media_crashsim               *handle;
    const struct chimera_media_provider         *provider;
    const struct chimera_media_crashsim_control *control;
    struct chimera_media_space                  *space;
    struct facade_alloc                         *alloc;
    char                                         path[700];
};

static void
pump(
    struct evpl *evpl,
    const int   *done)
{
    while (!*done) {
        evpl_continue(evpl);
    }
} /* pump */

struct open_w {
    int                         done;
    enum chimera_media_error st;
    struct chimera_media_space *space;
};

static void
on_mopen(
    enum chimera_media_error    st,
    struct chimera_media_space *space,
    void                       *priv)
{
    struct open_w *w = priv;

    w->st    = st;
    w->space = space;
    w->done  = 1;
} /* on_mopen */

struct st_w {
    int done;
    enum facade_status st;
};

static void
on_fmt(
    enum facade_status st,
    void              *priv)
{
    struct st_w *w = priv;

    w->st   = st;
    w->done = 1;
} /* on_fmt */

struct open_fa_w {
    int                  done;
    enum facade_status st;
    struct facade_alloc *alloc;
};

static void
on_fopen(
    enum facade_status   st,
    struct facade_alloc *alloc,
    void                *priv)
{
    struct open_fa_w *w = priv;

    w->st    = st;
    w->alloc = alloc;
    w->done  = 1;
} /* on_fopen */

struct epoch_w {
    int      done;
    enum facade_status st;
    uint64_t epoch;
};

static void
on_epoch(
    enum facade_status st,
    uint64_t           epoch,
    void              *priv)
{
    struct epoch_w *w = priv;

    w->st    = st;
    w->epoch = epoch;
    w->done  = 1;
} /* on_epoch */

static void
on_io(
    enum chimera_media_error st,
    void                    *priv)
{
    (void) st;
    *(int *) priv = 1;
} /* on_io */

/* ---- harness ---- */

static void
ts_open_media(
    struct ts  *t,
    const char *dir,
    int         idx)
{
    struct chimera_media_crashsim_config cfg;
    struct open_w                        ow = { 0, CHIMERA_MEDIA_E_IO, NULL };

    memset(&cfg, 0, sizeof(cfg));
    cfg.delegate = chimera_media_loopback();
    cfg.seed     = 0x5a5a + idx;
    cfg.w_keep   = 1;                       /* keep-biased: durable records survive */
    cfg.w_drop   = 0;
    cfg.w_tear   = 0;

    snprintf(t->path, sizeof(t->path), "%s/dev-%d.img", dir, idx);
    CHECK(chimera_media_crashsim_create(&cfg, &t->handle, &t->provider, &t->control) == 0);
    chimera_media_crashsim_open(t->handle, t->evpl, t->path, ONE_GIB, NULL, on_mopen, &ow);
    pump(t->evpl, &ow.done);
    CHECK(ow.st == CHIMERA_MEDIA_OK && ow.space);
    t->space = ow.space;
} /* ts_open_media */

static void
ts_format(struct ts *t)
{
    struct facade_geometry g = { 0 };
    struct st_w            w = { 0, FACADE_E_IO };

    facade_alloc_format(t->provider, t->space, &g, on_fmt, &w);
    pump(t->evpl, &w.done);
    CHECK(w.st == FACADE_OK);
} /* ts_format */

static void
ts_open(struct ts *t)
{
    struct open_fa_w w = { 0, FACADE_E_IO, NULL };

    facade_alloc_open(t->provider, t->space, on_fopen, &w);
    pump(t->evpl, &w.done);
    CHECK(w.st == FACADE_OK && w.alloc);
    t->alloc = w.alloc;
} /* ts_open */

static void
ts_reopen(struct ts *t)
{
    facade_alloc_abandon(t->alloc);
    t->alloc = NULL;
    ts_open(t);
} /* ts_reopen */

static void
ts_crash_recover(struct ts *t)
{
    CHECK(t->control->crash(t->space) == 0);
    CHECK(t->control->recover(t->space) == 0);
} /* ts_crash_recover */

static void
ts_close_media(struct ts *t)
{
    int ok = 0;

    if (t->alloc) {
        facade_alloc_abandon(t->alloc);
        t->alloc = NULL;
    }
    t->provider->close(t->space, on_io, &ok);
    pump(t->evpl, &ok);
    chimera_media_crashsim_destroy(t->handle);
    CHECK(unlink(t->path) == 0);
} /* ts_close_media */

/* Write one FULL 4 KiB block of `data` at block offset `lb` of object `id`,
 * durably.  A full-block write stages `data` verbatim, so the stored block (and
 * thus its data_csum) is facade_crc32(data, BS). */
static void
ts_write_block(
    struct ts    *t,
    const uint8_t id[16],
    uint64_t      lb,
    const void   *data)
{
    struct epoch_w w = { 0, FACADE_E_IO, 0 };

    if (!facade_obj_find(t->alloc, id)) {
        facade_obj_create_entry(t->alloc, id, NULL);
    }
    facade_obj_write(t->alloc, id, lb * BS, data, BS, CHIMERA_SC_WRITE_DURABLE,
                     on_epoch, &w);
    pump(t->evpl, &w.done);
    CHECK(w.st == FACADE_OK);
} /* ts_write_block */

/* Assert object `id`'s extent at block `lb` is DATA with the expected checksum. */
static void
check_block_csum(
    struct ts    *t,
    const uint8_t id[16],
    uint64_t      lb,
    uint32_t      expect_csum)
{
    struct facade_obj   *o = facade_obj_find(t->alloc, id);
    struct facade_extent ext;

    CHECK(o != NULL);
    CHECK(facade_extent_map_lookup(o->map, lb, &ext) == 1);
    CHECK(ext.kind == CHIMERA_SC_SEG_DATA);
    CHECK(ext.csum_algo == FACADE_DATA_CSUM_CRC32);
    CHECK(ext.data_csum == expect_csum);
} /* check_block_csum */

/* Raw media read/write of one block (bypassing the facade), FUA. */
static void
ts_raw_read(
    struct ts *t,
    uint64_t   lba,
    uint8_t   *out)
{
    struct evpl_iovec iov;
    int               done = 0;
    int               n    = evpl_iovec_alloc(t->evpl, BS, BS, 1, 0, &iov);

    CHECK(n == 1);
    t->provider->read(t->space, &iov, 1, lba, 1, on_io, &done);
    pump(t->evpl, &done);
    memcpy(out, evpl_iovec_data(&iov), BS);
    evpl_iovec_release(t->evpl, &iov);
} /* ts_raw_read */

static void
ts_raw_write(
    struct ts     *t,
    uint64_t       lba,
    const uint8_t *blk)
{
    struct evpl_iovec iov;
    int               done = 0;
    int               n    = evpl_iovec_alloc(t->evpl, BS, BS, 1, 0, &iov);

    CHECK(n == 1);
    memcpy(evpl_iovec_data(&iov), blk, BS);
    t->provider->write(t->space, &iov, 1, lba, 1, CHIMERA_MEDIA_WRITE_FUA, on_io, &done);
    pump(t->evpl, &done);
    evpl_iovec_release(t->evpl, &iov);
} /* ts_raw_write */

/* Build one log-record block exactly like facade_lr_build, but with a settable
 * `format` (facade_lr_build always writes FACADE_LR_FORMAT). */
static void
build_lr_block(
    uint8_t    *block,
    uint32_t    type,
    uint64_t    seq,
    uint64_t    txn,
    uint32_t    format,
    const void *payload,
    uint32_t    len)
{
    struct facade_lr_hdr *h = (struct facade_lr_hdr *) block;

    memset(block, 0, BS);
    h->magic    = FACADE_LR_MAGIC;
    h->seq      = seq;
    h->txn      = txn;
    h->type     = type;
    h->format   = format;
    h->len      = len;
    h->hdr_crc  = 0;
    h->body_crc = 0;
    h->reserved = 0;
    /* hdr_crc over the header with both CRC fields zeroed (facade_lr_build). */
    h->hdr_crc = facade_crc32(h, (uint32_t) sizeof(*h));
    if (len) {
        memcpy(block + sizeof(*h), payload, len);
    }
    h->body_crc = facade_crc32(block + sizeof(*h), len);
} /* build_lr_block */

static void
mkid(
    uint8_t id[16],
    uint8_t tag)
{
    memset(id, 0, 16);
    id[0] = tag;
} /* mkid */

static void
fillpat(
    uint8_t *buf,
    uint32_t len,
    uint8_t  seed)
{
    uint32_t i;

    for (i = 0; i < len; i++) {
        buf[i] = (uint8_t) (i * 131u + seed * 7u + 17u);
    }
} /* fillpat */

int
main(void)
{
    const char *tmp = getenv("TMPDIR");
    char        dir[512];
    struct ts   t;

    /* ---- 1: compile/layout size pins (no media) ---- */
    CHECK(sizeof(struct facade_ckpt_objext) == 40);
    CHECK(sizeof(struct facade_obj_rec) == 136);
    CHECK(sizeof(struct facade_lr_hdr) == 48);
    CHECK(sizeof(struct facade_lr_hdr) + sizeof(struct facade_obj_rec)
          + (uint64_t) FACADE_OBJ_MAX_EXT * sizeof(struct facade_ckpt_objext) == 2744);
    CHECK(sizeof(struct facade_lr_hdr) + sizeof(struct facade_obj_rec)
          + (uint64_t) FACADE_OBJ_MAX_EXT * sizeof(struct facade_ckpt_objext)
          <= CHIMERA_MEDIA_BLOCK_SIZE);

    snprintf(dir, sizeof(dir), "%s/facade_dverify_XXXXXX",
             (tmp && tmp[0]) ? tmp : "/tmp");
    CHECK(mkdtemp(dir) != NULL);

    memset(&t, 0, sizeof(t));
    t.evpl = evpl_create(NULL);
    CHECK(t.evpl != NULL);

    /* ---- 2: data_csum round-trips through LOG replay AND CHECKPOINT load ---- */
    {
        uint8_t  id[16];
        uint8_t  b0[BS], b1[BS], b2[BS];
        uint32_t c0, c1, c2;

        mkid(id, 0x01);
        fillpat(b0, BS, 0x10);
        fillpat(b1, BS, 0x20);
        fillpat(b2, BS, 0x30);
        c0 = facade_crc32(b0, BS);
        c1 = facade_crc32(b1, BS);
        c2 = facade_crc32(b2, BS);
        /* distinct blocks must have distinct checksums for the test to bite */
        CHECK(c0 != c1 && c1 != c2 && c0 != c2);

        ts_open_media(&t, dir, 1);
        ts_format(&t);
        ts_open(&t);

        ts_write_block(&t, id, 0, b0);
        ts_write_block(&t, id, 1, b1);
        ts_write_block(&t, id, 2, b2);

        /* (a) in-memory, straight off the write path. */
        check_block_csum(&t, id, 0, c0);
        check_block_csum(&t, id, 1, c1);
        check_block_csum(&t, id, 2, c2);

        /* (b) LOG replay: crash + recover replays the OBJ log records (carrying
         * the objext csum) via facade_obj_put_rec, then folds them into a fresh
         * checkpoint; reopen rebuilds the map. */
        ts_crash_recover(&t);
        ts_reopen(&t);
        check_block_csum(&t, id, 0, c0);
        check_block_csum(&t, id, 1, c1);
        check_block_csum(&t, id, 2, c2);

        /* (c) CHECKPOINT load: a second reopen (no new writes) loads the object
         * from the checkpoint section (facade_ckpt_load). */
        ts_reopen(&t);
        check_block_csum(&t, id, 0, c0);
        check_block_csum(&t, id, 1, c1);
        check_block_csum(&t, id, 2, c2);

        ts_close_media(&t);
    }

    /* ---- 3: facade_obj_put_rec refuses a multi-block DATA extent ---- */
    {
        struct facade_obj_rec     hdr;
        struct facade_ckpt_objext ext;

        ts_open_media(&t, dir, 2);
        ts_format(&t);
        ts_open(&t);

        memset(&hdr, 0, sizeof(hdr));
        hdr.obj_id[0] = 0xEE;
        hdr.state     = CHIMERA_SC_OBJ_OPEN;
        hdr.nextents  = 1;

        memset(&ext, 0, sizeof(ext));
        ext.logical_block = 0;
        ext.phys_block    = t.alloc->data_first;
        ext.nblocks       = 2;                 /* > 1: forbidden for DATA */
        ext.kind          = CHIMERA_SC_SEG_DATA;
        ext.owned         = 1;

        CHECK(facade_obj_put_rec(t.alloc, &hdr, &ext) == NULL);

        /* A single-block DATA extent is accepted (control). */
        ext.nblocks = 1;
        CHECK(facade_obj_put_rec(t.alloc, &hdr, &ext) != NULL);

        ts_close_media(&t);
    }

    /* ---- 4: a format==1 OBJ log record under a v5 superblock stops the prefix
     *         (record not applied, open OK) -- NOT FACADE_E_FORMAT. ---- */
    {
        uint8_t               id[16], idv1[16];
        uint8_t               b[BS], rec[BS];
        struct facade_obj_rec orec;
        uint64_t              log_block, slot, seq, cseq;

        mkid(id,   0x41);
        mkid(idv1, 0x4F);
        fillpat(b, BS, 0x44);

        ts_open_media(&t, dir, 3);
        ts_format(&t);
        ts_open(&t);

        log_block = t.alloc->log_block;
        ts_write_block(&t, id, 0, b);          /* ALLOC, OBJ, COMMIT */

        /* The next durable slot is where a stray record would sit. */
        seq  = t.alloc->next_seq;
        cseq = t.alloc->checkpoint_seq;
        slot = seq - cseq - 1;                 /* contiguous log, no compaction */

        memset(&orec, 0, sizeof(orec));
        memcpy(orec.obj_id, idv1, 16);
        orec.state    = CHIMERA_SC_OBJ_OPEN;
        orec.nextents = 0;
        build_lr_block(rec, FACADE_LR_OBJ, seq, seq /* txn */, 1u /* v1 format */,
                       &orec, (uint32_t) sizeof(orec));
        ts_raw_write(&t, log_block + slot, rec);

        ts_crash_recover(&t);
        ts_reopen(&t);                         /* open must SUCCEED (not FORMAT) */

        CHECK(facade_obj_find(t.alloc, id) != NULL);     /* before the gap: applied */
        CHECK(facade_obj_find(t.alloc, idv1) == NULL);   /* the v1 record: dropped */

        ts_close_media(&t);
    }

    /* ---- 5: a v4 superblock is refused with FACADE_E_FORMAT ---- */
    {
        uint8_t             sb[BS];
        struct facade_super s;
        struct open_fa_w    w = { 0, FACADE_E_IO, NULL };

        ts_open_media(&t, dir, 4);
        ts_format(&t);                         /* writes a v5 superblock */

        ts_raw_read(&t, FACADE_SUPER_BLOCK, sb);
        memcpy(&s, sb, sizeof(s));
        CHECK(s.magic == FACADE_SUPER_MAGIC && s.version == FACADE_FORMAT_VERSION);
        s.version = 4u;                        /* downgrade the version field */
        s.crc32   = 0;
        memcpy(sb, &s, sizeof(s));
        s.crc32 = facade_crc32(sb, FACADE_SUPER_SIZE);
        memcpy(sb, &s, sizeof(s));
        ts_raw_write(&t, FACADE_SUPER_BLOCK, sb);

        facade_alloc_open(t.provider, t.space, on_fopen, &w);
        pump(t.evpl, &w.done);
        CHECK(w.st == FACADE_E_FORMAT);

        t.alloc = NULL;                        /* open failed: no handle to abandon */
        ts_close_media(&t);
    }

    evpl_destroy(t.evpl);
    CHECK(rmdir(dir) == 0);
    printf("facade_data_verify_test: OK\n");
    return 0;
} /* main */
