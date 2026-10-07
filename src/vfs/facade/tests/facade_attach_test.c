// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Attach / capabilities / single-node epoch lease, against the real facade
 * chimera_sc_backend over a crash-sim media provider on a loopback file
 * (backend-attach-capabilities).  Exit criteria: E3's harness attaches the
 * prototype; a version-mismatch fails at attach (E_VERSION); an expired epoch
 * is refused (E_FENCED).
 *
 * The fence-epoch lease is driven by an INJECTED MONOTONIC CLOCK (facade_sc_bind
 * now_ns/now_arg), not wall-clock, so expiry and renewal are deterministic with
 * no sleeps: the harness pump uses its own real CLOCK_MONOTONIC for its 10 s
 * completion deadline, which is independent of this lease clock.  Unprivileged
 * (the crash-sim delegate is the pread loopback on a sparse temp file).
 */

#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "evpl/evpl.h"

#include "crashsim/chimera_media_crashsim.h"
#include "loopback/media_loopback.h"
#include "facade_alloc.h"
#include "facade_sc_backend.h"
#include "sc_conformance.h"

#define ONE_GIB  (1ULL << 30)

/* The backend's advertised lease length (FSB_LEASE_NS in facade_sc_backend.c),
 * mirrored here on the lease clock the test controls. */
#define LEASE_NS 10000000000ULL

