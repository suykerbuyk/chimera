// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Compile-time and version-negotiation checks for the storage contract
 * interface.
 *
 * Every value a backend or consumer can store or send is asserted
 * individually, so renumbering, a collision or a swap fails to build.  The
 * LP64 sizes and offsets pin the C layout: changing any of them is an ABI
 * change and needs a CHIMERA_SC_ABI_VERSION bump (update the assertions in
 * the same commit).
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

/* ---- layout: sizes that hold on every target ---- */
_Static_assert(sizeof(struct chimera_sc_obj_id) == 16, "object ids are 128 bits (contract 4.1)");
_Static_assert(sizeof(struct chimera_sc_node_id) == 16, "node ids are 128 bits");
_Static_assert(sizeof(struct chimera_sc_quota_domains) == 32, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_stamps) == 56, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_stamps, quota_domains) == 8, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_stamps, owner_principal) == 40, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_stamps, origin) == 48, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_caller) == 24, "ABI layout");
_Static_assert(sizeof(struct chimera_sc_obj_meta) == 208, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_meta, state) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_meta, size) == 24, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_meta, checksum) == 40, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_meta, stamps) == 112, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_meta, retention) == 168, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_meta, spoke_marker) == 184, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_meta, refs) == 200, "ABI layout");
_Static_assert(offsetof(struct chimera_sc_obj_meta, held) == 204, "ABI layout");

/* ---- layout: pointer-bearing structures, pinned for LP64 ---- */
#if UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(struct chimera_sc_bytes) == 16, "ABI layout (LP64)");
_Static_assert(sizeof(struct chimera_sc_record) == 40, "ABI layout (LP64)");
_Static_assert(sizeof(struct chimera_sc_delta) == 80, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_delta, create) == 8, "ABI layout (LP64)");
_Static_assert(sizeof(struct chimera_sc_attach_args) == 96, "ABI layout (LP64)");
_Static_assert(sizeof(struct chimera_sc_request) == 280, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_request, opcode) == 0, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_request, status) == 4, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_request, caller) == 8, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_request, complete) == 32, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_request, caller_private) == 40, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_request, backend_private) == 48, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_request, attach) == 56, "ABI layout (LP64)");
_Static_assert(sizeof(struct chimera_sc_backend) == 64, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_backend, name) == 8, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_backend, init) == 16, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_backend, dispatch) == 48, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_sc_backend, set_upcalls) == 56, "ABI layout (LP64)");
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
