// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * The crash-simulating media provider: a decorator over another media provider
 * (the delegate, e.g. the loopback-file provider) for the E14 backend-facade
 * prototype.
 *
 * It fills the same struct chimera_media_provider the facade already consumes
 * (chimera_media.h), so the facade swaps chimera_media_loopback() for this with
 * no call-site change, and adds a small control interface the facade re-exports
 * to E3's conformance harness as its crash and corruption hooks.
 *
 * How a crash is made constructible, not retro-active: every non-durable write
 * (fua == 0, since the last flush) is staged in this layer's own memory and is
 * NOT passed to the delegate.  Only force-unit-access writes and flush drain
 * staged data to the delegate.  So the delegate always holds exactly what would
 * survive a power loss, and crash() is modelled by choosing -- under a seeded
 * policy -- which staged writes drain and which are lost or torn.  Reads merge
 * staged data over the delegate (newest wins), so a running facade still sees
 * its own buffered writes; after crash()+recover() staging is empty and reads
 * come straight from the delegate's post-crash image.
 *
 * Durability faithfulness (contract D5): a write the backend made durable must
 * survive.  FUA and flush-drained records are NEVER dropped or torn by a crash;
 * only not-yet-durable staged writes are.  Corruption (D9) flips bits in the
 * delegate's stored data so the facade's own verification fails and the read
 * returns DATA_UNRECOVERABLE -- this layer stores no checksums and does not
 * verify; verification belongs to the facade above.
 */

#include <stdint.h>
#include "chimera_media.h"

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

struct evpl;

/* Opaque handle: owns the delegate binding and the crash policy.  One handle
 * may open more than one space (the media multi-space model). */
struct chimera_media_crashsim;

/*
 * Crash policy.  w_keep / w_drop / w_tear are the relative weights of the three
 * per-write outcomes a crash draws for each not-yet-durable write record:
 *   - KEEP: the write survived the power loss intact;
 *   - DROP: the write was lost;
 *   - TEAR: the write landed only partially (a block prefix, with a partial
 *           final block), modelling a torn write across the power cut.
 * All three zero selects a sane default (KEEP-biased with occasional DROP, no
 * tear).  The replay-ordering hazard (a later write surviving while an earlier
 * one it depended on is lost) is modelled directly by per-record DROP leaving
 * gaps in submission order.
 *
 * reorder is reserved (currently ignored).  A crash's result is a single final
 * image, in which the order writes were persisted is not observable for
 * distinct blocks, and for the same block the newest surviving write must win
 * (a durable write is never clobbered by an older one -- D5); so permuting the
 * drain order has no correct observable effect here.  The field is retained for
 * a future model that injects a crash mid-drain, where partial-order would
 * become observable.
 */
struct chimera_media_crashsim_config {
    const struct chimera_media_provider *delegate;
    uint64_t                             seed;
    uint32_t                             w_keep;
    uint32_t                             w_drop;
    uint32_t                             w_tear;
    uint8_t                              reorder;    /* reserved; see above */
    uint8_t                              reserved[7];
};

/* Observable counters, for a test to assert determinism and policy effect.
 * last_crash_* describe the most recent crash() call. */
struct chimera_media_crashsim_stats {
    uint64_t writes_total;       /* writes submitted since open */
    uint64_t staged_current;     /* not-yet-durable records currently staged */
    uint64_t last_crash_kept;    /* records drained intact by the last crash */
    uint64_t last_crash_dropped; /* records lost by the last crash */
    uint64_t last_crash_torn;    /* records torn by the last crash */
};

/*
 * The control seam E14's facade re-exports to E3's conformance fixture
 * (struct chimera_sc_conformance_fixture: crash / recover / corrupt).  Each op
 * is keyed on the crash-sim space the facade holds and runs on that space's
 * evpl thread.  The ops are synchronous from the caller's view: any delegate
 * I/O they issue is pumped to completion internally before they return, so a
 * facade adapter can return the fixture hook's int result directly.  (The
 * media vtable's flush, by contrast, is asynchronous.)
 */
struct chimera_media_crashsim_control {
    /* Simulate power loss on `space`: apply the seeded policy to every write
    * staged since the last durability point, draining the surviving (possibly
    * torn) subset -- together with the durable records, re-asserted in
    * submission order so a durable write is never clobbered by an older staged
    * one -- to the delegate, and discard the rest.  Afterwards the delegate
    * holds exactly the post-crash image and staging is empty.  Returns 0. */
    int  (*crash)(
        struct chimera_media_space *space);

    /* Power back on after crash(): a logical reset.  The loopback delegate's
     * file persists across the simulated power cycle, so reads now serve the
     * delegate's post-crash image.  Returns 0. */
    int  (*recover)(
        struct chimera_media_space *space);

    /* Flip bits in the STORED (delegate) data of [lba, lba + nblocks) so the
     * facade's verification fails and a read returns DATA_UNRECOVERABLE (D9).
     * Bit positions are drawn from the PRNG.  Does not touch staging.  Returns
     * 0, or -1 if the range is outside the space. */
    int  (*corrupt)(
        struct chimera_media_space *space,
        chimera_media_block_t       lba,
        uint32_t                    nblocks);

    /* Reset the PRNG to `seed` and clear crash history, for replay or a seed
     * sweep.  Does not touch staged data or the delegate. */
    void (*reseed)(
        struct chimera_media_space *space,
        uint64_t                    seed);

    /* Snapshot the counters into *out. */
    void (*stats)(
        struct chimera_media_space          *space,
        struct chimera_media_crashsim_stats *out);
};

/*
 * Build a crash-sim handle binding cfg->delegate and the policy.  On success
 * returns 0 and fills the out-parameters: *handle_out (own it, destroy with
 * chimera_media_crashsim_destroy), *provider_out (use for read/write/flush/
 * close/info/size_bytes, exactly like any media provider), and *control_out
 * (use for crash/recover/corrupt/reseed/stats).  Returns -1 if cfg, its
 * delegate or handle_out is NULL, or the delegate's abi_version does not match
 * this build (the same gate chimera_media_open applies).
 *
 * handle_out is required (it owns the allocation the caller must later destroy,
 * and chimera_media_crashsim_open needs it); provider_out and control_out may
 * be NULL if the caller does not need them.
 */
int
chimera_media_crashsim_create(
    const struct chimera_media_crashsim_config   *cfg,
    struct chimera_media_crashsim               **handle_out,
    const struct chimera_media_provider         **provider_out,
    const struct chimera_media_crashsim_control **control_out);

/*
 * Open the wrapped space: opens the delegate under uri / min_size and wraps the
 * space it reports.  The crash-sim space is reported through cb (the ordinary
 * chimera_media_open_cb_t).  Must run on the evpl thread that will own the
 * space; all later I/O uses the provider vtable from chimera_media_crashsim_create.
 *
 * (Construction binds a delegate, which the fixed provider->open signature
 * cannot carry, so opening is this typed entry rather than the generic
 * chimera_media_open -- the same way the loopback provider is obtained by its
 * own call.)
 */
void
chimera_media_crashsim_open(
    struct chimera_media_crashsim *handle,
    struct evpl                   *evpl,
    const char                    *uri,
    uint64_t                       min_size,
    struct prometheus_metrics     *metrics,
    chimera_media_open_cb_t        cb,
    void                          *private_data);

/* Free the handle.  Spaces opened through it must already be closed. */
void
chimera_media_crashsim_destroy(
    struct chimera_media_crashsim *handle);

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
