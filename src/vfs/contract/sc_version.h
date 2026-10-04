// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * Chimera storage contract: versions.
 *
 * Two version concepts, deliberately separate:
 *
 *   - CHIMERA_SC_ABI_VERSION covers the C layout of the request, argument
 *     and descriptor structures in these headers.  A backend stamps it into
 *     struct chimera_sc_backend.abi_version; the consumer (the namespace
 *     module, the conformance harness) refuses a backend built against a
 *     different layout at registration, as chimera_vfs_register() does for
 *     the module SDK.  Bumped on any incompatible layout change.
 *
 *   - The contract semantic version (contract D12) is negotiated at ATTACH.
 *     A major mismatch refuses to attach with CHIMERA_SC_E_VERSION; the
 *     negotiated minor is the lower of the two; patch is informational.
 *     Optional behaviour within a version is a declared capability
 *     (sc_types.h); mandatory behaviour never is.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

#define CHIMERA_SC_ABI_VERSION    1

#define CHIMERA_SC_CONTRACT_MAJOR 1
#define CHIMERA_SC_CONTRACT_MINOR 0
#define CHIMERA_SC_CONTRACT_PATCH 0

struct chimera_sc_version {
    uint16_t major;
    uint16_t minor;
    uint32_t patch;
};

/* Negotiate the contract version between what the caller wants and what the
 * backend has.  Returns 0 and fills *out on success, -1 on a major mismatch.
 * Both sides and the conformance harness use this one rule. */
static inline int
chimera_sc_version_negotiate(
    const struct chimera_sc_version *want,
    const struct chimera_sc_version *have,
    struct chimera_sc_version       *out)
{
    if (want->major != have->major) {
        return -1;
    }
    out->major = want->major;
    out->minor = want->minor < have->minor ? want->minor : have->minor;
    out->patch = have->patch;
    return 0;
} /* chimera_sc_version_negotiate */

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
