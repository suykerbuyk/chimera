// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Exit criterion: facade-layer crash recovery over the crash-sim + loopback
 * providers, with seeded crashes.  A white-box test (it reaches into
 * facade_internal.h for the log geometry and object table) covering:
 *
 *   A. a DURABLE object survives a crash + recover + reopen (D5/HA-3);
 *   B. STOP-AT-FIRST-GAP (H4): a corrupted log record ends replay -- objects
 *      whose records sit AFTER it are NOT recovered even though they are
 *      physically intact (the diskfs scan-and-skip regression guard);
 *   C. OBJECT DURABILITY ACROSS COMPACTION (round-2 CRITICAL): an object made
 *      durable, then a compaction forced via the triggering-append path, then a
 *      crash -- the object still reads back (it was folded into the checkpoint);
 *   D. the power-loss generation CHANGES after an unclean recover and is EQUAL
 *      across a clean reopen.
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

#include "facade_internal.h"   /* white-box: struct facade_alloc, facade_media_read */
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

/* ---- media open / completion waiters ---- */

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
on_mclose(
    enum chimera_media_error st,
    void                    *priv)
{
    (void) st;
    *(int *) priv = 1;
} /* on_mclose */

/* ---- harness ---- */

static void
ts_open_media(
    struct ts  *t,
    const char *dir,
    int         idx,
    uint32_t    w_keep,
    uint32_t    w_drop,
    uint32_t    w_tear)
{
    struct chimera_media_crashsim_config cfg;
    struct open_w                        ow = { 0, CHIMERA_MEDIA_E_IO, NULL };

    memset(&cfg, 0, sizeof(cfg));
    cfg.delegate = chimera_media_loopback();
    cfg.seed     = 0x1234 + idx;
    cfg.w_keep   = w_keep;
    cfg.w_drop   = w_drop;
    cfg.w_tear   = w_tear;

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

static uint64_t
ts_write(
    struct ts    *t,
    const uint8_t id[16],
    const void   *data,
    uint32_t      len)
{
    struct epoch_w w = { 0, FACADE_E_IO, 0 };

    if (!facade_obj_find(t->alloc, id)) {
        facade_obj_create_entry(t->alloc, id, NULL);
    }
    facade_obj_write(t->alloc, id, 0, data, len, CHIMERA_SC_WRITE_DURABLE,
                     on_epoch, &w);
    pump(t->evpl, &w.done);
    CHECK(w.st == FACADE_OK);
    return w.epoch;
} /* ts_write */

/* Read an object's first `len` bytes into out; returns 1 if the object exists. */
static int
ts_read_obj(
    struct ts    *t,
    const uint8_t id[16],
    uint8_t      *out,
    uint32_t      len)
{
    struct facade_obj   *o = facade_obj_find(t->alloc, id);
    struct facade_extent ext;
    struct evpl_iovec    iov;
    int                  done = 0;
    int                  n;

    if (!o || !facade_extent_map_lookup(o->map, 0, &ext) ||
        ext.kind == CHIMERA_SC_SEG_HOLE) {
        return 0;
    }
    n = evpl_iovec_alloc(t->evpl, BS, BS, 1, 0, &iov);
    CHECK(n == 1);
    t->provider->read(t->space, &iov, 1, ext.phys_block, 1, on_mclose, &done);
    pump(t->evpl, &done);
    memcpy(out, evpl_iovec_data(&iov), len);
    evpl_iovec_release(t->evpl, &iov);
    return 1;
} /* ts_read_obj */

/* A park-aware id draw: on the async crash-sim fixture the first draw after an
 * open parks (id_next == id_floor until the floor-extension FUA lands), so the
 * resume re-issues it when the extension completes under the evpl loop. */
struct ts_draw {
    struct facade_alloc      *a;
    struct chimera_sc_obj_id *id;
    int                       done;
    int                       ok;
};

static void
ts_draw_resume(void *arg)
{
    struct ts_draw    *d  = arg;
    enum facade_status st = facade_objid_next(d->a, d->id, ts_draw_resume, d);

    if (st == FACADE_OK) {
        d->done = 1; d->ok = 1;
    } else if (st != FACADE_AGAIN) {
        d->done = 1; d->ok = 0;
    }
} /* ts_draw_resume */

static void
ts_seal(
    struct ts    *t,
    const uint8_t id[16])
{
    struct epoch_w w = { 0, FACADE_E_IO, 0 };

    facade_obj_seal(t->alloc, id, on_epoch, &w);
    pump(t->evpl, &w.done);
    CHECK(w.st == FACADE_OK);
} /* ts_seal */

/* Clone a working version of sealed_id into *wid (drawn here), durably. */
static void
ts_clone(
    struct ts                *t,
    const uint8_t             sealed_id[16],
    struct chimera_sc_obj_id *wid)
{
    struct epoch_w     w = { 0, FACADE_E_IO, 0 };
    struct ts_draw     d = { t->alloc, wid, 0, 0 };
    enum facade_status st;

    st = facade_objid_next(t->alloc, wid, ts_draw_resume, &d);
    if (st == FACADE_OK) {
        d.done = 1; d.ok = 1;
    } else {
        CHECK(st == FACADE_AGAIN);
        pump(t->evpl, &d.done);   /* the floor extension lands, the resume re-draws */
    }
    CHECK(d.ok);

    facade_obj_clone_working(t->alloc, sealed_id, wid, on_epoch, &w);
    pump(t->evpl, &w.done);
    CHECK(w.st == FACADE_OK);
} /* ts_clone */

/* Write `len` bytes at a byte offset (for the working-version COW write). */
static void
ts_write_at(
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
    CHECK(w.st == FACADE_OK);
} /* ts_write_at */

/* Deterministically destroy a log record by overwriting its block with a zeroed
 * (magic == 0) block, FUA so it is durable.  (crash-sim corrupt() flips random
 * bits, which for a ~72-byte record in a 4 KiB block almost always land in the
 * zero padding and leave the header/payload CRC valid -- correctly -- so it
 * cannot deterministically force the gap this test needs.) */
static void
ts_garble_block(
    struct ts *t,
    uint64_t   lba)
{
    struct evpl_iovec iov;
    uint8_t          *buf  = calloc(1, BS);
    int               done = 0;
    int               n;

    CHECK(buf != NULL);
    n = evpl_iovec_alloc(t->evpl, BS, BS, 1, 0, &iov);
    CHECK(n == 1);
    memset(evpl_iovec_data(&iov), 0, BS);
    t->provider->write(t->space, &iov, 1, lba, 1, CHIMERA_MEDIA_WRITE_FUA,
                       on_mclose, &done);
    pump(t->evpl, &done);
    evpl_iovec_release(t->evpl, &iov);
    free(buf);
} /* ts_garble_block */

static void
ts_crash_recover(struct ts *t)
{
    CHECK(t->control->crash(t->space) == 0);
    CHECK(t->control->recover(t->space) == 0);
} /* ts_crash_recover */

static void
ts_reopen(struct ts *t)
{
    facade_alloc_abandon(t->alloc);
    t->alloc = NULL;
    ts_open(t);
} /* ts_reopen */

static void
ts_close_media(struct ts *t)
{
    int ok = 0;

    if (t->alloc) {
        facade_alloc_abandon(t->alloc);   /* release the in-memory handle (no media) */
        t->alloc = NULL;
    }
    t->provider->close(t->space, on_mclose, &ok);
    pump(t->evpl, &ok);
    chimera_media_crashsim_destroy(t->handle);
    CHECK(unlink(t->path) == 0);
} /* ts_close_media */

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
        buf[i] = (uint8_t) (i * 31u + seed);
    }
} /* fillpat */

