// SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
//
// SPDX-License-Identifier: LGPL-2.1-only

#pragma once

/*
 * Chimera storage contract, version 1: the C interface.
 *
 * This is the interface between Chimera's namespace module and its backend
 * storage facade.  It is not the VFS module SDK (src/vfs/sdk/): the SDK is
 * POSIX-shaped and serves development, test and gateway modules.
 * Interpretive positions, provisional additions and deferred items each
 * appear below as a PROVISIONAL marker.
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
