/* SPDX-FileCopyrightText: 2025-2026 Chimera-NAS Project Contributors */
/* SPDX-License-Identifier: LGPL-2.1-only */

/*
 * Fixture source for the patches/ overlay self-test.  It stands in for a file
 * inside the pinned libevpl/xdrzcc overlay so the self-test can apply a real
 * git-style patch to it the same way the mechanism does at configure time.
 * It is never compiled.
 */

int
chimera_patch_selftest_fixture(void)
{
    return 0;
}
