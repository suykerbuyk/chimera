// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * Chimera storage contract: core types.
 *
 * Identities, epochs, extents, object states and metadata, the stamps
 * Chimera puts on objects, protection classes, capabilities and limits.
 *
 * This header also states the object lifecycle and quota rules (below).
 * They are normative for every backend, and each "harness obligation" is a
 * case the conformance suite must test.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

/* An object's identity: opaque, immutable, 128 bits, assigned by the backend
 * at creation and never reused (contract 4.1). */
struct chimera_sc_obj_id {
    uint8_t b[16];
};

/* A Chimera node's identity, as presented to the backend. */
struct chimera_sc_node_id {
    uint8_t b[16];
};

/*
 * Carried in every request (contract section 6, D10).
 *
 * caller.node is the only node identity in the contract.  A fence epoch
 * belongs to a node, not to a thread: every thread of a node carries the
 * same epoch.  ATTACH carries fence_epoch 0 and returns the node's epoch.
 *
 * Fence epochs, durability epochs and group tags are opaque uint64_t values
 * compared for equality only.
 */
struct chimera_sc_caller {
    struct chimera_sc_node_id node;
    uint64_t                  fence_epoch;
};

struct chimera_sc_extent {
    uint64_t offset;
    uint64_t length;
};

/* Contract 4.2.  open: writable by extent.  sealed: immutable, movable to a
 * spoke.  stubbed: data on a spoke only; recall returns it to sealed. */
enum chimera_sc_obj_state {
    CHIMERA_SC_OBJ_OPEN    = 1,
    CHIMERA_SC_OBJ_SEALED  = 2,
    CHIMERA_SC_OBJ_STUBBED = 3,
};

/* Contract D5.  A backend never reports DURABLE for data a power loss can
 * lose.  Not NFS stable_how: the namespace module maps between them. */
enum chimera_sc_write_level {
    CHIMERA_SC_WRITE_BUFFERED = 0,
    CHIMERA_SC_WRITE_DURABLE  = 1,
};

/* Origin protocol (PRD NS-3).  An opaque stamp to the backend (LAY-3). */
enum chimera_sc_origin {
    CHIMERA_SC_ORIGIN_UNSET = 0,
    CHIMERA_SC_ORIGIN_NFS3  = 1,
    CHIMERA_SC_ORIGIN_NFS4  = 2,
    CHIMERA_SC_ORIGIN_SMB   = 3,
    CHIMERA_SC_ORIGIN_S3    = 4,
    CHIMERA_SC_ORIGIN_FUSE  = 5,
    CHIMERA_SC_ORIGIN_OTHER = 6,
};

/* Quota domains (D13).  Opaque identifiers minted by Chimera's identity
 * mapping; the backend never parses a path.  PROVISIONAL representation. */
#define CHIMERA_SC_QD_OWNER   (1U << 0)
#define CHIMERA_SC_QD_GROUP   (1U << 1)
#define CHIMERA_SC_QD_PROJECT (1U << 2)

struct chimera_sc_quota_domains {
    uint64_t owner;
    uint64_t group;
    uint64_t project;
    uint32_t valid;     /* CHIMERA_SC_QD_* */
    uint32_t reserved;  /* must be zero */
};

/* What Chimera stamps on an object (D7, D13, NS-3): at OBJ_CREATE, again
 * (unchanged) by the CREATE delta, and changed only by RETAG. */
struct chimera_sc_stamps {
    uint64_t                        policy_set;
    struct chimera_sc_quota_domains quota_domains;
    uint64_t                        owner_principal;
    uint32_t                        origin;   /* enum chimera_sc_origin */
    uint32_t                        reserved; /* must be zero */
};

/* PROVISIONAL (S6): checksum algorithm identifiers.  The contract does not
 * yet name an algorithm; Chimera reads the checksum for protocol use only
 * (S3 ETag, Content-MD5), never to verify stored data (D9). */
/* Values 2-255 are reserved for future contract versions: a v1 backend
 * never reports them, and a consumer that reads one treats the checksum as
 * absent.  Values from CHIMERA_SC_CHECKSUM_PRIVATE up are backend-private. */
#define CHIMERA_SC_CHECKSUM_NONE    0
#define CHIMERA_SC_CHECKSUM_MD5     1   /* 16 bytes; serves S3 ETag and Content-MD5 */
#define CHIMERA_SC_CHECKSUM_PRIVATE 256
#define CHIMERA_SC_CHECKSUM_MAX     64

/* Retention and immutability (WORM) lock (D8).  While locked, the backend
 * refuses REPLACE or DELETE of the object with CHIMERA_SC_E_RETENTION_LOCKED. */
#define CHIMERA_SC_RETENTION_NONE   0
#define CHIMERA_SC_RETENTION_LOCKED 1

/* Object metadata (contract 5.1), returned by OBJ_STAT. */
struct chimera_sc_obj_meta {
    struct chimera_sc_obj_id id;
    uint32_t                 state;         /* enum chimera_sc_obj_state */
    uint32_t                 protection_class;
    uint64_t                 size;
    uint64_t                 allocated_bytes;
    struct {
        uint32_t algorithm;
        uint32_t len;
        uint8_t  bytes[CHIMERA_SC_CHECKSUM_MAX];
    }                        checksum;
    struct chimera_sc_stamps stamps;
    struct {
        uint64_t until_ns;
        uint32_t mode;      /* CHIMERA_SC_RETENTION_* */
        uint32_t reserved;  /* must be zero */
    }                        retention;
    uint64_t                 spoke_marker;  /* opaque; nonzero if a spoke copy exists */
    uint64_t                 change;        /* advances on every data or metadata change */
    uint32_t                 refs;          /* binding references: 0 or 1 in v1.0 */
    uint32_t                 held;          /* hold flag (S13): 0 or 1 */
};

