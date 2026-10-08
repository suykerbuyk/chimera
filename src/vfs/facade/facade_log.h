// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * Backend-facade durability log (media-log-and-recovery).
 *
 * An append-ordered redo log in the media space's log region (facade_super.h
 * layout).  One record per 4 KiB block; records are globally sequenced and
 * carry two CRCs -- a header CRC validated BEFORE `len` is trusted, and a body
 * CRC over the payload -- so a torn write is caught, not mis-parsed.  The log
 * never physically wraps between checkpoints (compaction-before-wrap), so it is
 * always a contiguous run from log_block and recovery stops at the first gap.
 *
 * A durable object write is one transaction: {ALLOC, OBJ, COMMIT}.  The COMMIT
 * is made durable only AFTER the object-data FUA completes (gate G2), so
 * recovery never installs a mapping onto non-durable data.  If the transaction
 * does not fit the log, a compaction runs first at the transaction boundary,
 * before any in-memory allocation delta (gate G1: a consistent cut at
 * checkpoint_seq), so no post-reset record is double-applied on replay.
 */

#include <stdint.h>

#include "facade_alloc.h"   /* enum facade_status, facade_epoch_cb_t */
#include "facade_super.h"   /* struct facade_ckpt_objext */

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

struct facade_alloc;

/* "FACLOGR2" as a little-endian u64. */
#define FACADE_LR_MAGIC  0x3252474F4C434146ULL
#define FACADE_LR_FORMAT 1u

enum facade_lr_type {
    FACADE_LR_ALLOC  = 1, /* a run of blocks was allocated (removed from free) */
    FACADE_LR_FREE   = 2, /* a run of blocks was freed (returned to free) */
    FACADE_LR_OBJ    = 3, /* an object record: full facade_obj_rec + extents */
    FACADE_LR_COMMIT = 4, /* closes a transaction */
    FACADE_LR_NS     = 5, /* a namespace record put/delete (namespace-record-store) */
};

/* FACADE_LR_NS op: a put installs key->value at revision; a delete removes key
 * (val_len 0).  One NS record is one 4 KiB log block (the key/value caps in
 * facade_ns.h keep header + key + value within the block). */
enum facade_lr_ns_op {
    FACADE_LR_NS_PUT    = 1,
    FACADE_LR_NS_DELETE = 2,
};

/* FACADE_LR_NS payload: this header, then key_len key bytes, then val_len value
 * bytes (val_len 0 for a delete).  The checkpoint stores the same key/value
 * after struct facade_ns_ckpt_rec, so a record is identical in log and
 * checkpoint. */
struct facade_lr_ns {
    uint64_t revision;
    uint32_t key_len;
    uint32_t val_len;
    uint32_t op;       /* enum facade_lr_ns_op */
    uint32_t reserved; /* must be zero */
};

/* One log-record header.  Followed by `len` payload bytes, the whole padded to
 * one 4 KiB block.  hdr_crc covers the header with hdr_crc and body_crc zeroed;
 * body_crc covers the payload. */
struct facade_lr_hdr {
    uint64_t magic;    /* FACADE_LR_MAGIC */
    uint64_t seq;      /* global monotonic sequence number */
    uint64_t txn;      /* transaction id */
    uint32_t type;     /* enum facade_lr_type */
    uint32_t format;   /* FACADE_LR_FORMAT */
    uint32_t len;      /* payload bytes after the header */
    uint32_t hdr_crc;  /* over the header, hdr_crc+body_crc zeroed */
    uint32_t body_crc; /* over the payload */
    uint32_t reserved; /* must be zero */
};

/* ALLOC / FREE payload. */
struct facade_lr_alloc {
    uint64_t start;
    uint64_t nblocks;
};

/* The OBJ payload is a struct facade_obj_rec (facade_super.h) followed by its
* nextents struct facade_ckpt_objext -- the same shape the checkpoint stores, so
* an object's durable record is identical in the log and in the checkpoint. */

/* COMMIT payload. */
struct facade_lr_commit {
    uint64_t txn;
    uint64_t nrecords;
};

/* Build one log record into a zeroed 4 KiB block (header two-CRC, payload).
 * Exposed so the generalized transaction machinery (facade_object.c) builds
 * ALLOC/FREE/OBJ/COMMIT records with it. */
void
facade_lr_build(
    void               *block,
    enum facade_lr_type type,
    uint64_t            seq,
    uint64_t            txn,
    const void         *payload,
    uint32_t            len);

/* facade_recover.c: replay the contiguous durable prefix of the log over the
 * loaded checkpoint baseline (called by facade_alloc_open when clean == 0).
 * Applies ALLOC/FREE to the free set and OBJ to the object table, stopping at
 * the first missing/torn/out-of-order record or an uncommitted tail txn. */
void
facade_recover_replay(
    struct facade_alloc *alloc,
    facade_cb_t          cb,
    void                *private_data);

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
