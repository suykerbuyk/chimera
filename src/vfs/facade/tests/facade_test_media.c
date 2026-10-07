// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/* In-RAM media provider for the facade tests.  See facade_test_media.h. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "evpl/evpl.h"   /* struct evpl_iovec (type only; not linked) */

#include "common/macros.h"   /* SYMBOL_EXPORT */
#include "facade_test_media.h"

#define TM_MAX_BACKINGS 16

/* Per-uri durable image: survives space close/open (a crash keeps only this). */
struct tm_backing {
    char     uri[256];
    uint8_t *durable;
    uint64_t nbytes;
    int      used;
};

/* A queued completion whose byte effect is applied only on release. */
struct tm_pending {
    struct tm_pending    *next;
    chimera_media_io_cb_t cb;
    void                 *priv;
    int                   is_flush;
    int                   fua;
    uint64_t              lba;       /* for a write */
    uint32_t              nblocks;
    uint8_t              *bytes;     /* copied write payload (write only) */
};

struct chimera_media_space {
    struct tm_backing *backing;
    uint8_t           *view;     /* current bytes (durable + applied volatile) */
    uint64_t           nbytes;
    uint64_t           nblocks;
    uint32_t           max_io_blocks;
    int                stall;
    struct tm_pending *q_head;
    struct tm_pending *q_tail;
};

static struct tm_backing g_backings[TM_MAX_BACKINGS];

