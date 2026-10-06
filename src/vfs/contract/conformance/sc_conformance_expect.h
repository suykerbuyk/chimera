// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

#pragma once

/*
 * The obligation-assertion vocabulary.
 *
 * These macros are how an obligation primitive (sc_conformance.h) or a sibling
 * suite states an expectation.  On failure each logs a diagnostic through
 * chimera_sc_conf_logf() and returns CHIMERA_SC_CONF_FAIL from the enclosing
 * function, so a conformance check reports a failure rather than aborting.
 *
 * They do NOT use assert(): a conformance library must behave the same under
 * NDEBUG, where a bare assert() would vanish and turn the check into a no-op.
 * Use these in anything that returns enum chimera_sc_conf_result.
 */

#include "sc_conformance.h"

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */

/* Fail the enclosing check unless cond holds. */
#define CHIMERA_SC_EXPECT(ctx, cond, ...)                              \
        do {                                                           \
            if (!(cond)) {                                             \
                chimera_sc_conf_logf((ctx), __FILE__, __LINE__,        \
                                     #cond, __VA_ARGS__);              \
                return CHIMERA_SC_CONF_FAIL;                           \
            }                                                          \
        } while (0)

/* Expect a request to have completed with the wanted contract status. */
#define CHIMERA_SC_EXPECT_STATUS(ctx, got, want)                       \
        CHIMERA_SC_EXPECT((ctx), (uint32_t) (got) == (uint32_t) (want), \
                          "status %u, want %u",                        \
                          (unsigned) (got), (unsigned) (want))

/* Expect two byte ranges to be equal. */
#define CHIMERA_SC_EXPECT_BYTES(ctx, a, b, n)                          \
        CHIMERA_SC_EXPECT((ctx), memcmp((a), (b), (n)) == 0,           \
                          "%u bytes differ", (unsigned) (n))

#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */
