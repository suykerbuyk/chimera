// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * The minimal real storage-contract backend over the E14 facade
 * (media-log-and-recovery, decision #4 = EXPAND).  It implements exactly the
 * `chimera_sc_backend` op slice that E3's durability-under-power-loss obligation
 * (chimera_sc_oblig_durable_survives_crash) drives -- ATTACH, OBJ_CREATE, a
 * single-HOLD NS_TXN, OBJ_WRITE at DURABLE, OBJ_READ -- so the REAL
 * conformance-durability-power-loss suite runs green against the facade.
 *
 * It is a thin, real layer, NOT a throwaway shim: object-ops-v1 and
 * namespace-record-store EXTEND it (the full object lifecycle/states and the
 * first-class namespace records + multi-record transactions).  Every operation
 * is a switch inside the single async dispatch(), completing through the struct
 * evpl * from thread_init; crash/recover route to the crash-sim control seam.
 * Unimplemented opcodes return CHIMERA_SC_E_NOT_SUPPORTED.
 */

#include <stdint.h>

#include "sc_conformance.h"
#include "crashsim/chimera_media_crashsim.h"

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

/*
 * The media binding the backend opens the facade over.  Passed to the backend
 * through the fixture's cfgdata (a pointer, not a string, for this backend); it
 * must outlive the run.  The facade must already be formatted on `space`.
 *
 * MUST be constructed with facade_sc_bind_init() below -- never declared and
 * field-assigned directly.  The optional `now_ns`/`now_arg` clock seam has to
 * read NULL when unused, and a bare `struct facade_sc_bind b;` leaves it holding
 * uninitialised stack: under a Release build that garbage is typically non-NULL,
 * so fsb_init() installs it and the first non-ATTACH op's clock call segfaults
 * (a Debug build often hides it).  facade_sc_bind_init() zeroes the whole struct
 * and sets the required fields, so a caller that does not need a custom clock
 * never touches the optional half.
 */
struct facade_sc_bind {
    const struct chimera_media_provider         *provider; /* crash-sim provider */
    const struct chimera_media_crashsim_control *control;  /* crash/recover seam */
    struct chimera_media_space                  *space;     /* opened crash-sim space */

    /* Optional monotonic clock seam for the fence-epoch lease (6.5, D10, S14).
     * NULL -> the backend reads CLOCK_MONOTONIC.  A test injects a fake clock
     * to drive lease expiry deterministically, without sleeping.  When needed,
     * set it AFTER facade_sc_bind_init(). */
    uint64_t                                     (*now_ns)(
        void *arg);
    void                                        *now_arg;
};

/* Zero-initialise `b` and set the three required fields, leaving the optional
 * clock seam (now_ns/now_arg) NULL so the backend applies its CLOCK_MONOTONIC
 * default.  This is the ONLY supported way to construct a facade_sc_bind; a
 * caller needing a custom lease clock sets b->now_ns/now_arg afterward. */
void
facade_sc_bind_init(
    struct facade_sc_bind                       *b,
    const struct chimera_media_provider         *provider,
    const struct chimera_media_crashsim_control *control,
    struct chimera_media_space                  *space);

/* Fill fx with the facade-backed backend and the crash/recover hooks (which
 * drive the crash-sim control on bind->space).  bind must outlive the run. */
void
facade_sc_fixture_init(
    struct chimera_sc_conformance_fixture *fx,
    struct facade_sc_bind                 *bind);

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
