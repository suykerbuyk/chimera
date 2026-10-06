// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * Chimera media interface: core types.
 *
 * Addressing.  The media interface presents a backing store as one or more
 * linear address spaces of fixed-size blocks (CHIMERA_MEDIA_BLOCK_SIZE, 4 KiB
 * in v1).  Callers address a space by a linear block index
 * (chimera_media_block_t), starting at 0; a provider converts that to a byte
 * offset internally (block << CHIMERA_MEDIA_BLOCK_SHIFT).  The block size is
 * fixed in v1 but reported in chimera_media_space_info so callers do not bake
 * it in.
 *
 * Address spaces (LAY-4, contract O15, HA-6).  A caller opens a space and
 * receives an opaque struct chimera_media_space *.  A backing store may carry
 * more than one space, so a caller never assumes a single address space:
 * "one address space per namespace" (O15) and real NVMe namespaces on a
 * multi-drive node (HA-6) are reachable later by opening additional spaces,
 * with no change to the read/write/flush callers.  Whether each namespace
 * gets its own space is deferred (O15); the prototype loopback provider backs
 * a single space with the whole file.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

/* Fixed logical block size.  v1 is 4 KiB; a block address is an index in
 * these units, a byte offset is (block << CHIMERA_MEDIA_BLOCK_SHIFT). */
#define CHIMERA_MEDIA_BLOCK_SIZE  4096u
#define CHIMERA_MEDIA_BLOCK_SHIFT 12

/* A linear block index within one space. */
typedef uint64_t chimera_media_block_t;

/* Durability flag for a write.  CHIMERA_MEDIA_WRITE_FUA requests that the
 * data is durable before the completion fires (force unit access); 0 permits
 * the backing store to defer it until a flush.  Passed as the write's int
 * fua argument, it maps to libevpl's evpl_block_write(..., int sync, ...). */
enum chimera_media_write_flag {
    CHIMERA_MEDIA_WRITE_FUA = 1,
};

/* An opened address space.  Opaque to callers; each provider defines its own
 * concrete type. */
struct chimera_media_space;

/* The geometry of an opened space.  No implicit padding on LP64. */
struct chimera_media_space_info {
    uint64_t nblocks;       /* size of the space, in blocks */
    uint32_t block_size;    /* CHIMERA_MEDIA_BLOCK_SIZE */
    uint32_t max_io_blocks; /* largest read/write, in blocks */
};

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
