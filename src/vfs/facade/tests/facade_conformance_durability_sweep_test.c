// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * conformance-durability-power-loss CAMPAIGN (sibling of the single-shot smoke
 * facade_conformance_durability_test.c).
 *
 * Runs chimera_sc_oblig_durable_survives_crash across N seeds, each on a FRESH
 * space: a new sparse 1 GiB loopback file + facade_alloc_format + bind, THEN
 * control->reseed(space, seed), THEN run_one.  `reseed` alone does not make a
 * mid-sweep failure replayable (it does not rewind staged data or the
 * delegate), so each iteration -- and the single-seed replay process -- starts
 * from a fresh space and reseeds before any write.  On FAIL the replay seed is
 * printed to stderr (set CHIMERA_SC_DURABILITY_SWEEP_SEED=<seed> to re-run just
 * that one).  N defaults to 64 (env CHIMERA_SC_DURABILITY_SWEEP_SEEDS raises it);
 * the 1 GiB images are sparse (ftruncate only) so each iteration writes only a
 * superblock + small log + one data block before the crash/recover -- cheap.
 *
 * Crash policy is KEEP-biased with a nonzero DROP weight.  Durable records are
 * never dropped or torn (crash-sim invariant), so durable_survives_crash stays
 * green at every random crash point; the campaign catches any facade regression
 * that mis-reports a durable write.  w_tear is left 0: facade OBJ_WRITE submits
 * one block and crashsim folds tear into drop for nblocks<2, so no torn record
 * is staged -- a torn-object obligation is deferred (needs an >=2-block record
 * OBJ_WRITE cannot stage).  The dishonest-variant proof for this obligation is
 * the existing in-mem ack_durable_without_persist cheat (selftest), not a tear.
 *
 * (A buffered-loss-signals-epoch obligation was intentionally NOT added here:
 * the facade prototype is write-through -- a BUFFERED write is made durable and
 * the crash-sim never drops a durable record -- so the buffered bytes always
 * survive the crash, the "lost" branch never fires, and such an obligation would
 * assert nothing (vacuous GREEN).  It is untestable until a backend exposes a
 * genuinely non-durable BUFFERED write path; tracked as a follow-on.)
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

#define ONE_GIB         (1ULL << 30)
#define SWEEP_BASE_SEED 0xC0FFEEULL
#define SWEEP_DEFAULT_N 64u

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

/*
 * One fresh-space run of `tcase` under crash policy (w_keep,w_drop), reseeded to
 * `seed` after format and before any write.  Returns the case result; on PASS
 * adds this crash's dropped-record count to *r_dropped.  Everything is torn down
 * (close + destroy + unlink) before return so iterations do not share an image.
 */
static enum chimera_sc_conf_result
run_one_seed(
    struct evpl                              *evpl,
    const char                               *dir,
    unsigned                                  idx,
    uint64_t                                  seed,
    uint32_t                                  w_keep,
    uint32_t                                  w_drop,
    const struct chimera_sc_conformance_case *tcase,
    uint64_t                                 *r_writes)
{
    char path[700];
    struct chimera_media_crashsim               *handle;
    const struct chimera_media_provider         *provider;
    const struct chimera_media_crashsim_control *control;
    struct chimera_media_crashsim_config         cfg;
    struct chimera_media_crashsim_stats          stats;
    struct open_wait                             ow   = { 0, CHIMERA_MEDIA_E_IO, NULL };
    struct fmt_wait                              fw   = { 0, FACADE_E_IO };
    struct facade_geometry                       geom = { 0 };
    struct facade_sc_bind                        bind;
    struct chimera_sc_conformance_fixture        fx;
    enum chimera_sc_conf_result                  result;
    int ok = 0;

    snprintf(path, sizeof(path), "%s/dev_%u.img", dir, idx);

    memset(&cfg, 0, sizeof(cfg));
    cfg.delegate = chimera_media_loopback();
    cfg.seed     = seed;
    cfg.w_keep   = w_keep;
    cfg.w_drop   = w_drop;
    cfg.w_tear   = 0;
    CHECK(chimera_media_crashsim_create(&cfg, &handle, &provider, &control) == 0);

    /* Fresh sparse 1 GiB image (ftruncate only; the loopback floors to 1 GiB). */
    chimera_media_crashsim_open(handle, evpl, path, ONE_GIB, NULL, on_open, &ow);
    pump(evpl, &ow.done);
    CHECK(ow.status == CHIMERA_MEDIA_OK && ow.space != NULL);

    facade_alloc_format(provider, ow.space, &geom, on_fmt, &fw);
    pump(evpl, &fw.done);
    CHECK(fw.st == FACADE_OK);

    /* Reseed AFTER the fresh format, BEFORE any write (format/compaction writes
    * are not drawn from the crash PRNG, so the seed governs only the crash). */
    control->reseed(ow.space, seed);

    facade_sc_bind_init(&bind, provider, control, ow.space);
    facade_sc_fixture_init(&fx, &bind);

    result = chimera_sc_conformance_run_one(&fx, tcase, evpl);

    if (result == CHIMERA_SC_CONF_PASS && r_writes) {
        /* The crash-sim's last_crash_kept/dropped count only STAGED (not-yet-
         * durable) records drained by the crash.  durable_survives_crash flushes
         * everything durable BEFORE it crashes, so its staging is empty at crash
         * time and both counters are 0 by construction.  writes_total (media
         * writes since open: format + the durable write) is the counter that is
         * nonzero, so it is the liveness smoke that real I/O happened this seed.
         * The strong proof is the per-seed run_one PASS asserted by the caller. */
        control->stats(ow.space, &stats);
        *r_writes += stats.writes_total;
    }

    provider->close(ow.space, on_mclose, &ok);
    pump(evpl, &ok);
    CHECK(ok);
    chimera_media_crashsim_destroy(handle);
    CHECK(unlink(path) == 0);

    return result;
} /* run_one_seed */

