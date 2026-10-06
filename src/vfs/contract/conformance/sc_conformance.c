// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * The conformance harness driver and obligation primitives.  See
 * sc_conformance.h for the design and the doc citations each primitive enforces.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "evpl/evpl.h"

#include "common/macros.h"
#include "sc_conformance.h"
#include "sc_conformance_expect.h"

/* A deterministic node identity for the harness's single node. */
static const struct chimera_sc_node_id conf_node = { { 0x11 } };

/* Default bounded-pump deadline: 10 s is orders of magnitude above a
 * legitimate in-memory or loopback completion, so it never trips a conformant
 * backend, yet it turns a dropped completion into a reported FAIL, not a hang. */
#define CHIMERA_SC_CONF_PUMP_DEADLINE_NS 10000000000ULL

/* Heartbeat interval for the bounded pump.  evpl_continue() blocks when the
 * loop has no other pending work; a periodic timer keeps it returning at least
 * this often so the deadline below is actually checked.  A one-shot timer would
 * not do: it fires inside evpl_continue() and is removed, after which the same
 * evpl_continue() call blocks again with no timer left. */
#define CHIMERA_SC_CONF_PUMP_TICK_US     5000

static uint64_t
conf_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
} /* conf_now_ns */

static void
conf_tick_cb(
    struct evpl       *evpl,
    struct evpl_timer *timer)
{
    /* Only to wake the loop so the deadline is re-checked. */
    (void) evpl;
    (void) timer;
} /* conf_tick_cb */

/* ---- completion ---- */

static void
conf_on_complete(struct chimera_sc_request *request)
{
    struct chimera_sc_conf_call *call = request->caller_private;

    /* S11: an NS_GET return is valid only until complete() returns, so copy
     * what we keep here, inside the callback.  A backend that frees or
     * overwrites the buffer before this runs is caught: the copy is wrong. */
    if (request->opcode == CHIMERA_SC_OP_NS_GET &&
        request->status == CHIMERA_SC_OK) {
        uint32_t n = request->ns_get.r_value.len;

        if (n > CHIMERA_SC_CONF_MAX_VALUE) {
            n = CHIMERA_SC_CONF_MAX_VALUE;
        }
        if (n && request->ns_get.r_value.data) {
            memcpy(call->ns_value, request->ns_get.r_value.data, n);
        }
        call->ns_value_len   = n;
        call->ns_value_valid = 1;
    }

    call->done = 1;
    call->ctx->in_flight--;

    if (call->on_done) {
        call->on_done(call);
    }
} /* conf_on_complete */

/* ---- lifecycle ---- */

SYMBOL_EXPORT int
chimera_sc_conf_open(
    struct chimera_sc_conformance_ctx           *ctx,
    const struct chimera_sc_conformance_fixture *fixture,
    struct evpl                                 *evpl)
{
    static const struct chimera_sc_upcalls upcalls = { NULL };

    memset(ctx, 0, sizeof(*ctx));

    if (!fixture || !fixture->backend) {
        return -1;
    }

    /* The ABI gate: refuse a backend built against a different C layout, as
     * chimera_vfs_register() does for the module SDK. */
    if (fixture->backend->abi_version != CHIMERA_SC_ABI_VERSION) {
        return -1;
    }

    ctx->fixture          = fixture;
    ctx->backend          = fixture->backend;
    ctx->evpl             = evpl;
    ctx->node             = conf_node;
    ctx->pump_deadline_ns = CHIMERA_SC_CONF_PUMP_DEADLINE_NS;

    ctx->priv = ctx->backend->init(fixture->cfgdata ? fixture->cfgdata : "", NULL);
    if (!ctx->priv) {
        return -1;
    }

    ctx->backend->set_upcalls(ctx->priv, &upcalls, NULL);
    ctx->thread_private = ctx->backend->thread_init(evpl, ctx->priv);
    if (!ctx->thread_private) {
        /* A backend whose thread_init fails would NULL-deref at first dispatch;
         * tear down and refuse the attach instead. */
        ctx->backend->destroy(ctx->priv);
        ctx->priv = NULL;
        return -1;
    }
    return 0;
} /* chimera_sc_conf_open */

