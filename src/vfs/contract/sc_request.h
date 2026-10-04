// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * Chimera storage contract: operations and the request structure.
 *
 * Each operation of contract section 6 is an opcode with a tagged argument
 * structure held in the request's anonymous union.  Inputs are plain fields;
 * results are prefixed r_.  The caller fills the request and calls the
 * backend's dispatch(); the backend sets status and the r_ fields and calls
 * request->complete(request) (sc_backend.h for the threading rules).
 *
 * Ownership:
 *   - Every input array and byte string is caller-owned and must stay valid
 *     until complete() is called.
 *   - OBJ_READ: the caller supplies the iov array and its capacity niov; the
 *     backend allocates data buffers into it from the dispatching thread's
 *     evpl and sets r_niov; the caller owns and releases those buffers after
 *     complete().  OBJ_WRITE buffers are borrowed until complete().
 *   - After calling complete(), the backend must not touch the request or
 *     anything it points to.
 *   - PROVISIONAL (S11): bytes the backend returns (NS_GET r_value, the key
 *     and value bytes inside NS_SCAN records) are valid only until complete()
 *     returns.  The caller copies what it keeps.  This holds with many
 *     requests in flight per thread, and when the caller dispatches a new
 *     request from inside complete().
 */

#include <stdint.h>
#include "sc_error.h"
#include "sc_types.h"
#include "sc_version.h"

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

struct evpl_iovec;

enum chimera_sc_op {
    /* 6.5 attach (+ DETACH, PROVISIONAL S2) */
    CHIMERA_SC_OP_ATTACH            = 1,
    CHIMERA_SC_OP_RENEW             = 2,
    CHIMERA_SC_OP_FENCE             = 3,
    CHIMERA_SC_OP_DETACH            = 4,
    /* 6.1 object data (+ OBJ_DISCARD S13, OBJ_MAP S3) */
    CHIMERA_SC_OP_OBJ_CREATE        = 5,
    CHIMERA_SC_OP_OBJ_CLONE_WORKING = 6,
    CHIMERA_SC_OP_OBJ_DISCARD       = 7,
    CHIMERA_SC_OP_OBJ_READ          = 8,
    CHIMERA_SC_OP_OBJ_WRITE         = 9,
    CHIMERA_SC_OP_OBJ_FLUSH         = 10,
    CHIMERA_SC_OP_OBJ_TRUNCATE      = 11,
    CHIMERA_SC_OP_OBJ_ALLOCATE      = 12,
    CHIMERA_SC_OP_OBJ_PUNCH         = 13,
    CHIMERA_SC_OP_OBJ_SEAL          = 14,
    CHIMERA_SC_OP_OBJ_STAT          = 15,
    CHIMERA_SC_OP_OBJ_MAP           = 16,
    /* 6.2 namespace records (+ NS_UNWATCH, S2) */
    CHIMERA_SC_OP_NS_GET            = 17,
    CHIMERA_SC_OP_NS_SCAN           = 18,
    CHIMERA_SC_OP_NS_TXN            = 19,
    CHIMERA_SC_OP_NS_WATCH          = 20,
    CHIMERA_SC_OP_NS_UNWATCH        = 21,
    /* 6.4 recall */
    CHIMERA_SC_OP_RECALL            = 22,
    CHIMERA_SC_OP_RECALL_WAIT       = 23,
};

#define CHIMERA_SC_OP_COUNT 23

/* Opaque byte string: namespace record keys and values (LAY-1, LAY-3). */
struct chimera_sc_bytes {
    const uint8_t *data;
    uint32_t       len;
    uint32_t       reserved;    /* must be zero */
};

struct chimera_sc_record {
    struct chimera_sc_bytes key;
    struct chimera_sc_bytes value;
    uint64_t                revision;
};

/* NS_TXN compare: the record's current revision must equal revision, and
 * revision 0 asserts that the record does not exist.  PROVISIONAL (S4). */
struct chimera_sc_compare {
    struct chimera_sc_bytes key;
    uint64_t                revision;
};

/* Opaque, stable scan cursor (6.2): all zero starts a scan; a cursor stays
 * valid across concurrent puts and deletes and resumes after the last key it
 * returned. */
