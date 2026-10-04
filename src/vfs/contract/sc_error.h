// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * Chimera storage contract: errors.
 *
 * Contract errors have their own numbering; they are not errno values and
 * not enum chimera_vfs_error.  The namespace module (E4) maps them to each
 * protocol.  The mappings Chimera owes (contract section 11) are:
 *
 *   backend error        NFSv3            NFSv4            SMB                     S3
 *   QUOTA_EXCEEDED       NFS3ERR_DQUOT    NFS4ERR_DQUOT    STATUS_QUOTA_EXCEEDED   403 QuotaExceeded
 *   NO_SPACE             NFS3ERR_NOSPC    NFS4ERR_NOSPC    STATUS_DISK_FULL        507 InsufficientCapacity
 *   RECALL_PENDING       NFS3ERR_JUKEBOX  NFS4ERR_DELAY    interim STATUS_PENDING  GET 403 InvalidObjectState
 *   RETENTION_LOCKED     NFS3ERR_ACCES    NFS4ERR_ACCESS   STATUS_ACCESS_DENIED    403 AccessDenied
 *   KEY_UNAVAILABLE      NFS3ERR_ACCES    NFS4ERR_ACCESS   STATUS_ACCESS_DENIED    403 AccessDenied
 *   DATA_UNRECOVERABLE   NFS3ERR_IO       NFS4ERR_IO       STATUS_DATA_ERROR       500 InternalError
 *
 * FUSE returns EIO for DATA_UNRECOVERABLE.  Cross-device rename (contract
 * D7) is not a backend error: Chimera refuses it before any transaction, so
 * it has no value here.
 *
 * There is no "cursor invalid" error: scan cursors are stable across
 * concurrent changes (contract 6.2).  A malformed or foreign cursor is
 * CHIMERA_SC_E_INVALID, a caller bug.
 */

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

enum chimera_sc_error {
    CHIMERA_SC_OK                   = 0,

    /* Contract section 11 domain errors.  Values are frozen. */
    CHIMERA_SC_E_QUOTA_EXCEEDED     = 1,  /* D13 byte or object-count quota */
    CHIMERA_SC_E_NO_SPACE           = 2,  /* D13 pool-wide space exhaustion */
    CHIMERA_SC_E_RECALL_PENDING     = 3,  /* D6: object stubbed; see r_ticket */
    CHIMERA_SC_E_RETENTION_LOCKED   = 4,  /* D8 retention or immutability lock */
    CHIMERA_SC_E_KEY_UNAVAILABLE    = 5,  /* D9 encryption key withdrawn */
    CHIMERA_SC_E_DATA_UNRECOVERABLE = 6,  /* D9 verification failed, no repair */

    /* Generic errors the operations need.  PROVISIONAL (S1). */
    CHIMERA_SC_E_FENCED             = 32, /* revoked or expired fence epoch (D10) */
    CHIMERA_SC_E_VERSION            = 33, /* contract major mismatch at ATTACH (D12) */
    CHIMERA_SC_E_NOT_FOUND          = 34, /* unknown or released object; unknown key or ticket */
    CHIMERA_SC_E_STATE              = 35, /* object, reference or hold state forbids the op */
    CHIMERA_SC_E_COMPARE_FAILED     = 36, /* NS_TXN compare mismatch (D4) */
    CHIMERA_SC_E_INVALID            = 37, /* malformed request, foreign cursor, over a limit */
    CHIMERA_SC_E_NOT_SUPPORTED      = 38, /* op needs a capability the backend did not declare */
    CHIMERA_SC_E_IO                 = 39, /* transient media failure; retryable */
};

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