SYMBOL_EXPORT void
chimera_sc_conf_close(struct chimera_sc_conformance_ctx *ctx)
{
    if (ctx->thread_private) {
        ctx->backend->thread_destroy(ctx->thread_private);
        ctx->thread_private = NULL;
    }
    if (ctx->priv) {
        ctx->backend->destroy(ctx->priv);
        ctx->priv = NULL;
    }
} /* chimera_sc_conf_close */

/* ---- requests ---- */

SYMBOL_EXPORT void
chimera_sc_conf_call_init(
    struct chimera_sc_conf_call       *call,
    struct chimera_sc_conformance_ctx *ctx,
    uint32_t                           opcode)
{
    memset(call, 0, sizeof(*call));
    call->ctx                    = ctx;
    call->req.opcode             = opcode;
    call->req.status             = CHIMERA_SC_E_INVALID;
    call->req.caller.node        = ctx->node;
    call->req.caller.fence_epoch = ctx->epoch;
    call->req.complete           = conf_on_complete;
    call->req.caller_private     = call;
} /* chimera_sc_conf_call_init */

SYMBOL_EXPORT void
chimera_sc_conf_submit(struct chimera_sc_conf_call *call)
{
    call->done = 0;
    call->ctx->in_flight++;
    call->ctx->backend->dispatch(&call->req, call->ctx->thread_private);
} /* chimera_sc_conf_submit */

SYMBOL_EXPORT void
chimera_sc_conf_drain(struct chimera_sc_conformance_ctx *ctx)
{
    struct evpl_timer tick;
    uint64_t          deadline = conf_now_ns() + ctx->pump_deadline_ns;

    evpl_add_timer(ctx->evpl, &tick, conf_tick_cb, CHIMERA_SC_CONF_PUMP_TICK_US);
    while (ctx->in_flight > 0 && conf_now_ns() <= deadline) {
        evpl_continue(ctx->evpl);
    }
    evpl_remove_timer(ctx->evpl, &tick);

    if (ctx->in_flight > 0) {
        chimera_sc_conf_logf(ctx, __FILE__, __LINE__, "pump deadline",
                             "%d request(s) did not complete within %llu ms",
                             ctx->in_flight,
                             (unsigned long long) (ctx->pump_deadline_ns / 1000000));
    }
} /* chimera_sc_conf_drain */

SYMBOL_EXPORT void
chimera_sc_conf_run(struct chimera_sc_conf_call *call)
{
    struct chimera_sc_conformance_ctx *ctx = call->ctx;
    struct evpl_timer                  tick;
    uint64_t                           deadline;

    chimera_sc_conf_submit(call);
    deadline = conf_now_ns() + ctx->pump_deadline_ns;
    evpl_add_timer(ctx->evpl, &tick, conf_tick_cb, CHIMERA_SC_CONF_PUMP_TICK_US);
    while (!call->done && conf_now_ns() <= deadline) {
        evpl_continue(ctx->evpl);
    }
    evpl_remove_timer(ctx->evpl, &tick);

    if (!call->done) {
        /* A dropped completion -- the non-conformance this harness exists to
         * catch -- becomes a reported failure, not a hang.  The request status
         * stays stale, so the caller's EXPECT reports FAIL. */
        chimera_sc_conf_logf(ctx, __FILE__, __LINE__, "pump deadline",
                             "backend did not complete request (opcode %u) within %llu ms",
                             (unsigned) call->req.opcode,
                             (unsigned long long) (ctx->pump_deadline_ns / 1000000));
    }
} /* chimera_sc_conf_run */

SYMBOL_EXPORT uint32_t
chimera_sc_conf_attach(
    struct chimera_sc_conformance_ctx *ctx,
    uint16_t                           want_major,
    uint16_t                           want_minor)
{
    struct chimera_sc_conf_call call;

    chimera_sc_conf_call_init(&call, ctx, CHIMERA_SC_OP_ATTACH);
    call.req.caller.fence_epoch  = 0;  /* ATTACH carries epoch 0 */
    call.req.attach.want.major   = want_major;
    call.req.attach.want.minor   = want_minor;
    call.req.attach.want.patch   = 0;
    call.req.attach.pclasses     = ctx->pclass_buf;
    call.req.attach.pclasses_cap = CHIMERA_SC_CONF_PCLASS_CAP;

    chimera_sc_conf_run(&call);

    if (call.req.status == CHIMERA_SC_OK) {
        ctx->version          = call.req.attach.r_version;
        ctx->caps             = call.req.attach.r_caps;
        ctx->limits           = call.req.attach.r_limits;
        ctx->epoch            = call.req.attach.r_fence_epoch;
        ctx->durability_epoch = call.req.attach.r_durability_epoch;
        ctx->r_npclasses      = call.req.attach.r_npclasses;
        ctx->attached         = 1;
    }
    return call.req.status;
} /* chimera_sc_conf_attach */

