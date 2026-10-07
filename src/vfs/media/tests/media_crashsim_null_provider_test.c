// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * chimera_media_crashsim_create validation.  No evpl loop is driven: create
 * only binds the delegate and checks its ABI, the way chimera_media_open's gate
 * does.  Proves it refuses a NULL cfg / NULL delegate / wrong-ABI delegate and
 * accepts a conformant one (the loopback provider), handing back a provider
 * whose own ABI and name are correct.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crashsim/chimera_media_crashsim.h"
#include "loopback/media_loopback.h"

#define CHECK(cond)                                                      \
        do {                                                             \
            if (!(cond)) {                                               \
                fprintf(stderr, "CHECK failed: %s (%s:%d)\n",            \
                        #cond, __FILE__, __LINE__);                      \
                exit(1);                                                 \
            }                                                            \
        } while (0)

/* A deliberately wrong-ABI provider descriptor. */
static const struct chimera_media_provider bad_abi_provider = {
    .abi_version = CHIMERA_MEDIA_ABI_VERSION + 1000u,
    .name        = "bad-abi",
};

int
main(void)
{
    struct chimera_media_crashsim               *h  = NULL;
    struct chimera_media_crashsim               *h2 = NULL;
    const struct chimera_media_provider         *p  = NULL;
    const struct chimera_media_crashsim_control *c  = NULL;
    struct chimera_media_crashsim_config         cfg;

    /* NULL cfg. */
    CHECK(chimera_media_crashsim_create(NULL, &h, &p, &c) == -1);

    /* NULL delegate. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.delegate = NULL;
    CHECK(chimera_media_crashsim_create(&cfg, &h, &p, &c) == -1);

    /* Wrong-ABI delegate. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.delegate = &bad_abi_provider;
    CHECK(chimera_media_crashsim_create(&cfg, &h, &p, &c) == -1);

    /* Conformant delegate (loopback): accepted. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.delegate = chimera_media_loopback();
    cfg.seed     = 7;
    CHECK(chimera_media_crashsim_create(&cfg, &h, &p, &c) == 0);
    CHECK(h != NULL);
    CHECK(p != NULL);
    CHECK(c != NULL);
    CHECK(p->abi_version == CHIMERA_MEDIA_ABI_VERSION);
    CHECK(p->name != NULL && strcmp(p->name, "crashsim") == 0);
    /* The vtable I/O entries are present; open is the typed entry, so the
     * descriptor's open slot is intentionally unused. */
    CHECK(p->read && p->write && p->flush && p->close && p->size_bytes && p->info);
    CHECK(c->crash && c->recover && c->corrupt && c->reseed && c->stats);

    /* handle_out is required (the caller must be able to free the handle). */
    CHECK(chimera_media_crashsim_create(&cfg, NULL, &p, &c) == -1);

    /* provider_out / control_out are optional. */
    CHECK(chimera_media_crashsim_create(&cfg, &h2, NULL, NULL) == 0);
    CHECK(h2 != NULL);
    chimera_media_crashsim_destroy(h2);

    chimera_media_crashsim_destroy(h);

    printf("media_crashsim_null_provider_test: ok\n");
    return 0;
} /* main */
