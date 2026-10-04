// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * Chimera storage contract, version 1: the C interface.
 *
 * This is the interface between Chimera's namespace module and its backend
 * storage facade, as specified by docs/product/storage-contract.md (draft
 * 0.2).  It is not the VFS module SDK (src/vfs/sdk/): the SDK is
 * POSIX-shaped and serves development, test and gateway modules (contract
 * D1).  Interpretive positions, provisional additions (S1-S7, S10-S14) and
 * deferred items (S8, S9) are recorded in
 * docs/product/storage-contract-interface.md; each appears below as a
 * PROVISIONAL marker.
 *
 * Boundary rules for anything added here:
 *   - types, constants and inline helpers of ten lines or fewer only;
 *   - no includes beyond libc (<stdint.h>, <stddef.h>) and these headers.
 *     libevpl and prometheus types are forward-declared, never included.
 *     The sc_selfcontained build target enforces this;
 *   - valid C11 and valid C++: tagged structures only inside anonymous
 *     unions, and no file-scope _Static_assert.
 *
 * Summary (details in the headers named):
 *   - Versions (sc_version.h): CHIMERA_SC_ABI_VERSION guards the C layout
 *     and is refused on mismatch at registration; the contract semantic
 *     version is negotiated at ATTACH (D12).
 *   - Threading (sc_backend.h): dispatch() never blocks; complete() runs on
 *     the dispatching thread; many requests may be in flight per thread.
 *     One backend instance per Chimera node, in process (PROVISIONAL S10).
 *   - Requests (sc_request.h): inputs are caller-owned until complete();
 *     returned bytes live until complete() returns (S11); the backend does
 *     not touch a request after completing it.  NS_TXN checks per-delta
 *     preconditions in order and invariants and quota net.
 *   - Objects (sc_types.h): at most one reference per object in v1.0; only
 *     never-referenced objects can be held (S13); a released object is
 *     E_NOT_FOUND to every op; object count is charged on the first
 *     reference and released on the last (S12).
 *   - Errors (sc_error.h): own numbering; section 11 mappings.
 *   - ABI: no implicit padding (sc_selfcontained builds with -Wpadded on
 *     LP64), reserved fields must be zero, and enum values are carried in
 *     uint32_t fields.
 */

#include "sc_version.h"
#include "sc_error.h"
#include "sc_types.h"
#include "sc_request.h"
#include "sc_backend.h"