/* ---- diagnostics ---- */

SYMBOL_EXPORT void
chimera_sc_conf_logf(
    struct chimera_sc_conformance_ctx *ctx,
    const char                        *file,
    int                                line,
    const char                        *expr,
    const char                        *fmt,
    ...)
{
    va_list ap;
    int     off;

    ctx->failed = 1;

    off = snprintf(ctx->log, sizeof(ctx->log), "%s:%d: (%s) ", file, line, expr);
    if (off < 0) {
        off = 0;
    }
    if ((size_t) off < sizeof(ctx->log)) {
        va_start(ap, fmt);
        vsnprintf(ctx->log + off, sizeof(ctx->log) - (size_t) off, fmt, ap);
        va_end(ap);
    }
    fprintf(stderr, "conformance: %s\n", ctx->log);
} /* chimera_sc_conf_logf */

/* ---- runner ---- */

SYMBOL_EXPORT enum chimera_sc_conf_result
chimera_sc_conformance_run_one(
    const struct chimera_sc_conformance_fixture *fixture,
    const struct chimera_sc_conformance_case    *tcase,
    struct evpl                                 *evpl)
{
    struct chimera_sc_conformance_ctx ctx;
    enum chimera_sc_conf_result       result;
    uint32_t status;

    if (chimera_sc_conf_open(&ctx, fixture, evpl) != 0) {
        fprintf(stderr, "conformance: %s: open failed\n", tcase->name);
        return CHIMERA_SC_CONF_FAIL;
    }

    status = chimera_sc_conf_attach(&ctx, CHIMERA_SC_CONTRACT_MAJOR,
                                    CHIMERA_SC_CONTRACT_MINOR);
    if (status != CHIMERA_SC_OK) {
        fprintf(stderr, "conformance: %s: attach failed (%u)\n",
                tcase->name, (unsigned) status);
        chimera_sc_conf_close(&ctx);
        return CHIMERA_SC_CONF_FAIL;
    }

    if (tcase->required_caps &&
        (ctx.caps & tcase->required_caps) != tcase->required_caps) {
        chimera_sc_conf_close(&ctx);
        return CHIMERA_SC_CONF_SKIP;
    }

    result = tcase->run(&ctx);
    chimera_sc_conf_close(&ctx);
    return result;
} /* chimera_sc_conformance_run_one */

/* ---- op helpers for the obligation primitives ---- */

static const uint8_t conf_pattern[64] = {
    0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x23, 0x45, 0x67,
    0x89, 0xAB, 0xCD, 0xEF, 0x10, 0x32, 0x54, 0x76,
    0x98, 0xBA, 0xDC, 0xFE, 0x0F, 0x1E, 0x2D, 0x3C,
    0x4B, 0x5A, 0x69, 0x78, 0x87, 0x96, 0xA5, 0xB4,
    0xC3, 0xD2, 0xE1, 0xF0, 0x11, 0x22, 0x33, 0x44,
    0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC,
    0xDD, 0xEE, 0xFF, 0x00, 0x13, 0x37, 0x42, 0x24,
    0x7E, 0xE7, 0x6C, 0xC6, 0x5D, 0xD5, 0x3A, 0xA3,
};

static uint32_t
conf_obj_create(
    struct chimera_sc_conformance_ctx *ctx,
    struct chimera_sc_obj_id          *r_id)
{
    struct chimera_sc_conf_call call;

    chimera_sc_conf_call_init(&call, ctx, CHIMERA_SC_OP_OBJ_CREATE);
    chimera_sc_conf_run(&call);
    if (call.req.status == CHIMERA_SC_OK) {
        *r_id = call.req.obj_create.r_id;
    }
    return call.req.status;
} /* conf_obj_create */

