// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * Chimera storage-contract conformance harness: the driver and the obligation
 * vocabulary.
 *
 * This is the reusable machinery a conformance suite is built from, not a
 * suite itself.  It is a CONSUMER of the contract (src/vfs/contract/,
 * chimera_sc_*): it attaches any backend descriptor, negotiates the version,
 * drives requests through the backend's asynchronous dispatch() on a libevpl
 * loop, and collects request->complete() callbacks.  The three sibling suites
 * (durability under power loss, transaction atomicity across a crash, the
 * error obligations) are separate tasks; each links this library and the
 * fixture library (sc_fixture.h) and lands as its own ctest executable of
 * cases, rather than reinventing a driver.
 *
 * Normative text.  The obligation primitives below check single, canonical
 * cases of rules whose full normative text lives in the contract DOCUMENTS,
 * not in the headers (chimera_sc.h:10-17 points at them).  Each primitive
 * names the doc section it enforces so a suite author lands on the normative
 * text before freezing a broader campaign:
 *   - D4 transaction atomicity:     docs/product/storage-contract.md s5.2 (D4)
 *   - D5 durability / epoch:        docs/product/storage-contract.md s7  (D5)
 *                                   + src/vfs/contract/sc_types.h:63-68
 *   - S11 returned-byte lifetime:   docs/product/storage-contract-interface.md
 *                                   s4 (S11) + src/vfs/contract/sc_request.h:24-29
 *   - held version survives a crash: storage-contract-interface.md s1 & s4 (S13)
 *                                   + src/vfs/contract/sc_types.h:249-251
 *   - section 11 error obligations: docs/product/storage-contract.md s11
 *                                   + src/vfs/contract/sc_error.h
 * The one-line "Harness obligations" list at sc_types.h:236-251 is a checklist,
 * not the acceptance text: a suite confirms the doc text before freezing.
 */

#include <stdint.h>
#include "chimera_sc.h"

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

struct evpl;

/* A case, an obligation primitive and the self-test all report one of these. */
enum chimera_sc_conf_result {
    CHIMERA_SC_CONF_PASS = 0,
    CHIMERA_SC_CONF_FAIL = 1,
    CHIMERA_SC_CONF_SKIP = 2,   /* a capability or fixture hook was absent */
};

/*
 * The backend-provided fixture-control interface: the abstract crash/recall
 * primitive the durability and error obligations drive, kept backend-agnostic.
 * A backend (the in-memory fixture here; E14's facade through
 * media-crash-simulation later) supplies the descriptor, the cfgdata that
 * selects its behaviour, and these OPTIONAL hooks.  The hooks act on the
 * backend's private_data (what init() returned); a NULL hook makes the cases
 * that need it SKIP rather than fail.
 */
struct chimera_sc_conformance_fixture {
    const char                      *name;
    const struct chimera_sc_backend *backend;
    const char                      *cfgdata;   /* selects the backend's profile */

    /* Simulate power loss: discard every write and namespace change that was
     * not made durable.  Returns 0 on success. */
    int                              (*crash)(
        void *backend_private);

    /* Power back on after crash(); the next ATTACH reflects survived state. */
    int                              (*recover)(
        void *backend_private);

    /* Corrupt an object's stored data so a verified read cannot return it
     * (D9).  Returns 0 on success, -1 if the object is unknown. */
    int                              (*corrupt)(
        void                           *backend_private,
        const struct chimera_sc_obj_id *id);

    /* Exhaust the backing store so the next allocation fails NO_SPACE (D13). */
    int                              (*fill)(
        void *backend_private);
};

/* Longest byte string the driver snapshots inside complete() (the S11-correct
 * copy of an NS_GET return). */
#define CHIMERA_SC_CONF_MAX_VALUE  256
#define CHIMERA_SC_CONF_PCLASS_CAP 8

/*
 * Harness context: owns the attachment to one backend on one evpl loop.
 * The evpl loop is supplied by the caller (so a suite can share one loop
 * across cases) and is not destroyed here.
 */
struct chimera_sc_conformance_ctx {
    struct evpl                                 *evpl;
    const struct chimera_sc_conformance_fixture *fixture;
    const struct chimera_sc_backend             *backend;
    void                                        *priv;             /* init() */
    void                                        *thread_private;   /* thread_init() */

    struct chimera_sc_node_id                    node;
    uint64_t                                     epoch;            /* current fence epoch */

    /* Cached ATTACH results. */
    struct chimera_sc_version                    version;
    uint64_t                                     caps;
    struct chimera_sc_limits                     limits;
    uint64_t                                     durability_epoch;
    int                                          attached;

    int                                          in_flight;        /* for drain() */

    /* Bounded-pump deadline in ns (monotonic). chimera_sc_conf_open sets a
     * generous default; a test may shorten it.  If the pump exceeds it without
     * the awaited completion, it logs "backend did not complete" and returns
     * so the obligation reports FAIL instead of hanging on a dropped completion. */
    uint64_t                                     pump_deadline_ns;

    struct chimera_sc_protection_class           pclass_buf[CHIMERA_SC_CONF_PCLASS_CAP];
    uint32_t                                     r_npclasses;

    /* Last obligation diagnostic, also written to stderr. */
    char                                         log[512];
    int                                          failed;
};