/* Protection classes (D9), declared by the backend at ATTACH. */
#define CHIMERA_SC_PROTECT_REPLICA 1
#define CHIMERA_SC_PROTECT_EC      2

struct chimera_sc_protection_class {
    uint32_t id;
    uint32_t kind;          /* CHIMERA_SC_PROTECT_* */
    uint16_t n;             /* replica count */
    uint16_t k;             /* erasure-code data shards */
    uint16_t m;             /* erasure-code parity shards */
    uint16_t reserved;      /* must be zero */
};

/*
 * Capabilities (D12): optional behaviour only.  Mandatory behaviour is
 * never a capability.  In particular RECALL, RECALL_WAIT, the stubbed state
 * and CHIMERA_SC_E_RECALL_PENDING are mandatory for every backend --
 * PROVISIONAL (O5).  CAP_SPOKES says only whether objects can ever become
 * stubbed; a backend without it answers RECALL with a ticket whose
 * RECALL_WAIT completes at once.
 */
#define CHIMERA_SC_CAP_SPOKES             (1ULL << 0)
#define CHIMERA_SC_CAP_PROTECTION_CLASSES (1ULL << 1)
#define CHIMERA_SC_CAP_GROUP_RECALL       (1ULL << 2)
#define CHIMERA_SC_CAP_ESCROW_QUOTA       (1ULL << 3)

/* Limits the backend declares at ATTACH.  Exceeding one is E_INVALID. */
struct chimera_sc_limits {
    uint32_t max_txn_ops;     /* compares + puts + deletes + deltas */
    uint32_t max_key_len;
    uint32_t max_value_len;
    uint32_t max_io;          /* bytes per OBJ_READ / OBJ_WRITE */
    uint32_t max_watch;
    uint32_t max_recall_ids;
    uint64_t lease_ns;        /* nominal fence-epoch lease length; the granted
                               * length is ATTACH/RENEW r_lease_ns */
};

/*
 * Object lifecycle.  PROVISIONAL (S13).
 *
 * An object id is in exactly one of these states, as Chimera sees it:
 *
 *   epoch-held  never referenced; created (OBJ_CREATE, OBJ_CLONE_WORKING)
 *               under a fence epoch that is still live; hold flag clear.
 *   held        never referenced; hold flag set (HOLD delta).
 *   referenced  refs == 1 (CREATE, or the new side of REPLACE).
 *   released    see below.  Every Chimera op on a released id returns
 *               CHIMERA_SC_E_NOT_FOUND, whether or not the backend has
 *               physically freed it.
 *
 * Release: an object that was referenced is released when its refs return
 * to 0 (DELETE, or the old side of REPLACE).  A never-referenced object is
 * released by OBJ_DISCARD, or when its creator's epoch ends (DETACH, FENCE,
 * lease expiry, re-ATTACH) while its hold flag is clear.  A released object
 * belongs to the backend (contract 4.1): collected, or retained for a
 * snapshot or retention pin.
 *
 * HOLD is legal only on an object that has never been referenced.  Chimera
 * holds exactly two kinds of object, both unbound by design:
 *   1. D2 working versions (OBJ_CLONE_WORKING);
 *   2. full-overwrite replacements (D2): O_TRUNC, SMB OVERWRITE or
 *      SUPERSEDE, truncate to zero, S3 PUT or multipart over an existing
 *      key.
 *
 * Obligation on Chimera: before any durability acknowledgement for such an
 * object ahead of its seal point -- an OBJ_WRITE at DURABLE, or an OBJ_FLUSH
 * that is not a seal point -- Chimera commits HOLD together with the record
 * that names the object.  At the seal point it commits REPLACE with UNHOLD in
 * one transaction.
 *
 * A new file, or an S3 upload to a new key, is not held: its pending binding
 * is a CREATE (contract D3).  Unlink-while-open is a Chimera-side pattern and
 * involves no hold (see the companion document).
 *
 * Relation to contract draft 0.2: this clarifies that 4.1 and D8 govern
 * objects that became unreferenced (D2, PRD MUT-8); never-referenced objects
 * are kept as D2 requires.  E_NOT_FOUND on a released id is the immediate
 * collection 4.1 already permits, made deterministic.
 *
 * Quota.  PROVISIONAL (S12).
 *
 * Object count is charged when refs goes 0 -> 1 and released when it goes
 * 1 -> 0, checked net after the whole NS_TXN, so a REPLACE is never refused
 * at a domain's exact limit.  Holds charge nothing.  Bytes are charged at
 * extent allocation or flush against the object's stamps and released when
 * the backend frees the extents; a working version's shared extents stay
 * charged to the sealed object, and the working version is charged only for
 * extents it allocates (copy-on-write).
 *
 * Harness obligations (conformance suite, against every backend):
 *   - HOLD on an object that is or was referenced: E_STATE.
 *   - HOLD on a held object, UNHOLD on an unheld one: E_STATE.
 *   - CREATE or REPLACE.new on a referenced object: E_STATE.
 *   - CREATE or REPLACE.new on a held object without UNHOLD in the same
 *     NS_TXN: E_STATE.
 *   - OBJ_DISCARD of another node's object, or of a held or referenced one:
 *     E_STATE.
 *   - Any op on a released id, including an unheld object whose creating
 *     epoch has ended: E_NOT_FOUND.
 *   - REPLACE in a domain at its exact object-count limit: succeeds.
 *   - Per-delta preconditions are checked in order; the never-both-
 *     referenced-and-held invariant and quota are checked net.
 *   - A held working version that received a DURABLE write survives a
 *     crash and re-ATTACH.
 */

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