/* A single-delta NS_TXN carrying only obj, used for HOLD. */
static uint32_t
conf_ns_txn_hold(
    struct chimera_sc_conformance_ctx *ctx,
    const struct chimera_sc_obj_id    *id)
{
    struct chimera_sc_conf_call call;
    struct chimera_sc_delta     delta;

    memset(&delta, 0, sizeof(delta));
    delta.kind     = CHIMERA_SC_DELTA_HOLD;
    delta.hold.obj = *id;

    chimera_sc_conf_call_init(&call, ctx, CHIMERA_SC_OP_NS_TXN);
    call.req.ns_txn.deltas  = &delta;
    call.req.ns_txn.ndeltas = 1;
    chimera_sc_conf_run(&call);
    return call.req.status;
} /* conf_ns_txn_hold */

static uint32_t
conf_obj_write(
    struct chimera_sc_conformance_ctx *ctx,
    const struct chimera_sc_obj_id    *id,
    uint64_t                           offset,
    const uint8_t                     *data,
    uint32_t                           len,
    uint32_t                           level)
{
    struct chimera_sc_conf_call call;
    struct evpl_iovec           iov;
    int                         n;

    n = evpl_iovec_alloc(ctx->evpl, len, 8, 1, 0, &iov);
    if (n != 1) {
        return CHIMERA_SC_E_IO;
    }
    memcpy(evpl_iovec_data(&iov), data, len);

    chimera_sc_conf_call_init(&call, ctx, CHIMERA_SC_OP_OBJ_WRITE);
    call.req.obj_write.id     = *id;
    call.req.obj_write.offset = offset;
    call.req.obj_write.iov    = &iov;
    call.req.obj_write.niov   = 1;
    call.req.obj_write.level  = level;
    chimera_sc_conf_run(&call);

    evpl_iovec_release(ctx->evpl, &iov);
    return call.req.status;
} /* conf_obj_write */

static uint32_t
conf_obj_flush(
    struct chimera_sc_conformance_ctx *ctx,
    const struct chimera_sc_obj_id    *id)
{
    struct chimera_sc_conf_call call;

    chimera_sc_conf_call_init(&call, ctx, CHIMERA_SC_OP_OBJ_FLUSH);
    call.req.obj_flush.id = *id;
    chimera_sc_conf_run(&call);
    return call.req.status;
} /* conf_obj_flush */

/* Read up to cap bytes from offset 0 into out; sets *r_len.  Returns status. */
static uint32_t
conf_obj_read(
    struct chimera_sc_conformance_ctx *ctx,
    const struct chimera_sc_obj_id    *id,
    uint64_t                           length,
    uint8_t                           *out,
    uint32_t                           cap,
    uint32_t                          *r_len)
{
    struct chimera_sc_conf_call call;
    struct evpl_iovec           iov[4];
    uint32_t                    copied = 0;
    int                         i;

    chimera_sc_conf_call_init(&call, ctx, CHIMERA_SC_OP_OBJ_READ);
    call.req.obj_read.id            = *id;
    call.req.obj_read.extent.offset = 0;
    call.req.obj_read.extent.length = length;
    call.req.obj_read.iov           = iov;
    call.req.obj_read.niov          = (int) (sizeof(iov) / sizeof(iov[0]));
    chimera_sc_conf_run(&call);

    if (call.req.status == CHIMERA_SC_OK) {
        for (i = 0; i < call.req.obj_read.r_niov; i++) {
            unsigned int l = evpl_iovec_length(&iov[i]);

            if (copied + l > cap) {
                l = cap - copied;
            }
            memcpy(out + copied, evpl_iovec_data(&iov[i]), l);
            copied += l;
        }
        for (i = 0; i < call.req.obj_read.r_niov; i++) {
            evpl_iovec_release(ctx->evpl, &iov[i]);
        }
    }
    *r_len = copied;
    return call.req.status;
} /* conf_obj_read */

static uint32_t
conf_ns_put(
    struct chimera_sc_conformance_ctx *ctx,
    const uint8_t                     *key,
    uint32_t                           klen,
    const uint8_t                     *val,
    uint32_t                           vlen)
{
    struct chimera_sc_conf_call call;
    struct chimera_sc_record    put;

    memset(&put, 0, sizeof(put));
    put.key.data   = key;
    put.key.len    = klen;
    put.value.data = val;
    put.value.len  = vlen;

    chimera_sc_conf_call_init(&call, ctx, CHIMERA_SC_OP_NS_TXN);
    call.req.ns_txn.puts  = &put;
    call.req.ns_txn.nputs = 1;
    chimera_sc_conf_run(&call);
    return call.req.status;
} /* conf_ns_put */

