// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * Backend-facade object lifecycle (object-ops-v1): the full contract 6.1 object
 * surface over the block allocator (facade_alloc.h), the per-object extent map
 * (facade_extent.h) and the durability log (facade_log.h).
 *
 * Every durable mutation (write, seal, clone, truncate, allocate, punch) is one
 * generalized {ALLOC*, FREE*, OBJ, COMMIT} log transaction -- the slice's
 * single-extent OBJ_WRITE(DURABLE) is now the first caller of this machinery --
 * serialized one at a time through the allocator's transaction FIFO (real
 * concurrency), honoring G1 (compact at the txn boundary, before any in-memory
 * allocation delta), G2 (the COMMIT is durable only after the data FUA) and the
 * no-reuse-of-a-freed-block-until-its-FREE-is-durable invariant (a freed owned
 * block is returned to the allocatable free set only when the COMMIT is
 * durable).  An object release (discard, epoch-held) is made durable by a forced
 * compaction rather than a log record, and is also GC'd at re-ATTACH, so it
 * never resurrects.
 *
 * 4.2 state rules: writable only while OPEN; OPEN -> SEALED on seal; a mutator
 * on a sealed object returns FACADE_E_STATE (mapped to CHIMERA_SC_E_STATE).
 *
 * The core links no libevpl: callers pass plain buffers; the contract adapter
 * (facade_sc_backend.c) marshals evpl iovecs around these calls.
 *
 * ---- data-verification (D9, on-media v5) ---------------------------------
 *
 * The backend verifies its own stored data and never returns bytes that fail
 * verification; unrecoverable data fails CHIMERA_SC_E_DATA_UNRECOVERABLE (the
 * prototype has no protection class, so there is no repair source).
 *
 *  - Format v5 (facade_super.h): each extent record (facade_ckpt_objext) carries
 *    a per-physical-block data checksum (data_csum + csum_algo), and the durable
 *    object record (facade_obj_rec) carries an object content hash (obj_csum +
 *    algo/len).  A v4 image is refused, not read as blank.  The grown OBJ record
 *    still fits one 4 KiB log block: hdr 48 + obj_rec 136 + 64 * objext 40 =
 *    2744 <= 4096; FACADE_LR_FORMAT is bumped so a v4-shaped log record is
 *    dropped, not mis-parsed.
 *
 *  - DATA => 1 block + CRC32 invariant.  A stored DATA extent is exactly one
 *    block and must carry a usable (FACADE_DATA_CSUM_CRC32) checksum; the write
 *    path computes it, and facade_obj_put_rec (the checkpoint/log reinsert)
 *    REFUSES a DATA extent that is multi-block or not CRC32-tagged, so a
 *    malformed/future image is rejected rather than read unverified.  Both read
 *    sites -- OBJ_READ (facade_read_done, before the memcpy) and the sub-block
 *    RMW pre-read (facade_write_after_read, before the merge) -- recompute the
 *    checksum and fail CLOSED.
 *
 *  - Object content MD5 at SEAL, in three steps (so the hash is stable without
 *    ever holding the one-slot txn FIFO across a full-object read):
 *      0. freeze: set o->sealing synchronously; every object-state writer then
 *         refuses the object -- the object-op entry rejects + the prepare
 *         chokepoint, the epoch-held/discard release sweep, and the NS-txn
 *         ns_evaluate reject (the writer set is complete);
 *      1. quiesce: one brief FIFO-slot barrier snapshots the DATA extent list +
 *         o->change, then releases;
 *      2. hash: an async, media-completion-driven callback chain over the
 *         snapshot (verify each block, crypto_update, read the next); a corrupt
 *         block aborts SEAL DATA_UNRECOVERABLE;
 *      3. commit: the seal txn rechecks o->change, stamps obj_csum + MD5 and
 *         flips OPEN->SEALED, with full rollback of the new fields; o->sealing is
 *         cleared on every exit (a failed seal stays OPEN and writable).
 *    o->sealing is in-memory only (never serialized), so a crash before the
 *    commit leaves an OPEN object with no digest.  The core links chimera_crypto
 *    (OpenSSL only -- NOT evpl) for the MD5.  OBJ_STAT reports the MD5 once
 *    SEALED, NONE while open/mid-seal.
 *
 *  - Scrub (facade_scrub): a backend/operator tool (NOT a contract op, O6) that
 *    verifies every stored DATA block and reports unrecoverable object ids.  It
 *    snapshots the DATA extents under one brief FIFO-slot barrier (no I/O under
 *    the slot), verifies OUT of the slot, and -- to avoid a false positive from a
 *    block freed+realloced mid-scrub -- rechecks a per-lba monotonic reuse stamp
 *    AFTER each read: changed => skip.  The stamp is an lba-indexed uint64_t
 *    array over the data region (not a free-run node field), bumped per-lba
 *    across the whole run in facade_alloc_free and facade_alloc_blocks;
 *    in-memory, not persisted.
 *
 *  - Corruption injection (fsb_corrupt, facade_sc_backend.c, for the E3
 *    conformance fixture): resolve id -> DATA phys (facade_obj_data_phys; STAT/MAP
 *    do not expose phys), DRAIN with a crash()+recover() cycle so the delegate
 *    image is authoritative (the crash-sim journal would otherwise mask a
 *    delegate corruption; recover() must run, else the next read is fenced), then
 *    flip bits in each DATA block.
 */

