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

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