struct chimera_sc_cursor {
    uint8_t b[32];
};

/* OBJ_MAP segment kinds.  PROVISIONAL (O12, S3): a hole is unallocated and
 * reads as zeros; UNWRITTEN is space reserved by OBJ_ALLOCATE. */
#define CHIMERA_SC_SEG_DATA      1
#define CHIMERA_SC_SEG_HOLE      2
#define CHIMERA_SC_SEG_UNWRITTEN 3

struct chimera_sc_segment {
    struct chimera_sc_extent extent;
    uint32_t                 kind;      /* CHIMERA_SC_SEG_* */
    uint32_t                 reserved;  /* must be zero */
};

/*
 * Namespace events (6.3), carried inside NS_TXN as deltas[].
 *
 * Evaluation model:
 *   - per-delta preconditions are checked in order, against the state left by
 *     the earlier deltas of the same transaction;
 *   - the invariant "never both referenced and held" and quota are checked
 *     net, after all deltas.
 * Any failure aborts the whole transaction (D4).
 */
enum chimera_sc_delta_kind {
    CHIMERA_SC_DELTA_CREATE      = 1,
    CHIMERA_SC_DELTA_REPLACE     = 2,
    CHIMERA_SC_DELTA_DELETE      = 3,
    CHIMERA_SC_DELTA_SEAL        = 4,
    CHIMERA_SC_DELTA_RETAG       = 5,
    CHIMERA_SC_DELTA_RECALL_HINT = 6,
    CHIMERA_SC_DELTA_HOLD        = 7,   /* PROVISIONAL (S13) */
    CHIMERA_SC_DELTA_UNHOLD      = 8,   /* PROVISIONAL (S13) */
};

#define CHIMERA_SC_DELTA_KIND_COUNT 8

/* CREATE(binding -> obj): refs 0 -> 1 (precondition: refs 0, else E_STATE);
 * charges object count.  stamps must equal the OBJ_CREATE stamps, compared
 * field by field (reserved fields are zero and not compared), else
 * E_INVALID; changing stamps is RETAG's job.  A pending binding (D3) is a
 * CREATE. */
struct chimera_sc_delta_create {
    struct chimera_sc_obj_id obj;
    struct chimera_sc_stamps stamps;
};

/* REPLACE(old, new) (D2): new 0 -> 1 (precondition: refs 0), old 1 -> 0
 * (precondition: refs 1).  Never recalls old_obj, even when it is stubbed. */
struct chimera_sc_delta_replace {
    struct chimera_sc_obj_id old_obj;
    struct chimera_sc_obj_id new_obj;
};

/* DELETE(obj): refs 1 -> 0 (precondition: refs 1); the object is released. */
struct chimera_sc_delta_delete {
    struct chimera_sc_obj_id obj;
};

/* SEAL(obj): the object becomes eligible for placement, atomically with the
 * binding change.  OBJ_SEAL (6.1) is the open -> sealed state change. */
struct chimera_sc_delta_seal {
    struct chimera_sc_obj_id obj;
};

/* RETAG(obj, policy set, quota domains) (D7, D13): re-stamp, moving byte and
 * object-count charges between domains. */
struct chimera_sc_delta_retag {
    struct chimera_sc_obj_id        obj;
    uint64_t                        policy_set;
    struct chimera_sc_quota_domains quota_domains;
};

/* RECALL_HINT(set or group tag) (D6, RCL-4): advisory.  Always accepted; a
 * backend without CAP_GROUP_RECALL ignores a group hint.  A hint never fails
 * the transaction.  Exactly one of ids/nids or group is used: nids 0 means
 * the group tag, and group tag 0 is reserved as invalid. */
struct chimera_sc_delta_recall_hint {
    const struct chimera_sc_obj_id *ids;
    uint64_t                        group;
    uint32_t                        nids;
    uint32_t                        reserved;  /* must be zero */
};

/* HOLD / UNHOLD (S13): set or clear the hold flag.  HOLD requires a
 * never-referenced, unheld object; UNHOLD a held one; else E_STATE. */
struct chimera_sc_delta_hold {
    struct chimera_sc_obj_id obj;
};

