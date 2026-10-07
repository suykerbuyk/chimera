// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * Backend-facade allocator: private shared definitions.  Not installed; the
 * public headers keep struct facade_alloc opaque.
 *
 * media-log-and-recovery extends the block-allocator-and-identity allocator
 * with a durability log (facade_log.{h,c}), crash recovery (facade_recover.c)
 * and a durable object table folded into an A/B checkpoint, so an acknowledged
 * durable object survives a log compaction and a crash.
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
#define FACADE_ID_RESERVE  1024ULL

/* Prototype object-table budget: the maximum live objects and the maximum
 * extents per object the checkpoint is sized for.  object-ops-v1 generalizes
 * both (and swaps the flat extent array for facade_extent_map). */
#define FACADE_OBJ_BUDGET  256u
#define FACADE_OBJ_MAX_EXT 8u

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

/* One extent of an in-memory object-table entry (mirrors facade_ckpt_objext). */
struct facade_objext {
    uint64_t              logical_block;
    chimera_media_block_t phys_block;
    uint64_t              nblocks;
    uint32_t              kind;    /* CHIMERA_SC_SEG_DATA / _UNWRITTEN */
};

/* An in-memory object-table entry: the durable object record (id -> hold flag
 * + extents).  Persisted in the checkpoint and (incrementally) in the log. */
struct facade_obj {
    int                  used;
    uint8_t              id[16];
    uint32_t             flags;    /* FACADE_OBJ_* */
    uint32_t             nextents;
    struct facade_objext ext[FACADE_OBJ_MAX_EXT];
};

struct facade_alloc {
    const struct chimera_media_provider *provider;
    struct chimera_media_space          *space;

    /* geometry (from the superblock) */
    uint64_t                             nblocks;
    uint64_t                             data_first;
    uint64_t                             data_last; /* one past the last data block */
    uint64_t                             ckpt_a_block;
    uint64_t                             ckpt_b_block;
    uint64_t                             ckpt_nblocks; /* size of EACH slot */
    uint64_t                             log_block;
    uint64_t                             log_nblocks;
    uint32_t                             max_io_blocks;

    /* A/B checkpoint selection + generation (mirrored for a super write). */
    uint32_t                             active_ckpt;    /* 0 => A, 1 => B */
    uint64_t                             ckpt_generation;

    /* free space */
    struct rb_tree                       free_by_start;
    uint64_t                             free_blocks;

    /* object table (durable; folded into the checkpoint) */
    struct facade_obj                   *objs;      /* FACADE_OBJ_BUDGET entries */
    uint32_t                             nobjs;     /* used entries */

    /* durability epoch (H5): in-memory ordering counter for allocations */
    uint64_t                             cur_epoch;     /* epoch new allocations are tagged with */
    uint64_t                             durable_epoch; /* highest epoch persisted + flushed */

    /* persisted power-loss generation (equality token; bumped on recovery) */
    uint64_t                             power_loss_generation;

    /* log cursor (facade_log.c) */
    uint64_t                             next_seq;       /* seq for the next appended record */
    uint64_t                             durable_seq;    /* highest seq whose commit is durable */
    uint64_t                             checkpoint_seq; /* seq the active checkpoint reflects */
    uint64_t                             log_next;       /* next free record slot (0-based from log_block) */

    /* clean / dirty-window marking (facade_alloc.c) */
    uint32_t                             clean;        /* on-media clean flag value */
    int                                  dirty_armed;  /* clean=0 already made durable this window */

    /* object identity (H2) */
    uint64_t                             id_nonce;
    uint64_t                             id_next; /* next sequence to issue */
    uint64_t                             id_floor; /* durable high-water */
    struct facade_waiter                *waiters;

