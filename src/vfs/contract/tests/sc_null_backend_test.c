// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Shape test for the backend descriptor: a minimal in-test backend fills
 * struct chimera_sc_backend and a minimal consumer drives it through
 * registration, ATTACH and a handful of requests.
 *
 * The stub is a TEST STUB, NOT A CONFORMANT BACKEND.  It completes every
 * request synchronously on the dispatching thread, models the fence epochs
 * of a few nodes and nothing else, and answers the mandatory operations it
 * does not model with CHIMERA_SC_E_NOT_SUPPORTED.  That is a deliberate shortcut
 * outside the error's contract meaning ("needs an undeclared capability").
 * The object lifecycle and quota rules (sc_types.h) are harness obligations
 * for the conformance suite, not checked here.  A cross-thread case would
 * need a second evpl thread and is likewise left to the harness.
 *
 * Only the contract headers and libc are used: no libevpl type is needed
 * and nothing links libevpl.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "chimera_sc.h"

/* ---- the stub backend ---- */

#define STUB_NODES    4
#define STUB_LEASE_NS 10000000000ULL

struct stub_node {
    struct chimera_sc_node_id node;
    uint64_t                  epoch;   /* 0: no live epoch */
    int                       used;
};

struct stub_state {
    const struct chimera_sc_upcalls *upcalls;
    struct stub_node                 nodes[STUB_NODES];
    uint64_t                         next_epoch;
    uint64_t                         next_ticket;
    uint64_t                         revision;
    uint8_t                          value[32]; /* backing for NS_GET */
};

static const struct chimera_sc_protection_class stub_pclasses[] = {
    {
        .id   = 1,
        .kind = CHIMERA_SC_PROTECT_REPLICA,
        .n    = 2,
    },
    {
        .id   = 2,
        .kind = CHIMERA_SC_PROTECT_EC,
        .k    = 4,
        .m    = 2,
    },
};

static struct stub_state                        stub;

static void *
stub_init(
    const char                *cfgdata,
    struct prometheus_metrics *metrics)
{
    (void) cfgdata;
    (void) metrics;
    memset(&stub, 0, sizeof(stub));
    stub.next_epoch  = 100;
    stub.next_ticket = 1;
    return &stub;
} /* stub_init */

static void
stub_destroy(void *private_data)
{
    (void) private_data;
} /* stub_destroy */

static void *
stub_thread_init(
    struct evpl *evpl,
    void        *private_data)
{
    (void) evpl;
    return private_data;
} /* stub_thread_init */

static void
stub_thread_destroy(void *thread_private)
{
    (void) thread_private;
} /* stub_thread_destroy */

static void
stub_set_upcalls(
    void                            *private_data,
    const struct chimera_sc_upcalls *upcalls,
    void                            *up_private)
{
    struct stub_state *s = private_data;

    (void) up_private;
    s->upcalls = upcalls;
} /* stub_set_upcalls */

static struct stub_node *
stub_node_find(
    struct stub_state               *s,
    const struct chimera_sc_node_id *node,
    int                              create)
{
    int i;

    for (i = 0; i < STUB_NODES; i++) {
        if (s->nodes[i].used && memcmp(&s->nodes[i].node, node, sizeof(*node)) == 0) {
            return &s->nodes[i];
        }
    }
    for (i = 0; create && i < STUB_NODES; i++) {
        if (!s->nodes[i].used) {
            s->nodes[i].used = 1;
            s->nodes[i].node = *node;
            return &s->nodes[i];
        }
    }
    return NULL;
} /* stub_node_find */

static int
stub_epoch_ok(
    struct stub_state               *s,
    const struct chimera_sc_request *request)
{
    struct stub_node *n = stub_node_find(s, &request->caller.node, 0);

    return n && n->epoch != 0 && request->caller.fence_epoch == n->epoch;
} /* stub_epoch_ok */

static void
stub_attach(
    struct stub_state         *s,
    struct chimera_sc_request *request)
{
    struct chimera_sc_attach_args  *a = &request->attach;
    struct stub_node               *node;
    const struct chimera_sc_version have = {
        CHIMERA_SC_CONTRACT_MAJOR, CHIMERA_SC_CONTRACT_MINOR, CHIMERA_SC_CONTRACT_PATCH
    };
    uint32_t                        n = sizeof(stub_pclasses) / sizeof(stub_pclasses[0]);
    uint32_t                        i;

