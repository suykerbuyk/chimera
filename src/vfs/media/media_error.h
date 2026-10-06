// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * Chimera media interface: errors.
 *
 * The media interface is the backend facade's lower interface, below the
 * storage contract (contract section 3).  It presents a backing store as one
 * or more linear address spaces of fixed 4 KiB blocks.  These errors have
 * their own small numbering; they are not errno values and not
 * enum chimera_sc_error.  A provider reports them through the completion
 * callback; the facade above maps them onto the contract's errors (e.g.
 * E_IO -> CHIMERA_SC_E_IO or, after the facade's own verification fails,
 * CHIMERA_SC_E_DATA_UNRECOVERABLE; E_NO_SPACE -> CHIMERA_SC_E_NO_SPACE).
 *
 * The loopback provider derives E_IO / E_NO_SPACE from the positive errno a
 * libevpl block completion carries (ENOSPC -> E_NO_SPACE, everything else
 * -> E_IO).  E_RANGE and E_INVAL are caller bugs caught before any I/O is
 * submitted.
 */

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

enum chimera_media_error {
    CHIMERA_MEDIA_OK              = 0,

    /* Values are frozen; append, never renumber. */
    CHIMERA_MEDIA_E_IO            = 1, /* transient media failure; retryable */
    CHIMERA_MEDIA_E_NO_SPACE      = 2, /* backing store is full (ENOSPC) */
    CHIMERA_MEDIA_E_RANGE         = 3, /* block range falls outside the space */
    CHIMERA_MEDIA_E_INVAL         = 4, /* bad niov, or request over max_io_blocks */
    CHIMERA_MEDIA_E_NOT_SUPPORTED = 5, /* provider ABI mismatch, or op not offered */
    CHIMERA_MEDIA_E_NODEV         = 6, /* backing store could not be opened */
};

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
