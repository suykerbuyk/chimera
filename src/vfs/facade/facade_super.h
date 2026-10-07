// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * Backend-facade on-media format (v2): the superblock, the A/B free-set +
 * object-table checkpoint, and the CRC helper.  See facade_log.h for the
 * durability log that layers on top.
 *
 * Layout on a formatted media space:
 *   block 0                               superblock (FUA, this header)
 *   [ckpt_a_block, +ckpt_nblocks)         checkpoint slot A
 *   [ckpt_b_block, +ckpt_nblocks)         checkpoint slot B
 *   [log_block,    +log_nblocks)          durability log (facade_log.h)
 *   [data_first,   data_last)             data region
 *
 * Each checkpoint slot holds a header, then the free-set records, then the
 * object records (id -> hold flag + its extents).  The two slots ping-pong
 * (A/B) so a compaction is crash-atomic: the inactive slot is written fully
 * durable, then the superblock flips `active_ckpt` to name it (FUA), then the
 * log is reset.  A crash mid-write of the inactive slot leaves the superblock
 * naming the old, still-valid slot.  The slot's CRC covers the WHOLE slot
 * (header + both sections), so a torn or bit-rotten data section of the active
 * slot is detected, not trusted.
 *
 * v1 (block-allocator-and-identity) had a single checkpoint of free extents
 * only and no log; v2 is refused-not-reformatted by an older/newer reader
 * (facade_alloc_open -> FACADE_E_FORMAT), never read as blank.  The layout is a
 * single-host LP64 prototype image copied verbatim; a portable wire encoding is
 * deferred, and the version field is what makes it landable later.
 */

#include <stdint.h>
#include "media_types.h"

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

/* "FACALLOC" as a little-endian u64. */
#define FACADE_SUPER_MAGIC    0x434F4C4C41434146ULL
/* "FACCKPT2" as a little-endian u64. */
#define FACADE_CKPT_MAGIC     0x32545049434341F0ULL
/*
 * v3 (object-ops-v1): the durable object record carries the full object state
 * (open/sealed), logical size, change counter, stamps and retention, and each
 * extent carries an `owned` bit (0 = borrowed from a clone origin), so the 4.2
 * states, 5.1 metadata and the D2 working-version clone survive a crash.  A v2
 * image (free set + object table of {id,flags,extents} only) is refused, not
 * read as blank (facade_alloc_open -> FACADE_E_FORMAT).
 */
#define FACADE_FORMAT_VERSION 3u

#define FACADE_SUPER_BLOCK    0u /* superblock lba */
#define FACADE_SUPER_SIZE     CHIMERA_MEDIA_BLOCK_SIZE /* one block */

/*
 * The superblock image.  Copied verbatim into a zeroed 4 KiB block; crc32
 * covers the whole block with the crc32 field zeroed.  u64s first, then u32s,
 * so there is no implicit padding on LP64.
 */
struct facade_super {
    uint64_t magic;          /* FACADE_SUPER_MAGIC */
    uint64_t nblocks;        /* total blocks in the space */
    uint64_t data_first;     /* first allocatable block */
    uint64_t data_last;      /* one past the last allocatable block */
    uint64_t ckpt_a_block;   /* first block of checkpoint slot A */
    uint64_t ckpt_b_block;   /* first block of checkpoint slot B */
    uint64_t ckpt_nblocks;   /* size of EACH checkpoint slot, in blocks */
    uint64_t log_block;      /* first block of the durability log */
    uint64_t log_nblocks;    /* log size, in blocks (== max live records) */
    uint64_t checkpoint_seq; /* log seq the active checkpoint reflects */
    uint64_t power_loss_generation; /* equality token; bumped on crash recovery */
    uint64_t id_nonce;       /* high 64 bits of every object id */
    uint64_t id_floor;       /* persisted id high-water (fua, ahead of issue) */
    uint32_t version;        /* FACADE_FORMAT_VERSION */
    uint32_t block_size;     /* CHIMERA_MEDIA_BLOCK_SIZE */
    uint32_t active_ckpt;    /* 0 => slot A, 1 => slot B */
    uint32_t clean;          /* 1 => clean shutdown; 0 => run recovery */
    uint32_t crc32;          /* over the 4 KiB block, this field zeroed */
    uint32_t reserved;       /* must be zero */
};

/* Header at the start of each checkpoint slot.  crc32 covers the whole slot
 * (header + free + object sections) with the crc32 field zeroed. */
struct facade_ckpt_hdr {
    uint64_t magic;          /* FACADE_CKPT_MAGIC */
    uint64_t generation;     /* checkpoint generation; the higher valid slot wins */
    uint64_t checkpoint_seq; /* log seq this checkpoint reflects */
    uint64_t n_free_ext;     /* free-extent records that follow */
    uint64_t n_objs;         /* object records that follow the free records */
    uint64_t content_blocks; /* blocks the header + both sections occupy (CRC span) */
    uint32_t crc32;          /* whole-slot CRC (over content_blocks), this field zeroed */
    uint32_t reserved;       /* must be zero */
};

/* One on-media free-extent record in the checkpoint. */
struct facade_ckpt_ext {
    uint64_t start;   /* first free block */
    uint64_t nblocks; /* run length, in blocks */
};

/*
 * One on-media object record header; followed by nextents facade_ckpt_objext.
 * The checkpoint and the durability log share this shape (the log OBJ record is
 * this header + its extents); it carries everything OBJ_STAT reports and every
 * state the 4.2 machine and the D2 clone must survive a crash with.  u64s first,
 * then u32s, so there is no implicit padding on the single-host LP64 image.
 */
struct facade_obj_rec {
    uint8_t  obj_id[16];
    uint64_t size;               /* logical byte length (5.1) */
    uint64_t change;             /* advances on every data/metadata change (5.1) */
    uint64_t policy_set;         /* stamps (D7/D13) */
    uint64_t qd_owner;
    uint64_t qd_group;
    uint64_t qd_project;
    uint64_t owner_principal;
    uint64_t retention_until_ns; /* D8 */
    uint32_t flags;              /* FACADE_OBJ_* */
    uint32_t state;              /* enum chimera_sc_obj_state (OPEN/SEALED) */
    uint32_t qd_valid;           /* CHIMERA_SC_QD_* */
    uint32_t origin;             /* enum chimera_sc_origin */
    uint32_t retention_mode;     /* CHIMERA_SC_RETENTION_* */
    uint32_t nextents;
};

/* One extent of an object record (checkpoint and log share this shape). */
struct facade_ckpt_objext {
    uint64_t logical_block;
    uint64_t phys_block;
    uint64_t nblocks;
    uint32_t kind;     /* CHIMERA_SC_SEG_DATA / _UNWRITTEN */
    uint32_t owned;    /* 1 = this object allocated the blocks; 0 = borrowed (clone) */
};

/* Object-record flags. */
#define FACADE_OBJ_HELD 0x1u

/* IEEE CRC-32 (bitwise). */
uint32_t
facade_crc32(
    const void *data,
    uint32_t    len);

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