    if (chimera_sc_version_negotiate(&a->want, &have, &a->r_version) != 0) {
        request->status = CHIMERA_SC_E_VERSION;
        return;
    }

    node = stub_node_find(s, &request->caller.node, 1);
    if (!node) {
        request->status = CHIMERA_SC_E_INVALID;
        return;
    }

    /* Re-attach from a node with a live epoch revokes that epoch first.  The
     * stub has nothing in flight, so revocation completes at once. */
    node->epoch = s->next_epoch++;

    for (i = 0; i < n && i < a->pclasses_cap; i++) {
        a->pclasses[i] = stub_pclasses[i];
    }
    a->r_npclasses        = n;
    a->r_caps             = CHIMERA_SC_CAP_PROTECTION_CLASSES;
    a->r_fence_epoch      = node->epoch;
    a->r_lease_ns         = STUB_LEASE_NS;
    a->r_durability_epoch = 1;
    memset(&a->r_limits, 0, sizeof(a->r_limits));
    a->r_limits.max_txn_ops = 64;
    a->r_limits.max_key_len = 1024;
    a->r_limits.lease_ns    = STUB_LEASE_NS;
    request->status         = CHIMERA_SC_OK;
} /* stub_attach */

static void
stub_ns_txn(
    struct stub_state         *s,
    struct chimera_sc_request *request)
{
    struct chimera_sc_ns_txn_args *t = &request->ns_txn;
    uint32_t                       i;

    /* The stub models only advisory RECALL_HINT deltas, which a backend must
     * always accept, with or without CAP_GROUP_RECALL. */
    if (t->ncompares || t->nputs || t->ndeletes) {
        request->status = CHIMERA_SC_E_NOT_SUPPORTED;
        return;
    }
    for (i = 0; i < t->ndeltas; i++) {
        if (t->deltas[i].kind != CHIMERA_SC_DELTA_RECALL_HINT) {
            t->r_failed_part  = CHIMERA_SC_TXN_DELTA;
            t->r_failed_index = i;
            request->status   = CHIMERA_SC_E_NOT_SUPPORTED;
            return;
        }
    }
    t->r_revision   = ++s->revision;
    request->status = CHIMERA_SC_OK;
} /* stub_ns_txn */

static void
stub_dispatch(
    struct chimera_sc_request *request,
    void                      *thread_private)
{
    struct stub_state *s = thread_private;

    if (request->opcode == CHIMERA_SC_OP_ATTACH) {
        stub_attach(s, request);
        request->complete(request);
        return;
    }

    if (!stub_epoch_ok(s, request)) {
        request->status = CHIMERA_SC_E_FENCED;
        request->complete(request);
        return;
    }

    switch (request->opcode) {
        case CHIMERA_SC_OP_RENEW:
            request->renew.r_lease_ns = STUB_LEASE_NS;
            request->status           = CHIMERA_SC_OK;
            break;
        case CHIMERA_SC_OP_FENCE: {
            /* FENCE of a node with no live epoch completes OK: nothing
             * carrying its epoch can commit.  Nothing is in flight in the
             * stub, so revocation is immediate. */
            struct stub_node *target = stub_node_find(s, &request->fence.node, 0);

            if (memcmp(&request->fence.node, &request->caller.node,
                       sizeof(request->fence.node)) == 0) {
                /* A node ends its own epoch with DETACH. */
                request->status = CHIMERA_SC_E_INVALID;
                break;
            }
            if (target) {
                target->epoch = 0;
            }
            request->status = CHIMERA_SC_OK;
            break;
        }
        case CHIMERA_SC_OP_DETACH:
            stub_node_find(s, &request->caller.node, 0)->epoch = 0;
            request->status                                    = CHIMERA_SC_OK;
            break;
        case CHIMERA_SC_OP_OBJ_READ:
            /* Every object of the stub is empty. */
            request->obj_read.r_niov   = 0;
            request->obj_read.r_length = 0;
            request->obj_read.r_eof    = 1;
            request->status            = CHIMERA_SC_OK;
            break;
        case CHIMERA_SC_OP_RECALL:
            if (request->recall.nids == 0 && request->recall.group == 0) {
                /* Group tag 0 is reserved as invalid. */
                request->status = CHIMERA_SC_E_INVALID;
            } else if (request->recall.nids == 0) {
                /* A group recall needs CAP_GROUP_RECALL, not declared. */
                request->status = CHIMERA_SC_E_NOT_SUPPORTED;
            } else {
                request->recall.r_ticket = s->next_ticket++;
                request->status          = CHIMERA_SC_OK;
            }
            break;
        case CHIMERA_SC_OP_NS_TXN:
            stub_ns_txn(s, request);
            break;
        case CHIMERA_SC_OP_NS_GET:
            memcpy(s->value, "binding", 8);
            request->ns_get.r_value.data = s->value;
            request->ns_get.r_value.len  = 8;
            request->ns_get.r_revision   = s->revision;
            request->status              = CHIMERA_SC_OK;
            request->complete(request);
            /* S11: the bytes were valid only until complete() returned. */
            memset(s->value, 0xAA, sizeof(s->value));
            return;
        default:
            request->status = CHIMERA_SC_E_NOT_SUPPORTED;
            break;
    } /* switch */

    request->complete(request);
} /* stub_dispatch */

