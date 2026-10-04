// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Compile-time and version-negotiation checks for the storage contract
 * interface.
 *
 * Every value a backend or consumer can store or send is asserted
 * individually, so renumbering, a collision or a swap fails to build.  Every
 * member of every ABI structure has its offset pinned on LP64, with every
 * structure's size: changing any of them is an ABI change and needs a
 * CHIMERA_SC_ABI_VERSION bump (update the assertions in the same commit).
 */

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "chimera_sc.h"

#define SC_ASSERT_VALUE(name, value) \
        _Static_assert((name) == (value), #name " is frozen at " #value)

/* ---- versions ---- */
SC_ASSERT_VALUE(CHIMERA_SC_ABI_VERSION, 1);
SC_ASSERT_VALUE(CHIMERA_SC_CONTRACT_MAJOR, 1);
SC_ASSERT_VALUE(CHIMERA_SC_CONTRACT_MINOR, 0);
SC_ASSERT_VALUE(CHIMERA_SC_CONTRACT_PATCH, 0);

/* ---- errors ---- */
SC_ASSERT_VALUE(CHIMERA_SC_OK, 0);
SC_ASSERT_VALUE(CHIMERA_SC_E_QUOTA_EXCEEDED, 1);
SC_ASSERT_VALUE(CHIMERA_SC_E_NO_SPACE, 2);
SC_ASSERT_VALUE(CHIMERA_SC_E_RECALL_PENDING, 3);
SC_ASSERT_VALUE(CHIMERA_SC_E_RETENTION_LOCKED, 4);
SC_ASSERT_VALUE(CHIMERA_SC_E_KEY_UNAVAILABLE, 5);
SC_ASSERT_VALUE(CHIMERA_SC_E_DATA_UNRECOVERABLE, 6);
SC_ASSERT_VALUE(CHIMERA_SC_E_FENCED, 32);
SC_ASSERT_VALUE(CHIMERA_SC_E_VERSION, 33);
SC_ASSERT_VALUE(CHIMERA_SC_E_NOT_FOUND, 34);
SC_ASSERT_VALUE(CHIMERA_SC_E_STATE, 35);
SC_ASSERT_VALUE(CHIMERA_SC_E_COMPARE_FAILED, 36);
SC_ASSERT_VALUE(CHIMERA_SC_E_INVALID, 37);
SC_ASSERT_VALUE(CHIMERA_SC_E_NOT_SUPPORTED, 38);
SC_ASSERT_VALUE(CHIMERA_SC_E_IO, 39);

/* ---- operations ---- */
SC_ASSERT_VALUE(CHIMERA_SC_OP_ATTACH, 1);
SC_ASSERT_VALUE(CHIMERA_SC_OP_RENEW, 2);
SC_ASSERT_VALUE(CHIMERA_SC_OP_FENCE, 3);
SC_ASSERT_VALUE(CHIMERA_SC_OP_DETACH, 4);
SC_ASSERT_VALUE(CHIMERA_SC_OP_OBJ_CREATE, 5);
SC_ASSERT_VALUE(CHIMERA_SC_OP_OBJ_CLONE_WORKING, 6);
SC_ASSERT_VALUE(CHIMERA_SC_OP_OBJ_DISCARD, 7);
SC_ASSERT_VALUE(CHIMERA_SC_OP_OBJ_READ, 8);
SC_ASSERT_VALUE(CHIMERA_SC_OP_OBJ_WRITE, 9);
SC_ASSERT_VALUE(CHIMERA_SC_OP_OBJ_FLUSH, 10);
SC_ASSERT_VALUE(CHIMERA_SC_OP_OBJ_TRUNCATE, 11);
SC_ASSERT_VALUE(CHIMERA_SC_OP_OBJ_ALLOCATE, 12);
SC_ASSERT_VALUE(CHIMERA_SC_OP_OBJ_PUNCH, 13);
SC_ASSERT_VALUE(CHIMERA_SC_OP_OBJ_SEAL, 14);
SC_ASSERT_VALUE(CHIMERA_SC_OP_OBJ_STAT, 15);
SC_ASSERT_VALUE(CHIMERA_SC_OP_OBJ_MAP, 16);
SC_ASSERT_VALUE(CHIMERA_SC_OP_NS_GET, 17);
SC_ASSERT_VALUE(CHIMERA_SC_OP_NS_SCAN, 18);
SC_ASSERT_VALUE(CHIMERA_SC_OP_NS_TXN, 19);
SC_ASSERT_VALUE(CHIMERA_SC_OP_NS_WATCH, 20);
SC_ASSERT_VALUE(CHIMERA_SC_OP_NS_UNWATCH, 21);
SC_ASSERT_VALUE(CHIMERA_SC_OP_RECALL, 22);
SC_ASSERT_VALUE(CHIMERA_SC_OP_RECALL_WAIT, 23);
SC_ASSERT_VALUE(CHIMERA_SC_OP_COUNT, 23);

/* ---- delta kinds and NS_TXN parts ---- */
SC_ASSERT_VALUE(CHIMERA_SC_DELTA_CREATE, 1);
SC_ASSERT_VALUE(CHIMERA_SC_DELTA_REPLACE, 2);
SC_ASSERT_VALUE(CHIMERA_SC_DELTA_DELETE, 3);
SC_ASSERT_VALUE(CHIMERA_SC_DELTA_SEAL, 4);
SC_ASSERT_VALUE(CHIMERA_SC_DELTA_RETAG, 5);
SC_ASSERT_VALUE(CHIMERA_SC_DELTA_RECALL_HINT, 6);
SC_ASSERT_VALUE(CHIMERA_SC_DELTA_HOLD, 7);
SC_ASSERT_VALUE(CHIMERA_SC_DELTA_UNHOLD, 8);
SC_ASSERT_VALUE(CHIMERA_SC_DELTA_KIND_COUNT, 8);

SC_ASSERT_VALUE(CHIMERA_SC_TXN_COMPARE, 1);
SC_ASSERT_VALUE(CHIMERA_SC_TXN_PUT, 2);
SC_ASSERT_VALUE(CHIMERA_SC_TXN_DELETE, 3);
SC_ASSERT_VALUE(CHIMERA_SC_TXN_DELTA, 4);

/* ---- object model vocabularies ---- */
SC_ASSERT_VALUE(CHIMERA_SC_OBJ_OPEN, 1);
SC_ASSERT_VALUE(CHIMERA_SC_OBJ_SEALED, 2);
SC_ASSERT_VALUE(CHIMERA_SC_OBJ_STUBBED, 3);

SC_ASSERT_VALUE(CHIMERA_SC_WRITE_BUFFERED, 0);
SC_ASSERT_VALUE(CHIMERA_SC_WRITE_DURABLE, 1);

SC_ASSERT_VALUE(CHIMERA_SC_ORIGIN_UNSET, 0);
SC_ASSERT_VALUE(CHIMERA_SC_ORIGIN_NFS3, 1);
SC_ASSERT_VALUE(CHIMERA_SC_ORIGIN_NFS4, 2);
SC_ASSERT_VALUE(CHIMERA_SC_ORIGIN_SMB, 3);
SC_ASSERT_VALUE(CHIMERA_SC_ORIGIN_S3, 4);
SC_ASSERT_VALUE(CHIMERA_SC_ORIGIN_FUSE, 5);
SC_ASSERT_VALUE(CHIMERA_SC_ORIGIN_OTHER, 6);

SC_ASSERT_VALUE(CHIMERA_SC_QD_OWNER, 1U);
SC_ASSERT_VALUE(CHIMERA_SC_QD_GROUP, 2U);
SC_ASSERT_VALUE(CHIMERA_SC_QD_PROJECT, 4U);

SC_ASSERT_VALUE(CHIMERA_SC_CHECKSUM_NONE, 0);
SC_ASSERT_VALUE(CHIMERA_SC_CHECKSUM_MD5, 1);
SC_ASSERT_VALUE(CHIMERA_SC_CHECKSUM_PRIVATE, 256);
SC_ASSERT_VALUE(CHIMERA_SC_CHECKSUM_MAX, 64);

SC_ASSERT_VALUE(CHIMERA_SC_RETENTION_NONE, 0);
SC_ASSERT_VALUE(CHIMERA_SC_RETENTION_LOCKED, 1);

SC_ASSERT_VALUE(CHIMERA_SC_PROTECT_REPLICA, 1);
SC_ASSERT_VALUE(CHIMERA_SC_PROTECT_EC, 2);

SC_ASSERT_VALUE(CHIMERA_SC_SEG_DATA, 1);
SC_ASSERT_VALUE(CHIMERA_SC_SEG_HOLE, 2);
SC_ASSERT_VALUE(CHIMERA_SC_SEG_UNWRITTEN, 3);

SC_ASSERT_VALUE(CHIMERA_SC_CAP_SPOKES, 1ULL << 0);
SC_ASSERT_VALUE(CHIMERA_SC_CAP_PROTECTION_CLASSES, 1ULL << 1);
SC_ASSERT_VALUE(CHIMERA_SC_CAP_GROUP_RECALL, 1ULL << 2);
SC_ASSERT_VALUE(CHIMERA_SC_CAP_ESCROW_QUOTA, 1ULL << 3);

/* ---- enumerated values travel in uint32_t fields ----
 * The build also compiles this file with -fshort-enums (sc_header_short_enums
 * in CMakeLists.txt), where an enum-typed field would shrink and fail here. */
_Static_assert(sizeof(((struct chimera_sc_request *) 0)->opcode) == 4, "uint32_t field");
_Static_assert(sizeof(((struct chimera_sc_request *) 0)->status) == 4, "uint32_t field");
_Static_assert(sizeof(((struct chimera_sc_delta *) 0)->kind) == 4, "uint32_t field");
_Static_assert(sizeof(((struct chimera_sc_obj_meta *) 0)->state) == 4, "uint32_t field");
_Static_assert(sizeof(((struct chimera_sc_stamps *) 0)->origin) == 4, "uint32_t field");

/* ---- layout ----
 * Every ABI structure is pinned on LP64.  The four with nested or anonymous
 * members are pinned by hand; the rest were generated from the compiler's
 * own sizeof/offsetof (one assertion per member) and must be regenerated,
 * with a CHIMERA_SC_ABI_VERSION bump, whenever a layout changes. */
_Static_assert(sizeof(struct chimera_sc_obj_meta) == 208, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_meta, id) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_meta, state) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_meta, protection_class) == 20, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_meta, size) == 24, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_meta, allocated_bytes) == 32, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_meta, checksum) == 40, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_meta, checksum.len) == 44, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_meta, checksum.bytes) == 48, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_meta, stamps) == 112, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_meta, retention) == 168, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_meta, retention.mode) == 176, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_meta, spoke_marker) == 184, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_meta, change) == 192, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_meta, refs) == 200, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_meta, held) == 204, "ABI layout");

