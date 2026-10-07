// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Integration proof: the allocator over the real loopback (PREAD) media
 * provider, driven through a genuine evpl loop.  Unlike the in-RAM fixture,
 * the provider's completions are asynchronous, so this also exercises:
 *   - the allocator's async format/open/flush/close chains over real media;
 *   - the object-id FACADE_AGAIN park/resume path in its natural case (the
 *     first post-open draw sits at the durable floor and must await the
 *     floor-extension's fua write);
 *   - free-space and id durability across a real close + reopen.
 */

#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "evpl/evpl.h"

#include "chimera_media.h"
#include "media_loopback.h"
#include "facade_alloc.h"
#include "facade_objid.h"

#define ONE_GIB (1ULL << 30)

#define LCHECK(cond, ...)                               \
        do {                                            \
            if (!(cond)) {                              \
                fprintf(stderr, "FAIL %s:%d: ",         \
                        __FILE__, __LINE__);            \
                fprintf(stderr, __VA_ARGS__);           \
                fprintf(stderr, "\n");                  \
                abort();                                \
            }                                           \
        } while (0)

static void
pump(
    struct evpl *evpl,
    const int   *done)
{
    while (!*done) {
        evpl_continue(evpl);
    }
} /* pump */

struct media_open_w {
    enum chimera_media_error st;
    struct chimera_media_space *space;
    int                         done;
};

static void
on_media_open(
    enum chimera_media_error    st,
    struct chimera_media_space *space,
    void                       *priv)
{
    struct media_open_w *w = priv;

    w->st    = st;
    w->space = space;
    w->done  = 1;
} /* on_media_open */

struct media_io_w {
    enum chimera_media_error st;
    int done;
};

static void
on_media_io(
    enum chimera_media_error st,
    void                    *priv)
{
    struct media_io_w *w = priv;

    w->st   = st;
    w->done = 1;
} /* on_media_io */

struct status_w {
    enum facade_status st;
    int done;
};

static void
on_status(
    enum facade_status st,
    void              *priv)
{
    struct status_w *w = priv;

    w->st   = st;
    w->done = 1;
} /* on_status */

struct open_w {
    enum facade_status st;
    struct facade_alloc *a;
    int                  done;
};

static void
on_open(
    enum facade_status   st,
    struct facade_alloc *a,
    void                *priv)
{
    struct open_w *w = priv;

    w->st   = st;
    w->a    = a;
    w->done = 1;
} /* on_open */

struct epoch_w {
    enum facade_status st;
    uint64_t epoch;
    int      done;
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

/* park-aware id draw over async media */
struct iddraw {
    struct facade_alloc     *a;
    struct chimera_sc_obj_id id;
    int                      done;
};

static void
iddraw_resume(void *arg)
{
    struct iddraw     *d  = arg;
    enum facade_status st = facade_objid_next(d->a, &d->id, iddraw_resume, d);

    if (st == FACADE_OK) {
        d->done = 1;
    }
} /* iddraw_resume */

static void
draw_id(
    struct evpl              *evpl,
    struct facade_alloc      *a,
    struct chimera_sc_obj_id *out)
{
    struct iddraw      d  = { a, { { 0 } }, 0 };
    enum facade_status st = facade_objid_next(a, &d.id, iddraw_resume, &d);

    if (st == FACADE_OK) {
        d.done = 1;
    } else {
        LCHECK(st == FACADE_AGAIN, "unexpected status %d", (int) st);
        pump(evpl, &d.done);   /* awaits the floor extension */
    }
    *out = d.id;
} /* draw_id */

int
main(void)
{
    const char                          *tmp = getenv("TMPDIR");
    char                                 dir[512];
    char                                 path[600];
    struct evpl                         *evpl;
    const struct chimera_media_provider *mp;
    struct media_open_w                  ow;
    struct status_w                      sw;
    struct open_w                        opw;
    struct epoch_w                       ew;
    struct chimera_media_space          *space;
    struct facade_alloc                 *a;
    struct facade_geometry               geom = { 0 };
    struct chimera_sc_obj_id             id1, id2;
    chimera_media_block_t                s;
    uint64_t                             total, free_after, e;

    snprintf(dir, sizeof(dir), "%s/facade_mbt_XXXXXX",
             (tmp && tmp[0]) ? tmp : "/tmp");
    LCHECK(mkdtemp(dir) != NULL, "mkdtemp");
    snprintf(path, sizeof(path), "%s/device-0.img", dir);

    evpl = evpl_create(NULL);
    LCHECK(evpl != NULL, "evpl_create");
    mp = chimera_media_loopback();
    LCHECK(mp != NULL, "loopback provider");

    /* open media */
    ow.done = 0;
    LCHECK(chimera_media_open(mp, evpl, path, ONE_GIB, NULL, on_media_open, &ow)
           == CHIMERA_MEDIA_OK, "media open start");
    pump(evpl, &ow.done);
    LCHECK(ow.st == CHIMERA_MEDIA_OK && ow.space, "media open");
    space = ow.space;

    /* format */
    sw.done = 0;
    facade_alloc_format(mp, space, &geom, on_status, &sw);
    pump(evpl, &sw.done);
    LCHECK(sw.st == FACADE_OK, "format");

    /* open allocator */
    opw.done = 0;
    facade_alloc_open(mp, space, on_open, &opw);
    pump(evpl, &opw.done);
    LCHECK(opw.st == FACADE_OK && opw.a, "open");
    a     = opw.a;
    total = facade_alloc_total_blocks(a);
    LCHECK(facade_alloc_free_blocks(a) == total, "fresh free");

    /* allocate; the allocation is BUFFERED until a flush covers its epoch (H5) */
    LCHECK(facade_alloc_blocks(a, 64, &s, &e) == FACADE_OK, "alloc 64");
    LCHECK(facade_alloc_durability(a, e) == FACADE_BUFFERED,
           "allocation durable before any flush?");
    free_after = facade_alloc_free_blocks(a);
    LCHECK(free_after == total - 64, "free after alloc");

    /* first id draw: parks on the async floor extension, resumes via pump */
    draw_id(evpl, a, &id1);

    /* flush: the allocation's epoch is now durable */
    ew.done = 0;
    facade_alloc_flush(a, on_epoch, &ew);
    pump(evpl, &ew.done);
    LCHECK(ew.st == FACADE_OK, "flush");
    LCHECK(facade_alloc_durability(a, e) == FACADE_DURABLE,
           "allocation should be DURABLE after a covering flush");

    /* close (persists) */
    sw.done = 0;
    facade_alloc_close(a, on_status, &sw);
    pump(evpl, &sw.done);
    LCHECK(sw.st == FACADE_OK, "close");

    /* reopen the allocator on the same open media space */
    opw.done = 0;
    facade_alloc_open(mp, space, on_open, &opw);
    pump(evpl, &opw.done);
    LCHECK(opw.st == FACADE_OK && opw.a, "reopen");
    a = opw.a;

    /* free-space survived the clean close */
    LCHECK(facade_alloc_free_blocks(a) == free_after,
           "free count must survive close+reopen: %llu vs %llu",
           (unsigned long long) facade_alloc_free_blocks(a),
           (unsigned long long) free_after);

    /* a post-reopen id is disjoint from and advances past the pre-close id */
    draw_id(evpl, a, &id2);
    LCHECK(memcmp(&id1, &id2, sizeof(id1)) != 0, "id reused across reopen");

    /* teardown */
    sw.done = 0;
    facade_alloc_close(a, on_status, &sw);
    pump(evpl, &sw.done);
    LCHECK(sw.st == FACADE_OK, "final close");

    {
        struct media_io_w mw = { CHIMERA_MEDIA_E_IO, 0 };

        mp->close(space, on_media_io, &mw);
        pump(evpl, &mw.done);
        LCHECK(mw.st == CHIMERA_MEDIA_OK, "media close");
    }

    evpl_destroy(evpl);
    unlink(path);
    rmdir(dir);

    printf("facade_loopback_test: ok\n");
    return 0;
} /* main */
