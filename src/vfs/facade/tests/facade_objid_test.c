// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Exit criterion: object ids are unique across restarts (the H2 fix).
 *
 * Three cases:
 *   1. clean restart: close + reopen, ids stay disjoint and strictly advancing;
 *   2. crash restart: reopen WITHOUT a clean close, ids still disjoint -- this
 *      is what the reserve-ahead fua floor buys that a shutdown-only counter
 *      (cairn's defect) does not;
 *   3. the FACADE_AGAIN park/resume path, exercised deterministically by
 *      stalling the floor-extension write.
 */

#undef NDEBUG
#include <assert.h>

#include "facade_test_util.h"

#define MAX_IDS 4096

static struct chimera_sc_obj_id g_ids[MAX_IDS];
static int                      g_nids;

static void
record_id(const struct chimera_sc_obj_id *id)
{
    int i;

    /* global uniqueness across every id this run has ever issued */
    for (i = 0; i < g_nids; i++) {
        TCHECK(memcmp(&g_ids[i], id, sizeof(*id)) != 0,
               "object id reused (index %d of %d)", i, g_nids);
    }
    TCHECK(g_nids < MAX_IDS, "id buffer overflow");
    g_ids[g_nids++] = *id;
} /* record_id */

static void
draw_n(
    struct facade_alloc *a,
    int                  n)
{
    struct chimera_sc_obj_id id;
    int                      i;

    for (i = 0; i < n; i++) {
        enum facade_status st = facade_objid_next(a, &id, NULL, NULL);
        /* These draws never park: the fixture completes the floor extension
         * inline, so the floor is always ahead by the time the next draw
         * runs. */
        TCHECK(st == FACADE_OK, "unexpected park in a synchronous-media draw");
        record_id(&id);
    }
} /* draw_n */

/* --- park/resume harness --- */

struct park_state {
    struct facade_alloc     *alloc;
    struct chimera_sc_obj_id id;
    int                      got;
    int                      parked_again;
};

static void
park_resume(void *arg)
{
    struct park_state *ps = arg;
    enum facade_status st;

    st = facade_objid_next(ps->alloc, &ps->id, park_resume, ps);
    if (st == FACADE_OK) {
        ps->got = 1;
    } else {
        ps->parked_again = 1;
    }
} /* park_resume */

int
main(void)
{
    struct chimera_media_space *space;
    struct facade_alloc        *a;
    uint64_t                    nonce0;

    facade_test_media_reset();
    g_nids = 0;

    /* ---------- clean + crash restart ---------- */
    space = tu_media_open("id", 4u << 20);
    tu_format(space);
    a = tu_open(space);

    draw_n(a, 50);
    nonce0 = tu_id_nonce(&g_ids[0]);

    /* clean restart */
    tu_close(a);
    a = tu_open(space);
    TCHECK(tu_id_nonce(&g_ids[0]) == nonce0, "nonce stable");
    draw_n(a, 50);   /* record_id enforces global uniqueness */

    /* crash restart: draw, then reopen WITHOUT a clean close.  Abandon the
     * handle (no persist) to model the crash: media keeps only the fua'd id
     * floor, not a final checkpoint. */
    draw_n(a, 50);
    facade_alloc_abandon(a);
    a = tu_open(space);
    draw_n(a, 50);   /* must not collide with anything drawn pre-"crash" */

    /* the instance nonce is stable across every restart */
    {
        int i;
        for (i = 0; i < g_nids; i++) {
            TCHECK(tu_id_nonce(&g_ids[i]) == nonce0, "nonce changed across restart");
        }
    }

    tu_close(a);
    tu_media_close(space);

    /* ---------- FACADE_AGAIN park/resume ---------- */
    facade_test_media_reset();
    space = tu_media_open("idpark", 4u << 20);
    tu_format(space);
    a = tu_open(space);          /* reopen => id_next == id_floor */

    /* Reopen again so the very next draw sits exactly at the durable floor and
     * must extend it before it can issue. */
    tu_close(a);
    a = tu_open(space);

    {
        struct park_state  ps = { a, { { 0 } }, 0, 0 };
        enum facade_status st;

        facade_test_media_stall_writes(space, 1);
        st = facade_objid_next(a, &ps.id, park_resume, &ps);

        TCHECK(st == FACADE_AGAIN, "draw at the floor should park");
        TCHECK(facade_test_media_write_pending(space),
               "a floor-extension fua write should be in flight");
        TCHECK(ps.got == 0, "must not have an id before the extension lands");

        /* Fire the stalled completion: floor lands, waiter resumes, draw wins. */
        facade_test_media_release(space);
        TCHECK(ps.got == 1 && !ps.parked_again,
               "resume should have produced an id");
    }

    facade_alloc_abandon(a);   /* stall still on; drop without persisting */
    tu_media_close(space);
    facade_test_media_reset();

    printf("facade_objid_test: ok (%d ids, all unique)\n", g_nids);
    return 0;
} /* main */