struct chimera_sc_delta {
    uint32_t kind;      /* enum chimera_sc_delta_kind */
    uint32_t reserved;  /* must be zero */
    union {
        struct chimera_sc_delta_create      create;
        struct chimera_sc_delta_replace     replace;
        struct chimera_sc_delta_delete      del;
        struct chimera_sc_delta_seal        seal;
        struct chimera_sc_delta_retag       retag;
        struct chimera_sc_delta_recall_hint recall_hint;
        struct chimera_sc_delta_hold        hold;   /* HOLD and UNHOLD */
    };
};

/* Which part of an NS_TXN failed. */
enum chimera_sc_txn_part {
    CHIMERA_SC_TXN_COMPARE = 1,
    CHIMERA_SC_TXN_PUT     = 2,
    CHIMERA_SC_TXN_DELETE  = 3,
    CHIMERA_SC_TXN_DELTA   = 4,
};

/* ---- per-op arguments ---- */

/* ATTACH (6.5, D10, D12).  caller.node identifies the node; caller.fence_epoch
 * is 0.  If the node already holds a live epoch, the backend revokes it first
 * and completes only once no op carrying it can still commit (6.5's fence
 * condition), then grants a new one.  PROVISIONAL: draft 0.2 is silent on
 * re-attach.
 *
 * Lease time base.  PROVISIONAL (S14): the contract (6.5, D10) makes the
 * epoch a lease but names no clock.  r_lease_ns is the lease length granted,
 * in nanoseconds, relative to the moment the caller dispatched this request.
 * The caller measures it on its own CLOCK_MONOTONIC: its renew deadline is
 * its CLOCK_MONOTONIC reading taken before dispatch plus r_lease_ns, and it
 * must RENEW before then.  The backend never expires the epoch sooner than
 * r_lease_ns after it completed the request, so the caller's deadline is
 * never later than the backend's.  No clock is shared between caller and
 * backend, and r_lease_ns <= limits.lease_ns.
 *
 * This is a lower bound for the lease holder only.  No other node can
 * compute when an epoch expires: it cannot see the holder's last accepted
 * RENEW, and a partitioned holder may still be renewing.  So in v1 failover
 * always uses FENCE (S14): a cluster manager never infers another node's
 * expiry, and D10's "or wait for the lease to expire" is satisfied by a
 * FENCE, which completes OK at once for an epoch that has already expired. */
struct chimera_sc_attach_args {
    struct chimera_sc_version           want;
    struct chimera_sc_protection_class *pclasses;      /* caller array */
    uint32_t                            pclasses_cap;
    uint32_t                            r_npclasses;   /* true count; at most pclasses_cap written */
    struct chimera_sc_version           r_version;
    uint64_t                            r_caps;        /* CHIMERA_SC_CAP_* */
    uint64_t                            r_fence_epoch;
    uint64_t                            r_lease_ns;    /* relative; see above */
    uint64_t                            r_durability_epoch;
    struct chimera_sc_limits            r_limits;
};

/* RENEW: the new lease is r_lease_ns from this RENEW's dispatch, on the
 * caller's CLOCK_MONOTONIC, with the guarantee stated for ATTACH.  It
 * replaces the remaining lease rather than adding to it.  E_FENCED if the
 * epoch is revoked or expired; a lease is never revived.  PROVISIONAL
 * (S14). */
struct chimera_sc_renew_args {
    uint64_t r_lease_ns;
};

/* FENCE(node_id): revoke that node's epoch now.  Completes only once no op
 * carrying it can still commit (D10).  A cluster manager calls this, and
 * waits for it, before a surviving node takes over the dead node's clients
 * (O14); in v1 this is the only failover path (S14, see ATTACH).
 *   - FENCE of a node with no live epoch, including one whose lease has
 *     expired, completes OK: its postcondition already holds.
 *   - The caller must itself hold a live epoch, else E_FENCED.
 *   - FENCE of the caller's own node is E_INVALID; a node ends its own
 *     epoch with DETACH.  PROVISIONAL (S14). */
struct chimera_sc_fence_args {
    struct chimera_sc_node_id node;
};

/* DETACH (PROVISIONAL S2): ends the caller node's epoch.  Parked requests of
 * that epoch complete E_FENCED; its epoch-held objects are released. */