static const struct chimera_sc_backend stub_backend = {
    .abi_version    = CHIMERA_SC_ABI_VERSION,
    .name           = "null",
    .init           = stub_init,
    .destroy        = stub_destroy,
    .thread_init    = stub_thread_init,
    .thread_destroy = stub_thread_destroy,
    .dispatch       = stub_dispatch,
    .set_upcalls    = stub_set_upcalls,
};

/* ---- the minimal consumer ---- */

static const struct chimera_sc_upcalls consumer_upcalls = { NULL };

/* Registration: the ABI gate a consumer applies, as chimera_vfs_register()
 * does for the module SDK. */
static int
consumer_register(const struct chimera_sc_backend *backend)
{
    return backend->abi_version == CHIMERA_SC_ABI_VERSION ? 0 : -1;
} /* consumer_register */

struct completion {
    int     called;
    uint8_t value_copy[16];
    int     value_len;
};

static void
on_complete(struct chimera_sc_request *request)
{
    struct completion *c = request->caller_private;

    c->called++;
    if (request->opcode == CHIMERA_SC_OP_NS_GET && request->status == CHIMERA_SC_OK) {
        /* S11: copy what we keep before returning. */
        c->value_len = (int) request->ns_get.r_value.len;
        memcpy(c->value_copy, request->ns_get.r_value.data, request->ns_get.r_value.len);
    }
} /* on_complete */

static void
request_init(
    struct chimera_sc_request       *request,
    struct completion               *c,
    uint32_t                         opcode,
    const struct chimera_sc_node_id *node,
    uint64_t                         epoch)
{
    memset(request, 0, sizeof(*request));
    memset(c, 0, sizeof(*c));
    request->opcode             = opcode;
    request->status             = CHIMERA_SC_E_INVALID;
    request->caller.node        = *node;
    request->caller.fence_epoch = epoch;
    request->complete           = on_complete;
    request->caller_private     = c;
} /* request_init */

static uint64_t
do_attach(
    void                            *thread_private,
    const struct chimera_sc_node_id *node,
    uint16_t                         major,
    uint32_t                        *status)
{
    struct chimera_sc_request          request;
    struct completion                  c;
    struct chimera_sc_protection_class pclasses[1];

    request_init(&request, &c, CHIMERA_SC_OP_ATTACH, node, 0);
    request.attach.want.major   = major;
    request.attach.want.minor   = CHIMERA_SC_CONTRACT_MINOR;
    request.attach.pclasses     = pclasses;
    request.attach.pclasses_cap = 1;
    stub_backend.dispatch(&request, thread_private);
    assert(c.called == 1);
    *status = request.status;
    if (request.status != CHIMERA_SC_OK) {
        return 0;
    }

    assert(request.attach.r_version.major == CHIMERA_SC_CONTRACT_MAJOR);
    assert(request.attach.r_caps == CHIMERA_SC_CAP_PROTECTION_CLASSES);
    assert(request.attach.r_fence_epoch != 0);
    assert(request.attach.r_lease_ns == STUB_LEASE_NS);
    /* The true count comes back even when the caller's array is smaller. */
    assert(request.attach.r_npclasses == 2);
    assert(pclasses[0].id == 1 && pclasses[0].kind == CHIMERA_SC_PROTECT_REPLICA);
    return request.attach.r_fence_epoch;
} /* do_attach */

