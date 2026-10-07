// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Functional proof of the crash-sim media provider over the real loopback
 * delegate, driven through a real evpl loop.  Unprivileged (the delegate is the
 * pread loopback on a sparse temp file).  Proves:
 *   - a force-unit-access write and a plain write + flush survive a crash;
 *   - a plain write never flushed is lost on a drop crash;
 *   - a staged write is visible to a read before a crash, gone after;
 *   - a multi-block write is torn (partly lost) under a tear policy;
 *   - a seeded crash replays deterministically (same seed + trace => same
 *     surviving set and image), with survival gaps (the H4/H5 hazard);
 *   - the corruption hook changes stored bytes.
 * The loop is driven with the while(!done) evpl_continue() idiom.
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

struct open_wait {
    int                         done;
    enum chimera_media_error status;
    struct chimera_media_space *space;
};

struct io_wait {
    int done;
    enum chimera_media_error status;
};

static void
on_open(
    enum chimera_media_error    status,
    struct chimera_media_space *space,
    void                       *private_data)
{
    struct open_wait *w = private_data;

    w->status = status;
    w->space  = space;
    w->done   = 1;
} /* on_open */

static void
on_io(
    enum chimera_media_error status,
    void                    *private_data)
{
    struct io_wait *w = private_data;

    w->status = status;
    w->done   = 1;
} /* on_io */

static void
pump(
    struct evpl *evpl,
    const int   *done)
{
    while (!*done) {
        evpl_continue(evpl);
    }
} /* pump */

static uint8_t
patt(
    uint64_t gidx,
    unsigned seed)
{
    return (uint8_t) (gidx * 31u + seed);
} /* patt */

/* ---- a case: a fresh crash-sim over a fresh backing file ---- */

struct tcase {
    struct chimera_media_crashsim               *h;
    const struct chimera_media_provider         *p;
    const struct chimera_media_crashsim_control *c;
    struct chimera_media_space                  *space;
    char                                         path[700];
};

static void
case_open(
    struct tcase                        *tc,
    struct evpl                         *evpl,
    const char                          *dir,
    int                                  idx,
    struct chimera_media_crashsim_config cfg)
{
    struct open_wait ow = { 0, CHIMERA_MEDIA_E_IO, NULL };

    snprintf(tc->path, sizeof(tc->path), "%s/dev-%d.img", dir, idx);
    cfg.delegate = chimera_media_loopback();
    CHECK(chimera_media_crashsim_create(&cfg, &tc->h, &tc->p, &tc->c) == 0);

    chimera_media_crashsim_open(tc->h, evpl, tc->path, ONE_GIB, NULL,
                                on_open, &ow);
    pump(evpl, &ow.done);
    CHECK(ow.status == CHIMERA_MEDIA_OK);
    CHECK(ow.space != NULL);
    tc->space = ow.space;
} /* case_open */

static void
case_close(
    struct tcase *tc,
    struct evpl  *evpl)
{
    struct io_wait w = { 0, CHIMERA_MEDIA_E_IO };

    tc->p->close(tc->space, on_io, &w);
    pump(evpl, &w.done);
    CHECK(w.status == CHIMERA_MEDIA_OK);
    chimera_media_crashsim_destroy(tc->h);
    CHECK(unlink(tc->path) == 0);
} /* case_close */

/* Write one block of pattern `seed` at `lba` (plain or fua) and wait. */
static void
wr_block(
    struct tcase *tc,
    struct evpl  *evpl,
    uint64_t      lba,
    unsigned      seed,
    int           fua)
{
    struct evpl_iovec iov;
    struct io_wait    w = { 0, CHIMERA_MEDIA_E_IO };
    uint8_t          *d;
    unsigned          i;
    int               n;

    n = evpl_iovec_alloc(evpl, BS, BS, 1, 0, &iov);
    CHECK(n == 1);
    d = evpl_iovec_data(&iov);
    for (i = 0; i < BS; i++) {
        d[i] = patt(i, seed);
    }
    tc->p->write(tc->space, &iov, 1, lba, 1, fua, on_io, &w);
    pump(evpl, &w.done);
    CHECK(w.status == CHIMERA_MEDIA_OK);
    evpl_iovec_release(evpl, &iov);
} /* wr_block */

