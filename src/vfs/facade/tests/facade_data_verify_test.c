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
 *      spans more than one block OR carries no usable (CRC32) checksum (H1) --
 *      the inline csum covers exactly one block and must be verifiable.
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
#include "facade_ns.h"         /* facade_ns_txn (N5 freeze oracle) */
#include "common/crypto.h"     /* chimera_crypto_digest (expected object MD5) */
#include "sc_types.h"          /* chimera_sc_obj_meta, CHIMERA_SC_CHECKSUM_MD5 */

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

/* Write `len` bytes at byte `offset` of object `id`; returns the facade status
 * (does not CHECK -- the caller asserts).  A len < BS write over an existing
 * DATA block takes the RMW pre-read path (facade_write_after_read). */
static enum facade_status
ts_write_sub(
    struct ts    *t,
    const uint8_t id[16],
    uint64_t      offset,
    const void   *data,
    uint32_t      len)
{
    struct epoch_w w = { 0, FACADE_E_IO, 0 };

    facade_obj_write(t->alloc, id, offset, data, len, CHIMERA_SC_WRITE_DURABLE,
                     on_epoch, &w);
    pump(t->evpl, &w.done);
    return w.st;
} /* ts_write_sub */

struct read_w {
    int      done;
    enum facade_status st;
    uint32_t len;
    uint64_t size;
};

static void
on_read(
    enum facade_status st,
    uint32_t           len,
    uint64_t           size,
    void              *priv)
{
    struct read_w *w = priv;

    w->st   = st;
    w->len  = len;
    w->size = size;
    w->done = 1;
} /* on_read */

/* Read `len` bytes at byte `offset` of object `id` into out; returns the facade
 * status and (via r_len) the reported length.  out is the caller buffer the
 * verify path must leave untouched on a DATA_UNRECOVERABLE failure. */
static enum facade_status
ts_read(
    struct ts    *t,
    const uint8_t id[16],
    uint64_t      offset,
    void         *out,
    uint32_t      len,
    uint32_t     *r_len)
{
    struct read_w w = { 0, FACADE_E_IO, 0, 0 };

    facade_obj_read(t->alloc, id, offset, len, out, on_read, &w);
    pump(t->evpl, &w.done);
    if (r_len) {
        *r_len = w.len;
    }
    return w.st;
} /* ts_read */

/* The physical lba backing block `lb` of object `id` (for raw corruption). */
static uint64_t
ts_phys(
    struct ts    *t,
    const uint8_t id[16],
    uint64_t      lb)
{
    struct facade_obj   *o = facade_obj_find(t->alloc, id);
    struct facade_extent ext;

    CHECK(o != NULL);
    CHECK(facade_extent_map_lookup(o->map, lb, &ext) == 1 &&
          ext.kind == CHIMERA_SC_SEG_DATA);
    return ext.phys_block + (lb - ext.logical_block);
} /* ts_phys */

/* ---- P3 (object MD5 at SEAL) helpers ---- */

static enum facade_status
ts_seal(
    struct ts    *t,
    const uint8_t id[16])
{
    struct epoch_w w = { 0, FACADE_E_IO, 0 };

    facade_obj_seal(t->alloc, id, on_epoch, &w);
    pump(t->evpl, &w.done);
    return w.st;
} /* ts_seal */

/* Dispatch a SEAL but do NOT wait: leaves o->sealing set and the hash chain in
 * flight, for the freeze-interleave oracles.  *w receives the completion. */
static void
ts_seal_start(
    struct ts      *t,
    const uint8_t   id[16],
    struct epoch_w *w)
{
    w->done = 0;
    w->st   = FACADE_E_IO;
    facade_obj_seal(t->alloc, id, on_epoch, w);
} /* ts_seal_start */

static enum facade_status
ts_truncate(
    struct ts    *t,
    const uint8_t id[16],
    uint64_t      new_size)
{
    struct epoch_w w = { 0, FACADE_E_IO, 0 };

    facade_obj_truncate(t->alloc, id, new_size, on_epoch, &w);
    pump(t->evpl, &w.done);
    return w.st;
} /* ts_truncate */

