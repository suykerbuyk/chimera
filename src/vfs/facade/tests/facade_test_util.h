// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * Shared helpers for the facade allocator tests over the in-RAM fixture, whose
 * lifecycle callbacks fire synchronously.  TCHECK is an invariant guard that
 * survives NDEBUG (unlike assert), so the Release re-gate keeps exercising it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "facade_alloc.h"
#include "facade_extent.h"
#include "facade_internal.h"   /* white-box: facade_obj_find / facade_obj_create_entry */
#include "facade_object.h"
#include "facade_objid.h"
#include "facade_test_media.h"

#define TCHECK(cond, ...)                               \
        do {                                            \
            if (!(cond)) {                              \
                fprintf(stderr, "FAIL %s:%d: ",         \
                        __FILE__, __LINE__);            \
                fprintf(stderr, __VA_ARGS__);           \
                fprintf(stderr, "\n");                  \
                abort();                                \
            }                                           \
        } while (0)

struct tu_media_open_w {
    enum chimera_media_error st;
    struct chimera_media_space *space;
    int                         done;
};

static inline void
tu_on_media_open(
    enum chimera_media_error    st,
    struct chimera_media_space *space,
    void                       *priv)
{
    struct tu_media_open_w *w = priv;

    w->st    = st;
    w->space = space;
    w->done  = 1;
} /* tu_on_media_open */

static inline struct chimera_media_space *
tu_media_open(
    const char *uri,
    uint64_t    size)
{
    struct tu_media_open_w   w = { CHIMERA_MEDIA_E_IO, NULL, 0 };
    enum chimera_media_error e;

    e = chimera_media_open(facade_test_media(), NULL, uri, size, NULL,
                           tu_on_media_open, &w);
    TCHECK(e == CHIMERA_MEDIA_OK && w.done && w.st == CHIMERA_MEDIA_OK && w.space,
           "media open failed");
    return w.space;
} /* tu_media_open */

struct tu_status_w {
    enum facade_status st;
    int done;
};

static inline void
tu_on_status(
    enum facade_status st,
    void              *priv)
{
    struct tu_status_w *w = priv;

    w->st   = st;
    w->done = 1;
} /* tu_on_status */

static inline void
tu_format(struct chimera_media_space *space)
{
    struct tu_status_w     w = { FACADE_E_IO, 0 };
    struct facade_geometry g = { 0 };

    facade_alloc_format(facade_test_media(), space, &g, tu_on_status, &w);
    TCHECK(w.done && w.st == FACADE_OK, "format failed");
} /* tu_format */

struct tu_open_w {
    enum facade_status st;
    struct facade_alloc *alloc;
    int                  done;
};

static inline void
tu_on_open(
    enum facade_status   st,
    struct facade_alloc *alloc,
    void                *priv)
{
    struct tu_open_w *w = priv;

    w->st    = st;
    w->alloc = alloc;
    w->done  = 1;
} /* tu_on_open */

static inline struct facade_alloc *
tu_open(struct chimera_media_space *space)
{
    struct tu_open_w w = { FACADE_E_IO, NULL, 0 };

    facade_alloc_open(facade_test_media(), space, tu_on_open, &w);
    TCHECK(w.done && w.st == FACADE_OK && w.alloc, "open failed");
    return w.alloc;
} /* tu_open */

static inline void
tu_close(struct facade_alloc *a)
{
    struct tu_status_w w = { FACADE_E_IO, 0 };

    facade_alloc_close(a, tu_on_status, &w);
    TCHECK(w.done && w.st == FACADE_OK, "close failed");
} /* tu_close */

struct tu_epoch_w {
    enum facade_status st;
    uint64_t epoch;
    int      done;
};

static inline void
tu_on_epoch(
    enum facade_status st,
    uint64_t           epoch,
    void              *priv)
{
    struct tu_epoch_w *w = priv;

    w->st    = st;
    w->epoch = epoch;
    w->done  = 1;
} /* tu_on_epoch */

