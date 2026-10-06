// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * An in-memory backend that implements enough of the storage contract for the
 * harness to drive, and the siblings to get a green baseline against, before
 * E14's facade exists.  It is the harness's self-test double, NOT the product
 * backend.
 *
 * Two deliberate properties, both required by the review of the plan:
 *   - It completes DEFERRED on the evpl loop (through an evpl_deferral marshal),
 *     never synchronously inside dispatch() like the null stub, so the harness
 *     genuinely exercises the evpl_continue pump, many requests in flight and
 *     re-dispatch from inside complete().
 *   - Its behaviour is chosen by a cheat profile selected through init(cfgdata).
 *     The honest profile obeys the contract; each cheat violates exactly one
 *     obligation, so a suite can prove it is CAPABLE OF FAILING.
 *
 * It also supplies the fixture-control hooks (crash/recover/corrupt/fill) that
 * back the abstract crash primitive the durability and error obligations drive.
 */

#include "sc_conformance.h"

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

enum chimera_sc_fixture_profile {
    CHIMERA_SC_FIXTURE_HONEST = 0,
    /* OBJ_WRITE at DURABLE is acknowledged but not persisted: a crash loses it
     * (violates D5). */
    CHIMERA_SC_FIXTURE_ACK_DURABLE_WITHOUT_PERSIST,
    /* The last put of a multi-put NS_TXN is not made durable: a crash tears the
     * transaction (violates D4). */
    CHIMERA_SC_FIXTURE_SPLIT_TXN,
    /* A read of corrupted data returns the bad bytes instead of
     * DATA_UNRECOVERABLE (violates D9 / section 11). */
    CHIMERA_SC_FIXTURE_RETURN_CORRUPT,
    /* An NS_GET return buffer is overwritten before complete() returns
     * (violates S11). */
    CHIMERA_SC_FIXTURE_S11_CHEAT,
    /* Every non-ATTACH request is dropped: it is never completed.  Used to
     * prove the harness's bounded pump reports FAIL instead of hanging. */
    CHIMERA_SC_FIXTURE_DROP_COMPLETION,
};

/* Fill fx with the in-memory backend, the cfgdata that selects the profile,
 * and the fixture-control hooks. */
void
chimera_sc_fixture_init(
    struct chimera_sc_conformance_fixture *fx,
    enum chimera_sc_fixture_profile        profile);

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