/*
 * One request in flight.  The driver's completion copies an NS_GET return into
 * ns_value INSIDE complete() (honouring S11), so a case reads the copy, not the
 * backend's buffer, and a backend that frees the buffer too early is caught.
 * Non-byte results stay in req after the call returns and are read from there.
 */
struct chimera_sc_conf_call {
    struct chimera_sc_conformance_ctx *ctx;
    struct chimera_sc_request          req;
    int                                done;

    uint8_t                            ns_value[CHIMERA_SC_CONF_MAX_VALUE];
    uint32_t                           ns_value_len;
    int                                ns_value_valid;

    /* Optional hook run at the end of complete(); may dispatch another call,
     * which exercises re-dispatch-from-complete. */
    void                               (*on_done)(
        struct chimera_sc_conf_call *call);
    void                              *user;
};

/* ---- lifecycle ---- */

/* ABI-gate, init, set_upcalls and thread_init the fixture's backend on evpl.
 * Returns 0, or -1 on an ABI-version mismatch or an init failure (the ABI gate
 * a consumer applies, as chimera_vfs_register() does for the module SDK). */
int
chimera_sc_conf_open(
    struct chimera_sc_conformance_ctx           *ctx,
    const struct chimera_sc_conformance_fixture *fixture,
    struct evpl                                 *evpl);

/* thread_destroy and destroy.  Safe on a ctx that never attached. */
void
chimera_sc_conf_close(
    struct chimera_sc_conformance_ctx *ctx);

/* Dispatch ATTACH with the wanted semantic version and cache the results.
 * Returns the request status; CHIMERA_SC_OK updates epoch/caps/limits.  Used
 * for the first attach and for re-attach after a crash. */
uint32_t
chimera_sc_conf_attach(
    struct chimera_sc_conformance_ctx *ctx,
    uint16_t                           want_major,
    uint16_t                           want_minor);

/* ---- requests ---- */

/* Zero a call, wire its request to this ctx (opcode, caller node + epoch,
 * completion), ready for the case to fill the op's union member. */
void
chimera_sc_conf_call_init(
    struct chimera_sc_conf_call       *call,
    struct chimera_sc_conformance_ctx *ctx,
    uint32_t                           opcode);

/* Dispatch without waiting (many may be in flight at once). */
void
chimera_sc_conf_submit(
    struct chimera_sc_conf_call *call);

/* Pump the loop until every submitted call on this ctx has completed, or the
* pump deadline is exceeded (then it logs and returns; callers detect the
* incomplete result via the still-stale request status / unset done flag). */
void
chimera_sc_conf_drain(
    struct chimera_sc_conformance_ctx *ctx);

/* Submit one call and pump until it completes (or the deadline is exceeded).
 * Do NOT use this for a call whose on_done re-dispatches another call: run
 * returns as soon as THIS call completes, before the re-dispatched one does.
 * Use chimera_sc_conf_submit + chimera_sc_conf_drain for that. */
void
chimera_sc_conf_run(
    struct chimera_sc_conf_call *call);

/* ---- diagnostics (used by the expect vocabulary) ---- */

void
chimera_sc_conf_logf(
    struct chimera_sc_conformance_ctx *ctx,
    const char                        *file,
    int                                line,
    const char                        *expr,
    const char                        *fmt,
    ...);

/* ---- runner ---- */

struct chimera_sc_conformance_case {
    const char *name;
    uint64_t    required_caps;                  /* r_caps bits; 0 = always */
    enum chimera_sc_conf_result (*run)(struct chimera_sc_conformance_ctx *ctx);
};

/* Open + attach the fixture, run one case (SKIP if its required caps are not
 * declared), then close.  evpl is borrowed.  Returns the case's result, or
 * FAIL if attach itself failed. */
enum chimera_sc_conf_result
chimera_sc_conformance_run_one(
    const struct chimera_sc_conformance_fixture *fixture,
    const struct chimera_sc_conformance_case    *tcase,
    struct evpl                                 *evpl);

/* ---- obligation primitives (the vocabulary suites consume) ----
 *
 * Each is a single canonical check of one contract obligation; a sibling suite
 * wraps these (across many crash points, randomised seeds and the full
 * obligation list) into its campaign.  A primitive returns SKIP when a fixture
 * hook it needs is absent. */

/* A create/write-durable/flush/read round trip and an NS_TXN put + NS_GET. */
enum chimera_sc_conf_result
chimera_sc_oblig_trivial_roundtrip(
    struct chimera_sc_conformance_ctx *ctx);

/* D5 + S13: a held object that took a DURABLE write survives crash + re-ATTACH. */
enum chimera_sc_conf_result
chimera_sc_oblig_durable_survives_crash(
    struct chimera_sc_conformance_ctx *ctx);

/* D4: the records of one NS_TXN are all present or all absent after a crash. */
enum chimera_sc_conf_result
chimera_sc_oblig_txn_atomic_across_crash(
    struct chimera_sc_conformance_ctx *ctx);

/* S11: an NS_GET return is valid through complete(); the driver copies it there
 * and the bytes must match what was written. */
enum chimera_sc_conf_result
chimera_sc_oblig_ns_get_byte_lifetime(
    struct chimera_sc_conformance_ctx *ctx);

/* D9 / section 11: a read of corrupted data returns DATA_UNRECOVERABLE, never
 * the bad bytes. */
enum chimera_sc_conf_result
chimera_sc_oblig_corrupt_is_unrecoverable(
    struct chimera_sc_conformance_ctx *ctx);

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
