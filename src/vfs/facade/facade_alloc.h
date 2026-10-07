// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * Backend-facade block allocator (E14 backend-facade-prototype).
 *
 * The allocator owns free space over one media address space
 * (src/vfs/media/chimera_media.h): it hands out runs of fixed 4 KiB blocks,
 * takes them back, reports pool-wide exhaustion as NO_SPACE (contract section
 * 11), and exposes when an allocation has been made durable (the H5 lesson)
 * WITHOUT ordering durability -- the ordering/recovery machinery is a later
 * unit (media-log-and-recovery).  Object identity lives alongside the
 * allocator (facade_objid.h): the id floor is persisted in the same
 * superblock.
 *
 * All media access is through the media provider vtable.  The allocator calls
 * no libevpl function: it builds the struct evpl_iovec for its own metadata
 * blocks over its own buffers (data + length only; the prototype's providers
 * -- loopback/PREAD and the test fixture -- read nothing else) and passes it
 * to provider->read / ->write / ->flush.  So this library links no evpl.
 *
 * Threading: an allocator is bound to the media space's evpl thread, like the
 * space itself.  The lifecycle calls (format/open/close/flush) and the id
 * floor extension are asynchronous and complete through their callbacks on
 * that thread; block alloc/free are synchronous in-memory operations.  One
 * metadata write is in flight at a time.
 */

#include <stdint.h>
#include "media_types.h"   /* chimera_media_block_t, CHIMERA_MEDIA_BLOCK_* */
#include "sc_types.h"      /* struct chimera_sc_obj_id (the 128-bit identity) */

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

struct chimera_media_provider;
struct chimera_media_space;
struct facade_alloc;

enum facade_status {
    FACADE_OK         = 0,
    FACADE_E_NO_SPACE = 1, /* allocation could not be satisfied (contract NO_SPACE) */
    FACADE_E_IO       = 2, /* media failure, or a corrupt/absent superblock */
    FACADE_E_INVAL    = 3, /* malformed request */
    FACADE_AGAIN      = 4, /* id draw parked; the resume callback will fire */
    FACADE_E_SHUTDOWN = 5, /* allocator is being torn down; the op is cancelled */
};

/* Durability of an allocation relative to the allocator's durable epoch (D5,
 * H5): BUFFERED data may be lost by a power cut, DURABLE data may not. */
enum facade_durability {
    FACADE_BUFFERED = 0,
    FACADE_DURABLE  = 1,
};

/* Format geometry.  v1 carries no options; reserved must be zero. */
struct facade_geometry {
    uint32_t reserved;
};

typedef void (*facade_cb_t)(
    enum facade_status status,
    void              *private_data);

typedef void (*facade_open_cb_t)(
    enum facade_status   status,
    struct facade_alloc *alloc,
    void                *private_data);

typedef void (*facade_epoch_cb_t)(
    enum facade_status status,
    uint64_t           durable_epoch,
    void              *private_data);

/* ---- lifecycle (asynchronous; all media I/O via the provider vtable) ---- */

/* Lay a fresh allocator onto an opened media space: whole data region free, a
 * random id nonce, id_floor seeded ahead.  Writes the checkpoint + superblock
 * (fua) and flushes, then reports through cb.  The space must already be open
 * (chimera_media_open). */
void
facade_alloc_format(
    const struct chimera_media_provider *provider,
    struct chimera_media_space          *space,
    const struct facade_geometry        *geom,
    facade_cb_t                          cb,
    void                                *private_data);

/* Open an existing allocator from a media space: validates the superblock,
 * loads the free-set checkpoint, and resumes the id sequence at the durable
 * floor (so no id is ever reissued, even after an unclean restart).  Reports
 * the allocator, or FACADE_E_IO for an absent/corrupt superblock. */
void
facade_alloc_open(
    const struct chimera_media_provider *provider,
    struct chimera_media_space          *space,
    facade_open_cb_t                     cb,
    void                                *private_data);

/* Persist the free set + superblock and release the allocator.  After cb the
 * handle is freed; the media space is the caller's to close. */
void
facade_alloc_close(
    struct facade_alloc *alloc,
    facade_cb_t          cb,
    void                *private_data);

/* Release the in-memory handle WITHOUT persisting anything (error teardown, or
 * a crash in a test).  Media keeps only what prior fua writes and flushes made
 * durable -- in particular the id floor, so a subsequent open never reissues an
 * id.  Synchronous; touches no media. */
void
facade_alloc_abandon(
    struct facade_alloc *alloc);

/* ---- block allocation (synchronous, in-memory) ---- */

/* Allocate nblocks contiguous blocks (best-fit).  FACADE_OK with *r_start, or
 * FACADE_E_NO_SPACE when no run is large enough.  *r_epoch (optional) is the
 * durability epoch this allocation is recorded in; compare it against
 * facade_alloc_durable_epoch() to learn BUFFERED vs DURABLE (H5). */
enum facade_status
facade_alloc_blocks(
    struct facade_alloc   *alloc,
    uint32_t               nblocks,
    chimera_media_block_t *r_start,
    uint64_t              *r_epoch);

/* Return a run to the free set (coalescing).  Aborts on a double-free or
 * overlap -- a caller-side invariant violation, not a runtime condition. */
void
facade_alloc_free(
    struct facade_alloc  *alloc,
    chimera_media_block_t start,
    uint32_t              nblocks);

/* Free / total allocatable blocks (statfs; the NO_SPACE accounting source). */
uint64_t
facade_alloc_free_blocks(
    const struct facade_alloc *alloc);

uint64_t
facade_alloc_total_blocks(
    const struct facade_alloc *alloc);

/* ---- durability surface: EXPOSE when durable, do not order (H5) ---- */

/* The highest epoch whose allocations are durable on media. */
uint64_t
facade_alloc_durable_epoch(
    const struct facade_alloc *alloc);

/* Is the allocation tagged with epoch durable yet? */
static inline enum facade_durability
facade_alloc_durability(
    const struct facade_alloc *alloc,
    uint64_t                   epoch)
{
    return epoch <= facade_alloc_durable_epoch(alloc)
           ? FACADE_DURABLE : FACADE_BUFFERED;
} /* facade_alloc_durability */

/* Persist the allocator state (free-set checkpoint + superblock, flushed) and
 * report the epoch now durable, covering every allocation issued before this
 * call.  The allocator never decides WHEN to flush relative to object data or
 * the namespace transaction -- that ordering is the caller's / a later unit's. */
void
facade_alloc_flush(
    struct facade_alloc *alloc,
    facade_epoch_cb_t    cb,
    void                *private_data);

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
