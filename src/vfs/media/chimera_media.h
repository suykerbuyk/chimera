// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * Chimera media interface -- umbrella header.
 *
 * The media interface is the backend facade's lower interface, below the
 * storage contract (src/vfs/contract/, contract section 3).  It presents a
 * backing store as one or more linear address spaces of fixed 4 KiB blocks
 * and offers asynchronous read, write (with an optional force-unit-access
 * durability flag), flush and close.  The facade (allocation, logging,
 * durability, verification; E14) is built on top of it; this unit defines
 * the interface and one provider, the loopback-file implementation.
 *
 * Boundary rules (mirroring the storage-contract headers):
 *   - These headers include nothing beyond libc (<stdint.h>); libevpl and
 *     prometheus types are forward-declared, never included, so a consumer
 *     can depend on the interface without pulling in those trees.  The
 *     self-containment is proved by the media_selfcontained target and the
 *     include-directive check in CMakeLists.txt.
 *   - A provider is a vtable (media_provider.h) carrying a uint32_t
 *     abi_version.  chimera_media_open() validates it, the way
 *     chimera_vfs_register() validates the module SDK version, before using
 *     the vtable.
 *
 * O15 stays open: the interface keeps per-namespace address spaces reachable
 * (a caller may open more than one space) without committing v1 to them.  The
 * rationale lives in these header comments; there is no separate companion
 * document.
 */

#include <stdint.h>
#include "media_version.h"
#include "media_error.h"
#include "media_types.h"
#include "media_provider.h"

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

/*
 * Open a space through a provider, validating the provider's ABI first.
 *
 * Returns CHIMERA_MEDIA_OK when the provider is accepted and the open has
 * been started: the caller then awaits cb, which reports the opened space or
 * a failure.  Returns CHIMERA_MEDIA_E_NOT_SUPPORTED when the provider's
 * abi_version does not match this header's CHIMERA_MEDIA_ABI_VERSION; in that
 * case cb is NOT called.  The ABI check is synchronous, the way module
 * registration is; only the open itself is asynchronous.
 */
static inline enum chimera_media_error
chimera_media_open(
    const struct chimera_media_provider *provider,
    struct evpl                         *evpl,
    const char                          *uri,
    uint64_t                             min_size,
    struct prometheus_metrics           *metrics,
    chimera_media_open_cb_t              cb,
    void                                *private_data)
{
    if (!provider || provider->abi_version != CHIMERA_MEDIA_ABI_VERSION) {
        return CHIMERA_MEDIA_E_NOT_SUPPORTED;
    }
    provider->open(evpl, uri, min_size, metrics, cb, private_data);
    return CHIMERA_MEDIA_OK;
} /* chimera_media_open */

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
