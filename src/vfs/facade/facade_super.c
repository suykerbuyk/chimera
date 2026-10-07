// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Superblock serialization and CRC for the backend-facade allocator.  See
 * facade_super.h for the on-media format.
 */

#include <string.h>

#include "facade_internal.h"

uint32_t
facade_crc32(
    const void *data,
    uint32_t    len)
{
    const uint8_t *p   = data;
    uint32_t       crc = 0xFFFFFFFFu;
    uint32_t       i;
    int            k;

    for (i = 0; i < len; i++) {
        crc ^= p[i];
        for (k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t) (-(int32_t) (crc & 1)));
        }
    }
    return ~crc;
} /* facade_crc32 */

/*
 * Pack the allocator's current static state into a zeroed 4 KiB superblock
 * image, including the crc.  Every field is taken live from *alloc, so a
 * floor-only extension write preserves the last-persisted checkpoint
 * descriptor (alloc->ckpt_count / alloc->clean) while advancing id_floor.
 */
void
facade_super_pack(
    const struct facade_alloc *alloc,
    void                      *block)
{
    struct facade_super super;

    memset(block, 0, FACADE_SUPER_SIZE);
    memset(&super, 0, sizeof(super));

    super.magic        = FACADE_SUPER_MAGIC;
    super.version      = FACADE_FORMAT_VERSION;
    super.block_size   = CHIMERA_MEDIA_BLOCK_SIZE;
    super.nblocks      = alloc->nblocks;
    super.data_first   = alloc->data_first;
    super.data_last    = alloc->data_last;
    super.ckpt_block   = alloc->ckpt_block;
    super.ckpt_nblocks = alloc->ckpt_nblocks;
    super.id_nonce     = alloc->id_nonce;
    super.id_floor     = alloc->id_floor;
    super.ckpt_count   = alloc->ckpt_count;
    super.clean        = alloc->clean;

    memcpy(block, &super, sizeof(super));
    ((struct facade_super *) block)->crc32 =
        facade_crc32(block, FACADE_SUPER_SIZE);
} /* facade_super_pack */
