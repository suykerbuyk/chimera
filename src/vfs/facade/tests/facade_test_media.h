// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * In-RAM media provider for the facade allocator tests.
 *
 * Durability model (so the tests can actually prove the reserve-ahead-FUA and
 * the metadata-write serialization, not just a false green):
 *   - each opened space has a volatile VIEW, seeded from the backing's DURABLE
 *     image at open;
 *   - a write updates the view; a force-unit-access (fua) write also commits to
 *     durable immediately; a plain write stays volatile until a flush;
 *   - flush commits the whole view to durable;
 *   - the DURABLE image is per-uri and survives close/open, so reopening a uri
 *     on a fresh space is a CRASH view: it sees only fua'd + flushed bytes, and
 *     drops volatile writes that were never flushed.
 *
 * Completion model: by default completions fire inline.  With stall on, every
 * write/flush is queued with its effect PENDING (applied only when released),
 * so a test can hold one write in flight while issuing another and release the
 * completions in any order / on any block.  Reads always complete inline.
 *
 * Reads only {data,length} from each evpl_iovec (as the loopback/PREAD provider
 * does), so it links no evpl.
 */

#include "chimera_media.h"

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

/* The provider.  Pass to chimera_media_open with a NULL evpl. */
const struct chimera_media_provider *
facade_test_media(
    void);

/* Drop every backing buffer (call between independent test cases). */
void
facade_test_media_reset(
    void);

/* When on, write/flush completions are queued PENDING instead of firing inline;
 * their byte effects apply only on release. */
void
facade_test_media_stall_writes(
    struct chimera_media_space *space,
    int                         on);

/* Number of queued (pending) completions. */
int
facade_test_media_pending_count(
    struct chimera_media_space *space);

/* Back-compat: nonzero if any completion is pending. */
int
facade_test_media_write_pending(
    struct chimera_media_space *space);

/* Apply + fire the oldest pending completion (FIFO).  No-op if none. */
void
facade_test_media_release(
    struct chimera_media_space *space);

/* Apply + fire the oldest pending completion whose target block == lba.
 * Returns 1 if one was fired, 0 if none matched. */
int
facade_test_media_release_lba(
    struct chimera_media_space *space,
    uint64_t                    lba);

/* Drain: apply + fire pending completions until the queue is empty (new ones
 * enqueued by a fired completion are also drained). */
void
facade_test_media_release_all(
    struct chimera_media_space *space);

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