#define CHECK(cond)                                                      \
        do {                                                             \
            if (!(cond)) {                                               \
                fprintf(stderr, "CHECK failed: %s (%s:%d)\n",            \
                        # cond, __FILE__, __LINE__);                     \
                exit(1);                                                 \
            }                                                            \
        } while (0)

/* The injected lease clock: a value the test advances by hand. */
struct fake_clock {
    uint64_t now;
};

static uint64_t
fake_now_ns(void *arg)
{
    return ((const struct fake_clock *) arg)->now;
} /* fake_now_ns */

/* ---- media setup (as the durability test does) ---- */

struct open_wait {
    int                         done;
    enum chimera_media_error status;
    struct chimera_media_space *space;
};

static void
on_open(
    enum chimera_media_error    status,
    struct chimera_media_space *space,
    void                       *private_data)
{
    struct open_wait *w = private_data;

    w->status = status;
    w->space  = space;
    w->done   = 1;
} /* on_open */

struct fmt_wait {
    int done;
    enum facade_status st;
};

static void
on_fmt(
    enum facade_status st,
    void              *private_data)
{
    struct fmt_wait *w = private_data;

    w->st   = st;
    w->done = 1;
} /* on_fmt */

static void
on_mclose(
    enum chimera_media_error st,
    void                    *private_data)
{
    *(int *) private_data = (st == CHIMERA_MEDIA_OK);
} /* on_mclose */

static void
pump(
    struct evpl *evpl,
    const int   *done)
{
    while (!*done) {
        evpl_continue(evpl);
    }
} /* pump */

/* ---- op helpers over the public conformance driver ---- */

/* ATTACH exposing the full reply, and caching the granted epoch into the ctx so
 * later call_init() stamps it.  This is the harness call path (call_init + run),
 * driving the backend's real ATTACH. */
static uint32_t
do_attach(
    struct chimera_sc_conformance_ctx *ctx,
    uint16_t                           want_major,
    uint16_t                           want_minor,
    struct chimera_sc_attach_args     *out)
{
    struct chimera_sc_conf_call call;

    chimera_sc_conf_call_init(&call, ctx, CHIMERA_SC_OP_ATTACH);
    call.req.caller.fence_epoch  = 0;   /* ATTACH carries epoch 0 */
    call.req.attach.want.major   = want_major;
    call.req.attach.want.minor   = want_minor;
    call.req.attach.want.patch   = 0;
    call.req.attach.pclasses     = ctx->pclass_buf;
    call.req.attach.pclasses_cap = CHIMERA_SC_CONF_PCLASS_CAP;
    chimera_sc_conf_run(&call);

    if (out) {
        *out = call.req.attach;
    }
    if (call.req.status == CHIMERA_SC_OK) {
        ctx->epoch    = call.req.attach.r_fence_epoch;
        ctx->version  = call.req.attach.r_version;
        ctx->caps     = call.req.attach.r_caps;
        ctx->limits   = call.req.attach.r_limits;
        ctx->attached = 1;
    }
    return call.req.status;
} /* do_attach */

/* A canary non-ATTACH op: OBJ_CREATE under the ctx's current caller epoch.  Its
 * only job here is to be accepted when the lease is live and rejected (E_FENCED)
 * when it is not. */
static uint32_t
canary_create(struct chimera_sc_conformance_ctx *ctx)
{
    struct chimera_sc_conf_call call;

    chimera_sc_conf_call_init(&call, ctx, CHIMERA_SC_OP_OBJ_CREATE);
    chimera_sc_conf_run(&call);
    return call.req.status;
} /* canary_create */

/* OBJ_CREATE carrying a caller the test overrides (forged epoch or wrong node). */
static uint32_t
canary_create_as(
    struct chimera_sc_conformance_ctx *ctx,
    const struct chimera_sc_node_id   *node,
    uint64_t                           fence_epoch)
{
    struct chimera_sc_conf_call call;

    chimera_sc_conf_call_init(&call, ctx, CHIMERA_SC_OP_OBJ_CREATE);
    call.req.caller.node        = *node;
    call.req.caller.fence_epoch = fence_epoch;
    chimera_sc_conf_run(&call);
    return call.req.status;
} /* canary_create_as */

static uint32_t
do_renew(
    struct chimera_sc_conformance_ctx *ctx,
    uint64_t                          *r_lease_ns)
{
    struct chimera_sc_conf_call call;

    chimera_sc_conf_call_init(&call, ctx, CHIMERA_SC_OP_RENEW);
    chimera_sc_conf_run(&call);
    if (r_lease_ns) {
        *r_lease_ns = call.req.renew.r_lease_ns;
    }
    return call.req.status;
} /* do_renew */

static uint32_t
do_fence(
    struct chimera_sc_conformance_ctx *ctx,
    const struct chimera_sc_node_id   *target)
{
    struct chimera_sc_conf_call call;

    chimera_sc_conf_call_init(&call, ctx, CHIMERA_SC_OP_FENCE);
    call.req.fence.node = *target;
    chimera_sc_conf_run(&call);
    return call.req.status;
} /* do_fence */

static uint32_t
do_detach(struct chimera_sc_conformance_ctx *ctx)
{
    struct chimera_sc_conf_call call;

    chimera_sc_conf_call_init(&call, ctx, CHIMERA_SC_OP_DETACH);
    chimera_sc_conf_run(&call);
    return call.req.status;
} /* do_detach */

/* ---- cases ----
 *
 * Each case opens a fresh ctx (a fresh backend instance) over the shared,
 * already-formatted space, so the version-mismatch case starts un-attached, and
 * resets the lease clock to a known base so deadlines are predictable. */

static void
case_attach_honest_declaration(
    const struct chimera_sc_conformance_fixture *fx,
    struct evpl                                 *evpl,
    struct fake_clock                           *clk)
{
    struct chimera_sc_conformance_ctx ctx;
    struct chimera_sc_attach_args     a;

    clk->now = 0;
    CHECK(chimera_sc_conf_open(&ctx, fx, evpl) == 0);

    /* E3's harness attaches the prototype at the current contract version. */
    CHECK(do_attach(&ctx, CHIMERA_SC_CONTRACT_MAJOR, CHIMERA_SC_CONTRACT_MINOR,
                    &a) == CHIMERA_SC_OK);

    /* Version echoed back by the negotiation rule, expressed from the
     * constants (not a {1,0,0} literal): want==have, so major==want.major,
     * minor==min(want,have), patch==have.patch. */
    CHECK(a.r_version.major == CHIMERA_SC_CONTRACT_MAJOR);
    CHECK(a.r_version.minor == CHIMERA_SC_CONTRACT_MINOR);
    CHECK(a.r_version.patch == CHIMERA_SC_CONTRACT_PATCH);

    /* Honest zero declaration (O5): no optional capabilities, no protection
     * classes. */
    CHECK(a.r_caps == 0);
    CHECK(a.r_npclasses == 0);

    /* The epoch is a lease: a nonzero epoch and r_lease_ns at most
     * limits.lease_ns (here exactly, both FSB_LEASE_NS). */
    CHECK(a.r_fence_epoch != 0);
    CHECK(a.r_lease_ns == a.r_limits.lease_ns);
    CHECK(a.r_lease_ns == LEASE_NS);

    /* An op is accepted under the fresh live lease. */
    CHECK(canary_create(&ctx) == CHIMERA_SC_OK);

    chimera_sc_conf_close(&ctx);
} /* case_attach_honest_declaration */

static void
case_version_mismatch(
    const struct chimera_sc_conformance_fixture *fx,
    struct evpl                                 *evpl,
    struct fake_clock                           *clk)
{
    struct chimera_sc_conformance_ctx ctx;
    struct chimera_sc_attach_args     a;

    clk->now = 0;
    CHECK(chimera_sc_conf_open(&ctx, fx, evpl) == 0);

    /* A major mismatch refuses to attach (D12). */
    CHECK(do_attach(&ctx, CHIMERA_SC_CONTRACT_MAJOR + 1, 0, &a)
          == CHIMERA_SC_E_VERSION);

    /* No epoch was granted, so no op is accepted afterward. */
    CHECK(canary_create(&ctx) == CHIMERA_SC_E_FENCED);

    /* A minor above the backend's negotiates down to the backend's minor. */
    CHECK(do_attach(&ctx, CHIMERA_SC_CONTRACT_MAJOR,
                    (uint16_t) (CHIMERA_SC_CONTRACT_MINOR + 5), &a)
          == CHIMERA_SC_OK);
    CHECK(a.r_version.minor == CHIMERA_SC_CONTRACT_MINOR);

    chimera_sc_conf_close(&ctx);
} /* case_version_mismatch */

static void
case_renew_extends_lease(
    const struct chimera_sc_conformance_fixture *fx,
    struct evpl                                 *evpl,
    struct fake_clock                           *clk)
{
    struct chimera_sc_conformance_ctx ctx;
    struct chimera_sc_attach_args     a;
    uint64_t                          lease = 0;

    clk->now = 0;
    CHECK(chimera_sc_conf_open(&ctx, fx, evpl) == 0);
    CHECK(do_attach(&ctx, CHIMERA_SC_CONTRACT_MAJOR, CHIMERA_SC_CONTRACT_MINOR,
                    &a) == CHIMERA_SC_OK);   /* deadline = 0 + LEASE_NS */

    /* Just before the deadline: still live. */
    clk->now = LEASE_NS - 1;
    CHECK(canary_create(&ctx) == CHIMERA_SC_OK);

    /* RENEW replaces the lease from now: new deadline = (LEASE_NS-1)+LEASE_NS. */
    CHECK(do_renew(&ctx, &lease) == CHIMERA_SC_OK);
    CHECK(lease == LEASE_NS);

    /* Past the ORIGINAL deadline but before the renewed one: still live. */
    clk->now = LEASE_NS + 1000;
    CHECK(canary_create(&ctx) == CHIMERA_SC_OK);

    chimera_sc_conf_close(&ctx);
} /* case_renew_extends_lease */

static void
case_expired_epoch_refused(
    const struct chimera_sc_conformance_fixture *fx,
    struct evpl                                 *evpl,
    struct fake_clock                           *clk)
{
    struct chimera_sc_conformance_ctx ctx;
    struct chimera_sc_attach_args     a;
    uint64_t                          lease = 0;

    clk->now = 0;
    CHECK(chimera_sc_conf_open(&ctx, fx, evpl) == 0);
    CHECK(do_attach(&ctx, CHIMERA_SC_CONTRACT_MAJOR, CHIMERA_SC_CONTRACT_MINOR,
                    &a) == CHIMERA_SC_OK);   /* deadline = LEASE_NS */

    /* At the deadline (now < deadline is false): expired. */
    clk->now = LEASE_NS;
    CHECK(canary_create(&ctx) == CHIMERA_SC_E_FENCED);

    /* A lease is never revived: RENEW on an expired epoch is E_FENCED. */
    CHECK(do_renew(&ctx, &lease) == CHIMERA_SC_E_FENCED);
    CHECK(canary_create(&ctx) == CHIMERA_SC_E_FENCED);

    chimera_sc_conf_close(&ctx);
} /* case_expired_epoch_refused */

static void
case_forged_and_wrong_node_refused(
    const struct chimera_sc_conformance_fixture *fx,
    struct evpl                                 *evpl,
    struct fake_clock                           *clk)
{
    struct chimera_sc_conformance_ctx ctx;
    struct chimera_sc_attach_args     a;
    const struct chimera_sc_node_id   other = { { 0x22 } };
    uint64_t                          old_epoch;

    clk->now = 0;
    CHECK(chimera_sc_conf_open(&ctx, fx, evpl) == 0);
    CHECK(do_attach(&ctx, CHIMERA_SC_CONTRACT_MAJOR, CHIMERA_SC_CONTRACT_MINOR,
                    &a) == CHIMERA_SC_OK);
    old_epoch = ctx.epoch;

    /* A forged, stale epoch value is rejected. */
    CHECK(canary_create_as(&ctx, &ctx.node, ctx.epoch - 1)
          == CHIMERA_SC_E_FENCED);

    /* The right epoch from the wrong node is rejected (node-identity check). */
    CHECK(canary_create_as(&ctx, &other, ctx.epoch) == CHIMERA_SC_E_FENCED);

    /* The correct live epoch is still accepted. */
    CHECK(canary_create(&ctx) == CHIMERA_SC_OK);

    /* Re-attach grants a new epoch; the old one is now revoked. */
    CHECK(do_attach(&ctx, CHIMERA_SC_CONTRACT_MAJOR, CHIMERA_SC_CONTRACT_MINOR,
                    &a) == CHIMERA_SC_OK);
    CHECK(ctx.epoch != old_epoch);
    CHECK(canary_create_as(&ctx, &ctx.node, old_epoch) == CHIMERA_SC_E_FENCED);

    chimera_sc_conf_close(&ctx);
} /* case_forged_and_wrong_node_refused */

static void
case_fence_subset(
    const struct chimera_sc_conformance_fixture *fx,
    struct evpl                                 *evpl,
    struct fake_clock                           *clk)
{
    struct chimera_sc_conformance_ctx ctx;
    struct chimera_sc_attach_args     a;
    const struct chimera_sc_node_id   other = { { 0x22 } };

    clk->now = 0;
    CHECK(chimera_sc_conf_open(&ctx, fx, evpl) == 0);
    CHECK(do_attach(&ctx, CHIMERA_SC_CONTRACT_MAJOR, CHIMERA_SC_CONTRACT_MINOR,
                    &a) == CHIMERA_SC_OK);

    /* Fencing the caller's own node is E_INVALID (a node ends its own epoch
     * with DETACH), and does not disturb the live epoch. */
    CHECK(do_fence(&ctx, &ctx.node) == CHIMERA_SC_E_INVALID);

    /* Fencing any other node id completes OK (no other node holds a live
     * epoch), and still does not disturb the caller's epoch. */
    CHECK(do_fence(&ctx, &other) == CHIMERA_SC_OK);
    CHECK(canary_create(&ctx) == CHIMERA_SC_OK);

    /* FENCE while holding no live epoch is E_FENCED. */
    CHECK(do_detach(&ctx) == CHIMERA_SC_OK);
    CHECK(do_fence(&ctx, &other) == CHIMERA_SC_E_FENCED);

    chimera_sc_conf_close(&ctx);
} /* case_fence_subset */

static void
case_detach_ends_epoch(
    const struct chimera_sc_conformance_fixture *fx,
    struct evpl                                 *evpl,
    struct fake_clock                           *clk)
{
    struct chimera_sc_conformance_ctx ctx;
    struct chimera_sc_attach_args     a;

    clk->now = 0;
    CHECK(chimera_sc_conf_open(&ctx, fx, evpl) == 0);
    CHECK(do_attach(&ctx, CHIMERA_SC_CONTRACT_MAJOR, CHIMERA_SC_CONTRACT_MINOR,
                    &a) == CHIMERA_SC_OK);

    CHECK(canary_create(&ctx) == CHIMERA_SC_OK);
    CHECK(do_detach(&ctx) == CHIMERA_SC_OK);
    CHECK(canary_create(&ctx) == CHIMERA_SC_E_FENCED);

    chimera_sc_conf_close(&ctx);
} /* case_detach_ends_epoch */

static void
case_attach_nonzero_epoch_invalid(
    const struct chimera_sc_conformance_fixture *fx,
    struct evpl                                 *evpl,
    struct fake_clock                           *clk)
{
    struct chimera_sc_conformance_ctx ctx;
    struct chimera_sc_conf_call       call;

    clk->now = 0;
    CHECK(chimera_sc_conf_open(&ctx, fx, evpl) == 0);

    /* ATTACH carries fence_epoch 0 (contract 6.5); a nonzero epoch is a
     * malformed request -> E_INVALID, not a fencing failure.  (do_attach forces
     * 0, so this case builds the ATTACH by hand.) */
    chimera_sc_conf_call_init(&call, &ctx, CHIMERA_SC_OP_ATTACH);
    call.req.caller.fence_epoch  = 42;
    call.req.attach.want.major   = CHIMERA_SC_CONTRACT_MAJOR;
    call.req.attach.want.minor   = CHIMERA_SC_CONTRACT_MINOR;
    call.req.attach.want.patch   = 0;
    call.req.attach.pclasses     = ctx.pclass_buf;
    call.req.attach.pclasses_cap = CHIMERA_SC_CONF_PCLASS_CAP;
    chimera_sc_conf_run(&call);
    CHECK(call.req.status == CHIMERA_SC_E_INVALID);

    chimera_sc_conf_close(&ctx);
} /* case_attach_nonzero_epoch_invalid */

int
main(void)
{
    const char                                  *tmp = getenv("TMPDIR");
    char                                         dir[512];
    char                                         path[700];
    struct evpl                                 *evpl;
    struct chimera_media_crashsim               *handle;
    const struct chimera_media_provider         *provider;
    const struct chimera_media_crashsim_control *control;
    struct chimera_media_crashsim_config         cfg;
    struct open_wait                             ow   = { 0, CHIMERA_MEDIA_E_IO, NULL };
    struct fmt_wait                              fw   = { 0, FACADE_E_IO };
    struct facade_geometry                       geom = { 0 };
    struct facade_sc_bind                        bind;
    struct chimera_sc_conformance_fixture        fx;
    struct fake_clock                            clk = { 0 };
    int                                          ok  = 0;

    snprintf(dir, sizeof(dir), "%s/facade_attach_XXXXXX",
             (tmp && tmp[0]) ? tmp : "/tmp");
    CHECK(mkdtemp(dir) != NULL);
    snprintf(path, sizeof(path), "%s/dev.img", dir);

    evpl = evpl_create(NULL);
    CHECK(evpl != NULL);

    memset(&cfg, 0, sizeof(cfg));
    cfg.delegate = chimera_media_loopback();
    cfg.seed     = 0xC0FFEE;
    CHECK(chimera_media_crashsim_create(&cfg, &handle, &provider, &control) == 0);

    chimera_media_crashsim_open(handle, evpl, path, ONE_GIB, NULL, on_open, &ow);
    pump(evpl, &ow.done);
    CHECK(ow.status == CHIMERA_MEDIA_OK && ow.space != NULL);

    facade_alloc_format(provider, ow.space, &geom, on_fmt, &fw);
    pump(evpl, &fw.done);
    CHECK(fw.st == FACADE_OK);

    facade_sc_bind_init(&bind, provider, control, ow.space);
    bind.now_ns  = fake_now_ns;   /* injected lease clock, advanced by the cases */
    bind.now_arg = &clk;
    facade_sc_fixture_init(&fx, &bind);

    case_attach_honest_declaration(&fx, evpl, &clk);
    case_attach_nonzero_epoch_invalid(&fx, evpl, &clk);
    case_version_mismatch(&fx, evpl, &clk);
    case_renew_extends_lease(&fx, evpl, &clk);
    case_expired_epoch_refused(&fx, evpl, &clk);
    case_forged_and_wrong_node_refused(&fx, evpl, &clk);
    case_fence_subset(&fx, evpl, &clk);
    case_detach_ends_epoch(&fx, evpl, &clk);

    provider->close(ow.space, on_mclose, &ok);
    pump(evpl, &ok);
    CHECK(ok);
    chimera_media_crashsim_destroy(handle);
    CHECK(unlink(path) == 0);
    evpl_destroy(evpl);
    CHECK(rmdir(dir) == 0);

    printf("facade_attach_test: ok "
           "(attach/version-negotiation/capabilities/lease/renew/fence/detach)\n");
    return 0;
} /* main */
