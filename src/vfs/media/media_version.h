// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * Chimera media interface: versions.
 *
 * Two version concepts, deliberately separate, mirroring the storage
 * contract (src/vfs/contract/sc_version.h):
 *
 *   - CHIMERA_MEDIA_ABI_VERSION covers the C layout of the provider
 *     descriptor and the types in these headers.  A provider stamps it into
 *     struct chimera_media_provider.abi_version; the consumer refuses a
 *     provider built against a different layout at chimera_media_open(), as
 *     chimera_vfs_register() does for the module SDK.  Bumped on any
 *     incompatible layout change, with the test assertions regenerated in the
 *     same commit.
 *
 *   - The media semantic version is informational for v1: one provider vtable,
 *     one block size.  It is carried so a later revision can negotiate between
 *     a consumer and a provider the way the contract negotiates at ATTACH.
 *     A major mismatch is a refusal; the negotiated minor is the lower of the
 *     two; patch is informational.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

#define CHIMERA_MEDIA_ABI_VERSION 1

#define CHIMERA_MEDIA_MAJOR       1
#define CHIMERA_MEDIA_MINOR       0
#define CHIMERA_MEDIA_PATCH       0

struct chimera_media_version {
    uint16_t major;
    uint16_t minor;
    uint32_t patch;
};

/* Negotiate the media version between what the caller wants and what the
 * provider has.  Returns 0 and fills *out on success, -1 on a major mismatch.
 * Both sides use this one rule. */
static inline int
chimera_media_version_negotiate(
    const struct chimera_media_version *want,
    const struct chimera_media_version *have,
    struct chimera_media_version       *out)
{
    if (want->major != have->major) {
        return -1;
    }
    out->major = want->major;
    out->minor = want->minor < have->minor ? want->minor : have->minor;
    out->patch = have->patch;
    return 0;
} /* chimera_media_version_negotiate */

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
