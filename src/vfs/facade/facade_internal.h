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
#include "facade_extent.h"
#include "facade_objid.h"
#include "facade_super.h"
#include "sc_types.h"   /* struct chimera_sc_stamps */

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

/*
 * Object ids reserved ahead of the durable floor per superblock write.  The
 * floor is extended at half this, so an extension's fua write normally lands
 * long before the counter catches the floor.  Also the grow step the fresh
 * format seeds.
 */
#define FACADE_ID_RESERVE            1024ULL

/* Prototype object-table budget: the maximum live objects and the maximum
 * extents per object the checkpoint, the log OBJ record and recovery are all
 * sized for.  The full object surface (arbitrary-offset writes, allocate, punch)
 * fragments an object's map, so object-ops-v1 raised the per-object extent cap
 * from the slice's 8; one log OBJ record is still one 4 KiB block
 * (sizeof(facade_lr_hdr) + sizeof(facade_obj_rec) + CAP * sizeof(objext) <=
 * 4096), and an op that would exceed the cap is refused NO_SPACE-shaped rather
 * than corrupting the map. */
#define FACADE_OBJ_BUDGET            256u
#define FACADE_OBJ_MAX_EXT           64u

/*
 * Namespace-record-store caps (v4).  A put/delete is one FACADE_LR_NS log block;
 * key + value stay well within one 4 KiB block (facade_lr_hdr 48 + facade_lr_ns
 * 24 + 256 + 512 = 840 <= 4096), so no multi-block record is ever needed.
 * FACADE_NS_MAX_TXN_LOG_BLOCKS bounds a whole NS transaction's log footprint
 * (NS records + OBJ records + COMMIT) to the raised log floor; the geometry's
 * log_nblocks floor is >= this, so any accepted txn fits an empty log.
 * FACADE_NS_BUDGET is the full-size live/checkpoint record cap; it scales down on
 * a small media space (facade_compute_geometry, like obj_budget) into ns_budget.
 */
#define FACADE_NS_MAX_KEY            256u
#define FACADE_NS_MAX_VALUE          512u
#define FACADE_NS_MAX_TXN_LOG_BLOCKS 16u
#define FACADE_NS_BUDGET             256u

/*
 * One in-memory namespace record.  The store is a sorted array of these ordered
 * by EXACT key bytes (facade_ns.c) -- NOT common/rbtree.h, whose macro key is a
 * single scalar field compared with < / > and so cannot express byte-string
 * order (the M32 fix needs exact byte comparison, and the stable scan cursor
 * needs true byte order, which a scalar/hash key would violate -- the H1 bug).
 * key/val are owned heap copies; revision advances on each put of the key.
 */
struct facade_ns_rec {
    uint8_t *key;
    uint32_t key_len;
    uint8_t *val;
    uint32_t val_len;
    uint64_t revision;
};

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

/*
 * An in-memory object-table entry: the durable object record.  The extent map
 * (facade_extent_map) is the authority for the object's logical->physical
 * mapping, holes-by-absence and the owned/borrowed bit; the flat
 * facade_ckpt_objext list is only the serialization form (checkpoint + log OBJ
 * record).  The scalars (state, size, change, stamps, retention, hold flag) are
 * everything OBJ_STAT reports and everything the 4.2 state machine and the D2
 * clone must survive a crash with.  Persisted in the A/B checkpoint and
 * (incrementally, per durable transaction) in the log.
 */
struct facade_obj {
    int                       used;
    uint8_t                   id[16];
    uint32_t                  flags;    /* FACADE_OBJ_* */
    uint32_t                  state;    /* enum chimera_sc_obj_state (OPEN/SEALED) */
    uint64_t                  size;     /* logical byte length */
    uint64_t                  change;   /* advances on every data/metadata change */
    struct chimera_sc_stamps  stamps;
    uint64_t                  retention_until_ns;
    uint32_t                  retention_mode;
    uint32_t                  refs;     /* minimal binding refcount: 0 or 1 (v4) */
    struct facade_extent_map *map;      /* logical->physical authority (NULL only when unused) */
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

