// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * Backend-facade namespace-record store (namespace-record-store): the
 * contract-6.2 surface -- NS_GET, NS_SCAN with a stable cursor, and a
 * compare-and-set NS_TXN with event deltas -- over the facade allocator, log and
 * A/B checkpoint.
 *
 * The in-memory store is a sorted array ordered by EXACT key bytes (fixing as-is
 * finding M32: a 63-bit hash with no name comparison); its record struct and the
 * checkpoint/replay helpers are in facade_internal.h.  This header adds the parts
 * that speak the storage contract (sc_request.h): the scan cursor table, NS_GET,
 * NS_SCAN and the durable NS_TXN evaluator + builder.
 *
 * Durability model (plan Revision 2, C1): a committed NS_TXN is one
 * {FACADE_LR_NS*, FACADE_LR_OBJ*, COMMIT} log transaction built under the
 * facade_txn FIFO, with the clean=0 dirty-window arm and the G1
 * compaction-before-wrap the object-txn starter runs; the staged mutations are
 * applied to live memory ONLY in the COMMIT-durable callback, so a crash with no
 * COMMIT leaves memory at the pre-transaction baseline (facade_recover.c drops a
 * txn whose COMMIT did not land).
 */

#include <stdint.h>

#include "facade_alloc.h"   /* struct facade_alloc, enum facade_status */
#include "sc_request.h"     /* chimera_sc_ns_txn_args, chimera_sc_record, ... */

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

/* ---- scan cursor table (H1) ----
 *
 * chimera_sc_cursor is 32 opaque bytes.  A key up to 30 bytes is encoded INLINE
 * and resumes by exact byte order with no server state.  A longer key cannot fit,
 * so the cursor carries an opaque 64-bit id into this bounded, in-memory table,
 * which holds the full last key; resume seeks strictly past it in exact byte
 * order -- correct for any key up to FACADE_NS_MAX_KEY.  A foreign or LRU-evicted
 * id fails loud with CHIMERA_SC_E_INVALID (the contract's "foreign cursor",
 * sc_error.h), never a wrong page.  The table is per-backend-session, not durable
 * (a scan simply restarts after a crash).  Fail-loud eviction is an accepted
 * prototype limitation; a no-eviction/persistable cursor is the follow-on
 * ns-scan-cursor-no-evict.
 */
#define FACADE_NS_CURSOR_INLINE_MAX 30u
#define FACADE_NS_CURSOR_SLOTS      64u

struct facade_ns_cursor_slot {
    uint64_t id;       /* 0 = empty */
    uint8_t *key;      /* owned copy of the full last key */
    uint32_t key_len;
    uint64_t lru;      /* last-use tick, for eviction */
};

struct facade_ns_cursor_table {
    struct facade_ns_cursor_slot slots[FACADE_NS_CURSOR_SLOTS];
    uint64_t                     next_id;
    uint64_t                     tick;
};

void
facade_ns_cursor_table_init(
    struct facade_ns_cursor_table *t);
void
facade_ns_cursor_table_destroy(
    struct facade_ns_cursor_table *t);

/* ---- NS_GET ----
 *
 * CHIMERA_SC_OK with *r_val / *r_vlen / *r_rev (the value points into the live
 * record and is valid only until the caller completes the request -- S11), or
 * CHIMERA_SC_E_NOT_FOUND. */
uint32_t
facade_ns_get(
    struct facade_alloc *a,
    const uint8_t       *key,
    uint32_t             klen,
    const uint8_t      **r_val,
    uint32_t            *r_vlen,
    uint64_t            *r_rev);

/* ---- NS_SCAN ----
 *
 * Fill up to cap records whose key has `prefix` as a prefix, in exact key order,
 * resuming after the key encoded in cur_in[32] (all-zero = start).  Record key
 * and value pointers are into live records (S11).  Writes *r_n, cur_out[32] (the
 * next cursor; all-zero when *r_eof is set) and *r_eof.  Returns CHIMERA_SC_OK,
 * or CHIMERA_SC_E_INVALID for a foreign/evicted long-key cursor. */
uint32_t
facade_ns_scan(
    struct facade_alloc           *a,
    struct facade_ns_cursor_table *ct,
    const uint8_t                 *prefix,
    uint32_t                       plen,
    const uint8_t                  cur_in[32],
    struct chimera_sc_record      *r_recs,
    uint32_t                       cap,
    uint32_t                      *r_n,
    uint8_t                        cur_out[32],
    uint32_t                      *r_eof);

/* ---- NS_TXN ---- */

struct facade_ns_txn_result {
    uint32_t status;         /* CHIMERA_SC_* */
    uint64_t r_revision;     /* resulting revision of the transaction's last put */
    uint32_t r_failed_part;  /* enum chimera_sc_txn_part on failure */
    uint32_t r_failed_index;
};

typedef void (*facade_ns_txn_cb)(
    const struct facade_ns_txn_result *res,
    void                              *private_data);

/* Evaluate and durably commit one NS_TXN: check compares (revision match;
 * revision 0 = must-not-exist), apply puts/deletes and the eight delta kinds in
 * order (preconditions against the intermediate state), check the net invariants
 * (never both referenced and held), then one atomic durable commit via the
 * facade_txn FIFO.  Reports through cb.  The args arrays must stay valid until cb
 * fires (the backend's request outlives the call). */
void
facade_ns_txn(
    struct facade_alloc                 *a,
    const struct chimera_sc_ns_txn_args *args,
    facade_ns_txn_cb                     cb,
    void                                *private_data);

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