struct chimera_sc_detach_args {
    uint32_t reserved;
};

/* OBJ_CREATE: a new open object; never referenced and epoch-held (S13). */
struct chimera_sc_obj_create_args {
    struct chimera_sc_stamps stamps;
    struct chimera_sc_obj_id r_id;
};

/* OBJ_CLONE_WORKING (D2): metadata-only clone of a sealed object's extent
 * map into a new open object, epoch-held, inheriting the sealed object's
 * stamps and retention.  E_RECALL_PENDING with r_ticket if stubbed;
 * E_RETENTION_LOCKED if locked (D8). */
struct chimera_sc_obj_clone_working_args {
    struct chimera_sc_obj_id sealed_id;
    struct chimera_sc_obj_id r_id;
    uint64_t                 r_ticket;
};

/* OBJ_DISCARD (PROVISIONAL S13): releases an epoch-held object created under
 * the calling node's epoch, from any of its threads.  Held, referenced or
 * another node's object: E_STATE.  Released: E_NOT_FOUND. */
struct chimera_sc_obj_discard_args {
    struct chimera_sc_obj_id id;
};

/* OBJ_READ: E_RECALL_PENDING with r_ticket if stubbed (D6);
 * E_DATA_UNRECOVERABLE if verification failed and repair is impossible (D9). */
struct chimera_sc_obj_read_args {
    struct chimera_sc_obj_id id;
    struct chimera_sc_extent extent;
    struct evpl_iovec       *iov;       /* caller array */
    int                      niov;      /* its capacity */
    int                      r_niov;
    uint64_t                 r_length;
    uint64_t                 r_ticket;
    uint32_t                 r_eof;
    uint32_t                 reserved;  /* must be zero */
};

/* OBJ_WRITE (D5): into an open object.  Returns the level actually achieved
 * and the durability epoch; if the epoch changes, buffered data from an
 * earlier epoch may have been lost. */
struct chimera_sc_obj_write_args {
    struct chimera_sc_obj_id id;
    uint64_t                 offset;
    struct evpl_iovec       *iov;
    int                      niov;
    uint32_t                 level;     /* enum chimera_sc_write_level */
    uint32_t                 r_level;
    uint32_t                 reserved;  /* must be zero */
    uint64_t                 r_length;
    uint64_t                 r_durability_epoch;
};

/* OBJ_FLUSH: make prior buffered writes durable.  extent.length 0 means the
 * whole object. */
struct chimera_sc_obj_flush_args {
    struct chimera_sc_obj_id id;
    struct chimera_sc_extent extent;
    uint64_t                 r_durability_epoch;
};

/* OBJ_TRUNCATE, OBJ_ALLOCATE, OBJ_PUNCH: size and space changes on an open
 * object.  TRUNCATE uses extent.offset as the new size; ALLOCATE reserves
 * the extent (UNWRITTEN); PUNCH deallocates it (a hole). */
struct chimera_sc_obj_space_args {
    struct chimera_sc_obj_id id;
    struct chimera_sc_extent extent;
    uint32_t                 level;     /* enum chimera_sc_write_level */
    uint32_t                 r_level;
    uint64_t                 r_durability_epoch;
};

/* OBJ_SEAL: open -> sealed.  Never E_RETENTION_LOCKED: a lock applied while a
 * working version is outstanding is inherited at seal (O13 default). */
struct chimera_sc_obj_seal_args {
    struct chimera_sc_obj_id id;
};

struct chimera_sc_obj_stat_args {
    struct chimera_sc_obj_id   id;
    struct chimera_sc_obj_meta r_meta;
};

/* OBJ_MAP (PROVISIONAL S3, O12). */
struct chimera_sc_obj_map_args {
    struct chimera_sc_obj_id   id;
    struct chimera_sc_extent   extent;
    struct chimera_sc_segment *segs;    /* caller array */
    uint32_t                   segs_cap;
    uint32_t                   r_nsegs;
    uint32_t                   r_more;
    uint32_t                   reserved;  /* must be zero */
};

struct chimera_sc_ns_get_args {
    struct chimera_sc_bytes key;
    struct chimera_sc_bytes r_value;    /* valid until complete() returns (S11) */
    uint64_t                r_revision;
};

