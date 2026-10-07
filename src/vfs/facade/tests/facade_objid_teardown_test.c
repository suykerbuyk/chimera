// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Regression test for F2 (heap-UAF / hang on teardown with an in-flight
 * superblock write) over the async media fixture.
 *
 * close/abandon is issued while an id-floor extension's superblock write is
 * still in flight AND an id draw is parked.  The fix must:
 *   - defer the free until the extension's completion lands (no UAF when the
 *     deferred completion dereferences the allocator); and
 *   - resume the parked draw cleanly (FACADE_E_SHUTDOWN), never hang it.
 *
 * ASan proves no use-after-free and no leak; the draw's terminal status proves
 * no hang.  Covers both abandon and close.
 */

#undef NDEBUG
#include <assert.h>

#include "facade_test_util.h"

/* Drive one teardown-with-in-flight-extension case; close selects close vs
 * abandon. */
static void
run_case(
    const char *uri,
    int         use_close)
{
    struct chimera_media_space *space;
    struct facade_alloc        *a;
    struct tu_draw              d;
    struct tu_status_w          cw = { FACADE_E_IO, 0 };

    space = tu_media_open(uri, 4u << 20);
    tu_format(space);
    a = tu_open(space);
    tu_close(a);
    a = tu_open(space);   /* next draw sits at the floor */

    facade_test_media_stall_writes(space, 1);

    /* Park a draw with the extension's block-0 write in flight. */
    TCHECK(tu_draw_start(&d, a) == FACADE_AGAIN, "draw should park");
    TCHECK(facade_test_media_pending_count(space) == 1, "extension in flight");
    TCHECK(d.got == 0 && d.shutdown == 0, "draw neither issued nor failed yet");

    /* Tear down with the extension still in flight and the draw parked. */
    if (use_close) {
        facade_alloc_close(a, tu_on_status, &cw);
        TCHECK(cw.done == 0, "close deferred behind the in-flight extension");
    } else {
        facade_alloc_abandon(a);   /* deferred: mw_busy */
    }

    /* The deferred completion lands: teardown runs, the parked draw is resumed
     * and fails cleanly, and the allocator is freed.  ASan would flag a UAF or
     * a leak here on the pre-fix code. */
    facade_test_media_release_all(space);

    if (use_close) {
        TCHECK(cw.done == 1 && cw.st == FACADE_OK, "close should complete");
    }
    /* The parked draw reached a terminal state with no hang: resumed with an id
     * (close -- the extension lands and resumes it before the close runs) or
     * failed cleanly with FACADE_E_SHUTDOWN (abandon -- teardown resumes it with
     * shutting_down set).  Either is correct; what must not happen is a hang
     * (still parked) or a UAF/leak (ASan would abort). */
    TCHECK(d.again == 0, "parked draw must not still be parked (hang)");
    TCHECK(d.got == 1 || d.shutdown == 1, "parked draw must reach a terminal state");
    if (use_close) {
        TCHECK(d.got == 1, "close: the extension lands first, so the draw resumes");
    } else {
        TCHECK(d.shutdown == 1, "abandon: the draw is failed cleanly on teardown");
    }

    tu_media_close(space);
} /* run_case */

int
main(void)
{
    facade_test_media_reset();

    run_case("teardown_abandon", 0);
    run_case("teardown_close", 1);

    facade_test_media_reset();

    printf("facade_objid_teardown_test: ok\n");
    return 0;
} /* main */
