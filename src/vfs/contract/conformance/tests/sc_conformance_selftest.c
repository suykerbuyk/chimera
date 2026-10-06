// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Self-test of the conformance harness: it proves the mechanism works, so the
 * sibling suites can be trusted to catch a real backend's faults.
 *
 * It establishes three things:
 *   1. The driver attaches, negotiates the version, and drives requests to
 *      completion over a real evpl loop, with many requests in flight and a
 *      request re-dispatched from inside complete().
 *   2. Against the honest in-memory fixture, every obligation primitive PASSES.
 *   3. Against a fixture running the matching cheat profile, each obligation
 *      primitive FAILS -- the capable-of-failing proof, without which a green
 *      suite would prove nothing.
 *
 * Built with -UNDEBUG so the assertions stay live in every build type.
 */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "evpl/evpl.h"

#include "sc_conformance.h"
#include "sc_fixture.h"

typedef enum chimera_sc_conf_result (*oblig_fn)(struct chimera_sc_conformance_ctx *);

/* Open and attach a fresh ctx against the given profile, run one obligation
 * primitive, close, and return its result. */
static enum chimera_sc_conf_result
run_oblig(
    struct evpl                    *evpl,
    enum chimera_sc_fixture_profile profile,
    oblig_fn                        fn)
{
    struct chimera_sc_conformance_fixture fx;
    struct chimera_sc_conformance_ctx     ctx;
    enum chimera_sc_conf_result           result;

    chimera_sc_fixture_init(&fx, profile);
    assert(chimera_sc_conf_open(&ctx, &fx, evpl) == 0);
    assert(chimera_sc_conf_attach(&ctx, CHIMERA_SC_CONTRACT_MAJOR,
                                  CHIMERA_SC_CONTRACT_MINOR) == CHIMERA_SC_OK);
    result = fn(&ctx);
    chimera_sc_conf_close(&ctx);
    return result;
} /* run_oblig */

/* ---- 1. driver mechanics ---- */

static void
test_negotiation(struct evpl *evpl)
{
    struct chimera_sc_conformance_fixture fx;
    struct chimera_sc_conformance_fixture badfx;
    struct chimera_sc_backend             bad;
    struct chimera_sc_conformance_ctx     ctx;
    uint32_t                              status;

    chimera_sc_fixture_init(&fx, CHIMERA_SC_FIXTURE_HONEST);

    /* The ABI gate refuses a backend built against a different C layout. */
    bad             = *fx.backend;
    bad.abi_version = CHIMERA_SC_ABI_VERSION + 1;
    badfx           = fx;
    badfx.backend   = &bad;
    assert(chimera_sc_conf_open(&ctx, &badfx, evpl) == -1);

    assert(chimera_sc_conf_open(&ctx, &fx, evpl) == 0);

    /* A contract-major mismatch refuses to attach. */
    status = chimera_sc_conf_attach(&ctx, CHIMERA_SC_CONTRACT_MAJOR + 1,
                                    CHIMERA_SC_CONTRACT_MINOR);
    assert(status == CHIMERA_SC_E_VERSION);

    /* The negotiated minor is the lower of the two; caps and limits report. */
    status = chimera_sc_conf_attach(&ctx, CHIMERA_SC_CONTRACT_MAJOR, 5);
    assert(status == CHIMERA_SC_OK);
    assert(ctx.version.major == CHIMERA_SC_CONTRACT_MAJOR);
    assert(ctx.version.minor == CHIMERA_SC_CONTRACT_MINOR);
    assert(ctx.caps == CHIMERA_SC_CAP_PROTECTION_CLASSES);
    assert(ctx.limits.max_txn_ops == 64);
    assert(ctx.r_npclasses == 1);

    chimera_sc_conf_close(&ctx);
    printf("  negotiation: ok\n");
} /* test_negotiation */

