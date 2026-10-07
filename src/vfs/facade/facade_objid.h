// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * Backend-facade object identity: opaque, immutable, 128-bit, never-reused
 * object ids (contract 4.1; the H2 fix).
 *
 * An id is a per-format random 64-bit instance nonce (stable across restarts
 * of the same formatted space) concatenated with a 64-bit monotonic sequence.
 * The sequence is drawn from an in-memory counter behind a persisted
 * high-water floor: the floor is written with force-unit-access AHEAD of
 * issuing, the counter resumes at the durable floor on open, so no sequence
 * value is ever reissued -- even after an unclean crash.  Adapted from
 * diskfs's inode-generation reserve-ahead allocator; see facade_objid.c.
 *
 * The identity state (nonce, counter, floor) lives in struct facade_alloc and
 * is persisted in the allocator superblock, so these calls take the allocator.
 */

#include "facade_alloc.h"   /* struct facade_alloc, enum facade_status, chimera_sc_obj_id */

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

/* Resume a parked id draw (see FACADE_AGAIN below). */
typedef void (*facade_resume_t)(
    void *arg);

/*
 * Draw the next never-reused 128-bit id into *r_id.
 *
 * Returns FACADE_OK synchronously on the common path.  Returns FACADE_AGAIN,
 * having parked resume(arg), only when the in-memory counter has reached the
 * durable floor and the floor extension is still in flight -- a rare burst
 * case (see the reachability note in facade_objid.c).  When the extension's
 * fua write completes, resume(arg) fires and the caller re-issues the draw,
 * which then succeeds against the advanced floor.  Parking (rather than
 * failing) is required: it preserves the never-reuse invariant without
 * spuriously failing a legitimate request.
 */
enum facade_status
facade_objid_next(
    struct facade_alloc      *alloc,
    struct chimera_sc_obj_id *r_id,
    facade_resume_t           resume,
    void                     *arg);

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