static enum facade_status
ts_allocate(
    struct ts    *t,
    const uint8_t id[16],
    uint64_t      offset,
    uint64_t      length)
{
    struct epoch_w w = { 0, FACADE_E_IO, 0 };

    facade_obj_allocate(t->alloc, id, offset, length, on_epoch, &w);
    pump(t->evpl, &w.done);
    return w.st;
} /* ts_allocate */

static enum facade_status
ts_punch(
    struct ts    *t,
    const uint8_t id[16],
    uint64_t      offset,
    uint64_t      length)
{
    struct epoch_w w = { 0, FACADE_E_IO, 0 };

    facade_obj_punch(t->alloc, id, offset, length, on_epoch, &w);
    pump(t->evpl, &w.done);
    return w.st;
} /* ts_punch */

struct cb_w {
    int done;
    enum facade_status st;
};

static void
on_cb(
    enum facade_status st,
    void              *priv)
{
    struct cb_w *w = priv;

    w->st   = st;
    w->done = 1;
} /* on_cb */

static enum facade_status
ts_discard(
    struct ts    *t,
    const uint8_t id[16])
{
    struct cb_w w = { 0, FACADE_E_IO };

    facade_obj_discard(t->alloc, id, on_cb, &w);
    pump(t->evpl, &w.done);
    return w.st;
} /* ts_discard */

/* obj_stat of `id` into m; returns 0 on success. */
static int
ts_stat(
    struct ts                  *t,
    const uint8_t               id[16],
    struct chimera_sc_obj_meta *m)
{
    return facade_obj_stat(t->alloc, id, m);
} /* ts_stat */

/* o->change of a live object (white-box, for the freeze oracle). */
static uint64_t
ts_change(
    struct ts    *t,
    const uint8_t id[16])
{
    struct facade_obj *o = facade_obj_find(t->alloc, id);

    CHECK(o != NULL);
    return o->change;
} /* ts_change */

struct ns_w {
    int                         done;
    struct facade_ns_txn_result res;
};