static void
test_optional_cap_not_at_attach(struct evpl *evpl)
{
    struct chimera_sc_conformance_fixture fx;
    struct chimera_sc_conformance_ctx     ctx;
    struct chimera_sc_conf_call           call;

    chimera_sc_fixture_init(&fx, CHIMERA_SC_FIXTURE_HONEST);
    assert(chimera_sc_conf_open(&ctx, &fx, evpl) == 0);
    assert(chimera_sc_conf_attach(&ctx, CHIMERA_SC_CONTRACT_MAJOR,
                                  CHIMERA_SC_CONTRACT_MINOR) == CHIMERA_SC_OK);

    /* A missing OPTIONAL capability is not an attach failure: it surfaces as
     * E_NOT_SUPPORTED at the op that needs it (here, a group recall). */
    chimera_sc_conf_call_init(&call, &ctx, CHIMERA_SC_OP_RECALL);
    call.req.recall.group = 42;
    chimera_sc_conf_run(&call);
    assert(call.req.status == CHIMERA_SC_E_NOT_SUPPORTED);

    chimera_sc_conf_close(&ctx);
    printf("  optional-cap-not-at-attach: ok\n");
} /* test_optional_cap_not_at_attach */

static enum chimera_sc_conf_result
case_needs_spokes(struct chimera_sc_conformance_ctx *ctx)
{
    (void) ctx;
    return CHIMERA_SC_CONF_PASS;   /* never reached: the cap is not declared */
} /* case_needs_spokes */

static void
test_cap_skip(struct evpl *evpl)
{
    struct chimera_sc_conformance_fixture    fx;
    const struct chimera_sc_conformance_case tcase = {
        "needs_spokes", CHIMERA_SC_CAP_SPOKES, case_needs_spokes
    };

    chimera_sc_fixture_init(&fx, CHIMERA_SC_FIXTURE_HONEST);
    assert(chimera_sc_conformance_run_one(&fx, &tcase, evpl) == CHIMERA_SC_CONF_SKIP);
    printf("  cap-skip: ok\n");
} /* test_cap_skip */

static void
test_many_in_flight(struct evpl *evpl)
{
    struct chimera_sc_conformance_fixture fx;
    struct chimera_sc_conformance_ctx     ctx;
    struct chimera_sc_conf_call           calls[8];
    int                                   i;

    chimera_sc_fixture_init(&fx, CHIMERA_SC_FIXTURE_HONEST);
    assert(chimera_sc_conf_open(&ctx, &fx, evpl) == 0);
    assert(chimera_sc_conf_attach(&ctx, CHIMERA_SC_CONTRACT_MAJOR,
                                  CHIMERA_SC_CONTRACT_MINOR) == CHIMERA_SC_OK);

    /* All submitted before any completes: the fixture defers them onto the
     * loop, so they are genuinely in flight at once. */
    for (i = 0; i < 8; i++) {
        chimera_sc_conf_call_init(&calls[i], &ctx, CHIMERA_SC_OP_RENEW);
        chimera_sc_conf_submit(&calls[i]);
    }
    chimera_sc_conf_drain(&ctx);
    for (i = 0; i < 8; i++) {
        assert(calls[i].done && calls[i].req.status == CHIMERA_SC_OK);
    }

    chimera_sc_conf_close(&ctx);
    printf("  many-in-flight: ok\n");
} /* test_many_in_flight */

static struct chimera_sc_conf_call g_redispatch_second;

static void
on_first_done(struct chimera_sc_conf_call *call)
{
    /* Dispatch a second request from inside the first's completion. */
    chimera_sc_conf_call_init(&g_redispatch_second, call->ctx, CHIMERA_SC_OP_RENEW);
    chimera_sc_conf_submit(&g_redispatch_second);
} /* on_first_done */

static void
test_redispatch_from_complete(struct evpl *evpl)
{
    struct chimera_sc_conformance_fixture fx;
    struct chimera_sc_conformance_ctx     ctx;
    struct chimera_sc_conf_call           first;

    chimera_sc_fixture_init(&fx, CHIMERA_SC_FIXTURE_HONEST);
    assert(chimera_sc_conf_open(&ctx, &fx, evpl) == 0);
    assert(chimera_sc_conf_attach(&ctx, CHIMERA_SC_CONTRACT_MAJOR,
                                  CHIMERA_SC_CONTRACT_MINOR) == CHIMERA_SC_OK);

    memset(&g_redispatch_second, 0, sizeof(g_redispatch_second));
    chimera_sc_conf_call_init(&first, &ctx, CHIMERA_SC_OP_RENEW);
    first.on_done = on_first_done;
    chimera_sc_conf_submit(&first);
    chimera_sc_conf_drain(&ctx);

    assert(first.done && first.req.status == CHIMERA_SC_OK);
    assert(g_redispatch_second.done &&
           g_redispatch_second.req.status == CHIMERA_SC_OK);

    chimera_sc_conf_close(&ctx);
    printf("  re-dispatch-from-complete: ok\n");
} /* test_redispatch_from_complete */

