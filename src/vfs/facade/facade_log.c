// SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * Backend-facade durability log: the on-media record format and its builder.
 * See facade_log.h for the record layout and the two ordering gates
 * (compact only at a transaction boundary; commit only after the data FUA).
 *
 * One record per 4 KiB block, written force-unit-access, so a record recovery
 * reads is durable; a transaction's COMMIT is the last record issued, after the
 * data FUA, so a durable COMMIT implies the whole durable transaction.
 * Recovery (facade_recover.c) stops at the first gap and drops a torn-off tail
 * transaction whose COMMIT is absent.
 *
 * The generalized {ALLOC*, FREE*, OBJ, COMMIT} transaction machinery that
 * appends these records lives in facade_object.c (the single-extent durable
 * write is one caller of it); this file is just
 * the record encoding, shared by that machinery and by recovery.
 *
 * This file links no libevpl.
 */

#include <string.h>

#include "common/macros.h"
#include "facade_internal.h"
#include "facade_log.h"
#include "media_types.h"

/* Build one log record into a zeroed 4 KiB block. */
SYMBOL_EXPORT void
facade_lr_build(
    void               *block,
    enum facade_lr_type type,
    uint64_t            seq,
    uint64_t            txn,
    const void         *payload,
    uint32_t            len)
{
    struct facade_lr_hdr *h = block;
    uint32_t              hc;

    FACADE_CHECK(sizeof(*h) + len <= CHIMERA_MEDIA_BLOCK_SIZE, "log record too large");

    memset(block, 0, CHIMERA_MEDIA_BLOCK_SIZE);
    h->magic    = FACADE_LR_MAGIC;
    h->seq      = seq;
    h->txn      = txn;
    h->type     = (uint32_t) type;
    h->format   = FACADE_LR_FORMAT;
    h->len      = len;
    h->hdr_crc  = 0;
    h->body_crc = 0;
    h->reserved = 0;

    /* hdr_crc covers the header with BOTH crc fields zeroed (validated before
     * len is trusted); body_crc covers the payload. */
    hc = facade_crc32(h, (uint32_t) sizeof(*h));
    if (len) {
        memcpy((uint8_t *) block + sizeof(*h), payload, len);
    }
    h->body_crc = facade_crc32((uint8_t *) block + sizeof(*h), len);
    h->hdr_crc  = hc;
} /* facade_lr_build */