/* ---- obligation primitives ---- */

SYMBOL_EXPORT enum chimera_sc_conf_result
chimera_sc_oblig_trivial_roundtrip(struct chimera_sc_conformance_ctx *ctx)
{
    struct chimera_sc_obj_id    id;
    struct chimera_sc_conf_call get;
    uint8_t buf[64];
    uint32_t len               = 0;
    static const uint8_t key[] = "binding/trivial";
    static const uint8_t val[] = "object-id-bytes";

    CHIMERA_SC_EXPECT_STATUS(ctx, conf_obj_create(ctx, &id), CHIMERA_SC_OK);
    CHIMERA_SC_EXPECT_STATUS(ctx,
                             conf_obj_write(ctx, &id, 0, conf_pattern,
                                            sizeof(conf_pattern),
                                            CHIMERA_SC_WRITE_DURABLE),
                             CHIMERA_SC_OK);
    CHIMERA_SC_EXPECT_STATUS(ctx, conf_obj_flush(ctx, &id), CHIMERA_SC_OK);
    CHIMERA_SC_EXPECT_STATUS(ctx,
                             conf_obj_read(ctx, &id, sizeof(conf_pattern),
                                           buf, sizeof(buf), &len),
                             CHIMERA_SC_OK);
    CHIMERA_SC_EXPECT(ctx, len == sizeof(conf_pattern),
                      "read %u bytes, want %u",
                      (unsigned) len, (unsigned) sizeof(conf_pattern));
    CHIMERA_SC_EXPECT_BYTES(ctx, buf, conf_pattern, sizeof(conf_pattern));

    CHIMERA_SC_EXPECT_STATUS(ctx,
                             conf_ns_put(ctx, key, sizeof(key) - 1,
                                         val, sizeof(val) - 1),
                             CHIMERA_SC_OK);

    chimera_sc_conf_call_init(&get, ctx, CHIMERA_SC_OP_NS_GET);
    get.req.ns_get.key.data = key;
    get.req.ns_get.key.len  = sizeof(key) - 1;
    chimera_sc_conf_run(&get);
    CHIMERA_SC_EXPECT_STATUS(ctx, get.req.status, CHIMERA_SC_OK);
    CHIMERA_SC_EXPECT(ctx, get.ns_value_len == sizeof(val) - 1,
                      "value %u bytes, want %u",
                      (unsigned) get.ns_value_len, (unsigned) (sizeof(val) - 1));
    CHIMERA_SC_EXPECT_BYTES(ctx, get.ns_value, val, sizeof(val) - 1);

    return CHIMERA_SC_CONF_PASS;
} /* chimera_sc_oblig_trivial_roundtrip */

SYMBOL_EXPORT enum chimera_sc_conf_result
chimera_sc_oblig_durable_survives_crash(struct chimera_sc_conformance_ctx *ctx)
{
    struct chimera_sc_obj_id id;
    uint8_t buf[64];
    uint32_t len = 0;
    uint32_t status;

    if (!ctx->fixture->crash || !ctx->fixture->recover) {
        return CHIMERA_SC_CONF_SKIP;
    }

    /* A held, never-referenced object (S13) that took a DURABLE write. */
    CHIMERA_SC_EXPECT_STATUS(ctx, conf_obj_create(ctx, &id), CHIMERA_SC_OK);
    CHIMERA_SC_EXPECT_STATUS(ctx, conf_ns_txn_hold(ctx, &id), CHIMERA_SC_OK);
    CHIMERA_SC_EXPECT_STATUS(ctx,
                             conf_obj_write(ctx, &id, 0, conf_pattern,
                                            sizeof(conf_pattern),
                                            CHIMERA_SC_WRITE_DURABLE),
                             CHIMERA_SC_OK);

    CHIMERA_SC_EXPECT(ctx, ctx->fixture->crash(ctx->priv) == 0, "crash hook failed");
    CHIMERA_SC_EXPECT(ctx, ctx->fixture->recover(ctx->priv) == 0, "recover hook failed");

    status = chimera_sc_conf_attach(ctx, CHIMERA_SC_CONTRACT_MAJOR,
                                    CHIMERA_SC_CONTRACT_MINOR);
    CHIMERA_SC_EXPECT_STATUS(ctx, status, CHIMERA_SC_OK);

    /* D5 + S13: the DURABLE write survived the crash and the new epoch. */
    CHIMERA_SC_EXPECT_STATUS(ctx,
                             conf_obj_read(ctx, &id, sizeof(conf_pattern),
                                           buf, sizeof(buf), &len),
                             CHIMERA_SC_OK);
    CHIMERA_SC_EXPECT(ctx, len == sizeof(conf_pattern),
                      "survived %u bytes, want %u",
                      (unsigned) len, (unsigned) sizeof(conf_pattern));
    CHIMERA_SC_EXPECT_BYTES(ctx, buf, conf_pattern, sizeof(conf_pattern));

    return CHIMERA_SC_CONF_PASS;
} /* chimera_sc_oblig_durable_survives_crash */