static void
test_pump_timeout(struct evpl *evpl)
{
    struct chimera_sc_conformance_fixture fx;
    struct chimera_sc_conformance_ctx     ctx;

    /* A backend that drops a completion -- the non-conformance the harness
     * exists to catch -- must make the obligation report FAIL, not hang.  The
     * capable-of-failing proof for the bounded pump (M1). */
    chimera_sc_fixture_init(&fx, CHIMERA_SC_FIXTURE_DROP_COMPLETION);
    assert(chimera_sc_conf_open(&ctx, &fx, evpl) == 0);
    assert(chimera_sc_conf_attach(&ctx, CHIMERA_SC_CONTRACT_MAJOR,
                                  CHIMERA_SC_CONTRACT_MINOR) == CHIMERA_SC_OK);

    /* The drop fixture never completes a non-ATTACH op; shorten the deadline so
     * the proof is fast. */
    ctx.pump_deadline_ns = 50000000ULL;   /* 50 ms */
    assert(chimera_sc_oblig_trivial_roundtrip(&ctx) == CHIMERA_SC_CONF_FAIL);

    chimera_sc_conf_close(&ctx);
    printf("  pump-timeout: ok (bounded pump reports FAIL, no hang)\n");
} /* test_pump_timeout */

/* ---- 2 + 3. obligations: honest passes, the matching cheat fails ---- */

static void
test_obligation(
    struct evpl                    *evpl,
    const char                     *name,
    oblig_fn                        fn,
    enum chimera_sc_fixture_profile cheat)
{
    assert(run_oblig(evpl, CHIMERA_SC_FIXTURE_HONEST, fn) == CHIMERA_SC_CONF_PASS);
    if (cheat != CHIMERA_SC_FIXTURE_HONEST) {
        assert(run_oblig(evpl, cheat, fn) == CHIMERA_SC_CONF_FAIL);
        printf("  obligation %s: honest PASS, cheat FAIL\n", name);
    } else {
        printf("  obligation %s: honest PASS (positive smoke test)\n", name);
    }
} /* test_obligation */

int
main(void)
{
    struct evpl *evpl;

    evpl = evpl_create(NULL);
    assert(evpl != NULL);

    printf("sc_conformance_selftest: driver mechanics\n");
    test_negotiation(evpl);
    test_optional_cap_not_at_attach(evpl);
    test_cap_skip(evpl);
    test_many_in_flight(evpl);
    test_redispatch_from_complete(evpl);
    test_pump_timeout(evpl);

    printf("sc_conformance_selftest: obligations (capable-of-failing)\n");
    test_obligation(evpl, "trivial_roundtrip",
                    chimera_sc_oblig_trivial_roundtrip,
                    CHIMERA_SC_FIXTURE_HONEST);
    test_obligation(evpl, "durable_survives_crash",
                    chimera_sc_oblig_durable_survives_crash,
                    CHIMERA_SC_FIXTURE_ACK_DURABLE_WITHOUT_PERSIST);
    test_obligation(evpl, "txn_atomic_across_crash",
                    chimera_sc_oblig_txn_atomic_across_crash,
                    CHIMERA_SC_FIXTURE_SPLIT_TXN);
    test_obligation(evpl, "ns_get_byte_lifetime",
                    chimera_sc_oblig_ns_get_byte_lifetime,
                    CHIMERA_SC_FIXTURE_S11_CHEAT);
    test_obligation(evpl, "corrupt_is_unrecoverable",
                    chimera_sc_oblig_corrupt_is_unrecoverable,
                    CHIMERA_SC_FIXTURE_RETURN_CORRUPT);

    evpl_destroy(evpl);

    printf("sc_conformance_selftest: ok\n");
    return 0;
} /* main */
