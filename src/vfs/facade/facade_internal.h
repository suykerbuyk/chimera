// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * Backend-facade allocator: private shared definitions.  Not installed; the
 * public headers keep struct facade_alloc opaque.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "common/rbtree.h"

#include "facade_alloc.h"
#include "facade_objid.h"
#include "facade_super.h"

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

/*
 * Object ids reserved ahead of the durable floor per superblock write.  The
 * floor is extended at half this, so an extension's fua write normally lands
 * long before the counter catches the floor.  Also the grow step the fresh
 * format seeds.
 */
#define FACADE_ID_RESERVE 1024ULL

/* Invariant guard that survives NDEBUG (unlike assert): used for caller-side
 * violations that must never reach a release build silently (e.g. a
 * double-free). */
#define FACADE_CHECK(cond, msg)                                       \
        do {                                                          \
            if (!(cond)) {                                            \
                fprintf(stderr, "facade: CHECK failed: %s (%s:%d)\n", \
                        (msg), __FILE__, __LINE__);                   \
                abort();                                              \
            }                                                         \
        } while (0)

/* In-memory free extent, keyed by start block in free_by_start. */
struct facade_free_ext {
    struct rb_node node;
    uint64_t       start;
    uint64_t       nblocks;
};

/* A parked id draw, waiting for the floor extension to land. */
struct facade_waiter {
    facade_resume_t       resume;
    void                 *arg;
    struct facade_waiter *next;
};

/* A persist job (flush/close/format), forward-declared so the allocator can
 * queue pending ones behind an in-flight metadata write (see facade_alloc.c). */
struct facade_persist_ctx;

struct facade_alloc {
    const struct chimera_media_provider *provider;
    struct chimera_media_space          *space;

    /* geometry (from the superblock) */
    uint64_t                             nblocks;
    uint64_t                             data_first;
    uint64_t                             data_last; /* one past the last data block */
    uint64_t                             ckpt_block;
    uint64_t                             ckpt_nblocks;
    uint32_t                             max_io_blocks;

    /* last-persisted checkpoint descriptor, mirrored so a floor-only
     * superblock write preserves it (ckpt_count free extents, clean => the
     * checkpoint reflects a consistent state). */
    uint64_t                             ckpt_count;
    uint32_t                             clean;

    /* free space */
    struct rb_tree                       free_by_start;
    uint64_t                             free_blocks;

    /* durability epoch (H5) */
    uint64_t                             cur_epoch; /* epoch new allocations are tagged with */
    uint64_t                             durable_epoch; /* highest epoch persisted + flushed */

    /* object identity (H2) */
    uint64_t                             id_nonce;
    uint64_t                             id_next; /* next sequence to issue */
    uint64_t                             id_floor; /* durable high-water */
    struct facade_waiter                *waiters;

    /*
     * Metadata-write serialization (F1/F2).  Every superblock (block 0) write
     * -- an id-floor extension OR a persist's superblock step -- runs through a
     * single in-flight slot, so block 0 is never written by two ops at once and
     * its durable floor is monotonic.  A persist requested while busy is queued
     * (FIFO); an extension requested while busy is coalesced into ext_pending.
     * mw_complete() runs the next waiting op when the current one finishes.
     *
     * Teardown (close/abandon) is deferred until the slot is free: destroy_after
     * frees the allocator from mw_complete() once no write is in flight, so a
     * deferred completion never dereferences freed memory.  shutting_down makes
     * a resumed id draw fail cleanly instead of re-parking on a dying allocator.
     */
    int                                  mw_busy;       /* a block-0 write is in flight */
    int                                  ext_pending;   /* extension requested while busy */
    int                                  shutting_down; /* teardown: draws fail, no new ext */
    int                                  destroy_after; /* free when the current op completes */
    struct facade_persist_ctx           *persist_head;  /* queued persists (FIFO) */
    struct facade_persist_ctx           *persist_tail;

    /* one metadata block (superblock) buffer, reused for in-flight writes */
    void                                *super_buf;
};

/* facade_super.c */
void
facade_super_pack(
    const struct facade_alloc *alloc,
    void                      *block /* FACADE_SUPER_SIZE bytes */);

/* facade_objid.c: issue the id-floor extension's superblock write.  Requires
* the metadata slot free (mw_busy == 0); called by mw_complete() to run a
* coalesced ext_pending, and by the objid draw path when the slot is idle. */
void
facade_objid_start_extension(
    struct facade_alloc *alloc);

/* facade_objid.c: resume every parked id draw.  On a normal floor landing a
* resumed draw retries; during teardown (shutting_down) it fails cleanly. */
void
facade_objid_drain_waiters(
    struct facade_alloc *alloc);

/* facade_alloc.c: the current metadata (block-0) write has completed; release
 * the slot and start the next waiting op, or tear the allocator down. */
void
facade_mw_complete(
    struct facade_alloc *alloc);

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