    /*
     * Metadata-write serialization (F1/F2).  Every superblock (block 0) write
     * -- an id-floor extension OR a persist/compaction's superblock step -- runs
     * through a single in-flight slot, so block 0 is never written by two ops at
     * once and its durable floor is monotonic.  A persist requested while busy is
     * queued (FIFO); an extension requested while busy is coalesced into
     * ext_pending.  mw_complete() runs the next waiting op when the current one
     * finishes.  `inflight` additionally counts log/object-data media ops so
     * teardown waits for them too.
     *
     * Teardown (close/abandon) is deferred until the slot is free AND inflight
     * is zero: destroy_after frees the allocator once no media op is in flight,
     * so a deferred completion never dereferences freed memory.  shutting_down
     * makes a resumed id draw or a late op fail cleanly on a dying allocator.
     */
    int         mw_busy;                                /* a block-0 write is in flight */
    int         ext_pending;                            /* extension requested while busy */
    /* A compaction (flush/close) requested while a block-0 id-floor extension is
     * in flight is deferred here and run by mw_complete(), so the two never write
     * block 0 concurrently (F1).  The caller awaits every other op, so at most
     * one compaction is ever queued. */
    int         compact_pending;
    facade_cb_t compact_cb;
    void       *compact_priv;
    /* A dirty-mark (clean=0 block-0 write) requested while the block-0 slot is
     * busy is deferred here and issued by mw_complete(), so the clean=0 write
     * never races an id-floor extension on block 0 (F1). */
    int         dirty_mark_pending;
    facade_cb_t dirty_cb;
    void       *dirty_priv;
    int         shutting_down;                          /* teardown: draws fail, no new ext */
    int         destroy_after;                          /* free when the last op completes */
    int         inflight;                               /* log/data/ckpt media ops in flight */
    int         cb_depth;                               /* nested media completions on the stack
                                                         * (a provider that completes inline nests
                                                         * them); only the outermost tears down */

    /* one metadata block (superblock) buffer, reused for in-flight writes */
    void       *super_buf;
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

/* facade_objid.c: resume every parked id draw. */
void
facade_objid_drain_waiters(
    struct facade_alloc *alloc);

/* facade_alloc.c: the current metadata (block-0) write has completed; release
 * the slot and start the next waiting op, or tear the allocator down. */
void
facade_mw_complete(
    struct facade_alloc *alloc);

/* facade_alloc.c: remove a specific allocated run from the free set (ALLOC
* replay); the run must lie wholly within one free extent.  Returns 0, or -1 if
* the range is not free (a corrupt/inconsistent log record -- rejected). */
int
facade_free_remove(
    struct facade_alloc  *alloc,
    chimera_media_block_t start,
    uint64_t              nblocks);

/* facade_alloc.c: add a run back to the free set (FREE replay). */
void
facade_free_add(
    struct facade_alloc  *alloc,
    chimera_media_block_t start,
    uint64_t              nblocks);

/* facade_alloc.c: object-table helpers (used by facade_log.c and recovery). */
struct facade_obj *
facade_obj_find(
    struct facade_alloc *alloc,
    const uint8_t        id[16]);

/* Upsert an object record (create-or-replace id -> flags + the given extents).
 * Returns the entry, or NULL if the object budget is exhausted. */
struct facade_obj *
facade_obj_put(
    struct facade_alloc        *alloc,
    const uint8_t               id[16],
    uint32_t                    flags,
    const struct facade_objext *ext,
    uint32_t                    nextents);

/* facade_alloc.c: pack the active (de)serialization of the free set + object
 * table into a checkpoint-slot buffer; returns the number of blocks used. */
uint64_t
facade_ckpt_serialize(
    struct facade_alloc *alloc,
    uint64_t             generation,
    uint64_t             checkpoint_seq,
    void               **r_buf);

/* facade_alloc.c: begin a dirty window if not already armed -- make clean=0
 * durable (FUA super) before the first dependent log record.  Calls cb when the
 * window is armed (immediately if already armed). */
void
facade_mark_dirty(
    struct facade_alloc *alloc,
    facade_cb_t          cb,
    void                *private_data);

/* facade_alloc.c: run an A/B compaction now (serialize free set + object table
 * into the inactive slot, flip the superblock, reset the log).  cb on done. */
void
facade_compact(
    struct facade_alloc *alloc,
    facade_cb_t          cb,
    void                *private_data);

/* facade_alloc.c: in-flight accounting for log/data media ops (teardown). */
void
facade_inflight_begin(
    struct facade_alloc *alloc);
void
facade_inflight_end(
    struct facade_alloc *alloc);

/* facade_alloc.c: chunked async media helpers over the provider vtable, with
 * in-flight accounting.  The buffer is caller-owned and must stay valid until
 * cb fires.  cb(status, private_data) runs on the space's evpl thread. */
void
facade_media_write(
    struct facade_alloc *alloc,
    const void          *buf,
    uint64_t             lba,
    uint64_t             nblocks,
    int                  fua,
    facade_cb_t          cb,
    void                *private_data);
void
facade_media_read(
    struct facade_alloc *alloc,
    void                *buf,
    uint64_t             lba,
    uint64_t             nblocks,
    facade_cb_t          cb,
    void                *private_data);
void
facade_media_flush(
    struct facade_alloc *alloc,
    facade_cb_t          cb,
    void                *private_data);

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