int
main(void)
{
    const char *tmp = getenv("TMPDIR");
    char        dir[512];
    struct ts   t;
    uint8_t     pat[64], rb[64];

    snprintf(dir, sizeof(dir), "%s/facade_recover_XXXXXX",
             (tmp && tmp[0]) ? tmp : "/tmp");
    CHECK(mkdtemp(dir) != NULL);

    memset(&t, 0, sizeof(t));
    t.evpl = evpl_create(NULL);
    CHECK(t.evpl != NULL);

    /* ---- A: a DURABLE object survives a crash + recover (DROP-biased) ---- */
    {
        uint8_t id[16];

        mkid(id, 0xA1);
        fillpat(pat, sizeof(pat), 0x11);

        ts_open_media(&t, dir, 1, 0, 1, 0);   /* drop-biased */
        ts_format(&t);
        ts_open(&t);
        {
            uint64_t g0 = t.alloc->power_loss_generation;
            ts_write(&t, id, pat, sizeof(pat));
            ts_crash_recover(&t);
            ts_reopen(&t);
            /* the unclean recover bumped the generation (D) */
            CHECK(t.alloc->power_loss_generation > g0);
        }
        CHECK(ts_read_obj(&t, id, rb, sizeof(pat)) == 1);
        CHECK(memcmp(rb, pat, sizeof(pat)) == 0);
        ts_close_media(&t);
    }

    /* ---- B: STOP-AT-FIRST-GAP (H4).  Corrupt the 2nd object's ALLOC record;
     * recovery must stop there -- obj1 survives, obj2 and obj3 do not, even
     * though obj3's records are intact (a scan-and-skip impl would keep obj3). */
    {
        uint8_t  id1[16], id2[16], id3[16];
        uint64_t log_block;

        mkid(id1, 0xB1);
        mkid(id2, 0xB2);
        mkid(id3, 0xB3);

        ts_open_media(&t, dir, 2, 1, 0, 0);   /* keep-biased: records would survive */
        ts_format(&t);
        ts_open(&t);
        log_block = t.alloc->log_block;

        fillpat(pat, sizeof(pat), 0x21);
        ts_write(&t, id1, pat, sizeof(pat));   /* records at log_block + 0,1,2 */
        ts_write(&t, id2, pat, sizeof(pat));   /* records at log_block + 3,4,5 */
        ts_write(&t, id3, pat, sizeof(pat));   /* records at log_block + 6,7,8 */

        /* Destroy obj2's ALLOC record block (log_block + 3) -- a torn/missing
         * record in the middle of an otherwise-intact log. */
        ts_garble_block(&t, log_block + 3);

        ts_crash_recover(&t);
        ts_reopen(&t);

        CHECK(ts_read_obj(&t, id1, rb, sizeof(pat)) == 1);   /* before the gap */
        CHECK(memcmp(rb, pat, sizeof(pat)) == 0);
        CHECK(facade_obj_find(t.alloc, id2) == NULL);        /* the gap */
        CHECK(facade_obj_find(t.alloc, id3) == NULL);        /* after the gap: NOT skipped-to */
        ts_close_media(&t);
    }

    /* ---- C: OBJECT DURABILITY ACROSS COMPACTION (round-2 CRITICAL / G1).
     * Write one object, then keep writing (new objects) until a write's own
     * transaction triggers a forced compaction via the triggering-append path;
     * then crash + recover.  The first object -- folded into the checkpoint by
     * the compaction, its log record trimmed -- must still read back. */
    {
        uint8_t  id0[16], idn[16];
        uint64_t gen_before;
        int      i;

        mkid(id0, 0xC0);
        fillpat(pat, sizeof(pat), 0x31);

        ts_open_media(&t, dir, 3, 1, 0, 0);
        ts_format(&t);
        ts_open(&t);

        ts_write(&t, id0, pat, sizeof(pat));
        gen_before = t.alloc->ckpt_generation;

        /* Each write is a 3-record txn; log_nblocks is small, so a handful of
        * writes forces at least one compaction (ckpt_generation advances). */
        for (i = 0; i < (int) t.alloc->log_nblocks + 4; i++) {
            uint8_t fill[64];
            mkid(idn, (uint8_t) (0x40 + i));
            fillpat(fill, sizeof(fill), (uint8_t) (0x50 + i));
            ts_write(&t, idn, fill, sizeof(fill));
        }
        CHECK(t.alloc->ckpt_generation > gen_before);   /* a compaction happened */

        ts_crash_recover(&t);
        ts_reopen(&t);

        /* id0's log record was trimmed by the compaction; it survives only
         * because the object table is folded into the checkpoint. */
        CHECK(ts_read_obj(&t, id0, rb, sizeof(pat)) == 1);
        CHECK(memcmp(rb, pat, sizeof(pat)) == 0);
        ts_close_media(&t);
    }

    /* ---- D: a clean close + reopen keeps the generation EQUAL. ---- */
    {
        uint8_t     id[16];
        uint64_t    g1;
        struct st_w cw = { 0, FACADE_E_IO };

        mkid(id, 0xD1);
        fillpat(pat, sizeof(pat), 0x61);

        ts_open_media(&t, dir, 4, 1, 0, 0);
        ts_format(&t);
        ts_open(&t);
        ts_write(&t, id, pat, sizeof(pat));
        g1 = t.alloc->power_loss_generation;

        /* Clean close (persists a checkpoint) then reopen: no recovery, so the
         * generation is unchanged. */
        facade_alloc_close(t.alloc, on_fmt /*reuse st_w cb shape*/, &cw);
        pump(t.evpl, &cw.done);
        CHECK(cw.st == FACADE_OK);
        ts_open(&t);
        CHECK(t.alloc->power_loss_generation == g1);
        CHECK(ts_read_obj(&t, id, rb, sizeof(pat)) == 1);
        CHECK(memcmp(rb, pat, sizeof(pat)) == 0);
        ts_close_media(&t);
    }

    /* Case 5: a SEALED object and a HELD working-version clone survive a crash
     * and recovery (the plan's seal + held-clone crash-survival coverage; the
     * other cases exercise only plain writes).  Keep-biased so nothing is lost
     * for the wrong reason -- the durable records+checkpoint carry the state. */
    {
        uint8_t                  sid[16], patS[64], patW[64], rb[64];
        struct chimera_sc_obj_id wid;
        struct facade_obj       *w;

        mkid(sid, 0xE1);
        fillpat(patS, sizeof(patS), 0x71);
        fillpat(patW, sizeof(patW), 0x93);

        ts_open_media(&t, dir, 5, 1, 0, 0);   /* keep-biased */
        ts_format(&t);
        ts_open(&t);

        ts_write(&t, sid, patS, sizeof(patS));   /* origin data, durable */
        ts_seal(&t, sid);                        /* OPEN -> SEALED, durable */

        ts_clone(&t, sid, &wid);                 /* metadata-only working version */
        /* Make the working version HELD (a durable record state), then give it
         * its own copy-on-write durable write at offset 0. */
        w = facade_obj_find(t.alloc, wid.b);
        CHECK(w != NULL);
        w->flags |= FACADE_OBJ_HELD;
        ts_write_at(&t, wid.b, 0, patW, sizeof(patW));

        ts_crash_recover(&t);
        ts_reopen(&t);

        /* The sealed origin survived: state SEALED, bytes intact. */
        {
            struct facade_obj *s = facade_obj_find(t.alloc, sid);

            CHECK(s != NULL && s->state == CHIMERA_SC_OBJ_SEALED);
        }
        CHECK(ts_read_obj(&t, sid, rb, sizeof(patS)) == 1);
        CHECK(memcmp(rb, patS, sizeof(patS)) == 0);

        /* The held working version survived: HELD flag kept, its COW bytes
         * intact, and the origin is unchanged (COW did not touch it). */
        w = facade_obj_find(t.alloc, wid.b);
        CHECK(w != NULL && (w->flags & FACADE_OBJ_HELD));
        CHECK(ts_read_obj(&t, wid.b, rb, sizeof(patW)) == 1);
        CHECK(memcmp(rb, patW, sizeof(patW)) == 0);

        ts_close_media(&t);
    }

    evpl_destroy(t.evpl);
    CHECK(rmdir(dir) == 0);

    printf("facade_recover_crash_test: ok\n");
    return 0;
} /* main */
