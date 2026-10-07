// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * Backend-facade allocator: the on-media superblock and free-set checkpoint.
 *
 * The allocator keeps one fixed 4 KiB superblock at block 0 of the media
 * space, and a free-set checkpoint region immediately after it.  The
 * superblock carries the static geometry, the object-id instance nonce and the
 * persisted id high-water floor (the H2 fix), and the checkpoint descriptor.
 * It is written with force-unit-access (fua) so the id floor is durable ahead
 * of any id being issued.
 *
 * The free-set checkpoint is a condensed list of free extents, written at a
 * clean persist (flush/close) only.  Crash-consistent replay of allocations
 * made since the last checkpoint is media-log-and-recovery's job and is out of
 * scope for this unit; a crash falls back to the last checkpoint (the id floor
 * stays correct regardless, because it is persisted ahead with fua).
 *
 * The layout is a single-host LP64 prototype format: the structure is copied
 * into the block verbatim.  A portable, versioned wire encoding is deferred.
 */

#include <stdint.h>
#include "media_types.h"

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

/* "FACALLOC" as a little-endian u64. */
#define FACADE_SUPER_MAGIC    0x434F4C4C41434146ULL
#define FACADE_FORMAT_VERSION 1u

#define FACADE_SUPER_BLOCK    0u /* superblock lba */
#define FACADE_SUPER_SIZE     CHIMERA_MEDIA_BLOCK_SIZE /* one block */

/*
 * The superblock image.  Copied verbatim into a zeroed 4 KiB block; crc32
 * covers the whole block with the crc32 field zeroed.
 */
struct facade_super {
    uint64_t magic;              /* FACADE_SUPER_MAGIC */
    uint32_t version;      /* FACADE_FORMAT_VERSION */
    uint32_t block_size;   /* CHIMERA_MEDIA_BLOCK_SIZE */
    uint64_t nblocks;      /* total blocks in the space */
    uint64_t data_first;   /* first allocatable block */
    uint64_t data_last;    /* one past the last allocatable block */
    uint64_t ckpt_block;   /* first checkpoint block */
    uint64_t ckpt_nblocks; /* checkpoint region size, in blocks */
    uint64_t id_nonce;     /* high 64 bits of every object id of this format */
    uint64_t id_floor;     /* persisted id high-water (fua, ahead of issue) */
    uint64_t ckpt_count;   /* free-extent records in the checkpoint */
    uint32_t clean;        /* 1 => the checkpoint reflects a consistent state */
    uint32_t crc32;        /* over the 4 KiB block, this field zeroed */
};

/* One on-media free-extent record in the checkpoint region. */
struct facade_ckpt_ext {
    uint64_t start;   /* first free block */
    uint64_t nblocks; /* run length, in blocks */
};

/* IEEE CRC-32 (bitwise), used for the superblock.  Adequate for this
 * one-record-at-a-time path. */
uint32_t
facade_crc32(
    const void *data,
    uint32_t    len);

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
