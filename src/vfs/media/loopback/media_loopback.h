// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * The loopback-file media provider: the prototype implementation of the
 * media interface (chimera_media.h) for the E14 backend-facade prototype.
 *
 * It presents a regular file of at least 1 GiB as one linear address space of
 * 4 KiB blocks, built on libevpl's block layer with the PREAD protocol pinned
 * by name (EVPL_BLOCK_PROTOCOL_PREAD): buffered pread/pwrite/fsync on a
 * libevpl service thread, the only block protocol that runs unprivileged
 * under the local Podman seccomp profile (no O_DIRECT, no io_uring, no root).
 *
 * The backing file is created and sized before it is opened: the PREAD
 * protocol opens O_RDWR with no O_CREAT, so a missing path is created with
 * open(O_CREAT|O_RDWR) + ftruncate and closed before evpl_block_open_device.
 */

#include "chimera_media.h"

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

/* The loopback provider descriptor.  Pass it to chimera_media_open(); the
 * returned pointer has static lifetime. */
const struct chimera_media_provider * chimera_media_loopback(
    void);

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