SYMBOL_EXPORT enum chimera_sc_conf_result
chimera_sc_oblig_txn_atomic_across_crash(struct chimera_sc_conformance_ctx *ctx)
{
    struct chimera_sc_conf_call call;
    struct chimera_sc_record    puts[2];
    struct chimera_sc_conf_call g1, g2;
    int found1, found2;
    uint32_t status;
    static const uint8_t k1[] = "txn/atomic/a";
    static const uint8_t k2[] = "txn/atomic/b";
    static const uint8_t v1[] = "aaaa";
    static const uint8_t v2[] = "bbbb";

    if (!ctx->fixture->crash || !ctx->fixture->recover) {
        return CHIMERA_SC_CONF_SKIP;
    }

    /* Two records committed in one NS_TXN (D4). */
    memset(puts, 0, sizeof(puts));
    puts[0].key.data   = k1;
    puts[0].key.len    = sizeof(k1) - 1;
    puts[0].value.data = v1;
    puts[0].value.len  = sizeof(v1) - 1;
    puts[1].key.data   = k2;
    puts[1].key.len    = sizeof(k2) - 1;
    puts[1].value.data = v2;
    puts[1].value.len  = sizeof(v2) - 1;

    chimera_sc_conf_call_init(&call, ctx, CHIMERA_SC_OP_NS_TXN);
    call.req.ns_txn.puts  = puts;
    call.req.ns_txn.nputs = 2;
    chimera_sc_conf_run(&call);
    CHIMERA_SC_EXPECT_STATUS(ctx, call.req.status, CHIMERA_SC_OK);

    CHIMERA_SC_EXPECT(ctx, ctx->fixture->crash(ctx->priv) == 0, "crash hook failed");
    CHIMERA_SC_EXPECT(ctx, ctx->fixture->recover(ctx->priv) == 0, "recover hook failed");

    status = chimera_sc_conf_attach(ctx, CHIMERA_SC_CONTRACT_MAJOR,
                                    CHIMERA_SC_CONTRACT_MINOR);
    CHIMERA_SC_EXPECT_STATUS(ctx, status, CHIMERA_SC_OK);

    chimera_sc_conf_call_init(&g1, ctx, CHIMERA_SC_OP_NS_GET);
    g1.req.ns_get.key.data = k1;
    g1.req.ns_get.key.len  = sizeof(k1) - 1;
    chimera_sc_conf_run(&g1);
    CHIMERA_SC_EXPECT(ctx, g1.req.status == CHIMERA_SC_OK ||
                      g1.req.status == CHIMERA_SC_E_NOT_FOUND,
                      "unexpected status %u", (unsigned) g1.req.status);

    chimera_sc_conf_call_init(&g2, ctx, CHIMERA_SC_OP_NS_GET);
    g2.req.ns_get.key.data = k2;
    g2.req.ns_get.key.len  = sizeof(k2) - 1;
    chimera_sc_conf_run(&g2);
    CHIMERA_SC_EXPECT(ctx, g2.req.status == CHIMERA_SC_OK ||
                      g2.req.status == CHIMERA_SC_E_NOT_FOUND,
                      "unexpected status %u", (unsigned) g2.req.status);

    /* D4: all of the transaction survived, or none of it did. */
    found1 = g1.req.status == CHIMERA_SC_OK;
    found2 = g2.req.status == CHIMERA_SC_OK;
    CHIMERA_SC_EXPECT(ctx, found1 == found2,
                      "torn transaction: a=%d b=%d", found1, found2);

    return CHIMERA_SC_CONF_PASS;
} /* chimera_sc_oblig_txn_atomic_across_crash */

