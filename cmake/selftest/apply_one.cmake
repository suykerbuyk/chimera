# SPDX-FileCopyrightText: 2025-2026 Chimera-NAS Project Contributors
#
# SPDX-License-Identifier: LGPL-2.1-only

# apply_one.cmake -- one-shot child entry point for the overlay self-test.
#
# Run as `cmake -P cmake/selftest/apply_one.cmake -D...`.  It calls exactly one
# of the shared PatchApply primitives so the self-test can assert that each of
# the two detectors fires INDEPENDENTLY and that each fires with a non-zero
# exit.  A configure-time FATAL_ERROR cannot be caught within the same
# configure, which is why these legs run as separate child processes.
#
#   MODE=APPLY   chimera_patch_apply_one(...) -- used with ISOLATE=OFF inside a
#                work tree to reproduce the silent "Skipped" no-op; the Skipped
#                detector must fire (non-zero exit).
#   MODE=VERIFY  chimera_patch_verify_one(...) -- run against an UNMODIFIED
#                staging tree; the sha256/sentinel verifier must fire.

cmake_minimum_required(VERSION 3.22)

include("${CMAKE_CURRENT_LIST_DIR}/../PatchApply.cmake")

if(NOT DEFINED MODE)
    message(FATAL_ERROR "apply_one.cmake: -DMODE=APPLY|VERIFY is required")
endif()

if(MODE STREQUAL "APPLY")
    if(NOT DEFINED ISOLATE)
        set(ISOLATE ON)
    endif()
    chimera_patch_apply_one(
        GIT "${GIT_EXECUTABLE}"
        WORKDIR "${WORKDIR}"
        STAGING_PARENT "${STAGING_PARENT}"
        DIRECTORY "${REPO_ROOT}"
        PATCH "${PATCH}"
        ISOLATE ${ISOLATE}
        LABEL "apply_one")
elseif(MODE STREQUAL "VERIFY")
    chimera_patch_verify_one(
        STAGING "${STAGING}"
        REPO_ROOT "${REPO_ROOT}"
        PATCH "${PATCH}"
        PRE_DIR "${PRE_DIR}"
        SENTINEL "${SENTINEL}"
        LABEL "apply_one")
else()
    message(FATAL_ERROR "apply_one.cmake: unknown MODE '${MODE}'")
endif()