int
main(void)
{
    struct chimera_sc_backend bad;
    struct chimera_sc_node_id node = { { 7 } };
    struct chimera_sc_node_id peer = { { 9 } };
    struct chimera_sc_node_id gone = { { 11 } };
    struct chimera_sc_request request;
    struct completion         c;
    struct chimera_sc_delta   hint;
    uint32_t                  status;
    void                     *priv, *thread_private;
    uint64_t                  epoch, epoch2, peer_epoch;

    /* ABI gate. */
    bad             = stub_backend;
    bad.abi_version = CHIMERA_SC_ABI_VERSION + 1;
    assert(consumer_register(&bad) == -1);
    assert(consumer_register(&stub_backend) == 0);

    priv = stub_backend.init("{}", NULL);
    stub_backend.set_upcalls(priv, &consumer_upcalls, NULL);
    thread_private = stub_backend.thread_init(NULL, priv);

    /* A major-version mismatch refuses to attach. */
    epoch = do_attach(thread_private, &node, CHIMERA_SC_CONTRACT_MAJOR + 1, &status);
    assert(status == CHIMERA_SC_E_VERSION && epoch == 0);

    /* Attach; the node identity is caller.node. */
    epoch = do_attach(thread_private, &node, CHIMERA_SC_CONTRACT_MAJOR, &status);
    assert(status == CHIMERA_SC_OK);
    assert(stub_node_find(&stub, &node, 0) != NULL);

    request_init(&request, &c, CHIMERA_SC_OP_RENEW, &node, epoch);
    stub_backend.dispatch(&request, thread_private);
    assert(c.called == 1 && request.status == CHIMERA_SC_OK);

    /* A wrong epoch is fenced. */
    request_init(&request, &c, CHIMERA_SC_OP_RENEW, &node, epoch + 1000);
    stub_backend.dispatch(&request, thread_private);
    assert(request.status == CHIMERA_SC_E_FENCED);

    /* Re-attach revokes the live epoch: the old one is now fenced. */
    epoch2 = do_attach(thread_private, &node, CHIMERA_SC_CONTRACT_MAJOR, &status);
    assert(status == CHIMERA_SC_OK && epoch2 != epoch);
    request_init(&request, &c, CHIMERA_SC_OP_RENEW, &node, epoch);
    stub_backend.dispatch(&request, thread_private);
    assert(request.status == CHIMERA_SC_E_FENCED);
    epoch = epoch2;

    /* FENCE: a surviving node revokes another node's epoch.  Every request
     * carrying the fenced epoch is refused afterwards. */
    peer_epoch = do_attach(thread_private, &peer, CHIMERA_SC_CONTRACT_MAJOR, &status);
    assert(status == CHIMERA_SC_OK && peer_epoch != epoch);
    request_init(&request, &c, CHIMERA_SC_OP_FENCE, &peer, peer_epoch);
    request.fence.node = node;
    stub_backend.dispatch(&request, thread_private);
    assert(c.called == 1 && request.status == CHIMERA_SC_OK);
    request_init(&request, &c, CHIMERA_SC_OP_RENEW, &node, epoch);
    stub_backend.dispatch(&request, thread_private);
    assert(request.status == CHIMERA_SC_E_FENCED);
    request_init(&request, &c, CHIMERA_SC_OP_OBJ_READ, &node, epoch);
    stub_backend.dispatch(&request, thread_private);
    assert(request.status == CHIMERA_SC_E_FENCED);

    /* FENCE needs a live epoch on the caller: from the fenced node, or from a
     * node that never attached, it is refused. */
    request_init(&request, &c, CHIMERA_SC_OP_FENCE, &node, epoch);
    request.fence.node = peer;
    stub_backend.dispatch(&request, thread_private);
    assert(request.status == CHIMERA_SC_E_FENCED);
    request_init(&request, &c, CHIMERA_SC_OP_FENCE, &gone, 0);
    request.fence.node = peer;
    stub_backend.dispatch(&request, thread_private);
    assert(request.status == CHIMERA_SC_E_FENCED);

    /* FENCE of the caller's own node is invalid; DETACH is the way. */
    request_init(&request, &c, CHIMERA_SC_OP_FENCE, &peer, peer_epoch);
    request.fence.node = peer;
    stub_backend.dispatch(&request, thread_private);
    assert(request.status == CHIMERA_SC_E_INVALID);

    /* FENCE of a node with no live epoch completes OK. */
    request_init(&request, &c, CHIMERA_SC_OP_FENCE, &peer, peer_epoch);
    request.fence.node = gone;
    stub_backend.dispatch(&request, thread_private);
    assert(request.status == CHIMERA_SC_OK);

    /* A fenced node may attach again and gets a new, different epoch. */
    epoch2 = do_attach(thread_private, &node, CHIMERA_SC_CONTRACT_MAJOR, &status);
    assert(status == CHIMERA_SC_OK && epoch2 != epoch && epoch2 != peer_epoch);
    epoch = epoch2;

    /* DETACH ends the caller's own epoch; a fresh ATTACH grants a new one. */
    request_init(&request, &c, CHIMERA_SC_OP_DETACH, &peer, peer_epoch);
    stub_backend.dispatch(&request, thread_private);
    assert(c.called == 1 && request.status == CHIMERA_SC_OK);
    request_init(&request, &c, CHIMERA_SC_OP_RENEW, &peer, peer_epoch);
    stub_backend.dispatch(&request, thread_private);
    assert(request.status == CHIMERA_SC_E_FENCED);
    epoch2 = do_attach(thread_private, &peer, CHIMERA_SC_CONTRACT_MAJOR, &status);
    assert(status == CHIMERA_SC_OK && epoch2 != peer_epoch);

    /* The node fenced earlier is unaffected by the peer's DETACH. */
    request_init(&request, &c, CHIMERA_SC_OP_RENEW, &node, epoch);
    stub_backend.dispatch(&request, thread_private);
    assert(request.status == CHIMERA_SC_OK && request.renew.r_lease_ns == STUB_LEASE_NS);

    /* OBJ_READ of an empty object with no caller array. */
    request_init(&request, &c, CHIMERA_SC_OP_OBJ_READ, &node, epoch);
    request.obj_read.iov  = NULL;
    request.obj_read.niov = 0;
    stub_backend.dispatch(&request, thread_private);
    assert(request.status == CHIMERA_SC_OK);
    assert(request.obj_read.r_niov == 0 && request.obj_read.r_length == 0);
    assert(request.obj_read.r_eof == 1);

    /* A group recall without CAP_GROUP_RECALL is refused. */
    request_init(&request, &c, CHIMERA_SC_OP_RECALL, &node, epoch);
    request.recall.group = 42;
    stub_backend.dispatch(&request, thread_private);
    assert(request.status == CHIMERA_SC_E_NOT_SUPPORTED);

    /* Group tag 0 is reserved as invalid. */
    request_init(&request, &c, CHIMERA_SC_OP_RECALL, &node, epoch);
    stub_backend.dispatch(&request, thread_private);
    assert(request.status == CHIMERA_SC_E_INVALID);

    /* ...but a group RECALL_HINT never fails the transaction. */
    memset(&hint, 0, sizeof(hint));
    hint.kind              = CHIMERA_SC_DELTA_RECALL_HINT;
    hint.recall_hint.group = 42;
    request_init(&request, &c, CHIMERA_SC_OP_NS_TXN, &node, epoch);
    request.ns_txn.deltas  = &hint;
    request.ns_txn.ndeltas = 1;
    stub_backend.dispatch(&request, thread_private);
    assert(request.status == CHIMERA_SC_OK && request.ns_txn.r_revision == 1);

    /* S11: returned bytes are copied inside complete(); the backend may reuse
     * them as soon as complete() returns. */
    request_init(&request, &c, CHIMERA_SC_OP_NS_GET, &node, epoch);
    stub_backend.dispatch(&request, thread_private);
    assert(request.status == CHIMERA_SC_OK);
    assert(c.value_len == 8 && memcmp(c.value_copy, "binding", 8) == 0);
    assert(stub.value[0] == 0xAA);

    /* An operation the stub does not model. */
    request_init(&request, &c, CHIMERA_SC_OP_OBJ_STAT, &node, epoch);
    stub_backend.dispatch(&request, thread_private);
    assert(c.called == 1 && request.status == CHIMERA_SC_E_NOT_SUPPORTED);

    stub_backend.thread_destroy(thread_private);
    stub_backend.destroy(priv);

    printf("sc_null_backend_test: ok\n");
    return 0;
} /* main */
