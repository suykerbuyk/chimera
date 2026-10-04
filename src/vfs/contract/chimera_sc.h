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
 * D1).  Interpretive positions, provisional additions (S1-S7, S10-S13) and
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
 * Versions: sc_version.h.  Errors: sc_error.h.  Types, the object
 * lifecycle and quota rules: sc_types.h.  Operations and ownership:
 * sc_request.h.  Descriptor, threading and deployment: sc_backend.h.
 */

#include "sc_version.h"
#include "sc_error.h"
#include "sc_types.h"
#include "sc_request.h"
#include "sc_backend.h"