    /* namespace record store (durable; folded into the checkpoint after the
     * object section, v4).  A sorted-by-exact-key array of record pointers. */
    struct facade_ns_rec               **ns_recs;   /* sorted ascending by key bytes */
    uint32_t                             nns;        /* live record count */
    uint32_t                             ns_cap;     /* allocated slots in ns_recs */
    uint32_t                             ns_budget;  /* live + checkpoint cap (M3-scaled) */

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
    int                       mw_busy;                  /* a block-0 write is in flight */
    int                       ext_pending;              /* extension requested while busy */
    /* A compaction (flush/close) requested while a block-0 id-floor extension is
     * in flight is deferred here and run by mw_complete(), so the two never write
     * block 0 concurrently (F1).  The caller awaits every other op, so at most
     * one compaction is ever queued. */
    int                       compact_pending;
    facade_cb_t               compact_cb;
    void                     *compact_priv;
    /* A dirty-mark (clean=0 block-0 write) requested while the block-0 slot is
     * busy is deferred here and issued by mw_complete(), so the clean=0 write
     * never races an id-floor extension on block 0 (F1). */
    int                       dirty_mark_pending;
    facade_cb_t               dirty_cb;
    void                     *dirty_priv;
    int                       shutting_down;            /* teardown: draws fail, no new ext */
    int                       destroy_after;            /* free when the last op completes */
    int                       inflight;                 /* log/data/ckpt media ops in flight */
    int                       cb_depth;                 /* nested media completions on the stack
                                                         * (a provider that completes inline nests
                                                         * them); only the outermost tears down */

    /*
     * Durable-object-transaction FIFO (object-ops-v1, real concurrency).  Many
     * requests are in flight per thread; every durable object transaction (a
     * write/seal/clone/size-change's log append sequence, or an object release's
     * forced compaction) runs one at a time through this slot, so two in-flight
     * writers can never race the log cursor or a forced compaction.  The slice's
     * single-slot "only one queued" asserts could not express that.  A txn that
     * finds the slot busy enqueues a starter; txn_release() runs the next.
     */
    int                       txn_busy;
    struct facade_txn_waiter *txn_head;
    struct facade_txn_waiter *txn_tail;

    /* one metadata block (superblock) buffer, reused for in-flight writes */
    void                     *super_buf;
};

