// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Exit criterion (conformance-error-obligations): the section-11 NO_SPACE
 * obligation runs GREEN against the real facade-backed contract backend.
 *
 * It drives chimera_sc_oblig_no_space over the in-RAM facade_test_media() at
 * 128 KiB -- NOT the crash-sim + loopback stack the corrupt case uses, because
 * the loopback provider floors every space to 1 GiB (media_loopback.c) and a
 * single-object one-block fill on a 1 GiB pool trips the 64-extent per-object
 * cap (FACADE_OBJ_MAX_EXT) rather than genuine pool exhaustion.  A 128 KiB
 * facade image leaves a data region (~15 blocks) BELOW that cap, so the
 * obligation's OBJ_CREATE -> OBJ_WRITE loop reaches real pool exhaustion
 * (facade_txn_alloc -> FACADE_E_NO_SPACE, mapped to CHIMERA_SC_E_NO_SPACE) and
 * a following read-side op still succeeds.
 *
 * The facade exposes no fill() hook (fx.fill == NULL); the obligation's write
 * loop is what exhausts the pool.  No crash-sim control is needed (the case
 * never crashes/recovers/corrupts), so the bind uses a NULL control.
 * Unprivileged and sub-second: in-RAM media, no file I/O, ~15 one-block writes.
 */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "evpl/evpl.h"

#include "facade_sc_backend.h"
#include "sc_conformance.h"
#include "facade_test_util.h"   /* tu_media_open / tu_format / facade_test_media */

#define CHECK(cond)                                                      \
        do {                                                             \
            if (!(cond)) {                                               \
                fprintf(stderr, "CHECK failed: %s (%s:%d)\n",            \
                        #cond, __FILE__, __LINE__);                      \
                exit(1);                                                 \
            }                                                            \
        } while (0)

int
main(void)
{
    struct evpl                             *evpl;
    struct chimera_media_space              *space;
    struct facade_sc_bind                    bind;
    struct chimera_sc_conformance_fixture    fx;
    const struct chimera_sc_conformance_case tcase = {
        "no_space", 0, chimera_sc_oblig_no_space
    };
    enum chimera_sc_conf_result              result;

    evpl = evpl_create(NULL);
    CHECK(evpl != NULL);

    facade_test_media_reset();

    /* 128 KiB => a small data region (< the 64-extent cap), so a single-object
     * OBJ_WRITE loop reaches genuine pool exhaustion. */
    space = tu_media_open("nospace_conf", 128u << 10);
    tu_format(space);

    /* Test media provider, NO crash-sim control (this case never crashes). */
    facade_sc_bind_init(&bind, facade_test_media(), NULL, space);
    facade_sc_fixture_init(&fx, &bind);

    /* The facade supplies no fill() hook; the obligation's write loop exhausts
     * the pool itself. */
    CHECK(fx.fill == NULL);

    result = chimera_sc_conformance_run_one(&fx, &tcase, evpl);
    CHECK(result == CHIMERA_SC_CONF_PASS);

    tu_media_close(space);
    facade_test_media_reset();
    evpl_destroy(evpl);

    printf("facade_conformance_nospace_test: ok "
           "(conformance-error-obligations NO_SPACE GREEN over 128 KiB in-RAM media)\n");
    return 0;
} /* main */