static struct tm_backing *
tm_find_or_create(
    const char *uri,
    uint64_t    min_size)
{
    uint64_t           nbytes;
    int                i, free_slot = -1;
    struct tm_backing *b;

    for (i = 0; i < TM_MAX_BACKINGS; i++) {
        if (g_backings[i].used && strcmp(g_backings[i].uri, uri) == 0) {
            return &g_backings[i];
        }
        if (!g_backings[i].used && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        return NULL;
    }

    nbytes = min_size ? min_size : CHIMERA_MEDIA_BLOCK_SIZE;
    nbytes = (nbytes + CHIMERA_MEDIA_BLOCK_SIZE - 1) & ~((uint64_t) CHIMERA_MEDIA_BLOCK_SIZE - 1);

    b = &g_backings[free_slot];
    memset(b, 0, sizeof(*b));
    snprintf(b->uri, sizeof(b->uri), "%s", uri);
    b->durable = calloc(1, nbytes);
    if (!b->durable) {
        return NULL;
    }
    b->nbytes = nbytes;
    b->used   = 1;
    return b;
} /* tm_find_or_create */

SYMBOL_EXPORT void
facade_test_media_reset(void)
{
    int i;

    for (i = 0; i < TM_MAX_BACKINGS; i++) {
        if (g_backings[i].used) {
            free(g_backings[i].durable);
            memset(&g_backings[i], 0, sizeof(g_backings[i]));
        }
    }
} /* facade_test_media_reset */

static void
tm_open(
    struct evpl               *evpl,
    const char                *uri,
    uint64_t                   min_size,
    struct prometheus_metrics *metrics,
    chimera_media_open_cb_t    cb,
    void                      *private_data)
{
    struct tm_backing          *b;
    struct chimera_media_space *space;

    (void) evpl;
    (void) metrics;

    b = tm_find_or_create(uri, min_size);
    if (!b) {
        cb(CHIMERA_MEDIA_E_NODEV, NULL, private_data);
        return;
    }

    space = calloc(1, sizeof(*space));
    if (!space) {
        cb(CHIMERA_MEDIA_E_NODEV, NULL, private_data);
        return;
    }
    space->backing = b;
    space->nbytes  = b->nbytes;
    space->nblocks = b->nbytes >> CHIMERA_MEDIA_BLOCK_SHIFT;
    space->view    = malloc(b->nbytes);
    if (!space->view) {
        free(space);
        cb(CHIMERA_MEDIA_E_NODEV, NULL, private_data);
        return;
    }
    memcpy(space->view, b->durable, b->nbytes);   /* crash view => durable only */
    space->max_io_blocks = 256;
    cb(CHIMERA_MEDIA_OK, space, private_data);
} /* tm_open */

/* Validate like the loopback provider. */
static enum chimera_media_error
tm_check(
    const struct chimera_media_space *space,
    const struct evpl_iovec          *iov,
    int                               niov,
    chimera_media_block_t             lba,
    uint32_t                          nblocks)
{
    uint64_t total = 0;
    int i;

    if (!iov || niov <= 0 || nblocks == 0 || nblocks > space->max_io_blocks) {
        return CHIMERA_MEDIA_E_INVAL;
    }
    if (nblocks > space->nblocks || lba > space->nblocks - nblocks) {
        return CHIMERA_MEDIA_E_RANGE;
    }
    for (i = 0; i < niov; i++) {
        total += iov[i].length;
    }
    if (total != ((uint64_t) nblocks << CHIMERA_MEDIA_BLOCK_SHIFT)) {
        return CHIMERA_MEDIA_E_INVAL;
    }
    return CHIMERA_MEDIA_OK;
} /* tm_check */

/* Apply a pending op's byte effect, then free it. */
static void
tm_apply(
    struct chimera_media_space *space,
    struct tm_pending          *op)
{
    uint64_t off;

    if (op->is_flush) {
        memcpy(space->backing->durable, space->view, space->nbytes);
    } else {
        off = op->lba << CHIMERA_MEDIA_BLOCK_SHIFT;
        memcpy(space->view + off, op->bytes, op->nblocks << CHIMERA_MEDIA_BLOCK_SHIFT);
        if (op->fua) {
            memcpy(space->backing->durable + off, op->bytes,
                   op->nblocks << CHIMERA_MEDIA_BLOCK_SHIFT);
        }
    }
} /* tm_apply */

static void
tm_enqueue(
    struct chimera_media_space *space,
    struct tm_pending          *op)
{
    op->next = NULL;
    if (space->q_tail) {
        space->q_tail->next = op;
    } else {
        space->q_head = op;
    }
    space->q_tail = op;
} /* tm_enqueue */

/* Complete op now: apply + fire cb + free. */
static void
tm_complete(
    struct chimera_media_space *space,
    struct tm_pending          *op)
{
    chimera_media_io_cb_t cb   = op->cb;
    void                 *priv = op->priv;

    tm_apply(space, op);
    free(op->bytes);
    free(op);
    cb(CHIMERA_MEDIA_OK, priv);
} /* tm_complete */

static void
tm_read(
    struct chimera_media_space *space,
    struct evpl_iovec          *iov,
    int                         niov,
    chimera_media_block_t       lba,
    uint32_t                    nblocks,
    chimera_media_io_cb_t       cb,
    void                       *private_data)
{
    enum chimera_media_error err = tm_check(space, iov, niov, lba, nblocks);
    uint8_t                 *src;
    int                      i;

    if (err != CHIMERA_MEDIA_OK) {
        cb(err, private_data);
        return;
    }
    src = space->view + (lba << CHIMERA_MEDIA_BLOCK_SHIFT);
    for (i = 0; i < niov; i++) {
        memcpy(iov[i].data, src, iov[i].length);
        src += iov[i].length;
    }
    cb(CHIMERA_MEDIA_OK, private_data);   /* reads always complete inline */
} /* tm_read */

static void
tm_write(
    struct chimera_media_space *space,
    const struct evpl_iovec    *iov,
    int                         niov,
    chimera_media_block_t       lba,
    uint32_t                    nblocks,
    int                         fua,
    chimera_media_io_cb_t       cb,
    void                       *private_data)
{
    enum chimera_media_error err = tm_check(space, iov, niov, lba, nblocks);
    struct tm_pending       *op;
    uint8_t                 *p;
    int                      i;

    if (err != CHIMERA_MEDIA_OK) {
        cb(err, private_data);
        return;
    }

    op          = calloc(1, sizeof(*op));
    op->cb      = cb;
    op->priv    = private_data;
    op->fua     = fua ? 1 : 0;
    op->lba     = lba;
    op->nblocks = nblocks;
    op->bytes   = malloc(nblocks << CHIMERA_MEDIA_BLOCK_SHIFT);
    p           = op->bytes;
    for (i = 0; i < niov; i++) {
        memcpy(p, iov[i].data, iov[i].length);
        p += iov[i].length;
    }

    if (space->stall) {
        tm_enqueue(space, op);
        return;
    }
    tm_complete(space, op);
} /* tm_write */

static void
tm_flush(
    struct chimera_media_space *space,
    chimera_media_io_cb_t       cb,
    void                       *private_data)
{
    struct tm_pending *op = calloc(1, sizeof(*op));

    op->cb       = cb;
    op->priv     = private_data;
    op->is_flush = 1;

    if (space->stall) {
        tm_enqueue(space, op);
        return;
    }
    tm_complete(space, op);
} /* tm_flush */

static void
tm_close(
    struct chimera_media_space *space,
    chimera_media_io_cb_t       cb,
    void                       *private_data)
{
    struct tm_pending *op;

    /* Any still-queued completions are lost (a crash drops in-flight ops); the
     * durable image is unaffected. */
    while (space->q_head) {
        op            = space->q_head;
        space->q_head = op->next;
        free(op->bytes);
        free(op);
    }
    free(space->view);
    free(space);
    cb(CHIMERA_MEDIA_OK, private_data);
} /* tm_close */

static uint64_t
tm_size_bytes(struct chimera_media_space *space)
{
    return space->nblocks << CHIMERA_MEDIA_BLOCK_SHIFT;
} /* tm_size_bytes */

static void
tm_info(
    struct chimera_media_space      *space,
    struct chimera_media_space_info *out)
{
    out->nblocks       = space->nblocks;
    out->block_size    = CHIMERA_MEDIA_BLOCK_SIZE;
    out->max_io_blocks = space->max_io_blocks;
} /* tm_info */

static const struct chimera_media_provider tm_provider = {
    .abi_version = CHIMERA_MEDIA_ABI_VERSION,
    .reserved    = 0,
    .name        = "facade-test-ram",
    .open        = tm_open,
    .read        = tm_read,
    .write       = tm_write,
    .flush       = tm_flush,
    .close       = tm_close,
    .size_bytes  = tm_size_bytes,
    .info        = tm_info,
};

SYMBOL_EXPORT const struct chimera_media_provider *
facade_test_media(void)
{
    return &tm_provider;
} /* facade_test_media */

SYMBOL_EXPORT void
facade_test_media_stall_writes(
    struct chimera_media_space *space,
    int                         on)
{
    space->stall = on ? 1 : 0;
} /* facade_test_media_stall_writes */

SYMBOL_EXPORT int
facade_test_media_pending_count(struct chimera_media_space *space)
{
    struct tm_pending *op = space->q_head;
    int                n  = 0;

    while (op) {
        n++;
        op = op->next;
    }
    return n;
} /* facade_test_media_pending_count */

SYMBOL_EXPORT int
facade_test_media_write_pending(struct chimera_media_space *space)
{
    return space->q_head != NULL;
} /* facade_test_media_write_pending */

/* Detach the oldest pending op and complete it. */
static void
tm_release_head(struct chimera_media_space *space)
{
    struct tm_pending *op = space->q_head;

    space->q_head = op->next;
    if (!space->q_head) {
        space->q_tail = NULL;
    }
    tm_complete(space, op);   /* may enqueue more */
} /* tm_release_head */

SYMBOL_EXPORT void
facade_test_media_release(struct chimera_media_space *space)
{
    if (space->q_head) {
        tm_release_head(space);
    }
} /* facade_test_media_release */

SYMBOL_EXPORT int
facade_test_media_release_lba(
    struct chimera_media_space *space,
    uint64_t                    lba)
{
    struct tm_pending *op   = space->q_head;
    struct tm_pending *prev = NULL;

    while (op) {
        if (!op->is_flush && op->lba == lba) {
            if (prev) {
                prev->next = op->next;
            } else {
                space->q_head = op->next;
            }
            if (space->q_tail == op) {
                space->q_tail = prev;
            }
            tm_complete(space, op);
            return 1;
        }
        prev = op;
        op   = op->next;
    }
    return 0;
} /* facade_test_media_release_lba */

SYMBOL_EXPORT void
facade_test_media_release_all(struct chimera_media_space *space)
{
    while (space->q_head) {
        tm_release_head(space);
    }
} /* facade_test_media_release_all */