struct chimera_sc_ns_scan_args {
    struct chimera_sc_bytes   prefix;
    struct chimera_sc_cursor  cursor;
    struct chimera_sc_record *recs;     /* caller array; bytes valid until complete() returns */
    uint32_t                  recs_cap;
    uint32_t                  r_nrecs;
    struct chimera_sc_cursor  r_next_cursor;
    uint32_t                  r_eof;
    uint32_t                  reserved;  /* must be zero */
};

/* NS_TXN (D4): compare-and-set over namespace records, with event deltas,
 * atomic, linearizable, durable on commit.  Rename is one transaction. */
struct chimera_sc_ns_txn_args {
    const struct chimera_sc_compare *compares;
    const struct chimera_sc_record  *puts;      /* revision ignored on input */
    const struct chimera_sc_bytes   *deletes;
    const struct chimera_sc_delta   *deltas;
    uint32_t                         ncompares;
    uint32_t                         nputs;
    uint32_t                         ndeletes;
    uint32_t                         ndeltas;
    uint64_t                         r_revision;
    uint32_t                         r_failed_part;   /* enum chimera_sc_txn_part */
    uint32_t                         r_failed_index;
};

/* NS_WATCH (D10): changes by other nodes are delivered through the
 * watch_event upcall (sc_backend.h). */
struct chimera_sc_ns_watch_args {
    struct chimera_sc_bytes prefix;
    uint64_t                r_watch_id;
};

/* NS_UNWATCH (PROVISIONAL S2). */
struct chimera_sc_ns_unwatch_args {
    uint64_t watch_id;
};

/* RECALL (6.4, D6, RCL-4): exactly one of ids/nids or group.  nids 0 means
 * the group tag; group tag 0 is reserved as invalid (E_INVALID).  A group
 * without CAP_GROUP_RECALL: E_NOT_SUPPORTED. */
struct chimera_sc_recall_args {
    const struct chimera_sc_obj_id *ids;
    uint64_t                        group;
    uint64_t                        r_ticket;
    uint32_t                        nids;
    uint32_t                        reserved;  /* must be zero */
};

/* RECALL_WAIT (6.4, D6): a parked request that completes when the recall
 * ends, holding no thread.  DETACH, FENCE, lease expiry and re-ATTACH
 * complete it E_FENCED.  Guidance: one wait per ticket, shared by every
 * protocol request waiting on it. */
struct chimera_sc_recall_wait_args {
    uint64_t ticket;
};

struct chimera_sc_request {
    uint32_t                 opcode;    /* enum chimera_sc_op */
    uint32_t                 status;    /* enum chimera_sc_error */
    struct chimera_sc_caller caller;
    void                     (*complete)(
        struct chimera_sc_request *request);
    void                    *caller_private;    /* Chimera / harness */
    void                    *backend_private;   /* backend scratch */
    union {
        struct chimera_sc_attach_args            attach;
        struct chimera_sc_renew_args             renew;
        struct chimera_sc_fence_args             fence;
        struct chimera_sc_detach_args            detach;
        struct chimera_sc_obj_create_args        obj_create;
        struct chimera_sc_obj_clone_working_args obj_clone_working;
        struct chimera_sc_obj_discard_args       obj_discard;
        struct chimera_sc_obj_read_args          obj_read;
        struct chimera_sc_obj_write_args         obj_write;
        struct chimera_sc_obj_flush_args         obj_flush;
        struct chimera_sc_obj_space_args         obj_space;   /* TRUNCATE, ALLOCATE, PUNCH */
        struct chimera_sc_obj_seal_args          obj_seal;
        struct chimera_sc_obj_stat_args          obj_stat;
        struct chimera_sc_obj_map_args           obj_map;
        struct chimera_sc_ns_get_args            ns_get;
        struct chimera_sc_ns_scan_args           ns_scan;
        struct chimera_sc_ns_txn_args            ns_txn;
        struct chimera_sc_ns_watch_args          ns_watch;
        struct chimera_sc_ns_unwatch_args        ns_unwatch;
        struct chimera_sc_recall_args            recall;
        struct chimera_sc_recall_wait_args       recall_wait;
    };
};

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