int
main(void)
{
    const char                              *tmp = getenv("TMPDIR");
    const char                              *env_n;
    const char                              *env_seed;
    char                                     dir[512];
    struct evpl                             *evpl;
    uint64_t                                 total_writes = 0;
    unsigned                                 n;
    unsigned                                 i;
    int                                      single      = 0;
    uint64_t                                 single_seed = 0;
    const struct chimera_sc_conformance_case durable     = {
        "durability_power_loss", 0, chimera_sc_oblig_durable_survives_crash
    };

    env_seed = getenv("CHIMERA_SC_DURABILITY_SWEEP_SEED");
    if (env_seed && env_seed[0]) {
        single      = 1;
        single_seed = strtoull(env_seed, NULL, 0);
    }

    env_n = getenv("CHIMERA_SC_DURABILITY_SWEEP_SEEDS");
    n     = (env_n && env_n[0]) ? (unsigned) strtoul(env_n, NULL, 0) : SWEEP_DEFAULT_N;
    if (n == 0) {
        n = 1;
    }

    snprintf(dir, sizeof(dir), "%s/facade_confsweep_XXXXXX",
             (tmp && tmp[0]) ? tmp : "/tmp");
    CHECK(mkdtemp(dir) != NULL);

    evpl = evpl_create(NULL);
    CHECK(evpl != NULL);

    if (single) {
        /* Replay: the SAME fresh-space-then-reseed sequence for one seed. */
        enum chimera_sc_conf_result r =
            run_one_seed(evpl, dir, 0, single_seed, 8, 2, &durable, &total_writes);

        if (r != CHIMERA_SC_CONF_PASS) {
            fprintf(stderr, "FAIL replaying seed %llu\n",
                    (unsigned long long) single_seed);
            evpl_destroy(evpl);
            CHECK(rmdir(dir) == 0);
            return 1;
        }
        printf("facade_conformance_durability_sweep_test: replay seed %llu PASS\n",
               (unsigned long long) single_seed);
        evpl_destroy(evpl);
        CHECK(rmdir(dir) == 0);
        return 0;
    }

    for (i = 0; i < n; i++) {
        uint64_t                    seed = SWEEP_BASE_SEED + i;
        enum chimera_sc_conf_result r    =
            run_one_seed(evpl, dir, i, seed, 8, 2, &durable, &total_writes);

        if (r != CHIMERA_SC_CONF_PASS) {
            fprintf(stderr,
                    "durable_survives_crash FAILED at seed %llu -- replay with "
                    "CHIMERA_SC_DURABILITY_SWEEP_SEED=%llu\n",
                    (unsigned long long) seed, (unsigned long long) seed);
            evpl_destroy(evpl);
            return 1;
        }
    }

    /* Liveness smoke: real media I/O happened across the fresh spaces.  (The
     * plan's draft asserted Sigma last_crash_dropped > 0, but durable_survives_
     * crash flushes everything durable before it crashes, so NOTHING is staged
     * at crash time and both last_crash_kept and last_crash_dropped are 0 by
     * construction -- neither can be the coverage metric.  writes_total is the
     * nonzero counter; the STRONG proof is the per-seed run_one PASS the loops
     * above assert.  Exercising the DROP policy needs an obligation that stages
     * non-durable records -- out of scope for this unit.) */
    fprintf(stderr, "sweep: %u seeds, Sigma writes_total = %llu\n",
            n, (unsigned long long) total_writes);
    CHECK(total_writes > 0);

    evpl_destroy(evpl);
    CHECK(rmdir(dir) == 0);

    printf("facade_conformance_durability_sweep_test: ok (%u seeds GREEN)\n", n);
    return 0;
} /* main */