static void
on_ns(
    const struct facade_ns_txn_result *r,
    void                              *priv)
{
    struct ns_w *w = priv;

    w->res  = *r;
    w->done = 1;
} /* on_ns */

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
        ext.kind          = CHIMERA_SC_SEG_DATA;
        ext.owned         = 1;

        /* A multi-block DATA extent is refused by the nblocks guard (even when
         * correctly CRC32-tagged). */
        ext.nblocks   = 2;
        ext.csum_algo = FACADE_DATA_CSUM_CRC32;
        CHECK(facade_obj_put_rec(t.alloc, &hdr, &ext) == NULL);

        /* A single-block DATA extent with NO usable checksum (NONE) is refused
         * too -- it could otherwise be read from media unverified (H1). */
        ext.nblocks   = 1;
        ext.csum_algo = FACADE_DATA_CSUM_NONE;
        CHECK(facade_obj_put_rec(t.alloc, &hdr, &ext) == NULL);

        /* A single-block, CRC32-tagged DATA extent is accepted (control). */
        ext.csum_algo = FACADE_DATA_CSUM_CRC32;
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

    /* ---- 6 (P2): verify-on-read -- a corrupted stored block reads back
     *      DATA_UNRECOVERABLE with NO bytes delivered, a clean block reads back
     *      its bytes, and a hole read is not verified. ---- */
    {
        uint8_t  id[16], idh[16];
        uint8_t  b[BS], out[BS], hole[BS];
        uint64_t phys;
        uint32_t rlen;
        int      i;

        mkid(id,  0x61);
        mkid(idh, 0x6E);
        fillpat(b, BS, 0x66);

        ts_open_media(&t, dir, 5);
        ts_format(&t);
        ts_open(&t);

        ts_write_block(&t, id, 0, b);          /* full block at lb 0, durable */
        ts_crash_recover(&t);                  /* drain: the delegate is authoritative */
        ts_reopen(&t);

        /* a clean read returns the right bytes (verify passes). */
        memset(out, 0xCD, BS);
        CHECK(ts_read(&t, id, 0, out, BS, &rlen) == FACADE_OK);
        CHECK(rlen == BS && memcmp(out, b, BS) == 0);

        /* corrupt the stored block; the read must fail and deliver no bytes. */
        phys = ts_phys(&t, id, 0);
        CHECK(t.control->corrupt(t.space, phys, 1) == 0);
        memset(out, 0xCD, BS);                 /* sentinel */
        CHECK(ts_read(&t, id, 0, out, BS, &rlen) == FACADE_E_DATA_UNRECOVERABLE);
        CHECK(rlen == 0);                      /* no length reported */
        for (i = 0; i < (int) BS; i++) {
            CHECK(out[i] == 0xCD);             /* caller buffer byte-unchanged */
        }

        /* a hole read (kind != DATA) is zeros and is NOT verified. */
        fillpat(hole, BS, 0x6A);
        ts_write_block(&t, idh, 2, hole);      /* lb 2 written; lb 0,1 are holes */
        memset(out, 0xCD, BS);
        CHECK(ts_read(&t, idh, 0, out, BS, &rlen) == FACADE_OK);
        CHECK(rlen == BS);
        for (i = 0; i < (int) BS; i++) {
            CHECK(out[i] == 0);
        }

        ts_close_media(&t);
    }

    /* ---- 7 (P2): RMW launder guard -- a sub-block overwrite of a corrupted
     *      block fails DATA_UNRECOVERABLE (facade_write_after_read verifies the
     *      pre-read before the copy-on-write merge). ---- */
    {
        uint8_t  id[16];
        uint8_t  b[BS], patch[64];
        uint64_t phys;

        mkid(id, 0x71);
        fillpat(b, BS, 0x77);
        fillpat(patch, sizeof(patch), 0x7A);

        ts_open_media(&t, dir, 6);
        ts_format(&t);
        ts_open(&t);

        ts_write_block(&t, id, 0, b);          /* full block at lb 0, durable */
        ts_crash_recover(&t);
        ts_reopen(&t);

        phys = ts_phys(&t, id, 0);
        CHECK(t.control->corrupt(t.space, phys, 1) == 0);

        /* 64 bytes at offset 0: boff==0, len<BS, DATA exists -> RMW pre-read. */
        CHECK(ts_write_sub(&t, id, 0, patch, sizeof(patch))
              == FACADE_E_DATA_UNRECOVERABLE);

        ts_close_media(&t);
    }

    /* ---- 8 (P3): object content MD5 at SEAL -- MD5(content), survives reopen,
     *      and an OPEN object reports NONE. ---- */
    {
        uint8_t                    id[16];
        uint8_t                    b0[BS], b1[BS], b2[BS], content[3 * BS], expect[16];
        struct chimera_sc_obj_meta m;

        mkid(id, 0x81);
        fillpat(b0, BS, 0x80);
        fillpat(b1, BS, 0x81);
        fillpat(b2, BS, 0x82);
        memcpy(content, b0, BS);
        memcpy(content + BS, b1, BS);
        memcpy(content + 2 * BS, b2, BS);
        CHECK(chimera_crypto_digest(CHIMERA_CRYPTO_MD5, content, sizeof(content),
                                    expect, sizeof(expect)) == 1);

        ts_open_media(&t, dir, 7);
        ts_format(&t);
        ts_open(&t);

        ts_write_block(&t, id, 0, b0);
        ts_write_block(&t, id, 1, b1);
        ts_write_block(&t, id, 2, b2);

        /* OPEN object reports NONE. */
        CHECK(ts_stat(&t, id, &m) == 0 && m.state == CHIMERA_SC_OBJ_OPEN);
        CHECK(m.checksum.algorithm == CHIMERA_SC_CHECKSUM_NONE && m.checksum.len == 0);

        /* seal -> MD5(content). */
        CHECK(ts_seal(&t, id) == FACADE_OK);
        CHECK(ts_stat(&t, id, &m) == 0 && m.state == CHIMERA_SC_OBJ_SEALED);
        CHECK(m.checksum.algorithm == CHIMERA_SC_CHECKSUM_MD5 && m.checksum.len == 16);
        CHECK(memcmp(m.checksum.bytes, expect, 16) == 0);

        /* digest survives close + reopen (persisted in the durable OBJ record). */
        ts_reopen(&t);
        CHECK(ts_stat(&t, id, &m) == 0 && m.state == CHIMERA_SC_OBJ_SEALED);
        CHECK(m.checksum.algorithm == CHIMERA_SC_CHECKSUM_MD5 && m.checksum.len == 16);
        CHECK(memcmp(m.checksum.bytes, expect, 16) == 0);

        ts_close_media(&t);
    }

    /* ---- 9 (P3): the SEAL freeze (N1) -- every object-op is refused while an
     *      object is sealing, including one parked in its RMW read; the map and
     *      o->change are unchanged, and the seal still completes. ---- */
    {
        uint8_t                    x[16], y[16];
        uint8_t                    b[BS], patch[64];
        struct epoch_w             sw, pw;
        uint64_t                   ch0;
        struct chimera_sc_obj_meta m;

        mkid(x, 0x91);
        mkid(y, 0x92);
        fillpat(b, BS, 0x90);
        fillpat(patch, sizeof(patch), 0x9A);

        ts_open_media(&t, dir, 8);
        ts_format(&t);
        ts_open(&t);

        ts_write_block(&t, x, 0, b);
        ts_write_block(&t, y, 0, b);

        /* X: entry rejects.  Start a seal (o->sealing set, hash in flight), then
         * every mutator is refused E_STATE at entry without touching the object. */
        ts_seal_start(&t, x, &sw);
        CHECK(!sw.done);                       /* seal pending (async hash) */
        ch0 = ts_change(&t, x);

        CHECK(ts_write_sub(&t, x, 0, patch, sizeof(patch)) == FACADE_E_STATE);
        CHECK(ts_truncate(&t, x, 0) == FACADE_E_STATE);
        CHECK(ts_allocate(&t, x, 4 * BS, BS) == FACADE_E_STATE);
        CHECK(ts_punch(&t, x, 0, BS) == FACADE_E_STATE);
        CHECK(ts_discard(&t, x) == FACADE_E_STATE);

        /* none of the rejected ops changed the object. */
        CHECK(ts_change(&t, x) == ch0);
        check_block_csum(&t, x, 0, facade_crc32(b, BS));   /* map intact */

        /* let the seal finish. */
        pump(t.evpl, &sw.done);
        CHECK(sw.st == FACADE_OK);
        CHECK(ts_stat(&t, x, &m) == 0 && m.state == CHIMERA_SC_OBJ_SEALED);
        CHECK(m.checksum.algorithm == CHIMERA_SC_CHECKSUM_MD5);

        /* Y: a sub-block write parks in its RMW read BEFORE the seal, then submits
         * after the seal began -- it is frozen out at the prepare chokepoint. */
        pw.done = 0;
        pw.st   = FACADE_E_IO;
        facade_obj_write(t.alloc, y, 0, patch, sizeof(patch),
                         CHIMERA_SC_WRITE_DURABLE, on_epoch, &pw);
        CHECK(!pw.done);                       /* parked in the RMW pre-read */
        ts_seal_start(&t, y, &sw);
        CHECK(!sw.done);
        pump(t.evpl, &pw.done);
        CHECK(pw.st == FACADE_E_STATE);        /* parked RMW write frozen out */
        pump(t.evpl, &sw.done);
        CHECK(sw.st == FACADE_OK);             /* the seal still completes */
        CHECK(ts_stat(&t, y, &m) == 0 && m.state == CHIMERA_SC_OBJ_SEALED);

        ts_close_media(&t);
    }

    /* ---- 10 (P3): the SEAL freeze across the NS-txn path (N5) -- an NS delta
     *      targeting a sealing object is refused E_STATE with r_failed_part DELTA,
     *      nothing committed, and the in-flight seal completes with the right MD5. ---- */
    {
        uint8_t                       x[16], y[16];
        uint8_t                       b[BS], expect[16];
        struct epoch_w                sw;
        struct ns_w                   nw;
        struct chimera_sc_delta       d;
        struct chimera_sc_ns_txn_args args;
        struct chimera_sc_obj_meta    m;

        mkid(x, 0xA1);
        mkid(y, 0xA2);
        fillpat(b, BS, 0xA0);
        CHECK(chimera_crypto_digest(CHIMERA_CRYPTO_MD5, b, BS, expect, sizeof(expect)) == 1);

        ts_open_media(&t, dir, 9);
        ts_format(&t);
        ts_open(&t);

        ts_write_block(&t, x, 0, b);
        ts_write_block(&t, y, 0, b);

        /* a DELETE delta against a sealing X. */
        ts_seal_start(&t, x, &sw);
        CHECK(!sw.done);
        memset(&d, 0, sizeof(d));
        d.kind = CHIMERA_SC_DELTA_DELETE;
        memcpy(d.del.obj.b, x, 16);
        memset(&args, 0, sizeof(args));
        args.deltas  = &d;
        args.ndeltas = 1;
        nw.done      = 0;
        facade_ns_txn(t.alloc, &args, on_ns, &nw);
        pump(t.evpl, &nw.done);
        CHECK(nw.res.status == CHIMERA_SC_E_STATE &&
              nw.res.r_failed_part == CHIMERA_SC_TXN_DELTA);
        pump(t.evpl, &sw.done);
        CHECK(sw.st == FACADE_OK);
        CHECK(ts_stat(&t, x, &m) == 0 && m.state == CHIMERA_SC_OBJ_SEALED);
        CHECK(memcmp(m.checksum.bytes, expect, 16) == 0);   /* right MD5 despite the NS txn */

        /* a SEAL delta against a sealing Y. */
        ts_seal_start(&t, y, &sw);
        CHECK(!sw.done);
        memset(&d, 0, sizeof(d));
        d.kind = CHIMERA_SC_DELTA_SEAL;
        memcpy(d.seal.obj.b, y, 16);
        memset(&args, 0, sizeof(args));
        args.deltas  = &d;
        args.ndeltas = 1;
        nw.done      = 0;
        facade_ns_txn(t.alloc, &args, on_ns, &nw);
        pump(t.evpl, &nw.done);
        CHECK(nw.res.status == CHIMERA_SC_E_STATE &&
              nw.res.r_failed_part == CHIMERA_SC_TXN_DELTA);
        pump(t.evpl, &sw.done);
        CHECK(sw.st == FACADE_OK);
        CHECK(ts_stat(&t, y, &m) == 0 && m.state == CHIMERA_SC_OBJ_SEALED);
        CHECK(memcmp(m.checksum.bytes, expect, 16) == 0);

        ts_close_media(&t);
    }

    /* ---- 11 (P3): sealing an object with a corrupted block fails
     *      DATA_UNRECOVERABLE and leaves it OPEN and WRITABLE. ---- */
    {
        uint8_t                    id[16];
        uint8_t                    b[BS], b2[BS];
        uint64_t                   phys;
        struct chimera_sc_obj_meta m;

        mkid(id, 0xB1);
        fillpat(b, BS, 0xB0);
        fillpat(b2, BS, 0xBB);

        ts_open_media(&t, dir, 10);
        ts_format(&t);
        ts_open(&t);

        ts_write_block(&t, id, 0, b);
        ts_crash_recover(&t);                  /* drain: delegate authoritative */
        ts_reopen(&t);

        phys = ts_phys(&t, id, 0);
        CHECK(t.control->corrupt(t.space, phys, 1) == 0);

        CHECK(ts_seal(&t, id) == FACADE_E_DATA_UNRECOVERABLE);
        CHECK(ts_stat(&t, id, &m) == 0 && m.state == CHIMERA_SC_OBJ_OPEN);
        CHECK(m.checksum.algorithm == CHIMERA_SC_CHECKSUM_NONE);

        /* the sealing flag was cleared: the object is writable again. */
        ts_write_block(&t, id, 0, b2);         /* CHECKs OK internally */
        CHECK(ts_stat(&t, id, &m) == 0 && m.state == CHIMERA_SC_OBJ_OPEN);

        ts_close_media(&t);
    }

    /* ---- 12 (P3): durability -- a COMMITTED seal's digest survives crash +
     *      recover (durable only via the seal txn COMMIT); an object whose seal
     *      never committed carries no durable digest. ---- */
    {
        uint8_t                    x[16], y[16];
        uint8_t                    b[BS], expect[16];
        struct chimera_sc_obj_meta m;

        mkid(x, 0xC1);
        mkid(y, 0xC2);
        fillpat(b, BS, 0xC0);
        CHECK(chimera_crypto_digest(CHIMERA_CRYPTO_MD5, b, BS, expect, sizeof(expect)) == 1);

        ts_open_media(&t, dir, 11);
        ts_format(&t);
        ts_open(&t);

        ts_write_block(&t, x, 0, b);
        CHECK(ts_seal(&t, x) == FACADE_OK);    /* X committed SEALED with the digest */
        ts_write_block(&t, y, 0, b);           /* Y written but never sealed */

        ts_crash_recover(&t);
        ts_reopen(&t);

        CHECK(ts_stat(&t, x, &m) == 0 && m.state == CHIMERA_SC_OBJ_SEALED);
        CHECK(m.checksum.algorithm == CHIMERA_SC_CHECKSUM_MD5 &&
              memcmp(m.checksum.bytes, expect, 16) == 0);   /* digest durable */

        CHECK(ts_stat(&t, y, &m) == 0 && m.state == CHIMERA_SC_OBJ_OPEN);
        CHECK(m.checksum.algorithm == CHIMERA_SC_CHECKSUM_NONE);   /* no commit -> no digest */

        ts_close_media(&t);
    }

    /* ---- 13 (P3, H1): the epoch-held release sweep must NOT free a sealing
     *      object's blocks mid-hash.  A DETACH/re-ATTACH sweep queued after the
     *      seal barrier released the slot and before the seal commits must SKIP
     *      the sealing object; the seal then completes on valid blocks. ---- */
    {
        uint8_t                    id[16];
        uint8_t                    b[BS], expect[16];
        struct epoch_w             sw;
        struct cb_w                rw;
        struct chimera_sc_obj_meta m;
        uint64_t                   phys0, free0;

        mkid(id, 0xD1);
        fillpat(b, BS, 0xD0);
        CHECK(chimera_crypto_digest(CHIMERA_CRYPTO_MD5, b, BS, expect, sizeof(expect)) == 1);

        ts_open_media(&t, dir, 12);
        ts_format(&t);
        ts_open(&t);

        ts_write_block(&t, id, 0, b);

        /* Start the seal: the barrier snapshots + releases the slot, the hash is
         * in flight, o->sealing is set. */
        ts_seal_start(&t, id, &sw);
        CHECK(!sw.done);
        phys0 = ts_phys(&t, id, 0);
        free0 = t.alloc->free_blocks;

        /* Queue the epoch-held release sweep (DETACH/re-ATTACH) now -- after the
         * barrier, before the seal commits.  It must skip the sealing object. */
        rw.done = 0;
        rw.st   = FACADE_E_IO;
        facade_obj_release_epoch_held(t.alloc, on_cb, &rw);
        pump(t.evpl, &rw.done);
        CHECK(rw.st == FACADE_OK);

        /* the sealing object survives the sweep with its blocks intact. */
        CHECK(facade_obj_find(t.alloc, id) != NULL);
        CHECK(ts_phys(&t, id, 0) == phys0);
        CHECK(t.alloc->free_blocks == free0);   /* nothing was freed */

        /* the seal then completes correctly on the valid blocks. */
        pump(t.evpl, &sw.done);
        CHECK(sw.st == FACADE_OK);
        CHECK(ts_stat(&t, id, &m) == 0 && m.state == CHIMERA_SC_OBJ_SEALED);
        CHECK(m.checksum.algorithm == CHIMERA_SC_CHECKSUM_MD5 &&
              memcmp(m.checksum.bytes, expect, 16) == 0);

        ts_close_media(&t);
    }

    evpl_destroy(t.evpl);
    CHECK(rmdir(dir) == 0);
    printf("facade_data_verify_test: OK\n");
    return 0;
} /* main */
