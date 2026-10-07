// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Exit criterion (decision #4 = EXPAND): E3's conformance-durability-power-loss
 * runs GREEN against the real facade-backed contract backend.
 *
 * It drives the storage-contract durability obligation
 * (chimera_sc_oblig_durable_survives_crash) through the minimal facade
 * chimera_sc_backend over a crash-sim media provider on a loopback file:
 * OBJ_CREATE -> NS_TXN(HOLD) -> OBJ_WRITE(DURABLE) -> crash -> recover ->
 * re-ATTACH -> OBJ_READ -> the durable bytes survive.  Unprivileged (the
 * delegate is the pread loopback on a sparse temp file).
 */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "evpl/evpl.h"

#include "crashsim/chimera_media_crashsim.h"
#include "loopback/media_loopback.h"
#include "facade_alloc.h"
#include "facade_sc_backend.h"
#include "sc_conformance.h"

#define ONE_GIB (1ULL << 30)

#define CHECK(cond)                                                      \
        do {                                                             \
            if (!(cond)) {                                               \
                fprintf(stderr, "CHECK failed: %s (%s:%d)\n",            \
                        #cond, __FILE__, __LINE__);                      \
                exit(1);                                                 \
            }                                                            \
        } while (0)

static void
pump(
    struct evpl *evpl,
    const int   *done)
{
    while (!*done) {
        evpl_continue(evpl);
    }
} /* pump */

struct open_wait {
    int                         done;
    enum chimera_media_error status;
    struct chimera_media_space *space;
};

static void
on_open(
    enum chimera_media_error    status,
    struct chimera_media_space *space,
    void                       *private_data)
{
    struct open_wait *w = private_data;

    w->status = status;
    w->space  = space;
    w->done   = 1;
} /* on_open */

struct fmt_wait {
    int done;
    enum facade_status st;
};

static void
on_fmt(
    enum facade_status st,
    void              *private_data)
{
    struct fmt_wait *w = private_data;

    w->st   = st;
    w->done = 1;
} /* on_fmt */

static void
on_mclose(
    enum chimera_media_error st,
    void                    *private_data)
{
    *(int *) private_data = (st == CHIMERA_MEDIA_OK);
} /* on_mclose */

int
main(void)
{
    const char                                  *tmp = getenv("TMPDIR");
    char                                         dir[512];
    char                                         path[700];
    struct evpl                                 *evpl;
    struct chimera_media_crashsim               *handle;
    const struct chimera_media_provider         *provider;
    const struct chimera_media_crashsim_control *control;
    struct chimera_media_crashsim_config         cfg;
    struct open_wait                             ow   = { 0, CHIMERA_MEDIA_E_IO, NULL };
    struct fmt_wait                              fw   = { 0, FACADE_E_IO };
    struct facade_geometry                       geom = { 0 };
    struct facade_sc_bind                        bind;
    struct chimera_sc_conformance_fixture        fx;
    const struct chimera_sc_conformance_case     tcase = {
        "durability_power_loss", 0, chimera_sc_oblig_durable_survives_crash
    };
    enum chimera_sc_conf_result                  result;
    int                                          ok = 0;

    snprintf(dir, sizeof(dir), "%s/facade_conf_XXXXXX",
             (tmp && tmp[0]) ? tmp : "/tmp");
    CHECK(mkdtemp(dir) != NULL);
    snprintf(path, sizeof(path), "%s/dev.img", dir);

    evpl = evpl_create(NULL);
    CHECK(evpl != NULL);

    /* A KEEP-biased crash policy (the default): durable records always survive;
     * the obligation's object was made durable before the crash. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.delegate = chimera_media_loopback();
    cfg.seed     = 0xC0FFEE;
    CHECK(chimera_media_crashsim_create(&cfg, &handle, &provider, &control) == 0);

    chimera_media_crashsim_open(handle, evpl, path, ONE_GIB, NULL, on_open, &ow);
    pump(evpl, &ow.done);
    CHECK(ow.status == CHIMERA_MEDIA_OK && ow.space != NULL);

    /* Format the facade once before the first ATTACH opens it. */
    facade_alloc_format(provider, ow.space, &geom, on_fmt, &fw);
    pump(evpl, &fw.done);
    CHECK(fw.st == FACADE_OK);

    bind.provider = provider;
    bind.control  = control;
    bind.space    = ow.space;
    facade_sc_fixture_init(&fx, &bind);

    result = chimera_sc_conformance_run_one(&fx, &tcase, evpl);
    CHECK(result == CHIMERA_SC_CONF_PASS);

    provider->close(ow.space, on_mclose, &ok);
    pump(evpl, &ok);   /* on_mclose sets ok; reuse it as a done flag */
    CHECK(ok);
    chimera_media_crashsim_destroy(handle);
    CHECK(unlink(path) == 0);
    evpl_destroy(evpl);
    CHECK(rmdir(dir) == 0);

    printf("facade_conformance_durability_test: ok (conformance-durability-power-loss GREEN)\n");
    return 0;
} /* main */
