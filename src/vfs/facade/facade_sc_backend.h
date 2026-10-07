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

#include "sc_conformance.h"
#include "crashsim/chimera_media_crashsim.h"

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

/*
 * The media binding the backend opens the facade over.  Passed to the backend
 * through the fixture's cfgdata (a pointer, not a string, for this backend); it
 * must outlive the run.  The facade must already be formatted on `space`.
 */
struct facade_sc_bind {
    const struct chimera_media_provider         *provider; /* crash-sim provider */
    const struct chimera_media_crashsim_control *control;  /* crash/recover seam */
    struct chimera_media_space                  *space;     /* opened crash-sim space */
};

/* Fill fx with the facade-backed backend and the crash/recover hooks (which
 * drive the crash-sim control on bind->space).  bind must outlive the run. */
void
facade_sc_fixture_init(
    struct chimera_sc_conformance_fixture *fx,
    struct facade_sc_bind                 *bind);

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