SYMBOL_EXPORT enum chimera_sc_conf_result
chimera_sc_oblig_ns_get_byte_lifetime(struct chimera_sc_conformance_ctx *ctx)
{
    struct chimera_sc_conf_call get;
    static const uint8_t key[] = "binding/s11";
    static const uint8_t val[] = "value-that-must-survive-complete";

    CHIMERA_SC_EXPECT_STATUS(ctx,
                             conf_ns_put(ctx, key, sizeof(key) - 1,
                                         val, sizeof(val) - 1),
                             CHIMERA_SC_OK);

    chimera_sc_conf_call_init(&get, ctx, CHIMERA_SC_OP_NS_GET);
    get.req.ns_get.key.data = key;
    get.req.ns_get.key.len  = sizeof(key) - 1;
    chimera_sc_conf_run(&get);
    CHIMERA_SC_EXPECT_STATUS(ctx, get.req.status, CHIMERA_SC_OK);

    /* S11: the driver copied the return inside complete().  It must match --
     * a backend that freed or scribbled the buffer before complete() returned
     * is caught here. */
    CHIMERA_SC_EXPECT(ctx, get.ns_value_valid, "no value captured in complete()");
    CHIMERA_SC_EXPECT(ctx, get.ns_value_len == sizeof(val) - 1,
                      "value %u bytes, want %u",
                      (unsigned) get.ns_value_len, (unsigned) (sizeof(val) - 1));
    CHIMERA_SC_EXPECT_BYTES(ctx, get.ns_value, val, sizeof(val) - 1);

    return CHIMERA_SC_CONF_PASS;
} /* chimera_sc_oblig_ns_get_byte_lifetime */

SYMBOL_EXPORT enum chimera_sc_conf_result
chimera_sc_oblig_corrupt_is_unrecoverable(struct chimera_sc_conformance_ctx *ctx)
{
    struct chimera_sc_conf_call read;
    struct chimera_sc_obj_id    id;
    struct evpl_iovec           iov[4];
    int i;

    if (!ctx->fixture->corrupt) {
        return CHIMERA_SC_CONF_SKIP;
    }

    CHIMERA_SC_EXPECT_STATUS(ctx, conf_obj_create(ctx, &id), CHIMERA_SC_OK);
    CHIMERA_SC_EXPECT_STATUS(ctx,
                             conf_obj_write(ctx, &id, 0, conf_pattern,
                                            sizeof(conf_pattern),
                                            CHIMERA_SC_WRITE_DURABLE),
                             CHIMERA_SC_OK);
    CHIMERA_SC_EXPECT_STATUS(ctx, conf_obj_flush(ctx, &id), CHIMERA_SC_OK);

    CHIMERA_SC_EXPECT(ctx, ctx->fixture->corrupt(ctx->priv, &id) == 0,
                      "corrupt hook failed");

    /* D9 / section 11: the read must report DATA_UNRECOVERABLE, never the bad
     * bytes.  A backend that returns OK here is non-conformant. */
    chimera_sc_conf_call_init(&read, ctx, CHIMERA_SC_OP_OBJ_READ);
    read.req.obj_read.id            = id;
    read.req.obj_read.extent.offset = 0;
    read.req.obj_read.extent.length = sizeof(conf_pattern);
    read.req.obj_read.iov           = iov;
    read.req.obj_read.niov          = (int) (sizeof(iov) / sizeof(iov[0]));
    chimera_sc_conf_run(&read);

    if (read.req.status == CHIMERA_SC_OK) {
        for (i = 0; i < read.req.obj_read.r_niov; i++) {
            evpl_iovec_release(ctx->evpl, &iov[i]);
        }
    }

    CHIMERA_SC_EXPECT_STATUS(ctx, read.req.status, CHIMERA_SC_E_DATA_UNRECOVERABLE);
    return CHIMERA_SC_CONF_PASS;
} /* chimera_sc_oblig_corrupt_is_unrecoverable */