/* Read one block at `lba` into out[BS]. */
static void
rd_block(
    struct tcase *tc,
    struct evpl  *evpl,
    uint64_t      lba,
    uint8_t      *out)
{
    struct evpl_iovec iov;
    struct io_wait    w = { 0, CHIMERA_MEDIA_E_IO };
    int               n;

    n = evpl_iovec_alloc(evpl, BS, BS, 1, 0, &iov);
    CHECK(n == 1);
    tc->p->read(tc->space, &iov, 1, lba, 1, on_io, &w);
    pump(evpl, &w.done);
    CHECK(w.status == CHIMERA_MEDIA_OK);
    memcpy(out, evpl_iovec_data(&iov), BS);
    evpl_iovec_release(evpl, &iov);
} /* rd_block */

static int
block_is(
    const uint8_t *buf,
    unsigned       seed)
{
    unsigned i;

    for (i = 0; i < BS; i++) {
        if (buf[i] != patt(i, seed)) {
            return 0;
        }
    }
    return 1;
} /* block_is */

/* Write a contiguous nblk-block region of pattern `seed` (one record). */
static void
wr_region(
    struct tcase *tc,
    struct evpl  *evpl,
    uint64_t      lba,
    uint32_t      nblk,
    unsigned      seed,
    int           fua)
{
    struct evpl_iovec iov[16];
    struct io_wait    w = { 0, CHIMERA_MEDIA_E_IO };
    uint64_t          g = 0;
    int               n;
    int               k;

    CHECK(nblk <= 16);
    n = evpl_iovec_alloc(evpl, nblk * BS, BS, 16, 0, iov);
    CHECK(n >= 1);
    for (k = 0; k < n; k++) {
        uint8_t *d   = evpl_iovec_data(&iov[k]);
        unsigned len = evpl_iovec_length(&iov[k]);
        unsigned j;

        for (j = 0; j < len; j++) {
            d[j] = patt(g++, seed);
        }
    }
    tc->p->write(tc->space, iov, n, lba, nblk, fua, on_io, &w);
    pump(evpl, &w.done);
    CHECK(w.status == CHIMERA_MEDIA_OK);
    for (k = 0; k < n; k++) {
        evpl_iovec_release(evpl, &iov[k]);
    }
} /* wr_region */

static void
do_crash(struct tcase *tc)
{
    CHECK(tc->c->crash(tc->space) == 0);
    CHECK(tc->c->recover(tc->space) == 0);
} /* do_crash */