#include <stdint.h>

#include "facade_alloc.h"   /* enum facade_status, facade_cb_t, facade_epoch_cb_t */
#include "sc_types.h"       /* struct chimera_sc_stamps, struct chimera_sc_obj_meta */

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

struct facade_alloc;

/* OBJ_READ completion: status, the number of bytes placed in the caller buffer
 * (holes/unwritten read as zeros), and the object's logical size (for r_eof). */
typedef void (*facade_obj_read_cb_t)(
    enum facade_status status,
    uint32_t           length,
    uint64_t           obj_size,
    void              *private_data);

/*
 * OBJ_WRITE.  Writes len bytes (1..one block, within a single logical block --
 * no block straddle; the namespace layer splits larger writes) at byte offset
 * into object id, copy-on-write (a shared clone block is never overwritten in
 * place).  level is BUFFERED or DURABLE; the prototype makes both durable and
 * reports DURABLE through cb's epoch.  The object must exist and be OPEN.
 */
void
facade_obj_write(
    struct facade_alloc *alloc,
    const uint8_t        id[16],
    uint64_t             offset,
    const void          *data,
    uint32_t             len,
    uint32_t             level,
    facade_epoch_cb_t    cb,
    void                *private_data);

/* OBJ_READ: up to len bytes at byte offset into out (caller buffer >= len). */
void
facade_obj_read(
    struct facade_alloc *alloc,
    const uint8_t        id[16],
    uint64_t             offset,
    uint32_t             len,
    void                *out,
    facade_obj_read_cb_t cb,
    void                *private_data);

/* OBJ_FLUSH: make prior writes durable.  The prototype writes through, so this
 * reports the current durability generation. */
void
facade_obj_flush(
    struct facade_alloc *alloc,
    const uint8_t        id[16],
    facade_epoch_cb_t    cb,
    void                *private_data);

/* OBJ_SEAL: OPEN -> SEALED (durable).  E_STATE if not OPEN. */
void
facade_obj_seal(
    struct facade_alloc *alloc,
    const uint8_t        id[16],
    facade_epoch_cb_t    cb,
    void                *private_data);

/* OBJ_TRUNCATE: set the logical size to new_size (durable).  Shrinking frees the
 * owned blocks past new_size; growing leaves a hole.  OPEN only. */
void
facade_obj_truncate(
    struct facade_alloc *alloc,
    const uint8_t        id[16],
    uint64_t             new_size,
    facade_epoch_cb_t    cb,
    void                *private_data);

/* OBJ_ALLOCATE: reserve [offset, offset+length) as UNWRITTEN (reads as zeros
 * until written).  OPEN only. */
void
facade_obj_allocate(
    struct facade_alloc *alloc,
    const uint8_t        id[16],
    uint64_t             offset,
    uint64_t             length,
    facade_epoch_cb_t    cb,
    void                *private_data);

/* OBJ_PUNCH: deallocate [offset, offset+length) (it becomes a hole), freeing
 * the owned blocks.  OPEN only. */
void
facade_obj_punch(
    struct facade_alloc *alloc,
    const uint8_t        id[16],
    uint64_t             offset,
    uint64_t             length,
    facade_epoch_cb_t    cb,
    void                *private_data);

