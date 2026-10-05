// SPDX-FileCopyrightText: 2025-2026 Chimera-NAS Project Contributors
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * Unit coverage for the per-connection parked-CREATE ceiling.
 *
 * The ceiling is the single gate every CREATE park path consults before it
 * parks: the share-conflict yield (chimera_smb_create_gen_open_file), the
 * caching-break park (chimera_smb_create_open_finish) and the durable-reconnect
 * retry (chimera_smb_durable_reconnect) all refuse once
 * chimera_smb_conn_parked_creates_full() reports the connection full, and the
 * count itself is kept at the one funnel every park flows through
 * (chimera_smb_async_interim_begin / _cancel).  This test drives that gate and
 * its configuration knob directly; the full protocol exercise of the three park
 * paths lives in the model/smbtorture tiers, which stand up a live connection.
 *
 * Run under the Debug (AddressSanitizer) build.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server/server.h"
#include "server/smb/smb_internal.h"

static int passed = 0;
static int failed = 0;

#define CHECK(cond, name)                                      \
        do {                                                   \
            if (cond) { fprintf(stderr, "  PASS: %s\n", name); passed++; } \
            else { fprintf(stderr, "  FAIL: %s\n", name); failed++; } \
        } while (0)

/* The configuration knob: default and round-trip through the public setter and
 * getter (server.c field + default + setter + getter; daemon.c reads the same
 * setter from JSON). */
static void
test_config_knob(void)
{
    struct chimera_server_config *config = chimera_server_config_init();

    CHECK(config != NULL, "config_init returns a config");
    if (!config) {
        return;
    }

    CHECK(chimera_server_config_get_smb2_max_parked_creates(config) == 512,
          "parked-CREATE ceiling defaults to 512");

    chimera_server_config_set_smb2_max_parked_creates(config, 4);
    CHECK(chimera_server_config_get_smb2_max_parked_creates(config) == 4,
          "parked-CREATE ceiling round-trips through set/get");

    free(config);
} /* test_config_knob */

/* The gate: chimera_smb_conn_parked_creates_full() reports the connection full
 * exactly at the configured ceiling, so a park is admitted below it and refused
 * at or above it. */
static void
test_cap_predicate(void)
{
    struct chimera_server_smb_shared *shared = calloc(1, sizeof(*shared));
    struct chimera_server_smb_thread *thread = calloc(1, sizeof(*thread));
    struct chimera_smb_conn          *conn   = calloc(1, sizeof(*conn));

    shared->config.smb2_max_parked_creates = 4;
    thread->shared                         = shared;
    conn->thread                           = thread;

    conn->parked_creates = 0;
    CHECK(!chimera_smb_conn_parked_creates_full(conn),
          "empty connection admits a park");

    conn->parked_creates = 3;
    CHECK(!chimera_smb_conn_parked_creates_full(conn),
          "one below the ceiling still admits a park");

    conn->parked_creates = 4;
    CHECK(chimera_smb_conn_parked_creates_full(conn),
          "at the ceiling a further park is refused");

    conn->parked_creates = 5;
    CHECK(chimera_smb_conn_parked_creates_full(conn),
          "above the ceiling a further park is refused");

    free(conn);
    free(thread);
    free(shared);
} /* test_cap_predicate */

int
main(
    int   argc,
    char *argv[])
{
    test_config_knob();
    test_cap_predicate();

    fprintf(stderr, "\nparked-CREATE cap test: %d passed, %d failed\n",
            passed, failed);
    return failed == 0 ? 0 : 1;
} /* main */