#if UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(struct chimera_sc_delta) == 80, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_delta, kind) == 0, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_delta, create) == 8, "ABI layout (LP64)");
_Static_assert(sizeof(struct chimera_sc_request) == 280, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_request, opcode) == 0, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_request, status) == 4, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_request, caller) == 8, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_request, complete) == 32, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_request, caller_private) == 40, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_request, backend_private) == 48, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_request, attach) == 56, "ABI layout (LP64)");
_Static_assert(sizeof(struct chimera_sc_backend) == 64, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_backend, abi_version) == 0, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_backend, name) == 8, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_backend, init) == 16, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_backend, destroy) == 24, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_backend, thread_init) == 32, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_backend, thread_destroy) == 40, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_backend, dispatch) == 48, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_backend, set_upcalls) == 56, "ABI layout (LP64)");
_Static_assert(sizeof(struct chimera_sc_upcalls) == 8, "ABI layout (LP64)");

/* Generated: one assertion per member of every other ABI structure. */
_Static_assert(sizeof(struct chimera_sc_version) == 8, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_version, major) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_version, minor) == 2, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_version, patch) == 4, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_obj_id) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_id, b) == 0, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_node_id) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_node_id, b) == 0, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_caller) == 24, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_caller, node) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_caller, fence_epoch) == 16, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_extent) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_extent, offset) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_extent, length) == 8, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_quota_domains) == 32, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_quota_domains, owner) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_quota_domains, group) == 8, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_quota_domains, project) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_quota_domains, valid) == 24, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_quota_domains, reserved) == 28, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_stamps) == 56, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_stamps, policy_set) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_stamps, quota_domains) == 8, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_stamps, owner_principal) == 40, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_stamps, origin) == 48, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_stamps, reserved) == 52, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_protection_class) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_protection_class, id) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_protection_class, kind) == 4, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_protection_class, n) == 8, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_protection_class, k) == 10, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_protection_class, m) == 12, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_protection_class, reserved) == 14, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_limits) == 32, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_limits, max_txn_ops) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_limits, max_key_len) == 4, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_limits, max_value_len) == 8, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_limits, max_io) == 12, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_limits, max_watch) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_limits, max_recall_ids) == 20, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_limits, lease_ns) == 24, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_bytes) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_bytes, data) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_bytes, len) == 8, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_bytes, reserved) == 12, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_record) == 40, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_record, key) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_record, value) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_record, revision) == 32, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_compare) == 24, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_compare, key) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_compare, revision) == 16, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_cursor) == 32, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_cursor, b) == 0, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_segment) == 24, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_segment, extent) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_segment, kind) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_segment, reserved) == 20, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_delta_create) == 72, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_delta_create, obj) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_delta_create, stamps) == 16, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_delta_replace) == 32, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_delta_replace, old_obj) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_delta_replace, new_obj) == 16, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_delta_delete) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_delta_delete, obj) == 0, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_delta_seal) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_delta_seal, obj) == 0, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_delta_retag) == 56, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_delta_retag, obj) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_delta_retag, policy_set) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_delta_retag, quota_domains) == 24, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_delta_recall_hint) == 24, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_delta_recall_hint, ids) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_delta_recall_hint, group) == 8, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_delta_recall_hint, nids) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_delta_recall_hint, reserved) == 20, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_delta_hold) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_delta_hold, obj) == 0, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_attach_args) == 96, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_attach_args, want) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_attach_args, pclasses) == 8, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_attach_args, pclasses_cap) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_attach_args, r_npclasses) == 20, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_attach_args, r_version) == 24, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_attach_args, r_caps) == 32, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_attach_args, r_fence_epoch) == 40, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_attach_args, r_lease_ns) == 48, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_attach_args, r_durability_epoch) == 56, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_attach_args, r_limits) == 64, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_renew_args) == 8, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_renew_args, r_lease_ns) == 0, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_fence_args) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_fence_args, node) == 0, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_detach_args) == 4, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_detach_args, reserved) == 0, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_obj_create_args) == 72, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_create_args, stamps) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_create_args, r_id) == 56, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_obj_clone_working_args) == 40, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_clone_working_args, sealed_id) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_clone_working_args, r_id) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_clone_working_args, r_ticket) == 32, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_obj_discard_args) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_discard_args, id) == 0, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_obj_read_args) == 72, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_read_args, id) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_read_args, extent) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_read_args, iov) == 32, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_read_args, niov) == 40, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_read_args, r_niov) == 44, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_read_args, r_length) == 48, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_read_args, r_ticket) == 56, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_read_args, r_eof) == 64, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_read_args, reserved) == 68, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_obj_write_args) == 64, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_write_args, id) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_write_args, offset) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_write_args, iov) == 24, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_write_args, niov) == 32, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_write_args, level) == 36, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_write_args, r_level) == 40, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_write_args, reserved) == 44, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_write_args, r_length) == 48, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_write_args, r_durability_epoch) == 56, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_obj_flush_args) == 40, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_flush_args, id) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_flush_args, extent) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_flush_args, r_durability_epoch) == 32, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_obj_space_args) == 48, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_space_args, id) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_space_args, extent) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_space_args, level) == 32, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_space_args, r_level) == 36, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_space_args, r_durability_epoch) == 40, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_obj_seal_args) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_seal_args, id) == 0, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_obj_stat_args) == 224, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_stat_args, id) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_stat_args, r_meta) == 16, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_obj_map_args) == 56, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_map_args, id) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_map_args, extent) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_map_args, segs) == 32, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_map_args, segs_cap) == 40, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_map_args, r_nsegs) == 44, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_map_args, r_more) == 48, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_map_args, reserved) == 52, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_ns_get_args) == 40, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_ns_get_args, key) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_ns_get_args, r_value) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_ns_get_args, r_revision) == 32, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_ns_scan_args) == 104, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_ns_scan_args, prefix) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_ns_scan_args, cursor) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_ns_scan_args, recs) == 48, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_ns_scan_args, recs_cap) == 56, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_ns_scan_args, r_nrecs) == 60, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_ns_scan_args, r_next_cursor) == 64, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_ns_scan_args, r_eof) == 96, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_ns_scan_args, reserved) == 100, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_ns_txn_args) == 64, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_ns_txn_args, compares) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_ns_txn_args, puts) == 8, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_ns_txn_args, deletes) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_ns_txn_args, deltas) == 24, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_ns_txn_args, ncompares) == 32, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_ns_txn_args, nputs) == 36, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_ns_txn_args, ndeletes) == 40, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_ns_txn_args, ndeltas) == 44, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_ns_txn_args, r_revision) == 48, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_ns_txn_args, r_failed_part) == 56, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_ns_txn_args, r_failed_index) == 60, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_ns_watch_args) == 24, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_ns_watch_args, prefix) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_ns_watch_args, r_watch_id) == 16, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_ns_unwatch_args) == 8, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_ns_unwatch_args, watch_id) == 0, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_recall_args) == 32, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_recall_args, ids) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_recall_args, group) == 8, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_recall_args, r_ticket) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_recall_args, nids) == 24, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_recall_args, reserved) == 28, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_recall_wait_args) == 8, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_recall_wait_args, ticket) == 0, "ABI layout");
#endif /* if UINTPTR_MAX == UINT64_MAX */

static void
test_negotiate(void)
{
    struct chimera_sc_version want = { 1, 3, 0 };
    struct chimera_sc_version have = { 1, 1, 7 };
    struct chimera_sc_version out  = { 0, 0, 0 };

    /* Equal majors: minor is the lower of the two, patch is the backend's. */
    assert(chimera_sc_version_negotiate(&want, &have, &out) == 0);
    assert(out.major == 1 && out.minor == 1 && out.patch == 7);

    want.minor = 0;
    assert(chimera_sc_version_negotiate(&want, &have, &out) == 0);
    assert(out.minor == 0);

    /* Identical versions. */
    have = want;
    assert(chimera_sc_version_negotiate(&want, &have, &out) == 0);
    assert(out.major == want.major && out.minor == want.minor);

    /* A major mismatch refuses, in either direction. */
    have.major = 2;
    assert(chimera_sc_version_negotiate(&want, &have, &out) == -1);
    want.major = 3;
    assert(chimera_sc_version_negotiate(&want, &have, &out) == -1);
} /* test_negotiate */

int
main(void)
{
    test_negotiate();
    printf("sc_header_test: ok\n");
    return 0;
} /* main */
