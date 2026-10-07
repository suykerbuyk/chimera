// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Regression test for F1 (H2 defeat via a superblock-write race) over the
 * async media fixture.
 *
 * With an id-floor extension's superblock (block 0) write still in flight, a
 * close is submitted.  The invariant the fix restores:
 *   - the two never write block 0 concurrently (the close is serialized behind
 *     the in-flight extension -- exactly one pending completion, and the close
 *     has not started its own writes); and
 *   - after driving everything to completion and a CRASH reopen (durable image
 *     only), the durable id floor has not regressed below any issued id, so no
 *     id is reused.
 *
 * On the pre-fix code the close's superblock write (carrying the stale floor)
 * was a second, unserialized writer to block 0; landing it last regressed the
 * durable floor and reissued an id after reopen.
 */

#undef NDEBUG
#include <assert.h>

#include "facade_test_util.h"

int
main(void)
{
    struct chimera_media_space *space;
    struct facade_alloc        *a;
    struct tu_draw              d;
    struct tu_status_w          cw = { FACADE_E_IO, 0 };
    uint64_t                    nonce, raced_seq, reopened_seq;

    facade_test_media_reset();

    space = tu_media_open("f1race", 4u << 20);
    tu_format(space);
    a = tu_open(space);
    /* Reopen so the next draw sits exactly at the durable floor and must extend
     * it before it can issue. */
    tu_close(a);
    a = tu_open(space);

    /* Hold the floor-extension's block-0 write in flight. */
    facade_test_media_stall_writes(space, 1);

    TCHECK(tu_draw_start(&d, a) == FACADE_AGAIN, "draw at the floor should park");
    TCHECK(facade_test_media_pending_count(space) == 1,
           "exactly one block-0 write (the extension) in flight, got %d",
           facade_test_media_pending_count(space));

    /* Submit close while the extension is in flight.  It must defer (serialized
     * behind the extension) -- NOT start a second block-0 write. */
    facade_alloc_close(a, tu_on_status, &cw);
    TCHECK(cw.done == 0, "close must not complete while the extension is in flight");
    TCHECK(facade_test_media_pending_count(space) == 1,
           "close must be queued, not racing block 0: pending=%d",
           facade_test_media_pending_count(space));

    /* Drive the extension completion, the resumed draw, and the whole queued
     * close chain (checkpoint -> super -> flush) to completion. */
    facade_test_media_release_all(space);
    TCHECK(cw.done == 1 && cw.st == FACADE_OK, "close should complete cleanly");
    TCHECK(d.got == 1, "the parked draw should have issued an id");
    raced_seq = tu_id_seq(&d.id);
    nonce     = tu_id_nonce(&d.id);

    /* Crash reopen: drop the space (its volatile view), reopen the durable
     * image, reopen the allocator. */
    tu_media_close(space);
    space = tu_media_open("f1race", 4u << 20);
    a     = tu_open(space);

    {
        struct tu_draw     d2;
        enum facade_status st = tu_draw_start(&d2, a);

        /* Not stalled: the reopen extension completes inline, so the draw does
         * not park. */
        TCHECK(st == FACADE_OK && d2.got == 1, "post-crash draw should succeed");
        reopened_seq = tu_id_seq(&d2.id);
        TCHECK(tu_id_nonce(&d2.id) == nonce, "instance nonce changed across crash");
        TCHECK(reopened_seq > raced_seq,
               "durable floor regressed: reopened seq %llu <= raced seq %llu (H2 reuse!)",
               (unsigned long long) reopened_seq, (unsigned long long) raced_seq);
    }

    tu_close(a);
    tu_media_close(space);
    facade_test_media_reset();

    printf("facade_objid_race_test: ok (raced seq %llu, reopened seq %llu)\n",
           (unsigned long long) raced_seq, (unsigned long long) reopened_seq);
    return 0;
} /* main */