/*
 * OBJ_CLONE_WORKING (D2): new_id (drawn by the caller, which handles the
 * FACADE_AGAIN park) becomes a new OPEN object whose extent map is a
 * metadata-only (borrowed) copy of the sealed origin's -- same physical blocks,
 * no data copy -- inheriting the origin's stamps and retention.  Durable.  The
 * origin must be SEALED (else FACADE_E_STATE) and present (else E_NOT_FOUND).
 */
void
facade_obj_clone_working(
    struct facade_alloc            *alloc,
    const uint8_t                   sealed_id[16],
    const struct chimera_sc_obj_id *new_id,
    facade_epoch_cb_t               cb,
    void                           *private_data);

/* OBJ_STAT (5.1): fill r_meta from the object record.  Synchronous (all state is
 * in memory).  Returns 0, or -1 if the object is unknown. */
int
facade_obj_stat(
    struct facade_alloc        *alloc,
    const uint8_t               id[16],
    struct chimera_sc_obj_meta *r_meta);

/* OBJ_MAP (S3, O12): enumerate [offset, offset+length) as DATA/HOLE/UNWRITTEN
 * byte segments into segs[0..segs_cap).  Synchronous.  *r_nsegs set; *r_more
 * nonzero when the range did not fit.  Returns 0, or -1 if unknown. */
int
facade_obj_map(
    struct facade_alloc       *alloc,
    const uint8_t              id[16],
    uint64_t                   offset,
    uint64_t                   length,
    struct chimera_sc_segment *segs,
    uint32_t                   segs_cap,
    uint32_t                  *r_nsegs,
    uint32_t                  *r_more);

/*
 * Release an OPEN && !HELD (epoch-held) object: remove it, free its owned
 * blocks, made durable by a forced compaction.  FACADE_E_STATE if the object is
 * sealed or held; FACADE_E_NOT_FOUND if unknown.  (OBJ_DISCARD.)
 */
void
facade_obj_discard(
    struct facade_alloc *alloc,
    const uint8_t        id[16],
    facade_cb_t          cb,
    void                *private_data);

/*
 * Release EVERY OPEN && !HELD object (an epoch ended: DETACH/FENCE/lease
 * expiry/re-ATTACH).  Removes them, frees their owned blocks, and forces one
 * compaction so the release is durable.  Completes OK even when none exist.
 */
void
facade_obj_release_epoch_held(
    struct facade_alloc *alloc,
    facade_cb_t          cb,
    void                *private_data);

/*
 * Scrub (data-verification, D9): walk the object table, verify every stored DATA
 * block against its per-block checksum, and report the objects that cannot be
 * recovered.  A backend/operator tool, NOT a Chimera-facing contract op (O6:
 * Chimera consumes storage and does not verify it); surfaced through the facade's
 * public API for the sc_backend adapter / an operator control seam.  It repairs
 * nothing.
 *
 * Concurrency: one brief metadata-only snapshot under the FIFO slot (DATA extents
 * only, with each block's reuse stamp), then media reads OUT of the slot; each
 * block's reuse stamp is rechecked AFTER its read, so a block freed+realloced
 * since the snapshot is skipped rather than false-reported.
 *
 * `report` fires once per unrecoverable object, with its first bad logical block.
 * `done` fires once at the end with the count of unrecoverable objects.
 */
typedef void (*facade_scrub_report_cb)(
    const uint8_t obj_id[16],
    uint64_t      first_bad_logical_block,
    void         *private_data);

typedef void (*facade_scrub_done_cb)(
    enum facade_status status,
    uint32_t           n_unrecoverable,
    void              *private_data);

void
facade_scrub(
    struct facade_alloc   *alloc,
    facade_scrub_report_cb report,
    facade_scrub_done_cb   done,
    void                  *private_data);

/*
 * Resolve object `id`'s stored DATA blocks to their physical lbas (one block per
 * DATA extent), in logical order, into out[0..cap).  Returns the number written
 * (<= cap), or 0 if the object is unknown or has no DATA blocks.  Used by the E3
 * conformance fixture's corrupt hook (OBJ_STAT/OBJ_MAP do not expose phys).
 */
uint32_t
facade_obj_data_phys(
    struct facade_alloc   *alloc,
    const uint8_t          id[16],
    chimera_media_block_t *out,
    uint32_t               cap);

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
