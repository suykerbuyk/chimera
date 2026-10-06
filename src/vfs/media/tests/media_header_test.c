// SPDX-FileCopyrightText: 2026 Quantum Corporation
//
// SPDX-License-Identifier: MIT

/*
 * Compile-time and version-negotiation checks for the media interface.
 *
 * Every value a provider or consumer relies on is asserted individually, so
 * renumbering, a collision or a swap fails to build.  Every member of every
 * ABI structure has its offset pinned on LP64, with every structure's size:
 * changing any of them is an ABI change and needs a CHIMERA_MEDIA_ABI_VERSION
 * bump (update the assertions in the same commit).
 */

#undef NDEBUG
#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "chimera_media.h"

#define MEDIA_ASSERT_VALUE(name, value) \
        _Static_assert((name) == (value), #name " is frozen at " #value)

/* ---- versions ---- */
MEDIA_ASSERT_VALUE(CHIMERA_MEDIA_ABI_VERSION, 1);
MEDIA_ASSERT_VALUE(CHIMERA_MEDIA_MAJOR, 1);
MEDIA_ASSERT_VALUE(CHIMERA_MEDIA_MINOR, 0);
MEDIA_ASSERT_VALUE(CHIMERA_MEDIA_PATCH, 0);

/* ---- block geometry ---- */
MEDIA_ASSERT_VALUE(CHIMERA_MEDIA_BLOCK_SIZE, 4096u);
MEDIA_ASSERT_VALUE(CHIMERA_MEDIA_BLOCK_SHIFT, 12);
MEDIA_ASSERT_VALUE(1u << CHIMERA_MEDIA_BLOCK_SHIFT, CHIMERA_MEDIA_BLOCK_SIZE);

/* ---- errors ---- */
MEDIA_ASSERT_VALUE(CHIMERA_MEDIA_OK, 0);
MEDIA_ASSERT_VALUE(CHIMERA_MEDIA_E_IO, 1);
MEDIA_ASSERT_VALUE(CHIMERA_MEDIA_E_NO_SPACE, 2);
MEDIA_ASSERT_VALUE(CHIMERA_MEDIA_E_RANGE, 3);
MEDIA_ASSERT_VALUE(CHIMERA_MEDIA_E_INVAL, 4);
MEDIA_ASSERT_VALUE(CHIMERA_MEDIA_E_NOT_SUPPORTED, 5);
MEDIA_ASSERT_VALUE(CHIMERA_MEDIA_E_NODEV, 6);

/* ---- write flag ---- */
MEDIA_ASSERT_VALUE(CHIMERA_MEDIA_WRITE_FUA, 1);

/* ---- the ABI version travels in a uint32_t field ----
 * The build also compiles this file with -fshort-enums
 * (media_header_short_enums in CMakeLists.txt), where an enum-typed field
 * would shrink and fail here. */
_Static_assert(sizeof(((struct chimera_media_provider *) 0)->abi_version) == 4,
               "uint32_t field");
_Static_assert(sizeof(((struct chimera_media_provider *) 0)->reserved) == 4,
               "uint32_t field");

/* ---- layout ----
 * Every ABI structure is pinned on LP64.  Changing any of them is an ABI
 * change and needs a CHIMERA_MEDIA_ABI_VERSION bump, with these assertions
 * regenerated in the same commit. */
_Static_assert(sizeof(struct chimera_media_version) == 8, "ABI layout");
_Static_assert(offsetof(struct chimera_media_version, major) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_media_version, minor) == 2, "ABI layout");
_Static_assert(offsetof(struct chimera_media_version, patch) == 4, "ABI layout");

_Static_assert(sizeof(struct chimera_media_space_info) == 16, "ABI layout");
_Static_assert(offsetof(struct chimera_media_space_info, nblocks) == 0, "ABI layout");
_Static_assert(offsetof(struct chimera_media_space_info, block_size) == 8, "ABI layout");
_Static_assert(offsetof(struct chimera_media_space_info, max_io_blocks) == 12, "ABI layout");

#if UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(struct chimera_media_provider) == 72, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_media_provider, abi_version) == 0, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_media_provider, reserved) == 4, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_media_provider, name) == 8, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_media_provider, open) == 16, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_media_provider, read) == 24, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_media_provider, write) == 32, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_media_provider, flush) == 40, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_media_provider, close) == 48, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_media_provider, size_bytes) == 56, "ABI layout (LP64)");
_Static_assert(offsetof(struct chimera_media_provider, info) == 64, "ABI layout (LP64)");
#endif /* if UINTPTR_MAX == UINT64_MAX */

static void
test_negotiate(void)
{
    struct chimera_media_version want = { 1, 3, 0 };
    struct chimera_media_version have = { 1, 1, 7 };
    struct chimera_media_version out  = { 0, 0, 0 };

    /* Equal majors: minor is the lower of the two, patch is the provider's. */
    assert(chimera_media_version_negotiate(&want, &have, &out) == 0);
    assert(out.major == 1 && out.minor == 1 && out.patch == 7);

    want.minor = 0;
    assert(chimera_media_version_negotiate(&want, &have, &out) == 0);
    assert(out.minor == 0);

    /* Identical versions. */
    have = want;
    assert(chimera_media_version_negotiate(&want, &have, &out) == 0);
    assert(out.major == want.major && out.minor == want.minor);

    /* A major mismatch refuses, in either direction. */
    have.major = 2;
    assert(chimera_media_version_negotiate(&want, &have, &out) == -1);
    want.major = 3;
    assert(chimera_media_version_negotiate(&want, &have, &out) == -1);
} /* test_negotiate */

int
main(void)
{
    test_negotiate();
    printf("media_header_test: ok\n");
    return 0;
} /* main */