int
main(void)
{
    const char  *tmp = getenv("TMPDIR");
    char         dir[512];
    struct evpl *evpl;
    uint8_t      b0[BS], b1[BS];

    snprintf(dir, sizeof(dir), "%s/media_crashsim_XXXXXX",
             (tmp && tmp[0]) ? tmp : "/tmp");
    CHECK(mkdtemp(dir) != NULL);

    evpl = evpl_create(NULL);
    CHECK(evpl != NULL);

    /* Case 1: a FUA write and a plain-no-flush write, then a DROP-only crash.
     * The FUA write survives (D5: a durable write is never lost); the buffered
     * write is gone. */
    {
        struct tcase                         tc;
        struct chimera_media_crashsim_config cfg;

        memset(&cfg, 0, sizeof(cfg));
        cfg.w_keep = 0; cfg.w_drop = 1; cfg.w_tear = 0;   /* drop-only */
        case_open(&tc, evpl, dir, 1, cfg);

        wr_block(&tc, evpl, 5, 0xA1u, 1 /* fua */);
        wr_block(&tc, evpl, 6, 0xB2u, 0 /* plain, never flushed */);
        do_crash(&tc);

        rd_block(&tc, evpl, 5, b0);
        rd_block(&tc, evpl, 6, b1);
        CHECK(block_is(b0, 0xA1u));           /* FUA survived */
        CHECK(!block_is(b1, 0xB2u));          /* buffered lost */

        case_close(&tc, evpl);
    }

    /* Case 2: a plain write + flush survives a DROP-only crash. */
    {
        struct tcase                         tc;
        struct chimera_media_crashsim_config cfg;
        struct io_wait                       w = { 0, CHIMERA_MEDIA_E_IO };

        memset(&cfg, 0, sizeof(cfg));
        cfg.w_keep = 0; cfg.w_drop = 1; cfg.w_tear = 0;
        case_open(&tc, evpl, dir, 2, cfg);

        wr_block(&tc, evpl, 7, 0xC3u, 0 /* plain */);
        tc.p->flush(tc.space, on_io, &w);
        pump(evpl, &w.done);
        CHECK(w.status == CHIMERA_MEDIA_OK);

        do_crash(&tc);
        rd_block(&tc, evpl, 7, b0);
        CHECK(block_is(b0, 0xC3u));           /* flushed data survived */

        case_close(&tc, evpl);
    }

    /* Case 3: read merge -- a staged write is visible before a crash, gone
     * after a DROP-only crash + recover. */
    {
        struct tcase                         tc;
        struct chimera_media_crashsim_config cfg;

        memset(&cfg, 0, sizeof(cfg));
        cfg.w_keep = 0; cfg.w_drop = 1; cfg.w_tear = 0;
        case_open(&tc, evpl, dir, 3, cfg);

        wr_block(&tc, evpl, 8, 0xD4u, 0 /* plain, staged */);
        rd_block(&tc, evpl, 8, b0);
        CHECK(block_is(b0, 0xD4u));           /* visible before crash */

        do_crash(&tc);
        rd_block(&tc, evpl, 8, b1);
        CHECK(!block_is(b1, 0xD4u));          /* gone after crash */

        case_close(&tc, evpl);
    }

    /* Case 4: a multi-block plain write is torn under a TEAR-only policy -- not
     * fully intact and not fully gone. */
    {
        struct tcase                         tc;
        struct chimera_media_crashsim_config cfg;
        struct chimera_media_crashsim_stats  st;
        uint32_t                             nblk = 6;
        uint32_t                             intact = 0, blk;

        memset(&cfg, 0, sizeof(cfg));
        cfg.w_keep = 0; cfg.w_drop = 0; cfg.w_tear = 1;   /* tear-only */
        cfg.seed   = 12345;
        case_open(&tc, evpl, dir, 4, cfg);

        wr_region(&tc, evpl, 20, nblk, 0xE5u, 0 /* plain */);
        do_crash(&tc);

        tc.c->stats(tc.space, &st);
        CHECK(st.last_crash_torn == 1);
        CHECK(st.last_crash_kept == 0 && st.last_crash_dropped == 0);

        for (blk = 0; blk < nblk; blk++) {
            uint8_t  rb[BS];
            unsigned i;
            int      allmatch = 1;

            rd_block(&tc, evpl, 20 + blk, rb);
            for (i = 0; i < BS; i++) {
                if (rb[i] != patt((uint64_t) blk * BS + i, 0xE5u)) {
                    allmatch = 0;
                    break;
                }
            }
            if (allmatch) {
                intact++;
            }
        }
        /* A tear leaves some leading blocks intact but not all of them. */
        CHECK(intact < nblk);

        case_close(&tc, evpl);
    }

    /* Case 5: determinism + survival gaps.  Eight single-block plain writes
     * under a mixed keep/drop policy and the same seed, run on two independent
     * backing stores, crash to identical surviving sets and identical images. */
    {
        struct tcase                         a, b;
        struct chimera_media_crashsim_config cfg;
        struct chimera_media_crashsim_stats  sa, sb;
        uint64_t                             lba;
        int                                  survivors = 0;

        memset(&cfg, 0, sizeof(cfg));
        cfg.w_keep = 1; cfg.w_drop = 1; cfg.w_tear = 0;   /* 50/50 */
        cfg.seed   = 0xFEEDFACEu;

        case_open(&a, evpl, dir, 50, cfg);
        case_open(&b, evpl, dir, 51, cfg);

        for (lba = 100; lba < 108; lba++) {
            wr_block(&a, evpl, lba, (unsigned) (0x10u + lba), 0 /* plain */);
            wr_block(&b, evpl, lba, (unsigned) (0x10u + lba), 0 /* plain */);
        }
        CHECK(a.c->crash(a.space) == 0);
        CHECK(b.c->crash(b.space) == 0);
        a.c->recover(a.space);
        b.c->recover(b.space);

        a.c->stats(a.space, &sa);
        b.c->stats(b.space, &sb);
        CHECK(sa.last_crash_kept == sb.last_crash_kept);
        CHECK(sa.last_crash_dropped == sb.last_crash_dropped);
        /* A 50/50 policy on 8 writes should keep some and drop some: a gap. */
        CHECK(sa.last_crash_kept > 0 && sa.last_crash_dropped > 0);

        for (lba = 100; lba < 108; lba++) {
            uint8_t ra[BS], rb[BS];

            rd_block(&a, evpl, lba, ra);
            rd_block(&b, evpl, lba, rb);
            CHECK(memcmp(ra, rb, BS) == 0);       /* deterministic image */
            if (block_is(ra, (unsigned) (0x10u + lba))) {
                survivors++;
            }
        }
        CHECK(survivors == (int) sa.last_crash_kept);

        case_close(&a, evpl);
        case_close(&b, evpl);
    }

    /* Case 6: the corruption hook changes stored bytes (the facade's
     * DATA_UNRECOVERABLE mapping is proven in E3, not here). */
    {
        struct tcase                         tc;
        struct chimera_media_crashsim_config cfg;
        struct io_wait                       w = { 0, CHIMERA_MEDIA_E_IO };

        memset(&cfg, 0, sizeof(cfg));
        cfg.seed = 777;
        case_open(&tc, evpl, dir, 6, cfg);

        wr_block(&tc, evpl, 9, 0xF6u, 0 /* plain */);
        tc.p->flush(tc.space, on_io, &w);
        pump(evpl, &w.done);
        CHECK(w.status == CHIMERA_MEDIA_OK);

        rd_block(&tc, evpl, 9, b0);
        CHECK(block_is(b0, 0xF6u));

        CHECK(tc.c->corrupt(tc.space, 9, 1) == 0);
        rd_block(&tc, evpl, 9, b1);
        CHECK(!block_is(b1, 0xF6u));              /* stored bytes changed */
        CHECK(memcmp(b0, b1, BS) != 0);

        /* An out-of-range corrupt is rejected. */
        CHECK(tc.c->corrupt(tc.space, ONE_GIB / BS, 1) == -1);

        case_close(&tc, evpl);
    }

    /* Case 7 (F1 oracle, flush): a plain write A then a FUA write B to the SAME
     * LBA must read back B after a flush -- the drain must not clobber the
     * durable B with the older staged A.  Plus a partial-overlap variant:
     * plain A=[30,31], FUA B=[30] -> LBA30=B, LBA31=A. */
    {
        struct tcase                         tc;
        struct chimera_media_crashsim_config cfg;
        struct io_wait                       w = { 0, CHIMERA_MEDIA_E_IO };

        memset(&cfg, 0, sizeof(cfg));
        case_open(&tc, evpl, dir, 7, cfg);

        /* Same LBA: older plain A, then newer FUA B. */
        wr_block(&tc, evpl, 40, 0xAAu, 0 /* plain, seq0 */);
        wr_block(&tc, evpl, 40, 0xBBu, 1 /* fua,   seq1 */);

        /* Partial overlap: older plain A=[30,31], newer FUA B=[30]. */
        wr_region(&tc, evpl, 30, 2, 0x11u, 0 /* plain */);
        wr_block(&tc, evpl, 30, 0x22u, 1 /* fua */);

        tc.p->flush(tc.space, on_io, &w);
        pump(evpl, &w.done);
        CHECK(w.status == CHIMERA_MEDIA_OK);

        rd_block(&tc, evpl, 40, b0);
        CHECK(block_is(b0, 0xBBu));               /* durable B wins */
        rd_block(&tc, evpl, 30, b0);
        CHECK(block_is(b0, 0x22u));               /* durable B wins on LBA30 */
        rd_block(&tc, evpl, 31, b1);
        {
            unsigned i;
            int      match = 1;
            for (i = 0; i < BS; i++) {
                if (b1[i] != patt((uint64_t) 1 * BS + i, 0x11u)) {
                    match = 0;
                    break;
                }
            }
            CHECK(match);                         /* A survives on LBA31 */
        }

        case_close(&tc, evpl);
    }

    /* Case 8 (F1 oracle, crash): the same ordering must hold across a crash
    * with a keep-only policy -- the durable B survives, not the older A. */
    {
        struct tcase                         tc;
        struct chimera_media_crashsim_config cfg;

        memset(&cfg, 0, sizeof(cfg));
        cfg.w_keep = 1; cfg.w_drop = 0; cfg.w_tear = 0;   /* keep-only */
        case_open(&tc, evpl, dir, 8, cfg);

        wr_block(&tc, evpl, 40, 0xAAu, 0 /* plain, seq0 */);
        wr_block(&tc, evpl, 40, 0xBBu, 1 /* fua,   seq1 */);

        do_crash(&tc);

        rd_block(&tc, evpl, 40, b0);
        CHECK(block_is(b0, 0xBBu));               /* durable B survives */

        case_close(&tc, evpl);
    }

    /* Case 9 (F2): a buffered write, then close() WITHOUT pumping the deferral.
     * The queued completion must fire (not be silently dropped) and nothing may
     * leak (ASan/LSan).  close() fires the pending cb with OK; the buffered data
     * it discards was never flushed. */
    {
        struct tcase                         tc;
        struct chimera_media_crashsim_config cfg;
        struct evpl_iovec                    iov;
        struct io_wait                       ww = { 0, CHIMERA_MEDIA_E_IO };
        struct io_wait                       cw = { 0, CHIMERA_MEDIA_E_IO };
        uint8_t                             *d;
        unsigned                             i;
        int                                  n;

        memset(&cfg, 0, sizeof(cfg));
        case_open(&tc, evpl, dir, 9, cfg);

        n = evpl_iovec_alloc(evpl, BS, BS, 1, 0, &iov);
        CHECK(n == 1);
        d = evpl_iovec_data(&iov);
        for (i = 0; i < BS; i++) {
            d[i] = patt(i, 0x5Au);
        }
        /* Submit, then do NOT pump: the completion is still queued on the
         * deferral.  crashsim copies the bytes synchronously, so the caller
         * iovec can be released immediately. */
        tc.p->write(tc.space, &iov, 1, 11, 1, 0 /* plain */, on_io, &ww);
        evpl_iovec_release(evpl, &iov);
        CHECK(ww.done == 0);                      /* not yet completed */

        /* Close without pumping: the pending completion must fire here. */
        tc.p->close(tc.space, on_io, &cw);
        pump(evpl, &cw.done);
        CHECK(cw.status == CHIMERA_MEDIA_OK);
        CHECK(ww.done == 1);                      /* write completion fired */
        CHECK(ww.status == CHIMERA_MEDIA_OK);

        chimera_media_crashsim_destroy(tc.h);
        CHECK(unlink(tc.path) == 0);
    }

    evpl_destroy(evpl);
    CHECK(rmdir(dir) == 0);

    printf("media_crashsim_test: ok\n");
    return 0;
} /* main */