static inline uint64_t
tu_flush(struct facade_alloc *a)
{
    struct tu_epoch_w w = { FACADE_E_IO, 0, 0 };

    facade_alloc_flush(a, tu_on_epoch, &w);
    TCHECK(w.done && w.st == FACADE_OK, "flush failed");
    return w.epoch;
} /* tu_flush */

static inline void
tu_on_mclose(
    enum chimera_media_error st,
    void                    *priv)
{
    *(int *) priv = (st == CHIMERA_MEDIA_OK);
} /* tu_on_mclose */

static inline void
tu_media_close(struct chimera_media_space *space)
{
    int ok = 0;

    facade_test_media()->close(space, tu_on_mclose, &ok);
    TCHECK(ok, "media close failed");
} /* tu_media_close */

/*
 * A park-aware id draw.  tu_draw_start issues the draw; if it parks
 * (FACADE_AGAIN) the resume re-issues it when the floor extension lands.  The
 * flags record the terminal outcome: got (an id was produced), shutdown (the
 * allocator was torn down under it), again (still parked).
 */
struct tu_draw {
    struct facade_alloc     *a;
    struct chimera_sc_obj_id id;
    int                      got;
    int                      shutdown;
    int                      again;
};

static inline void
tu_draw_resume(void *arg)
{
    struct tu_draw    *d  = arg;
    enum facade_status st = facade_objid_next(d->a, &d->id, tu_draw_resume, d);

    d->again = 0;
    if (st == FACADE_OK) {
        d->got = 1;
    } else if (st == FACADE_E_SHUTDOWN) {
        d->shutdown = 1;
    } else if (st == FACADE_AGAIN) {
        d->again = 1;
    }
} /* tu_draw_resume */

static inline enum facade_status
tu_draw_start(
    struct tu_draw      *d,
    struct facade_alloc *a)
{
    enum facade_status st;

    d->a        = a;
    d->got      = 0;
    d->shutdown = 0;
    d->again    = 0;
    st          = facade_objid_next(a, &d->id, tu_draw_resume, d);
    if (st == FACADE_OK) {
        d->got = 1;
    } else if (st == FACADE_E_SHUTDOWN) {
        d->shutdown = 1;
    } else if (st == FACADE_AGAIN) {
        d->again = 1;
    }
    return st;
} /* tu_draw_start */

/*
 * A durable single-block write at offset 0, creating the object entry if absent
 * (object-ops-v1 split create from write; this mirrors the slice's auto-create
 * so the inherited media-log tests keep driving one call).  The in-RAM fixture
 * completes inline, so the epoch result is set by return.
 */
static inline void
tu_write0(
    struct facade_alloc *a,
    const uint8_t        id[16],
    const void          *data,
    uint32_t             len,
    struct tu_epoch_w   *w)
{
    if (!facade_obj_find(a, id)) {
        facade_obj_create_entry(a, id, NULL);
    }
    w->st   = FACADE_E_IO;
    w->done = 0;
    facade_obj_write(a, id, 0, data, len, CHIMERA_SC_WRITE_DURABLE, tu_on_epoch, w);
} // tu_write0

/* Physical block backing an object's logical block 0, or a miss. */
static inline int
tu_obj_phys0(
    struct facade_alloc   *a,
    const uint8_t          id[16],
    chimera_media_block_t *r_phys)
{
    struct facade_obj   *o = facade_obj_find(a, id);
    struct facade_extent ext;

    if (!o || !facade_extent_map_lookup(o->map, 0, &ext) ||
        ext.kind == CHIMERA_SC_SEG_HOLE) {
        return 0;
    }
    *r_phys = ext.phys_block + (0 - ext.logical_block);
    return 1;
} // tu_obj_phys0

/* The 64-bit sequence half of an id (low 8 bytes); the high 8 are the nonce. */
static inline uint64_t
tu_id_seq(const struct chimera_sc_obj_id *id)
{
    uint64_t seq;

    memcpy(&seq, &id->b[8], sizeof(seq));
    return seq;
} /* tu_id_seq */

static inline uint64_t
tu_id_nonce(const struct chimera_sc_obj_id *id)
{
    uint64_t nonce;

    memcpy(&nonce, &id->b[0], sizeof(nonce));
    return nonce;
} /* tu_id_nonce */