/* A pending durable-object-transaction starter, queued behind txn_busy. */
struct facade_txn_waiter {
    void                      (*start)(
        void *arg);
    void                     *arg;
    struct facade_txn_waiter *next;
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

/* facade_alloc.c: object-table helpers (used by facade_log.c, facade_object.c
 * and recovery). */
struct facade_obj *
facade_obj_find(
    struct facade_alloc *alloc,
    const uint8_t        id[16]);

/* Create a fresh OPEN object entry (empty extent map) with the given id and
 * stamps.  Returns the entry, or NULL if the object budget is exhausted. */
struct facade_obj *
facade_obj_create_entry(
    struct facade_alloc            *alloc,
    const uint8_t                   id[16],
    const struct chimera_sc_stamps *stamps);

/* Reconstruct an object from an on-media record header + its extents (recovery
 * and checkpoint load): create-or-replace id -> the full durable record, with a
 * fresh extent map built from exts.  Returns the entry, or NULL on budget
 * exhaustion / a self-inconsistent record. */
struct facade_obj *
facade_obj_put_rec(
    struct facade_alloc             *alloc,
    const struct facade_obj_rec     *hdr,
    const struct facade_ckpt_objext *exts);

/* Serialize an object entry's scalar metadata into an on-media record header
 * (the caller serializes the extents from o->map via facade_extent_map_foreach). */
void
facade_obj_serialize_hdr(
    const struct facade_obj *o,
    struct facade_obj_rec   *hdr);

/* Remove an object entry: destroy its extent map and mark the slot free.  Does
 * NOT free its physical blocks (the caller returns owned blocks to the free set
 * first, so an in-flight release controls ordering). */
void
facade_obj_remove(
    struct facade_alloc *alloc,
    struct facade_obj   *o);

/* facade_ns.c: namespace record store helpers used by facade_alloc.c (checkpoint
 * (de)serialize, teardown) and facade_recover.c (log replay).  The contract-typed
 * transaction evaluator, scan and cursor table are in facade_ns.h. */

/* Total byte order over key strings: memcmp over the shorter, then by length. */
int
facade_ns_keycmp(
    const uint8_t *a,
    uint32_t       alen,
    const uint8_t *b,
    uint32_t       blen);

/* Find a live record by exact key, or NULL. */
struct facade_ns_rec *
facade_ns_find(
    struct facade_alloc *alloc,
    const uint8_t       *key,
    uint32_t             klen);

/* Lower-bound index of key in the sorted array; returns 1 and *r_idx = the match
 * when present, else 0 and *r_idx = the insertion point. */
int
facade_ns_lower_bound(
    struct facade_alloc *alloc,
    const uint8_t       *key,
    uint32_t             klen,
    uint32_t            *r_idx);

/* Install key->value at revision (create or replace), or remove key.  Used by
 * the transaction commit, log replay and checkpoint load, so the revision is
 * explicit.  put returns 0, or -1 on budget exhaustion / oom. */
int
facade_ns_apply_put(
    struct facade_alloc *alloc,
    const uint8_t       *key,
    uint32_t             klen,
    const uint8_t       *val,
    uint32_t             vlen,
    uint64_t             revision);
void
facade_ns_apply_delete(
    struct facade_alloc *alloc,
    const uint8_t       *key,
    uint32_t             klen);

/* Free every record and the array (teardown, and the reset before checkpoint
 * load). */
void
facade_ns_destroy_all(
    struct facade_alloc *alloc);

/* Checkpoint (de)serialize for the NS section (facade_super.h facade_ns_ckpt_rec
 * + key + value), after the object section.  _bytes sizes the section; _serialize
 * writes it and advances *pp; _load reads n_ns records between *pp and end,
 * returning 0 or -1 on a self-inconsistent record. */
uint64_t
facade_ns_ckpt_bytes(
    const struct facade_alloc *alloc);
void
facade_ns_ckpt_serialize(
    struct facade_alloc *alloc,
    uint8_t            **pp);
int
facade_ns_ckpt_load(
    struct facade_alloc *alloc,
    const uint8_t      **pp,
    const uint8_t       *end,
    uint64_t             n_ns);

/* The M3-scaled NS record budget for a space of nblocks blocks. */
uint32_t
facade_ns_budget_for(
    uint64_t nblocks);

/* Object-table iteration (epoch-held release scans it). */
uint32_t
facade_obj_slot_count(
    const struct facade_alloc *alloc);
struct facade_obj *
facade_obj_at(
    struct facade_alloc *alloc,
    uint32_t             slot);

/* Durable-object-transaction FIFO (facade_alloc.c).  acquire runs start(arg)
 * now if the slot is free, else queues it; release runs the next queued starter
 * (or clears the slot).  Every durable object transaction brackets itself with
 * these so only one runs at a time (real concurrency). */
void
facade_txn_acquire(
    struct facade_alloc *alloc,
    void (              *start )(void *arg),
    void                *arg);
void
facade_txn_release(
    struct facade_alloc *alloc);

/* facade_alloc.c: serialize a map's extents (facade_ckpt_objext records) into
 * *pp, advancing it.  Shared by the checkpoint and the log OBJ record so an
 * object's durable record is byte-identical in both. */
void
facade_ckpt_serialize_objext(
    const struct facade_extent_map *map,
    uint8_t                       **pp);

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
