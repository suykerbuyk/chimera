// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * Chimera media interface: the provider descriptor.
 *
 * A media provider fills one struct chimera_media_provider: a per-operation
 * vtable of named function pointers, the idiom libevpl's evpl_block and
 * Chimera's own VFS backends (struct chimera_vfs_module) use.  The media
 * layer has a handful of homogeneous, status-only operations, so it does not
 * carry the storage contract's tagged request union -- that exists for the
 * contract's many heterogeneous operations.  What it does keep is the
 * contract's registration discipline: a uint32_t abi_version the consumer
 * validates (chimera_media.h, chimera_media_open) before it trusts the
 * vtable.
 *
 * Asynchrony and threading (from the libevpl block layer this wraps):
 *   - Any operation that reaches media is asynchronous: its completion
 *     callback fires from a later iteration of the opening thread's event
 *     loop, never inline from the submitting call, including on a media
 *     failure.  No provider thread blocks the caller on media.
 *   - The exceptions are failures detected before any media access, which are
 *     reported through the completion callback synchronously, before the
 *     submitting call returns: a request rejected by argument validation -- a
 *     caller bug such as a block range outside the space
 *     (CHIMERA_MEDIA_E_RANGE), or a malformed request such as a null/empty
 *     iovec or an iovec total that is not nblocks * the block size
 *     (CHIMERA_MEDIA_E_INVAL) -- and submission-time resource exhaustion, when
 *     the provider cannot allocate the state to track the operation
 *     (CHIMERA_MEDIA_E_IO).  So everything that reaches media completes
 *     asynchronously, and a caller must be prepared for the callback to run
 *     either way.
 *   - A struct chimera_media_space is bound to the evpl thread that opened it;
 *     read/write/flush/close for a space run on that thread.  A provider that
 *     finishes work on another thread marshals the completion back.
 *   - Many operations may be in flight on a space, and the caller may submit
 *     a new one from inside a completion callback.
 *   - The iovecs a read or write names, and any bytes a read returns, are
 *     caller-owned until the completion fires; the provider borrows them for
 *     the duration of the operation and no longer.
 */

#include <stdint.h>
#include "media_types.h"
#include "media_error.h"

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

struct evpl;
struct evpl_iovec;
struct prometheus_metrics;

/* Completion for read/write/flush/close.  status is CHIMERA_MEDIA_OK or an
 * enum chimera_media_error. */
typedef void (*chimera_media_io_cb_t)(
    enum chimera_media_error status,
    void                    *private_data);

/* Completion for open.  On success status is CHIMERA_MEDIA_OK and space names
 * the opened space; on failure status is the error and space is NULL. */
typedef void (*chimera_media_open_cb_t)(
    enum chimera_media_error    status,
    struct chimera_media_space *space,
    void                       *private_data);

struct chimera_media_provider {
    /* Required.  CHIMERA_MEDIA_ABI_VERSION; a mismatch is refused at
     * chimera_media_open(). */
    uint32_t    abi_version;
    uint32_t    reserved;   /* must be zero */

    /* Required.  Short name of the provider implementation. */
    const char *name;

    /* Required.  Open a space on evpl's thread.  uri and the provider's own
     * configuration name the backing store; min_size is the smallest
     * acceptable size in bytes (a provider that creates the store sizes it to
     * at least this).  The space is reported through cb. */
    void        (*open)(
        struct evpl               *evpl,
        const char                *uri,
        uint64_t                   min_size,
        struct prometheus_metrics *metrics,
        chimera_media_open_cb_t    cb,
        void                      *private_data);

    /* Required.  Read nblocks starting at lba into iov; reports through cb. */
    void        (*read)(
        struct chimera_media_space *space,
        struct evpl_iovec          *iov,
        int                         niov,
        chimera_media_block_t       lba,
        uint32_t                    nblocks,
        chimera_media_io_cb_t       cb,
        void                       *private_data);

    /* Required.  Write nblocks starting at lba from iov.  fua != 0 requests
     * the data be durable before cb fires (maps to evpl_block_write's sync). */
    void        (*write)(
        struct chimera_media_space *space,
        const struct evpl_iovec    *iov,
        int                         niov,
        chimera_media_block_t       lba,
        uint32_t                    nblocks,
        int                         fua,
        chimera_media_io_cb_t       cb,
        void                       *private_data);

    /* Required.  Make all prior writes to the space durable. */
    void        (*flush)(
        struct chimera_media_space *space,
        chimera_media_io_cb_t       cb,
        void                       *private_data);

    /* Required.  Close the space.  cb fires once it is released. */
    void        (*close)(
        struct chimera_media_space *space,
        chimera_media_io_cb_t       cb,
        void                       *private_data);

    /* Required.  The size of the space in bytes; a cheap synchronous accessor
     * (nblocks = size_bytes() >> CHIMERA_MEDIA_BLOCK_SHIFT). */
    uint64_t    (*size_bytes)(
        struct chimera_media_space *space);

    /* Required.  The space's geometry, filled into *out: block_size, the block
     * count, and the largest single read/write in blocks.  Synchronous. */
    void        (*info)(
        struct chimera_media_space      *space,
        struct chimera_media_space_info *out);
};

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
