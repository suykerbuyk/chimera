// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Compile-time and version-negotiation checks for the storage contract
 * interface.  The static assertions freeze the parts of the ABI that other
 * components and the companion document rely on.
 */

#include <assert.h>
#include <stdio.h>

#include "chimera_sc.h"

_Static_assert(sizeof(struct chimera_sc_obj_id) == 16,
               "object ids are 128 bits (contract 4.1)");
_Static_assert(sizeof(struct chimera_sc_node_id) == 16,
               "node ids are 128 bits");

_Static_assert(CHIMERA_SC_E_QUOTA_EXCEEDED == 1, "frozen error value");
_Static_assert(CHIMERA_SC_E_NO_SPACE == 2, "frozen error value");
_Static_assert(CHIMERA_SC_E_RECALL_PENDING == 3, "frozen error value");
_Static_assert(CHIMERA_SC_E_RETENTION_LOCKED == 4, "frozen error value");
_Static_assert(CHIMERA_SC_E_KEY_UNAVAILABLE == 5, "frozen error value");
_Static_assert(CHIMERA_SC_E_DATA_UNRECOVERABLE == 6, "frozen error value");

_Static_assert(CHIMERA_SC_OP_COUNT == 23, "contract v1.0 has 23 operations");
_Static_assert(CHIMERA_SC_OP_RECALL_WAIT == CHIMERA_SC_OP_COUNT,
               "opcodes are dense, 1..CHIMERA_SC_OP_COUNT");
_Static_assert(CHIMERA_SC_DELTA_KIND_COUNT == 8,
               "contract v1.0 has 8 delta kinds");
_Static_assert(CHIMERA_SC_DELTA_UNHOLD == CHIMERA_SC_DELTA_KIND_COUNT,
               "delta kinds are dense, 1..CHIMERA_SC_DELTA_KIND_COUNT");

_Static_assert(CHIMERA_SC_TXN_COMPARE == 1 && CHIMERA_SC_TXN_PUT == 2 &&
               CHIMERA_SC_TXN_DELETE == 3 && CHIMERA_SC_TXN_DELTA == 4,
               "frozen NS_TXN part values");

_Static_assert(CHIMERA_SC_ABI_VERSION == 1, "ABI version of this layout");
_Static_assert(CHIMERA_SC_CONTRACT_MAJOR == 1, "contract major version");

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
